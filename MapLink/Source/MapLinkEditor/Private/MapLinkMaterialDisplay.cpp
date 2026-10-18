// Copyright MapModelers. All Rights Reserved.
//
// v1.7.3 — Material Instance display level generator.
//
// Creates a regular (non-WP) .umap at the configured path, populates it
// with a grid of cubes showing each MI on Element 0.  Actors tagged
// "MapLinkMaterialDisplay" are removed on refresh; everything in the
// "Persistent" outliner folder is preserved.

#include "MapLinkMaterialDisplay.h"
#include "MapLinkSettings.h"
#include "MapLinkPipeline.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "EngineUtils.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/ScopedSlowTask.h"
#include "HAL/PlatformFileManager.h"
#include "UObject/SavePackage.h"

namespace
{
    /** Tag applied to every actor spawned by this helper — used for cleanup. */
    static const FName TAG_MaterialDisplay(TEXT("MapLinkMaterialDisplay"));

    /** Remove all actors tagged MapLinkMaterialDisplay from the world,
     *  while preserving everything in the "Persistent" outliner folder
     *  and all engine actors (WorldSettings, etc.). */
    int32 CleanupDisplayActors(UWorld* World)
    {
        TArray<AActor*> ToRemove;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* Actor = *It;
            if (Actor->Tags.Contains(TAG_MaterialDisplay))
            {
                ToRemove.Add(Actor);
            }
        }

        for (AActor* Actor : ToRemove)
        {
            World->DestroyActor(Actor);
        }

        return ToRemove.Num();
    }
}

