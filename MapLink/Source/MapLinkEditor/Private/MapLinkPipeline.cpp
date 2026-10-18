// Copyright MapModelers. All Rights Reserved.
//
// v1.4.5 — Optimisations:
//   - Skip unchanged dependency nodes during import: if proxy exists,
//     hash matches, AND primary asset exists on disk, skip entirely.
//   - Removed redundant Phase 0 analyze: BuildGraph now handles its own
//     analysis caching (launches Blender only if analysis.json is missing
//     or the .blend hash changed).
//
// v1.7 — WP Expansion:
//   - After import completes, check if any proxy has bExpandIntoWP=true
//     and trigger auto-expansion into the target WP level.

#include "MapLinkPipeline.h"
#include "MapLinkUEStatus.h"   // v1.13 — close-the-loop status file
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
#include "MapLinkSourceControl.h"    // v1.9 -- unified P4 changelists
#include "MapLinkEditorState.h"      // v1.9 -- Manager window state
#include "MapLinkSettings.h"         // v1.9 -- warm-pass toggle
#include "MapLinkNotify.h"           // v1.21 -- MapLinkResultToText
#include "Misc/MessageDialog.h"      // v1.21 -- one failure summary
#include "Misc/PackageName.h"        // v1.21 -- stem must be a package name
#include "Engine/Blueprint.h"        // v1.8 — TSoftObjectPtr<UBlueprint>
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/DateTime.h"
#include "Misc/ScopedSlowTask.h"
#include "Misc/SecureHash.h"
#include "FileHelpers.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SavePackage.h"

// ---------------------------------------------------------------------------
// Local helpers
// ---------------------------------------------------------------------------

namespace
{
    /** Check if a node can be skipped during import:
     *  - Proxy asset exists with a stored hash
     *  - Stored hash matches current .blend file
     *  - Primary asset (BP .uasset or Level .umap) exists on disk */
    bool CanSkipNode(const FString& NodeBlendPath,
                     const FString& NodeBlendName,
                     const FString& PluginBaseDir)
    {
        // 1. Check for stored C++ hash
        FString CppHashPath = FMapLinkUtils::ResolveArtifactLogFile(
            NodeBlendName / TEXT("filehash_cpp.txt"));
        FString StoredHash;
        if (!FFileHelper::LoadFileToString(StoredHash, *CppHashPath))
        {
            return false;  // Never imported before
        }
        StoredHash.TrimStartAndEndInline();
        if (StoredHash.IsEmpty())
        {
            return false;
        }

        // 2. Compare with current file hash
        FString CurrentHash = FMapLinkUtils::ComputeFileMD5(NodeBlendPath);
        if (CurrentHash.IsEmpty() || StoredHash != CurrentHash)
        {
            return false;  // File changed
        }

        // 3. Verify primary asset exists on disk
        //    SM_ files don't produce a BP/Level — check that the Meshes folder exists instead.
        bool bIsStaticMesh = NodeBlendName.StartsWith(TEXT("SM_"));
        if (bIsStaticMesh)
        {
            FString MeshDirPath = FPackageName::LongPackageNameToFilename(
                FString::Printf(TEXT("/Game/MapLink/%s/Meshes"), *NodeBlendName));
            if (!IFileManager::Get().DirectoryExists(*MeshDirPath))
            {
                return false;  // Meshes were deleted manually
            }
        }
        else
        {
            bool bIsLevel = NodeBlendName.StartsWith(TEXT("LI_"));

            // v1.8 — BP Reference Mode: there is NO synthesized
            // /Game/MapLink/<name>/<name>.uasset to check for.  The
            // Proxy carries the binding; verify the proxy + the bound
            // BP file exist instead.
            if (!bIsLevel)
            {
                FString BoundBPPath;
                UMapLinkProxyAsset* Proxy = nullptr;
                if (FMapLinkBPReferenceMode::IsProxyReferenceMode(
                        NodeBlendName, BoundBPPath, &Proxy))
                {
                    // Proxy exists + bIsBPReference == true.  Skipping
                    // is safe IF the bound BP is still resolvable OR
                    // BP_Deprecated will substitute (we don't try to
                    // distinguish; either way, no synthesis to re-do).
                    return true;
                }
            }

            FString AssetPkgPath = FString::Printf(
                TEXT("/Game/MapLink/%s/%s"), *NodeBlendName, *NodeBlendName);
            FString AssetDiskPath = FPackageName::LongPackageNameToFilename(
                AssetPkgPath,
                bIsLevel ? FPackageName::GetMapPackageExtension()
                         : FPackageName::GetAssetPackageExtension());

            if (!FPaths::FileExists(AssetDiskPath))
            {
                return false;  // Asset was deleted manually
            }
        }

        return true;  // Safe to skip
    }
}

// ---------------------------------------------------------------------------
// v1.21 - run failures
// ---------------------------------------------------------------------------

namespace
{
    struct FMapLinkRunFailureEntry
    {
        FString        Node;
        EMapLinkResult Code = EMapLinkResult::Success;
        FString        Detail;
    };

    int32 GRunFailureDepth = 0;
    bool  GRunSummaryShown  = false;
    TArray<FMapLinkRunFailureEntry> GRunFailures;
}

