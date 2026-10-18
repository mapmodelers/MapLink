#!/usr/bin/env python3
# maplink_export.py — Blender headless script for the MapLink UE5 plugin.
# Runs under Blender 3.6 LTS, 4.x and 5.x.
#
# v1.4.5 — Optimisations:
#   - "both" mode: analyze+export in a single Blender session (1 file open)
#   - Compatible with --factory-startup (no user prefs loaded)
#   - Blender is launched WITHOUT the .blend on the command line;
#     the script opens it once via open_mainfile.
#
# Usage (invoked by UE5, not by the user directly):
#   blender --background --factory-startup --python maplink_export.py \
#           -- --blend <file.blend> --outdir <artifact_root> --mode analyze|export|both
#
# v1.13: --outdir is the ARTIFACT ROOT (UE passes <ProjectSaved>/MapLink;
#        the addon passes its maplink_artifact_root preference). Artifacts
#        land in <outdir>/Logs/<stem>/ and <outdir>/Export/<stem>/. The
#        plugin directory is never written to (read-only for
#        engine-installed marketplace plugins).

import bpy
import json
import sys
import os
import argparse
import hashlib
import mathutils
import traceback



# ---------------------------------------------------------------------------
# v1.21 - version gate
# ---------------------------------------------------------------------------
# The UE side refuses an old Blender before it launches anything, but this
# worker is also started by the addon's save handler and by hand, where there
# is no UE in front of it. Refuse by name rather than failing later in a way
# nobody can read.
MIN_BLENDER = (5, 0, 0)


def _require_blender_version():
    if bpy.app.version < MIN_BLENDER:
        print("[MapLink] ERROR: MapLink needs Blender "
              "{}.{} or newer. This is Blender {}.".format(
                  MIN_BLENDER[0], MIN_BLENDER[1], bpy.app.version_string),
              flush=True)
        raise SystemExit(1)

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

def _parse_args():
    """Parse arguments after the '--' separator."""
    argv = sys.argv
    if '--' not in argv:
        raise RuntimeError("Expected '--' separator in command-line arguments.")
    args = argv[argv.index('--') + 1:]
    ap = argparse.ArgumentParser(description='MapLink Blender export script')
    ap.add_argument('--blend', required=True, help='Path to the .blend file')
    ap.add_argument('--outdir', required=True,
                    help='Artifact root directory (UE: <ProjectSaved>/MapLink)')
    ap.add_argument('--mode', required=True, choices=['analyze', 'export', 'both'],
                    help='Operation mode')
    ap.add_argument('--lock', action='store_true',
                    help='v1.4.0 addon save_post: write/remove .export.lock at '
                         '<outdir>/Export/<stem>/.export.lock to serialise with UE imports.')
    return ap.parse_args(args)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _is_linked_asset_instance(obj):
    """Return True if *obj* is an instance of a linked collection marked as an Asset."""
    return (obj.instance_type == 'COLLECTION'
            and obj.instance_collection is not None
            and obj.instance_collection.library is not None
            and obj.instance_collection.asset_data is not None)


def _is_in_noexport_collection(obj):
    """Return True if *obj* belongs to any collection named 'NoExport'
    (case-insensitive), or to any descendant of such a collection.

    Walks up the full collection hierarchy via bpy.data.collections
    parent chains, so deeply nested objects are caught too."""
    # Build a set of all NoExport collections and their descendants (cached)
    if not hasattr(_is_in_noexport_collection, '_cache_id') or \
       _is_in_noexport_collection._cache_id != id(bpy.data.collections):
        excluded = set()
        # First pass: find root NoExport collections
        roots = [c for c in bpy.data.collections if c.name.lower() == 'noexport']
        # BFS to collect all descendants
        queue = list(roots)
        while queue:
            col = queue.pop()
            excluded.add(col.name)
            for child in col.children:
                if child.name not in excluded:
                    queue.append(child)
        _is_in_noexport_collection._excluded = excluded
        _is_in_noexport_collection._cache_id = id(bpy.data.collections)

    excluded = _is_in_noexport_collection._excluded
    if not excluded:
        return False

    # Check every collection the object belongs to
    for col in obj.users_collection:
        if col.name in excluded:
            return True
    return False


def _matrix_to_list(m):
    """Flatten a 4x4 Matrix to a 16-element row-major list."""
    return [m[row][col] for row in range(4) for col in range(4)]


