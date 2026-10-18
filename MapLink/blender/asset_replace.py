"""MapLink_Blender — Replace linked collection instances.

Right-click an asset in the Asset Browser → "Replace Selected Instances".
Swaps instance_collection on all selected collection-instance objects to the
picked asset's collection, preserving each empty's world transform.

Works for assets in EXTERNAL libraries (the normal MapLink case): if the asset
isn't local to the current file, its collection is linked in from the asset's
source .blend.  The original version only handled local_id, so right-clicking a
library asset resolved to None and the replace silently failed.
"""

import os

import bpy
from bpy.types import Operator

from . import icons
from . import ui_menus


class MAPLINK_OT_ReplaceInstances(Operator):
    bl_idname = "maplink.replace_instances"
    bl_label = "Replace Selected Instances"
    bl_description = "Replace selected collection instances with this asset"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        # Lightweight only — poll runs on every redraw, so it must NOT link
        # anything.  Need at least one selected collection instance...
        if not any(
            obj.select_get()
            and obj.instance_type == 'COLLECTION'
            and obj.instance_collection
            for obj in context.selected_objects
        ):
            return False
        # ...and an active Asset-Browser asset that is a collection.
        asset = getattr(context, "asset", None)
        return asset is not None and getattr(asset, "id_type", None) == 'COLLECTION'

    def execute(self, context):
        target = _resolve_asset_collection(context)
        if target is None:
            self.report({'ERROR'},
                        "Could not resolve the selected asset to a collection "
                        "(see console).")
            return {'CANCELLED'}

        count = 0
        skipped_same = 0
        for obj in context.selected_objects:
            if (obj.instance_type == 'COLLECTION'
                    and obj.instance_collection):
                if obj.instance_collection == target:
                    skipped_same += 1
                    continue
                # Reassigning instance_collection leaves the empty's transform
                # untouched, so the world placement is preserved.
                obj.instance_collection = target
                count += 1

        if count == 0:
            msg = ("All selected instances are already this asset"
                   if skipped_same else "No collection instances selected")
            self.report({'WARNING'}, msg + ".")
            return {'CANCELLED'}

        self.report({'INFO'},
                    f"Replaced {count} instance(s) with {target.name}.")
        return {'FINISHED'}


def _resolve_asset_collection(context):
    """Resolve the active Asset-Browser asset to a Collection datablock,
    linking it from its library .blend if it isn't local to this file."""
    asset = getattr(context, "asset", None)
    if asset is None:
        return None
    if getattr(asset, "id_type", None) != 'COLLECTION':
        return None

    # Local asset (the data-block lives in this file): use it directly.
    local = getattr(asset, "local_id", None)
    if local and isinstance(local, bpy.types.Collection):
        return local

    # Library asset: resolve via the source .blend.
    try:
        lib_path = bpy.path.abspath(asset.full_library_path)
        coll_name = asset.name
    except AttributeError:
        return None
    if not lib_path or not coll_name:
        return None

    def _find_linked():
        for c in bpy.data.collections:
            if (c.name == coll_name and c.library is not None
                    and bpy.path.abspath(c.library.filepath) == lib_path):
                return c
        return None

    # Reuse an existing link to the same collection from the same library.
    existing = _find_linked()
    if existing is not None:
        return existing

    # Otherwise link the collection in from the library .blend.
    try:
        with bpy.data.libraries.load(lib_path, link=True) as (df, dt):
            if coll_name in df.collections:
                dt.collections = [coll_name]
        linked = next((c for c in dt.collections if c is not None), None)
    except Exception as e:
        print(f"[MapLink] Replace: could not link '{coll_name}' "
              f"from {lib_path}: {e}")
        return None

    if linked is None:
        print(f"[MapLink] Replace: collection '{coll_name}' not found in "
              f"{lib_path}")
    return linked or _find_linked()


def _linked_collection_items(self, context):
    """Every linked collection currently reachable, as enum items.

    Built from bpy.data rather than the asset libraries on disk: this runs from
    the viewport with no Asset Browser open, and anything already linked into
    the file is both the common case and the cheap one. Linking a library in
    just to populate a dropdown would stall the popup.
    """
    global _ENUM_CACHE
    seen, items = set(), []
    for coll in sorted(bpy.data.collections, key=lambda c: c.name.lower()):
        if coll.library is None or coll.name in seen:
            continue
        seen.add(coll.name)
        lib = os.path.basename(bpy.path.abspath(coll.library.filepath))
        items.append((coll.name, coll.name, lib))
    if not items:
        items = [('', "No linked collections in this file", "")]
    # Blender does not keep a reference to strings returned from an items
    # callback, so they have to be held here or the enum shows garbage.
    _ENUM_CACHE = items
    return _ENUM_CACHE


_ENUM_CACHE = []


class MAPLINK_OT_ReplaceWith(Operator):
    bl_idname = "maplink.replace_with"
    bl_label = "Replace Selected With…"
    bl_description = ("Swap the selected collection instances for another "
                      "linked collection, picked from a list")
    bl_options = {'REGISTER', 'UNDO'}

    target: bpy.props.EnumProperty(
        name="Asset", items=_linked_collection_items,
        description="Linked collection to swap every selection onto",
    )

    @classmethod
    def poll(cls, context):
        return any(obj.instance_type == 'COLLECTION' and obj.instance_collection
                   for obj in context.selected_objects)

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self, width=380)

    def draw(self, context):
        n = sum(1 for o in context.selected_objects
                if o.instance_type == 'COLLECTION' and o.instance_collection)
        col = self.layout.column()
        col.label(text=f"{n} instance(s) selected.", icon='MOD_INSTANCE')
        col.prop(self, "target", **icons.label_args(self.target, catalog=True))

    def execute(self, context):
        if not self.target:
            self.report({'ERROR'}, "No linked collection to swap to.")
            return {'CANCELLED'}
        target = bpy.data.collections.get(self.target)
        if target is None:
            self.report({'ERROR'}, f"'{self.target}' is no longer available.")
            return {'CANCELLED'}

        count = same = 0
        for obj in context.selected_objects:
            if not (obj.instance_type == 'COLLECTION' and obj.instance_collection):
                continue
            if obj.instance_collection == target:
                same += 1
                continue
            # Reassigning leaves the empty's transform alone, so world
            # placement survives the swap.
            obj.instance_collection = target
            count += 1

        if count == 0:
            self.report({'WARNING'},
                        "All selected instances are already this asset."
                        if same else "No collection instances selected.")
            return {'CANCELLED'}
        self.report({'INFO'}, f"Replaced {count} instance(s) with {target.name}.")
        return {'FINISHED'}


_classes = (
    MAPLINK_OT_ReplaceInstances,
    MAPLINK_OT_ReplaceWith,
)


def register():
    # The Asset Browser entry lives in context_menu.MAPLINK_MT_asset_context
    # so Replace and Run Thumbnail sit together under one MapLink submenu.
    for cls in _classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(_classes):
        bpy.utils.unregister_class(cls)
