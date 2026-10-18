"""MapLink_Blender — Hierarchy navigation (drill down / up).

Prefab drill.  Alt+Page Down opens a linked collection
instance's source .blend.  Alt+Page Up returns.  Session-only stack.

Context is manually managed via Add/Delete/Update buttons.
Stacks context for the full drill chain: every ancestor (parent, grandparent,
…up to the root) is shown, composed through the instance matrices.
Context files are hidden (Windows).
"""

import ctypes
import glob
import os
import platform
import subprocess
import time
from pathlib import Path

import bpy
from bpy.app.handlers import persistent
from bpy.types import Operator, PropertyGroup
from bpy.props import BoolProperty, EnumProperty
from . import ui_menus

from . import p4_integration


# ---------------------------------------------------------------------------
# Open Recent integration (v1.4.5)
#
# bpy.ops.wm.open_mainfile already updates Blender's "File > Open Recent" list
# in a normal interactive session, but when we reach a path *programmatically*
# (drill-down / drill-up / jump-to-ancestor) we register it explicitly so the
# drilled file always lands in the recent list.  Best-effort and fully silent:
# any failure is printed and swallowed, never blocking the drill.
# ---------------------------------------------------------------------------

def _recent_files_path():
    """Resolve the path to Blender's recent-files.txt (kw/positional safe)."""
    try:
        return bpy.utils.user_resource('CONFIG', path="recent-files.txt")
    except TypeError:
        # Older/newer signatures may take 'path' positionally.
        return bpy.utils.user_resource('CONFIG', "recent-files.txt")


def _recent_files_limit(default=30):
    """Max recent-files count from prefs, robust across RNA layouts."""
    prefs = bpy.context.preferences
    for owner in (getattr(prefs, "filepaths", None),
                  getattr(prefs, "system", None)):
        n = getattr(owner, "recent_files", None)
        if isinstance(n, int) and n > 0:
            return n
    return default


def _register_recent_file(filepath: str):
    """Best-effort: add filepath to the top of File > Open Recent."""
    try:
        recent_path = _recent_files_path()
        filepath = os.path.abspath(bpy.path.abspath(filepath))

        try:
            with open(recent_path, 'r', encoding='utf-8') as f:
                lines = [l.strip() for l in f if l.strip()]
        except FileNotFoundError:
            lines = []

        # Drop any existing entry for this file (case/path-normalised), then
        # push it to the front.
        lines = [l for l in lines if os.path.abspath(l) != filepath]
        lines.insert(0, filepath)
        lines = lines[:_recent_files_limit()]

        with open(recent_path, 'w', encoding='utf-8') as f:
            f.write('\n'.join(lines) + '\n')
    except Exception as e:
        print(f"[MapLink] Open Recent registration skipped: {e}")

# ---------------------------------------------------------------------------
# State
# ---------------------------------------------------------------------------

_drill_stack: list[dict] = []
_drill_in_progress: bool = False
_pending_drill: dict | None = None
_context_loaded: bool = False
_context_was_loaded_before_save: bool = False
_last_save_time: float = 0.0  # monotonic seconds; 0 means "never saved this session"

_CONTEXT_COL = "_MapLink_Context"
_CONTEXT_FILENAME = "_maplink_context.blend"
# Legacy: the dropped Flat mode wrote this alongside the context. Still
# recognised so leftovers are unlinked and deleted rather than orphaned.
_CONTEXT_FLAT_FILENAME = "_maplink_context_flat.blend"
_CONTEXT_ANCHOR_PREFIX = "_ML_CTX_"
_NOEXPORT = "NoExport"
_SAVE_PROMPT_GRACE_SECONDS = 60.0


# ---------------------------------------------------------------------------
# Properties
# ---------------------------------------------------------------------------

# Context display mode -> Object.display_type.
#
# Solid and Wire only. A third "flat" mode -- context white, scene shaded --
# was tried and dropped: Blender applies one solid-shading color_type to the
# whole viewport, and the context .blend is a file of LINKS to the real assets,
# so there is nothing local to strip materials from. The only way to get it is
# to localise every mesh into the context cache, which turns a few hundred KB
# of links into a full duplicate of the map. Not worth it; Wire already
# separates context from what you are editing.
_CONTEXT_DISPLAY = {
    'SHADED':    'TEXTURED',
    'WIREFRAME': 'WIRE',
}


def _update_context_display(self, context):
    display = _CONTEXT_DISPLAY.get(self.context_wireframe, 'TEXTURED')
    for obj in bpy.data.objects:
        if obj.name.startswith(_CONTEXT_ANCHOR_PREFIX):
            obj.display_type = display


