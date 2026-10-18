// Copyright MapModelers. All Rights Reserved.
//
// v1.9 — Unified source-control helper.
//
// One implementation of "find-or-create a pending changelist by description
// and stack files into it", shared by every MapLink C++ call site:
//   - Import pipeline   → "[MapLink] Import"
//   - Reimport pipeline → "[MapLink] Reimport"
//   - Material Sync     → "[MapLink] Material Sync"
//   - BP Stand-ins      → "[MapLink] Stand-in Creation" / "Cleanup"
//
// All operations are graceful no-ops when no source-control provider is
// enabled (marketplace users without Perforce).

#pragma once

#include "CoreMinimal.h"
#include "ISourceControlChangelist.h"

class UPackage;

class FMapLinkSourceControl
{
public:
    // ── Changelist resolution ─────────────────────────────────────────
    //
    // Find an existing PENDING changelist whose NORMALIZED description
    // matches (see NormalizeChangelistDescription), or create a new one.
    // Results are cached per description for the editor session to avoid
    // refreshing the pending CL cache on every file op; a cache hit is
    // verified (the CL must still exist and be pending) before reuse, so
    // externally-submitted CLs never produce stale handles.  A new CL is
    // NEVER created while a normalized match exists.  Returns nullptr
    // when SCC is disabled or the provider doesn't support changelists
    // (→ default CL).
    //
    // CONTRACT (v1.13): the description strings are the cross-tool merge
    // key.  The UE plugin and the Blender addon (blender/p4_integration.py)
    // must produce/reuse the SAME pending CL for the same description
    // (e.g. "[MapLink] Import"), so UE-side and Blender-side files stack
    // together.  Do not rename these strings casually — both sides and
    // users' muscle memory depend on them.
    static FSourceControlChangelistPtr GetOrCreateChangelist(
        const FString& Description);

    /** Normalize a CL description for comparison: split into lines, trim
     *  each line's leading/trailing whitespace, drop trailing empty lines,
     *  rejoin with '\n'.  Comparison stays case-SENSITIVE.  Mirrored by
     *  _normalize_description() in blender/p4_integration.py — keep in
     *  sync. */
    static FString NormalizeChangelistDescription(const FString& Description);

    /** Drop the per-description CL cache (e.g. at session start, or after
     *  a CL may have been submitted externally). */
    static void ResetChangelistCache();

    // ── Session description ───────────────────────────────────────────
    //
    // The pipeline sets this once at the start of RunImport / RunReimport;
    // every package save inside the session then stacks into the right CL
    // without threading a description through every call.
    static void    SetSessionDescription(const FString& Description);
    static FString GetSessionDescription();

    // ── File / package ops ────────────────────────────────────────────

    /**
     * Ensure a file is opened in the changelist with the given description:
     *   - already source-controlled → check out (if not already open)
     *   - not in the depot          → mark for add
     * Returns true on success OR when SCC is disabled (the file write is
     * what matters).
     */
    static bool EnsureFileInChangelist(
        const FString& AbsoluteFilePath,
        const FString& Description);

    /**
     * v1.16 — Remove a file, correctly, in one call.
     *
     * EnsureFileInChangelist is the WRONG verb for a delete, and using it was
     * a real bug: a file that had never been submitted got marked for add and
     * was then unlinked, leaving the changelist holding a pending add for a
     * path that no longer exists.  That CL cannot be submitted.
     *
     * The three cases, all handled here:
     *   opened for add   → revert (un-add), then delete from disk
     *   in the depot     → FDelete (p4 delete opens it and removes the file)
     *   not tracked      → clear read-only and delete from disk
     *
     * Returns true when the file is gone from disk afterwards. A missing file
     * is success, not an error — this is idempotent by design.
     */
    static bool DeleteFileTracked(
        const FString& AbsoluteFilePath,
        const FString& Description);

    /**
     * Call BEFORE UPackage::SavePackage on a package that may already be
     * in the depot (read-only on disk).  Checks the file out into the
     * session CL so the save can write it.  No-op if the file doesn't
     * exist on disk yet (first save → handled by AddAfterSave).
     */
    static void CheckoutBeforeSave(UPackage* Package);

    /**
     * Call AFTER a successful UPackage::SavePackage.  Marks brand-new
     * files for add into the session CL (and verifies checkout state on
     * existing ones).
     */
    static void AddAfterSave(UPackage* Package);

    /** Resolve a package's on-disk filename (asset vs map extension). */
    static FString GetPackageFilename(UPackage* Package);

    // ── v1.13 — Pre-reimport shelve (opt-in safety net) ──────────────
    //
    // Called by RunReimportImpl before the destructive per-node loop when
    // UMapLinkSettings::bShelveBeforeReimport is ON.  Collects every
    // /Game/MapLink/<stem>/ package file about to be nuked plus the source
    // .blend(s) and their P4-tracked sidecars, ensures they are opened in
    // the "[MapLink] Reimport" CL, then shelves that CL via the provider's
    // FShelve operation.  ALL failures are non-fatal (toast + log); the
    // reimport always proceeds.
    static void ShelveReimportSnapshot(
        const TArray<FString>& ChangedBlendPaths);

private:
    static FString SessionDescription;
    static TMap<FString, FSourceControlChangelistPtr> ChangelistCache;
};
