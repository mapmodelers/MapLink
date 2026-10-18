// Copyright MapModelers. All Rights Reserved.

#include "MapLinkBPReferenceMode.h"

#include "MapLinkProxyAsset.h"
#include "MapLinkUtils.h"
#include "MapLinkPipeline.h"  // LogMapLink

#include "Engine/Blueprint.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "UObject/UObjectGlobals.h"

// v1.8 D7 hotfix3 — authoritative existence check (LoadObject alone
// can find UClasses that survive in memory after CB delete until GC).
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "UObject/SoftObjectPath.h"

namespace FMapLinkBPReferenceMode
{
    const TCHAR* BP_DEPRECATED_PRIMARY_PATH =
        TEXT("/MapLink/Blueprints/BP_Deprecated.BP_Deprecated");

    const TCHAR* BP_DEPRECATED_FALLBACK_PATH =
        TEXT("/Game/Blueprints/BP_Deprecated.BP_Deprecated");

    // -----------------------------------------------------------------
    // Sidecar parsing
    // -----------------------------------------------------------------

    bool ParseSidecarForBoundBP(
        const FString& BlendFilePath,
        FString&       OutBoundBPPath,
        FString&       OutStandInMode)
    {
        OutBoundBPPath.Reset();
        OutStandInMode.Reset();

        if (BlendFilePath.IsEmpty()) return false;

        // Sidecar lives at <blend_dir>/<stem>_analysis.json (v1.7.6+ convention).
        const FString Dir  = FPaths::GetPath(BlendFilePath);
        const FString Stem = FPaths::GetBaseFilename(BlendFilePath);
        const FString SidecarPath = Dir / (Stem + TEXT("_analysis.json"));

        if (!FPaths::FileExists(SidecarPath))
        {
            return false;
        }

        FString JsonStr;
        if (!FFileHelper::LoadFileToString(JsonStr, *SidecarPath))
        {
            UE_LOG(LogMapLinkBPStandIn, Warning,
                   TEXT("Sidecar load failed: %s"), *SidecarPath);
            return false;
        }

        TSharedPtr<FJsonObject> Root;
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
        if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
        {
            UE_LOG(LogMapLinkBPStandIn, Warning,
                   TEXT("Sidecar parse failed: %s"), *SidecarPath);
            return false;
        }

        if (!Root->TryGetStringField(TEXT("bound_blueprint_path"), OutBoundBPPath))
        {
            return false;
        }
        if (OutBoundBPPath.IsEmpty())
        {
            return false;
        }
        // standin_mode is optional — if absent we still treat the
        // sidecar as reference-mode based on bound_blueprint_path alone.
        Root->TryGetStringField(TEXT("standin_mode"), OutStandInMode);
        return true;
    }

    // -----------------------------------------------------------------
    // Proxy inspection
    // -----------------------------------------------------------------

    bool IsProxyReferenceMode(
        const FString& NodeBlendName,
        FString&       OutBoundBPPath,
        UMapLinkProxyAsset** OutProxy)
    {
        OutBoundBPPath.Reset();
        if (OutProxy) *OutProxy = nullptr;

        const FString ProxyPath = FString::Printf(
            TEXT("/Game/MapLink/%s/%s_Proxy"),
            *NodeBlendName, *NodeBlendName);

        UMapLinkProxyAsset* Proxy =
            LoadObject<UMapLinkProxyAsset>(nullptr, *ProxyPath);
        if (!Proxy)
        {
            return false;
        }
        if (OutProxy) *OutProxy = Proxy;
        if (!Proxy->bIsBPReference)
        {
            return false;
        }
        // OriginalBoundBlueprintPath is the frozen copy — it survives
        // even if BoundBlueprint stops resolving (BP deleted, etc).
        OutBoundBPPath = Proxy->OriginalBoundBlueprintPath;
        if (OutBoundBPPath.IsEmpty())
        {
            // Fall back to the live soft ref if frozen copy missing.
            OutBoundBPPath = Proxy->BoundBlueprint.ToSoftObjectPath().ToString();
        }
        return true;
    }

