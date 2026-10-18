"""MapLink_Blender — Modeling additions (v1.4.6, standardization v1.14).

Clean-room tools implemented purely against bpy / bmesh:

  • maplink.standardize_vc  — normalise every visible local mesh to the
        single-channel "Color" standard (BYTE_COLOR / Face Corner, active +
        active-render, black-opaque default).  Confirm dialog before any
        destructive work.  (v1.14: renamed from "Conform Vertex Colors"; the
        old id maplink.conform_vertex_colors remains as a deprecated alias so
        saved keymaps still resolve.)
  • maplink.standardize_uv  — rename every visible local mesh's UV layers to
        the canonical uv0 / uv1 / uv2 scheme in index order (extras → uv3,
        uv4, …).  Idempotent; zero-UV meshes are skipped; active/render UV
        selections are preserved; material UV Map references are retargeted.
  • Vertex-Color toolset (all gated on the active mesh being standard; all
        touch ONLY the "Color" attribute; per-channel read-modify-write that
        preserves the other channels, except VC-from-Base-Color which writes
        full RGB):
            maplink.vc_from_cavity      (native Dirty Vertex Colors, masked
                                         into the selected channel)
            maplink.vc_contrast         (remap about 0.5)
            maplink.vc_set_selected     (set selected verts / whole mesh)
            maplink.vc_set_outer        (open-boundary verts)
            maplink.vc_set_inner        (non-boundary verts)
            maplink.vc_from_base_color  (sample active material Base-Color
                                         image through the active UV map)
  • maplink.patch_unwrap  — Reset → Follow Active Quads (from the active quad)
        → texel-density normalise → per-axis de-stretch, on the selected quad
        faces in Edit mode.

UI
  • MAPLINK_PT_Modeling — single "Modeling" panel hosting the whole toolset.
        Layout: a shared channel selector at the top; a "Set" box with the
        value field plus one row [Selected / Outer / Inner]; a row
        [Contrast / Cavity] whose parameters appear as a popup on use (not as
        permanent tab fields); the VC-from-Base-Color button; and
        Patch Unwrap (also popup-on-use) below a separator.
  • The current-file standardization actions (Standardize VC, Standardize UV,
        Outliner Cleanup, Cleanup Collections) live under the Tools panel
        (quick_tools.MAPLINK_PT_Tools), not here.  Batch equivalents live
        under Settings ▸ Batch (batch_standardize.py).

Every operator lives in the ``maplink.*`` namespace, is labelled
"MapLink: ...", carries {'REGISTER','UNDO'}, runs entirely inside execute()
(no timers / modal / deferred work), and wraps its body in try/except so a
failure reports an error and returns {'CANCELLED'}.
"""

import math
import bpy
import bmesh
from bpy.types import Operator, Panel, PropertyGroup
from bpy.props import (
    EnumProperty,
    FloatProperty,
    IntProperty,
    BoolProperty,
)


# ---------------------------------------------------------------------------
# v1.14 — shared UI helpers (confirmation gate + end-of-action popup)
# ---------------------------------------------------------------------------

def _prefs():
    try:
        return bpy.context.preferences.addons[
            __package__.split('.')[0]].preferences
    except Exception:
        return None


def confirm_destructive_enabled():
    """Preferences > 'Warn before destructive standardization' (default ON)."""
    p = _prefs()
    return bool(getattr(p, 'confirm_destructive', True)) if p else True


def popup_report(title, lines, icon='INFO'):
    """End-of-action report popup.  Never raises; no-op headless."""
    if bpy.app.background:
        return

    def _draw(self, _context):
        col = self.layout.column(align=True)
        for ln in lines:
            col.label(text=ln)

    try:
        bpy.context.window_manager.popup_menu(_draw, title=title, icon=icon)
    except Exception:
        pass


# ---------------------------------------------------------------------------
# VC standard + small helpers
# ---------------------------------------------------------------------------

STANDARD_NAME = "Color"
STANDARD_TYPE = 'BYTE_COLOR'
STANDARD_DOMAIN = 'CORNER'
BLACK = (0.0, 0.0, 0.0, 1.0)
WHITE = (1.0, 1.0, 1.0, 1.0)

CHANNEL_INDEX = {'R': 0, 'G': 1, 'B': 2, 'A': 3}
_CHANNEL_ITEMS = [
    ('R', "R", "Red channel"),
    ('G', "G", "Green channel"),
    ('B', "B", "Blue channel"),
    ('A', "A", "Alpha channel"),
]


def _get_color_attr(mesh, name):
    """Return the color attribute matching *name* (by name, not identity)."""
    for a in mesh.color_attributes:
        if a.name == name:
            return a
    return None


def _is_standard(mesh):
    """A mesh is standard iff it has exactly one color attribute named "Color",
    BYTE_COLOR / Face-Corner, set as both active and active-render."""
    cas = mesh.color_attributes
    if len(cas) != 1:
        return False
    a = cas[0]
    if (a.name != STANDARD_NAME
            or a.data_type != STANDARD_TYPE
            or a.domain != STANDARD_DOMAIN):
        return False
    ac = cas.active_color
    if ac is None or ac.name != STANDARD_NAME:
        return False
    try:
        if cas.render_color_index != 0:
            return False
    except Exception:
        pass
    return True


def survey_conformance(scene):
    """Non-destructive: what would Standardize change in this file?

    Answers the only question worth asking before a save -- is this file
    already conventional -- without touching anything. The UV side reuses the
    same naming rule the standardizer applies, so the check can never disagree
    with what the operator would then do.

    -> dict(meshes, vc, uv, names) where vc/uv are counts of meshes that would
    change and names is a short sample for the report.
    """
    vc = uv = total = 0
    names = []
    seen = set()
    for obj in getattr(scene, "objects", []):
        if obj.type != 'MESH' or obj.library is not None:
            continue
        mesh = obj.data
        if mesh is None or mesh.library is not None or mesh.name in seen:
            continue
        seen.add(mesh.name)
        total += 1

        needs_vc = not _is_standard(mesh)
        needs_uv = any(l.name != _uv_std_name(i)
                       for i, l in enumerate(mesh.uv_layers))
        if needs_vc:
            vc += 1
        if needs_uv:
            uv += 1
        if (needs_vc or needs_uv) and len(names) < 8:
            flags = "/".join(f for f, on in (("VC", needs_vc), ("UV", needs_uv)) if on)
            names.append(f"{obj.name} ({flags})")

    return {"meshes": total, "vc": vc, "uv": uv, "names": names}


def is_conform(scene):
    r = survey_conformance(scene)
    return r["vc"] == 0 and r["uv"] == 0


