"""MapLink asset type icons.

Custom preview icons for the MapLink asset types, so a row's type reads without
parsing its filename prefix.  Art is authored separately and built by
MapLink/authoring_icons/build_icons.py, which writes straight into
resources/icons/<size>/ next to this module.

The PNGs live under resources/ rather than in a sibling icons/ directory
because that would shadow this module on import.

Everything here degrades.  An unauthored or deleted PNG produces icon_value 0,
Blender draws nothing for a zero icon_value, so every accessor also hands back
a stock icon name and the caller uses that instead.  A missing icon is a
plainer row, never a blank one.

    get_icon('LEVEL')                  -> icon_value, or 0
    icon_for_datablock('LI_Rooftop')   -> ('WORLD', icon_value)
    label_args('LI_Rooftop')           -> {'icon_value': n} or {'icon': 'WORLD'}

The last one is what UI code should normally call:

    layout.label(text=stem, **icons.label_args(stem))

Do not add an SVG dependency or rasterise at runtime.  Consume the built PNGs.
"""

import os

import bpy
import bpy.utils.previews


# key -> (png stem, stock Blender icon to fall back to)
#
# The fallbacks are deliberately ordinary.  They are what the artist sees when
# art is missing, so they have to be legible and roughly on-meaning rather than
# clever.
_TYPES = {
    'LEVEL':             ('level',             'WORLD'),
    'BLUEPRINT':         ('blueprint',         'NODETREE'),
    'STATICMESH':        ('staticmesh',        'OUTLINER_OB_MESH'),
    'GAMEPLAY':          ('gameplay',          'GHOST_ENABLED'),
    'PROXY':             ('proxy',             'LINKED'),
    'MATERIAL_INSTANCE': ('material_instance', 'MATERIAL'),
    'MISSING':           ('missing',           'ERROR'),
    'PRESET':            ('preset',            'PRESET'),
    # Catch-all for a catalog asset with no MapLink prefix.
    'ASSET':             ('asset',             'ASSET_MANAGER'),
    # The brand mark, not a type. Identifies a menu or row as MapLink's rather
    # than saying what kind of asset it is, so classify() never returns it --
    # ask for it by key through brand().
    'MAPLINK':           ('maplink',           'OUTLINER_COLLECTION'),
}

# Sizes build_icons.py ships.  32 is what the UI draws; 64 is there for high
# UI scale.  Keep in step with ADDON_SIZES in that script.
SIZES = (32, 64)
DEFAULT_SIZE = 32

_ICON_DIR = os.path.join(os.path.dirname(__file__), "resources", "icons")

# Prefix table.  Order matters only in that it is checked before the _GP
# suffix, which cannot occur on its own.
_PREFIXES = (
    ("LI_", 'LEVEL'),
    ("BP_", 'BLUEPRINT'),
    ("SM_", 'STATICMESH'),
    ("PRESET_", 'PRESET'),
)

# Anything in a MapLink catalog that is not one of the pipeline types still
# gets an icon, so a row is never the odd one out with a stock icon beside
# badged neighbours. Resolved only when the caller asks for it, because outside
# a catalog an unprefixed name is simply not a MapLink asset.
GENERIC = 'ASSET'

_pcoll = None


# --------------------------------------------------------------- classifying

def stem_of(name):
    """Filename, path or datablock name -> bare stem.

    Tolerates a full path, a trailing .blend, and Blender's .001 duplicate
    suffix, all of which show up in the places this gets called from.
    """
    if not name:
        return ""
    stem = os.path.basename(str(name))
    if stem.lower().endswith(".blend"):
        stem = stem[:-6]
    # LI_Rooftop.001 -> LI_Rooftop
    head, dot, tail = stem.rpartition(".")
    if dot and tail.isdigit() and len(tail) == 3:
        stem = head
    return stem


def classify(name, catalog=False):
    """-> type key, or None when the name carries no MapLink prefix.

    *catalog* says the name came from a MapLink asset catalog, where an
    unprefixed name is still a MapLink asset -- a preset, say -- rather than
    something unrelated. Those resolve to the generic asset icon instead of
    None, so a catalog never mixes badged rows with stock-icon rows.

    This is the icon module's own reading of the naming convention and it is
    used for display only.  The export and standardization paths keep their own
    prefix tuples, which are not all the same set and whose differences are
    load-bearing; unifying them is a separate change and not one to make from
    a cosmetics pass.
    """
    stem = stem_of(name)
    if not stem:
        return None
    # UE's gameplay wrapper suffix.  Nothing in the addon produces these today,
    # but a linked name can arrive from a UE-authored file.
    if stem.endswith("_GP"):
        return 'GAMEPLAY'
    upper = stem.upper()
    for prefix, key in _PREFIXES:
        if upper.startswith(prefix):
            return key
    return GENERIC if catalog else None


