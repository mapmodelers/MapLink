"""MapLink FBX export-on-save — eliminates UE-side Blender launches on import.

v1.4.0 — Initial release.

The MapLink_Blender addon already writes sidecar analysis.json + filehash.txt
on save_post (see sidecar_analysis.py).  This module ADDITIONALLY spawns a
detached Blender subprocess to produce the FBX file and refresh the plugin
cache (analysis.json + filehash.txt in <artifact_root>/Logs/<stem>/).

v1.13 — Artifact relocation.  Artifacts no longer live in the UE plugin dir
(read-only for engine-installed marketplace plugins).  The output root is the
"artifact root": <UEProject>/Saved/MapLink, fed by UE into the addon's
maplink_artifact_root preference at addon install.  When the preference is
missing (addon updated before relinking from UE) we fall back to the legacy
plugin-dir layout for one release, with a one-time console note.

The subprocess is fire-and-forget — Blender's UI is never blocked.  The UE
plugin's pre-flight check (v1.7.5+) sees the freshly-populated cache and
skips its own Blender launch entirely, turning the cold-import case from
N × (1.8–3 s subprocess + startup) into N × (~0 ms file existence checks).

Plugin path resolution order (script location only, never written to):
    1. Addon preferences:  maplink_plugin_dir
    2. Environment variable: MAPLINK_PLUGIN_DIR
    3. Skip silently (with a one-time warning per session)

Artifact root resolution order (where artifacts are written):
    1. Addon preferences:  maplink_artifact_root
    2. Environment variable: MAPLINK_ARTIFACT_ROOT
    3. Legacy fallback: the plugin dir (one release of grace)

The subprocess is launched as:
    <bpy.app.binary_path>
        --background --factory-startup
        --python <plugin>/Scripts/maplink_export.py
        --
        --mode both --blend <this.blend> --outdir <artifact_root> --lock

Lock file: <artifact_root>/Export/<stem>/.export.lock — created by maplink_export.py
at start, removed on success or exception.  The UE plugin waits on this lock
(up to ~8 s) before launching its own Blender for the same .blend.
"""

import os
import sys
import subprocess
import traceback

import bpy
from bpy.app.handlers import persistent


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

_MAPLINK_PREFIXES = ("BP_", "LI_", "SM_")
_ENV_PLUGIN_DIR   = "MAPLINK_PLUGIN_DIR"
_ENV_ARTIFACT_ROOT = "MAPLINK_ARTIFACT_ROOT"

# Module-level state — single warning per session, tracked launches.
_warned_no_plugin_dir = False
_warned_legacy_artifact_root = False
_active_launches      = {}  # blend_path -> subprocess.Popen


# ---------------------------------------------------------------------------
# Plugin-dir resolution
# ---------------------------------------------------------------------------

def _get_addon_prefs():
    """Return this addon's AddonPreferences object, or None if not yet
    registered.  Looks up by package name (the parent package of this module)."""
    try:
        pkg = __package__
        if not pkg:
            return None
        addons = bpy.context.preferences.addons
        if pkg in addons:
            return addons[pkg].preferences
    except (AttributeError, KeyError):
        pass
    return None


def resolve_plugin_dir() -> str:
    """Resolve the UE plugin base directory.  Returns empty string if not set."""
    prefs = _get_addon_prefs()
    if prefs is not None:
        path = getattr(prefs, 'maplink_plugin_dir', '') or ''
        if path:
            return bpy.path.abspath(path).rstrip(os.sep + '/')
    env = os.environ.get(_ENV_PLUGIN_DIR, '').strip()
    if env:
        return env.rstrip(os.sep + '/')
    return ''


def resolve_artifact_root() -> str:
    """v1.13: Resolve the artifact root (<UEProject>/Saved/MapLink), where all
    generated artifacts (Logs/, Export/) are written.  Fed by UE into the
    maplink_artifact_root preference at addon install.  Empty if unset."""
    prefs = _get_addon_prefs()
    if prefs is not None:
        path = getattr(prefs, 'maplink_artifact_root', '') or ''
        if path:
            return bpy.path.abspath(path).rstrip(os.sep + '/')
    env = os.environ.get(_ENV_ARTIFACT_ROOT, '').strip()
    if env:
        return env.rstrip(os.sep + '/')
    return ''


def resolve_outdir(plugin_dir: str) -> str:
    """The --outdir passed to maplink_export.py: the artifact root when
    configured, else the legacy plugin dir (one release of grace, v1.13)."""
    root = resolve_artifact_root()
    if root:
        return root
    global _warned_legacy_artifact_root
    if not _warned_legacy_artifact_root:
        _warned_legacy_artifact_root = True
        print(
            "[MapLink] Note: 'Artifact root' preference not set — writing "
            "artifacts to the legacy plugin-dir layout.\n"
            "[MapLink]   Re-run 'Install/Update Blender Addon' from UE "
            "(MapLink v1.13+) to configure it; the legacy fallback will be "
            "removed in a future release."
        )
    return plugin_dir


