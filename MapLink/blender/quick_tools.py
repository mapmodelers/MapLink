"""MapLink_Blender — Quick authoring tools (v1.4.5, Pack C).

Adds four viewport/asset-browser conveniences that sit on top of the existing
pipeline.  Nothing here introduces new sidecar / hash / P4 logic: asset
creation defers entirely to the headless worker, which in turn reuses
``batch_sidecar``'s helpers so every Quick-Created asset is byte-for-byte
conventional (same analysis schema, same MD5, same add-first P4 pattern).

Operators
    maplink.reload_blend          — revert current file to last saved state
    maplink.quick_create_asset    — make an SM_/LI_ asset from the selection
    maplink.run_thumbnail         — (Asset Browser) regenerate a collection preview
    maplink.break_linked_to_local — localise a linked collection instance to root

UI
    MAPLINK_PT_Tools              — sidebar panel hosting Reload + Quick Create
    (context menus live in context_menu.py)
"""

import os
import json
import stat
import tempfile
import shutil
import subprocess

import bpy
import mathutils
from bpy.types import Operator, Panel
from . import ui_menus


# ---------------------------------------------------------------------------
# Feature 1 — Reload Current .blend
# ---------------------------------------------------------------------------

class MAPLINK_OT_ReloadBlend(Operator):
    bl_idname = "maplink.reload_blend"
    bl_label = "Reload .blend"
    bl_description = "Revert the current file to its last saved state (immediate)"

    def execute(self, context):
        if not bpy.data.filepath:
            self.report({'ERROR'}, "File has not been saved yet.")
            return {'CANCELLED'}
        # Called straight from execute() (not invoke) → no confirm popup.
        # Reverting keeps the same file open, so nav_drill's session stack
        # stays valid — no reset needed.
        bpy.ops.wm.revert_mainfile()
        return {'FINISHED'}


# ---------------------------------------------------------------------------
# Feature 3.1 — Quick Create SM_ / LI_ from selection (foreground operator)
# ---------------------------------------------------------------------------

def _find_project_root(start):
    """Walk up to 4 levels looking for a dir holding sibling SM/ and LI/."""
    cur = start
    for _ in range(4):
        if (os.path.isdir(os.path.join(cur, 'SM'))
                and os.path.isdir(os.path.join(cur, 'LI'))):
            return cur
        cur = os.path.dirname(cur)
    return None


def _find_preset_dir(blend_dir):
    """Locate the project's 'Preset' folder — a sibling of SM/ and LI/ at the
    project root (e.g. <Blender>/Preset), NOT <Blender>/LI/Preset.

    Prefers <project_root>/Preset, then walks up from *blend_dir* for the
    nearest 'Preset' directory.  Returns the path or None.
    """
    root = _find_project_root(blend_dir)
    if root:
        cand = os.path.join(root, "Preset")
        if os.path.isdir(cand):
            return cand
    cur = blend_dir
    for _ in range(6):
        cand = os.path.join(cur, "Preset")
        if os.path.isdir(cand):
            return cand
        parent = os.path.dirname(cur)
        if parent == cur:
            break
        cur = parent
    return None


def _resolve_export_objs(op, context):
    """Resolve the objects to export + the origin anchor location.

    Uses the names captured at invoke time (``op._sel_names`` /
    ``op._anchor_name``), falling back to the live selection.  The anchor is
    the last-selected (active) object and defines the asset origin.

    If that anchor is a plain *locator* empty (an Empty that does not instance
    a collection), it is used ONLY as the origin and is NOT baked into the
    asset content — so "select meshes, click a locator empty last, create"
    yields a clean SM/LI placed at the locator.

    Returns ``(export_objs, anchor_loc)`` or ``(None, None)`` after reporting.
    """
    sel_names = getattr(op, "_sel_names", None) or \
        [o.name for o in context.selected_objects]
    sel_objs = [bpy.data.objects[n] for n in sel_names
                if n in bpy.data.objects]
    if not sel_objs:
        op.report({'ERROR'}, "No selected objects to export.")
        return None, None, None

    anchor = bpy.data.objects.get(getattr(op, "_anchor_name", "") or "")
    if anchor is None or anchor not in sel_objs:
        anchor = sel_objs[0]
    anchor_loc = list(anchor.matrix_world.translation)
    # v1.4.7 - capture the anchor's FULL world matrix (location + rotation +
    # scale) as 4 rows of 4 floats.  The worker re-expresses the asset in this
    # frame so the anchor lands at IDENTITY (clean 0,0,0 / 0-rotation) inside the
    # .blend, and the dropped instance carries this matrix so the placement
    # reproduces the original transform (cube authored at rot 7/45/45 -> reads
    # 0/0/0 in the asset .blend, and the LI instance reads 7/45/45).
    anchor_mat = [list(row) for row in anchor.matrix_world]

    export_objs = list(sel_objs)
    is_locator_empty = (anchor.type == 'EMPTY'
                        and getattr(anchor, "instance_collection", None) is None)
    if is_locator_empty:
        export_objs = [o for o in sel_objs if o != anchor]
        if not export_objs:
            op.report({'ERROR'},
                      "Only a locator empty is selected — nothing to export.")
            return None, None, None

    return export_objs, anchor_loc, anchor_mat


