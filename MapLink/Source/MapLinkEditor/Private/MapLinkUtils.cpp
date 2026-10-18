// Copyright MapModelers. All Rights Reserved.

#include "MapLinkUtils.h"
#include "MapLinkPipeline.h"  // for DECLARE_LOG_CATEGORY_EXTERN(LogMapLink)
#include "MapLinkSettings.h"  // v1.16 — texture dirs hang off the project root
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Misc/DateTime.h"
#include "Interfaces/IPluginManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/FileManager.h"
#include "Misc/ScopeLock.h"

DEFINE_LOG_CATEGORY(LogMapLink);
DEFINE_LOG_CATEGORY(LogMapLinkBPStandIn);

// ---------------------------------------------------------------------------
// GetMapLinkContentRoot — single flat content root (merge seam). v1.10
// ---------------------------------------------------------------------------

const FString& FMapLinkUtils::GetMapLinkContentRoot()
{
    // Flat, project-agnostic. All MapLink content routes through here so that
    // two independently-authored MapLink projects merge cleanly under one root.
    static const FString ContentRoot = TEXT("/Game/MapLink/");
    return ContentRoot;
}

bool FMapLinkUtils::IsPathUnder(const FString& Path, const FString& Root)
{
    if (Path.IsEmpty() || Root.IsEmpty())
    {
        return false;
    }

    FString P = FPaths::ConvertRelativePathToFull(Path);
    FString R = FPaths::ConvertRelativePathToFull(Root);
    FPaths::NormalizeFilename(P);
    FPaths::NormalizeDirectoryName(R);

    if (P.Equals(R, ESearchCase::IgnoreCase))
    {
        return true;
    }
    // The trailing separator is what makes this a component-wise test:
    // "C:/MapLink/Foo" must not count as being under "C:/MapLink/Fo".
    return P.StartsWith(R + TEXT("/"), ESearchCase::IgnoreCase);
}

FString FMapLinkUtils::GetMaterialInstancesContentPath()
{
    // Drop the trailing slash from the root, append the MI subfolder.
    FString Root = GetMapLinkContentRoot();           // "/Game/MapLink/"
    Root.RemoveFromEnd(TEXT("/"));                     // "/Game/MapLink"
    return Root / TEXT("MaterialInstances");           // "/Game/MapLink/MaterialInstances"
}

const FString& FMapLinkUtils::GetMapLinkPluginContentRoot()
{
    static const FString PluginRoot = TEXT("/MapLink/");
    return PluginRoot;
}

bool FMapLinkUtils::IsMapLinkMaterialPath(const FString& PackageOrObjectPath)
{
    return PackageOrObjectPath.StartsWith(GetMapLinkPluginContentRoot())   // "/MapLink/"
        || PackageOrObjectPath.StartsWith(GetMapLinkContentRoot());        // "/Game/MapLink/"
}

// ---------------------------------------------------------------------------
// ComputeFileMD5
// ---------------------------------------------------------------------------

FString FMapLinkUtils::ComputeFileMD5(const FString& FilePath)
{
    // v1.9.4 — cache by (path, mtime, size).  A single import/reimport hashes
    // the same .blend up to 3× per node (skip-check → fbx-cache-check →
    // proxy-stamp); for production-size location .blends that triple full-file
    // read dominates iteration time.  The cache self-invalidates whenever the
    // file's modification time or size changes, so it never returns a stale
    // hash for an edited file.  Guarded by a critical section in case the
    // warm pass ever hashes off-thread.
    struct FHashEntry
    {
        FDateTime Mtime;
        int64     Size = -1;
        FString   Hash;
    };
    static FCriticalSection            CacheCS;
    static TMap<FString, FHashEntry>   Cache;

    const FFileStatData Stat = IFileManager::Get().GetStatData(*FilePath);
    const bool bStatUsable = Stat.bIsValid && !Stat.bIsDirectory;

    if (bStatUsable)
    {
        FScopeLock Lock(&CacheCS);
        if (const FHashEntry* Hit = Cache.Find(FilePath))
        {
            if (Hit->Mtime == Stat.ModificationTime &&
                Hit->Size  == Stat.FileSize)
            {
                return Hit->Hash;
            }
        }
    }

    TArray<uint8> FileData;
    if (!FFileHelper::LoadFileToArray(FileData, *FilePath))
    {
        return FString();
    }

    FMD5 Md5Gen;
    Md5Gen.Update(FileData.GetData(), FileData.Num());
    uint8 Digest[16];
    Md5Gen.Final(Digest);

    FString HashStr;
    for (int32 i = 0; i < 16; ++i)
    {
        HashStr += FString::Printf(TEXT("%02x"), Digest[i]);
    }

    if (bStatUsable)
    {
        FScopeLock Lock(&CacheCS);
        Cache.Add(FilePath,
            FHashEntry{ Stat.ModificationTime, Stat.FileSize, HashStr });
    }

    return HashStr;
}

