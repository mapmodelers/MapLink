"""MapLink_Blender — Outliner Cleanup operator.

Renames mesh objects (and their single-user meshes) so each name reflects
the user-collection chain containing it, joined by '_', with per-basename
'.001/.002/...' numbering assigned in alphabetical object order. Skips
non-mesh types, NoExport branches, collection instances, and linked data.
Uses a mandatory two-pass rename (temp names first) to defeat Blender's
auto-suffix on collisions.
"""

import os
import re
import bpy
from bpy.types import Operator


_TMP_PREFIX = "__MAPLINK_TMP_"
_SANITIZE_RE = re.compile(r'[^A-Za-z0-9_]')


def _sanitize(name: str) -> str:
    """Replace anything not [A-Za-z0-9_] with '_'."""
    return _SANITIZE_RE.sub('_', name)


def _build_parent_map(root):
    """DFS from root through .children, returning {child_collection: parent}.

    A Blender collection can technically have multiple parents (DAG), but we
    only record the first one we find — deterministic and sufficient for
    deriving a single naming chain.
    """
    parent = {}
    stack = [root]
    while stack:
        coll = stack.pop()
        for child in coll.children:
            if child not in parent:
                parent[child] = coll
                stack.append(child)
    return parent


def _ancestor_path(root, target, parent_map):
    """Return collection chain from just-below-root down to target inclusive.

    Empty list if target is root itself, or if target isn't reachable from
    root via parent_map (orphaned collection).
    """
    if target == root:
        return []
    path = []
    cur = target
    while cur is not None and cur != root:
        path.append(cur)
        cur = parent_map.get(cur)
    if cur != root:
        return []  # unreachable
    path.reverse()
    return path


def _is_in_noexport(obj, scene, parent_map):
    """True if any of obj's user_collections (or any ancestor) is named
    'NoExport' (case-sensitive). Walks every membership chain, not just the
    chosen one — intent is 'don't touch anything the user flagged'.
    """
    for coll in obj.users_collection:
        cur = coll
        while cur is not None and cur != scene.collection:
            if cur.name == "NoExport":
                return True
            cur = parent_map.get(cur)
    return False