# ------------------------------------------------------------------ accessors

def _cache_key(key, size):
    return f"{key}@{size}"


def get_icon(key, size=DEFAULT_SIZE):
    """-> icon_value for a type key, 0 when unavailable.

    0 is a valid thing to hand Blender: it draws nothing.  Callers must pair it
    with a stock icon, which is what icon_for_datablock and label_args do.
    """
    if _pcoll is None:
        return 0
    try:
        return _pcoll[_cache_key(key, size)].icon_id
    except KeyError:
        return 0


def icon_for_datablock(name, size=DEFAULT_SIZE, default='NONE', catalog=False):
    """-> (stock icon name, icon_value).

    *default* is the stock icon for a name that carries no MapLink prefix, so
    a caller can pass whatever it drew before and keep that appearance for
    non-MapLink rows.
    """
    key = classify(name, catalog=catalog)
    if key is None:
        return default, 0
    return _TYPES[key][1], get_icon(key, size)


def label_args(name, size=DEFAULT_SIZE, default='NONE', catalog=False):
    """Keyword arguments for layout.label / layout.operator / layout.prop.

    Returns exactly one of icon_value or icon, never both, so there is no
    reliance on Blender's precedence between them.
    """
    fallback, icon_id = icon_for_datablock(name, size, default, catalog)
    if icon_id:
        return {"icon_value": icon_id}
    return {"icon": fallback}


def brand(size=DEFAULT_SIZE):
    """Keyword arguments carrying the MapLink mark.

    For places that identify themselves as MapLink -- the top-bar menu, the
    viewport submenu -- rather than places that show an asset's type. Falls
    back to a stock icon while the mark is unauthored, same contract as
    label_args.
    """
    icon_id = get_icon('MAPLINK', size)
    if icon_id:
        return {"icon_value": icon_id}
    return {"icon": _TYPES['MAPLINK'][1]}


def enum_icon(name, size=DEFAULT_SIZE):
    """-> int for the 5-tuple form of an EnumProperty item.

    EnumProperty items take an integer icon id in slot 3.  A stock icon can be
    named there as a string instead, but the two cannot be mixed within one
    item, so this returns 0 and the caller decides.
    """
    return icon_for_datablock(name, size)[1]


# --------------------------------------------------------------- registration

def register():
    global _pcoll

    # Reload leaves the old collection alive and Blender leaks it, which is the
    # standard way this API goes wrong.  Clear first, unconditionally.
    unregister()

    # Headless has no icons to draw and the pipeline workers all run headless,
    # so skip the load entirely there.  Accessors return 0 and callers fall
    # back, which is the same path a missing PNG takes.
    if bpy.app.background:
        return

    try:
        _pcoll = bpy.utils.previews.new()
    except Exception as e:                                  # pragma: no cover
        print(f"[MapLink] icons: preview collection unavailable ({e}); "
              f"stock icons will be used.")
        _pcoll = None
        return

    missing = []
    for key, (fname, _fallback) in _TYPES.items():
        for size in SIZES:
            path = os.path.join(_ICON_DIR, str(size), f"{fname}.png")
            if not os.path.exists(path):
                if size == DEFAULT_SIZE:
                    missing.append(fname)
                continue
            try:
                _pcoll.load(_cache_key(key, size), path, 'IMAGE')
            except Exception as e:                          # pragma: no cover
                print(f"[MapLink] icons: could not load {path}: {e}")

    # load() is lazy -- nothing is decoded until icon_id is read, and the first
    # draw after register would otherwise come up blank.  Touch them all once,
    # here, where it costs nothing anyone can see.
    for cache_key in list(_pcoll.keys()):
        try:
            _ = _pcoll[cache_key].icon_id
        except Exception:                                   # pragma: no cover
            pass

    # One line at register, not one per draw.
    if missing:
        print(f"[MapLink] icons: {len(missing)} type icon(s) not present "
              f"({', '.join(sorted(missing))}); those rows use stock Blender "
              f"icons.")


def unregister():
    global _pcoll
    if _pcoll is not None:
        try:
            bpy.utils.previews.remove(_pcoll)
        except Exception:                                   # pragma: no cover
            pass
        _pcoll = None
