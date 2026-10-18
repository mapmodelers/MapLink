# Copyright MapModelers. All Rights Reserved.
# v1.8 — BP Reference Mode — headless stand-in builder.
#
# Usage:
#   blender --background --factory-startup \
#       --python maplink_create_bp_standin.py -- --spec <path/to/spec.json>
#
# Reads a spec JSON written by the UE-side creator
# (FMapLinkBPStandInCreator) and produces:
#   - <target_blend>                       (the stand-in .blend)
#   - <target_blend stem>_analysis.json    (sidecar, with v1.8 fields)
#   - <target_blend stem>_filehash.txt     (MD5 of the saved .blend)
#   - <spec.report_path>                   (JSON staged report, schema
#                                           matches FMapLinkStandInReport
#                                           on the C++ side)
#
# Exit codes:
#   0 — success (report.result == "success")
#   1 — any failure (report.result == "failure" / "partial")
#
# All errors are caught, logged to stderr, and written to the report
# JSON before the process exits.  The UE-side caller reads the report
# to surface staged diagnostics in the dialog.
#
# Blender 5.0 quirks honoured:
#   - Save with compress=False (use_file_compression pref is silently
#     ignored in headless).
#   - Never trust bpy.data.filepath; everything comes from the spec.
#   - Linked collection .name is read-only — we only ever rename LOCAL
#     collections (the template's MapLink_BP_Template).
#   - FBX import axes: axis_forward='X', axis_up='Z'.

import argparse
import hashlib
import json
import math
import os
import sys
import time
import traceback

import bpy
import mathutils


# ---------------------------------------------------------------------------
# Reporter (schema-compatible with FMapLinkStandInReport C++ side)
# ---------------------------------------------------------------------------

class StandInReport:
    """Staged JSON report.  Mirrors the FMapLinkStandInReport schema so
    that the UE-side caller can deserialize and surface stages/errors
    in the post-op dialog."""

    def __init__(self, operation, blueprint_path, report_path,
                 blueprint_class=None):
        self.operation = operation
        self.blueprint_path = blueprint_path
        self.blueprint_class = blueprint_class
        self.standin_blend_path = None
        self.report_path = report_path

        self.timestamp_utc = _iso_utc_now()
        self.stages = []
        self.errors = []
        self.warnings = []
        self.outputs = {}

        # Default to failure until set_result('success') is called.
        # Mirrors C++ semantics: explicit success required.
        self._result = "failure"

        # Currently-open stage
        self._open_name = None
        self._open_t0 = None

    # -- stages --------------------------------------------------------

    def begin_stage(self, name):
        if self._open_name:
            # Caller forgot to end the prior stage — warn-close it.
            self.end_stage("warn", None, "auto_closed_by_new_begin_stage")
        self._open_name = name
        self._open_t0 = time.time()
        _stdout(f"[BPStandIn] stage begin: {name}")

    def end_stage(self, status, details, message):
        if not self._open_name:
            return
        duration_ms = int((time.time() - self._open_t0) * 1000)
        self.stages.append({
            "name": self._open_name,
            "status": status,
            "duration_ms": duration_ms,
            "message": message or "",
            "details": details or {},
        })
        _stdout(f"[BPStandIn] stage end:   {self._open_name} "
                f"status={status} ({duration_ms}ms) {message or ''}")
        self._open_name = None
        self._open_t0 = None

    # -- top-level fields ---------------------------------------------

    def add_error(self, msg):
        self.errors.append(msg)
        _stderr(f"[BPStandIn] ERROR: {msg}")

    def add_warning(self, msg):
        self.warnings.append(msg)
        _stderr(f"[BPStandIn] WARN: {msg}")

    def set_output(self, role, path):
        self.outputs[role] = path

    def set_result(self, result):
        # result ∈ {"success", "partial", "failure"}
        self._result = result

    # -- persistence ---------------------------------------------------

    def write(self):
        # Auto-close any still-open stage so the JSON is well-formed.
        if self._open_name:
            self.end_stage("warn", None, "auto_closed_in_write")

        # Errors-present-with-success downgrade → partial
        final = self._result
        if self.errors and final == "success":
            final = "partial"

        obj = {
            "operation": self.operation,
            "feature": "bp_reference_mode",
            "version": "1.8",
            "timestamp_utc": self.timestamp_utc,
            "blueprint_path": self.blueprint_path,
            "blueprint_class": self.blueprint_class,  # may be None
            "standin_blend_path": self.standin_blend_path,  # may be None
            "stages": self.stages,
            "result": final,
            "errors": self.errors,
            "warnings": self.warnings,
            "outputs": self.outputs,
        }

        try:
            os.makedirs(os.path.dirname(self.report_path), exist_ok=True)
            with open(self.report_path, "w", encoding="utf-8") as f:
                json.dump(obj, f, indent=2)
            _stdout(f"[BPStandIn] report written: {self.report_path} "
                    f"(result={final})")
        except Exception as e:
            _stderr(f"[BPStandIn] FAILED to write report: {e}")


