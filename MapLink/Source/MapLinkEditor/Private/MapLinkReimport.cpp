// Copyright MapModelers. All Rights Reserved.
//
// REIMPORT v2.4 — v1.7 WP Expansion:
//
// New in v2.4:
//   - After reimporting changed nodes, trigger WP re-expansion for any
//     recorded WP expansion that is affected by the changed nodes.
//     Uses FMapLinkWPExpansionHelper::ReExpandIfNeeded().
//
// Previous: v2.3 — v1.6.2 bugfix:
//   - MISSING ASSET BUG: When a dependency's .blend hash matches the stored
//     C++ hash (filehash_cpp.txt), the node was silently skipped. But if the
//     UE5 asset was deleted or never fully created, the swap phase would fail
//     with "No BP asset found for 'X'".
//     FIX: After confirming hashes match, also verify the primary asset exists
//     on disk via DoesPrimaryAssetExist(). If missing, treat as changed.
//
// Previous fixes (v2.2, v1.6.1):
//   - NEW DEPENDENCY BUG
//
// Previous improvements (v2.1, v1.4.5):
//   - Fixed TRIPLE BLENDER LAUNCH bug
//   - Removed redundant root analyze before BuildGraph
//   - BuildGraph skips Blender for deps with valid cached analysis.json

#include "MapLinkPipeline.h"
#include "MapLinkUtils.h"
#include "MapLinkBlenderBridge.h"
#include "MapLinkDependencyGraph.h"
#include "MapLinkInterchangeHelper.h"
#include "MapLinkBlueprintHelper.h"
#include "MapLinkLevelHelper.h"
#include "MapLinkWPExpansionHelper.h"
#include "MapLinkProxyAsset.h"
#include "MapLinkTiming.h"
#include "MapLinkBPReferenceMode.h"  // v1.8
#include "MapLinkSettings.h"         // v1.9 -- warm-pass toggle
#include "MapLinkSourceControl.h"    // v1.13 -- pre-reimport shelve
#include "MapLinkUEStatus.h"         // v1.13.1 -- per-node close-the-loop badge
#include "MapLinkNotify.h"           // v1.21 -- MapLinkResultToText
#include "Misc/MessageDialog.h"      // v1.21 -- one failure summary
#include "Containers/Ticker.h"       // v1.14 -- deferred in-place outliner refresh
#include "Engine/Engine.h"           // v1.15.1 -- rebroadcast after deferred prune
#include "LevelInstance/LevelInstanceActor.h"  // v1.13.1 -- switch-away guard
#include "MapLinkEditorState.h"      // v1.9 -- Manager window state
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Engine/World.h"
#include "Engine/Blueprint.h"
#include "UObject/UObjectHash.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopedSlowTask.h"
#include "FileHelpers.h"
#include "Misc/SecureHash.h"
#include "UObject/LinkerLoad.h"
#include "HAL/PlatformFileManager.h"
#include "ObjectTools.h"
#include "EngineUtils.h"

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------

namespace
{
    /** Read the stored C++ hash from <ArtifactLogDir>/<blend_name>/filehash_cpp.txt */
    FString ReadStoredCppHash(const FString& PluginBaseDir, const FString& BlendName)
    {
        FString HashPath = FMapLinkUtils::ResolveArtifactLogFile(
            BlendName / TEXT("filehash_cpp.txt"));
        FString Hash;
        FFileHelper::LoadFileToString(Hash, *HashPath);
        Hash.TrimStartAndEndInline();
        return Hash;
    }

