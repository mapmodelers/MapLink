// Copyright MapModelers. All Rights Reserved.
//
// v1.16 — Byte-level TGA writer (see header for the contract).

#include "MapLinkTgaWriter.h"

#include "MapLinkTexturePreviewSpec.h"
#include "MapLinkPipeline.h"          // LogMapLink lives here, NOT in MapLinkUtils.h

#include "Engine/Texture2D.h"
#include "Engine/TextureDefines.h"
#include "ImageCore.h"
#include "Math/Float16.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"

namespace
{
    // ── Quantisation ──────────────────────────────────────────────────────
    //
    // (v * 255 + 32767) / 65535 rather than (v >> 8). The shift is a truncating
    // divide by 65536/255-ish and drifts every value downward: 0xFFFF would
    // become 0xFF only by luck and mid-grey lands a step low. Round-to-nearest
    // costs one add and is exact at both ends.

    FORCEINLINE uint8 Quantize16(uint16 Value)
    {
        return static_cast<uint8>((static_cast<uint32>(Value) * 255u + 32767u) / 65535u);
    }

    FORCEINLINE uint8 QuantizeFloat(float Value)
    {
        // Sources above 8 bits may legitimately exceed [0,1] (EXR, HDR
        // authoring). Clamping is the only defined answer when the destination
        // is 8-bit unsigned, and it is recorded as a lossy reduction.
        return static_cast<uint8>(
            FMath::Clamp(FMath::RoundToInt(Value * 255.0f), 0, 255));
    }

    FORCEINLINE void PutU16LE(TArray64<uint8>& Out, uint16 Value)
    {
        Out.Add(static_cast<uint8>(Value & 0xFF));
        Out.Add(static_cast<uint8>((Value >> 8) & 0xFF));
    }

    FORCEINLINE void PutU32LE(TArray64<uint8>& Out, uint32 Value)
    {
        Out.Add(static_cast<uint8>(Value & 0xFF));
        Out.Add(static_cast<uint8>((Value >> 8) & 0xFF));
        Out.Add(static_cast<uint8>((Value >> 16) & 0xFF));
        Out.Add(static_cast<uint8>((Value >> 24) & 0xFF));
    }

    FORCEINLINE uint16 ReadU16LE(const uint8* Data)
    {
        return static_cast<uint16>(Data[0] | (static_cast<uint16>(Data[1]) << 8));
    }

    /** True for the FTextureSource formats that carry exactly one channel. */
    bool IsSingleChannelSourceFormat(ETextureSourceFormat Format)
    {
        return Format == TSF_G8
            || Format == TSF_G16
            || Format == TSF_R16F
            || Format == TSF_R32F;
    }
}

// ---------------------------------------------------------------------------
// ExtractSurface — mip 0 out of FTextureSource, normalised to 8-bit BGRA
// ---------------------------------------------------------------------------

