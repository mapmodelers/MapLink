#!/usr/bin/env python3
"""
maplink_material_sync.py — Blender headless script for MapLink Material Sync.

Parent materials are LINKED in Library_MaterialInstances.blend from
Library_Materials.blend. They already have the full shader wired up
(node group → Material Output, including alpha). To create an MI we
duplicate the linked parent material, rename it, and set parameters.

Palette cubes + MI materials live in the 'Library_Material_Instances'
collection — this collection is linked into consumer scenes, so every
sync must leave it clean (no orphans from previously-deleted MIs).

Each sync:
  1. Verifies 'Library_Material_Instances' collection exists (error if not).
  2. Deletes every object in the collection EXCEPT 'mat_cube', plus their
     local mesh/material data (orphans only).
  3. Rebuilds MI materials + palette cubes from the JSON.

'mat_cube' is NEVER touched by sync — it's the template source and must
always be present in 'Library_Material_Instances'.

Called via:
    blender --background "<MaterialInstancesBlend>" --factory-startup \
            --python maplink_material_sync.py -- \
            --json "<path>/material_instances_all.json" \
            --materials-lib "<MaterialsLibraryBlend>" \
            --logdir "<PluginDir>/Logs/material_sync/" \
            --mode execute|dry-run
"""

import bpy
import json
import sys
import os
import argparse
import traceback

# The sync runs --background --factory-startup, so there is no addon package to
# import from. asset_catalog lives in the addon package next door and is
# deliberately bare-importable for exactly this; the same insertion
# maplink_create_bp_standin uses, and on the same grounds — this script is only
# ever run by UE from the plugin directory, where that sibling is present.
try:
    sys.path.insert(0, os.path.join(
        os.path.dirname(os.path.abspath(__file__)), os.pardir, "blender"))
    import asset_catalog as _ac                          # noqa: E402
except Exception as _exc:                                # noqa: BLE001
    _ac = None
    print(f"[MatSync] asset_catalog unavailable ({_exc}) — material instances "
          f"will be created but not marked as assets", flush=True)

try:
    import thumbnail_badge as _badge                     # noqa: E402
except Exception as _exc:                                # noqa: BLE001
    _badge = None
    print(f"[MatSync] thumbnail_badge unavailable ({_exc}) — MI previews will "
          f"carry no type badge", flush=True)

# ──────────────────────────────────────────────────────────────────────
# Globals
# ──────────────────────────────────────────────────────────────────────

COLUMNS = 10
SPACING = 2.5

# Collection in Library_MaterialInstances.blend that is LINKED into consumer scenes.
# Every MI object (palette cube) must live inside this collection, and the
# collection must already exist in the .blend file (we do NOT create it).
LIBRARY_COLLECTION = "Library_Material_Instances"

# Template object/mesh that is never touched by sync — must always be present
# inside LIBRARY_COLLECTION so MI cubes can be duplicated from it.
TEMPLATE_NAME = "mat_cube"

_log_lines = []
_warnings = []
_errors = []

# v1.20 — material instances are Asset Browser assets. Counted rather than
# logged one by one: 86 "marked" lines would bury the warnings that matter.
_asset_counts = {"marked": 0, "previewed": 0, "no_thumbnail": 0,
                 "preview_failed": 0, "badged": 0}

# ──────────────────────────────────────────────────────────────────────
# Logging
# ──────────────────────────────────────────────────────────────────────

# Images that SURVIVED the orphan purge, with their user counts. In the report
# because the script's stdout does not reach the editor log: when the library
# still holds something it should not, this list is the only thing that says
# whether it survived because something references it or because the purge rule
# was wrong. Not having it cost a full round-trip.
_images_kept = []
_cleanup_counts = {}

# Absolute texture path -> Image datablock, for this run only. See the load
# site for why bpy's own check_existing cannot do this job here.
_image_cache = {}


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


def log(msg):
    """Print to stdout AND buffer for the report."""
    print(msg, flush=True)
    _log_lines.append(msg)


def log_warning(mi_name, msg):
    log(f"[MatSync]   WARNING: {msg}")
    _warnings.append({"mi": mi_name, "message": msg})


def log_error(mi_name, msg):
    log(f"[MatSync]   ERROR: {msg}")
    _errors.append({"mi": mi_name, "message": msg})


# ──────────────────────────────────────────────────────────────────────
# Helpers
# ──────────────────────────────────────────────────────────────────────

def _mi_catalog_uuid():
    """The catalog every MI in this library belongs to.  "" when unresolvable.

    `Library_MaterialInstances.blend` lives in `<root>/MaterialInstances/`, so
    asset_catalog resolves the project's `Materials` child from the folder
    without being told anything.  Resolved once: it reads files off disk.
    """
    if _ac is None:
        return ""
    try:
        uuid = _ac.uuid_for_blend(bpy.data.filepath)
        if uuid:
            root = _ac.root_for_blend(bpy.data.filepath)
            if root:
                # MaterialInstances/ ONLY. The definition files in LI/, SM/,
                # BP/ and preset/ are checked into Perforce and are not this
                # script's to rewrite — Repair Asset Catalogs opens them for
                # edit first, which is why it, and not the sync, owns them.
                _ac.ensure_cats_files(root, subdirs=("MaterialInstances",),
                                      force_writable=True)
                for b in getattr(_ac.ensure_cats_files, "blocked", []):
                    log_warning("<catalog>", f"read-only, not written: {b}")
        return uuid
    except Exception as exc:                             # noqa: BLE001
        log(f"[MatSync] catalog unresolved ({exc}) — MIs will be Unassigned")
        return ""