class MAPLINK_PG_NavProps(PropertyGroup):
    auto_context: BoolProperty(
        name="Context on Drill",
        description="Automatically generate context when drilling down",
        default=False,
    )  # type: ignore
    prompt_save_before_drill: BoolProperty(
        name="Prompt Save Before Drill",
        description=("Ask whether to save the current .blend before drilling "
                     "when it has unsaved changes and wasn't saved in the "
                     "last minute"),
        default=False,
    )  # type: ignore
    context_wireframe: EnumProperty(
        name="Display Mode",
        description="Display context as shaded or as wireframe",
        items=[
            ('SHADED', "Solid",
             "Show context with full textured shading",
             'SHADING_TEXTURE', 0),
            ('WIREFRAME', "Wire",
             "Show context as wireframe only",
             'SHADING_WIRE', 1),
        ],
        default='SHADED',
        update=_update_context_display,
    )  # type: ignore


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def reset_drill_stack():
    global _drill_stack, _context_loaded, _pending_drill
    _drill_stack.clear()
    _context_loaded = False
    _pending_drill = None


def is_drilled() -> bool:
    return len(_drill_stack) > 0


def get_breadcrumb() -> list[str]:
    crumbs = []
    for entry in _drill_stack:
        crumbs.append(Path(entry["filepath"]).stem)
    try:
        if bpy.data.filepath:
            crumbs.append(Path(bpy.data.filepath).stem)
    except (AttributeError, TypeError):
        pass
    return crumbs


def has_context() -> bool:
    for obj in bpy.data.objects:
        if obj.name.startswith(_CONTEXT_ANCHOR_PREFIX):
            return True
    for lib in bpy.data.libraries:
        if _is_context_lib(lib):
            return True
    return False


def context_hidden() -> bool:
    for obj in bpy.data.objects:
        if obj.name.startswith(_CONTEXT_ANCHOR_PREFIX):
            return obj.hide_viewport
    return False


# ---------------------------------------------------------------------------
# Context file management
# ---------------------------------------------------------------------------

def _is_context_lib(lib) -> bool:
    """True for the context library, or a leftover flat variant.

    Both names are matched so a flat file written by the experimental build
    is still recognised and unlinked instead of being orphaned in the file.
    Note the flat name does not end with the shaded one, so a single
    endswith() would miss it.
    """
    # Blender library paths are often relative and mix separators
    # ("//..\LI_x\file.blend"), so split on both rather than trusting
    # os.path.basename to know what a separator is.
    name = lib.filepath.replace("\\", "/").rsplit("/", 1)[-1]
    return name in (_CONTEXT_FILENAME, _CONTEXT_FLAT_FILENAME)


def _context_path_for(blend_path: str) -> str:
    return os.path.join(os.path.dirname(blend_path), _CONTEXT_FILENAME)


def _flat_variant_of(ctx_path: str) -> str:
    """Path the dropped Flat mode used. Kept so cleanup removes leftovers."""
    return ctx_path[:-len(".blend")] + "_flat.blend"


def _hide_file(filepath: str):
    """Set file to hidden on Windows."""
    if platform.system() == "Windows":
        try:
            # FILE_ATTRIBUTE_HIDDEN = 0x02
            ctypes.windll.kernel32.SetFileAttributesW(filepath, 0x02)
        except Exception:
            pass


def _generate_context_file(parent_blend: str, exclude_blend: str = "") -> str | None:
    ctx_path = _context_path_for(parent_blend)
    script = os.path.join(os.path.dirname(__file__), "strip_context.py")

    cmd = [
        bpy.app.binary_path,
        "--factory-startup",
        "--background",
        parent_blend,
        "--python", script,
        "--", ctx_path,
    ]
    if exclude_blend:
        cmd.append(exclude_blend)

    try:
        r = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=120,
            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0),
        )
        if r.returncode != 0:
            print(f"[MapLink] Context strip failed:\n{r.stderr}")
            return None
    except Exception as exc:
        print(f"[MapLink] Context generation error: {exc}")
        return None

    if os.path.exists(ctx_path):
        _hide_file(ctx_path)
        return ctx_path
    return None


