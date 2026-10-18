"""MapLink project roots — scaffold, remember, and the Shelf.

MapLink has always been able to run without Unreal.  ``find_project_root()``
and ``_find_manifest()`` walk *up* from the open .blend to the nearest
``maplink_project.json``, whose paths are all relative; UE never tells Blender
where it is.  The one thing missing was that only the UE Manager had a button
that ran the scaffolder, so a Blender-only artist could use a MapLink project
but could not make one.  ``maplink.new_project`` closes that.

The Shelf
---------
The Shelf is an ordinary MapLink project — same tree, same manifest, no ``kind``
field, no new schema version.  Shelf-ness is the ``shelf_root`` *preference*,
not a property of the folder.  That is load-bearing: the moment the Shelf
becomes a second species in the data model, every keyed lookup, the Manager
rows and the P4 config have to learn about it.

It is created on first need, never at install time: install cannot know the
right drive, the scaffolder needs a (slow, failure-prone) Blender run, and
writing outside the plugin at install is the smell v1.13 removed.

Recent roots live in a small JSON under the Blender config directory rather
than in addon preferences, because prefs writes cost a ``save_userpref()`` and
this module would otherwise trigger one every time a project is opened.
"""

import json
import os
import subprocess
import sys

import bpy
from bpy.types import Operator


MANIFEST_NAME = "maplink_project.json"
SHELF_NAME = "Shelf"
_MAX_RECENT = 8


def _log(msg):
    print(f"[MapLink Roots] {msg}", flush=True)


def _prefs():
    try:
        addons = bpy.context.preferences.addons
        ad = addons.get(__package__) or addons.get(__name__.split('.')[0])
        return ad.preferences if ad is not None else None
    except Exception:                                   # noqa: BLE001
        return None


def addon_dir():
    return os.path.dirname(os.path.abspath(__file__))


def scaffolder_path():
    """<addon>/maplink_project_setup.py — moved here in v1.19 (Finding 3).

    It used to live at ``<plugin>/Scripts/``, which MapLink_UpdateAddon.cmd
    does not install into Blender, so a Blender-only user had the template
    .blend files and not the script that uses them.
    """
    return os.path.join(addon_dir(), "maplink_project_setup.py")


def resources_dir():
    return os.path.join(addon_dir(), "resources")


def addon_version():
    try:
        pkg = sys.modules.get(__package__)
        v = getattr(pkg, "bl_info", {}).get("version", (0, 0, 0))
        return ".".join(str(x) for x in v)
    except Exception:                                   # noqa: BLE001
        return "0.0.0"


def default_parent():
    """Where a new root is proposed: C:\\MapLink on Windows, ~/MapLink else."""
    if os.name == "nt":
        return os.path.join(os.environ.get("SystemDrive", "C:") + os.sep, "MapLink")
    return os.path.join(os.path.expanduser("~"), "MapLink")


# ---------------------------------------------------------------------------
# Recent roots
# ---------------------------------------------------------------------------

def _roots_json():
    try:
        cfg = bpy.utils.user_resource('CONFIG', path='maplink', create=True)
    except Exception:                                   # noqa: BLE001
        return ""
    return os.path.join(cfg, "roots.json") if cfg else ""


def recent_roots():
    """Most-recent-first list of roots that still exist on disk."""
    path = _roots_json()
    if not path or not os.path.isfile(path):
        return []
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        roots = [str(r) for r in data.get("roots", [])]
    except Exception as exc:                            # noqa: BLE001
        _log(f"could not read {path}: {exc}")
        return []
    return [r for r in roots if os.path.isfile(os.path.join(r, MANIFEST_NAME))]


def remember_root(root):
    """Push *root* to the front of the recent list.  Best-effort, never raises."""
    if not root:
        return
    root = os.path.abspath(root)
    path = _roots_json()
    if not path:
        return
    keep = [r for r in recent_roots()
            if os.path.normcase(r) != os.path.normcase(root)]
    keep.insert(0, root)
    try:
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"roots": keep[:_MAX_RECENT]}, f, indent=2)
    except Exception as exc:                            # noqa: BLE001
        _log(f"could not write {path}: {exc}")


