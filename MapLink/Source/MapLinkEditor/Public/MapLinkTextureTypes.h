// Copyright MapModelers. All Rights Reserved.
//
// v1.16 — Texture Manager shared vocabulary.
//
// Three enums, deliberately kept apart, because collapsing any two of them is
// what makes this kind of system rot:
//
//   EMapLinkTextureClass   a DECISION the user records and Perforce carries.
//   EMapLinkTextureStatus  a COMPUTED state, recomputed by every scan, shown
//                          in the UI. Never persisted as truth.
//   EMapLinkTextureResult  what ONE operation did to ONE texture in one run.
//
// A value like "already conform" has no business in a filter dropdown, and
// "LocalOnly" has no business in a run summary.
//
// RESERVED WORD: "Proxy" belongs to UMapLinkProxyAsset and the WP expansion
// system. Nothing in the texture feature spells it — not bare, not compounded.
// The Blender-facing derivative PNGs are PREVIEWS.

#pragma once

#include "CoreMinimal.h"
#include "Engine/TextureDefines.h"
#include "MapLinkTextureTypes.generated.h"

// ---------------------------------------------------------------------------
// Class — the recorded decision
// ---------------------------------------------------------------------------

UENUM()
enum class EMapLinkTextureClass : uint8
{
    /** No decision recorded in texture_policy.json. Never conform. */
    Unclassified,

    /** MapLink owns it: /Game/MapLink/Textures/T_Mk_*, backed by
     *  <Root>/Textures/T_Mk_*.tga, and MapLink may rewrite that file. */
    MapLink,

    /** Third-party, accepted as-is. Megascans, /Engine/, other plugin mounts,
     *  or simply a texture the team chose not to bring in. MapLink reads it and
     *  records its sRGB / compression settings for export.
     *  MapLink NEVER writes an External original, under any operation. */
    External,
};

// ---------------------------------------------------------------------------
// Status — the computed state
// ---------------------------------------------------------------------------

UENUM()
enum class EMapLinkTextureStatus : uint8
{
    /** No class decision recorded yet. */
    Unclassified,

    /** MapLink class, correct folder, correct name, correct container, and the
     *  cached FTextureSource::GetId() still matches. Nothing to do. */
    Conform,

    /** The texture is MapLink's OWN shipped content, on the plugin's /MapLink/
     *  mount.  It arrives with the plugin, is identical in every project that
     *  installed the same version, and needs no help from the host project to
     *  resolve -- so "is its source file inside this project" is not a question
     *  that applies to it.
     *
     *  It is a separate status and not merely "Portable" because the ANSWER is
     *  different: conforming a Portable texture is a reasonable thing to want,
     *  and conforming a Plugin one is never correct.  Doing it would copy
     *  MapLink's own debug textures into <Root>/Textures under T_Mk_ names and
     *  repoint MapLink's own shipped material instances at the copies -- the
     *  plugin editing itself, differently, in every customer project.  See
     *  `finding-plugin-content-portability`: /Game/ references inside plugin
     *  content are the bug this is the mirror image of.
     *
     *  Nothing to do, ever.  Excluded from Conform All and refused by Conform. */
    Plugin,

    /** Not conform, but its source file resolves for the whole team — it lives
     *  inside the synced tree, so a path relative to the material library
     *  reaches it on every machine. A legitimate resting place, not a warning.
     *  Conform it when you want the container and the ownership too. */
    Portable,

    /** The source file is outside the synced tree, so no relative path can
     *  express it. Works on the machine that ran the sync and nowhere else.
     *  Conforming (copying the pixels in) is the ONLY fix. */
    LocalOnly,

    /** No source data, the file is gone, or the resulting path would exceed the
     *  length limit. Cannot be exported or conformed as-is. */
    Broken,
};

// ---------------------------------------------------------------------------
// Result — what one operation did to one texture
// ---------------------------------------------------------------------------

UENUM()
enum class EMapLinkTextureResult : uint8
{
    Conformed,
    SkippedAlreadyConform,
    SkippedExternal,
    /** Refused because the asset lives on MapLink's own plugin mount. */
    SkippedPluginContent,
    /** The run was cancelled before this one was reached. Nothing was done to
     *  it, and conform is idempotent, so re-running picks it up. */
    SkippedCancelled,
    SkippedUnclassified,
    SkippedNoSourceData,
    SkippedUnsupportedType,
    ReducedLossy,
    AlphaSplit,
    Orphan,
    Failed,
};

