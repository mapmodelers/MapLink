# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 MapModelers. See LICENSE-GPL-3.0.txt.
"""MapLink_Blender — Pipeline tools for the MapLink UE5 workflow.

Blender 5.0 addon. Self-contained, no UE5 dependency at runtime.

v1.9.0 — Shelf, Blender-first authoring, cross-root link correctness.
         • link_scope.py — Conform / Portable / Foreign / Broken for every
           library link.  The SIDECAR is the gate: a file with a Foreign or
           Broken link produces no analysis.json, so UE cannot import a path
           that resolves only on the machine that made it.  Portable → Conform
           is rewritten silently on every save, without consulting Blender's
           per-user "Relative Paths" preference.
         • The Asset Browser shows exactly this project plus your Shelf.  Every
           other MapLink_* entry is deregistered on file open (opt out with the
           new "Show other MapLink projects" preference).
         • maplink_project_setup.py moved into the addon package, so a
           Blender-only artist can scaffold a project: MapLink ▸ Project ▸
           New MapLink Project…, plus an empty state in the sidebar mirroring
           the UE Manager's two doors, and a recent-roots list.
         • The Shelf — an ordinary MapLink project that is always available.
           Created on first need, one confirm, never asked about again.
         • Vendor-in (maplink.vendor_asset): copy an asset and its whole link
           closure into this project, with a dry-run preview.  Reachable from
           Import Asset…, Copy to Project… and the save prompt.

v1.8.0 — Asset type icons.  New icons module: a preview collection resolving a
         .blend stem's prefix to a type icon (LI_ amber, BP_ azure, SM_ teal),
         drawn in the drill breadcrumb, the Asset Browser replace entry and
         the Batch Standardize file table.  Every accessor returns a stock
         Blender icon alongside, so a missing PNG gives a plainer row and
         never a blank one; headless skips the load entirely.
         The collection colour code now follows the same type code (LI_ moves
         to orange, BP_ gains a col_bp preference) and the file's own asset
         collection is tagged by the type its name declares.  object.color is
         untouched -- it already carries per-collection information a flat
         per-type accent would erase.

v1.7.0 — MapLink v1.14 pairing (standardization).
         • "Conform" → "Standardize" for vertex colors: the operator is now
           maplink.standardize_vc ("Standardize VC" under Tools); the old id
           maplink.conform_vertex_colors survives as a deprecated alias so
           saved keymaps keep resolving.
         • New "Standardize UV" (maplink.standardize_uv, under Tools):
           renames each mesh's UV layers to the canonical uv0/uv1/uv2 scheme
           in index order (extras → uv3, uv4, …).  Idempotent, zero-UV meshes
           skipped, active/render selections preserved, material UV Map
           references retargeted.
         • Batch Standardize (Settings ▸ Batch): per-run toggles
           (VC / UVs / Clean Outliner / Clean Collections / Force re-save),
           a per-file selection table over every project .blend, headless
           one-subprocess-per-file execution with a watchdog timeout, and a
           single shared P4 CL ("[MapLink] Batch Standardize").  Force
           re-save folds in the old force-resave behaviour: saving is what
           regenerates the sidecars.  Replaces the former Batch Clean
           Outliner / Batch Conform VC / Batch Clean Collections buttons
           (and batch_modeling.py).
         • Batch Root auto-default: on file load, a blank Settings ▸ Batch
           Root is populated with the MapLink project root (nearest
           maplink_project.json above the current .blend).

v1.5.0 — MapLink v1.13 pairing.
         • Artifact relocation: new "Artifact root" preference (fed by UE at
           addon install → <UEProject>/Saved/MapLink); export-on-save,
           force-save and batch workers write Logs/ + Export/ there.  Legacy
           plugin-dir layout remains a read fallback for one release.
         • UE status badge: new ue_status module — a "UE Link" sidebar panel
           showing per-file import status written by UE
           (<stem>_uestatus.json): importing… / ✓ up to date / ✗ failed.
           Shows a subdued "UE: no link" for standalone artists.
         • Copy Diagnostics: new diagnostics module — one click copies a
           markdown support report (addon/Blender versions, file, manifest,
           P4 availability, prefs, last export result, UE status).
         • P4 changelist dedup: description matching now normalizes per-line
           whitespace exactly like the UE plugin, verifies cached CLs are
           still pending with an unchanged description, and NEVER creates a
           duplicate CL when a match exists (contract: same description ⇒
           same CL on both sides).

v1.4.6 — Modeling additions.  New modeling_tools module:
         • Conform Vertex Colors — normalise every visible local mesh to the
           single "Color" channel standard (BYTE_COLOR / Face Corner, active +
           render, black default), with a confirm dialog showing counts.
         • Vertex-Color toolset (gated on a conform mesh; touches only Color):
           from-Cavity (native Dirty Vertex Colors masked into a channel),
           Contrast, Set Selected, Set Outer/Inner (open-boundary verts),
           and from-Base-Color (sample the active material's base-color image
           through the active UV map into RGB).
         • Patch Unwrap — Reset → Follow Active Quads → texel-density
           normalise → per-axis de-stretch over the selected quad faces.
         New panels: Modeling (Conform box now hosts Outliner Cleanup beside
         Conform VC, with Patch Unwrap below) and Vertex Color.  The old
         standalone Outliner Cleanup panel is relocated into the Conform box.

v1.4.5 — Pack C (Blender Addon UX).  New quick_tools module:
         • Reload .blend (instant revert).
         • Quick Create SM_/LI_ asset from selection — headless worker builds
           the asset and writes sidecars + P4-adds via batch_sidecar's helpers,
           so created assets are byte-for-byte conventional.  A linked instance
           is dropped at the first selected object.
         • Run Thumbnail (Asset Browser) — windowed collection-preview regen.
         • Break Linked → Local — localise a linked collection instance to root.
         Also: drilled-open files (drill down/up, jump-to-ancestor) now register
         into File > Open Recent.

v1.4.1 — Added Force-Save All: walks current .blend's referenced .blends
         recursively (via existing sidecars) and runs each through the full
         save pipeline (P4 checkout → save → sidecars → FBX export → P4 add)
         in parallel headless Blender subprocesses.  Use this to pre-populate
         the UE plugin's cache for a whole asset tree at once.

v1.4.0 — Added fbx_export_on_save: spawns a detached Blender subprocess on
         every save of a BP_/LI_/SM_ .blend to pre-produce the FBX + analysis
         + filehash artifacts the UE plugin needs.  Eliminates UE-side
         Blender launches on import for any file already saved at least once.
         Requires the UE plugin path (Preferences > Add-ons > MapLink, or the
         MAPLINK_PLUGIN_DIR environment variable).
"""