bool FMapLinkTgaWriter::ExtractSurface(
    const UTexture2D*       Texture,
    bool                    bAllowBitDepthReduction,
    FMapLinkTextureSurface& OutSurface,
    FString&                OutError)
{
    OutSurface = FMapLinkTextureSurface();

    if (!Texture)
    {
        OutError = TEXT("Null texture.");
        return false;
    }

    // MapLinkEditor is an Editor-type module, so WITH_EDITORONLY_DATA is
    // always on here and FTextureSource is always present.
    //
    // The const_cast is deliberate and load-bearing: FTextureSource's mip
    // accessors are non-const because they may decompress and cache internally,
    // but reading mip 0 does not modify the texture in any sense the caller
    // cares about, and every call site legitimately holds a const pointer.
    FTextureSource& Source = const_cast<UTexture2D*>(Texture)->Source;

    if (!Source.IsValid())
    {
        OutError = FString::Printf(
            TEXT("'%s' has no editor source data. It cannot be rewritten from "
                 "FTextureSource — re-import it from its original file first."),
            *Texture->GetName());
        return false;
    }

    // Pixels ALWAYS come from the source, never from the on-disk file. That is
    // what makes this independent of whatever UE originally imported.
    FImage Image;
    if (!Source.GetMipImage(Image, /*MipIndex=*/0))
    {
        OutError = FString::Printf(
            TEXT("Could not read mip 0 of '%s' from FTextureSource."),
            *Texture->GetName());
        return false;
    }

    const int32 Width  = Image.SizeX;
    const int32 Height = Image.SizeY;
    if (Width <= 0 || Height <= 0)
    {
        OutError = FString::Printf(TEXT("'%s' mip 0 has a zero dimension (%dx%d)."),
                                   *Texture->GetName(), Width, Height);
        return false;
    }

    const int64 NumPixels = static_cast<int64>(Width) * static_cast<int64>(Height);
    const uint8* Raw = Image.RawData.GetData();
    const int64  RawNum = Image.RawData.Num();

    // Reject before allocating rather than after, so a bad format costs nothing.
    auto RequireBytes = [&](int64 BytesPerPixel) -> bool
    {
        if (RawNum < NumPixels * BytesPerPixel)
        {
            OutError = FString::Printf(
                TEXT("'%s' mip 0 is %lld bytes, short of the %lld its %dx%d "
                     "format needs."),
                *Texture->GetName(), RawNum, NumPixels * BytesPerPixel,
                Width, Height);
            return false;
        }
        return true;
    };

    const ETextureSourceFormat SourceFormat = Source.GetFormat();
    const bool bSingleChannel = IsSingleChannelSourceFormat(SourceFormat);

    // Formats above 8 bits per channel are accepted and reduced (§5.3), but
    // only when the project allows it — height maps terrace, and a studio may
    // prefer a hard skip and a report over a silent stair-step.
    const bool bNeedsReduction =
           Image.Format == ERawImageFormat::G16
        || Image.Format == ERawImageFormat::RGBA16
        || Image.Format == ERawImageFormat::RGBA16F
        || Image.Format == ERawImageFormat::RGBA32F
        || Image.Format == ERawImageFormat::R16F
        || Image.Format == ERawImageFormat::R32F;

    if (bNeedsReduction && !bAllowBitDepthReduction)
    {
        OutError = FString::Printf(
            TEXT("'%s' is above 8 bits per channel and bAllowBitDepthReduction "
                 "is off, so it was skipped rather than reduced."),
            *Texture->GetName());
        return false;
    }

    OutSurface.Width  = Width;
    OutSurface.Height = Height;
    OutSurface.bSingleChannelSource = bSingleChannel;
    OutSurface.bReducedBitDepth     = bNeedsReduction;
    OutSurface.BGRA.SetNumUninitialized(NumPixels * 4);
    uint8* Dst = OutSurface.BGRA.GetData();

    switch (Image.Format)
    {
    case ERawImageFormat::G8:
    {
        if (!RequireBytes(1)) return false;
        for (int64 i = 0; i < NumPixels; ++i)
        {
            const uint8 G = Raw[i];
            Dst[i * 4 + 0] = G; Dst[i * 4 + 1] = G;
            Dst[i * 4 + 2] = G; Dst[i * 4 + 3] = 255;
        }
        break;
    }
    case ERawImageFormat::G16:
    {
        if (!RequireBytes(2)) return false;
        const uint16* Src = reinterpret_cast<const uint16*>(Raw);
        for (int64 i = 0; i < NumPixels; ++i)
        {
            const uint8 G = Quantize16(Src[i]);
            Dst[i * 4 + 0] = G; Dst[i * 4 + 1] = G;
            Dst[i * 4 + 2] = G; Dst[i * 4 + 3] = 255;
        }
        break;
    }
    case ERawImageFormat::R16F:
    {
        if (!RequireBytes(2)) return false;
        const FFloat16* Src = reinterpret_cast<const FFloat16*>(Raw);
        for (int64 i = 0; i < NumPixels; ++i)
        {
            const uint8 G = QuantizeFloat(Src[i].GetFloat());
            Dst[i * 4 + 0] = G; Dst[i * 4 + 1] = G;
            Dst[i * 4 + 2] = G; Dst[i * 4 + 3] = 255;
        }
        break;
    }
    case ERawImageFormat::R32F:
    {
        if (!RequireBytes(4)) return false;
        const float* Src = reinterpret_cast<const float*>(Raw);
        for (int64 i = 0; i < NumPixels; ++i)
        {
            const uint8 G = QuantizeFloat(Src[i]);
            Dst[i * 4 + 0] = G; Dst[i * 4 + 1] = G;
            Dst[i * 4 + 2] = G; Dst[i * 4 + 3] = 255;
        }
        break;
    }
    case ERawImageFormat::BGRA8:
    {
        if (!RequireBytes(4)) return false;
        FMemory::Memcpy(Dst, Raw, NumPixels * 4);
        OutSurface.bHasAlphaChannel = true;
        break;
    }
    case ERawImageFormat::RGBA16:
    {
        if (!RequireBytes(8)) return false;
        const uint16* Src = reinterpret_cast<const uint16*>(Raw);
        for (int64 i = 0; i < NumPixels; ++i)
        {
            // Source is R,G,B,A — destination is B,G,R,A.
            Dst[i * 4 + 0] = Quantize16(Src[i * 4 + 2]);
            Dst[i * 4 + 1] = Quantize16(Src[i * 4 + 1]);
            Dst[i * 4 + 2] = Quantize16(Src[i * 4 + 0]);
            Dst[i * 4 + 3] = Quantize16(Src[i * 4 + 3]);
        }
        OutSurface.bHasAlphaChannel = true;
        break;
    }
    case ERawImageFormat::RGBA16F:
    {
        if (!RequireBytes(8)) return false;
        const FFloat16* Src = reinterpret_cast<const FFloat16*>(Raw);
        for (int64 i = 0; i < NumPixels; ++i)
        {
            Dst[i * 4 + 0] = QuantizeFloat(Src[i * 4 + 2].GetFloat());
            Dst[i * 4 + 1] = QuantizeFloat(Src[i * 4 + 1].GetFloat());
            Dst[i * 4 + 2] = QuantizeFloat(Src[i * 4 + 0].GetFloat());
            Dst[i * 4 + 3] = QuantizeFloat(Src[i * 4 + 3].GetFloat());
        }
        OutSurface.bHasAlphaChannel = true;
        break;
    }
    case ERawImageFormat::RGBA32F:
    {
        if (!RequireBytes(16)) return false;
        const float* Src = reinterpret_cast<const float*>(Raw);
        for (int64 i = 0; i < NumPixels; ++i)
        {
            Dst[i * 4 + 0] = QuantizeFloat(Src[i * 4 + 2]);
            Dst[i * 4 + 1] = QuantizeFloat(Src[i * 4 + 1]);
            Dst[i * 4 + 2] = QuantizeFloat(Src[i * 4 + 0]);
            Dst[i * 4 + 3] = QuantizeFloat(Src[i * 4 + 3]);
        }
        OutSurface.bHasAlphaChannel = true;
        break;
    }
    default:
        // BGRE8 (shared-exponent HDR) and anything a future engine adds. We
        // refuse rather than guess: a wrong guess here silently corrupts pixel
        // values, which is the one thing conformance must never do.
        OutError = FString::Printf(
            TEXT("'%s' has an unsupported source format (%d). MapLink refuses "
                 "to guess at it rather than risk changing pixel values."),
            *Texture->GetName(), static_cast<int32>(Image.Format));
        OutSurface = FMapLinkTextureSurface();
        return false;
    }

    // ── Analysis passes ───────────────────────────────────────────────────

    if (OutSurface.bHasAlphaChannel)
    {
        const uint8 First = Dst[3];
        OutSurface.bAlphaUniform = true;
        for (int64 i = 1; i < NumPixels; ++i)
        {
            if (Dst[i * 4 + 3] != First) { OutSurface.bAlphaUniform = false; break; }
        }
    }

    // Grayscale CANDIDATE detection — reported, never acted on. Converting
    // would change what CompressionSettings ought to be, and that is a
    // material decision the tool has no business making.
    OutSurface.bChannelsIdentical = true;
    for (int64 i = 0; i < NumPixels; ++i)
    {
        const uint8 B = Dst[i * 4 + 0];
        if (B != Dst[i * 4 + 1] || B != Dst[i * 4 + 2])
        {
            OutSurface.bChannelsIdentical = false;
            break;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Byte assembly
// ---------------------------------------------------------------------------

void FMapLinkTgaWriter::AppendHeader(TArray64<uint8>& Out,
                                     int32 Width, int32 Height, bool bGrayscale)
{
    Out.Add(MapLinkTgaSpec::IdLength);        // 0  — no image ID field
    Out.Add(MapLinkTgaSpec::ColourMapType);   // 1  — no colour map
    Out.Add(bGrayscale ? MapLinkTgaSpec::ImageTypeGray
                       : MapLinkTgaSpec::ImageTypeRGB);   // 2

    // 3..7 — colour-map spec (first entry, length, entry size), all zero.
    PutU16LE(Out, 0);
    PutU16LE(Out, 0);
    Out.Add(0);

    // 8..11 — x/y origin, both zero.
    PutU16LE(Out, 0);
    PutU16LE(Out, 0);

    // 12..15 — dimensions.
    PutU16LE(Out, static_cast<uint16>(Width));
    PutU16LE(Out, static_cast<uint16>(Height));

    // 16..17 — pixel depth and descriptor.
    Out.Add(bGrayscale ? MapLinkTgaSpec::PixelDepthGray
                       : MapLinkTgaSpec::PixelDepthRGB);
    Out.Add(bGrayscale ? MapLinkTgaSpec::DescriptorGray
                       : MapLinkTgaSpec::DescriptorRGB);

    check(Out.Num() >= MapLinkTgaSpec::HeaderBytes);
}

void FMapLinkTgaWriter::AppendFooter(TArray64<uint8>& Out)
{
    PutU32LE(Out, MapLinkTgaSpec::ExtensionAreaOffset);
    PutU32LE(Out, MapLinkTgaSpec::DeveloperDirOffset);
    for (int32 i = 0; i < 16; ++i)
    {
        Out.Add(static_cast<uint8>(MapLinkTgaSpec::Signature[i]));
    }
    Out.Add(MapLinkTgaSpec::SignatureDot);
    Out.Add(MapLinkTgaSpec::SignatureTerminator);
}

void FMapLinkTgaWriter::BuildFileBytes(const FMapLinkTextureSurface& Surface,
                                       bool bForceGrayscale,
                                       TArray64<uint8>& Out)
{
    Out.Reset();
    if (!Surface.IsValid())
    {
        return;
    }

    const int64 BytesPerPixel = bForceGrayscale ? 1 : 3;
    Out.Reserve(MapLinkTgaSpec::HeaderBytes
                + Surface.NumPixels() * BytesPerPixel
                + MapLinkTgaSpec::FooterBytes);

    AppendHeader(Out, Surface.Width, Surface.Height, bForceGrayscale);

    // Descriptor origin bits are 00 = BOTTOM-left, so the first row in the file
    // is the BOTTOM row of the image. FTextureSource is top-down. Rows go out
    // in reverse — get this wrong and every texture is flipped with nothing to
    // warn you, because a flipped texture is still a valid TGA.
    const uint8* Src = Surface.BGRA.GetData();
    for (int32 y = Surface.Height - 1; y >= 0; --y)
    {
        const uint8* Row = Src + static_cast<int64>(y) * Surface.Width * 4;
        if (bForceGrayscale)
        {
            for (int32 x = 0; x < Surface.Width; ++x)
            {
                // R channel. For a single-channel source B == G == R, so the
                // choice is arbitrary; for a TC_Grayscale BGRA source, R is
                // UE's convention.
                Out.Add(Row[x * 4 + 2]);
            }
        }
        else
        {
            // 24-bit TGA is B,G,R — the same order the surface already holds,
            // minus alpha. Verified against the exemplar: file bytes 47 94 FF
            // decode as RGB(255,148,71).
            for (int32 x = 0; x < Surface.Width; ++x)
            {
                Out.Add(Row[x * 4 + 0]);
                Out.Add(Row[x * 4 + 1]);
                Out.Add(Row[x * 4 + 2]);
            }
        }
    }

    AppendFooter(Out);
}

// ---------------------------------------------------------------------------
// Disk
// ---------------------------------------------------------------------------

FMapLinkTgaWriteResult FMapLinkTgaWriter::WriteSurface(
    const FMapLinkTextureSurface& Surface,
    const FString&                AbsolutePath,
    bool                          bForceGrayscale)
{
    FMapLinkTgaWriteResult Result;
    Result.bWroteGrayscale      = bForceGrayscale;
    Result.bReducedBitDepth     = Surface.bReducedBitDepth;
    Result.bGrayscaleCandidate  = !bForceGrayscale && Surface.bChannelsIdentical;
    Result.bHasNonUniformAlpha  = Surface.bHasAlphaChannel && !Surface.bAlphaUniform;
    Result.bDroppedUniformAlpha = Surface.bHasAlphaChannel && Surface.bAlphaUniform;
    Result.bDiscardedColourChannels =
        bForceGrayscale && !Surface.bSingleChannelSource && !Surface.bChannelsIdentical;

    if (!Surface.IsValid())
    {
        Result.Error = TEXT("Empty surface.");
        return Result;
    }

    TArray64<uint8> Bytes;
    BuildFileBytes(Surface, bForceGrayscale, Bytes);

    const int64 Expected = MapLinkTgaSpec::ExpectedFileSize(
        Surface.Width, Surface.Height, bForceGrayscale);
    if (Bytes.Num() != Expected)
    {
        Result.Error = FString::Printf(
            TEXT("Assembled %lld bytes but the spec accounts for %lld — refusing "
                 "to write."), static_cast<int64>(Bytes.Num()), Expected);
        return Result;
    }

    IFileManager& FM = IFileManager::Get();

    // ── Idempotence, as a byte compare rather than a heuristic ────────────
    //
    // This is the acceptance test expressed as code: an SD-exported TGA that is
    // already correctly located and named assembles to exactly the same bytes,
    // so nothing is written, and a second full run over a conformed library
    // performs zero writes. A header-inspection shortcut would be faster and
    // would eventually be wrong.
    if (FM.FileSize(*AbsolutePath) == Bytes.Num())
    {
        TArray64<uint8> Existing;
        if (FFileHelper::LoadFileToArray(Existing, *AbsolutePath)
            && Existing.Num() == Bytes.Num()
            && FMemory::Memcmp(Existing.GetData(), Bytes.GetData(), Bytes.Num()) == 0)
        {
            Result.bSuccess          = true;
            Result.bSkippedIdentical = true;
            Result.BytesWritten      = 0;
            return Result;
        }
    }

    // ── Temp-then-move, so a failure never destroys a valid texture ───────
    const FString Dir = FPaths::GetPath(AbsolutePath);
    FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*Dir);

    const FString TempPath = AbsolutePath + TEXT(".maplinktmp");
    FM.Delete(*TempPath, /*RequireExists=*/false, /*EvenReadOnly=*/true);

    if (!FFileHelper::SaveArrayToFile(Bytes, *TempPath))
    {
        Result.Error = FString::Printf(TEXT("Could not write temp file '%s'."),
                                       *TempPath);
        return Result;
    }

    // Move over the destination. The caller is responsible for having cleared
    // the read-only bit through source control first (CheckoutBeforeSave /
    // EnsureFileInChangelist); we clear it defensively so a stale local
    // read-only flag is not mistaken for a P4 lock.
    if (FM.FileExists(*AbsolutePath))
    {
        FPlatformFileManager::Get().GetPlatformFile()
            .SetReadOnly(*AbsolutePath, false);
    }

    if (!FM.Move(*AbsolutePath, *TempPath,
                 /*bReplace=*/true, /*bEvenIfReadOnly=*/true,
                 /*bAttributes=*/false, /*bDoNotRetryOrError=*/false))
    {
        FM.Delete(*TempPath, false, true);
        Result.Error = FString::Printf(
            TEXT("Could not move '%s' over '%s' — is it checked out?"),
            *TempPath, *AbsolutePath);
        return Result;
    }

    Result.bSuccess     = true;
    Result.BytesWritten = Bytes.Num();
    return Result;
}

FMapLinkTgaWriteResult FMapLinkTgaWriter::WriteFromTexture(
    const UTexture2D* Texture,
    const FString&    AbsolutePath,
    bool              bAllowBitDepthReduction)
{
    FMapLinkTgaWriteResult Result;

    FMapLinkTextureSurface Surface;
    FString Error;
    if (!ExtractSurface(Texture, bAllowBitDepthReduction, Surface, Error))
    {
        Result.Error = Error;
        return Result;
    }

    // §5.1 — grayscale when the source is single-channel, or when the asset's
    // CompressionSettings declares it. Otherwise RGB, even if the channels
    // happen to be identical (that is reported as a candidate, not acted on).
    bool bGrayscale = Surface.bSingleChannelSource;
    if (Texture)
    {
        const TextureCompressionSettings Compression = Texture->CompressionSettings;
        if (Compression == TC_Grayscale || Compression == TC_Alpha)
        {
            bGrayscale = true;
        }
    }

    Result = WriteSurface(Surface, AbsolutePath, bGrayscale);

    if (Result.bDiscardedColourChannels)
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("[Textures] '%s' is set to a grayscale CompressionSettings "
                    "but its source channels differ. Wrote 8-bit grayscale from "
                    "the R channel per the container rule; G and B were "
                    "discarded. Fix the asset's compression setting if that is "
                    "not what you meant."),
               Texture ? *Texture->GetName() : TEXT("<null>"));
    }
    if (Result.bGrayscaleCandidate)
    {
        UE_LOG(LogMapLink, Log,
               TEXT("[Textures] '%s' is RGB but all three channels are "
                    "pixel-identical — a grayscale candidate. Not converted: "
                    "that would change what CompressionSettings should be, "
                    "which is a material decision."),
               Texture ? *Texture->GetName() : TEXT("<null>"));
    }

    return Result;
}