def run_cleanup(scene):
    """Core Outliner-name cleanup, headless-safe (no operator/report deps).

    Renames mesh objects (and their single-user mesh data) by collection
    hierarchy with per-basename .001/.002 numbering.  Returns
    ``(msg, n_obj, n_mesh)`` or ``None`` when *scene* is None.
    """
    if scene is None:
        return None

    # Filename fallback for objects sitting directly under Scene Collection.
    blend_path = bpy.data.filepath
    file_stem = ""
    if blend_path:
        stem = os.path.splitext(os.path.basename(blend_path))[0]
        file_stem = _sanitize(stem)

    parent_map = _build_parent_map(scene.collection)

    # Skip-bucket counters
    skip_noexport = 0
    skip_instance = 0
    skip_empty = 0     # non-mesh types (empties/lights/cameras/etc.)
    skip_linked = 0
    skip_no_basename = 0
    skip_other = 0
    warnings = []

    # Iterate in alphabetical order matching bpy.data.objects sorted by name.
    # Restrict to scene-bound objects so we have a real collection chain.
    sorted_objs = sorted(scene.objects, key=lambda o: o.name)

    # Phase A — classify and derive basenames.
    eligible = []  # list of (obj, basename) in sort order
    for obj in sorted_objs:
        # Linked from another file → never touch.
        if obj.library is not None:
            skip_linked += 1
            continue
        # Wrong type → empties/lights/cameras/armatures/curves/lattices/gpencil.
        if obj.type != 'MESH':
            skip_empty += 1
            continue
        # Defensive: mesh-typed object with no data slot.
        if obj.data is None:
            skip_other += 1
            continue
        # Linked mesh data.
        if obj.data.library is not None:
            skip_linked += 1
            continue
        # Collection instances.
        if obj.instance_type == 'COLLECTION':
            skip_instance += 1
            continue
        # NoExport anywhere in the parent chain (across all memberships).
        if _is_in_noexport(obj, scene, parent_map):
            skip_noexport += 1
            continue

        users = list(obj.users_collection)
        if not users:
            # Not in any collection at all — can't derive a chain.
            skip_no_basename += 1
            continue

        if len(users) > 1:
            warnings.append(
                f"'{obj.name}' is in {len(users)} collections; "
                f"using '{users[0].name}'"
            )
        chosen = users[0]

        if chosen == scene.collection:
            # Directly under Scene Collection → use sanitized .blend stem.
            if not file_stem:
                warnings.append(
                    f"'{obj.name}' is directly under Scene Collection but the "
                    f"file has never been saved; skipping"
                )
                skip_no_basename += 1
                continue
            basename = file_stem
        else:
            chain = _ancestor_path(scene.collection, chosen, parent_map)
            if not chain:
                skip_other += 1
                continue
            basename = "_".join(_sanitize(c.name) for c in chain)

        if not basename:
            skip_no_basename += 1
            continue

        eligible.append((obj, basename))

    # Phase B — assign .NNN suffixes. Counter is per final basename across
    # the whole scene, so two collections sharing a basename share a counter.
    counters = {}
    plan = []  # list of (obj, final_name)
    for obj, basename in eligible:
        n = counters.get(basename, 0) + 1
        counters[basename] = n
        plan.append((obj, f"{basename}.{n:03d}"))

    # Phase C — Pass 1: rename objects (and single-user meshes) to temp names.
    # This frees every final target so Pass 2 can assign them without Blender
    # appending its own .NNN suffix on collision.
    tmp_idx = 0
    obj_temp = []         # (obj, final_name) ready for Pass 2
    mesh_temp = []        # (mesh, final_name) ready for Pass 2

    for obj, final_name in plan:
        obj.name = f"{_TMP_PREFIX}{tmp_idx}__"
        tmp_idx += 1
        obj_temp.append((obj, final_name))

        mesh = obj.data
        # users == 1 means this object is the mesh's sole user (and no fake
        # user). users >= 2 — including the fake-user case — means something
        # else relies on the current mesh name; leave it.
        if mesh is not None and mesh.library is None and mesh.users == 1:
            mesh.name = f"{_TMP_PREFIX}{tmp_idx}__"
            tmp_idx += 1
            mesh_temp.append((mesh, final_name))

    # Phase D — Pass 2: assign final names.
    n_obj = 0
    n_mesh = 0
    for obj, final_name in obj_temp:
        obj.name = final_name
        n_obj += 1
    for mesh, final_name in mesh_temp:
        mesh.name = final_name
        n_mesh += 1

    total_skipped = (skip_noexport + skip_instance + skip_empty
                     + skip_linked + skip_no_basename + skip_other)

    # Surface warnings on the system console for inspection.
    for w in warnings:
        print(f"[MapLink Cleanup] WARN: {w}")

    msg = (
        f"MapLink Cleanup: renamed {n_obj} objects, {n_mesh} meshes. "
        f"Skipped {total_skipped} ("
        f"NoExport: {skip_noexport}, instances: {skip_instance}, "
        f"empties: {skip_empty}, linked: {skip_linked}, "
        f"no-basename: {skip_no_basename}, other: {skip_other}). "
        f"Warnings: {len(warnings)}."
    )
    return (msg, n_obj, n_mesh)


class MAPLINK_OT_CleanupOutlinerNames(Operator):
    bl_idname = "maplink.cleanup_outliner_names"
    bl_label = "MapLink: Cleanup Outliner Names"
    bl_description = (
        "Rename mesh objects (and their single-user mesh data) based on "
        "collection hierarchy, with per-basename .001/.002 numbering"
    )
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        try:
            res = run_cleanup(context.scene)
            if res is None:
                self.report({'ERROR'}, "No active scene")
                return {'CANCELLED'}
            msg = res[0]
            self.report({'INFO'}, msg)
            print(f"[MapLink Cleanup] {msg}")
            return {'FINISHED'}
        except Exception as exc:
            import traceback
            traceback.print_exc()
            self.report({'ERROR'}, f"MapLink Cleanup failed: {exc}")
            return {'CANCELLED'}


