"""MapLink asset-link scope — where does a linked .blend actually live?

A MapLink project is a self-contained tree that a teammate syncs whole:

    <root>/<TYPE>/<Name>/<Name>.blend
    <root>/MaterialInstances/Library_MaterialInstances.blend

Every link inside it must be expressible as a ``//``-relative path that stays
inside the tree.  A link that leaves the tree resolves on the machine that made
it and nowhere else -- and, before v1.19, the sidecar recorded it anyway, so UE
imported a path no teammate had.  That is the defect this module closes.

The ladder
----------
    Conform   resolves under project_root AND is stored '//' relative
    Portable  resolves under project_root but is stored absolute (auto-fixable)
    Foreign   resolves OUTSIDE project_root -- the Shelf case, vendor-in candidate
    Broken    does not resolve at all

This is deliberately NOT ``EMapLinkTextureStatus`` verbatim, and the next reader
should not "fix" the inconsistency.  ``LocalOnly`` over there means *no relative
path is expressible* -- the file sits on a different drive, or outside every
MapLink tree, so there is nothing to write.  A foreign asset link is usually
perfectly expressible (``//../../../Shelf/SM/Foo/Foo.blend`` is a valid Blender
path) and is *still* wrong, because expressible is not the same as inside the
synced tree.  Different failure, different word.

Bare-importable: no addon module is imported at module scope, nothing here
requires a UE project, a MapLink project, Perforce or even a saved file.
"""

import os

import bpy
from bpy.app.handlers import persistent


CONFORM = 'Conform'
PORTABLE = 'Portable'
FOREIGN = 'Foreign'
BROKEN = 'Broken'

#: Statuses that are wrong *for a library MapLink owns*.  Not a gate on its
#: own -- see is_pipeline_library().
BLOCKING = (FOREIGN, BROKEN)

MANIFEST_NAME = "maplink_project.json"

# ---------------------------------------------------------------------------
# Scope — which libraries MapLink is actually responsible for
# ---------------------------------------------------------------------------
# A .blend links more than its MapLink dependencies.  Measured on a 185-asset
# production project: 25 of those assets linked a bundled Blender asset library,
# which lives in the Blender installation and therefore resolves outside every
# MapLink root, forever, by design.  Treating "resolves outside the root" as the
# gate on its own would have refused a sidecar for all 25 -- twelve times more
# false positives than the two files that genuinely linked another project.
#
# So the gate is scoped, not universal.  MapLink owns exactly two kinds of
# library:
#
#   * the assets UE imports          -- BP_ / LI_ / SM_ stems
#   * the project's material libraries -- Library_MaterialInstances / Library_Materials
#
# Those must live inside the project tree.  Everything else -- bundled
# essentials, a modelling utility library, a GIS or reference .blend the artist
# links for context -- is not part of the synced contract and never reaches UE,
# because `sidecar_analysis` filters by the same prefixes.  Such a link is
# reported (see unexported_libraries) and never blocks: refusing to save because
# an artist opened Blender's own asset library would be MapLink deciding how
# somebody models.

#: Stems whose owner is MapLink and whose home is the project tree.
PIPELINE_PREFIXES = ("BP_", "LI_", "SM_")
PIPELINE_STEMS = ("library_materialinstances", "library_materials")


def is_pipeline_library(path_or_stem):
    """True when MapLink owns this library and requires it inside the tree.

    Mirrors `sidecar_analysis._MAPLINK_PREFIXES` on purpose: the gate must
    cover exactly what the sidecar records, no more. A library the sidecar
    would skip cannot break an import, so it cannot justify refusing one.
    """
    # Separator-agnostic on purpose. os.path.basename only splits the host's
    # separator, and Blender hands back '//..\..\SM\Foo\Foo.blend' with
    # backslashes; on a POSIX host that whole string is one "basename" and
    # every MapLink asset silently stops being recognised as one — which turns
    # the gate off without a word. Cheap to be right for both.
    raw = str(path_or_stem or "").replace("\\", "/")
    stem = os.path.splitext(raw.rsplit("/", 1)[-1])[0]
    if stem.startswith(PIPELINE_PREFIXES):
        return True
    return stem.lower() in PIPELINE_STEMS

