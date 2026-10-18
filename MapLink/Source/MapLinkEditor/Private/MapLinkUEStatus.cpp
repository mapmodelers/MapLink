// Copyright MapModelers. All Rights Reserved.
//
// v1.13 — Close-the-loop status file (UE → Blender). See header.

#include "MapLinkUEStatus.h"
#include "MapLinkNotify.h"     // MapLinkResultToText
#include "MapLinkPipeline.h"   // LogMapLink

#include "Dom/JsonObject.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"

namespace
{
    /** Enum name for the JSON "code" field (EMapLinkResult is a plain enum
     *  class, not a UENUM — keep this switch complete). */
    const TCHAR* ResultCodeName(EMapLinkResult Result)
    {
        switch (Result)
        {
        case EMapLinkResult::Success:                return TEXT("Success");
        case EMapLinkResult::BlenderNotFound:        return TEXT("BlenderNotFound");
        case EMapLinkResult::BlenderScriptFailed:    return TEXT("BlenderScriptFailed");
        case EMapLinkResult::FbxNotFound:            return TEXT("FbxNotFound");
        case EMapLinkResult::TempLevelMissing:       return TEXT("TempLevelMissing");
        case EMapLinkResult::InterchangeFailed:      return TEXT("InterchangeFailed");
        case EMapLinkResult::AssetNotFound:          return TEXT("AssetNotFound");
        case EMapLinkResult::LevelLocked:            return TEXT("LevelLocked");
        case EMapLinkResult::DependencyCycle:        return TEXT("DependencyCycle");
        case EMapLinkResult::BlueprintCompileError:  return TEXT("BlueprintCompileError");
        case EMapLinkResult::MaterialLibraryNotFound:         return TEXT("MaterialLibraryNotFound");
        case EMapLinkResult::MaterialInstancesBlendNotFound:  return TEXT("MaterialInstancesBlendNotFound");
        case EMapLinkResult::MaterialSyncFailed:              return TEXT("MaterialSyncFailed");
        case EMapLinkResult::NoMaterialInstancesFound:        return TEXT("NoMaterialInstancesFound");
        case EMapLinkResult::MaterialDisplayLevelCreateFailed: return TEXT("MaterialDisplayLevelCreateFailed");
        case EMapLinkResult::MaterialDisplayMeshNotFound:      return TEXT("MaterialDisplayMeshNotFound");
        case EMapLinkResult::Cancelled:              return TEXT("Cancelled");
        case EMapLinkResult::BlenderTimedOut:        return TEXT("BlenderTimedOut");
        case EMapLinkResult::InvalidAssetName:        return TEXT("InvalidAssetName");
        }
        return TEXT("Unknown");
    }

    FString StatusPathFor(const FString& BlendFilePath)
    {
        const FString Dir  = FPaths::GetPath(BlendFilePath);
        const FString Stem = FPaths::GetBaseFilename(BlendFilePath);
        if (Dir.IsEmpty() || Stem.IsEmpty())
        {
            return FString();
        }
        return Dir / (Stem + TEXT("_uestatus.json"));
    }

    void WriteStatusJson(const FString& BlendFilePath,
                         const TSharedRef<FJsonObject>& Root)
    {
        const FString Path = StatusPathFor(BlendFilePath);
        if (Path.IsEmpty())
        {
            return;
        }

        Root->SetStringField(TEXT("utc"),
            FDateTime::UtcNow().ToIso8601());

        FString Out;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
        FJsonSerializer::Serialize(Root, Writer);

        // Best-effort: log-and-continue on failure (read-only dir, P4 lock,
        // network share hiccup — never fatal for the import itself).
        if (!FFileHelper::SaveStringToFile(Out, *Path,
                FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        {
            UE_LOG(LogMapLink, Log,
                   TEXT("[UEStatus] Could not write status file (non-fatal): %s"),
                   *Path);
        }
    }
}

namespace MapLinkUEStatus
{
    void WriteImporting(const FString& BlendFilePath)
    {
        TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetStringField(TEXT("state"), TEXT("importing"));
        WriteStatusJson(BlendFilePath, Root);
    }

    void WriteResult(const FString& BlendFilePath, EMapLinkResult Result)
    {
        TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetStringField(TEXT("state"),
            Result == EMapLinkResult::Success ? TEXT("ok") : TEXT("failed"));
        Root->SetStringField(TEXT("code"), ResultCodeName(Result));
        Root->SetStringField(TEXT("message"),
            MapLinkResultToText(Result).ToString());
        WriteStatusJson(BlendFilePath, Root);
    }
}