def _ensure_noexport_collection() -> bpy.types.Collection:
    scene = bpy.context.scene
    noexport = None
    for col in bpy.data.collections:
        if col.name.strip().lower() == "noexport" and not col.library:
            noexport = col
            break

    if noexport is None:
        noexport = bpy.data.collections.new(_NOEXPORT)
        scene.collection.children.link(noexport)

    if noexport.name not in scene.collection.children:
        try:
            scene.collection.children.link(noexport)
        except RuntimeError:
            pass

    # Only flip when needed.  Touching Collection.hide_viewport is mostly
    # harmless, but we keep it minimal anyway.
    if noexport.hide_viewport:
        noexport.hide_viewport = False
    noexport.hide_render = True

    # Make NoExport visible/included in the view layer WITHOUT clobbering
    # the visibility of unrelated child collections inside NoExport.
    #
    # Toggling LayerCollection.exclude from True→False rebuilds the entire
    # children layer-collection subtree, which resets every descendant's
    # hide_viewport / exclude flags to the defaults.  That used to wipe out
    # the user's eye-icon state on unrelated sub-collections (e.g. a custom
    # "NoExport/HelperGeo" the user had hidden).  Same hazard for
    # LayerCollection.hide_viewport, though milder.  We capture descendant
    # state first, apply the minimal change, then restore.

    def _find_lc(lc):
        if lc.collection == noexport:
            return lc
        for ch in lc.children:
            found = _find_lc(ch)
            if found:
                return found
        return None

    noexport_lc = _find_lc(bpy.context.view_layer.layer_collection)
    if noexport_lc is None:
        return noexport

    def _capture(lc, out):
        for ch in lc.children:
            out[ch.collection.name] = {
                'exclude': ch.exclude,
                'hide_viewport': ch.hide_viewport,
            }
            _capture(ch, out)

    saved: dict = {}
    _capture(noexport_lc, saved)

    changed = False
    if noexport_lc.exclude:
        noexport_lc.exclude = False
        changed = True
    if noexport_lc.hide_viewport:
        noexport_lc.hide_viewport = False
        changed = True

    if changed and saved:
        def _restore(lc):
            for ch in lc.children:
                s = saved.get(ch.collection.name)
                if s is not None:
                    if ch.exclude != s['exclude']:
                        ch.exclude = s['exclude']
                    if ch.hide_viewport != s['hide_viewport']:
                        ch.hide_viewport = s['hide_viewport']
                _restore(ch)
        _restore(noexport_lc)

    return noexport


def _link_single_context(ctx_path: str, anchor_matrix_flat: list[float],
                         noexport: bpy.types.Collection, suffix: str = ""):
    import mathutils

    try:
        with bpy.data.libraries.load(ctx_path, link=True) as (data_from, data_to):
            if _CONTEXT_COL in data_from.collections:
                data_to.collections = [_CONTEXT_COL]
            else:
                print(f"[MapLink] No '{_CONTEXT_COL}' in {ctx_path}")
                return
    except Exception as exc:
        print(f"[MapLink] Failed to link context: {exc}")
        return

    linked_col = None
    ctx_norm = os.path.normpath(ctx_path).lower()
    for col in bpy.data.collections:
        if col.name.startswith(_CONTEXT_COL) and col.library:
            lib_norm = os.path.normpath(bpy.path.abspath(col.library.filepath)).lower()
            if lib_norm == ctx_norm:
                linked_col = col
                break

    if linked_col is None:
        print(f"[MapLink] Linked context collection not found for {ctx_path}")
        return

    anchor = bpy.data.objects.new(f"{_CONTEXT_ANCHOR_PREFIX}{suffix}", None)
    anchor.instance_type = 'COLLECTION'
    anchor.instance_collection = linked_col
    anchor.hide_select = True
    anchor.hide_render = True

    # Same table the property update uses, so a new anchor and an existing one
    # can never disagree about what the current mode means.
    try:
        mode = bpy.context.scene.maplink_nav.context_wireframe
    except (AttributeError, TypeError):
        mode = 'SHADED'
    anchor.display_type = _CONTEXT_DISPLAY.get(mode, 'TEXTURED')

    mat = mathutils.Matrix((
        anchor_matrix_flat[0:4],
        anchor_matrix_flat[4:8],
        anchor_matrix_flat[8:12],
        anchor_matrix_flat[12:16],
    ))
    anchor.matrix_world = mat

    noexport.objects.link(anchor)


def _link_contexts(contexts: list[dict]):
    global _context_loaded

    _unlink_context()

    if not contexts:
        return

    noexport = _ensure_noexport_collection()

    for i, ctx in enumerate(contexts):
        ctx_path = ctx.get("ctx_path")
        mat_flat = ctx.get("matrix_flat")
        if ctx_path and mat_flat and os.path.exists(ctx_path):
            _link_single_context(ctx_path, mat_flat, noexport, f"L{i}")

    _context_loaded = True


