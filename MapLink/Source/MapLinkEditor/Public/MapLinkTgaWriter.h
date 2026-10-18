// Copyright MapModelers. All Rights Reserved.
//
// v1.16 — Byte-level TGA writer.
//
// Deliberately NOT IImageWrapper, and not any serialiser whose byte layout we
// do not control: the acceptance test is that a Substance-exported TGA which is
// already correctly located and named is detected as conform and NOT rewritten,
// verified byte-identical with `fc /b`, and that a second full run over a
// conformed library performs zero writes. That is only achievable if every one
// of the 18 header bytes and 26 footer bytes is ours to choose. Constants come
// from MapLinkTexturePreviewSpec.h, which took them from the exemplar bytes.
//
// PIXELS ALWAYS COME FROM UTexture2D::Source (FTextureSource, mip 0).
// The on-disk file is never decoded. That is what makes the pipeline
// independent of whatever format UE originally imported — PSD, TIF, PNG, EXR.
//
// CONFORMANCE GOVERNS CONTAINER, LOCATION AND NAME — NEVER PIXEL VALUES.
// No sRGB<->linear conversion, no channel math, no resizing. The one exception
// is the explicit, recorded, opt-outable bit-depth reduction in §5.3, which is
// flagged lossy on the policy entry precisely because it is a value change.

#pragma once

#include "CoreMinimal.h"

class UTexture2D;

/** Mip 0, normalised to 8 bits per channel, in UE's native top-down BGRA
 *  order. The writer flips to bottom-up and drops to BGR or G8 on the way out;
 *  keeping this buffer in UE order means the alpha analysis and the grayscale
 *  detection all run in one coordinate system. */
struct FMapLinkTextureSurface
{
    int32 Width  = 0;
    int32 Height = 0;

    /** Width * Height * 4, top-down, B,G,R,A. */
    TArray64<uint8> BGRA;

    /** The FTextureSource format was single-channel (TSF_G8 / TSF_G16 /
     *  TSF_R16F / TSF_R32F). Forces grayscale output regardless of content. */
    bool bSingleChannelSource = false;

    /** Source was above 8 bits per channel and was scaled down. Sets
     *  lossy_reduction on the policy entry — height maps terrace. */
    bool bReducedBitDepth = false;

    /** Source format carried an alpha channel at all. */
    bool bHasAlphaChannel = false;

    /** Every alpha pixel is the same value. Dropped silently (§5.2). */
    bool bAlphaUniform = true;

    /** R == G == B for every pixel. Reported as a grayscale CANDIDATE, never
     *  auto-converted: converting changes what CompressionSettings should be,
     *  which is a material decision, not a container one. */
    bool bChannelsIdentical = false;

    bool IsValid() const { return Width > 0 && Height > 0 && BGRA.Num() > 0; }

    int64 NumPixels() const
    {
        return static_cast<int64>(Width) * static_cast<int64>(Height);
    }
};

/** What a write attempt actually did. Every field feeds the run manifest. */
struct FMapLinkTgaWriteResult
{
    bool bSuccess = false;

    /** The bytes on disk already matched byte for byte, so nothing was
     *  written. This is the acceptance test, expressed as a return value. */
    bool bSkippedIdentical = false;

    bool bWroteGrayscale     = false;
    bool bReducedBitDepth    = false;
    bool bDroppedUniformAlpha = false;

    /** Non-uniform alpha was present. The caller must extract it into a
     *  separate T_Mk_<Stem>_A grayscale texture and raise an action item: the
     *  parent material needs a new texture parameter. The writer does not and
     *  must not author material parameters. */
    bool bHasNonUniformAlpha = false;

    /** Channels were pixel-identical on an RGB source — worth telling the user
     *  about, never worth acting on automatically. */
    bool bGrayscaleCandidate = false;

    /** CompressionSettings said grayscale but the source channels were NOT
     *  identical, so G and B were discarded to honour §5.1. A contradiction in
     *  the asset rather than in the writer, but it is a real pixel loss and it
     *  is reported as one rather than buried. */
    bool bDiscardedColourChannels = false;

    int64   BytesWritten = 0;
    FString Error;
};

/** Parsed 18-byte header of an on-disk TGA, for conformance evaluation. */
struct FMapLinkTgaHeaderInfo
{
    uint8  IdLength      = 0;
    uint8  ColourMapType = 0;
    uint8  ImageType     = 0;
    uint16 Width         = 0;
    uint16 Height        = 0;
    uint8  PixelDepth    = 0;
    uint8  Descriptor    = 0;
    bool   bHasFooter    = false;
    int64  FileSize      = 0;

