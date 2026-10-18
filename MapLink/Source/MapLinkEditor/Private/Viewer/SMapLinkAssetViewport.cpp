// Copyright MapModelers. All Rights Reserved.

#include "Viewer/SMapLinkAssetViewport.h"
#include "MapLinkUtils.h"
#include "MapLinkSettings.h"        // v1.15 — bShowAssetNames overlay
#include "MapLinkNameDisplay.h"     // v1.15.1 — shared label palette
#include "MapLinkInterchangeHelper.h" // v1.15.3 — analysis → label anchors
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#include "AdvancedPreviewScene.h"
#include "Engine/AssetManager.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/StaticMeshActor.h"   // v1.15.6 — SM child anchors
#include "GameFramework/Actor.h"
#include "Materials/MaterialInterface.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"        // v1.13.1 — ancestor-path recursion guard

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"

#include "LevelInstance/LevelInstanceInterface.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"

#include "MapLinkProxyAsset.h"
#include "MapLinkPipeline.h"        // LogMapLink, GetPluginBaseDir
#include "MapLinkLevelHelper.h"     // ExpandSM
#include "MapLinkTypes.h"

#define LOCTEXT_NAMESPACE "MapLinkAssetViewport"

namespace
{
    /** Resolve a soft path to a loaded object. ResolveObject() only matches an
     *  exact in-memory object path; MapLink stores PrimaryAsset and ExpandSM
     *  mesh paths as package-only paths (e.g. "/Game/MapLink/X/X"), so fall
     *  back to TryLoad(), which normalises and loads like the Manager's
     *  PrimaryAsset.LoadSynchronous() does. This was why the viewport stayed
     *  blank — the async warm completed but ResolveObject() returned null. */
    UObject* ResolveLoaded(const FSoftObjectPath& Path)
    {
        if (!Path.IsValid())
        {
            return nullptr;
        }
        if (UObject* Obj = Path.ResolveObject())
        {
            return Obj;
        }
        return Path.TryLoad();
    }
}

// ───────────────────────────────────────────────────────────────────────
// Construction
// ───────────────────────────────────────────────────────────────────────

void SMapLinkAssetViewport::Construct(const FArguments& InArgs)
{
    PreviewScene = MakeShared<FAdvancedPreviewScene>(
        FPreviewScene::ConstructionValues());
    // Fixed default lighting comes from FAdvancedPreviewScene's rig; its
    // settings UI is never surfaced, so lighting is effectively constant.
    PreviewScene->SetFloorVisibility(false);

    SEditorViewport::Construct(SEditorViewport::FArguments());
}

SMapLinkAssetViewport::~SMapLinkAssetViewport()
{
    if (PendingLoadHandle.IsValid())
    {
        PendingLoadHandle->CancelHandle();
        PendingLoadHandle.Reset();
    }
    if (ViewportClient.IsValid())
    {
        ViewportClient->Viewport = nullptr;
    }
}

TSharedRef<FEditorViewportClient> SMapLinkAssetViewport::MakeEditorViewportClient()
{
    ViewportClient = MakeShared<FMapLinkAssetViewerViewportClient>(
        PreviewScene.Get(), SharedThis(this));
    return ViewportClient.ToSharedRef();
}

void SMapLinkAssetViewport::PopulateViewportOverlays(TSharedRef<SOverlay> Overlay)
{
    SEditorViewport::PopulateViewportOverlays(Overlay);

    // v1.15 — floating asset name, mirroring the level viewport's name
    // display.  Same setting (bShowAssetNames), same look (small white
    // text, shadow), anchored top-center over the preview.
    Overlay->AddSlot()
    .HAlign(HAlign_Center)
    .VAlign(VAlign_Top)
    .Padding(0.f, 10.f, 0.f, 0.f)
    [
        SNew(STextBlock)
        // v1.15.3 — the top overlay is ALWAYS on (it identifies what the
        // viewer shows); the Show Asset Names toggle drives the 3D
        // labels inside the viewport instead.
        .Visibility_Lambda([this]() -> EVisibility
        {
            return !CurrentStem.IsEmpty()
                ? EVisibility::HitTestInvisible : EVisibility::Collapsed;
        })
        .Text_Lambda([this]()
        {
            return FText::FromString(CurrentStem);
        })
        // v1.15.1 — same palette as the level viewport labels: type
        // color, out-of-date override (Manager yellow).
        .ColorAndOpacity_Lambda([this]() -> FSlateColor
        {
            return FSlateColor(CurrentStem.IsEmpty()
                ? FLinearColor::White
                : FMapLinkNameDisplay::GetLabelColor(CurrentStem));
        })
        .ShadowOffset(FVector2D(1.f, 1.f))
        .ShadowColorAndOpacity(FLinearColor::Black)
    ];
}

