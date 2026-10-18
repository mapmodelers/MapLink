"""MapLink vendor-in — one operation, reachable from several doors.

Moving an asset between MapLink roots on one machine is an **Import at the
destination**, because every option in the dialog belongs to the receiver.
"Export" stays reserved for material-library bundles: a transferable artifact
for a machine that does not have the source.  So there is no "Export Asset"
button here, and the two entry points below — ``maplink.import_asset``
(MapLink ▸ Project) and ``maplink.copy_to_project`` (right-click a Shelf asset)
— are the same operator through opposite doors.

Why a copy and not a rewrite
----------------------------
The layout ``<root>/<TYPE>/<Name>/<Name>.blend`` makes an LI→SM link
``//../../SM/Foo/Foo.blend``.  That relative geometry is identical in every
root, so a copied subtree resolves unchanged on any machine, with no path
surgery at all.  ``vendor_worker.py`` runs afterwards only to catch links that
were stored ABSOLUTE in the source, which a copy cannot fix by itself.

The closure is read from the sidecars
-------------------------------------
``<Name>_analysis.json`` already records every library a file links, and it is
what UE consumes, so it is the right source of truth here too — and reading it
costs no Blender launch.  A source file with no sidecar is reported rather than
guessed at: no sidecar is precisely the state the v1.19 link gate produces for
a damaged file, and vendoring a damaged asset in would carry the damage.

Deferred to the next drop (item 7): the UE side of material instances.  This
operator repoints material links at the destination's own MI library and
records the names that are not there yet in
``<dest_root>/MaterialInstances/pending_matlib_import.json``.  UE reading that
file is v1.20; until then the names are reported in the dialog and the console
so nothing is silently lost.
"""

import json
import os
import shutil
import subprocess
import tempfile

import bpy
from bpy.types import Operator

from . import link_scope

TYPES = ("LI", "SM", "BP")
_WORKER = "vendor_worker.py"
_PENDING = "pending_matlib_import.json"


def _log(msg):
    print(f"[MapLink Vendor] {msg}", flush=True)


# ---------------------------------------------------------------------------
# Closure walk
# ---------------------------------------------------------------------------

def _asset_parts(blend_path):
    """(root, TYPE, Name) for <root>/<TYPE>/<Name>/<Name>.blend, else None."""
    blend_path = os.path.abspath(blend_path)
    folder = os.path.dirname(blend_path)
    name = os.path.basename(folder)
    type_dir = os.path.dirname(folder)
    atype = os.path.basename(type_dir)
    if atype not in TYPES:
        return None
    return os.path.dirname(type_dir), atype, name


def _sidecar_libraries(blend_path):
    """Absolute library paths recorded in the blend's sidecar, or None."""
    stem = os.path.splitext(os.path.basename(blend_path))[0]
    sidecar = os.path.join(os.path.dirname(blend_path), f"{stem}_analysis.json")
    if not os.path.isfile(sidecar):
        return None
    try:
        with open(sidecar, "r", encoding="utf-8") as f:
            data = json.load(f)
    except Exception as exc:                            # noqa: BLE001
        _log(f"unreadable sidecar {sidecar}: {exc}")
        return None
    return [str(e.get("filepath", "")) for e in data.get("libraries", [])
            if e.get("filepath")]


def build_plan(sources, dest_root):
    """Walk the closure of *sources*.  -> plan dict.

    ``items``   [{blend, root, type, name, dest_folder, exists}]  deduped
    ``no_sidecar`` blends whose closure could not be read
    ``missing``    linked paths that do not exist on disk
    """
    plan = {"items": [], "no_sidecar": [], "missing": [],
            "source_roots": set(), "dest_root": os.path.abspath(dest_root)}

    seen = set()
    queue = [os.path.abspath(s) for s in sources]
    while queue:
        blend = queue.pop(0)
        key = os.path.normcase(blend)
        if key in seen:
            continue
        seen.add(key)

        if not os.path.isfile(blend):
            plan["missing"].append(blend)
            continue

        parts = _asset_parts(blend)
        if parts is None:
            plan["missing"].append(f"{blend} (not <root>/<TYPE>/<Name>/)")
            continue
        root, atype, name = parts

        if link_scope.is_under(blend, plan["dest_root"]):
            continue                       # already home; nothing to vendor

        plan["source_roots"].add(root)
        dest_folder = os.path.join(plan["dest_root"], atype, name)
        plan["items"].append({
            "blend": blend,
            "folder": os.path.dirname(blend),
            "root": root,
            "type": atype,
            "name": name,
            "dest_folder": dest_folder,
            "exists": os.path.isdir(dest_folder),
        })

        libs = _sidecar_libraries(blend)
        if libs is None:
            plan["no_sidecar"].append(blend)
            continue
        for lib in libs:
            queue.append(lib)

    plan["source_roots"] = sorted(plan["source_roots"])
    return plan