# ---------------------------------------------------------------------------
# Collection cleanup — move SM_/LI_ instances into LINK > LINK_SM / LINK_LI
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Collection cleanup + viewport colouring
#   Target layout (children of the Scene Collection; NoExport pinned on top):
#       NoExport                       (outside the asset-marked collection)
#       <main collection / asset>
#           LINK
#               LINK_LI
#               LINK_SM
#           <other collections / objects>
# ---------------------------------------------------------------------------

_LINK_ROOT = "LINK"
_LINK_SUB = {'SM': "LINK_SM", 'LI': "LINK_LI"}
_NOEXPORT = "NoExport"
_STRUCTURAL = {_LINK_ROOT, "LINK_SM", "LINK_LI", _NOEXPORT}
_COLOR_TAGS = ['COLOR_01', 'COLOR_02', 'COLOR_03', 'COLOR_04',
               'COLOR_05', 'COLOR_06', 'COLOR_07', 'COLOR_08']
_BLACK = (0.0, 0.0, 0.0, 1.0)


def _classify_link(obj):
    """Return 'SM' / 'LI' if *obj* is an SM_/LI_ collection instance, else None.

    Classifies by the instanced collection's name (the robust signal), falling
    back to the object's own name when there is no instance collection.
    """
    if obj.instance_type != 'COLLECTION':
        return None
    ic = obj.instance_collection
    name = ic.name if ic is not None else obj.name
    if name.startswith("SM_"):
        return 'SM'
    if name.startswith("LI_"):
        return 'LI'
    return None


def _child_named(coll, name):
    return next((c for c in coll.children if c.name == name), None)


def _find_asset_collection(scene):
    """The 'main' collection — the one marked as an asset.  Fallback: the first
    child of the Scene Collection that isn't NoExport or a LINK bucket."""
    master = scene.collection
    stack = list(master.children)
    while stack:
        c = stack.pop()
        if getattr(c, 'asset_data', None) is not None:
            return c
        stack.extend(c.children)
    for c in master.children:
        if c.name not in _STRUCTURAL and c.library is None:
            return c
    return None


def _ensure_link_sub(parent, sub_name):
    """Find-or-create LINK under *parent* and its <sub_name> child; return sub."""
    link = _child_named(parent, _LINK_ROOT)
    if link is None:
        link = bpy.data.collections.new(_LINK_ROOT)
        parent.children.link(link)
    sub = _child_named(link, sub_name)
    if sub is None:
        sub = bpy.data.collections.new(sub_name)
        link.children.link(sub)
    return sub


def _theme_tag_colors():
    """RGB list for COLOR_01..COLOR_08 from the active theme, or None."""
    try:
        tc = bpy.context.preferences.themes[0].collection_color
        cols = [(c.color[0], c.color[1], c.color[2]) for c in tc]
        return cols or None
    except Exception:
        return None


def _stable_rgb(name):
    """Deterministic, well-separated colour from a collection name."""
    import hashlib
    import colorsys
    h = hashlib.md5(name.encode('utf-8')).digest()
    hue = h[0] / 255.0
    sat = 0.55 + (h[1] / 255.0) * 0.35
    val = 0.65 + (h[2] / 255.0) * 0.30
    return colorsys.hsv_to_rgb(hue, sat, val)


def _value_offset(name):
    """Slight deterministic value delta (about +/-0.08) keyed on a name, so
    sibling sub-collections read as different shades of the same main colour."""
    import hashlib
    h = hashlib.md5(name.encode('utf-8')).digest()
    return ((h[3] / 255.0) - 0.5) * 0.16


def _noexport_chain(coll, parent_map, master):
    cur = coll
    while cur is not None and cur != master:
        if cur.name == _NOEXPORT:
            return True
        cur = parent_map.get(cur)
    return False


def _set_color_if_diff(obj, rgba):
    """Set obj.color only if it differs; return True when changed."""
    cur = tuple(obj.color)
    if any(abs(a - b) > 1e-4 for a, b in zip(cur, rgba)):
        obj.color = rgba
        return True
    return False


