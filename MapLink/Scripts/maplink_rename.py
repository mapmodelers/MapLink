#!/usr/bin/env python3
"""
MapLink Blend Rename Tool
=========================
Recursively scans .blend file dependencies, lets you plan renames via a JSON file,
then executes renames with Perforce integration (move, checkout, add, delete, clean CL).

Usage (run via headless Blender):
  blender --background --python maplink_rename.py -- --mode scan --blend <file> [--plan rename_plan.json]
  blender --background --python maplink_rename.py -- --mode scan   (prompts for the parent .blend path)
  blender --background --python maplink_rename.py -- --mode execute --plan rename_plan.json [--dry-run]

Directory convention:
  <root>/<PREFIX>/<STEM>/<STEM>.blend
  e.g. C:/Project/Blender/BP/BP_wall/BP_wall.blend
  Prefix = everything before the first '_' in the stem.
  Root is auto-derived: 3 directories up from the .blend file.
"""

import bpy
import sys
import os
import json
import argparse
import subprocess
import shutil
import glob
from datetime import datetime, timezone
from collections import OrderedDict

# ---------------------------------------------------------------------------
# Arg parsing (everything after ' -- ')
# ---------------------------------------------------------------------------

def parse_args():
    argv = sys.argv
    if '--' in argv:
        argv = argv[argv.index('--') + 1:]
    else:
        argv = []

    ap = argparse.ArgumentParser(description='MapLink Blend Rename Tool')
    ap.add_argument('--mode', required=True, choices=['scan', 'execute'],
                    help='scan = discover deps & write plan; execute = apply renames')
    ap.add_argument('--root', default=None,
                    help='Root Blender directory. Auto-derived from --blend if omitted.')
    ap.add_argument('--blend', default=None,
                    help='Parent .blend file to scan from. Prompted if omitted in scan mode.')
    ap.add_argument('--plan', default='rename_plan.json',
                    help='Path to rename plan JSON (default: rename_plan.json)')
    ap.add_argument('--dry-run', action='store_true',
                    help='Execute mode: log all operations without touching files or P4')
    ap.add_argument('--p4-port', default=None, help='P4PORT override')
    ap.add_argument('--p4-client', default=None, help='P4CLIENT override')
    ap.add_argument('--p4-user', default=None, help='P4USER override')
    return ap.parse_args(argv)


def derive_root_from_blend(blend_path):
    """
    Auto-derive the root directory from a .blend path using the convention:
      <root>/<PREFIX>/<STEM>/<STEM>.blend
    Root = 3 directories up from the .blend file.

    Example:
      C:/MapLink/MyProject/LI/LI_MyLevel/LI_MyLevel.blend
      -> C:/MapLink/MyProject
    """
    p = norm(blend_path)
    for _ in range(3):
        p = os.path.dirname(p)
    return p


def prompt_blend_path():
    """Prompt the user for the parent .blend file path."""
    print('')
    print('=' * 60)
    print('  MapLink Rename Tool — Scan Mode')
    print('=' * 60)
    print('')
    print('  Enter the path to the PARENT .blend file.')
    print('  This is the top-level file whose full dependency')
    print('  tree will be scanned for rename candidates.')
    print('')
    print('  Example:')
    # Generic on purpose: this is help text a customer reads. It also shows
    # the CURRENT layout (<root>/<TYPE>/<Name>/<Name>.blend) rather than the
    # pre-v1.12 one with a 'Blender' folder in the middle.
    print('    C:\\MapLink\\MyProject\\LI\\LI_MyLevel\\LI_MyLevel.blend')
    print('')
    path = input('  Parent .blend path: ').strip().strip('"').strip("'")
    print('')
    return path


# ---------------------------------------------------------------------------
# Utility helpers
# ---------------------------------------------------------------------------

def norm(path):
    """Normalize a path to forward slashes, resolved absolute."""
    return os.path.normpath(os.path.abspath(path)).replace('\\', '/')


def stem(path):
    """Filename without extension."""
    return os.path.splitext(os.path.basename(path))[0]


def prefix_from_name(name):
    """Extract prefix before the first underscore. BP_wall -> BP, SM_rock -> SM."""
    idx = name.find('_')
    if idx > 0:
        return name[:idx]
    return name  # fallback: no underscore, use full name as prefix


