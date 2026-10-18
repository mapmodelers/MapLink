"""MapLink Force-Save All — save the current .blend + every referenced .blend
recursively, going through the full pipeline (P4 checkout → save → sidecars
→ FBX export → P4 add).

v1.4.1 — Initial release.

Design summary:
 * Current .blend → saved in-process via bpy.ops.wm.save_mainfile().  All
   existing handlers fire (nav_drill save_pre P4 checkout, sidecar_analysis
   save_post, fbx_export_on_save save_post subprocess spawn).
 * Referenced .blends → each runs in a parallel headless Blender subprocess
   via maplink_force_save.py, which performs the same full pipeline inline.
 * Discovery is in-process and fast: bpy.data.libraries of the current .blend
   + recursive walk of each dep's <stem>_analysis.json sidecar.  No subprocess
   needed just to discover deps as long as sidecars exist.
 * Modal operator with timer: parallel pool of <max_parallel> subprocesses,
   non-blocking UI.  Cancel via Esc.
"""

import json
import os
import stat
import subprocess
import sys
import time

import bpy
from bpy.types import Operator
from bpy.props import IntProperty, BoolProperty

from . import p4_integration


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

_MAPLINK_PREFIXES = ("BP_", "LI_", "SM_")


# ---------------------------------------------------------------------------
# Plugin-dir resolution (mirrors fbx_export_on_save.py)
# ---------------------------------------------------------------------------

def _get_plugin_dir() -> str:
    try:
        from . import fbx_export_on_save
        path = fbx_export_on_save.resolve_plugin_dir()
        if path:
            return path
    except Exception:
        pass
    return os.environ.get("MAPLINK_PLUGIN_DIR", "").rstrip(os.sep + '/')


def _get_artifact_root(plugin_dir: str) -> str:
    """v1.13: artifact root for Logs/Export output (legacy plugin dir when
    the preference isn't set — one release of grace)."""
    try:
        from . import fbx_export_on_save
        return fbx_export_on_save.resolve_outdir(plugin_dir)
    except Exception:
        pass
    return os.environ.get("MAPLINK_ARTIFACT_ROOT", "").rstrip(os.sep + '/') \
        or plugin_dir


# ---------------------------------------------------------------------------
# Discovery — recursive set of .blends starting from current
# ---------------------------------------------------------------------------

def _read_sidecar_libs(blend_path: str):
    """Return absolute paths of libraries listed in <stem>_analysis.json
    next to the given .blend.  Returns [] if the sidecar is missing or
    malformed."""
    blend_dir = os.path.dirname(blend_path)
    stem = os.path.splitext(os.path.basename(blend_path))[0]
    sidecar_path = os.path.join(blend_dir, f"{stem}_analysis.json")
    if not os.path.isfile(sidecar_path):
        return []
    try:
        with open(sidecar_path, 'r', encoding='utf-8') as f:
            report = json.load(f)
    except Exception as exc:
        print(f"[force-save] WARN: could not read sidecar for {stem}: {exc}")
        return []

    deps = []
    for lib in report.get('libraries', []) or []:
        fp = lib.get('filepath', '')
        if fp:
            deps.append(os.path.normpath(os.path.abspath(fp)))
    return deps


def _current_blend_libs() -> list:
    """Walk bpy.data.libraries for the currently-open .blend.  Returns absolute,
    normalized library paths (only BP_/LI_/SM_ prefixed)."""
    out = []
    for lib in bpy.data.libraries:
        abs_path = os.path.normpath(os.path.abspath(bpy.path.abspath(lib.filepath)))
        stem = os.path.splitext(os.path.basename(abs_path))[0]
        if stem.startswith(_MAPLINK_PREFIXES):
            out.append(abs_path)
    return out


def _is_maplink_blend(path: str) -> bool:
    stem = os.path.splitext(os.path.basename(path))[0]
    return stem.startswith(_MAPLINK_PREFIXES)