class MAPLINK_OT_CheckStandardization(Operator):
    bl_idname = "maplink.check_standardization"
    bl_label = "Check Standardization"
    bl_description = ("Report whether this file is conventional -- canonical "
                      "vertex-colour channel and uv0/uv1/uv2 layer names -- "
                      "without changing anything")
    bl_options = {'REGISTER'}

    def execute(self, context):
        r = survey_conformance(context.scene)
        if r["meshes"] == 0:
            self.report({'INFO'}, "No local meshes in this file.")
            return {'FINISHED'}

        if r["vc"] == 0 and r["uv"] == 0:
            self.report({'INFO'},
                        f"Conform — all {r['meshes']} mesh(es) standard.")
            return {'FINISHED'}

        for n in r["names"]:
            print(f"[MapLink Conform] needs standardizing: {n}")
        self.report({'WARNING'},
                    f"Needs standardizing: {r['vc']} VC, {r['uv']} UV "
                    f"of {r['meshes']} mesh(es). See console for names.")
        return {'FINISHED'}


def _set_active_render(mesh, name):
    """Make the color attribute *name* both the active and active-render one."""
    cas = mesh.color_attributes
    idx = None
    for i, a in enumerate(cas):
        if a.name == name:
            idx = i
            break
    if idx is None:
        return
    try:
        cas.active_color = cas[idx]
    except Exception:
        try:
            cas.active_color_index = idx
        except Exception:
            pass
    try:
        cas.render_color_index = idx
    except Exception:
        pass


def _fill(attr, rgba):
    n = len(attr.data)
    if n:
        attr.data.foreach_set('color', tuple(rgba) * n)


def _unique_attr_name(mesh, base):
    names = {a.name for a in mesh.attributes}
    if base not in names:
        return base
    i = 1
    while f"{base}.{i:03d}" in names:
        i += 1
    return f"{base}.{i:03d}"


def _coerce(mesh, attr_name):
    """Ensure the color attribute *attr_name* is BYTE_COLOR / Face-Corner.

    If it already is, returns its (unchanged) name.  Otherwise recreates it
    with the correct type/domain — mapping POINT→CORNER via loop vertex
    indices, CORNER→CORNER directly — preserving the name.  All access is by
    name to avoid stale RNA references across the remove() call.
    """
    attr = _get_color_attr(mesh, attr_name)
    if attr is None:
        return None
    if attr.data_type == STANDARD_TYPE and attr.domain == STANDARD_DOMAIN:
        return attr_name

    src_domain = attr.domain
    n = len(attr.data)
    src = [0.0] * (n * 4)
    attr.data.foreach_get('color', src)

    tmp = _unique_attr_name(mesh, "__maplink_coerce__")
    new = mesh.color_attributes.new(name=tmp, type=STANDARD_TYPE,
                                    domain=STANDARD_DOMAIN)
    nn = len(new.data)
    if src_domain == STANDARD_DOMAIN and n == nn:
        new.data.foreach_set('color', src)
    elif src_domain == 'POINT':
        buf = [0.0] * (nn * 4)
        for li, loop in enumerate(mesh.loops):
            o = li * 4
            s = loop.vertex_index * 4
            buf[o] = src[s]
            buf[o + 1] = src[s + 1]
            buf[o + 2] = src[s + 2]
            buf[o + 3] = src[s + 3]
        new.data.foreach_set('color', buf)
    else:
        _fill(new, BLACK)

    old = _get_color_attr(mesh, attr_name)
    if old is not None:
        mesh.color_attributes.remove(old)
    new2 = _get_color_attr(mesh, tmp)
    if new2 is not None:
        new2.name = attr_name
    return attr_name


def _pick_primary_name(mesh):
    """Choose the attribute that becomes "Color": a pre-existing "Color" wins,
    else the active color attribute, else the first."""
    cas = mesh.color_attributes
    for a in cas:
        if a.name == STANDARD_NAME:
            return a.name
    ac = cas.active_color
    if ac is not None:
        return ac.name
    if len(cas):
        return cas[0].name
    return None


def _classify(mesh):
    """Read-only classification: ('already'|'create'|'standardize', n_extra)."""
    if _is_standard(mesh):
        return ('already', 0)
    cas = mesh.color_attributes
    if len(cas) == 0:
        return ('create', 0)
    return ('standardize', len(cas) - 1)


def _standardize_vc_mesh(mesh):
    """Apply the VC standard.  Returns ('already'|'create'|'standardize',
    n_extra_deleted)."""
    if _is_standard(mesh):
        return ('already', 0)

    cas = mesh.color_attributes
    if len(cas) == 0:
        new = cas.new(name=STANDARD_NAME, type=STANDARD_TYPE,
                      domain=STANDARD_DOMAIN)
        _fill(new, BLACK)
        _set_active_render(mesh, STANDARD_NAME)
        return ('create', 0)

    primary = _pick_primary_name(mesh)
    extra_names = [a.name for a in cas if a.name != primary]
    n_extra = len(extra_names)
    for nm in extra_names:
        a = _get_color_attr(mesh, nm)
        if a is not None:
            mesh.color_attributes.remove(a)

    primary = _coerce(mesh, primary)
    if primary != STANDARD_NAME:
        a = _get_color_attr(mesh, primary)
        if a is not None:
            a.name = STANDARD_NAME
            primary = STANDARD_NAME

    _set_active_render(mesh, STANDARD_NAME)
    return ('standardize', n_extra)


def _gather(context):
    """Unique local visible mesh datablocks + count of linked objects skipped."""
    meshes = []
    seen = set()
    n_skipped = 0
    view_layer = context.view_layer
    for obj in view_layer.objects:
        if obj.type != 'MESH':
            continue
        try:
            if not obj.visible_get():
                continue
        except Exception:
            continue
        me = obj.data
        if me is None:
            continue
        if (obj.library is not None
                or me.library is not None
                or any(c.library is not None for c in obj.users_collection)):
            n_skipped += 1
            continue
        if me.name in seen:
            continue
        seen.add(me.name)
        meshes.append(me)
    return meshes, n_skipped


def standardize_scene_vc(scene):
    """Standardize vertex colors on every local mesh used in *scene*.
    Headless-safe (no context).

    Returns dict(create, standardize, already, extra, extra_meshes, total).
    """
    meshes = []
    seen = set()
    for obj in scene.objects:
        if obj.type != 'MESH':
            continue
        me = obj.data
        if me is None or me.library is not None:
            continue
        if me.name in seen:
            continue
        seen.add(me.name)
        meshes.append(me)

    n_create = n_std = n_already = 0
    n_extra = n_extra_meshes = 0
    for me in meshes:
        action, ne = _standardize_vc_mesh(me)
        if action == 'create':
            n_create += 1
        elif action == 'standardize':
            n_std += 1
            if ne:
                n_extra += ne
                n_extra_meshes += 1
        else:
            n_already += 1
    return dict(create=n_create, standardize=n_std, already=n_already,
                extra=n_extra, extra_meshes=n_extra_meshes, total=len(meshes))