#: Cheap per-file cache so the sidebar can show a count without re-walking
#: bpy.data.libraries on every UI redraw.  Keyed by bpy.data.filepath.
_last_scan = {"filepath": None, "counts": {}, "entries": []}


def _log(msg):
    print(f"[MapLink LinkScope] {msg}", flush=True)


# ---------------------------------------------------------------------------
# Project root
# ---------------------------------------------------------------------------

def find_project_root(start_dir):
    """Nearest directory at or above *start_dir* holding maplink_project.json."""
    if not start_dir:
        return None
    d = os.path.abspath(start_dir)
    while True:
        if os.path.isfile(os.path.join(d, MANIFEST_NAME)):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            return None
        d = parent


def current_project_root():
    """The root owning the open .blend, or None (unsaved / standalone)."""
    path = bpy.data.filepath
    if not path:
        return None
    return find_project_root(os.path.dirname(bpy.path.abspath(path)))


def is_under(path, root):
    """True when *path* resolves inside *root* (case-insensitive on Windows)."""
    if not path or not root:
        return False
    try:
        p = os.path.normcase(os.path.abspath(path))
        r = os.path.normcase(os.path.abspath(root))
    except Exception:                                   # noqa: BLE001
        return False
    return p == r or p.startswith(r + os.sep)


# ---------------------------------------------------------------------------
# The ladder
# ---------------------------------------------------------------------------

def classify_library(lib_filepath, project_root):
    """Classify one library path.  -> 'Conform' | 'Portable' | 'Foreign' | 'Broken'.

    *lib_filepath* is the RAW stored value (``lib.filepath``), because whether
    it starts with ``//`` is half the answer.  *project_root* may be None, which
    means "no project owns the open file" -- then anything that resolves is
    Foreign, because there is no tree for it to be inside of.
    """
    if not lib_filepath:
        return BROKEN

    try:
        abs_path = bpy.path.abspath(lib_filepath)
    except Exception:                                   # noqa: BLE001
        return BROKEN

    if not abs_path or not os.path.isfile(abs_path):
        return BROKEN

    if not is_under(abs_path, project_root):
        return FOREIGN

    return CONFORM if str(lib_filepath).startswith("//") else PORTABLE


def scan(project_root=None):
    """Classify every library in the open file.

    -> list of dicts: ``{name, raw, abspath, status}``, in bpy.data order.
    """
    if project_root is None:
        project_root = current_project_root()

    entries = []
    for lib in bpy.data.libraries:
        raw = lib.filepath
        try:
            abs_path = bpy.path.abspath(raw)
        except Exception:                               # noqa: BLE001
            abs_path = ""
        # Blender records how a library was reached. A link the artist made is
        # `parent is None` ("<direct>" in the export logs); one dragged in by
        # another library is indirect, and is that library's problem, not this
        # file's. See blocking_entries().
        parent = getattr(lib, "parent", None)
        entries.append({
            "name": lib.name,
            "raw": raw,
            "abspath": abs_path,
            "status": classify_library(raw, project_root),
            "pipeline": is_pipeline_library(abs_path or raw),
            "direct": parent is None,
            "parent": getattr(parent, "name", "") if parent is not None else "",
        })
    return entries


def counts(entries=None, project_root=None):
    """{status: n} over *entries* (scanned fresh when omitted)."""
    if entries is None:
        entries = scan(project_root)
    out = {CONFORM: 0, PORTABLE: 0, FOREIGN: 0, BROKEN: 0}
    for e in entries:
        out[e["status"]] = out.get(e["status"], 0) + 1
    return out


