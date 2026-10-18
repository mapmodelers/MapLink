#!/usr/bin/env python3
# maplink_project_setup.py — Blender headless scaffolder for the MapLink UE5 plugin.
# Runs under Blender 4.x / 5.x.  SELF-CONTAINED: imports no addon modules
# (launched with --factory-startup), so the minimal sidecar / analysis write
# is replicated inline here (batch_sidecar.py pattern).
#
# v1.19 — MOVED from <plugin>/Scripts/ into the addon package.  It sits beside
# the addon modules but is never imported by them; --factory-startup means no
# addon is loaded when it runs, so the self-containment above still holds.
# The move is what lets a Blender-only artist scaffold a project:
# MapLink_UpdateAddon.cmd installs ue\blender and nothing else, so from
# <plugin>/Scripts this script never reached Blender while its own templates
# (blender/resources) always did.  Invoked from Blender by
# project_roots.scaffold(), and from UE by FMapLinkProjectSetup.
#
# Scaffolds a fresh, PORTABLE, marketplace-clean MapLink .blend set from the
# templates bundled at <plugin>/blender/resources:
#   - directory tree  <root>/{LI,SM,BP,MaterialInstances,Textures}
#   - project-local material libraries (copied together as siblings)
#   - top-level LI_<SetupName> .blend (no level binding — user picks later)
#   - seeded LI_Template, BP_Template and SM_Template (the reusable per-type
#     templates the "Create <type> from template" operators copy from)
#   - manifest  <root>/maplink_project.json  (ALL paths RELATIVE)
#
# Nothing in this script assumes the *contents* of the template .blend files;
# it ENFORCES the required invariants on the produced top-level instead.
#
# Usage (invoked by UE5 via FMapLinkBlenderBridge::RunBlenderScriptGeneric,
# which passes a throwaway .blend to --background; this script ignores it and
# drives its own open_mainfile/save):
#   blender --background <any>.blend --factory-startup \
#           --python <plugin>/blender/maplink_project_setup.py -- \
#           --root <root> --setup-name <SetupName> \
#           --resources-dir <plugin>/blender/resources --unit-scale 1.0 \
#           [--maplink-version 1.9.4] [--mi-collection Library_Material_Instances]

import os
import sys
import json
import stat
import shutil
import hashlib
import argparse
import traceback
import uuid

import bpy

SCHEMA_VERSION = 1
MANIFEST_NAME = "maplink_project.json"

# Templates that MUST exist under --resources-dir for this pass.
REQUIRED_RESOURCES = (
    "Library_Materials.blend",
    "Library_MaterialInstances.blend",
    "LI_Template.blend",
    "SM_Template.blend",
    "BP_Template.blend",
)

# Basename stem (lower-cased prefix) used to recognise the MI library so a
# pre-existing/absolute MI link can be stripped before we re-link fresh.
_MI_BASENAME_PREFIX = "library_materialinstances"


# ---------------------------------------------------------------------------
# Small filesystem / hashing helpers
# ---------------------------------------------------------------------------

def _log(msg):
    print(f"[MapLink Setup] {msg}")


def _ensure_writable(path):
    """Clear the read-only bit.  The bundled resources ship read-only, and a
    read-only target makes save_as_mainfile() return {'CANCELLED'} *silently*
    in background mode (exit 0, no exception)."""
    try:
        os.chmod(path, os.stat(path).st_mode | stat.S_IWRITE | stat.S_IWUSR)
    except Exception as exc:
        _log(f"WARN: could not clear read-only on {path}: {exc}")