def _unlink_context():
    global _context_loaded
    for obj in list(bpy.data.objects):
        if obj.name.startswith(_CONTEXT_ANCHOR_PREFIX):
            bpy.data.objects.remove(obj, do_unlink=True)
    for lib in list(bpy.data.libraries):
        if _is_context_lib(lib):
            bpy.data.libraries.remove(lib)
    _context_loaded = False


def _cleanup_stale_context_files():
    try:
        fp = bpy.data.filepath
    except (AttributeError, TypeError):
        return
    if not fp:
        return
    ctx = _context_path_for(fp)
    # Both variants, or the flat one lingers as a stale orphan next to a
    # regenerated shaded file.
    for path in (ctx, _flat_variant_of(ctx)):
        if os.path.exists(path):
            try:
                os.remove(path)
            except OSError:
                pass


def _build_contexts_for_current_level() -> list[dict]:
    """Build the visual context for every ancestor on the drill stack.

    Walks the full stack from the immediate parent up to the root, composing
    instance matrices cumulatively so each ancestor's stripped context file is
    placed correctly in the current file's space.  Ancestors whose context
    file is missing are skipped, but their instance matrix still contributes to
    the chain so deeper ancestors stay aligned.
    """
    contexts = []
    if not _drill_stack:
        return contexts

    import mathutils

    def _to_mat(flat):
        return mathutils.Matrix((flat[0:4], flat[4:8], flat[8:12], flat[12:16]))

    # Accumulate the instance-matrix product from the immediate parent (top of
    # stack) upward.  At each ancestor the cumulative product maps that
    # ancestor's space down into the current file; its inverse places the
    # ancestor's context around us.
    #   parent:       inv(P)
    #   grandparent:  inv(GP @ P)
    #   great-gp:     inv(GGP @ GP @ P)   ... etc, full depth.
    cumulative = mathutils.Matrix.Identity(4)
    for entry in reversed(_drill_stack):
        try:
            cumulative = _to_mat(entry["instance_matrix"]) @ cumulative
        except (KeyError, ValueError, TypeError):
            # Malformed entry — leave cumulative as-is and keep going.
            pass
        ctx_path = _context_path_for(entry["filepath"])
        if not os.path.exists(ctx_path):
            continue
        try:
            inv = cumulative.inverted()
        except ValueError:
            inv = mathutils.Matrix.Identity(4)
        mat_flat = [inv[r][c] for r in range(4) for c in range(4)]
        contexts.append({"ctx_path": ctx_path, "matrix_flat": mat_flat})

    return contexts


def _checkout_and_save() -> bool:
    fp = bpy.data.filepath
    if fp:
        ok, msg = p4_integration.checkout_file(fp)
        if not ok:
            print(f"[MapLink] Auto-checkout warning: {msg}")
    try:
        bpy.ops.wm.save_mainfile()
        return True
    except RuntimeError as exc:
        print(f"[MapLink] Save failed: {exc}")
        return False


# ---------------------------------------------------------------------------
# Drill save prompt (opt-in via the "Prompt Save Before Drill" setting)
# ---------------------------------------------------------------------------

_DRILL_SAVE_MODE_ITEMS = [
    ('SAVE', "Save + Drill",
     "Save the current .blend then drill",
     'FILE_TICK', 0),
    ('DISCARD', "Drill without Saving",
     "Drill immediately, discarding unsaved changes",
     'TRASH', 1),
]


def _should_prompt_save() -> bool:
    """True only when ALL of the following hold:

    * the user has enabled the prompt setting,
    * the current .blend has a real on-disk path,
    * it has unsaved changes,
    * it was last saved more than _SAVE_PROMPT_GRACE_SECONDS ago
      (i.e. the user hasn't just hit Ctrl+S a moment ago).
    """
    try:
        nav = bpy.context.scene.maplink_nav
        if not nav.prompt_save_before_drill:
            return False
    except (AttributeError, TypeError):
        return False

    try:
        fp = bpy.data.filepath
    except (AttributeError, TypeError):
        return False
    if not fp:
        return False

    if not bool(getattr(bpy.data, 'is_dirty', False)):
        return False

    if _last_save_time > 0.0:
        if (time.monotonic() - _last_save_time) < _SAVE_PROMPT_GRACE_SECONDS:
            return False

    return True


def _save_before_drill() -> tuple[bool, str]:
    """Persist the current file prior to a drill.  The save_pre handler
    in this module already unlinks context and performs P4 auto-checkout
    when drilled, so we only need to call save_mainfile."""
    try:
        bpy.ops.wm.save_mainfile()
    except RuntimeError as exc:
        return False, str(exc)
    return True, ""


