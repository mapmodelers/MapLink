"""MapLink batch FBX export — pre-populate the cache for many .blend files.

Walks each root recursively, finds BP_/LI_/SM_*.blend files, and runs
`maplink_export.py --mode both` on each in parallel.  Same work the
MapLink_Blender addon does on save_post for one file, just done in bulk so
you don't have to open & save every .blend manually.

Run via the .cmd wrapper, or directly with Blender's Python:

    blender.exe --background --factory-startup --python maplink_batch_export.py -- \\
        --plugin-dir  C:/path/to/MapLink \\
        --blender-exe C:/path/to/blender.exe \\
        C:/Blender/BP  C:/Blender/LI  C:/Blender/SM
"""

import argparse
import concurrent.futures as cf
import os
import subprocess
import sys
import time


_MAPLINK_PREFIXES = ("BP_", "LI_", "SM_")


def _parse_args():
    # Strip everything before '--' (Blender's convention when invoked via --python).
    argv = sys.argv
    if '--' in argv:
        argv = argv[argv.index('--') + 1:]
    else:
        argv = argv[1:]

    ap = argparse.ArgumentParser(description="MapLink batch FBX export")
    ap.add_argument('--plugin-dir', required=True,
                    help="MapLink UE plugin root (contains Scripts/maplink_export.py)")
    ap.add_argument('--artifact-root', default='',
                    help="v1.13: root for generated Logs/Export artifacts "
                         "(UE uses <ProjectSaved>/MapLink; defaults to "
                         "--plugin-dir, legacy layout)")
    ap.add_argument('--blender-exe', required=True,
                    help="Path to blender.exe")
    ap.add_argument('--workers', type=int, default=4,
                    help="Parallel Blender processes (default: 4)")
    ap.add_argument('--timeout', type=int, default=300,
                    help="Per-file timeout seconds (default: 300)")
    ap.add_argument('--dry-run', action='store_true',
                    help="List files that would be exported, don't run.")
    ap.add_argument('roots', nargs='+',
                    help="Root directories to scan recursively")
    return ap.parse_args(argv)


def _find_blends(roots):
    found = []
    for root in roots:
        if not os.path.isdir(root):
            print(f"[batch] WARN: not a directory, skipping: {root}", flush=True)
            continue
        for dirpath, _dirs, files in os.walk(root):
            for fn in files:
                if not fn.lower().endswith('.blend'):
                    continue
                stem = os.path.splitext(fn)[0]
                if not stem.startswith(_MAPLINK_PREFIXES):
                    continue
                found.append(os.path.normpath(os.path.join(dirpath, fn)))
    return sorted(set(found))


def _export_one(blender_exe, plugin_dir, blend_path, timeout,
                artifact_root=''):
    script = os.path.join(plugin_dir, 'Scripts', 'maplink_export.py')
    args = [
        blender_exe, '--background', '--factory-startup',
        '--python', script, '--',
        '--mode',   'both',
        '--blend',  blend_path,
        '--outdir', artifact_root or plugin_dir,
    ]
    t0 = time.monotonic()
    try:
        proc = subprocess.run(
            args,
            capture_output=True, text=True,
            encoding='utf-8', errors='replace',
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return (blend_path, 'timeout', '', time.monotonic() - t0)
    except Exception as exc:
        return (blend_path, 'error', f"{type(exc).__name__}: {exc}", time.monotonic() - t0)

    elapsed = time.monotonic() - t0
    if proc.returncode != 0:
        # Keep only the last few lines so failure tails are short.
        tail = '\n'.join((proc.stdout or '').splitlines()[-15:])
        return (blend_path, 'fail', tail, elapsed)
    return (blend_path, 'ok', '', elapsed)


def main():
    opts = _parse_args()

    if not os.path.isfile(opts.blender_exe):
        print(f"[batch] ERROR: Blender exe not found: {opts.blender_exe}")
        return 1
    script = os.path.join(opts.plugin_dir, 'Scripts', 'maplink_export.py')
    if not os.path.isfile(script):
        print(f"[batch] ERROR: maplink_export.py not found: {script}")
        return 1

    blends = _find_blends(opts.roots)
    print(f"[batch] Found {len(blends)} MapLink .blend file(s) under {len(opts.roots)} root(s).")
    if not blends:
        return 0

    if opts.dry_run:
        for b in blends:
            print(f"  would export: {b}")
        return 0

    print(f"[batch] Plugin:  {opts.plugin_dir}")
    print(f"[batch] Blender: {opts.blender_exe}")
    print(f"[batch] Workers: {opts.workers}")
    print("[batch] " + "-" * 60, flush=True)

    start = time.monotonic()
    successes = []
    failures  = []

    with cf.ThreadPoolExecutor(max_workers=opts.workers) as ex:
        futures = {
            ex.submit(_export_one, opts.blender_exe, opts.plugin_dir, b,
                      opts.timeout, opts.artifact_root): b
            for b in blends
        }
        for done_idx, fut in enumerate(cf.as_completed(futures), 1):
            path, status, msg, elapsed = fut.result()
            stem = os.path.splitext(os.path.basename(path))[0]
            tag  = 'OK  ' if status == 'ok' else status.upper().ljust(4)
            print(f"[{done_idx:3d}/{len(blends)}] {tag} {stem:42s} ({elapsed:5.1f}s)",
                  flush=True)
            if status == 'ok':
                successes.append(path)
            else:
                failures.append((path, msg))

    total = time.monotonic() - start
    print("[batch] " + "-" * 60)
    print(f"[batch] Done in {total:.1f}s — {len(successes)} ok, {len(failures)} failed.")

    if failures:
        print("[batch] Failures (last 3 lines of each):")
        for path, msg in failures:
            print(f"  {path}")
            for line in msg.splitlines()[-3:]:
                print(f"      {line}")
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