def _find_collection(scene, name):
    """Find a collection by exact name within the scene's hierarchy, else None."""
    stack = [scene.collection]
    while stack:
        c = stack.pop()
        if c.name == name:
            return c
        stack.extend(c.children)
    return None


def _run_create(op, context, atype, asset_name, tmpl_path, dest_folder,
                dest_blend, export_objs, anchor_loc, anchor_matrix=None,
                drop_instance=True,
                skip_sidecars=False, open_after=False, dest_is_folder=True):
    """Export the selection, run the headless worker, then either drop a linked
    instance back at the anchor or open the new file, and report.  Shared by
    SM/LI assets and Presets.

    ``skip_sidecars`` — tell the worker not to write analysis.json / filehash
        (presets aren't imported as instanced meshes, so they don't need them).
    ``open_after``    — launch the saved .blend in a new Blender process
        instead of (or in addition to) dropping an instance.
    ``dest_is_folder``— True when dest_blend lives in its own asset folder (so a
        failed create can rmtree it); False for a flat file (remove just it).
    """
    # Capture the CURRENT in-memory selection (including unsaved edits) by
    # writing the export objects + their dependencies to a throwaway .blend
    # and pointing the worker at that.  Reading bpy.data.filepath instead
    # misses anything not yet saved on disk — which is exactly how an asset
    # ends up empty (the object simply wasn't in the saved source the worker
    # opened).
    src_tmp_dir = tempfile.mkdtemp(prefix="maplink_src_")
    src_export = os.path.join(src_tmp_dir, "selection_src.blend")
    try:
        bpy.data.libraries.write(
            src_export,
            set(export_objs),
            path_remap='ABSOLUTE',
            fake_user=False,
            compress=True,
        )
    except Exception as e:
        shutil.rmtree(src_tmp_dir, ignore_errors=True)
        op.report({'ERROR'}, f"Could not export selection: {e}")
        return {'CANCELLED'}

    manifest = {
        "asset_name":       asset_name,
        "asset_type":       atype,
        "source_blend":     src_export,
        "selected_objects": [o.name for o in export_objs],
        "anchor_location":  anchor_loc,
        "spawn_location":   anchor_loc,
        "anchor_matrix":    anchor_matrix,
        "created_cl_desc":  "[MapLink][Blender] Created Asset",
        "skip_sidecars":    bool(skip_sidecars),
    }
    fd, manifest_path = tempfile.mkstemp(suffix=".json",
                                         prefix="maplink_create_")
    with os.fdopen(fd, 'w', encoding='utf-8') as f:
        json.dump(manifest, f)

    # Template copy becomes the new .blend; the worker fills it in.  Templates
    # live under Perforce and are read-only on disk; copy2 carries that bit
    # onto the new file, which blocks the worker from saving it.  Clear it.
    shutil.copy2(tmpl_path, dest_blend)
    try:
        os.chmod(dest_blend, stat.S_IWRITE | stat.S_IREAD)
    except OSError as e:
        print(f"[MapLink] could not clear read-only on {dest_blend}: {e}")

    worker = os.path.join(os.path.dirname(__file__),
                          "quick_create_worker.py")
    cmd = [bpy.app.binary_path, "--factory-startup", "--background",
           dest_blend, "--python", worker, "--", manifest_path]
    cf = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=180, creationflags=cf)
    except subprocess.TimeoutExpired:
        op.report({'ERROR'}, "Asset creation timed out.")
        return {'CANCELLED'}
    finally:
        shutil.rmtree(src_tmp_dir, ignore_errors=True)

    stdout = r.stdout or ""
    for line in stdout.splitlines():
        print(line)
    if r.returncode != 0:
        # The worker aborts before any save/P4 when nothing was appended; the
        # dest is then just an untouched template copy, safe to remove so the
        # same name can be retried.
        if "SAFE_TO_DELETE_DEST" in stdout:
            if dest_is_folder:
                shutil.rmtree(dest_folder, ignore_errors=True)
            else:
                try:
                    os.remove(dest_blend)
                except OSError:
                    pass
        op.report({'ERROR'},
                  f"Asset creation failed (rc={r.returncode}); see console.")
        print(r.stderr)
        return {'CANCELLED'}

    if drop_instance:
        # Spawn a linked collection instance at the anchor.
        with bpy.data.libraries.load(dest_blend, link=True) as (df, dt):
            dt.collections = [asset_name]
        coll = bpy.data.collections.get(asset_name)
        if coll:
            inst = bpy.data.objects.new(name=asset_name, object_data=None)
            inst.instance_type = 'COLLECTION'
            inst.instance_collection = coll
            if anchor_matrix is not None:
                # Carry the anchor's full transform (loc + rot + scale) onto the
                # placed instance; the asset content is authored clean at
                # identity, so this reproduces the original world transform.
                inst.matrix_world = mathutils.Matrix(anchor_matrix)
            else:
                inst.location = anchor_loc
            # Place it in LINK > LINK_SM / LINK_LI when that collection exists;
            # otherwise leave it at the scene root (don't create anything).
            target = _find_collection(context.scene, f"LINK_{atype}")
            (target or context.scene.collection).objects.link(inst)
        else:
            op.report({'WARNING'},
                      "Created, but could not link the collection for placement.")

    if open_after:
        try:
            flags = (getattr(subprocess, 'DETACHED_PROCESS', 0)
                     | getattr(subprocess, 'CREATE_NEW_PROCESS_GROUP', 0))
            subprocess.Popen([bpy.app.binary_path, dest_blend],
                             creationflags=flags, close_fds=True)
        except Exception as e:
            op.report({'WARNING'},
                      f"Created, but could not open it in a new Blender: {e}")

    verb = "Created + opened" if open_after else "Created + P4-added"
    op.report({'INFO'}, f"{verb}: {asset_name}")
    return {'FINISHED'}