bl_info = {
    "name": "MapLink Blender",
    "author": "MapModelers",
    "version": (1, 11, 0),
    "blender": (5, 0, 0),
    "location": "3D Viewport > Sidebar > MapLink",
    "description": "Pipeline tools for MapLink UE5 workflow: hierarchy drill, P4, asset replace, FBX export-on-save, standardize (VC/UV), batch standardize",
    "category": "Pipeline",
}

import os
import subprocess
import bpy
from bpy.types import Panel, Operator, PropertyGroup, AddonPreferences
from bpy.props import StringProperty, EnumProperty, BoolProperty
from pathlib import Path

from . import icons
from . import ui_menus
from . import panels
from . import registry
from . import topbar
from . import context_menu
from . import theme_colors
from . import thumbnail_badge
from . import nav_drill
from . import p4_integration
from . import asset_replace
from . import outliner_cleanup
from . import link_scope
from . import asset_catalog
from . import catalog_repair
from . import project_roots
from . import vendor_in
from . import sidecar_analysis
from . import fbx_export_on_save
from . import force_save_all
from . import quick_tools
from . import modeling_tools
from . import batch_standardize
from . import maplink_asset_library
from . import ue_status
from . import diagnostics

# Reinstalling/reloading the add-on over an already-imported copy re-runs this
# __init__, but the `from . import X` lines above only rebind the *cached*
# submodules — they do NOT re-execute their updated .py files.  That is why an
# edit to e.g. quick_tools.py can appear not to take effect after a reinstall
# until Blender is fully restarted.  Force a reload of each submodule so updates
# apply on reinstall without a restart.
if "_MAPLINK_INITIALISED" in locals():
    import importlib
    for _mod in (icons, ui_menus, panels, registry, topbar, context_menu, theme_colors, thumbnail_badge, nav_drill, p4_integration, asset_replace,
                 outliner_cleanup, link_scope, asset_catalog, catalog_repair,
                 project_roots, vendor_in,
                 sidecar_analysis, fbx_export_on_save,
                 force_save_all, quick_tools, modeling_tools,
                 batch_standardize, maplink_asset_library, ue_status,
                 diagnostics):
        importlib.reload(_mod)
_MAPLINK_INITIALISED = True


# ---------------------------------------------------------------------------
# Batch Sidecar — property group + operator
# ---------------------------------------------------------------------------

class MAPLINK_PG_BatchSidecar(PropertyGroup):
    root_path: StringProperty(
        name="Root Path",
        description="Root directory to scan for .blend files (recursive)",
        subtype='DIR_PATH',
        default="",
    )


