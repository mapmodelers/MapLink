"""MapLink asset catalogs — one catalog per project, and everything in it.

Each MapLink project owns exactly one Blender asset catalog, named
``MapLink_<Setup>``, and every asset the project contains belongs to it.  The
catalog definition (`blender_assets.cats.txt`) is written into **each per-type
library root** — `LI/`, `SM/`, `BP/`, `preset/` — plus the project root,
because an asset library rooted at `<root>/LI` cannot see a definition file at
`<root>`.  All five files define the same catalog with the same deterministic
UUID, so the four libraries merge into one node in the All Libraries view.

The v1.19 defect
------------------
Writing the definition files and cataloguing the seeded templates was
`maplink_project_setup.py`'s job, and it did it.  Nothing else did:

* `quick_create_worker.py` called `asset_mark()` and added a type tag, and
  never set `catalog_id`.  Every asset made with Create SM/LI from selection
  landed in **Unassigned**.
* `maplink_create_bp_standin.py` did the same for BP stand-ins.
* `thumbnail_worker.py` was the sharp one: it `asset_clear()`s and re-marks to
  reset preview state, preserving *tags* but not the catalog. So **Rebuild All
  Thumbnails actively un-catalogued assets that were correct**, which is why a
  project can drift into Unassigned over time rather than starting there.

So this module is the single place that answers "which catalog does this asset
belong to", and the three workers above now ask it.

Resolution order is **cats.txt first, computed second.**  The file on disk is
the source of truth: a project scaffolded by any past version keeps working
even if the constant below were ever to drift, because the UUID that project's
libraries already resolve is the one sitting in its own definition file.

Bare-importable: no package-relative imports, no addon dependency, so the
headless workers can pull it in the same way they reach `batch_sidecar`.
"""

import os
import re
import stat
import uuid

MANIFEST_NAME = "maplink_project.json"
CATS_NAME = "blender_assets.cats.txt"
NIL_UUID = "00000000-0000-0000-0000-000000000000"

#: Must match `maplink_project_setup._CATALOG_NS`.  That script is deliberately
#: self-contained (it runs under --factory-startup as UE's scaffolder) and
#: keeps its own copy; `uuid_for_root` reads the produced file rather than
#: trusting either constant, so a drift here cannot mis-file an asset.
_CATALOG_NS = uuid.UUID("6d61706c-696e-6b00-0000-6361746c6f67")

#: The project catalog's children, keyed by the folder that owns each one.
#: THE FOLDER IS THE FACT.  A datablock's `LI_`/`SM_`/`BP_` prefix is a naming
#: convention a rename can break; the directory a .blend sits in is the same
#: thing the asset library is rooted at, so the two cannot disagree.
#:
#: `MaterialInstances/` -> `Materials` is a deliberate mismatch.  The folder
#: name is a layout contract written into `maplink_project.json` and read by
#: the UE side; the catalog label is read at a glance in a narrow tree.
#: Changing one should not force the other.
CHILDREN = (
    ("LI", "LI"),
    ("SM", "SM"),
    ("BP", "BP"),
    ("preset", "Presets"),
    ("MaterialInstances", "Materials"),
)
_CHILD_BY_SUBDIR = dict(CHILDREN)

#: Where a definition file has to exist for a library rooted there to see it.
#: "." is the project root: no library is rooted there, but it carries the
#: parent line so a human opening the folder can see what the catalog is called.
LIBRARY_SUBDIRS = tuple(sub for sub, _c in CHILDREN) + (".",)


def _log(msg):
    print(f"[MapLink Catalog] {msg}", flush=True)


# ---------------------------------------------------------------------------
# Project resolution
# ---------------------------------------------------------------------------

def find_project_root(start_dir):
    """Nearest directory at or above *start_dir* holding the manifest, or ''."""
    d = os.path.abspath(start_dir or "")
    if not d or not os.path.isdir(d):
        return ""
    for _ in range(64):
        if os.path.isfile(os.path.join(d, MANIFEST_NAME)):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            return ""
        d = parent
    return ""


def root_for_blend(blend_path):
    return find_project_root(os.path.dirname(os.path.abspath(blend_path or "")))