class MAPLINK_OT_QuickCreateAsset(Operator):
    bl_idname = "maplink.quick_create_asset"
    bl_label = "Create SM/LI Asset from Selection"
    bl_description = ("Create a new SM_/LI_ asset .blend from the selected "
                      "objects (headless), P4-add it + sidecars, then drop a "
                      "linked instance at the first selected object")

    asset_type: bpy.props.EnumProperty(
        name="Type",
        items=[('SM', "SM_ (Static Mesh)", ""),
               ('LI', "LI_ (Level Instance)", "")],
        default='SM',
    )
    asset_stem: bpy.props.StringProperty(name="Name", default="")

    @classmethod
    def poll(cls, context):
        try:
            return context.mode == 'OBJECT' and bool(context.selected_objects)
        except Exception:
            return False

    def invoke(self, context, event):
        # v1.19 — an unsaved file is fine as long as SOMETHING can receive the
        # asset.  That is what the Shelf is for: "I modelled a fountain, keep
        # it" should not require setting up a project first.
        if not bpy.data.filepath:
            from . import project_roots
            if not project_roots.shelf_root():
                bpy.ops.maplink.ensure_shelf('INVOKE_DEFAULT')
                return {'CANCELLED'}
        if not context.selected_objects:
            self.report({'ERROR'}, "No objects selected.")
            return {'CANCELLED'}
        # Snapshot the selection now, while the context is the viewport and the
        # selection is unambiguous.  When the operator is launched from a menu,
        # context.selected_objects can come back empty inside execute() (the
        # popup dialog's context), so we must not rely on it there.
        self._sel_names = [o.name for o in context.selected_objects]
        # The active object is the LAST-selected one — used as the anchor that
        # lands at the asset origin (so multi-object layouts are preserved).
        act = context.view_layer.objects.active
        self._anchor_name = (act.name
                             if act is not None and act in set(context.selected_objects)
                             else (self._sel_names[0] if self._sel_names else ""))
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        from . import project_roots

        stem = self.asset_stem.strip()
        if not stem:
            self.report({'ERROR'}, "Name is required.")
            return {'CANCELLED'}

        fp = bpy.data.filepath
        blend_dir = os.path.dirname(bpy.path.abspath(fp)) if fp else ""

        # v1.19 — no project above this file falls through to the Shelf, which
        # is offered once and then never asked about again.  Telling the artist
        # "project root not found" and stopping was the behaviour that made
        # MapLink look like it needed Unreal to be useful.
        root = _find_project_root(blend_dir) if blend_dir else None
        landed_in_shelf = False
        if not root:
            root, _name = project_roots.require_root(context, blend_dir)
            if not root:
                return {'CANCELLED'}
            landed_in_shelf = True

        atype = self.asset_type
        tmpl_name = f"{atype}_Template"
        tmpl_path = os.path.join(root, atype, tmpl_name, f"{tmpl_name}.blend")
        if not os.path.isfile(tmpl_path):
            self.report({'ERROR'}, f"Template not found: {tmpl_path}")
            return {'CANCELLED'}

        asset_name = f"{atype}_{stem}"
        dest_folder = os.path.join(root, atype, asset_name)
        dest_blend = os.path.join(dest_folder, f"{asset_name}.blend")
        if os.path.exists(dest_blend):
            self.report({'ERROR'}, f"Asset already exists: {dest_blend}")
            return {'CANCELLED'}
        # Resolve export objects + origin anchor.  This handles the case where
        # the active (last-selected) object is a plain locator Empty: it is
        # used ONLY as the asset origin and is NOT baked into the asset, so
        # "select meshes, click an Empty last, create" yields a clean SM/LI
        # placed at the Empty.  Runs before makedirs so a bad selection doesn't
        # leave an empty asset folder behind.
        export_objs, anchor_loc, anchor_mat = _resolve_export_objs(self, context)
        if export_objs is None:
            return {'CANCELLED'}

        os.makedirs(dest_folder, exist_ok=True)
        result = _run_create(self, context, atype, asset_name, tmpl_path,
                             dest_folder, dest_blend, export_objs, anchor_loc,
                             anchor_matrix=anchor_mat, drop_instance=True)
        if landed_in_shelf and 'FINISHED' in result:
            # Tell after, do not ask before. Creating in the obvious place and
            # saying where is faster than a dialog nobody would answer
            # differently.
            self.report({'INFO'},
                        f"Created {asset_name} in your Shelf ({root}). "
                        f"Move it into a project any time.")
        return result


