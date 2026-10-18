// Copyright MapModelers. All Rights Reserved.
//
// v1.16 — "Does MapLink own this material?"
//
// THIS SINGLE TEST GOVERNS EVERY REPOINT DECISION. Non-MapLink materials are
// never modified, under any operation, for any reason. Every place that is
// about to write to a UMaterialInterface asks here first.

#pragma once

#include "CoreMinimal.h"

class UMaterialInterface;
struct FAssetData;

class MAPLINKEDITOR_API FMapLinkTextureOwnership
{
public:
    /** How far up a parent chain to walk before giving up. Eight is far past
     *  anything sane; the cap exists so a pathological asset cannot hang a
     *  scan, not because depth eight is meaningful. */
    static constexpr int32 MaxParentDepth = 8;

    /**
     * True when this material, or any material in its parent chain, lives under
     * /MapLink/ (plugin mount) or /Game/MapLink/ (project).
     *
     * Built on FMapLinkUtils::IsMapLinkMaterialPath, so the definition of "a
     * MapLink material" stays in one place. Walks up to MaxParentDepth links
     * with a visited set, because a corrupt asset can produce a parent cycle
     * and an unguarded walk would spin forever inside a scan.
     */
    static bool IsMapLinkMaterial(const UMaterialInterface* Material);

    /** AssetRegistry-only form: true when the package path alone settles it, so
     *  a scan can answer without loading. Returns false for anything that would
     *  need the parent chain walked — the caller escalates to
     *  IsMapLinkMaterial() on the loaded asset when it needs certainty. */
    static bool IsMapLinkMaterialPathOnly(const FAssetData& AssetData);

    /**
     * Every UMaterialInstanceConstant MapLink owns, by AssetRegistry query.
     * Does not load packages.
     */
    static void GetMapLinkMaterialInstanceAssets(TArray<FAssetData>& OutAssets);
};
