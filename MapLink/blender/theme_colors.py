"""Write MapLink's colour code into Blender's collection colour theme.

collection.color_tag is an enum over eight *theme* swatches, not RGB, so the
tag alone can only ever get as close as the user's theme happens to be.  That
is why SM_ teal had to compromise to the green swatch.

Editing the theme removes the compromise: swatch 4 simply becomes teal, and the
Outliner then shows the exact accent the type icons use.

This writes to the user's global Blender preferences, which is a real
intrusion, so it is opt-in and reversible -- the values replaced are captured
on first apply and put back when the preference is switched off.

Bare-importable; no package-relative imports at module level.
"""

import bpy


# color_tag COLOR_0N -> the accent that swatch should carry.
# Only the swatches the MapLink colour code actually uses are touched; the rest
# of the user's theme is left exactly as they set it.
SWATCH_ACCENTS = {
    2: "#E4872A",   # COLOR_02 — LI_ amber
    4: "#18CAB9",   # COLOR_04 — SM_ teal, no longer a green stand-in
    5: "#2C7EB8",   # COLOR_05 — BP_ azure
    1: "#D53C3D",   # COLOR_01 — missing / deprecated red
    8: "#8B7355",   # COLOR_08 — NoExport, deliberately drab
}

# What the preferences UI shows: one label, one swatch. The slot number is an
# implementation detail of Blender's tag enum and means nothing to the artist,
# so it is not surfaced -- you pick "SM_", not "Color 04".
TYPE_SLOTS = (
    ("LI_",      2),
    ("SM_",      4),
    ("BP_",      5),
    ("Missing",  1),
    ("NoExport", 8),
)

# Populated on first apply so the change can be undone exactly.
_saved = {}


def _hex_to_rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4))


def _srgb_to_linear(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def _swatches():
    """-> the theme's ThemeCollectionColor list, or None."""
    try:
        theme = bpy.context.preferences.themes[0]
        return theme.collection_color
    except (AttributeError, IndexError, TypeError):
        return None


def apply(linear=True):
    """Push the MapLink accents into the theme. -> (changed, message)."""
    sw = _swatches()
    if sw is None:
        return 0, "collection colour theme unavailable on this build"

    changed = 0
    for index, hexv in SWATCH_ACCENTS.items():
        i = index - 1                      # COLOR_01 is element 0
        if not (0 <= i < len(sw)):
            continue
        rgb = _hex_to_rgb(hexv)
        if linear:
            rgb = tuple(_srgb_to_linear(c) for c in rgb)
        _saved.setdefault(i, tuple(sw[i].color))
        if tuple(sw[i].color) != rgb:
            sw[i].color = rgb
            changed += 1
    return changed, (f"{changed} swatch(es) set to the MapLink colour code"
                     if changed else "theme already matches")


def restore():
    """Put back whatever the swatches were before the first apply."""
    sw = _swatches()
    if sw is None or not _saved:
        return 0, "nothing to restore"
    n = 0
    for i, rgb in _saved.items():
        if 0 <= i < len(sw):
            sw[i].color = rgb
            n += 1
    _saved.clear()
    return n, f"{n} swatch(es) restored"


class MAPLINK_OT_ResetThemeDefaults(bpy.types.Operator):
    bl_idname = "maplink.reset_theme_defaults"
    bl_label = "Reset to Defaults"
    bl_description = ("Put the collection colour tags back to the MapLink "
                      "colour code, and re-apply the accents to the theme "
                      "swatches if the override is on")
    bl_options = {'REGISTER'}

    def execute(self, context):
        try:
            prefs = context.preferences.addons[__package__].preferences
        except (KeyError, AttributeError):
            self.report({'ERROR'}, "MapLink preferences unavailable.")
            return {'CANCELLED'}

        # The tag assignments: mirror of outliner_cleanup._DEFAULT_COLOR_MAP,
        # reached through the module so there is one table.
        try:
            from . import outliner_cleanup
            defaults = outliner_cleanup._DEFAULT_COLOR_MAP
        except Exception:
            defaults = {}
        for key, attr in (('sm', 'col_sm'), ('li', 'col_li'), ('bp', 'col_bp'),
                          ('other', 'col_other'),
                          ('noexport', 'col_noexport')):
            if key in defaults:
                try:
                    setattr(prefs, attr, defaults[key])
                except Exception:
                    pass

        n, msg = (apply() if getattr(prefs, "theme_match_maplink", False)
                  else (0, "swatch override is off"))
        self.report({'INFO'}, f"Colour code reset ({msg}).")
        return {'FINISHED'}


def register():
    bpy.utils.register_class(MAPLINK_OT_ResetThemeDefaults)


def unregister():
    try:
        bpy.utils.unregister_class(MAPLINK_OT_ResetThemeDefaults)
    except Exception:
        pass


def sync_from_prefs(prefs):
    """Apply or restore to match the preference. Never raises."""
    try:
        if getattr(prefs, "theme_match_maplink", False):
            n, msg = apply()
        else:
            n, msg = restore()
        if n:
            print(f"[MapLink] theme: {msg}")
    except Exception as e:                                  # pragma: no cover
        print(f"[MapLink] theme update skipped: {e}")