# ---------------------------------------------------------------------------
# Scaffolding
# ---------------------------------------------------------------------------

def scaffold(root, setup_name, timeout=600):
    """Run the headless scaffolder.  -> (ok, message).

    Same launch shape quick_tools._run_create already uses: a throwaway .blend
    positional (the script drives its own open/save and ignores it),
    --factory-startup so no addon is loaded, CREATE_NO_WINDOW, a timeout.
    """
    script = scaffolder_path()
    res = resources_dir()
    if not os.path.isfile(script):
        return False, f"Scaffolder not found at {script}"
    throwaway = os.path.join(res, "LI_Template.blend")
    if not os.path.isfile(throwaway):
        return False, f"Bundled templates not found in {res}"

    try:
        os.makedirs(root, exist_ok=True)
        probe = os.path.join(root, ".maplink_write_test")
        with open(probe, "w", encoding="utf-8") as f:
            f.write("ok")
        os.remove(probe)
    except Exception as exc:                            # noqa: BLE001
        return False, (f"Cannot create or write {root} ({exc}). If this is "
                       f"under OneDrive/Documents, pick a local folder.")

    cmd = [bpy.app.binary_path, "--factory-startup", "--background",
           throwaway, "--python", script, "--",
           "--root", root,
           "--setup-name", setup_name,
           "--resources-dir", res,
           "--unit-scale", "1.0",
           "--maplink-version", addon_version(),
           "--mi-collection", "Library_Material_Instances",
           "--instance-content-path", "/Game/MapLink/MaterialInstances/"]

    cf = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=timeout, creationflags=cf)
    except subprocess.TimeoutExpired:
        return False, f"Scaffolding timed out after {timeout}s."
    except Exception as exc:                            # noqa: BLE001
        return False, f"Could not launch Blender: {exc}"

    for line in (r.stdout or "").splitlines():
        print(line)
    if r.returncode != 0:
        print(r.stderr or "")
        return False, f"Scaffolding failed (rc={r.returncode}); see console."

    if not os.path.isfile(os.path.join(root, MANIFEST_NAME)):
        return False, "Scaffolder finished but wrote no manifest; see console."

    remember_root(root)
    return True, root


def top_level_blend(root, setup_name):
    """<root>/LI/LI_<Setup>/LI_<Setup>.blend if it exists, else ''."""
    stem = f"LI_{setup_name}"
    cand = os.path.join(root, "LI", stem, f"{stem}.blend")
    return cand if os.path.isfile(cand) else ""


# ---------------------------------------------------------------------------
# The Shelf
# ---------------------------------------------------------------------------

def shelf_root():
    p = _prefs()
    root = (getattr(p, "shelf_root", "") or "").strip() if p else ""
    if not root:
        return ""
    root = bpy.path.abspath(root)
    return root if os.path.isfile(os.path.join(root, MANIFEST_NAME)) else ""


def default_shelf_path():
    return os.path.join(default_parent(), SHELF_NAME)


def set_shelf_root(root):
    p = _prefs()
    if p is None:
        return False
    try:
        p.shelf_root = root
        bpy.ops.wm.save_userpref()
        return True
    except Exception as exc:                            # noqa: BLE001
        _log(f"could not persist shelf_root: {exc}")
        return False


# ---------------------------------------------------------------------------
# Operators
# ---------------------------------------------------------------------------

