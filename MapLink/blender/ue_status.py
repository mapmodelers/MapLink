"""MapLink UE-status badge — close-the-loop feedback from UE (v1.13 / 1.5.0).

The UE plugin writes a tiny per-stem JSON next to the source .blend on every
import/reimport (see MapLinkUEStatus.cpp):

    <blend_dir>/<stem>_uestatus.json
      { "state": "importing",                                  "utc": ... }
      { "state": "ok"|"failed", "code": "...", "message": "...", "utc": ... }

This module shows that as a small badge for the CURRENT file's stem.
v1.14 — the standalone "UE Link" panel is GONE; the badge now renders
inside the Settings panel (via draw_badge) and includes the .blend name:

    LI_zidane.blend — UE: importing…
    LI_zidane.blend — UE: ✓ up to date HH:MM
    LI_zidane.blend — UE: ✗ failed — <message>

Design constraints:
  - CHEAP polling: the file is only re-read when its mtime changes (checked
    at draw time); a 2 s timer tags the sidebar for redraw so the badge
    updates without mouse-over.  Never blocks.
  - GRACEFUL WHEN ABSENT: a Blender artist with no UE project (standalone
    workflow) simply sees a subdued "UE: no link".  No file, no UE, no
    manifest, no P4 — nothing errors.
  - Status files are transient/machine-local: .gitignore'd and P4-ignored,
    never added to any changelist.
"""

import datetime
import json
import os

import bpy


# ---------------------------------------------------------------------------
# Cached status read (mtime-gated)
# ---------------------------------------------------------------------------

# path -> (mtime, parsed dict or None)
_cache = {}


def _status_path() -> str:
    """Status file path for the CURRENT .blend, or '' if unsaved."""
    try:
        blend = bpy.data.filepath
        if not blend:
            return ""
        stem = os.path.splitext(os.path.basename(blend))[0]
        return os.path.join(os.path.dirname(blend), stem + "_uestatus.json")
    except Exception:
        return ""


def read_status() -> dict | None:
    """Parsed status dict for the current file, or None when absent/invalid.
    Re-reads only when the file's mtime changed — draw-safe and cheap."""
    path = _status_path()
    if not path:
        return None
    try:
        mtime = os.path.getmtime(path)
    except OSError:
        _cache.pop(path, None)
        return None
    cached = _cache.get(path)
    if cached is not None and cached[0] == mtime:
        return cached[1]
    data = None
    try:
        with open(path, "r", encoding="utf-8") as f:
            parsed = json.load(f)
        if isinstance(parsed, dict):
            data = parsed
    except Exception:
        data = None   # unreadable / partial write / bad JSON → treat as absent
    _cache[path] = (mtime, data)
    return data


def _local_hhmm(utc_iso: str) -> str:
    """'2026-07-07T12:34:56Z' (or ISO w/ offset) → local 'HH:MM', '' on error."""
    try:
        s = utc_iso.strip()
        if s.endswith("Z"):
            s = s[:-1] + "+00:00"
        dt = datetime.datetime.fromisoformat(s)
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=datetime.timezone.utc)
        return dt.astimezone().strftime("%H:%M")
    except Exception:
        return ""


# ---------------------------------------------------------------------------
# Badge drawing (shared helper + own small panel)
# ---------------------------------------------------------------------------

def _blend_label() -> str:
    """'<stem>.blend — ' prefix for the current file, '' when unsaved."""
    try:
        blend = bpy.data.filepath
        if not blend:
            return ""
        return os.path.splitext(os.path.basename(blend))[0] + ".blend — "
    except Exception:
        return ""


def draw_badge(layout):
    """Draw the badge into *layout* (v1.14: rendered from the Settings
    panel; includes the .blend name).  Never raises."""
    try:
        name = _blend_label()
        status = read_status()
        if status is None:
            row = layout.row()
            row.enabled = False          # subdued
            row.label(text=f"{name}UE: no link", icon='UNLINKED')
            return

        state = str(status.get("state", ""))
        if state == "importing":
            layout.label(text=f"{name}UE: importing…", icon='TEMP')
        elif state == "ok":
            hhmm = _local_hhmm(str(status.get("utc", "")))
            text = f"{name}UE: ✓ up to date {hhmm}".rstrip()
            layout.label(text=text, icon='CHECKMARK')
        elif state == "failed":
            msg = str(status.get("message", "")) or str(status.get("code", ""))
            layout.label(text=f"{name}UE: ✗ failed — {msg}", icon='ERROR')
        else:
            row = layout.row()
            row.enabled = False
            row.label(text=f"{name}UE: no link", icon='UNLINKED')
    except Exception:
        # A badge must never break the panel.
        pass


# ---------------------------------------------------------------------------
# Cheap redraw timer (picks up UE-side writes without mouse-over)
# ---------------------------------------------------------------------------

_POLL_SEC = 2.0
_timer_registered = False
_last_seen = None   # (path, mtime) of the last redraw trigger


def _poll_timer():
    """Tag the 3D-viewport sidebar for redraw when the status file changed.
    Runs every _POLL_SEC seconds; O(1) stat call, no file reads."""
    global _last_seen
    try:
        path = _status_path()
        current = None
        if path:
            try:
                current = (path, os.path.getmtime(path))
            except OSError:
                current = (path, None)
        if current != _last_seen:
            _last_seen = current
            wm = bpy.context.window_manager
            if wm:
                for window in wm.windows:
                    for area in window.screen.areas:
                        if area.type == 'VIEW_3D':
                            for region in area.regions:
                                if region.type == 'UI':
                                    region.tag_redraw()
    except Exception:
        pass
    return _POLL_SEC


# ---------------------------------------------------------------------------
# register / unregister (symmetric, headless-safe)
# ---------------------------------------------------------------------------
# v1.14 — no classes: the "UE Link" panel is gone (badge lives in Settings).

_classes = ()


def register():
    global _timer_registered
    for cls in _classes:
        bpy.utils.register_class(cls)
    # No UI to refresh in headless runs (workers, batch exports).
    if not bpy.app.background and not _timer_registered:
        try:
            bpy.app.timers.register(_poll_timer,
                                    first_interval=_POLL_SEC,
                                    persistent=True)
            _timer_registered = True
        except Exception:
            _timer_registered = False


def unregister():
    global _timer_registered
    if _timer_registered:
        try:
            bpy.app.timers.unregister(_poll_timer)
        except Exception:
            pass
        _timer_registered = False
    for cls in reversed(_classes):
        try:
            bpy.utils.unregister_class(cls)
        except Exception:
            pass
    _cache.clear()