// ---------------------------------------------------------------------------
// Claim presets — what happens to the ORIGINAL when MapLink takes a texture
// ---------------------------------------------------------------------------
//
// The operation is Claim, not Adopt: Adopt is one of its presets, and
// "MapLink.Textures.Adopt <path> Adopt" was a name colliding with itself.
//
// HARD INVARIANT: deleting the original uasset requires repointing EVERY
// referencer. Migrate and Adopt enumerate referencers through the AssetRegistry
// and repoint all of them before deleting. If any referencer cannot be
// repointed, that texture's whole operation aborts. Never a dangling reference.

UENUM()
enum class EMapLinkTextureClaimPreset : uint8
{
    /** Keep the original file, keep the original uasset, repoint MapLink-owned
     *  MIs only. The default: MapLink owns its copies, the user's library stays
     *  untouched, and editing a MapLink texture affects nothing else. */
    Fork,

    /** Delete the original file AND the original uasset, repoint every
     *  referencer. Destructive — dry run + modal confirm. */
    Migrate,

    /** Keep the original file, delete the original uasset, repoint every
     *  referencer. Destructive — dry run + modal confirm. */
    Adopt,

    /** Keep everything, repoint nothing. A pure copy into MapLink. */
    Copy,
};

// ---------------------------------------------------------------------------
// Policy entry — one row of texture_policy.json (P4-tracked, project data)
// ---------------------------------------------------------------------------

USTRUCT()
struct FMapLinkTexturePolicyEntry
{
    GENERATED_BODY()

    /** UE asset path, e.g. "/Game/MapLink/Textures/T_Mk_Wall_D". Key. */
    UPROPERTY()
    FString AssetPath;

    UPROPERTY()
    EMapLinkTextureClass TextureClass = EMapLinkTextureClass::Unclassified;

    /** As read from the asset. Recorded, never changed — conformance governs
     *  container, location and name, never pixel values. */
    UPROPERTY()
    bool bSRGB = false;

    /** TextureCompressionSettings as its enum name, e.g. "TC_Normalmap". */
    UPROPERTY()
    FString Compression;

    /** Optional per-texture cap override for the Phase 2 preview PNG.
     *  0 = use the project default. Phase 1 stores and validates it. */
    UPROPERTY()
    int32 PreviewMaxRes = 0;

    /** Free-text, survives every rewrite of the file. */
    UPROPERTY()
    FString Note;

    /** ISO-8601 UTC timestamp of the last conform, plus the operation name. */
    UPROPERTY()
    FString LastConform;

    /** Set when a source above 8 bits per channel was reduced on the way to
     *  the TGA. Height maps terrace; this is how you find out why. */
    UPROPERTY()
    bool bLossyReduction = false;
};

// ---------------------------------------------------------------------------
// Scan row — the in-memory view. Never serialised to the policy file.
// ---------------------------------------------------------------------------

struct FMapLinkTextureRow
{
    /** UE asset path (object path without the class prefix). */
    FString AssetPath;

    /** Leaf asset name, e.g. "T_Mk_Wall_D". */
    FString AssetName;

    /** Package path, e.g. "/Game/MapLink/Textures". */
    FString PackagePath;

    /** AssetImportData->GetFirstFilename(), converted to a full path.
     *  Empty when the asset carries no import record. */
    FString SourceDiskPath;

    /** Path relative to the material library .blend, forward slashes, no "//"
     *  prefix. Empty when no portable form exists — that is exactly what
     *  Status == LocalOnly means. */
    FString RelativeToLibrary;

    EMapLinkTextureClass  TextureClass = EMapLinkTextureClass::Unclassified;
    EMapLinkTextureStatus Status       = EMapLinkTextureStatus::Unclassified;

    /** The class the auto-suggester would propose. Pre-filled in the UI, never
     *  applied without the user accepting — acceptance is what lands in the
     *  P4-tracked policy file. */
    EMapLinkTextureClass SuggestedClass = EMapLinkTextureClass::Unclassified;

    bool bSRGB = false;
    FString Compression;