    // -----------------------------------------------------------------
    // Class resolution
    // -----------------------------------------------------------------

    namespace
    {
        // v1.8 D7 hotfix3 — Asset Registry existence check.
        // LoadObject() can return a non-null pointer for a Blueprint
        // that the user just deleted in Content Browser, because the
        // UBlueprint and its GeneratedClass live on in memory (kept
        // alive by soft-pointer resolutions, level fixups, undo
        // buffers, etc.) until garbage collection.  The Asset
        // Registry is the authoritative answer to "is this asset
        // still present on disk and known to the project?".
        bool AssetExistsInRegistry(const FString& BPObjectPath)
        {
            if (BPObjectPath.IsEmpty()) return false;
            FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<
                FAssetRegistryModule>(TEXT("AssetRegistry"));
            IAssetRegistry& AR = ARM.Get();

            // UE 5.1+ AssetRegistry prefers FSoftObjectPath over the
            // deprecated FName overload.
            const FSoftObjectPath SoftPath(BPObjectPath);
            const FAssetData Data = AR.GetAssetByObjectPath(SoftPath);
            return Data.IsValid();
        }

        UClass* TryLoadBPClass(const FString& BPPath)
        {
            if (BPPath.IsEmpty()) return nullptr;

            // Authoritative existence gate first — LoadObject alone
            // is not enough (see comment on AssetExistsInRegistry).
            if (!AssetExistsInRegistry(BPPath))
            {
                UE_LOG(LogMapLink, Verbose,
                       TEXT("[BPRef] AssetRegistry says '%s' is gone; "
                            "treating as missing (LoadObject may still "
                            "return a stale in-memory class)."),
                       *BPPath);
                return nullptr;
            }

            UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *BPPath);
            if (!BP || !BP->GeneratedClass) return nullptr;
            return BP->GeneratedClass;
        }