def catalog_path_for_root(root):
    """``MapLink_<Setup>`` — the catalog's path in the tree view."""
    setup = os.path.basename(os.path.normpath(root))
    manifest = os.path.join(root, MANIFEST_NAME)
    if os.path.isfile(manifest):
        try:
            import json
            with open(manifest, "r", encoding="utf-8") as f:
                setup = str(json.load(f).get("project_name") or setup)
        except Exception as exc:                        # noqa: BLE001
            _log(f"could not read {manifest}: {exc}")
    return f"MapLink_{setup}"


def computed_uuid(catalog_path):
    """The deterministic UUID for a catalog path.  Fallback, not first choice."""
    return str(uuid.uuid5(_CATALOG_NS, catalog_path))


def child_name_for_subdir(subdir):
    """``"LI"`` -> ``"LI"``, ``"MaterialInstances"`` -> ``"Materials"``, else ""."""
    return _CHILD_BY_SUBDIR.get(str(subdir or "").strip("/\\"), "")


def catalog_path_for(root, subdir=None):
    """The catalog path an asset in *subdir* belongs to.

    No subdir, or one this project has no child for, resolves to the PARENT —
    so a .blend somewhere unexpected is still filed under the project rather
    than dropped into Unassigned, which is the failure this whole module
    exists to prevent.
    """
    parent = catalog_path_for_root(root)
    child = child_name_for_subdir(subdir) if subdir else ""
    return f"{parent}/{child}" if child else parent


def subdir_for_blend(blend_path, root=None):
    """Which top-level project folder holds *blend_path*.  "" when none does."""
    if root is None:
        root = root_for_blend(blend_path)
    if not root or not blend_path:
        return ""
    try:
        rel = os.path.relpath(os.path.abspath(blend_path), os.path.abspath(root))
    except ValueError:                                  # different drive
        return ""
    if rel.startswith(os.pardir):
        return ""
    first = rel.replace("\\", "/").split("/")[0]
    return first if first in _CHILD_BY_SUBDIR else ""


def uuid_for_catalog_path(root, catalog_path):
    """UUID for one catalog path in this project.  cats.txt first, computed second.

    Same rule as `uuid_for_root`, for the same reason: the definition file on
    disk holds the UUID the assets already point at, so a drift in the
    namespace constant cannot mis-file anything that already exists.
    """
    if not root or not catalog_path:
        return ""
    for sub in LIBRARY_SUBDIRS:
        d = root if sub == "." else os.path.join(root, sub)
        for cid, cpath, _name in _read_cats(d):
            if cpath == catalog_path:
                return cid
    return computed_uuid(catalog_path)


def uuid_for(root, subdir=None):
    """The catalog UUID for assets in *subdir* of the project at *root*."""
    return uuid_for_catalog_path(root, catalog_path_for(root, subdir))