# ---------------------------------------------------------------------------
# Addon Preferences (v1.4.0)
# ---------------------------------------------------------------------------

_TAG_ITEMS = [
    ('NONE', "None", "No colour tag (objects fall back to black)"),
    ('COLOR_01', "Color 01 (red)", ""),
    ('COLOR_02', "Color 02 (orange)", ""),
    ('COLOR_03', "Color 03 (yellow)", ""),
    ('COLOR_04', "Color 04 (green)", ""),
    ('COLOR_05', "Color 05 (blue)", ""),
    ('COLOR_06', "Color 06 (violet)", ""),
    ('COLOR_07', "Color 07 (pink)", ""),
    ('COLOR_08', "Color 08 (brown)", ""),
]


class MAPLINK_AP_Preferences(AddonPreferences):
    """v1.4.0 — UE plugin path for FBX-export-on-save subprocess.

    The path resolution order is:
        1. This preference (if set)
        2. MAPLINK_PLUGIN_DIR environment variable
        3. Skip (with a one-time warning)
    """
    bl_idname = __package__

    maplink_plugin_dir: StringProperty(
        name="UE plugin dir",
        description=(
            "Path to the MapLink UE plugin "
            "(contains Scripts/maplink_export.py).  Leave empty to use the "
            "MAPLINK_PLUGIN_DIR environment variable."
        ),
        subtype='DIR_PATH',
        default="",
    )

    # v1.13 (addon 1.5.0) — where generated artifacts (Logs/, Export/) are
    # written.  UE feeds this at addon install: <UEProject>/Saved/MapLink.
    # Empty → MAPLINK_ARTIFACT_ROOT env var → legacy plugin-dir layout
    # (one release of grace; the plugin dir may be read-only).
    maplink_artifact_root: StringProperty(
        name="Artifact root",
        description=(
            "Where export artifacts (Logs/, Export/) are written.  Set "
            "automatically by UE's Install/Update Blender Addon "
            "(<UEProject>/Saved/MapLink).  Leave empty to use the "
            "MAPLINK_ARTIFACT_ROOT environment variable, else the legacy "
            "in-plugin layout"
        ),
        subtype='DIR_PATH',
        default="",
    )

    # v1.19 — the Shelf.  An ordinary MapLink project that happens to be the
    # one you always have; Shelf-ness is THIS PREFERENCE and nothing else, so
    # nothing in the data model has to learn about a second species of project.
    # Empty until the Shelf is created on first need (maplink.ensure_shelf).
    shelf_root: StringProperty(
        name="Shelf",
        description=(
            "Your personal MapLink project — always available in the Asset "
            "Browser, whatever file is open.  Assets created with no project "
            "around land here.  Created on first need; a team that wants a "
            "shared kit opens it as an ordinary project root instead"
        ),
        subtype='DIR_PATH',
        default="",
    )

    show_other_maplink_projects: BoolProperty(
        name="Show other MapLink projects",
        description=(
            "Keep every MapLink asset library registered, including projects "
            "you are not working in.  Off by default: eight near-identically "
            "named libraries in one Asset Browser is how an asset gets dragged "
            "out of the wrong root, and that link does not resolve for anyone "
            "else"
        ),
        default=False,
    )

    # v1.20 — the escape hatch for the save-prompt scoping.  MapLink's Ctrl+S
    # override now steps aside entirely outside a MapLink project; this brings
    # back the Perforce half of it, and only that half.  Auto-Standardize and
    # the foreign-link prompt stay off out there whatever this is set to —
    # they are about a project, not about a depot.
    p4_outside_projects: BoolProperty(
        name="Offer Perforce checkout outside MapLink projects",
        description=(
            "Prompt to check out any .blend that lives in your Perforce "
            "workspace, not just the ones inside a MapLink project. Off by "
            "default: MapLink has no business interrupting a save it knows "
            "nothing about. Files outside the workspace are never touched "
            "either way"
        ),
        default=False,
    )

    # v1.14 — gate the confirmation shown before any standardization that
    # may alter data (Standardize VC deletes/renames color attributes,
    # Standardize UV renames layers + retargets material references).
    auto_standardize: BoolProperty(
        name="Auto-Standardize on save",
        description=(
            "Standardize vertex colours and UV layer names on every Ctrl+S, "
            "so a file can never be saved unconventional. Off by default: it "
            "rewrites data, and that should be a choice rather than a surprise"
        ),
        default=False,
    )

    auto_color_objects: BoolProperty(
        name="Auto-colour objects on save",
        description=(
            "Re-apply Color Objects by Collection on every Ctrl+S, so the "
            "viewport colour code stays current as assets are added and moved"
        ),
        default=False,
    )

    confirm_destructive: BoolProperty(
        name="Warn before destructive standardization",
        description=(
            "Show a confirmation (pursue or cancel) before any "
            "standardization that may alter data — Standardize VC, "
            "Standardize UV.  Untick to run them immediately"
        ),
        default=True,
    )

    # --- Collection colour code (used by Color Objects by Collection and by
    #     Collection Cleanup, in-app and in batch).  Names must match
    #     outliner_cleanup._prefs_color_map (col_sm / col_li / col_bp /
    #     col_other / col_noexport).
    #
    #     Each swatch applies to both carriers of that type: the LINK_SM /
    #     LINK_LI structural collections, and the file's own asset collection
    #     when its .blend name declares that type.  LINK itself falls under
    #     'other'; NoExport keeps its own swatch and its objects are painted
    #     black.
    #
    #     The defaults follow the same colour code as the type icons.  Blender
    #     has eight fixed theme swatches and no cyan, so SM_ compromises to
    #     green -- the icon carries the exact accent, the tag is a grouping
    #     cue.  These stay editable: this is the Outliner the artist works in
    #     all day, and the icon is the authoritative type carrier regardless.
    #     ------------------------------------------------------------------
    sidebar_space: EnumProperty(
        name="Sidebar location",
        description=(
            "Where the MapLink tab appears. Text Editor gives you the full "
            "set — including Settings and Perforce — in an area you can park "
            "on a second monitor, instead of competing with the model for "
            "viewport width"
        ),
        items=[
            ('VIEW_3D', "3D Viewport",
             "Navigation, Tools and Modeling in the viewport sidebar"),
            ('TEXT_EDITOR', "Text Editor",
             "Everything, in a Text Editor sidebar"),
            ('BOTH', "Both",
             "Lean set in the viewport, full set in the Text Editor"),
        ],
        default='VIEW_3D',
        update=lambda self, ctx: panels.refresh(self, ctx),
    )

    theme_match_maplink: BoolProperty(
        name="Allow override of Blender's theme colours",
        description=(
            "Write the MapLink accents into Blender's collection colour theme, "
            "so the Outliner shows the exact type colours instead of the "
            "nearest stock swatch -- SM_ becomes real teal rather than green. "
            "This edits your global Blender preferences; unticking puts the "
            "original swatches back"
        ),
        default=False,
        update=lambda self, ctx: theme_colors.sync_from_prefs(self),
    )

    col_sm: EnumProperty(
        name="SM_", items=_TAG_ITEMS, default='COLOR_04')                # green
    col_li: EnumProperty(
        name="LI_", items=_TAG_ITEMS, default='COLOR_02')                # orange
    col_bp: EnumProperty(
        name="BP_", items=_TAG_ITEMS, default='COLOR_05')                # blue
    col_other: EnumProperty(
        name="Other collections", items=_TAG_ITEMS, default='NONE')
    col_noexport: EnumProperty(
        name="NoExport", items=_TAG_ITEMS, default='COLOR_08')           # brown

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "maplink_plugin_dir")
        env = os.environ.get("MAPLINK_PLUGIN_DIR", "")
        if env:
            layout.label(text=f"MAPLINK_PLUGIN_DIR = {env}", icon='INFO')
        else:
            layout.label(text="MAPLINK_PLUGIN_DIR not set", icon='INFO')
        layout.prop(self, "maplink_artifact_root")
        if not self.maplink_artifact_root:
            env_root = os.environ.get("MAPLINK_ARTIFACT_ROOT", "")
            if env_root:
                layout.label(text=f"MAPLINK_ARTIFACT_ROOT = {env_root}",
                             icon='INFO')
            else:
                layout.label(text="Artifact root unset — legacy plugin-dir "
                                  "layout in use (set by UE v1.13+ addon "
                                  "install)", icon='INFO')

        box = layout.box()
        box.label(text="Projects", icon='FILE_FOLDER')
        row = box.row(align=True)
        row.prop(self, "shelf_root")
        if not self.shelf_root:
            row.operator("maplink.ensure_shelf", text="", icon='ADD')
            box.label(text="No Shelf yet — it is created the first time "
                           "something needs a project.", icon='INFO')
        box.prop(self, "show_other_maplink_projects")
        box.prop(self, "p4_outside_projects")

        box = layout.box()
        box.label(text="Conformation", icon='CHECKMARK')
        box.prop(self, "confirm_destructive")
        box.prop(self, "auto_standardize")
        box.prop(self, "auto_color_objects")

        box = layout.box()
        header = box.row(align=True)
        header.label(text="Theme", icon='COLOR')
        # Defaults ARE the MapLink colour code, so there is no separate switch
        # to turn it on -- Reset is how you get it back after changing one.
        header.operator("maplink.reset_theme_defaults",
                        text="Reset to Defaults", icon='LOOP_BACK')
        box.prop(self, "theme_match_maplink")

        # One colour per type, edited directly on the theme swatch that type
        # uses. The slot dropdown and a separate swatch grid said the same
        # thing twice, and neither showed you the colour you were choosing.
        #
        # Greyed out when the override is off, because these edit Blender's
        # own theme config -- offering them while MapLink is forbidden to write
        # it would be a control that does nothing.
        swatches = None
        try:
            swatches = context.preferences.themes[0].collection_color
        except (AttributeError, IndexError, TypeError):
            pass

        if swatches is None:
            box.label(text="Collection colour theme unavailable on this build",
                      icon='INFO')
        else:
            sub = box.column(align=True)
            sub.enabled = self.theme_match_maplink
            for label, slot in theme_colors.TYPE_SLOTS:
                idx = slot - 1
                if 0 <= idx < len(swatches):
                    sub.prop(swatches[idx], "color", text=label)

        layout.separator()
        layout.prop(self, "sidebar_space")
        box.label(text="NoExport objects are also painted black in the "
                       "viewport by Color Objects by Collection.", icon='INFO')