def _compute_md5(filepath):
    h = hashlib.md5()
    with open(filepath, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _rel_posix(target_abs, start_dir):
    """Filesystem-relative path with forward slashes (manifest convention)."""
    return os.path.relpath(target_abs, start_dir).replace(os.sep, "/")


def _blender_rel(target_abs, blend_path):
    """A Blender //-relative library path, relative to the .blend's folder."""
    return "//" + _rel_posix(target_abs, os.path.dirname(blend_path))


def _sanitize_token(s):
    """Keep a name safe for a stem / collection / filesystem segment."""
    out = []
    for ch in (s or "").strip():
        if ch.isalnum() or ch in ("_", "-"):
            out.append(ch)
        elif ch.isspace():
            out.append("_")
        # drop everything else (slashes, dots, etc.)
    return "".join(out)


def _copy_template(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)
    _ensure_writable(dst)
    _log(f"copied {os.path.basename(src)} -> {dst}")


# ---------------------------------------------------------------------------
# Top-level .blend invariant enforcement (idempotent)
# ---------------------------------------------------------------------------

def _find_local_collection(name_lower):
    for c in bpy.data.collections:
        if c.library is None and c.name.lower() == name_lower:
            return c
    return None


def _ensure_noexport(scene):
    noexport = _find_local_collection("noexport")
    if noexport is None:
        noexport = bpy.data.collections.new("NoExport")
        scene.collection.children.link(noexport)
        _log("created NoExport collection")
    # Convention: NoExport never renders.
    noexport.hide_render = True
    return noexport


def _rename_root_collection(scene, stem):
    """Rename the asset's root collection to the stem.  The root is taken to
    be the most-populated LOCAL, non-NoExport direct child of the master
    collection (template internals are unknown, so this is discovered, not
    assumed).  Idempotent: a collection already named <stem> is left alone."""
    if _find_local_collection(stem.lower()) is not None:
        return  # already named correctly

    candidates = [
        c for c in scene.collection.children
        if c.library is None and c.name.lower() != "noexport"
    ]
    if candidates:
        root = max(candidates, key=lambda c: len(c.all_objects))
        old = root.name
        root.name = stem
        _log(f"renamed root collection '{old}' -> '{root.name}'")
    else:
        root = bpy.data.collections.new(stem)
        scene.collection.children.link(root)
        _log(f"no root collection found; created '{stem}'")


def _strip_mi_links():
    """Remove any pre-existing Library_MaterialInstances link (objects that
    instance it, then the library datablock itself) so we re-link cleanly."""
    targets = []
    for lib in list(bpy.data.libraries):
        base = os.path.basename(bpy.path.abspath(lib.filepath)).lower()
        if base.startswith(_MI_BASENAME_PREFIX):
            targets.append(lib)
    if not targets:
        return
    target_set = set(targets)
    for obj in list(bpy.data.objects):
        ic = getattr(obj, "instance_collection", None)
        if ic is not None and ic.library in target_set:
            bpy.data.objects.remove(obj, do_unlink=True)
    for lib in targets:
        try:
            bpy.data.libraries.remove(lib)
            _log("stripped pre-existing MI link")
        except Exception as exc:
            _log(f"WARN: could not remove pre-existing MI library: {exc}")


def _link_mi_relative(noexport, mi_abs, hint):
    """Fresh link (link=True) of the MI instance collection from mi_abs, store
    its library path RELATIVE (//...), and place a hidden collection-instance
    empty inside NoExport.  Never uses reload()/user_remap().  Returns the
    linked collection name ('' if the library had no collections)."""
    chosen = None
    try:
        with bpy.data.libraries.load(mi_abs, link=True) as (data_from, data_to):
            names = list(data_from.collections)
            if hint:
                for n in names:
                    if n.lower() == hint.lower():
                        chosen = n
                        break
            if chosen is None:
                for n in names:
                    if "instance" in n.lower():
                        chosen = n
                        break
            if chosen is None and names:
                chosen = names[0]
            if chosen is not None:
                data_to.collections = [chosen]
    except Exception as exc:
        _log(f"WARN: failed to link MI library {mi_abs}: {exc}")
        return ""

    if chosen is None:
        _log(f"WARN: MI library has no collections: {mi_abs}")
        return ""

    # Locate the freshly-linked collection datablock from this library.
    mi_norm = os.path.normpath(mi_abs).lower()
    linked_col = None
    for col in bpy.data.collections:
        if not col.library:
            continue
        lib_norm = os.path.normpath(bpy.path.abspath(col.library.filepath)).lower()
        if lib_norm != mi_norm:
            continue
        if col.name == chosen or col.name.startswith(chosen):
            linked_col = col
            break
        if linked_col is None:
            linked_col = col  # fallback: any collection from this library

    if linked_col is None:
        _log(f"WARN: linked MI collection not found after load: {mi_abs}")
        return ""

    # Force a //-relative library filepath (portable, no absolute user path).
    try:
        linked_col.library.filepath = _blender_rel(mi_abs, bpy.data.filepath)
    except Exception as exc:
        _log(f"WARN: could not set relative MI library path: {exc}")

    anchor = bpy.data.objects.new(f"MI_{linked_col.name}", None)
    anchor.instance_type = "COLLECTION"
    anchor.instance_collection = linked_col
    anchor.hide_select = True
    anchor.hide_viewport = True
    anchor.hide_render = True
    noexport.objects.link(anchor)
    _log(f"linked MI collection '{linked_col.name}' (relative) into NoExport, hidden")
    return linked_col.name


# ---------------------------------------------------------------------------
# Asset catalogs + marking
# ---------------------------------------------------------------------------
# Deterministic catalog UUID (uuid5 of the catalog path). One catalog per
# project, named "MapLink_<Setup>", holds every MapLink asset (LI/SM/BP/preset
# together) so the All Libraries view groups them under a single project node.
_CATALOG_NS = uuid.UUID("6d61706c-696e-6b00-0000-6361746c6f67")


#: v1.20 — the catalog's children, keyed by the folder that owns each.  Kept
#: in step with `asset_catalog.CHILDREN` by hand, on purpose: this script runs
#: --factory-startup as UE's scaffolder and imports no addon module.  The two
#: cannot silently disagree in practice because `asset_catalog` resolves the
#: file this one writes rather than trusting either constant.
_CHILDREN = (
    ("LI", "LI"),
    ("SM", "SM"),
    ("BP", "BP"),
    ("preset", "Presets"),
    ("MaterialInstances", "Materials"),
)


def _catalog_uuid(cat_path):
    return str(uuid.uuid5(_CATALOG_NS, cat_path))


def _child_path(catalog_path, subdir):
    """``"MapLink_X", "preset"`` -> ``"MapLink_X/Presets"``; "" when no child."""
    child = dict(_CHILDREN).get(subdir, "")
    return f"{catalog_path}/{child}" if child else ""


def child_catalog_uuid(catalog_path, subdir):
    """UUID an asset in *subdir* belongs to — the child's, or the parent's."""
    cp = _child_path(catalog_path, subdir)
    return _catalog_uuid(cp) if cp else _catalog_uuid(catalog_path)


def _write_cats_file(dir_path, catalog_path, subdir="."):
    """Write blender_assets.cats.txt into dir_path.  Returns the file path.

    Defines the project catalog, plus this folder's own child when it has one.
    Not the whole set: a library rooted at LI/ that declared SM/BP/Materials
    would show three empty nodes whenever it is viewed on its own.  The
    All-Libraries view builds the full tree from the union.
    """
    cats = os.path.join(dir_path, "blender_assets.cats.txt")
    paths = [catalog_path]
    child = _child_path(catalog_path, subdir)
    if child:
        paths.append(child)
    lines = [
        "# This is an Asset Catalog Definition file for Blender.",
        "#",
        "# Empty lines and lines starting with `#` are ignored.",
        "# The first non-ignored line should be the version indicator.",
        "# Other lines are of the format",
        '# "UUID:catalog/path/for/tree/view:simple catalog name"',
        "",
        "VERSION 1",
        "",
    ]
    lines += [f"{_catalog_uuid(cp)}:{cp}:{cp.replace('/', '-')}" for cp in paths]
    with open(cats, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    return cats


def write_catalog_files(root, catalog_path):
    """Write the catalog definitions into EACH per-type library root plus the
    project root.

    Required because each asset library is rooted at a type subfolder and
    Blender only resolves blender_assets.cats.txt at/below a library's own
    root — a single file at the project root is invisible to a library rooted
    at <root>/LI, which is what left every asset 'Unassigned'.

    v1.20: MaterialInstances/ joins the list (its material instances are assets
    now) and every file also declares its own child, so the browser tree reads
    MapLink_<Setup> ▸ LI / SM / BP / Presets / Materials instead of one flat
    node holding a couple of hundred assets."""
    written = []
    for sub, _child in _CHILDREN:
        d = os.path.join(root, sub)
        if os.path.isdir(d):
            written.append(_write_cats_file(d, catalog_path, sub))
    written.append(_write_cats_file(root, catalog_path, "."))
    for w in written:
        _log(f"catalog file written: {w}")
    return written


def _mark_collection_asset(coll, catalog_uuid):
    """Mark a collection as an asset under catalog_uuid. Best-effort."""
    try:
        if getattr(coll, "asset_data", None) is None:
            coll.asset_mark()
        ad = getattr(coll, "asset_data", None)
        if ad is not None:
            ad.catalog_id = catalog_uuid
            try:
                ad.author = "MapLink"
            except Exception:
                pass
        _log(f"marked asset collection '{coll.name}'")
        return True
    except Exception as exc:
        _log(f"WARN: could not mark collection '{getattr(coll, 'name', '?')}': {exc}")
        return False


def assign_catalog_to_marked(blend_path, catalog_uuid):
    """Open a copied template and assign `catalog_uuid` to the data-blocks the
    TEMPLATE AUTHOR has already marked as assets (collections and objects). Does
    NOT mark anything new — collections the author left unmarked (e.g. preset
    helper collections) are untouched. Never fatal."""
    if not (blend_path and os.path.isfile(blend_path)):
        return
    try:
        _ensure_writable(blend_path)
        bpy.ops.wm.open_mainfile(filepath=blend_path)
        touched = 0
        for datablocks in (bpy.data.collections, bpy.data.objects):
            for db in datablocks:
                ad = getattr(db, "asset_data", None)
                if ad is None:                      # not marked by the author
                    continue
                try:
                    ad.catalog_id = catalog_uuid
                    if not getattr(ad, "author", ""):
                        ad.author = "MapLink"
                    touched += 1
                except Exception as exc:            # noqa: BLE001
                    _log(f"WARN: catalog assign failed on "
                         f"'{getattr(db, 'name', '?')}': {exc}")
        if touched == 0:
            _log(f"note: no author-marked asset in "
                 f"{os.path.basename(blend_path)} (nothing catalogued)")
        else:
            _log(f"catalogued {touched} author-marked asset(s) in "
                 f"{os.path.basename(blend_path)}")
        _ensure_writable(blend_path)
        result = bpy.ops.wm.save_as_mainfile(filepath=blend_path, compress=False)
        if "FINISHED" not in result:
            _log(f"WARN: catalog save returned {result} for {blend_path}")
    except Exception as exc:                        # noqa: BLE001
        _log(f"WARN: catalog assignment failed for {blend_path}: {exc}\n"
             f"{traceback.format_exc()}")


def repath_mi_to_materials(mi_blend, materials_basename="Library_Materials.blend"):
    """Force the copied Library_MaterialInstances.blend's link to the parent
    shader library to a SIBLING relative path (//Library_Materials.blend), so it
    resolves no matter where the project lives. Belt-and-suspenders on top of
    authoring the template with a sibling link — this is the durable fix for the
    'parent shaders missing after copy' bug. Never fatal."""
    if not (mi_blend and os.path.isfile(mi_blend)):
        return
    try:
        _ensure_writable(mi_blend)
        bpy.ops.wm.open_mainfile(filepath=mi_blend)
        target = "//" + materials_basename
        fixed = 0
        for lib in bpy.data.libraries:
            raw = (lib.filepath or "").replace("\\", "/")
            base = raw.rsplit("/", 1)[-1]
            if base.lower() == materials_basename.lower() and lib.filepath != target:
                lib.filepath = target
                fixed += 1
                try:
                    lib.reload()
                except Exception as exc:           # noqa: BLE001
                    _log(f"note: lib.reload() failed (path still rewritten): {exc}")
        if fixed:
            _log(f"repathed {fixed} parent-shader link -> {target} in "
                 f"{os.path.basename(mi_blend)}")
        else:
            _log(f"note: no parent-shader link needed repathing in "
                 f"{os.path.basename(mi_blend)} (already sibling or absent)")
        _ensure_writable(mi_blend)
        result = bpy.ops.wm.save_as_mainfile(filepath=mi_blend, compress=False)
        if "FINISHED" not in result:
            _log(f"WARN: MI repath save returned {result} for {mi_blend}")
    except Exception as exc:                        # noqa: BLE001
        _log(f"WARN: MI repath failed for {mi_blend}: {exc}\n"
             f"{traceback.format_exc()}")


def enforce_top_level(dest_blend, stem, unit_scale, mi_abs, mi_hint,
                      li_catalog_uuid=None):
    """Open the produced top-level .blend and enforce all invariants, then
    save it decompressed.  Returns the linked MI collection name ('' if none)."""
    _ensure_writable(dest_blend)
    bpy.ops.wm.open_mainfile(filepath=dest_blend)

    scene = bpy.context.scene

    # Units: Metric, scale = unit_scale.
    scene.unit_settings.system = "METRIC"
    scene.unit_settings.scale_length = float(unit_scale)

    # Root collection -> stem.
    _rename_root_collection(scene, stem)

    # NoExport collection.
    noexport = _ensure_noexport(scene)

    # MI link: strip any pre-existing, then link fresh + relative into NoExport.
    _strip_mi_links()
    mi_collection = _link_mi_relative(noexport, mi_abs, mi_hint)

    # Mark the top-level LI_ collection as a catalogued asset (best-effort).
    if li_catalog_uuid:
        root_coll = bpy.data.collections.get(stem)
        if root_coll is not None:
            _mark_collection_asset(root_coll, li_catalog_uuid)

    # Save (decompressed) and verify it actually wrote.
    _ensure_writable(dest_blend)
    result = bpy.ops.wm.save_as_mainfile(filepath=dest_blend, compress=False)
    if "FINISHED" not in result:
        raise RuntimeError(
            f"save_as_mainfile returned {result} (silent CANCELLED?) for {dest_blend}")
    if (not os.path.isfile(dest_blend)) or os.path.getsize(dest_blend) == 0:
        raise RuntimeError(f"saved top-level missing or empty: {dest_blend}")
    _log(f"saved top-level: {dest_blend}")
    return mi_collection


# ---------------------------------------------------------------------------
# Sidecar (analysis.json + filehash.txt) — inline, batch_sidecar.py pattern
# ---------------------------------------------------------------------------

def write_sidecar(blend_path, stem, unit_scale):
    """A freshly-scaffolded top-level has no exportable geometry and no
    MapLink-prefixed linked assets (the only link is the NoExport-excluded MI
    library), so the analysis arrays are empty by construction.  Keeping them
    empty also guarantees no absolute user path leaks into the sidecar."""
    blend_dir = os.path.dirname(blend_path)
    analysis_path = os.path.join(blend_dir, f"{stem}_analysis.json")
    hash_path = os.path.join(blend_dir, f"{stem}_filehash.txt")

    report = {
        "blend_name": stem,
        "unit_scale": float(unit_scale),
        "blend_own_geo": [],
        "linked_assets": [],
        "libraries": [],
    }
    with open(analysis_path, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2)

    file_hash = _compute_md5(blend_path)
    with open(hash_path, "w", encoding="utf-8") as f:
        f.write(file_hash)

    _log(f"sidecar written: {analysis_path}")
    _log(f"sidecar hash: {hash_path} ({file_hash})")


# ---------------------------------------------------------------------------
# Manifest (write or append) — all paths RELATIVE to the manifest dir (root)
# ---------------------------------------------------------------------------

def write_manifest(root, setup_name, stem, top_rel, mi_collection,
                   instance_content_path, maplink_version, unit_scale):
    manifest_path = os.path.join(root, MANIFEST_NAME)

    manifest = {}
    if os.path.isfile(manifest_path):
        try:
            with open(manifest_path, "r", encoding="utf-8") as f:
                manifest = json.load(f)
        except Exception as exc:
            _log(f"WARN: existing manifest unreadable, rewriting: {exc}")
            manifest = {}

    manifest["schema_version"] = SCHEMA_VERSION
    manifest["maplink_version"] = maplink_version
    manifest["project_name"] = setup_name
    manifest["unit_scale"] = float(unit_scale)
    manifest["material_library"] = {
        "materials_blend_rel": "MaterialInstances/Library_Materials.blend",
        "instances_blend_rel": "MaterialInstances/Library_MaterialInstances.blend",
        "instances_collection": mi_collection or "Library_Material_Instances",
        "instance_content_path": instance_content_path,
    }

    # No level binding at setup time — the user picks the level later.
    entry = {
        "stem": stem,
        "rel_path": top_rel,
    }
    blends = manifest.get("top_level_blends")
    if not isinstance(blends, list):
        blends = []
    # Append; replace any existing entry with the same stem (Add-Level safe).
    blends = [b for b in blends if b.get("stem") != stem]
    blends.append(entry)
    manifest["top_level_blends"] = blends

    with open(manifest_path, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
    _log(f"manifest written: {manifest_path}")
    return manifest_path


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def _parse_args():
    if "--" not in sys.argv:
        raise RuntimeError("Expected '--' separator in command-line arguments.")
    args = sys.argv[sys.argv.index("--") + 1:]
    ap = argparse.ArgumentParser(description="MapLink 'Setup New .blends' (headless)")
    ap.add_argument("--root", required=True, help="Blender setup root directory")
    ap.add_argument("--setup-name", dest="setup_name", required=True,
                    help="Setup name (project id: root folder, library names, "
                         "manifest project_name, MI level)")
    ap.add_argument("--li-stem", dest="li_stem", default="",
                    help="Top-level LI stem, already prefixed (e.g. LI_Hub). "
                         "Falls back to LI_<SetupName> if omitted.")
    ap.add_argument("--resources-dir", required=True,
                    help="<plugin>/blender/resources (bundled templates)")
    ap.add_argument("--unit-scale", type=float, default=1.0)
    ap.add_argument("--maplink-version", default="1.9.4")
    ap.add_argument("--mi-collection", default="Library_Material_Instances",
                    help="Expected MI instance-collection name (discovery hint only)")
    ap.add_argument("--instance-content-path", default="/Game/MaterialInstances/")
    return ap.parse_args(args)


def main():
    args = _parse_args()

    root = os.path.abspath(args.root)
    setup_name = _sanitize_token(args.setup_name)
    if not setup_name:
        _log(f"ERROR: invalid setup name after sanitize "
             f"(setup_name='{args.setup_name}').")
        print("MAPLINK_SETUP_FAIL")
        sys.exit(1)

    # One catalog per project, named after the setup.
    catalog_path = f"MapLink_{setup_name}"

    res_dir = os.path.abspath(args.resources_dir)
    if not os.path.isdir(res_dir):
        _log(f"ERROR: resources dir not found: {res_dir}")
        print("MAPLINK_SETUP_FAIL")
        sys.exit(1)
    missing = [r for r in REQUIRED_RESOURCES
               if not os.path.isfile(os.path.join(res_dir, r))]
    if missing:
        _log(f"ERROR: resources dir missing template(s): {missing} (in {res_dir})")
        print("MAPLINK_SETUP_FAIL")
        sys.exit(1)

    # Top-level stem: prefer the explicit LI stem from the dialog; fall back to
    # LI_<SetupName>.  Normalise the LI_ prefix (don't double it).
    li_in = _sanitize_token(args.li_stem) if args.li_stem else ""
    if li_in:
        if li_in.lower().startswith("li_"):
            li_in = li_in[3:]
        stem = f"LI_{li_in}" if li_in else ""
    else:
        stem = ""
    if not stem:
        stem = f"LI_{setup_name}"

    # 1. Directory tree.  Create the root explicitly first and probe that it is
    #    actually writable — on machines where the chosen location is under
    #    OneDrive Known-Folder-Move or Controlled Folder Access, the external
    #    blender.exe is blocked from creating folders there and os.makedirs can
    #    fail with a confusing WinError 2 ("file not found") on a phantom dir.
    #    Fail cleanly with a readable hint instead of an unhandled traceback.
    try:
        os.makedirs(root, exist_ok=True)
        _probe = os.path.join(root, ".maplink_write_test")
        with open(_probe, "w", encoding="utf-8") as _f:
            _f.write("ok")
        os.remove(_probe)
    except OSError as exc:
        _log(f"ERROR: setup root not creatable/writable: {root} ({exc}). "
             f"If this path is under OneDrive/Documents or Controlled Folder "
             f"Access, choose a local non-synced folder (e.g. C:\\MapLink\\{setup_name}).")
        print("MAPLINK_SETUP_FAIL")
        sys.exit(1)

    try:
        for sub in ("LI", "SM", "BP", "MaterialInstances", "Textures", "preset"):
            os.makedirs(os.path.join(root, sub), exist_ok=True)
    except OSError as exc:
        _log(f"ERROR: failed creating subfolders under {root} ({exc}).")
        print("MAPLINK_SETUP_FAIL")
        sys.exit(1)
    _log(f"ensured tree under {root}")

    # 2. Copy both material libraries together as siblings (no rename/repath).
    mi_dir = os.path.join(root, "MaterialInstances")
    _copy_template(os.path.join(res_dir, "Library_Materials.blend"),
                   os.path.join(mi_dir, "Library_Materials.blend"))
    _copy_template(os.path.join(res_dir, "Library_MaterialInstances.blend"),
                   os.path.join(mi_dir, "Library_MaterialInstances.blend"))
    mi_abs = os.path.join(mi_dir, "Library_MaterialInstances.blend")

    # Normalise the parent-shader link to a sibling path before anything links
    # the MI library into the LI (so the whole chain resolves regardless of
    # where the project lives or how the template was authored).
    repath_mi_to_materials(mi_abs)

    # 3. Top-level LI_ at <root>/LI/<stem>/<stem>.blend.
    top_rel = f"LI/{stem}/{stem}.blend"
    top_blend = os.path.join(root, "LI", stem, f"{stem}.blend")
    _copy_template(os.path.join(res_dir, "LI_Template.blend"), top_blend)

    # 4. Seed LI_Template, BP_Template and SM_Template so the "Create <type>
    #    from template" quick-create operators can find them at
    #    <root>/<TYPE>/<TYPE>_Template/<TYPE>_Template.blend. The top-level LI_
    #    above is a *named* instance copied from LI_Template; this seeds the
    #    reusable LI template itself (previously missing — which is why
    #    "Create LI from template" reported 'LI_Template not found').
    _copy_template(os.path.join(res_dir, "LI_Template.blend"),
                   os.path.join(root, "LI", "LI_Template", "LI_Template.blend"))
    _copy_template(os.path.join(res_dir, "BP_Template.blend"),
                   os.path.join(root, "BP", "BP_Template", "BP_Template.blend"))
    _copy_template(os.path.join(res_dir, "SM_Template.blend"),
                   os.path.join(root, "SM", "SM_Template", "SM_Template.blend"))

    # 4b. Seed preset_template.blend (best-effort — presets are APPENDED, not
    #     linked; see the asset-library notes). Non-fatal if not deployed.
    preset_src = os.path.join(res_dir, "preset_template.blend")
    preset_dst = os.path.join(root, "preset", "preset_template.blend")
    if os.path.isfile(preset_src):
        _copy_template(preset_src, preset_dst)
    else:
        preset_dst = ""
        _log("WARN: preset_template.blend not found in resources; "
             "skipping preset seed (preset/ folder still created).")

    # 5. Enforce invariants on the new top-level (marks the LI_ collection).
    mi_collection = enforce_top_level(
        top_blend, stem, args.unit_scale, mi_abs, args.mi_collection,
        li_catalog_uuid=child_catalog_uuid(catalog_path, "LI"))

    # 6. Sidecar.
    write_sidecar(top_blend, stem, args.unit_scale)

    # 6b. Asset catalogs + catalog assignment on author-marked template assets.
    #     (LI root is auto-marked in enforce_top_level; SM/BP/preset rely on the
    #     template author having marked the intended collection — unmarked helper
    #     collections, e.g. in preset_template, are deliberately left alone.)
    write_catalog_files(root, catalog_path)
    assign_catalog_to_marked(
        os.path.join(root, "LI", "LI_Template", "LI_Template.blend"),
        child_catalog_uuid(catalog_path, "LI"))
    assign_catalog_to_marked(
        os.path.join(root, "BP", "BP_Template", "BP_Template.blend"),
        child_catalog_uuid(catalog_path, "BP"))
    assign_catalog_to_marked(
        os.path.join(root, "SM", "SM_Template", "SM_Template.blend"),
        child_catalog_uuid(catalog_path, "SM"))
    if preset_dst:
        assign_catalog_to_marked(preset_dst,
                                 child_catalog_uuid(catalog_path, "preset"))

    # 7. Manifest.
    manifest_path = write_manifest(
        root, setup_name, stem, top_rel, mi_collection,
        args.instance_content_path, args.maplink_version, args.unit_scale)

    # Machine-parseable final summary line + success token.
    summary = {
        "ok": True,
        "root": root,
        "manifest": manifest_path,
        "top_level_blend": top_blend,
        "top_level_stem": stem,
        "mi_collection": mi_collection,
        "materials_blend": os.path.join(mi_dir, "Library_Materials.blend"),
        "instances_blend": mi_abs,
        "li_template": os.path.join(root, "LI", "LI_Template", "LI_Template.blend"),
        "sm_template": os.path.join(root, "SM", "SM_Template", "SM_Template.blend"),
        "preset_template": preset_dst,
    }
    print("MAPLINK_SETUP_RESULT " + json.dumps(summary))
    print("MAPLINK_SETUP_OK")


if __name__ == "__main__":
    try:
        main()
    except SystemExit:
        raise
    except Exception:
        traceback.print_exc()
        print("MAPLINK_SETUP_FAIL")
        sys.exit(1)
