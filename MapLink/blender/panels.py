"""Where the MapLink sidebar lives.

A Panel's bl_space_type is fixed at registration, so a panel cannot follow a
preference by itself.  Instead the panel classes in the other modules are kept
*unregistered* and used purely as definitions; this module stamps out a
concrete subclass per space and registers those.  Changing the preference
re-runs that, so the tab moves without a restart.

Two different sets, on purpose:

    3D Viewport   the lean set -- Navigation, Tools, Modeling.  What you touch
                  while modelling, and nothing else, because that sidebar is
                  competing for space with the model.
    Text Editor   everything, including Settings and Perforce.  A Text Editor
                  area costs nothing to open and can be parked on a second
                  monitor, which makes it the right home for a full control
                  panel that would otherwise crowd the viewport.

Settings and Perforce are deliberately absent from the viewport set: they moved
to the MapLink header menu, and mirroring them in the viewport sidebar would
undo the space saving that motivated the move.
"""

import bpy

VIEW = 'VIEW_3D'
TEXT = 'TEXT_EDITOR'

_SUFFIX = {VIEW: "V3D", TEXT: "TXT"}

# Concrete classes made by _build(), in registration order. Kept so unregister
# takes down exactly what went up, even after the preference changed.
_live = []


def _build(base, space):
    """A registerable subclass of *base* bound to *space*."""
    suffix = _SUFFIX[space]
    return type(
        f"{base.__name__}_{suffix}",
        (base,),
        {
            # Every registered panel needs its own idname, or the second one
            # silently replaces the first.
            "bl_idname": f"{base.bl_idname}_{suffix.lower()}",
            "bl_space_type": space,
            "bl_region_type": 'UI',
            "bl_category": "MapLink",
            "__doc__": getattr(base, "__doc__", None),
        },
    )


def _spaces(mode):
    if mode == 'TEXT_EDITOR':
        return (TEXT,)
    if mode == 'BOTH':
        return (VIEW, TEXT)
    return (VIEW,)


def register_panels(mode, viewport_bases, text_bases):
    """(Re)register the sidebar for *mode*. Safe to call repeatedly."""
    unregister_panels()

    for space in _spaces(mode):
        bases = viewport_bases if space is VIEW else text_bases
        for base in bases:
            cls = _build(base, space)
            try:
                bpy.utils.register_class(cls)
                _live.append(cls)
            except Exception as e:                      # pragma: no cover
                print(f"[MapLink] could not register {cls.__name__}: {e}")


def unregister_panels():
    for cls in reversed(_live):
        try:
            bpy.utils.unregister_class(cls)
        except Exception:
            pass
    _live.clear()


def refresh(_self=None, _context=None):
    """Preference update callback — rebuild the panels in place.

    Signed to match Blender's update(self, context) so it can be handed
    straight to an EnumProperty.
    """
    from . import registry
    registry.rebuild_panels()