def _drill_invoke(op, context):
    """Shared invoke for drill operators.  Pop the save-prompt dialog if
    the gate conditions are met; otherwise execute directly."""
    op.save_mode = 'SAVE'
    if _should_prompt_save():
        return context.window_manager.invoke_props_dialog(op, width=340)
    return op.execute(context)


def _drill_draw(op, layout, direction: str):
    """Shared draw() body for the drill-confirm popup."""
    layout.label(text="Current file has unsaved changes.", icon='ERROR')
    try:
        fp = bpy.data.filepath
        if fp:
            layout.label(text=Path(fp).name)
    except (AttributeError, TypeError):
        pass
    layout.separator()
    layout.prop(op, "save_mode", expand=True)
    layout.separator()
    layout.label(text=f"Cancel aborts the drill {direction}.")


def _maybe_save_for_drill(op) -> tuple[bool, str]:
    """Honor the save_mode choice if the prompt was actually shown.

    The prompt-shown signal is "_should_prompt_save() is still True", which
    will only be True when the setting is on AND the file is still dirty
    AND we're past the grace window — i.e. the same gate that opened the
    dialog.  If invoke skipped the dialog, this returns (True, '') without
    touching the file.
    """
    if not _should_prompt_save():
        return True, ""
    if op.save_mode == 'SAVE':
        return _save_before_drill()
    # DISCARD — proceed without saving
    return True, ""


# ---------------------------------------------------------------------------
# View preservation
# ---------------------------------------------------------------------------

def _get_region_3d():
    """Find the active 3D viewport's region_3d."""
    try:
        for area in bpy.context.screen.areas:
            if area.type == 'VIEW_3D':
                return area.spaces.active.region_3d
    except (AttributeError, TypeError):
        pass
    return None


def _get_space_lens():
    """Focal length of the 3D viewport, or None.

    Lives on SpaceView3D, not RegionView3D, so it is fetched separately from
    the rest of the view state.
    """
    try:
        for area in bpy.context.screen.areas:
            if area.type == 'VIEW_3D':
                return area.spaces.active.lens
    except (AttributeError, TypeError):
        pass
    return None


def _set_space_lens(lens):
    """Restore the viewport focal length. Silent no-op when unavailable."""
    if not lens:
        return
    try:
        for area in bpy.context.screen.areas:
            if area.type == 'VIEW_3D':
                area.spaces.active.lens = lens
    except (AttributeError, TypeError):
        pass


def _capture_view() -> dict | None:
    """Capture current viewport camera state."""
    r3d = _get_region_3d()
    if r3d is None:
        return None
    return {
        "view_location": list(r3d.view_location),
        "view_rotation": list(r3d.view_rotation),
        "view_distance": r3d.view_distance,
        "view_perspective": r3d.view_perspective,
        # Focal length is a property of the space, not the region, and it is
        # what makes a drilled view feel like the same camera. Without it the
        # lens snaps back to the new file's default on every hop.
        "lens": _get_space_lens(),
    }


def _transform_view(view: dict, matrix_flat: list[float], invert: bool = False) -> dict:
    """Transform captured view through an instance matrix.

    For drill DOWN: invert=True  (parent→child, apply M.inverted())
    For drill UP:   invert=False (child→parent, apply M directly)
    """
    import mathutils

    M = mathutils.Matrix((
        matrix_flat[0:4], matrix_flat[4:8],
        matrix_flat[8:12], matrix_flat[12:16],
    ))

    if invert:
        try:
            M = M.inverted()
        except ValueError:
            return view

    # Transform orbit center as a point
    loc = mathutils.Vector(view["view_location"])
    new_loc = M @ loc

    # Transform rotation by the rotation component of M
    old_rot = mathutils.Quaternion(view["view_rotation"])
    m_rot = M.to_3x3().normalized().to_quaternion()
    new_rot = m_rot @ old_rot

    return {
        "view_location": list(new_loc),
        "view_rotation": list(new_rot),
        "view_distance": view["view_distance"],
        "view_perspective": view["view_perspective"],
        # Focal length is a property of the lens, not of where the camera is,
        # so the instance matrix must not touch it.
        "lens": view.get("lens"),
    }


