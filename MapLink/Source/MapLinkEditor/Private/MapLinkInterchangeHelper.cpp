// Copyright MapModelers. All Rights Reserved.

#include "MapLinkInterchangeHelper.h"
#include "MapLinkUtils.h"
#include "MapLinkPipeline.h"
#include "MapLinkSettings.h"   // v1.14 -- FolderMode / FolderScope

#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Components/StaticMeshComponent.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"
#include "FileHelpers.h"
#include "HAL/PlatformFileManager.h"   // locked-.umap release/probe before SavePackage

// FBX import
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxStaticMeshImportData.h"
#include "Factories/FbxTextureImportData.h"
#include "Factories/MaterialImportHelpers.h"

// Asset tools
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AutomatedAssetImportData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectHash.h"       // v1.13.1 -- GetObjectsOfClass (linker release)
#include "MapLinkSourceControl.h"   // v1.9
#include "UObject/LinkerLoad.h"

// JSON
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"

// ───────────────────────────────────────────────────────────────
// Public
// ───────────────────────────────────────────────────────────────

EMapLinkResult FMapLinkInterchangeHelper::ImportAndCollectResults(
    const FString& FbxFilePath,
    const FString& BlendName,
    const FString& PluginBaseDir,
    TArray<FMapLinkImportedMesh>& OutMeshes,
    TArray<FMapLinkImportedEmpty>& OutEmpties)
{
    // 1. Parse analysis.json
    FString AnalysisPath = FMapLinkUtils::ResolveArtifactLogFile(
        BlendName / TEXT("analysis.json"));
    FMapLinkAnalysisReport Report;
    if (!ParseAnalysisJson(AnalysisPath, Report))
    {
        UE_LOG(LogMapLink, Error,
               TEXT("Failed to parse analysis.json at '%s'."), *AnalysisPath);
        return EMapLinkResult::FbxNotFound;
    }

    UE_LOG(LogMapLink, Log,
           TEXT("Analysis: %d own-geo, %d linked-assets for '%s' (unit_scale=%.4f, pos_factor=%.1f)."),
           Report.OwnGeo.Num(), Report.LinkedAssets.Num(), *BlendName,
           Report.UnitScale, Report.UnitScale * 100.0);

    // 2. Import mesh assets from FBX (only if there is own geometry)
    TMap<FString, UStaticMesh*> MeshMap;
    if (Report.OwnGeo.Num() > 0)
    {
        if (!FPaths::FileExists(FbxFilePath))
        {
            UE_LOG(LogMapLink, Error,
                   TEXT("FBX file not found: '%s'."), *FbxFilePath);
            return EMapLinkResult::FbxNotFound;
        }

        EMapLinkResult R = ImportMeshAssetsFromFbx(FbxFilePath, BlendName, MeshMap);
        if (R != EMapLinkResult::Success)
        {
            return R;
        }

        UE_LOG(LogMapLink, Log,
               TEXT("Imported %d mesh assets from FBX."), MeshMap.Num());
    }

    // 3. Build OutMeshes from analysis.json + imported mesh map
    //    FBX import prepends the blend filename to mesh names:
    //      "Cube" → "LI_Test01_Cube"    (prefixed)
    //      "Plane" → "LI_Test03"         (single mesh gets just the filename)
    //    Also, dots become underscores: "Cylinder.001" → "Cylinder_001"
    for (const FMapLinkOwnGeoEntry& Geo : Report.OwnGeo)
    {
        FMapLinkImportedMesh Entry;
        Entry.Name = Geo.Name;
        Entry.Transform = Geo.WorldTransform;
        Entry.CollectionPath = Geo.CollectionPath;   // v1.14 — folder scope
        Entry.StaticMesh = MeshMap.FindRef(Geo.Name);

        if (!Entry.StaticMesh)
        {
            // Try matching by stripping the BlendName prefix from imported names
            // and handling dot→underscore conversion
            FString GeoNameSanitized = Geo.Name.Replace(TEXT("."), TEXT("_"));
            FString Prefix = BlendName + TEXT("_");

            for (auto& Pair : MeshMap)
            {
                FString ImportedName = Pair.Key;

                // Strip the BlendName_ prefix from the imported name
                FString StrippedName = ImportedName;
                if (StrippedName.StartsWith(Prefix))
                {
                    StrippedName = StrippedName.RightChop(Prefix.Len());
                }

                // Match: original name, sanitized name, or contains check
                if (StrippedName == Geo.Name ||
                    StrippedName == GeoNameSanitized ||
                    ImportedName.Contains(GeoNameSanitized) ||
                    ImportedName.Contains(Geo.Name))
                {
                    Entry.StaticMesh = Pair.Value;
                    break;
                }
            }
        }

        if (!Entry.StaticMesh)
        {
            // Last resort: if there's only one mesh in the map, use it
            if (MeshMap.Num() == 1 && Report.OwnGeo.Num() == 1)
            {
                for (auto& Pair : MeshMap)
                {
                    Entry.StaticMesh = Pair.Value;
                }
            }
        }

        if (!Entry.StaticMesh)
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("No matching mesh asset for own-geo '%s'. Available: %d"),
                   *Geo.Name, MeshMap.Num());
            for (auto& Pair : MeshMap)
            {
                UE_LOG(LogMapLink, Warning,
                       TEXT("  Available mesh: '%s'"), *Pair.Key);
            }
        }

        OutMeshes.Add(Entry);
    }

    // 4. Build OutEmpties from analysis.json linked_assets
    //    CRITICAL: The placeholder actor label must use the BLEND FILE STEM
    //    (e.g. "LI_Test01") not the Blender collection name (e.g. "LI_Test_01"),
    //    because UE assets are created at /Game/MapLink/<BlendStem>/<BlendStem>.
    //    The instance suffix (.001, .002) is preserved for uniqueness.
    for (const FMapLinkLinkedAssetEntry& LA : Report.LinkedAssets)
    {
        FMapLinkImportedEmpty Entry;

        // Derive the blend file stem from source_blend_path
        FString BlendStem = FPaths::GetBaseFilename(LA.SourceBlendPath);

        if (BlendStem.IsEmpty())
        {
            // Fallback: use the Blender collection name as-is
            BlendStem = LA.AssetName;
        }

        // Preserve the Blender instance suffix (.001, .002, etc.)
        FString OriginalName = LA.Name;
        int32 DotIdx = INDEX_NONE;
        if (OriginalName.FindLastChar(TEXT('.'), DotIdx))
        {
            FString Suffix = OriginalName.RightChop(DotIdx + 1);
            if (Suffix.IsNumeric())
            {
                Entry.Name = FString::Printf(TEXT("%s.%s"), *BlendStem, *Suffix);
            }
            else
            {
                Entry.Name = BlendStem;
            }
        }
        else
        {
            Entry.Name = BlendStem;
        }

        Entry.Transform = LA.WorldTransform;
        Entry.CollectionPath = LA.CollectionPath;    // v1.14 — folder scope
        OutEmpties.Add(Entry);

        UE_LOG(LogMapLink, Log,
               TEXT("  Empty: '%s' → label '%s' (collection '%s')"),
               *LA.Name, *Entry.Name, *LA.AssetName);
    }

    UE_LOG(LogMapLink, Log,
           TEXT("Collected results: %d meshes, %d empties for '%s'."),
           OutMeshes.Num(), OutEmpties.Num(), *BlendName);

    return EMapLinkResult::Success;
}