def compute_path(root, name):
    """Compute the canonical path for a .blend with the given name under root."""
    pfx = prefix_from_name(name)
    return norm(os.path.join(root, pfx, name, name + '.blend'))


def log(msg, indent=0):
    prefix = '  ' * indent
    print(f'[MapLink Rename] {prefix}{msg}')


def log_dry(msg, dry_run, indent=0):
    tag = '[DRY-RUN] ' if dry_run else ''
    prefix = '  ' * indent
    print(f'[MapLink Rename] {tag}{prefix}{msg}')


# ---------------------------------------------------------------------------
# Blender dependency scanning
# ---------------------------------------------------------------------------

def scan_libraries(blend_path):
    """
    Open a .blend file and return a list of absolute paths it references
    via bpy.data.libraries (linked .blend files).
    """
    blend_path = norm(blend_path)
    if not os.path.isfile(blend_path):
        log(f'WARNING: File not found, skipping: {blend_path}')
        return []

    try:
        bpy.ops.wm.open_mainfile(filepath=blend_path)
    except Exception as e:
        log(f'WARNING: Could not open {blend_path}: {e}')
        return []

    deps = []
    for lib in bpy.data.libraries:
        abs_path = norm(bpy.path.abspath(lib.filepath, start=os.path.dirname(blend_path)))
        if abs_path != blend_path:
            deps.append(abs_path)

    return deps


def build_dependency_graph(root_blend):
    """
    Recursively scan from root_blend. Returns:
      graph: dict { blend_path: [dep_paths] }   (adjacency list)
    """
    graph = {}
    queue = [norm(root_blend)]
    visited = set()

    while queue:
        current = queue.pop(0)
        if current in visited:
            continue
        visited.add(current)

        deps = scan_libraries(current)
        graph[current] = deps

        for dep in deps:
            if dep not in visited:
                queue.append(dep)

    return graph


def compute_referenced_by(graph):
    """Invert the dependency graph: for each file, who references it."""
    ref_by = {}
    for path in graph:
        ref_by.setdefault(path, [])
    for parent, deps in graph.items():
        for dep in deps:
            ref_by.setdefault(dep, []).append(parent)
    return ref_by


def glob_all_blends(root):
    """Find every .blend file under root."""
    results = []
    for dirpath, dirnames, filenames in os.walk(root):
        for fn in filenames:
            if fn.endswith('.blend') and not fn.endswith('.blend1') and not fn.endswith('.blend2'):
                results.append(norm(os.path.join(dirpath, fn)))
    return results


def find_all_references_to(target_path, all_blends, graph):
    """
    Find ALL .blend files under root that reference target_path,
    including those outside the direct dependency tree.
    Opens each file not yet in graph to check.
    """
    referencing = []
    target_norm = norm(target_path)

    for blend_path in all_blends:
        blend_norm = norm(blend_path)
        if blend_norm == target_norm:
            continue

        # Check if already scanned
        if blend_norm in graph:
            if target_norm in graph[blend_norm]:
                referencing.append(blend_norm)
        else:
            # Quick scan of this file's libraries
            deps = scan_libraries(blend_path)
            graph[blend_norm] = deps  # cache it
            if target_norm in deps:
                referencing.append(blend_norm)

    return referencing


# ---------------------------------------------------------------------------
# SCAN MODE
# ---------------------------------------------------------------------------