    /** Why this row is not Conform, in one line, phrased as what to do about
     *  it. Empty when Status == Conform. */
    FString Remedy;

    /** Every failed check, for the report and the manifest. */
    TArray<FString> Issues;

    /** MI asset paths that reference this texture. THE reverse index: one bad
     *  texture on a shared parent shows up as fifteen bad materials, and this
     *  is what turns that back into one fix. */
    TArray<FString> ReferencingMaterials;

    /** True when the asset lives under /Game/MapLink/Textures/ but no
     *  MapLink-owned MI references it. Non-blocking status; never auto-deleted. */
    bool bOrphan = false;

    /** FTextureSource::GetId() at scan time. Invalid when the package was not
     *  loaded on this pass (AssetRegistry-only rows). */
    FGuid SourceId;

    bool IsConform() const { return Status == EMapLinkTextureStatus::Conform; }

    /** Everything a teammate can still open. Portable is a resting place. */
    bool ResolvesForTeam() const
    {
        return Status == EMapLinkTextureStatus::Conform
            || Status == EMapLinkTextureStatus::Portable;
    }
};

// ---------------------------------------------------------------------------
// Aggregate — one material's worst news, for the Materials tab
// ---------------------------------------------------------------------------

struct FMapLinkTextureAggregate
{
    int32 NumConform      = 0;
    int32 NumPlugin       = 0;
    int32 NumPortable     = 0;
    int32 NumLocalOnly    = 0;
    int32 NumUnclassified = 0;
    int32 NumBroken       = 0;

    int32 Total() const
    {
        return NumConform + NumPlugin + NumPortable + NumLocalOnly
             + NumUnclassified + NumBroken;
    }

    /** Worst status present, ordered by how much it will cost someone:
     *  Broken > LocalOnly > Unclassified > Portable > Conform.
     *  Plugin never ranks: it costs nobody anything, so a material whose only
     *  textures ship with MapLink reads clean, which it is. */
    EMapLinkTextureStatus Worst() const
    {
        if (NumBroken       > 0) return EMapLinkTextureStatus::Broken;
        if (NumLocalOnly    > 0) return EMapLinkTextureStatus::LocalOnly;
        if (NumUnclassified > 0) return EMapLinkTextureStatus::Unclassified;
        if (NumPortable     > 0) return EMapLinkTextureStatus::Portable;
        if (NumConform      > 0) return EMapLinkTextureStatus::Conform;
        return NumPlugin > 0 ? EMapLinkTextureStatus::Plugin
                             : EMapLinkTextureStatus::Conform;
    }

    bool IsClean() const
    {
        return NumBroken == 0 && NumLocalOnly == 0 && NumUnclassified == 0;
    }
};

// ---------------------------------------------------------------------------
// Enum <-> string. One place, used by the policy file, the manifest, the
// console commands and the UI, so none of them can drift.
// ---------------------------------------------------------------------------

namespace MapLinkTexture
{
    MAPLINKEDITOR_API const TCHAR* ToString(EMapLinkTextureClass  In);
    MAPLINKEDITOR_API const TCHAR* ToString(EMapLinkTextureStatus In);
    MAPLINKEDITOR_API const TCHAR* ToString(EMapLinkTextureResult In);
    MAPLINKEDITOR_API const TCHAR* ToString(EMapLinkTextureClaimPreset In);

    /** Case-insensitive. Returns false and leaves Out untouched on no match. */
    MAPLINKEDITOR_API bool ParseClass (const FString& In, EMapLinkTextureClass&  Out);
    MAPLINKEDITOR_API bool ParsePreset(const FString& In, EMapLinkTextureClaimPreset& Out);

    /** "TC_Normalmap" <-> TextureCompressionSettings. Round-trips through the
     *  reflected enum so a UE upgrade that adds a compression mode does not
     *  silently record it as TC_Default. */
    MAPLINKEDITOR_API FString CompressionToString(TextureCompressionSettings In);
    MAPLINKEDITOR_API TextureCompressionSettings CompressionFromString(const FString& In);

    /** A one-line human sentence for a status, phrased as consequence rather
     *  than as a label — "works only on this machine" beats "LocalOnly". */
    MAPLINKEDITOR_API FText StatusText(EMapLinkTextureStatus In);
}