def _read_cats(dir_path):
    """[(uuid, path, name)] from a definition file, or []."""
    cats = os.path.join(dir_path, CATS_NAME)
    if not os.path.isfile(cats):
        return []
    out = []
    try:
        with open(cats, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or line.startswith("VERSION"):
                    continue
                parts = line.split(":", 2)
                if len(parts) == 3 and re.fullmatch(
                        r"[0-9a-fA-F-]{36}", parts[0]):
                    out.append((parts[0], parts[1], parts[2]))
    except Exception as exc:                            # noqa: BLE001
        _log(f"could not read {cats}: {exc}")
    return out


def uuid_for_root(root):
    """The project's catalog UUID.  Reads its own cats.txt first.  '' on failure."""
    if not root:
        return ""
    wanted = catalog_path_for_root(root)
    for sub in LIBRARY_SUBDIRS:
        d = root if sub == "." else os.path.join(root, sub)
        for cid, cpath, _name in _read_cats(d):
            if cpath == wanted:
                return cid
    # No definition file names it yet (a pre-catalog project, or a subfolder
    # created later). Compute it; ensure_cats_files() writes the same value.
    return computed_uuid(wanted)


def uuid_for_blend(blend_path):
    """The catalog an asset in *blend_path* belongs to — the child where one fits.

    v1.20: this used to answer the project's single flat catalog.  Every
    creation path goes through here (`mark_and_assign`, `catalog_worker`,
    `thumbnail_worker`'s restore), so making it folder-aware upgrades all of
    them at once and keeps exactly one place that decides.
    """
    root = root_for_blend(blend_path)
    if not root:
        return ""
    return uuid_for(root, subdir_for_blend(blend_path, root))


# ---------------------------------------------------------------------------
# Definition files
# ---------------------------------------------------------------------------

def simple_name(catalog_path):
    """Blender's third field — the flat label, dashes for slashes."""
    return catalog_path.replace("/", "-")


def _cats_body(entries):
    """A definition file defining *entries* — [(uuid, path, simple_name)]."""
    lines = [
        "# This is an Asset Catalog Definition file for Blender.",
        "#",
        "# Empty lines and lines starting with `#` are ignored.",
        "# The first non-ignored line should be the version indicator.",
        "# Other lines are of the format",
        '# "UUID:catalog/path/for/tree/view:simple catalog name"',
        "",
        "VERSION 1",
        "",
    ]
    lines += [f"{cid}:{cpath}:{cname}" for cid, cpath, cname in entries]
    return "\n".join(lines) + "\n"


def desired_entries(root, subdir):
    """What the definition file in *subdir* must define.  -> [(uuid, path, name)].

    The parent, plus this folder's own child and nothing else.  Declaring the
    whole set in every file would make a library rooted at `LI/` show empty
    `SM`/`BP`/`Materials` nodes when it is viewed on its own; the All-Libraries
    view builds the full tree from the union anyway.
    """
    out = []
    parent = catalog_path_for_root(root)
    out.append((uuid_for_catalog_path(root, parent), parent, simple_name(parent)))
    child = child_name_for_subdir(subdir) if subdir and subdir != "." else ""
    if child:
        cpath = f"{parent}/{child}"
        out.append((uuid_for_catalog_path(root, cpath), cpath, simple_name(cpath)))
    return out


def cats_files_needing_update(root, subdirs=None):
    """Which definition files are missing a line they should have.  -> [paths].

    Pure: reads, never writes.  The foreground operator calls this FIRST so it
    can open exactly these files in Perforce before anything tries to write
    them, and so a dry run can state a number without touching the disk.
    """
    if not root or not os.path.isdir(root):
        return []
    out = []
    for sub in (subdirs or LIBRARY_SUBDIRS):
        d = root if sub == "." else os.path.join(root, sub)
        if not os.path.isdir(d):
            continue
        have = {c[1] for c in _read_cats(d)}
        if any(cpath not in have for _cid, cpath, _n in desired_entries(root, sub)):
            out.append(os.path.join(d, CATS_NAME))
    return out


def _force_writable(path):
    """Clear a read-only bit.  -> True when the file can now be written.

    Perforce leaves unopened files read-only.  Callers that have already
    opened the file for edit pass force_writable as the belt to that braces —
    the same thing `catalog_worker._writable()` does for .blend files, and for
    the same reason: a write that fails here is reported as a success by
    nothing, but it is much better not to fail at all.
    """
    try:
        if not os.path.exists(path) or os.access(path, os.W_OK):
            return True
        os.chmod(path, os.stat(path).st_mode | stat.S_IWRITE)
        return os.access(path, os.W_OK)
    except Exception:                                   # noqa: BLE001
        return False


def ensure_cats_files(root, subdirs=None, force_writable=False):
    """Make sure every library root defines the catalogs it needs.  -> [written].

    *subdirs* limits the pass to a subset — the material sync only owns
    `MaterialInstances/` and has no business rewriting the catalog file in
    `LI/`.  *force_writable* clears a read-only bit before writing, for callers
    that have already opened the file in Perforce.

    v1.20 defect, found on the live project: these files are checked into
    Perforce, so on any machine where they are not checked out every write
    fails with EACCES.  The failure was logged and swallowed, so the material
    sync created `MaterialInstances/blender_assets.cats.txt` (no file existed,
    nothing to be read-only) and silently failed to add the child line to the
    other five.  The result read as "only the Materials catalog works", which
    points at the material code rather than at file permissions.  Failures are
    now returned to the caller as well as logged.

    Idempotent, and it NEVER changes the UUID on a path a file already
    declares: that UUID is what the assets on disk point at, so rewriting it
    would un-file every one of them.  Lines for paths this function does not
    know about are preserved untouched — an artist's own catalog is theirs.

    v1.20 note: the old version skipped a directory as soon as it found the
    parent line, which is why adding the child catalogs needed this to become
    a merge rather than a create-if-absent.
    """
    if not root or not os.path.isdir(root):
        return []
    written = []
    blocked = []
    for sub in (subdirs or LIBRARY_SUBDIRS):
        d = root if sub == "." else os.path.join(root, sub)
        if not os.path.isdir(d):
            continue

        existing = _read_cats(d)
        have = {cpath: (cid, cname) for cid, cpath, cname in existing}
        merged = list(existing)
        added = 0
        for cid, cpath, cname in desired_entries(root, sub):
            if cpath in have:
                continue                       # keep its UUID — authoritative
            merged.append((cid, cpath, cname))
            added += 1
        if not added:
            continue

        merged.sort(key=lambda e: e[1])
        path = os.path.join(d, CATS_NAME)
        if force_writable:
            _force_writable(path)
        try:
            with open(path, "w", encoding="utf-8") as f:
                f.write(_cats_body(merged))
            written.append(path)
        except OSError as exc:
            blocked.append(path)
            _log(f"could not write catalog file in {d}: {exc}")
        except Exception as exc:                        # noqa: BLE001
            blocked.append(path)
            _log(f"could not write catalog file in {d}: {exc}")
    for w in written:
        _log(f"catalog file written: {w}")
    if blocked:
        _log(f"BLOCKED — {len(blocked)} catalog file(s) are read-only. On "
             f"Perforce that means not checked out; the catalog tree will be "
             f"incomplete until they are: " + ", ".join(blocked))
    ensure_cats_files.blocked = blocked
    return written


#: Populated by the last `ensure_cats_files` call.  A module attribute rather
#: than a second return value because four callers already unpack one list, and
#: only the operator that can DO something about it needs to look.
ensure_cats_files.blocked = []


# ---------------------------------------------------------------------------
# Assignment
# ---------------------------------------------------------------------------

def has_catalog(datablock):
    ad = getattr(datablock, "asset_data", None)
    if ad is None:
        return False
    cid = str(getattr(ad, "catalog_id", "") or "")
    return bool(cid) and cid != NIL_UUID


def assign(datablock, catalog_uuid, author="MapLink"):
    """Put an already-marked datablock in the catalog.  -> True when it changed."""
    ad = getattr(datablock, "asset_data", None)
    if ad is None or not catalog_uuid:
        return False
    if str(getattr(ad, "catalog_id", "") or "") == catalog_uuid:
        return False
    try:
        ad.catalog_id = catalog_uuid
    except Exception as exc:                            # noqa: BLE001
        _log(f"could not set catalog on '{getattr(datablock, 'name', '?')}': {exc}")
        return False
    if author:
        try:
            if not getattr(ad, "author", ""):
                ad.author = author
        except Exception:                               # noqa: BLE001
            pass
    return True


def mark_and_assign(datablock, blend_path=None, catalog_uuid=None, tags=()):
    """Mark as an asset if needed, then catalogue it.  -> True when it changed.

    This is what every creation path should call instead of a bare
    ``asset_mark()``.  An asset marked without a catalog is an asset the artist
    has to go and find in Unassigned.
    """
    changed = False
    if getattr(datablock, "asset_data", None) is None:
        try:
            datablock.asset_mark()
            changed = True
        except Exception as exc:                        # noqa: BLE001
            _log(f"asset_mark failed on '{getattr(datablock, 'name', '?')}': {exc}")
            return False

    ad = getattr(datablock, "asset_data", None)
    if ad is None:
        return changed

    for t in tags:
        try:
            ad.tags.new(t, skip_if_exists=True)
        except Exception:                               # noqa: BLE001
            pass

    if catalog_uuid is None:
        catalog_uuid = uuid_for_blend(blend_path) if blend_path else ""
    if catalog_uuid:
        changed = assign(datablock, catalog_uuid) or changed
    else:
        _log(f"no project catalog resolved for "
             f"'{getattr(datablock, 'name', '?')}' — left Unassigned")
    return changed


def marked_datablocks(bpy_data):
    """Every LOCAL collection, object and material carrying asset_data.

    Linked datablocks are excluded throughout: their asset metadata lives in
    the file that owns them, and writing to it here would be editing somebody
    else's .blend through a reference.  In `Library_MaterialInstances.blend`
    the materials ARE the assets, which is why they joined the list in v1.20.
    """
    out = []
    for attr in ("collections", "objects", "materials"):
        for db in getattr(bpy_data, attr, ()):
            if getattr(db, "library", None) is not None:
                continue
            if getattr(db, "asset_data", None) is not None:
                out.append(db)
    return out
