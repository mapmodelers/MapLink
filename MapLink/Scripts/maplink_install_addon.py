# maplink_install_addon.py — install / update + enable the MapLink Blender addon
# into the user's Blender, and (optionally) set its UE-plugin-dir preference.
#
# IMPORTANT: run WITHOUT --factory-startup. This script saves user preferences;
# under --factory-startup that would overwrite your real prefs (themes, keymaps,
# other addons) with factory defaults. The script refuses to run if it detects
# factory-startup.
#
# Usage:
#   blender --background --python maplink_install_addon.py -- \
#       --addon-dir "<plugin>/blender" [--plugin-dir "<plugin>"]
#
# It zips the addon package folder keeping its own top-folder name as the module
# name (so it updates the existing install in place rather than duplicating it —
# e.g. your current install is the folder "blender"), installs with overwrite,
# enables it, sets the UE-plugin-dir pref if given, and saves user prefs.

import argparse
import os
import sys
import tempfile
import zipfile

import bpy


def _log(msg):
    print(f"[MapLink InstallAddon] {msg}", flush=True)


def _argv():
    return sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []


def _parse(argv):
    ap = argparse.ArgumentParser(description="Install/update the MapLink Blender addon")
    ap.add_argument("--addon-dir", required=True,
                    help="The addon package folder, i.e. <plugin>/blender")
    ap.add_argument("--plugin-dir", default="",
                    help="<plugin> root; sets the addon's 'UE plugin dir' pref")
    ap.add_argument("--artifact-root", default="",
                    help="v1.13: <UEProject>/Saved/MapLink; sets the addon's "
                         "'Artifact root' pref (where Logs/Export artifacts go)")
    return ap.parse_args(argv)


def _zip_addon(addon_dir):
    """Zip the addon package so its top entry is <module>/… (module = folder name)."""
    addon_dir = os.path.abspath(addon_dir.rstrip("/\\"))
    module = os.path.basename(addon_dir)
    tmp = tempfile.mkdtemp(prefix="maplink_addon_")
    zpath = os.path.join(tmp, f"{module}.zip")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for root, dirs, files in os.walk(addon_dir):
            dirs[:] = [d for d in dirs if d != "__pycache__"]
            for fn in files:
                if fn.endswith(".pyc"):
                    continue
                full = os.path.join(root, fn)
                rel = os.path.relpath(full, addon_dir)
                z.write(full, os.path.join(module, rel))
    return zpath, module


def main():
    # Guard: never save prefs from a factory-startup session — it would clobber
    # the user's real preferences with factory defaults.
    if getattr(bpy.app, "factory_startup", False):
        _log("ERROR: run WITHOUT --factory-startup (saving prefs under "
             "factory-startup would overwrite your settings).")
        print("MAPLINK_ADDON_FAIL")
        sys.exit(1)

    args = _parse(_argv())
    addon_dir = os.path.abspath(args.addon_dir)
    if not os.path.isfile(os.path.join(addon_dir, "__init__.py")):
        _log(f"ERROR: no __init__.py under {addon_dir}")
        print("MAPLINK_ADDON_FAIL")
        sys.exit(1)

    zpath, module = _zip_addon(addon_dir)
    _log(f"packaged addon '{module}' -> {zpath}")

    try:
        bpy.ops.preferences.addon_install(filepath=zpath, overwrite=True)
    except Exception as exc:                       # noqa: BLE001
        _log(f"ERROR: addon_install failed: {exc}")
        print("MAPLINK_ADDON_FAIL")
        sys.exit(1)

    try:
        bpy.ops.preferences.addon_enable(module=module)
    except Exception as exc:                       # noqa: BLE001
        _log(f"ERROR: addon_enable('{module}') failed: {exc}")
        print("MAPLINK_ADDON_FAIL")
        sys.exit(1)

    # Optionally set the addon's UE-plugin-dir / artifact-root preferences.
    if args.plugin_dir or args.artifact_root:
        try:
            ap = bpy.context.preferences.addons[module].preferences
            if args.plugin_dir and hasattr(ap, "maplink_plugin_dir"):
                ap.maplink_plugin_dir = os.path.abspath(args.plugin_dir)
                _log(f"set maplink_plugin_dir = {ap.maplink_plugin_dir}")
            if args.artifact_root and hasattr(ap, "maplink_artifact_root"):
                ap.maplink_artifact_root = os.path.abspath(args.artifact_root)
                _log(f"set maplink_artifact_root = {ap.maplink_artifact_root}")
        except Exception as exc:                   # noqa: BLE001
            _log(f"WARN: could not set addon prefs: {exc}")

    try:
        bpy.ops.wm.save_userpref()
    except Exception as exc:                       # noqa: BLE001
        _log(f"WARN: save_userpref failed: {exc}")

    _log(f"installed + enabled '{module}'. Restart any open Blender to pick it up.")
    print("MAPLINK_ADDON_OK")
    sys.exit(0)


if __name__ == "__main__":
    main()
