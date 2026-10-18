# maplink_verify.py — headless MapLink project / .blend verifier.
#
# Read-only sanity pass over a produced MapLink set. Opens each target .blend in
# a single background Blender and reports the invariants the pipeline relies on,
# so a clean-PC / marketplace validation can confirm "the setup is correct"
# without importing into UE. Self-contained: no addon imports (runs under
# --factory-startup).
#
# Usage (either mode):
#   blender --background <any>.blend --factory-startup \
#           --python maplink_verify.py -- --root <projectRoot> [--unit-scale 1.0]
#   blender --background <any>.blend --factory-startup \
#           --python maplink_verify.py -- --blend <file.blend> [--unit-scale 1.0]
#
# Output: one "MAPLINK_VERIFY_FILE {json}" line per inspected .blend, then a
# final "MAPLINK_VERIFY_RESULT {json}" summary line and a MAPLINK_VERIFY_OK /
# MAPLINK_VERIFY_FAIL token (FAIL if any blocking issue is found).

import argparse
import json
import os
import sys

import bpy


def _log(msg):
    print(f"[MapLinkVerify] {msg}", flush=True)


def _argv():
    return sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []


def _parse_args(argv):
    ap = argparse.ArgumentParser(description="MapLink project / .blend verifier")
    ap.add_argument("--root", default="", help="Project root to walk (LI/SM/BP/preset)")
    ap.add_argument("--blend", default="", help="Single .blend to inspect")
    ap.add_argument("--unit-scale", type=float, default=1.0)
    return ap.parse_args(argv)


def _collect_targets(root):
    """Find the .blend files we care about under a project root."""
    targets = []
    for sub in ("LI", "SM", "BP", "preset"):
        base = os.path.join(root, sub)
        if not os.path.isdir(base):
            continue
        for dirpath, _dirs, files in os.walk(base):
            for f in files:
                if f.lower().endswith(".blend"):
                    targets.append(os.path.join(dirpath, f))
    return sorted(targets)


def _inspect_open_blend(path, expected_scale):
    """Inspect the currently-open .blend; return (report_dict, issues_list)."""
    issues = []
    report = {"blend": path}

    # Unit scale.
    scene = bpy.context.scene if bpy.context else None
    if scene is None and len(bpy.data.scenes):
        scene = bpy.data.scenes[0]
    scale = float(scene.unit_settings.scale_length) if scene else 0.0
    report["unit_scale"] = scale
    if abs(scale - expected_scale) > 1e-6:
        issues.append(f"unit_scale {scale} != expected {expected_scale}")

    # NoExport collection(s): presence + render-disabled.
    noexport = [c for c in bpy.data.collections if "NoExport" in c.name]
    report["noexport_collections"] = [
        {"name": c.name, "hide_render": bool(c.hide_render)} for c in noexport
    ]
    for c in noexport:
        if not c.hide_render:
            issues.append(f"NoExport collection '{c.name}' is not render-disabled")

    # Linked libraries: relative? resolves on disk? ghost (no users)?
    libs = []
    for lib in bpy.data.libraries:
        raw = lib.filepath or ""
        is_rel = raw.startswith("//")
        abs_path = bpy.path.abspath(raw) if raw else ""
        exists = bool(abs_path) and os.path.isfile(abs_path)
        # A library with no referencing datablocks is a ghost (cross-import bleed).
        users = getattr(lib, "users_id", None)
        ghost = (users is not None and len(users) == 0)
        libs.append({
            "name": lib.name, "filepath": raw, "relative": is_rel,
            "resolves": exists, "ghost": ghost,
        })
        if not is_rel:
            issues.append(f"library '{lib.name}' has an ABSOLUTE path: {raw}")
        if not exists:
            issues.append(f"library '{lib.name}' does not resolve on disk: {raw}")
        if ghost:
            issues.append(f"library '{lib.name}' is a ghost (no users) — should be purged")
    report["libraries"] = libs

    # Marked assets (asset_mark) by type, with catalog ids.
    marked = []
    for coll in (bpy.data.collections, bpy.data.objects,
                 bpy.data.materials, bpy.data.meshes):
        for dat in coll:
            ad = getattr(dat, "asset_data", None)
            if ad is not None:
                marked.append({
                    "id_type": type(dat).__name__,
                    "name": dat.name,
                    "catalog_id": getattr(ad, "catalog_id", "") or "",
                })
    report["marked_assets"] = marked

    # Own-geo mesh objects (rough payload indicator).
    report["mesh_object_count"] = sum(
        1 for o in bpy.data.objects if o.type == "MESH")

    return report, issues


def main():
    args = _parse_args(_argv())
    expected_scale = float(args.unit_scale)

    targets = []
    if args.blend:
        targets = [os.path.abspath(args.blend)]
    elif args.root:
        root = os.path.abspath(args.root)
        if not os.path.isdir(root):
            _log(f"ERROR: root not found: {root}")
            print("MAPLINK_VERIFY_FAIL")
            sys.exit(1)
        targets = _collect_targets(root)
    else:
        _log("ERROR: pass either --root or --blend.")
        print("MAPLINK_VERIFY_FAIL")
        sys.exit(1)

    if not targets:
        _log("ERROR: no .blend files found to inspect.")
        print("MAPLINK_VERIFY_FAIL")
        sys.exit(1)

    all_issues = {}
    inspected = 0
    for path in targets:
        if not os.path.isfile(path):
            all_issues[path] = ["file not found"]
            _log(f"MISSING: {path}")
            continue
        try:
            # Read-only open. bpy.context is invalidated after this call, so
            # inspection reads bpy.data directly rather than relying on context.
            bpy.ops.wm.open_mainfile(filepath=path)
        except Exception as exc:  # noqa: BLE001 — verifier must never hard-crash
            all_issues[path] = [f"open failed: {exc}"]
            _log(f"OPEN-FAIL: {path}: {exc}")
            continue

        report, issues = _inspect_open_blend(path, expected_scale)
        inspected += 1
        if issues:
            all_issues[path] = issues
        report["issues"] = issues
        print("MAPLINK_VERIFY_FILE " + json.dumps(report), flush=True)

    blocking = sum(len(v) for v in all_issues.values())
    summary = {
        "ok": blocking == 0,
        "inspected": inspected,
        "targets": len(targets),
        "files_with_issues": len(all_issues),
        "issue_count": blocking,
        "issues": all_issues,
    }
    print("MAPLINK_VERIFY_RESULT " + json.dumps(summary), flush=True)
    print("MAPLINK_VERIFY_OK" if blocking == 0 else "MAPLINK_VERIFY_FAIL")
    sys.exit(0 if blocking == 0 else 2)


if __name__ == "__main__":
    main()