# ---------------------------------------------------------------------------
# Helpers — stdout/stderr, time
# ---------------------------------------------------------------------------

def _stdout(msg):
    print(msg, flush=True)


def _stderr(msg):
    print(msg, file=sys.stderr, flush=True)


def _iso_utc_now():
    # ISO-8601 UTC with 'Z' suffix to match C++ FDateTime::UtcNow().ToIso8601()
    import datetime
    return datetime.datetime.utcnow().strftime("%Y-%m-%dT%H:%M:%S.") \
        + f"{datetime.datetime.utcnow().microsecond // 1000:03d}Z"


# ---------------------------------------------------------------------------
# Spec loading + arg parsing
# ---------------------------------------------------------------------------

def _parse_argv():
    """Pull args after the '--' separator (Blender convention)."""
    argv = sys.argv
    if "--" in argv:
        argv = argv[argv.index("--") + 1:]
    else:
        argv = []
    p = argparse.ArgumentParser(prog="maplink_create_bp_standin")
    p.add_argument("--spec", required=True,
                   help="Path to the spec JSON.")
    return p.parse_args(argv)


def _load_spec(spec_path):
    if not os.path.isfile(spec_path):
        raise RuntimeError(f"Spec file not found: {spec_path}")
    with open(spec_path, "r", encoding="utf-8") as f:
        spec = json.load(f)
    # Validate required fields up front so we fail fast.
    for k in ("blueprint_path", "blueprint_name", "target_blend",
              "template_blend", "mode", "report_path"):
        if k not in spec:
            raise RuntimeError(f"Spec missing required field: {k}")
    if spec["mode"] not in ("extracted", "bounds", "marker"):
        raise RuntimeError(
            f"Spec.mode must be one of extracted|bounds|marker, "
            f"got {spec['mode']!r}")
    return spec


# ---------------------------------------------------------------------------
# Blender helpers
# ---------------------------------------------------------------------------

def _ensure_fbx_addon():
    """With --factory-startup the FBX importer may not be enabled in
    some Blender 5.x configurations.  Same fix used by maplink_export.py."""
    try:
        import addon_utils
        addon_utils.enable("io_scene_fbx", default_set=True)
    except Exception:
        pass  # already on, or handled as extension in Blender 5.x


def _matrix_to_list(m):
    """4x4 → 16-element row-major flat list.  Matches maplink_export.py."""
    return [m[r][c] for r in range(4) for c in range(4)]


def _world_bbox(obj):
    """[min_x, min_y, min_z, max_x, max_y, max_z] in world space.
    Matches maplink_export.py."""
    if not obj.bound_box:
        loc = obj.matrix_world.translation
        return [loc.x, loc.y, loc.z, loc.x, loc.y, loc.z]
    corners = [obj.matrix_world @ mathutils.Vector(c) for c in obj.bound_box]
    xs = [c.x for c in corners]
    ys = [c.y for c in corners]
    zs = [c.z for c in corners]
    return [min(xs), min(ys), min(zs), max(xs), max(ys), max(zs)]


def _find_template_collection():
    """Locate the root collection in the loaded template .blend.
    Prefers the canonical name 'MapLink_BP_Template'.  Falls back to
    the first local (non-linked, non-NoExport) child of the scene root."""
    # Canonical name first
    col = bpy.data.collections.get("MapLink_BP_Template")
    if col is not None and col.library is None:
        return col

    # Fallback: any local collection that's a child of the scene root.
    scene_root = bpy.context.scene.collection
    for child in scene_root.children:
        if child.library is not None:
            continue
        if child.name.lower() == "noexport":
            continue
        return child
    return None