def run_scan(opts):
    root_blend = norm(opts.blend)

    # Auto-derive root if not explicitly given
    if opts.root:
        root = norm(opts.root)
    else:
        root = derive_root_from_blend(root_blend)
        log(f'Auto-derived root: {root}')

    if not os.path.isdir(root):
        log(f'ERROR: Root directory does not exist: {root}')
        sys.exit(1)
    if not os.path.isfile(root_blend):
        log(f'ERROR: Parent .blend file does not exist: {root_blend}')
        sys.exit(1)

    log(f'Parent .blend: {root_blend}')
    log(f'Root directory: {root}')

    # Phase 1: Walk dependency tree from root .blend
    graph = build_dependency_graph(root_blend)

    log(f'Found {len(graph)} .blend files in dependency tree')

    # Phase 2: Glob ALL .blend files under root for reverse-reference check
    log('Globbing all .blend files under root for cross-reference check...')
    all_blends = glob_all_blends(root)
    log(f'Found {len(all_blends)} total .blend files under root')

    # Phase 3: For each file in the dep tree, find ALL files that reference it
    # (including outside the tree)
    full_referenced_by = {}
    for blend_path in list(graph.keys()):
        refs = find_all_references_to(blend_path, all_blends, graph)
        full_referenced_by[blend_path] = refs

    # Phase 4: Build plan JSON
    assets = OrderedDict()
    for blend_path in sorted(graph.keys()):
        name = stem(blend_path)
        direct_deps = [stem(d) for d in graph.get(blend_path, [])]
        refs = [stem(r) for r in full_referenced_by.get(blend_path, [])]

        assets[name] = {
            'current_name': name,
            'new_name': name,  # <-- USER EDITS THIS TO RENAME
            'current_path': blend_path,
            'references': sorted(direct_deps),
            'referenced_by': sorted(set(refs)),
        }

    plan = {
        '_comment': (
            'Edit "new_name" for any asset you want to rename. '
            'The tool computes new paths from the prefix convention. '
            'Run with --mode execute --plan <this file> to apply.'
        ),
        'root': root,
        'parent_blend': root_blend,
        'scan_timestamp': datetime.now(timezone.utc).isoformat(),
        'assets': assets,
    }

    plan_path = norm(opts.plan)
    with open(plan_path, 'w', encoding='utf-8') as f:
        json.dump(plan, f, indent=2, ensure_ascii=False)

    log(f'Wrote rename plan to: {plan_path}')
    log(f'Total assets in plan: {len(assets)}')
    log('')
    log('=== Dependency tree ===')
    for name, info in assets.items():
        refs_str = ', '.join(info['references']) if info['references'] else '(none)'
        log(f'  {name}  ->  [{refs_str}]')
    log('')
    log(f'Edit "{plan_path}" to set new_name values, then run:')
    log(f'  blender --background --python maplink_rename.py -- --mode execute --plan "{plan_path}"')
    log(f'  Add --dry-run for a safe preview first.')


# ---------------------------------------------------------------------------
# Perforce helpers
# ---------------------------------------------------------------------------

class P4:
    """Thin wrapper around the p4 CLI."""

    def __init__(self, port=None, client=None, user=None, dry_run=False):
        self.dry_run = dry_run
        self.env_overrides = {}
        if port:
            self.env_overrides['P4PORT'] = port
        if client:
            self.env_overrides['P4CLIENT'] = client
        if user:
            self.env_overrides['P4USER'] = user

    def _run(self, *args, check=True, allow_failure=False):
        cmd = ['p4'] + list(args)
        env = os.environ.copy()
        env.update(self.env_overrides)

        if self.dry_run:
            log_dry(f'p4 {" ".join(args)}', dry_run=True, indent=2)
            return ''

        try:
            result = subprocess.run(
                cmd, capture_output=True, text=True, env=env,
                timeout=30
            )
            if check and result.returncode != 0 and not allow_failure:
                log(f'P4 ERROR: {" ".join(cmd)}')
                log(f'  stdout: {result.stdout.strip()}')
                log(f'  stderr: {result.stderr.strip()}')
            return result.stdout
        except FileNotFoundError:
            log('ERROR: p4 CLI not found. Is Perforce installed and in PATH?')
            sys.exit(1)
        except subprocess.TimeoutExpired:
            log(f'WARNING: p4 command timed out: {" ".join(cmd)}')
            return ''

    def create_changelist(self, description):
        """Create a new pending CL, return its number."""
        if self.dry_run:
            log_dry(f'p4 change -i (create CL: "{description}")', dry_run=True, indent=2)
            return '99999'

        spec = self._run('change', '-o')
        # Replace description
        lines = spec.split('\n')
        new_lines = []
        in_desc = False
        for line in lines:
            if line.startswith('Description:'):
                new_lines.append('Description:')
                new_lines.append(f'\t{description}')
                in_desc = True
                continue
            if in_desc:
                if line.startswith('\t'):
                    continue  # skip old description lines
                else:
                    in_desc = False
            # Remove Files section (new CL should be empty)
            if line.startswith('Files:'):
                break
            new_lines.append(line)

        new_spec = '\n'.join(new_lines) + '\n'

        result = subprocess.run(
            ['p4', 'change', '-i'],
            input=new_spec, capture_output=True, text=True,
            env={**os.environ, **self.env_overrides}
        )

        # Parse "Change NNNNN created."
        output = result.stdout.strip()
        for word in output.split():
            if word.isdigit():
                return word

        log(f'WARNING: Could not parse CL number from: {output}')
        return None

    def edit(self, cl, filepath):
        """Open file for edit in the given CL."""
        self._run('edit', '-c', cl, filepath, allow_failure=True)

    def move(self, cl, old_path, new_path):
        """Move (rename) a file in P4."""
        self._run('move', '-c', cl, old_path, new_path, allow_failure=True)

    def add(self, cl, filepath):
        """Mark a file for add."""
        self._run('add', '-c', cl, filepath, allow_failure=True)

    def delete(self, cl, filepath):
        """Mark a file for delete."""
        self._run('delete', '-c', cl, filepath, allow_failure=True)

    def reconcile(self, cl, path):
        """Reconcile a path (catches adds/edits/deletes P4 doesn't know about)."""
        self._run('reconcile', '-c', cl, path, allow_failure=True)

    def revert_unchanged(self, cl):
        """Revert unchanged files in the CL."""
        self._run('revert', '-a', '-c', cl, allow_failure=True)