def blocking_entries(project_root=None, entries=None):
    """What stops a sidecar: a DIRECT link to a library MapLink owns that is
    Foreign or Broken.

    Scoped twice, and both scopes are load-bearing:

    * **Ownership** — a Foreign bundled Blender asset library is not a defect;
      a Foreign `SM_Fountain` is.
    * **Directness** — an LI that links an unprefixed .blend from another
      project drags in THAT project's `Library_MaterialInstances` and, behind
      it, a third `Library_Materials`.  Those two are pipeline-named and
      outside the tree, but nobody linked them from this file and unlinking
      them is not something this file can do.  The one actionable link is the
      direct one, and it is the only one worth refusing a save over.  Blocking
      on the collateral would mean an error the artist cannot clear.
    """
    if entries is None:
        entries = scan(project_root)
    return [e for e in entries
            if e["pipeline"] and e["direct"] and e["status"] in BLOCKING]


def unexported_libraries(project_root=None, entries=None):
    """Outside-the-tree links MapLink does NOT own.  Reported, never blocking.

    These are already dropped by the sidecar's prefix filter, so anything the
    scene instances from one is invisible to UE: the artist sees it in Blender
    and it is simply not there after import.  That is worth saying out loud —
    it is the quiet sibling of the foreign-link defect, and the reason it is a
    warning rather than a refusal is that the content never travelled in the
    first place, so refusing the save would not protect anyone.
    """
    if entries is None:
        entries = scan(project_root)
    blocked = {id(e) for e in blocking_entries(entries=entries)}
    return [e for e in entries
            if id(e) not in blocked and e["status"] in BLOCKING]


def cached_counts():
    """Counts from the last load_post/save scan, for panel draw code.

    Draw functions run many times a second; re-walking bpy.data.libraries and
    stat()ing every path from a panel would be a per-frame filesystem hit.
    """
    if _last_scan["filepath"] != bpy.data.filepath:
        refresh_cache()
    return dict(_last_scan["counts"])


def refresh_cache(project_root=None):
    entries = scan(project_root)
    _last_scan["filepath"] = bpy.data.filepath
    _last_scan["entries"] = entries
    _last_scan["counts"] = counts(entries)
    return entries


# ---------------------------------------------------------------------------
# Portable -> Conform
# ---------------------------------------------------------------------------

def make_relative(project_root=None):
    """Rewrite in-root ABSOLUTE library paths to ``//``-relative.  -> n fixed.

    Blender has a "Relative Paths" user preference that governs what a *new*
    link is stored as.  It is per-user and it is off on some machines, so one
    artist can bake ``C:\\MapLink\\Shelf\\SM\\Foo\\Foo.blend`` into a .blend and
    the asset is unportable the moment anyone else syncs it.  MapLink does not
    ask; anything that resolves inside the project is stored relative.

    Only Portable entries are touched.  A Foreign link is NOT made relative --
    ``//../../../Shelf/...`` is a valid path and writing it would make the file
    look fixed while still pointing outside the tree.
    """
    if project_root is None:
        project_root = current_project_root()
    if not project_root or not bpy.data.filepath:
        return 0

    fixed = 0
    for lib in bpy.data.libraries:
        if classify_library(lib.filepath, project_root) != PORTABLE:
            continue
        if not is_pipeline_library(bpy.path.abspath(lib.filepath)):
            # An in-root library MapLink does not own (a reference .blend, a
            # GIS export) is the artist's business. Rewriting their paths
            # because the file happens to sit under the root is scope creep
            # with a save_pre handler behind it.
            continue
        abs_path = bpy.path.abspath(lib.filepath)
        try:
            rel = bpy.path.relpath(abs_path)
        except Exception as exc:                        # noqa: BLE001
            _log(f"could not relativise '{abs_path}': {exc}")
            continue
        if rel and rel != lib.filepath:
            lib.filepath = rel
            fixed += 1
            _log(f"{lib.name}: absolute -> {rel}")
    return fixed


def describe(entries):
    """One human line per blocking entry, for dialogs and error reports."""
    # Deduped by resolved path: one .blend reached twice (linked directly and
    # again through a parent, or two datablocks of the same file) is one
    # problem, and listing it twice reads as two — the user counts the lines.
    lines, seen = [], set()
    for e in entries:
        key = os.path.normcase(e["abspath"] or e["raw"])
        if key in seen:
            continue
        seen.add(key)
        stem = os.path.splitext(os.path.basename(e["abspath"] or e["raw"]))[0]
        if e["status"] == BROKEN:
            lines.append(f"{stem or e['name']} — does not resolve ({e['raw']})")
        else:
            lines.append(f"{stem or e['name']} — {e['abspath']}")
    return lines