def _unlink_from_all_collections(obj):
    """Detach an object from every collection it's currently in.
    Necessary before re-linking to the stand-in root."""
    for col in list(obj.users_collection):
        try:
            col.objects.unlink(obj)
        except Exception:
            pass


def _apply_relative_transform(obj, relative_transform):
    """Apply a relative_transform dict from the spec to a Blender object.

    Spec convention (set by the UE-side caller): values are in UE space.
      - location:           [x, y, z] in meters (UE divides cm by 100), but
                            still UE-handed (left-handed, X fwd / Y right / Z up)
      - rotation_euler_deg: [roll, pitch, yaw] in degrees, UE rotator
      - scale:              [x, y, z] unitless

    This routine performs the UE -> Blender conversion (see below).  If the
    spec omits relative_transform entirely, the object stays at its imported
    origin.
    """
    if not relative_transform:
        return
    loc = relative_transform.get("location", [0.0, 0.0, 0.0])
    rot = relative_transform.get("rotation_euler_deg", [0.0, 0.0, 0.0])
    scl = relative_transform.get("scale", [1.0, 1.0, 1.0])
    # UE (left-handed) -> Blender (right-handed) is a reflection across Y.
    # The C++ side only did the cm->m scale on location and left the handedness,
    # so a UE component at Y=-64.07 cm landed at -0.64 m instead of +0.64 m.
    # Apply the full Y-flip so location, rotation and scale stay consistent with
    # the axis-converted geometry:
    #     location  (x, -y, z)
    #     rotation  (roll, -pitch, -yaw)     # B * R_ue * B, R_ue from UE's
    #                                        # FRotationMatrix (verified exact vs
    #                                        # the full matrix over 20k samples)
    #     scale     unchanged
    obj.location = (loc[0], -loc[1], loc[2])
    obj.rotation_euler = (
        math.radians(rot[0]),
        math.radians(-rot[1]),
        math.radians(-rot[2]),
    )
    obj.scale = (scl[0], scl[1], scl[2])


def _strip_materials(obj):
    """Remove every material slot from a mesh object."""
    if obj.type == "MESH" and obj.data is not None:
        obj.data.materials.clear()


# Module-level constant: UE exports FBX in cm, Blender wants m.
# UE-exported FBXs don't carry reliable unit metadata, so Blender's
# importer can't auto-convert.  We bake a 1/100 scale into the mesh
# vertex data right after import so placed objects keep clean (1,1,1)
# scale values and the relative_transform.location (which the C++
# side already converted from cm → m before writing the spec) lands
# everything at the right place.
_SCALE_CM_TO_M = mathutils.Matrix.Scale(0.01, 4)


def _scale_imported_meshes_cm_to_m(mesh_objs):
    """Apply 1/100 scale to mesh data of every imported mesh.
    Deduplicates by mesh data to avoid double-scaling instanced meshes."""
    seen = set()
    for o in mesh_objs:
        if o.type != "MESH" or o.data is None:
            continue
        # Use the mesh data block name as the dedup key.  Different
        # objects pointing at the same UE mesh asset share a data block.
        if o.data.name in seen:
            continue
        o.data.transform(_SCALE_CM_TO_M)
        seen.add(o.data.name)


# ---------------------------------------------------------------------------
# Mode handlers
# ---------------------------------------------------------------------------