        UClass* TryLoadDeprecated()
        {
            // Try each capitalization variant — Windows is filesystem-
            // case-insensitive but package paths in UE are NOT, so the
            // actual on-disk file capitalization determines what
            // LoadObject can resolve.  Order: canonical plugin paths
            // first, then project fallbacks; for each location, try
            // both common capitalizations.
            const TCHAR* Variants[] = {
                TEXT("/MapLink/Blueprints/BP_Deprecated.BP_Deprecated"),
                TEXT("/MapLink/Blueprints/bp_deprecated.bp_deprecated"),
                TEXT("/Game/Blueprints/BP_Deprecated.BP_Deprecated"),
                TEXT("/Game/Blueprints/bp_deprecated.bp_deprecated"),
            };

            for (const TCHAR* Path : Variants)
            {
                if (UClass* C = TryLoadBPClass(Path))
                {
                    const FString PathStr(Path);
                    const bool bCanonical = PathStr.StartsWith(TEXT("/MapLink/"));
                    if (bCanonical)
                    {
                        UE_LOG(LogMapLinkBPStandIn, Log,
                               TEXT("BP_Deprecated resolved at: %s"), Path);
                    }
                    else
                    {
                        UE_LOG(LogMapLinkBPStandIn, Warning,
                               TEXT("BP_Deprecated resolved at FALLBACK path: %s "
                                    "— consider moving the asset to /MapLink/Blueprints/ "
                                    "for canonical plugin distribution."),
                               Path);
                    }
                    return C;
                }
            }

            UE_LOG(LogMapLinkBPStandIn, Error,
                   TEXT("BP_Deprecated not found at any of /MapLink/Blueprints/ "
                        "or /Game/Blueprints/ (tried both BP_Deprecated and "
                        "bp_deprecated case variants).  Missing-BP placeholders "
                        "will be left unswapped."));
            return nullptr;
        }
    }

    UClass* ResolveBoundClass(
        UMapLinkProxyAsset* Proxy,
        bool&               OutbWasDeprecated)
    {
        OutbWasDeprecated = false;
        if (!Proxy) return nullptr;

        // 1) Try the bound BP via its soft path
        const FString BoundPath =
            Proxy->BoundBlueprint.ToSoftObjectPath().ToString();
        if (UClass* C = TryLoadBPClass(BoundPath))
        {
            return C;
        }
        // 2) Try the frozen Original path (e.g. if BoundBlueprint got cleared)
        if (UClass* C = TryLoadBPClass(Proxy->OriginalBoundBlueprintPath))
        {
            return C;
        }
        // 3) Fall back to BP_Deprecated
        OutbWasDeprecated = true;
        return TryLoadDeprecated();
    }

    UClass* ResolveBoundClassFromPath(
        const FString& BoundBPPath,
        bool&          OutbWasDeprecated)
    {
        OutbWasDeprecated = false;
        if (UClass* C = TryLoadBPClass(BoundBPPath))
        {
            return C;
        }
        OutbWasDeprecated = true;
        return TryLoadDeprecated();
    }

    // -----------------------------------------------------------------
    // Placement reporting (per-import accumulator)
    // -----------------------------------------------------------------

    namespace
    {
        // Lives across the RunImport call.  Cleared by
        // WriteAndClearPlacementsReport at the end of every import.
        static TArray<FPlacement> GPlacements;

        const TCHAR* StatusToString(EPlacementStatus S)
        {
            switch (S)
            {
                case EPlacementStatus::Ok:                    return TEXT("ok");
                case EPlacementStatus::DeprecatedSubstituted: return TEXT("deprecated_substituted");
                case EPlacementStatus::Missing:               return TEXT("missing");
            }
            return TEXT("ok");
        }
    }

    void RecordPlacement(const FPlacement& Placement)
    {
        GPlacements.Add(Placement);
        UE_LOG(LogMapLinkBPStandIn, Log,
               TEXT("[RefMode] placement %s [%s] proxy=%s bp=%s owner=%s"),
               StatusToString(Placement.Status),
               *Placement.PlacementContext,
               *Placement.ProxyPath,
               *Placement.BoundBlueprintPath,
               *Placement.OwnerAssetPath);
    }

    void WriteAndClearPlacementsReport(const FString& RootBlendName)
    {
        if (GPlacements.Num() == 0)
        {
            return;
        }

        TArray<TSharedPtr<FJsonValue>> Arr;
        for (const FPlacement& P : GPlacements)
        {
            TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(TEXT("proxy_path"),           P.ProxyPath);
            Obj->SetStringField(TEXT("bound_blueprint_path"), P.BoundBlueprintPath);
            Obj->SetStringField(TEXT("placement_context"),    P.PlacementContext);
            Obj->SetStringField(TEXT("owner_asset_path"),     P.OwnerAssetPath);
            Obj->SetStringField(TEXT("status"),               StatusToString(P.Status));
            Arr.Add(MakeShared<FJsonValueObject>(Obj));
        }

        TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetStringField(TEXT("root_blend_name"), RootBlendName);
        Root->SetArrayField (TEXT("bp_reference_mode_placements"), Arr);

        const FString OutDir = FMapLinkUtils::GetArtifactLogDir() / RootBlendName;
        FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*OutDir);
        const FString OutPath = OutDir / TEXT("bp_reference_mode_placements.json");

        FString Out;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
        FJsonSerializer::Serialize(Root, Writer);
        FFileHelper::SaveStringToFile(Out, *OutPath,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

        UE_LOG(LogMapLinkBPStandIn, Log,
               TEXT("[RefMode] %d placement(s) written to %s"),
               GPlacements.Num(), *OutPath);

        GPlacements.Reset();
    }
}
