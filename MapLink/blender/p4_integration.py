"""MapLink_Blender — Perforce integration.

Provides Show In Explorer, Checkout, Revert operators and a shared
changelist with a fixed description ("[Blender] [MapLink] .blend
edition").  Existing pending CLs with that exact description are
reused; otherwise a new one is created.  CL is cached per session.

Adds a Ctrl+S override: if the active .blend is not currently opened in
Perforce, prompt the user to check it out before saving.

CONTRACT (v1.13 / addon 1.5.0): changelist DESCRIPTION STRINGS are the
cross-tool merge key.  For the same description string, this module and the
UE plugin (FMapLinkSourceControl::GetOrCreateChangelist) must find/reuse the
SAME pending CL — never create a duplicate — so Blender-side and UE-side
files stack together.  Matching uses _normalize_description(): per-line
trim, trailing empty lines dropped, case-SENSITIVE.  Keep it in sync with
NormalizeChangelistDescription() in MapLinkSourceControl.cpp.
"""

import subprocess
import os
import bpy
from bpy.types import Operator, PropertyGroup
from bpy.props import EnumProperty
from . import ui_menus

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

_P4_AVAILABLE: bool | None = None
_cached_cl: int | None = None
_cached_cl_desc: str = ""

_DEFAULT_CL_DESC = "[Blender] [MapLink] .blend edition"

# v1.21: CREATE_NO_WINDOW is Windows-only. Naming it directly raised
# AttributeError on macOS and Linux - and since Ctrl+S routes through
# file_p4_state(), that broke saving outright there.
_CF = getattr(subprocess, 'CREATE_NO_WINDOW', 0)


