"""MapLink Batch Standardize (v1.14) — run the current-file standardization
actions over every project .blend, headlessly, one Blender subprocess per
file.

UI (Settings ▸ Batch ▸ "Batch Standardize…"):
  • Per-run toggles: Standardize VC / Standardize UVs / Clean Outliner /
    Clean Collections / Force re-save (regenerate sidecars).
  • Scope: every .blend discovered under the Batch root (recursive, files
    starting with '_' excluded), with a per-file selection table
    (checkbox per blend; Select All / None).

Execution model (locked): headless, ONE Blender subprocess per file
(maplink_force_save.py --standardize …).  Chosen over a serial load/save
loop in the running instance: it never mutates the artist's open session,
isolates a crash to a single file, and avoids undo/context bleed between
files.  Driven through the same modal pool pattern as Force-Save All, with
a per-file watchdog timeout that terminates hung workers.

Semantics per file (see maplink_force_save.py):
  • Standardize actions only mutate LI_/SM_ stems (BP_ is never touched by
    actions — re-saving a BP_ proxy can disturb its linked Blueprint).
  • Unchanged files are skipped (no checkout/save) unless Force re-save is
    ticked — then every selected file is opened + saved, which regenerates
    the sidecars (<stem>_analysis.json / <stem>_filehash.txt) through the
    normal export-on-save path.  Sidecars are never hand-deleted.
  • P4: the orchestrator pre-creates ONE pending CL ("[MapLink] Batch
    Standardize") via the shared find-or-create helper and passes it to
    every worker; only files that actually save are checked out.  Any P4 or
    per-file failure logs and continues — one file never aborts the batch.

Standalone-safe: registers with zero MapLink project configured; without a
UE plugin dir the workers still standardize/save/regenerate sidecars and
just skip the inline FBX export.
"""

import os
import subprocess
import sys
import time

import bpy
from bpy.types import Operator, PropertyGroup, UIList
from bpy.props import (
    BoolProperty,
    CollectionProperty,
    IntProperty,
    StringProperty,
)

from . import icons
from . import p4_integration
from . import force_save_all as _fsa
from . import modeling_tools as _mt   # v1.14 — popup_report


CL_DESC = "[MapLink] Batch Standardize"

# Standardize actions mutate LI_/SM_ only; Force re-save may also re-save
# BP_/unprefixed .blends (content-neutral save + sidecar/FBX regen).
_ACTION_PREFIXES = ("LI_", "SM_")


# ---------------------------------------------------------------------------
# Project-root discovery (shared with __init__'s Batch-root autofill)
# ---------------------------------------------------------------------------

def find_project_root(start_dir: str) -> str:
    """Walk up from *start_dir* to the nearest maplink_project.json.
    Returns the containing directory, or '' if none found."""
    d = os.path.abspath(start_dir or "")
    if not d or not os.path.isdir(d):
        return ""
    for _ in range(64):
        if os.path.isfile(os.path.join(d, "maplink_project.json")):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            return ""
        d = parent
    return ""


def _discover_blends(root: str):
    """Every .blend under *root* (recursive), excluding files starting with
    '_' (worker/template convention).  Sorted, absolute paths."""
    out = []
    for dirpath, _dirs, filenames in os.walk(root):
        for fn in filenames:
            if fn.endswith(".blend") and not fn.startswith("_"):
                out.append(os.path.normpath(os.path.join(dirpath, fn)))
    out.sort()
    return out


# ---------------------------------------------------------------------------
# Selection table plumbing
# ---------------------------------------------------------------------------

class MAPLINK_PG_BatchStdFile(PropertyGroup):
    enabled: BoolProperty(name="", default=True)
    path: StringProperty(name="Path", default="")
    label: StringProperty(name="File", default="")


class MAPLINK_UL_BatchStdFiles(UIList):
    def draw_item(self, context, layout, data, item, icon, active_data,
                  active_propname, index):
        row = layout.row(align=True)
        row.prop(item, "enabled", text="")
        stem = os.path.splitext(os.path.basename(item.path))[0]
        # The type icon replaces the old FILE_BLEND/FILE pair, which only ever
        # said "this row gets standardized or it doesn't".  That distinction is
        # still visible: unprefixed rows are the ones with no type icon.
        actionable = stem.startswith(_ACTION_PREFIXES)
        default = 'FILE_BLEND' if actionable else 'FILE'
        row.label(text=item.label, **icons.label_args(stem, default=default))