# ---------------------------------------------------------------------------
# Feature 3.2 — Create Preset from selection (from preset_template.blend)
# ---------------------------------------------------------------------------

class MAPLINK_OT_CreatePreset(Operator):
    bl_idname = "maplink.create_preset"
    bl_label = "Create Preset from Selection"
    bl_description = ("Create a new preset_<Name>.blend from the selected "
                      "objects (headless) using ../Preset/preset_template.blend, "
                      "then open it in a separate Blender. No sidecars; no "
                      "instance dropped (the Preset library is appended)")

    preset_stem: bpy.props.StringProperty(name="Name", default="")

    @classmethod
    def poll(cls, context):
        try:
            return context.mode == 'OBJECT' and bool(context.selected_objects)
        except Exception:
            return False

    def invoke(self, context, event):
        if not bpy.data.filepath:
            self.report({'ERROR'}, "Save the current file first.")
            return {'CANCELLED'}
        if not context.selected_objects:
            self.report({'ERROR'}, "No objects selected.")
            return {'CANCELLED'}
        # Snapshot the selection + anchor now (see QuickCreateAsset.invoke).
        self._sel_names = [o.name for o in context.selected_objects]
        act = context.view_layer.objects.active
        self._anchor_name = (
            act.name
            if act is not None and act in set(context.selected_objects)
            else (self._sel_names[0] if self._sel_names else ""))
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        stem = self.preset_stem.strip()
        if not stem:
            self.report({'ERROR'}, "Name is required.")
            return {'CANCELLED'}

        blend_dir = os.path.dirname(bpy.path.abspath(bpy.data.filepath))

        # The Preset folder is a sibling of SM/ and LI/ at the project root
        # (<root>/Preset), holding both preset_template.blend and the new flat
        # preset_<Name>.blend.
        preset_dir = _find_preset_dir(blend_dir)
        if preset_dir is None:
            self.report({'ERROR'},
                        "Preset folder not found (expected a 'Preset' "
                        "directory beside SM/ and LI/ at the project root).")
            return {'CANCELLED'}
        tmpl_path = os.path.join(preset_dir, "preset_template.blend")
        if not os.path.isfile(tmpl_path):
            self.report({'ERROR'},
                        f"preset_template.blend not found at '{tmpl_path}'.")
            return {'CANCELLED'}

        # Named "preset_<Name>", saved flat directly under ../Preset/.
        asset_name = f"preset_{stem}"
        dest_blend = os.path.join(preset_dir, f"{asset_name}.blend")
        if os.path.exists(dest_blend):
            self.report({'ERROR'}, f"Preset already exists: {dest_blend}")
            return {'CANCELLED'}

        export_objs, anchor_loc, anchor_mat = _resolve_export_objs(self, context)
        if export_objs is None:
            return {'CANCELLED'}

        # No instance drop (the Preset library is appended, not linked, so the
        # current scene doesn't need it); no sidecars (not instanced meshes);
        # open the finished file in a separate Blender instead.
        return _run_create(self, context, "preset", asset_name, tmpl_path,
                           preset_dir, dest_blend, export_objs, anchor_loc,
                           anchor_matrix=anchor_mat,
                           drop_instance=False, skip_sidecars=True,
                           open_after=True, dest_is_folder=False)


# ---------------------------------------------------------------------------
# Feature 3.3 — Run Thumbnail (Asset Browser right-click)
# ---------------------------------------------------------------------------