def _restore_view(view: dict):
    """Apply captured view state to the active 3D viewport."""
    import mathutils

    r3d = _get_region_3d()
    if r3d is None:
        return

    r3d.view_location = mathutils.Vector(view["view_location"])
    r3d.view_rotation = mathutils.Quaternion(view["view_rotation"])
    r3d.view_distance = view["view_distance"]
    # Carry the focal length across the hop. Without it the lens resets to the
    # opened file's default and the same scene reads at a different width.
    _set_space_lens(view.get("lens"))
    if view.get("view_perspective") in {'PERSP', 'ORTHO'}:
        r3d.view_perspective = view["view_perspective"]


# ---------------------------------------------------------------------------
# Handlers
# ---------------------------------------------------------------------------

@persistent
def _on_save_pre(*_args):
    global _context_was_loaded_before_save
    if _drill_in_progress:
        return

    _context_was_loaded_before_save = _context_loaded or has_context()

    found = False
    for lib in bpy.data.libraries:
        if _is_context_lib(lib):
            found = True
            break
    for obj in bpy.data.objects:
        if obj.name.startswith(_CONTEXT_ANCHOR_PREFIX):
            found = True
            break

    if found:
        _unlink_context()

    if is_drilled() and bpy.data.filepath:
        try:
            ok, msg = p4_integration.checkout_file(bpy.data.filepath)
            if not ok:
                print(f"[MapLink] Auto-checkout warning: {msg}")
        except Exception as exc:
            print(f"[MapLink] Auto-checkout error: {exc}")


@persistent
def _on_save_post(*_args):
    global _context_was_loaded_before_save, _last_save_time
    # Stamp the timestamp on every save so the drill prompt's grace window
    # works even when the save happens outside a drill.
    _last_save_time = time.monotonic()

    if _drill_in_progress:
        return
    if not is_drilled():
        return
    if not _context_was_loaded_before_save:
        return
    _context_was_loaded_before_save = False

    contexts = _build_contexts_for_current_level()
    if contexts:
        _link_contexts(contexts)


@persistent
def _on_load_post(*_args):
    global _pending_drill, _context_loaded, _last_save_time

    # Reset the save-time stamp — the grace window must reflect saves of
    # *this* file, not whatever was open before.
    _last_save_time = 0.0

    if _pending_drill is not None:
        pending = _pending_drill
        _pending_drill = None
        contexts = pending.get("contexts")
        if contexts:
            _link_contexts(contexts)
        # Restore camera view
        view = pending.get("view")
        if view:
            _restore_view(view)
        return

    reset_drill_stack()
    _cleanup_stale_context_files()


@persistent
def _on_quit(*_args):
    for entry in _drill_stack:
        ctx = _context_path_for(entry["filepath"])
        if os.path.exists(ctx):
            try:
                os.remove(ctx)
            except OSError:
                pass
    _cleanup_stale_context_files()


# ---------------------------------------------------------------------------
# Drill Operators
# ---------------------------------------------------------------------------

class MAPLINK_OT_DrillDown(Operator):
    bl_idname = "maplink.drill_down"
    bl_label = "Drill Down"
    bl_description = "Open linked collection instance's source .blend"
    bl_options = {'INTERNAL'}

    fresh_context: BoolProperty(default=True)  # type: ignore
    save_mode: EnumProperty(
        name="If file has unsaved changes",
        items=_DRILL_SAVE_MODE_ITEMS,
        default='SAVE',
    )  # type: ignore

    @classmethod
    def poll(cls, context):
        obj = context.active_object
        if not obj:
            return False
        try:
            # Access a cheap attribute to verify RNA is still valid before
            # touching users_collection (which iterates all collections and
            # can segfault if the backing RNA struct has been freed).
            _ = obj.name
            if obj.instance_type != 'COLLECTION':
                return False
            if not obj.instance_collection:
                return False
            if not obj.instance_collection.library:
                return False
            for col in obj.users_collection:
                if col.name.strip().lower() == "noexport":
                    return False
        except Exception:
            return False
        return True

    def invoke(self, context, event):
        return _drill_invoke(self, context)

    def draw(self, context):
        _drill_draw(self, self.layout, direction="down")

    def execute(self, context):
        global _drill_in_progress, _pending_drill

        ok, err = _maybe_save_for_drill(self)
        if not ok:
            self.report({'ERROR'}, f"Save failed: {err}")
            return {'CANCELLED'}

        obj = context.active_object
        if not obj or not obj.instance_collection or not obj.instance_collection.library:
            self.report({'ERROR'}, "No valid target")
            return {'CANCELLED'}

        target_lib = bpy.path.abspath(obj.instance_collection.library.filepath)

        if not os.path.exists(target_lib):
            self.report({'ERROR'}, f"File not found: {target_lib}")
            return {'CANCELLED'}

        # Read auto_context from scene property
        try:
            auto_ctx = context.scene.maplink_nav.auto_context
        except (AttributeError, TypeError):
            auto_ctx = False

        mat = obj.matrix_world
        mat_flat = [mat[row][col] for row in range(4) for col in range(4)]
        current_filepath = bpy.data.filepath

        do_fresh = self.fresh_context and auto_ctx
        self.fresh_context = True

        # Push to stack
        _drill_stack.append({
            "filepath": current_filepath,
            "instance_matrix": mat_flat,
        })

        # Generate context if fresh
        if do_fresh:
            _generate_context_file(current_filepath, exclude_blend=target_lib)

        # Build contexts for the new level
        contexts = _build_contexts_for_current_level()

        # Capture and transform viewport camera
        view = _capture_view()
        if view:
            view = _transform_view(view, mat_flat, invert=True)

        _pending_drill = {
            "target": target_lib,
            "contexts": contexts if (contexts and do_fresh) else None,
            "view": view,
        }

        _drill_in_progress = True
        try:
            _register_recent_file(target_lib)
            bpy.ops.wm.open_mainfile(filepath=target_lib)
        finally:
            _drill_in_progress = False

        self.report({'INFO'}, f"Drilled into {Path(target_lib).stem}")
        return {'FINISHED'}


