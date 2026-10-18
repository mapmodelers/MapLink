// Copyright MapModelers. All Rights Reserved.
//
// v1.5 — SM_ leaf resolution + _GP wrapper lookup
// v1.6 — SM_ composite recursive expansion with tagging
// v1.6.1 — FIX: multi-mesh SM_ leaf (OwnGeo > 1, LinkedAssets == 0) now
//          routes through ExpandSM instead of the single-mesh leaf path.
//          Composite detection: LinkedAssets > 0 || OwnGeo > 1.

#include "MapLinkLevelHelper.h"
#include "MapLinkUtils.h"
#include "MapLinkInterchangeHelper.h"
#include "MapLinkPipeline.h"
#include "MapLinkBPReferenceMode.h"  // v1.8
#include "MapLinkProxyAsset.h"        // v1.8
#include "Editor.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Blueprint.h"
#include "Components/StaticMeshComponent.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"
#include "FileHelpers.h"
#include "LevelInstance/LevelInstanceSubsystem.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"
#include "UObject/SavePackage.h"

// ---------------------------------------------------------------------------
// ExpandSM — recursive SM_ composite expansion (v1.6)
// ---------------------------------------------------------------------------

TArray<FExpandedMesh> FMapLinkLevelHelper::ExpandSM(
    const FString& AssetName,
    const FTransform& ParentTransform,
    TSet<FString>& VisitedSet)
{
    TArray<FExpandedMesh> Results;

    // Cycle detection
    if (VisitedSet.Contains(AssetName))
    {
        UE_LOG(LogMapLink, Error,
               TEXT("ExpandSM: cycle detected for '%s' — aborting expansion."),
               *AssetName);
        return Results;
    }
    VisitedSet.Add(AssetName);

    // Read analysis.json for this SM_
    FString PluginBaseDir = FMapLinkPipeline::GetPluginBaseDir();
    FString AnalysisPath = FMapLinkUtils::ResolveArtifactLogFile(
        AssetName / TEXT("analysis.json"));

    FMapLinkAnalysisReport Report;
    if (!FMapLinkInterchangeHelper::ParseAnalysisJson(AnalysisPath, Report))
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("ExpandSM: failed to read analysis.json for '%s'."), *AssetName);
        VisitedSet.Remove(AssetName);
        return Results;
    }

    int32 GlobalIndex = 0;

    // 1. Own geometry — each becomes a mesh placement
    for (const FMapLinkOwnGeoEntry& Geo : Report.OwnGeo)
    {
        // FbxFactory names assets <FBXfilename>_<nodename> (dots→underscores)
        // ONLY for multi-mesh imports; a single mesh node is named after the
        // FBX file itself (v1.13.1 FIX — a composite with exactly one own
        // mesh previously resolved to a non-existent asset).
        FString SanitizedGeoName = Geo.Name.Replace(TEXT("."), TEXT("_"));
        FString MeshAssetName = (Report.OwnGeo.Num() == 1)
            ? AssetName
            : FString::Printf(TEXT("%s_%s"), *AssetName, *SanitizedGeoName);
        FString MeshPath = FString::Printf(
            TEXT("/Game/MapLink/%s/Meshes/%s"), *AssetName, *MeshAssetName);

        FTransform FinalTransform = Geo.WorldTransform * ParentTransform;

        FExpandedMesh Entry;
        Entry.MeshPath = MeshPath;
        Entry.Transform = FinalTransform;
        Entry.SourceComposite = AssetName;
        Entry.Index = GlobalIndex++;
        Results.Add(Entry);
    }

    // 2. Linked SM_ assets — recursive expansion
    for (const FMapLinkLinkedAssetEntry& Linked : Report.LinkedAssets)
    {
        FString SubName = FPaths::GetBaseFilename(Linked.SourceBlendPath);

        FTransform SubTransform = Linked.WorldTransform * ParentTransform;

        // v1.8.1 — BP_ child of an SM_ composite: emit a BPReference entry so
        // the caller resolves and spawns the bound (reference-mode) BP instead
        // of silently dropping it.  (Previously this branch hit the skip below.)
        if (SubName.StartsWith(TEXT("BP_")))
        {
            FExpandedMesh BPEntry;
            BPEntry.Kind = EMapLinkExpandedKind::BPReference;
            BPEntry.BPNodeName = SubName;
            BPEntry.Transform = SubTransform;
            BPEntry.SourceComposite = AssetName;
            BPEntry.Index = GlobalIndex++;
            Results.Add(BPEntry);
            continue;
        }

        if (!SubName.StartsWith(TEXT("SM_")))
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("ExpandSM: SM_ composite '%s' references unsupported '%s' — skipping."),
                   *AssetName, *SubName);
            continue;
        }

        // Check if sub needs expansion (has linked assets OR multiple own-geo meshes)
        FString SubAnalysisPath = FMapLinkUtils::ResolveArtifactLogFile(
            SubName / TEXT("analysis.json"));
        FMapLinkAnalysisReport SubReport;
        bool bSubNeedsExpansion = false;
        if (FMapLinkInterchangeHelper::ParseAnalysisJson(SubAnalysisPath, SubReport))
        {
            // v1.6.1 FIX: multi-mesh SM_ leaf also needs expansion
            bSubNeedsExpansion = (SubReport.LinkedAssets.Num() > 0 ||
                                  SubReport.OwnGeo.Num() > 1);
        }

        if (bSubNeedsExpansion)
        {
            // Recursive expansion (composite or multi-mesh leaf)
            TArray<FExpandedMesh> SubResults = ExpandSM(SubName, SubTransform, VisitedSet);
            for (FExpandedMesh& Sub : SubResults)
            {
                Sub.Index = GlobalIndex++;
                Results.Add(Sub);
            }
        }
        else
        {
            // True leaf SM_ — single mesh
            FString MeshPath = FString::Printf(
                TEXT("/Game/MapLink/%s/Meshes/%s"), *SubName, *SubName);

            FExpandedMesh Entry;
            Entry.MeshPath = MeshPath;
            Entry.Transform = SubTransform;
            Entry.SourceComposite = AssetName;
            Entry.Index = GlobalIndex++;
            Results.Add(Entry);
        }
    }

    VisitedSet.Remove(AssetName);
    return Results;
}