# ---------------------------------------------------------------------------
# Handlers
# ---------------------------------------------------------------------------

@persistent
def _on_load_post(_dummy):
    """Classify on open so an already-damaged file announces itself."""
    try:
        if not bpy.data.filepath:
            _last_scan.update({"filepath": "", "counts": {}, "entries": []})
            return
        entries = refresh_cache()
        blocking = blocking_entries(entries=entries)
        if blocking:
            _log(f"{len(blocking)} MapLink link(s) leave this project. "
                 f"No sidecar will be written until they are resolved:")
            for line in describe(blocking):
                _log(f"    {line}")
        for e in unexported_libraries(entries=entries):
            _log(f"NOTE: '{e['name']}' is linked from outside this project and "
                 f"is not a MapLink asset — anything instanced from it is not "
                 f"exported to Unreal. ({e['abspath'] or e['raw']})")
    except Exception as exc:                            # noqa: BLE001
        _log(f"load_post error: {exc}")


@persistent
def _on_save_pre(_dummy):
    """Portable -> Conform, silently, on every save.

    save_pre rather than save_post: the rewrite has to be in the bytes being
    written, not applied to memory after the file has already landed on disk.
    """
    try:
        n = make_relative()
        if n:
            _log(f"{n} library path(s) rewritten relative before save.")
    except Exception as exc:                            # noqa: BLE001
        _log(f"save_pre error: {exc}")


@persistent
def _on_save_post(_dummy):
    try:
        refresh_cache()
    except Exception:                                   # noqa: BLE001
        pass


# ---------------------------------------------------------------------------
# Panel helper
# ---------------------------------------------------------------------------

def draw_badge(layout):
    """Link health, or nothing when there is nothing to say.

    Two separate statements, because they are two separate problems and
    collapsing them into one count is what would send someone hunting for a
    defect in a link that is working as intended.
    """
    if _last_scan["filepath"] != bpy.data.filepath:
        try:
            refresh_cache()
        except Exception:                               # noqa: BLE001
            return
    entries = _last_scan["entries"]
    if not entries:
        return

    blocking = blocking_entries(entries=entries)
    unexported = unexported_libraries(entries=entries)

    if blocking:
        n_broken = sum(1 for e in blocking if e["status"] == BROKEN)
        n_foreign = len(blocking) - n_broken
        box = layout.box()
        box.alert = True
        if n_broken:
            box.label(text=f"{n_broken} MapLink link(s) do not resolve",
                      icon='ERROR')
        if n_foreign:
            box.label(text=f"{n_foreign} MapLink link(s) outside this project",
                      icon='ERROR')
        box.label(text="No sidecar is written while this is true.")
        box.operator("maplink.vendor_asset", text="Copy into project…",
                     icon='IMPORT')

    if unexported:
        box = layout.box()
        box.label(text=f"{len(unexported)} non-MapLink link(s) from outside",
                  icon='INFO')
        for e in unexported[:3]:
            box.label(text=f"    {e['name']}")
        box.label(text="Not exported to Unreal. Saving is unaffected.")


# ---------------------------------------------------------------------------
# Registration
# ---------------------------------------------------------------------------

def register():
    if _on_load_post not in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.append(_on_load_post)
    if _on_save_pre not in bpy.app.handlers.save_pre:
        bpy.app.handlers.save_pre.append(_on_save_pre)
    if _on_save_post not in bpy.app.handlers.save_post:
        bpy.app.handlers.save_post.append(_on_save_post)


def unregister():
    for handlers, fn in ((bpy.app.handlers.load_post, _on_load_post),
                         (bpy.app.handlers.save_pre, _on_save_pre),
                         (bpy.app.handlers.save_post, _on_save_post)):
        if fn in handlers:
            handlers.remove(fn)
    _last_scan.update({"filepath": None, "counts": {}, "entries": []})
