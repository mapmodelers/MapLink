"""thumbnail_worker.py — windowed asset-preview regeneration (v1.4.5).

Invoked by maplink.run_thumbnail:

    blender <asset.blend> --python thumbnail_worker.py

Opens the asset .blend WITH a window (a real GPU/draw context is required to
render previews), regenerates the asset-marked collection's preview, saves, and
quits.

Why the previous approach did nothing: it looked for a FILE_BROWSER area to
override into, but a freshly opened .blend has no Asset Browser area, so the
loop never ran.  Here we drive the preview directly via temp_override(id=coll)
and, per request, clear + re-mark the asset first (preserving its SM_/LI_ tag)
to force a clean regeneration.  Preview rendering is asynchronous, so we poll
the RENDER_PREVIEW job and only save once it finishes (or a timeout elapses).

Scripted collection-preview generation is version-sensitive — treat console
output as ground truth.  If generate_preview is unavailable on this build, the
fallback is an offscreen render assigned to coll.preview.image_pixels_float.
"""

import os
import sys

import bpy

# Reuse the badge compositor rather than a second copy of it. This script is
# run with --python, so it has no package context; putting its own folder on
# sys.path is how the other headless-safe modules are reached.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    import thumbnail_badge
except Exception as _e:                                     # pragma: no cover
    thumbnail_badge = None
    print(f"[MapLink Thumb] type badge unavailable: {_e}")

POLL = 0.5          # seconds between job-status polls
MIN_WAIT = 2.0      # always give the async job at least this long
MAX_WAIT = 45.0     # hard ceiling before we save regardless
_state = {"elapsed": 0.0}


def _find_asset_collection():
    return next((c for c in bpy.data.collections if c.asset_data), None)


def _report_no_asset():
    """Say why there is nothing to render, precisely enough to act on.

    "no asset-marked collection" on its own sends you looking in the wrong
    place. Blender only renders previews for datablocks marked as assets, so
    a file whose collection was never marked is the common cause and the fix
    is Mark as Asset, not anything to do with thumbnails.
    """
    local = [c.name for c in bpy.data.collections if c.library is None]
    linked = sum(1 for c in bpy.data.collections if c.library is not None)
    print("[MapLink Thumb] ERROR: no asset-marked collection in this file.")
    print(f"[MapLink Thumb]   local collections ({len(local)}): "
          f"{', '.join(local[:8]) or 'none'}")
    print(f"[MapLink Thumb]   linked collections: {linked}")
    print("[MapLink Thumb]   Blender only renders previews for datablocks "
          "marked as assets.")
    print("[MapLink Thumb]   Right-click the collection in the Outliner > "
          "Mark as Asset, then run this again.")


def _apply_type_badge():
    """Composite the asset's type icon into the freshly rendered preview.

    Has to happen here, after the render job finishes and before the save.
    Blender regenerates previews from scratch in this path, so a badge applied
    at any earlier point would have just been thrown away -- which is exactly
    why the badge is not a save-time step.
    """
    if thumbnail_badge is None:
        return
    try:
        done, msgs = thumbnail_badge.badge_current_file()
        for m in msgs:
            print(f"[MapLink Thumb] badge {m}")
        if done:
            print(f"[MapLink Thumb] type badge applied to {done} preview(s)")
    except Exception as e:
        # A failed badge must never cost the thumbnail that was just rendered.
        print(f"[MapLink Thumb] badge skipped: {e}")


# Sentinel the caller greps for. Blender exits 0 whether or not the preview
# actually rendered -- quit_blender() does not carry a status and sys.exit from
# inside a timer is unreliable -- so success is reported in the log instead of
# through the exit code. Without this, a file with nothing to render counted as
# a successful rebuild and the operator said so.
RESULT_OK = "[MapLink Thumb] RESULT: OK"
RESULT_FAIL = "[MapLink Thumb] RESULT: FAILED"


def _save_and_quit(ok=True):
    print(RESULT_OK if ok else RESULT_FAIL)
    if not ok:
        # Nothing rendered, so there is nothing worth writing back. Saving here
        # would dirty the file in Perforce for no change at all.
        print("[MapLink Thumb] not saving — no preview was produced")
        bpy.ops.wm.quit_blender()
        return
    _apply_type_badge()
    try:
        bpy.ops.wm.save_mainfile()
        print("[MapLink Thumb] saved with updated preview")
    except Exception as e:
        print(f"[MapLink Thumb] save failed: {e}")
    print("[MapLink Thumb] done")
    bpy.ops.wm.quit_blender()


def _preview_job_running():
    try:
        return bool(bpy.app.is_job_running('RENDER_PREVIEW'))
    except Exception:
        # Unknown job-type string on this build → can't poll; rely on timeout.
        return False


def _wait_then_finish():
    _state["elapsed"] += POLL
    if _state["elapsed"] < MIN_WAIT:
        return POLL
    if _preview_job_running() and _state["elapsed"] < MAX_WAIT:
        return POLL
    _save_and_quit()
    return None


def _trigger_preview():
    coll = _find_asset_collection()
    if not coll:
        _report_no_asset()
        return False
    print(f"[MapLink Thumb] target collection: {coll.name}")

    # Clear + re-mark to reset preview state (per request); preserve any tags
    # (e.g. the SM_/LI_ tag) so re-marking doesn't strip asset metadata.
    #
    # v1.19 -- the CATALOG has to be preserved too, and was not. asset_clear()
    # throws away asset_data wholesale; re-marking produced a fresh one with an
    # empty catalog_id, so Rebuild All Thumbnails silently moved correctly
    # filed assets into "Unassigned". Tags were carried over and the catalog was
    # not, which is why the damage looked like it came from somewhere else.
    tags = [t.name for t in coll.asset_data.tags] if coll.asset_data else []
    _prev_catalog = (str(getattr(coll.asset_data, "catalog_id", "") or "")
                     if coll.asset_data else "")
    try:
        coll.asset_clear()
    except Exception as e:
        print(f"[MapLink Thumb] asset_clear skipped: {e}")
    try:
        coll.asset_mark()
    except Exception as e:
        print(f"[MapLink Thumb] asset_mark failed: {e}")
        return False
    if coll.asset_data:
        for t in tags:
            try:
                coll.asset_data.tags.new(t, skip_if_exists=True)
            except Exception:
                pass
        # Restore the catalog we just cleared, falling back to the owning
        # project's when the asset had none to begin with.
        try:
            import os as _os, sys as _sys
            _sys.path.insert(0, _os.path.dirname(_os.path.abspath(__file__)))
            import asset_catalog as ac
            cat = _prev_catalog
            if not cat or cat == ac.NIL_UUID:
                cat = ac.uuid_for_blend(bpy.data.filepath)
            if cat:
                ac.assign(coll, cat)
        except Exception as e:
            print(f"[MapLink Thumb] catalog restore skipped: {e}")

    try:
        coll.preview_ensure()
    except Exception as e:
        print(f"[MapLink Thumb] preview_ensure skipped: {e}")

    try:
        with bpy.context.temp_override(id=coll):
            bpy.ops.ed.lib_id_generate_preview()
        print("[MapLink Thumb] preview render triggered")
        return True
    except Exception as e:
        print(f"[MapLink Thumb] generate_preview failed: {e}")
        return False


def run():
    if not _trigger_preview():
        _save_and_quit(ok=False)
        return None
    bpy.app.timers.register(_wait_then_finish, first_interval=MIN_WAIT)
    return None


# Defer so the window/GPU context is fully up before we drive the preview.
bpy.app.timers.register(run, first_interval=1.5)