// ---------------------------------------------------------------------------
// StripBlenderSuffix
// ---------------------------------------------------------------------------

FString FMapLinkUtils::StripBlenderSuffix(const FString& Name)
{
    FString Result = Name;
    int32 DotIdx = INDEX_NONE;
    if (Result.FindLastChar(TEXT('.'), DotIdx))
    {
        FString Suffix = Result.RightChop(DotIdx + 1);
        if (Suffix.IsNumeric())
        {
            Result = Result.Left(DotIdx);
        }
    }
    return Result;
}

// ---------------------------------------------------------------------------
// WriteSwapLog
// ---------------------------------------------------------------------------

void FMapLinkUtils::WriteSwapLog(
    const FString& BlendName,
    const TArray<FMapLinkStubRecord>& Records,
    const FString& LogFileName)
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    TArray<TSharedPtr<FJsonValue>> Entries;

    for (const FMapLinkStubRecord& Rec : Records)
    {
        TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("actor_label"), Rec.ActorLabel);
        Entry->SetStringField(TEXT("asset_name"), Rec.AssetName);
        Entry->SetStringField(TEXT("owner_asset_path"), Rec.OwnerAssetPath);
        Entry->SetBoolField(TEXT("is_level"), Rec.bOwnerIsLevel);

        FVector Loc = Rec.SavedTransform.GetLocation();
        FRotator Rot = Rec.SavedTransform.Rotator();
        FVector Scale = Rec.SavedTransform.GetScale3D();

        TSharedRef<FJsonObject> TransformObj = MakeShared<FJsonObject>();
        TransformObj->SetNumberField(TEXT("loc_x"), Loc.X);
        TransformObj->SetNumberField(TEXT("loc_y"), Loc.Y);
        TransformObj->SetNumberField(TEXT("loc_z"), Loc.Z);
        TransformObj->SetNumberField(TEXT("rot_pitch"), Rot.Pitch);
        TransformObj->SetNumberField(TEXT("rot_yaw"), Rot.Yaw);
        TransformObj->SetNumberField(TEXT("rot_roll"), Rot.Roll);
        TransformObj->SetNumberField(TEXT("scale_x"), Scale.X);
        TransformObj->SetNumberField(TEXT("scale_y"), Scale.Y);
        TransformObj->SetNumberField(TEXT("scale_z"), Scale.Z);
        Entry->SetObjectField(TEXT("transform"), TransformObj);

        Entries.Add(MakeShared<FJsonValueObject>(Entry));
    }

    Root->SetArrayField(TEXT("swaps"), Entries);

    FString OutputString;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
    FJsonSerializer::Serialize(Root, Writer);

    FString SwapLogDir = GetArtifactLogDir() / BlendName;

    // Ensure directory exists
    IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
    PlatformFile.CreateDirectoryTree(*SwapLogDir);

    FString SwapLogPath = SwapLogDir / LogFileName;
    FFileHelper::SaveStringToFile(OutputString, *SwapLogPath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

// ---------------------------------------------------------------------------
// GetPluginBaseDir (null-safe)
// ---------------------------------------------------------------------------

FString FMapLinkUtils::GetPluginBaseDir()
{
    TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MapLink"));
    if (Plugin.IsValid())
    {
        return FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
    }
    // Fallback
    return FPaths::ProjectPluginsDir() / TEXT("MapLink");
}

// ---------------------------------------------------------------------------
// v1.13 — Generated artifacts under <ProjectSaved>/MapLink/
// ---------------------------------------------------------------------------

FString FMapLinkUtils::GetArtifactRootDir()
{
    return FPaths::ConvertRelativePathToFull(
        FPaths::ProjectSavedDir() / TEXT("MapLink"));
}

FString FMapLinkUtils::GetArtifactLogDir()
{
    return GetArtifactRootDir() / TEXT("Logs");
}

FString FMapLinkUtils::GetArtifactExportDir()
{
    return GetArtifactRootDir() / TEXT("Export");
}

// ---------------------------------------------------------------------------
// v1.16 — Texture Manager directories, inside the MapLink project
// ---------------------------------------------------------------------------
//
// These are NOT artifact paths. Artifacts are per-machine and live under
// Saved/; these are P4-tracked production data the whole team syncs, so they
// hang off UMapLinkSettings::BlenderProjectRoot.
//
// Always resolved with both prompts OFF. A texture scan runs on a timer and in
// the background; ResolveBlenderProjectRoot's defaulted `true` arguments would
// pop a directory picker in the middle of one.

namespace
{
    FString TextureRootOrEmpty()
    {
        const FString Root =
            UMapLinkSettings::ResolveBlenderProjectRoot(
                /*bPromptOnAutoDetect=*/false, /*bPromptOnMissing=*/false);
        return Root.IsEmpty() ? FString() : FPaths::ConvertRelativePathToFull(Root);
    }
}

FString FMapLinkUtils::GetTextureSourceDir()
{
    const FString Root = TextureRootOrEmpty();
    return Root.IsEmpty() ? FString() : Root / TEXT("Textures");
}

FString FMapLinkUtils::GetTexturePreviewDir()
{
    const FString Root = TextureRootOrEmpty();
    return Root.IsEmpty() ? FString() : Root / TEXT("TexturePreviews");
}

FString FMapLinkUtils::GetExternalTexturePreviewDir()
{
    const FString Dir = GetTexturePreviewDir();
    return Dir.IsEmpty() ? FString() : Dir / TEXT("External");
}

FString FMapLinkUtils::GetMapLinkTexturePreviewDir()
{
    const FString Dir = GetTexturePreviewDir();
    return Dir.IsEmpty() ? FString() : Dir / TEXT("MapLink");
}

FString FMapLinkUtils::ResolveArtifactLogFile(const FString& RelPath)
{
    const FString NewPath = GetArtifactLogDir() / RelPath;
    if (FPaths::FileExists(NewPath))
    {
        return NewPath;
    }
    // Pre-1.13 location — read-only migration grace (one release).
    const FString Legacy = GetPluginBaseDir() / TEXT("Logs") / RelPath;
    if (FPaths::FileExists(Legacy))
    {
        return Legacy;
    }
    return NewPath;
}

FString FMapLinkUtils::ResolveArtifactExportFile(const FString& RelPath)
{
    const FString NewPath = GetArtifactExportDir() / RelPath;
    if (FPaths::FileExists(NewPath))
    {
        return NewPath;
    }
    // Pre-1.13 locations — the tree historically used both "Exports"
    // (pipeline FBX cache) and "Export". Read-only migration grace.
    const FString LegacyPlural = GetPluginBaseDir() / TEXT("Exports") / RelPath;
    if (FPaths::FileExists(LegacyPlural))
    {
        return LegacyPlural;
    }
    const FString LegacySingular = GetPluginBaseDir() / TEXT("Export") / RelPath;
    if (FPaths::FileExists(LegacySingular))
    {
        return LegacySingular;
    }
    return NewPath;
}

// ---------------------------------------------------------------------------
// v1.7.5 — FBX-export-on-save cache
// ---------------------------------------------------------------------------

bool FMapLinkUtils::AnalysisDeclaresOwnGeo(const FString& BlendName)
{
    const FString AnalysisPath =
        ResolveArtifactLogFile(BlendName / TEXT("analysis.json"));

    FString AnalysisJson;
    if (!FFileHelper::LoadFileToString(AnalysisJson, *AnalysisPath))
    {
        return false;
    }
    TSharedPtr<FJsonObject> Root;
    const TSharedRef<TJsonReader<>> Reader =
        TJsonReaderFactory<>::Create(AnalysisJson);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        return false;
    }
    const TArray<TSharedPtr<FJsonValue>>* OwnGeoArray = nullptr;
    return Root->TryGetArrayField(TEXT("blend_own_geo"), OwnGeoArray)
        && OwnGeoArray && OwnGeoArray->Num() > 0;
}

