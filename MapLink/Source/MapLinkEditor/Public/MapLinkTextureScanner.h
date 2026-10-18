// Copyright MapModelers. All Rights Reserved.
//
// v1.16 — The texture scan.
//
// Fast enough that a Textures view opens warm on several thousand textures,
// which dictates the shape:
//
//   1. AssetRegistry enumeration only, no package loads. Produces the rows.
//   2. Rows resolve their class from texture_policy.json immediately.
//   3. A precise pass loads packages ONLY where the cache is missing or the
//      package mtime moved. Staleness is FTextureSource::GetId() — a GUID the
//      engine already maintains — never a pixel hash.
//   4. Disk checks are size + mtime first; MD5 only on a mismatch (and
//      FMapLinkUtils::ComputeFileMD5 is itself cached on (path, mtime, size),
//      so a repeat call on an unchanged file is a map lookup, not a read).
//   5. A manual rescan forces a full recompute.
//
// Reserved and IGNORED by every pass:
//     <Root>/TexturePreviews/External/   (Phase 2 populates)
//     <Root>/TexturePreviews/MapLink/    (reserved, empty in v1.16)

#pragma once

#include "CoreMinimal.h"
#include "MapLinkTextureTypes.h"

struct FAssetData;
class UTexture2D;

/** One complete scan. Rebuilt wholesale; never patched in place, because a
 *  partially-updated view is worse than a slightly stale one. */
struct FMapLinkTextureScanResult
{
    TArray<FMapLinkTextureRow> Rows;

    /** MI asset path -> the worst news about its textures. What the Materials
     *  tab reads; also what a future export gate would read. */
    TMap<FString, FMapLinkTextureAggregate> MaterialAggregates;

    /** Texture asset path -> index into Rows. */
    TMap<FString, int32> RowIndexByAssetPath;

    /** Assets that were referenced but are not UTexture2D — cubes, arrays,
     *  render targets, virtual textures. Counted rather than dropped, so
     *  "62 of 71" never looks like a bug. */
    TArray<FString> SkippedUnsupported;

    int32 NumConform      = 0;
    /** MapLink's own shipped plugin textures. Counted so "248 textures" adds
     *  up, and excluded from every "needs work" number. */
    int32 NumPlugin       = 0;
    int32 NumPortable     = 0;
    int32 NumLocalOnly    = 0;
    int32 NumUnclassified = 0;
    int32 NumBroken       = 0;
    int32 NumOrphans      = 0;

    /** Packages actually loaded this pass. Zero on a fully warm rescan is the
     *  number that says the cache is doing its job. */
    int32 NumPackagesLoaded = 0;

    /** Rows the fast pass proposed and the precise pass disowned: a package
     *  still names the texture, but no MapLink material uses it through a
     *  parameter. Dropped from Rows — reported so the drop is never silent. */
    int32 NumReleased = 0;

    double    ScanSeconds = 0.0;
    FDateTime ScannedAtUtc;

    int32 Total() const { return Rows.Num(); }

    bool IsClean() const
    {
        return NumBroken == 0 && NumLocalOnly == 0 && NumUnclassified == 0;
    }
};

class MAPLINKEDITOR_API FMapLinkTextureScanner
{
public:
    /** Run a scan and cache the result. bForceFullRecompute ignores every
     *  cached row and reloads everything. */
    static const FMapLinkTextureScanResult& Scan(bool bForceFullRecompute = false);

    /** Clear the per-user cache and scan from scratch. */
    static const FMapLinkTextureScanResult& Rescan();

    /** The last result, empty if nothing has been scanned this session. Does
     *  NOT scan — callers that need data call Scan(). */
    static const FMapLinkTextureScanResult& GetLastResult();

    static bool HasResult();

    /** Null when this texture was not in the last scan. */
    static const FMapLinkTextureRow* FindRow(const FString& TextureAssetPath);

    /**
     * What the auto-suggester would propose for an asset, per §6.4.
     *   under /Game/MapLink/                          -> MapLink
     *   under /Engine/, /Megascans/, any plugin mount  -> External
     *   everything else                                -> Unclassified
     *
     * PRE-FILLED, NEVER AUTO-APPLIED. The user accepts, and the acceptance is
     * what lands in the P4-tracked policy file — that file is an audit trail of
     * decisions people made, and a suggestion is not a decision.
     */
    static EMapLinkTextureClass SuggestClass(const FString& AssetPath);

    /**
     * The class an operation should act on: the recorded decision when there
     * is one, otherwise the folder-derived suggestion.
     *
     * v1.16 — "Unclassified" used to be a state the user had to clear by hand
     * before anything would run. That was a decision the tool was making the
     * user take on its behalf: the answer is derivable from where the asset
     * lives, it is deterministic, and guessing wrong is harmless because
     * External is the read-only class. Deriving it here means the state never
     * reaches the UI and no button is needed to clear it.
     *
     * Nothing is written. texture_policy.json records deviations and conform
     * history; it is not where the obvious answer gets stored.
     */
    static EMapLinkTextureClass EffectiveClass(const FString& AssetPath);

    /** Apply every suggestion to the policy store. Only ever called from an
     *  explicit AutoClassify -execute; returns how many entries changed. */
    static int32 ApplySuggestions(bool bExecute, TArray<FString>& OutChanged);

    /** One material's aggregate from the last scan. An all-zero aggregate means
     *  the material references no textures, which reads as clean. */
    static FMapLinkTextureAggregate GetMaterialAggregate(const FString& MaterialAssetPath);

    /** Human-readable multi-line report of the last scan, for the console
     *  command and the clipboard. */
    static FString BuildReport(const FMapLinkTextureScanResult& Result,
                               bool bVerbose);

private:
    static void EnumerateFast(FMapLinkTextureScanResult& Out,
                              TMap<FString, TArray<FString>>& OutTextureToMaterials);

    static void ResolvePrecise(FMapLinkTextureScanResult& InOut,
                               bool bForceFullRecompute);

    static void EvaluateRow(FMapLinkTextureRow& Row);

    /** Drop rows the precise pass proved nothing uses. See the definition. */
    static void DropReleasedRows(FMapLinkTextureScanResult& InOut);

    static void BuildAggregates(FMapLinkTextureScanResult& InOut);
};
