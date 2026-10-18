// Copyright MapModelers. All Rights Reserved.
//
// v1.16 — texture_policy.json: the P4-tracked record of what the team decided.
//
// TWO STORES, DIFFERENT LIFETIMES. This is the one that matters:
//
//   <Root>/texture_policy.json   project data, source-controlled, shared.
//                                A class decision here is a decision the whole
//                                team made and can see in a diff.
//
//   UMapLinkEditorState          per-user scan cache, regenerable, disposable.
//
// Never confuse them. Auto-classification writes NOTHING here — a suggestion is
// pre-filled in the UI and only the user's acceptance lands in this file,
// because this file is the audit trail.

#pragma once

#include "CoreMinimal.h"
#include "MapLinkTextureTypes.h"

class MAPLINKEDITOR_API FMapLinkTexturePolicy
{
public:
    /** Bumped when the on-disk shape changes in a way a reader must notice. */
    static constexpr int32 SchemaVersion = 1;

    /** The changelist every policy write stacks into. */
    static const TCHAR* GetChangelistDescription();

    /** <Root>/texture_policy.json, next to maplink_project.json.
     *  Empty when the project root is unresolved. */
    static FString GetPolicyFilePath();

    // ── Load / save ───────────────────────────────────────────────────────

    /** Read from disk into the in-memory map. A missing file is not an error —
     *  it means no decisions have been recorded yet, which is the correct state
     *  for a project that has never run a scan. Returns false only on a file
     *  that exists and does not parse. */
    static bool Load();

    /** Load once per session unless already loaded. */
    static void EnsureLoaded();

    /**
     * Write the in-memory map back to disk.
     *
     * Atomic, following AtomicRegenLibrary: temp file on the same volume, parse
     * it back to prove it is valid, then replace. A half-written policy file
     * would silently reclassify textures on the next load, so there is never a
     * moment where the real file is partial.
     *
     * ALSO change-compared: when the serialised bytes match what is already on
     * disk, nothing is written and nothing is checked out. A rescan that
     * changes no decisions must not dirty a P4-tracked file.
     *
     * Returns true when the file is up to date afterwards, whether or not it
     * was actually written. bOutWrote distinguishes the two.
     */
    static bool Save(bool* bOutWrote = nullptr);

    /** Drop the in-memory map, forcing the next access to re-read. */
    static void Invalidate();

    // ── Access ────────────────────────────────────────────────────────────

    /** Null when no decision has been recorded for this asset. */
    static const FMapLinkTexturePolicyEntry* Find(const FString& AssetPath);

    /** Unclassified when there is no entry. */
    static EMapLinkTextureClass GetClass(const FString& AssetPath);

    /** Record the class decision. In memory only — call Save(). */
    static void SetClass(const FString& AssetPath, EMapLinkTextureClass NewClass);

    /** Record the whole entry, replacing any existing one. In memory only. */
    static void Set(const FMapLinkTexturePolicyEntry& Entry);

    /** Refresh the recorded sRGB / compression for an asset without disturbing
     *  its class, note or conform history. These are read from the asset and
     *  preserved, never changed — conformance governs container, location and
     *  name, never pixel values. */
    static void RecordAssetSettings(const FString& AssetPath,
                                    bool bSRGB,
                                    const FString& Compression);

    /** Stamp last_conform with an ISO-8601 UTC timestamp and the operation
     *  name, and set lossy_reduction. In memory only. */
    static void StampConform(const FString& AssetPath,
                             const FString& OperationName,
                             bool bLossyReduction);

    static void Remove(const FString& AssetPath);

    /** Every entry, sorted by asset path — the same order Save() writes. */
    static void GetAll(TArray<FMapLinkTexturePolicyEntry>& OutEntries);

    static int32 Num();

    // ── Validation ────────────────────────────────────────────────────────

    /** Check an entry for values a future reader would choke on: an unresolvable
     *  compression name, a negative or absurd preview cap. Returns false with a
     *  human reason. Used on load so a hand-edited file reports its problem
     *  once, loudly, instead of behaving strangely forever. */
    static bool ValidateEntry(const FMapLinkTexturePolicyEntry& Entry,
                              FString& OutReason);
};
