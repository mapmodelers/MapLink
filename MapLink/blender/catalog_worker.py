#!/usr/bin/env python3
"""MapLink catalog repair worker — put stray assets back in their catalog.

SELF-CONTAINED apart from its sibling `asset_catalog`, reached the same way
`quick_create_worker` reaches `batch_sidecar`: a sys.path insertion, no addon
package import.  Launched with --factory-startup.

For every .blend given, it opens the file, finds every locally-marked asset
datablock with no catalog, assigns the owning project's catalog, and saves —
**only** files that actually changed.  A dry run does everything except save,
which is how the operator can state a number before touching anything.

Usage:
    blender --background <throwaway>.blend --factory-startup \\
            --python catalog_worker.py -- <spec.json>

spec.json:
    {"blends": ["..."], "dry_run": true|false, "root": "..."}
    root is optional; omitted, each file resolves its own project's.
    "catalog_uuid" is accepted and ignored (pre-v1.20 callers).

v1.20 -- the catalog is a tree, so this does two jobs rather than one:

    Unassigned          -> the asset's folder catalog      ("assigned")
    the project PARENT  -> the asset's folder catalog      ("moved")
    anything else       -> left exactly as it is           ("custom")

The third line is the important one.  An artist who filed something in a
catalog of their own has said where it goes, and a repair pass is not an
argument.  Only MapLink's own two states are touched.
"""

import json
import os
import stat
import sys

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import asset_catalog as ac                              # noqa: E402

BEGIN = "###MAPLINK_CATALOG_REPORT_BEGIN###"
END = "###MAPLINK_CATALOG_REPORT_END###"


def _log(msg):
    print(f"[MapLink CatalogFix] {msg}", flush=True)


def _writable(path):
    """Clear a read-only bit, reporting whether it had to.

    Perforce leaves unopened files read-only, and `save_mainfile` returns
    CANCELLED *silently* in background mode against one.  The operator checks
    the files out before the write pass, so this is the belt to that braces:
    if a file is still read-only here we say so rather than reporting a
    success that did not happen.
    """
    try:
        if os.access(path, os.W_OK):
            return True
        os.chmod(path, os.stat(path).st_mode | stat.S_IWRITE)
        return os.access(path, os.W_OK)
    except Exception:                                   # noqa: BLE001
        return False


def _process(blend, dry_run, root):
    entry = {"blend": blend, "assigned": [], "moved": [], "already": 0,
             "custom": 0, "saved": False, "error": ""}
    try:
        bpy.ops.wm.open_mainfile(filepath=blend, load_ui=False)
    except Exception as exc:                            # noqa: BLE001
        entry["error"] = f"could not open: {exc}"
        return entry

    own_root = root or ac.root_for_blend(blend)
    cat = ac.uuid_for_blend(blend)          # the child for this file's folder
    parent = ac.uuid_for_root(own_root) if own_root else ""
    if not cat:
        entry["error"] = "no MapLink project above this file"
        return entry

    for db in ac.marked_datablocks(bpy.data):
        ad = getattr(db, "asset_data", None)
        current = str(getattr(ad, "catalog_id", "") or "") if ad else ""

        if current == cat:
            # Already right. A .blend loose at the project root resolves cat
            # == parent, so this is also what stops those churning every run.
            entry["already"] += 1
        elif not current or current == ac.NIL_UUID:
            if ac.assign(db, cat):
                entry["assigned"].append(db.name)
        elif parent and current == parent:
            if ac.assign(db, cat):
                entry["moved"].append(db.name)
        else:
            entry["custom"] += 1

    if not (entry["assigned"] or entry["moved"]) or dry_run:
        return entry

    if not _writable(blend):
        entry["error"] = "read-only (not checked out) — not saved"
        return entry
    try:
        bpy.ops.wm.save_mainfile(filepath=blend, compress=True)
        entry["saved"] = True
    except Exception as exc:                            # noqa: BLE001
        entry["error"] = f"could not save: {exc}"
    return entry


def main():
    argv = sys.argv
    args = argv[argv.index("--") + 1:] if "--" in argv else []
    if not args:
        _log("ERROR: no spec file given")
        sys.exit(2)

    with open(args[0], "r", encoding="utf-8") as f:
        spec = json.load(f)

    dry_run = bool(spec.get("dry_run", True))
    root = str(spec.get("root", "") or "")
    blends = [os.path.abspath(b) for b in spec.get("blends", [])]

    if root and not dry_run:
        # The operator has already opened these in Perforce by the time the
        # write pass runs, so force_writable only covers a file whose read-only
        # bit survived. A DRY RUN writes nothing at all — it used to, which
        # made "report only" untrue for the one kind of file it created.
        ac.ensure_cats_files(root, force_writable=True)

    report = {"dry_run": dry_run, "files": []}
    for blend in blends:
        if not os.path.isfile(blend):
            report["files"].append({"blend": blend, "error": "missing",
                                    "assigned": [], "moved": [], "already": 0,
                                    "custom": 0, "saved": False})
            continue
        report["files"].append(_process(blend, dry_run, root))

    print(BEGIN)
    print(json.dumps(report))
    print(END)


if __name__ == "__main__":
    main()