void FMapLinkRunFailures::Begin()
{
    if (GRunFailureDepth == 0)
    {
        GRunFailures.Reset();
        GRunSummaryShown = false;
    }
    ++GRunFailureDepth;
}

void FMapLinkRunFailures::End()
{
    GRunFailureDepth = FMath::Max(0, GRunFailureDepth - 1);
    if (GRunFailureDepth == 0)
    {
        GRunFailures.Reset();
    }
}

void FMapLinkRunFailures::Add(const FString& NodeName, EMapLinkResult Code,
                              const FString& Detail)
{
    FMapLinkRunFailureEntry Entry;
    Entry.Node   = NodeName;
    Entry.Code   = Code;
    Entry.Detail = Detail;
    GRunFailures.Add(MoveTemp(Entry));
}

bool  FMapLinkRunFailures::HasAny() { return GRunFailures.Num() > 0; }
bool  FMapLinkRunFailures::WasSummaryShown() { return GRunSummaryShown; }
int32 FMapLinkRunFailures::Num()    { return GRunFailures.Num();    }

EMapLinkResult FMapLinkRunFailures::FirstResult()
{
    return GRunFailures.Num() > 0
        ? GRunFailures[0].Code
        : EMapLinkResult::Success;
}

void FMapLinkRunFailures::ShowSummary(const FString& Operation,
                                      const FString& Subject)
{
    if (GRunFailures.Num() == 0)
    {
        return;
    }

    FString Body = FString::Printf(
        TEXT("MapLink %s of '%s' finished, but %d file(s) failed.\n\n"),
        *Operation, *Subject, GRunFailures.Num());

    // Twelve is enough to see the shape of the problem; a hundred rows in a
    // modal is a wall nobody reads.
    const int32 MaxListed = 12;
    for (int32 i = 0; i < GRunFailures.Num() && i < MaxListed; ++i)
    {
        const FMapLinkRunFailureEntry& Entry = GRunFailures[i];
        Body += FString::Printf(TEXT("    %s\n        %s\n"),
            *Entry.Node, *MapLinkResultToText(Entry.Code).ToString());
        if (!Entry.Detail.IsEmpty())
        {
            Body += FString::Printf(TEXT("        %s\n"), *Entry.Detail);
        }
    }
    if (GRunFailures.Num() > MaxListed)
    {
        Body += FString::Printf(TEXT("    ... and %d more.\n"),
                                GRunFailures.Num() - MaxListed);
    }

    Body += TEXT("\nEverything that did import is saved. Fix the files above "
                 "and import them again.");

    UE_LOG(LogMapLink, Error, TEXT("%s"), *Body);
    GRunSummaryShown = true;
    FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Body));
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

EMapLinkResult FMapLinkPipeline::RunImport(const FString& BlendFilePath)
{
    // v1.13 — close-the-loop status file for the Blender addon badge
    // (best-effort; see MapLinkUEStatus.h).
    MapLinkUEStatus::WriteImporting(BlendFilePath);
    const EMapLinkResult Result = RunImportImpl(BlendFilePath);
    MapLinkUEStatus::WriteResult(BlendFilePath, Result);
    return Result;
}

