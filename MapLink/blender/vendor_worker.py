#!/usr/bin/env python3
"""MapLink vendor-in worker — conform library paths inside copied .blend files.

SELF-CONTAINED.  Launched with --factory-startup by vendor_in.py, so it imports
no addon module.

Vendor-in is a copy, not a rewrite, and that is deliberate: the layout
``<root>/<TYPE>/<Name>/<Name>.blend`` makes an LI→SM link
``//../../SM/Foo/Foo.blend`` — relative geometry that is identical in every
root — so a copied subtree resolves unchanged on any machine.

The one thing a copy cannot fix is a link that was stored ABSOLUTE in the
source.  Those still point at the source root after the copy.  This worker
opens each copied file, rewrites any library path that resolves under a source
root to the equivalent path under the destination root, stores it ``//``
relative, and saves only files it actually changed.

It also reports, per file, which linked material datablocks are not present in
the destination's Library_MaterialInstances.blend — the cross-root material
question, answered with names rather than a guess.

Usage:
    blender --background <throwaway>.blend --factory-startup \\
            --python vendor_worker.py -- <spec.json>

spec.json:
    {"dest_root": "...", "source_roots": ["..."], "blends": ["...", ...]}

Writes one JSON object to stdout between the markers below.
"""

import json
import os
import sys

import bpy

BEGIN = "###MAPLINK_VENDOR_REPORT_BEGIN###"
END = "###MAPLINK_VENDOR_REPORT_END###"

MI_LIBRARY = "Library_MaterialInstances.blend"


def _log(msg):
    print(f"[MapLink Vendor] {msg}", flush=True)


def _norm(p):
    return os.path.normcase(os.path.abspath(p))


def _under(path, root):
    if not path or not root:
        return False
    p, r = _norm(path), _norm(root)
    return p == r or p.startswith(r + os.sep)


def _relocate(abs_path, source_roots, dest_root):
    """Same path expressed under dest_root, or "" when it is not in a source."""
    for src in source_roots:
        if _under(abs_path, src):
            rel = os.path.relpath(abs_path, src)
            return os.path.join(dest_root, rel)
    return ""


def _dest_mi_names(dest_root):
    """Material datablock names in the destination MI library.

    ``libraries.load`` lists a file's datablock names WITHOUT linking any of
    them, so this costs one file read and leaves bpy.data untouched.
    """
    lib = os.path.join(dest_root, "MaterialInstances", MI_LIBRARY)
    if not os.path.isfile(lib):
        return None
    try:
        with bpy.data.libraries.load(lib, link=True) as (data_from, _to):
            return set(data_from.materials)
    except Exception as exc:                            # noqa: BLE001
        _log(f"could not read {lib}: {exc}")
        return None


def _process(blend, source_roots, dest_root, mi_names):
    entry = {"blend": blend, "rewritten": [], "unresolved": [],
             "missing_materials": [], "saved": False, "error": ""}
    try:
        bpy.ops.wm.open_mainfile(filepath=blend, load_ui=False)
    except Exception as exc:                            # noqa: BLE001
        entry["error"] = f"could not open: {exc}"
        return entry

    changed = False
    for lib in bpy.data.libraries:
        raw = lib.filepath
        abs_path = bpy.path.abspath(raw)

        if _under(abs_path, dest_root):
            # Already inside the destination; only the storage form may be wrong.
            if not raw.startswith("//"):
                try:
                    lib.filepath = bpy.path.relpath(abs_path)
                    entry["rewritten"].append([raw, lib.filepath])
                    changed = True
                except Exception as exc:                # noqa: BLE001
                    entry["unresolved"].append(f"{raw} ({exc})")
            continue

        moved = _relocate(abs_path, source_roots, dest_root)
        if moved and os.path.isfile(moved):
            try:
                lib.filepath = bpy.path.relpath(moved)
                entry["rewritten"].append([raw, lib.filepath])
                changed = True
            except Exception as exc:                    # noqa: BLE001
                entry["unresolved"].append(f"{raw} ({exc})")
        else:
            entry["unresolved"].append(raw)

    if mi_names is not None:
        for mat in bpy.data.materials:
            if mat.library is None:
                continue
            if os.path.basename(bpy.path.abspath(mat.library.filepath)).lower() \
                    != MI_LIBRARY.lower():
                continue
            if mat.name not in mi_names:
                entry["missing_materials"].append(mat.name)

    if changed:
        try:
            bpy.ops.wm.save_mainfile(filepath=blend, compress=True)
            entry["saved"] = True
        except Exception as exc:                        # noqa: BLE001
            entry["error"] = f"could not save: {exc}"

    return entry


def main():
    argv = sys.argv
    args = argv[argv.index("--") + 1:] if "--" in argv else []
    if not args:
        _log("ERROR: no spec file given")
        sys.exit(2)

    with open(args[0], "r", encoding="utf-8") as f:
        spec = json.load(f)

    dest_root = os.path.abspath(spec["dest_root"])
    source_roots = [os.path.abspath(r) for r in spec.get("source_roots", [])]
    blends = [os.path.abspath(b) for b in spec.get("blends", [])]

    mi_names = _dest_mi_names(dest_root)
    report = {"dest_root": dest_root, "files": []}
    for blend in blends:
        if not os.path.isfile(blend):
            report["files"].append({"blend": blend, "error": "missing"})
            continue
        report["files"].append(_process(blend, source_roots, dest_root, mi_names))

    print(BEGIN)
    print(json.dumps(report))
    print(END)


if __name__ == "__main__":
    main()