def plan_counts(plan):
    out = {t: 0 for t in TYPES}
    for item in plan["items"]:
        out[item["type"]] = out.get(item["type"], 0) + 1
    return out


# ---------------------------------------------------------------------------
# Copy + conform
# ---------------------------------------------------------------------------

def _copy_folder(src, dst, overwrite):
    """-> 'copied' | 'skipped' | 'overwritten'."""
    if os.path.isdir(dst):
        if not overwrite:
            return "skipped"
        shutil.copytree(src, dst, dirs_exist_ok=True)
        return "overwritten"
    shutil.copytree(src, dst)
    return "copied"


def _run_worker(dest_root, source_roots, blends, timeout=900):
    """Conform library paths inside the copied files.  -> (report, error)."""
    if not blends:
        return {"files": []}, ""

    worker = os.path.join(os.path.dirname(__file__), _WORKER)
    if not os.path.isfile(worker):
        return None, f"worker not found: {worker}"

    fd, spec_path = tempfile.mkstemp(suffix=".json", prefix="maplink_vendor_")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump({"dest_root": dest_root, "source_roots": list(source_roots),
                   "blends": list(blends)}, f)

    throwaway = blends[0]
    cmd = [bpy.app.binary_path, "--factory-startup", "--background",
           throwaway, "--python", worker, "--", spec_path]
    cf = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=timeout, creationflags=cf)
    except subprocess.TimeoutExpired:
        return None, f"path conforming timed out after {timeout}s"
    except Exception as exc:                            # noqa: BLE001
        return None, f"could not launch Blender: {exc}"
    finally:
        try:
            os.remove(spec_path)
        except OSError:
            pass

    out = r.stdout or ""
    for line in out.splitlines():
        print(line)
    from .vendor_worker import BEGIN, END
    if BEGIN not in out or END not in out:
        print(r.stderr or "")
        return None, f"path conforming produced no report (rc={r.returncode})"
    body = out.split(BEGIN, 1)[1].split(END, 1)[0].strip()
    try:
        return json.loads(body), ""
    except Exception as exc:                            # noqa: BLE001
        return None, f"unreadable worker report: {exc}"


def _repoint_open_file(plan):
    """Point the open file's libraries at the vendored copies.  -> n."""
    dest_root = plan["dest_root"]
    moved = 0
    for lib in bpy.data.libraries:
        abs_path = bpy.path.abspath(lib.filepath)
        for src in plan["source_roots"]:
            if not link_scope.is_under(abs_path, src):
                continue
            new_abs = os.path.join(dest_root, os.path.relpath(abs_path, src))
            if not os.path.isfile(new_abs):
                break
            try:
                lib.filepath = bpy.path.relpath(new_abs)
                moved += 1
            except Exception as exc:                    # noqa: BLE001
                _log(f"could not repoint {lib.name}: {exc}")
            break
    return moved


def _write_pending(dest_root, names, source_roots):
    """Record MI names the destination does not have yet.  -> path or ''."""
    names = sorted(set(n for n in names if n))
    if not names:
        return ""
    folder = os.path.join(dest_root, "MaterialInstances")
    try:
        os.makedirs(folder, exist_ok=True)
        path = os.path.join(folder, _PENDING)
        existing = []
        if os.path.isfile(path):
            try:
                with open(path, "r", encoding="utf-8") as f:
                    existing = json.load(f).get("material_instances", [])
            except Exception:                           # noqa: BLE001
                existing = []
        merged = sorted(set(existing) | set(names))
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"schema_version": 1,
                       "material_instances": merged,
                       "source_roots": list(source_roots)}, f, indent=2)
        return path
    except Exception as exc:                            # noqa: BLE001
        _log(f"could not write {_PENDING}: {exc}")
        return ""


# ---------------------------------------------------------------------------
# The operator
# ---------------------------------------------------------------------------