// ---------------------------------------------------------------------------
// SpawnExpandedBPReference (v1.8.1) — place a BP_ child of an SM_ composite
// ---------------------------------------------------------------------------

AActor* FMapLinkLevelHelper::SpawnExpandedBPReference(
    UWorld* World,
    const FString& BPNodeName,
    const FTransform& WorldTransform,
    const FString& OwnerAssetPath,
    const FString& PlacementContext,
    const FName& CleanupTag)
{
    if (!World)
    {
        return nullptr;
    }

    FString RefBoundBPPath;
    UMapLinkProxyAsset* RefProxy = nullptr;
    const bool bRefMode = FMapLinkBPReferenceMode::IsProxyReferenceMode(
        BPNodeName, RefBoundBPPath, &RefProxy);

    if (!bRefMode)
    {
        UE_LOG(LogMapLinkBPStandIn, Warning,
               TEXT("  Nested BP_ '%s' has no reference-mode proxy at "
                    "/Game/MapLink/%s/%s_Proxy — cannot place.  Create the "
                    "stand-in, then reimport the owning SM_."),
               *BPNodeName, *BPNodeName, *BPNodeName);
        return nullptr;
    }

    bool bWasDeprecated = false;
    UClass* SpawnClass = FMapLinkBPReferenceMode::ResolveBoundClass(
        RefProxy, bWasDeprecated);

    FMapLinkBPReferenceMode::FPlacement Rec;
    Rec.ProxyPath          = RefProxy ? RefProxy->GetPathName() : FString();
    Rec.BoundBlueprintPath = RefBoundBPPath;
    Rec.PlacementContext   = PlacementContext;
    Rec.OwnerAssetPath     = OwnerAssetPath;

    if (!SpawnClass)
    {
        UE_LOG(LogMapLinkBPStandIn, Error,
               TEXT("  Nested BP_ '%s' placement failed: neither bound BP "
                    "'%s' nor BP_Deprecated could be loaded."),
               *BPNodeName, *RefBoundBPPath);
        Rec.Status = FMapLinkBPReferenceMode::EPlacementStatus::Missing;
        FMapLinkBPReferenceMode::RecordPlacement(Rec);
        return nullptr;
    }

    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride =
        ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    AActor* Spawned = World->SpawnActor<AActor>(
        SpawnClass, WorldTransform, SpawnParams);
    if (Spawned)
    {
        Spawned->SetActorScale3D(WorldTransform.GetScale3D());
        if (!CleanupTag.IsNone())
        {
            Spawned->Tags.Add(CleanupTag);
        }

        if (bWasDeprecated)
        {
            Spawned->SetActorLabel(FString::Printf(TEXT("MISSING_%s"), *BPNodeName));
            Spawned->Tags.Add(FName(*FString::Printf(
                TEXT("MapLinkMissingBP:%s"), *RefBoundBPPath)));
            UE_LOG(LogMapLinkBPStandIn, Warning,
                   TEXT("  Nested BP_ '%s' → BP_Deprecated substituted "
                        "(bound '%s' missing)."),
                   *BPNodeName, *RefBoundBPPath);
        }
        else
        {
            Spawned->SetActorLabel(BPNodeName);
            UE_LOG(LogMapLink, Log,
                   TEXT("  Expanded nested BP_ '%s' → bound BP '%s'."),
                   *BPNodeName, *RefBoundBPPath);
        }
    }

    Rec.Status = bWasDeprecated
        ? FMapLinkBPReferenceMode::EPlacementStatus::DeprecatedSubstituted
        : FMapLinkBPReferenceMode::EPlacementStatus::Ok;
    FMapLinkBPReferenceMode::RecordPlacement(Rec);

    return Spawned;
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

EMapLinkResult FMapLinkLevelHelper::SwapPlaceholdersInLevel(
    const FString& LevelPath,
    const FString& BlendName)
{
    // Load the level
    FEditorFileUtils::LoadMap(LevelPath);
    UWorld* LIWorld = GEditor->GetEditorWorldContext().World();
    if (!LIWorld)
    {
        UE_LOG(LogMapLink, Error,
               TEXT("Failed to load level '%s' for placeholder swap."), *LevelPath);
        return EMapLinkResult::TempLevelMissing;
    }

    FString PluginBaseDir = FMapLinkPipeline::GetPluginBaseDir();

    TArray<FMapLinkStubRecord> SwapLog;

    // --- v1.6: Clean up any previously-expanded SM_ actors (tagged MapLinkExpanded:*) ---
    {
        TArray<AActor*> ExpandedToRemove;
        for (TActorIterator<AActor> It(LIWorld); It; ++It)
        {
            for (const FName& Tag : (*It)->Tags)
            {
                if (Tag.ToString().StartsWith(TEXT("MapLinkExpanded:")))
                {
                    ExpandedToRemove.Add(*It);
                    break;
                }
            }
        }
        for (AActor* Actor : ExpandedToRemove)
        {
            LIWorld->DestroyActor(Actor);
        }
        if (ExpandedToRemove.Num() > 0)
        {
            UE_LOG(LogMapLink, Log,
                   TEXT("Cleaned up %d previously-expanded SM_ actors."),
                   ExpandedToRemove.Num());
        }
    }

    // Collect actors to process (can't modify while iterating)
    // ONLY process actors explicitly tagged MapLinkPlaceholder during creation.
    // This avoids false positives from user-placed actors or engine actors.
    struct FSwapCandidate
    {
        AActor* Actor;
        FString ActorLabel;
        FString AssetName;
        FTransform Transform;
        FName   FolderPath;   // v1.14 — swapped actors inherit the
                              // placeholder's outliner folder (NAME_None
                              // when folder creation is off — a no-op)
    };
    TArray<FSwapCandidate> Candidates;

    for (TActorIterator<AActor> It(LIWorld); It; ++It)
    {
        AActor* Actor = *It;

        // Only process actors we explicitly tagged as placeholders
        if (!Actor->Tags.Contains(FName(TEXT("MapLinkPlaceholder"))))
        {
            continue;
        }

        FString ActorLabel = Actor->GetActorLabel();
        if (ActorLabel.IsEmpty())
        {
            continue;
        }

        FSwapCandidate Candidate;
        Candidate.Actor = Actor;
        Candidate.ActorLabel = ActorLabel;
        Candidate.AssetName = FMapLinkUtils::StripBlenderSuffix(Candidate.ActorLabel);
        Candidate.Transform = Actor->GetActorTransform();
        Candidate.FolderPath = Actor->GetFolderPath();   // v1.14
        Candidates.Add(Candidate);
    }

    for (const FSwapCandidate& Candidate : Candidates)
    {
        const FString& AssetName = Candidate.AssetName;
        const FTransform& PlaceholderTransform = Candidate.Transform;
        const FString& ActorLabel = Candidate.ActorLabel;

        // v1.14 — replacements inherit the placeholder's outliner folder
        // (set by CreateLevelFromResults when FolderScope == AllLevels;
        // NAME_None otherwise, which makes this a no-op).
        auto ApplyFolder = [&Candidate](AActor* A)
        {
            if (A && !Candidate.FolderPath.IsNone())
            {
                A->SetFolderPath(Candidate.FolderPath);
            }
        };

        // ==============================================================
        // SM_ resolution (v1.5 leaf + v1.6 composite + v1.6.1 multi-mesh)
        // ==============================================================
        if (AssetName.StartsWith(TEXT("SM_")))
        {
            // Determine leaf vs composite/multi-mesh from analysis.json
            FString SubAnalysisPath = FMapLinkUtils::ResolveArtifactLogFile(
                AssetName / TEXT("analysis.json"));
            FMapLinkAnalysisReport SubReport;
            bool bNeedsExpansion = false;
            if (FMapLinkInterchangeHelper::ParseAnalysisJson(SubAnalysisPath, SubReport))
            {
                // v1.6.1 FIX: multi-mesh SM_ (OwnGeo > 1) also needs expansion
                bNeedsExpansion = (SubReport.LinkedAssets.Num() > 0 ||
                                   SubReport.OwnGeo.Num() > 1);
            }

            LIWorld->DestroyActor(Candidate.Actor);

            if (bNeedsExpansion)
            {
                // v1.6 / v1.6.1: Recursive expansion (composite or multi-mesh)
                TSet<FString> Visited;
                TArray<FExpandedMesh> Expanded = ExpandSM(AssetName, PlaceholderTransform, Visited);

                for (int32 i = 0; i < Expanded.Num(); ++i)
                {
                    // v1.8.1 — nested BP_ reference expanded from the composite
                    if (Expanded[i].Kind == EMapLinkExpandedKind::BPReference)
                    {
                        SpawnExpandedBPReference(
                            LIWorld,
                            Expanded[i].BPNodeName,
                            Expanded[i].Transform,
                            LevelPath,
                            TEXT("bp_nested"),
                            FName(*FString::Printf(
                                TEXT("MapLinkExpanded:%s:%d"), *AssetName, i)));
                        continue;
                    }

                    UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *Expanded[i].MeshPath);
                    if (!Mesh)
                    {
                        UE_LOG(LogMapLink, Warning,
                               TEXT("  ExpandSM mesh not found: '%s'"), *Expanded[i].MeshPath);
                        continue;
                    }

                    FActorSpawnParameters SpawnParams;
                    SpawnParams.SpawnCollisionHandlingOverride =
                        ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

                    AStaticMeshActor* SMActor = LIWorld->SpawnActor<AStaticMeshActor>(
                        AStaticMeshActor::StaticClass(),
                        Expanded[i].Transform, SpawnParams);
                    if (SMActor)
                    {
                        SMActor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
                        SMActor->SetActorScale3D(Expanded[i].Transform.GetScale3D());

                        // Tag for identification during reimport cleanup
                        SMActor->Tags.Add(FName(*FString::Printf(
                            TEXT("MapLinkExpanded:%s:%d"), *AssetName, i)));

                        SMActor->SetActorLabel(FString::Printf(
                            TEXT("%s_exp_%d"), *ActorLabel, i));
                        ApplyFolder(SMActor);   // v1.14
                    }
                }

                UE_LOG(LogMapLink, Log,
                       TEXT("  Expanded SM_ '%s' → %d mesh actors."),
                       *ActorLabel, Expanded.Num());
            }
            else
            {
                // v1.5: True leaf — single mesh
                FString MeshPath = FString::Printf(
                    TEXT("/Game/MapLink/%s/Meshes/%s"), *AssetName, *AssetName);
                UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);

                if (Mesh)
                {
                    FActorSpawnParameters SpawnParams;
                    SpawnParams.SpawnCollisionHandlingOverride =
                        ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

                    AStaticMeshActor* SMActor = LIWorld->SpawnActor<AStaticMeshActor>(
                        AStaticMeshActor::StaticClass(),
                        PlaceholderTransform, SpawnParams);
                    if (SMActor)
                    {
                        SMActor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
                        SMActor->SetActorScale3D(PlaceholderTransform.GetScale3D());
                        SMActor->SetActorLabel(ActorLabel);
                        ApplyFolder(SMActor);   // v1.14
                    }

                    UE_LOG(LogMapLink, Log,
                           TEXT("  Swapped '%s' → StaticMesh (leaf SM_)"), *ActorLabel);
                }
                else
                {
                    UE_LOG(LogMapLink, Warning,
                           TEXT("  SM_ mesh not found at '%s' for '%s'."),
                           *MeshPath, *ActorLabel);
                }
            }

            FMapLinkStubRecord Record;
            Record.OwnerAssetPath = LevelPath;
            Record.ActorLabel = ActorLabel;
            Record.AssetName = AssetName;
            Record.SavedTransform = PlaceholderTransform;
            Record.bOwnerIsLevel = false;
            SwapLog.Add(Record);
            continue;
        }

        // ==============================================================
        // BP_ resolution with _GP wrapper lookup (v1.5)
        // v1.8 — Reference-mode pre-check via the proxy.
        // ==============================================================
        if (AssetName.StartsWith(TEXT("BP_")))
        {
            // ----- v1.8 reference-mode short-circuit ------------------
            FString RefBoundBPPath;
            UMapLinkProxyAsset* RefProxy = nullptr;
            const bool bRefMode =
                FMapLinkBPReferenceMode::IsProxyReferenceMode(
                    AssetName, RefBoundBPPath, &RefProxy);

            if (bRefMode)
            {
                bool bWasDeprecated = false;
                UClass* SpawnClass = FMapLinkBPReferenceMode::ResolveBoundClass(
                    RefProxy, bWasDeprecated);

                if (SpawnClass)
                {
                    LIWorld->DestroyActor(Candidate.Actor);

                    FActorSpawnParameters SpawnParams;
                    SpawnParams.SpawnCollisionHandlingOverride =
                        ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

                    AActor* SpawnedActor = LIWorld->SpawnActor<AActor>(
                        SpawnClass, PlaceholderTransform, SpawnParams);
                    if (SpawnedActor)
                    {
                        SpawnedActor->SetActorScale3D(
                            PlaceholderTransform.GetScale3D());
                        if (bWasDeprecated)
                        {
                            SpawnedActor->SetActorLabel(
                                FString::Printf(TEXT("MISSING_%s"), *ActorLabel));
                            SpawnedActor->Tags.Add(FName(*FString::Printf(
                                TEXT("MapLinkMissingBP:%s"), *RefBoundBPPath)));
                            UE_LOG(LogMapLinkBPStandIn, Warning,
                                   TEXT("  BP_ '%s' → BP_Deprecated substituted "
                                        "(bound '%s' missing)."),
                                   *ActorLabel, *RefBoundBPPath);
                        }
                        else
                        {
                            SpawnedActor->SetActorLabel(ActorLabel);
                            UE_LOG(LogMapLink, Log,
                                   TEXT("  Swapped '%s' → bound BP '%s'."),
                                   *ActorLabel, *RefBoundBPPath);
                        }
                        ApplyFolder(SpawnedActor);   // v1.14
                    }

                    // Per-import side-report
                    FMapLinkBPReferenceMode::FPlacement Rec;
                    Rec.ProxyPath           = RefProxy
                        ? RefProxy->GetPathName() : FString();
                    Rec.BoundBlueprintPath  = RefBoundBPPath;
                    Rec.PlacementContext    = TEXT("level_swap");
                    Rec.OwnerAssetPath      = LevelPath;
                    Rec.Status              = bWasDeprecated
                        ? FMapLinkBPReferenceMode::EPlacementStatus::DeprecatedSubstituted
                        : FMapLinkBPReferenceMode::EPlacementStatus::Ok;
                    FMapLinkBPReferenceMode::RecordPlacement(Rec);

                    FMapLinkStubRecord Record;
                    Record.OwnerAssetPath = LevelPath;
                    Record.ActorLabel = ActorLabel;
                    Record.AssetName = AssetName;
                    Record.SavedTransform = PlaceholderTransform;
                    Record.bOwnerIsLevel = false;
                    SwapLog.Add(Record);
                }
                else
                {
                    // Even BP_Deprecated wasn't loadable — leave the
                    // placeholder in place so the user notices.
                    UE_LOG(LogMapLinkBPStandIn, Error,
                           TEXT("  BP_ '%s' reference-mode swap failed: "
                                "neither bound BP nor BP_Deprecated could "
                                "be loaded.  Placeholder left in level."),
                           *ActorLabel);

                    FMapLinkBPReferenceMode::FPlacement Rec;
                    Rec.ProxyPath           = RefProxy
                        ? RefProxy->GetPathName() : FString();
                    Rec.BoundBlueprintPath  = RefBoundBPPath;
                    Rec.PlacementContext    = TEXT("level_swap");
                    Rec.OwnerAssetPath      = LevelPath;
                    Rec.Status              = FMapLinkBPReferenceMode::EPlacementStatus::Missing;
                    FMapLinkBPReferenceMode::RecordPlacement(Rec);
                }
                continue;
            }

            // ----- legacy path (synthesized BP under /Game/MapLink/) --
            // Check for gameplay wrapper FIRST
            FString GPPath = FString::Printf(
                TEXT("/Game/MapLink/%s/%s_GP"), *AssetName, *AssetName);
            UBlueprint* GPBP = LoadObject<UBlueprint>(nullptr, *GPPath);

            FString MainPath = FString::Printf(
                TEXT("/Game/MapLink/%s/%s"), *AssetName, *AssetName);
            UBlueprint* MainBP = LoadObject<UBlueprint>(nullptr, *MainPath);

            UBlueprint* SpawnBP = nullptr;
            if (GPBP && GPBP->GeneratedClass)
            {
                SpawnBP = GPBP;
                UE_LOG(LogMapLink, Log,
                       TEXT("  Using gameplay wrapper '%s_GP' for '%s'."),
                       *AssetName, *ActorLabel);
            }
            else if (MainBP && MainBP->GeneratedClass)
            {
                SpawnBP = MainBP;
            }

            if (SpawnBP)
            {
                LIWorld->DestroyActor(Candidate.Actor);

                FActorSpawnParameters SpawnParams;
                SpawnParams.SpawnCollisionHandlingOverride =
                    ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

                TSubclassOf<AActor> ActorClass =
                    static_cast<TSubclassOf<AActor>>(SpawnBP->GeneratedClass);

                AActor* SpawnedActor = LIWorld->SpawnActor<AActor>(
                    ActorClass, PlaceholderTransform, SpawnParams);
                if (SpawnedActor)
                {
                    SpawnedActor->SetActorScale3D(PlaceholderTransform.GetScale3D());
                    SpawnedActor->SetActorLabel(ActorLabel);
                    ApplyFolder(SpawnedActor);   // v1.14
                }

                FMapLinkStubRecord Record;
                Record.OwnerAssetPath = LevelPath;
                Record.ActorLabel = ActorLabel;
                Record.AssetName = AssetName;
                Record.SavedTransform = PlaceholderTransform;
                Record.bOwnerIsLevel = false;
                SwapLog.Add(Record);

                UE_LOG(LogMapLink, Log,
                       TEXT("  Swapped '%s' → Blueprint (scale %.2f, %.2f, %.2f)"),
                       *ActorLabel,
                       PlaceholderTransform.GetScale3D().X,
                       PlaceholderTransform.GetScale3D().Y,
                       PlaceholderTransform.GetScale3D().Z);
            }
            else
            {
                // v1.8 D7 hotfix2 — Legacy path found no synthesized BP
                // under /Game/MapLink/<asset>/.  In a BP-Reference-Mode
                // workflow this almost always means the user deleted
                // the stand-in (and the binding/proxy) without removing
                // the placeholder from their Blender scene.  Instead of
                // leaving the empty StaticMeshActor sitting in the level
                // (silent confusion), substitute BP_Deprecated and tag
                // the actor so the user sees it clearly in the outliner.
                bool bDummyDeprecated = false;
                UClass* DeprecatedClass =
                    FMapLinkBPReferenceMode::ResolveBoundClassFromPath(
                        FString(), bDummyDeprecated);

                if (DeprecatedClass)
                {
                    LIWorld->DestroyActor(Candidate.Actor);

                    FActorSpawnParameters SpawnParams;
                    SpawnParams.SpawnCollisionHandlingOverride =
                        ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

                    AActor* SpawnedActor = LIWorld->SpawnActor<AActor>(
                        DeprecatedClass, PlaceholderTransform, SpawnParams);
                    if (SpawnedActor)
                    {
                        SpawnedActor->SetActorScale3D(
                            PlaceholderTransform.GetScale3D());
                        SpawnedActor->SetActorLabel(
                            FString::Printf(TEXT("MISSING_%s"), *ActorLabel));
                        SpawnedActor->Tags.Add(FName(*FString::Printf(
                            TEXT("MapLinkMissingBP:%s"), *AssetName)));
                        ApplyFolder(SpawnedActor);   // v1.14
                    }

                    FMapLinkBPReferenceMode::FPlacement Rec;
                    Rec.ProxyPath           = FString();   // no proxy in legacy case
                    Rec.BoundBlueprintPath  = FString();   // unknown
                    Rec.PlacementContext    = TEXT("level_swap");
                    Rec.OwnerAssetPath      = LevelPath;
                    Rec.Status              =
                        FMapLinkBPReferenceMode::EPlacementStatus::DeprecatedSubstituted;
                    FMapLinkBPReferenceMode::RecordPlacement(Rec);

                    FMapLinkStubRecord Record;
                    Record.OwnerAssetPath = LevelPath;
                    Record.ActorLabel = ActorLabel;
                    Record.AssetName = AssetName;
                    Record.SavedTransform = PlaceholderTransform;
                    Record.bOwnerIsLevel = false;
                    SwapLog.Add(Record);

                    UE_LOG(LogMapLinkBPStandIn, Warning,
                           TEXT("  BP_ '%s' (asset '%s') -> BP_Deprecated "
                                "substituted (no proxy, no synth BP, no ref-mode "
                                "binding found).  Actor labeled 'MISSING_%s' and "
                                "tagged 'MapLinkMissingBP:%s'."),
                           *ActorLabel, *AssetName, *ActorLabel, *AssetName);
                }
                else
                {
                    UE_LOG(LogMapLink, Warning,
                           TEXT("No BP asset found for '%s' (asset name '%s') "
                                "AND BP_Deprecated could not be loaded "
                                "(expected at /MapLink/Blueprints/BP_Deprecated "
                                "or /Game/Blueprints/BP_Deprecated) — left as empty."),
                           *ActorLabel, *AssetName);
                }
            }
            continue;
        }

        // ==============================================================
        // LI_ resolution (existing behavior)
        // ==============================================================
        if (AssetName.StartsWith(TEXT("LI_")))
        {
            FString AssetPath = FString::Printf(
                TEXT("/Game/MapLink/%s/%s"), *AssetName, *AssetName);
            UWorld* World = LoadObject<UWorld>(nullptr, *AssetPath);

            if (World)
            {
                LIWorld->DestroyActor(Candidate.Actor);

                FActorSpawnParameters SpawnParams;
                SpawnParams.SpawnCollisionHandlingOverride =
                    ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

                ALevelInstance* LIActor = LIWorld->SpawnActor<ALevelInstance>(
                    ALevelInstance::StaticClass(),
                    PlaceholderTransform, SpawnParams);
                if (LIActor)
                {
                    LIActor->SetActorScale3D(PlaceholderTransform.GetScale3D());
                    LIActor->SetActorLabel(ActorLabel);
                    LIActor->SetWorldAsset(
                        TSoftObjectPtr<UWorld>(FSoftObjectPath(AssetPath)));
                    ApplyFolder(LIActor);   // v1.14
                }

                FMapLinkStubRecord Record;
                Record.OwnerAssetPath = LevelPath;
                Record.ActorLabel = ActorLabel;
                Record.AssetName = AssetName;
                Record.SavedTransform = PlaceholderTransform;
                Record.bOwnerIsLevel = true;
                SwapLog.Add(Record);

                UE_LOG(LogMapLink, Log,
                       TEXT("  Swapped '%s' → LevelInstance (scale %.2f, %.2f, %.2f)"),
                       *ActorLabel,
                       PlaceholderTransform.GetScale3D().X,
                       PlaceholderTransform.GetScale3D().Y,
                       PlaceholderTransform.GetScale3D().Z);
            }
            else
            {
                UE_LOG(LogMapLink, Warning,
                       TEXT("No level asset found for '%s' (asset name '%s') — left as empty."),
                       *ActorLabel, *AssetName);
            }
            continue;
        }

        // ==============================================================
        // Fallback: unknown prefix — try generic load (legacy behavior)
        // ==============================================================
        {
            FString AssetPath = FString::Printf(
                TEXT("/Game/MapLink/%s/%s"), *AssetName, *AssetName);

            UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *AssetPath);
            UWorld* World = nullptr;
            if (!BP)
            {
                World = LoadObject<UWorld>(nullptr, *AssetPath);
            }

            if (!BP && !World)
            {
                UE_LOG(LogMapLink, Warning,
                       TEXT("No asset found for placeholder '%s' (asset name '%s') — left as empty."),
                       *ActorLabel, *AssetName);
                continue;
            }

            LIWorld->DestroyActor(Candidate.Actor);
            bool bSpawnedAsLevel = false;

            if (World)
            {
                FActorSpawnParameters SpawnParams;
                SpawnParams.SpawnCollisionHandlingOverride =
                    ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
                ALevelInstance* LIActor = LIWorld->SpawnActor<ALevelInstance>(
                    ALevelInstance::StaticClass(), PlaceholderTransform, SpawnParams);
                if (LIActor)
                {
                    LIActor->SetActorScale3D(PlaceholderTransform.GetScale3D());
                    LIActor->SetActorLabel(ActorLabel);
                    LIActor->SetWorldAsset(
                        TSoftObjectPtr<UWorld>(FSoftObjectPath(AssetPath)));
                    ApplyFolder(LIActor);   // v1.14
                    bSpawnedAsLevel = true;
                }
            }
            else if (BP && BP->GeneratedClass)
            {
                FActorSpawnParameters SpawnParams;
                SpawnParams.SpawnCollisionHandlingOverride =
                    ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
                TSubclassOf<AActor> ActorClass =
                    static_cast<TSubclassOf<AActor>>(BP->GeneratedClass);
                AActor* SpawnedActor = LIWorld->SpawnActor<AActor>(
                    ActorClass, PlaceholderTransform, SpawnParams);
                if (SpawnedActor)
                {
                    SpawnedActor->SetActorScale3D(PlaceholderTransform.GetScale3D());
                    SpawnedActor->SetActorLabel(ActorLabel);
                    ApplyFolder(SpawnedActor);   // v1.14
                }
            }

            FMapLinkStubRecord Record;
            Record.OwnerAssetPath = LevelPath;
            Record.ActorLabel = ActorLabel;
            Record.AssetName = AssetName;
            Record.SavedTransform = PlaceholderTransform;
            Record.bOwnerIsLevel = bSpawnedAsLevel;
            SwapLog.Add(Record);
        }
    }

    // Save the level
    FEditorFileUtils::SaveCurrentLevel();

    // Write swap log
    WriteSwapLog(BlendName, SwapLog, TEXT("swap_li.json"));

    UE_LOG(LogMapLink, Log,
           TEXT("Level swap complete for '%s': %d placeholders processed."),
           *BlendName, SwapLog.Num());

    return EMapLinkResult::Success;
}

void FMapLinkLevelHelper::WriteSwapLog(
    const FString& BlendName,
    const TArray<FMapLinkStubRecord>& Records,
    const FString& LogFileName)
{
    FMapLinkUtils::WriteSwapLog(BlendName, Records, LogFileName);
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

FString FMapLinkLevelHelper::StripBlenderSuffix(const FString& Name)
{
    return FMapLinkUtils::StripBlenderSuffix(Name);
}
