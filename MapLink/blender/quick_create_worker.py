"""quick_create_worker.py — headless Quick-Create asset builder (v1.4.5).

Invoked by maplink.quick_create_asset:

    blender --factory-startup --background <dest.blend> \
            --python quick_create_worker.py -- <manifest.json>

<dest.blend> is a fresh copy of "<TYPE>_Template.blend".  Its template root
collection "<TYPE>_Template" contains sub-collections LINK and NoExport.  We:

    1. rename ONLY the root collection to <asset_name>
    2. (re-)mark it as a Blender asset
    3. append the manifest's selected objects from the source .blend into root
    4. re-anchor into the active object's local frame: anchor -> identity
       (clean 0/0/0) in the asset; the foreground drops the instance at the
       anchor's full world matrix so the placement keeps the original transform
    5. tidy data-block names that carry the template prefix (LINK/NoExport untouched)
    6. save
    7. resolve the shared "[MapLink][Blender] Created Asset" CL (find-or-create)
    8. write {stem}_analysis.json + {stem}_filehash.txt and P4-add the .blend +
       both sidecars onto that CL

Crucially, steps 7-8 reuse ``batch_sidecar``'s helpers verbatim, so the analysis
schema, the MD5, the filenames, and the add-first P4 pattern are identical to
every other asset in the project.  Under --factory-startup the addon is NOT
loaded, so neither the sidecar save_post handler nor fbx_export_on_save fire in
here — the worker writes the sidecars explicitly, exactly as batch_sidecar does.
FBX-cache population happens later on the asset's first normal save.
"""

import bpy
import sys
import os
import json
import mathutils

PRESERVED = {"LINK", "NoExport"}


