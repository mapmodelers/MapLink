// Copyright MapModelers. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MapLinkTypes.h"

class ISourceControlChangelist;

/**
 * Scans UE5 content for UMaterialInstanceConstant assets, writes a single
 * unified JSON, then invokes the Blender headless script to create materials
 * and palette cubes in Library_MaterialInstances.blend.
 *
 * v1.10 — also hosts the invisible MIM auto-sync engine (Resync / SyncAfterOp).
 */
class FMapLinkMaterialExporter
{
public:
    /**
     * The one-time migration: re-render every MapLink MI's thumbnail through
     * the MapLink scene, cache it into the .uasset, and save the packages onto
     * one changelist.  After this the Content Browser and the Blender Asset
     * Browser show the same picture.
     *
     * Deliberately NOT part of a normal sync. A material sync that checks out
     * every material instance in the project is not something anyone asked
     * for, and the stamp only needs doing when the look changes.
     */
    static EMapLinkResult StampAssetThumbnails();

    // ── v1.10 — Material Instance Manager auto-sync engine ─────────────
    //
    // UE is authoritative; Blender is a one-way consumer.  The engine
    // regenerates every sibling material library (diffuse + PBR) from the
    // current UE MI state in a single batched op, writing each library
    // ATOMICALLY (temp .blend on the same volume → validate it opens →
    // atomic replace) and staging the result into a find-or-create source-
    // control changelist.  On any regen failure it raises the library-dirty
    // flag and reports MaterialSyncFailed WITHOUT rolling back UE work; the
    // caller's UE-side op stays committed and the user recovers via Resync.

    /** Manual recovery escape hatch (Settings tab "Resync").  Runs the full
     *  sync engine under the "[MapLink] Material Sync" changelist. */
    static EMapLinkResult Resync();

    /** Invisible tail step every material op (create / duplicate / rename /
     *  edit-save / reparent / conform) calls once at its end.  Same engine as
     *  Resync; the op's changelist description is threaded through so the
     *  library regen is staged into the SAME CL as the op.  Empty description
     *  falls back to "[MapLink] Material Sync".
     *  NOTE (P1): no op is wired to this yet — ops land in the next P1 drop.
     *  The API is proven now via Resync(), which shares RunSyncEngine(). */
    static EMapLinkResult SyncAfterOp(const FString& OpChangelistDescription);

    /** True when the last sync left the material libraries out of date. */
    static bool IsLibraryDirty();

    /** Set/clear the library-dirty flag (persisted in UMapLinkEditorState). */
    static void MarkLibraryDirty(bool bDirty);

    // ── Legacy dev diagnostic ──────────────────────────────────────────
    //
    // v1.10: the user-facing "Sync Materials to Blender" menu entry was
    // removed (invariant: sync is never a button).  This single-library,
    // in-place sync is RETAINED as a dev-only diagnostic and is no longer
    // surfaced in shipped UI.  Production paths use the atomic engine above.

    /** Full pipeline: scan MIs → write unified JSON → invoke Blender
     *  (in-place save of the configured Material Instances .blend). */
    static EMapLinkResult SyncToBlender();

private:
    // ── Sync engine internals (v1.10) ──────────────────────────────────

    /** One sibling material library to regenerate. */
    struct FLibraryTarget
    {
        FString BlendPath;     // absolute path to the real library .blend
        bool    bIsPBR = false;// channel-scope marker (full maps); P2 uses it
    };

    /** Resolve the sibling library set: the configured diffuse library
     *  (Library_MaterialInstances.blend) plus the "_PBR" sibling when it
     *  exists on disk.  Returns false if the diffuse library is unresolved. */
    static bool ResolveLibraryTargets(TArray<FLibraryTarget>& OutTargets);

    /** Core engine: scan MIs → JSON → atomic regen of every sibling library,
     *  all staged into the CL named by Description.  On failure: raise the
     *  dirty flag and return MaterialSyncFailed; on success: clear the flag. */
    static EMapLinkResult RunSyncEngine(const FString& ChangelistDescription);