class MAPLINK_OT_VendorAsset(Operator):
    """Copy an asset and everything it links into this MapLink project"""
    bl_idname = "maplink.vendor_asset"
    bl_label = "Copy into Project"
    bl_options = {'REGISTER'}

    source_blend: bpy.props.StringProperty(
        name="Asset",
        description="The .blend to copy in. Leave empty to take every link in "
                    "this file that points outside the project",
        subtype='FILE_PATH',
        default="",
    )
    dest_root: bpy.props.StringProperty(
        name="Into",
        subtype='DIR_PATH',
        default="",
    )
    overwrite: bpy.props.BoolProperty(
        name="Overwrite existing",
        description="Names are identity in MapLink. Off (the default) keeps "
                    "what is already in this project and skips the copy",
        default=False,
    )
    repoint: bpy.props.BoolProperty(
        name="Repoint this file's links",
        description="Point the open file at the vendored copies once they land",
        default=True,
    )

    _plan = None

    # -- plumbing ---------------------------------------------------------

    def _sources(self):
        if self.source_blend:
            return [bpy.path.abspath(self.source_blend)]
        # Only what MapLink owns. An unscoped Foreign check would offer to
        # vendor Blender's own bundled asset libraries into the project, which
        # is not a thing anyone wants.
        return [e["abspath"] for e in link_scope.blocking_entries()
                if e["status"] == link_scope.FOREIGN]

    def invoke(self, context, event):
        if not self.dest_root:
            root = link_scope.current_project_root()
            if not root:
                from . import project_roots
                root, _name = project_roots.require_root(context)
                if not root:
                    return {'CANCELLED'}
            self.dest_root = root

        sources = self._sources()
        if not sources:
            self.report({'INFO'},
                        "Nothing to copy — every link already resolves inside "
                        "this project.")
            return {'CANCELLED'}

        self._plan = build_plan(sources, bpy.path.abspath(self.dest_root))
        if not self._plan["items"]:
            self.report({'INFO'}, "Nothing to copy.")
            return {'CANCELLED'}
        return context.window_manager.invoke_props_dialog(self, width=520)

    def draw(self, context):
        layout = self.layout
        plan = self._plan or {}
        items = plan.get("items", [])
        counts = plan_counts(plan) if items else {}
        dest_name = os.path.basename(os.path.normpath(plan.get("dest_root", "")))

        head = items[0]["name"] if items else "?"
        layout.label(text=f"Copy  {head}  →  {dest_name}", icon='IMPORT')

        summary = "  ·  ".join(f"{counts.get(t, 0)} {t}_" for t in TYPES)
        layout.label(text=f"    {summary}  ·  {len(items)} folder(s)")

        clashes = [i for i in items if i["exists"]]
        if clashes:
            box = layout.box()
            box.label(text=f"{len(clashes)} already in {dest_name}:", icon='ERROR')
            for i in clashes[:6]:
                box.label(text=f"    {i['name']}")
            if len(clashes) > 6:
                box.label(text=f"    …and {len(clashes) - 6} more")
            box.prop(self, "overwrite")

        if plan.get("no_sidecar"):
            box = layout.box()
            box.label(text="No sidecar — dependencies unknown:", icon='ERROR')
            for b in plan["no_sidecar"][:4]:
                box.label(text=f"    {os.path.basename(b)}")
            box.label(text="They copy, but anything they link may not follow.")

        if plan.get("missing"):
            box = layout.box()
            box.label(text=f"{len(plan['missing'])} link(s) do not resolve and "
                           f"cannot be copied", icon='ERROR')

        layout.separator()
        layout.prop(self, "repoint")
        layout.label(text="Textures stay where they are — the Textures tab's "
                          "Conform owns residency.", icon='INFO')
        layout.label(text="Material instances resolve against this project's "
                          "own library.", icon='INFO')

    # -- work -------------------------------------------------------------

    def execute(self, context):
        plan = self._plan
        if plan is None:
            # EXEC_DEFAULT path (the save gate): no dialog ran, so resolve the
            # destination and build the plan here.
            dest = bpy.path.abspath(self.dest_root) if self.dest_root else ""
            if not dest:
                dest = link_scope.current_project_root() or ""
            if not dest:
                self.report({'ERROR'}, "No MapLink project to copy into.")
                return {'CANCELLED'}
            self.dest_root = dest
            plan = build_plan(self._sources(), dest)
        items = plan["items"]
        if not items:
            self.report({'INFO'}, "Nothing to copy.")
            return {'CANCELLED'}

        copied, skipped, failed = [], [], []
        for item in items:
            try:
                verdict = _copy_folder(item["folder"], item["dest_folder"],
                                       self.overwrite)
            except Exception as exc:                    # noqa: BLE001
                failed.append(f"{item['name']}: {exc}")
                continue
            if verdict == "skipped":
                skipped.append(item["name"])
            else:
                copied.append(item)

        if failed:
            for line in failed:
                _log(f"copy failed — {line}")
            self.report({'ERROR'},
                        f"{len(failed)} folder(s) could not be copied; see console.")
            return {'CANCELLED'}

        # Conform absolute links inside everything now living in the
        # destination — copied this run, and anything skipped that was already
        # there and may still point at the source root.
        touch = [os.path.join(i["dest_folder"], os.path.basename(i["blend"]))
                 for i in items]
        touch = [p for p in touch if os.path.isfile(p)]
        report, err = _run_worker(plan["dest_root"], plan["source_roots"], touch)
        if err:
            self.report({'WARNING'}, f"Copied, but path conforming failed: {err}")
            report = {"files": []}

        missing_mi, unresolved = [], []
        for entry in report.get("files", []):
            missing_mi.extend(entry.get("missing_materials", []))
            unresolved.extend(entry.get("unresolved", []))

        pending = _write_pending(plan["dest_root"], missing_mi,
                                 plan["source_roots"])

        repointed = _repoint_open_file(plan) if self.repoint else 0
        link_scope.refresh_cache()

        bits = [f"{len(copied)} copied"]
        if skipped:
            bits.append(f"{len(skipped)} skipped (already present)")
        if repointed:
            bits.append(f"{repointed} link(s) repointed")
        if missing_mi:
            bits.append(f"{len(set(missing_mi))} material instance(s) not in "
                        f"this project")
        self.report({'INFO'}, "Vendor-in: " + ", ".join(bits) + ".")

        if skipped:
            _log("skipped (already present): " + ", ".join(skipped))
        if unresolved:
            _log(f"{len(unresolved)} link(s) still unresolved after the copy:")
            for u in unresolved[:20]:
                _log(f"    {u}")
        if pending:
            _log(f"material instances recorded for Unreal: {pending}")
        return {'FINISHED'}


