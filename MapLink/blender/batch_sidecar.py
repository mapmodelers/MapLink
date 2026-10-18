"""Headless batch sidecar generation for MapLink.

Run via:
    blender --factory-startup --background --python batch_sidecar.py -- <root_path>

Recursively finds all BP_/LI_/SM_ .blend files under root_path,
opens each one, generates sidecar analysis.json + filehash.txt,
and moves to the next. Single Blender process, N file opens.

P4 integration: sidecars are added to the same CL as their .blend.
If the .blend is not checked out, all sidecars go to a single shared CL:
    [MapLink Blender] Sidecar Batch Export

v1.2 — Removed hidden file attribute (broke P4 add on Windows)
"""

import os
import sys
import json
import hashlib
import subprocess
import traceback

import bpy
import mathutils

_MAPLINK_PREFIXES = ("BP_", "LI_", "SM_")
_BATCH_CL_DESC = "[MapLink Blender] Sidecar Batch Export"

# ---------------------------------------------------------------------------
# P4 helpers (standalone — addon is not loaded in --factory-startup)
# ---------------------------------------------------------------------------

_CF = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


def _run_p4(args):
    try:
        r = subprocess.run(
            ["p4"] + args,
            capture_output=True, text=True,
            creationflags=_CF,
        )
        return r.stdout.strip(), r.stderr.strip(), r.returncode
    except Exception as exc:
        return "", str(exc), 1


def _p4_available():
    try:
        subprocess.run(["p4", "-V"], stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL, creationflags=_CF)
        return True
    except FileNotFoundError:
        return False


def _get_client():
    out, _, _ = _run_p4(["set", "P4CLIENT"])
    for line in out.splitlines():
        if "P4CLIENT" in line:
            return line.split("=", 1)[-1].strip().split()[0]
    return ""


def _get_user():
    out, _, _ = _run_p4(["set", "P4USER"])
    for line in out.splitlines():
        if "P4USER" in line:
            return line.split("=", 1)[-1].strip().split()[0]
    return ""


def _normalize_description(s):
    """v1.13: per-line trim, drop trailing empty lines, '\n' joins.
    Standalone copy for --factory-startup workers — keep in sync with
    p4_integration._normalize_description and the UE side
    (NormalizeChangelistDescription in MapLinkSourceControl.cpp)."""
    lines = [ln.strip() for ln in s.replace("\r\n", "\n").split("\n")]
    while lines and not lines[-1]:
        lines.pop()
    return "\n".join(lines)


def _find_pending_cl(description):
    """Find an existing pending CL whose description matches *description*
    (normalized — see _normalize_description), so callers reuse it instead
    of creating a duplicate.  Parses ``p4 changes -l`` block by block."""
    client = _get_client()
    user = _get_user()
    args = ["changes", "-s", "pending", "-l"]
    if client:
        args += ["-c", client]
    if user:
        args += ["-u", user]
    out, _, rc = _run_p4(args)
    if rc != 0 or not out:
        return None

    target = _normalize_description(description)
    blocks = []  # [cl, [desc lines]]
    for line in out.splitlines():
        # Header lines start at column 0 ("Change N on ... *pending*"); the
        # description lines that follow are indented, so they never match here.
        if line.startswith("Change "):
            parts = line.split()
            cl = int(parts[1]) if len(parts) >= 2 and parts[1].isdigit() else None
            blocks.append([cl, []])
        elif blocks:
            blocks[-1][1].append(line.strip())

    for cl, lines in blocks:
        if cl is not None and _normalize_description("\n".join(lines)) == target:
            return cl
    return None


def _create_cl(description):
    """Create a new pending CL. Returns CL number or None."""
    spec = f"Change: new\n\nDescription:\n\t{description}\n"
    try:
        r = subprocess.run(
            ["p4", "change", "-i"], input=spec,
            capture_output=True, text=True, creationflags=_CF,
        )
        if r.returncode == 0:
            for w in r.stdout.split():
                if w.isdigit():
                    return int(w)
        else:
            print(f"  [P4] Failed to create CL: {r.stderr}")
    except Exception as exc:
        print(f"  [P4] Exception creating CL: {exc}")
    return None


def _get_or_create_batch_cl():
    """Find or create the single batch CL. Returns CL number or None."""
    found = _find_pending_cl(_BATCH_CL_DESC)
    if found is not None:
        print(f"[MapLink Batch] [P4] Found existing CL {found}: {_BATCH_CL_DESC}")
        return found
    cl = _create_cl(_BATCH_CL_DESC)
    if cl is not None:
        print(f"[MapLink Batch] [P4] Created CL {cl}: {_BATCH_CL_DESC}")
    else:
        print(f"[MapLink Batch] [P4] WARNING: Could not create CL, will use default")
    return cl