EMapLinkResult FMapLinkInterchangeHelper::CreateLevelFromResults(
    const FString& BlendName,
    const TArray<FMapLinkImportedMesh>& Meshes,
    const TArray<FMapLinkImportedEmpty>& Empties)
{
    FString FullDestPath = FString::Printf(
        TEXT("/Game/MapLink/%s/%s"), *BlendName, *BlendName);
    FString PackageFilename = FPackageName::LongPackageNameToFilename(
        FullDestPath, FPackageName::GetMapPackageExtension());

    UWorld* TargetWorld = nullptr;
    UPackage* Package = nullptr;
    bool bModifyInPlace = false;

    // v1.13.1 — Level Instances load the target level as INSTANCED packages
    // (unique in-memory names, SAME file on disk). Their linkers keep an OS
    // handle on the .umap even after the embedding world is torn down, which
    // made the final SavePackage fail with a share violation (error 32) and
    // — via the old GError route — crash the editor. Reset loaders on every
    // in-memory package whose loaded path maps to this file.
    auto ReleaseLinkersForFile = [&FullDestPath, &PackageFilename]()
    {
        const FString NormTarget =
            FPaths::ConvertRelativePathToFull(PackageFilename);
        TArray<UObject*> AllPackages;
        GetObjectsOfClass(UPackage::StaticClass(), AllPackages,
                          /*bIncludeDerivedClasses=*/false);
        for (UObject* Obj : AllPackages)
        {
            UPackage* Pkg = Cast<UPackage>(Obj);
            if (!Pkg)
            {
                continue;
            }
            bool bMatch = (Pkg->GetName() == FullDestPath);
            if (!bMatch && !Pkg->GetLoadedPath().IsEmpty())
            {
                const FString LocalPath = FPaths::ConvertRelativePathToFull(
                    Pkg->GetLoadedPath().GetLocalFullPath());
                bMatch = LocalPath.Equals(NormTarget, ESearchCase::IgnoreCase);
            }
            if (bMatch)
            {
                UE_LOG(LogMapLink, Log,
                       TEXT("[CreateLevel] Releasing linker for in-memory "
                            "package '%s' (maps to target .umap)."),
                       *Pkg->GetName());
                ResetLoaders(Pkg);
            }
        }
    };

    // --- Diagnostic: log current editor state ---
    {
        UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        FString EditorPkgName = EditorWorld
            ? EditorWorld->GetOutermost()->GetName() : TEXT("<none>");
        UE_LOG(LogMapLink, Log,
               TEXT("[CreateLevel] BlendName='%s', FullDestPath='%s', EditorWorld='%s'"),
               *BlendName, *FullDestPath, *EditorPkgName);
    }

    // --- Check if this level is currently the active editor world ---
    // If so, we CANNOT delete/evict it. Instead, clear actors in-place.
    {
        UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (EditorWorld)
        {
            FString CurrentPkgName = EditorWorld->GetOutermost()->GetName();
            if (CurrentPkgName == FullDestPath)
            {
                UE_LOG(LogMapLink, Log,
                       TEXT("[CreateLevel] Level '%s' is currently open — modifying in-place."),
                       *BlendName);
                TargetWorld = EditorWorld;
                Package = EditorWorld->GetOutermost();
                bModifyInPlace = true;
            }
        }
    }

    if (bModifyInPlace)
    {
        // --- In-place path: clear only PERSISTENT level actors ---
        TArray<AActor*> ToDestroy;
        for (AActor* Actor : TargetWorld->PersistentLevel->Actors)
        {
            if (Actor && !Actor->IsA<AWorldSettings>())
            {
                ToDestroy.Add(Actor);
            }
        }
        for (AActor* Actor : ToDestroy)
        {
            TargetWorld->DestroyActor(Actor);
        }
        UE_LOG(LogMapLink, Log,
               TEXT("[CreateLevel] Cleared %d persistent actors from level '%s'."),
               ToDestroy.Num(), *BlendName);
    }
    else
    {
        // --- New-creation path: delete old file, evict stale package, create fresh ---

        IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
        PlatformFile.CreateDirectoryTree(*FPaths::GetPath(PackageFilename));
        if (PlatformFile.FileExists(*PackageFilename))
        {
            PlatformFile.DeleteFile(*PackageFilename);

            // v1.15 — FIRST check the cheap, common cause: a READ-ONLY
            // file (P4 sync state; with no Revision Control provider
            // connected the checkout discipline never runs).  The old
            // code went straight to the blank-map dance and then
            // misreported this as "locked by another process", which
            // cost a full day of chasing phantom file locks.
            if (PlatformFile.FileExists(*PackageFilename) &&
                PlatformFile.IsReadOnly(*PackageFilename))
            {
                UE_LOG(LogMapLink, Warning,
                       TEXT("[CreateLevel] .umap is READ-ONLY (P4-synced, "
                            "provider not connected?) — clearing the "
                            "attribute: '%s'"), *PackageFilename);
                PlatformFile.SetReadOnly(*PackageFilename, false);
                PlatformFile.DeleteFile(*PackageFilename);
            }

            if (PlatformFile.FileExists(*PackageFilename))
            {
                // File is locked — the target level is loaded, typically as a
                // Level Instance / streamed sub-level inside the level that is
                // currently open in the editor (which need NOT be a MapLink
                // level itself, so the pipeline's earlier switch-away guard may
                // not have fired). Open a fresh blank map to release every OS
                // file handle, then retry the delete. Portable across any host
                // project — never load a hardcoded content map. The reimport
                // pipeline restores the user's original level at the end.
                UE_LOG(LogMapLink, Warning,
                       TEXT("[CreateLevel] .umap locked — opening a blank map to "
                            "release handles: '%s'"), *PackageFilename);
                if (GEditor)
                {
                    GEditor->CreateNewMapForEditing();
                    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
                }
                // v1.13.1 — the blank map alone does not close linkers of
                // INSTANCED copies of this level (RF_Standalone survives the
                // KEEPFLAGS purge). Drop their file handles explicitly.
                ReleaseLinkersForFile();
                PlatformFile.DeleteFile(*PackageFilename);
            }

            // If the old .umap is STILL on disk, it is held by something a blank
            // map did not release. SavePackage's internal atomic MoveFile would
            // then fail on the lock and call appError() — a FATAL editor crash
            // (the exact crash this guard fixes). The file must be *removable*
            // for the save to succeed, so treat "still present" as fatal-to-this
            // -node and abort gracefully instead of entering SavePackage.
            if (PlatformFile.FileExists(*PackageFilename))
            {
                UE_LOG(LogMapLink, Error,
                       TEXT("[CreateLevel] Target .umap could not be deleted: "
                            "'%s' (%s). Aborting reimport of '%s' — nothing "
                            "further is destroyed. If read-only: connect "
                            "Revision Control or check the file out in P4V. "
                            "If locked: close the level that embeds it (or "
                            "unload the Level Instance) and reimport."),
                       *PackageFilename,
                       PlatformFile.IsReadOnly(*PackageFilename)
                           ? TEXT("READ-ONLY — P4 sync state, provider not "
                                  "connected?")
                           : TEXT("file handle still held by this or another "
                                  "process"),
                       *BlendName);
                return EMapLinkResult::LevelLocked;
            }

            UE_LOG(LogMapLink, Log,
                   TEXT("[CreateLevel] Deleted old .umap: '%s'"), *PackageFilename);
        }

        // Evict any stale in-memory package
        UPackage* StalePkg = FindPackage(nullptr, *FullDestPath);
        if (!StalePkg)
            StalePkg = FindObject<UPackage>(nullptr, *FullDestPath);
        if (StalePkg)
        {
            UE_LOG(LogMapLink, Log,
                   TEXT("[CreateLevel] Evicting stale package '%s' (flags=0x%08X)"),
                   *StalePkg->GetName(), StalePkg->GetPackageFlags());
            ResetLoaders(StalePkg);
            FName TempName = MakeUniqueObjectName(
                GetTransientPackage(), UPackage::StaticClass(),
                FName(*FString::Printf(TEXT("MapLink_Evicted_%s"), *BlendName)));
            StalePkg->Rename(*TempName.ToString(), nullptr,
                REN_DontCreateRedirectors | REN_NonTransactional | REN_ForceNoResetLoaders);
        }
        else
        {
            UE_LOG(LogMapLink, Log,
                   TEXT("[CreateLevel] No stale package found for '%s'."), *FullDestPath);
        }
        CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

        Package = CreatePackage(*FullDestPath);
        if (!Package)
        {
            UE_LOG(LogMapLink, Error,
                   TEXT("[CreateLevel] Failed to create package for '%s'."), *FullDestPath);
            return EMapLinkResult::TempLevelMissing;
        }

        // CRITICAL: Force-clear any inherited flags from stale packages.
        Package->MarkAsFullyLoaded();
        UE_LOG(LogMapLink, Log,
               TEXT("[CreateLevel] Fresh package created (flags=0x%08X)"),
               Package->GetPackageFlags());

        TargetWorld = UWorld::CreateWorld(EWorldType::Editor, false,
            *BlendName, Package);
        if (!TargetWorld)
        {
            UE_LOG(LogMapLink, Error,
                   TEXT("[CreateLevel] Failed to create world for '%s'."), *BlendName);
            return EMapLinkResult::TempLevelMissing;
        }
        TargetWorld->SetFlags(RF_Public | RF_Standalone);
    }

    // --- Spawn actors (same for both paths) ---
    int32 MeshCount = 0;
    int32 EmptyCount = 0;

    // v1.14 — folder creation inside generated levels: only when the user
    // opted into "All created levels" (default "Expanded level only"
    // reproduces the pre-v1.14 flat layout).  Generated LI_ levels are
    // plain levels, so SetFolderPath() is sufficient (the actor-backed-WP
    // EnsureFolderHierarchy dance is only needed in WP levels).
    const EMapLinkFolderMode  FolderMode  = UMapLinkSettings::GetFolderMode();
    const EMapLinkFolderScope FolderScope = UMapLinkSettings::GetFolderScope();
    const bool bFoldersInCreatedLevels =
        (FolderScope == EMapLinkFolderScope::AllLevels) &&
        (FolderMode  != EMapLinkFolderMode::None);

    for (const FMapLinkImportedMesh& Mesh : Meshes)
    {
        if (!Mesh.StaticMesh)
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("Skipping mesh '%s' — no StaticMesh asset."), *Mesh.Name);
            continue;
        }

        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride =
            ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        SpawnParams.OverrideLevel = TargetWorld->PersistentLevel;

        AStaticMeshActor* SMActor = TargetWorld->SpawnActor<AStaticMeshActor>(
            AStaticMeshActor::StaticClass(),
            Mesh.Transform,
            SpawnParams);

        if (SMActor)
        {
            SMActor->GetStaticMeshComponent()->SetStaticMesh(Mesh.StaticMesh);
            SMActor->SetActorLabel(Mesh.Name);
            SMActor->SetActorScale3D(Mesh.Transform.GetScale3D());
            if (bFoldersInCreatedLevels && !Mesh.CollectionPath.IsEmpty())
            {
                SMActor->SetFolderPath(FName(*Mesh.CollectionPath));
            }
            ++MeshCount;
        }
    }

    for (const FMapLinkImportedEmpty& Empty : Empties)
    {
        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride =
            ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        SpawnParams.OverrideLevel = TargetWorld->PersistentLevel;

        AStaticMeshActor* PlaceholderActor = TargetWorld->SpawnActor<AStaticMeshActor>(
            AStaticMeshActor::StaticClass(),
            Empty.Transform,
            SpawnParams);

        if (PlaceholderActor)
        {
            PlaceholderActor->SetActorLabel(Empty.Name);
            PlaceholderActor->Tags.Add(FName(TEXT("MapLinkPlaceholder")));
            PlaceholderActor->SetActorScale3D(Empty.Transform.GetScale3D());
            if (bFoldersInCreatedLevels)
            {
                // Collection chain; in "Collection + instanced collection"
                // mode each instance additionally gets its OWN folder named
                // after the instance ("LI_uneplace", "LI_uneplace_001", …) —
                // one folder per instanced collection, not per asset type.
                // Dots sanitized ('.NNN' → '_NNN'): outliner folder objects
                // cannot carry '.' in their names.
                FString Folder = Empty.CollectionPath;
                if (FolderMode ==
                    EMapLinkFolderMode::CollectionAndInstanceFolders)
                {
                    const FString Seg =
                        Empty.Name.Replace(TEXT("."), TEXT("_"));
                    Folder = Folder.IsEmpty() ? Seg : (Folder / Seg);
                }
                if (!Folder.IsEmpty())
                {
                    PlaceholderActor->SetFolderPath(FName(*Folder));
                }
            }
            ++EmptyCount;
        }
    }

    // --- Save ---
    if (bModifyInPlace)
    {
        // Save the currently open level directly
        FEditorFileUtils::SaveCurrentLevel();
        UE_LOG(LogMapLink, Log,
               TEXT("Level updated in-place '%s': %d meshes, %d placeholders."),
               *FullDestPath, MeshCount, EmptyCount);
    }
    else
    {
        // Pre-save safety: force-mark package as fully loaded.
        // During reimport, CreatePackage can return a package that inherited
        // a "not fully loaded" state from a stale evicted package, which
        // causes SavePackage to fatal-assert.
        Package->MarkAsFullyLoaded();

        UE_LOG(LogMapLink, Log,
               TEXT("[CreateLevel] Saving package '%s' (flags=0x%08X, world='%s')"),
               *Package->GetName(), Package->GetPackageFlags(),
               *TargetWorld->GetName());

        // v1.9 — route generated level packages through the session
        // changelist (checkout existing / add new).
        FMapLinkSourceControl::CheckoutBeforeSave(Package);

        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        // v1.13.1 — CRASH FIX: the default FSavePackageArgs routes save
        // errors to GError, turning ANY failed save (e.g. a transient share
        // violation when a Level Instance re-grabbed the .umap handle) into
        // appError() — a fatal editor crash with unsaved user work. Route to
        // the log, retry after releasing handles, then fail THIS NODE
        // gracefully.
        SaveArgs.Error     = GLog;
        SaveArgs.SaveFlags = SAVE_NoError;

        bool bSaved = false;
        for (int32 Attempt = 0; Attempt < 3 && !bSaved; ++Attempt)
        {
            if (Attempt > 0)
            {
                UE_LOG(LogMapLink, Warning,
                       TEXT("[CreateLevel] Save failed for '%s' — releasing "
                            "handles and retrying (%d/2)."),
                       *PackageFilename, Attempt);
                FlushAsyncLoading();
                ReleaseLinkersForFile();
                CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
                FPlatformProcess::Sleep(0.3f);
            }
            bSaved = UPackage::SavePackage(
                Package, TargetWorld, *PackageFilename, SaveArgs);
        }

        if (bSaved)
        {
            FMapLinkSourceControl::AddAfterSave(Package);
        }

        if (bSaved)
        {
            FAssetRegistryModule::AssetCreated(TargetWorld);
            UE_LOG(LogMapLink, Log,
                   TEXT("Level saved to '%s': %d meshes, %d placeholders."),
                   *FullDestPath, MeshCount, EmptyCount);
        }
        else
        {
            UE_LOG(LogMapLink, Error,
                   TEXT("Failed to save level package '%s' after 3 attempts "
                        "(usually a locked .umap — close any level embedding "
                        "it as a Level Instance). The editor stays alive; "
                        "re-run Reimport to rebuild this node."),
                   *FullDestPath);
            TargetWorld->DestroyWorld(false);
            return EMapLinkResult::LevelLocked;
        }

        // Clean up — SwapPlaceholdersInLevel will load it fresh
        TargetWorld->DestroyWorld(false);
    }

    return EMapLinkResult::Success;
}

