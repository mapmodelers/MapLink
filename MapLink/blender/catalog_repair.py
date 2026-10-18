"""MapLink — Repair Asset Catalogs.

The foreground half of `catalog_worker.py`.  It exists because three creation
paths marked assets without cataloguing them, and one of them (Rebuild All
Thumbnails) actively cleared the catalog off assets that were already correct.
Those bugs are fixed at the source in v1.19, but a project that has been
authored in for a while already holds the damage, and no amount of fixing the
creation paths repairs a .blend that is already on disk.

Perforce stays in the foreground, where `p4_integration` lives: the dry run
names the files that need a write, those files (and only those) are checked out
onto one changelist, and the write pass runs against them.  Unchanged files are
never opened for edit, so a repair that finds nothing costs the depot nothing.
"""

import json
import os
import subprocess
import tempfile

import bpy
from bpy.types import Operator

from . import asset_catalog
from . import p4_integration

CL_DESC = "[MapLink-Blender] Repair Asset Catalogs"
_WORKER = "catalog_worker.py"


def _log(msg):
    print(f"[MapLink CatalogFix] {msg}", flush=True)


def discover_blends(root):
    """Every .blend under *root*, excluding the '_' worker/template convention.

    Same rule as batch_standardize._discover_blends, deliberately: the set of
    files a batch operation acts on should not depend on which batch it is.
    """
    out = []
    for dirpath, _dirs, filenames in os.walk(root):
        for fn in filenames:
            if fn.endswith(".blend") and not fn.startswith("_"):
                out.append(os.path.normpath(os.path.join(dirpath, fn)))
    out.sort()
    return out


def _run_worker(blends, dry_run, root, timeout=3600):
    """-> (report, error)."""
    if not blends:
        return {"files": []}, ""

    worker = os.path.join(os.path.dirname(__file__), _WORKER)
    if not os.path.isfile(worker):
        return None, f"worker not found: {worker}"

    fd, spec_path = tempfile.mkstemp(suffix=".json", prefix="maplink_catfix_")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump({"blends": list(blends), "dry_run": bool(dry_run),
                   "root": root or ""}, f)

    cmd = [bpy.app.binary_path, "--factory-startup", "--background",
           blends[0], "--python", worker, "--", spec_path]
    cf = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
    try:
        r = subprocess.run(cmd, capture_output=True, text=True,
                           timeout=timeout, creationflags=cf)
    except subprocess.TimeoutExpired:
        return None, f"timed out after {timeout}s"
    except Exception as exc:                            # noqa: BLE001
        return None, f"could not launch Blender: {exc}"
    finally:
        try:
            os.remove(spec_path)
        except OSError:
            pass

    out = r.stdout or ""
    from .catalog_worker import BEGIN, END
    if BEGIN not in out or END not in out:
        for line in out.splitlines()[-40:]:
            print(line)
        print(r.stderr or "")
        return None, f"no report produced (rc={r.returncode})"
    try:
        return json.loads(out.split(BEGIN, 1)[1].split(END, 1)[0].strip()), ""
    except Exception as exc:                            # noqa: BLE001
        return None, f"unreadable report: {exc}"