void SMapLinkAssetViewport::ToggleCameraMode()
{
    if (ViewportClient.IsValid())
    {
        ViewportClient->ToggleCameraMode();
    }
}

EMapLinkViewerCameraMode SMapLinkAssetViewport::GetCameraMode() const
{
    return ViewportClient.IsValid()
        ? ViewportClient->GetCameraMode()
        : EMapLinkViewerCameraMode::Orbit;
}

void SMapLinkAssetViewport::SetPaused(bool bPaused)
{
    if (ViewportClient.IsValid())
    {
        ViewportClient->SetPaused(bPaused);
    }
}

void SMapLinkAssetViewport::SetCameraSpeed(float Multiplier)
{
    if (ViewportClient.IsValid())
    {
        ViewportClient->SetSpeedMultiplier(Multiplier);
    }
}

void SMapLinkAssetViewport::Reframe()
{
    if (!ViewportClient.IsValid())
    {
        return;
    }
    FBoxSphereBounds Total(FVector::ZeroVector, FVector(100.f), 100.f);
    bool bFirst = true;
    for (const TObjectPtr<UStaticMeshComponent>& Comp : SpawnedComps)
    {
        if (Comp)
        {
            Total = bFirst ? Comp->Bounds : Total + Comp->Bounds;
            bFirst = false;
        }
    }
    ViewportClient->FrameBounds(Total);
}

// ───────────────────────────────────────────────────────────────────────
// Scene teardown
// ───────────────────────────────────────────────────────────────────────

void SMapLinkAssetViewport::ClearScene()
{
    if (PendingLoadHandle.IsValid())
    {
        PendingLoadHandle->CancelHandle();
        PendingLoadHandle.Reset();
    }
    // Invalidate any in-flight completion.
    ++LoadGeneration;

    if (PreviewScene.IsValid())
    {
        for (const TObjectPtr<UStaticMeshComponent>& Comp : SpawnedComps)
        {
            if (Comp)
            {
                PreviewScene->RemoveComponent(Comp);
            }
        }
    }
    SpawnedComps.Reset();
    InFlightMeshPlacements.Reset();
    InFlightWorldPath.Reset();
    InFlightBPPath.Reset();
    LabelAnchors.Reset();   // v1.15.3

    if (ViewportClient.IsValid())
    {
        ViewportClient->Invalidate();
    }
}

// ───────────────────────────────────────────────────────────────────────
// Selection
// ───────────────────────────────────────────────────────────────────────

void SMapLinkAssetViewport::ShowEntry(const FMapLinkViewerEntry& Entry)
{
    ClearScene();
    const int32 Gen = ++LoadGeneration;   // ClearScene already bumped; re-bump = this load's id
    CurrentStem = Entry.Stem;

    // v1.15.3 — root asset label at its origin; linked-instance anchors
    // are appended during population (ReplicateWorld / analysis parse).
    LabelAnchors.Add({ Entry.Stem, FVector::ZeroVector, /*bChild=*/false });

    UE_LOG(LogMapLink, Log,
           TEXT("[AssetViewer] ShowEntry stem='%s' type=%d"),
           *Entry.Stem, static_cast<int32>(Entry.Type));

    switch (Entry.Type)
    {
    case EMapLinkViewerAssetType::StaticMesh:    BeginShowStaticMesh(Entry, Gen); break;
    case EMapLinkViewerAssetType::LevelInstance: BeginShowLevel(Entry, Gen);      break;
    case EMapLinkViewerAssetType::Blueprint:     BeginShowBlueprint(Entry, Gen);  break;
    default:                                     FinishPopulate(Gen);             break;
    }
}

