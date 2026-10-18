"""MapLink thumbnail badging — composite the type icon into an asset preview.

The preview bitmap is the only surface where a type icon reaches the Asset
Browser grid, the File Browser thumbnail mode, template_ID_preview() and the
drag-and-drop ghost.  Drawing over the Asset Browser with a draw handler is not
viable (Blender exposes no per-tile bounding boxes to Python, so the grid would
have to be reconstructed from display_size, scroll offset and region width --
version-brittle and not shippable), and replacing the preview outright throws
away the rendered mesh.  Compositing into the existing bitmap keeps the render
and needs no layout guessing.

NOTHING HERE RUNS AUTOMATICALLY.  The badge is applied only when the thumbnail
is being regenerated anyway -- Run Thumbnail, or the explicit re-bake operator.
It is deliberately not on save_post: the preview lives inside the .blend, so
badging on save would dirty every asset file in Perforce as a side effect of
an unrelated action.

Bare-importable on purpose (no package-relative import at module level) so the
headless thumbnail worker can use it the same way batch_sidecar is reused.

Buffer shapes, probed on Blender 5.0 rather than assumed -- they differ, and
getting this wrong is what makes a badge silently do nothing:

    preview.image_pixels        length w*h      ONE packed RGBA int per pixel
    preview.image_pixels_float  length w*h*4    four floats per pixel, 0..1

The float view is the one with per-channel data, so that is what gets written.
maplink.probe_preview prints both for any asset.

Known limitation, accepted for this pass: ImagePreview also carries icon_pixels
at a second, smaller resolution, and the two do not sync.  Only the image-sized
buffer is written, so the badge shows in grid view and not in list view.
Overwriting icon_pixels wholesale is the fix if list view ever matters;
compositing into it is not, because it is too small to carry both the render
and a legible badge.
"""

import os

import bpy


# Preview must be at least this wide for a badge to be legible rather than
# noise. Below it the icon is left off entirely.
MIN_PREVIEW = 64

# Badge edge as a fraction of the preview edge, and its inset from the corner.
BADGE_FRACTION = 0.42
MARGIN_FRACTION = 0.055

# Both ImagePreview.image_pixels and Image.pixels are documented bottom-row
# first, the OpenGL convention. If a future Blender changes that, the badge
# lands in the top-left instead of the bottom-left and this is the one line to
# flip. maplink.probe_preview reports the layout it actually sees.
BOTTOM_ROW_FIRST = True

_ICON_DIR = os.path.join(os.path.dirname(__file__), "resources", "icons")

# Badge pixels are read once per source and reused. Re-reading per asset is the
# expensive part of a bulk re-bake.
_badge_cache = {}


def _classify(name):
    """Type key for a .blend stem, via the addon's one classifier."""
    try:
        from . import icons           # addon package
    except ImportError:
        try:
            import icons              # bare, headless worker
        except ImportError:
            return None
    return icons.classify(name)


def _badge_source(key, size=32):
    """-> (w, h, [float RGBA]) for a type key, or None when unavailable."""
    cache_key = (key, size)
    if cache_key in _badge_cache:
        return _badge_cache[cache_key]

    # Ask icons.py for the filename rather than keeping a copy of the table.
    # A second copy is how PRESET ended up with art on disk that this module
    # did not know the name of: the icon showed everywhere except the badge.
    name = None
    try:
        from . import icons
    except ImportError:
        try:
            import icons
        except ImportError:
            icons = None
    if icons is not None:
        entry = getattr(icons, "_TYPES", {}).get(key)
        if entry:
            name = entry[0]
    if not name:
        return None

    path = os.path.join(_ICON_DIR, str(size), f"{name}.png")
    if not os.path.exists(path):
        _badge_cache[cache_key] = None
        return None

    # v1.20 — the icon datablock has to go again afterwards.
    #
    # This loaded the PNG and left it in bpy.data.images, so every badged file
    # gained an orphan image datablock that the next save wrote into the .blend.
    # One stray image per asset was survivable while badging was a manual
    # re-bake; the material sync badges 86 materials in a library whose whole
    # cleanup contract is "leave no orphans", and it reports images_kept
    # precisely so a leak like this is visible. Found before it shipped.
    #
    # Only a datablock WE created is removed: check_existing=True can hand back
    # an image the artist already had open, and deleting that would be worse
    # than the leak.
    pre_existing = {i.name for i in bpy.data.images}
    img = None
    try:
        img = bpy.data.images.load(path, check_existing=True)
        # Read the stored values, not a colour-managed version of them. The
        # preview buffer is sRGB bytes, so raw 0..1 maps onto it directly and
        # no linear/sRGB conversion is needed in the composite.
        try:
            img.colorspace_settings.name = 'Raw'
        except Exception:
            try:
                img.colorspace_settings.name = 'Non-Color'
            except Exception:
                pass
        w, h = img.size
        px = list(img.pixels[:])
    except Exception as e:
        print(f"[MapLink Badge] could not read {path}: {e}")
        _badge_cache[cache_key] = None
        return None
    finally:
        # The cache keeps a plain list of floats, so the datablock is not
        # needed once the pixels are copied out.
        if img is not None and img.name not in pre_existing:
            try:
                bpy.data.images.remove(img)
            except Exception as exc:                       # noqa: BLE001
                print(f"[MapLink Badge] could not release {path}: {exc}")

    if w <= 0 or h <= 0 or len(px) != w * h * 4:
        _badge_cache[cache_key] = None
        return None

    _badge_cache[cache_key] = (w, h, px)
    return _badge_cache[cache_key]