# How much darker a local object is than a linked asset collection. Bright
# means "this is a linked asset, edit it by drilling"; dark means "this is
# geometry in the file you already have open".
_LOCAL_VALUE_SCALE = 0.45


def _type_rgb(type_key, theme, color_map):
    """Theme RGB for a colour-code key ('sm' / 'li' / 'bp'), or None.

    Goes through the theme swatch the tag points at, so object colours track
    whatever the Theme preferences say rather than carrying a second copy of
    the palette.
    """
    if not type_key or theme is None:
        return None
    tag = (color_map or {}).get(type_key)
    if not tag or tag == 'NONE':
        return None
    try:
        idx = int(tag.rsplit('_', 1)[-1]) - 1
    except (ValueError, IndexError):
        return None
    return theme[idx] if 0 <= idx < len(theme) else None


def _instanced_type_key(obj):
    """Colour-code key for a linked collection instance, else None.

    Keyed on the INSTANCED collection's name, not the object's. An object
    called LI_something is not a level -- it is a mesh that happens to live in
    an LI_ file, and it will import into UE as a StaticMesh.
    """
    if obj.instance_type != 'COLLECTION':
        return None
    coll = obj.instance_collection
    if coll is None or coll.library is None:
        return None
    return _asset_type_key(coll.name)


def color_objects_by_collection(scene):
    """Set obj.color so the viewport says what each object IS in UE terms.

    Two things are being distinguished, and they are not the same question:

      what type is it   a linked SM_ collection instance takes the SM colour,
                        a linked LI_ collection instance takes the LI colour.
                        A plain mesh takes the SM colour whatever it is called,
                        because that is what it becomes on import -- an object
                        named LI_wall inside an LI_ file is still a StaticMesh.

      is it linked      linked asset collections are drawn at full brightness;
                        local geometry is darkened. So "bright = drill into it,
                        dark = it lives here" reads at a glance.

    NoExport objects are always black. Headless-safe.
    Returns dict(set, skipped, black, changed)."""
    import colorsys
    master = scene.collection
    theme = _theme_tag_colors()
    color_map = _prefs_color_map()
    parent_map = _build_parent_map(master)

    def _tag_rgb(coll):
        tag = getattr(coll, 'color_tag', 'NONE')
        if theme is not None and tag and tag != 'NONE':
            try:
                idx = int(tag.rsplit('_', 1)[-1]) - 1
                if 0 <= idx < len(theme):
                    return theme[idx]
            except (ValueError, IndexError):
                pass
        return None

    def _main_color_and_top(coll):
        chain = []
        cur = coll
        while cur is not None and cur != master:
            chain.append(cur)
            rgb = _tag_rgb(cur)
            if rgb is not None:
                return rgb, chain[-1]
            cur = parent_map.get(cur)
        top = chain[-1] if chain else coll
        return _stable_rgb(top.name), top

    n_set = n_skip = n_black = n_changed = 0
    for obj in scene.objects:
        if obj.library is not None:
            n_skip += 1
            continue
        users = obj.users_collection
        if not users:
            n_skip += 1
            continue
        coll = next((c for c in users if c != master), users[0])

        if _noexport_chain(coll, parent_map, master):
            if _set_color_if_diff(obj, _BLACK):
                n_changed += 1
            n_black += 1
            n_set += 1
            continue

        type_key = _instanced_type_key(obj)
        is_linked_asset = type_key is not None

        # A local mesh is a StaticMesh once imported, whatever its name says.
        base = _type_rgb(type_key or 'sm', theme, color_map)
        if base is None:
            base, _top = _main_color_and_top(coll)

        hh, ss, vv = colorsys.rgb_to_hsv(*base)
        if is_linked_asset:
            # Keep sibling assets in one LINK bucket distinguishable, but stay
            # in the bright half so the linked/local split still reads.
            vv = min(1.0, max(0.55, vv + _value_offset(
                obj.instance_collection.name)))
        else:
            vv = min(1.0, max(0.06, vv * _LOCAL_VALUE_SCALE))
        base = colorsys.hsv_to_rgb(hh, ss, vv)
        if _set_color_if_diff(obj, (base[0], base[1], base[2], 1.0)):
            n_changed += 1
        n_set += 1

    return dict(set=n_set, skipped=n_skip, black=n_black, changed=n_changed)