bool FMapLinkUtils::IsFbxCacheFresh(
    const FString& BlendPath,
    const FString& BlendName)
{
    // 1. Blender-produced hash (written by maplink_export.py analyze/both modes,
    //    and by MapLink_Blender addon v1.4.0+ save_post subprocess)
    const FString HashPath =
        ResolveArtifactLogFile(BlendName / TEXT("filehash.txt"));
    FString StoredHash;
    if (!FFileHelper::LoadFileToString(StoredHash, *HashPath))
    {
        return false;
    }
    StoredHash.TrimStartAndEndInline();
    if (StoredHash.IsEmpty())
    {
        return false;
    }

    // 2. Analysis must exist (Phase2 reads it for OwnGeo / LinkedAssets)
    const FString AnalysisPath =
        ResolveArtifactLogFile(BlendName / TEXT("analysis.json"));
    if (!FPaths::FileExists(AnalysisPath))
    {
        return false;
    }

    // 3. v1.15 — the FBX itself must exist (non-empty) whenever the blend
    //    has own geometry.  The artifact dir lives under <ProjectSaved>/
    //    MapLink (v1.13) and Saved/ is routinely cleaned: hash + analysis
    //    can be intact while the FBX is gone.  A stale "fresh" verdict
    //    skips the Blender launch, the nuke runs, and Phase 2 then fails
    //    on the missing FBX — assets destroyed with nothing to rebuild
    //    (observed live: LI_bg_ho9 / LI_bg_barn1 / LI_loc_bzn10).
    //    Missing FBX stays legitimate for empties-only blends, so gate on
    //    the analysis's blend_own_geo count.  Any parse failure → MISS
    //    (relaunching Blender is always safe, just slower).
    {
        FString AnalysisJson;
        if (!FFileHelper::LoadFileToString(AnalysisJson, *AnalysisPath))
        {
            return false;
        }
        TSharedPtr<FJsonObject> Root;
        const TSharedRef<TJsonReader<>> Reader =
            TJsonReaderFactory<>::Create(AnalysisJson);
        if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
        {
            return false;
        }
        const TArray<TSharedPtr<FJsonValue>>* OwnGeoArray = nullptr;
        const bool bHasOwnGeo =
            Root->TryGetArrayField(TEXT("blend_own_geo"), OwnGeoArray) &&
            OwnGeoArray && OwnGeoArray->Num() > 0;

        if (bHasOwnGeo)
        {
            const FString FbxPath = ResolveArtifactExportFile(
                BlendName / (BlendName + TEXT(".fbx")));
            if (!FPaths::FileExists(FbxPath) ||
                IFileManager::Get().FileSize(*FbxPath) <= 0)
            {
                UE_LOG(LogMapLink, Warning,
                       TEXT("[FbxCache] Hash fresh for '%s' but the FBX is "
                            "missing/empty ('%s') — forcing a Blender "
                            "re-export."),
                       *BlendName, *FbxPath);
                return false;
            }
        }
    }

    // 4. Current .blend hash must equal stored hash
    const FString CurrentHash = ComputeFileMD5(BlendPath);
    if (CurrentHash.IsEmpty())
    {
        return false;
    }
    return CurrentHash == StoredHash;
}

