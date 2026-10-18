// Copyright MapModelers. All Rights Reserved.
//
// v1.16 — MapLink material ownership (see header).

#include "MapLinkTextureOwnership.h"

#include "MapLinkUtils.h"
#include "MapLinkPipeline.h"   // LogMapLink

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"

bool FMapLinkTextureOwnership::IsMapLinkMaterial(const UMaterialInterface* Material)
{
    if (!Material)
    {
        return false;
    }

    TSet<const UMaterialInterface*> Visited;
    const UMaterialInterface* Link = Material;

    for (int32 Depth = 0; Depth < MaxParentDepth && Link; ++Depth)
    {
        // Cycle guard. A duplicated-then-reparented asset can produce a loop,
        // and an unguarded walk inside a scan over thousands of materials would
        // hang the editor with no clue why.
        bool bAlreadyVisited = false;
        Visited.Add(Link, &bAlreadyVisited);
        if (bAlreadyVisited)
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("[Textures] Parent-chain cycle detected at '%s'. "
                        "Treating it as not MapLink-owned."),
                   *Link->GetPathName());
            return false;
        }

        if (FMapLinkUtils::IsMapLinkMaterialPath(Link->GetPathName()))
        {
            return true;
        }

        const UMaterialInstance* AsInstance = Cast<UMaterialInstance>(Link);
        Link = AsInstance ? AsInstance->Parent : nullptr;
    }

    return false;
}

bool FMapLinkTextureOwnership::IsMapLinkMaterialPathOnly(const FAssetData& AssetData)
{
    return FMapLinkUtils::IsMapLinkMaterialPath(AssetData.GetObjectPathString());
}

void FMapLinkTextureOwnership::GetMapLinkMaterialInstanceAssets(
    TArray<FAssetData>& OutAssets)
{
    OutAssets.Reset();

    const FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
            TEXT("AssetRegistry"));
    const IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    FARFilter Filter;
    Filter.bRecursivePaths   = true;
    Filter.bRecursiveClasses = true;
    Filter.ClassPaths.Add(UMaterialInstanceConstant::StaticClass()->GetClassPathName());

    // Both mounts: the project content root and the plugin's own mount. Shipped
    // parent materials live under /MapLink/, and a project may legitimately
    // instance them without the instance being under /Game/MapLink/ — those are
    // caught by the parent-chain walk, not here, so this pass stays cheap.
    FString ProjectRoot = FMapLinkUtils::GetMapLinkContentRoot();
    ProjectRoot.RemoveFromEnd(TEXT("/"));
    Filter.PackagePaths.Add(*ProjectRoot);

    FString PluginRoot = FMapLinkUtils::GetMapLinkPluginContentRoot();
    PluginRoot.RemoveFromEnd(TEXT("/"));
    Filter.PackagePaths.Add(*PluginRoot);

    AssetRegistry.GetAssets(Filter, OutAssets);
}
