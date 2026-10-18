// Copyright MapModelers. All Rights Reserved.
//
// v1.5 — SM_ leaf resolution + _GP wrapper lookup
// v1.6 — SM_ composite recursive expansion with tagging
// v1.6.1 — FIX: multi-mesh SM_ leaf (OwnGeo > 1, LinkedAssets == 0) now
//          routes through ExpandSM instead of the single-mesh leaf path.
//          Composite detection: LinkedAssets > 0 || OwnGeo > 1.

#include "MapLinkBlueprintHelper.h"
#include "MapLinkUtils.h"
#include "MapLinkLevelHelper.h"
#include "MapLinkInterchangeHelper.h"
#include "MapLinkPipeline.h"
#include "MapLinkBPReferenceMode.h"  // v1.8
#include "MapLinkProxyAsset.h"        // v1.8
#include "Engine/Blueprint.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/ChildActorComponent.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "UObject/Package.h"
#include "UObject/LinkerLoad.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/PlatformFileManager.h"

// ---------------------------------------------------------------------------
// Public — CreateBlueprint (unchanged from v1.4)
// ---------------------------------------------------------------------------

EMapLinkResult FMapLinkBlueprintHelper::CreateBlueprint(
    const FString& BlendName,
    const TArray<FMapLinkImportedMesh>& ImportedMeshes,
    const TArray<FMapLinkImportedEmpty>& ImportedEmpties)
{
    FString PackagePath = FString::Printf(
        TEXT("/Game/MapLink/%s/%s"), *BlendName, *BlendName);

    // --- If a BP already exists (reimport), nuke it completely ---
    {
        UPackage* ExistingPkg = FindPackage(nullptr, *PackagePath);
        if (!ExistingPkg)
            ExistingPkg = FindObject<UPackage>(nullptr, *PackagePath);

        if (ExistingPkg)
        {
            UE_LOG(LogMapLink, Log,
                   TEXT("Evicting existing Blueprint package '%s' for clean recreate."),
                   *BlendName);

            FString DiskPath = FPackageName::LongPackageNameToFilename(
                PackagePath, FPackageName::GetAssetPackageExtension());
            IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
            if (PlatformFile.FileExists(*DiskPath))
            {
                PlatformFile.DeleteFile(*DiskPath);
            }

            ResetLoaders(ExistingPkg);
            FName TempName = MakeUniqueObjectName(
                GetTransientPackage(), UPackage::StaticClass(),
                FName(*FString::Printf(TEXT("MapLink_BPEvict_%s"), *BlendName)));
            ExistingPkg->Rename(*TempName.ToString(), nullptr,
                REN_DontCreateRedirectors | REN_NonTransactional | REN_ForceNoResetLoaders);

            CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
        }
    }

    // --- Create fresh package and Blueprint ---
    UPackage* Package = CreatePackage(*PackagePath);
    if (!Package)
    {
        UE_LOG(LogMapLink, Error,
               TEXT("Failed to create package '%s'."), *PackagePath);
        return EMapLinkResult::BlueprintCompileError;
    }

    UBlueprint* NewBP = FKismetEditorUtilities::CreateBlueprint(
        AActor::StaticClass(),
        Package,
        *BlendName,
        BPTYPE_Normal,
        UBlueprint::StaticClass(),
        UBlueprintGeneratedClass::StaticClass());

    if (!NewBP)
    {
        UE_LOG(LogMapLink, Error,
               TEXT("Failed to create Blueprint '%s'."), *BlendName);
        return EMapLinkResult::BlueprintCompileError;
    }

    USimpleConstructionScript* SCS = NewBP->SimpleConstructionScript;
    if (!SCS)
    {
        UE_LOG(LogMapLink, Error,
               TEXT("Blueprint '%s' has no SimpleConstructionScript."), *BlendName);
        return EMapLinkResult::BlueprintCompileError;
    }

    // --- Create a DefaultSceneRoot as the single root node ---
    USCS_Node* RootNode = SCS->CreateNode(
        USceneComponent::StaticClass(), TEXT("DefaultSceneRoot"));
    if (!RootNode)
    {
        UE_LOG(LogMapLink, Error,
               TEXT("Failed to create root SceneComponent for '%s'."), *BlendName);
        return EMapLinkResult::BlueprintCompileError;
    }
    SCS->AddNode(RootNode);

    // --- Add StaticMeshComponents for each own-geometry mesh ---
    for (const FMapLinkImportedMesh& Mesh : ImportedMeshes)
    {
        USCS_Node* NewNode = SCS->CreateNode(
            UStaticMeshComponent::StaticClass(), *Mesh.Name);

        if (!NewNode) continue;

        UStaticMeshComponent* SMComp =
            Cast<UStaticMeshComponent>(NewNode->ComponentTemplate);
        if (SMComp && Mesh.StaticMesh)
        {
            SMComp->SetStaticMesh(Mesh.StaticMesh);
        }

        Cast<USceneComponent>(NewNode->ComponentTemplate)
            ->SetRelativeTransform_Direct(Mesh.Transform);

        RootNode->AddChildNode(NewNode);
    }

    // --- Add SceneComponent placeholders for each linked asset empty ---
    for (const FMapLinkImportedEmpty& Empty : ImportedEmpties)
    {
        FString PlaceholderName = FString::Printf(
            TEXT("Placeholder_%s"), *Empty.Name);

        USCS_Node* PlaceholderNode = SCS->CreateNode(
            USceneComponent::StaticClass(), *PlaceholderName);

        if (!PlaceholderNode) continue;

        PlaceholderNode->ComponentTemplate->ComponentTags.Add(
            FName(*FString::Printf(TEXT("MapLinkPlaceholder:%s"), *Empty.Name)));

        Cast<USceneComponent>(PlaceholderNode->ComponentTemplate)
            ->SetRelativeTransform_Direct(Empty.Transform);

        RootNode->AddChildNode(PlaceholderNode);
    }

    // Compile and save
    FKismetEditorUtilities::CompileBlueprint(NewBP);
    FAssetRegistryModule::AssetCreated(NewBP);
    NewBP->MarkPackageDirty();
    SaveBPPackage(NewBP);

    UE_LOG(LogMapLink, Log,
           TEXT("Blueprint '%s' created: %d mesh components, %d placeholders."),
           *BlendName, ImportedMeshes.Num(), ImportedEmpties.Num());

    return EMapLinkResult::Success;
}