def _find_file_cl(filepath):
    """Return the CL number a file is currently opened in, or None."""
    out, _, rc = _run_p4(["opened", filepath])
    if rc != 0 or not out:
        return None
    parts = out.split()
    for i, p in enumerate(parts):
        if p == "change" and i + 1 < len(parts):
            next_p = parts[i + 1]
            if next_p == "default":
                return 0
            if next_p.isdigit():
                return int(next_p)
    return None


def _p4_add_or_edit(filepath, cl):
    """Add or edit a file into the given CL. Returns (success, msg)."""
    cl_args = ["-c", str(cl)] if cl and cl > 0 else []

    # Try add first (most sidecar files are new / not yet in depot)
    out, err, rc = _run_p4(["add"] + cl_args + [filepath])
    if rc == 0 and "opened for add" in out:
        return True, "add"

    # Already in depot — edit
    out, err, rc = _run_p4(["edit"] + cl_args + [filepath])
    if rc == 0 and "opened for edit" in out:
        return True, "edit"

    return False, f"add: {err}" if err else "p4 add/edit failed silently"


# ---------------------------------------------------------------------------
# Analysis helpers (duplicated — addon not importable in headless mode)
# ---------------------------------------------------------------------------

def _is_linked_asset_instance(obj):
    return (obj.instance_type == 'COLLECTION'
            and obj.instance_collection is not None
            and obj.instance_collection.library is not None
            and obj.instance_collection.asset_data is not None)


def _is_in_noexport_collection(obj):
    excluded = set()
    roots = [c for c in bpy.data.collections if c.name.lower() == 'noexport']
    queue = list(roots)
    while queue:
        col = queue.pop()
        excluded.add(col.name)
        for child in col.children:
            if child.name not in excluded:
                queue.append(child)
    if not excluded:
        return False
    for col in obj.users_collection:
        if col.name in excluded:
            return True
    return False


def _matrix_to_list(m):
    return [m[row][col] for row in range(4) for col in range(4)]


def _world_bbox(obj):
    if not obj.bound_box:
        loc = obj.matrix_world.translation
        return [loc.x, loc.y, loc.z, loc.x, loc.y, loc.z]
    world_corners = [obj.matrix_world @ mathutils.Vector(corner)
                     for corner in obj.bound_box]
    xs = [c.x for c in world_corners]
    ys = [c.y for c in world_corners]
    zs = [c.z for c in world_corners]
    return [min(xs), min(ys), min(zs), max(xs), max(ys), max(zs)]