# ---------------------------------------------------------------------------
# UV standardization (v1.14) — canonical uv0 / uv1 / uv2 … layer names
# ---------------------------------------------------------------------------

def _uv_std_name(i):
    return f"uv{i}"


def _classify_uv(mesh):
    """Read-only: ('none'|'already'|'rename', n_layers_to_rename)."""
    uvs = mesh.uv_layers
    if len(uvs) == 0:
        return ('none', 0)
    n = sum(1 for i, l in enumerate(uvs) if l.name != _uv_std_name(i))
    return ('already', 0) if n == 0 else ('rename', n)


def _standardize_uv_mesh(mesh):
    """Rename UV layers to uv0/uv1/uv2… in index order (extra layers become
    uv3, uv4, …).  Idempotent — a second run is a no-op.  Never reorders
    layers, so the active / active-render / active-clone selections (all
    index-based) are preserved.  Meshes with zero UV layers are skipped.

    Returns ('none'|'already'|'renamed', {old_name: new_name}); the rename
    map is used to retarget UV-map references in the mesh's materials."""
    uvs = mesh.uv_layers
    if len(uvs) == 0:
        return ('none', {})
    rename = {l.name: _uv_std_name(i)
              for i, l in enumerate(uvs) if l.name != _uv_std_name(i)}
    if not rename:
        return ('already', {})

    # Two-phase rename so a swapped uv0/uv1 pair can't collide (a direct
    # clash would make Blender silently append ".001").
    taken = {l.name for l in uvs}
    for i, l in enumerate(uvs):
        if l.name != _uv_std_name(i):
            tmp = f"__mlk_uv_tmp_{i}"
            while tmp in taken:
                tmp += "_"
            taken.add(tmp)
            l.name = tmp
    for i, l in enumerate(uvs):
        want = _uv_std_name(i)
        if l.name != want:
            l.name = want
    return ('renamed', rename)


def _retarget_uv_nodes(mesh, rename):
    """Blender material nodes reference UV layers BY NAME (UV Map / Normal
    Map / any node with a `uv_map` property), so a rename would silently
    break them.  Point every reference in the mesh's materials at the new
    names; recurses into node groups.  UE is unaffected either way (UV
    channels import by index).  Returns node references updated."""
    if not rename:
        return 0
    n = 0
    trees = [mat.node_tree for mat in mesh.materials
             if mat is not None and mat.use_nodes
             and mat.node_tree is not None]
    seen = set()
    while trees:
        tree = trees.pop()
        key = getattr(tree, 'name_full', None) or id(tree)
        if key in seen:
            continue
        seen.add(key)
        for node in tree.nodes:
            sub = getattr(node, 'node_tree', None)
            if sub is not None:
                trees.append(sub)
            uvm = getattr(node, 'uv_map', None)
            if uvm and uvm in rename:
                try:
                    node.uv_map = rename[uvm]
                    n += 1
                except Exception:
                    pass
    return n


def standardize_scene_uv(scene):
    """Standardize UV layer names on every local mesh used in *scene*.
    Headless-safe (no context).

    Returns dict(renamed, already, none, nodes, total)."""
    meshes = []
    seen = set()
    for obj in scene.objects:
        if obj.type != 'MESH':
            continue
        me = obj.data
        if me is None or me.library is not None:
            continue
        if me.name in seen:
            continue
        seen.add(me.name)
        meshes.append(me)

    n_renamed = n_already = n_none = n_nodes = 0
    for me in meshes:
        action, rename = _standardize_uv_mesh(me)
        if action == 'renamed':
            n_renamed += 1
            n_nodes += _retarget_uv_nodes(me, rename)
        elif action == 'already':
            n_already += 1
        else:
            n_none += 1
    return dict(renamed=n_renamed, already=n_already, none=n_none,
                nodes=n_nodes, total=len(meshes))


def _gate(op, context):
    """Return the active mesh object iff it exists and is standard, else warn."""
    obj = context.view_layer.objects.active
    if obj is None or obj.type != 'MESH' or obj.data is None:
        op.report({'WARNING'}, "No active mesh object.")
        return None
    if not _is_standard(obj.data):
        op.report({'WARNING'},
                  "Mesh not standard — run Standardize Vertex Colors first")
        return None
    return obj


# ---------------------------------------------------------------------------
# Per-corner color access, unified across Object / Edit mode
# ---------------------------------------------------------------------------

class _ColorAccess:
    """Read/write the "Color" attribute per face-corner, in either mode.

    Standardised on RAW byte space (``color_srgb`` in Object mode, the bmesh
    byte-color layer in Edit mode), so a 0.5 write lands on byte 128 in both
    modes and per-channel round-trips are exact.

    No ``bpy.ops`` may run between construction and ``flush()`` — that would
    invalidate the Edit-mode bmesh.
    """

    def __init__(self, obj):
        self.obj = obj
        self.mesh = obj.data
        self.edit = (obj.mode == 'EDIT')
        if self.edit:
            self.bm = bmesh.from_edit_mesh(self.mesh)
            self.col = self.bm.loops.layers.color.get(STANDARD_NAME)
            self.uv = self.bm.loops.layers.uv.active
            self._vsel = None
        else:
            self.bm = None
            self.col = _get_color_attr(self.mesh, STANDARD_NAME)
            self.uv = self.mesh.uv_layers.active
            nv = len(self.mesh.vertices)
            arr = [False] * nv
            self.mesh.vertices.foreach_get('select', arr)
            self._vsel = arr

    def corners(self):
        """Yield (key, vert_index, vert_selected) for every face-corner."""
        if self.edit:
            for f in self.bm.faces:
                for loop in f.loops:
                    v = loop.vert
                    yield loop, v.index, v.select
        else:
            vsel = self._vsel
            for loop in self.mesh.loops:
                vi = loop.vertex_index
                yield loop.index, vi, vsel[vi]

    def get_color(self, key):
        if self.edit:
            return list(key[self.col])
        return list(self.col.data[key].color_srgb)

    def set_color(self, key, rgba):
        if self.edit:
            key[self.col] = rgba
        else:
            self.col.data[key].color_srgb = rgba

    def get_uv(self, key):
        if self.edit:
            return key[self.uv].uv
        return self.uv.data[key].uv

    def boundary_vert_indices(self):
        """Vertices on an open boundary: on an edge used by exactly one face."""
        bset = set()
        if self.edit:
            for e in self.bm.edges:
                if len(e.link_faces) == 1:
                    bset.add(e.verts[0].index)
                    bset.add(e.verts[1].index)
        else:
            counts = {}
            for poly in self.mesh.polygons:
                for ek in poly.edge_keys:
                    counts[ek] = counts.get(ek, 0) + 1
            for ek, c in counts.items():
                if c == 1:
                    bset.add(ek[0])
                    bset.add(ek[1])
        return bset

    def flush(self):
        if self.edit:
            bmesh.update_edit_mesh(self.mesh)
        else:
            try:
                self.mesh.update_tag()
            except Exception:
                pass