# ---------------------------------------------------------------------------
# File operations
# ---------------------------------------------------------------------------

def move_blend_directory(old_blend_path, new_blend_path, p4, cl, dry_run):
    """
    Move all files from the old .blend's directory to the new directory.
    Uses P4 move for tracked files.
    """
    old_dir = os.path.dirname(old_blend_path)
    new_dir = os.path.dirname(new_blend_path)

    if norm(old_dir) == norm(new_dir):
        # Same directory, just rename the .blend file itself
        log_dry(f'Renaming in-place: {os.path.basename(old_blend_path)} -> {os.path.basename(new_blend_path)}', dry_run, indent=2)
        p4.edit(cl, old_blend_path)
        p4.move(cl, old_blend_path, new_blend_path)
        if not dry_run:
            # p4 move should handle the filesystem move, but ensure dir exists
            pass
        return

    # Different directory: move all files
    if not dry_run:
        os.makedirs(new_dir, exist_ok=True)

    if not os.path.isdir(old_dir):
        log(f'WARNING: Source directory does not exist: {old_dir}', indent=2)
        return

    old_name_stem = stem(old_blend_path)
    new_name_stem = stem(new_blend_path)

    for filename in os.listdir(old_dir):
        old_file = norm(os.path.join(old_dir, filename))
        # Rename files that contain the old stem
        new_filename = filename.replace(old_name_stem, new_name_stem)
        new_file = norm(os.path.join(new_dir, new_filename))

        log_dry(f'Move: {old_file} -> {new_file}', dry_run, indent=3)

        p4.edit(cl, old_file)
        p4.move(cl, old_file, new_file)

        if not dry_run and os.path.exists(old_file) and not os.path.exists(new_file):
            # Fallback: if p4 move didn't do the filesystem move (e.g. file not tracked)
            os.makedirs(os.path.dirname(new_file), exist_ok=True)
            shutil.move(old_file, new_file)

    # Clean up empty old directory
    if not dry_run and os.path.isdir(old_dir) and not os.listdir(old_dir):
        try:
            os.rmdir(old_dir)
            log(f'Removed empty directory: {old_dir}', indent=3)
            # Also try removing the parent prefix dir if empty
            parent = os.path.dirname(old_dir)
            if os.path.isdir(parent) and not os.listdir(parent):
                os.rmdir(parent)
                log(f'Removed empty prefix directory: {parent}', indent=3)
        except OSError:
            pass