def _check_p4() -> bool:
    global _P4_AVAILABLE
    if _P4_AVAILABLE is None:
        try:
            subprocess.run(
                ["p4", "-V"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                creationflags=_CF,
            )
            _P4_AVAILABLE = True
        except FileNotFoundError:
            _P4_AVAILABLE = False
    return _P4_AVAILABLE


def p4_available() -> bool:
    return _check_p4()


# ---------------------------------------------------------------------------
# Where does THIS FILE stand with Perforce?  (v1.20)
# ---------------------------------------------------------------------------
# `p4 -V` answers "is the p4 executable on this machine".  That is a fact about
# the machine and never about the file, and it was half of the test that
# decided whether to prompt on save.  The other half was `p4 opened`, which
# returns nothing for three different states:
#
#   in the client view, in the depot, not opened -> the ONLY case worth a prompt
#   in the client view, never added             -> `p4 add`, not `p4 edit`
#   outside every client view                   -> nothing at all
#
# The third is the reported bug: a .blend in Downloads, on a machine with p4
# installed, was told it would be checked out before saving.  It also fires for
# a Shelf that lives outside the workspace, which is a MapLink project -- so
# scoping the prompt to MapLink projects does not fix it on its own.
#
# Measured against P4D 2025.2 (client root C:\Work):
#   where/fstat/opened, path outside the root -> rc 1, "is not under client's
#                                                root '<root>'."
#   fstat, path inside the view but unadded   -> rc 0, "... - no such file(s)."
#   opened, in depot but not checked out      -> rc 0, "file(s) not opened on
#                                                this client."
# So neither the return code nor the text is sufficient alone.  Both are read.

NOT_IN_WORKSPACE = 'NOT_IN_WORKSPACE'
NOT_IN_DEPOT = 'NOT_IN_DEPOT'
NEEDS_CHECKOUT = 'NEEDS_CHECKOUT'
CHECKED_OUT = 'CHECKED_OUT'

#: (P4CLIENT, client root).  Keyed on the client so switching workspaces
#: mid-session invalidates it rather than silently answering for the old one.
_client_root_cache: tuple = ()


def _client_root() -> str:
    global _client_root_cache
    client = _get_client()
    if _client_root_cache and _client_root_cache[0] == client:
        return _client_root_cache[1]
    root = ""
    out, _err, rc = _run_p4(["info"])
    if rc == 0:
        for line in out.splitlines():
            if line.startswith("Client root:"):
                root = line.split(":", 1)[1].strip()
                break
    if root.lower() in ("null", "none", ""):
        root = ""
    _client_root_cache = (client, root)
    return root


def _norm(path: str) -> str:
    """Compare form for a path: forward slashes, no trailing one, lower case.

    Separator-agnostic on purpose.  `os.sep` and `os.path.normcase` only know
    about the HOST, and v1.19 already paid for that once --
    `link_scope.is_pipeline_library` used `os.path.basename` on stored Windows
    paths and silently recognised nothing when the host was POSIX, which turned
    the sidecar gate off without saying so.  These paths come from `p4 info`
    and from `.blend` files, not from this machine's filesystem, so they get
    compared on their own terms.
    """
    if not path:
        return ""
    p = str(path).replace("\\", "/").rstrip("/")
    return p.lower()


def _is_under(path: str, root: str) -> bool:
    p, r = _norm(path), _norm(root)
    if not p or not r:
        return False
    return p == r or p.startswith(r + "/")


def file_p4_state(filepath: str) -> str:
    """Which of the four states *filepath* is in.  Never raises.

    Every failure mode answers NOT_IN_WORKSPACE: when MapLink cannot tell, it
    does nothing.  That is the correct default for a save the artist asked for.
    """
    if not filepath or not _check_p4():
        return NOT_IN_WORKSPACE

    # Cheap gate first.  After the first call this costs no subprocess at all,
    # which is what makes it safe to run on every Ctrl+S.
    root = _client_root()
    if root and not _is_under(filepath, root):
        return NOT_IN_WORKSPACE

    # Necessary but not sufficient: a client view can exclude paths under its
    # own root, so the mapping has to be asked for rather than inferred.
    out, err, rc = _run_p4(["where", filepath])
    blob = f"{out}\n{err}".lower()
    if (rc != 0
            or "not under client" in blob
            or "not in client view" in blob
            or not out.strip()):
        return NOT_IN_WORKSPACE

    if find_file_changelist(filepath) is not None:
        return CHECKED_OUT

    out, err, rc = _run_p4(["fstat", filepath])
    blob = f"{out}\n{err}".lower()
    if rc != 0:
        return NOT_IN_WORKSPACE
    if "no such file" in blob or "not on client" in blob:
        return NOT_IN_DEPOT
    if "... headaction delete" in blob:
        # In the depot, deleted at head.  `p4 edit` fails on it; a save here is
        # a re-add, so it belongs with the unadded case.
        return NOT_IN_DEPOT
    if "... depotfile" in blob or "... headrev" in blob:
        return NEEDS_CHECKOUT
    return NOT_IN_DEPOT


def _p4_outside_projects() -> bool:
    """The escape hatch preference.  Off by default; see MAPLINK_AP_Preferences."""
    try:
        prefs = bpy.context.preferences.addons[
            __package__.split('.')[0]].preferences
        return bool(getattr(prefs, "p4_outside_projects", False))
    except (KeyError, AttributeError):
        return False


def _run_p4(args: list[str]) -> tuple[str, str, int]:
    try:
        r = subprocess.run(
            ["p4"] + args,
            capture_output=True,
            text=True,
            creationflags=_CF,
        )
        return r.stdout.strip(), r.stderr.strip(), r.returncode
    except Exception as exc:
        return "", str(exc), 1


def _get_client() -> str:
    out, _, _ = _run_p4(["set", "P4CLIENT"])
    for line in out.splitlines():
        if "P4CLIENT" in line:
            return line.split("=", 1)[-1].strip().split()[0]
    return ""


def _get_user() -> str:
    out, _, _ = _run_p4(["set", "P4USER"])
    for line in out.splitlines():
        if "P4USER" in line:
            return line.split("=", 1)[-1].strip().split()[0]
    return ""


def _normalize_description(s: str) -> str:
    """v1.13: normalize a CL description for comparison — split into lines,
    trim each line, drop trailing empty lines, join with '\n'.  Case stays
    SENSITIVE.  Mirrors NormalizeChangelistDescription() on the UE side
    (MapLinkSourceControl.cpp) — the two must stay identical so both tools
    agree on "same CL"."""
    lines = [ln.strip() for ln in s.replace("\r\n", "\n").split("\n")]
    while lines and not lines[-1]:
        lines.pop()
    return "\n".join(lines)


def _find_pending_cl(description: str) -> int | None:
    """Search for an existing pending CL whose NORMALIZED description
    matches (see _normalize_description), supporting multi-line
    descriptions.  ``p4 changes -l`` prints the header line
    ``Change N on DATE by USER *pending*`` followed by one or more
    indented description lines."""
    client = _get_client()
    user = _get_user()
    # Don't bail when detection comes back empty — that would force a brand-new
    # CL every time and pile up duplicates.  Filter by whatever we have (still
    # prefer -u for correctness on shared servers); query unfiltered otherwise.
    args = ["changes", "-s", "pending", "-l"]
    if client:
        args += ["-c", client]
    if user:
        args += ["-u", user]
    out, _, rc = _run_p4(args)
    if rc != 0 or not out:
        return None

    target_desc = _normalize_description(description)

    current_cl: int | None = None
    current_lines: list[str] = []

    def _match(cl: int | None, lines: list[str]) -> int | None:
        if cl is None:
            return None
        desc = _normalize_description("\n".join(lines))
        return cl if desc == target_desc else None

    for line in out.splitlines():
        if line.startswith("Change "):
            # Entering a new CL — first check the one we just finished.
            matched = _match(current_cl, current_lines)
            if matched is not None:
                return matched
            parts = line.strip().split()
            if len(parts) >= 2 and parts[1].isdigit():
                current_cl = int(parts[1])
                current_lines = []
            else:
                current_cl = None
                current_lines = []
        elif current_cl is not None:
            # Keep interior empty lines — _normalize_description culls the
            # trailing ones, so per-line structure matches the UE side.
            current_lines.append(line.strip())

    # Tail-match the last CL in the output.
    return _match(current_cl, current_lines)


def _spec_is_pending_with_desc(spec_out: str, description: str) -> bool:
    """Parse ``p4 change -o <cl>`` output: True only if Status is pending
    and the Description field still normalized-matches *description*."""
    status_pending = False
    desc_lines: list[str] = []
    in_desc = False
    for line in spec_out.splitlines():
        if line.startswith("Status:"):
            status_pending = "pending" in line.lower()
            in_desc = False
        elif line.startswith("Description:"):
            in_desc = True
        elif in_desc:
            if line.startswith(("\t", " ")):
                desc_lines.append(line.strip())
            elif line.strip() == "":
                desc_lines.append("")
            else:
                in_desc = False
    if not status_pending:
        return False
    return (_normalize_description("\n".join(desc_lines))
            == _normalize_description(description))


def _get_or_create_changelist(description: str) -> int | None:
    """Find existing pending CL with *description* (normalized match — see
    _normalize_description) or create one.  Cached per session; the cache
    is verified before reuse.  NEVER creates while a match exists."""
    global _cached_cl, _cached_cl_desc

    # Return cached if description matches
    if _cached_cl is not None and _cached_cl_desc == description:
        # v1.13: verify the CL still exists, is still PENDING, and its
        # description still normalized-matches (it may have been submitted,
        # deleted, or re-described externally in P4V).
        out, _, rc = _run_p4(["change", "-o", str(_cached_cl)])
        if rc == 0 and _spec_is_pending_with_desc(out, description):
            return _cached_cl
        # Stale cache — drop and re-search.
        _cached_cl = None

    # Search for existing CL
    found = _find_pending_cl(description)
    if found is not None:
        _cached_cl = found
        _cached_cl_desc = description
        return found

    # Create new — each description line needs a leading tab so P4 treats
    # them all as part of the Description field.
    desc_block = "\n".join("\t" + ln for ln in description.splitlines()) or "\t"
    spec = f"Change: new\nDescription:\n{desc_block}\n"
    try:
        r = subprocess.run(
            ["p4", "change", "-i"],
            input=spec,
            capture_output=True,
            text=True,
            creationflags=_CF,
        )
        if r.returncode == 0:
            for word in r.stdout.split():
                if word.isdigit():
                    _cached_cl = int(word)
                    _cached_cl_desc = description
                    return _cached_cl
    except Exception:
        pass
    return None


def checkout_file(filepath: str, description: str = "") -> tuple[bool, str]:
    """Checkout a file in P4. Returns (success, message)."""
    if not _check_p4():
        return False, "p4 not in PATH"
    if not description:
        description = _DEFAULT_CL_DESC
    cl = _get_or_create_changelist(description)
    if cl is None:
        _, err, rc = _run_p4(["edit", filepath])
        if rc != 0:
            return False, err
        return True, "Checked out (default CL)"
    _, err, rc = _run_p4(["edit", "-c", str(cl), filepath])
    if rc != 0:
        return False, err
    return True, f"Checked out in CL {cl}"


def find_file_changelist(filepath: str) -> int | None:
    """Return the CL number a file is currently opened in, or None."""
    if not _check_p4():
        return None
    out, _, rc = _run_p4(["opened", filepath])
    if rc != 0 or not out:
        return None
    # Output: //depot/path#rev - edit change 12345 (text)
    #   or:  //depot/path#rev - edit default change (text)
    #   or:  <path> - file(s) not opened on this client.   <- rc 0, not opened
    #
    # v1.20: the old scan called out.split().index(part), which finds the FIRST
    # occurrence of that token rather than the one being examined -- so a rev
    # number that repeated later in the line could be read as the changelist.
    # Walk the tokens once and look at the actual neighbour.
    tokens = out.split()
    for i, tok in enumerate(tokens):
        nxt = tokens[i + 1] if i + 1 < len(tokens) else ""
        if tok == "default" and nxt == "change":
            return 0  # sentinel: file is in the default changelist
        if tok == "change" and nxt.isdigit():
            return int(nxt)
    return None


def add_file(filepath: str, cl: int | None = None) -> tuple[bool, str]:
    """p4 add a file. If cl is given, add to that CL."""
    if not _check_p4():
        return False, "p4 not in PATH"
    args = ["add"]
    if cl is not None and cl > 0:
        args += ["-c", str(cl)]
    args.append(filepath)
    _, err, rc = _run_p4(args)
    if rc != 0:
        return False, err
    return True, f"Added to CL {cl}" if cl else "Added (default CL)"


def checkout_or_add(filepath: str, cl: int | None = None,
                    description: str = "") -> tuple[bool, str]:
    """Edit if in depot, else add. Uses specific CL if provided,
    otherwise creates/finds one from description."""
    if not _check_p4():
        return False, "p4 not in PATH"

    # Resolve CL
    if cl is None and description:
        cl = _get_or_create_changelist(description)

    # Try add first (most common for sidecar files)
    args_add = ["add"]
    if cl is not None and cl > 0:
        args_add += ["-c", str(cl)]
    args_add.append(filepath)
    out, err, rc = _run_p4(args_add)
    if rc == 0 and "opened for add" in out:
        return True, f"Added in CL {cl}" if cl else "Added"

    # Already in depot — edit
    args_edit = ["edit"]
    if cl is not None and cl > 0:
        args_edit += ["-c", str(cl)]
    args_edit.append(filepath)
    out, err, rc = _run_p4(args_edit)
    if rc == 0 and "opened for edit" in out:
        return True, f"Checked out in CL {cl}" if cl else "Checked out"

    # Carry what p4 actually said. "failed silently" was true of the message,
    # not of p4 -- it prints its reason on stdout for the ordinary cases
    # (not under client root, already opened elsewhere, exclusive lock) and
    # swallowing that leaves nothing to act on.
    detail = " | ".join(part.strip() for part in (out, err) if part.strip())
    return False, detail or "p4 add and edit both produced no output"


def _selected_library_paths(context) -> list[str]:
    """Source .blend of every selected linked collection instance, deduped."""
    paths = []
    try:
        selected = context.selected_objects
    except AttributeError:
        return paths
    for obj in selected:
        if (obj.instance_type == 'COLLECTION'
                and obj.instance_collection
                and obj.instance_collection.library):
            p = bpy.path.abspath(obj.instance_collection.library.filepath)
            if p and p not in paths:
                paths.append(p)
    return paths


def resolve_targets(context) -> list[str]:
    """What a Perforce action should act on, inferred from the selection.

    Nothing selected means the file you are looking at; a selection means the
    assets you picked. That is what you would have set the old Target toggle to
    anyway, so the toggle was asking a question the selection already answered.
    """
    paths = _selected_library_paths(context)
    if paths:
        return paths
    fp = bpy.data.filepath
    return [fp] if fp else []


def _resolve_target(context) -> str | None:
    """First inferred target. Kept for callers that act on one file."""
    targets = resolve_targets(context)
    return targets[0] if targets else None


# ---------------------------------------------------------------------------
# Properties
# ---------------------------------------------------------------------------

class MAPLINK_PG_P4Props(PropertyGroup):
    target: EnumProperty(
        name="Target",
        items=[
            ('ACTIVE', "Active File", "Current .blend file"),
            ('SELECTED', "Selected Asset", "Linked collection from selected instance"),
        ],
        default='ACTIVE',
    )  # type: ignore


def get_cl_description(props=None) -> str:
    """Return the CL description.  Fixed value — the description is no
    longer user-editable.  *props* is accepted for backwards-compat with
    callers that still pass it."""
    return _DEFAULT_CL_DESC


def wrap_cl_for_display(context, text: str) -> list[str]:
    """Wrap *text* across multiple visual rows so the full contents can
    be read inside a narrow sidebar panel.  Wrap width is estimated from
    the live region width so the preview adapts when the sidebar is
    resized."""
    import textwrap

    if not text:
        return ["(empty)"]

    # Default Blender UI font averages ~6.2 px per glyph at common DPI.
    char_px = 6.2
    try:
        region_w = context.region.width
    except (AttributeError, TypeError):
        region_w = 220
    # Subtract box padding + a comfort margin so lines never spill.
    width_chars = max(16, int((region_w - 45) / char_px))

    lines: list[str] = []
    for paragraph in text.splitlines() or [""]:
        wrapped = textwrap.wrap(paragraph, width=width_chars) or [""]
        lines.extend(wrapped)
    return lines


# ---------------------------------------------------------------------------
# Core P4 operators
# ---------------------------------------------------------------------------

class MAPLINK_OT_P4ShowInExplorer(Operator):
    bl_idname = "maplink.p4_show_in_explorer"
    bl_label = "Show In Explorer"
    bl_description = "Open file location in Explorer"
    bl_options = {'INTERNAL'}

    @classmethod
    def poll(cls, context):
        return bpy.data.filepath != "" and _resolve_target(context) is not None

    def execute(self, context):
        fp = _resolve_target(context)
        if not fp or not os.path.exists(fp):
            self.report({'ERROR'}, f"File not found: {fp}")
            return {'CANCELLED'}
        subprocess.Popen(
            ["explorer", "/select,", os.path.normpath(fp)],
            creationflags=_CF,
        )
        return {'FINISHED'}


class MAPLINK_OT_P4Checkout(Operator):
    bl_idname = "maplink.p4_checkout"
    bl_label = "Checkout"
    bl_description = "p4 edit the target file"
    bl_options = {'INTERNAL'}

    @classmethod
    def poll(cls, context):
        return (_check_p4()
                and bpy.data.filepath != ""
                and _resolve_target(context) is not None)

    def execute(self, context):
        fp = _resolve_target(context)
        if not fp:
            self.report({'ERROR'}, "No target file")
            return {'CANCELLED'}
        props = context.scene.maplink_p4
        ok, msg = checkout_file(fp, get_cl_description(props))
        self.report({'INFO'} if ok else {'ERROR'}, msg)
        return {'FINISHED'} if ok else {'CANCELLED'}


class MAPLINK_OT_P4Revert(Operator):
    bl_idname = "maplink.p4_revert"
    bl_label = "Revert"
    bl_description = "p4 revert the target file"
    bl_options = {'INTERNAL'}

    def invoke(self, context, event):
        return context.window_manager.invoke_confirm(self, event)

    @classmethod
    def poll(cls, context):
        return (_check_p4()
                and bpy.data.filepath != ""
                and _resolve_target(context) is not None)

    def execute(self, context):
        fp = _resolve_target(context)
        if not fp:
            self.report({'ERROR'}, "No target file")
            return {'CANCELLED'}
        _, err, rc = _run_p4(["revert", fp])
        if rc != 0:
            self.report({'ERROR'}, f"Revert failed: {err}")
            return {'CANCELLED'}
        self.report({'INFO'}, f"Reverted: {os.path.basename(fp)}")
        if os.path.normpath(fp) == os.path.normpath(bpy.data.filepath):
            from . import nav_drill
            nav_drill.reset_drill_stack()
            bpy.ops.wm.open_mainfile(filepath=bpy.data.filepath)
        return {'FINISHED'}


# ---------------------------------------------------------------------------
# Ctrl+S override — prompt for P4 checkout on save when file isn't open
# ---------------------------------------------------------------------------

class MAPLINK_OT_SaveWithP4Prompt(Operator):
    """MapLink's Ctrl+S.  Three behaviours, each with its own scope.

    * Never saved -> `wm.save_mainfile`'s own invoke (the Save-As dialog).
    * Inside a MapLink project -> the foreign-link prompt, Auto-Standardize /
      Auto-colour, and the Perforce prompt.
    * Outside one -> Blender's Ctrl+S, and nothing else. v1.20; see invoke().

    The Perforce prompt is scoped a second time, by `file_p4_state()`: it
    appears only for a file this client can actually check out. Having p4 on
    PATH is not a reason to interrupt a save.
    """

    bl_idname = "maplink.save_with_p4_prompt"
    bl_label = "Save"
    bl_description = "Save file — prompt for P4 checkout when needed"
    bl_options = {'INTERNAL'}

    # v1.19 — this operator already owns Ctrl+S, so it is where the foreign-
    # link prompt belongs.  A save_pre handler cannot open a dialog, and a
    # second interception mechanism competing for the same key is exactly the
    # kind of thing that ends up firing twice or not at all.
    link_action: bpy.props.EnumProperty(
        name="Links outside this project",
        items=[
            ('COPY', "Copy into project and save",
             "Vendor the foreign assets in, repoint this file at the copies, "
             "then save. The sidecar is written and UE can import it"),
            ('ANYWAY', "Save anyway (no sidecar will be written)",
             "Save the .blend as it is. UE cannot import it until the links "
             "resolve inside this project"),
        ],
        default='COPY',
    )

    _foreign = ()
    _root = None
    _state = None

    def _scan_links(self):
        # blocking_entries(), NOT a raw status filter. The gate that decides
        # whether a sidecar is written is scoped by ownership and directness
        # (see link_scope.blocking_entries); this prompt must ask exactly the
        # same question or it prompts about links the save does not care
        # about. Blender's bundled `geometry_nodes_essentials` is the case
        # that proved it: Foreign forever, by design, on 25 of 185 assets --
        # and `Copy into project` for it resolves to an empty vendor plan,
        # which CANCELS, which aborts the save. A cosmetic-looking warning
        # that made the file unsaveable.
        from . import link_scope
        try:
            self._foreign = tuple(link_scope.blocking_entries())
        except Exception:                              # noqa: BLE001
            self._foreign = ()
        return self._foreign

    def _resolve(self):
        """Everything this operator needs to know about the open file.

        Split out because EXEC_DEFAULT callers reach execute() without ever
        running invoke(), and a save that skipped the scan must not then act on
        a stale one from a previous file.
        """
        from . import link_scope
        fp = bpy.data.filepath
        self._root = link_scope.current_project_root() if fp else None
        self._foreign = ()
        self._state = file_p4_state(fp) if fp else NOT_IN_WORKSPACE

    def invoke(self, context, event):
        fp = bpy.data.filepath
        if not fp:
            # Never-saved file → fall back to Save-As dialog.
            return bpy.ops.wm.save_mainfile('INVOKE_DEFAULT')

        self._resolve()

        # v1.20 -- MapLink owns Ctrl+S, so MapLink is responsible for getting
        # out of the way.  Outside a MapLink project this operator has nothing
        # to say and three ways to be wrong about it:
        #
        #   * link_scope.classify_library() returns Foreign for EVERYTHING when
        #     there is no project root (its own docstring says so), so a
        #     stranger's .blend linking any SM_*.blend was offered "Copy into
        #     project and save" -- into a project that does not exist.
        #   * _pre_save_conformation() rewrites vertex colours and UV names.
        #     The artist consented to that for their MapLink assets.
        #   * the Perforce prompt, which is the reported bug.
        #
        # So the whole operator steps aside, and Ctrl+S is Blender's Ctrl+S.
        # The preference re-enables the Perforce branch alone, never the other
        # two -- they are about a project, not about a depot.
        if self._root is None and not _p4_outside_projects():
            return self._do_save(context, conform=False)

        foreign = self._scan_links() if self._root is not None else ()
        needs_p4 = self._state == NEEDS_CHECKOUT

        if not foreign and not needs_p4:
            return self._do_save(context, conform=self._root is not None)

        return context.window_manager.invoke_props_dialog(self, width=480)

    def draw(self, context):
        layout = self.layout
        fp = bpy.data.filepath
        if fp:
            layout.label(text=os.path.basename(fp), icon='FILE_BLEND')

        if self._foreign:
            from . import link_scope
            # Count the deduped lines, not the raw entries — the number in
            # the sentence has to match what is listed under it.
            lines = link_scope.describe(self._foreign)
            box = layout.box()
            box.label(
                text=f"This file links {len(lines)} asset(s) from "
                     f"outside this project:", icon='ERROR')
            for line in lines[:6]:
                box.label(text=f"    {line}")
            if len(lines) > 6:
                box.label(text=f"    …and {len(lines) - 6} more")
            box.label(text="They will not resolve for anyone else.")
            box.prop(self, "link_action", expand=True)

        if self._state == NEEDS_CHECKOUT:
            box = layout.box()
            box.label(text="This file is not checked out in Perforce.",
                      icon='ERROR')
            box.label(text="It will be checked out before saving.")

        layout.label(text="(Cancel aborts the save entirely.)")

    def execute(self, context):
        fp = bpy.data.filepath
        if self._state is None:                    # EXEC_DEFAULT, no invoke()
            self._resolve()

        # Vendor first: the copy has to land, and this file has to be pointing
        # at it, BEFORE the bytes are written — otherwise the save produces
        # the same foreign links it was prompting about.
        if self._foreign and self.link_action == 'COPY':
            try:
                result = bpy.ops.maplink.vendor_asset('EXEC_DEFAULT')
            except Exception as exc:                   # noqa: BLE001
                self.report({'ERROR'}, f"Copy failed, nothing saved: {exc}")
                return {'CANCELLED'}
            if 'FINISHED' not in result:
                self.report({'ERROR'}, "Copy did not complete; nothing saved.")
                return {'CANCELLED'}

        if fp and self._state == NEEDS_CHECKOUT:
            props = context.scene.maplink_p4
            ok, msg = checkout_file(fp, get_cl_description(props))
            if ok:
                self.report({'INFO'}, msg)
            else:
                self.report({'WARNING'}, f"Checkout failed: {msg}")
        elif fp and self._state == NOT_IN_DEPOT and self._root is not None:
            # In the workspace, in a MapLink project, never added.  `p4 edit`
            # can only fail on it; adding is what was meant, and it is the same
            # thing sidecar_analysis already does for the files it writes.
            ok, msg = checkout_or_add(fp, description=get_cl_description())
            if ok:
                self.report({'INFO'}, msg)
            else:
                self.report({'WARNING'}, f"p4 add failed: {msg}")

        return self._do_save(context, conform=self._root is not None)

    def _pre_save_conformation(self, context):
        """Auto-Standardize / Auto-colour, run just before the write.

        This is the only place it can live. A save_pre handler cannot open a
        confirmation dialog, and MapLink already owns Ctrl+S through this
        operator, so the work happens here where the user has a UI and the
        existing warning behaviour still applies.

        Everything is best effort: a failure here must never cost the save.
        """
        try:
            prefs = context.preferences.addons[
                __package__.split('.')[0]].preferences
        except (KeyError, AttributeError):
            return

        if getattr(prefs, "auto_standardize", False):
            try:
                from . import modeling_tools
                before = modeling_tools.survey_conformance(context.scene)
                if before["vc"] or before["uv"]:
                    # EXEC_DEFAULT deliberately: the confirmation these
                    # operators show on invoke is for an explicit, one-off
                    # action. Auto-Standardize IS the consent, and prompting on
                    # every Ctrl+S would train the user to click through it.
                    if before["vc"]:
                        bpy.ops.maplink.standardize_vc('EXEC_DEFAULT')
                    if before["uv"]:
                        bpy.ops.maplink.standardize_uv('EXEC_DEFAULT')
                    print(f"[MapLink] Auto-Standardize: {before['vc']} VC, "
                          f"{before['uv']} UV mesh(es) brought to standard.")
            except Exception as exc:
                print(f"[MapLink] Auto-Standardize skipped: {exc}")

        if getattr(prefs, "auto_color_objects", False):
            try:
                from . import outliner_cleanup
                outliner_cleanup.color_objects_by_collection(context.scene)
            except Exception as exc:
                print(f"[MapLink] Auto-colour skipped: {exc}")

    def _do_save(self, context, conform=True):
        # conform=False is the standalone path: no MapLink project owns this
        # file, so Auto-Standardize and Auto-colour have no consent to act on.
        if conform:
            self._pre_save_conformation(context)
        try:
            bpy.ops.wm.save_mainfile()
        except RuntimeError as exc:
            self.report({'ERROR'}, f"Save failed: {exc}")
            return {'CANCELLED'}
        return {'FINISHED'}


# ---------------------------------------------------------------------------
# Outliner context-menu operators
# ---------------------------------------------------------------------------

def _get_outliner_blend_path(context) -> str | None:
    try:
        for item in context.selected_ids:
            if hasattr(item, 'library') and item.library:
                return bpy.path.abspath(item.library.filepath)
    except Exception:
        pass
    return None


class MAPLINK_OT_OutlinerShowInExplorer(Operator):
    bl_idname = "maplink.outliner_show_in_explorer"
    bl_label = "Show In Explorer"
    bl_options = {'INTERNAL'}

    @classmethod
    def poll(cls, context):
        return _get_outliner_blend_path(context) is not None

    def execute(self, context):
        fp = _get_outliner_blend_path(context)
        if not fp or not os.path.exists(fp):
            self.report({'ERROR'}, f"File not found: {fp}")
            return {'CANCELLED'}
        subprocess.Popen(
            ["explorer", "/select,", os.path.normpath(fp)],
            creationflags=_CF,
        )
        return {'FINISHED'}


class MAPLINK_OT_OutlinerCheckout(Operator):
    bl_idname = "maplink.outliner_checkout"
    bl_label = "Checkout"
    bl_options = {'INTERNAL'}

    @classmethod
    def poll(cls, context):
        return _check_p4() and _get_outliner_blend_path(context) is not None

    def execute(self, context):
        fp = _get_outliner_blend_path(context)
        if not fp:
            self.report({'ERROR'}, "No target file")
            return {'CANCELLED'}
        ok, msg = checkout_file(fp)
        self.report({'INFO'} if ok else {'ERROR'}, msg)
        return {'FINISHED'} if ok else {'CANCELLED'}


def _outliner_collection_menu_draw(self, context):
    self.layout.separator()
    self.layout.operator("maplink.outliner_show_in_explorer", icon='FILE_FOLDER')
    self.layout.operator("maplink.outliner_checkout", icon='IMPORT')


# ---------------------------------------------------------------------------
# Keymap
# ---------------------------------------------------------------------------

_addon_keymaps: list = []


def _register_keymaps():
    wm = bpy.context.window_manager
    if wm.keyconfigs.addon is None:
        return
    # The 'Window' keymap is where the default Ctrl+S binding for
    # wm.save_mainfile lives; an addon-scoped entry with the same
    # shortcut takes priority.
    km = wm.keyconfigs.addon.keymaps.new(name='Window', space_type='EMPTY')
    kmi = km.keymap_items.new("maplink.save_with_p4_prompt",
                              'S', 'PRESS', ctrl=True)
    _addon_keymaps.append((km, kmi))


def _unregister_keymaps():
    for km, kmi in _addon_keymaps:
        try:
            km.keymap_items.remove(kmi)
        except Exception:
            pass
    _addon_keymaps.clear()


# ---------------------------------------------------------------------------
# Registration
# ---------------------------------------------------------------------------

_classes = (
    MAPLINK_PG_P4Props,
    MAPLINK_OT_P4ShowInExplorer,
    MAPLINK_OT_P4Checkout,
    MAPLINK_OT_P4Revert,
    MAPLINK_OT_SaveWithP4Prompt,
    MAPLINK_OT_OutlinerShowInExplorer,
    MAPLINK_OT_OutlinerCheckout,
)


def register():
    for cls in _classes:
        bpy.utils.register_class(cls)
    bpy.types.Scene.maplink_p4 = bpy.props.PointerProperty(type=MAPLINK_PG_P4Props)
    ui_menus.append_once(bpy.types.OUTLINER_MT_collection,
                         _outliner_collection_menu_draw)

    _register_keymaps()


def unregister():
    _unregister_keymaps()

    ui_menus.detach(bpy.types.OUTLINER_MT_collection,
                    _outliner_collection_menu_draw)
    del bpy.types.Scene.maplink_p4
    for cls in reversed(_classes):
        bpy.utils.unregister_class(cls)
