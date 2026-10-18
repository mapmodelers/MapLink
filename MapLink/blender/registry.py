"""Which panel definitions go into which sidebar.

Separate from panels.py so that module stays about the mechanics of stamping
out per-space classes, and this one is the single place to answer "what is in
the viewport sidebar" without reading any registration code.

Kept out of __init__ so the preference's update callback can reach it without
importing the package into itself.
"""

import bpy

from . import panels


def collect():
    """-> (viewport_bases, text_bases).

    The viewport keeps the modelling set. The Text Editor gets everything,
    which is what makes it usable as a dedicated MapLink window.
    """
    import sys
    pkg = sys.modules[__package__]
    from . import quick_tools, modeling_tools

    nav = getattr(pkg, "MAPLINK_PT_Navigation", None)
    settings = getattr(pkg, "MAPLINK_PT_Settings", None)
    display = getattr(pkg, "MAPLINK_PT_Display", None)
    perforce = getattr(pkg, "MAPLINK_PT_Perforce", None)
    tools = getattr(quick_tools, "MAPLINK_PT_Tools", None)
    modeling = getattr(modeling_tools, "MAPLINK_PT_Modeling", None)

    viewport = [c for c in (nav, tools, display, modeling) if c is not None]
    text = [c for c in (nav, tools, display, modeling, perforce, settings)
            if c is not None]
    return viewport, text


def sidebar_mode():
    """The configured sidebar location, defaulting to the viewport."""
    try:
        addons = bpy.context.preferences.addons
        ad = addons.get(__package__) or addons.get(__name__.split('.')[0])
        if ad and ad.preferences is not None:
            return getattr(ad.preferences, "sidebar_space", 'VIEW_3D')
    except Exception:
        pass
    return 'VIEW_3D'


def rebuild_panels():
    viewport, text = collect()
    panels.register_panels(sidebar_mode(), viewport, text)


def unregister_panels():
    panels.unregister_panels()
