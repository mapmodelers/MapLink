// Copyright MapModelers. All Rights Reserved.
//
// v1.13 — Copy Diagnostics (see header).

#include "MapLinkDiagnostics.h"

#include "MapLinkNotify.h"
#include "MapLinkPipeline.h"   // LogMapLink
#include "MapLinkProxyAsset.h"
#include "MapLinkSettings.h"
#include "MapLinkUtils.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDeviceFile.h"
#include "Misc/Paths.h"
#include "HAL/PlatformOutputDevices.h"
#include "UObject/UnrealType.h"

namespace
{
    FString GetPluginVersionName()
    {
        TSharedPtr<IPlugin> Plugin =
            IPluginManager::Get().FindPlugin(TEXT("MapLink"));
        return Plugin.IsValid()
            ? Plugin->GetDescriptor().VersionName
            : FString(TEXT("unknown"));
    }

    /** Parse bl_info "version": (1, 5, 0) out of blender/__init__.py.
     *  Best-effort — returns "unknown" when not discoverable. */
    FString GetAddonVersion()
    {
        const FString InitPy =
            FMapLinkUtils::GetPluginBaseDir() / TEXT("blender") / TEXT("__init__.py");
        FString Content;
        if (!FFileHelper::LoadFileToString(Content, *InitPy))
        {
            return TEXT("unknown");
        }
        // Look inside the first ~40 lines after "bl_info" for `"version":`.
        int32 BlInfoIdx = Content.Find(TEXT("bl_info"));
        if (BlInfoIdx == INDEX_NONE)
        {
            return TEXT("unknown");
        }
        int32 VerIdx = Content.Find(TEXT("\"version\":"), ESearchCase::CaseSensitive,
                                    ESearchDir::FromStart, BlInfoIdx);
        if (VerIdx == INDEX_NONE)
        {
            return TEXT("unknown");
        }
        int32 Open  = Content.Find(TEXT("("), ESearchCase::CaseSensitive,
                                   ESearchDir::FromStart, VerIdx);
        int32 Close = Content.Find(TEXT(")"), ESearchCase::CaseSensitive,
                                   ESearchDir::FromStart, VerIdx);
        if (Open == INDEX_NONE || Close == INDEX_NONE || Close <= Open)
        {
            return TEXT("unknown");
        }
        FString Tuple = Content.Mid(Open + 1, Close - Open - 1); // "1, 5, 0"
        Tuple.ReplaceInline(TEXT(" "), TEXT(""));
        Tuple.ReplaceInline(TEXT(","), TEXT("."));
        return Tuple;
    }

    /** Reflection dump of every config property on UMapLinkSettings. */
    FString GetSettingsSnapshot()
    {
        FString Out;
        const UMapLinkSettings* S = UMapLinkSettings::Get();
        for (TFieldIterator<FProperty> It(UMapLinkSettings::StaticClass());
             It; ++It)
        {
            const FProperty* Prop = *It;
            if (!Prop->HasAnyPropertyFlags(CPF_Config))
            {
                continue;
            }
            FString Value;
            const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(S);
            Prop->ExportTextItem_Direct(Value, ValuePtr, nullptr, nullptr,
                                        PPF_None);
            Out += FString::Printf(TEXT("- %s = %s\n"),
                                   *Prop->GetName(), *Value);
        }
        return Out;
    }

    /** Last N lines of the current session's log that mention LogMapLink. */
    FString GetLogMapLinkTail(int32 MaxLines)
    {
        // Flush buffered log output so the tail is current.
        if (GLog)
        {
            GLog->Flush();
        }
        const FString LogFile = FPlatformOutputDevices::GetAbsoluteLogFilename();
        FString Content;
        if (LogFile.IsEmpty() ||
            !FFileHelper::LoadFileToString(Content, *LogFile,
                FFileHelper::EHashOptions::None,
                FILEREAD_AllowWrite))   // the logger holds the file open
        {
            return FString();   // inaccessible → section is skipped
        }
        TArray<FString> Lines;
        Content.ParseIntoArrayLines(Lines);
        TArray<FString> Filtered;
        for (const FString& Line : Lines)
        {
            if (Line.Contains(TEXT("LogMapLink")))
            {
                Filtered.Add(Line);
            }
        }
        const int32 Start = FMath::Max(0, Filtered.Num() - MaxLines);
        FString Out;
        for (int32 i = Start; i < Filtered.Num(); ++i)
        {
            Out += Filtered[i] + TEXT("\n");
        }
        return Out;
    }

