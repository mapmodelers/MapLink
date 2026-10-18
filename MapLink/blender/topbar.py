"""MapLink menu in the top bar, next to File / Edit / Render.

Everything that is not touched while modelling lives here instead of in the
viewport sidebar: project settings, Perforce, batch operations, diagnostics.
The sidebar keeps Navigation, Tools and Modeling.

Menus cannot host arbitrary panel layouts, but layout.prop works inside one, so
the settings that were rows in a panel are rows here.  Anything that needs a
real panel -- the batch file table, for instance -- stays an operator that
opens its own dialog, which is what it already was.

Bare-importable, no package-relative imports at module level, so a headless
worker importing this by accident cannot fail on it.
"""

import bpy


def _p4(layout, context):
    from . import p4_integration

    if not p4_integration.p4_available():
        # v1.21: no Perforce is the normal case for most users. A red error
        # icon told them something was broken when nothing was.
        layout.label(text="Perforce not installed", icon='INFO')
        return

    # No Target toggle. The selection already answers it: nothing selected acts
    # on the open file, a selection acts on those assets. The header shows
    # which, so the menu is never ambiguous about what it is about to touch.
    targets = p4_integration.resolve_targets(context)
    if not targets:
        layout.label(text="Save the file first", icon='INFO')
        return
    if len(targets) == 1:
        import os
        layout.label(text=os.path.basename(targets[0]), icon='FILE_BLEND')
    else:
        layout.label(text=f"{len(targets)} selected asset(s)", icon='FILE_BLEND')

    layout.separator()
    layout.operator("maplink.p4_show_in_explorer", icon='FILE_FOLDER')
    layout.operator("maplink.p4_checkout", icon='IMPORT')
    layout.operator("maplink.p4_revert", icon='LOOP_BACK')
    layout.separator()
    layout.operator("maplink.save_with_p4_prompt", icon='FILE_TICK')


class MAPLINK_MT_perforce(bpy.types.Menu):
    bl_label = "Perforce"
    bl_idname = "MAPLINK_MT_perforce"

    def draw(self, context):
        _p4(self.layout, context)


class MAPLINK_MT_batch(bpy.types.Menu):
    bl_label = "Batch"
    bl_idname = "MAPLINK_MT_batch"

    def draw(self, context):
        layout = self.layout
        # Root is not here: it says which project you are working on, not how
        # a batch runs. It lives under Project.
        layout.operator("maplink.batch_standardize",
                        text="Batch Standardize…", icon='CHECKMARK')
        layout.operator("maplink.batch_sidecar",
                        text="Generate All Sidecars", icon='FILE_REFRESH')
        layout.operator("maplink.rebuild_all_thumbnails",
                        text="Rebuild All Thumbnails…", icon='IMAGE_DATA')
        layout.operator("maplink.repair_asset_catalogs",
                        text="Repair Asset Catalogs…", icon='ASSET_MANAGER')
        layout.operator("maplink.force_save_all",
                        text="Force-Save All (recursive)", icon='FILE_TICK')


class MAPLINK_MT_project(bpy.types.Menu):
    bl_label = "Project"
    bl_idname = "MAPLINK_MT_project"

    def draw(self, context):
        import os as _os
        from . import project_roots

        layout = self.layout

        # v1.19 — the same two doors the UE Manager offers, so "how do I start
        # a project" has one answer on both sides.
        layout.operator("maplink.new_project",
                        text="New MapLink Project…", icon='ADD')
        layout.operator("maplink.open_project",
                        text="Open Existing…", icon='FILE_FOLDER')

        recent = project_roots.recent_roots()
        if recent:
            sub = layout.column()
            sub.label(text="Recent")
            for r in recent[:5]:
                op = sub.operator(
                    "maplink.open_project",
                    text=_os.path.basename(_os.path.normpath(r)),
                    icon='FILE_FOLDER')
                op.root = r

        layout.separator()

        # Vendor-in. One operation, several doors: this one starts from "which
        # asset", the Asset Browser context menu starts from "this asset".
        # Deliberately no "Export Asset" beside it — Export means a bundle for
        # a machine that does not have the source, and that meaning belongs to
        # the material library.
        layout.operator("maplink.import_asset",
                        text="Import Asset…", icon='IMPORT')
        if not project_roots.shelf_root():
            layout.operator("maplink.ensure_shelf",
                            text="Create your Shelf…", icon='ASSET_MANAGER')

        layout.separator()

        # Which project you are working on. Every batch operation resolves its
        # scope from this, which is why it is here and not inside Batch.
        batch = getattr(context.scene, "maplink_batch", None)
        if batch is None:
            layout.label(text="Project properties not loaded", icon='ERROR')
            return
        layout.prop(batch, "root_path", text="Project Root")
        layout.separator()
        layout.operator("maplink.copy_diagnostics",
                        text="Copy Diagnostics", icon='CONSOLE')


class MAPLINK_MT_topbar(bpy.types.Menu):
    bl_label = "MapLink"
    bl_idname = "MAPLINK_MT_topbar"

    def draw(self, context):
        layout = self.layout

        # Deliberately not here: drill, Quick Create and Display. Drill and
        # Quick Create are done with a hand on the model, so they belong to the
        # viewport (sidebar, context menu, keymap) and a menu trip is slower
        # than what already exists. Display is a viewport setting and reads
        # wrong anywhere else.
        layout.menu(MAPLINK_MT_project.bl_idname, icon='FILE_FOLDER')
        layout.menu(MAPLINK_MT_perforce.bl_idname, icon='IMPORT')
        layout.menu(MAPLINK_MT_batch.bl_idname, icon='FILE_REFRESH')
        layout.separator()

        layout.operator("maplink.check_standardization",
                        text="Check Standardization", icon='CHECKMARK')
        layout.operator("maplink.reload_blend", icon='FILE_REFRESH')
        layout.separator()

        # Straight to the addon preferences rather than a duplicate of them.
        # One place owns each setting.
        op = layout.operator("preferences.addon_show",
                             text="MapLink Preferences…", icon='PREFERENCES')
        op.module = __package__


def _topbar_draw(self, context):
    from . import icons
    self.layout.menu(MAPLINK_MT_topbar.bl_idname, **icons.brand())


_classes = (
    MAPLINK_MT_project,
    MAPLINK_MT_perforce,
    MAPLINK_MT_batch,
    MAPLINK_MT_topbar,
)


def register():
    from . import ui_menus

    for cls in _classes:
        bpy.utils.register_class(cls)
    ui_menus.append_once(bpy.types.TOPBAR_MT_editor_menus, _topbar_draw)


def unregister():
    from . import ui_menus

    ui_menus.detach(bpy.types.TOPBAR_MT_editor_menus, _topbar_draw)
    for cls in reversed(_classes):
        try:
            bpy.utils.unregister_class(cls)
        except Exception:
            pass