# Fixed description, so every thumbnail operation lands in one changelist and
# a second run stacks into the same one instead of opening another. Matching is
# by normalized description, the same cross-tool merge key the UE side uses.
THUMBNAIL_CL_DESC = "[Blender] [MapLink] Thumbnails"


def _checkout_for_thumbnails(paths, op=None):
    """Open every .blend for edit in the shared thumbnail CL.

    Best effort by design: no Perforce, or a file that simply is not in the
    depot, must not stop a thumbnail rebuild. Anything that stays read-only is
    reported so a failed save is not a mystery afterwards.
    """
    from . import p4_integration

    try:
        if not p4_integration.p4_available():
            return [], []
    except Exception:
        return [], []

    opened, refused = [], []
    for p in paths:
        try:
            ok, msg = p4_integration.checkout_or_add(
                p, description=THUMBNAIL_CL_DESC)
        except Exception as e:
            ok, msg = False, str(e)
        if ok:
            opened.append(p)
        elif not os.access(p, os.W_OK):
            # Only a genuine problem when the file is still read-only. An
            # already-open file returns False from add/edit and is fine.
            refused.append(f"{os.path.basename(p)}: {msg}")
        print(f"[MapLink Thumb] p4 {os.path.basename(p)}: {msg}")

    if refused and op is not None:
        op.report({'WARNING'},
                  f"{len(refused)} file(s) still read-only — see console.")
    return opened, refused


class MAPLINK_OT_RunThumbnail(Operator):
    bl_idname = "maplink.run_thumbnail"
    bl_label = "Run MapLink Thumbnail"
    bl_description = ("Regenerate the preview(s) for the selected asset(s). "
                      "Opens each asset .blend with a window to render the "
                      "preview, then refreshes the browser. The UI is frozen "
                      "while this runs")

    def execute(self, context):
        # Multi-selection: regenerate every selected asset; fall back to the
        # active one.
        assets = []
        try:
            assets = list(getattr(context, "selected_assets", None) or [])
        except Exception:
            assets = []
        if not assets:
            a = getattr(context, "asset", None)
            if a is not None:
                assets = [a]
        if not assets:
            self.report({'ERROR'}, "No asset selected.")
            return {'CANCELLED'}

        # Resolve each to its .blend path (dedupe).
        paths = []
        for a in assets:
            try:
                p = a.full_library_path
            except AttributeError:
                p = None
            if p and os.path.isfile(p) and p not in paths:
                paths.append(p)
        if not paths:
            self.report({'ERROR'}, "Could not resolve any asset .blend path.")
            return {'CANCELLED'}

        # The worker saves each .blend, so every file has to be writable before
        # a single one is launched. Perforce leaves them read-only, and the
        # save then fails inside a subprocess where the error is easy to miss.
        _checkout_for_thumbnails(paths, self)

        worker = os.path.join(os.path.dirname(__file__), "thumbnail_worker.py")
        win = context.window
        done = 0
        failed = []

        # Freeze the UI while regenerating: each worker is launched WINDOWED (a
        # real GPU context is needed to render previews) and run synchronously,
        # so the user can't interact until every preview is done.  Each worker
        # renders, saves and auto-closes itself.
        try:
            if win:
                win.cursor_set('WAIT')
        except Exception:
            pass
        try:
            for p in paths:
                try:
                    r = subprocess.run([bpy.app.binary_path, p,
                                        "--python", worker],
                                       capture_output=True, text=True,
                                       timeout=300)
                    out = r.stdout or ""
                    for line in out.splitlines():
                        print(line)
                    # Exit code is 0 either way; the worker states its result
                    # in the log because quit_blender() carries no status.
                    if r.returncode == 0 and "RESULT: OK" in out:
                        done += 1
                    else:
                        failed.append(os.path.basename(p))
                        if r.stderr:
                            print(r.stderr)
                except subprocess.TimeoutExpired:
                    failed.append(f"{os.path.basename(p)} (timeout)")
        finally:
            try:
                if win:
                    win.cursor_set('DEFAULT')
            except Exception:
                pass

        # Refresh the Asset Browser so the new previews show immediately.
        try:
            bpy.ops.asset.library_refresh()
        except Exception as e:
            print(f"[MapLink Thumb] library_refresh skipped: {e}")

        if done and not failed:
            self.report({'INFO'}, f"Updated {done} thumbnail(s).")
        elif done:
            self.report({'WARNING'},
                        f"Updated {done}; failed: {', '.join(failed)}")
        else:
            self.report({'ERROR'},
                        f"Thumbnail update failed: {', '.join(failed)}")
        return {'FINISHED'} if done else {'CANCELLED'}


