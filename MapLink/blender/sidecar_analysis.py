"""MapLink sidecar analysis — auto-generate analysis.json + filehash on .blend save.

v1.19 — Link-scope gate.  The sidecar is what UE reads, so the sidecar is where
        cross-root correctness has to be enforced: a .blend holding a Foreign
        or Broken library link produces NO sidecar.  See link_scope.py.
v1.2 — Removed hidden file attribute (broke P4 add on Windows)
v1.1 — Same-CL P4 handling
v1.0 — Sidecar analysis.json on save_post

Convention:
    LI_foo.blend  →  LI_foo_analysis.json
    LI_foo.blend  →  LI_foo_filehash.txt
"""

import os
import json
import hashlib
import traceback

import bpy
import mathutils

from . import p4_integration
from . import link_scope


class LinkScopeRefused(Exception):
    """Raised instead of recording a library that leaves the project tree.

    Carries the blocking entries so callers can name the files without
    re-scanning.
    """

    def __init__(self, entries):
        self.entries = list(entries)
        super().__init__("; ".join(link_scope.describe(self.entries))
                         or "foreign or broken library links")

# ---------------------------------------------------------------------------
# Prefixes that identify MapLink assets
# ---------------------------------------------------------------------------

_MAPLINK_PREFIXES = ("BP_", "LI_", "SM_")


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _is_linked_asset_instance(obj):
    return (obj.instance_type == 'COLLECTION'
            and obj.instance_collection is not None
            and obj.instance_collection.library is not None
            and obj.instance_collection.asset_data is not None)


def _is_in_noexport_collection(obj):
    if not hasattr(_is_in_noexport_collection, '_cache_id') or \
       _is_in_noexport_collection._cache_id != id(bpy.data.collections):
        excluded = set()
        roots = [c for c in bpy.data.collections if c.name.lower() == 'noexport']
        queue = list(roots)
        while queue:
            col = queue.pop()
            excluded.add(col.name)
            for child in col.children:
                if child.name not in excluded:
                    queue.append(child)
        _is_in_noexport_collection._excluded = excluded
        _is_in_noexport_collection._cache_id = id(bpy.data.collections)
    excluded = _is_in_noexport_collection._excluded
    if not excluded:
        return False
    for col in obj.users_collection:
        if col.name in excluded:
            return True
    return False


def _matrix_to_list(m):
    return [m[row][col] for row in range(4) for col in range(4)]


def _world_bbox(obj):
    if not obj.bound_box:
        loc = obj.matrix_world.translation
        return [loc.x, loc.y, loc.z, loc.x, loc.y, loc.z]
    world_corners = [obj.matrix_world @ mathutils.Vector(corner)
                     for corner in obj.bound_box]
    xs = [c.x for c in world_corners]
    ys = [c.y for c in world_corners]
    zs = [c.z for c in world_corners]
    return [min(xs), min(ys), min(zs), max(xs), max(ys), max(zs)]


def _collection_parent_map():
    """Map child-collection-name -> parent Collection for the active scene,
    rooted at the scene master collection. Used to resolve an object's full
    collection path (Blender collections have no direct parent pointer)."""
    master = bpy.context.scene.collection
    parent = {}

    def _walk(col):
        for child in col.children:
            parent[child.name] = col
            _walk(child)

    _walk(master)
    return parent, master


def _object_collection_path(obj, parent_map, master, strip_root=None):
    """v1.13.1 — Slash-joined chain of the USER collections the object lives in,
    from just under the scene master down to the object's own collection.
    Mirrors the artist's Blender outliner org into UE outliner folders.

    Excludes the master collection itself and any leading segment equal to
    strip_root (the blend stem — already represented by the UE MapLink/<Root>
    folder, so we don't duplicate it). Returns '' when the object sits directly
    in the master collection (→ no extra sub-folder, legacy behaviour)."""
    cols = list(obj.users_collection)
    if not cols:
        return ''
    # Prefer a collection that is actually part of this scene's hierarchy.
    col = None
    for c in cols:
        if c is master or c.name in parent_map:
            col = c
            break
    if col is None:
        col = cols[0]

    chain = []
    cur = col
    guard = 0
    while cur is not None and cur is not master and guard < 64:
        chain.append(cur.name)
        cur = parent_map.get(cur.name)
        guard += 1
    chain.reverse()

    if strip_root and chain and chain[0] == strip_root:
        chain = chain[1:]

    return '/'.join(chain)