def _nearest_resize(src, sw, sh, dw, dh):
    """Nearest-neighbour RGBA resize. Good enough: the source is authored at
    128 and only ever scales down to a corner, and pulling in a filtering
    dependency for that would be silly."""
    if (sw, sh) == (dw, dh):
        return src
    out = [0.0] * (dw * dh * 4)
    for y in range(dh):
        sy = min(sh - 1, (y * sh) // dh)
        for x in range(dw):
            sx = min(sw - 1, (x * sw) // dw)
            s = (sy * sw + sx) * 4
            d = (y * dw + x) * 4
            out[d:d + 4] = src[s:s + 4]
    return out


def probe(datablock):
    """-> dict describing the preview buffer, for diagnosis.

    Answers the questions the design left open: whether image_size is non-zero
    after preview_ensure(), whether the byte or float array is the populated
    one, and what range each carries.
    """
    info = {}
    try:
        datablock.preview_ensure()
    except Exception as e:
        info["preview_ensure"] = f"failed: {e}"
    p = getattr(datablock, "preview", None)
    if p is None:
        info["preview"] = "None"
        return info

    info["image_size"] = tuple(p.image_size)
    for attr in ("image_pixels", "image_pixels_float"):
        try:
            buf = getattr(p, attr)
            n = len(buf)
            head = list(buf[:8])
            info[attr] = {
                "len": n,
                "head": head,
                "max": max(buf[:4096]) if n else None,
            }
        except Exception as e:
            info[attr] = f"unavailable: {e}"
    w, h = info["image_size"]
    info["expected_len"] = w * h * 4
    return info


def apply_badge(datablock, key=None, name_hint=None):
    """Composite the type badge into *datablock*'s preview. -> (ok, message).

    Never raises. A datablock whose preview is empty or whose buffer does not
    match its declared size is left alone and reported, because writing a
    half-understood buffer would corrupt a thumbnail that is currently fine.
    """
    if key is None:
        key = _classify(name_hint or getattr(datablock, "name", "")
                        or bpy.data.filepath)
    if key is None:
        return False, "not a MapLink asset name"

    try:
        datablock.preview_ensure()
    except Exception as e:
        return False, f"preview_ensure failed: {e}"

    p = getattr(datablock, "preview", None)
    if p is None:
        return False, "no preview"

    try:
        w, h = p.image_size
    except Exception as e:
        return False, f"no image_size: {e}"
    if w <= 0 or h <= 0:
        return False, "preview is empty (generate the thumbnail first)"
    if w < MIN_PREVIEW:
        return False, f"preview too small ({w}x{h})"

    # ImagePreview exposes the same bitmap two ways, and they are NOT the same
    # shape:
    #     image_pixels        length w*h    — ONE packed RGBA int per pixel
    #     image_pixels_float  length w*h*4  — four floats per pixel, 0..1
    # The float view is the one with per-channel data, so that is what gets
    # composited. Probed on Blender 5.0 with maplink.probe_preview.
    #
    # Whichever array actually carries w*h*4 elements is used, so a build that
    # changes this does not silently write a mangled buffer.
    want = w * h * 4
    dst, target, scale = None, None, 1.0
    for attr, sc in (("image_pixels_float", 1.0), ("image_pixels", 255.0)):
        try:
            buf = getattr(p, attr)
        except Exception:
            continue
        if len(buf) == want:
            dst, target, scale = list(buf[:]), attr, sc
            break
    if dst is None:
        try:
            sizes = {a: len(getattr(p, a))
                     for a in ("image_pixels", "image_pixels_float")}
        except Exception:
            sizes = {}
        return False, (f"no preview buffer of length {want} for {w}x{h} "
                       f"(found {sizes}) — refusing to write")

    src = _badge_source(key)
    if src is None:
        return False, f"no badge art for {key}"
    sw, sh, spx = src

    bw = max(8, int(w * BADGE_FRACTION))
    bh = max(8, int(h * BADGE_FRACTION))
    badge = _nearest_resize(spx, sw, sh, bw, bh)

    mx = max(1, int(w * MARGIN_FRACTION))
    my = max(1, int(h * MARGIN_FRACTION))
    # Bottom-left in buffer order. With bottom-row-first that is the visual
    # bottom-left; the constant documents the assumption.
    # Top-left. Buffer row 0 is the visual BOTTOM, so the top of the image is
    # the high end of the buffer.
    oy = (h - bh - my) if BOTTOM_ROW_FIRST else my
    ox = mx

    # Coverage is thresholded to binary, so a pixel is either fully replaced or
    # untouched. That makes the composite idempotent: re-applying the badge to
    # an already-badged preview writes identical values.
    #
    # A true alpha-over is not idempotent -- each pass blends the antialiased
    # edge pixels further toward the badge -- and this runs behind a button
    # called "Re-apply", which has to be safe to press twice. The cost is an
    # aliased edge on the plate's four rounded corners, which is nothing next
    # to the nearest-neighbour resize already applied to the interior.
    # Badge values are 0..1 (read from a Raw-colourspace image), so writing the
    # float view is a straight copy and the byte view needs the 255 scale.
    hi = 1.0 * scale
    for by in range(bh):
        ty = oy + by
        if not (0 <= ty < h):
            continue
        for bx in range(bw):
            tx = ox + bx
            if not (0 <= tx < w):
                continue
            s = (by * bw + bx) * 4
            if badge[s + 3] < 0.5:
                continue
            d = (ty * w + tx) * 4
            for c in range(3):
                v = badge[s + c] * scale
                v = hi if v > hi else (0.0 if v < 0.0 else v)
                dst[d + c] = v if scale == 1.0 else int(v + 0.5)
            dst[d + 3] = hi if scale == 1.0 else int(hi)

    buf = getattr(p, target)
    try:
        buf.foreach_set(dst)
    except Exception:
        try:
            setattr(p, target, dst)
        except Exception as e:
            return False, f"could not write {target}: {e}"

    return True, (f"badged {key.lower()} at {bw}x{bh} into {w}x{h} "
                  f"via {target}")


def write_icon_buffer(datablock, key):
    """Fill the preview's small icon buffer with the type icon, whole.

    ImagePreview carries two bitmaps: the big one the Asset Browser draws at
    large display sizes, and a small icon_size one used when the preview is
    drawn small.  At that size a corner badge is a handful of pixels and reads
    as dirt, so the small buffer gets the type icon at full size instead --
    the render is unreadable there anyway, and the type is the only thing worth
    showing.

    Returns a short status string. Never raises.
    """
    p = getattr(datablock, "preview", None)
    if p is None:
        return "no preview"

    try:
        iw, ih = p.icon_size
    except Exception as e:
        return f"no icon_size: {e}"
    if iw <= 0 or ih <= 0:
        # Normal for a collection preview: Blender allocates the small icon
        # bitmap lazily and for collections it stays empty, which is also why
        # the Asset Browser scales the large one at every display size instead
        # of switching to this. Nothing to do and nothing wrong.
        return "no small icon buffer (normal for collections)"

    src = _badge_source(key)
    if src is None:
        return f"no art for {key}"
    sw, sh, spx = src

    want = iw * ih * 4
    for attr, scale in (("icon_pixels_float", 1.0), ("icon_pixels", 255.0)):
        try:
            buf = getattr(p, attr)
        except Exception:
            continue
        if len(buf) != want:
            continue

        scaled = _nearest_resize(spx, sw, sh, iw, ih)
        if scale == 1.0:
            out = [min(1.0, max(0.0, v)) for v in scaled]
        else:
            out = [int(min(255.0, max(0.0, v * 255.0)) + 0.5) for v in scaled]
        # The icon plate is opaque, so a straight overwrite is correct here and
        # is idempotent for free.
        try:
            buf.foreach_set(out)
        except Exception:
            try:
                setattr(p, attr, out)
            except Exception as e:
                return f"could not write {attr}: {e}"
        return f"icon {iw}x{ih} via {attr}"

    return f"no icon buffer of length {want} for {iw}x{ih}"


def asset_collections():
    """Every local asset-marked collection in the file."""
    return [c for c in bpy.data.collections
            if c.library is None and getattr(c, "asset_data", None) is not None]


def badge_current_file():
    """Badge every asset-marked collection in the open file. -> (n, messages).

    This is what the worker and the re-bake operator both call, so the badge is
    identical however it was triggered.
    """
    stem = os.path.splitext(os.path.basename(bpy.data.filepath))[0]
    key = _classify(stem)
    done, msgs = 0, []
    for coll in asset_collections():
        ok, msg = apply_badge(coll, key=key, name_hint=stem)
        # The small icon buffer is written regardless of how the big one went:
        # it is a straight overwrite with no dependence on the render, so it
        # still works for a preview the corner badge had to refuse.
        icon_msg = write_icon_buffer(coll, key) if key else "no type"
        msgs.append(f"{coll.name}: {msg} | {icon_msg}")
        if ok:
            done += 1
    return done, msgs