def _build_extracted(spec, root_col, report):
    """Import each component's FBX, strip materials, place in root."""
    components = spec.get("components") or []
    if not components:
        report.add_warning(
            "extracted mode with zero components — collection will be empty")
        return

    for idx, comp in enumerate(components):
        obj_name = comp.get("object_name") or f"Component_{idx}"
        fbx_path = comp.get("fbx_path")
        if not fbx_path or not os.path.isfile(fbx_path):
            report.add_warning(
                f"component[{idx}] '{obj_name}': FBX not found ({fbx_path})"
                f" — skipping")
            continue

        # Snapshot existing objects so we can identify what the FBX added.
        before = set(bpy.data.objects.keys())

        try:
            bpy.ops.import_scene.fbx(
                filepath=fbx_path,
                use_image_search=False,
                use_custom_props=False,
                axis_forward="X",
                axis_up="Z",
            )
        except Exception as e:
            report.add_warning(
                f"component[{idx}] '{obj_name}': FBX import failed "
                f"({type(e).__name__}: {e}) — skipping")
            continue

        new_names = [n for n in bpy.data.objects.keys() if n not in before]
        new_objs = [bpy.data.objects[n] for n in new_names]
        new_meshes = [o for o in new_objs if o.type == "MESH"]
        non_meshes = [o for o in new_objs if o.type != "MESH"]

        # v1.8.1 — drop FBX import junk so it never reaches the stand-in.
        # UE's default static-mesh FBX export ships collision + sockets + LODs
        # alongside the render mesh:
        #   * collision primitives (UCX_/UBX_/USP_/UCP_*) import as extra meshes,
        #   * sockets / null nodes import as vertex-only objects that render as
        #     'points'.
        # Keep only real render geometry (a mesh with at least one polygon and
        # no collision-prefix name); route the rest to the removal list below.
        _COLLISION_PREFIXES = ("UCX_", "UBX_", "USP_", "UCP_")
        render_meshes = []
        for o in new_meshes:
            is_collision = o.name.startswith(_COLLISION_PREFIXES)
            is_empty_mesh = (o.data is None or len(o.data.polygons) == 0)
            if is_collision or is_empty_mesh:
                non_meshes.append(o)
            else:
                render_meshes.append(o)
        new_meshes = render_meshes

        # Discard non-render imports (lights/cameras/empties/armatures/collision)
        for o in non_meshes:
            bpy.data.objects.remove(o, do_unlink=True)

        if not new_meshes:
            report.add_warning(
                f"component[{idx}] '{obj_name}': FBX produced zero meshes "
                f"— skipping")
            continue

        # UE→Blender unit conversion: bake 1/100 into mesh vertex data
        # so placed objects keep clean (1,1,1) scale.  Must be done BEFORE
        # any transform is applied to the object, otherwise scale + location
        # interact and the bake produces the wrong result.
        _scale_imported_meshes_cm_to_m(new_meshes)

        # Primary mesh = first.  Multi-mesh FBXs are uncommon (UE exports
        # one UStaticMesh per FBX in our pipeline) but we handle them.
        primary = new_meshes[0]
        _strip_materials(primary)
        primary.name = obj_name
        _unlink_from_all_collections(primary)
        root_col.objects.link(primary)
        primary["mesh_uasset_path"] = comp.get("mesh_uasset_path", "")
        _apply_relative_transform(primary, comp.get("relative_transform"))

        for j, extra in enumerate(new_meshes[1:], start=1):
            _strip_materials(extra)
            extra.name = f"{obj_name}_part_{j}"
            _unlink_from_all_collections(extra)
            root_col.objects.link(extra)
            extra["mesh_uasset_path"] = comp.get("mesh_uasset_path", "")
            _apply_relative_transform(extra, comp.get("relative_transform"))

        _stdout(f"[BPStandIn] component[{idx}] '{obj_name}': "
                f"placed ({len(new_meshes)} mesh{'es' if len(new_meshes)>1 else ''})")


def _build_bounds(spec, root_col, report):
    """Single materialless cube scaled to bounds_fallback_extent."""
    extent = spec.get("bounds_fallback_extent") or [200.0, 200.0, 200.0]
    if len(extent) != 3:
        report.add_warning(
            f"bounds_fallback_extent length != 3 ({extent!r}) — "
            f"falling back to [200,200,200]")
        extent = [200.0, 200.0, 200.0]

    bpy.ops.mesh.primitive_cube_add(size=2.0, location=(0, 0, 0))
    cube = bpy.context.active_object
    # size=2 → 2x2x2 → scale by extent/2 to get a cube of `extent` units.
    cube.scale = (extent[0] / 2.0, extent[1] / 2.0, extent[2] / 2.0)
    cube.name = f"{spec['blueprint_name']}_Bounds"
    _strip_materials(cube)
    _unlink_from_all_collections(cube)
    root_col.objects.link(cube)


def _build_marker(spec, root_col, report):
    """200cm wireframe cube (= 2m in Blender), wireframe viewport display.

    The marker is a fixed-size placeholder for pure-logic BPs that have
    neither static meshes nor meaningful bounds.  Size is small (about
    the footprint of a human-sized actor) so it doesn't dwarf the
    surrounding level when linked in.
    """
    # primitive_cube_add(size=2.0) → 2m cube in Blender → 200cm in UE
    # after the standard 1:100 unit conversion downstream.  No scale.
    bpy.ops.mesh.primitive_cube_add(size=2.0, location=(0, 0, 0))
    cube = bpy.context.active_object
    cube.name = f"{spec['blueprint_name']}_LogicMarker"
    _strip_materials(cube)
    cube.display_type = "WIRE"
    _unlink_from_all_collections(cube)
    root_col.objects.link(cube)