// ── SM_ ────────────────────────────────────────────────────────────────

void SMapLinkAssetViewport::BeginShowStaticMesh(const FMapLinkViewerEntry& Entry, int32 Gen)
{
    InFlightMeshPlacements.Reset();

    // 1. Own meshes — enumerate by PATH under /Game/MapLink/<stem>/Meshes/.
    //    This is the authoritative, name-agnostic source and is immune to the
    //    Interchange mesh-naming convention. (ExpandSM only *guesses* the name
    //    as "<stem>_<geo>", which does not match the imported asset for a leaf
    //    SM_ — that mismatch is why a freshly-imported SM_ rendered blank.)
    const FString OwnMeshesDir =
        FString::Printf(TEXT("/Game/MapLink/%s/Meshes"), *Entry.Stem);
    {
        FAssetRegistryModule& ARModule =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
        IAssetRegistry& AR = ARModule.Get();

        FARFilter Filter;
        Filter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());
        Filter.PackagePaths.Add(FName(*OwnMeshesDir));
        Filter.bRecursivePaths = true;

        TArray<FAssetData> Meshes;
        AR.GetAssets(Filter, Meshes);
        for (const FAssetData& MeshData : Meshes)
        {
            InFlightMeshPlacements.Emplace(MeshData.GetSoftObjectPath(), FTransform::Identity);
        }
    }

    // 2. Composite children — place linked SM_ at their transforms via ExpandSM,
    //    but only when a fresh analysis.json exists (the per-iteration clean
    //    wipes Logs/). Keep only entries that live under a DIFFERENT stem's
    //    /Meshes/ (own geo is already covered above by the path enumeration).
    const FString AnalysisPath =
        FMapLinkUtils::ResolveArtifactLogFile(Entry.Stem / TEXT("analysis.json"));
    if (FPaths::FileExists(AnalysisPath))
    {
        const FString OwnPrefix = OwnMeshesDir + TEXT("/");
        TSet<FString> Visited;
        const TArray<FExpandedMesh> Expanded =
            FMapLinkLevelHelper::ExpandSM(Entry.Stem, FTransform::Identity, Visited);
        for (const FExpandedMesh& Ex : Expanded)
        {
            if (Ex.Kind == EMapLinkExpandedKind::StaticMesh
                && !Ex.MeshPath.IsEmpty()
                && !Ex.MeshPath.StartsWith(OwnPrefix))
            {
                InFlightMeshPlacements.Emplace(FSoftObjectPath(Ex.MeshPath), Ex.Transform);
            }
            // BPReference children are skipped in the preview (read-only viewer);
            // their own SM_/BP_ proxies remain individually viewable.
        }

        // v1.15.3 — name-label anchors: one per DIRECT linked collection
        // instance, at its collection transform (the analysis is the
        // same source Blender uses — LinkedAssets ARE the linked
        // instances).  The flattened ExpandSM mesh list is deliberately
        // NOT used here: labels are per asset instance, never per mesh.
        FMapLinkAnalysisReport Report;
        if (FMapLinkInterchangeHelper::ParseAnalysisJson(AnalysisPath, Report))
        {
            for (const FMapLinkLinkedAssetEntry& Linked : Report.LinkedAssets)
            {
                const FString ChildStem =
                    FPaths::GetBaseFilename(Linked.SourceBlendPath);
                // BP references are not rendered in the read-only preview
                // — a label would float over nothing.
                if (ChildStem.StartsWith(TEXT("BP_")))
                {
                    continue;
                }
                LabelAnchors.Add({ ChildStem,
                                   Linked.WorldTransform.GetLocation(),
                                   /*bChild=*/true });
            }
        }
    }

    if (InFlightMeshPlacements.Num() == 0)
    {
        FinishPopulate(Gen);
        return;
    }

    TArray<FSoftObjectPath> Paths;
    Paths.Reserve(InFlightMeshPlacements.Num());
    for (const TPair<FSoftObjectPath, FTransform>& P : InFlightMeshPlacements)
    {
        Paths.Add(P.Key);
    }

    TWeakPtr<SMapLinkAssetViewport> WeakSelf = SharedThis(this);
    PendingLoadHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(
        Paths,
        FStreamableDelegate::CreateLambda([WeakSelf, Gen]()
        {
            if (TSharedPtr<SMapLinkAssetViewport> S = WeakSelf.Pin())
            {
                S->OnMeshesLoaded(Gen);
            }
        }));

    // RequestAsyncLoad returns null when there is nothing to stream (all
    // already loaded) or the paths are unresolvable — resolve immediately so
    // the scene still populates and the overlay clears.
    if (!PendingLoadHandle.IsValid())
    {
        OnMeshesLoaded(Gen);
    }
}