bool FMapLinkTgaWriter::ExtractAlphaSurface(const FMapLinkTextureSurface& Source,
                                            FMapLinkTextureSurface& OutAlpha)
{
    if (!Source.IsValid() || !Source.bHasAlphaChannel)
    {
        return false;
    }

    OutAlpha = FMapLinkTextureSurface();
    OutAlpha.Width  = Source.Width;
    OutAlpha.Height = Source.Height;
    OutAlpha.bSingleChannelSource = true;
    OutAlpha.bReducedBitDepth     = Source.bReducedBitDepth;
    OutAlpha.bChannelsIdentical   = true;

    const int64 NumPixels = Source.NumPixels();
    OutAlpha.BGRA.SetNumUninitialized(NumPixels * 4);

    const uint8* Src = Source.BGRA.GetData();
    uint8*       Dst = OutAlpha.BGRA.GetData();
    for (int64 i = 0; i < NumPixels; ++i)
    {
        const uint8 A = Src[i * 4 + 3];
        Dst[i * 4 + 0] = A; Dst[i * 4 + 1] = A;
        Dst[i * 4 + 2] = A; Dst[i * 4 + 3] = 255;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Inspection
// ---------------------------------------------------------------------------

bool FMapLinkTgaWriter::ReadHeader(const FString& AbsolutePath,
                                   FMapLinkTgaHeaderInfo& Out)
{
    Out = FMapLinkTgaHeaderInfo();

    IFileManager& FM = IFileManager::Get();
    const int64 Size = FM.FileSize(*AbsolutePath);
    if (Size < MapLinkTgaSpec::HeaderBytes + MapLinkTgaSpec::FooterBytes)
    {
        return false;
    }
    Out.FileSize = Size;

    TArray64<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *AbsolutePath))
    {
        return false;
    }
    if (Bytes.Num() < MapLinkTgaSpec::HeaderBytes + MapLinkTgaSpec::FooterBytes)
    {
        return false;
    }

    const uint8* H = Bytes.GetData();
    Out.IdLength      = H[0];
    Out.ColourMapType = H[1];
    Out.ImageType     = H[2];
    Out.Width         = ReadU16LE(H + 12);
    Out.Height        = ReadU16LE(H + 14);
    Out.PixelDepth    = H[16];
    Out.Descriptor    = H[17];

    const uint8* Foot = Bytes.GetData() + Bytes.Num() - MapLinkTgaSpec::FooterBytes;
    Out.bHasFooter = FMemory::Memcmp(Foot + 8, MapLinkTgaSpec::Signature, 16) == 0;

    return true;
}