// ───────────────────────────────────────────────────────────────
// Analysis JSON parsing
// ───────────────────────────────────────────────────────────────

bool FMapLinkInterchangeHelper::ParseAnalysisJson(
    const FString& JsonPath,
    FMapLinkAnalysisReport& OutReport)
{
    FString JsonString;
    if (!FFileHelper::LoadFileToString(JsonString, *JsonPath))
    {
        return false;
    }

    TSharedPtr<FJsonObject> Root;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        return false;
    }

    // Parse unit_scale (default 1.0 for backwards compatibility)
    OutReport.UnitScale = 1.0;
    Root->TryGetNumberField(TEXT("unit_scale"), OutReport.UnitScale);

    // Parse blend_own_geo
    const TArray<TSharedPtr<FJsonValue>>* OwnGeoArray = nullptr;
    if (Root->TryGetArrayField(TEXT("blend_own_geo"), OwnGeoArray))
    {
        for (const TSharedPtr<FJsonValue>& Val : *OwnGeoArray)
        {
            const TSharedPtr<FJsonObject>& Obj = Val->AsObject();
            if (!Obj.IsValid()) continue;

            FMapLinkOwnGeoEntry Entry;
            Entry.Name = Obj->GetStringField(TEXT("name"));
            Obj->TryGetStringField(TEXT("collection_path"), Entry.CollectionPath);  // v1.13.1

            // Parse world_transform (16-element array)
            TArray<double> MatrixValues;
            const TArray<TSharedPtr<FJsonValue>>* TransformArray = nullptr;
            if (Obj->TryGetArrayField(TEXT("world_transform"), TransformArray))
            {
                for (const TSharedPtr<FJsonValue>& TV : *TransformArray)
                {
                    MatrixValues.Add(TV->AsNumber());
                }
            }
            Entry.WorldTransform = BlenderMatrixToUE5Transform(
                MatrixValues, OutReport.UnitScale);

            OutReport.OwnGeo.Add(Entry);
        }
    }

    // Parse linked_assets
    const TArray<TSharedPtr<FJsonValue>>* LinkedArray = nullptr;
    if (Root->TryGetArrayField(TEXT("linked_assets"), LinkedArray))
    {
        for (const TSharedPtr<FJsonValue>& Val : *LinkedArray)
        {
            const TSharedPtr<FJsonObject>& Obj = Val->AsObject();
            if (!Obj.IsValid()) continue;

            FMapLinkLinkedAssetEntry Entry;
            Entry.Name = Obj->GetStringField(TEXT("name"));
            Entry.AssetName = Obj->GetStringField(TEXT("asset_name"));
            Obj->TryGetStringField(TEXT("source_blend_path"), Entry.SourceBlendPath);
            Obj->TryGetStringField(TEXT("collection_path"), Entry.CollectionPath);  // v1.13.1

            TArray<double> MatrixValues;
            const TArray<TSharedPtr<FJsonValue>>* TransformArray = nullptr;
            if (Obj->TryGetArrayField(TEXT("world_transform"), TransformArray))
            {
                for (const TSharedPtr<FJsonValue>& TV : *TransformArray)
                {
                    MatrixValues.Add(TV->AsNumber());
                }
            }
            Entry.WorldTransform = BlenderMatrixToUE5Transform(
                MatrixValues, OutReport.UnitScale);

            OutReport.LinkedAssets.Add(Entry);
        }
    }

    // Parse libraries
    const TArray<TSharedPtr<FJsonValue>>* LibArray = nullptr;
    if (Root->TryGetArrayField(TEXT("libraries"), LibArray))
    {
        for (const TSharedPtr<FJsonValue>& Val : *LibArray)
        {
            const TSharedPtr<FJsonObject>& Obj = Val->AsObject();
            if (!Obj.IsValid()) continue;

            FString Path;
            if (Obj->TryGetStringField(TEXT("filepath"), Path))
            {
                OutReport.LibraryPaths.Add(Path);
            }
        }
    }

    return true;
}