def _write_channel(access, channel, predicate, fn):
    """Read-modify-write one channel where predicate(vert_index, vert_sel)."""
    idx = CHANNEL_INDEX[channel]
    n = 0
    for key, vi, vsel in access.corners():
        if not predicate(vi, vsel):
            continue
        c = access.get_color(key)
        v = fn(c[idx])
        if v < 0.0:
            v = 0.0
        elif v > 1.0:
            v = 1.0
        c[idx] = v
        access.set_color(key, c)
        n += 1
    access.flush()
    return n


def _sel_predicate(access):
    """Selection-respecting: Edit→selected verts, Object→whole mesh."""
    if access.edit:
        return lambda vi, vs: vs
    return lambda vi, vs: True


# ---------------------------------------------------------------------------
# Base-color sampling helpers
# ---------------------------------------------------------------------------

def _trace_image(node, visited):
    if node is None or node in visited:
        return None
    visited.add(node)
    if node.type == 'TEX_IMAGE' and node.image is not None:
        return node.image
    for inp in node.inputs:
        if inp.is_linked and inp.links:
            img = _trace_image(inp.links[0].from_node, visited)
            if img is not None:
                return img
    return None


def _find_base_color_image(mat):
    """Trace the Principled BSDF "Base Color" input to its image texture;
    fall back to the first image texture in the tree."""
    nt = getattr(mat, "node_tree", None)
    if nt is None:
        return None
    principled = None
    for node in nt.nodes:
        if node.type == 'BSDF_PRINCIPLED':
            principled = node
            break
    if principled is not None:
        inp = principled.inputs.get("Base Color")
        if inp is not None and inp.is_linked and inp.links:
            img = _trace_image(inp.links[0].from_node, set())
            if img is not None:
                return img
    for node in nt.nodes:
        if node.type == 'TEX_IMAGE' and node.image is not None:
            return node.image
    return None


def _sample_bilinear(px, w, h, u, v):
    """Bilinear sample of a flat RGBA pixel list (bottom-to-top), repeat wrap.
    Returns (r, g, b) in the image's stored (raw/sRGB) space."""
    u = u - math.floor(u)
    v = v - math.floor(v)
    x = u * w - 0.5
    y = v * h - 0.5
    x0 = math.floor(x)
    y0 = math.floor(y)
    fx = x - x0
    fy = y - y0

    def fetch(ix, iy):
        ix %= w
        iy %= h
        o = (iy * w + ix) * 4
        return px[o], px[o + 1], px[o + 2]

    c00 = fetch(x0, y0)
    c10 = fetch(x0 + 1, y0)
    c01 = fetch(x0, y0 + 1)
    c11 = fetch(x0 + 1, y0 + 1)
    r = (c00[0] * (1 - fx) + c10[0] * fx) * (1 - fy) + \
        (c01[0] * (1 - fx) + c11[0] * fx) * fy
    g = (c00[1] * (1 - fx) + c10[1] * fx) * (1 - fy) + \
        (c01[1] * (1 - fx) + c11[1] * fx) * fy
    b = (c00[2] * (1 - fx) + c10[2] * fx) * (1 - fy) + \
        (c01[2] * (1 - fx) + c11[2] * fx) * fy
    return r, g, b


# ---------------------------------------------------------------------------
# Operator — Standardize Vertex Colors (v1.14: renamed from "Conform")
# ---------------------------------------------------------------------------

class MAPLINK_OT_StandardizeVC(Operator):
    bl_idname = "maplink.standardize_vc"
    bl_label = "MapLink: Standardize Vertex Colors"
    bl_description = (
        "Normalise every visible local mesh to a single BYTE_COLOR / "
        "Face-Corner attribute named 'Color' (active + render, black default). "
        "Shows a confirmation with counts before any destructive change"
    )
    bl_options = {'REGISTER', 'UNDO'}

    def _scan_counts(self, classify_fn, meshes):
        n_create = n_std = n_already = 0
        n_extra = n_extra_meshes = 0
        for me in meshes:
            action, ne = classify_fn(me)
            if action == 'create':
                n_create += 1
            elif action == 'standardize':
                n_std += 1
                if ne:
                    n_extra += ne
                    n_extra_meshes += 1
            else:
                n_already += 1
        return dict(create=n_create, standardize=n_std, already=n_already,
                    extra=n_extra, extra_meshes=n_extra_meshes)

    def invoke(self, context, event):
        meshes, n_skipped = _gather(context)
        c = self._scan_counts(_classify, meshes)
        c['skipped'] = n_skipped
        c['total'] = len(meshes)
        self._scan = c
        # v1.14 — destructive-standardization warning is gated by the
        # 'Warn before destructive standardization' preference: OFF → run
        # immediately, ON → confirm (OK = pursue, Esc/Cancel = ignore).
        if not confirm_destructive_enabled():
            return self.execute(context)
        return context.window_manager.invoke_props_dialog(self, width=340)

    def draw(self, context):
        layout = self.layout
        s = getattr(self, '_scan', None)
        if not s:
            layout.label(text="Scanning...")
            return
        col = layout.column(align=True)
        col.label(text=f"{s['total']} mesh datablock(s):", icon='INFO')
        col.label(text=f"   {s['create']} will get a new Color")
        col.label(text=f"   {s['standardize']} will be standardized")
        col.label(text=f"   {s['extra']} extra channel(s) deleted "
                       f"across {s['extra_meshes']} mesh(es)")
        col.label(text=f"   {s['already']} already standard")
        col.label(text=f"   {s['skipped']} skipped (linked)")
        col.separator()
        col.label(text="WARNING: permanently alters color attributes.",
                  icon='ERROR')
        col.label(text="OK to pursue — Esc / Cancel to ignore.")

    def execute(self, context):
        try:
            return self._run(context)
        except Exception as exc:
            import traceback
            traceback.print_exc()
            self.report({'ERROR'}, f"MapLink Standardize VC failed: {exc}")
            return {'CANCELLED'}

    def _run(self, context):
        # Color-attribute edits are only reliable in Object mode.
        prev_mode = None
        active = context.view_layer.objects.active
        if active is not None and active.mode != 'OBJECT':
            prev_mode = active.mode
            bpy.ops.object.mode_set(mode='OBJECT')

        try:
            meshes, n_skipped = _gather(context)
            c = self._scan_counts(_standardize_vc_mesh, meshes)
        finally:
            if prev_mode is not None:
                try:
                    bpy.ops.object.mode_set(mode=prev_mode)
                except Exception:
                    pass

        msg = (f"MapLink Standardize VC: {c['create']} created, "
               f"{c['standardize']} renamed, {c['extra']} extra deleted across "
               f"{c['extra_meshes']} meshes, {c['already']} already standard, "
               f"{n_skipped} skipped (linked).")
        self.report({'INFO'}, msg)
        print(f"[MapLink] {msg}")
        # v1.14 — explicit end-of-action report.
        popup_report("Standardize VC — done", [
            f"{c['create']} Color attribute(s) created",
            f"{c['standardize']} standardized (renamed/coerced)",
            f"{c['extra']} extra channel(s) deleted "
            f"across {c['extra_meshes']} mesh(es)",
            f"{c['already']} already standard",
            f"{n_skipped} skipped (linked)",
        ], icon='CHECKMARK')
        return {'FINISHED'}


