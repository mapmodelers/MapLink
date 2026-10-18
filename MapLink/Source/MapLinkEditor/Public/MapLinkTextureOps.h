// Copyright MapModelers. All Rights Reserved.
//
// v1.16 — Texture operations: conform, claim, promote, orphans.
//
// EVERY OPERATION SUPPORTS AND DEFAULTS TO DRY RUN. Nothing here writes unless
// bExecute is explicitly true, and the destructive presets additionally require
// a modal confirm from the caller.
//
// TWO INVARIANTS GOVERN THE WHOLE FILE:
//
//   1. Non-MapLink materials are never modified, under any operation, for any
//      reason. FMapLinkTextureOwnership::IsMapLinkMaterial is the only test.
//
//   2. Deleting the original uasset requires repointing EVERY referencer. If
//      one cannot be repointed, the texture's whole operation aborts. There is
//      never a dangling reference.
//
// Those two meet in one place worth stating plainly: a NON-MapLink material
// that references a texture is a referencer that cannot be repointed — not
// because it is locked, but because touching it is forbidden. So Migrate and
// Adopt abort on it. That is the only reading under which both invariants hold.

#pragma once

#include "CoreMinimal.h"
#include "MapLinkTextureTypes.h"
#include "MapLinkTypes.h"

class UTexture2D;

/** What happened to one texture in one run. Every field reaches the manifest. */
struct FMapLinkTextureOpEntry
{
    FString AssetPathBefore;
    FString AssetPathAfter;
    FString DiskPathBefore;
    FString DiskPathAfter;

    EMapLinkTextureClass  TextureClass = EMapLinkTextureClass::Unclassified;
    EMapLinkTextureResult Result       = EMapLinkTextureResult::Failed;

    bool bLossyReduction   = false;
    bool bRenamed          = false;
    bool bSkippedIdentical = false;

    /** Set when non-uniform alpha was extracted into its own texture. */
    FString AlphaAssetPath;

    /** Something a human must do that the tool must not: authoring a material
     *  parameter, deciding a compression setting. Surfaced separately from
     *  warnings because it is work, not noise. */
    FString ActionItem;

    TArray<FString> Warnings;
    TArray<FString> Errors;

    /** MIs repointed at the new texture. */
    TArray<FString> RepointedMaterials;
};

/** One removable texture, for the cleanup dialog. */
struct FMapLinkTextureCleanupItem
{
    FString AssetPath;
    FString AssetName;
    FString PackagePath;
    FString SourceDiskPath;

    /** True when the source file is inside the MapLink project and would be
     *  deleted along with the asset. False means the asset goes and the file
     *  on disk stays, because it is not ours. */
    bool bSourceIsOurs = false;

    /** "Orphan" or "Superseded" — see FindCleanupCandidates. */
    FString Reason;

    /** The T_Mk_ asset that replaced this one. Superseded items only. */
    FString SupersededBy;

    int64 SourceBytes = 0;
};

struct FMapLinkTextureOpReport
{
    FString OperationName;
    FString Preset;
    bool    bExecuted = false;      // false == dry run

    TArray<FMapLinkTextureOpEntry> Entries;

    int32 NumConformed  = 0;
    int32 NumSkipped    = 0;
    int32 NumFailed     = 0;
    int32 NumLossy      = 0;
    int32 NumAlphaSplit = 0;
    int32 NumWrites     = 0;   // actual disk writes; zero on a warm re-run

    /** Where the manifest landed. Always written, including on a dry run —
     *  a dry run's whole product IS the manifest. */
    FString ManifestPath;

    double RunSeconds = 0.0;

    bool HasFailures() const { return NumFailed > 0; }
};

class MAPLINKEDITOR_API FMapLinkTextureOps
{
public:
    /** The changelist source files and uassets stack into. */
    static const TCHAR* GetChangelistDescription();

    // ── §6.1 Conform ──────────────────────────────────────────────────────

    /**
     * Bring MapLink-class textures into the locked container, at the right
     * name, in the right folder, with AssetImportData pointing at the file
     * MapLink owns.
     *
     * Conformance governs container, location and name — so yes, this renames
     * and moves when it has to. It never touches pixel values, except for the
     * explicit bit-depth reduction in §5.3, which is recorded as lossy.
     *
     * An already-conform texture assembles to byte-identical output and is not
     * rewritten. A second full run performs zero writes.
     */
    static FMapLinkTextureOpReport Conform(const TArray<FString>& AssetPaths,
                                           bool bExecute);

    /** Every MapLink-class row from the last scan. */
    static FMapLinkTextureOpReport ConformAll(bool bExecute);

    // ── §6.2 Claim ────────────────────────────────────────────────────────