# One colour code, two carriers: the LINK_SM / LINK_LI structural collections
# this file has always tagged, and the file's own asset collection at the top
# (added alongside the type icons).  Same type, same swatch, so an SM_ reads
# the same colour whether you are looking at it in its own file or in the LINK
# branch of a file that consumes it.
#
# color_tag is an enum over eight fixed theme swatches, not RGB, so the exact
# accent cannot be reproduced here.  There is no cyan in the palette; green is
# the nearest free swatch once blue is spent on BP_.  That is a deliberate
# compromise -- the tag is a coarse Outliner grouping cue and the icon carries
# the real accent.  Rewriting the user's theme to force a cyan swatch is not on
# the table: a plugin editing a global theme is hostile.
_DEFAULT_COLOR_MAP = {'sm': 'COLOR_04',        # green
                      'li': 'COLOR_02',        # orange
                      'bp': 'COLOR_05',        # blue
                      'other': 'NONE',
                      'noexport': 'COLOR_08'}  # brown

# icons.classify() keys -> colour-code keys.
_TYPE_TO_TAG_KEY = {
    'STATICMESH': 'sm',
    'LEVEL':      'li',
    'BLUEPRINT':  'bp',
}


def _asset_type_key(name):
    """-> 'sm' / 'li' / 'bp' for a .blend path or stem, else None.

    Imported lazily and defensively on purpose.  This module is also imported
    bare (not as a package member) by the headless batch workers, where a
    module-level relative import would fail outright.  Losing the asset-
    collection tag in that case is cosmetic; failing the import is not.
    """
    try:
        from . import icons
        return _TYPE_TO_TAG_KEY.get(icons.classify(name))
    except Exception:
        return None


def _asset_collection(master):
    """The file's own top-level asset collection, or None.

    Prefers the asset-marked collection, which is what MapLink actually creates
    and what the thumbnail worker keys off.  Falls back to a top-level child
    named after the .blend, for files predating the marking.
    """
    for c in bpy.data.collections:
        if c.library is None and getattr(c, 'asset_data', None) is not None:
            return c
    stem = os.path.splitext(os.path.basename(bpy.data.filepath))[0]
    if not stem:
        return None
    return next((c for c in master.children
                 if c.library is None and c.name == stem), None)


def _prefs_color_map():
    """Read the user's collection colour code from the addon preferences.
    Falls back to the defaults when prefs aren't available (e.g. headless)."""
    try:
        addons = bpy.context.preferences.addons
        ad = addons.get(__package__) or addons.get(__name__.split('.')[0])
        if ad and ad.preferences is not None:
            p = ad.preferences
            # No master switch: the preference defaults ARE the MapLink colour
            # code, so an untouched install already matches and "Reset to
            # Defaults" is how you get back to it. A second toggle saying the
            # same thing was one more state to reason about for no gain.
            return {'sm': getattr(p, 'col_sm', _DEFAULT_COLOR_MAP['sm']),
                    'li': getattr(p, 'col_li', _DEFAULT_COLOR_MAP['li']),
                    'bp': getattr(p, 'col_bp', _DEFAULT_COLOR_MAP['bp']),
                    'other': getattr(p, 'col_other', _DEFAULT_COLOR_MAP['other']),
                    'noexport': getattr(p, 'col_noexport',
                                        _DEFAULT_COLOR_MAP['noexport'])}
    except Exception:
        pass
    return dict(_DEFAULT_COLOR_MAP)


def _collection_color_type(coll):
    """'sm' for LINK_SM, 'li' for LINK_LI, else 'other'."""
    if coll.name == "LINK_SM":
        return 'sm'
    if coll.name == "LINK_LI":
        return 'li'
    return 'other'