class MAPLINK_OT_ConformVertexColorsAlias(Operator):
    """DEPRECATED v1.14 — back-compat alias for saved keymaps / muscle memory.
    The old operator id 'maplink.conform_vertex_colors' resolves here and
    calls straight through to maplink.standardize_vc."""
    bl_idname = "maplink.conform_vertex_colors"
    bl_label = "MapLink: Standardize Vertex Colors (deprecated id)"
    bl_description = ("Deprecated alias of 'Standardize Vertex Colors' — "
                      "kept so old keymaps keep working")
    bl_options = {'REGISTER'}

    def invoke(self, context, event):
        return bpy.ops.maplink.standardize_vc('INVOKE_DEFAULT')

    def execute(self, context):
        return bpy.ops.maplink.standardize_vc('EXEC_DEFAULT')


# ---------------------------------------------------------------------------
# Operator — Standardize UV (v1.14)
# ---------------------------------------------------------------------------

class MAPLINK_OT_StandardizeUV(Operator):
    bl_idname = "maplink.standardize_uv"
    bl_label = "MapLink: Standardize UV"
    bl_description = (
        "Rename every visible local mesh's UV layers to the canonical "
        "uv0 / uv1 / uv2 scheme (index order; extras become uv3, uv4, …). "
        "Material UV Map references follow the rename. Idempotent; "
        "meshes without UVs are skipped"
    )
    bl_options = {'REGISTER', 'UNDO'}

    def invoke(self, context, event):
        meshes, n_skipped = _gather(context)
        n_rename = n_already = n_none = 0
        for me in meshes:
            action, _n = _classify_uv(me)
            if action == 'rename':
                n_rename += 1
            elif action == 'already':
                n_already += 1
            else:
                n_none += 1
        self._scan = dict(rename=n_rename, already=n_already, none=n_none,
                          skipped=n_skipped, total=len(meshes))
        # v1.14 — destructive-standardization warning gate (see VC).
        if not confirm_destructive_enabled():
            return self.execute(context)
        return context.window_manager.invoke_props_dialog(self, width=340)

    def draw(self, context):
        layout = self.layout
        s = getattr(self, '_scan', None)
        if not s:
            layout.label(text="Scanning...")
            return
        col = layout.column(align=True)
        col.label(text=f"{s['total']} mesh datablock(s):", icon='INFO')
        col.label(text=f"   {s['rename']} will get uv0/uv1/… names")
        col.label(text=f"   {s['already']} already standard")
        col.label(text=f"   {s['none']} without UVs (skipped)")
        col.label(text=f"   {s['skipped']} skipped (linked)")
        col.separator()
        col.label(text="WARNING: renames UV layers and retargets material "
                       "UV references.", icon='ERROR')
        col.label(text="OK to pursue — Esc / Cancel to ignore.")

    def execute(self, context):
        try:
            meshes, n_skipped = _gather(context)
            n_renamed = n_already = n_none = n_nodes = 0
            for me in meshes:
                action, rename = _standardize_uv_mesh(me)
                if action == 'renamed':
                    n_renamed += 1
                    n_nodes += _retarget_uv_nodes(me, rename)
                elif action == 'already':
                    n_already += 1
                else:
                    n_none += 1
        except Exception as exc:
            import traceback
            traceback.print_exc()
            self.report({'ERROR'}, f"MapLink Standardize UV failed: {exc}")
            return {'CANCELLED'}

        msg = (f"MapLink Standardize UV: {n_renamed} renamed "
               f"({n_nodes} material UV reference(s) retargeted), "
               f"{n_already} already standard, {n_none} without UVs, "
               f"{n_skipped} skipped (linked).")
        self.report({'INFO'}, msg)
        print(f"[MapLink] {msg}")
        # v1.14 — explicit end-of-action report.
        popup_report("Standardize UV — done", [
            f"{n_renamed} mesh(es) renamed to uv0/uv1/…",
            f"{n_nodes} material UV reference(s) retargeted",
            f"{n_already} already standard",
            f"{n_none} without UVs (skipped)",
            f"{n_skipped} skipped (linked)",
        ], icon='CHECKMARK')
        return {'FINISHED'}


# ---------------------------------------------------------------------------
# Operator — VC from Cavity (native Dirty Vertex Colors)
# ---------------------------------------------------------------------------