EMapLinkResult FMapLinkPipeline::RunImportImpl(const FString& BlendFilePath)
{
    if (!FPaths::FileExists(BlendFilePath))
    {
        UE_LOG(LogMapLink, Error,
               TEXT("Blend file not found: '%s'."), *BlendFilePath);
        return EMapLinkResult::FbxNotFound;
    }

    const FString InputBlendName = FPaths::GetBaseFilename(BlendFilePath);
    const FString PluginBaseDir = GetPluginBaseDir();

    // Reject files that don't start with BP_, LI_, or SM_
    if (!InputBlendName.StartsWith(TEXT("BP_")) &&
        !InputBlendName.StartsWith(TEXT("LI_")) &&
        !InputBlendName.StartsWith(TEXT("SM_")))
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("Skipping '%s' — filename must start with BP_, LI_, or SM_."),
               *InputBlendName);
        return EMapLinkResult::Success;
    }

    // v1.21 - the stem becomes /Game/MapLink/<stem>/<stem>. A space or an
    // apostrophe in it is not a package name, and UE fails much later in a
    // way nobody traces back to the filename.
    {
        FText PackageError;
        const FString CandidatePackage = FString::Printf(
            TEXT("/Game/MapLink/%s/%s"), *InputBlendName, *InputBlendName);
        if (!FPackageName::IsValidLongPackageName(
                CandidatePackage, /*bIncludeReadOnlyRoots=*/true, &PackageError))
        {
            const FString Msg = FString::Printf(TEXT(
                "MapLink cannot import '%s.blend'.\n\n"
                "The file name has to work as an Unreal asset name, and this "
                "one does not:\n    %s\n\n"
                "Rename the .blend using letters, digits and underscores only "
                "- no spaces, dots, apostrophes or accents - then import it "
                "again."),
                *InputBlendName, *PackageError.ToString());
            UE_LOG(LogMapLink, Error, TEXT("%s"),
                   *Msg.Replace(TEXT("\n"), TEXT(" ")));
            FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(Msg));
            return EMapLinkResult::InvalidAssetName;
        }
    }

    // v1.21 - resolve and version-check Blender ONCE, before the graph is
    // built: BuildGraph launches Blender itself for any missing analysis.
    // Without this the "Blender not found" dialog fired once per stale node.
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
           TEXT("=== MapLink Import: %s ==="), *InputBlendName);

    // v1.9 — every package saved during this session stacks into the
    // "[MapLink] Import" pending changelist.
    FMapLinkSourceControl::SetSessionDescription(TEXT("[MapLink] Import"));
    FMapLinkSourceControl::ResetChangelistCache();

    // v1.7.4 — timing session (RAII: writes JSON when this scope exits)
    FMapLinkTimingSessionGuard MapLinkTimingGuard(
        TEXT("Import"), InputBlendName, PluginBaseDir);

    // ----- Build dependency graph -----
    // v1.4.5: No separate Phase 0 analyze call. BuildGraph handles its own
    // analysis caching — it only launches Blender for nodes whose
    // analysis.json is missing or whose .blend hash has changed.
    FScopedSlowTask TopTask(5.f,
        FText::FromString(FString::Printf(
            TEXT("MapLink: Importing %s"), *InputBlendName)));
    TopTask.MakeDialog(/*bShowCancelButton=*/true);  // v1.9.4 — cancellable

    FMapLinkDependencyGraph Graph;
    FMapLinkDependencyNode* Root = Graph.BuildGraph(BlendFilePath, PluginBaseDir);
    FMapLinkProcessOrder ProcessOrder = FMapLinkDependencyGraph::FlattenLeafFirst(Root);

    UE_LOG(LogMapLink, Log,
           TEXT("Dependency order (%d nodes):"), ProcessOrder.Num());
    for (int32 i = 0; i < ProcessOrder.Num(); ++i)
    {
        UE_LOG(LogMapLink, Log, TEXT("  [%d] %s"), i, *ProcessOrder[i]);
    }

    TopTask.EnterProgressFrame(1.f);

    // ----- v1.9: Parallel FBX warm pass -----------------------------
    // Collect every LI_/SM_ node that the loop below WOULD launch
    // Blender for (not skippable, FBX cache stale) and export them all
    // in parallel up-front.  The sequential loop then hits the cache.
    // BP_ nodes are reference-mode and never launch Blender.
    if (UMapLinkSettings::Get()->bParallelWarmPass)
    {
        MAPLINK_SCOPED_PHASE("Phase0c_ParallelWarm");
        TArray<FString> WarmList;
        for (const FString& NodeBlendPath : ProcessOrder)
        {
            const FString NodeBlendName = FPaths::GetBaseFilename(NodeBlendPath);
            if (!NodeBlendName.StartsWith(TEXT("LI_")) &&
                !NodeBlendName.StartsWith(TEXT("SM_")))
            {
                continue;
            }
            if (CanSkipNode(NodeBlendPath, NodeBlendName, PluginBaseDir))
            {
                continue;
            }
            FMapLinkUtils::WaitForExportLock(NodeBlendName);
            if (FMapLinkUtils::IsFbxCacheFresh(NodeBlendPath, NodeBlendName))
            {
                continue;
            }
            WarmList.Add(NodeBlendPath);
        }

        if (WarmList.Num() > 1)
        {
            FMapLinkBlenderBridge::RunBlenderExportsParallel(
                WarmList, PluginBaseDir, TEXT("import_warm"));
        }
    }

    // ----- Process each node leaf-first -----
    int32 SkippedCount = 0;
    bool  bCancelled   = false;
    for (const FString& NodeBlendPath : ProcessOrder)
    {
        // v1.9.4 — honour the Cancel button between nodes.  (A single
        // in-flight Blender export is interrupted by the bridge's own
        // ShouldCancel/timeout check; this catches it at node boundaries.)
        if (TopTask.ShouldCancel())
        {
            bCancelled = true;
            UE_LOG(LogMapLink, Warning,
                   TEXT("Import cancelled by user before '%s'."),
                   *FPaths::GetBaseFilename(NodeBlendPath));
            break;
        }

        const FString NodeBlendName = FPaths::GetBaseFilename(NodeBlendPath);

        // Skip nodes that are not BP_, LI_, or SM_ prefixed (belt-and-suspenders)
        if (!NodeBlendName.StartsWith(TEXT("BP_")) &&
            !NodeBlendName.StartsWith(TEXT("LI_")) &&
            !NodeBlendName.StartsWith(TEXT("SM_")))
        {
            UE_LOG(LogMapLink, Log,
                   TEXT("Skipping node '%s' — not a BP_/LI_/SM_ file."),
                   *NodeBlendName);
            continue;
        }

        // v1.4.5: Skip unchanged nodes — if the .blend hasn't changed
        // and the UE5 asset already exists, no need to re-export/re-import.
        if (CanSkipNode(NodeBlendPath, NodeBlendName, PluginBaseDir))
        {
            UE_LOG(LogMapLink, Log,
                   TEXT("Skipping '%s' — unchanged and asset exists."),
                   *NodeBlendName);
            ++SkippedCount;
            continue;
        }

        const bool bIsLevel      = NodeBlendName.StartsWith(TEXT("LI_"));
        const bool bIsStaticMesh = NodeBlendName.StartsWith(TEXT("SM_"));

        // v1.8 — Detect BP Reference Mode UPFRONT so we can skip the
        // expensive Blender export + Interchange mesh import.  For a
        // reference-mode BP_ the bound user-authored BP carries its
        // own real meshes — synthesizing /Game/MapLink/<name>/Meshes/
        // produces dead assets nothing references.
        //
        // v1.8 D7 — Detection is PROXY-FIRST, sidecar-fallback:
        //   1. If a Proxy exists at /Game/MapLink/<name>/<name>_Proxy
        //      with bIsBPReference == true → trust the proxy.  This
        //      makes ref-mode RESILIENT to the sidecar being rewritten
        //      by external scripts (e.g. the MapLink_Blender addon's
        //      save_post hook running maplink_export.py and producing
        //      a generic sidecar without the v1.8 fields).
        //   2. Else fall back to parsing the sidecar (this is the
        //      first-time-import case, before the Proxy exists).
        FString RefBoundBPPath;
        FString RefStandInMode;
        bool    bIsRefModeBP = false;
        if (!bIsLevel && !bIsStaticMesh)
        {
            UMapLinkProxyAsset* ExistingProxy = nullptr;
            if (FMapLinkBPReferenceMode::IsProxyReferenceMode(
                    NodeBlendName, RefBoundBPPath, &ExistingProxy))
            {
                bIsRefModeBP = true;
                // standin_mode isn't stored on the proxy; leave empty.
                // It's purely informational in the log.
            }
            else
            {
                bIsRefModeBP = FMapLinkBPReferenceMode::ParseSidecarForBoundBP(
                    NodeBlendPath, RefBoundBPPath, RefStandInMode);
            }

            // v1.8 D7 hotfix4 — POLICY: ALL BP_ assets are reference-mode.
            // Legacy BP synthesis from a Blender hierarchy is no longer
            // supported.  The only way for a BP_ to gain a real bound
            // class is right-click → Create MapLink Stand-in in UE.
            //
            // A BP_ blend that arrives without a binding (no proxy
            // flagged ref-mode AND no sidecar bound_blueprint_path) is
            // treated as an "orphan" — it still goes through ref-mode
            // (skips Phase 1 + Phase 2 + synth) and placement substitutes
            // BP_Deprecated.  The user can then either delete the .blend
            // or create a proper stand-in binding for it.
            if (!bIsRefModeBP)
            {
                bIsRefModeBP = true;        // ← force ref-mode regardless
                RefBoundBPPath.Reset();     // ← empty binding → BP_Deprecated
                RefStandInMode = TEXT("orphan");
            }
        }

        UE_LOG(LogMapLink, Log,
               TEXT("--- Processing: %s (type: %s%s) ---"),
               *NodeBlendName,
               bIsLevel ? TEXT("Level") :
               bIsStaticMesh ? TEXT("StaticMesh") : TEXT("Blueprint"),
               bIsRefModeBP ? TEXT(" [reference-mode]") : TEXT(""));

        // v1.7.4 — per-node timing scope
        MAPLINK_SCOPED_PHASE_KEY("Node", NodeBlendName);

        // v1.8 — Reference-mode BP_: skip ALL of Phase 1 (Blender export),
        // Phase 2 (Interchange mesh import), and Phase 2B/2D (BP synthesis).
        // Only Phase 3 (Proxy create/update) runs — to capture/refresh
        // the binding info.
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
                // v1.8 D7 hotfix4 — orphan BP_ (no binding sidecar, no
                // ref-mode proxy).  Legacy synthesis is disabled; the
                // placement helpers will substitute BP_Deprecated.
                UE_LOG(LogMapLink, Warning,
                       TEXT("BP_ '%s' is ORPHAN — no reference-mode binding "
                            "(no /Game/MapLink/%s/%s_Proxy flagged ref-mode, "
                            "NO sidecar bound_blueprint_path).  Skipping "
                            "Blender export, mesh import, and BP synthesis; "
                            "placement will substitute BP_Deprecated.  "
                            "To bind a real BP, right-click the user-"
                            "authored BP in UE → MapLink → Create MapLink "
                            "Stand-in.  To remove an unwanted .blend, use "
                            "MapLink → Delete MapLink Stand-in."),
                       *NodeBlendName, *NodeBlendName, *NodeBlendName);
            }
            // Fall through to Phase 3 (Proxy save) below; everything
            // between here and the Proxy block is gated on !bIsRefModeBP.
        }
        else
        {
        // v1.8 D7 hotfix4 — DEAD CODE PATH.  With the policy change in
        // the ref-mode detection block above (all BP_ → bIsRefModeBP=true),
        // this branch only runs for LI_ (levels) and SM_ (static meshes).
        // The old "BP_ legacy synthesis fallback" path is no longer
        // reachable for BP_ assets.
        // v1.7.5 — FBX cache pre-flight.  If the Blender artifacts for this
        // .blend are already up-to-date (e.g. the MapLink_Blender addon's
        // save_post subprocess produced them), skip the Phase1 launch
        // entirely.  Wait briefly for an in-flight save_post export first.
        FMapLinkUtils::WaitForExportLock(NodeBlendName);
        if (FMapLinkUtils::IsFbxCacheFresh(NodeBlendPath, NodeBlendName))
        {
            FMapLinkTimingSession::Get().IncrementCounter(TEXT("fbx_cache_hits"));
            UE_LOG(LogMapLink, Log,
                   TEXT("[FbxCache] HIT for '%s' — skipping Blender launch."),
                   *NodeBlendName);
        }
        else
        {
            FMapLinkTimingSession::Get().IncrementCounter(TEXT("fbx_cache_misses"));
            // Phase 1: Blender analyze + export (single launch via "both" mode)
            MAPLINK_SCOPED_PHASE("Phase1_BlenderExport");
            const FString Reason = (NodeBlendName == InputBlendName)
                ? FString(TEXT("import_root"))
                : FString(TEXT("import_dep"));
            EMapLinkResult R = FMapLinkBlenderBridge::RunBlenderExport(
                NodeBlendPath, PluginBaseDir, Reason);
            if (R == EMapLinkResult::Cancelled)
            {
                bCancelled = true;
                break;   // user aborted mid-export
            }
            if (R != EMapLinkResult::Success)
            {
                LogError(NodeBlendName, TEXT("Phase1_BlenderExport"), R,
                         TEXT("Blender export failed."));
                // Continue with other nodes if this one fails
                continue;
            }
        }

        // FBX path (may not exist if blend has no own geometry — that's OK)
        FString FbxPath = FMapLinkUtils::ResolveArtifactExportFile(
            NodeBlendName / (NodeBlendName + TEXT(".fbx")));

        // Phase 2: Import mesh assets + collect transforms from analysis.json
        TArray<FMapLinkImportedMesh> ImportedMeshes;
        TArray<FMapLinkImportedEmpty> ImportedEmpties;
        {
            MAPLINK_SCOPED_PHASE("Phase2_Interchange");
            EMapLinkResult R = FMapLinkInterchangeHelper::ImportAndCollectResults(
                FbxPath, NodeBlendName, PluginBaseDir,
                ImportedMeshes, ImportedEmpties);
            if (R != EMapLinkResult::Success)
            {
                LogError(NodeBlendName, TEXT("Phase2_ImportCollect"), R,
                         TEXT("FBX import / analysis parse failed."));
                continue;
            }
        }

        if (bIsStaticMesh)
        {
            // SM_ path: mesh assets are the final deliverables.
            // No Blueprint, no Level, no swap phase.
            UE_LOG(LogMapLink, Log,
                   TEXT("SM_ node '%s': %d mesh(es) imported. No BP/Level created."),
                   *NodeBlendName, ImportedMeshes.Num());
        }
        else if (bIsLevel)
        {
            // Phase 2A: Create level with placed actors
            EMapLinkResult R;
            {
                MAPLINK_SCOPED_PHASE("Phase2A_CreateLevel");
                R = FMapLinkInterchangeHelper::CreateLevelFromResults(
                    NodeBlendName, ImportedMeshes, ImportedEmpties);
            }
            if (R != EMapLinkResult::Success)
            {
                LogError(NodeBlendName, TEXT("Phase2A_CreateLevel"), R,
                         TEXT("Failed to create level."));
                continue;
            }

            // Phase 2C: Swap placeholders in the level
            FString DestLevelPath = FString::Printf(
                TEXT("/Game/MapLink/%s/%s"), *NodeBlendName, *NodeBlendName);
            {
                MAPLINK_SCOPED_PHASE("Phase2C_LevelSwap");
                R = FMapLinkLevelHelper::SwapPlaceholdersInLevel(
                    DestLevelPath, NodeBlendName);
            }
            if (R != EMapLinkResult::Success)
            {
                LogError(NodeBlendName, TEXT("Phase2C_LevelSwap"), R,
                         TEXT("Level placeholder swap failed."));
                // Non-fatal: level is usable, just with unswapped placeholders
            }
        }
        else
        {
            // v1.9.4 — UNREACHABLE by policy.  Every BP_ is forced into
            // reference-mode at the top of the loop (bIsRefModeBP = true),
            // so the !bIsRefModeBP block only ever runs for LI_ (levels) or
            // SM_ (static meshes), both handled above.  The old BP_ legacy
            // synthesis path (Phase2B_CreateBP / Phase2D_BPSwap) was removed.
            // If we somehow land here, log loudly and skip rather than
            // silently resurrecting dead synthesis.
            UE_LOG(LogMapLink, Error,
                   TEXT("[Pipeline] Unexpected non-ref-mode BP_ node '%s' "
                        "reached the removed legacy-synthesis branch — "
                        "skipping.  This indicates a ref-mode detection "
                        "regression."),
                   *NodeBlendName);
            continue;
        }
        } // end of !bIsRefModeBP block

        // ----- Create / update Proxy Asset -----
        {
            MAPLINK_SCOPED_PHASE("Phase3_ProxySave");
            FString ProxyPath = FString::Printf(
                TEXT("/Game/MapLink/%s/%s_Proxy"), *NodeBlendName, *NodeBlendName);

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

            Proxy->BlendFilePath = NodeBlendPath;
            Proxy->BlendFileHash = ReadHashFromLogFile(NodeBlendName);
            Proxy->LastImportTimestamp = FDateTime::UtcNow().ToIso8601();
            // v1.13.1 — stamp the imported .blend's mtime for the Manager's
            // mtime-vs-mtime staleness check (see UMapLinkProxyAsset).
            Proxy->ImportedBlendModTime =
                IFileManager::Get().GetTimeStamp(*NodeBlendPath);
            Proxy->bIsLevel = bIsLevel;

            // v1.8 — BP Reference Mode: stamp the proxy with the bound
            // BP info detected upfront (bIsRefModeBP / RefBoundBPPath).
            Proxy->bIsBPReference = bIsRefModeBP;
            if (bIsRefModeBP)
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

            // v1.8 — PrimaryAsset semantics:
            //   - Legacy BP_/LI_: points at the synthesized asset under
            //     /Game/MapLink/<name>/<name>.  Double-click on the
            //     proxy → opens the synthesized BP/Level editor.
            //   - Reference-mode BP_: NULL.  Double-click → opens the
            //     proxy's property panel so the user can inspect the
            //     binding fields (BoundBlueprint, OriginalBoundBlueprintPath).
            //     To open the actual bound BP, click the BoundBlueprint
            //     soft reference in the property panel.
            //   - SM_: NULL (no primary asset).
            if (!bIsStaticMesh && !bIsRefModeBP)
            {
                FString PrimaryAssetPath = FString::Printf(
                    TEXT("/Game/MapLink/%s/%s"), *NodeBlendName, *NodeBlendName);
                Proxy->PrimaryAsset = TSoftObjectPtr<UObject>(
                    FSoftObjectPath(PrimaryAssetPath));
            }
            else
            {
                Proxy->PrimaryAsset.Reset();
            }

            Proxy->MarkPackageDirty();
            SaveProxyPackage(Proxy);

            // Write C++ hash for fast change detection
            {
                FString HashStr = FMapLinkUtils::ComputeFileMD5(NodeBlendPath);
                if (!HashStr.IsEmpty())
                {
                    FString CppHashPath = FMapLinkUtils::GetArtifactLogDir() /
                        NodeBlendName / TEXT("filehash_cpp.txt");
                    IFileManager::Get().MakeDirectory(
                        *FPaths::GetPath(CppHashPath), /*Tree=*/true);
                    FFileHelper::SaveStringToFile(HashStr, *CppHashPath,
                        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
                }
            }
        }

        UE_LOG(LogMapLink, Log,
               TEXT("--- Finished: %s ---"), *NodeBlendName);
    }

    if (SkippedCount > 0)
    {
        UE_LOG(LogMapLink, Log,
               TEXT("Skipped %d unchanged node(s)."), SkippedCount);
    }

    TopTask.EnterProgressFrame(3.f);

    // Auto-save all dirty packages (also done on cancel so partial work
    // already written to disk isn't lost).
    UE_LOG(LogMapLink, Log, TEXT("Auto-saving all dirty packages..."));
    {
        MAPLINK_SCOPED_PHASE("Phase4_SaveDirtyPackages");
        FEditorFileUtils::SaveDirtyPackages(
            /*bPromptUserToSave=*/false,
            /*bSaveMapPackages=*/true,
            /*bSaveContentPackages=*/true);
    }

    // v1.9.4 — user cancelled: persist what we have and bail out before
    // opening the primary asset / running WP expansion.
    if (bCancelled)
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("=== MapLink Import CANCELLED: %s (partial work saved) ==="),
               *InputBlendName);
        FMapLinkBPReferenceMode::WriteAndClearPlacementsReport(InputBlendName);
        return EMapLinkResult::Cancelled;
    }

    // Phase 3: Open the primary asset for the input_blend (not dependencies)
    // SM_ files have no primary BP/Level to open — skip
    if (!InputBlendName.StartsWith(TEXT("SM_")))
    {
        MAPLINK_SCOPED_PHASE("Phase5_OpenPrimaryAsset");
        OpenPrimaryAsset(InputBlendName,
                         InputBlendName.StartsWith(TEXT("LI_")));
    }

    TopTask.EnterProgressFrame(1.f);

    // ----- Auto WP Expansion -----
    {
        MAPLINK_SCOPED_PHASE("Phase6_WPExpansion");
        // v1.9 — expansion state lives in UMapLinkEditorState records
        // (managed from the MapLink Manager window), not on proxies.
        // ReExpandIfNeeded re-expands every recorded root whose tree
        // contains the imported blend.
        TArray<FString> ImportedNames;
        ImportedNames.Add(InputBlendName);
        FMapLinkWPExpansionHelper::ReExpandIfNeeded(ImportedNames);
    }

    // v1.9 — remember the last imported blend for the Manager window.
    {
        UMapLinkEditorState* EditorState = UMapLinkEditorState::Get();
        EditorState->LastImportedBlendName = InputBlendName;
        EditorState->Save();
    }

    // v1.8 — Flush BP-reference-mode placement records to a side report.
    FMapLinkBPReferenceMode::WriteAndClearPlacementsReport(InputBlendName);

    // v1.21 - a node that failed no longer vanishes into the log.
    if (FMapLinkRunFailures::HasAny())
    {
        UE_LOG(LogMapLink, Error,
               TEXT("=== MapLink Import FINISHED WITH %d FAILURE(S): %s ==="),
               FMapLinkRunFailures::Num(), *InputBlendName);
        FMapLinkRunFailures::ShowSummary(TEXT("import"), InputBlendName);
        return FMapLinkRunFailures::FirstResult();
    }

    UE_LOG(LogMapLink, Log,
           TEXT("=== MapLink Import Complete: %s ==="), *InputBlendName);

    return EMapLinkResult::Success;
}