void SMapLinkAssetViewport::OnMeshesLoaded(int32 Gen)
{
    if (Gen != LoadGeneration)
    {
        return;   // superseded by a newer selection / pause
    }
    int32 Added = 0;
    for (const TPair<FSoftObjectPath, FTransform>& P : InFlightMeshPlacements)
    {
        if (UStaticMesh* Mesh = Cast<UStaticMesh>(ResolveLoaded(P.Key)))
        {
            if (AddStaticMesh(Mesh, /*Overrides=*/{}, P.Value))
            {
                ++Added;
            }
        }
    }
    UE_LOG(LogMapLink, Log,
           TEXT("[AssetViewer] SM_ '%s': placed %d / %d mesh(es)."),
           *CurrentStem, Added, InFlightMeshPlacements.Num());
    FinishPopulate(Gen);
}

// ── LI_ ────────────────────────────────────────────────────────────────

void SMapLinkAssetViewport::BeginShowLevel(const FMapLinkViewerEntry& Entry, int32 Gen)
{
    UMapLinkProxyAsset* Proxy = Entry.Proxy.Get();
    InFlightWorldPath = (Proxy ? Proxy->PrimaryAsset.ToSoftObjectPath() : FSoftObjectPath());

    if (!InFlightWorldPath.IsValid())
    {
        FinishPopulate(Gen);
        return;
    }

    TWeakPtr<SMapLinkAssetViewport> WeakSelf = SharedThis(this);
    PendingLoadHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(
        InFlightWorldPath,
        FStreamableDelegate::CreateLambda([WeakSelf, Gen]()
        {
            if (TSharedPtr<SMapLinkAssetViewport> S = WeakSelf.Pin())
            {
                S->OnLevelLoaded(Gen);
            }
        }));

    if (!PendingLoadHandle.IsValid())
    {
        OnLevelLoaded(Gen);
    }
}

void SMapLinkAssetViewport::OnLevelLoaded(int32 Gen)
{
    if (Gen != LoadGeneration)
    {
        return;
    }
    UWorld* World = Cast<UWorld>(ResolveLoaded(InFlightWorldPath));
    if (World)
    {
        const int32 Before = SpawnedComps.Num();
        TSet<FString> VisitedWorlds;
        ReplicateWorld(World, FTransform::Identity, /*Depth=*/0, VisitedWorlds);
        UE_LOG(LogMapLink, Log,
               TEXT("[AssetViewer] LI_ '%s': world '%s' → %d component(s)."),
               *CurrentStem, *World->GetOutermost()->GetName(),
               SpawnedComps.Num() - Before);
    }
    else
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("[AssetViewer] LI_ '%s': could not resolve world from '%s'."),
               *CurrentStem, *InFlightWorldPath.ToString());
    }
    FinishPopulate(Gen);
}