class MAPLINK_OT_RepairAssetCatalogs(Operator):
    """File every marked asset under the catalog for the folder it lives in"""
    bl_idname = "maplink.repair_asset_catalogs"
    bl_label = "Repair Asset Catalogs…"
    bl_options = {'REGISTER'}

    root: bpy.props.StringProperty(
        name="Project Root",
        subtype='DIR_PATH',
        default="",
    )
    dry_run: bpy.props.BoolProperty(
        name="Dry run (report only, write nothing)",
        description="Open every .blend and count what would change, without "
                    "saving anything or opening a single file in Perforce",
        default=True,
    )

    def invoke(self, context, event):
        if not self.root:
            from . import link_scope
            self.root = link_scope.current_project_root() or ""
        if not self.root:
            batch = getattr(context.scene, "maplink_batch", None)
            self.root = (getattr(batch, "root_path", "") or "").strip()
        if not self.root:
            self.report({'ERROR'},
                        "No MapLink project — open a project file or set "
                        "MapLink ▸ Project ▸ Project Root.")
            return {'CANCELLED'}
        return context.window_manager.invoke_props_dialog(self, width=520)

    def draw(self, context):
        layout = self.layout
        root = bpy.path.abspath(self.root or "")
        n = len(discover_blends(root)) if os.path.isdir(root) else 0

        layout.label(text=f"{os.path.basename(os.path.normpath(root))}  —  "
                          f"{n} .blend file(s)", icon='FILE_FOLDER')
        layout.label(text="Every marked asset is filed under the folder it "
                          "lives in — LI, SM, BP, Presets, Materials.")
        layout.label(text="Uncatalogued assets are filed; assets sitting on the "
                          "project catalog move down to their folder's.")
        layout.label(text="Assets in a catalog of your own are left alone.")
        layout.label(text="Files that need no change are never saved and never "
                          "checked out.")
        layout.separator()
        layout.prop(self, "dry_run")
        if not self.dry_run:
            layout.label(text="This opens the files it changes in Perforce, on "
                              "one changelist.", icon='INFO')
        layout.label(text=f"Opening {n} files takes a while — Blender will be "
                          f"busy.", icon='TIME')

    def execute(self, context):
        root = bpy.path.abspath((self.root or "").strip())
        if not os.path.isdir(root):
            self.report({'ERROR'}, f"Not a directory: {root}")
            return {'CANCELLED'}

        # v1.20 — the definition files are CHECKED INTO PERFORCE, so on a
        # machine where they are not checked out every write fails with EACCES.
        # They therefore have to be planned first, opened for edit with the
        # .blends, and written after — and a dry run must not write them at
        # all, which the previous version did.
        pending_cats = asset_catalog.cats_files_needing_update(root)

        if not asset_catalog.uuid_for_root(root):
            self.report({'ERROR'}, "Could not resolve this project's catalog.")
            return {'CANCELLED'}

        blends = discover_blends(root)
        if not blends:
            self.report({'INFO'}, "No .blend files under this root.")
            return {'CANCELLED'}

        # Pass 1 — always a dry run, even when applying. It is what tells us
        # which files to open in Perforce, and checking out all 46 to change 12
        # is how a changelist becomes noise nobody reviews.
        report, err = _run_worker(blends, True, root)
        if err:
            self.report({'ERROR'}, f"Scan failed: {err}")
            return {'CANCELLED'}

        def _touched(e):
            return e.get("assigned") or e.get("moved")

        need = [e for e in report["files"] if _touched(e)]
        already = sum(e.get("already", 0) for e in report["files"])
        custom = sum(e.get("custom", 0) for e in report["files"])
        broken = [e for e in report["files"] if e.get("error")]
        n_assigned = sum(len(e.get("assigned", ())) for e in need)
        n_moved = sum(len(e.get("moved", ())) for e in need)

        for e in need:
            name = os.path.basename(e["blend"])
            if e.get("assigned"):
                _log(f"{name}: filing {len(e['assigned'])} — "
                     f"{', '.join(e['assigned'][:6])}"
                     f"{' …' if len(e['assigned']) > 6 else ''}")
            if e.get("moved"):
                _log(f"{name}: moving {len(e['moved'])} out of the project "
                     f"catalog — {', '.join(e['moved'][:6])}"
                     f"{' …' if len(e['moved']) > 6 else ''}")
        for e in broken:
            _log(f"{os.path.basename(e['blend'])}: {e['error']}")

        if self.dry_run:
            bits = []
            if n_assigned:
                bits.append(f"{n_assigned} uncatalogued asset(s) would be filed")
            if n_moved:
                bits.append(f"{n_moved} would move to a folder catalog")
            bits.append(f"{len(need)} file(s) affected")
            bits.append(f"{already} already correct")
            if custom:
                bits.append(f"{custom} in your own catalogs, untouched")
            if pending_cats:
                bits.append(f"{len(pending_cats)} catalog definition file(s) "
                            f"would be written")
            if broken:
                bits.append(f"{len(broken)} file(s) could not be read")
            self.report({'INFO'}, "Dry run: " + ", ".join(bits) + ". See console.")
            return {'FINISHED'}

        if not need and not pending_cats:
            self.report({'INFO'},
                        f"Nothing to repair — {already} asset(s) already "
                        f"catalogued.")
            return {'FINISHED'}

        # Pass 2 — check out exactly the files that change, then write.
        cl = None
        if p4_integration.p4_available():
            try:
                cl = p4_integration._get_or_create_changelist(CL_DESC)
            except Exception as exc:                    # noqa: BLE001
                _log(f"P4 changelist failed (non-fatal): {exc}")
            for target in [e["blend"] for e in need] + pending_cats:
                try:
                    ok, msg = p4_integration.checkout_or_add(
                        target, cl=cl if cl and cl > 0 else None)
                    if not ok:
                        _log(f"checkout said no for {target}: {msg}")
                except Exception as exc:                # noqa: BLE001
                    _log(f"checkout failed for {target}: {exc}")

        # Now they are open for edit (or were never in Perforce at all), so the
        # write can succeed. force_writable is the belt to that braces.
        written = asset_catalog.ensure_cats_files(root, force_writable=True)
        blocked = list(getattr(asset_catalog.ensure_cats_files, "blocked", []))
        for b in blocked:
            _log(f"STILL READ-ONLY, catalog tree will be incomplete: {b}")

        report2, err = _run_worker([e["blend"] for e in need], False, root)
        if err:
            self.report({'ERROR'}, f"Repair failed: {err}")
            return {'CANCELLED'}

        saved = [e for e in report2["files"] if e.get("saved")]
        failed = [e for e in report2["files"] if e.get("error")]
        for e in failed:
            _log(f"{os.path.basename(e['blend'])}: {e['error']}")

        msg = (f"Catalogued {n_assigned + n_moved} asset(s) "
               f"({n_moved} moved) in {len(saved)} file(s)"
               + (f", {len(written)} catalog file(s) written" if written else "")
               + (f" on CL {cl}" if cl else "") + ".")
        if blocked:
            msg += (f" {len(blocked)} catalog file(s) are still read-only — "
                    f"check them out and re-run.")
        if failed:
            msg += f" {len(failed)} file(s) failed — see console."
        self.report({'WARNING'} if (failed or blocked) else {'INFO'}, msg)
        return {'FINISHED'}


def register():
    bpy.utils.register_class(MAPLINK_OT_RepairAssetCatalogs)


def unregister():
    try:
        bpy.utils.unregister_class(MAPLINK_OT_RepairAssetCatalogs)
    except Exception:                                   # noqa: BLE001
        pass
