"""Menu draw-function registration that survives an addon reload.

bpy.types.Menu.append() stores the function object itself.  Reinstalling the
addon over a running copy re-executes every module, so unregister() ends up
calling menu.remove() with a *newly created* function object that was never in
the list -- the removal quietly fails, the previous one stays attached, and the
next append adds a second entry.  The visible symptom is every MapLink item
appearing twice in a context menu, then three times, and so on.

Matching by __name__ instead of identity fixes it, because the name is stable
across module reloads while the function object is not.

Bare-importable: no package-relative imports, so the headless workers can pull
it in the same way they reach batch_sidecar.
"""


def _draw_funcs(menu):
    """The menu's appended draw functions, or [] if that is unavailable.

    _dyn_ui_initialize() is how Menu.append and Menu.remove reach the list
    themselves, so reading it is the same contract they use.
    """
    try:
        return list(menu._dyn_ui_initialize())
    except Exception:
        return []


def remove_all(menu, func_name):
    """Detach every draw function called *func_name*. -> how many went."""
    if menu is None or not func_name:
        return 0
    removed = 0
    for func in _draw_funcs(menu):
        if getattr(func, "__name__", None) == func_name:
            try:
                menu.remove(func)
                removed += 1
            except Exception:
                pass
    return removed


def append_once(menu, func):
    """Append *func*, clearing any stale copy left behind by an earlier load.

    Safe to call repeatedly: the menu ends up with exactly one entry.
    """
    if menu is None or func is None:
        return False
    stale = remove_all(menu, getattr(func, "__name__", ""))
    if stale:
        print(f"[MapLink] cleared {stale} stale '{func.__name__}' entry(s) "
              f"from {getattr(menu, '__name__', menu)}")
    menu.append(func)
    return True


def detach(menu, func):
    """Remove *func* and any same-named leftovers. Never raises."""
    if menu is None or func is None:
        return
    try:
        menu.remove(func)
    except Exception:
        pass
    remove_all(menu, getattr(func, "__name__", ""))