# ---------------------------------------------------------------------------
# Feature 4 — Break Linked Collection → Local
#
# One-level "break" (like breaking an actor in Unreal, but in Blender):
#   • the linked collection instance's own geometry is copied in as LOOSE LOCAL
#     objects at the instance's world position (transform preserved);
#   • any nested collection-INSTANCES it contains are carried over as instances
#     (still linked) — they are NOT recursively exploded;
#   • everything lands in a NEW local collection that reuses the source
#     collection's name;
#   • the original instance (and its library) are left untouched — fully
#     non-destructive.
# ---------------------------------------------------------------------------

class MAPLINK_OT_BreakLinkedToLocal(Operator):
    bl_idname = "maplink.break_linked_to_local"
    bl_label = "Break Linked → Local"
    bl_description = ("Copy the selected linked collection instance's contents "
                      "in as loose local objects at the same position (nested "
                      "instances kept as instances), into a new same-named "
                      "collection. Non-destructive: the original is left as-is")

    @classmethod
    def poll(cls, context):
        try:
            return context.mode == 'OBJECT' and any(
                o.instance_type == 'COLLECTION' and o.instance_collection
                and o.instance_collection.library is not None
                for o in context.selected_objects)
        except Exception:
            return False

    def execute(self, context):
        import mathutils
        scene = context.scene

        targets = [o for o in context.selected_objects
                   if o.instance_type == 'COLLECTION'
                   and o.instance_collection
                   and o.instance_collection.library is not None]

        made = []
        done = 0
        for empty in targets:
            coll = empty.instance_collection
            # An instanced collection displays each member O at:
            #   world(O) = M_empty @ Translate(-instance_offset) @ world_in_coll(O)
            delta = (empty.matrix_world
                     @ mathutils.Matrix.Translation(-coll.instance_offset))
            group_name = coll.name      # carry over the same name

            # One level only: the collection's own objects plus the objects of
            # its organisational child collections (all_objects).  Nested
            # collection-INSTANCES are reached via instance_collection, not
            # membership, so they are NOT in all_objects — they get copied as
            # single instance empties below rather than exploded.
            src = list(coll.all_objects)

            grp = bpy.data.collections.new(group_name)
            scene.collection.children.link(grp)

            n_local = 0
            n_inst = 0
            first_pos = None
            for o in src:
                world = o.matrix_world.copy()     # authored transform within coll
                new = o.copy()                    # ID.copy() -> a LOCAL copy
                if new.instance_type == 'COLLECTION' and new.instance_collection:
                    # Nested sub-instance: keep it linked/instanced, don't explode.
                    n_inst += 1
                elif new.data is not None and getattr(new.data, "library", None):
                    new.data = new.data.copy()    # localise geometry (mats stay linked)
                    n_local += 1
                grp.objects.link(new)
                new.parent = None                 # loose
                new.matrix_world = delta @ world  # bake the instance position
                made.append(new)
                if first_pos is None:
                    first_pos = tuple(round(v, 3)
                                      for v in new.matrix_world.translation)

            print(f"[MapLink] BreakLinked: '{grp.name}' <- {n_local} local "
                  f"object(s) + {n_inst} sub-instance(s); "
                  f"empty@{tuple(round(v, 3) for v in empty.matrix_world.translation)} "
                  f"first_obj@{first_pos} (original left in place)")
            done += 1

        if not done:
            self.report({'WARNING'},
                        "No linked collection instances in selection.")
            return {'CANCELLED'}

        # Select the freshly-created objects for convenience.
        try:
            bpy.ops.object.select_all(action='DESELECT')
            for o in made:
                o.select_set(True)
            if made:
                context.view_layer.objects.active = made[0]
        except Exception:
            pass

        self.report({'INFO'},
                    f"Broke {done} instance(s) into local copies "
                    f"(transform kept; originals left).")
        return {'FINISHED'}


# ---------------------------------------------------------------------------
# Panel — MapLink > Tools  (Reload + Quick Create)
# ---------------------------------------------------------------------------

