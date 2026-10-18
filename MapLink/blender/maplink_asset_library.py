# maplink_asset_library.py — MapLink Blender asset-library registration.
#
# Registers the active MapLink project as Blender Asset Libraries with the
# correct import modes, so LI_/SM_/BP_ assets browse-and-drag as LINK and
# presets as APPEND. This is an ADDON module (it mutates user preferences, which
# a --factory-startup headless process cannot persist) — wire register()/
# unregister() from your addon __init__.
#
# Behaviour:
#   * A load_post handler fires whenever a .blend opens. It walks up from the
#     file to the nearest maplink_project.json and ensures two library entries:
#       - "MapLink: <Setup>"          -> <root>        import_method = LINK
#       - "MapLink: <Setup> Presets"  -> <root>/preset import_method = APPEND
#     (in-session only; it persists prefs once, the first time it adds one).
#   * An operator (maplink.register_asset_library) does the same on demand and
#     always saves user prefs — expose it from a panel/menu.
#
# Caveat: the root LINK library scans recursively, so preset assets also appear
# under it; the dedicated Presets library is the more-specific APPEND entry.
# Catalogs (blender_assets.cats.txt at <root>) keep the two visually separated.

import json
import os

import bpy
from bpy.app.handlers import persistent

#: Every library this addon registers starts with this.
_MANAGED_PREFIX = "MapLink_"

#: (name suffix, subfolder, import method) — the ONE place this set is listed.
#:
#: It used to be spelled out separately in `library_names_for`,
#: `ensure_project_libraries`, `ensure_shelf_libraries` and
#: `_remove_stale_project_libraries`.  Adding a fifth entry to four of the five
#: would have left the new one looking stale to the purge that missed it, and
#: it would have been deregistered on the next file open — silently, and only
#: for people who had already used it.
#:
#: `MI` is LINK and that is the whole point of it.  A material instance is
#: authored in Unreal and owned by Unreal; linking makes the datablock
#: read-only in the consuming file, so "UE-authoritative materials" stops being
#: a rule artists have to remember and becomes something Blender enforces.
#: APPEND would copy the material in and the next append would make `.001`,
#: which is exactly the resource duplication this drop exists to prevent.
LIBRARY_SPECS = (
    ("LI",      "LI",                "LINK"),
    ("SM",      "SM",                "LINK"),
    ("BP",      "BP",                "LINK"),
    ("Presets", "preset",            "APPEND"),
    ("MI",      "MaterialInstances", "LINK"),
)


def _log(msg):
    print(f"[MapLink AssetLib] {msg}", flush=True)


def _prefs():
    """The addon preferences, or None (headless, or addon not enabled)."""
    try:
        addons = bpy.context.preferences.addons
        ad = addons.get(__package__) or addons.get(__name__.split('.')[0])
        return ad.preferences if ad is not None else None
    except Exception:                              # noqa: BLE001
        return None


def shelf_root():
    """The configured Shelf root, or "".

    The Shelf is an ordinary MapLink project; Shelf-ness is this preference and
    nothing else.  Read defensively — a headless run has no preferences.
    """
    p = _prefs()
    root = (getattr(p, "shelf_root", "") or "").strip() if p else ""
    if not root:
        return ""
    root = bpy.path.abspath(root)
    return root if os.path.isdir(root) else ""


def library_names_for(root):
    """The four canonical library names for the project rooted at *root*."""
    manifest = os.path.join(root, "maplink_project.json")
    data = _read_manifest(manifest) if os.path.isfile(manifest) else None
    setup = str((data or {}).get("project_name")
                or os.path.basename(os.path.normpath(root)) or "MapLink")
    return tuple(f"MapLink_{setup}_{suffix}" for suffix, _sub, _m in LIBRARY_SPECS)


# ---------------------------------------------------------------------------
# Manifest discovery
# ---------------------------------------------------------------------------
def _find_manifest(start_dir):
    """Walk up from start_dir to the nearest maplink_project.json."""
    if not start_dir:
        return None
    d = os.path.abspath(start_dir)
    while True:
        cand = os.path.join(d, "maplink_project.json")
        if os.path.isfile(cand):
            return cand
        parent = os.path.dirname(d)
        if parent == d:           # hit the drive/filesystem root
            return None
        d = parent