class MAPLINK_OT_BatchSidecar(Operator):
    bl_idname = "maplink.batch_sidecar"
    bl_label = "Generate All Sidecars"
    bl_description = ("Recursively scan root path for BP_/LI_/SM_ .blend files "
                      "and generate sidecar analysis.json + filehash.txt for each")

    @classmethod
    def poll(cls, context):
        try:
            props = context.scene.maplink_batch
        except (AttributeError, TypeError):
            return False
        return bool(props.root_path.strip())

    def execute(self, context):
        props = context.scene.maplink_batch
        root = bpy.path.abspath(props.root_path.strip())
        if not os.path.isdir(root):
            self.report({'ERROR'}, f"Not a directory: {root}")
            return {'CANCELLED'}

        script = os.path.join(os.path.dirname(__file__), "batch_sidecar.py")
        cmd = [
            bpy.app.binary_path,
            "--factory-startup",
            "--background",
            "--python", script,
            "--", root,
        ]

        creationflags = 0
        if hasattr(subprocess, 'CREATE_NO_WINDOW'):
            creationflags = subprocess.CREATE_NO_WINDOW

        try:
            r = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=3600,
                creationflags=creationflags,
            )
            # Print output to console
            if r.stdout:
                for line in r.stdout.splitlines():
                    print(line)
            if r.stderr:
                for line in r.stderr.splitlines():
                    print(f"[STDERR] {line}")
            if r.returncode != 0:
                self.report({'WARNING'}, f"Batch finished with errors (rc={r.returncode})")
                if r.stderr:
                    print(r.stderr)
            else:
                # Extract summary line
                lines = r.stdout.strip().splitlines()
                summary = lines[-1] if lines else "Done"
                self.report({'INFO'}, summary)
        except subprocess.TimeoutExpired:
            self.report({'ERROR'}, "Batch timed out (1h limit)")
            return {'CANCELLED'}
        except Exception as exc:
            self.report({'ERROR'}, str(exc))
            return {'CANCELLED'}

        return {'FINISHED'}