class MAPLINK_OT_VCFromCavity(Operator):
    bl_idname = "maplink.vc_from_cavity"
    bl_label = "MapLink: VC from Cavity"
    bl_description = ("Bake a cavity (Dirty Vertex Colors) mask over the whole "
                      "mesh into the selected channel of Color")
    bl_options = {'REGISTER', 'UNDO'}

    channel: EnumProperty(name="Channel", items=_CHANNEL_ITEMS, default='R')
    invert: BoolProperty(name="Invert", default=False)
    blur_strength: FloatProperty(name="Blur Strength", default=1.0,
                                 min=0.0, soft_max=1.0)
    blur_iterations: IntProperty(name="Blur Iterations", default=1,
                                 min=0, soft_max=40)
    dirt_angle: FloatProperty(name="Dirt Angle", default=0.0,
                              min=0.0, max=math.pi, subtype='ANGLE')
    clean_angle: FloatProperty(name="Clean Angle", default=math.pi,
                               min=0.0, max=math.pi, subtype='ANGLE')

    def invoke(self, context, event):
        vc = getattr(context.scene, 'maplink_vc', None)
        if vc is not None:
            self.channel = vc.channel
        return context.window_manager.invoke_props_dialog(self, width=300)

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "channel", expand=True)
        layout.prop(self, "invert")
        layout.prop(self, "blur_strength")
        layout.prop(self, "blur_iterations")
        layout.prop(self, "dirt_angle")
        layout.prop(self, "clean_angle")

    def execute(self, context):
        try:
            return self._run(context)
        except Exception as exc:
            import traceback
            traceback.print_exc()
            self.report({'ERROR'}, f"MapLink VC from Cavity failed: {exc}")
            return {'CANCELLED'}

    def _dirt(self):
        kwargs = dict(blur_strength=self.blur_strength,
                      blur_iterations=self.blur_iterations,
                      dirt_angle=self.dirt_angle,
                      clean_angle=self.clean_angle,
                      dirt_only=False)
        try:
            bpy.ops.paint.vertex_color_dirt(**kwargs)
        except RuntimeError:
            # Some builds require Vertex Paint mode for this operator.
            bpy.ops.object.mode_set(mode='VERTEX_PAINT')
            try:
                bpy.ops.paint.vertex_color_dirt(**kwargs)
            finally:
                bpy.ops.object.mode_set(mode='OBJECT')

    def _run(self, context):
        obj = _gate(self, context)
        if obj is None:
            return {'CANCELLED'}
        mesh = obj.data

        prev_mode = obj.mode
        if prev_mode != 'OBJECT':
            bpy.ops.object.mode_set(mode='OBJECT')

        tmp_name = _unique_attr_name(mesh, "__maplink_cavity__")
        tmp = mesh.color_attributes.new(name=tmp_name, type=STANDARD_TYPE,
                                        domain=STANDARD_DOMAIN)
        _fill(tmp, WHITE)
        _set_active_render(mesh, tmp_name)

        try:
            self._dirt()
            scratch = _get_color_attr(mesh, tmp_name)
            col = _get_color_attr(mesh, STANDARD_NAME)
            idx = CHANNEL_INDEX[self.channel]
            n = len(scratch.data)
            for i in range(n):
                g = scratch.data[i].color_srgb[0]
                if self.invert:
                    g = 1.0 - g
                c = col.data[i].color_srgb
                cc = [c[0], c[1], c[2], c[3]]
                cc[idx] = g
                col.data[i].color_srgb = cc
        finally:
            t = _get_color_attr(mesh, tmp_name)
            if t is not None:
                mesh.color_attributes.remove(t)
            _set_active_render(mesh, STANDARD_NAME)
            if prev_mode != 'OBJECT':
                try:
                    bpy.ops.object.mode_set(mode=prev_mode)
                except Exception:
                    pass

        suffix = " (inverted)" if self.invert else ""
        self.report({'INFO'},
                    f"MapLink VC from Cavity: written to channel "
                    f"{self.channel}{suffix}.")
        return {'FINISHED'}


# ---------------------------------------------------------------------------
# Operator — VC Contrast
# ---------------------------------------------------------------------------

class MAPLINK_OT_VCContrast(Operator):
    bl_idname = "maplink.vc_contrast"
    bl_label = "MapLink: VC Contrast"
    bl_description = ("Remap the selected channel about 0.5 "
                      "(new = 0.5 + (v - 0.5) * factor)")
    bl_options = {'REGISTER', 'UNDO'}

    channel: EnumProperty(name="Channel", items=_CHANNEL_ITEMS, default='R')
    factor: FloatProperty(name="Factor", default=1.0, min=0.0, soft_max=4.0)

    def invoke(self, context, event):
        vc = getattr(context.scene, 'maplink_vc', None)
        if vc is not None:
            self.channel = vc.channel
        return context.window_manager.invoke_props_dialog(self, width=260)

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "channel", expand=True)
        layout.prop(self, "factor")

    def execute(self, context):
        try:
            obj = _gate(self, context)
            if obj is None:
                return {'CANCELLED'}
            acc = _ColorAccess(obj)
            if acc.col is None:
                self.report({'ERROR'}, "Color layer not found.")
                return {'CANCELLED'}
            f = self.factor
            n = _write_channel(acc, self.channel, _sel_predicate(acc),
                               lambda v: 0.5 + (v - 0.5) * f)
            self.report({'INFO'},
                        f"MapLink VC Contrast: {n} corner(s), channel "
                        f"{self.channel}, factor {f:.2f}.")
            return {'FINISHED'}
        except Exception as exc:
            import traceback
            traceback.print_exc()
            self.report({'ERROR'}, f"MapLink VC Contrast failed: {exc}")
            return {'CANCELLED'}


# ---------------------------------------------------------------------------
# Operator — VC Set Selected
# ---------------------------------------------------------------------------

class MAPLINK_OT_VCSetSelected(Operator):
    bl_idname = "maplink.vc_set_selected"
    bl_label = "MapLink: VC Set Selected"
    bl_description = ("Set the selected channel to a value over the selected "
                      "vertices (Edit) or the whole mesh (Object)")
    bl_options = {'REGISTER', 'UNDO'}

    channel: EnumProperty(name="Channel", items=_CHANNEL_ITEMS, default='R')
    value: FloatProperty(name="Value", default=1.0, min=0.0, max=1.0,
                         subtype='FACTOR')

    def execute(self, context):
        try:
            obj = _gate(self, context)
            if obj is None:
                return {'CANCELLED'}
            acc = _ColorAccess(obj)
            if acc.col is None:
                self.report({'ERROR'}, "Color layer not found.")
                return {'CANCELLED'}
            val = self.value
            n = _write_channel(acc, self.channel, _sel_predicate(acc),
                               lambda v: val)
            self.report({'INFO'},
                        f"MapLink VC Set Selected: {n} corner(s), channel "
                        f"{self.channel} = {val:.3f}.")
            return {'FINISHED'}
        except Exception as exc:
            import traceback
            traceback.print_exc()
            self.report({'ERROR'}, f"MapLink VC Set Selected failed: {exc}")
            return {'CANCELLED'}


# ---------------------------------------------------------------------------
# Operator — VC Set Outer / Inner (open-boundary verts)
# ---------------------------------------------------------------------------

class MAPLINK_OT_VCSetOuter(Operator):
    bl_idname = "maplink.vc_set_outer"
    bl_label = "MapLink: VC Set Outer"
    bl_description = ("Set the selected channel on vertices that sit on an open "
                      "boundary (edges used by exactly one face)")
    bl_options = {'REGISTER', 'UNDO'}

    channel: EnumProperty(name="Channel", items=_CHANNEL_ITEMS, default='R')
    value: FloatProperty(name="Value", default=1.0, min=0.0, max=1.0,
                         subtype='FACTOR')

    def execute(self, context):
        try:
            obj = _gate(self, context)
            if obj is None:
                return {'CANCELLED'}
            acc = _ColorAccess(obj)
            if acc.col is None:
                self.report({'ERROR'}, "Color layer not found.")
                return {'CANCELLED'}
            bset = acc.boundary_vert_indices()
            val = self.value
            n = _write_channel(acc, self.channel,
                               lambda vi, vs: vi in bset, lambda v: val)
            self.report({'INFO'},
                        f"MapLink VC Set Outer: {n} corner(s) on boundary, "
                        f"channel {self.channel} = {val:.3f}.")
            return {'FINISHED'}
        except Exception as exc:
            import traceback
            traceback.print_exc()
            self.report({'ERROR'}, f"MapLink VC Set Outer failed: {exc}")
            return {'CANCELLED'}


