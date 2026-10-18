// Copyright MapModelers. All Rights Reserved.
//
// v1.11 — MapLink Asset Viewer.
//
// Enumeration of all MapLink proxies under the single content root, plus
// classification by prefix. The viewer keeps a stable, stem-sorted list so a
// reselection survives the destructive reimport recreate (identity is the
// BlendName stem — MapLink has no per-asset GUID).

#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"

class UMapLinkProxyAsset;

enum class EMapLinkViewerAssetType : uint8
{
    LevelInstance,   // LI_  → generated UWorld
    StaticMesh,      // SM_  → /Meshes/ assets (+ ExpandSM when analysis is fresh)
    Blueprint,       // BP_  → bound Blueprint visuals / BP_Deprecated
    Unknown,
};

/** One selectable asset in the viewer. */
struct FMapLinkViewerEntry
{
    FString Stem;                                  // e.g. "LI_MyLevel"
    EMapLinkViewerAssetType Type = EMapLinkViewerAssetType::Unknown;
    TWeakObjectPtr<UMapLinkProxyAsset> Proxy;
};

class FMapLinkAssetViewerProxyList
{
public:
    /** Rebuild the stem-sorted entry list from the asset registry. Cheap; the
     *  viewer calls this on open and after every reimport-end. */
    static void Enumerate(TArray<FMapLinkViewerEntry>& OutEntries);

    /** Prefix → type. Unknown for anything that isn't LI_/SM_/BP_. */
    static EMapLinkViewerAssetType ClassifyStem(const FString& Stem);

    /** Stem for a proxy: base filename of its .blend, falling back to the
     *  asset name minus the "_Proxy" suffix. */
    static FString StemForProxy(const UMapLinkProxyAsset* Proxy,
                                const FString& ProxyAssetName);
};