class MAPLINK_PT_Tools(Panel):
    bl_label = "Tools"
    bl_idname = "MAPLINK_PT_tools"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "MapLink"
    bl_order = 0

    def draw(self, context):
        layout = self.layout

        col = layout.column(align=True)
        col.scale_y = 1.2
        col.operator(MAPLINK_OT_ReloadBlend.bl_idname, icon='FILE_REFRESH')
        col.operator(MAPLINK_OT_QuickCreateAsset.bl_idname,
                     text="Create SM/LI Asset from Selection", icon='ADD')
        col.operator(MAPLINK_OT_CreatePreset.bl_idname,
                     text="Create Preset from Selection", icon='PRESET')
        col.operator(MAPLINK_OT_BreakLinkedToLocal.bl_idname,
                     text="Break Linked → Local", icon='UNLINKED')

        # Standardize — current-file standardization actions (v1.14: renamed
        # from "Conform"; batch equivalents live under Settings ▸ Batch).
        # The box header already says Standardize, so the buttons do not need
        # to repeat it. Two pairs, each a row: what is standardized on the
        # mesh, then what is standardized in the Outliner.
        box = layout.box()
        box.label(text="Standardize", icon='CHECKMARK')
        row = box.row(align=True)
        row.scale_y = 1.2
        row.operator("maplink.standardize_vc", text="VertCol", icon='COLOR')
        row.operator("maplink.standardize_uv", text="UVs", icon='UV')
        row = box.row(align=True)
        row.scale_y = 1.2
        row.operator("maplink.cleanup_outliner_names",
                     text="Outliner", icon='SORTALPHA')
        row.operator("maplink.cleanup_outliner_collections",
                     text="Collections", icon='OUTLINER_COLLECTION')

        # No thumbnail section here on purpose. Badging is not a thing the
        # artist invokes: it happens inside Run Thumbnail, which is where the
        # preview is regenerated anyway. Bulk work lives under Settings.


# ---------------------------------------------------------------------------
# Menu draws — kept separate from nav_drill's draw to isolate concerns.
# ---------------------------------------------------------------------------

def _viewport_menu(self, context):
    """Break Linked → Local in the 3D viewport object context menu.

    Quick Create lives in the MapLink > Tools side panel only (by design),
    so it is intentionally absent here.
    """
    if context.mode != 'OBJECT':
        return
    layout = self.layout
    layout.separator()
    # poll() greys this out when the selection has no linked collection instance.
    layout.operator(MAPLINK_OT_BreakLinkedToLocal.bl_idname,
                    text="Break Linked → Local", icon='UNLINKED')


# ---------------------------------------------------------------------------
# Registration — mirrors the per-module pattern used elsewhere.
# ---------------------------------------------------------------------------

class MAPLINK_OT_RebakeThumbnailBadge(Operator):
    bl_idname = "maplink.rebake_thumbnail_badge"
    bl_label = "Re-apply Type Badge"
    bl_description = ("Composite the asset type icon back into this file's "
                      "existing preview, without re-rendering it. Use after a "
                      "manual Generate Preview, which wipes the badge")
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return bool(bpy.data.filepath)

    def execute(self, context):
        from . import thumbnail_badge

        colls = thumbnail_badge.asset_collections()
        if not colls:
            self.report({'WARNING'},
                        "No asset-marked collection in this file.")
            return {'CANCELLED'}

        done, msgs = thumbnail_badge.badge_current_file()
        for m in msgs:
            print(f"[MapLink Badge] {m}")

        if not done:
            # The usual cause is an empty preview: nothing has rendered a
            # thumbnail for this asset yet, so there is no bitmap to composite
            # into. Say so rather than reporting a vague failure.
            self.report({'WARNING'},
                        f"Nothing badged — {msgs[0] if msgs else 'no preview'}. "
                        f"Run Thumbnail first.")
            return {'CANCELLED'}

        self.report({'INFO'},
                    f"Type badge re-applied to {done} preview(s). "
                    f"Save the file to keep it.")
        return {'FINISHED'}