# ---------------------------------------------------------------------------
# Sidecar writers
# ---------------------------------------------------------------------------

def _write_sidecar_analysis(spec, target_blend, mode):
    """Write <stem>_analysis.json next to the .blend.

    Field set matches what MapLinkDependencyGraph.cpp and the rest of
    the pipeline read from any analysis.json — plus the two v1.8
    additions ('bound_blueprint_path', 'standin_mode') that gate
    reference-mode behavior at reimport time.
    """
    blend_dir = os.path.dirname(target_blend)
    stem = os.path.splitext(os.path.basename(target_blend))[0]
    out_path = os.path.join(blend_dir, f"{stem}_analysis.json")

    blend_own_geo = []
    for obj in bpy.context.scene.objects:
        if obj.type != "MESH":
            continue
        blend_own_geo.append({
            "name": obj.name,
            "world_transform": _matrix_to_list(obj.matrix_world),
            "bbox_world": _world_bbox(obj),
        })

    report_dict = {
        "blend_name": stem,
        "unit_scale": 1.0,
        "blend_own_geo": blend_own_geo,
        "linked_assets": [],   # stand-ins link nothing
        "libraries": [],       # stand-ins reference no other .blends
        # ---- v1.8 BP Reference Mode additions (the reimport-time gate) ----
        "bound_blueprint_path": spec["blueprint_path"],
        "standin_mode": mode,
    }

    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(report_dict, f, indent=2)
    return out_path


def _write_sidecar_filehash(target_blend):
    """Write <stem>_filehash.txt with the MD5 of the saved .blend.

    Algorithm matches maplink_export.py (v1.7.6+ unified-MD5 pipeline)
    so the UE-side IsFbxCacheFresh / IsSidecarAnalysisValid checks
    compare against this hash interchangeably."""
    blend_dir = os.path.dirname(target_blend)
    stem = os.path.splitext(os.path.basename(target_blend))[0]
    out_path = os.path.join(blend_dir, f"{stem}_filehash.txt")

    h = hashlib.md5()
    with open(target_blend, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)

    with open(out_path, "w", encoding="utf-8") as f:
        f.write(h.hexdigest())
    return out_path


# ---------------------------------------------------------------------------
# Stage orchestration
# ---------------------------------------------------------------------------

def _stage_load_template(spec, report):
    report.begin_stage("load_template")
    _ensure_fbx_addon()
    template = spec["template_blend"]
    if not os.path.isfile(template):
        raise RuntimeError(f"Template .blend not found: {template}")
    # Wipe state first.  open_mainfile replaces the scene wholesale,
    # but we also clear any factory-startup junk just in case.
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.wm.open_mainfile(filepath=template)
    report.end_stage("ok", {"template": template}, "")


def _stage_prepare_root(spec, report):
    report.begin_stage("prepare_root_collection")
    root_col = _find_template_collection()
    if root_col is None:
        raise RuntimeError(
            "No usable root collection in template — expected a local "
            "(non-linked) collection named 'MapLink_BP_Template'.")
    bp_name = spec["blueprint_name"]
    # Local collection rename — .name is writable on non-linked collections.
    if root_col.name != bp_name:
        root_col.name = bp_name
    # Mark as asset so other .blends can link this collection, and put it in
    # the project's catalog -- v1.19: marking alone left every BP stand-in
    # under "Unassigned" in the Asset Browser.  asset_catalog lives in the
    # addon package next door; this script is only ever run by UE from the
    # plugin directory, where that sibling is present.
    try:
        root_col.asset_mark()
    except Exception as e:
        # asset_mark exists in Blender 3.0+; if it fails, warn and proceed.
        report.add_warning(f"asset_mark() failed: {e}")
    try:
        import sys as _sys
        _sys.path.insert(0, os.path.join(
            os.path.dirname(os.path.abspath(__file__)), os.pardir, "blender"))
        import asset_catalog as ac
        cat = ac.uuid_for_blend(spec.get("target_blend") or bpy.data.filepath)
        if cat:
            ac.assign(root_col, cat)
        else:
            report.add_warning("no MapLink project catalog resolved; the "
                               "stand-in will show under Unassigned")
    except Exception as e:
        report.add_warning(f"catalog assignment skipped: {e}")
    report.end_stage(
        "ok",
        {"collection_name": bp_name, "is_asset": bool(root_col.asset_data)},
        "")
    return root_col