# ---------------------------------------------------------------------------
# Viewport Display — color objects by their collection
# ---------------------------------------------------------------------------

def _apply_show_names(scene, kind, show):
    """Set show_name on every SM_/LI_ collection instance (matching *kind*)."""
    n = 0
    for obj in scene.objects:
        if obj.library is not None:
            continue
        if outliner_cleanup._classify_link(obj) == kind:
            obj.show_name = bool(show)
            n += 1
    return n


def _upd_show_sm(self, context):
    _apply_show_names(self, 'SM', self.maplink_show_sm_names)


def _upd_show_li(self, context):
    _apply_show_names(self, 'LI', self.maplink_show_li_names)


class MAPLINK_OT_ColorByCollection(Operator):
    bl_idname = "maplink.color_objects_by_collection"
    bl_label = "Color Objects by Collection"
    bl_description = (
        "Set every local object's Viewport Display color to its collection's "
        "main color (from the nearest tagged ancestor, with a slight per-"
        "collection value offset so sub-collections differ). NoExport is "
        "black. (Set Viewport Shading > Color > Object to see it.)"
    )
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        scene = context.scene
        if scene is None:
            self.report({'ERROR'}, "No active scene")
            return {'CANCELLED'}
        res = outliner_cleanup.color_objects_by_collection(scene)
        self.report(
            {'INFO'},
            f"MapLink: viewport color set on {res['set']} object(s) "
            f"({res['black']} NoExport black), {res['skipped']} skipped. "
            f"Viewport Shading > Color > Object to view."
        )
        return {'FINISHED'}