class MAPLINK_OT_NewProject(Operator):
    """Scaffold a new MapLink project tree from the bundled templates"""
    bl_idname = "maplink.new_project"
    bl_label = "New MapLink Project…"
    bl_options = {'REGISTER'}

    project_name: bpy.props.StringProperty(
        name="Project Name",
        description="Folder name and project_name in the manifest",
        default="MyProject",
    )
    parent_dir: bpy.props.StringProperty(
        name="In",
        description="Where the project folder is created",
        subtype='DIR_PATH',
        default="",
    )
    open_after: bpy.props.BoolProperty(
        name="Open the top-level LI",
        description="Open <root>/LI/LI_<Name>/LI_<Name>.blend when done",
        default=True,
    )

    def invoke(self, context, event):
        if not self.parent_dir:
            self.parent_dir = default_parent()
        return context.window_manager.invoke_props_dialog(self, width=460)

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "project_name")
        layout.prop(self, "parent_dir")
        root = self._root()
        if root:
            layout.label(text=root, icon='FILE_FOLDER')
            if os.path.isfile(os.path.join(root, MANIFEST_NAME)):
                layout.label(text="A MapLink project already lives here.",
                             icon='ERROR')
        layout.prop(self, "open_after")

    def _root(self):
        name = (self.project_name or "").strip()
        parent = bpy.path.abspath((self.parent_dir or "").strip())
        if not name or not parent:
            return ""
        return os.path.join(parent, name)

    def execute(self, context):
        name = (self.project_name or "").strip()
        root = self._root()
        if not name or not root:
            self.report({'ERROR'}, "A project name and a parent folder are required.")
            return {'CANCELLED'}
        if os.path.isfile(os.path.join(root, MANIFEST_NAME)):
            self.report({'ERROR'}, f"A MapLink project already exists at {root}")
            return {'CANCELLED'}

        ok, msg = scaffold(root, name)
        if not ok:
            self.report({'ERROR'}, msg)
            return {'CANCELLED'}

        from . import maplink_asset_library
        try:
            maplink_asset_library.ensure_project_libraries(
                os.path.join(root, MANIFEST_NAME), save_prefs=True)
        except Exception as exc:                        # noqa: BLE001
            _log(f"library registration skipped: {exc}")

        top = top_level_blend(root, name)
        if self.open_after and top:
            bpy.ops.wm.open_mainfile(filepath=top)
            self.report({'INFO'}, f"Created {name} at {root}")
            return {'FINISHED'}

        self.report({'INFO'}, f"Created {name} at {root}")
        return {'FINISHED'}


class MAPLINK_OT_OpenProject(Operator):
    """Point MapLink at an existing project root (its maplink_project.json)"""
    bl_idname = "maplink.open_project"
    bl_label = "Open Existing…"
    bl_options = {'REGISTER'}

    directory: bpy.props.StringProperty(subtype='DIR_PATH')
    #: Set by the recent-roots buttons so no file dialog is needed.
    root: bpy.props.StringProperty(default="", options={'HIDDEN'})

    def invoke(self, context, event):
        if self.root:
            return self.execute(context)
        context.window_manager.fileselect_add(self)
        return {'RUNNING_MODAL'}

    def execute(self, context):
        root = bpy.path.abspath(self.root or self.directory or "")
        if not root:
            return {'CANCELLED'}
        root = os.path.normpath(root)
        manifest = os.path.join(root, MANIFEST_NAME)
        if not os.path.isfile(manifest):
            self.report({'ERROR'},
                        f"No {MANIFEST_NAME} in {root} — pick a project root.")
            return {'CANCELLED'}

        remember_root(root)
        from . import maplink_asset_library
        maplink_asset_library.ensure_project_libraries(manifest, save_prefs=True)

        name = os.path.basename(root)
        try:
            with open(manifest, "r", encoding="utf-8") as f:
                name = json.load(f).get("project_name") or name
        except Exception:                               # noqa: BLE001
            pass

        top = top_level_blend(root, name)
        if top:
            bpy.ops.wm.open_mainfile(filepath=top)
        self.report({'INFO'}, f"Opened {name}")
        return {'FINISHED'}