def discover_recursive(current_blend: str):
    """Return (current, refs) where:
        - current is the absolute path of the current .blend (if maplink-prefixed)
        - refs is a sorted list of absolute paths of all referenced .blends,
          recursively, NOT including current.
    Missing dep files are skipped with a warning.  Cycles are handled."""
    current_abs = os.path.normpath(os.path.abspath(current_blend))
    seen = {current_abs}
    queue = list(_current_blend_libs())

    while queue:
        path = queue.pop(0)
        if path in seen:
            continue
        seen.add(path)
        if not os.path.isfile(path):
            print(f"[force-save] WARN: missing dep, skipping: {path}")
            continue
        if not _is_maplink_blend(path):
            continue
        for dep in _read_sidecar_libs(path):
            if dep not in seen and _is_maplink_blend(dep):
                queue.append(dep)

    refs = sorted(p for p in seen if p != current_abs and _is_maplink_blend(p))
    return current_abs, refs


# ---------------------------------------------------------------------------
# Subprocess plumbing
# ---------------------------------------------------------------------------

def _blender_exe() -> str:
    """The blender.exe to use for subprocesses — the same one running this addon."""
    return bpy.app.binary_path


def _addon_package_name() -> str:
    """The Python package name of this addon (e.g. 'MapLink_BlenderV1_4_1').
    This is needed by the subprocess script to enable the addon."""
    return __package__.split('.')[0]


def _force_save_script_path() -> str:
    """Path to maplink_force_save.py inside this addon."""
    return os.path.join(os.path.dirname(__file__), 'maplink_force_save.py')


def _spawn_force_save(blend_path: str, plugin_dir: str, log_dir: str,
                      artifact_root: str):
    """Spawn one detached force-save subprocess.  Returns (Popen, log_path)
    or (None, log_path) on failure."""
    stem = os.path.splitext(os.path.basename(blend_path))[0]
    log_path = os.path.join(log_dir, f'force_save_{stem}.log')

    args = [
        _blender_exe(),
        '--background',
        '--python', _force_save_script_path(),
        '--',
        '--blend',         blend_path,
        '--plugin-dir',    plugin_dir,
        '--artifact-root', artifact_root,
        '--addon-package', _addon_package_name(),
    ]

    creationflags = 0
    if sys.platform.startswith('win'):
        for f in ('CREATE_NO_WINDOW', 'DETACHED_PROCESS',
                  'CREATE_NEW_PROCESS_GROUP'):
            creationflags |= getattr(subprocess, f, 0)

    try:
        log_fh = open(log_path, 'wb')
    except OSError as exc:
        print(f"[force-save] Could not open log {log_path}: {exc}")
        return None, log_path
    try:
        proc = subprocess.Popen(
            args,
            stdout=log_fh,
            stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
            creationflags=creationflags,
            close_fds=True,
        )
        return proc, log_path
    except OSError as exc:
        log_fh.close()
        print(f"[force-save] Popen failed for {stem}: {exc}")
        return None, log_path


# ---------------------------------------------------------------------------
# Modal operator
# ---------------------------------------------------------------------------