    /**
     * Take a texture that is outside MapLink and make it MapLink's.
     *
     * Named Claim, not Adopt: Adopt is one of the four presets, and
     * "Textures.Adopt <path> Adopt" was a command colliding with itself.
     *
     *   Fork     keep file, keep uasset, repoint MapLink-owned MIs only  (default)
     *   Migrate  delete file, delete uasset, repoint every referencer
     *   Adopt    keep file,   delete uasset, repoint every referencer
     *   Copy     keep both,   repoint nothing
     *
     * Fork is the default because MapLink owning its copies means editing a
     * MapLink texture affects nothing else in the project.
     */
    static FMapLinkTextureOpReport Claim(const FString& AssetPath,
                                         EMapLinkTextureClaimPreset Preset,
                                         bool bExecute);

    /** §6.3 — External becomes MapLink. Identical to Fork, plus the policy
     *  class flips and any existing preview PNG is removed. */
    static FMapLinkTextureOpReport Promote(const FString& AssetPath, bool bExecute);

    // ── §6.5 Orphans ──────────────────────────────────────────────────────

    /** MapLink-class textures under /Game/MapLink/Textures/ that no
     *  MapLink-owned MI references. Non-blocking status. bDelete is gated
     *  behind bExecute and a caller-side modal confirm. NEVER auto-deletes. */
    static FMapLinkTextureOpReport Orphans(bool bDelete, bool bExecute);

    // ── Unified conform (v1.16) ───────────────────────────────────────────

    /**
     * Make these textures conform, whatever state they are in.
     *
     * "Conform" as a user sees it is one outcome: the texture is MapLink's and
     * its source file is in the MapLink project. Whether that took a rename
     * and a write, or a duplicate first, is an implementation detail — and
     * making the user pick the right verb for a distinction they cannot see
     * was a design error. This is the single entry point the UI calls.
     *
     *   already MapLink-owned  → Conform  (rename if needed, write the TGA)
     *   anything else          → Promote  (duplicate in, write, repoint)
     *
     * Entries from both paths are merged into one report, so a mixed selection
     * produces one honest summary rather than two partial ones.
     */
    static FMapLinkTextureOpReport ConformAny(const TArray<FString>& AssetPaths,
                                              bool bExecute);

    /**
     * Import an image file from disk straight into MapLink, then conform it.
     *
     * The path a texture that does not exist in the project yet has to take.
     * Without it, "get this file into MapLink" means import by hand into the
     * right folder, then find it in the list, then conform it.
     *
     * Returns a report whose entries are the conform results.
     */
    static FMapLinkTextureOpReport ImportAndConform(const TArray<FString>& SourceFiles,
                                                    bool bExecute);

    // ── §6.6 Cleanup (v1.16) ──────────────────────────────────────────────

    /**
     * Textures nothing references any more, with a reason.
     *
     * Two ways a texture gets here, and both are the tool's own doing, which
     * is why the tool should offer to tidy up after itself:
     *
     *   Orphan      a T_Mk_ texture MapLink made, whose material no longer
     *               uses it — a parameter was repointed or a material deleted.
     *
     *   Superseded  the ORIGINAL of something you copied into MapLink. Once
     *               every material has moved to the T_Mk_ twin, the original
     *               is dead weight the project still carries.
     *
     * The gate for both is the same and it is absolute: the AssetRegistry
     * must report ZERO referencers of any kind. Not "no MapLink materials" —
     * nothing at all, including levels, blueprints and data assets. A texture
     * one level still uses is not cleanup, it is a broken level.
     */
    static TArray<FMapLinkTextureCleanupItem> FindCleanupCandidates();

    /**
     * Delete these assets and their MapLink-owned source files.
     *
     * Re-checks the zero-referencer gate on every asset at execute time
     * rather than trusting the list it was handed — the list may have been
     * sitting in a dialog while the user did something else. An asset that
     * has gained a referencer since is skipped and reported, never deleted.
     *
     * Source files are only removed when they live under the MapLink project.
     * Somebody else's file on disk is not ours to delete just because the UE
     * asset pointing at it has gone.
     */
    static FMapLinkTextureOpReport DeleteCleanupItems(
        const TArray<FString>& AssetPaths, bool bExecute);

    // ── Reporting ─────────────────────────────────────────────────────────

    /** <ArtifactLogDir>/TextureConform/texture_conform_<timestamp>.json.
     *  Written for every run, dry or not. Returns the path. */
    static FString WriteManifest(FMapLinkTextureOpReport& Report);

    /** Toast summary per the feedback policy — never a modal. Modals are for
     *  the destructive-preset confirm only, which is the caller's job. */
    static void NotifySummary(const FMapLinkTextureOpReport& Report);

    /** Human-readable multi-line form, for the console and the clipboard. */
    static FString BuildReportText(const FMapLinkTextureOpReport& Report);

    /** True when this preset can destroy something the user cannot get back
     *  from the editor. Callers must show a modal confirm first. */
    static bool IsDestructive(EMapLinkTextureClaimPreset Preset);
};