def _assign_collection_tags(scene, color_map=None):
    """Assign colour_tags by collection TYPE (a configurable colour code):
    LINK_SM -> color_map['sm'], LINK_LI -> color_map['li'], the NoExport
    subtree -> color_map['noexport'], and every other local collection (incl.
    LINK and the asset collection) -> color_map['other'] (which may be 'NONE'
    for no tag).  NoExport *objects* are still painted black in the viewport by
    color_objects_by_collection — only the NoExport collection's outliner tag
    is set here.  *color_map* lets the in-app op (from prefs) and the batch
    worker (from CLI tokens) share the same code.  Returns the number changed."""
    if scene is None:
        return 0
    if color_map is None:
        color_map = _prefs_color_map()
    master = scene.collection
    parent_map = _build_parent_map(master)

    stack = list(master.children)
    all_colls = []
    while stack:
        c = stack.pop()
        all_colls.append(c)
        stack.extend(c.children)

    # The file's own asset collection is tagged by the type its .blend name
    # declares, which is the one piece of type information the structural
    # LINK_SM / LINK_LI naming cannot carry.
    asset_coll = _asset_collection(master)
    asset_key = _asset_type_key(bpy.data.filepath) if asset_coll else None

    changed = 0
    for c in all_colls:
        if c.library is not None:
            continue
        if asset_key and c is asset_coll:
            want = color_map.get(asset_key, color_map.get('other', 'NONE'))
        elif _noexport_chain(c, parent_map, master):   # NoExport (or inside it)
            want = color_map.get('noexport', _DEFAULT_COLOR_MAP['noexport'])
        else:
            want = color_map.get(_collection_color_type(c),
                                 color_map.get('other', 'NONE'))
        if getattr(c, 'color_tag', 'NONE') != want:
            try:
                c.color_tag = want
                changed += 1
            except Exception:
                pass
    return changed


def _pin_link_top(parent):
    """Make the LINK collection the first child of *parent* (top of the asset
    collection's contents).  Returns True if it moved.  Same outliner caveat as
    NoExport pinning: link order is only honoured with Sort Alphabetically OFF."""
    if parent is None:
        return False
    link = _child_named(parent, _LINK_ROOT)
    if link is None:
        return False
    children = list(parent.children)
    if children and children[0] is link:
        return False
    rest = [c for c in children if c is not link]
    for c in children:
        try:
            parent.children.unlink(c)
        except Exception:
            pass
    parent.children.link(link)
    for c in rest:
        try:
            parent.children.link(c)
        except Exception:
            pass
    return True


def _delete_empty_collections(scene):
    """Remove local, non-structural, non-asset collections that hold no objects
    and no child collections.  Bottom-up (repeat until stable).  Returns count."""
    master = scene.collection
    removed = 0
    changed = True
    while changed:
        changed = False
        stack = list(master.children)
        all_colls = []
        while stack:
            c = stack.pop()
            all_colls.append(c)
            stack.extend(c.children)
        for c in all_colls:
            if c.library is not None or c.name in _STRUCTURAL:
                continue
            if getattr(c, 'asset_data', None) is not None:
                continue
            if len(c.objects) == 0 and len(c.children) == 0:
                try:
                    bpy.data.collections.remove(c)
                    removed += 1
                    changed = True
                except Exception:
                    pass
    return removed


def _pin_noexport_top(scene):
    """Make a NoExport collection a direct child of the Scene Collection (out of
    the asset) and first in link order (top of the outliner).  Returns True if
    anything moved.

    NOTE: the outliner only honours link order when 'Sort Alphabetically' is
    OFF; with it on, Blender sorts by name regardless of link order.
    """
    master = scene.collection
    stack = list(master.children)
    noexp = None
    while stack:
        c = stack.pop()
        if c.name == _NOEXPORT:
            noexp = c
            break
        stack.extend(c.children)
    if noexp is None:
        return False

    moved = False
    parent_map = _build_parent_map(master)
    cur_parent = parent_map.get(noexp)
    if cur_parent is not None and cur_parent is not master:
        try:
            cur_parent.children.unlink(noexp)
        except Exception:
            pass
        if noexp.name not in {c.name for c in master.children}:
            master.children.link(noexp)
        moved = True

    children = list(master.children)
    if noexp in children and children[0] is not noexp:
        rest = [c for c in children if c is not noexp]
        for c in children:
            try:
                master.children.unlink(c)
            except Exception:
                pass
        master.children.link(noexp)
        for c in rest:
            master.children.link(c)
        moved = True
    return moved