class MAPLINK_OT_BatchStdSelect(Operator):
    bl_idname = "maplink.batch_standardize_select"
    bl_label = "Select"
    bl_description = "Select all / none in the Batch Standardize file table"
    bl_options = {'INTERNAL'}

    select_all: BoolProperty(default=True, options={'HIDDEN'})

    def execute(self, context):
        for item in context.window_manager.maplink_batch_std_files:
            item.enabled = self.select_all
        return {'FINISHED'}


# ---------------------------------------------------------------------------
# Main operator — modal subprocess pool with watchdog
# ---------------------------------------------------------------------------

class MAPLINK_OT_BatchStandardize(Operator):
    """Standardize every selected project .blend headlessly (one Blender
    subprocess per file), with per-run action toggles and one shared P4 CL."""

    bl_idname = "maplink.batch_standardize"
    bl_label = "Batch Standardize"
    bl_options = {'REGISTER'}

    do_vc: BoolProperty(
        name="Standardize VC?", default=True,
        description="Normalise vertex colors to the single 'Color' standard "
                    "(LI_/SM_ only)")
    do_uv: BoolProperty(
        name="Standardize UVs?", default=True,
        description="Rename UV layers to uv0/uv1/uv2… in index order "
                    "(LI_/SM_ only)")
    do_outliner: BoolProperty(
        name="Clean Outliner?", default=True,
        description="Outliner name cleanup (LI_/SM_ only)")
    do_collections: BoolProperty(
        name="Clean Collections?", default=True,
        description="Collection layout cleanup + colour tags (LI_/SM_ only)")
    do_force_resave: BoolProperty(
        name="Force re-save (regenerate sidecars)?", default=False,
        description="Open and save every selected .blend even if no action "
                    "changed it — the save regenerates the "
                    "<stem>_analysis.json / <stem>_filehash.txt sidecars "
                    "through the normal export-on-save path")
    do_dry_run: BoolProperty(
        name="Dry run (report only)?", default=False,
        description="Measure the impact only: actions run in-memory per "
                    "file, NOTHING is checked out, saved or exported — the "
                    "end report lists which files would change")

    max_parallel: IntProperty(
        name="Parallel workers", default=4, min=1, max=16,
        description="How many headless Blender subprocesses to run at once")
    per_file_timeout: IntProperty(
        name="Per-file timeout (s)", default=900, min=60, max=7200,
        description="Watchdog: a worker exceeding this is terminated and "
                    "counted as failed (the batch continues)")

    # Modal state
    _timer = None
    _queue = None       # list[str]
    _running = None     # list[(Popen, path, log_path, t0)]
    _done_saved = None  # list[(path, elapsed)]
    _done_skip = None   # list[path]
    _done_would = None  # list[path] — dry run: would change/save (rc 9)
    _done_fail = None   # list[(path, log_path, rc)]
    _start_time = 0.0
    _plugin_dir = ''
    _artifact_root = ''
    _log_dir = ''
    _cl = 0
    _actions = ''
    _total = 0

    # ------------------------------------------------------------------ UI

    def invoke(self, context, event):
        # Resolve the Batch root (item 1: auto-defaulted from the project
        # manifest; still overridable in Settings ▸ Batch).
        try:
            props = context.scene.maplink_batch
        except (AttributeError, TypeError):
            self.report({'ERROR'}, "Batch properties not available.")
            return {'CANCELLED'}

        root = bpy.path.abspath(props.root_path.strip()) \
            if props.root_path.strip() else ""
        if not root and bpy.data.filepath:
            root = find_project_root(os.path.dirname(bpy.data.filepath))
            if root:
                props.root_path = root
        if not root or not os.path.isdir(root):
            self.report({'ERROR'},
                        "Set a valid Batch root first (Settings ▸ Batch — "
                        "auto-filled when the current .blend belongs to a "
                        "MapLink project).")
            return {'CANCELLED'}
        self._root = root

        # Discover + populate the selection table.
        blends = _discover_blends(root)
        if not blends:
            self.report({'WARNING'}, f"No .blend files found under {root}")
            return {'CANCELLED'}

        wm = context.window_manager
        wm.maplink_batch_std_files.clear()
        for p in blends:
            item = wm.maplink_batch_std_files.add()
            item.path = p
            item.enabled = True
            item.label = os.path.relpath(p, root)
        wm.maplink_batch_std_index = 0

        return wm.invoke_props_dialog(self, width=520)

    def draw(self, context):
        layout = self.layout
        wm = context.window_manager

        box = layout.box()
        box.label(text="Actions (identical to the single-file Tools "
                       "actions):", icon='CHECKMARK')
        col = box.column(align=True)
        col.prop(self, "do_vc")
        col.prop(self, "do_uv")
        col.prop(self, "do_outliner")
        col.prop(self, "do_collections")
        box.separator()
        box.prop(self, "do_force_resave")
        box.prop(self, "do_dry_run")
        if (self.do_vc or self.do_uv) and not self.do_dry_run:
            box.label(text="WARNING: VC/UV standardization permanently "
                           "alters mesh data.", icon='ERROR')

        row = layout.row(align=True)
        row.prop(self, "max_parallel")
        row.prop(self, "per_file_timeout")

        n_sel = sum(1 for i in wm.maplink_batch_std_files if i.enabled)
        n_all = len(wm.maplink_batch_std_files)
        layout.label(text=f"Scope: {n_sel}/{n_all} .blend under "
                          f"{getattr(self, '_root', '')}", icon='FILE_FOLDER')
        row = layout.row(align=True)
        op = row.operator("maplink.batch_standardize_select", text="Select All")
        op.select_all = True
        op = row.operator("maplink.batch_standardize_select", text="Select None")
        op.select_all = False
        layout.template_list(
            "MAPLINK_UL_BatchStdFiles", "", wm, "maplink_batch_std_files",
            wm, "maplink_batch_std_index", rows=8)
        layout.label(text="LI_/SM_ get the ticked actions; other .blends "
                          "only participate in Force re-save.", icon='INFO')

    # ------------------------------------------------------------- execute

    def execute(self, context):
        wm = context.window_manager
        files = [i.path for i in wm.maplink_batch_std_files if i.enabled]
        if not files:
            self.report({'ERROR'}, "No files selected.")
            return {'CANCELLED'}

        actions = []
        if self.do_vc:
            actions.append('vc')
        if self.do_uv:
            actions.append('uv')
        if self.do_outliner:
            actions.append('outliner')
        if self.do_collections:
            actions.append('collections')
        if not actions and not self.do_force_resave:
            self.report({'ERROR'},
                        "Nothing to do — tick at least one action or "
                        "Force re-save.")
            return {'CANCELLED'}
        self._actions = ",".join(actions)

        # Plugin dir / artifact root are optional (standalone-safe): without
        # them the workers skip the inline FBX export but still save +
        # regenerate sidecars.
        self._plugin_dir = _fsa._get_plugin_dir()
        self._artifact_root = _fsa._get_artifact_root(self._plugin_dir) \
            if self._plugin_dir else ''
        log_base = self._artifact_root or self._plugin_dir
        if not log_base:
            import tempfile
            log_base = os.path.join(tempfile.gettempdir(), "maplink")
        self._log_dir = os.path.join(log_base, 'Logs', 'batch_standardize')
        try:
            os.makedirs(self._log_dir, exist_ok=True)
        except OSError as exc:
            self.report({'ERROR'}, f"Cannot create log dir: {exc}")
            return {'CANCELLED'}

        # ONE pending CL for the whole batch, pre-created here so parallel
        # workers can't race a duplicate create.  Non-fatal on failure.
        # Dry run never touches P4.
        self._cl = 0
        try:
            if self.do_dry_run:
                print("[batch-std] DRY RUN — no P4, no saves, no exports")
            elif p4_integration.p4_available():
                cl = p4_integration._get_or_create_changelist(CL_DESC)
                if cl is not None and cl > 0:
                    self._cl = cl
                    print(f"[batch-std] P4 CL {cl}: {CL_DESC}")
                else:
                    print("[batch-std] P4 WARN: no CL — workers fall back "
                          "to description matching")
            else:
                print("[batch-std] P4 not available — read-only bits will "
                      "be cleared directly")
        except Exception as exc:
            print(f"[batch-std] P4 error (non-fatal): {exc}")

        self._queue = files
        self._running = []
        self._done_saved = []
        self._done_skip = []
        self._done_would = []
        self._done_fail = []
        self._total = len(files)
        self._start_time = time.monotonic()

        print(f"[batch-std] Starting: {self._total} file(s), "
              f"actions=[{self._actions or 'none'}], "
              f"force_resave={self.do_force_resave}, "
              f"dry_run={self.do_dry_run}, "
              f"parallel={self.max_parallel}, "
              f"timeout={self.per_file_timeout}s")

        self._timer = context.window_manager.event_timer_add(
            0.5, window=context.window)
        context.window_manager.modal_handler_add(self)
        self._update_header(context)
        return {'RUNNING_MODAL'}

    # --------------------------------------------------------------- modal

    def modal(self, context, event):
        if event.type == 'ESC':
            self.report({'WARNING'}, "Batch Standardize cancelled by user.")
            self._cancel_running()
            self._finish(context)
            return {'CANCELLED'}

        if event.type != 'TIMER':
            return {'PASS_THROUGH'}

        now = time.monotonic()

        # 1. Reap finished / watchdog-terminate hung subprocesses.
        still_running = []
        for proc, path, log_path, t0 in self._running:
            rc = proc.poll()
            if rc is None:
                if now - t0 > self.per_file_timeout:
                    # Watchdog: terminate and count as failed; batch continues.
                    try:
                        proc.terminate()
                    except Exception:
                        pass
                    self._done_fail.append((path, log_path, 'timeout'))
                    print(f"[batch-std] TIMEOUT ({self.per_file_timeout}s) "
                          f"{os.path.basename(path)} — terminated")
                else:
                    still_running.append((proc, path, log_path, t0))
            elif rc == 0:
                self._done_saved.append((path, now - t0))
                print(f"[batch-std] SAVED {os.path.basename(path)} "
                      f"({now - t0:.1f}s)")
            elif rc == 9:
                self._done_would.append(path)
                print(f"[batch-std] WOULD {os.path.basename(path)} "
                      f"(dry run: would change/save)")
            elif rc == 7:
                self._done_skip.append(path)
                print(f"[batch-std] SKIP  {os.path.basename(path)} "
                      f"(unchanged)")
            else:
                self._done_fail.append((path, log_path, rc))
                print(f"[batch-std] FAIL  {os.path.basename(path)} rc={rc} "
                      f"— log: {log_path}")
        self._running = still_running

        # 2. Top up the pool.
        while self._queue and len(self._running) < self.max_parallel:
            path = self._queue.pop(0)
            if not os.path.isfile(path):
                self._done_fail.append((path, '', 'missing'))
                print(f"[batch-std] FAIL  missing file: {path}")
                continue
            proc, log_path = self._spawn(path)
            if proc is None:
                self._done_fail.append((path, log_path, 'spawn'))
                continue
            self._running.append((proc, path, log_path, time.monotonic()))

        # 3. Header.
        self._update_header(context)

        # 4. Done?
        if not self._running and not self._queue:
            self._finish(context)
            elapsed = time.monotonic() - self._start_time
            n_ok, n_skip, n_would, n_fail = (len(self._done_saved),
                                             len(self._done_skip),
                                             len(self._done_would),
                                             len(self._done_fail))
            if self.do_dry_run:
                summary = (f"Batch Standardize DRY RUN done in {elapsed:.1f}s "
                           f"— {n_would} would change/save, "
                           f"{n_skip} would skip, {n_fail} failed")
            else:
                summary = (f"Batch Standardize done in {elapsed:.1f}s — "
                           f"{n_ok} saved, {n_skip} skipped (unchanged), "
                           f"{n_fail} failed"
                           + (f" (CL {self._cl})" if self._cl else ""))
            print(f"[batch-std] {summary}")
            for path, log_path, rc in self._done_fail:
                print(f"[batch-std]   FAILED rc={rc}: {path}"
                      + (f" — {log_path}" if log_path else ""))
            self.report({'WARNING' if n_fail else 'INFO'}, summary)

            # v1.14 — explicit end-of-batch report popup.
            lines = [summary]
            if self.do_dry_run and self._done_would:
                lines.append("Would change/save:")
                lines += [f"   {os.path.basename(p)}"
                          for p in self._done_would[:12]]
                if n_would > 12:
                    lines.append(f"   … and {n_would - 12} more")
            if self._done_fail:
                lines.append("Failed:")
                lines += [f"   {os.path.basename(p)} (rc={rc})"
                          for p, _lp, rc in self._done_fail[:12]]
                if n_fail > 12:
                    lines.append(f"   … and {n_fail - 12} more")
                lines.append(f"Logs: {self._log_dir}")
            _mt.popup_report(
                "Batch Standardize — dry run" if self.do_dry_run
                else "Batch Standardize — done",
                lines,
                icon='ERROR' if n_fail else 'CHECKMARK')
            return {'FINISHED'}

        return {'RUNNING_MODAL'}

    # ------------------------------------------------------------- helpers

    def _spawn(self, blend_path):
        stem = os.path.splitext(os.path.basename(blend_path))[0]
        log_path = os.path.join(self._log_dir, f'batch_std_{stem}.log')

        script = os.path.join(os.path.dirname(__file__),
                              'maplink_force_save.py')
        args = [
            bpy.app.binary_path,
            '--background',
            '--python', script,
            '--',
            '--blend',         blend_path,
            '--plugin-dir',    self._plugin_dir or '',
            '--artifact-root', self._artifact_root or '',
            '--addon-package', __package__.split('.')[0],
            '--cl-desc',       CL_DESC,
        ]
        if self._actions:
            args += ['--standardize', self._actions]
        if not self.do_force_resave:
            args += ['--skip-unchanged']
        if self.do_dry_run:
            args += ['--dry-run', '--no-p4']
        if self._cl:
            args += ['--cl', str(self._cl)]

        creationflags = 0
        if sys.platform.startswith('win'):
            for f in ('CREATE_NO_WINDOW', 'DETACHED_PROCESS',
                      'CREATE_NEW_PROCESS_GROUP'):
                creationflags |= getattr(subprocess, f, 0)

        try:
            log_fh = open(log_path, 'wb')
        except OSError as exc:
            print(f"[batch-std] Could not open log {log_path}: {exc}")
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
            print(f"[batch-std] Popen failed for {stem}: {exc}")
            return None, log_path

    def _update_header(self, context):
        if not context.area:
            return
        done = (len(self._done_saved) + len(self._done_skip)
                + len(self._done_fail))
        msg = (f"MapLink Batch Standardize: {done}/{self._total} done "
               f"({len(self._done_saved)} saved, "
               f"{len(self._done_skip)} skipped, "
               f"{len(self._done_fail)} failed), "
               f"{len(self._running)} running, {len(self._queue)} queued "
               f"— Esc to cancel")
        try:
            context.area.header_text_set(msg)
        except Exception:
            pass

    def _cancel_running(self):
        for proc, _p, _l, _t in self._running or []:
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

_classes = (
    MAPLINK_PG_BatchStdFile,
    MAPLINK_UL_BatchStdFiles,
    MAPLINK_OT_BatchStdSelect,
    MAPLINK_OT_BatchStandardize,
)


def register():
    for c in _classes:
        bpy.utils.register_class(c)
    bpy.types.WindowManager.maplink_batch_std_files = CollectionProperty(
        type=MAPLINK_PG_BatchStdFile)
    bpy.types.WindowManager.maplink_batch_std_index = IntProperty(default=0)


def unregister():
    try:
        del bpy.types.WindowManager.maplink_batch_std_files
        del bpy.types.WindowManager.maplink_batch_std_index
    except Exception:
        pass
    for c in reversed(_classes):
        try:
            bpy.utils.unregister_class(c)
        except Exception:
            pass
