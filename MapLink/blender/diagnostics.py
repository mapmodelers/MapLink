"""MapLink Copy Diagnostics — Blender side (v1.13 / addon 1.5.0).

One click copies a markdown support report to the clipboard: addon version,
Blender version, current file, manifest discovery, P4 availability, the UE
plugin dir / artifact root preferences, the last export-on-save result, and
the UE status file content when present.

UE-side counterpart: MapLinkDiagnostics.cpp (MapLink menu → Copy
Diagnostics).

Standalone-safe: every section degrades to "n/a" — no UE project, manifest,
or P4 required.
"""

import datetime
import os
import sys

import bpy
from bpy.types import Operator


def _addon_version() -> str:
    try:
        pkg = sys.modules.get(__package__)
        ver = getattr(pkg, "bl_info", {}).get("version")
        if ver:
            return ".".join(str(v) for v in ver)
    except Exception:
        pass
    return "unknown"


def _find_manifest(start_dir: str) -> str:
    """Walk up from start_dir to the nearest maplink_project.json, '' if none."""
    try:
        d = os.path.abspath(start_dir)
        while True:
            cand = os.path.join(d, "maplink_project.json")
            if os.path.isfile(cand):
                return cand
            parent = os.path.dirname(d)
            if parent == d:
                return ""
            d = parent
    except Exception:
        return ""


def _p4_available() -> str:
    try:
        from . import p4_integration
        return "yes" if p4_integration.p4_available() else "no"
    except Exception:
        return "unknown"


def _prefs() -> tuple[str, str]:
    """(plugin dir, artifact root) preferences, best-effort."""
    plugin_dir = ""
    artifact_root = ""
    try:
        from . import fbx_export_on_save
        plugin_dir = fbx_export_on_save.resolve_plugin_dir() or ""
        artifact_root = fbx_export_on_save.resolve_artifact_root() or ""
    except Exception:
        pass
    return plugin_dir, artifact_root


def _last_export_result(artifact_root: str, plugin_dir: str, stem: str) -> str:
    """Tail of the save_post export log for the current stem, '' if none."""
    if not stem:
        return ""
    for root in (artifact_root, plugin_dir):
        if not root:
            continue
        log_path = os.path.join(root, "Logs", stem, "save_post_export.log")
        try:
            with open(log_path, "r", encoding="utf-8", errors="replace") as f:
                lines = [ln.rstrip() for ln in f.readlines() if ln.strip()]
            if lines:
                mtime = datetime.datetime.fromtimestamp(
                    os.path.getmtime(log_path)).strftime("%Y-%m-%d %H:%M")
                tail = "\n".join(lines[-5:])
                return f"({log_path}, {mtime})\n```\n{tail}\n```"
        except OSError:
            continue
        except Exception:
            continue
    return ""


def _ue_status_content() -> str:
    try:
        from . import ue_status
        status = ue_status.read_status()
        if status is not None:
            return repr(status)
    except Exception:
        pass
    return ""


def build_report() -> str:
    blend = ""
    stem = ""
    try:
        blend = bpy.data.filepath or ""
        stem = os.path.splitext(os.path.basename(blend))[0] if blend else ""
    except Exception:
        pass

    plugin_dir, artifact_root = _prefs()
    manifest = _find_manifest(os.path.dirname(blend)) if blend else ""

    md = []
    md.append("# MapLink Blender Diagnostics\n")
    md.append(f"- Addon version: {_addon_version()}")
    md.append(f"- Blender: {bpy.app.version_string}")
    md.append(f"- File: {blend or '(unsaved)'}")
    md.append(f"- Stem: {stem or 'n/a'}")
    md.append(f"- Manifest: {manifest or '(none found)'}")
    md.append(f"- P4 available: {_p4_available()}")
    md.append(f"- UE plugin dir pref: {plugin_dir or '(unset)'}")
    md.append(f"- Artifact root pref: {artifact_root or '(unset — legacy layout)'}")
    md.append(f"- UTC: {datetime.datetime.utcnow().isoformat()}Z")

    last_export = _last_export_result(artifact_root, plugin_dir, stem)
    md.append("\n## Last export-on-save\n")
    md.append(last_export or "(no export log found)")

    ue = _ue_status_content()
    md.append("\n## UE status file\n")
    md.append(ue or "(none — no UE link)")

    return "\n".join(md) + "\n"


class MAPLINK_OT_copy_diagnostics(Operator):
    """Copy a MapLink support report (markdown) to the clipboard"""
    bl_idname = "maplink.copy_diagnostics"
    bl_label = "Copy Diagnostics"
    bl_options = {'REGISTER'}

    def execute(self, context):
        try:
            report = build_report()
            context.window_manager.clipboard = report
            self.report({'INFO'}, "Diagnostics copied")
            return {'FINISHED'}
        except Exception as exc:                    # noqa: BLE001
            self.report({'WARNING'}, f"Diagnostics failed: {exc}")
            return {'CANCELLED'}


_classes = (MAPLINK_OT_copy_diagnostics,)


def register():
    for cls in _classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(_classes):
        try:
            bpy.utils.unregister_class(cls)
        except Exception:
            pass