def run_collection_cleanup(scene, color_map=None):
    """Organise the scene into the MapLink outliner layout:
        NoExport (top, outside the asset)
        <main asset collection>
            LINK > LINK_LI / LINK_SM   (SM_/LI_ instances moved here)
            <other collections / objects>
    plus delete empty leftover collections, assign collection colour tags, and
    colour objects by collection (NoExport black).  Headless-safe.
    Returns ``(msg, n_changed)`` or None.
    """
    if scene is None:
        return None
    master = scene.collection
    main = _find_asset_collection(scene)
    link_parent = main if main is not None else master

    # 1. Move SM_/LI_ instances into LINK > LINK_SM / LINK_LI (lazy creation).
    moved_sm = moved_li = 0
    sub_cache = {}
    for obj in list(scene.objects):
        if obj.library is not None:
            continue
        kind = _classify_link(obj)
        if kind is None:
            continue
        sub_name = _LINK_SUB[kind]
        users = list(obj.users_collection)
        if len(users) == 1 and users[0].name == sub_name:
            continue
        sub = sub_cache.get(sub_name)
        if sub is None:
            sub = _ensure_link_sub(link_parent, sub_name)
            sub_cache[sub_name] = sub
        for c in users:
            try:
                c.objects.unlink(obj)
            except Exception:
                pass
        try:
            sub.objects.link(obj)
        except Exception:
            pass
        if kind == 'SM':
            moved_sm += 1
        else:
            moved_li += 1

    # 2. LINK to the top of the asset collection; NoExport out of the asset.
    link_pinned = _pin_link_top(link_parent)
    reordered = _pin_noexport_top(scene)

    # 3. Delete empty leftover collections.
    n_deleted = _delete_empty_collections(scene)

    # 4. Assign collection colour tags (configurable code) + colour objects.
    n_tags = _assign_collection_tags(scene, color_map)
    cres = color_objects_by_collection(scene)

    n_changed = (moved_sm + moved_li + n_deleted + n_tags + cres['changed']
                 + (1 if reordered else 0) + (1 if link_pinned else 0))
    msg = (f"MapLink Collections: moved {moved_sm} SM + {moved_li} LI, "
           f"deleted {n_deleted} empty, tagged {n_tags} collection(s), "
           f"recoloured {cres['changed']} object(s) "
           f"({cres['black']} NoExport black)"
           f"{'; LINK pinned to top' if link_pinned else ''}"
           f"{'; NoExport pinned to top' if reordered else ''}.")
    return (msg, n_changed)


class MAPLINK_OT_CleanupOutlinerCollections(Operator):
    bl_idname = "maplink.cleanup_outliner_collections"
    bl_label = "MapLink: Cleanup Outliner Collections"
    bl_description = ("Move every SM_/LI_ collection instance into "
                      "LINK > LINK_SM / LINK_LI (created if missing)")
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        try:
            res = run_collection_cleanup(context.scene)
            if res is None:
                self.report({'ERROR'}, "No active scene")
                return {'CANCELLED'}
            msg, _n = res
            self.report({'INFO'}, msg)
            print(f"[MapLink Collections] {msg}")
            return {'FINISHED'}
        except Exception as exc:
            import traceback
            traceback.print_exc()
            self.report({'ERROR'},
                        f"MapLink Collection cleanup failed: {exc}")
            return {'CANCELLED'}


def register():
    bpy.utils.register_class(MAPLINK_OT_CleanupOutlinerNames)
    bpy.utils.register_class(MAPLINK_OT_CleanupOutlinerCollections)


def unregister():
    bpy.utils.unregister_class(MAPLINK_OT_CleanupOutlinerCollections)
    bpy.utils.unregister_class(MAPLINK_OT_CleanupOutlinerNames)
