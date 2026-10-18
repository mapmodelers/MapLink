#!/usr/bin/env python3
"""maplink_vendor_cli.py — run vendor-in headlessly, for UE to call.

    blender --background <throwaway>.blend --factory-startup \\
            --python maplink_vendor_cli.py -- \\
            --source "<...>/SM/SM_Foo/SM_Foo.blend" \\
            --dest-root "<project root>" [--overwrite] [--addon-package <name>]

Why this exists rather than a C++ reimplementation
--------------------------------------------------
UE's `Import .blend…` can be pointed at a file outside `BlenderProjectRoot`.
That import works on the machine that did it and nowhere else, which is the
whole defect v1.19 exists to close, so the dialog offers to copy the asset in
first.  "Copy it in" is vendor-in, and vendor-in already exists: the walk, the
conflict policy, the post-copy path conforming and the material-instance
reporting all live in `vendor_in.py`.  A second copy of that in C++ would agree
with it right up until somebody fixed a bug in one of them.  So UE launches
Blender and calls the operator, exactly as an artist would.

--factory-startup means no addon is loaded, so this enables it first — the same
thing `maplink_force_save.py` does, for the same reason.  It stays a *script*
rather than an addon module: nothing here registers anything, and it is only
ever invoked by path.

The addon package name is DISCOVERED, not assumed.  `maplink_install_addon.py`
names the installed module after the source folder (`os.path.basename`), so it
is `blender` for a repo install and something else for anyone who renamed the
directory — the historical `MapLink_BlenderV1_4_1` in force_save_all's docstring
is what that looks like in practice.  Guessing the name gives a silent no-op:
the addon never enables, the operator is missing, and the only symptom is that
nothing was copied.  `--addon-package` overrides the search when a caller does
know.

Prints one machine-readable line before exiting:

    ###MAPLINK_VENDOR_CLI### {"ok": true, "copied": 5, "vendored_blend": "..."}
"""

import argparse
import json
import os
import sys

import bpy

MARKER = "###MAPLINK_VENDOR_CLI###"

#: The module that proves we found the right addon, not merely an addon.
_REQUIRED_SUBMODULE = "vendor_in"


def log(msg):
    print(f"[VendorCLI] {msg}", flush=True)


def _emit(payload):
    print(MARKER + " " + json.dumps(payload), flush=True)


def _candidate_packages(explicit):
    """Addon module names that might be MapLink, best guess first."""
    if explicit:
        return [explicit]

    names = []
    try:
        import addon_utils
        for mod in addon_utils.modules(refresh=True):
            modname = getattr(mod, "__name__", "")
            info = getattr(mod, "bl_info", None) or {}
            label = str(info.get("name", ""))
            if "maplink" in label.lower() or "maplink" in modname.lower():
                names.append(modname)
    except Exception as exc:                            # noqa: BLE001
        log(f"addon_utils scan failed: {exc}")

    # The repo install names the module after the folder, so a plain 'blender'
    # is the common case and would never match a "maplink" test.
    for fallback in ("blender", "MapLink_Blender", "MapLink"):
        if fallback not in names:
            names.append(fallback)
    return names


def _try_enable(pkg):
    """Enable *pkg* and confirm it is really MapLink.  -> True on success."""
    try:
        import addon_utils
        addon_utils.enable(pkg, default_set=False, persistent=False)
    except Exception:                                   # noqa: BLE001
        pass
    try:
        bpy.ops.preferences.addon_enable(module=pkg)
    except Exception:                                   # noqa: BLE001
        pass

    # The operator is the only proof that matters. A module can enable and
    # still be the wrong addon, or the right addon at a version without
    # vendor-in.
    if sys.modules.get(f"{pkg}.{_REQUIRED_SUBMODULE}") is None:
        return False
    return hasattr(bpy.ops.maplink, "vendor_asset")