# ---------------------------------------------------------------------------
# Doors — same operator, opposite sides
# ---------------------------------------------------------------------------

class MAPLINK_OT_ImportAsset(Operator):
    """Pick a .blend from another MapLink root and copy it into this project"""
    bl_idname = "maplink.import_asset"
    bl_label = "Import Asset…"
    bl_options = {'REGISTER'}

    filepath: bpy.props.StringProperty(subtype='FILE_PATH')
    filter_glob: bpy.props.StringProperty(default="*.blend", options={'HIDDEN'})

    def invoke(self, context, event):
        from . import project_roots
        start = project_roots.shelf_root()
        if start:
            self.filepath = start + os.sep
        context.window_manager.fileselect_add(self)
        return {'RUNNING_MODAL'}

    def execute(self, context):
        if not self.filepath:
            return {'CANCELLED'}
        return bpy.ops.maplink.vendor_asset(
            'INVOKE_DEFAULT', source_blend=bpy.path.abspath(self.filepath))


def _asset_browser_blend(context):
    """The .blend behind the right-clicked Asset Browser entry, or ""."""
    asset = getattr(context, "asset", None)
    if asset is None:
        return ""
    for attr in ("full_library_path", "full_path"):
        p = getattr(asset, attr, "")
        if p:
            # full_path is <blend>/Collection/<Name>; keep the .blend part.
            p = str(p)
            low = p.lower()
            cut = low.find(".blend")
            return p[:cut + 6] if cut != -1 else p
    return ""


class MAPLINK_OT_CopyToProject(Operator):
    """Copy this Shelf asset into the MapLink project you are working in"""
    bl_idname = "maplink.copy_to_project"
    bl_label = "Copy to Project…"
    bl_options = {'REGISTER'}

    @classmethod
    def poll(cls, context):
        blend = _asset_browser_blend(context)
        if not blend:
            return False
        root = link_scope.current_project_root()
        return bool(root) and not link_scope.is_under(blend, root)

    def execute(self, context):
        blend = _asset_browser_blend(context)
        if not blend:
            self.report({'ERROR'}, "Could not resolve the asset's .blend.")
            return {'CANCELLED'}
        return bpy.ops.maplink.vendor_asset('INVOKE_DEFAULT', source_blend=blend)


# ---------------------------------------------------------------------------
# Registration
# ---------------------------------------------------------------------------

_classes = (
    MAPLINK_OT_VendorAsset,
    MAPLINK_OT_ImportAsset,
    MAPLINK_OT_CopyToProject,
)


def register():
    for cls in _classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(_classes):
        try:
            bpy.utils.unregister_class(cls)
        except Exception:                               # noqa: BLE001
            pass