class MAPLINK_OT_DrillUp(Operator):
    bl_idname = "maplink.drill_up"
    bl_label = "Drill Up"
    bl_description = "Return to parent .blend"
    bl_options = {'INTERNAL'}

    save_mode: EnumProperty(
        name="If file has unsaved changes",
        items=_DRILL_SAVE_MODE_ITEMS,
        default='SAVE',
    )  # type: ignore

    @classmethod
    def poll(cls, context):
        return is_drilled()

    def invoke(self, context, event):
        return _drill_invoke(self, context)

    def draw(self, context):
        _drill_draw(self, self.layout, direction="up")

    def execute(self, context):
        global _drill_in_progress, _pending_drill

        if not _drill_stack:
            self.report({'WARNING'}, "Already at root")
            return {'CANCELLED'}

        ok, err = _maybe_save_for_drill(self)
        if not ok:
            self.report({'ERROR'}, f"Save failed: {err}")
            return {'CANCELLED'}

        # Clean up context file for current parent
        parent_entry = _drill_stack[-1]
        ctx = _context_path_for(parent_entry["filepath"])
        if os.path.exists(ctx):
            try:
                os.remove(ctx)
            except OSError:
                pass

        parent = _drill_stack.pop()
        target = parent["filepath"]
        parent_mat = parent["instance_matrix"]

        # Capture and transform viewport camera (child→parent: apply M directly)
        view = _capture_view()
        if view:
            view = _transform_view(view, parent_mat, invert=False)

        contexts = _build_contexts_for_current_level() if _drill_stack else []

        _pending_drill = {
            "target": target,
            "contexts": contexts if contexts else None,
            "view": view,
        }

        _drill_in_progress = True
        try:
            _register_recent_file(target)
            bpy.ops.wm.open_mainfile(filepath=target)
        finally:
            _drill_in_progress = False

        self.report({'INFO'}, f"Returned to {Path(target).stem}")
        return {'FINISHED'}