    /** Every MapLink proxy with a one-line state summary. */
    FString GetProxySummary()
    {
        FString Out;
        IAssetRegistry& AR =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
                TEXT("AssetRegistry")).Get();
        TArray<FAssetData> Assets;
        AR.GetAssetsByClass(
            UMapLinkProxyAsset::StaticClass()->GetClassPathName(), Assets);
        Assets.Sort([](const FAssetData& A, const FAssetData& B)
        {
            return A.AssetName.LexicalLess(B.AssetName);
        });
        for (const FAssetData& Asset : Assets)
        {
            const UMapLinkProxyAsset* Proxy =
                Cast<UMapLinkProxyAsset>(Asset.GetAsset());
            if (!Proxy)
            {
                continue;
            }
            const bool bBlendExists = FPaths::FileExists(Proxy->BlendFilePath);
            Out += FString::Printf(TEXT("- %s | blend: %s | %s\n"),
                *Asset.AssetName.ToString(),
                *Proxy->BlendFilePath,
                bBlendExists ? TEXT("on disk") : TEXT("MISSING"));
        }
        if (Out.IsEmpty())
        {
            Out = TEXT("(no proxies)\n");
        }
        return Out;
    }
}

FString FMapLinkDiagnostics::BuildReport()
{
    const UMapLinkSettings* S = UMapLinkSettings::Get();

    FString Md;
    Md += TEXT("# MapLink Diagnostics\n\n");
    Md += FString::Printf(TEXT("- Plugin version: %s\n"), *GetPluginVersionName());
    Md += FString::Printf(TEXT("- Blender addon version: %s\n"), *GetAddonVersion());
    Md += FString::Printf(TEXT("- Engine: %s\n"),
        *FEngineVersion::Current().ToString());
    Md += FString::Printf(TEXT("- Project: %s\n"), FApp::GetProjectName());
    Md += FString::Printf(TEXT("- UTC: %s\n"), *FDateTime::UtcNow().ToIso8601());

    // Manifest (when linked)
    Md += TEXT("\n## Project manifest\n\n");
    const FString Root = S ? S->BlenderProjectRoot.Path : FString();
    const FString ManifestPath =
        Root.IsEmpty() ? FString() : Root / TEXT("maplink_project.json");
    if (!ManifestPath.IsEmpty() && FPaths::FileExists(ManifestPath))
    {
        Md += FString::Printf(TEXT("Path: `%s`\n\n"), *ManifestPath);
        FString Manifest;
        if (FFileHelper::LoadFileToString(Manifest, *ManifestPath))
        {
            Md += TEXT("```json\n") + Manifest.TrimEnd() + TEXT("\n```\n");
        }
    }
    else
    {
        Md += TEXT("(not linked / manifest not found)\n");
    }

    // Settings snapshot
    Md += TEXT("\n## Settings\n\n");
    Md += GetSettingsSnapshot();

    // Proxy list
    Md += TEXT("\n## Proxies\n\n");
    Md += GetProxySummary();

    // Log tail
    const FString LogTail = GetLogMapLinkTail(200);
    if (!LogTail.IsEmpty())
    {
        Md += TEXT("\n## LogMapLink (last 200 lines, this session)\n\n");
        Md += TEXT("```\n") + LogTail + TEXT("```\n");
    }

    return Md;
}

void FMapLinkDiagnostics::CopyToClipboard()
{
    const FString Report = BuildReport();
    FPlatformApplicationMisc::ClipboardCopy(*Report);
    UE_LOG(LogMapLink, Log,
           TEXT("[Diagnostics] Copied %d chars to clipboard."), Report.Len());
    MapLinkNotify::Success(NSLOCTEXT("MapLink", "DiagCopied",
                                     "Diagnostics copied"));
}