def _apply_thumbnail(mat, tga_path):
    """Point *mat*'s asset preview at the thumbnail UE rendered.  -> bool.

    Material previews normally come from Blender's RENDER_PREVIEW job, which
    needs a window — and this script is --background.  Writing the pixels
    straight into the preview sidesteps the render entirely, which is the only
    reason previews are affordable here at all: UE already renders these for
    its own Content Browser, so the picture exists before Blender is launched.

    `image_pixels_float` round-trips through 8-bit storage (0.25 reads back as
    64/255). That is quantisation, not failure — measured on Blender 5.0.
    """
    img = None
    try:
        img = bpy.data.images.load(tga_path, check_existing=False)
        w, h = img.size
        if w <= 0 or h <= 0:
            return False
        mat.preview_ensure()
        mat.preview.image_size = (w, h)
        mat.preview.image_pixels_float = img.pixels[:]
        return True
    except Exception as exc:                             # noqa: BLE001
        log_warning(mat.name, f"preview failed: {exc}")
        return False
    finally:
        # The library reports every surviving image and its user count for a
        # reason — a stray datablock in this file has cost a round-trip before.
        if img is not None:
            try:
                bpy.data.images.remove(img)
            except Exception:                            # noqa: BLE001
                pass


def _thumbnail_path(entry, mi_name, mi_dir):
    """Where UE put this MI's thumbnail, or "" when it did not.

    `"thumbnail"` is stored relative to the MI directory, the same rule and for
    the same reason as `rel` in the schema_version 2 texture entries: the path
    has to survive the project being on a different drive on somebody else's
    machine.
    """
    rel = str(entry.get("thumbnail", "") or "").strip()
    cand = (os.path.normpath(os.path.join(mi_dir, rel)) if rel
            else os.path.join(mi_dir, "Thumbnails", f"{mi_name}.tga"))
    return cand if os.path.isfile(cand) else ""


def _publish_as_asset(mi_mat, entry, catalog_uuid, mi_dir):
    """Mark, catalogue, describe and preview one MI.  Never fatal.

    Every failure here is counted and carried in the report rather than raised.
    A sync that produced correct materials must not be failed by a picture.
    """
    if _ac is None:
        return
    try:
        _ac.mark_and_assign(mi_mat, catalog_uuid=catalog_uuid,
                            tags=("MaterialInstance",))
        ad = getattr(mi_mat, "asset_data", None)
        if ad is None:
            return
        _asset_counts["marked"] += 1
        try:
            parent = entry.get("MaterialParent", "?")
            ad.description = (f"Unreal material instance — read-only in "
                              f"Blender. Parent: {parent}")
        except Exception:                                # noqa: BLE001
            pass

        tga = _thumbnail_path(entry, mi_mat.name, mi_dir)
        if not tga:
            _asset_counts["no_thumbnail"] += 1
        elif _apply_thumbnail(mi_mat, tga):
            _asset_counts["previewed"] += 1
            # Badge it the same way every other MapLink asset is badged, in
            # the same corner, from the same icon set. thumbnail_badge is the
            # one place that decides placement, so an MI tile and an LI tile
            # cannot drift apart.
            #
            # The key is passed explicitly: _classify() reads a .blend STEM
            # (LI_/SM_/BP_), and a material is named MI_<something> with no
            # file of its own, so it would classify as nothing.
            if _badge is not None:
                try:
                    ok, why = _badge.apply_badge(mi_mat, key="MATERIAL_INSTANCE")
                    if ok:
                        _asset_counts["badged"] += 1
                    else:
                        log(f"[MatSync]   badge skipped for {mi_mat.name}: {why}")
                except Exception as exc:                 # noqa: BLE001
                    log(f"[MatSync]   badge failed for {mi_mat.name}: {exc}")
        else:
            _asset_counts["preview_failed"] += 1
    except Exception as exc:                             # noqa: BLE001
        log_warning(getattr(mi_mat, "name", "?"), f"asset marking failed: {exc}")


def _find_group_node(mat):
    """Find the first ShaderNodeGroup in a material's node tree."""
    if mat.node_tree is None:
        return None
    for node in mat.node_tree.nodes:
        if node.type == 'GROUP' and node.node_tree is not None:
            return node
    return None


def _read_texture_value(value):
    """
    v1.16 — one TextureParameters value, in either shape.

    Before v1.16 the value was a bare absolute path string. It is now an
    object, because a path alone cannot say what colour space the pixels are
    in — and that missing fact is the bug this drop exists to fix. Blender
    guesses from the file extension and assumes sRGB for TGA, which is right
    for albedo and wrong for every mask, blend weight and normal map. UE knows
    the answer (it is a flag on the asset), so UE now says it.

    Returns (abs_path, rel_path, srgb, status). rel_path is a "//"-prefixed
    Blender-relative path when UE could compute one, else None.

    Both shapes are accepted forever: an older material_instances.json is
    still a valid thing to sync from, and a reader that dropped every texture
    on an unexpected shape would present as "the materials came back blank".
    """
    if isinstance(value, str):
        return value, None, None, "Unknown"

    if isinstance(value, dict):
        return (value.get("path") or "",
                value.get("rel") or None,
                value.get("srgb"),
                value.get("status") or "Unknown")

    return "", None, None, "Unknown"


def _apply_colorspace(img, srgb, param_name, mi_name):
    """
    Set the image's colour space from UE's sRGB flag.

    srgb is None only for a pre-v1.16 JSON, where UE did not record it. In
    that case we do NOT guess: leaving Blender's own default in place is the
    old (wrong for masks) behaviour, but inventing an answer would be worse —
    it would silently change how existing libraries render on resync.

    'sRGB' and 'Non-Color' are the two names that exist in every Blender
    colour-management config, including the Filmic/AgX variants. Anything
    stranger is a configured OCIO the user chose, and we do not fight it: an
    assignment that raises is reported, not swallowed.
    """
    if srgb is None:
        return None

    wanted = "sRGB" if srgb else "Non-Color"
    try:
        if img.colorspace_settings.name != wanted:
            img.colorspace_settings.name = wanted
        return wanted
    except (TypeError, AttributeError) as e:
        log_warning(mi_name,
                    f"'{param_name}': could not set colour space to "
                    f"'{wanted}' on '{img.name}' ({e}). This image will "
                    f"render with the wrong gamma.")
        return None


def _find_or_create_image_node(node_tree, group_node, input_name):
    """
    Find an existing Image Texture node wired to group_node.inputs[input_name],
    or create one and wire it up.
    """
    socket = group_node.inputs.get(input_name)
    if socket is None:
        return None

    # Check existing links
    for link in node_tree.links:
        if link.to_socket == socket and link.from_node.type == 'TEX_IMAGE':
            return link.from_node

    # Create new node
    tex_node = node_tree.nodes.new('ShaderNodeTexImage')
    tex_node.name = f"Tex_{input_name}"
    tex_node.label = input_name
    # Position to the left of the group node
    tex_node.location = (group_node.location[0] - 350,
                         group_node.location[1] - len(node_tree.nodes) * 40)

    # Wire Color output → group input socket
    node_tree.links.new(tex_node.outputs['Color'], socket)

    return tex_node