void FMapLinkUtils::WaitForExportLock(
    const FString& BlendName,
    double         TimeoutSec)
{
    // v1.13: the lock normally lives under <ArtifactExportDir>; a pre-1.13
    // addon still writes it under the plugin dir — resolve both for reads.
    const FString LockPath =
        ResolveArtifactExportFile(BlendName / TEXT(".export.lock"));

    IFileManager& FM = IFileManager::Get();
    if (!FM.FileExists(*LockPath))
    {
        return;
    }

    // Stale-lock check before we start waiting.
    {
        const FDateTime LockMTime = FM.GetTimeStamp(*LockPath);
        const FTimespan Age = FDateTime::UtcNow() - LockMTime;
        if (Age.GetTotalSeconds() > 60.0)
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("[FbxCache] Stale lock (%.0fs old) — deleting and "
                        "proceeding: %s"),
                   Age.GetTotalSeconds(), *LockPath);
            FM.Delete(*LockPath, /*RequireExists=*/false,
                      /*EvenReadOnly=*/false, /*Quiet=*/true);
            return;
        }
    }

    UE_LOG(LogMapLink, Log,
           TEXT("[FbxCache] Waiting for export lock: %s (up to %.1fs)"),
           *LockPath, TimeoutSec);

    const double StartSec = FPlatformTime::Seconds();
    const float  PollSec  = 0.25f;

    while (FM.FileExists(*LockPath))
    {
        if (FPlatformTime::Seconds() - StartSec > TimeoutSec)
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("[FbxCache] Timed out (%.1fs) waiting for lock — "
                        "proceeding: %s"),
                   TimeoutSec, *LockPath);
            return;
        }
        FPlatformProcess::Sleep(PollSec);
    }

    const double ElapsedMs = (FPlatformTime::Seconds() - StartSec) * 1000.0;
    UE_LOG(LogMapLink, Log,
           TEXT("[FbxCache] Lock cleared after %.1f ms."), ElapsedMs);
}
