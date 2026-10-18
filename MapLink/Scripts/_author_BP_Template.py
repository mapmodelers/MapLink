# MapLink v1.8 — One-shot authoring of BP_Template.blend.
#
# Run this ONCE per Blender project root (not per UE project).
# The same template is reused by every MapLink BP stand-in created
# against the same BlenderProjectRoot.
#
# Usage:
#   1. Open Blender (5.0+).
#   2. Scripting workspace → New Text → paste this whole file.
#   3. Either open a .blend that already lives inside the MapLink project
#      (the output path is then derived automatically), or set OUTPUT_PATH
#      below to:
#        <MapLinkProjectRoot>/BP/BP_Template/BP_Template.blend
#      This follows the canonical MapLink convention
#        <root>/<PREFIX>/<STEM>/<STEM>.blend
#      — same layout as per-BP stand-ins and the rename tool's output.
#      For a project at C:/MapLink/MyProject that is:
#        C:/MapLink/MyProject/BP/BP_Template/BP_Template.blend
#   4. Click "Run Script".
#   5. P4-add the resulting BP_Template.blend.
#
# The template is a near-empty .blend containing one local (non-linked)
# collection named "MapLink_BP_Template" at the scene root.  The
# headless stand-in builder (maplink_create_bp_standin.py) opens this
# file, renames that collection to <BP name>, marks it as asset, and
# fills it with geometry.
#
# Saved with compress=False (Blender 5.0 silently ignores the
# use_file_compression preference in headless contexts — keep the
# template uncompressed so the same convention holds everywhere).

import os
import bpy

# ---- OPTIONAL OVERRIDE ---------------------------------------------------
# Absolute path where the template will be saved.  Forward slashes only.
#
# Leave this EMPTY and the path is derived from the .blend you have open, by
# walking up to the nearest maplink_project.json — the same rule every other
# MapLink tool uses to find a project root.
#
# v1.20: this used to default to a full path on the machine it was written on.
# A shipped default pointing at one person's drive is worse than no default:
# on anybody else's machine it either fails with a confusing error or, if that
# directory happens to exist, quietly writes a template into the wrong project.
OUTPUT_PATH = ""
# --------------------------------------------------------------------------

TEMPLATE_COLLECTION_NAME = "MapLink_BP_Template"
MANIFEST_NAME = "maplink_project.json"


def _find_project_root(start_dir):
    """Nearest directory at or above *start_dir* holding the manifest, or ""."""
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


def resolve_output_path():
    """OUTPUT_PATH, or the canonical location under the open file's project.

    Returns "" when neither is available, which is a refusal rather than a
    guess: this script writes a file, and it must never write it somewhere the
    user did not point it at.
    """
    if OUTPUT_PATH.strip():
        return OUTPUT_PATH.strip().replace("\\", "/")

    root = _find_project_root(os.path.dirname(bpy.data.filepath))
    if not root:
        return ""
    return "/".join([root.replace("\\", "/").rstrip("/"),
                     "BP", "BP_Template", "BP_Template.blend"])


def main():
    # Resolve the destination FIRST.  read_factory_settings() below clears
    # bpy.data.filepath, so deriving the project root after the wipe would
    # always come back empty — and the derivation is the whole point.
    out_path = resolve_output_path()
    if not out_path:
        print("[MapLink] Nothing was written.")
        print("[MapLink] Open a .blend inside the MapLink project first (any "
              "file under the root that holds maplink_project.json), or set "
              "OUTPUT_PATH at the top of this script to")
        print("[MapLink]   <MapLinkProjectRoot>/BP/BP_Template/BP_Template.blend")
        return

    # Wipe everything down to an empty scene.  Equivalent to File →
    # New → General then deleting the default cube/camera/light.
    bpy.ops.wm.read_factory_settings(use_empty=True)

    scene = bpy.context.scene

    # Create the template collection as a direct child of the scene root.
    col = bpy.data.collections.new(TEMPLATE_COLLECTION_NAME)
    scene.collection.children.link(col)

    # Ensure the output directory exists.
    out_dir = os.path.dirname(out_path)
    if out_dir and not os.path.isdir(out_dir):
        os.makedirs(out_dir, exist_ok=True)

    # IMPORTANT: compress=False.  use_file_compression preference is
    # silently ignored in headless; we keep all template/stand-in
    # files decompressed so the same convention holds end-to-end.
    bpy.ops.wm.save_as_mainfile(filepath=out_path, compress=False)

    print(f"[MapLink] BP_Template.blend saved to: {out_path}")
    print(f"[MapLink] Root collection: '{TEMPLATE_COLLECTION_NAME}'")


main()
