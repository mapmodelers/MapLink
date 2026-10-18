"""One MapLink submenu in the viewport object context menu.

Drill Down came from nav_drill and Break Linked came from quick_tools, so
right-clicking a linked asset showed two loose MapLink entries in different
parts of a long stock menu with nothing tying them together.  They are one
feature set and they belong in one place.

Everything MapLink adds to the object context menu goes through here.  Modules
keep owning their operators; this module owns only where they appear.
"""

import bpy

from . import icons
from . import ui_menus


def _linked_instance(context):
    """The active object's linked collection instance, or None."""
    obj = context.active_object
    if (obj and obj.instance_type == 'COLLECTION'
            and obj.instance_collection
            and obj.instance_collection.library):
        return obj
    return None


def _selected_instances(context):
    return [o for o in context.selected_objects
            if o.instance_type == 'COLLECTION' and o.instance_collection]


class MAPLINK_MT_object_context(bpy.types.Menu):
    bl_label = "MapLink"
    bl_idname = "MAPLINK_MT_object_context"

    def draw(self, context):
        layout = self.layout

        linked = _linked_instance(context)
        if linked is not None:
            name = linked.instance_collection.name
            # The entry carries the type of what you are standing on, so the
            # menu says which kind of asset you are about to open.
            layout.operator("maplink.drill_down",
                            **icons.label_args(name, default='IMPORT',
                                               catalog=True))
            layout.separator()

        # Creating an asset is the thing you do with a selection of meshes, so
        # it comes first for anything that is not already a linked instance.
        layout.operator("maplink.quick_create_asset",
                        text="Create SM/LI Asset from Selection", icon='ADD')
        layout.operator("maplink.create_preset",
                        text="Create Preset from Selection", icon='PRESET')

        if _selected_instances(context):
            layout.separator()
            layout.operator("maplink.replace_with", icon='MOD_INSTANCE')
            layout.operator("maplink.break_linked_to_local",
                            text="Break Linked → Local", icon='UNLINKED')


class MAPLINK_MT_asset_context(bpy.types.Menu):
    """MapLink's entries in the Asset Browser context menu.

    Same reasoning as the viewport submenu: Replace came from asset_replace and
    Run Thumbnail from quick_tools, so right-clicking an asset showed two loose
    MapLink entries in unrelated parts of a long stock menu.
    """
    bl_label = "MapLink"
    bl_idname = "MAPLINK_MT_asset_context"

    def draw(self, context):
        layout = self.layout

        # Carry the type of the asset being swapped *to*. Picking the wrong
        # type is the mistake this feature exists to prevent, and the tile
        # alone does not say whether a name is SM_ or LI_ without reading it.
        # Name only -- resolving the datablock would link it, and this runs on
        # every menu draw.
        asset = getattr(context, "asset", None)
        name = getattr(asset, "name", "") or ""
        layout.operator("maplink.replace_instances",
                        **icons.label_args(name, default='MOD_INSTANCE',
                                           catalog=True))
        layout.separator()
        # v1.19 -- vendor-in, from the side that starts with "this asset".
        # Same operator as MapLink | Project | Import Asset...; that one starts
        # with "which asset". Two doors, one operation, and no "Export Asset"
        # anywhere: Export means a bundle for a machine without the source.
        # poll() hides it unless the asset really is outside this project.
        layout.operator("maplink.copy_to_project",
                        text="Copy to Project...", icon='IMPORT')
        layout.separator()
        layout.operator("maplink.run_thumbnail", icon='RENDER_STILL')


def _draw(self, context):
    """Attach the submenu, but only when it would have something to offer.

    Any selection at all qualifies now: the common case is right-clicking a
    mesh to make an asset out of it, which is the whole point of Quick Create.
    Only an empty selection has nothing on offer.
    """
    if context.mode != 'OBJECT' or not context.selected_objects:
        return
    self.layout.separator()
    self.layout.menu(MAPLINK_MT_object_context.bl_idname, **icons.brand())


def _draw_asset(self, context):
    if getattr(context, "asset", None) is None:
        return
    self.layout.separator()
    self.layout.menu(MAPLINK_MT_asset_context.bl_idname, **icons.brand())


_classes = (
    MAPLINK_MT_object_context,
    MAPLINK_MT_asset_context,
)


def register():
    for cls in _classes:
        bpy.utils.register_class(cls)
    ui_menus.append_once(bpy.types.VIEW3D_MT_object_context_menu, _draw)

    # The Asset Browser menu type name varies by build, so a mismatch must not
    # hard-fail registration -- the submenu simply does not appear.
    ab = getattr(bpy.types, "ASSETBROWSER_MT_context_menu", None)
    if ab is not None:
        ui_menus.append_once(ab, _draw_asset)
        # Clear the per-module entries this submenu replaces, in case an older
        # build left them attached.
        ui_menus.remove_all(ab, "_asset_browser_menu_draw")
        ui_menus.remove_all(ab, "_assetbrowser_menu")
    else:
        print("[MapLink] ASSETBROWSER_MT_context_menu not found; "
              "the Asset Browser MapLink submenu is disabled on this build.")


def unregister():
    ui_menus.detach(bpy.types.VIEW3D_MT_object_context_menu, _draw)
    ab = getattr(bpy.types, "ASSETBROWSER_MT_context_menu", None)
    ui_menus.detach(ab, _draw_asset)
    for cls in reversed(_classes):
        try:
            bpy.utils.unregister_class(cls)
        except Exception:
            pass