# ---------------------------------------------------------------------------
# Panels
# ---------------------------------------------------------------------------

class MAPLINK_PT_Navigation(Panel):
    bl_label = "Navigation"
    bl_idname = "MAPLINK_PT_navigation"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "MapLink"
    bl_order = 0

    def draw(self, context):
        layout = self.layout

        # v1.19 — empty state, deliberately mirroring the UE Manager's
        # (v1.12 §A6): same two doors, same words, both sides.  Nothing below
        # this can do anything useful without a project, so nothing below this
        # is drawn.
        if project_roots.draw_empty_state(layout):
            return

        # Link health, when there is something to say. Silent when clean.
        link_scope.draw_badge(layout)

        # Breadcrumb — vertical stack of small buttons.  The current file
        # is shown as a non-clickable label; all ancestors are operator
        # buttons that jump straight to that level.
        # Each hop carries its own type icon, so the shape of the hierarchy
        # reads without parsing prefixes.  A crumb that isn't a MapLink asset
        # keeps the icon this panel has always drawn.
        crumbs = nav_drill.get_breadcrumb()
        if crumbs:
            col = layout.column(align=True)
            col.scale_y = 0.75
            last = len(crumbs) - 1
            for i, crumb in enumerate(crumbs):
                if i == last:
                    col.label(text=crumb,
                              **icons.label_args(crumb, default='RADIOBUT_ON'))
                else:
                    op = col.operator(
                        "maplink.drill_to_ancestor",
                        text=crumb,
                        **icons.label_args(crumb, default='TRIA_UP_BAR'),
                    )
                    op.target_index = i

        # Drill buttons
        row = layout.row(align=True)
        row.scale_y = 1.3
        row.operator("maplink.drill_up", text="Drill Up", icon='TRIA_UP')
        row.operator("maplink.drill_down", text="Drill Down", icon='TRIA_DOWN')

        # Context, on one line and labelled as such — the two shading buttons
        # said "Solid"/"Wire" with nothing tying them to context, which read as
        # a viewport shading control.  Save-before-drill sits with them rather
        # than under its own heading: it is a drill setting, and drill is what
        # this whole panel is.
        nav_props = getattr(context.scene, 'maplink_nav', None)
        if nav_props is not None:
            # Context on one line: the toggle, then the shading it will use.
            # The shading buttons grey out when there is nothing to shade --
            # they used to look live with no context in the file, which reads
            # as a broken control rather than an inactive one.
            live = nav_drill.has_context() and not nav_drill.context_hidden()

            row = layout.row(align=True)
            if nav_drill.has_context():
                hidden = nav_drill.context_hidden()
                row.operator("maplink.toggle_context",
                             text="Context",
                             icon='HIDE_ON' if hidden else 'HIDE_OFF',
                             depress=not hidden)
            else:
                # Same slot, same width, so the row does not jump about.
                row.prop(nav_props, "auto_context", text="Context",
                         icon='HIDE_OFF', toggle=True)

            sub = row.row(align=True)
            sub.enabled = live
            sub.prop(nav_props, "context_wireframe", expand=True, text="")

            col = layout.column(align=True)
            col.prop(nav_props, "auto_context", text="Enable Context-drill")
            col.prop(nav_props, "prompt_save_before_drill",
                     text="Prompt Save Before Drill")