// ───────────────────────────────────────────────────────────────
// Coordinate conversion
// ───────────────────────────────────────────────────────────────

FTransform FMapLinkInterchangeHelper::BlenderMatrixToUE5Transform(
    const TArray<double>& M,
    double UnitScale)
{
    if (M.Num() < 16)
    {
        return FTransform::Identity;
    }

    // Blender stores a 4×4 matrix in ROW-MAJOR order using COLUMN-VECTOR
    // convention (result = Matrix * Vector).
    //
    // Flat array layout:  a=M[0] b=M[1] c=M[2] tx=M[3]
    //                     d=M[4] e=M[5] f=M[6] ty=M[7]
    //                     g=M[8] h=M[9] i=M[10] tz=M[11]
    //
    // AXIS CONVERSION:
    //   Blender: X=right,   Y=forward,  Z=up  (right-handed)
    //   UE5:     X=forward, Y=right,    Z=up  (left-handed)
    //
    //   → Blender Y (forward) maps to UE5 X (forward)
    //   → Blender X (right)   maps to UE5 Y (right)
    //   → Blender Z (up)      maps to UE5 Z (up)
    //
    // SCALE FACTOR:
    //   Blender matrix_world values are in Blender Units (BU).
    //   Real metres = BU * UnitScale
    //   UE5 centimetres = real metres * 100
    //   So: position_cm = BU * UnitScale * 100.0
    //
    //   With UnitScale=0.01 (scene in cm): factor = 0.01 * 100 = 1.0
    //   With UnitScale=1.0  (scene in m):  factor = 1.0  * 100 = 100.0

    const double PosFactor = UnitScale * 100.0;

    // The coordinate swap matrix C swaps X↔Y: C = [[0,1,0],[1,0,0],[0,0,1]]
    // Full transform: M_ue5 = C * M_blender * C  (C is its own inverse)
    // Then transpose for UE5 row-vector convention.
    //
    // Result (UE5 FMatrix, row-vector convention):
    //   Row 0 (X/forward): from Blender Y-axis basis
    //   Row 1 (Y/right):   from Blender X-axis basis
    //   Row 2 (Z/up):      from Blender Z-axis basis
    //   Row 3 (translate):  swapped and scaled

    FMatrix UEMatrix = FMatrix::Identity;

    // Rotation/scale: axis-swapped + transposed
    UEMatrix.M[0][0] = M[5];   UEMatrix.M[0][1] = M[1];   UEMatrix.M[0][2] = M[9];
    UEMatrix.M[1][0] = M[4];   UEMatrix.M[1][1] = M[0];   UEMatrix.M[1][2] = M[8];
    UEMatrix.M[2][0] = M[6];   UEMatrix.M[2][1] = M[2];   UEMatrix.M[2][2] = M[10];

    // Translation: Blender Y → UE5 X, Blender X → UE5 Y, scaled BU→cm
    UEMatrix.M[3][0] = M[7]  * PosFactor;   // Blender ty → UE5 X (forward)
    UEMatrix.M[3][1] = M[3]  * PosFactor;   // Blender tx → UE5 Y (right)
    UEMatrix.M[3][2] = M[11] * PosFactor;   // Blender tz → UE5 Z (up)

    return FTransform(UEMatrix);
}