def _read_manifest(manifest_path):
    try:
        with open(manifest_path, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception as exc:                       # noqa: BLE001
        _log(f"could not read manifest '{manifest_path}': {exc}")
        return None


# ---------------------------------------------------------------------------
# Asset-library registration (defensive across Blender versions)
# ---------------------------------------------------------------------------
def _same_dir(a, b):
    try:
        return (os.path.normcase(os.path.abspath(a))
                == os.path.normcase(os.path.abspath(b)))
    except Exception:                              # noqa: BLE001
        return False


def _find_library(libs, name, directory):
    for lib in libs:
        if lib.name == name:
            return lib
        try:
            if _same_dir(lib.path, directory):
                return lib
        except Exception:                          # noqa: BLE001
            pass
    return None


def _add_library(libs, name, directory):
    """Add a library entry, preferring the RNA .new(), else the operator."""
    # Preferred: direct collection API (present on most 3.x+ builds).
    try:
        return libs.new(name=name, directory=directory)
    except (AttributeError, TypeError):
        pass
    # Fallback: the add operator, then grab the freshly-appended entry.
    try:
        before = len(libs)
        bpy.ops.preferences.asset_library_add(directory=directory)
        if len(libs) > before:
            return libs[-1]
    except Exception as exc:                       # noqa: BLE001
        _log(f"asset_library_add failed for '{name}': {exc}")
    return None


def _remove_library(libs, lib):
    """Remove a UserAssetLibrary entry. Best-effort across API variants.
    Pass a freshly-resolved reference (collection mutation invalidates others)."""
    name = getattr(lib, "name", "?")
    try:
        libs.remove(lib)
        return True
    except Exception:                              # noqa: BLE001
        pass
    try:
        for i, other in enumerate(libs):
            if other == lib:
                bpy.ops.preferences.asset_library_remove(index=i)
                return True
    except Exception as exc:                       # noqa: BLE001
        _log(f"WARN: could not remove library '{name}': {exc}")
    return False


def _ensure_library(name, directory, import_method):
    """Create or refresh one library entry. Returns (lib, changed_bool)."""
    if not os.path.isdir(directory):
        return None, False

    prefs = bpy.context.preferences.filepaths
    libs = prefs.asset_libraries

    lib = _find_library(libs, name, directory)
    changed = False
    if lib is None:
        lib = _add_library(libs, name, directory)
        if lib is None:
            return None, False
        changed = True

    # Set/refresh attributes defensively, only writing (and flagging change)
    # when a value actually differs — so we don't churn prefs on every open.
    def _apply(attr, value, equal=None):
        nonlocal changed
        if not hasattr(lib, attr):
            return
        try:
            cur = getattr(lib, attr)
            same = equal(cur, value) if equal else (cur == value)
            if not same:
                setattr(lib, attr, value)
                changed = True
        except Exception:                          # noqa: BLE001
            pass

    _apply("name", name)
    _apply("path", directory, equal=_same_dir)
    _apply("import_method", import_method)
    _apply("use_relative_path", True)
    return lib, changed


def _remove_stale_project_libraries(root, expected_names):
    """Remove legacy/stale MapLink asset libraries belonging to THIS project.

    The previous addon version registered a single library at the project ROOT
    with LINK; because libraries scan recursively, that entry re-indexes every
    asset under root and links it — so every asset shows up an extra time and
    presets resolve as LINK. We delete any library whose path is this project's
    root or one of its type subfolders but whose name is NOT one of the current
    canonical names. Other projects' libraries (different paths) are untouched.

    Returns the number of entries removed.
    """
    prefs = bpy.context.preferences.filepaths
    libs = prefs.asset_libraries

    root_abs = os.path.normcase(os.path.abspath(root))
    owned = {root_abs}
    for _suffix, sub, _m in LIBRARY_SPECS:
        owned.add(os.path.normcase(os.path.abspath(os.path.join(root, sub))))

    victims = []
    for i, lib in enumerate(libs):
        try:
            p = os.path.normcase(os.path.abspath(lib.path))
        except Exception:                              # noqa: BLE001
            continue
        if p in owned and lib.name not in expected_names:
            victims.append((i, lib.name))

    removed = 0
    for i, name in sorted(victims, reverse=True):
        ok = False
        try:
            bpy.ops.preferences.asset_library_remove(index=i)
            ok = True
        except Exception:                              # noqa: BLE001
            try:
                libs.remove(libs[i])
                ok = True
            except Exception as exc:                   # noqa: BLE001
                _log(f"WARN: could not remove stale library '{name}': {exc}")
        if ok:
            removed += 1
            _log(f"removed stale library '{name}'")
    return removed


def _remove_library_safe(libs, lib):
    """Remove one entry, re-resolving via operator index if direct remove fails."""
    try:
        libs.remove(lib)
        return True
    except Exception:                              # noqa: BLE001
        pass
    try:
        for i, other in enumerate(libs):
            if other == lib:
                bpy.ops.preferences.asset_library_remove(index=i)
                return True
    except Exception:                              # noqa: BLE001
        pass
    return False


def _dedupe_and_legacy_by_name(setup, expected_names):
    """Complement the path-based prune by NAME: remove duplicate entries among
    the expected names (keep one each), stale 'MapLink_<setup>_*' entries no
    longer in the desired set, and legacy 'MapLink: <Setup>[ Presets]' names the
    path pass may have missed. Returns the count removed.

    This is what kills the 'shows up 3 times' case: two identical
    MapLink_<setup>_LI entries both have an expected name, so the path pass keeps
    both — only a name-level dedup removes the extra.
    """
    prefs = bpy.context.preferences.filepaths
    libs = prefs.asset_libraries

    legacy_names = {f"MapLink: {setup}", f"MapLink: {setup} Presets"}
    managed_prefix = f"MapLink_{setup}_"

    to_remove = []                                 # names, with multiplicity
    seen = set()
    for lib in libs:
        name = lib.name
        if name in legacy_names:
            to_remove.append(name)
        elif name.startswith(managed_prefix):
            if name not in expected_names:
                to_remove.append(name)             # stale managed entry
            elif name in seen:
                to_remove.append(name)             # duplicate beyond the first
            else:
                seen.add(name)

    removed = 0
    for name in to_remove:
        target = None
        for lib in libs:                           # fresh reference each time
            if lib.name == name:
                target = lib
                break
        if target is not None and _remove_library_safe(libs, target):
            removed += 1
            _log(f"removed duplicate/legacy library '{name}'")
    return removed


def _purge_other_projects(keep_names):
    """Deregister every MapLink_* library that is not in *keep_names*.

    A second, WIDER pass than _remove_stale_project_libraries, and deliberately
    not merged with it: that one answers "which of THIS root's entries are
    obsolete", this one answers "which entries belong to other roots entirely".
    Merging them would mean one function that has to be told which question it
    is being asked, which is two functions wearing a coat.

    The defect being fixed (v1.19, Finding 1): ensure_project_libraries()
    registered four entries per project and purged only the root it was called
    for.  Open project A, then project B, and prefs hold eight MapLink
    libraries — all shown in the Asset Browser, distinguished only by a name
    most artists never read.  Dragging from the wrong one produced a
    cross-root link, which is exactly what the sidecar gate now refuses.

    Returns the number of entries removed.
    """
    if bpy.app.background:
        return 0
    prefs = _prefs()
    if prefs is not None and getattr(prefs, "show_other_maplink_projects", False):
        return 0

    try:
        libs = bpy.context.preferences.filepaths.asset_libraries
    except Exception:                                  # noqa: BLE001
        return 0

    keep = set(keep_names)
    victims = [lib.name for lib in libs
               if lib.name.startswith(_MANAGED_PREFIX) and lib.name not in keep]

    removed = 0
    for name in victims:
        target = next((lib for lib in libs if lib.name == name), None)
        if target is not None and _remove_library_safe(libs, target):
            removed += 1
            _log(f"deregistered other-project library '{name}'")
    return removed


def ensure_shelf_libraries():
    """Register the Shelf's four entries.  -> (names, changed).

    They stay registered whatever file is open — that is what makes the Shelf a
    Shelf.  Project libraries come and go with the open file.
    """
    root = shelf_root()
    if not root:
        return (), False
    names = library_names_for(root)
    changed = False
    for name, (_suffix, sub, method) in zip(names, LIBRARY_SPECS):
        directory = os.path.join(root, sub)
        if not os.path.isdir(directory):
            continue
        _lib, created = _ensure_library(name, directory, method)
        changed = changed or created
    return names, changed


def visible_library_names(project_root=None):
    """Exactly what the Asset Browser should show: this project + the Shelf.

    If the open project IS the Shelf, that is one set of four, not two.
    If no Shelf is configured, it is the project's four.
    If no project is open, it is the Shelf's four (or nothing).
    """
    names = set()
    if project_root:
        names.update(library_names_for(project_root))
    shelf = shelf_root()
    if shelf:
        names.update(library_names_for(shelf))
    return names


def ensure_project_libraries(manifest_path, save_prefs=False):
    """Ensure the per-type asset libraries for the project that owns
    manifest_path. They point at the type subfolders (not the project root) so
    they never overlap — that overlap is what made preset assets resolve as
    LINK instead of APPEND.

    Names follow the setup: MapLink_<Setup>_LI / _SM / _BP / _Presets / _MI.
    See LIBRARY_SPECS for what each one links or appends, and why.
    """
    data = _read_manifest(manifest_path)
    if not data:
        return False

    root = os.path.dirname(os.path.abspath(manifest_path))
    setup = str(data.get("project_name") or os.path.basename(root) or "MapLink")

    specs = LIBRARY_SPECS

    # First, purge any stale MapLink library owned by this project that is NOT
    # one of the current canonical entries — in particular the previous addon's
    # single LINK library at the project ROOT, which recursively re-indexed
    # everything (doubles, presets resolving as LINK).
    expected = tuple(f"MapLink_{setup}_{suffix}" for suffix, _s, _m in specs)
    removed = _remove_stale_project_libraries(root, expected)
    removed += _dedupe_and_legacy_by_name(setup, set(expected))

    any_created = False
    for suffix, subdir, method in specs:
        directory = os.path.join(root, subdir)
        if not os.path.isdir(directory):
            continue
        _lib, created = _ensure_library(
            f"MapLink_{setup}_{suffix}", directory, method)
        any_created = any_created or created

    # v1.19 — the Shelf is always available, and everything else goes away.
    shelf_names, shelf_created = ensure_shelf_libraries()
    any_created = any_created or shelf_created
    removed += _purge_other_projects(set(expected) | set(shelf_names))

    # Persist when asked, or once when we actually changed anything (so we are
    # not rewriting prefs on every single file open).
    if save_prefs or any_created or removed:
        try:
            bpy.ops.wm.save_userpref()
        except Exception as exc:                   # noqa: BLE001
            _log(f"save_userpref failed (libraries still active this session): {exc}")

    _log(f"libraries ensured for '{setup}' "
         f"(root={root}, new={any_created}, stale_removed={removed})")
    return True


def ensure_shelf_only(save_prefs=False):
    """No project open — show the Shelf and nothing else.

    The standalone case: a Blender artist with no MapLink project above the
    open file still gets their Shelf, and does not get a stale list of every
    project they have ever opened.
    """
    names, created = ensure_shelf_libraries()
    removed = _purge_other_projects(set(names))
    if save_prefs or created or removed:
        try:
            bpy.ops.wm.save_userpref()
        except Exception as exc:                   # noqa: BLE001
            _log(f"save_userpref failed: {exc}")
    return bool(names)


# ---------------------------------------------------------------------------
# Asset Browser import method (the per-browser "Import Method" dropdown)
# ---------------------------------------------------------------------------
def set_asset_browser_import_method(method="FOLLOW_PREFS"):
    """Force every open Asset Browser's Import Method to `method` so the
    per-library LINK/APPEND settings are actually honoured (a browser stuck on
    'Link' force-links everything, including presets). This is per-browser UI
    state Blender keeps re-defaulting, so we re-apply it on each project open.
    Best-effort and defensive across Blender versions (attr is 'import_method'
    on some builds, 'import_type' on others). Returns count changed."""
    changed = 0
    try:
        screens = list(bpy.data.screens)
    except Exception:                              # noqa: BLE001
        return 0
    for screen in screens:
        for area in getattr(screen, "areas", []):
            if getattr(area, "type", None) != "FILE_BROWSER":
                continue
            for space in getattr(area, "spaces", []):
                if getattr(space, "type", None) != "FILE_BROWSER":
                    continue
                if getattr(space, "browse_mode", None) != "ASSETS":
                    continue
                params = getattr(space, "params", None)
                if params is None:
                    continue
                for attr in ("import_method", "import_type"):
                    if not hasattr(params, attr):
                        continue
                    try:
                        if getattr(params, attr) != method:
                            setattr(params, attr, method)
                            changed += 1
                    except Exception as exc:       # noqa: BLE001
                        _log(f"WARN: could not set asset browser {attr}: {exc}")
                    break
    if changed:
        _log(f"set asset-browser import method -> {method} ({changed} browser(s))")
    return changed


# ---------------------------------------------------------------------------
# load_post handler
# ---------------------------------------------------------------------------
@persistent
def _on_load_post(_dummy):
    try:
        # v1.19 — never touch preferences from a headless run.  Asset libraries
        # are an Asset Browser feature and have no meaning without a UI, while
        # save_userpref() in a --factory-startup worker (force_save_all enables
        # the addon in one) would write factory defaults over the artist's real
        # preferences.  The wider purge this handler now performs made that
        # latent hazard worth closing.
        if bpy.app.background:
            return

        # v1.20 — Blender hands load_post a RESTRICTED `bpy.data` during some
        # load phases (startup file, factory startup, and the pass that runs
        # before the artist's file is really open).  Reading .filepath off it
        # raises AttributeError on `_RestrictData`, which this handler caught
        # and printed as an error on every single launch:
        #     [MapLink AssetLib] load_post error: '_RestrictData' object has
        #     no attribute 'filepath'
        # There is no file to scope to at that point and the real load_post for
        # the artist's file follows immediately, so this is a skip, not a
        # failure, and it should not look like one in the console.
        try:
            path = bpy.data.filepath
        except AttributeError:
            return

        manifest = _find_manifest(os.path.dirname(path)) if path else None
        if manifest:
            ensure_project_libraries(manifest, save_prefs=False)
        else:
            # No project above this file (or no file at all) — the Shelf is
            # still yours, and other projects still go away.
            ensure_shelf_only(save_prefs=False)
        set_asset_browser_import_method("FOLLOW_PREFS")
    except Exception as exc:                       # noqa: BLE001
        _log(f"load_post error: {exc}")


# ---------------------------------------------------------------------------
# Manual operator
# ---------------------------------------------------------------------------
class MAPLINK_OT_register_asset_library(bpy.types.Operator):
    """Register / refresh the MapLink asset libraries for the current project"""
    bl_idname = "maplink.register_asset_library"
    bl_label = "Register MapLink Asset Library"
    bl_options = {"REGISTER"}

    def execute(self, context):
        path = bpy.data.filepath
        manifest = _find_manifest(os.path.dirname(path)) if path else None
        if not manifest:
            self.report({"WARNING"},
                        "No maplink_project.json found above the current file. "
                        "Open a .blend inside a MapLink project first.")
            return {"CANCELLED"}
        if ensure_project_libraries(manifest, save_prefs=True):
            self.report({"INFO"}, "MapLink asset libraries registered.")
            return {"FINISHED"}
        self.report({"ERROR"}, "Failed to register MapLink asset libraries.")
        return {"CANCELLED"}


# ---------------------------------------------------------------------------
# Addon (un)registration — call these from your addon __init__
# ---------------------------------------------------------------------------
def register():
    try:
        bpy.utils.register_class(MAPLINK_OT_register_asset_library)
    except Exception as exc:                       # noqa: BLE001
        _log(f"operator register failed: {exc}")
    if _on_load_post not in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.append(_on_load_post)
    # Apply to any Asset Browser already open in the current session, and
    # settle the visible set now — enabling the addon with a project file
    # already open is the common case after an addon update.
    set_asset_browser_import_method("FOLLOW_PREFS")
    try:
        _on_load_post(None)
    except Exception as exc:                       # noqa: BLE001
        _log(f"initial scoping skipped: {exc}")


def unregister():
    if _on_load_post in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.remove(_on_load_post)
    try:
        bpy.utils.unregister_class(MAPLINK_OT_register_asset_library)
    except Exception:                              # noqa: BLE001
        pass