def _group_input_internally_used(group_node, input_name):
    """Inside the group's own node tree, is `input_name` actually routed
    anywhere (Group Input → ...)? Returns True/False, or None if unknown.

    If False, the group exposes the socket but never connects it to the
    shader — so feeding it a texture renders nothing (blank). That is a
    content gap in the parent node group (Library_Materials.blend), not
    something this sync can wire up from the consumer side."""
    gt = getattr(group_node, "node_tree", None)
    if gt is None:
        return None
    for node in gt.nodes:
        if node.type == 'GROUP_INPUT':
            out = node.outputs.get(input_name)
            if out is not None:
                return bool(out.is_linked)
    return False


# ──────────────────────────────────────────────────────────────────────
# Main
# ──────────────────────────────────────────────────────────────────────

def _relativise_materials_library(abs_target, rel_target):
    """Store the Library_Materials link as a portable '//' path before saving.

    v1.19.  Measured on a 185-asset production project: this ONE edge accounted
    for 180 of its 900 library links being absolute, inside the file 182 of the
    185 assets depend on.  A teammate syncing the project got a missing library at
    the root of its material graph, and nothing on the Blender side could fix
    it: UE regenerates this file headless, with the addon unloaded, so the
    addon's save-time relativiser never runs on it.

    How it got absolute, from the sync log rather than by guessing: the file on
    disk held '//Library_Materials.blend'.  The regen opens it inside
    <LibDir>/.maplink_sync_tmp/, so that relative path resolves one level too
    deep, the library arrives broken and its datablocks report missing.  The
    worker then re-links every parent material from the ABSOLUTE path it was
    given, and that is the entry that gets saved.  Nobody chose the absolute
    path; it is what the recovery leaves behind, every single run.

    Which is also why the path is handed to us instead of computed here:
    bpy.path.relpath() would describe the TEMP file's position, validate during
    the sync, and break the moment the atomic replace moves the file upward.
    Only UE knows the destination, so UE does the arithmetic
    (FMapLinkMaterialExporter::MakeLibraryRelativePath), the same division of
    labour v1.16 settled on for texture paths.

    The rewrite is the LAST thing before the save on purpose: while the worker
    is running, the absolute path is what makes the linked parents resolve out
    of the temp directory.  Setting .filepath does not reload the library, so
    nothing re-resolves between here and the write.
    """
    if not rel_target:
        log("[MatSync] WARN: no portable path supplied for the materials "
            "library; the absolute link stays and this file will not resolve "
            "on another machine.")
        return 0

    want = os.path.normcase(os.path.abspath(abs_target))
    basename = os.path.basename(abs_target).lower()

    resolving, stale = [], []
    for lib in bpy.data.libraries:
        if os.path.basename(bpy.path.abspath(lib.filepath)).lower() != basename:
            continue
        try:
            here = os.path.normcase(os.path.abspath(bpy.path.abspath(lib.filepath)))
        except Exception:
            continue
        (resolving if here == want else stale).append(lib)

    # The stale sibling. Opening the file inside .maplink_sync_tmp/ resolves an
    # existing '//Library_Materials.blend' one level too deep, so it arrives
    # broken and its datablocks come up missing; the worker then re-links every
    # parent from the absolute path, which is the entry that gets saved. Both
    # end up in the file. Left alone, this function would point the good one at
    # the same path the dead one claims, and the file would carry two entries
    # for one library.
    for lib in stale:
        log(f"[MatSync] Stale materials-library link (resolved into the temp "
            f"directory): {lib.filepath}")
        try:
            bpy.data.libraries.remove(lib)
            log("[MatSync]   removed; its datablocks were re-linked from the "
                "absolute path earlier in this run.")
        except Exception as e:
            log(f"[MatSync]   WARN: could not remove it ({e}); the saved file "
                f"will carry a duplicate library entry.")

    changed = 0
    for lib in resolving:
        if lib.filepath == rel_target:
            log(f"[MatSync] Materials library already portable: {rel_target}")
            continue
        try:
            log(f"[MatSync] Materials library link: {lib.filepath} -> {rel_target}")
            lib.filepath = rel_target
            changed += 1
        except Exception as e:
            log(f"[MatSync] WARN: could not relativise the materials library "
                f"link ({e}); the absolute path stays.")

    if not resolving:
        log("[MatSync] WARN: the materials library is not among this file's "
            "links; nothing to relativise.")
    return changed