EMapLinkResult FMapLinkPipeline::RunReimport(const FString& BlendFilePath)
{
    // v1.9 — packages saved during reimport stack into a separate
    // "[MapLink] Reimport" pending changelist.
    FMapLinkSourceControl::SetSessionDescription(TEXT("[MapLink] Reimport"));
    FMapLinkSourceControl::ResetChangelistCache();

    // v1.13 — close-the-loop status file for the Blender addon badge
    // (best-effort; see MapLinkUEStatus.h).
    MapLinkUEStatus::WriteImporting(BlendFilePath);
    const EMapLinkResult Result = RunReimportImpl(BlendFilePath);
    MapLinkUEStatus::WriteResult(BlendFilePath, Result);
    return Result;
}

void FMapLinkPipeline::OpenPrimaryAsset(
    const FString& BlendName, bool bIsLevel)
{
    FString AssetPath = FString::Printf(
        TEXT("/Game/MapLink/%s/%s"), *BlendName, *BlendName);

    UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
    if (!Asset)
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("Primary asset not found at '%s'."), *AssetPath);
        return;
    }

    GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Asset);
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

FString FMapLinkPipeline::GetPluginBaseDir()
{
    return FMapLinkUtils::GetPluginBaseDir();
}

// ---------------------------------------------------------------------------
// v1.19 — vendor a foreign .blend into the project before importing it
// ---------------------------------------------------------------------------