def _compute_md5(filepath):
    h = hashlib.md5()
    with open(filepath, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def _carry_forward_bp_binding(report):
    """v1.8.1 - Preserve a BP_ stand-in's reference-mode binding across re-saves.

    maplink_create_bp_standin.py stamps the binding as custom properties on the
    stand-in's ROOT collection, inside the .blend:
        root_col["blueprint_path"]        -> bound_blueprint_path (UE-side gate)
        root_col["maplink_standin_mode"]  -> standin_mode
    Those props live in the scene data and survive every save, but a freshly
    rebuilt sidecar would otherwise drop the two UE-side fields, demoting the
    BP_ to an ORPHAN (placement then substitutes BP_Deprecated).  Read them back
    so the binding persists.

    Guards:
      * LOCAL collections only (col.library is None) - never inherit a binding
        from a *linked* BP_ stand-in when saving a parent SM_/LI_ that references
        it.  This is critical: linked collections appear in bpy.data.collections.
      * No-op when no local collection carries the prop (SM_/LI_/quick-create).
    """
    matches = []
    for col in bpy.data.collections:
        if col.library is not None:
            continue  # belongs to another .blend (linked) - not our binding
        bp = col.get("blueprint_path", "")
        if bp:
            matches.append((col.name, bp, col.get("maplink_standin_mode", "")))

    if not matches:
        return  # not a BP_ stand-in (or binding prop absent) - leave as-is

    if len(matches) > 1:
        stem = report.get("blend_name", "")
        chosen = next((m for m in matches if m[0] == stem), matches[0])
        print(f"[MapLink] WARNING: {len(matches)} local collections carry a "
              f"blueprint_path binding {[m[0] for m in matches]}; using "
              f"'{chosen[0]}'.")
    else:
        chosen = matches[0]

    _, bound, mode = chosen
    report["bound_blueprint_path"] = bound
    if mode:
        report["standin_mode"] = mode
    print(f"[MapLink] Carried forward BP binding -> bound_blueprint_path="
          f"'{bound}', standin_mode='{mode}'")
    return report


def _build_report(blend_name):
    report = {
        'blend_name': blend_name,
        'unit_scale': 1.0,
        'blend_own_geo': [],
        'linked_assets': [],
        'libraries': [],
    }
    for obj in bpy.context.scene.objects:
        if _is_in_noexport_collection(obj):
            continue
        if _is_linked_asset_instance(obj):
            src_path = bpy.path.abspath(
                obj.instance_collection.library.filepath)
            src_stem = os.path.splitext(os.path.basename(src_path))[0]
            if not src_stem.startswith(_MAPLINK_PREFIXES):
                continue
            wm = obj.matrix_world
            report['linked_assets'].append({
                'name': obj.name,
                'world_transform': _matrix_to_list(wm),
                'bbox_world': _world_bbox(obj),
                'source_blend_path': src_path,
                'asset_name': obj.instance_collection.name,
            })
        elif obj.type == 'MESH':
            wm = obj.matrix_world
            report['blend_own_geo'].append({
                'name': obj.name,
                'world_transform': _matrix_to_list(wm),
                'bbox_world': _world_bbox(obj),
            })
    for lib in bpy.data.libraries:
        abs_path = bpy.path.abspath(lib.filepath)
        lib_stem = os.path.splitext(os.path.basename(abs_path))[0]
        if not lib_stem.startswith(_MAPLINK_PREFIXES):
            continue
        report['libraries'].append({
            'name': lib.name,
            'filepath': abs_path,
        })
    _carry_forward_bp_binding(report)
    return report


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    argv = sys.argv
    try:
        idx = argv.index("--")
    except ValueError:
        print("ERROR: Usage: ... --python batch_sidecar.py -- <root_path>")
        sys.exit(1)

    root_path = argv[idx + 1]
    if not os.path.isdir(root_path):
        print(f"ERROR: Not a directory: {root_path}")
        sys.exit(1)

    # Collect all MapLink .blend files
    blend_files = []
    for dirpath, _, filenames in os.walk(root_path):
        for fn in filenames:
            if fn.endswith('.blend') and not fn.startswith('_'):
                stem = os.path.splitext(fn)[0]
                if stem.startswith(_MAPLINK_PREFIXES):
                    blend_files.append(os.path.join(dirpath, fn))

    blend_files.sort()
    total = len(blend_files)
    print(f"[MapLink Batch] Found {total} MapLink .blend files under {root_path}")

    # P4: check availability and create batch CL ONCE before loop
    has_p4 = _p4_available()
    batch_cl = None
    if has_p4:
        print(f"[MapLink Batch] [P4] p4 available — client={_get_client()}, user={_get_user()}")
        batch_cl = _get_or_create_batch_cl()
    else:
        print(f"[MapLink Batch] [P4] p4 not found in PATH — skipping source control")

    ok_count = 0
    err_count = 0
    p4_add_count = 0

    for i, blend_path in enumerate(blend_files):
        stem = os.path.splitext(os.path.basename(blend_path))[0]
        print(f"[MapLink Batch] ({i+1}/{total}) {stem}")

        try:
            bpy.ops.wm.open_mainfile(filepath=blend_path)
        except Exception as exc:
            print(f"  ERROR opening: {exc}")
            err_count += 1
            continue

        blend_dir = os.path.dirname(blend_path)
        analysis_path = os.path.join(blend_dir, f"{stem}_analysis.json")
        hash_path = os.path.join(blend_dir, f"{stem}_filehash.txt")

        try:
            # Determine target CL for this file's sidecars
            target_cl = batch_cl  # default: shared batch CL
            if has_p4:
                blend_cl = _find_file_cl(blend_path)
                if blend_cl is not None and blend_cl > 0:
                    target_cl = blend_cl  # .blend is in a numbered CL — use it

                # Checkout existing sidecar files before overwriting
                for fp in (analysis_path, hash_path):
                    if os.path.isfile(fp):
                        _p4_add_or_edit(fp, target_cl)

            # Write analysis
            report = _build_report(stem)
            with open(analysis_path, 'w', encoding='utf-8') as f:
                json.dump(report, f, indent=2)

            # Write hash
            file_hash = _compute_md5(blend_path)
            with open(hash_path, 'w', encoding='utf-8') as f:
                f.write(file_hash)

            # P4: add/edit the newly written files
            if has_p4:
                for fp in (analysis_path, hash_path):
                    ok, action = _p4_add_or_edit(fp, target_cl)
                    if ok:
                        p4_add_count += 1
                    else:
                        print(f"  [P4] FAILED {os.path.basename(fp)}: {action}")

            print(f"  OK — {file_hash}")
            ok_count += 1

        except Exception:
            traceback.print_exc()
            err_count += 1

    print(f"[MapLink Batch] Done: {ok_count} OK, {err_count} errors, {total} total")
    if has_p4:
        print(f"[MapLink Batch] [P4] {p4_add_count} files added/edited in P4"
              f" (CL {batch_cl})" if batch_cl else "")


if __name__ == "__main__":
    main()