void SMapLinkAssetViewport::ReplicateWorld(
    UWorld* World, const FTransform& ParentXf, int32 Depth, TSet<FString>& VisitedWorlds)
{
    if (!World || Depth > kMaxLevelDepth)
    {
        return;
    }
    const FString WorldKey = World->GetOutermost()->GetName();
    if (VisitedWorlds.Contains(WorldKey))
    {
        return;   // true cycle: this world is already an ANCESTOR on the
                  // current recursion path — stop to avoid infinite recursion.
    }
    // v1.13.1 — ancestor-PATH guard, not a global seen-set: add on the way
    // down, remove on the way back up. Previously VisitedWorlds was never
    // cleared, so several sibling LevelInstances that reference the SAME child
    // world (e.g. multiple LI_uneplace under LI_zidane) were de-duped and only
    // the first — if any — rendered. Now each instance renders at its own xf.
    VisitedWorlds.Add(WorldKey);
    ON_SCOPE_EXIT { VisitedWorlds.Remove(WorldKey); };

    ULevel* Level = World->PersistentLevel;
    if (!Level)
    {
        return;
    }

    for (AActor* Actor : Level->Actors)
    {
        if (!Actor)
        {
            continue;
        }

        // Nested level instance → resolve referenced world and recurse.
        if (ILevelInstanceInterface* LII = Cast<ILevelInstanceInterface>(Actor))
        {
            const FTransform SubXf =
                (Actor->GetRootComponent()
                     ? ComputeComponentWorldTransform(Actor->GetRootComponent())
                     : FTransform::Identity) * ParentXf;

            // v1.13.1 — resolve the child world via ResolveLoaded() (ResolveObject
            // → TryLoad), matching the top-level load. LoadSynchronous() alone
            // returned null for these nested refs in the preview scene, which is
            // why nested LI_ (child levels) showed nothing while SM_ did.
            const FSoftObjectPath SubPath = LII->GetWorldAsset().ToSoftObjectPath();
            UWorld* SubWorld = Cast<UWorld>(ResolveLoaded(SubPath));
            UE_LOG(LogMapLink, Log,
                   TEXT("[AssetViewer]   nested LI '%s' → '%s' %s"),
                   *Actor->GetName(), *SubPath.ToString(),
                   SubWorld ? TEXT("loaded") : TEXT("LOAD FAILED"));

            // v1.15.3 — name-label anchor at this linked instance's
            // transform (Blender: the linked collection instance).
            {
                FString SubPkg = SubPath.ToString();
                int32 DotIdx;
                if (SubPkg.FindChar(TEXT('.'), DotIdx))
                {
                    SubPkg.LeftInline(DotIdx);
                }
                LabelAnchors.Add({ FPaths::GetBaseFilename(SubPkg),
                                   SubXf.GetLocation(), /*bChild=*/true });
            }

            if (SubWorld)
            {
                ReplicateWorld(SubWorld, SubXf, Depth + 1, VisitedWorlds);
            }
            continue;
        }

        // v1.15.6 — child anchor for leaf SM instances: an untagged
        // AStaticMeshActor whose mesh belongs to a DIFFERENT stem than
        // this world is a linked SM child — one actor, one instance,
        // one label at its transform.  (Tagged actors are composite
        // expansions — per-mesh, never labeled.)
        {
            bool bTagged = false;
            for (const FName& ActorTag : Actor->Tags)   // 'Tag' would shadow SWidget::Tag
            {
                if (ActorTag.ToString().StartsWith(TEXT("MapLink")))
                {
                    bTagged = true;
                    break;
                }
            }
            const AStaticMeshActor* SMA = Cast<AStaticMeshActor>(Actor);
            const UStaticMeshComponent* RootSMC =
                SMA ? SMA->GetStaticMeshComponent() : nullptr;
            if (!bTagged && RootSMC && RootSMC->GetStaticMesh())
            {
                FString MeshPkg =
                    RootSMC->GetStaticMesh()->GetOutermost()->GetName();
                static const FString Prefix = TEXT("/Game/MapLink/");
                if (MeshPkg.StartsWith(Prefix))
                {
                    FString After = MeshPkg.RightChop(Prefix.Len());
                    int32 SlashIdx;
                    const FString MeshStem = After.FindChar(TEXT('/'), SlashIdx)
                        ? After.Left(SlashIdx) : After;
                    const FString WorldStem = FPaths::GetBaseFilename(WorldKey);
                    if (!MeshStem.IsEmpty() && MeshStem != WorldStem)
                    {
                        const FTransform ChildXf =
                            (Actor->GetRootComponent()
                                 ? ComputeComponentWorldTransform(
                                       Actor->GetRootComponent())
                                 : FTransform::Identity) * ParentXf;
                        LabelAnchors.Add({ MeshStem,
                                           ChildXf.GetLocation(),
                                           /*bChild=*/true });
                    }
                }
            }
        }

        // Otherwise replicate every static-mesh (incl. instanced) component.
        TArray<UStaticMeshComponent*> MeshComps;
        Actor->GetComponents<UStaticMeshComponent>(MeshComps);
        for (UStaticMeshComponent* SMC : MeshComps)
        {
            UStaticMesh* Mesh = SMC ? SMC->GetStaticMesh() : nullptr;
            if (!Mesh)
            {
                continue;
            }

            TArray<UMaterialInterface*> Overrides;
            const int32 NumMats = SMC->GetNumMaterials();
            Overrides.Reserve(NumMats);
            for (int32 i = 0; i < NumMats; ++i)
            {
                Overrides.Add(SMC->GetMaterial(i));
            }

            const FTransform CompWorld =
                ComputeComponentWorldTransform(SMC) * ParentXf;

            if (UInstancedStaticMeshComponent* ISM =
                    Cast<UInstancedStaticMeshComponent>(SMC))
            {
                const int32 Count = FMath::Min(ISM->GetInstanceCount(), kMaxInstancesPerComp);
                for (int32 i = 0; i < Count; ++i)
                {
                    FTransform Inst;
                    if (ISM->GetInstanceTransform(i, Inst, /*bWorldSpace=*/false))
                    {
                        AddStaticMesh(Mesh, Overrides, Inst * CompWorld);
                    }
                }
            }
            else
            {
                AddStaticMesh(Mesh, Overrides, CompWorld);
            }
        }
    }
}