class MAPLINK_OT_ForceSaveAll(Operator):
    """Save the current .blend + every referenced .blend (recursive) through
    the full MapLink pipeline: P4 checkout, sidecars, FBX export, P4 add."""

    bl_idname  = "maplink.force_save_all"
    bl_label   = "Force-Save All (recursive)"
    bl_options = {'REGISTER'}

    max_parallel: IntProperty(
        name="Parallel workers",
        description="How many headless Blender subprocesses to run at once",
        default=4, min=1, max=16,
    )

    confirmed: BoolProperty(default=False, options={'HIDDEN'})

    # Modal state (instance attrs, populated in execute()).
    _timer       = None
    _queue       = None    # list[str]  remaining .blend paths
    _running     = None    # list[(Popen, blend_path, log_path, t0)]
    _done_ok     = None    # list[(blend_path, elapsed)]
    _done_fail   = None    # list[(blend_path, log_path, elapsed, rc)]
    _start_time  = 0.0
    _plugin_dir  = ''
    _artifact_root = ''
    _log_dir     = ''
    _refs_total  = 0

    # -----------------------------------------------------------------------

    @classmethod
    def poll(cls, context):
        return bool(bpy.data.filepath)

    def invoke(self, context, event):
        # Pre-flight check: file must be saved at least once on disk.
        if not bpy.data.filepath:
            self.report({'ERROR'}, "Current .blend is unsaved — save it first.")
            return {'CANCELLED'}

        # Resolve plugin dir
        self._plugin_dir = _get_plugin_dir()
        if not self._plugin_dir or not os.path.isdir(self._plugin_dir):
            self.report({'ERROR'},
                "MapLink plugin dir not configured.  Set Preferences > "
                "Add-ons > MapLink Blender > 'UE plugin dir', or "
                "MAPLINK_PLUGIN_DIR env var.")
            return {'CANCELLED'}

        # Discover.  Stash the count for the confirm dialog.
        current_abs, refs = discover_recursive(bpy.data.filepath)
        if not _is_maplink_blend(current_abs):
            self.report({'WARNING'},
                "Current .blend is not BP_/LI_/SM_ prefixed; "
                "only refs will be force-saved.")
        self._pending_refs = refs
        self._pending_current = current_abs
        self._refs_total = len(refs)

        # Confirmation dialog
        return context.window_manager.invoke_props_dialog(self, width=420)

    def draw(self, context):
        layout = self.layout
        layout.label(text=f"Force-save will mutate {self._refs_total + 1} .blend file(s):",
                     icon='ERROR')
        col = layout.column(align=True)
        col.label(text=f"  • Current:    {os.path.basename(self._pending_current)}")
        col.label(text=f"  • Referenced: {self._refs_total} file(s) "
                       f"(P4 checkout + save + FBX export)")
        layout.separator()
        layout.prop(self, "max_parallel")
        layout.separator()
        layout.label(text="Press OK to start; the operator runs in the background.",
                     icon='INFO')

    # -----------------------------------------------------------------------

    def execute(self, context):
        current_abs = self._pending_current
        refs        = list(self._pending_refs)

        # Set up logging dir under plugin Logs.
        self._artifact_root = _get_artifact_root(self._plugin_dir)
        self._log_dir = os.path.join(self._artifact_root, 'Logs', 'force_save_all')
        os.makedirs(self._log_dir, exist_ok=True)

        # ----- Save current .blend in-process -----
        # All existing handlers fire: P4 checkout (nav_drill _on_save_pre,
        # but only during drill — see explicit P4 checkout below),
        # sidecars (sidecar_analysis _on_save_post), FBX subprocess
        # (fbx_export_on_save _on_save_post_fbx).  We do not block on the
        # FBX subprocess — it runs in parallel with the ref subprocesses.
        if _is_maplink_blend(current_abs):
            # v1.4.3 — Robust make-writable: try P4 checkout, then chmod
            # fallback if the file is still read-only.  Covers cases where
            # the file isn't in P4 at all (local-only), p4 isn't on PATH,
            # or the checkout silently fails.
            self._make_writable_or_warn(current_abs)

            try:
                print(f"[force-save] Saving current .blend in-process: {current_abs}")
                bpy.ops.wm.save_mainfile()
            except Exception as exc:
                self.report({'ERROR'}, f"save_mainfile failed: {exc}")
                return {'CANCELLED'}

        # ----- Set up modal state for refs -----
        self._queue     = refs
        self._running   = []
        self._done_ok   = []
        self._done_fail = []
        self._start_time = time.monotonic()

        if not self._queue:
            self.report({'INFO'}, "Current .blend saved; no refs to process.")
            return {'FINISHED'}

        # Kick off the timer.
        self._timer = context.window_manager.event_timer_add(0.5, window=context.window)
        context.window_manager.modal_handler_add(self)
        self._update_header(context)
        return {'RUNNING_MODAL'}

    # -----------------------------------------------------------------------

    def modal(self, context, event):
        if event.type == 'ESC':
            self.report({'WARNING'}, "Force-save cancelled by user.")
            self._cancel_running()
            self._finish(context)
            return {'CANCELLED'}

        if event.type != 'TIMER':
            return {'PASS_THROUGH'}

        # 1. Reap any finished subprocesses.
        still_running = []
        for proc, blend_path, log_path, t0 in self._running:
            rc = proc.poll()
            if rc is None:
                still_running.append((proc, blend_path, log_path, t0))
            else:
                elapsed = time.monotonic() - t0
                if rc == 0:
                    self._done_ok.append((blend_path, elapsed))
                    print(f"[force-save] OK   {os.path.basename(blend_path)} ({elapsed:.1f}s)")
                else:
                    self._done_fail.append((blend_path, log_path, elapsed, rc))
                    print(f"[force-save] FAIL {os.path.basename(blend_path)} "
                          f"rc={rc} ({elapsed:.1f}s) — log: {log_path}")
        self._running = still_running

        # 2. Top up the pool.
        while self._queue and len(self._running) < self.max_parallel:
            blend_path = self._queue.pop(0)
            if not os.path.isfile(blend_path):
                self._done_fail.append((blend_path, '', 0.0, -1))
                print(f"[force-save] SKIP (missing file): {blend_path}")
                continue
            proc, log_path = _spawn_force_save(
                blend_path, self._plugin_dir, self._log_dir,
                self._artifact_root)
            if proc is None:
                self._done_fail.append((blend_path, log_path, 0.0, -1))
                continue
            self._running.append((proc, blend_path, log_path, time.monotonic()))

        # 3. Header update.
        self._update_header(context)

        # 4. Done?
        if not self._running and not self._queue:
            self._finish(context)
            elapsed = time.monotonic() - self._start_time
            ok = len(self._done_ok)
            fail = len(self._done_fail)
            if fail == 0:
                self.report({'INFO'},
                    f"Force-save complete in {elapsed:.1f}s — "
                    f"{ok} ref(s) ok.")
            else:
                self.report({'WARNING'},
                    f"Force-save done in {elapsed:.1f}s — "
                    f"{ok} ok, {fail} failed.  Logs in {self._log_dir}")
            return {'FINISHED'}

        return {'RUNNING_MODAL'}

    # -----------------------------------------------------------------------

    def _make_writable_or_warn(self, path: str) -> None:
        """v1.4.3 — Belt-and-suspenders: try p4 edit, then strip Windows
        read-only bit if needed.  Reports a WARNING to the user if we had to
        fall through to chmod, so they know P4 didn't catch this file."""
        p4_ok = False
        p4_msg = ""
        try:
            if p4_integration.p4_available():
                p4_ok, p4_msg = p4_integration.checkout_file(
                    path, description="[MapLink] Force-Save All")
                print(f"[force-save] P4 checkout {'OK' if p4_ok else 'WARN'}: "
                      f"{path} — {p4_msg}")
            else:
                print(f"[force-save] P4 not available (skipped checkout for {path})")
                p4_msg = "p4 not on PATH"
        except Exception as exc:
            print(f"[force-save] P4 checkout exception (non-fatal): {exc}")
            p4_msg = str(exc)

        # If the file is still read-only after the P4 attempt, force-clear it.
        try:
            if os.path.isfile(path) and not os.access(path, os.W_OK):
                os.chmod(path, os.stat(path).st_mode | stat.S_IWRITE)
                print(f"[force-save] chmod +w applied: {path}")
                if not p4_ok:
                    # User probably wants to know we bypassed P4 here.
                    self.report({'WARNING'},
                        f"{os.path.basename(path)}: bypassed P4 "
                        f"(reason: {p4_msg or 'unknown'}) — file is now "
                        f"writable but not opened for edit in P4.")
        except OSError as exc:
            print(f"[force-save] chmod failed for {path}: {exc}")

    # -----------------------------------------------------------------------

    def _update_header(self, context):
        if not context.area:
            return
        total = self._refs_total
        done  = len(self._done_ok) + len(self._done_fail)
        run   = len(self._running)
        msg = (f"MapLink Force-Save: {done}/{total} done "
               f"({len(self._done_fail)} failed), "
               f"{run} running, {len(self._queue)} queued — Esc to cancel")
        try:
            context.area.header_text_set(msg)
        except Exception:
            pass

    def _cancel_running(self):
        for proc, _bp, _lp, _t0 in self._running or []:
            try:
                proc.terminate()
            except Exception:
                pass

    def _finish(self, context):
        if self._timer is not None:
            try:
                context.window_manager.event_timer_remove(self._timer)
            except Exception:
                pass
            self._timer = None
        if context.area:
            try:
                context.area.header_text_set(None)
            except Exception:
                pass

    def cancel(self, context):
        self._cancel_running()
        self._finish(context)


# ---------------------------------------------------------------------------
# Registration
# ---------------------------------------------------------------------------

def register():
    bpy.utils.register_class(MAPLINK_OT_ForceSaveAll)


def unregister():
    bpy.utils.unregister_class(MAPLINK_OT_ForceSaveAll)
