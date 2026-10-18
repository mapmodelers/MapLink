"""Headless batch modeling operations for MapLink (v1.4.6).

Run via:
    blender --factory-startup --background --python batch_modeling.py -- <root> <mode>

where <mode> is "clean" (Outliner Cleanup) or "conform" (Conform Vertex
Colors).  Recursively finds every MapLink BP_/LI_/SM_ .blend under <root>,
opens each, runs the operation in memory, and — only for files that actually
change — checks the .blend out of Perforce and re-saves it.  Conform is
auto-confirmed (no dialog: this calls the conform logic directly).

The addon is NOT registered under --factory-startup, so the operation logic
is imported as plain functions from the sibling modules:
    outliner_cleanup.run_cleanup(scene)         -> (msg, n_obj, n_mesh)
    modeling_tools.conform_scene_meshes(scene)  -> dict(create, conform, ...)

NOTE: re-saving a .blend invalidates its sidecar analysis.json / filehash.txt.
Run "Generate All Sidecars" afterwards to refresh the UE-plugin cache.
"""

import os
import sys
import subprocess
import traceback

import bpy

# Make the sibling addon modules importable in --factory-startup.
_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
if _THIS_DIR not in sys.path:
    sys.path.insert(0, _THIS_DIR)

import outliner_cleanup        # noqa: E402
import modeling_tools          # noqa: E402

# BP_ is intentionally EXCLUDED from batch modeling ops: re-saving a BP_ proxy
# .blend can disturb the Blueprint it is linked to, so batch clean/conform/
# collections only touch LI_/SM_ for now.  (BP_ sidecars/force-save elsewhere
# read or re-save as-is and don't mutate content.)
_MAPLINK_PREFIXES = ("LI_", "SM_")
_CL_DESC = {
    "clean": "[MapLink Blender] Batch Clean Outliner",
    "conform": "[MapLink Blender] Batch Conform VC",
    "collections": "[MapLink Blender] Batch Clean Collections",
}

_CF = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


# ---------------------------------------------------------------------------
# P4 helpers (standalone — addon is not loaded in --factory-startup)
# ---------------------------------------------------------------------------

def _run_p4(args):
    try:
        r = subprocess.run(["p4"] + args, capture_output=True, text=True,
                           creationflags=_CF)
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


def _find_pending_cl(description):
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

    def _norm(s):
        return " ".join(s.split())

    target = _norm(description)
    blocks = []  # [cl, [desc lines]]
    for line in out.splitlines():
        if line.startswith("Change "):
            parts = line.split()
            cl = int(parts[1]) if len(parts) >= 2 and parts[1].isdigit() else None
            blocks.append([cl, []])
        elif blocks and line.strip():
            blocks[-1][1].append(line.strip())

    for cl, lines in blocks:
        if cl is not None and _norm(" ".join(lines)) == target:
            return cl
    return None


def _create_cl(description):
    spec = f"Change: new\n\nDescription:\n\t{description}\n"
    try:
        r = subprocess.run(["p4", "change", "-i"], input=spec,
                           capture_output=True, text=True, creationflags=_CF)
        if r.returncode == 0:
            for w in r.stdout.split():
                if w.isdigit():
                    return int(w)
        else:
            print(f"  [P4] Failed to create CL: {r.stderr}")
    except Exception as exc:
        print(f"  [P4] Exception creating CL: {exc}")
    return None


def _get_or_create_cl(desc):
    found = _find_pending_cl(desc)
    if found is not None:
        print(f"[MapLink Batch] [P4] Found existing CL {found}: {desc}")
        return found
    cl = _create_cl(desc)
    if cl is not None:
        print(f"[MapLink Batch] [P4] Created CL {cl}: {desc}")
    else:
        print("[MapLink Batch] [P4] WARNING: Could not create CL, "
              "will use default")
    return cl


def _p4_edit(filepath, cl):
    """Check a file out for edit (add if not yet in depot).  (ok, action)."""
    cl_args = ["-c", str(cl)] if cl and cl > 0 else []
    out, err, rc = _run_p4(["edit"] + cl_args + [filepath])
    if rc == 0 and "opened for edit" in out:
        return True, "edit"
    out, err, rc = _run_p4(["add"] + cl_args + [filepath])
    if rc == 0 and "opened for add" in out:
        return True, "add"
    return False, err or "p4 edit/add failed"


def _make_writable(filepath):
    """Best-effort clear of the read-only bit when P4 is unavailable."""
    try:
        import stat
        st = os.stat(filepath)
        os.chmod(filepath, st.st_mode | stat.S_IWRITE)
    except Exception:
        pass