class MAPLINK_OT_RebuildAllThumbnails(Operator):
    bl_idname = "maplink.rebuild_all_thumbnails"
    bl_label = "Rebuild All Thumbnails"
    bl_description = ("Regenerate the preview for every SM_/LI_/BP_ .blend "
                      "under the Batch Root, applying the type badge to each. "
                      "Opens one windowed Blender per file and checks them all "
                      "out into a single changelist. Slow — the UI is frozen "
                      "for the whole run")

    _paths = []

    def _root(self, context):
        """Batch Root if set, else the project root of the open file.

        Same source Batch Standardize reads, so the two agree on what "the
        project" means.
        """
        from . import batch_standardize
        try:
            root = getattr(context.scene.maplink_batch, "root_path", "") or ""
        except (AttributeError, TypeError):
            root = ""
        if not root and bpy.data.filepath:
            root = batch_standardize.find_project_root(
                os.path.dirname(bpy.data.filepath)) or ""
        return bpy.path.abspath(root) if root else ""

    def invoke(self, context, event):
        from . import batch_standardize

        root = self._root(context)
        if not root or not os.path.isdir(root):
            self.report({'ERROR'},
                        "No project root. Set Settings ▸ Batch ▸ Root first.")
            return {'CANCELLED'}

        # Any MapLink asset type, resolved through the one classifier rather
        # than a hardcoded prefix tuple. The tuple used to be SM_/LI_/BP_,
        # which silently excluded presets from bulk rebuilds — they were never
        # checked out and never re-rendered, with nothing said about it.
        from . import icons
        self._paths = [p for p in batch_standardize._discover_blends(root)
                       if icons.classify(os.path.basename(p)) is not None]
        if not self._paths:
            self.report({'WARNING'}, f"No SM_/LI_/BP_ .blend under {root}")
            return {'CANCELLED'}

        return context.window_manager.invoke_props_dialog(self, width=460)

    def draw(self, context):
        col = self.layout.column()
        col.label(text=f"{len(self._paths)} file(s) will be re-rendered.",
                  icon='IMAGE_DATA')
        col.separator()
        col.label(text="One windowed Blender per file, run one at a time.")
        col.label(text="Every file is checked out into:")
        col.label(text=f"    {THUMBNAIL_CL_DESC}", icon='DOT')
        col.separator()
        col.label(text="The UI is frozen until it finishes.", icon='INFO')

    def execute(self, context):
        if not self._paths:
            return {'CANCELLED'}

        _checkout_for_thumbnails(self._paths, self)

        worker = os.path.join(os.path.dirname(__file__), "thumbnail_worker.py")
        win = context.window
        done, failed = 0, []
        try:
            if win:
                win.cursor_set('WAIT')
        except Exception:
            pass
        try:
            for p in self._paths:
                try:
                    r = subprocess.run([bpy.app.binary_path, p,
                                        "--python", worker],
                                       capture_output=True, text=True,
                                       timeout=300)
                    out = r.stdout or ""
                    for line in out.splitlines():
                        print(line)
                    # Exit code is 0 either way; the worker states its result
                    # in the log because quit_blender() carries no status.
                    if r.returncode == 0 and "RESULT: OK" in out:
                        done += 1
                    else:
                        failed.append(os.path.basename(p))
                        if r.stderr:
                            print(r.stderr)
                except subprocess.TimeoutExpired:
                    failed.append(f"{os.path.basename(p)} (timeout)")
        finally:
            try:
                if win:
                    win.cursor_set('DEFAULT')
            except Exception:
                pass

        try:
            bpy.ops.asset.library_refresh()
        except Exception as e:
            print(f"[MapLink Thumb] library_refresh skipped: {e}")

        if failed:
            self.report({'WARNING'},
                        f"Rebuilt {done}; failed: {', '.join(failed[:5])}"
                        + (" …" if len(failed) > 5 else ""))
        else:
            self.report({'INFO'}, f"Rebuilt {done} thumbnail(s).")
        return {'FINISHED'}


class MAPLINK_OT_ProbePreview(Operator):
    bl_idname = "maplink.probe_preview"
    bl_label = "Probe Preview Buffer"
    bl_description = ("Print this asset's preview buffer layout to the console "
                      "(size, byte vs float, value range). Diagnostic for "
                      "thumbnail badging")
    bl_options = {'REGISTER'}

    @classmethod
    def poll(cls, context):
        return bool(bpy.data.filepath)

    def execute(self, context):
        from . import thumbnail_badge

        colls = thumbnail_badge.asset_collections()
        if not colls:
            self.report({'WARNING'}, "No asset-marked collection in this file.")
            return {'CANCELLED'}

        for coll in colls:
            print(f"[MapLink Probe] --- {coll.name} ---")
            for k, v in thumbnail_badge.probe(coll).items():
                print(f"[MapLink Probe]   {k}: {v}")
        self.report({'INFO'}, "Preview buffer details printed to the console.")
        return {'FINISHED'}


_classes = (
    MAPLINK_OT_ReloadBlend,
    MAPLINK_OT_QuickCreateAsset,
    MAPLINK_OT_CreatePreset,
    MAPLINK_OT_RunThumbnail,
    MAPLINK_OT_RebuildAllThumbnails,
    MAPLINK_OT_RebakeThumbnailBadge,
    MAPLINK_OT_ProbePreview,
    MAPLINK_OT_BreakLinkedToLocal,
)


def register():
    for c in _classes:
        bpy.utils.register_class(c)

    # Viewport entry moved to context_menu.MAPLINK_MT_object_context.

    # Asset Browser entry moved to context_menu.MAPLINK_MT_asset_context, which
    # also owns the build-mismatch guard that used to live here.


def unregister():
    # detach, not remove: a reinstall hands us a freshly created function
    # object, so removing by identity silently misses the one actually attached
    # and the entry survives into the next append as a duplicate.
    ui_menus.remove_all(getattr(bpy.types, "ASSETBROWSER_MT_context_menu", None),
                        "_assetbrowser_menu")
    ui_menus.remove_all(bpy.types.VIEW3D_MT_object_context_menu, "_viewport_menu")

    for c in reversed(_classes):
        bpy.utils.unregister_class(c)