// ---------------------------------------------------------------------------
// Public — SwapPlaceholdersInBlueprint (v1.5 + v1.6 + v1.6.1)
// ---------------------------------------------------------------------------

EMapLinkResult FMapLinkBlueprintHelper::SwapPlaceholdersInBlueprint(
    const FString& BlendName)
{
    FString BPPath = FString::Printf(
        TEXT("/Game/MapLink/%s/%s"), *BlendName, *BlendName);

    UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *BPPath);
    if (!BP)
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("Blueprint not found at '%s' for swap."), *BPPath);
        return EMapLinkResult::AssetNotFound;
    }

    USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
    if (!SCS)
    {
        return EMapLinkResult::BlueprintCompileError;
    }

    FString PluginBaseDir = FMapLinkPipeline::GetPluginBaseDir();

    // --- v1.6: Clean up any previously-expanded SM_ components (tagged MapLinkExpanded:*) ---
    {
        TArray<USCS_Node*> ExpandedToRemove;
        for (USCS_Node* Node : SCS->GetAllNodes())
        {
            if (!Node || !Node->ComponentTemplate) continue;
            for (const FName& Tag : Node->ComponentTemplate->ComponentTags)
            {
                if (Tag.ToString().StartsWith(TEXT("MapLinkExpanded:")))
                {
                    ExpandedToRemove.Add(Node);
                    break;
                }
            }
        }
        for (USCS_Node* Node : ExpandedToRemove)
        {
            SCS->RemoveNode(Node);
        }
        if (ExpandedToRemove.Num() > 0)
        {
            UE_LOG(LogMapLink, Log,
                   TEXT("Cleaned up %d previously-expanded SM_ component nodes."),
                   ExpandedToRemove.Num());
        }
    }

    // Find DefaultSceneRoot for parenting
    USCS_Node* ParentNode = nullptr;
    for (USCS_Node* RootCandidate : SCS->GetRootNodes())
    {
        if (RootCandidate && RootCandidate->GetVariableName().ToString()
                .Contains(TEXT("DefaultSceneRoot")))
        {
            ParentNode = RootCandidate;
            break;
        }
    }

    TArray<USCS_Node*> NodesToRemove;
    TArray<FMapLinkStubRecord> SwapLog;

    // Iterate all SCS nodes looking for MapLinkPlaceholder tags
    TArray<USCS_Node*> AllNodes = SCS->GetAllNodes();
    for (USCS_Node* Node : AllNodes)
    {
        if (!Node || !Node->ComponentTemplate) continue;

        FString RawPlaceholderName;
        for (const FName& Tag : Node->ComponentTemplate->ComponentTags)
        {
            FString TagStr = Tag.ToString();
            if (TagStr.StartsWith(TEXT("MapLinkPlaceholder:")))
            {
                RawPlaceholderName = TagStr.RightChop(19);
                break;
            }
        }

        if (RawPlaceholderName.IsEmpty()) continue;

        FString AssetName = StripBlenderSuffix(RawPlaceholderName);

        // Capture the full relative transform
        FTransform SavedTransform =
            Cast<USceneComponent>(Node->ComponentTemplate)->GetRelativeTransform();

        // ==============================================================
        // SM_ resolution (v1.5 leaf + v1.6 composite + v1.6.1 multi-mesh)
        // ==============================================================
        if (AssetName.StartsWith(TEXT("SM_")))
        {
            // Determine leaf vs composite/multi-mesh
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

            if (bNeedsExpansion)
            {
                // v1.6 / v1.6.1: Recursive expansion (composite or multi-mesh)
                TSet<FString> Visited;
                TArray<FExpandedMesh> Expanded =
                    FMapLinkLevelHelper::ExpandSM(AssetName, SavedTransform, Visited);

                for (int32 i = 0; i < Expanded.Num(); ++i)
                {
                    // v1.8.1 — nested BP_ reference: embed as a ChildActorComponent
                    if (Expanded[i].Kind == EMapLinkExpandedKind::BPReference)
                    {
                        FString RefBoundBPPath;
                        UMapLinkProxyAsset* RefProxy = nullptr;
                        bool bWasDeprecated = false;
                        UClass* SpawnClass = nullptr;
                        if (FMapLinkBPReferenceMode::IsProxyReferenceMode(
                                Expanded[i].BPNodeName, RefBoundBPPath, &RefProxy))
                        {
                            SpawnClass = FMapLinkBPReferenceMode::ResolveBoundClass(
                                RefProxy, bWasDeprecated);
                        }
                        if (!SpawnClass)
                        {
                            UE_LOG(LogMapLinkBPStandIn, Warning,
                                   TEXT("  Nested BP_ '%s' unresolved during BP "
                                        "synthesis — skipping."),
                                   *Expanded[i].BPNodeName);
                            continue;
                        }

                        USCS_Node* CANode = SCS->CreateNode(
                            UChildActorComponent::StaticClass(),
                            *FString::Printf(TEXT("BP_%s_exp_%d"),
                                             *Expanded[i].BPNodeName, i));
                        UChildActorComponent* CAComp =
                            Cast<UChildActorComponent>(CANode->ComponentTemplate);
                        CAComp->SetChildActorClass(SpawnClass);
                        CAComp->SetRelativeTransform_Direct(Expanded[i].Transform);
                        CAComp->ComponentTags.Add(FName(*FString::Printf(
                            TEXT("MapLinkExpanded:%s:%d"), *AssetName, i)));

                        if (ParentNode)
                            ParentNode->AddChildNode(CANode);
                        else
                            SCS->AddNode(CANode);
                        continue;
                    }

                    UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *Expanded[i].MeshPath);
                    if (!Mesh)
                    {
                        UE_LOG(LogMapLink, Warning,
                               TEXT("  ExpandSM mesh not found: '%s'"), *Expanded[i].MeshPath);
                        continue;
                    }

                    USCS_Node* SMNode = SCS->CreateNode(
                        UStaticMeshComponent::StaticClass(),
                        *FString::Printf(TEXT("SM_%s_exp_%d"), *AssetName, i));

                    UStaticMeshComponent* SMComp =
                        Cast<UStaticMeshComponent>(SMNode->ComponentTemplate);
                    SMComp->SetStaticMesh(Mesh);
                    SMComp->SetRelativeTransform_Direct(Expanded[i].Transform);

                    // Tag for reimport cleanup
                    SMComp->ComponentTags.Add(FName(*FString::Printf(
                        TEXT("MapLinkExpanded:%s:%d"), *AssetName, i)));

                    if (ParentNode)
                        ParentNode->AddChildNode(SMNode);
                    else
                        SCS->AddNode(SMNode);
                }

                UE_LOG(LogMapLink, Log,
                       TEXT("  Expanded SM_ '%s' → %d components in BP '%s'."),
                       *RawPlaceholderName, Expanded.Num(), *BlendName);
            }
            else
            {
                // v1.5: True leaf — single UStaticMeshComponent
                FString MeshPath = FString::Printf(
                    TEXT("/Game/MapLink/%s/Meshes/%s"), *AssetName, *AssetName);
                UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);

                if (Mesh)
                {
                    USCS_Node* SMNode = SCS->CreateNode(
                        UStaticMeshComponent::StaticClass(),
                        *FString::Printf(TEXT("SM_%s"), *AssetName));

                    UStaticMeshComponent* SMComp =
                        Cast<UStaticMeshComponent>(SMNode->ComponentTemplate);
                    SMComp->SetStaticMesh(Mesh);
                    SMComp->SetRelativeTransform_Direct(SavedTransform);

                    if (ParentNode)
                        ParentNode->AddChildNode(SMNode);
                    else
                        SCS->AddNode(SMNode);
                }
                else
                {
                    UE_LOG(LogMapLink, Warning,
                           TEXT("  SM_ mesh not found at '%s' for placeholder '%s'."),
                           *MeshPath, *RawPlaceholderName);
                }
            }

            NodesToRemove.Add(Node);

            FMapLinkStubRecord Record;
            Record.OwnerAssetPath = BPPath;
            Record.ActorLabel = RawPlaceholderName;
            Record.AssetName = AssetName;
            Record.SavedTransform = SavedTransform;
            Record.bOwnerIsLevel = false;
            SwapLog.Add(Record);
            continue;
        }

        // ==============================================================
        // BP_ with _GP lookup (v1.5)
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
                UClass* ChildClass =
                    FMapLinkBPReferenceMode::ResolveBoundClass(
                        RefProxy, bWasDeprecated);

                if (ChildClass)
                {
                    FString CAName =
                        FString::Printf(TEXT("Child_%s"), *RawPlaceholderName);
                    USCS_Node* CANode = SCS->CreateNode(
                        UChildActorComponent::StaticClass(), *CAName);
                    if (CANode)
                    {
                        UChildActorComponent* CAComp =
                            Cast<UChildActorComponent>(CANode->ComponentTemplate);
                        CAComp->SetChildActorClass(
                            static_cast<TSubclassOf<AActor>>(ChildClass));
                        CAComp->SetRelativeTransform_Direct(SavedTransform);

                        if (ParentNode) ParentNode->AddChildNode(CANode);
                        else            SCS->AddNode(CANode);

                        UE_LOG(LogMapLink, Log,
                               TEXT("  Nested BP_ '%s' → ChildActor with bound '%s'%s."),
                               *RawPlaceholderName, *RefBoundBPPath,
                               bWasDeprecated ? TEXT(" [DEPRECATED_FALLBACK]") : TEXT(""));
                    }

                    FMapLinkBPReferenceMode::FPlacement Rec;
                    Rec.ProxyPath          = RefProxy
                        ? RefProxy->GetPathName() : FString();
                    Rec.BoundBlueprintPath = RefBoundBPPath;
                    Rec.PlacementContext   = TEXT("bp_nested");
                    Rec.OwnerAssetPath     = BPPath;
                    Rec.Status             = bWasDeprecated
                        ? FMapLinkBPReferenceMode::EPlacementStatus::DeprecatedSubstituted
                        : FMapLinkBPReferenceMode::EPlacementStatus::Ok;
                    FMapLinkBPReferenceMode::RecordPlacement(Rec);
                }
                else
                {
                    UE_LOG(LogMapLinkBPStandIn, Error,
                           TEXT("  Nested BP_ '%s' reference-mode: neither "
                                "bound BP nor BP_Deprecated loadable."),
                           *RawPlaceholderName);

                    FMapLinkBPReferenceMode::FPlacement Rec;
                    Rec.ProxyPath          = RefProxy
                        ? RefProxy->GetPathName() : FString();
                    Rec.BoundBlueprintPath = RefBoundBPPath;
                    Rec.PlacementContext   = TEXT("bp_nested");
                    Rec.OwnerAssetPath     = BPPath;
                    Rec.Status             = FMapLinkBPReferenceMode::EPlacementStatus::Missing;
                    FMapLinkBPReferenceMode::RecordPlacement(Rec);
                }

                NodesToRemove.Add(Node);

                FMapLinkStubRecord Record;
                Record.OwnerAssetPath = BPPath;
                Record.ActorLabel = RawPlaceholderName;
                Record.AssetName = AssetName;
                Record.SavedTransform = SavedTransform;
                Record.bOwnerIsLevel = false;
                SwapLog.Add(Record);
                continue;
            }

            // ----- legacy path ----------------------------------------
            // Check for gameplay wrapper FIRST
            FString GPPath = FString::Printf(
                TEXT("/Game/MapLink/%s/%s_GP"), *AssetName, *AssetName);
            UBlueprint* GPBP = LoadObject<UBlueprint>(nullptr, *GPPath);

            FString MainPath = FString::Printf(
                TEXT("/Game/MapLink/%s/%s"), *AssetName, *AssetName);
            UBlueprint* MainBP = LoadObject<UBlueprint>(nullptr, *MainPath);

            UBlueprint* TargetBP = nullptr;
            if (GPBP && GPBP->GeneratedClass)
            {
                TargetBP = GPBP;
                UE_LOG(LogMapLink, Log,
                       TEXT("  Using gameplay wrapper '%s_GP' for placeholder '%s'."),
                       *AssetName, *RawPlaceholderName);
            }
            else if (MainBP && MainBP->GeneratedClass)
            {
                TargetBP = MainBP;
            }

            if (TargetBP)
            {
                FString CAName = FString::Printf(TEXT("Child_%s"), *RawPlaceholderName);
                USCS_Node* CANode = SCS->CreateNode(
                    UChildActorComponent::StaticClass(), *CAName);

                if (CANode)
                {
                    UChildActorComponent* CAComp =
                        Cast<UChildActorComponent>(CANode->ComponentTemplate);
                    CAComp->SetChildActorClass(
                        static_cast<TSubclassOf<AActor>>(TargetBP->GeneratedClass));
                    CAComp->SetRelativeTransform_Direct(SavedTransform);

                    if (ParentNode)
                        ParentNode->AddChildNode(CANode);
                    else
                        SCS->AddNode(CANode);
                }
            }
            else
            {
                // v1.8 D7 hotfix2 — Legacy nested-BP path found no
                // synthesized BP.  Fall back to BP_Deprecated and add
                // it as a ChildActorComponent so the placeholder is
                // visible in the outliner instead of silently vanishing.
                bool bDummyDeprecated = false;
                UClass* DeprecatedClass =
                    FMapLinkBPReferenceMode::ResolveBoundClassFromPath(
                        FString(), bDummyDeprecated);

                if (DeprecatedClass)
                {
                    FString CAName =
                        FString::Printf(TEXT("Child_MISSING_%s"), *RawPlaceholderName);
                    USCS_Node* CANode = SCS->CreateNode(
                        UChildActorComponent::StaticClass(), *CAName);

                    if (CANode)
                    {
                        UChildActorComponent* CAComp =
                            Cast<UChildActorComponent>(CANode->ComponentTemplate);
                        CAComp->SetChildActorClass(
                            static_cast<TSubclassOf<AActor>>(DeprecatedClass));
                        CAComp->SetRelativeTransform_Direct(SavedTransform);
                        CAComp->ComponentTags.Add(FName(*FString::Printf(
                            TEXT("MapLinkMissingBP:%s"), *AssetName)));

                        if (ParentNode)
                            ParentNode->AddChildNode(CANode);
                        else
                            SCS->AddNode(CANode);

                        FMapLinkBPReferenceMode::FPlacement Rec;
                        Rec.ProxyPath          = FString();
                        Rec.BoundBlueprintPath = FString();
                        Rec.PlacementContext   = TEXT("bp_nested");
                        Rec.OwnerAssetPath     = BPPath;
                        Rec.Status             =
                            FMapLinkBPReferenceMode::EPlacementStatus::DeprecatedSubstituted;
                        FMapLinkBPReferenceMode::RecordPlacement(Rec);

                        UE_LOG(LogMapLinkBPStandIn, Warning,
                               TEXT("  Nested BP_ '%s' (asset '%s') -> "
                                    "BP_Deprecated ChildActorComponent "
                                    "(no proxy, no synth BP, no ref-mode "
                                    "binding found)."),
                               *RawPlaceholderName, *AssetName);
                    }
                }
                else
                {
                    UE_LOG(LogMapLink, Warning,
                           TEXT("No BP asset for placeholder '%s' (asset name "
                                "'%s') AND BP_Deprecated could not be loaded."),
                           *RawPlaceholderName, *AssetName);
                }
            }

            NodesToRemove.Add(Node);

            FMapLinkStubRecord Record;
            Record.OwnerAssetPath = BPPath;
            Record.ActorLabel = RawPlaceholderName;
            Record.AssetName = AssetName;
            Record.SavedTransform = SavedTransform;
            Record.bOwnerIsLevel = false;
            SwapLog.Add(Record);
            continue;
        }

        // ==============================================================
        // LI_ and fallback — existing behavior
        // ==============================================================
        {
            FString AssetPath = FString::Printf(
                TEXT("/Game/MapLink/%s/%s"), *AssetName, *AssetName);

            UObject* MatchedAsset = LoadObject<UObject>(nullptr, *AssetPath);

            if (!MatchedAsset)
            {
                UE_LOG(LogMapLink, Warning,
                       TEXT("No asset for BP placeholder '%s' (asset name '%s')."),
                       *RawPlaceholderName, *AssetName);
                continue;
            }

            FString CAName = FString::Printf(TEXT("Child_%s"), *RawPlaceholderName);
            USCS_Node* CANode = SCS->CreateNode(
                UChildActorComponent::StaticClass(), *CAName);

            if (!CANode) continue;

            UChildActorComponent* CAComp =
                Cast<UChildActorComponent>(CANode->ComponentTemplate);

            bool bIsLevel = MatchedAsset->IsA<UWorld>();

            if (!bIsLevel)
            {
                UBlueprint* TargetBP = Cast<UBlueprint>(MatchedAsset);
                if (TargetBP && TargetBP->GeneratedClass)
                {
                    CAComp->SetChildActorClass(
                        static_cast<TSubclassOf<AActor>>(TargetBP->GeneratedClass));
                }
            }
            else
            {
                CAComp->ComponentTags.Add(
                    FName(*FString::Printf(
                        TEXT("MapLinkLevelRef:%s"), *AssetName)));
                UE_LOG(LogMapLink, Log,
                       TEXT("Level instance reference '%s' in BP '%s' tagged for runtime resolution."),
                       *AssetName, *BlendName);
            }

            CAComp->SetRelativeTransform_Direct(SavedTransform);

            if (ParentNode)
                ParentNode->AddChildNode(CANode);
            else
                SCS->AddNode(CANode);

            NodesToRemove.Add(Node);

            FMapLinkStubRecord Record;
            Record.OwnerAssetPath = BPPath;
            Record.ActorLabel = RawPlaceholderName;
            Record.AssetName = AssetName;
            Record.SavedTransform = SavedTransform;
            Record.bOwnerIsLevel = false;
            SwapLog.Add(Record);
        }
    }

    // Remove old placeholder nodes
    for (USCS_Node* Node : NodesToRemove)
    {
        SCS->RemoveNode(Node);
    }

    // Recompile and save
    FKismetEditorUtilities::CompileBlueprint(BP);
    BP->MarkPackageDirty();
    SaveBPPackage(BP);

    // Write swap log
    WriteSwapLog(BlendName, SwapLog, TEXT("swap_bp.json"));

    UE_LOG(LogMapLink, Log,
           TEXT("Blueprint swap complete for '%s': %d placeholders swapped."),
           *BlendName, SwapLog.Num());

    return EMapLinkResult::Success;
}

void FMapLinkBlueprintHelper::WriteSwapLog(
    const FString& BlendName,
    const TArray<FMapLinkStubRecord>& Records,
    const FString& LogFileName)
{
    FMapLinkUtils::WriteSwapLog(BlendName, Records, LogFileName);
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

FString FMapLinkBlueprintHelper::StripBlenderSuffix(const FString& Name)
{
    return FMapLinkUtils::StripBlenderSuffix(Name);
}

void FMapLinkBlueprintHelper::SaveBPPackage(UBlueprint* BP)
{
    if (!BP) return;

    UPackage* Package = BP->GetOutermost();
    FString PackageFilename = FPackageName::LongPackageNameToFilename(
        Package->GetName(), FPackageName::GetAssetPackageExtension());

    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    // v1.13.1 — never route save errors to GError (fatal appError on any
    // failed save). Log instead; callers/logs surface the failure.
    SaveArgs.Error     = GLog;
    SaveArgs.SaveFlags = SAVE_NoError;
    if (!UPackage::SavePackage(Package, BP, *PackageFilename, SaveArgs))
    {
        UE_LOG(LogMapLink, Error,
               TEXT("Failed to save Blueprint package '%s'."),
               *Package->GetName());
    }
}