class MAPLINK_PT_Settings(Panel):
    bl_label = "Settings"
    bl_idname = "MAPLINK_PT_settings"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "MapLink"
    bl_order = 3
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        layout = self.layout

        try:
            nav_props = context.scene.maplink_nav
        except (AttributeError, TypeError):
            return

        # UE status (v1.14 — relocated here from the removed "UE Link"
        # panel; includes the .blend name) + Copy Diagnostics.
        box = layout.box()
        box.label(text="UE Status", icon='LINKED')
        ue_status.draw_badge(box)
        try:
            if hasattr(bpy.ops.maplink, "copy_diagnostics"):
                box.operator("maplink.copy_diagnostics", icon='COPYDOWN')
        except Exception:
            pass

        # Drill settings live in the Navigation panel now, beside Context —
        # they are drill settings and drill is what that panel is for. No
        # category here just to hold one checkbox.

        # Display moved to its own panel — it is a viewport setting and it
        # belongs where the viewport is.

        # Batch — operations over a root tree of MapLink .blend files.
        # v1.14: the per-action batch buttons are folded into one
        # "Batch Standardize…" dialog (toggles + per-file selection table);
        # the Root auto-defaults to the project root on file load.
        box = layout.box()
        box.label(text="Batch", icon='FILE_REFRESH')
        try:
            batch_props = context.scene.maplink_batch
        except (AttributeError, TypeError):
            box.label(text="Properties not loaded", icon='ERROR')
            return
        box.prop(batch_props, "root_path", text="Root")
        box.operator("maplink.batch_standardize",
                     text="Batch Standardize…", icon='CHECKMARK')
        box.operator("maplink.batch_sidecar",
                     text="Generate All Sidecars", icon='FILE_REFRESH')
        # Bulk thumbnail work belongs here next to the other batch operations,
        # not in Tools: it is slow, it touches every asset file, and it is not
        # something done while modelling.
        box.operator("maplink.rebuild_all_thumbnails",
                     text="Rebuild All Thumbnails…", icon='IMAGE_DATA')
        box.operator("maplink.repair_asset_catalogs",
                     text="Repair Asset Catalogs…", icon='ASSET_MANAGER')
        # Force-Save All is a batch operation over the reference tree, so it
        # belongs with the other batch buttons rather than in a box of its own.
        box.operator("maplink.force_save_all",
                     text="Force-Save All (recursive)", icon='FILE_TICK')

        # Force-Save All moved into Batch above — it is a batch operation over
        # the reference tree, and a whole box for one button was spending
        # screen space to say so.



class MAPLINK_PT_Display(Panel):
    """Viewport organisation helpers.

    Its own panel rather than a box inside Settings: these are viewport
    settings, so they have to be reachable from the viewport, and Settings
    moved to the header menu and the Text Editor.
    """
    bl_label = "Display"
    bl_idname = "MAPLINK_PT_display"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "MapLink"
    bl_order = 2

    def draw(self, context):
        layout = self.layout
        layout.operator("maplink.color_objects_by_collection",
                        text="Color Objects by Collection",
                        icon='OUTLINER_COLLECTION')
        row = layout.row(align=True)
        row.prop(context.scene, "maplink_show_sm_names",
                 text="SM Names", toggle=True)
        row.prop(context.scene, "maplink_show_li_names",
                 text="LI Names", toggle=True)


class MAPLINK_PT_Perforce(Panel):
    bl_label = "Perforce"
    bl_idname = "MAPLINK_PT_perforce"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "MapLink"
    bl_order = 1

    def draw(self, context):
        layout = self.layout

        if not p4_integration.p4_available():
            # v1.21: see topbar.py - absence is a state, not an error.
            layout.label(text="Perforce not installed", icon='INFO')
            return

        props = context.scene.maplink_p4

        # Target toggle
        row = layout.row()
        row.prop(props, "target", expand=True)

        # Action buttons
        col = layout.column(align=True)
        col.operator("maplink.p4_show_in_explorer", icon='FILE_FOLDER')
        col.operator("maplink.p4_checkout", icon='IMPORT')
        col.operator("maplink.p4_revert", icon='LOOP_BACK')


# ---------------------------------------------------------------------------
# Batch Root auto-default (v1.14, item 1)
# ---------------------------------------------------------------------------

@bpy.app.handlers.persistent
def _autofill_batch_root(_dummy=None):
    """Default a blank Settings ▸ Batch Root to the MapLink project root
    (nearest maplink_project.json above the current .blend).  Never
    overwrites a user-set value; standalone-safe (silently does nothing
    without a manifest); creates no directories."""
    try:
        path = bpy.data.filepath
        if not path:
            return
        scene = bpy.context.scene
        props = getattr(scene, 'maplink_batch', None)
        if props is None or props.root_path.strip():
            return
        root = batch_standardize.find_project_root(os.path.dirname(path))
        if root:
            props.root_path = root
            print(f"[MapLink] Batch Root defaulted to project root: {root}")
    except Exception:
        pass


# ---------------------------------------------------------------------------
# Keymaps
# ---------------------------------------------------------------------------

_addon_keymaps: list = []