def rewrite_library_paths(blend_path, rename_map, dry_run):
    """
    Open a .blend file and update library paths that match rename_map.
    Also matches by filename stem for cross-references between renamed files.
    """
    blend_path = norm(blend_path)
    if not os.path.isfile(blend_path):
        log(f'WARNING: File not found: {blend_path}', indent=2)
        return False

    if dry_run:
        log_dry(f'Would rewrite library paths in: {blend_path}', dry_run, indent=2)
        return True

    try:
        bpy.ops.wm.open_mainfile(filepath=blend_path)
    except Exception as e:
        log(f'WARNING: Could not open {blend_path}: {e}', indent=2)
        return False

    blend_dir = os.path.dirname(blend_path)
    changed = False

    # Build stem-based lookup for cross-references between renamed files
    stem_to_new = {}
    for old_abs, new_abs in rename_map.items():
        stem_to_new[stem(old_abs)] = new_abs

    for lib in bpy.data.libraries:
        lib_abs = norm(bpy.path.abspath(lib.filepath, start=blend_dir))

        new_abs = None
        if lib_abs in rename_map:
            new_abs = rename_map[lib_abs]
        else:
            lib_stem = stem(lib_abs)
            if lib_stem in stem_to_new:
                new_abs = stem_to_new[lib_stem]

        if new_abs:
            new_rel = os.path.relpath(new_abs, blend_dir).replace(chr(92), '/')
            new_rel_blender = '//' + new_rel
            log(f'Rewriting lib: {lib.filepath} -> {new_rel_blender}', indent=3)
            lib.filepath = new_rel_blender
            changed = True

    if changed:
        try:
            # Save uncompressed so binary rename step can find strings
            bpy.ops.wm.save_as_mainfile(filepath=blend_path, compress=False)
            log(f'Saved (uncompressed): {blend_path}', indent=3)
        except Exception as e:
            log(f'WARNING: Could not save {blend_path}: {e}', indent=3)
            return False

    return changed


def binary_rename_in_blend(blend_path, name_map, dry_run):
    """
    Decompress a .blend file via bpy, then binary search-and-replace to rename
    collection references. Bypasses bpy's read-only limitation on linked collection names.
    Only safe when old and new names have the same byte length (e.g. BP_ -> SM_).
    """
    blend_path_native = blend_path.replace('/', os.sep)
    if not os.path.isfile(blend_path_native):
        log(f'WARNING: File not found for binary rename: {blend_path}', indent=2)
        return False

    if dry_run:
        log_dry(f'Would binary-rename in: {blend_path}', dry_run, indent=2)
        return True

    # Step A: Open with bpy and save UNCOMPRESSED so raw bytes are searchable
    try:
        bpy.ops.wm.open_mainfile(filepath=blend_path_native)
        bpy.ops.wm.save_as_mainfile(filepath=blend_path_native, compress=False)
    except Exception as e:
        log(f'WARNING: Could not decompress via bpy: {e}', indent=3)
        # Try alternate method
        try:
            bpy.ops.wm.save_mainfile(filepath=blend_path_native, compress=False)
        except Exception as e2:
            log(f'WARNING: Alternate save also failed: {e2}', indent=3)

    # Step B: Verify file is uncompressed
    with open(blend_path_native, 'rb') as f:
        header = f.read(7)
    if header != b'BLENDER':
        log(f'WARNING: File still compressed after bpy save (header: {header[:4].hex()}): {os.path.basename(blend_path)}', indent=3)

    # Step C: Binary replace
    with open(blend_path_native, 'rb') as f:
        data = f.read()

    original_len = len(data)
    replacements = 0

    for old_name, new_name in name_map.items():
        old_bytes = old_name.encode('utf-8')
        new_bytes = new_name.encode('utf-8')
        if len(old_bytes) != len(new_bytes):
            log(f'WARNING: Skipping {old_name}->{new_name}: different byte lengths', indent=3)
            continue
        count = data.count(old_bytes)
        if count > 0:
            data = data.replace(old_bytes, new_bytes)
            replacements += count

    if replacements > 0:
        assert len(data) == original_len, 'Binary rename changed file size!'
        with open(blend_path_native, 'wb') as f:
            f.write(data)
        log(f'Binary-renamed {replacements} occurrence(s) in {os.path.basename(blend_path)}', indent=3)
    else:
        log(f'No BP_ references found in {os.path.basename(blend_path)}', indent=3)

    return replacements > 0