    /** Write the C++ hash to <ArtifactLogDir>/<blend_name>/filehash_cpp.txt */
    void WriteStoredCppHash(const FString& PluginBaseDir, const FString& BlendName, const FString& Hash)
    {
        FString LogDir = FMapLinkUtils::GetArtifactLogDir() / BlendName;
        IFileManager::Get().MakeDirectory(*LogDir, true);
        FString HashPath = LogDir / TEXT("filehash_cpp.txt");
        FFileHelper::SaveStringToFile(Hash, *HashPath,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }

    /** Read the Blender-produced hash from <ArtifactLogDir>/<blend_name>/filehash.txt */
    FString ReadStoredBlenderHash(const FString& PluginBaseDir, const FString& BlendName)
    {
        FString HashPath = FMapLinkUtils::ResolveArtifactLogFile(
            BlendName / TEXT("filehash.txt"));
        FString Hash;
        FFileHelper::LoadFileToString(Hash, *HashPath);
        Hash.TrimStartAndEndInline();
        return Hash;
    }

    /** Check whether the primary asset for a node actually exists on disk. */
    bool DoesPrimaryAssetExist(const FString& NodeBlendName)
    {
        if (NodeBlendName.StartsWith(TEXT("SM_")))
        {
            FString MeshContentPath = FString::Printf(
                TEXT("/Game/MapLink/%s/Meshes"), *NodeBlendName);
            FString MeshDiskDir = FPackageName::LongPackageNameToFilename(MeshContentPath);
            return IFileManager::Get().DirectoryExists(*MeshDiskDir);
        }

        // v1.8 — BP Reference Mode: the "primary asset" for a
        // reference-mode BP_ is the bound BP, NOT a synthesized
        // /Game/MapLink/<name>/<name>.uasset.  Probe the proxy first.
        if (NodeBlendName.StartsWith(TEXT("BP_")))
        {
            FString BoundBPPath;
            UMapLinkProxyAsset* RefProxy = nullptr;
            if (FMapLinkBPReferenceMode::IsProxyReferenceMode(
                    NodeBlendName, BoundBPPath, &RefProxy))
            {
                // True if either the bound BP is loadable, OR the
                // package exists on disk under whatever soft-path.
                if (RefProxy && RefProxy->BoundBlueprint.LoadSynchronous())
                {
                    return true;
                }
                if (!BoundBPPath.IsEmpty())
                {
                    // Strip the .Object suffix the soft-path serializer adds.
                    FString PkgName = BoundBPPath;
                    int32 DotIdx;
                    if (PkgName.FindChar(TEXT('.'), DotIdx))
                    {
                        PkgName = PkgName.Left(DotIdx);
                    }
                    FString DiskPath = FPackageName::LongPackageNameToFilename(
                        PkgName, FPackageName::GetAssetPackageExtension());
                    if (FPaths::FileExists(DiskPath))
                    {
                        return true;
                    }
                }
                // Bound BP gone → caller will trigger re-process to
                // pick up BP_Deprecated substitution at swap time.
                return false;
            }
        }

        FString AssetPkgPath = FString::Printf(
            TEXT("/Game/MapLink/%s/%s"), *NodeBlendName, *NodeBlendName);

        bool bIsLevel = NodeBlendName.StartsWith(TEXT("LI_"));
        FString AssetDiskPath = FPackageName::LongPackageNameToFilename(
            AssetPkgPath,
            bIsLevel ? FPackageName::GetMapPackageExtension()
                     : FPackageName::GetAssetPackageExtension());

        return FPaths::FileExists(AssetDiskPath);
    }

    /** Nuke ALL generated assets for a blend node.
     *
     *  v1.7.6: Reordered for performance.  Previously the order was:
     *      1. Delete mesh assets (slow — level/BP holds live references)
     *      2. Delete mesh disk folder
     *      3. (SM_ early return)
     *      4. Rename main asset package + delete .uasset/.umap (does not
     *         destroy the level UObject in memory; references survive
     *         until the final CollectGarbage)
     *
     *  Now for BP_/LI_, the main asset is ForceDeleted FIRST.  That
     *  destroys the level/BP UObject and clears all of its component
     *  references to meshes.  Subsequent mesh deletion has no live
     *  referencers to walk and replace — what was 5+ seconds for a
     *  large LI_ becomes hundreds of ms.
     */
    void NukeAssetsForBlend(const FString& BlendName, bool bIsLevel)
    {
        IFileManager& FM = IFileManager::Get();
        const bool bIsStaticMesh = BlendName.StartsWith(TEXT("SM_"));

        FString MeshContentPath = FString::Printf(
            TEXT("/Game/MapLink/%s/Meshes"), *BlendName);
        FString MeshDiskDir = FPackageName::LongPackageNameToFilename(MeshContentPath);

        // ---------------------------------------------------------------
        // Phase 1c.0 (v1.7.6) — Delete main asset FIRST (BP_/LI_ only).
        // SM_ files have no main asset and skip this entire block.
        // ---------------------------------------------------------------
        if (!bIsStaticMesh)
        {
            MAPLINK_SCOPED_PHASE("Phase1c.0_DeleteMainAsset");

            FString AssetPkgPath = FString::Printf(
                TEXT("/Game/MapLink/%s/%s"), *BlendName, *BlendName);
            FString AssetDiskPath = FPackageName::LongPackageNameToFilename(
                AssetPkgPath,
                bIsLevel ? FPackageName::GetMapPackageExtension()
                         : FPackageName::GetAssetPackageExtension());

            // Protect the _GP companion asset.
            FString GPDiskPath = FPackageName::LongPackageNameToFilename(
                FString::Printf(TEXT("/Game/MapLink/%s/%s_GP"), *BlendName, *BlendName),
                FPackageName::GetAssetPackageExtension());

            if (AssetDiskPath == GPDiskPath)
            {
                UE_LOG(LogMapLink, Warning,
                       TEXT("Refusing to delete _GP asset for '%s'."), *BlendName);
            }
            else
            {
                // ForceDelete the actual UObject(s) inside the package so that
                // references TO meshes (held by ALevelInstance / SCS / etc.) are
                // nulled out before we try to delete the meshes themselves.
                UPackage* MainPkg = FindPackage(nullptr, *AssetPkgPath);
                TArray<UObject*> MainObjectsToDelete;
                if (MainPkg)
                {
                    ResetLoaders(MainPkg);
                    TArray<UObject*> ObjectsInPkg;
                    GetObjectsWithOuter(MainPkg, ObjectsInPkg, /*bIncludeNestedObjects=*/false);
                    for (UObject* O : ObjectsInPkg)
                    {
                        if (!O) continue;
                        if (O->IsA<UWorld>() || O->IsA<UBlueprint>())
                        {
                            MainObjectsToDelete.Add(O);
                        }
                    }
                }

                if (MainObjectsToDelete.Num() > 0)
                {
                    ObjectTools::ForceDeleteObjects(MainObjectsToDelete, /*bShowConfirmation=*/false);
                    UE_LOG(LogMapLink, Log,
                           TEXT("ForceDeleted main asset for '%s' (%d UObject(s) — references to meshes now cleared)."),
                           *BlendName, MainObjectsToDelete.Num());
                }
                else
                {
                    UE_LOG(LogMapLink, Verbose,
                           TEXT("Main asset for '%s' was not in memory — only on-disk cleanup needed."),
                           *BlendName);
                }

                // Belt-and-suspenders: ensure the disk file is gone.
                // v1.15 — CHECK the delete result.  The old code logged
                // "Deleted" unconditionally while the delete failed with
                // ERROR_ACCESS_DENIED on P4-synced read-only files
                // (Revision Control provider disconnected → the checkout
                // discipline is a silent no-op), which sent the whole
                // pipeline down a false "file locked" trail.
                if (FM.FileExists(*AssetDiskPath))
                {
                    bool bDeleted = FM.Delete(*AssetDiskPath);
                    if (!bDeleted)
                    {
                        IPlatformFile& PF =
                            FPlatformFileManager::Get().GetPlatformFile();
                        if (PF.IsReadOnly(*AssetDiskPath))
                        {
                            UE_LOG(LogMapLink, Warning,
                                   TEXT("Delete failed on READ-ONLY file "
                                        "(P4-synced, provider not connected?) "
                                        "— clearing the attribute: '%s'"),
                                   *AssetDiskPath);
                            PF.SetReadOnly(*AssetDiskPath, false);
                            bDeleted = FM.Delete(*AssetDiskPath);
                        }
                    }
                    if (bDeleted)
                    {
                        UE_LOG(LogMapLink, Log,
                               TEXT("Deleted %s on disk: '%s'"),
                               bIsLevel ? TEXT(".umap") : TEXT(".uasset"),
                               *AssetDiskPath);
                    }
                    else
                    {
                        UE_LOG(LogMapLink, Error,
                               TEXT("FAILED to delete %s on disk: '%s'"),
                               bIsLevel ? TEXT(".umap") : TEXT(".uasset"),
                               *AssetDiskPath);
                    }
                }
            }
        }

        // ---------------------------------------------------------------
        // Phase 1c.1/.2 — Now delete meshes.  For BP_/LI_, the previous
        // step removed the live referencers, so ForceDeleteObjects no
        // longer has to walk + replace 50×N references.
        // ---------------------------------------------------------------
        {
            TArray<UObject*> ObjectsToDelete;
            {
                MAPLINK_SCOPED_PHASE("Phase1c.1_FindMeshAssets");
                FAssetRegistryModule& ARModule =
                    FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
                IAssetRegistry& AR = ARModule.Get();

                FARFilter Filter;
                Filter.PackagePaths.Add(FName(*MeshContentPath));
                Filter.bRecursivePaths = true;

                TArray<FAssetData> MeshAssets;
                AR.GetAssets(Filter, MeshAssets);

                for (const FAssetData& AssetData : MeshAssets)
                {
                    UObject* Asset = AssetData.GetAsset();
                    if (Asset)
                        ObjectsToDelete.Add(Asset);
                }

                // Also catch any in-memory meshes the AR missed
                TArray<UStaticMesh*> StaleMeshes;
                for (TObjectIterator<UStaticMesh> It; It; ++It)
                    if (*It && (*It)->GetPathName().StartsWith(MeshContentPath))
                        StaleMeshes.Add(*It);
                for (UStaticMesh* SM : StaleMeshes)
                    ObjectsToDelete.AddUnique(SM);
            }

            if (ObjectsToDelete.Num() > 0)
            {
                MAPLINK_SCOPED_PHASE("Phase1c.2_DeleteMeshes");
                ObjectTools::ForceDeleteObjects(ObjectsToDelete, false);
                UE_LOG(LogMapLink, Log,
                       TEXT("ForceDeleteObjects: removed %d mesh asset(s) for '%s'."),
                       ObjectsToDelete.Num(), *BlendName);
            }
        }

        if (FM.DirectoryExists(*MeshDiskDir))
        {
            MAPLINK_SCOPED_PHASE("Phase1c.3_DeleteMeshDir");
            FM.DeleteDirectory(*MeshDiskDir, false, true);
            UE_LOG(LogMapLink, Log,
                   TEXT("Deleted Meshes folder on disk for '%s'."), *BlendName);
        }

        // ---------------------------------------------------------------
        // Final GC — single pass, handles meshes + main asset orphans.
        // ---------------------------------------------------------------
        if (bIsStaticMesh)
        {
            MAPLINK_SCOPED_PHASE("Phase1c.5_GC_SM");
            CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
        }
        else
        {
            MAPLINK_SCOPED_PHASE("Phase1c.5_GC");
            CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
        }

        UE_LOG(LogMapLink, Log,
               TEXT("Nuke complete for '%s' — all assets cleared."), *BlendName);
    }

} // anonymous namespace

// ---------------------------------------------------------------------------
// Reimport entry point (linked from MapLinkPipeline.cpp)
// ---------------------------------------------------------------------------

// v1.11 — reimport begin/end signal accessors (process-wide singletons).
FOnMapLinkReimport& FMapLinkPipeline::OnReimportBegin()
{
    static FOnMapLinkReimport Delegate;
    return Delegate;
}

FOnMapLinkReimport& FMapLinkPipeline::OnReimportEnd()
{
    static FOnMapLinkReimport Delegate;
    return Delegate;
}

namespace
{
    /** RAII: broadcasts OnReimportBegin on construction and OnReimportEnd on
     *  destruction, so End fires on every exit path of RunReimportImpl
     *  (success, error-continue drain, cancel). */
    struct FMapLinkReimportSignalGuard
    {
        FString Stem;
        explicit FMapLinkReimportSignalGuard(const FString& InStem)
            : Stem(InStem)
        {
            FMapLinkPipeline::OnReimportBegin().Broadcast(Stem);
        }
        ~FMapLinkReimportSignalGuard()
        {
            FMapLinkPipeline::OnReimportEnd().Broadcast(Stem);
        }
    };
}

EMapLinkResult FMapLinkPipeline::RunReimportImpl(const FString& BlendFilePath)
{
    if (!FPaths::FileExists(BlendFilePath))
    {
        UE_LOG(LogMapLink, Error,
               TEXT("Blend file not found: '%s'."), *BlendFilePath);
        return EMapLinkResult::FbxNotFound;
    }

    const FString InputBlendName = FPaths::GetBaseFilename(BlendFilePath);
    const FString PluginBaseDir = FMapLinkPipeline::GetPluginBaseDir();

    // Reject files that don't start with BP_, LI_, or SM_
    if (!InputBlendName.StartsWith(TEXT("BP_")) &&
        !InputBlendName.StartsWith(TEXT("LI_")) &&
        !InputBlendName.StartsWith(TEXT("SM_")))
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("Skipping reimport of '%s' — filename must start with BP_, LI_, or SM_."),
               *InputBlendName);
        return EMapLinkResult::Success;
    }

    // v1.21 - resolve and version-check Blender once, before the graph is
    // built. A reimport that cannot reach Blender must say so once and stop,
    // not once per stale node.
    {
        FString PreflightError;
        EMapLinkResult PreflightResult = EMapLinkResult::Success;
        if (!FMapLinkBlenderBridge::Preflight(PreflightError, PreflightResult))
        {
            UE_LOG(LogMapLink, Error, TEXT("%s"),
                   *PreflightError.Replace(TEXT("\n"), TEXT(" ")));
            FMessageDialog::Open(EAppMsgType::Ok,
                                 FText::FromString(PreflightError));
            return PreflightResult;
        }
    }

    // v1.21 - collect per-node failures for the summary at the end.
    FMapLinkRunFailureScope MapLinkFailureScope;

    UE_LOG(LogMapLink, Log,
           TEXT("=== MapLink Reimport: %s ==="), *InputBlendName);

    // v1.7.4 — timing session (RAII: writes JSON when this scope exits)
    FMapLinkTimingSessionGuard MapLinkTimingGuard(
        TEXT("Reimport"), InputBlendName, PluginBaseDir);

    // v1.11 — pause/resume signal for read-only consumers (Asset Viewer).
    //         Begin broadcasts now; End broadcasts when this scope exits.
    FMapLinkReimportSignalGuard MapLinkReimportSignal(InputBlendName);

    // Save the editor's current level so we can restore it at the end.
    FString OriginalEditorLevelPath;
    {
        UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (EditorWorld)
        {
            OriginalEditorLevelPath = EditorWorld->GetOutermost()->GetName();
        }
    }

    // 1. Build the dependency graph.
    FMapLinkDependencyGraph Graph;
    FMapLinkDependencyNode* Root = Graph.BuildGraph(BlendFilePath, PluginBaseDir);
    FMapLinkProcessOrder ProcessOrder =
        FMapLinkDependencyGraph::FlattenLeafFirst(Root);

    // 2. Detect changes
    TArray<FString> ChangedNodes;
    {
        MAPLINK_SCOPED_PHASE("Phase0b_ChangeDetection");

        for (const FString& NodePath : ProcessOrder)
        {
            FString NodeName = FPaths::GetBaseFilename(NodePath);

            if (!NodeName.StartsWith(TEXT("BP_")) &&
                !NodeName.StartsWith(TEXT("LI_")) &&
                !NodeName.StartsWith(TEXT("SM_")))
            {
                UE_LOG(LogMapLink, Log,
                       TEXT("Skipping '%s' in change detection — not a BP_/LI_/SM_ file."),
                       *NodeName);
                continue;
            }

        FString StoredHash = ReadStoredCppHash(PluginBaseDir, NodeName);
        FString CurrentHash = FMapLinkUtils::ComputeFileMD5(NodePath);

        if (StoredHash.IsEmpty())
        {
            WriteStoredCppHash(PluginBaseDir, NodeName, CurrentHash);

            FString BlenderHash = ReadStoredBlenderHash(PluginBaseDir, NodeName);
            if (BlenderHash.IsEmpty())
            {
                ChangedNodes.Add(NodePath);
                UE_LOG(LogMapLink, Log,
                       TEXT("No stored hash for %s — treating as changed."),
                       *NodeName);
            }
            else
            {
                if (!DoesPrimaryAssetExist(NodeName))
                {
                    ChangedNodes.Add(NodePath);
                    UE_LOG(LogMapLink, Log,
                           TEXT("New dependency detected: %s — has Blender hash but no asset on disk. Treating as changed."),
                           *NodeName);
                }
                else
                {
                    UE_LOG(LogMapLink, Log,
                           TEXT("Skipping %s — no change detected (first reimport, writing C++ hash)."),
                           *NodeName);
                }
            }
            continue;
        }

        if (StoredHash != CurrentHash)
        {
            ChangedNodes.Add(NodePath);
            UE_LOG(LogMapLink, Log,
                   TEXT("Change detected: %s"), *NodeName);
        }
        else if (!DoesPrimaryAssetExist(NodeName))
        {
            ChangedNodes.Add(NodePath);
            UE_LOG(LogMapLink, Log,
                   TEXT("Asset missing for '%s' (hash unchanged) — reimporting."),
                   *NodeName);
        }
    }

    // ----- v1.7.2: Propagate changes to parent LI_/BP_ nodes -----
    // When a child SM_ changes structure, parent LI_/BP_ that embed it via
    // the swap phase have stale mesh references and must be reimported.
    {
        TSet<FString> ChangedNameSet;
        for (const FString& N : ChangedNodes)
            ChangedNameSet.Add(FPaths::GetBaseFilename(N));

        TFunction<bool(const FString&, TSet<FString>&)> HasChangedDescendant;
        HasChangedDescendant = [&](const FString& NodeName,
                                    TSet<FString>& Visited) -> bool
        {
            if (Visited.Contains(NodeName)) return false;
            Visited.Add(NodeName);

            FString AP = FMapLinkUtils::ResolveArtifactLogFile(
                NodeName / TEXT("analysis.json"));
            FMapLinkAnalysisReport R;
            if (!FMapLinkInterchangeHelper::ParseAnalysisJson(AP, R))
            { Visited.Remove(NodeName); return false; }

            for (const FMapLinkLinkedAssetEntry& L : R.LinkedAssets)
            {
                FString CN = FPaths::GetBaseFilename(L.SourceBlendPath);
                if (ChangedNameSet.Contains(CN))
                { Visited.Remove(NodeName); return true; }
                if (CN.StartsWith(TEXT("SM_")) && HasChangedDescendant(CN, Visited))
                { Visited.Remove(NodeName); return true; }
            }
            Visited.Remove(NodeName);
            return false;
        };

        for (const FString& NP : ProcessOrder)
        {
            FString NN = FPaths::GetBaseFilename(NP);
            if (ChangedNameSet.Contains(NN)) continue;
            if (!NN.StartsWith(TEXT("LI_")) && !NN.StartsWith(TEXT("BP_"))) continue;
            TSet<FString> V;
            if (HasChangedDescendant(NN, V))
            {
                ChangedNameSet.Add(NN);
                UE_LOG(LogMapLink, Log,
                       TEXT("Stale parent: '%s' has changed descendant — forcing reimport."), *NN);
            }
        }

        TArray<FString> Ordered;
        for (const FString& NP : ProcessOrder)
            if (ChangedNameSet.Contains(FPaths::GetBaseFilename(NP)))
                Ordered.Add(NP);
        ChangedNodes = MoveTemp(Ordered);
    }
    }  // end Phase0b_ChangeDetection scope (v1.7.4)

    if (ChangedNodes.Num() == 0)
    {
        UE_LOG(LogMapLink, Log, TEXT("No changes detected. Reimport skipped."));
        return EMapLinkResult::Success;
    }

    // ----- v1.9: Parallel FBX warm pass -----------------------------
    // Export every changed LI_/SM_ node whose FBX cache is stale in
    // parallel BEFORE the sequential loop below.  The loop then hits
    // IsFbxCacheFresh() for all of them.  BP_ nodes are reference-mode
    // and never launch Blender.  Failed exports fall back to the
    // sequential single launch (cache stays stale for that node).
    if (UMapLinkSettings::Get()->bParallelWarmPass)
    {
        MAPLINK_SCOPED_PHASE("Phase0c_ParallelWarm");
        TArray<FString> WarmList;
        for (const FString& NodePath : ChangedNodes)
        {
            const FString NodeName = FPaths::GetBaseFilename(NodePath);
            if (!NodeName.StartsWith(TEXT("LI_")) &&
                !NodeName.StartsWith(TEXT("SM_")))
            {
                continue;
            }
            FMapLinkUtils::WaitForExportLock(NodeName);
            if (FMapLinkUtils::IsFbxCacheFresh(NodePath, NodeName))
            {
                continue;
            }
            WarmList.Add(NodePath);
        }

        if (WarmList.Num() > 1)
        {
            FMapLinkBlenderBridge::RunBlenderExportsParallel(
                WarmList, PluginBaseDir, TEXT("reimport_warm"));
        }
    }

    // v1.13 — opt-in safety net: shelve a snapshot of everything the
    // destructive per-node loop below is about to nuke (UE packages +
    // source .blends + sidecars).  Non-fatal on any failure.
    FMapLinkSourceControl::ShelveReimportSnapshot(ChangedNodes);

    FScopedSlowTask TopTask(
        static_cast<float>(ChangedNodes.Num()),
        FText::FromString(FString::Printf(
            TEXT("MapLink: Reimporting %d changed file(s)"),
            ChangedNodes.Num())));
    TopTask.MakeDialog(/*bShowCancelButton=*/true);  // v1.9.4 — cancellable

    // 3. Process each changed node (leaf-first order)
    //
    // v1.7: Collect changed blend names for WP re-expansion after reimport.
    TArray<FString> ChangedBlendNames;
    bool bCancelled = false;

    for (const FString& NodePath : ChangedNodes)
    {
        // v1.9.4 — honour Cancel between nodes.  Reimport nukes-and-recreates
        // per node, so a node either completes or hasn't started — cancelling
        // at the boundary never leaves a half-rebuilt asset.
        if (TopTask.ShouldCancel())
        {
            bCancelled = true;
            UE_LOG(LogMapLink, Warning,
                   TEXT("Reimport cancelled by user before '%s'."),
                   *FPaths::GetBaseFilename(NodePath));
            break;
        }

        const FString NodeBlendName = FPaths::GetBaseFilename(NodePath);
        const bool bIsLevel      = NodeBlendName.StartsWith(TEXT("LI_"));
        const bool bIsStaticMesh = NodeBlendName.StartsWith(TEXT("SM_"));
        const bool bIsBPPrefix   = (!bIsLevel && !bIsStaticMesh);

        // v1.8 D7 hotfix6 — Mirror the BP Reference-Mode policy from
        // MapLinkPipeline.cpp::RunImport here in Reimport.  Without this
        // mirror, reimport silently legacy-synthesizes any BP_ stand-in
        // back into /Game/MapLink/<name>/<name>.uasset + Meshes/, even
        // though the user authored the stand-in via right-click → Create
        // MapLink Stand-in.
        //
        //  1. Detect ref-mode (proxy bIsBPReference, else sidecar binding).
        //  2. Per hotfix4 policy: ALL BP_ are ref-mode.  If no binding
        //     signal, treat as ORPHAN — placement will substitute
        //     BP_Deprecated, no synthesis happens here.
        //  3. Skip Phase 1 (Blender export), Phase 2 (Interchange), and
        //     Phase 2B/2D (BP creation+swap).  Still run NukeAssetsForBlend
        //     to clear stale legacy assets from a pre-stand-in state, and
        //     run Phase 3 (Proxy save) with the ref-mode binding stamped.
        FString RefBoundBPPath;
        FString RefStandInMode;
        bool    bIsRefModeBP = false;
        if (bIsBPPrefix)
        {
            UMapLinkProxyAsset* ExistingProxy = nullptr;
            if (FMapLinkBPReferenceMode::IsProxyReferenceMode(
                    NodeBlendName, RefBoundBPPath, &ExistingProxy))
            {
                bIsRefModeBP = true;
            }
            else
            {
                bIsRefModeBP = FMapLinkBPReferenceMode::ParseSidecarForBoundBP(
                    NodePath, RefBoundBPPath, RefStandInMode);
            }
            if (!bIsRefModeBP)
            {
                bIsRefModeBP = true;        // policy: all BP_ are ref-mode
                RefBoundBPPath.Reset();     // empty → BP_Deprecated at swap
                RefStandInMode = TEXT("orphan");
            }
        }

        ChangedBlendNames.Add(NodeBlendName);

        TopTask.EnterProgressFrame(1.f,
            FText::FromString(FString::Printf(
                TEXT("Reimporting: %s"), *NodeBlendName)));

        UE_LOG(LogMapLink, Log,
               TEXT("--- Reimporting: %s%s ---"),
               *NodeBlendName,
               bIsRefModeBP ? TEXT(" [reference-mode]") : TEXT(""));

        // v1.7.4 — per-node timing scope
        MAPLINK_SCOPED_PHASE_KEY("Node", NodeBlendName);

        // v1.13.1 — close-the-loop badge PER NODE. RunReimport's wrapper only
        // writes the status file for the top-level blend it was called with;
        // in a recursive cascade the children (e.g. LI_uneplace under a
        // LI_zidane parent) were reimported here but never got a
        // <stem>_uestatus.json, so the Blender addon showed "UE: no link".
        // Mark importing now; each exit path below writes the result.
        MapLinkUEStatus::WriteImporting(NodePath);

        // v1.8 D7 hotfix6 — Reference-mode BP_ short-circuit.
        if (bIsRefModeBP)
        {
            if (!RefBoundBPPath.IsEmpty())
            {
                UE_LOG(LogMapLink, Log,
                       TEXT("BP_ '%s' is REFERENCE-MODE (bound to '%s', "
                            "standin_mode='%s') — skipping Blender export, "
                            "mesh import, and BP synthesis."),
                       *NodeBlendName, *RefBoundBPPath, *RefStandInMode);
            }
            else
            {
                UE_LOG(LogMapLink, Warning,
                       TEXT("BP_ '%s' is ORPHAN — no reference-mode binding. "
                            "Skipping Blender export, mesh import, and BP "
                            "synthesis; placement will substitute "
                            "BP_Deprecated."),
                       *NodeBlendName);
            }

            // Nuke any stale legacy-synth artefacts (BP + Meshes folder)
            // left over from a pre-stand-in state.  No-op if there are
            // none.  Note bIsLevel=false here (we're in the BP_ branch).
            {
                MAPLINK_SCOPED_PHASE("Phase1c_NukeAssets");
                NukeAssetsForBlend(NodeBlendName, /*bIsLevel=*/false);
            }

            // Update stored hash so future reimports see this node as
            // up-to-date (skip Blender on next pass).
            WriteStoredCppHash(PluginBaseDir, NodeBlendName,
                FMapLinkUtils::ComputeFileMD5(NodePath));

            // Phase 3 — Proxy save (ref-mode flavour: stamp binding).
            {
                MAPLINK_SCOPED_PHASE("Phase3_ProxySave");
                FString ProxyPath = FString::Printf(
                    TEXT("/Game/MapLink/%s/%s_Proxy"),
                    *NodeBlendName, *NodeBlendName);

                UMapLinkProxyAsset* Proxy = LoadObject<UMapLinkProxyAsset>(
                    nullptr, *ProxyPath);
                if (!Proxy)
                {
                    UPackage* ProxyPkg = CreatePackage(*ProxyPath);
                    Proxy = NewObject<UMapLinkProxyAsset>(
                        ProxyPkg,
                        *FString::Printf(TEXT("%s_Proxy"), *NodeBlendName),
                        RF_Public | RF_Standalone);
                    FAssetRegistryModule::AssetCreated(Proxy);
                }

                Proxy->BlendFilePath        = NodePath;
                Proxy->BlendFileHash        = ReadStoredBlenderHash(PluginBaseDir, NodeBlendName);
                Proxy->LastImportTimestamp  = FDateTime::UtcNow().ToIso8601();
                Proxy->ImportedBlendModTime = IFileManager::Get().GetTimeStamp(*NodePath);  // v1.13.1
                Proxy->bIsLevel             = false;
                Proxy->bIsBPReference       = true;
                if (!RefBoundBPPath.IsEmpty())
                {
                    Proxy->BoundBlueprint = TSoftObjectPtr<UBlueprint>(
                        FSoftObjectPath(RefBoundBPPath));
                    Proxy->OriginalBoundBlueprintPath = RefBoundBPPath;
                }
                else
                {
                    Proxy->BoundBlueprint.Reset();
                    Proxy->OriginalBoundBlueprintPath.Reset();
                }
                Proxy->MarkPackageDirty();
                FMapLinkPipeline::SaveProxyPackage(Proxy);
            }

            MapLinkUEStatus::WriteResult(NodePath, EMapLinkResult::Success);  // v1.13.1
            UE_LOG(LogMapLink, Log,
                   TEXT("--- Reimport finished: %s ---"), *NodeBlendName);
            continue;   // next node — skip the legacy synth path entirely
        }

        // v1.7.5 — FBX cache pre-flight (see MapLinkPipeline.cpp for rationale).
        FMapLinkUtils::WaitForExportLock(NodeBlendName);
        if (FMapLinkUtils::IsFbxCacheFresh(NodePath, NodeBlendName))
        {
            FMapLinkTimingSession::Get().IncrementCounter(TEXT("fbx_cache_hits"));
            UE_LOG(LogMapLink, Log,
                   TEXT("[FbxCache] HIT for reimport of '%s' — skipping Blender launch."),
                   *NodeBlendName);
        }
        else
        {
            FMapLinkTimingSession::Get().IncrementCounter(TEXT("fbx_cache_misses"));
            // A. Run Blender analyze + export
            MAPLINK_SCOPED_PHASE("Phase1_BlenderExport");
            EMapLinkResult R = FMapLinkBlenderBridge::RunBlenderExport(
                NodePath, PluginBaseDir, TEXT("reimport_changed"));
            if (R == EMapLinkResult::Cancelled)
            {
                bCancelled = true;
                MapLinkUEStatus::WriteResult(NodePath, EMapLinkResult::Cancelled);  // v1.13.1
                break;   // user aborted mid-export (before any nuke)
            }
            if (R != EMapLinkResult::Success)
            {
                UE_LOG(LogMapLink, Error,
                       TEXT("Reimport Blender export failed for '%s'. Skipping."),
                       *NodeBlendName);
                FMapLinkRunFailures::Add(NodeBlendName, R,
                    TEXT("Blender export failed - nothing was deleted."));
                MapLinkUEStatus::WriteResult(NodePath, R);  // v1.13.1
                continue;
            }
        }

        // B. If ANY MapLink level is currently open in the editor, switch away
        //    first to avoid "partially loaded" crashes during nuke+recreate.
        //    Reimport will reload the correct level at the end anyway.
        if (bIsLevel)
        {
            MAPLINK_SCOPED_PHASE("Phase1b_SwitchLevelAway");
            UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
            if (EditorWorld)
            {
                FString CurrentPkgName = EditorWorld->GetOutermost()->GetName();
                UE_LOG(LogMapLink, Log,
                       TEXT("[Reimport] Current editor world: '%s', reimporting: '%s'"),
                       *CurrentPkgName, *NodeBlendName);

                // Switch away if the current level is ANY MapLink level
                // (could be the target itself, or a parent that loaded it as sub-level)
                bool bSwitchAway = CurrentPkgName.Contains(TEXT("/MapLink/"));

                // v1.13.1 — a NON-MapLink world can still hold MapLink .umaps
                // loaded (and their files locked) through ALevelInstance
                // actors — e.g. a WP host level that received a MapLink
                // expansion. That lock made SavePackage fail mid-reimport
                // (crash before v1.13.1's non-fatal save). Switch away
                // whenever the open world embeds ANY /Game/MapLink/ level.
                if (!bSwitchAway)
                {
                    for (TActorIterator<ALevelInstance> It(EditorWorld); It; ++It)
                    {
                        const FString LIPkg =
                            It->GetWorldAsset().GetLongPackageName();
                        if (LIPkg.StartsWith(TEXT("/Game/MapLink/")))
                        {
                            UE_LOG(LogMapLink, Log,
                                   TEXT("[Reimport] Editor world '%s' embeds "
                                        "MapLink Level Instance '%s' — "
                                        "switching to new map before nuke."),
                                   *CurrentPkgName, *LIPkg);
                            bSwitchAway = true;
                            break;
                        }
                    }
                }

                if (bSwitchAway)
                {
                    UE_LOG(LogMapLink, Log,
                           TEXT("[Reimport] Switching editor to a new map before nuke (was '%s')."),
                           *CurrentPkgName);
                    GEditor->CreateNewMapForEditing();
                }
            }
        }

        // v1.9.4 — Integrity gate BEFORE the destructive nuke.
        //
        // Reimport is nuke-and-recreate: it destroys the old assets and
        // rebuilds them from the freshly exported FBX + analysis.json.  If
        // the export step silently produced no usable artifacts (failed
        // mid-write, deleted cache, etc.), nuking first would leave the
        // asset destroyed with nothing to rebuild from.  Validate the
        // rebuild inputs and abort THIS node (leaving its existing assets
        // intact) before touching anything destructive.
        //
        // Note: a MISSING .fbx is legitimate for a .blend with no own
        // geometry (Interchange still imports empties-only), so only a
        // 0-byte .fbx is treated as a failed export.  A missing
        // analysis.json is always fatal — Phase 2 reads it for transforms.
        const FString FbxPath = FMapLinkUtils::ResolveArtifactExportFile(
            NodeBlendName / (NodeBlendName + TEXT(".fbx")));
        {
            const FString AnalysisPath = FMapLinkUtils::ResolveArtifactLogFile(
                NodeBlendName / TEXT("analysis.json"));
            const bool bAnalysisOk = FPaths::FileExists(AnalysisPath);

            int64 FbxSize = -1;   // -1 = absent (allowed for no-geometry blends)
            if (FPaths::FileExists(FbxPath))
            {
                FbxSize = IFileManager::Get().FileSize(*FbxPath);
            }

            // v1.15 — an ABSENT fbx is only legitimate when the blend has
            // no own geometry.  The analysis says which case we're in —
            // gate on it (a cleaned Saved/ dir left hash+analysis intact
            // with the FBX gone, and the old gate nuked assets it could
            // not rebuild).
            const bool bFbxRequiredButAbsent =
                (FbxSize < 0) &&
                FMapLinkUtils::AnalysisDeclaresOwnGeo(NodeBlendName);

            // v1.15 — deletability gate: the nuke must be able to delete
            // and recreate this node's package files.  P4-synced files
            // are READ-ONLY on disk, and with no Revision Control
            // provider connected the checkout discipline is a silent
            // no-op: every delete then fails with ERROR_ACCESS_DENIED
            // (observed live: nuke "succeeded", recreation aborted, node
            // left destroyed).  Clear the attribute up front — recursing
            // over the node's content dir covers the level/BP file, the
            // meshes AND the proxy.  If a file still can't be made
            // writable, abort this node BEFORE anything is destroyed.
            bool bAllDeletable = true;
            {
                const FString NodeContentDir = FPaths::GetPath(
                    FPackageName::LongPackageNameToFilename(
                        FString::Printf(TEXT("/Game/MapLink/%s/%s"),
                                        *NodeBlendName, *NodeBlendName),
                        FPackageName::GetAssetPackageExtension()));
                IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
                int32 ClearedCount = 0;
                PF.IterateDirectoryRecursively(*NodeContentDir,
                    [&PF, &bAllDeletable, &ClearedCount]
                    (const TCHAR* Path, bool bIsDir) -> bool
                    {
                        if (!bIsDir && PF.IsReadOnly(Path))
                        {
                            PF.SetReadOnly(Path, false);
                            ++ClearedCount;
                            if (PF.IsReadOnly(Path))
                            {
                                bAllDeletable = false;
                                return false;   // stop iterating
                            }
                        }
                        return true;
                    });
                if (ClearedCount > 0)
                {
                    UE_LOG(LogMapLink, Warning,
                           TEXT("[Reimport] Cleared the read-only attribute on "
                                "%d file(s) under '%s' (P4-synced with no "
                                "Revision Control provider connected?  "
                                "Reconcile the folder in P4V afterwards)."),
                           ClearedCount, *NodeContentDir);
                }
                if (!bAllDeletable)
                {
                    FMapLinkTimingSession::Get().IncrementCounter(
                        TEXT("reimport_integrity_aborts"));
                    UE_LOG(LogMapLink, Error,
                           TEXT("[Reimport] Deletability gate FAILED for '%s' "
                                "— a package file is read-only and could not "
                                "be made writable.  SKIPPING nuke to preserve "
                                "the existing assets.  Connect Revision "
                                "Control (or check out /Game/MapLink/%s in "
                                "P4V) and reimport again."),
                           *NodeBlendName, *NodeBlendName);
                    FMapLinkRunFailures::Add(NodeBlendName,
                        EMapLinkResult::LevelLocked,
                        TEXT("Its assets could not be checked out or deleted."));
                    MapLinkUEStatus::WriteResult(NodePath, EMapLinkResult::LevelLocked);
                    continue;   // next node — old assets left untouched
                }
            }

            if (!bAnalysisOk || FbxSize == 0 || bFbxRequiredButAbsent)
            {
                FMapLinkTimingSession::Get().IncrementCounter(
                    TEXT("reimport_integrity_aborts"));
                UE_LOG(LogMapLink, Error,
                       TEXT("[Reimport] Integrity gate FAILED for '%s' "
                            "(analysis=%s, fbxBytes=%lld) — SKIPPING nuke to "
                            "preserve the existing assets.  Re-run the Blender "
                            "export for this file and reimport again."),
                       *NodeBlendName,
                       bAnalysisOk ? TEXT("present") : TEXT("MISSING"),
                       FbxSize);
                FMapLinkRunFailures::Add(NodeBlendName,
                    EMapLinkResult::FbxNotFound,
                    TEXT("Export artifacts missing - the existing assets were "
                         "left alone. Re-save the .blend and reimport."));
                MapLinkUEStatus::WriteResult(NodePath, EMapLinkResult::FbxNotFound);  // v1.13.1
                continue;   // next node — old assets left untouched
            }
        }

        // C. Nuke ALL old assets (meshes + BP/Level, keeps Proxy and _GP)
        {
            MAPLINK_SCOPED_PHASE("Phase1c_NukeAssets");
            NukeAssetsForBlend(NodeBlendName, bIsLevel);
        }

        // C. Import FBX meshes to same paths (FbxPath validated above)
        TArray<FMapLinkImportedMesh> Meshes;
        TArray<FMapLinkImportedEmpty> Empties;

        {
            MAPLINK_SCOPED_PHASE("Phase2_Interchange");
            EMapLinkResult R = FMapLinkInterchangeHelper::ImportAndCollectResults(
                FbxPath, NodeBlendName, PluginBaseDir, Meshes, Empties);
            if (R != EMapLinkResult::Success)
            {
                UE_LOG(LogMapLink, Error,
                       TEXT("Reimport FBX import failed for '%s'. Skipping."),
                       *NodeBlendName);
                FMapLinkRunFailures::Add(NodeBlendName, R,
                    TEXT("The FBX could not be imported after the old assets "
                         "were removed - reimport this file again."));
                MapLinkUEStatus::WriteResult(NodePath, R);  // v1.13.1
                continue;
            }
        }

        // D. Recreate BP or Level at same path
        if (bIsStaticMesh)
        {
            UE_LOG(LogMapLink, Log,
                   TEXT("SM_ reimport '%s': %d mesh(es) reimported."),
                   *NodeBlendName, Meshes.Num());
        }
        else if (bIsLevel)
        {
            {
                MAPLINK_SCOPED_PHASE("Phase2A_CreateLevel");
                const EMapLinkResult LevelRes =
                    FMapLinkInterchangeHelper::CreateLevelFromResults(
                        NodeBlendName, Meshes, Empties);
                if (LevelRes != EMapLinkResult::Success)
                {
                    // LevelLocked (target .umap held by another process) or a
                    // save failure. CreateLevelFromResults already aborted
                    // BEFORE SavePackage, so nothing fatal happened — surface a
                    // clear error and skip the swap for this node rather than
                    // crashing or building on a half-written level.
                    UE_LOG(LogMapLink, Error,
                           TEXT("Reimport: level (re)creation failed for '%s' "
                                "(code %d) — skipping. If it is a locked .umap, "
                                "close the level that embeds it and reimport."),
                           *NodeBlendName, static_cast<int32>(LevelRes));
                    FMapLinkRunFailures::Add(NodeBlendName, LevelRes,
                        TEXT("The level could not be recreated."));
                    MapLinkUEStatus::WriteResult(NodePath, LevelRes);  // v1.13.1
                    continue;
                }
            }

            FString DestPath = FString::Printf(
                TEXT("/Game/MapLink/%s/%s"), *NodeBlendName, *NodeBlendName);
            {
                MAPLINK_SCOPED_PHASE("Phase2C_LevelSwap");
                FMapLinkLevelHelper::SwapPlaceholdersInLevel(DestPath, NodeBlendName);
            }
        }
        else
        {
            {
                MAPLINK_SCOPED_PHASE("Phase2B_CreateBP");
                FMapLinkBlueprintHelper::CreateBlueprint(
                    NodeBlendName, Meshes, Empties);
            }
            {
                MAPLINK_SCOPED_PHASE("Phase2D_BPSwap");
                FMapLinkBlueprintHelper::SwapPlaceholdersInBlueprint(NodeBlendName);
            }
        }

        // E. Update stored hashes
        WriteStoredCppHash(PluginBaseDir, NodeBlendName,
            FMapLinkUtils::ComputeFileMD5(NodePath));

        // F. Create / update Proxy
        {
            MAPLINK_SCOPED_PHASE("Phase3_ProxySave");
            FString ProxyPath = FString::Printf(
                TEXT("/Game/MapLink/%s/%s_Proxy"), *NodeBlendName, *NodeBlendName);

            UMapLinkProxyAsset* Proxy = LoadObject<UMapLinkProxyAsset>(
                nullptr, *ProxyPath);

            if (!Proxy)
            {
                // v1.11 — a node newly introduced during this reimport (e.g. a
                // freshly-added SM_ dependency placed into the LI) has no proxy
                // yet. Create it so the asset becomes a first-class MapLink
                // proxy, exactly as a cold import would. Without this, new SM_
                // deps were left as plain meshes with no UMapLinkProxyAsset.
                UPackage* ProxyPkg = CreatePackage(*ProxyPath);
                Proxy = NewObject<UMapLinkProxyAsset>(
                    ProxyPkg,
                    *FString::Printf(TEXT("%s_Proxy"), *NodeBlendName),
                    RF_Public | RF_Standalone);
                FAssetRegistryModule::AssetCreated(Proxy);
            }

            Proxy->BlendFilePath       = NodePath;
            Proxy->BlendFileHash       = ReadStoredBlenderHash(PluginBaseDir, NodeBlendName);
            Proxy->LastImportTimestamp = FDateTime::UtcNow().ToIso8601();
            Proxy->ImportedBlendModTime = IFileManager::Get().GetTimeStamp(*NodePath);  // v1.13.1
            Proxy->bIsLevel            = bIsLevel;

            // PrimaryAsset: LI_ → generated UWorld; SM_ → none (matches the
            // cold-import semantics in MapLinkPipeline.cpp Phase 3).
            if (bIsLevel)
            {
                const FString PrimaryAssetPath = FString::Printf(
                    TEXT("/Game/MapLink/%s/%s"), *NodeBlendName, *NodeBlendName);
                Proxy->PrimaryAsset = TSoftObjectPtr<UObject>(
                    FSoftObjectPath(PrimaryAssetPath));
            }
            else
            {
                Proxy->PrimaryAsset.Reset();
            }

            Proxy->MarkPackageDirty();
            FMapLinkPipeline::SaveProxyPackage(Proxy);
        }

        MapLinkUEStatus::WriteResult(NodePath, EMapLinkResult::Success);  // v1.13.1
        UE_LOG(LogMapLink, Log,
               TEXT("--- Reimport finished: %s ---"), *NodeBlendName);
    }

    // Auto-save all dirty packages to prevent data loss
    UE_LOG(LogMapLink, Log, TEXT("Auto-saving all dirty packages..."));
    {
        MAPLINK_SCOPED_PHASE("Phase4_SaveDirtyPackages");
        FEditorFileUtils::SaveDirtyPackages(
            /*bPromptUserToSave=*/false,
            /*bSaveMapPackages=*/true,
            /*bSaveContentPackages=*/true);
    }

    // ----- WP Re-expansion -----
    // v1.9 — re-expand every UMapLinkEditorState expansion record whose
    // dependency tree contains a changed node.  Skipped on cancel.
    //
    // v1.13.1/v1.14 — this MUST run BEFORE the deferred outliner refresh
    // below, so the refresh (and, in the switched-away case, the level
    // restore) sees the re-expanded actors persisted on disk.
    if (!bCancelled && ChangedBlendNames.Num() > 0)
    {
        MAPLINK_SCOPED_PHASE("Phase6_WPReExpansion");
        UE_LOG(LogMapLink, Log,
               TEXT("Checking WP re-expansion for %d changed node(s)..."),
               ChangedBlendNames.Num());
        FMapLinkWPExpansionHelper::ReExpandIfNeeded(ChangedBlendNames);

        // Persist any packages the re-expansion dirtied so the deferred
        // refresh/restore below sees them on disk (WP external actor
        // packages).
        FEditorFileUtils::SaveDirtyPackages(
            /*bPromptUserToSave=*/false,
            /*bSaveMapPackages=*/true,
            /*bSaveContentPackages=*/true);
    }

    // v1.14 — the v1.13.x blanket deferred close→reopen of the editor level
    // is GONE.  Root cause of the stale/corrupt outliner after the
    // nuke-and-recreate pass (both "reimport changed" and "force full
    // reimport" run through this same tail):
    //   • loaded ALevelInstance actors kept referencing the DESTROYED
    //     in-memory /Game/MapLink/ worlds ("unloaded Level Instance" rows),
    //   • FActorFolders kept registrations for folders whose actors were
    //     deleted (phantom folders),
    //   • nothing rebroadcast the actor list, so the outliner never rebuilt.
    // RefreshEditorWorldAfterReimport repairs all three IN PLACE (force
    // LI reload + GC of the stale worlds, empty-folder prune, actor-list
    // broadcast) — no reopen needed.  Still deferred + idle-gated: the
    // repair must run after this call stack unwinds and async loading
    // settles (the same timing lesson as v1.13.1).
    //
    // One case still needs a map load: when the pre-nuke switch-away guard
    // moved the editor OFF the original level (it is now on a blank/temp
    // map).  That LoadMap is not an outliner fix — it just restores the
    // level the user had open, and it is a plain fresh open, not a
    // close→reopen of an already-open level.
    if (!OriginalEditorLevelPath.IsEmpty())
    {
        FString MapFilename;
        const bool bResolved =
            FPackageName::TryConvertLongPackageNameToFilename(
                OriginalEditorLevelPath, MapFilename,
                FPackageName::GetMapPackageExtension()) &&
            FPaths::FileExists(MapFilename);
        const FString RestoreTarget =
            bResolved ? MapFilename : OriginalEditorLevelPath;
        const FString OriginalPackage = OriginalEditorLevelPath;
        const TArray<FString> ChangedForRefresh = ChangedBlendNames;

        UE_LOG(LogMapLink, Log,
               TEXT("Scheduling deferred in-place outliner refresh "
                    "(editor level: '%s')."), *OriginalEditorLevelPath);

        FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateLambda(
                [RestoreTarget, OriginalPackage, ChangedForRefresh]
                (float) -> bool
            {
                // WAIT until the editor is idle — the reimport's async
                // streams + asset validation must finish flushing first
                // (returning true reschedules for the next tick).
                if (IsAsyncLoading() || !GEditor)
                {
                    return true;
                }

                UWorld* Current = GEditor->GetEditorWorldContext().World();
                const FString CurrentPackage =
                    (Current && Current->GetPackage())
                        ? Current->GetPackage()->GetName() : FString();

                if (CurrentPackage == OriginalPackage)
                {
                    // Level still open — repair its outliner in place.
                    UE_LOG(LogMapLink, Log,
                           TEXT("[Reimport] Deferred in-place outliner "
                                "refresh of '%s'."), *OriginalPackage);
                    FMapLinkWPExpansionHelper::RefreshEditorWorldAfterReimport(
                        ChangedForRefresh);
                }
                else
                {
                    // The pre-nuke guard switched away — restore the user's
                    // level (fresh from disk; GC first so the recreated
                    // packages can't re-associate stale in-memory worlds).
                    UE_LOG(LogMapLink, Log,
                           TEXT("[Reimport] Restoring switched-away editor "
                                "level '%s'."), *RestoreTarget);
                    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
                    FEditorFileUtils::LoadMap(RestoreTarget);

                    // v1.15.1 — deferred, idle-gated prune of the restored
                    // world.  History: a synchronous prune here crashed
                    // (ActorFolderTreeItem.cpp:287) because loaded-actor
                    // emptiness lied while WP cells streamed in.  The
                    // prune is now actor-descriptor based (unloaded
                    // actors protect their folders), and this call is
                    // still deferred until async loading settles — belt
                    // and suspenders.
                    const FString RestoredPackage = OriginalPackage;
                    FTSTicker::GetCoreTicker().AddTicker(
                        FTickerDelegate::CreateLambda(
                            [RestoredPackage](float) -> bool
                        {
                            if (IsAsyncLoading() || !GEditor)
                            {
                                return true;   // reschedule
                            }
                            UWorld* World =
                                GEditor->GetEditorWorldContext().World();
                            if (World &&
                                World->GetPackage() &&
                                World->GetPackage()->GetName() == RestoredPackage)
                            {
                                FMapLinkWPExpansionHelper::
                                    PruneEmptyMapLinkFolders(World);
                                if (GEngine)
                                {
                                    GEngine->BroadcastLevelActorListChanged();
                                }
                            }
                            return false;      // one-shot
                        }),
                        0.5f);
                }
                return false;   // one-shot
            }),
            0.1f);
    }

    // v1.9 — remember the last imported blend for the Manager window.
    {
        UMapLinkEditorState* EditorState = UMapLinkEditorState::Get();
        EditorState->LastImportedBlendName = InputBlendName;
        EditorState->Save();
    }

    if (bCancelled)
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("=== MapLink Reimport CANCELLED: %s (completed nodes saved) ==="),
               *InputBlendName);
        return EMapLinkResult::Cancelled;
    }

    // v1.21 - a node that failed no longer ends in "Reimport complete".
    if (FMapLinkRunFailures::HasAny())
    {
        UE_LOG(LogMapLink, Error,
               TEXT("=== MapLink Reimport FINISHED WITH %d FAILURE(S): %s "
                    "(%d file(s) updated) ==="),
               FMapLinkRunFailures::Num(), *InputBlendName,
               ChangedNodes.Num());
        FMapLinkRunFailures::ShowSummary(TEXT("reimport"), InputBlendName);
        return FMapLinkRunFailures::FirstResult();
    }

    UE_LOG(LogMapLink, Log,
           TEXT("=== MapLink Reimport Complete: %s (%d file(s) updated) ==="),
           *InputBlendName, ChangedNodes.Num());

    return EMapLinkResult::Success;
}