def _warn_no_plugin_dir_once():
    global _warned_no_plugin_dir
    if _warned_no_plugin_dir:
        return
    _warned_no_plugin_dir = True
    print(
        "[MapLink] FBX-export-on-save disabled: plugin directory not set.\n"
        "[MapLink]   Set Edit > Preferences > Add-ons > MapLink > 'UE plugin dir'\n"
        f"[MapLink]   or the {_ENV_PLUGIN_DIR} environment variable.\n"
        "[MapLink]   UE imports will fall back to launching Blender themselves "
        "(no regression — just no speed-up)."
    )


# ---------------------------------------------------------------------------
# Subprocess launch
# ---------------------------------------------------------------------------

def _is_maplink_blend(blend_path: str) -> bool:
    stem = os.path.splitext(os.path.basename(blend_path))[0]
    return stem.startswith(_MAPLINK_PREFIXES)


def _build_command(blender_exe: str, plugin_dir: str, blend_path: str,
                   outdir: str) -> list:
    script = os.path.join(plugin_dir, 'Scripts', 'maplink_export.py')
    return [
        blender_exe,
        '--background',
        '--factory-startup',
        # v1.21: a Python exception in the worker must become a non-zero exit
        # code.  Blender's default is to print the traceback and exit 0, which
        # made every failed save-time export look like a success.
        '--python-exit-code', '1',
        '--python', script,
        '--',
        '--mode',   'both',
        '--blend',  blend_path,
        '--outdir', outdir,
        '--lock',
    ]


def _spawn_detached(args, log_path: str):
    """Spawn the export subprocess detached from this Blender.  Returns the
    Popen handle (caller may track it) or None on failure."""
    creationflags = 0
    if sys.platform.startswith('win'):
        for flag_name in ('CREATE_NO_WINDOW', 'DETACHED_PROCESS',
                          'CREATE_NEW_PROCESS_GROUP'):
            flag = getattr(subprocess, flag_name, 0)
            creationflags |= flag

    os.makedirs(os.path.dirname(log_path), exist_ok=True)
    # 'wb' + close_fds so the log file handle isn't held by *this* Blender.
    log_fh = open(log_path, 'wb')
    try:
        proc = subprocess.Popen(
            args,
            stdout=log_fh,
            stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
            creationflags=creationflags,
            close_fds=True,
        )
    except OSError as exc:
        log_fh.close()
        print(f"[MapLink] FBX export subprocess failed to start: {exc}")
        return None
    # Don't close log_fh here — the subprocess inherits it.  Python's GC will
    # release our handle when proc is collected; the subprocess keeps its own.
    return proc


# ---------------------------------------------------------------------------
# save_post handler
# ---------------------------------------------------------------------------

@persistent
def _on_save_post_fbx(filepath_unused):
    try:
        blend_path = bpy.data.filepath
        if not blend_path:
            return
        if not _is_maplink_blend(blend_path):
            return

        plugin_dir = resolve_plugin_dir()
        if not plugin_dir:
            _warn_no_plugin_dir_once()
            return
        if not os.path.isdir(plugin_dir):
            print(f"[MapLink] Plugin dir does not exist: {plugin_dir}")
            return

        script = os.path.join(plugin_dir, 'Scripts', 'maplink_export.py')
        if not os.path.isfile(script):
            print(f"[MapLink] maplink_export.py not found at: {script}")
            return

        blender_exe = bpy.app.binary_path
        if not blender_exe or not os.path.isfile(blender_exe):
            print(f"[MapLink] Cannot resolve blender.exe: {blender_exe!r}")
            return

        stem = os.path.splitext(os.path.basename(blend_path))[0]
        outdir = resolve_outdir(plugin_dir)
        log_path = os.path.join(outdir, 'Logs', stem, 'save_post_export.log')

        args = _build_command(blender_exe, plugin_dir, blend_path, outdir)
        proc = _spawn_detached(args, log_path)
        if proc is None:
            return

        # Track only the most recent launch per .blend.  No serialisation —
        # if the user saves twice within ~2 s, both subprocesses will run.
        # The second one's atomic rename will win.  If you want strict
        # serialisation, terminate the prior Popen here (we choose not to,
        # to avoid leaving the lock file in a half-cleaned state).
        _active_launches[blend_path] = proc
        print(f"[MapLink] FBX export launched (pid={proc.pid}) for {stem}, "
              f"log -> {log_path}")

    except Exception:
        # Never let a non-fatal background export break the user's save.
        print("[MapLink] FBX-export-on-save handler failed (non-fatal):")
        traceback.print_exc()


# ---------------------------------------------------------------------------
# Registration
# ---------------------------------------------------------------------------

def register():
    if _on_save_post_fbx not in bpy.app.handlers.save_post:
        bpy.app.handlers.save_post.append(_on_save_post_fbx)


def unregister():
    if _on_save_post_fbx in bpy.app.handlers.save_post:
        bpy.app.handlers.save_post.remove(_on_save_post_fbx)