// ── BP_ ────────────────────────────────────────────────────────────────

void SMapLinkAssetViewport::BeginShowBlueprint(const FMapLinkViewerEntry& Entry, int32 Gen)
{
    UMapLinkProxyAsset* Proxy = Entry.Proxy.Get();

    FSoftObjectPath BPPath;
    if (Proxy && !Proxy->BoundBlueprint.IsNull())
    {
        BPPath = Proxy->BoundBlueprint.ToSoftObjectPath();
    }
    else
    {
        // Unbound / orphan BP_ → the level-swap substitute.
        BPPath = FSoftObjectPath(TEXT("/MapLink/Blueprints/BP_Deprecated.BP_Deprecated"));
    }
    InFlightBPPath = BPPath;

    TWeakPtr<SMapLinkAssetViewport> WeakSelf = SharedThis(this);
    PendingLoadHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(
        InFlightBPPath,
        FStreamableDelegate::CreateLambda([WeakSelf, Gen]()
        {
            if (TSharedPtr<SMapLinkAssetViewport> S = WeakSelf.Pin())
            {
                S->OnBlueprintLoaded(Gen);
            }
        }));

    if (!PendingLoadHandle.IsValid())
    {
        OnBlueprintLoaded(Gen);
    }
}

void SMapLinkAssetViewport::OnBlueprintLoaded(int32 Gen)
{
    if (Gen != LoadGeneration)
    {
        return;
    }
    if (UBlueprint* BP = Cast<UBlueprint>(ResolveLoaded(InFlightBPPath)))
    {
        CollectBlueprintMeshes(BP->GeneratedClass, FTransform::Identity);
    }
    else
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("[AssetViewer] BP_ '%s': could not resolve blueprint from '%s'."),
               *CurrentStem, *InFlightBPPath.ToString());
    }
    FinishPopulate(Gen);
}