def rename_root_collection(blend_path, old_name, new_name, dry_run):
    """
    Open a .blend and rename the root collection from old_name to new_name.
    """
    if dry_run:
        log_dry(f'Would rename root collection "{old_name}" -> "{new_name}" in {blend_path}', dry_run, indent=2)
        return True

    blend_path = norm(blend_path)
    if not os.path.isfile(blend_path):
        return False

    try:
        bpy.ops.wm.open_mainfile(filepath=blend_path)
    except Exception as e:
        log(f'WARNING: Could not open {blend_path}: {e}', indent=2)
        return False

    renamed = False
    for coll in bpy.data.collections:
        if coll.name == old_name:
            coll.name = new_name
            log(f'Renamed collection: {old_name} -> {new_name}', indent=3)
            renamed = True
            break

    # Also check scene collection children
    if not renamed:
        scene_coll = bpy.context.scene.collection
        # Try the scene's own name or first child collection
        for child in scene_coll.children:
            if child.name == old_name:
                child.name = new_name
                log(f'Renamed scene child collection: {old_name} -> {new_name}', indent=3)
                renamed = True
                break

    if renamed:
        try:
            bpy.ops.wm.save_as_mainfile(filepath=blend_path, compress=False)
        except Exception as e:
            log(f'WARNING: Could not save after collection rename: {e}', indent=3)

    return renamed


# ---------------------------------------------------------------------------
# EXECUTE MODE
# ---------------------------------------------------------------------------

