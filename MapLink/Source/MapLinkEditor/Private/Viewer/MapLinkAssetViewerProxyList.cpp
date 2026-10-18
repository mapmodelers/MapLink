// Copyright MapModelers. All Rights Reserved.

#include "Viewer/MapLinkAssetViewerProxyList.h"

#include "MapLinkProxyAsset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"
#include "Misc/Paths.h"

EMapLinkViewerAssetType FMapLinkAssetViewerProxyList::ClassifyStem(const FString& Stem)
{
    if (Stem.StartsWith(TEXT("LI_"))) return EMapLinkViewerAssetType::LevelInstance;
    if (Stem.StartsWith(TEXT("SM_"))) return EMapLinkViewerAssetType::StaticMesh;
    if (Stem.StartsWith(TEXT("BP_"))) return EMapLinkViewerAssetType::Blueprint;
    return EMapLinkViewerAssetType::Unknown;
}

FString FMapLinkAssetViewerProxyList::StemForProxy(
    const UMapLinkProxyAsset* Proxy, const FString& ProxyAssetName)
{
    if (Proxy && !Proxy->BlendFilePath.IsEmpty())
    {
        return FPaths::GetBaseFilename(Proxy->BlendFilePath);
    }
    // Fallback: "<stem>_Proxy" → "<stem>".
    FString Stem = ProxyAssetName;
    Stem.RemoveFromEnd(TEXT("_Proxy"));
    return Stem;
}

void FMapLinkAssetViewerProxyList::Enumerate(TArray<FMapLinkViewerEntry>& OutEntries)
{
    OutEntries.Reset();

    // Same AR-filter recipe the Manager uses (SMapLinkManagerWindow::CollectAllProxies),
    // replicated here so the viewer has no coupling to the Manager's privates.
    FAssetRegistryModule& ARModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AR = ARModule.Get();

    FARFilter Filter;
    Filter.ClassPaths.Add(UMapLinkProxyAsset::StaticClass()->GetClassPathName());
    Filter.bRecursivePaths = true;

    TArray<FAssetData> ProxyAssets;
    AR.GetAssets(Filter, ProxyAssets);

    for (const FAssetData& AssetData : ProxyAssets)
    {
        UMapLinkProxyAsset* Proxy = Cast<UMapLinkProxyAsset>(AssetData.GetAsset());
        if (!Proxy)
        {
            continue;
        }

        const FString Stem =
            StemForProxy(Proxy, AssetData.AssetName.ToString());

        const EMapLinkViewerAssetType Type = ClassifyStem(Stem);
        if (Type == EMapLinkViewerAssetType::Unknown)
        {
            continue;   // not an LI_/SM_/BP_ proxy — skip
        }

        FMapLinkViewerEntry Entry;
        Entry.Stem  = Stem;
        Entry.Type  = Type;
        Entry.Proxy = Proxy;
        OutEntries.Add(MoveTemp(Entry));
    }

    // Stable sort by stem (case-insensitive); identity survives recreate.
    OutEntries.Sort([](const FMapLinkViewerEntry& A, const FMapLinkViewerEntry& B)
    {
        return A.Stem.Compare(B.Stem, ESearchCase::IgnoreCase) < 0;
    });
}