void SMapLinkAssetViewport::CollectBlueprintMeshes(UClass* GeneratedClass, const FTransform& ParentXf)
{
    // Walk the class chain, reading static-mesh component templates from each
    // Blueprint's Simple Construction Script. Component visuals only — no actor
    // is ever instantiated and no construction script runs.
    for (UClass* C = GeneratedClass; C; C = C->GetSuperClass())
    {
        UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(C);
        if (!BPGC || !BPGC->SimpleConstructionScript)
        {
            continue;
        }
        for (USCS_Node* Node : BPGC->SimpleConstructionScript->GetAllNodes())
        {
            if (!Node)
            {
                continue;
            }
            UStaticMeshComponent* Template =
                Cast<UStaticMeshComponent>(Node->ComponentTemplate);
            UStaticMesh* Mesh = Template ? Template->GetStaticMesh() : nullptr;
            if (!Mesh)
            {
                continue;
            }

            TArray<UMaterialInterface*> Overrides;
            const int32 NumMats = Template->GetNumMaterials();
            Overrides.Reserve(NumMats);
            for (int32 i = 0; i < NumMats; ++i)
            {
                Overrides.Add(Template->GetMaterial(i));
            }

            AddStaticMesh(Mesh, Overrides, Template->GetRelativeTransform() * ParentXf);
        }
    }
}

// ───────────────────────────────────────────────────────────────────────
// Shared
// ───────────────────────────────────────────────────────────────────────

UStaticMeshComponent* SMapLinkAssetViewport::AddStaticMesh(
    UStaticMesh* Mesh,
    const TArray<UMaterialInterface*>& Overrides,
    const FTransform& WorldXf)
{
    if (!Mesh || !PreviewScene.IsValid())
    {
        return nullptr;
    }

    UStaticMeshComponent* Comp = NewObject<UStaticMeshComponent>(GetTransientPackage());
    Comp->SetStaticMesh(Mesh);
    for (int32 i = 0; i < Overrides.Num(); ++i)
    {
        if (Overrides[i])
        {
            Comp->SetMaterial(i, Overrides[i]);
        }
    }
    PreviewScene->AddComponent(Comp, WorldXf);
    SpawnedComps.Add(Comp);
    return Comp;
}

FTransform SMapLinkAssetViewport::ComputeComponentWorldTransform(const USceneComponent* Comp)
{
    // Compose serialized relative transforms up the attach chain. Works on a
    // loaded-but-unregistered actor (no reliance on ComponentToWorld).
    FTransform Xf = FTransform::Identity;
    for (const USceneComponent* Cur = Comp; Cur; Cur = Cur->GetAttachParent())
    {
        Xf = Xf * Cur->GetRelativeTransform();
    }
    return Xf;
}

void SMapLinkAssetViewport::FinishPopulate(int32 Gen)
{
    if (Gen != LoadGeneration)
    {
        return;
    }
    PendingLoadHandle.Reset();

    if (ViewportClient.IsValid())
    {
        FBoxSphereBounds Total(FVector::ZeroVector, FVector(100.f), 100.f);
        bool bFirst = true;
        for (const TObjectPtr<UStaticMeshComponent>& Comp : SpawnedComps)
        {
            if (Comp)
            {
                Total = bFirst ? Comp->Bounds : Total + Comp->Bounds;
                bFirst = false;
            }
        }
        UE_LOG(LogMapLink, Log,
               TEXT("[AssetViewer] '%s': %d component(s); bounds origin=(%.1f,%.1f,%.1f) r=%.1f"),
               *CurrentStem, SpawnedComps.Num(),
               Total.Origin.X, Total.Origin.Y, Total.Origin.Z, Total.SphereRadius);

        ViewportClient->FrameBounds(Total);
        ViewportClient->Invalidate();
    }

    OnPopulated.ExecuteIfBound();
}

void SMapLinkAssetViewport::AddReferencedObjects(FReferenceCollector& Collector)
{
    Collector.AddReferencedObjects(SpawnedComps);
}

#undef LOCTEXT_NAMESPACE