    /** Atomically regenerate a single library: copy real→temp (same volume),
     *  run the headless rebuild against the temp, validate it opened/saved,
     *  check the real file out into the CL, then atomic-replace real←temp.
     *  Never touches the real file until the temp validates. */
    static EMapLinkResult AtomicRegenLibrary(
        const FLibraryTarget& Target,
        const FString& JsonPath,
        const FString& MaterialsLibPath,
        const FString& ChangelistDescription,
        FString& OutError);

    /** Run maplink_material_sync.py against an explicit target .blend (the
     *  temp copy) with a per-library log dir.  No SCC on the target (scratch).
     *  Returns the bridge result (Success / *Failed / Cancelled / TimedOut). */
    /**
     * The '//'-relative form of *TargetPath* as seen from *FinalLibDir*, or
     * "" when no relative form exists (different volume).
     *
     * v1.19 — the sibling of the v1.16 texture-anchor arithmetic, and for the
     * same reason: AtomicRegenLibrary regenerates into
     * <LibDir>/.maplink_sync_tmp/, one level deeper than where the file lands,
     * so bpy.path.relpath() on the Blender side describes the temp file's
     * position and is wrong the instant the file is replaced upward. Only UE
     * knows the destination, so only UE can do this arithmetic.
     */
    static FString MakeLibraryRelativePath(const FString& FinalLibDir,
                                           const FString& TargetPath);

    static EMapLinkResult RunBlenderMaterialSyncToTarget(
        const FString& TargetBlendPath,
        const FString& JsonPath,
        const FString& MaterialsLibPath,
        const FString& LogDir,
        const FString& MaterialsLibRelPath = FString());

    // ── Shared ─────────────────────────────────────────────────────────

    /** Scan MIs, build JSON, write to disk. Returns the JSON path on success.
     *  The JSON file is staged into the CL named by ChangelistDescription. */
    static EMapLinkResult RunExport(
        FString& OutJsonPath,
        const FString& ChangelistDescription = TEXT("[MapLink] Material Sync"),
        bool bStampAssetThumbnails = false);

    /** Invoke the Blender material sync script (legacy in-place path). */
    static EMapLinkResult RunBlenderMaterialSync(const FString& JsonPath);

    // ── v1.20 — Asset Browser thumbnails ───────────────────────────────
    //
    // Material instances are Blender Asset Browser assets now, and an asset
    // with no preview is a grey sphere.  Blender cannot render one here:
    // material previews come from its RENDER_PREVIEW job, which needs a
    // window, and the sync runs --background.  UE already renders these for
    // its own Content Browser, so the cheapest correct picture is the one that
    // exists before Blender is even launched.

    /**
     * Render *MI*'s thumbnail and write it as a TGA under <MIDir>/Thumbnails/.
     *
     * Returns the path RELATIVE to MIDir — the form stored in the JSON, and
     * the same rule as the texture entries' `rel` field, for the same reason:
     * an absolute path is correct on exactly one machine.  Empty when nothing
     * was written, which is never an error the caller should fail on.
     */
    static FString WriteMIThumbnail(class UMaterialInterface* MI,
                                    const FString& MIDir,
                                    const FString& ChangelistDescription,
                                    class FMapLinkMaterialThumbnailScene& Scene,
                                    bool bCacheIntoPackage,
                                    bool& bOutPackageDirtied);

    /**
     * Delete thumbnails whose material instance no longer exists.  Returns the
     * count removed.  Uses DeleteFileTracked, never EnsureFileInChangelist —
     * using the checkout verb for a delete is what left v1.16 with a pending
     * add for an unlinked path and an unsubmittable changelist.
     */
    static int32 PruneStaleThumbnails(const FString& MIDir,
                                      const TSet<FString>& LiveMINames,
                                      const FString& ChangelistDescription);

    /** Save every MI package the thumbnail stamp dirtied.  -> count saved. */
    static int32 SaveDirtyMIPackages(const TArray<struct FAssetData>& MIAssets,
                                     const FString& ChangelistDescription);


    /** Write structured export_log.json to the material sync log directory. */
    static void WriteExportLog(
        const FString& ContentPath,
        const TArray<TSharedPtr<FJsonValue>>& Results,
        int32 TotalScanned, int32 Warnings);
};