bool FMapLinkTgaWriter::MatchesLockedSpec(const FMapLinkTgaHeaderInfo& Info,
                                          FString& OutReason)
{
    const bool bGray = Info.IsGrayscale();
    const bool bRGB  = Info.IsRGB24();

    if (!bGray && !bRGB)
    {
        OutReason = FString::Printf(
            TEXT("image type %d at %d bpp — MapLink textures are RGB 24-bit "
                 "(type 2) or grayscale 8-bit (type 3), never RGBA"),
            Info.ImageType, Info.PixelDepth);
        return false;
    }
    if (Info.IdLength != MapLinkTgaSpec::IdLength)
    {
        OutReason = FString::Printf(TEXT("carries a %d-byte image ID field"),
                                    Info.IdLength);
        return false;
    }
    if (Info.ColourMapType != MapLinkTgaSpec::ColourMapType)
    {
        OutReason = TEXT("declares a colour map");
        return false;
    }

    const uint8 ExpectedDescriptor = bGray ? MapLinkTgaSpec::DescriptorGray
                                           : MapLinkTgaSpec::DescriptorRGB;
    if (Info.Descriptor != ExpectedDescriptor)
    {
        OutReason = FString::Printf(
            TEXT("descriptor byte 0x%02X, expected 0x%02X (alpha bits and "
                 "bottom-left origin must match the exemplar)"),
            Info.Descriptor, ExpectedDescriptor);
        return false;
    }
    if (!Info.bHasFooter)
    {
        OutReason = TEXT("has no TGA 2.0 footer");
        return false;
    }

    const int64 Expected = MapLinkTgaSpec::ExpectedFileSize(
        Info.Width, Info.Height, bGray);
    if (Info.FileSize != Expected)
    {
        OutReason = FString::Printf(
            TEXT("is %lld bytes, but %dx%d %s accounts for exactly %lld "
                 "(RLE or an extension area?)"),
            Info.FileSize, Info.Width, Info.Height,
            bGray ? TEXT("grayscale") : TEXT("RGB"), Expected);
        return false;
    }

    OutReason.Reset();
    return true;
}

