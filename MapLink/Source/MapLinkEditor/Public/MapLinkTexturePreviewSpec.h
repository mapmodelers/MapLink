// Copyright MapModelers. All Rights Reserved.
//
// v1.16 — Container specifications, taken from the bytes of the four locked
// exemplars in ue/Tests/Fixtures/Textures/, never from documentation.
//
//   template_rgb.tga  md5 5caeaaf0c184687c634be3402e21ac0d   64x64 RGB
//   template_g.tga    md5 0fec198648e637106e565118b55a31fe   64x64 grayscale
//   template_rgb.png  md5 a189a04d65fb1e3706cf7ce3aac3467e   64x64 RGB
//   template_g.png    md5 02e0f9d5a44ce89c62a187821620bc9e   64x64 grayscale
//
// The TGA half is the live specification for FMapLinkTgaWriter (v1.16).
// The PNG half is recorded now and first used by FMapLinkTexturePreviewGen in
// Phase 2, so the byte analysis is done once and never repeated.
//
// PNG parity is PROPERTY parity — colour type, bit depth, interlace, chunk
// set — never byte parity. Two deflate encoders may legitimately produce
// different IDAT bytes for identical pixels.

#pragma once

#include "CoreMinimal.h"

namespace MapLinkTgaSpec
{
    // ── Header, 18 bytes, little-endian ───────────────────────────────────
    //
    // Verified identical across the two Substance exemplars AND the four
    // UE-written fixtures in ue/Content/Materials/. One writer satisfies both,
    // which is what makes the idempotence acceptance test achievable at all.

    static constexpr uint8 IdLength        = 0;    // offset 0  — no image ID field
    static constexpr uint8 ColourMapType   = 0;    // offset 1  — no colour map
    static constexpr uint8 ImageTypeRGB    = 2;    // offset 2  — uncompressed true-colour
    static constexpr uint8 ImageTypeGray   = 3;    // offset 2  — uncompressed grayscale
    // offsets 3..7 — colour-map spec, all zero
    // offsets 8..11 — x/y origin, both zero
    static constexpr uint8 PixelDepthRGB   = 24;   // offset 16
    static constexpr uint8 PixelDepthGray  = 8;    // offset 16

    /** Offset 17. Low nibble is attribute-bits-per-pixel, high nibble bits 4-5
     *  are the origin corner.
     *
     *  Substance Designer AND Unreal both write 0x08 on 8-bit grayscale — an
     *  attribute-bits value of 8 on a single-channel image is not what the TGA
     *  2.0 spec means by that field, but it is what both producers emit and
     *  what UE's importer reads back without complaint. Emitting the spec-pure
     *  0x00 instead would fail `fc /b` on every grayscale texture. Copy the
     *  producers, not the spec. */
    static constexpr uint8 DescriptorRGB   = 0x00; // alpha bits 0, origin bottom-left
    static constexpr uint8 DescriptorGray  = 0x08; // alpha bits 8, origin bottom-left

    /** Origin bits are 00 = BOTTOM-left, so the first row in the file is the
     *  BOTTOM row of the image. FTextureSource mip 0 is top-down, so rows are
     *  written in reverse. Getting this wrong flips every texture vertically
     *  and nothing warns you. */
    static constexpr bool  bBottomUpRows   = true;

    /** 24-bit TGA channel order is B, G, R. FTextureSource BGRA8 minus its
     *  alpha byte is already in that order — no swizzle, by luck rather than
     *  design, but verified against the exemplar: file bytes 47 94 FF decode
     *  as RGB(255,148,71). */
    static constexpr bool  bBGRChannelOrder = true;

    static constexpr bool  bRunLengthEncoded = false;

    static constexpr int32 HeaderBytes = 18;
    static constexpr int32 FooterBytes = 26;

    // ── Footer, 26 bytes, TGA 2.0 ─────────────────────────────────────────
    //
    //   uint32  extension area offset   = 0  (no extension area)
    //   uint32  developer dir offset    = 0  (no developer directory)
    //   char[16] "TRUEVISION-XFILE"
    //   char     '.'   (0x2E)
    //   char     '\0'  (0x00)

    static constexpr uint32 ExtensionAreaOffset  = 0;
    static constexpr uint32 DeveloperDirOffset   = 0;
    static const     ANSICHAR* const Signature   = "TRUEVISION-XFILE";
    static constexpr uint8  SignatureDot         = 0x2E;
    static constexpr uint8  SignatureTerminator  = 0x00;

    /** Exact file size for a conform TGA. The exemplars account exactly:
     *  no padding, no ID field, no colour map, no extension area. */
    inline int64 ExpectedFileSize(int32 Width, int32 Height, bool bGrayscale)
    {
        const int64 BytesPerPixel = bGrayscale ? 1 : 3;
        return HeaderBytes
             + static_cast<int64>(Width) * Height * BytesPerPixel
             + FooterBytes;
    }
}

namespace MapLinkPngSpec
{
    // ── Phase 2 preview PNG — property targets ────────────────────────────

    /** IHDR colour type. 2 = truecolour RGB, 0 = grayscale. Both exemplars
     *  are 8-bit and carry no alpha channel; previews never need one, since
     *  MapLink-class textures are RGB or grayscale by definition (§5.2) and an
     *  External preview only has to look right in a Blender viewport. */
    static constexpr uint8 ColourTypeRGB   = 2;
    static constexpr uint8 ColourTypeGray  = 0;
    static constexpr uint8 BitDepth        = 8;
    static constexpr uint8 InterlaceNone   = 0;   // Adam7 off
    static constexpr bool  bHasAlpha       = false;

    /** THE FINDING THAT MATTERS: neither exemplar carries sRGB, gAMA, cHRM or
     *  iCCP. The preview PNG is exactly as mute about colour space as the TGA
     *  is, so Blender guesses for it too. The JSON contract in
     *  FMapLinkMaterialExporter is the ONLY channel that carries colour-space
     *  intent, for both containers. Do not "improve" the preview encoder by
     *  writing an sRGB chunk — that would put a second, competing source of
     *  truth on disk. */
    static constexpr bool  bWriteSRGBChunk = false;
    static constexpr bool  bWriteGammaChunk = false;
    static constexpr bool  bWriteChromaChunk = false;
    static constexpr bool  bWriteIccpChunk = false;

    /** The exemplars carry pHYs (2834x2834 per metre = 72 DPI) and a 943-byte
     *  iTXt Adobe XMP packet naming Substance Designer as the creator.
     *
     *  A MapLink preview reproduces NEITHER. The DPI is meaningless for a
     *  texture, and the XMP would assert a provenance that is simply false on
     *  a file UE generated — on the RGB exemplar it is also 78% of the total
     *  file size. Chunk set is therefore IHDR, IDAT, IEND. */
    static constexpr bool  bWritePhysChunk = false;
    static constexpr bool  bWriteXmpChunk  = false;

    /** Default long-edge cap for a generated preview, overridable per texture
     *  through FMapLinkTexturePolicyEntry::PreviewMaxRes. */
    static constexpr int32 DefaultMaxResolution = 1024;
}