// ───────────────────────────────────────────────────────────────
// FBX import via UFbxFactory
// ───────────────────────────────────────────────────────────────

EMapLinkResult FMapLinkInterchangeHelper::ImportMeshAssetsFromFbx(
    const FString& FbxFilePath,
    const FString& BlendName,
    TMap<FString, UStaticMesh*>& OutMeshMap)
{
    FScopedSlowTask SlowTask(1.f,
        FText::FromString(FString::Printf(
            TEXT("MapLink: Importing FBX meshes for %s"), *BlendName)));
    SlowTask.MakeDialog(false);

    // Configure the FBX factory
    UFbxFactory* Factory = NewObject<UFbxFactory>();
    Factory->ImportUI->bImportMesh = true;
    Factory->ImportUI->bImportAnimations = false;
    Factory->ImportUI->bImportMaterials = true;
    Factory->ImportUI->bImportTextures = false;
    Factory->ImportUI->TextureImportData->MaterialSearchLocation = EMaterialSearchLocation::AllAssets;
    Factory->ImportUI->bAutomatedImportShouldDetectType = false;
    Factory->ImportUI->MeshTypeToImport = FBXIT_StaticMesh;

    // Keep mesh data in local/object space — world position comes from analysis.json.
    // bTransformVertexToAbsolute=false prevents baking FBX node transform into vertices.
    Factory->ImportUI->StaticMeshImportData->bTransformVertexToAbsolute = false;
    Factory->ImportUI->StaticMeshImportData->bBakePivotInVertex = false;
    Factory->ImportUI->StaticMeshImportData->bCombineMeshes = false;
    Factory->ImportUI->StaticMeshImportData->VertexColorImportOption = EVertexColorImportOption::Replace;

    // Build automated import data
    FString DestPath = FString::Printf(
        TEXT("/Game/MapLink/%s/Meshes"), *BlendName);

    UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
    ImportData->Filenames.Add(FbxFilePath);
    ImportData->DestinationPath = DestPath;
    ImportData->bReplaceExisting = true;
    ImportData->Factory = Factory;

    // Run the synchronous import
    IAssetTools& AssetTools =
        FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

    TArray<UObject*> ImportedAssets = AssetTools.ImportAssetsAutomated(ImportData);

    SlowTask.EnterProgressFrame(1.f);

    if (ImportedAssets.Num() == 0)
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("FBX import returned 0 assets for '%s'."), *BlendName);
        // Not necessarily fatal — the FBX might contain only empties
        // which are now handled via analysis.json instead.
        return EMapLinkResult::Success;
    }

    // Build the name→mesh map
    for (UObject* Obj : ImportedAssets)
    {
        UStaticMesh* SM = Cast<UStaticMesh>(Obj);
        if (!SM) continue;

        OutMeshMap.Add(SM->GetName(), SM);
        UE_LOG(LogMapLink, Log,
               TEXT("  Imported mesh: '%s' → %s"),
               *SM->GetName(), *SM->GetPathName());
    }

    return EMapLinkResult::Success;
}