def compute_file_md5(filepath: str) -> str:
    """MD5 hex digest — matches FMapLinkUtils::ComputeFileMD5 on the C++ side."""
    h = hashlib.md5()
    with open(filepath, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


# ---------------------------------------------------------------------------
# Core: build the analysis report from the current scene
# ---------------------------------------------------------------------------

def _carry_forward_bp_binding(report):
    """v1.8.1 - Preserve a BP_ stand-in's reference-mode binding across re-saves.

    maplink_create_bp_standin.py stamps the binding as custom properties on the
    stand-in's ROOT collection, inside the .blend:
        root_col["blueprint_path"]        -> bound_blueprint_path (UE-side gate)
        root_col["maplink_standin_mode"]  -> standin_mode
    Those props live in the scene data and survive every save, but a freshly
    rebuilt sidecar would otherwise drop the two UE-side fields, demoting the
    BP_ to an ORPHAN (placement then substitutes BP_Deprecated).  Read them back
    so the binding persists.

    Guards:
      * LOCAL collections only (col.library is None) - never inherit a binding
        from a *linked* BP_ stand-in when saving a parent SM_/LI_ that references
        it.  This is critical: linked collections appear in bpy.data.collections.
      * No-op when no local collection carries the prop (SM_/LI_/quick-create).
    """
    matches = []
    for col in bpy.data.collections:
        if col.library is not None:
            continue  # belongs to another .blend (linked) - not our binding
        bp = col.get("blueprint_path", "")
        if bp:
            matches.append((col.name, bp, col.get("maplink_standin_mode", "")))

    if not matches:
        return  # not a BP_ stand-in (or binding prop absent) - leave as-is

    if len(matches) > 1:
        stem = report.get("blend_name", "")
        chosen = next((m for m in matches if m[0] == stem), matches[0])
        print(f"[MapLink] WARNING: {len(matches)} local collections carry a "
              f"blueprint_path binding {[m[0] for m in matches]}; using "
              f"'{chosen[0]}'.")
    else:
        chosen = matches[0]

    _, bound, mode = chosen
    report["bound_blueprint_path"] = bound
    if mode:
        report["standin_mode"] = mode
    print(f"[MapLink] Carried forward BP binding -> bound_blueprint_path="
          f"'{bound}', standin_mode='{mode}'")
    return report


def build_analysis_report(blend_name: str, enforce_link_scope: bool = True) -> dict:
    """Walk the scene and return the same report dict that _do_analyze produces.

    v1.19 — refuses (LinkScopeRefused) when any library is Foreign or Broken.

    Before this, a foreign ``SM_Foo`` passed the `_MAPLINK_PREFIXES` filter, had
    its absolute path written into analysis.json, and UE imported it happily
    from wherever it happened to resolve on the authoring machine.  The author
    saw a correct import; the teammate saw a missing link and UE never
    complained.  The prefix filter answers "is this a MapLink asset", which is
    a different question from "will this resolve for anyone else".

    ``enforce_link_scope=False`` is for read-only inspection only (reporting on
    a damaged file).  Nothing that writes a sidecar may pass it.
    """
    if enforce_link_scope:
        blocking = link_scope.blocking_entries()
        if blocking:
            raise LinkScopeRefused(blocking)

    report = {
        'blend_name': blend_name,
        'unit_scale': 1.0,
        'blend_own_geo': [],
        'linked_assets': [],
        'libraries': [],
    }

    # v1.13.1 — resolve each object's Blender collection path so UE can mirror
    # the artist's outliner org as WP outliner folders.
    parent_map, master_col = _collection_parent_map()

    for obj in bpy.context.scene.objects:
        if _is_in_noexport_collection(obj):
            continue

        if _is_linked_asset_instance(obj):
            src_path = bpy.path.abspath(
                obj.instance_collection.library.filepath)
            src_stem = os.path.splitext(os.path.basename(src_path))[0]
            if not src_stem.startswith(_MAPLINK_PREFIXES):
                continue
            wm = obj.matrix_world
            report['linked_assets'].append({
                'name': obj.name,
                'world_transform': _matrix_to_list(wm),
                'bbox_world': _world_bbox(obj),
                'source_blend_path': src_path,
                'asset_name': obj.instance_collection.name,
                'collection_path': _object_collection_path(
                    obj, parent_map, master_col, strip_root=blend_name),
            })
        elif obj.type == 'MESH':
            wm = obj.matrix_world
            report['blend_own_geo'].append({
                'name': obj.name,
                'world_transform': _matrix_to_list(wm),
                'bbox_world': _world_bbox(obj),
                'collection_path': _object_collection_path(
                    obj, parent_map, master_col, strip_root=blend_name),
            })

    for lib in bpy.data.libraries:
        abs_path = bpy.path.abspath(lib.filepath)
        lib_stem = os.path.splitext(os.path.basename(abs_path))[0]
        if not lib_stem.startswith(_MAPLINK_PREFIXES):
            continue
        report['libraries'].append({
            'name': lib.name,
            'filepath': abs_path,
        })

    _carry_forward_bp_binding(report)
    return report


# ---------------------------------------------------------------------------
# Write sidecar files
# ---------------------------------------------------------------------------

def write_sidecar(blend_path: str, use_p4: bool = True):
    """Generate sidecar analysis.json + filehash.txt next to the .blend.

    v1.19 — this is THE gate.  A file with a Foreign or Broken library link
    gets no sidecar at all, so UE cannot import it: not a warning, not a
    best-effort record with a bad path in it, nothing.  Raises
    LinkScopeRefused; the save itself is never blocked.

    Known limitation: a sidecar written by an earlier, clean save is left on
    disk rather than deleted (it is under source control, and deleting a
    teammate's file to express a local problem is the wrong trade).  It is
    stale by definition -- the filehash beside it is not refreshed either, so
    UE reports the asset out of date rather than silently reusing it.
    """
    stem = os.path.splitext(os.path.basename(blend_path))[0]
    blend_dir = os.path.dirname(blend_path)
    analysis_path = os.path.join(blend_dir, f"{stem}_analysis.json")
    hash_path = os.path.join(blend_dir, f"{stem}_filehash.txt")

    # Build the report FIRST.  It raises before anything is opened for edit in
    # Perforce, so a refused save leaves no changelist churn behind.
    report = build_analysis_report(stem)

    # Find .blend's CL so sidecars go in the same one
    target_cl = None
    if use_p4 and p4_integration.p4_available():
        target_cl = p4_integration.find_file_changelist(blend_path)
        if target_cl is None:
            target_cl = p4_integration._get_or_create_changelist(
                "[MapLink-Blender] sidecar analysis")

        for fp in (analysis_path, hash_path):
            if os.path.isfile(fp):
                p4_integration.checkout_or_add(
                    fp, cl=target_cl if target_cl and target_cl > 0 else None)

    # Write analysis.json (built above, before any P4 checkout).
    with open(analysis_path, 'w', encoding='utf-8') as f:
        json.dump(report, f, indent=2)

    # Write MD5 hash
    file_hash = compute_file_md5(blend_path)
    with open(hash_path, 'w', encoding='utf-8') as f:
        f.write(file_hash)

    # P4 add for newly created files
    if use_p4 and p4_integration.p4_available():
        for fp in (analysis_path, hash_path):
            p4_integration.checkout_or_add(
                fp, cl=target_cl if target_cl and target_cl > 0 else None)

    print(f"[MapLink] Sidecar written: {analysis_path}")
    print(f"[MapLink] Sidecar hash: {hash_path} ({file_hash})")


# ---------------------------------------------------------------------------
# save_post handler
# ---------------------------------------------------------------------------

@bpy.app.handlers.persistent
def _on_save_post(filepath_unused):
    """Called after every .blend save. Generates sidecar analysis + hash."""
    try:
        blend_path = bpy.data.filepath
        if not blend_path:
            return

        stem = os.path.splitext(os.path.basename(blend_path))[0]
        if not stem.startswith(_MAPLINK_PREFIXES):
            return

        # Non-MapLink links from outside the tree do not block, but they are
        # never exported either — the prefix filter drops them, so whatever the
        # artist instanced from one is simply absent in Unreal. Say so on every
        # save rather than letting it be discovered in the level.
        for e in link_scope.unexported_libraries():
            print(f"[MapLink] NOTE: '{e['name']}' is outside this project and "
                  f"is not a MapLink asset — nothing instanced from it reaches "
                  f"Unreal. ({e['abspath'] or e['raw']})")

        try:
            write_sidecar(blend_path, use_p4=True)
        except LinkScopeRefused as refused:
            # Named, not swallowed.  The save succeeded; the sidecar did not,
            # and the reason is the whole point of the message.
            print("[MapLink] NO SIDECAR WRITTEN — this file links assets that "
                  "are outside its MapLink project:")
            for line in link_scope.describe(refused.entries):
                print(f"[MapLink]     {line}")
            print("[MapLink] UE cannot import it until they are copied in "
                  "(MapLink ▸ Project ▸ Import Asset…, or the prompt on save).")

    except Exception:
        print(f"[MapLink] Sidecar analysis failed (non-fatal):")
        traceback.print_exc()


# ---------------------------------------------------------------------------
# Registration
# ---------------------------------------------------------------------------

def register():
    if _on_save_post not in bpy.app.handlers.save_post:
        bpy.app.handlers.save_post.append(_on_save_post)


def unregister():
    if _on_save_post in bpy.app.handlers.save_post:
        bpy.app.handlers.save_post.remove(_on_save_post)