def _register_keymaps():
    wm = bpy.context.window_manager
    if wm.keyconfigs.addon is None:
        return

    # v1.14 — bind in the global 'Window' keymap instead of '3D View': the
    # 3D-View binding only fired with the cursor over a viewport (and was
    # reported dead on some setups); 'Window' resolves anywhere.
    km = wm.keyconfigs.addon.keymaps.new(name='Window', space_type='EMPTY')

    kmi = km.keymap_items.new("maplink.drill_down", 'PAGE_DOWN', 'PRESS', alt=True)
    _addon_keymaps.append((km, kmi))

    kmi = km.keymap_items.new("maplink.drill_up", 'PAGE_UP', 'PRESS', alt=True)
    _addon_keymaps.append((km, kmi))


def _unregister_keymaps():
    for km, kmi in _addon_keymaps:
        km.keymap_items.remove(kmi)
    _addon_keymaps.clear()


# ---------------------------------------------------------------------------
# Registration
# ---------------------------------------------------------------------------

# Panel *definitions*. They are never registered as-is: registry.collect()
# hands them to panels.py, which stamps out one concrete subclass per space so
# the sidebar can follow the "Sidebar location" preference.


def register():
    # Icons first — every other module's draw code may ask for one, and a
    # preview collection that isn't up yet just yields stock icons rather than
    # failing, but there is no reason to draw a single frame that way.
    icons.register()
    theme_colors.register()
    # Modules
    p4_integration.register()
    nav_drill.register()
    asset_replace.register()
    outliner_cleanup.register()
    # link_scope before sidecar_analysis: the sidecar gate asks it the
    # question, and its save_pre relativiser has to run before the write that
    # the sidecar's save_post then describes.
    link_scope.register()
    catalog_repair.register()
    project_roots.register()
    vendor_in.register()
    sidecar_analysis.register()
    fbx_export_on_save.register()
    force_save_all.register()
    quick_tools.register()
    modeling_tools.register()
    batch_standardize.register()
    maplink_asset_library.register()
    ue_status.register()
    diagnostics.register()
    topbar.register()
    context_menu.register()
    # Preferences (v1.4.0)
    bpy.utils.register_class(MAPLINK_AP_Preferences)

    # Batch sidecar
    bpy.utils.register_class(MAPLINK_PG_BatchSidecar)
    bpy.utils.register_class(MAPLINK_OT_BatchSidecar)
    bpy.types.Scene.maplink_batch = bpy.props.PointerProperty(type=MAPLINK_PG_BatchSidecar)

    # Viewport display helper
    bpy.utils.register_class(MAPLINK_OT_ColorByCollection)
    bpy.types.Scene.maplink_show_sm_names = bpy.props.BoolProperty(
        name="SM Names", default=False,
        description="Show SM_ instance names in the viewport",
        update=_upd_show_sm)
    bpy.types.Scene.maplink_show_li_names = bpy.props.BoolProperty(
        name="LI Names", default=False,
        description="Show LI_ instance names in the viewport",
        update=_upd_show_li)

    # Panels — built per space from the preference.
    registry.rebuild_panels()
    # Honour a theme preference saved from a previous session.
    try:
        theme_colors.sync_from_prefs(
            bpy.context.preferences.addons[__package__].preferences)
    except Exception:
        pass

    # Keymaps
    _register_keymaps()

    # Batch Root auto-default (v1.14) — on every file load, plus once now in
    # case the addon is enabled with a project file already open.
    bpy.app.handlers.load_post.append(_autofill_batch_root)
    _autofill_batch_root()


def unregister():
    try:
        bpy.app.handlers.load_post.remove(_autofill_batch_root)
    except ValueError:
        pass

    _unregister_keymaps()

    registry.unregister_panels()

    del bpy.types.Scene.maplink_show_sm_names
    del bpy.types.Scene.maplink_show_li_names
    bpy.utils.unregister_class(MAPLINK_OT_ColorByCollection)

    del bpy.types.Scene.maplink_batch
    bpy.utils.unregister_class(MAPLINK_OT_BatchSidecar)
    bpy.utils.unregister_class(MAPLINK_PG_BatchSidecar)

    bpy.utils.unregister_class(MAPLINK_AP_Preferences)

    context_menu.unregister()
    topbar.unregister()
    diagnostics.unregister()
    ue_status.unregister()
    maplink_asset_library.unregister()
    batch_standardize.unregister()
    modeling_tools.unregister()
    quick_tools.unregister()
    force_save_all.unregister()
    fbx_export_on_save.unregister()
    sidecar_analysis.unregister()
    vendor_in.unregister()
    project_roots.unregister()
    catalog_repair.unregister()
    link_scope.unregister()
    outliner_cleanup.unregister()
    asset_replace.unregister()
    nav_drill.unregister()
    p4_integration.unregister()
    # Last: nothing may look up an icon after the collection is gone, and
    # leaving it alive is what leaks across an addon reload.
    theme_colors.unregister()
    icons.unregister()