# ---------------------------------------------------------------------------
# Per-file operation
# ---------------------------------------------------------------------------

def _apply(mode, color_map=None):
    """Run the operation in memory.  Returns (changed, summary_str)."""
    scene = bpy.context.scene
    if mode == "clean":
        res = outliner_cleanup.run_cleanup(scene)
        if res is None:
            return False, "no scene"
        msg, n_obj, n_mesh = res
        return (n_obj + n_mesh) > 0, msg
    elif mode == "conform":
        c = modeling_tools.conform_scene_meshes(scene)
        changed = (c['create'] + c['conform']) > 0
        msg = (f"{c['create']} created, {c['conform']} conformed, "
               f"{c['extra']} extra deleted across {c['extra_meshes']} mesh(es), "
               f"{c['already']} already, {c['total']} total")
        return changed, msg
    else:  # collections
        res = outliner_cleanup.run_collection_cleanup(scene, color_map)
        if res is None:
            return False, "no scene"
        msg, n_moved = res
        return n_moved > 0, msg


def main():
    argv = sys.argv
    try:
        idx = argv.index("--")
    except ValueError:
        print("ERROR: Usage: ... batch_modeling.py -- <root_path> <clean|conform|collections>")
        sys.exit(1)

    rest = argv[idx + 1:]
    if len(rest) < 2:
        print("ERROR: expected <root_path> <clean|conform|collections>")
        sys.exit(1)
    root_path, mode = rest[0], rest[1]
    if mode not in ("clean", "conform", "collections"):
        print(f"ERROR: unknown mode '{mode}' "
              f"(expected clean|conform|collections)")
        sys.exit(1)
    if not os.path.isdir(root_path):
        print(f"ERROR: Not a directory: {root_path}")
        sys.exit(1)

    # Optional colour code for 'collections': <sm> <li> <other> tokens, so the
    # batch result matches the in-app colour code from the addon preferences.
    color_map = None
    if mode == "collections" and len(rest) >= 6:
        color_map = {'sm': rest[2], 'li': rest[3], 'other': rest[4],
                     'noexport': rest[5]}
        print(f"[MapLink Batch] colour code: {color_map}")

    blend_files = []
    for dirpath, _, filenames in os.walk(root_path):
        for fn in filenames:
            if fn.endswith('.blend') and not fn.startswith('_'):
                stem = os.path.splitext(fn)[0]
                if stem.startswith(_MAPLINK_PREFIXES):
                    blend_files.append(os.path.join(dirpath, fn))
    blend_files.sort()
    total = len(blend_files)
    label = {"clean": "Clean Outliner", "conform": "Conform VC",
             "collections": "Clean Collections"}[mode]
    print(f"[MapLink Batch] {label}: found {total} MapLink .blend under "
          f"{root_path}")

    has_p4 = _p4_available()
    cl = None
    if has_p4:
        print(f"[MapLink Batch] [P4] available — client={_get_client()}, "
              f"user={_get_user()}")
        cl = _get_or_create_cl(_CL_DESC[mode])
    else:
        print("[MapLink Batch] [P4] p4 not found — will clear read-only bit "
              "directly")

    n_changed = n_unchanged = n_err = 0
    for i, blend_path in enumerate(blend_files):
        stem = os.path.splitext(os.path.basename(blend_path))[0]
        print(f"[MapLink Batch] ({i+1}/{total}) {stem}")

        try:
            bpy.ops.wm.open_mainfile(filepath=blend_path)
        except Exception as exc:
            print(f"  ERROR opening: {exc}")
            n_err += 1
            continue

        try:
            changed, summary = _apply(mode, color_map)
            print(f"  {summary}")
            if not changed:
                n_unchanged += 1
                continue

            # Only files that actually change get checked out + saved.
            if has_p4:
                ok, action = _p4_edit(blend_path, cl)
                if not ok:
                    print(f"  [P4] checkout FAILED: {action} — trying chmod")
                    _make_writable(blend_path)
                else:
                    print(f"  [P4] {action}")
            else:
                _make_writable(blend_path)

            bpy.ops.wm.save_mainfile(compress=True)
            print("  SAVED")
            n_changed += 1
        except Exception:
            traceback.print_exc()
            n_err += 1

    print(f"[MapLink Batch] {label} done: {n_changed} changed+saved, "
          f"{n_unchanged} unchanged, {n_err} errors, {total} total"
          + (f" (CL {cl})" if cl else ""))


if __name__ == "__main__":
    main()