class MAPLINK_OT_VCSetInner(Operator):
    bl_idname = "maplink.vc_set_inner"
    bl_label = "MapLink: VC Set Inner"
    bl_description = ("Set the selected channel on vertices NOT on an open "
                      "boundary (the inverse of Set Outer)")
    bl_options = {'REGISTER', 'UNDO'}

    channel: EnumProperty(name="Channel", items=_CHANNEL_ITEMS, default='R')
    value: FloatProperty(name="Value", default=1.0, min=0.0, max=1.0,
                         subtype='FACTOR')

    def execute(self, context):
        try:
            obj = _gate(self, context)
            if obj is None:
                return {'CANCELLED'}
            acc = _ColorAccess(obj)
            if acc.col is None:
                self.report({'ERROR'}, "Color layer not found.")
                return {'CANCELLED'}
            bset = acc.boundary_vert_indices()
            val = self.value
            n = _write_channel(acc, self.channel,
                               lambda vi, vs: vi not in bset, lambda v: val)
            self.report({'INFO'},
                        f"MapLink VC Set Inner: {n} corner(s) interior, "
                        f"channel {self.channel} = {val:.3f}.")
            return {'FINISHED'}
        except Exception as exc:
            import traceback
            traceback.print_exc()
            self.report({'ERROR'}, f"MapLink VC Set Inner failed: {exc}")
            return {'CANCELLED'}


# ---------------------------------------------------------------------------
# Operator — VC from Base Color (writes full RGB)
# ---------------------------------------------------------------------------

class MAPLINK_OT_VCFromBaseColor(Operator):
    bl_idname = "maplink.vc_from_base_color"
    bl_label = "MapLink: VC from Base Color"
    bl_description = ("Sample the active material's Base-Color image through "
                      "the active UV map (bilinear) into Color RGB; alpha "
                      "preserved")
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        try:
            return self._run(context)
        except Exception as exc:
            import traceback
            traceback.print_exc()
            self.report({'ERROR'}, f"MapLink VC from Base Color failed: {exc}")
            return {'CANCELLED'}

    def _run(self, context):
        obj = _gate(self, context)
        if obj is None:
            return {'CANCELLED'}

        mat = obj.active_material
        if mat is None or not mat.use_nodes:
            self.report({'WARNING'}, "No active node-based material.")
            return {'CANCELLED'}
        img = _find_base_color_image(mat)
        if img is None:
            self.report({'WARNING'},
                        "No Base-Color image texture on active material.")
            return {'CANCELLED'}

        acc = _ColorAccess(obj)
        if acc.col is None:
            self.report({'ERROR'}, "Color layer not found.")
            return {'CANCELLED'}
        if acc.uv is None:
            self.report({'WARNING'}, "No active UV map.")
            return {'CANCELLED'}

        w, h = int(img.size[0]), int(img.size[1])
        if w == 0 or h == 0:
            self.report({'WARNING'}, "Base-Color image has no pixels.")
            return {'CANCELLED'}
        px = list(img.pixels)
        if len(px) < w * h * 4:
            self.report({'WARNING'}, "Base-Color image data unavailable.")
            return {'CANCELLED'}

        pred = _sel_predicate(acc)
        n = 0
        for key, vi, vsel in acc.corners():
            if not pred(vi, vsel):
                continue
            uvco = acc.get_uv(key)
            r, g, b = _sample_bilinear(px, w, h, uvco[0], uvco[1])
            c = acc.get_color(key)
            c[0] = r
            c[1] = g
            c[2] = b
            acc.set_color(key, c)
            n += 1
        acc.flush()

        self.report({'INFO'},
                    f"MapLink VC from Base Color: {n} corner(s) sampled "
                    f"from '{img.name}'.")
        return {'FINISHED'}


# ---------------------------------------------------------------------------
# Operator — Patch Unwrap
# ---------------------------------------------------------------------------