class MAPLINK_OT_DrillToAncestor(Operator):
    """Jump to an arbitrary ancestor in the breadcrumb stack.

    target_index is the position in the breadcrumb list (== position in
    _drill_stack).  A click on the current file is a no-op.  Any earlier
    crumb collapses the stack down to that level, cleaning up the context
    sidecar files at every level we skip over.
    """
    bl_idname = "maplink.drill_to_ancestor"
    bl_label = "Drill To Ancestor"
    bl_description = "Jump up to this ancestor in the breadcrumb"
    bl_options = {'INTERNAL'}

    target_index: bpy.props.IntProperty(default=0)  # type: ignore
    save_mode: EnumProperty(
        name="If file has unsaved changes",
        items=_DRILL_SAVE_MODE_ITEMS,
        default='SAVE',
    )  # type: ignore

    @classmethod
    def poll(cls, context):
        return is_drilled()

    def invoke(self, context, event):
        return _drill_invoke(self, context)

    def draw(self, context):
        _drill_draw(self, self.layout, direction="up")

    def execute(self, context):
        global _drill_in_progress, _pending_drill

        idx = self.target_index
        if idx < 0 or idx >= len(_drill_stack):
            # Either clicking the current file, or a stale index.  No-op.
            return {'CANCELLED'}

        ok, err = _maybe_save_for_drill(self)
        if not ok:
            self.report({'ERROR'}, f"Save failed: {err}")
            return {'CANCELLED'}

        # Collapse the stack down to length == idx, cleaning context files
        # for every level we pass through.  The entry at position idx is
        # the file we want to land in, so we pop it last and load it.
        target_filepath = None
        while len(_drill_stack) > idx:
            entry = _drill_stack[-1]
            ctx = _context_path_for(entry["filepath"])
            if os.path.exists(ctx):
                try:
                    os.remove(ctx)
                except OSError:
                    pass
            popped = _drill_stack.pop()
            if len(_drill_stack) == idx:
                target_filepath = popped["filepath"]
                break

        if not target_filepath:
            return {'CANCELLED'}

        # Multi-level jump: we don't try to compose the cascaded view
        # transforms — just let the target file's stored view show.
        contexts = _build_contexts_for_current_level() if _drill_stack else []

        _pending_drill = {
            "target": target_filepath,
            "contexts": contexts if contexts else None,
            "view": None,
        }

        _drill_in_progress = True
        try:
            _register_recent_file(target_filepath)
            bpy.ops.wm.open_mainfile(filepath=target_filepath)
        finally:
            _drill_in_progress = False

        self.report({'INFO'}, f"Jumped to {Path(target_filepath).stem}")
        return {'FINISHED'}


# ---------------------------------------------------------------------------
# Context Management Operators
# ---------------------------------------------------------------------------

class MAPLINK_OT_ToggleContext(Operator):
    bl_idname = "maplink.toggle_context"
    bl_label = "Hide Context"
    bl_description = "Toggle context geometry visibility"
    bl_options = {'INTERNAL'}

    @classmethod
    def poll(cls, context):
        return has_context()

    def execute(self, context):
        toggled = False
        for obj in bpy.data.objects:
            if obj.name.startswith(_CONTEXT_ANCHOR_PREFIX):
                obj.hide_viewport = not obj.hide_viewport
                toggled = True

        if toggled:
            label = "hidden" if context_hidden() else "visible"
            self.report({'INFO'}, f"Context {label}")
        return {'FINISHED'}


# ---------------------------------------------------------------------------
# 3D Viewport right-click menu
# ---------------------------------------------------------------------------

def _viewport_context_menu_draw(self, context):
    obj = context.active_object
    if (obj and obj.instance_type == 'COLLECTION'
            and obj.instance_collection
            and obj.instance_collection.library):
        self.layout.separator()
        self.layout.operator("maplink.drill_down", icon='IMPORT')


# ---------------------------------------------------------------------------
# Registration
# ---------------------------------------------------------------------------

_classes = (
    MAPLINK_PG_NavProps,
    MAPLINK_OT_DrillDown,
    MAPLINK_OT_DrillUp,
    MAPLINK_OT_DrillToAncestor,
    MAPLINK_OT_ToggleContext,
)


def register():
    for cls in _classes:
        bpy.utils.register_class(cls)

    bpy.types.Scene.maplink_nav = bpy.props.PointerProperty(type=MAPLINK_PG_NavProps)

    bpy.app.handlers.save_pre.append(_on_save_pre)
    bpy.app.handlers.save_post.append(_on_save_post)
    bpy.app.handlers.load_post.append(_on_load_post)

    if hasattr(bpy.app.handlers, 'quit'):
        bpy.app.handlers.quit.append(_on_quit)

    # Viewport context entry now lives in context_menu.MAPLINK_MT_object_context
    # so Drill Down and Break Linked sit together instead of as two loose
    # entries in different parts of the stock menu.

    _cleanup_stale_context_files()


def unregister():
    # Legacy attach point — cleared in case an older build left one behind.
    ui_menus.remove_all(bpy.types.VIEW3D_MT_object_context_menu,
                        "_viewport_context_menu_draw")

    if hasattr(bpy.app.handlers, 'quit'):
        if _on_quit in bpy.app.handlers.quit:
            bpy.app.handlers.quit.remove(_on_quit)

    if _on_load_post in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.remove(_on_load_post)
    if _on_save_post in bpy.app.handlers.save_post:
        bpy.app.handlers.save_post.remove(_on_save_post)
    if _on_save_pre in bpy.app.handlers.save_pre:
        bpy.app.handlers.save_pre.remove(_on_save_pre)

    del bpy.types.Scene.maplink_nav

    for cls in reversed(_classes):
        bpy.utils.unregister_class(cls)