bool FMapLinkTgaWriter::IsConformContainer(const FString& AbsolutePath,
                                           FString& OutReason)
{
    FMapLinkTgaHeaderInfo Info;
    if (!ReadHeader(AbsolutePath, Info))
    {
        OutReason = TEXT("is missing, truncated, or not a TGA at all");
        return false;
    }
    return MatchesLockedSpec(Info, OutReason);
}

// ---------------------------------------------------------------------------
// Self-test — synthetic 4x4 round-trip
// ---------------------------------------------------------------------------

bool FMapLinkTgaWriter::RunSelfTest()
{
    bool bOk = true;
    auto Expect = [&bOk](bool bCondition, const TCHAR* What)
    {
        if (!bCondition)
        {
            UE_LOG(LogMapLink, Error, TEXT("[Textures] TGA self-test FAILED: %s"), What);
            bOk = false;
        }
    };

    // ── RGB 4x4 ───────────────────────────────────────────────────────────
    {
        FMapLinkTextureSurface S;
        S.Width = 4; S.Height = 4;
        S.BGRA.SetNumZeroed(4 * 4 * 4);
        // Distinct value per pixel so a transposition or a flip cannot pass by
        // accident: B = x*16, G = y*16, R = 200.
        for (int32 y = 0; y < 4; ++y)
        {
            for (int32 x = 0; x < 4; ++x)
            {
                const int64 i = (static_cast<int64>(y) * 4 + x) * 4;
                S.BGRA[i + 0] = static_cast<uint8>(x * 16);
                S.BGRA[i + 1] = static_cast<uint8>(y * 16);
                S.BGRA[i + 2] = 200;
                S.BGRA[i + 3] = 255;
            }
        }

        TArray64<uint8> Bytes;
        BuildFileBytes(S, /*bForceGrayscale=*/false, Bytes);

        Expect(Bytes.Num() == 18 + 4 * 4 * 3 + 26, TEXT("RGB size accounting"));
        Expect(Bytes[0] == 0,    TEXT("RGB id length is 0"));
        Expect(Bytes[1] == 0,    TEXT("RGB colour-map type is 0"));
        Expect(Bytes[2] == 2,    TEXT("RGB image type is 2"));
        Expect(Bytes[16] == 24,  TEXT("RGB pixel depth is 24"));
        Expect(Bytes[17] == 0x00, TEXT("RGB descriptor is 0x00"));
        Expect(ReadU16LE(Bytes.GetData() + 12) == 4, TEXT("RGB width"));
        Expect(ReadU16LE(Bytes.GetData() + 14) == 4, TEXT("RGB height"));

        // First payload pixel must be the BOTTOM-left of the image (y = 3),
        // written B,G,R.
        const uint8* P = Bytes.GetData() + 18;
        Expect(P[0] == 0,   TEXT("bottom-up row order: first pixel B (x=0)"));
        Expect(P[1] == 48,  TEXT("bottom-up row order: first pixel G (y=3)"));
        Expect(P[2] == 200, TEXT("BGR channel order: first pixel R"));

        // Last payload pixel must be the TOP-right (x = 3, y = 0).
        const uint8* Q = Bytes.GetData() + Bytes.Num() - 26 - 3;
        Expect(Q[0] == 48, TEXT("bottom-up row order: last pixel B (x=3)"));
        Expect(Q[1] == 0,  TEXT("bottom-up row order: last pixel G (y=0)"));

        const uint8* F = Bytes.GetData() + Bytes.Num() - 26;
        Expect(F[0] == 0 && F[1] == 0 && F[2] == 0 && F[3] == 0,
               TEXT("footer extension-area offset is 0"));
        Expect(F[4] == 0 && F[5] == 0 && F[6] == 0 && F[7] == 0,
               TEXT("footer developer-dir offset is 0"));
        Expect(FMemory::Memcmp(F + 8, MapLinkTgaSpec::Signature, 16) == 0,
               TEXT("footer signature"));
        Expect(F[24] == 0x2E && F[25] == 0x00, TEXT("footer terminator"));
    }

    // ── Grayscale 4x4 ─────────────────────────────────────────────────────
    {
        FMapLinkTextureSurface S;
        S.Width = 4; S.Height = 4;
        S.bSingleChannelSource = true;
        S.bChannelsIdentical   = true;
        S.BGRA.SetNumZeroed(4 * 4 * 4);
        for (int32 y = 0; y < 4; ++y)
        {
            for (int32 x = 0; x < 4; ++x)
            {
                const int64 i = (static_cast<int64>(y) * 4 + x) * 4;
                const uint8 V = static_cast<uint8>(y * 64 + x * 16);
                S.BGRA[i + 0] = V; S.BGRA[i + 1] = V;
                S.BGRA[i + 2] = V; S.BGRA[i + 3] = 255;
            }
        }

        TArray64<uint8> Bytes;
        BuildFileBytes(S, /*bForceGrayscale=*/true, Bytes);

        Expect(Bytes.Num() == 18 + 4 * 4 + 26, TEXT("grayscale size accounting"));
        Expect(Bytes[2] == 3,     TEXT("grayscale image type is 3"));
        Expect(Bytes[16] == 8,    TEXT("grayscale pixel depth is 8"));
        Expect(Bytes[17] == 0x08, TEXT("grayscale descriptor is 0x08 "
                                       "(what Substance and UE both write)"));
        Expect(Bytes[18] == 192,  TEXT("grayscale bottom-up first pixel (y=3,x=0)"));
    }

    // ── Quantisation ──────────────────────────────────────────────────────
    Expect(Quantize16(65535) == 255, TEXT("16-bit white maps to 255"));
    Expect(Quantize16(0) == 0,       TEXT("16-bit black maps to 0"));
    Expect(Quantize16(32768) == 128, TEXT("16-bit mid maps to 128, no drift"));

    UE_LOG(LogMapLink, Log, TEXT("[Textures] TGA writer self-test: %s"),
           bOk ? TEXT("PASS") : TEXT("FAIL"));
    return bOk;
}