def _stage_build_geometry(spec, root_col, report):
    report.begin_stage("build_geometry")
    mode = spec["mode"]
    if mode == "extracted":
        _build_extracted(spec, root_col, report)
    elif mode == "bounds":
        _build_bounds(spec, root_col, report)
    elif mode == "marker":
        _build_marker(spec, root_col, report)
    # mode validity already enforced by _load_spec
    placed = sum(1 for o in root_col.objects if o.type == "MESH")
    report.end_stage("ok",
                     {"mode": mode, "placed_mesh_count": placed},
                     "")


def _stage_set_collection_metadata(spec, root_col, report):
    report.begin_stage("set_collection_metadata")
    root_col["blueprint_path"] = spec["blueprint_path"]
    root_col["blueprint_class"] = spec.get("blueprint_class", "") or ""
    root_col["maplink_standin_mode"] = spec["mode"]
    root_col["maplink_version"] = "1.8"
    report.end_stage("ok", None, "")


def _stage_save_blend(spec, report):
    report.begin_stage("save_blend")
    target = spec["target_blend"]
    os.makedirs(os.path.dirname(target), exist_ok=True)
    # compress=False is REQUIRED — the use_file_compression preference
    # is silently ignored in headless (Blender 5.0 quirk).  Decompressed
    # files also allow downstream sidecar tools to do raw binary edits
    # if ever needed.
    bpy.ops.wm.save_as_mainfile(filepath=target, compress=False)
    report.set_output("blend", target)
    report.standin_blend_path = target
    report.end_stage("ok", {"target": target, "compress": False}, "")


def _stage_write_sidecar_analysis(spec, report):
    report.begin_stage("write_sidecar_analysis")
    path = _write_sidecar_analysis(spec, spec["target_blend"], spec["mode"])
    report.set_output("sidecar_analysis", path)
    report.end_stage("ok", {"path": path}, "")


def _stage_write_sidecar_filehash(spec, report):
    report.begin_stage("write_sidecar_filehash")
    path = _write_sidecar_filehash(spec["target_blend"])
    report.set_output("sidecar_hash", path)
    report.end_stage("ok", {"path": path}, "")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    # Parse args (may itself fail if --spec missing)
    try:
        args = _parse_argv()
    except SystemExit:
        # argparse already printed an error
        raise

    # Load + validate the spec
    try:
        spec = _load_spec(args.spec)
    except Exception as e:
        _stderr(f"[BPStandIn] FATAL: cannot load spec: {e}")
        sys.exit(1)

    # Build the report — report_path comes from the spec so the C++
    # caller knows up front where to read after subprocess exits.
    report = StandInReport(
        operation="create",
        blueprint_path=spec.get("blueprint_path", ""),
        report_path=spec["report_path"],
        blueprint_class=spec.get("blueprint_class"),
    )

    try:
        _stage_load_template(spec, report)
        root_col = _stage_prepare_root(spec, report)
        _stage_build_geometry(spec, root_col, report)
        _stage_set_collection_metadata(spec, root_col, report)
        _stage_save_blend(spec, report)
        _stage_write_sidecar_analysis(spec, report)
        _stage_write_sidecar_filehash(spec, report)

        # If we got here, every stage closed cleanly.  Warnings may have
        # been added (e.g. an FBX missing in extracted mode) — write()
        # downgrades success→partial automatically if errors[] is non-empty.
        report.set_result("partial" if report.warnings else "success")
        report.write()
        sys.exit(0)

    except Exception as e:
        report.add_error(f"{type(e).__name__}: {e}")
        # Close whatever stage was open at fault time as 'error'
        if report._open_name:
            report.end_stage("error", None, f"{type(e).__name__}: {e}")
        traceback.print_exc()
        report.set_result("failure")
        report.write()
        sys.exit(1)


if __name__ == "__main__":
    main()