EMapLinkResult FMapLinkMaterialDisplay::UpdateMaterialDisplayLevel()
{
    const UMapLinkSettings* Settings = UMapLinkSettings::Get();
    const FString ScanPath  = Settings->MaterialDisplayScanPath.Path;
    const FString LevelName = Settings->MaterialDisplayLevelName;
    const float   Spacing   = Settings->MaterialDisplayGridSpacing;
    const int32   Columns   = FMath::Max(1, Settings->MaterialDisplayGridColumns);

    // ── 1. Scan for Material Instances ────────────────────────────
    FAssetRegistryModule& ARModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AR = ARModule.Get();

    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*ScanPath));
    Filter.bRecursivePaths = true;
    Filter.ClassPaths.Add(UMaterialInstanceConstant::StaticClass()->GetClassPathName());

    TArray<FAssetData> MIAssets;
    AR.GetAssets(Filter, MIAssets);

    if (MIAssets.Num() == 0)
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("[MatDisplay] No Material Instances found in %s"), *ScanPath);
        return EMapLinkResult::NoMaterialInstancesFound;
    }

    // Sort alphabetically for deterministic grid ordering
    MIAssets.Sort([](const FAssetData& A, const FAssetData& B)
    {
        return A.AssetName.LexicalLess(B.AssetName);
    });

    UE_LOG(LogMapLink, Log,
           TEXT("[MatDisplay] Found %d Material Instance(s) in %s."),
           MIAssets.Num(), *ScanPath);

    // ── 2. Resolve level path ────────────────────────────────────
    // Content path: e.g. /Game/MapLink/MaterialInstances/L_MaterialInstances
    FString LevelContentPath = ScanPath;
    if (!LevelContentPath.EndsWith(TEXT("/")))
    {
        LevelContentPath += TEXT("/");
    }
    LevelContentPath += LevelName;

    FString LevelDiskPath = FPackageName::LongPackageNameToFilename(
        LevelContentPath, FPackageName::GetMapPackageExtension());

    bool bLevelExists = FPaths::FileExists(LevelDiskPath);

    UE_LOG(LogMapLink, Log,
           TEXT("[MatDisplay] Level: %s (%s)"),
           *LevelContentPath,
           bLevelExists ? TEXT("exists — updating") : TEXT("creating new"));

    // ── 3. Prompt save current work ──────────────────────────────
    FEditorFileUtils::SaveDirtyPackages(
        /*bPromptUserToSave=*/true,
        /*bSaveMapPackages=*/true,
        /*bSaveContentPackages=*/true);

    // ── 4. Load or create the display level ──────────────────────
    if (bLevelExists)
    {
        FEditorFileUtils::LoadMap(LevelContentPath);
    }
    else
    {
        // Create directory on disk
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(LevelDiskPath), true);

        // Create a blank world package, save it to disk, then load it
        // into the editor via LoadMap.
        {
            UPackage* NewPackage = CreatePackage(*LevelContentPath);
            NewPackage->MarkAsFullyLoaded();
            NewPackage->MarkPackageDirty();

            UWorld* NewWorld = UWorld::CreateWorld(
                EWorldType::None, /*bInformEngineOfWorld=*/false,
                FName(*LevelName), NewPackage);
            NewWorld->SetFlags(RF_Public | RF_Standalone);

            // Save to disk
            FSavePackageArgs SaveArgs;
            SaveArgs.TopLevelFlags = RF_Standalone;
            // v1.13.1 — never route save errors to GError (fatal appError).
            SaveArgs.Error     = GLog;
            SaveArgs.SaveFlags = SAVE_NoError;
            if (!UPackage::SavePackage(NewPackage, NewWorld, *LevelDiskPath,
                                       SaveArgs))
            {
                UE_LOG(LogMapLink, Error,
                       TEXT("[MatDisplay] Failed to save '%s'."),
                       *LevelDiskPath);
            }

            // Cleanup the in-memory package before loading via editor
            NewWorld->DestroyWorld(/*bInformEngineOfWorld=*/false);
        }
        CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

        // Now load the freshly-created level into the editor
        FEditorFileUtils::LoadMap(LevelContentPath);
    }

    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        UE_LOG(LogMapLink, Error, TEXT("[MatDisplay] Failed to get editor world."));
        return EMapLinkResult::MaterialDisplayLevelCreateFailed;
    }

    // ── 5. Cleanup previous display cubes ────────────────────────
    int32 Removed = CleanupDisplayActors(World);
    if (Removed > 0)
    {
        UE_LOG(LogMapLink, Log,
               TEXT("[MatDisplay] Removed %d previous display actors."), Removed);
    }

    // ── 6. Load display meshes ───────────────────────────────────
    UStaticMesh* CubeNormal = LoadObject<UStaticMesh>(
        nullptr, TEXT("/MapLink/Materials/mat_cube.mat_cube"));
    UStaticMesh* CubeDsp = LoadObject<UStaticMesh>(
        nullptr, TEXT("/MapLink/Materials/mat_cube_dsp.mat_cube_dsp"));

    if (!CubeNormal)
    {
        UE_LOG(LogMapLink, Error,
               TEXT("[MatDisplay] mat_cube not found at /MapLink/Materials/mat_cube"));
        FMessageDialog::Open(EAppMsgType::Ok,
            FText::FromString(TEXT(
                "MapLink: Display mesh 'mat_cube' not found.\n"
                "Expected at /MapLink/Materials/mat_cube")));
        return EMapLinkResult::MaterialDisplayMeshNotFound;
    }

    // Fallback: use normal cube if Dsp cube not found
    if (!CubeDsp)
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("[MatDisplay] mat_cube_dsp not found — using mat_cube for Dsp MIs."));
        CubeDsp = CubeNormal;
    }

    // ── 7. Spawn cubes in grid ───────────────────────────────────
    FScopedSlowTask SlowTask(
        static_cast<float>(MIAssets.Num()),
        FText::FromString(TEXT("MapLink: Spawning material display cubes…")));
    SlowTask.MakeDialog(/*bShowCancelButton=*/false);

    int32 SpawnedCount = 0;

    for (int32 i = 0; i < MIAssets.Num(); ++i)
    {
        SlowTask.EnterProgressFrame(1.f);

        UMaterialInstanceConstant* MI =
            Cast<UMaterialInstanceConstant>(MIAssets[i].GetAsset());
        if (!MI) continue;

        const FString MIName = MI->GetName();

        // Select mesh: mat_cube_dsp if MI name contains "Dsp", else mat_cube
        bool bIsDsp = MIName.Contains(TEXT("Dsp"), ESearchCase::IgnoreCase);
        UStaticMesh* CubeMesh = bIsDsp ? CubeDsp : CubeNormal;

        // Grid position
        int32 Col = SpawnedCount % Columns;
        int32 Row = SpawnedCount / Columns;
        FVector Location(Col * Spacing, Row * Spacing, 0.f);

        // Spawn static mesh actor
        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride =
            ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

        AStaticMeshActor* CubeActor = World->SpawnActor<AStaticMeshActor>(
            AStaticMeshActor::StaticClass(),
            FTransform(FRotator::ZeroRotator, Location, FVector::OneVector),
            SpawnParams);

        if (!CubeActor) continue;

        CubeActor->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);

        // Set Element 0 = MI (outer shell), leave Element 1 default
        CubeActor->GetStaticMeshComponent()->SetMaterial(0, MI);

        // Label and tag
        CubeActor->SetActorLabel(MIName);
        CubeActor->Tags.Add(TAG_MaterialDisplay);

        // Place in "MaterialDisplay" outliner folder to keep organized
        CubeActor->SetFolderPath(FName(TEXT("MaterialDisplay")));

        ++SpawnedCount;

        UE_LOG(LogMapLink, Verbose,
               TEXT("[MatDisplay] Spawned '%s' at (%d, %d) — %s"),
               *MIName, Col, Row, bIsDsp ? TEXT("Dsp") : TEXT("Normal"));
    }

    UE_LOG(LogMapLink, Log,
           TEXT("[MatDisplay] Spawned %d display cubes (%d columns, %.0f spacing)."),
           SpawnedCount, Columns, Spacing);

    // ── 8. Save the level ────────────────────────────────────────
    // Package was already renamed to the target content path (for new levels)
    // or was loaded from the correct path (for existing levels).
    FEditorFileUtils::SaveCurrentLevel();

    UE_LOG(LogMapLink, Log,
           TEXT("[MatDisplay] Level saved: %s"), *LevelContentPath);

    return EMapLinkResult::Success;
}