    bool IsGrayscale() const { return ImageType == 3 && PixelDepth == 8; }
    bool IsRGB24()     const { return ImageType == 2 && PixelDepth == 24; }
};

class MAPLINKEDITOR_API FMapLinkTgaWriter
{
public:
    // ── Source extraction ─────────────────────────────────────────────────

    /**
     * Pull mip 0 out of the texture's editor source data and normalise it to
     * 8-bit BGRA. Never touches the file on disk.
     *
     * Sources above 8 bits per channel are scaled with (v * 255 + 32767) / 65535
     * — round-to-nearest rather than a truncating shift, so a full-white 16-bit
     * pixel lands on 255 and mid-grey does not drift downward. bAllowReduction
     * false makes such a source fail here instead, so the caller can report it
     * as skipped rather than silently degrade a height map.
     *
     * Returns false with OutError set when the texture has no source data at
     * all (EMapLinkResult::TextureNoSourceData territory) or the format is one
     * we refuse to guess at.
     */
    static bool ExtractSurface(
        const UTexture2D*        Texture,
        bool                     bAllowBitDepthReduction,
        FMapLinkTextureSurface&  OutSurface,
        FString&                 OutError);

    // ── Byte assembly ─────────────────────────────────────────────────────

    /** Build the complete file image — header, bottom-up payload, footer.
     *  Pure function of the surface; no disk access, which is what makes the
     *  synthetic 4x4 round-trip test possible. */
    static void BuildFileBytes(
        const FMapLinkTextureSurface& Surface,
        bool                          bForceGrayscale,
        TArray64<uint8>&              OutBytes);

    /** Header only, 18 bytes appended to OutBytes. */
    static void AppendHeader(TArray64<uint8>& OutBytes,
                             int32 Width, int32 Height, bool bGrayscale);

    /** TGA 2.0 footer, 26 bytes appended to OutBytes. */
    static void AppendFooter(TArray64<uint8>& OutBytes);

    // ── Disk ──────────────────────────────────────────────────────────────

    /**
     * Write the surface to AbsolutePath as a conform TGA — but ONLY if the
     * bytes would differ from what is already there.
     *
     * The comparison is a full byte compare of the assembled image against the
     * existing file, not a header inspection and not a heuristic. That makes
     * idempotence a property of the code rather than something the caller has
     * to be careful about, and it is why a second full conform run over an
     * already-conformed library performs zero writes.
     *
     * The write itself is temp-then-move on the same directory, so a failure
     * never leaves a half-written texture where a valid one used to be.
     */
    static FMapLinkTgaWriteResult WriteSurface(
        const FMapLinkTextureSurface& Surface,
        const FString&                AbsolutePath,
        bool                          bForceGrayscale);

    /** ExtractSurface + grayscale classification + WriteSurface. The path the
     *  conform operation actually calls. */
    static FMapLinkTgaWriteResult WriteFromTexture(
        const UTexture2D* Texture,
        const FString&    AbsolutePath,
        bool              bAllowBitDepthReduction);

    /** Extract the alpha channel as an 8-bit grayscale surface, for the
     *  non-uniform-alpha split in §5.2. */
    static bool ExtractAlphaSurface(
        const FMapLinkTextureSurface& Source,
        FMapLinkTextureSurface&       OutAlpha);

    // ── Inspection ────────────────────────────────────────────────────────

    /** Read and validate the 18-byte header and the 26-byte footer of an
     *  existing file. False when the file is missing, too short, or not a TGA
     *  at all — a .png that someone renamed to .tga fails here. */
    static bool ReadHeader(const FString& AbsolutePath,
                           FMapLinkTgaHeaderInfo& OutInfo);

    /** True when the on-disk file matches the locked exemplar spec: type 2/24
     *  or type 3/8, no ID field, no colour map, no RLE, bottom-left origin,
     *  the expected descriptor byte for its type, a TGA 2.0 footer, and a size
     *  that accounts exactly. Anything else is a container failure and the
     *  texture is not conform. */
    static bool MatchesLockedSpec(const FMapLinkTgaHeaderInfo& Info,
                                  FString& OutReason);

    /** Convenience: ReadHeader + MatchesLockedSpec. */
    static bool IsConformContainer(const FString& AbsolutePath,
                                   FString& OutReason);

    // ── Self-test ─────────────────────────────────────────────────────────

    /** Synthetic 4x4 round-trip: build RGB and grayscale images from known
     *  pixels, parse them back, and assert every constant plus the bottom-up
     *  row order and the BGR channel order. Logs to LogMapLink and returns
     *  false on the first mismatch. Wired to MapLink.Textures.SelfTest. */
    static bool RunSelfTest();
};