def _list_to_matrix(flat):
    """Reconstruct a 4x4 Matrix from a 16-element row-major list."""
    m = mathutils.Matrix()
    for row in range(4):
        for col in range(4):
            m[row][col] = flat[row * 4 + col]
    return m


def _world_bbox(obj):
    """Return [min_x, min_y, min_z, max_x, max_y, max_z] in world space."""
    if not obj.bound_box:
        loc = obj.matrix_world.translation
        return [loc.x, loc.y, loc.z, loc.x, loc.y, loc.z]

    world_corners = [obj.matrix_world @ mathutils.Vector(corner)
                     for corner in obj.bound_box]
    xs = [c.x for c in world_corners]
    ys = [c.y for c in world_corners]
    zs = [c.z for c in world_corners]
    return [min(xs), min(ys), min(zs), max(xs), max(ys), max(zs)]


def _compute_file_hash(filepath):
    """Return MD5 hex digest of the file at *filepath*.

    v1.7.6 — Changed from SHA-256 to MD5 to align with sidecar_analysis.py
    (addon) and MapLinkPipeline.cpp (UE post-import).  The whole pipeline
    now uses one hash algorithm, so the freshness check
    (FMapLinkUtils::IsFbxCacheFresh) can compare against any of the three
    filehash files interchangeably.
    """
    h = hashlib.md5()
    with open(filepath, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def _ensure_fbx_addon():
    """Ensure the FBX exporter addon is enabled.
    Needed when running with --factory-startup, which may not
    enable all addons by default in some Blender versions."""
    try:
        import addon_utils
        addon_utils.enable('io_scene_fbx', default_set=True)
    except Exception:
        pass  # Already enabled, or handled as extension in Blender 5.x


# ---------------------------------------------------------------------------
# Core: analyze (file must already be open)
# ---------------------------------------------------------------------------

def _do_analyze(opts, blend_name, log_dir):
    """Analyze the currently open scene. Returns the report dict.
    Also writes analysis.json and filehash.txt to log_dir."""

    unit_scale = 1.0

    report = {
        'blend_name': blend_name,
        'unit_scale': unit_scale,
        'blend_own_geo': [],
        'linked_assets': [],
        'libraries': [],
    }

    # Collect own geometry and linked asset instances
    noexport_count = 0
    for obj in bpy.context.scene.objects:
        # Skip anything inside a NoExport collection
        if _is_in_noexport_collection(obj):
            noexport_count += 1
            continue

        if _is_linked_asset_instance(obj):
            src_path = bpy.path.abspath(
                obj.instance_collection.library.filepath)
            src_stem = os.path.splitext(os.path.basename(src_path))[0]
            if not (src_stem.startswith('BP_') or src_stem.startswith('LI_') or src_stem.startswith('SM_')):
                print(f"[MapLink] Skipping linked asset '{obj.name}' — "
                      f"source '{src_stem}' is not a BP_/LI_/SM_ file.")
                continue
            wm = obj.matrix_world
            entry = {
                'name': obj.name,
                'world_transform': _matrix_to_list(wm),
                'bbox_world': _world_bbox(obj),
                'source_blend_path': src_path,
                'asset_name': obj.instance_collection.name,
            }
            report['linked_assets'].append(entry)
        elif obj.type == 'MESH':
            wm = obj.matrix_world
            entry = {
                'name': obj.name,
                'world_transform': _matrix_to_list(wm),
                'bbox_world': _world_bbox(obj),
            }
            report['blend_own_geo'].append(entry)

    # Collect ALL library references (only BP_ / LI_ prefixed files)
    for lib in bpy.data.libraries:
        abs_path = bpy.path.abspath(lib.filepath)
        lib_stem = os.path.splitext(os.path.basename(abs_path))[0]
        if not (lib_stem.startswith('BP_') or lib_stem.startswith('LI_') or lib_stem.startswith('SM_')):
            print(f"[MapLink] Skipping library '{lib_stem}' — "
                  f"not a BP_/LI_/SM_ file.")
            continue
        report['libraries'].append({
            'name': lib.name,
            'filepath': abs_path,
        })

    # Write analysis report
    analysis_path = os.path.join(log_dir, 'analysis.json')
    with open(analysis_path, 'w', encoding='utf-8') as f:
        json.dump(report, f, indent=2)
    print(f"[MapLink] Analysis written to {analysis_path}")
    print(f"[MapLink] Scene unit_scale: {unit_scale}")
    if noexport_count > 0:
        print(f"[MapLink] Excluded {noexport_count} object(s) in NoExport collections.")

    # Write file hash
    file_hash = _compute_file_hash(opts.blend)
    hash_path = os.path.join(log_dir, 'filehash.txt')
    with open(hash_path, 'w', encoding='utf-8') as f:
        f.write(file_hash)
    print(f"[MapLink] Hash written to {hash_path}")

    return report


# ---------------------------------------------------------------------------
# Core: export (file must already be open, report must be provided)
# ---------------------------------------------------------------------------

def _do_export(opts, blend_name, log_dir, report):
    """Export FBX from the currently open scene using the given report.
    Skips FBX export if no own geometry."""

    own_geo_names = {entry['name'] for entry in report.get('blend_own_geo', [])}

    if not own_geo_names:
        print(f"[MapLink] No own geometry in '{blend_name}' — skipping FBX export.")
        return

    _ensure_fbx_addon()

    # Set scene unit scale to 0.01 BEFORE export (as user requires)
    bpy.context.scene.unit_settings.system = 'METRIC'
    bpy.context.scene.unit_settings.scale_length = 0.01

    # Select ONLY own-geometry mesh objects (no empties, no linked instances)
    bpy.ops.object.select_all(action='DESELECT')
    export_objects = []
    for obj in bpy.context.scene.objects:
        if obj.type == 'MESH' and obj.name in own_geo_names:
            obj.select_set(True)
            export_objects.append(obj)

    if not export_objects:
        print("[MapLink] Warning: No mesh objects matched own_geo names.")
        return

    print(f"[MapLink] Exporting {len(export_objects)} own-geo meshes.")

    # v1.7.2: Unparent own-geo meshes so each is a root-level FBX node.
    # UE's FbxFactory combines child meshes into the parent even with
    # bCombineMeshes=false, producing 1 mesh instead of N.
    # Safe in headless mode — the .blend is never saved after export.
    for obj in export_objects:
        if obj.parent is not None:
            mat = obj.matrix_world.copy()
            obj.parent = None
            obj.matrix_world = mat

    # Export FBX — User's exact UE_Maps preset
    export_dir = os.path.join(opts.outdir, 'Export', blend_name)
    os.makedirs(export_dir, exist_ok=True)
    export_path = os.path.join(export_dir, blend_name + '.fbx')

    bpy.ops.export_scene.fbx(
        filepath=export_path,
        use_selection=True,
        use_visible=False,
        use_active_collection=False,
        collection='',
        global_scale=100.0,
        apply_unit_scale=True,
        apply_scale_options='FBX_SCALE_NONE',
        use_space_transform=True,
        bake_space_transform=True,
        object_types={'MESH'},
        use_mesh_modifiers=True,
        use_mesh_modifiers_render=True,
        mesh_smooth_type='SMOOTH_GROUP',
        colors_type='LINEAR',
        prioritize_active_color=False,
        use_subsurf=False,
        use_mesh_edges=False,
        use_tspace=False,
        use_triangles=False,
        use_custom_props=False,
        add_leaf_bones=True,
        primary_bone_axis='Y',
        secondary_bone_axis='X',
        use_armature_deform_only=False,
        armature_nodetype='NULL',
        bake_anim=False,
        bake_anim_use_all_bones=True,
        bake_anim_use_nla_strips=True,
        bake_anim_use_all_actions=True,
        bake_anim_force_startend_keying=True,
        bake_anim_step=1.0,
        bake_anim_simplify_factor=1.0,
        path_mode='AUTO',
        embed_textures=False,
        batch_mode='OFF',
        use_batch_own_dir=True,
        axis_forward='X',
        axis_up='Z',
    )
    print(f"[MapLink] FBX exported to {export_path}")


# ---------------------------------------------------------------------------
# Mode entry points
# ---------------------------------------------------------------------------

def _run_analyze(opts):
    """Mode: analyze only. Opens file, analyzes, writes JSON + hash."""
    blend_name = os.path.splitext(os.path.basename(opts.blend))[0]
    log_dir = os.path.join(opts.outdir, 'Logs', blend_name)
    os.makedirs(log_dir, exist_ok=True)

    bpy.ops.wm.open_mainfile(filepath=opts.blend)
    _do_analyze(opts, blend_name, log_dir)


def _run_export(opts):
    """Mode: export only. Reads existing analysis.json, opens file, exports FBX."""
    blend_name = os.path.splitext(os.path.basename(opts.blend))[0]
    log_dir = os.path.join(opts.outdir, 'Logs', blend_name)

    # Load the analysis report produced by a prior analyze pass
    analysis_path = os.path.join(log_dir, 'analysis.json')
    if not os.path.isfile(analysis_path):
        raise RuntimeError(
            f"Analysis file not found: {analysis_path}. Run analyze mode first.")

    with open(analysis_path, 'r', encoding='utf-8') as f:
        report = json.load(f)

    own_geo_names = {entry['name'] for entry in report.get('blend_own_geo', [])}

    if not own_geo_names:
        print(f"[MapLink] No own geometry in '{blend_name}' — skipping FBX export.")
        # Still write/update the file hash
        file_hash = _compute_file_hash(opts.blend)
        hash_path = os.path.join(log_dir, 'filehash.txt')
        with open(hash_path, 'w', encoding='utf-8') as f:
            f.write(file_hash)
        print(f"[MapLink] Hash updated at {hash_path}")
        return

    # Open the blend file
    bpy.ops.wm.open_mainfile(filepath=opts.blend)
    _do_export(opts, blend_name, log_dir, report)

    # Write/update file hash
    file_hash = _compute_file_hash(opts.blend)
    hash_path = os.path.join(log_dir, 'filehash.txt')
    with open(hash_path, 'w', encoding='utf-8') as f:
        f.write(file_hash)
    print(f"[MapLink] Hash updated at {hash_path}")


def _run_both(opts):
    """Mode: analyze + export in a single session. One file open."""
    blend_name = os.path.splitext(os.path.basename(opts.blend))[0]
    log_dir = os.path.join(opts.outdir, 'Logs', blend_name)
    os.makedirs(log_dir, exist_ok=True)

    # Open file ONCE
    bpy.ops.wm.open_mainfile(filepath=opts.blend)

    # Analyze (reads scene data, writes analysis.json + filehash.txt)
    report = _do_analyze(opts, blend_name, log_dir)

    # Export (modifies unit_scale, selects objects, writes FBX)
    # No re-open needed — file is still loaded from the analyze step.
    _do_export(opts, blend_name, log_dir, report)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def _acquire_lock(opts):
    """v1.4.0: Write <outdir>/Export/<stem>/.export.lock containing this PID
    and the start timestamp.  Returns the lock path, or None if --lock not set
    or lock creation failed."""
    if not opts.lock:
        return None
    try:
        stem = os.path.splitext(os.path.basename(opts.blend))[0]
        lock_dir = os.path.join(opts.outdir, 'Export', stem)
        os.makedirs(lock_dir, exist_ok=True)
        lock_path = os.path.join(lock_dir, '.export.lock')
        import datetime
        with open(lock_path, 'w', encoding='utf-8') as f:
            f.write(f"{os.getpid()}\n{datetime.datetime.utcnow().isoformat()}Z\n")
        print(f"[MapLink] Lock acquired: {lock_path}")
        return lock_path
    except OSError as exc:
        print(f"[MapLink] Lock acquire failed (non-fatal): {exc}")
        return None


def _release_lock(lock_path):
    if not lock_path:
        return
    try:
        if os.path.exists(lock_path):
            os.remove(lock_path)
            print(f"[MapLink] Lock released: {lock_path}")
    except OSError as exc:
        print(f"[MapLink] Lock release failed (non-fatal): {exc}")


def main():
    _require_blender_version()
    opts = _parse_args()
    print(f"[MapLink] Mode={opts.mode}, Blend={opts.blend}, OutDir={opts.outdir}, Lock={opts.lock}")

    lock_path = _acquire_lock(opts)
    try:
        if opts.mode == 'analyze':
            _run_analyze(opts)
        elif opts.mode == 'export':
            _run_export(opts)
        elif opts.mode == 'both':
            _run_both(opts)
        else:
            raise RuntimeError(f"Unknown mode: {opts.mode}")

        print("[MapLink] Done.")
    except Exception:
        # v1.21 - Blender's default for an uncaught exception in a --python
        # script is to print the traceback and exit 0, so UE read every
        # failure here as a success and carried on with a stale FBX. Print
        # the traceback, say so in one greppable line, and exit non-zero.
        traceback.print_exc()
        print("[MapLink] FAILED.", flush=True)
        raise SystemExit(1)
    finally:
        _release_lock(lock_path)


if __name__ == '__main__':
    main()