def resolve_addon(explicit):
    """-> the enabled MapLink package name, or "" with the reason logged."""
    tried = []
    for pkg in _candidate_packages(explicit):
        tried.append(pkg)
        if _try_enable(pkg):
            log(f"addon: {pkg}")
            return pkg
    log("ERROR: could not enable a MapLink addon exposing "
        "maplink.vendor_asset. Tried: " + ", ".join(tried))
    log("       Install or update the Blender addon first "
        "(MapLink_UpdateAddon.cmd, or UE's Install/Update Blender Addon).")
    return ""


def main():
    argv = sys.argv
    args = argv[argv.index("--") + 1:] if "--" in argv else []

    ap = argparse.ArgumentParser(description="MapLink headless vendor-in")
    ap.add_argument("--addon-package", default="",
                    help="Override the addon module name. Empty = discover it.")
    ap.add_argument("--source", required=True,
                    help="The .blend to copy in (absolute).")
    ap.add_argument("--dest-root", required=True,
                    help="Destination MapLink project root (absolute).")
    ap.add_argument("--overwrite", action="store_true",
                    help="Replace same-named assets at the destination. "
                         "Default is skip-existing: names are identity, so the "
                         "receiving project's copy wins unless told otherwise.")
    opts = ap.parse_args(args)

    source = os.path.abspath(opts.source)
    dest_root = os.path.abspath(opts.dest_root)

    if not os.path.isfile(source):
        _emit({"ok": False, "error": f"source not found: {source}"})
        sys.exit(2)
    if not os.path.isfile(os.path.join(dest_root, "maplink_project.json")):
        _emit({"ok": False, "error": f"no maplink_project.json in {dest_root}"})
        sys.exit(2)

    pkg = resolve_addon(opts.addon_package.strip())
    if not pkg:
        _emit({"ok": False,
               "error": "the MapLink Blender addon is not installed, or is "
                        "older than v1.19 (no vendor-in)"})
        sys.exit(3)

    # Report the plan before acting, so a failure mid-copy still leaves the
    # intent in the log next to whatever went wrong.
    plan = {"items": []}
    try:
        vendor_in = sys.modules[f"{pkg}.{_REQUIRED_SUBMODULE}"]
        plan = vendor_in.build_plan([source], dest_root)
        log(f"plan: {len(plan['items'])} folder(s), "
            f"{len(plan['no_sidecar'])} without a sidecar, "
            f"{len(plan['missing'])} unresolved")
        for item in plan["items"]:
            log(f"    {item['type']}/{item['name']}"
                f"{'  (already present)' if item['exists'] else ''}")
        for b in plan["no_sidecar"]:
            log(f"    NO SIDECAR: {os.path.basename(b)} — it will be copied, "
                f"but anything it links may not follow.")
    except Exception as exc:                            # noqa: BLE001
        log(f"could not pre-plan (continuing): {exc}")

    try:
        result = bpy.ops.maplink.vendor_asset(
            'EXEC_DEFAULT',
            source_blend=source,
            dest_root=dest_root,
            overwrite=bool(opts.overwrite),
            # Nothing is open in this process, so there are no links to
            # repoint. The copied files are conformed by the operator's own
            # worker pass regardless.
            repoint=False,
        )
    except Exception as exc:                            # noqa: BLE001
        _emit({"ok": False, "error": f"vendor_asset raised: {exc}"})
        sys.exit(4)

    if 'FINISHED' not in result:
        _emit({"ok": False, "error": f"vendor_asset returned {result}"})
        sys.exit(5)

    # Where the asset now lives, so UE can import THAT rather than the original.
    vendored = ""
    for item in plan.get("items", []):
        if os.path.normcase(item["blend"]) == os.path.normcase(source):
            vendored = os.path.join(item["dest_folder"],
                                    os.path.basename(source))
            break

    _emit({"ok": True,
           "addon_package": pkg,
           "copied": len(plan.get("items", [])),
           "dest_root": dest_root,
           "vendored_blend": vendored if os.path.isfile(vendored) else ""})


if __name__ == "__main__":
    main()