def main():
    argv = sys.argv
    sep = argv.index("--") if "--" in argv else -1
    if sep < 0:
        print("[QuickCreate] ERROR: no manifest")
        sys.exit(1)
    with open(argv[sep + 1], encoding='utf-8') as f:
        m = json.load(f)

    asset_name = m["asset_name"]
    atype = m["asset_type"]
    source = m["source_blend"]
    selected = m["selected_objects"]
    cl_desc = m["created_cl_desc"]
    skip_sidecars = bool(m.get("skip_sidecars", False))

    # Reuse the exact headless-safe helpers.  batch_sidecar has no relative
    # imports and is guarded by `if __name__ == "__main__"`, so importing it
    # here neither pulls in the addon package nor runs its batch main().
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import batch_sidecar as bs

    # 1. Rename root collection only.
    root = bpy.data.collections.get(f"{atype}_Template")
    if root is None:
        root = next((c for c in bpy.context.scene.collection.children), None)
    if root is None:
        root = bpy.data.collections.new(asset_name)
        bpy.context.scene.collection.children.link(root)
    else:
        root.name = asset_name
    for sub in PRESERVED:
        if sub not in root.children:
            print(f"[QuickCreate] WARNING: sub-collection '{sub}' missing in template")

    # Diagnostic: what did the template actually ship with?  (root direct
    # objects + each sub-collection's object count).  This is the ground truth
    # for deciding where asset geometry should live.
    print(f"[QuickCreate] template root='{root.name}' "
          f"direct_objects={[o.name for o in root.objects]}")
    for ch in root.children:
        print(f"[QuickCreate]   sub-collection '{ch.name}': "
              f"{len(ch.objects)} object(s) {[o.name for o in ch.objects]}")

    # 2. Re-mark as asset (idempotent) AND catalogue it.
    #    v1.19 -- this used to mark and tag and stop there, so every asset
    #    made with Create SM/LI from selection arrived in the Asset Browser
    #    under "Unassigned" while the four templates the scaffolder seeded sat
    #    correctly under MapLink_<Setup>. The tag was never the thing the
    #    browser groups by.
    import asset_catalog as ac
    ac.mark_and_assign(root, blend_path=bpy.data.filepath, tags=(atype,))

    # 3. Append selected objects into root.
    #    Capture the appended datablocks from `dt.objects` AFTER the context
    #    closes — these are the real references regardless of any collision
    #    rename (e.g. source "Cube" -> "Cube.001" when the dest already has a
    #    "Cube").  IMPORTANT: assign a *copy* to dt.objects.  Blender keeps a
    #    reference to the assigned list and replaces its string entries with the
    #    loaded datablocks on context exit; assigning our own `wanted` list
    #    directly would corrupt it (turning the name strings into objects and
    #    breaking the count + the missing-check below).
    with bpy.data.libraries.load(source, link=False) as (df, dt):
        wanted = [n for n in df.objects if n in selected]
        dt.objects = list(wanted)
    appended = [o for o in dt.objects if o is not None]

    print(f"[QuickCreate] selection={len(selected)} "
          f"found_in_source={len(wanted)} appended={len(appended)} "
          f"-> {[o.name for o in appended]}")
    missing = [n for n in selected if n not in wanted]
    if missing:
        print(f"[QuickCreate] WARNING: selected objects missing from the "
              f"selection export (unexpected): {missing}")

    # Guard: if nothing was appended, do NOT clear the placeholder, save, or
    # touch P4 — that only produces an empty, P4-added junk asset.  Fail loudly
    # before any side effect; the dest is still an untouched template copy, so
    # tell the foreground it's safe to delete (lets the same name be retried).
    if not appended:
        print("[QuickCreate] ERROR: no selected objects were appended; "
              "aborting before save/P4.")
        print("[QuickCreate] SAFE_TO_DELETE_DEST")
        sys.exit(2)

    # 3b. Clear the template's own placeholder MESHES from the asset root so the
    #     asset contains the selection, not the template stand-in.  Unlink-only
    #     and mesh-only: LINK / NoExport sub-collections and any non-mesh markers
    #     are left untouched; an unlinked placeholder with no other user is
    #     dropped on save.
    for ph in list(root.objects):
        if ph not in appended and ph.type == 'MESH':
            root.objects.unlink(ph)
            print(f"[QuickCreate] cleared template placeholder: {ph.name}")

    # 3c. Link appended objects into root; lift any out of the scene master.
    scene_master = bpy.context.scene.collection
    root_names = {o.name for o in root.objects}
    master_names = {o.name for o in scene_master.objects}
    for o in appended:
        if o.name not in root_names:
            root.objects.link(o)
        if o.name in master_names:
            scene_master.objects.unlink(o)

    # 4. Re-anchor into the anchor's LOCAL frame.  The anchor is the last-
    #    selected (active) object in the foreground; expressing each appended
    #    object as anchor_world^-1 @ object_world puts the anchor at IDENTITY
    #    (clean location 0,0,0 / 0 rotation / unit scale) in the asset, with the
    #    rest of the selection laid out relative to it.  The foreground then
    #    drops the linked instance at the anchor's full world matrix, so the
    #    placement reproduces the original transform (a cube authored at rot
    #    7/45/45 reads 0/0/0 in the .blend and 7/45/45 on the LI instance).
    #    Fallback (no anchor_matrix - legacy manifests only): the old
    #    translation-only re-anchor, which keeps each object's world rotation.
    bpy.context.view_layer.update()
    anchor_mat = m.get("anchor_matrix")
    if anchor_mat is not None:
        a_inv = mathutils.Matrix(anchor_mat).inverted_safe()
        for o in appended:
            o.matrix_world = a_inv @ o.matrix_world
    else:
        anchor = mathutils.Vector(m.get("anchor_location", (0.0, 0.0, 0.0)))
        shift = mathutils.Matrix.Translation(-anchor)
        for o in appended:
            o.matrix_world = shift @ o.matrix_world

    # 5. Name cleanup — only data-blocks carrying the template prefix;
    #    LINK / NoExport untouched.
    pref = f"{atype}_Template"
    for o in bpy.data.objects:
        if o.name.startswith(pref):
            o.name = o.name.replace(pref, asset_name, 1)
    for me in bpy.data.meshes:
        if me.name.startswith(pref):
            me.name = me.name.replace(pref, asset_name, 1)

    # 6. Save (no addon loaded → no save_post side effects).  This happens
    #    before any P4 op, so if it fails the dest is still untouched and safe
    #    to delete — tell the foreground so it can clean up and report failure
    #    instead of pretending the asset was created.
    try:
        bpy.ops.wm.save_mainfile(filepath=bpy.data.filepath, compress=True)
    except RuntimeError as e:
        print(f"[QuickCreate] ERROR: could not save asset .blend: {e}")
        print("[QuickCreate] SAFE_TO_DELETE_DEST")
        sys.exit(3)
    dest = bpy.data.filepath
    stem = os.path.splitext(os.path.basename(dest))[0]
    blend_dir = os.path.dirname(dest)
    analysis_path = os.path.join(blend_dir, f"{stem}_analysis.json")
    hash_path = os.path.join(blend_dir, f"{stem}_filehash.txt")

    # 7. Resolve the shared "Created Asset" CL (find-or-create, stacking).
    has_p4 = bs._p4_available()
    cl = None
    if has_p4:
        cl = bs._find_pending_cl(cl_desc) or bs._create_cl(cl_desc)
        bs._p4_add_or_edit(dest, cl)   # add the new .blend itself

    # 8. Sidecars via the SAME producers (schema + MD5 identical to every asset).
    #    Presets skip this entirely — they are appended as a library, not
    #    imported as instanced meshes, so they carry no analysis.json/filehash.
    if skip_sidecars:
        print(f"[QuickCreate] skip_sidecars=True — no analysis.json/filehash "
              f"written for {stem}")
    else:
        report = bs._build_report(stem)    # blend_own_geo lists appended meshes
        with open(analysis_path, 'w', encoding='utf-8') as f:
            json.dump(report, f, indent=2)
        with open(hash_path, 'w', encoding='utf-8') as f:
            f.write(bs._compute_md5(dest))

        if has_p4:
            for fp in (analysis_path, hash_path):
                ok, act = bs._p4_add_or_edit(fp, cl)
                if not ok:
                    print(f"[QuickCreate] [P4] FAILED {os.path.basename(fp)}: {act}")
            print(f"[QuickCreate] P4 CL {cl}: added {stem} .blend + sidecars")

    try:
        os.remove(argv[sep + 1])
    except Exception:
        pass
    print(f"[QuickCreate] Done: {dest}")


main()