bool FMapLinkPipeline::VendorBlendIntoProject(
    const FString& SourceBlend,
    const FString& DestRoot,
    bool bOverwrite,
    FString& OutVendoredBlend,
    FString& OutError)
{
    OutVendoredBlend.Reset();
    OutError.Reset();

    if (!FPaths::FileExists(SourceBlend))
    {
        OutError = FString::Printf(TEXT("Source not found:\n%s"), *SourceBlend);
        return false;
    }
    if (!FPaths::FileExists(DestRoot / TEXT("maplink_project.json")))
    {
        OutError = FString::Printf(
            TEXT("No maplink_project.json in the destination root:\n%s"),
            *DestRoot);
        return false;
    }

    const FString PluginBaseDir = GetPluginBaseDir();
    const FString ScriptRel = TEXT("blender/maplink_vendor_cli.py");
    if (!FPaths::FileExists(PluginBaseDir / ScriptRel))
    {
        OutError = FString::Printf(
            TEXT("Vendor helper missing:\n%s\n\nReinstall the plugin's Blender "
                 "addon files."), *(PluginBaseDir / ScriptRel));
        return false;
    }

    // No --addon-package: the CLI discovers it. maplink_install_addon.py names
    // the installed module after its source FOLDER, so it is "blender" for a
    // repo install and whatever the directory is called otherwise. A name
    // hardcoded here would enable nothing, the operator would be missing, and
    // the only symptom would be that no files were copied.
    FString Args;
    Args += FString::Printf(TEXT("--source \"%s\""), *SourceBlend);
    Args += FString::Printf(TEXT(" --dest-root \"%s\""), *DestRoot);
    if (bOverwrite)
    {
        Args += TEXT(" --overwrite");
    }

    // Everything the copy touches lands in one changelist, like every other
    // MapLink operation that writes to the Blender tree.
    FMapLinkSourceControl::SetSessionDescription(
        TEXT("[MapLink] Copy asset into project"));

    // The bridge needs a positional .blend for --background; the CLI ignores
    // it (it drives its own operator call), so the source doubles as the
    // throwaway.
    const EMapLinkResult R = FMapLinkBlenderBridge::RunBlenderScriptGeneric(
        SourceBlend,
        ScriptRel,
        PluginBaseDir,
        Args,
        TEXT("Copy asset into project"));

    if (R != EMapLinkResult::Success)
    {
        OutError = FString::Printf(
            TEXT("Blender could not copy the asset in (code %d). See the "
                 "Output Log for the [VendorCLI] lines."),
            static_cast<int32>(R));
        return false;
    }

    // Where the copy landed. The CLI prints it, but the layout is
    // deterministic — <root>/<TYPE>/<Name>/<Name>.blend — so deriving it here
    // costs nothing and does not depend on parsing stdout the bridge may have
    // truncated.
    const FString Stem = FPaths::GetBaseFilename(SourceBlend);
    FString Type;
    if (!Stem.Split(TEXT("_"), &Type, nullptr))
    {
        OutError = FString::Printf(
            TEXT("'%s' is not a MapLink asset name (expected LI_/SM_/BP_)."),
            *Stem);
        return false;
    }

    const FString Candidate =
        DestRoot / Type / Stem / (Stem + TEXT(".blend"));
    if (!FPaths::FileExists(Candidate))
    {
        OutError = FString::Printf(
            TEXT("The copy reported success but nothing is at:\n%s"),
            *Candidate);
        return false;
    }

    OutVendoredBlend = Candidate;
    UE_LOG(LogMapLink, Log,
           TEXT("[Pipeline] Vendored '%s' into '%s' -> %s"),
           *SourceBlend, *DestRoot, *Candidate);
    return true;
}