class MAPLINK_OT_PatchUnwrap(Operator):
    bl_idname = "maplink.patch_unwrap"
    bl_label = "MapLink: Patch Unwrap"
    bl_description = ("Reset → Follow Active Quads (from the active quad) → "
                      "texel-density normalise → per-axis de-stretch, on the "
                      "selected quad faces")
    bl_options = {'REGISTER', 'UNDO'}

    texture_size: IntProperty(name="Texture Size", default=1024,
                              min=1, soft_max=8192)
    target_texel_density: FloatProperty(name="Target Texel Density",
                                        default=1024.0, min=0.001,
                                        soft_max=8192.0)
    follow_mode: EnumProperty(
        name="Follow Mode",
        items=[('EVEN', "Even", ""),
               ('LENGTH', "Length", ""),
               ('LENGTH_AVERAGE', "Length Average", "")],
        default='LENGTH_AVERAGE',
    )

    @classmethod
    def poll(cls, context):
        obj = context.active_object
        return (obj is not None and obj.type == 'MESH'
                and obj.mode == 'EDIT')

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self, width=280)

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "texture_size")
        layout.prop(self, "target_texel_density", text="Target TD")
        layout.prop(self, "follow_mode", text="Follow")

    def execute(self, context):
        try:
            return self._run(context)
        except Exception as exc:
            import traceback
            traceback.print_exc()
            self.report({'ERROR'}, f"MapLink Patch Unwrap failed: {exc}")
            return {'CANCELLED'}

    def _run(self, context):
        obj = context.active_object
        mesh = obj.data
        bm = bmesh.from_edit_mesh(mesh)

        uv = bm.loops.layers.uv.active
        if uv is None:
            self.report({'WARNING'}, "No active UV map.")
            return {'CANCELLED'}

        af = bm.faces.active
        if af is None or not af.select or len(af.verts) != 4:
            # No usable active quad — seed one from the selected quads so the
            # user doesn't have to mark an active face first.  Any selected
            # quad works as the Follow-Active-Quads seed.
            cand = next((f for f in bm.faces
                         if f.select and len(f.verts) == 4), None)
            if cand is None:
                self.report({'WARNING'},
                            "No selected quad face to seed the unwrap.")
                return {'CANCELLED'}
            bm.faces.active = cand
            af = cand

        # Deselect non-quad selected faces so the native ops only touch quads.
        n_skipped = 0
        for f in bm.faces:
            if f.select and len(f.verts) != 4:
                f.select = False
                n_skipped += 1
        bmesh.update_edit_mesh(mesh)

        # Native UV ops — these invalidate bmesh references.
        bpy.ops.uv.reset()
        bpy.ops.uv.follow_active_quads('EXEC_DEFAULT', mode=self.follow_mode)

        # Re-acquire bmesh after the native ops.
        bm = bmesh.from_edit_mesh(mesh)
        uv = bm.loops.layers.uv.active
        mw = obj.matrix_world
        quad_faces = [f for f in bm.faces if f.select and len(f.verts) == 4]
        if not quad_faces:
            self.report({'WARNING'}, "No quad faces to unwrap.")
            return {'CANCELLED'}

        # Island centroid (UV), fixed point for all scaling.
        sum_u = sum_v = 0.0
        cnt = 0
        for f in quad_faces:
            for l in f.loops:
                co = l[uv].uv
                sum_u += co.x
                sum_v += co.y
                cnt += 1
        if cnt == 0:
            self.report({'WARNING'}, "Degenerate island.")
            return {'CANCELLED'}
        cu = sum_u / cnt
        cv = sum_v / cnt

        ts = float(self.texture_size)

        def measure():
            tot_td = 0.0
            nt = 0
            wu_td = wu = 0.0
            wv_td = wv = 0.0
            for f in quad_faces:
                loops = f.loops
                m = len(loops)
                for i in range(m):
                    l0 = loops[i]
                    l1 = loops[(i + 1) % m]
                    uv0 = l0[uv].uv
                    uv1 = l1[uv].uv
                    du = uv1.x - uv0.x
                    dv = uv1.y - uv0.y
                    uv_len = math.hypot(du, dv)
                    w0 = mw @ l0.vert.co
                    w1 = mw @ l1.vert.co
                    world_len = (w1 - w0).length
                    if uv_len <= 1e-12 or world_len <= 1e-9:
                        continue
                    td = (uv_len * ts) / world_len
                    tot_td += td
                    nt += 1
                    au = du * du
                    av = dv * dv
                    s = au + av
                    if s <= 1e-18:
                        continue
                    wu_td += td * (au / s)
                    wu += (au / s)
                    wv_td += td * (av / s)
                    wv += (av / s)
            td_avg = tot_td / nt if nt else 0.0
            td_u = wu_td / wu if wu > 1e-9 else td_avg
            td_v = wv_td / wv if wv > 1e-9 else td_avg
            return td_avg, td_u, td_v

        td_avg, td_u, td_v = measure()
        target = float(self.target_texel_density)

        if td_avg <= 1e-9:
            self.report({'WARNING'},
                        "Degenerate UVs after unwrap; density not normalized.")
            bmesh.update_edit_mesh(mesh)
            return {'FINISHED'}

        # Uniform scale to target, about the centroid.
        s = target / td_avg
        for f in quad_faces:
            for l in f.loops:
                co = l[uv].uv
                co.x = cu + (co.x - cu) * s
                co.y = cv + (co.y - cv) * s

        # Per-axis de-stretch: bring U and V densities each to target.
        _, td_u2, td_v2 = measure()
        su = target / td_u2 if td_u2 > 1e-9 else 1.0
        sv = target / td_v2 if td_v2 > 1e-9 else 1.0
        for f in quad_faces:
            for l in f.loops:
                co = l[uv].uv
                co.x = cu + (co.x - cu) * su
                co.y = cv + (co.y - cv) * sv

        bmesh.update_edit_mesh(mesh)

        n_quads = len(quad_faces)
        self.report({'INFO'},
                    f"MapLink Patch Unwrap: {n_quads} quad faces unwrapped "
                    f"(td≈{int(round(target))} px/m), {n_skipped} non-quad "
                    f"skipped.")
        return {'FINISHED'}


# ---------------------------------------------------------------------------
# Property group — shared panel state
# ---------------------------------------------------------------------------

class MAPLINK_PG_VertexColor(PropertyGroup):
    channel: EnumProperty(name="Channel", items=_CHANNEL_ITEMS, default='R')
    value: FloatProperty(name="Value", default=1.0, min=0.0, max=1.0,
                         subtype='FACTOR')


# ---------------------------------------------------------------------------
# Panels
# ---------------------------------------------------------------------------

class MAPLINK_PT_Modeling(Panel):
    bl_label = "Modeling"
    bl_idname = "MAPLINK_PT_modeling"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "MapLink"
    bl_order = 2

    def draw(self, context):
        layout = self.layout
        vc = getattr(context.scene, 'maplink_vc', None)
        if vc is None:
            layout.label(text="Properties not loaded", icon='ERROR')
            return

        # Shared channel selector.
        layout.row(align=True).prop(vc, "channel", expand=True)

        # Set: value + Selected / Outer / Inner on one row.
        box = layout.box()
        box.prop(vc, "value")
        r = box.row(align=True)
        for idn, txt in (("maplink.vc_set_selected", "Selected"),
                         ("maplink.vc_set_outer", "Outer"),
                         ("maplink.vc_set_inner", "Inner")):
            op = r.operator(idn, text=txt)
            op.channel = vc.channel
            op.value = vc.value

        # Contrast + Cavity — parameters pop up on click (seeded by channel).
        r = layout.row(align=True)
        r.operator("maplink.vc_contrast", text="Contrast", icon='IMAGE_DATA')
        r.operator("maplink.vc_from_cavity", text="Cavity", icon='BRUSH_DATA')

        # Bake from the active material's base-color texture.
        layout.operator("maplink.vc_from_base_color",
                        text="VC from Base Color", icon='IMAGE_DATA')

        # UV — Patch Unwrap (parameters pop up on click).
        layout.separator()
        layout.operator("maplink.patch_unwrap",
                        text="Patch Unwrap", icon='UV')


# ---------------------------------------------------------------------------
# Registration
# ---------------------------------------------------------------------------
# v1.14 — the per-action batch operators (batch_clean_outliner /
# batch_conform_vc / batch_clean_collections) and their one-subprocess-for-
# everything worker are superseded by Batch Standardize (batch_standardize.py:
# per-file headless subprocesses, toggles, selection table).

_classes = (
    MAPLINK_OT_CheckStandardization,
    MAPLINK_OT_StandardizeVC,
    MAPLINK_OT_ConformVertexColorsAlias,
    MAPLINK_OT_StandardizeUV,
    MAPLINK_OT_VCFromCavity,
    MAPLINK_OT_VCContrast,
    MAPLINK_OT_VCSetSelected,
    MAPLINK_OT_VCSetOuter,
    MAPLINK_OT_VCSetInner,
    MAPLINK_OT_VCFromBaseColor,
    MAPLINK_OT_PatchUnwrap,
    MAPLINK_PG_VertexColor,
)


def register():
    for c in _classes:
        bpy.utils.register_class(c)
    bpy.types.Scene.maplink_vc = bpy.props.PointerProperty(
        type=MAPLINK_PG_VertexColor)


def unregister():
    try:
        del bpy.types.Scene.maplink_vc
    except Exception:
        pass
    for c in reversed(_classes):
        try:
            bpy.utils.unregister_class(c)
        except Exception:
            pass
