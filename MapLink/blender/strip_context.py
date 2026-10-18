"""Headless context-strip script for MapLink_Blender.

Run via:  blender --factory-startup --background <source.blend> --python strip_context.py -- <output.blend>

Strips the loaded file down to geometry-only context, saves to output path.
"""

import os
import sys
import traceback

import bpy


def _is_noexport(name: str) -> bool:
    return name.strip().lower() == "noexport"


def _remove_collection_recursive(col):
    """Remove a collection and all its children/objects."""
    for child in list(col.children):
        _remove_collection_recursive(child)
    for obj in list(col.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    bpy.data.collections.remove(col)


def strip(exclude_blend_path: str = ""):
    # 0. Remove collection instances that reference the drill target
    #    (prevents circular reference when context is linked back into target)
    if exclude_blend_path:
        exclude_norm = os.path.normpath(exclude_blend_path).lower()
        for obj in list(bpy.data.objects):
            if (obj.instance_type == 'COLLECTION'
                    and obj.instance_collection
                    and obj.instance_collection.library):
                lib_path = os.path.normpath(
                    bpy.path.abspath(obj.instance_collection.library.filepath)).lower()
                if lib_path == exclude_norm:
                    bpy.data.objects.remove(obj, do_unlink=True)

    # 1. Remove NoExport collections (case-insensitive) + hidden/excluded
    scene = bpy.context.scene
    view_layer = bpy.context.view_layer

    # Gather collections to remove
    to_remove = []
    for col in list(bpy.data.collections):
        if _is_noexport(col.name):
            to_remove.append(col)

    # Also gather hidden/excluded collections
    def _check_layer_col(lc):
        if lc.exclude or lc.collection.hide_viewport:
            if lc.collection not in to_remove:
                to_remove.append(lc.collection)
        for child in lc.children:
            _check_layer_col(child)

    _check_layer_col(view_layer.layer_collection)

    for col in to_remove:
        try:
            _remove_collection_recursive(col)
        except Exception:
            pass

    # 2. Remove lights, cameras, speakers
    for obj in list(bpy.data.objects):
        if obj.type in {'LIGHT', 'CAMERA', 'SPEAKER'}:
            bpy.data.objects.remove(obj, do_unlink=True)

    # 3. Remove worlds only (keep materials, images, textures, node groups
    #    so context renders with correct materials from shared libraries)
    for w in list(bpy.data.worlds):
        if not w.library:
            bpy.data.worlds.remove(w)

    # 4. Consolidate under _MapLink_Context root collection
    root = bpy.data.collections.new("_MapLink_Context")
    scene.collection.children.link(root)

    # Move all top-level scene collections (except root) under it
    for col in list(scene.collection.children):
        if col is not root:
            scene.collection.children.unlink(col)
            root.children.link(col)

    # Move loose scene-level objects under root
    for obj in list(scene.collection.objects):
        scene.collection.objects.unlink(obj)
        root.objects.link(obj)

    # 5. Purge orphans
    bpy.ops.outliner.orphans_purge(do_recursive=True)


def main():
    argv = sys.argv
    try:
        idx = argv.index("--")
    except ValueError:
        print("ERROR: No output path. Usage: ... --python strip_context.py -- <output.blend>")
        sys.exit(1)

    output_path = argv[idx + 1]
    exclude_path = argv[idx + 2] if len(argv) > idx + 2 else ""

    try:
        strip(exclude_path)
    except Exception:
        traceback.print_exc()
        print("WARNING: Context strip had errors, saving partial result.")
    finally:
        bpy.ops.wm.save_as_mainfile(filepath=output_path, compress=False)
        print(f"Context saved: {output_path}")


if __name__ == "__main__":
    main()