void FMapLinkPipeline::LogError(
    const FString& BlendName,
    const FString& Phase,
    EMapLinkResult Code,
    const FString& Message)
{
    UE_LOG(LogMapLink, Error,
           TEXT("[%s] Phase=%s Code=%d: %s"),
           *BlendName, *Phase, static_cast<int32>(Code), *Message);

    // v1.21 - the run has to end on this, not on Success.
    FMapLinkRunFailures::Add(BlendName, Code, Message);

    // Append to errors.json
    FString ErrorLogPath =
        FMapLinkUtils::GetArtifactLogDir() / BlendName / TEXT("errors.json");

    // Read existing errors
    TArray<TSharedPtr<FJsonValue>> ErrorArray;
    FString ExistingJson;
    if (FFileHelper::LoadFileToString(ExistingJson, *ErrorLogPath))
    {
        TSharedRef<TJsonReader<>> Reader =
            TJsonReaderFactory<>::Create(ExistingJson);
        FJsonSerializer::Deserialize(Reader, ErrorArray);
    }

    // Add new error
    TSharedRef<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
    ErrorObj->SetStringField(TEXT("phase"), Phase);
    ErrorObj->SetNumberField(TEXT("code"), static_cast<int32>(Code));
    ErrorObj->SetStringField(TEXT("message"), Message);
    ErrorObj->SetStringField(TEXT("timestamp"),
                             FDateTime::UtcNow().ToIso8601());
    ErrorArray.Add(MakeShared<FJsonValueObject>(ErrorObj));

    // Write back
    FString OutputString;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
    FJsonSerializer::Serialize(ErrorArray, Writer);

    // Ensure directory exists
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    PlatformFile.CreateDirectoryTree(*FPaths::GetPath(ErrorLogPath));

    FFileHelper::SaveStringToFile(OutputString, *ErrorLogPath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

FString FMapLinkPipeline::ReadHashFromLogFile(const FString& BlendName)
{
    FString HashPath =
        FMapLinkUtils::ResolveArtifactLogFile(BlendName / TEXT("filehash.txt"));

    FString Hash;
    FFileHelper::LoadFileToString(Hash, *HashPath);
    Hash.TrimStartAndEndInline();
    return Hash;
}

void FMapLinkPipeline::SaveProxyPackage(UMapLinkProxyAsset* Proxy)
{
    if (!Proxy) return;

    UPackage* Package = Proxy->GetOutermost();
    FString PackageFilename = FPackageName::LongPackageNameToFilename(
        Package->GetName(), FPackageName::GetAssetPackageExtension());

    // v1.9 — check out an existing depot file before writing, mark
    // brand-new files for add afterwards (session changelist).  This is
    // what finally puts MapLink proxy .uassets into Perforce.
    FMapLinkSourceControl::CheckoutBeforeSave(Package);

    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    // v1.13.1 — never route save errors to GError (fatal appError on any
    // failed save). Log instead; callers/logs surface the failure.
    SaveArgs.Error     = GLog;
    SaveArgs.SaveFlags = SAVE_NoError;
    if (!UPackage::SavePackage(Package, Proxy, *PackageFilename, SaveArgs))
    {
        UE_LOG(LogMapLink, Error,
               TEXT("Failed to save proxy package '%s'."),
               *Package->GetName());
    }

    FMapLinkSourceControl::AddAfterSave(Package);
}

bool FMapLinkPipeline::SetProxyShowInReimport(const FString& BlendFilePath, bool bShow)
{
    const FString Stem = FPaths::GetBaseFilename(BlendFilePath);
    const FString ProxyPath = FString::Printf(
        TEXT("/Game/MapLink/%s/%s_Proxy"), *Stem, *Stem);

    UMapLinkProxyAsset* Proxy = LoadObject<UMapLinkProxyAsset>(nullptr, *ProxyPath);
    if (!Proxy)
    {
        UE_LOG(LogMapLink, Warning,
            TEXT("[Reimport flag] proxy not found at %s"), *ProxyPath);
        return false;
    }

    if (Proxy->bShowInReimportMenu != bShow)
    {
        Proxy->bShowInReimportMenu = bShow;
        Proxy->MarkPackageDirty();
        SaveProxyPackage(Proxy);
    }
    UE_LOG(LogMapLink, Log, TEXT("[Reimport flag] %s -> bShowInReimportMenu=%d"),
           *ProxyPath, bShow ? 1 : 0);
    return true;
}