def run_execute(opts):
    plan_path = norm(opts.plan)
    dry_run = opts.dry_run

    if not os.path.isfile(plan_path):
        log(f'ERROR: Plan file not found: {plan_path}')
        sys.exit(1)

    with open(plan_path, 'r', encoding='utf-8') as f:
        plan = json.load(f)

    root = norm(plan['root'])
    assets = plan['assets']

    # --- Identify renames ---
    renames = {}  # old_name -> new_name
    for key, info in assets.items():
        old_name = info['current_name']
        new_name = info['new_name']
        if old_name != new_name:
            renames[old_name] = new_name

    if not renames:
        log('No renames specified in plan (all new_name == current_name). Nothing to do.')
        return

    log(f'Found {len(renames)} rename(s) to execute:')
    for old, new in renames.items():
        log(f'  {old} -> {new}', indent=1)

    # --- Compute paths ---
    # old_abs_path -> new_abs_path for .blend files
    path_map = {}
    for old_name, new_name in renames.items():
        old_path = norm(assets[old_name]['current_path'])
        new_path = compute_path(root, new_name)
        path_map[old_path] = new_path
        log(f'  Path: {old_path}', indent=1)
        log(f'    ->  {new_path}', indent=1)

    # --- Identify files that need library path rewriting ---
    # Any .blend that references a renamed file (by old path) needs updating.
    # This includes: files in the plan's referenced_by, PLUS the renamed files
    # themselves if they reference other renamed files.
    files_needing_rewrite = set()

    for old_name, new_name in renames.items():
        info = assets[old_name]
        # All files that reference this one
        for ref_name in info.get('referenced_by', []):
            if ref_name in assets:
                ref_info = assets[ref_name]
                ref_path = ref_info['current_path']
                # If this referencing file is ALSO being renamed, use its NEW path
                if ref_name in renames:
                    ref_path = compute_path(root, renames[ref_name])
                files_needing_rewrite.add(norm(ref_path))

        # If the renamed file itself references other renamed files, it needs rewriting too
        for dep_name in info.get('references', []):
            if dep_name in renames:
                # This file references another renamed file
                new_path = compute_path(root, new_name)
                files_needing_rewrite.add(norm(new_path))

    # Don't include files that are being renamed in rewrite (they'll be rewritten
    # after move). Actually, DO include them — they might reference other renamed files.

    log(f'\nFiles needing library path rewrite: {len(files_needing_rewrite)}')
    for fp in sorted(files_needing_rewrite):
        log(f'  {fp}', indent=1)

    # --- P4 setup ---
    p4 = P4(
        port=opts.p4_port,
        client=opts.p4_client,
        user=opts.p4_user,
        dry_run=dry_run
    )

    timestamp = datetime.now().strftime('%Y-%m-%d %H:%M')
    desc_lines = [f'[MapLink Rename] {timestamp}']
    for old, new in renames.items():
        desc_lines.append(f'  {old} -> {new}')
    cl_desc = '\n'.join(desc_lines)

    log(f'\nCreating P4 changelist...')
    cl = p4.create_changelist(cl_desc)
    if cl:
        log(f'CL: {cl}')
    else:
        log('WARNING: Could not create CL, proceeding with default CL')
        cl = 'default'

    # === STEP 1: Move/rename files on disk via P4 ===
    log(f'\n=== Step 1: Moving files ===')
    for old_name, new_name in renames.items():
        old_path = norm(assets[old_name]['current_path'])
        new_path = compute_path(root, new_name)

        log(f'Moving: {old_name} -> {new_name}', indent=1)
        move_blend_directory(old_path, new_path, p4, cl, dry_run)

    # === STEP 2: Rename local collections in source files (bpy) ===
    # Local collections ARE writable. Saves uncompressed for step 4.
    log(f'\n=== Step 2: Renaming collections in source files ===')
    for old_name, new_name in renames.items():
        new_path = compute_path(root, new_name)
        rename_root_collection(new_path, old_name, new_name, dry_run)

    # === STEP 3: Rewrite library paths in parent files (bpy) ===
    # Saves uncompressed for step 4.
    log(f'\n=== Step 3: Rewriting library paths ===')
    for file_path in sorted(files_needing_rewrite):
        is_renamed = file_path in path_map.values()
        if not is_renamed:
            log(f'Checking out: {file_path}', indent=1)
            p4.edit(cl, file_path)

        log(f'Rewriting: {file_path}', indent=1)
        rewrite_library_paths(file_path, path_map, dry_run)

    # === STEP 4: Binary-rename BP_->SM_ in ALL affected .blend files ===
    # Steps 2+3 saved files uncompressed, so raw byte search works.
    # This catches: linked collection refs in parents, asset metadata, etc.
    name_map = dict(renames)

    all_files_for_binary = set()
    for old_name, new_name in renames.items():
        all_files_for_binary.add(compute_path(root, new_name))
    all_files_for_binary.update(files_needing_rewrite)

    log(f'\n=== Step 4: Binary-renaming collections ({len(all_files_for_binary)} files) ===')
    for file_path in sorted(all_files_for_binary):
        if file_path not in path_map.values():
            p4.edit(cl, file_path)
        binary_rename_in_blend(file_path, name_map, dry_run)

    # === STEP 5: Reconcile and clean CL ===
    log(f'\n=== Step 5: Cleaning up CL ===')
    if cl != 'default':
        p4.revert_unchanged(cl)

    # === STEP 6: Writing execution report ===
    log(f'\n=== Step 6: Writing execution report ===')
    report = {
        'executed_at': datetime.now(timezone.utc).isoformat(),
        'dry_run': dry_run,
        'p4_changelist': cl,
        'renames': [],
    }
    for old_name, new_name in renames.items():
        old_path = norm(assets[old_name]['current_path'])
        new_path = compute_path(root, new_name)
        report['renames'].append({
            'old_name': old_name,
            'new_name': new_name,
            'old_path': old_path,
            'new_path': new_path,
        })
    report['files_rewritten'] = sorted(files_needing_rewrite)

    report_path = plan_path.replace('.json', '_report.json')
    if not dry_run:
        with open(report_path, 'w', encoding='utf-8') as f:
            json.dump(report, f, indent=2, ensure_ascii=False)
        log(f'Report written to: {report_path}')

    log(f'\n=== Done {"(DRY RUN)" if dry_run else ""} ===')
    if cl != 'default':
        log(f'P4 Changelist: {cl}')
    log(f'Renames applied: {len(renames)}')
    log(f'Files rewritten: {len(files_needing_rewrite)}')

    if dry_run:
        log('\nRe-run without --dry-run to apply for real.')


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    opts = parse_args()

    if opts.mode == 'scan':
        # Prompt for parent .blend if not provided via CLI
        if not opts.blend:
            opts.blend = prompt_blend_path()
        if not opts.blend:
            log('ERROR: No parent .blend path provided.')
            sys.exit(1)
        run_scan(opts)

    elif opts.mode == 'execute':
        if not opts.plan:
            log('ERROR: --plan is required for execute mode.')
            sys.exit(1)
        run_execute(opts)


if __name__ == '__main__':
    main()