class MAPLINK_OT_EnsureShelf(Operator):
    """Create your Shelf — a personal MapLink project for assets that do not
    belong to a project yet"""
    bl_idname = "maplink.ensure_shelf"
    bl_label = "Create your Shelf"
    bl_options = {'REGISTER'}

    path: bpy.props.StringProperty(
        name="Shelf folder",
        subtype='DIR_PATH',
        default="",
    )

    def invoke(self, context, event):
        existing = shelf_root()
        if existing:
            self.report({'INFO'}, f"Shelf already at {existing}")
            return {'FINISHED'}
        if not self.path:
            self.path = default_shelf_path()
        # ONE confirm, showing the path and letting them change it. After this
        # the Shelf is never asked about again.
        return context.window_manager.invoke_props_dialog(self, width=460)

    def draw(self, context):
        layout = self.layout
        layout.label(text="Your Shelf is an ordinary MapLink project that is "
                          "always available,")
        layout.label(text="whatever file you have open. Assets created without "
                          "a project land here.")
        layout.separator()
        layout.prop(self, "path")

    def execute(self, context):
        root = os.path.normpath(bpy.path.abspath((self.path or "").strip()
                                                 or default_shelf_path()))
        if os.path.isfile(os.path.join(root, MANIFEST_NAME)):
            # Already a project on disk — adopt it rather than re-scaffolding.
            set_shelf_root(root)
            remember_root(root)
            self.report({'INFO'}, f"Shelf set to {root}")
            return {'FINISHED'}

        ok, msg = scaffold(root, SHELF_NAME)
        if not ok:
            self.report({'ERROR'}, msg)
            return {'CANCELLED'}

        set_shelf_root(root)
        from . import maplink_asset_library
        maplink_asset_library.ensure_shelf_libraries()
        self.report({'INFO'}, f"Created your Shelf at {root}")
        return {'FINISHED'}


def require_root(context, blend_dir=None):
    """The root an operator should write into, offering the Shelf if there is none.

    -> (root, setup_name) or (None, None) after invoking the Shelf confirm.
    Callers that get None should cancel; the user is looking at the one
    Shelf prompt and will re-run the operator.
    """
    from . import link_scope

    root = None
    if blend_dir:
        root = link_scope.find_project_root(blend_dir)
    if root is None:
        root = link_scope.current_project_root()
    if root:
        return root, os.path.basename(os.path.normpath(root))

    shelf = shelf_root()
    if shelf:
        return shelf, SHELF_NAME

    try:
        bpy.ops.maplink.ensure_shelf('INVOKE_DEFAULT')
    except Exception as exc:                            # noqa: BLE001
        _log(f"could not offer the Shelf: {exc}")
    return None, None


# ---------------------------------------------------------------------------
# Empty state — deliberately the same two doors as the UE Manager (v1.12 §A6)
# ---------------------------------------------------------------------------

def draw_empty_state(layout):
    """Draw the no-project state.  -> True when it drew (caller stops there)."""
    path = bpy.data.filepath
    from . import link_scope
    root = link_scope.find_project_root(os.path.dirname(path)) if path else None
    if root:
        return False

    box = layout.box()
    box.label(text="No MapLink project.", icon='INFO')
    row = box.row(align=True)
    row.operator("maplink.new_project", text="New MapLink Project…", icon='ADD')
    row.operator("maplink.open_project", text="Open Existing…",
                 icon='FILE_FOLDER')

    shelf = shelf_root()
    if shelf:
        box.label(text=f"Shelf: {shelf}", icon='ASSET_MANAGER')
    else:
        box.operator("maplink.ensure_shelf", text="Create your Shelf…",
                     icon='ASSET_MANAGER')

    recent = [r for r in recent_roots()
              if os.path.normcase(r) != os.path.normcase(shelf or "\0")]
    if recent:
        col = box.column(align=True)
        col.scale_y = 0.8
        col.label(text="Recent")
        for r in recent[:5]:
            op = col.operator("maplink.open_project",
                              text=os.path.basename(os.path.normpath(r)),
                              icon='FILE_FOLDER')
            op.root = r
    return True


# ---------------------------------------------------------------------------
# Registration
# ---------------------------------------------------------------------------

_classes = (
    MAPLINK_OT_NewProject,
    MAPLINK_OT_OpenProject,
    MAPLINK_OT_EnsureShelf,
)


def register():
    for cls in _classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(_classes):
        try:
            bpy.utils.unregister_class(cls)
        except Exception:                               # noqa: BLE001
            pass