def main():
    # ── Parse args ─────────────────────────────────────────────────
    argv = sys.argv
    if '--' in argv:
        argv = argv[argv.index('--') + 1:]
    else:
        argv = []

    ap = argparse.ArgumentParser(description="MapLink Material Sync")
    ap.add_argument('--json', required=True, help="Path to material_instances_all.json")
    ap.add_argument('--materials-lib', required=True, help="Path to Library_Materials.blend")
    ap.add_argument('--materials-lib-rel', dest='materials_lib_rel', default='',
                    help="v1.19 - the '//'-relative form of --materials-lib as "
                         "seen from the library's FINAL directory, computed in "
                         "UE. Stamped onto the library link just before saving; "
                         "see _relativise_materials_library().")
    ap.add_argument('--logdir', required=True, help="Output directory for report JSON")
    ap.add_argument('--mode', default='execute', choices=['execute', 'dry-run'],
                    help="'execute' or 'dry-run'")
    opts = ap.parse_args(argv)

    dry_run = (opts.mode == 'dry-run')
    materials_lib_path = os.path.abspath(opts.materials_lib)
    materials_lib_rel = (opts.materials_lib_rel or '').strip()
    json_path = os.path.abspath(opts.json)
    log_dir = os.path.abspath(opts.logdir)
    os.makedirs(log_dir, exist_ok=True)

    # ── Startup logging ────────────────────────────────────────────
    log(f"[MatSync] MaterialInstances blend: {bpy.data.filepath}")
    log(f"[MatSync] Materials library: {materials_lib_path}")
    if materials_lib_rel:
        log(f"[MatSync] Materials library (portable): {materials_lib_rel}")
    log(f"[MatSync] JSON input: {json_path}")
    log(f"[MatSync] Mode: {opts.mode}")

    # ── Pre-flight: target .blend must be writable on disk ─────────
    # If the .blend is on Perforce and not checked out, it is read-only.
    # bpy.ops.wm.save_mainfile() in --background mode silently CANCELs
    # in that case (Blender exits 0), so detect this up-front and fail
    # with a clear error code instead of pretending the sync worked.
    target_blend = bpy.data.filepath
    if not dry_run:
        if not target_blend:
            log("[MatSync] ERROR: bpy.data.filepath is empty (no .blend opened).")
            sys.exit(2)
        if not os.path.isfile(target_blend):
            log(f"[MatSync] ERROR: Target .blend does not exist: {target_blend}")
            sys.exit(2)
        if not os.access(target_blend, os.W_OK):
            log(f"[MatSync] ERROR: Target .blend is READ-ONLY: {target_blend}")
            log(f"[MatSync] ERROR: This usually means the file is on Perforce "
                f"and was not checked out before Blender was launched.")
            log(f"[MatSync] ERROR: Aborting — no changes will be written.")
            sys.exit(3)

    # Load JSON
    with open(json_path, 'r', encoding='utf-8') as f:
        mi_entries = json.load(f)

    log(f"[MatSync] MI entries in JSON: {len(mi_entries)}")

    # Log linked libraries
    for lib in bpy.data.libraries:
        log(f"[MatSync] Linked library: {lib.filepath} (name={lib.name})")

    # Log materials before cleanup (linked + local)
    log(f"[MatSync] Materials before cleanup: {len(bpy.data.materials)}")
    for mat in bpy.data.materials:
        is_linked = "LINKED" if mat.library else "LOCAL"
        log(f"[MatSync]   mat: {mat.name} (users={mat.users}, {is_linked})")

    # ── B.2 Clean slate (scoped to Library_Material_Instances collection) ──
    # REQUIREMENT: the collection must already exist in the .blend file.
    # It is LINKED into consumer scenes, so we never create or rename it —
    # just wipe its contents (except the mat_cube template) and rebuild.
    lib_coll = bpy.data.collections.get(LIBRARY_COLLECTION)
    if lib_coll is None:
        log(f"[MatSync] ERROR: Required collection '{LIBRARY_COLLECTION}' "
            f"not found in {bpy.data.filepath}. Aborting.")
        sys.exit(1)

    log(f"[MatSync] Library collection '{LIBRARY_COLLECTION}' found "
        f"(objects: {len(lib_coll.objects)})")

    deleted_objects = 0
    deleted_meshes = 0
    deleted_mats = 0
    deleted_images = 0

    if not dry_run:
        # Capture data blocks associated with deleted objects for orphan cleanup.
        meshes_to_check = set()
        mats_to_check = set()

        for obj in list(lib_coll.objects):
            if obj.name == TEMPLATE_NAME:
                continue  # mat_cube is protected — never touched

            # Record local mesh/materials so we can purge them if they
            # become orphans after the object is removed.
            if obj.data and not obj.data.library and obj.data.name != TEMPLATE_NAME:
                meshes_to_check.add(obj.data)
            for slot in obj.material_slots:
                mat = slot.material
                if mat and not mat.library:
                    mats_to_check.add(mat)

            log(f"[MatSync] CLEANUP: Deleted object '{obj.name}'")
            bpy.data.objects.remove(obj, do_unlink=True)
            deleted_objects += 1

        # Purge now-orphan local meshes (duplicates of mat_cube from prior syncs)
        for mesh in meshes_to_check:
            if mesh.users == 0 and mesh.name != TEMPLATE_NAME:
                log(f"[MatSync] CLEANUP: Deleted orphan mesh '{mesh.name}'")
                bpy.data.meshes.remove(mesh)
                deleted_meshes += 1

        # Purge now-orphan local materials (MI duplicates of linked parents).
        # Linked parent materials always have .library set and are skipped.
        for mat in mats_to_check:
            if mat.users == 0:
                log(f"[MatSync] CLEANUP: Deleted local material '{mat.name}'")
                bpy.data.materials.remove(mat)
                deleted_mats += 1

        # Purge now-orphan images.
        #
        # This step did not exist, and its absence is why the library kept
        # every texture it had ever seen. Objects, meshes and materials were
        # all cleaned; images never were, so an image datablock loaded by some
        # sync in 2026-05 was still sitting in the file in 2026-08 with nothing
        # pointing at it. After a texture conform that is the whole
        # pre-conform set, sat next to the T_Mk_ ones that replaced them —
        # which is exactly what the Blender file showed.
        #
        # It also explains the '.001' twins. Blender names an image by its
        # file's basename; a second DIFFERENT file with the same basename gets
        # a numeric suffix. A stale 'T_Mk_Wood1_d.tga' pointing at an old
        # location and a fresh one pointing at <Root>/Textures/ are two
        # different files with one basename, so the new arrival became
        # 'T_Mk_Wood1_d.tga.001'. Remove the stale datablock and the collision
        # it was causing goes with it.
        #
        # The test discounts the fake user, and it has to.
        #
        # `img.users` COUNTS the fake user, so a plain `users == 0` never fires
        # on a datablock that has one — which is every image in this library.
        # The first version of this purge tested exactly that and removed
        # nothing at all.
        #
        # Respecting the flag would be right in a file a person authors, where
        # the shield means "I want this kept". It is not right here. This
        # library is GENERATED: every sync deletes the MI objects and rebuilds
        # them from material_instances.json, and nobody hand-pins a datablock
        # in a file that is rewritten wholesale. A fake user in here is an
        # artefact of how the images arrived, not an instruction, and treating
        # it as one is what let the whole pre-conform texture set survive.
        #
        # Still self-protecting: this runs AFTER the MI objects and materials
        # are removed, so anything the rebuild needs is reloaded from disk a
        # moment later, and anything genuinely in use — mat_cube's own
        # textures above all — has a REAL user and is never considered.
        # Linked images carry .library and are not ours to delete.
        # Sweep local materials no surviving object uses.
        #
        # The pass above only reaches materials through obj.material_slots, so
        # a material whose object disappeared in an EARLIER sync — an MI that
        # was renamed or deleted in Unreal — has never been reachable by it.
        # It sits in the file forever holding its textures with a real user,
        # which is why 43 images survived a purge that was working correctly.
        #
        # Confirmed in the file: a Material 'MI_Opq_lerp2_Barndoor' holding
        # 'C:\\Art\\textures\\scan\\barndoor_d.tga', while
        # material_instances.json's 87 entries contain no Barndoor MI at all
        # and no non-conformed texture path anywhere. Unreal's export was
        # right; this file was remembering something Unreal had forgotten.
        #
        # Reachability from a surviving object is the test, and fake users do
        # not enter into it — same reasoning as the images: this library is
        # regenerated wholesale and nobody hand-pins in it. mat_cube's object
        # survives, so mat_cube's material is reachable and safe.
        in_use = set()
        for obj in bpy.data.objects:
            if obj.library is not None:
                continue
            for slot in obj.material_slots:
                if slot.material is not None and slot.material.library is None:
                    in_use.add(slot.material.name)

        for mat in list(bpy.data.materials):
            if mat.library is not None or mat.name in in_use:
                continue
            log(f"[MatSync] CLEANUP: Deleted unreachable material '{mat.name}'")
            bpy.data.materials.remove(mat)
            deleted_mats += 1

        # Fake users would block the cascade below exactly as they blocked the
        # image sweep. Clear them on local data first; in a generated file the
        # flag records how something arrived, not a decision to keep it.
        for coll in (bpy.data.node_groups, bpy.data.images, bpy.data.materials):
            for datablock in coll:
                if datablock.library is None and datablock.use_fake_user:
                    datablock.use_fake_user = False

        # Cascade whatever the object-driven pass could not see.
        #
        # The pass above only examines datablocks reachable from the objects it
        # deletes: obj.data and obj.material_slots. A material left over from an
        # older sync that is no longer assigned to any object is never looked
        # at, and neither is a node group, and either one keeps its image
        # textures alive with a REAL user. That is why 44 images survived a
        # purge that correctly removed 159 — they were not orphans, they were
        # held by something the cleanup never walked.
        #
        # orphans_purge is Blender's own answer and it recurses, so
        # material -> node group -> image collapses in dependency order in one
        # call. Local IDs only: linked datablocks belong to Library_Materials
        # and are not ours to delete.
        try:
            bpy.data.orphans_purge(do_local_ids=True, do_linked_ids=False,
                                   do_recursive=True)
        except Exception as e:                                  # noqa: BLE001
            log(f"[MatSync] CLEANUP: recursive orphan purge unavailable ({e}); "
                f"falling back to the per-type sweep only.")

        deleted_images = 0
        for img in list(bpy.data.images):
            if img.library is not None:
                continue
            real_users = img.users - (1 if img.use_fake_user else 0)
            if real_users <= 0:
                log(f"[MatSync] CLEANUP: Deleted orphan image '{img.name}' "
                    f"(users={img.users}, fake={img.use_fake_user})")
                bpy.data.images.remove(img)
                deleted_images += 1
            else:
                _images_kept.append({
                    "name": img.name,
                    "users": img.users,
                    "fake_user": bool(img.use_fake_user),
                })

        # Name the holders. "users: 1, fake_user: false" says an image is
        # referenced but not by what — and without that, the next step is
        # another guess. bpy.data.user_map answers it directly.
        if _images_kept:
            try:
                survivors = [i for i in bpy.data.images
                             if i.library is None
                             and i.name in {k["name"] for k in _images_kept}]
                umap = bpy.data.user_map(subset=survivors)
                by_name = {}
                for img, holders in umap.items():
                    by_name[img.name] = sorted(
                        f"{type(h).__name__}:{h.name}" for h in holders)
                # Record the HOLDER's own state, not just its name. A
                # material that survived the purge above did so for a reason,
                # and "fake user" vs "still on an object" are different bugs
                # with different fixes. Guessing between them from a name has
                # already cost two rounds.
                def _holder_state(h):
                    bits = [f"{type(h).__name__}:{h.name}"]
                    users = getattr(h, "users", None)
                    if users is not None:
                        bits.append(f"users={users}")
                    if getattr(h, "use_fake_user", False):
                        bits.append("FAKE")
                    lib = getattr(h, "library", None)
                    if lib is not None:
                        bits.append(f"LINKED({lib.name})")
                    return " ".join(bits)

                by_name = {}
                for img, holders in umap.items():
                    by_name[img.name] = sorted(_holder_state(h) for h in holders)
                for kept in _images_kept:
                    kept["held_by"] = by_name.get(kept["name"], [])

                _cleanup_counts["local_materials_remaining"] = sum(
                    1 for m in bpy.data.materials if m.library is None)
                _cleanup_counts["local_objects_remaining"] = sum(
                    1 for o in bpy.data.objects if o.library is None)
                _cleanup_counts["local_images_remaining"] = sum(
                    1 for i in bpy.data.images if i.library is None)
            except Exception as e:                              # noqa: BLE001
                log(f"[MatSync] CLEANUP: could not map image users ({e}).")

    _cleanup_counts.update({
        "objects": deleted_objects,
        "meshes": deleted_meshes,
        "materials": deleted_mats,
        "images": deleted_images,
        "images_kept": len(_images_kept),
    })

    log(f"[MatSync] CLEANUP: {deleted_objects} object(s), "
        f"{deleted_meshes} mesh(es), {deleted_mats} material(s), "
        f"{deleted_images} image(s) removed, {len(_images_kept)} image(s) "
        f"kept (mat_cube preserved).")

    # Build the MI-name set once for downstream lookups.
    mi_names = {entry["MaterialInstance"] for entry in mi_entries}

    # ── B.3 Resolve parent materials ───────────────────────────────
    # Parent materials should be LINKED in this file from Library_Materials.blend.
    # They have the full shader setup (node group → Material Output, alpha, etc.)

    parent_names_needed = set()
    for entry in mi_entries:
        parent_names_needed.add(entry["MaterialParent"])

    log(f"[MatSync] Parent materials needed: {sorted(parent_names_needed)}")

    # Check which parents are already available (linked or local)
    # Use case-insensitive matching since Blender/UE5 names may differ in case
    available_parents = {}
    parent_lower_map = {n.lower(): n for n in parent_names_needed}

    for mat in bpy.data.materials:
        ue_name = None
        if mat.name in parent_names_needed:
            ue_name = mat.name
        elif mat.name.lower() in parent_lower_map:
            ue_name = parent_lower_map[mat.name.lower()]

        if ue_name:
            available_parents[ue_name] = mat
            is_linked = "LINKED" if mat.library else "LOCAL"
            if ue_name != mat.name:
                log(f"[MatSync]   Parent '{ue_name}' → {is_linked} (Blender name: '{mat.name}')")
            else:
                log(f"[MatSync]   Parent '{ue_name}' → {is_linked}")

    missing_parents = parent_names_needed - set(available_parents.keys())
    if missing_parents and not dry_run:
        log(f"[MatSync] Linking {len(missing_parents)} missing parent material(s) from library…")
        log(f"[MatSync] Missing: {sorted(missing_parents)}")
        try:
            with bpy.data.libraries.load(materials_lib_path, link=True) as (data_from, data_to):
                # Build case-insensitive lookup: lowercase → actual name in library
                lib_lower_map = {m.lower(): m for m in data_from.materials}

                to_link = []
                # name_remap: UE5 name → Blender library name (when case differs)
                name_remap = {}

                for needed in missing_parents:
                    if needed in data_from.materials:
                        # Exact match
                        to_link.append(needed)
                    elif needed.lower() in lib_lower_map:
                        # Case-insensitive match
                        actual = lib_lower_map[needed.lower()]
                        to_link.append(actual)
                        name_remap[needed] = actual
                        log(f"[MatSync]   Case remap: '{needed}' → '{actual}'")
                    else:
                        log(f"[MatSync]   NOT in library (even case-insensitive): '{needed}'")

                data_to.materials = to_link
                log(f"[MatSync] Linked: {to_link}")
        except Exception as e:
            log(f"[MatSync] ERROR: Failed to link from library: {e}")
            name_remap = {}

        # Refresh available parents (check both exact and remapped names)
        for mat in bpy.data.materials:
            if mat.name in parent_names_needed and mat.name not in available_parents:
                available_parents[mat.name] = mat
                log(f"[MatSync]   Parent '{mat.name}' → newly linked")

        # Register remapped parents: UE5 name → Blender material under its actual name
        for ue_name, blender_name in name_remap.items():
            mat = bpy.data.materials.get(blender_name)
            if mat and ue_name not in available_parents:
                available_parents[ue_name] = mat
                log(f"[MatSync]   Parent '{ue_name}' → mapped to '{blender_name}'")

    # ── B.4 Material instance creation ─────────────────────────────
    created = 0
    skipped_parent = 0
    skipped_other = 0
    valid_entries = []

    # v1.20 — resolved once, before the loop: both of these read the disk, and
    # they are the same answer for all 87 entries.
    mi_catalog = "" if dry_run else _mi_catalog_uuid()

    # The MI directory is the JSON's directory, NOT the open .blend's.
    #
    # AtomicRegenLibrary runs this script against
    # <LibDir>/.maplink_sync_tmp/<Stem>.tmp.blend — one level deeper than where
    # the library finally lands. `bpy.data.filepath` therefore points into the
    # temp folder, so `Thumbnails/` resolved to a directory that does not
    # exist and every MI reported no_thumbnail while UE had written all 86.
    #
    # This is the same trap the v1.16 texture work documented as the reason
    # `rel` is computed in UE rather than here, and the JSON is the anchor for
    # exactly the same reason: material_instances.json is written into the
    # FINAL directory, so its own path is the one fixed point both sides agree
    # on. Anything derived from the open file is derived from a temp copy.
    mi_dir = os.path.dirname(json_path)
    if not dry_run:
        log("[MatSync] Asset catalog for these MIs: "
            + (mi_catalog or "(none — they will land in Unassigned)"))
        log(f"[MatSync] Thumbnail anchor (JSON dir): {mi_dir}")
        log(f"[MatSync]   open .blend is: {bpy.data.filepath}")

    for entry in mi_entries:
        mi_name = entry["MaterialInstance"]
        parent_name = entry["MaterialParent"]

        log(f"[MatSync] --- Processing: {mi_name} (parent: {parent_name}) ---")

        parent_mat = available_parents.get(parent_name)
        if parent_mat is None:
            log_error(mi_name, f"Parent material '{parent_name}' not found (linked or local)")
            skipped_parent += 1
            continue

        is_linked = "LINKED" if parent_mat.library else "LOCAL"
        log(f"[MatSync]   Parent '{parent_name}' found ({is_linked})")

        if dry_run:
            log(f"[MatSync]   [DRY-RUN] Would copy '{parent_name}' → '{mi_name}'")
            valid_entries.append(entry)
            created += 1
            continue

        # ── Duplicate the parent material (makes a local copy) ─────
        mi_mat = parent_mat.copy()
        mi_mat.name = mi_name
        # Also make the node tree local so we can edit it
        if mi_mat.node_tree and mi_mat.node_tree.library:
            mi_mat.node_tree = mi_mat.node_tree.copy()

        log(f"[MatSync]   Duplicated → '{mi_mat.name}'")

        # Find the group node in the duplicated material
        group_node = _find_group_node(mi_mat)
        if group_node is None:
            log(f"[MatSync]   No group node in duplicated material — skipping params")
            _publish_as_asset(mi_mat, entry, mi_catalog, mi_dir)
            valid_entries.append(entry)
            created += 1
            continue

        log(f"[MatSync]   Group node: '{group_node.name}' (tree: '{group_node.node_tree.name}')")
        socket_names = [inp.name for inp in group_node.inputs]
        log(f"[MatSync]   Available inputs: {socket_names}")

        # ── Set scalar parameters ──────────────────────────────────
        for param_name, value in entry.get("ScalarParameters", {}).items():
            if param_name in group_node.inputs:
                group_node.inputs[param_name].default_value = value
                log(f"[MatSync]   Scalar '{param_name}' = {value} → SET")
            else:
                log(f"[MatSync]   Scalar '{param_name}' = {value} → SOCKET NOT FOUND")

        # ── Set texture parameters ─────────────────────────────────
        for param_name, tex_value in entry.get("TextureParameters", {}).items():
            tex_path, tex_rel, tex_srgb, tex_status = _read_texture_value(tex_value)
            log(f"[MatSync]   Texture '{param_name}' → {tex_path} "
                f"(srgb={tex_srgb}, status={tex_status})")

            if not tex_path:
                log_warning(mi_name, f"'{param_name}': no path in the JSON entry")
                continue

            file_exists = os.path.isfile(tex_path)
            log(f"[MatSync]     File exists: {file_exists}")

            socket_found = param_name in group_node.inputs
            log(f"[MatSync]     Socket found: {socket_found}")

            if not socket_found:
                continue

            if not file_exists:
                log_warning(mi_name, f"Texture not found: {tex_path}")
                continue

            # Reuse within the run by ABSOLUTE PATH, keyed ourselves.
            #
            # check_existing cannot be trusted here and the file proves it:
            # the library held T_Mk_094212_baseTexBaked.tga, .001 AND .002.
            #
            # Blender matches check_existing by resolving each image's stored
            # filepath. A few lines below, this script overwrites that stored
            # path with UE's '//'-relative string — correct for the file's
            # final location, and this script runs from
            # <LibDir>/.maplink_sync_tmp/, one level deeper. So the stored
            # path resolves to somewhere that does not exist, never equals the
            # absolute path being requested, and Blender makes a new datablock.
            # Every MI after the first that wants the same texture gets a
            # fresh copy, and every subsequent sync adds another.
            #
            # Our own dict sidesteps the whole question: one datablock per
            # distinct file per run, decided before the path is rewritten.
            cache_key = os.path.normcase(os.path.abspath(tex_path))
            img = _image_cache.get(cache_key)
            if img is None:
                try:
                    img = bpy.data.images.load(tex_path, check_existing=True)
                except Exception as e:                          # noqa: BLE001
                    log_warning(mi_name,
                                f"Failed to load image '{tex_path}': {e}")
                    continue
                _image_cache[cache_key] = img
                log(f"[MatSync]     Image loaded: {img.name}")
            else:
                log(f"[MatSync]     Image reused: {img.name}")

            # ── v1.16 — colour space, stated rather than guessed ────────
            applied = _apply_colorspace(img, tex_srgb, param_name, mi_name)
            if applied:
                log(f"[MatSync]     Colour space: {applied}")

            # ── v1.16 — portability ─────────────────────────────────────
            #
            # UE computed this relative path against the FINAL library
            # directory. We must not recompute it here: this script runs
            # against <LibDir>/.maplink_sync_tmp/<Stem>.tmp.blend, one level
            # deeper than where the file lands, so bpy.path.relpath() would
            # produce a path that validates during the sync and breaks the
            # instant AtomicRegenLibrary replaces upward. Assigning UE's
            # string verbatim is the whole point.
            #
            # The image was loaded by absolute path, so it is already
            # resolved; setting filepath afterwards changes only how the
            # reference is SAVED, not what is in memory.
            if tex_rel:
                try:
                    img.filepath = tex_rel
                    log(f"[MatSync]     Portable path: {tex_rel}")
                except Exception as e:
                    log_warning(mi_name,
                                f"'{param_name}': could not set relative path "
                                f"'{tex_rel}' ({e}); the absolute path stays, "
                                f"so this texture is local to this machine.")
            elif tex_status == "LocalOnly":
                log_warning(mi_name,
                            f"'{param_name}': '{os.path.basename(tex_path)}' is "
                            f"outside the MapLink and UE project trees, so no "
                            f"portable path exists. It will be missing on any "
                            f"other machine. Conform it in UE "
                            f"(MapLink.Textures.Conform) to fix this.")

            # Wire to group input
            tex_node = _find_or_create_image_node(mi_mat.node_tree, group_node, param_name)
            if tex_node:
                # Assign image first, then make sure the link actually exists
                # (links.new can silently no-op on a socket-type mismatch, which
                # leaves the texture present but disconnected → blank material).
                tex_node.image = img
                socket = group_node.inputs.get(param_name)
                if socket is not None and not socket.is_linked:
                    mi_mat.node_tree.links.new(tex_node.outputs['Color'], socket)

                link_ok = bool(socket is not None and socket.is_linked)
                internal = _group_input_internally_used(group_node, param_name)
                log(f"[MatSync]     Node wired: {tex_node.name} "
                    f"(socket linked: {link_ok}; group routes input internally: {internal})")
                if link_ok and internal is False:
                    log_warning(mi_name,
                        f"'{param_name}' is connected at the material level but the "
                        f"parent node group '{group_node.node_tree.name}' never routes "
                        f"that input to its shader — texture will not render. Fix the "
                        f"node group in Library_Materials.blend (wire the input to the "
                        f"BSDF), then re-sync.")
            else:
                log(f"[MatSync]     Node wired: FAILED")

        # ── Set vector parameters ──────────────────────────────────
        for param_name, rgba in entry.get("VectorParameters", {}).items():
            if param_name in group_node.inputs:
                group_node.inputs[param_name].default_value = tuple(rgba)
                log(f"[MatSync]   Vector '{param_name}' = {rgba} → SET")
            else:
                log(f"[MatSync]   Vector '{param_name}' = {rgba} → SOCKET NOT FOUND")

        # Last, so the preview and the description describe a finished
        # material rather than one that is still having parameters set.
        _publish_as_asset(mi_mat, entry, mi_catalog, mi_dir)

        valid_entries.append(entry)
        created += 1

    # ── B.5 Visual palette (mat_cube grid) ─────────────────────────
    # Prefer the mesh used by the 'mat_cube' object in the library collection
    # — that is the canonical template and must not be modified.
    template_obj = lib_coll.objects.get(TEMPLATE_NAME)
    template_mesh = template_obj.data if (template_obj and template_obj.data) else None
    if template_mesh is None:
        template_mesh = bpy.data.meshes.get(TEMPLATE_NAME)

    if template_mesh is None and not dry_run:
        log(f"[MatSync] WARNING: '{TEMPLATE_NAME}' missing from "
            f"'{LIBRARY_COLLECTION}' — attempting fallback load from library.")
        try:
            with bpy.data.libraries.load(materials_lib_path, link=False) as (data_from, data_to):
                if TEMPLATE_NAME in data_from.meshes:
                    data_to.meshes = [TEMPLATE_NAME]
            template_mesh = bpy.data.meshes.get(TEMPLATE_NAME)
        except Exception as e:
            log(f"[MatSync] ERROR: Failed to load '{TEMPLATE_NAME}' mesh: {e}")

    log(f"[MatSync] Template mesh '{TEMPLATE_NAME}' found: {template_mesh is not None}")

    rows = (len(valid_entries) + COLUMNS - 1) // COLUMNS if valid_entries else 0
    log(f"[MatSync] Creating {len(valid_entries)} palette cubes (grid: {COLUMNS}x{rows})")

    if template_mesh is not None and not dry_run:
        # Target the required linked collection (verified to exist above).
        palette_col = lib_coll

        for i, entry in enumerate(valid_entries):
            mi_name = entry["MaterialInstance"]
            mi_mat = bpy.data.materials.get(mi_name)
            if mi_mat is None:
                continue

            row = i // COLUMNS
            col = i % COLUMNS
            x = col * SPACING
            y = row * SPACING

            # Cube named after the MI (no prefix)
            cube_obj = bpy.data.objects.new(mi_name, template_mesh.copy())
            cube_obj.location = (x, y, 0.0)

            # Assign material
            if cube_obj.data.materials:
                cube_obj.data.materials[0] = mi_mat
            else:
                cube_obj.data.materials.append(mi_mat)

            palette_col.objects.link(cube_obj)
            log(f"[MatSync]   Cube '{mi_name}' at ({x}, {y}, 0)")

    elif dry_run:
        for i, entry in enumerate(valid_entries):
            mi_name = entry["MaterialInstance"]
            row = i // COLUMNS
            col = i % COLUMNS
            x = col * SPACING
            y = row * SPACING
            log(f"[MatSync]   [DRY-RUN] Cube '{mi_name}' at ({x}, {y}, 0)")

    # ── B.5b Make the Library_Materials link portable (v1.19) ──────
    if not dry_run:
        _relativise_materials_library(materials_lib_path, materials_lib_rel)

    # ── B.6 Save ───────────────────────────────────────────────────
    if not dry_run:
        save_failed_reason = None
        try:
            result = bpy.ops.wm.save_mainfile()
        except Exception as e:
            result = None
            save_failed_reason = f"Exception during save: {e}"

        if save_failed_reason is None and (result is None or 'FINISHED' not in result):
            save_failed_reason = f"bpy.ops.wm.save_mainfile() returned {result}"

        if save_failed_reason is not None:
            log(f"[MatSync] ERROR: Failed to save .blend: {save_failed_reason}")
            log(f"[MatSync] ERROR: Changes to '{bpy.data.filepath}' were NOT persisted.")
            log(f"[MatSync] ERROR: Most likely cause: file is read-only "
                f"(on Perforce, not checked out).")
            # Best-effort: still write the report so UE has something to read.
            try:
                _report_payload = {
                    "dry_run": dry_run,
                    "total_in_json": len(mi_entries),
                    "created": created,
                    "skipped_missing_parent": skipped_parent,
                    "skipped_other": skipped_other,
                    "warnings": _warnings,
                    "errors": _errors + [{"mi": "<save>", "message": save_failed_reason}],
                    "save_failed": True,
                    "assets": dict(_asset_counts),
                }
                with open(os.path.join(log_dir, "material_sync_report.json"),
                          'w', encoding='utf-8') as _f:
                    json.dump(_report_payload, _f, indent=2)
            except Exception:
                pass
            sys.exit(2)

        log(f"[MatSync] File saved: {bpy.data.filepath}")
    else:
        log(f"[MatSync] [DRY-RUN] File NOT saved.")

    # ── B.7 Report ─────────────────────────────────────────────────
    total = len(mi_entries)

    log(f"[MatSync] === SUMMARY ===")
    log(f"[MatSync] Total in JSON: {total}")
    log(f"[MatSync] Created: {created}")
    log(f"[MatSync] Skipped (missing parent): {skipped_parent}")
    log(f"[MatSync] Skipped (other): {skipped_other}")
    log(f"[MatSync] Warnings: {len(_warnings)}")
    log(f"[MatSync] Errors: {len(_errors)}")
    log(f"[MatSync] Materials in file after sync: {len(bpy.data.materials)}")
    log(f"[MatSync] Objects in file after sync: {len(bpy.data.objects)}")

    report = {
        "dry_run": dry_run,
        "total_in_json": total,
        "created": created,
        "skipped_missing_parent": skipped_parent,
        "skipped_other": skipped_other,
        "warnings": _warnings,
        "errors": _errors,
        "cleanup": _cleanup_counts,
        # v1.20 — marked / previewed / no_thumbnail / preview_failed.  A run
        # where `no_thumbnail` equals `marked` means UE wrote no thumbnails at
        # all, which is a different problem from previews that failed to apply.
        "assets": dict(_asset_counts),
        # Capped: the point is to see WHAT survived and why, and thirty rows
        # answers that. An uncapped list on a library this size buries the
        # counts above it.
        "images_kept": _images_kept[:30],
    }

    report_path = os.path.join(log_dir, "material_sync_report.json")
    with open(report_path, 'w', encoding='utf-8') as f:
        json.dump(report, f, indent=2)
    log(f"[MatSync] Report written: {report_path}")


if __name__ == '__main__':
    # v1.21 - see maplink_export.py: an uncaught exception must not leave
    # Blender exiting 0, or UE reports a sync that never happened as done.
    try:
        _require_blender_version()
        main()
    except SystemExit:
        raise
    except Exception:
        traceback.print_exc()
        print("[MatSync] FAILED.", flush=True)
        raise SystemExit(1)
