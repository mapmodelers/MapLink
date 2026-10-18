// Copyright MapModelers. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MapLinkTypes.h"

/**
 * Handles FBX import via UFbxFactory (synchronous) and reads scene layout
 * from analysis.json produced by the Blender headless script.
 *
 * Replaces the previous Interchange-based approach which had async
 * completion issues on UE 5.5+.
 */
class FMapLinkInterchangeHelper
{
public:
    /**
     * Parse analysis.json and import mesh assets from FBX.
     * Populates OutMeshes with StaticMesh pointers + transforms,
     * and OutEmpties with linked-asset placeholder transforms.
     * All transforms are already converted to UE5 coordinate space.
     *
     * If the blend file has no own geometry, the FBX import is skipped
     * and only OutEmpties is populated.
     */
    static EMapLinkResult ImportAndCollectResults(
        const FString& FbxFilePath,
        const FString& BlendName,
        const FString& PluginBaseDir,
        TArray<FMapLinkImportedMesh>& OutMeshes,
        TArray<FMapLinkImportedEmpty>& OutEmpties);

    /**
     * Create a new Level (.umap) at the canonical path for BlendName,
     * populated with StaticMeshActors for own geometry and placeholder
     * actors for linked assets.  The placeholders are swapped later
     * by FMapLinkLevelHelper::SwapPlaceholdersInLevel.
     */
    static EMapLinkResult CreateLevelFromResults(
        const FString& BlendName,
        const TArray<FMapLinkImportedMesh>& Meshes,
        const TArray<FMapLinkImportedEmpty>& Empties);

    /**
     * Parse the Blender analysis.json file into a structured report.
     */
    static bool ParseAnalysisJson(
        const FString& JsonPath,
        FMapLinkAnalysisReport& OutReport);

    /**
     * Convert a Blender 4×4 row-major matrix (16 doubles) to an FTransform
     * in UE5 coordinate space.  Handles:
     *   - Axis swap: Blender Y(fwd)→UE5 X(fwd), Blender X(right)→UE5 Y(right)
     *   - Scale: BU * UnitScale * 100 → cm
     *
     * @param BlenderMatrix  16 doubles, row-major column-vector matrix.
     * @param UnitScale      Scene unit_settings.scale_factor (0.01 = cm, 1.0 = m).
     */
    static FTransform BlenderMatrixToUE5Transform(
        const TArray<double>& BlenderMatrix,
        double UnitScale = 1.0);

private:
    /** Import mesh assets from FBX using UFbxFactory.  Returns map name→mesh. */
    static EMapLinkResult ImportMeshAssetsFromFbx(
        const FString& FbxFilePath,
        const FString& BlendName,
        TMap<FString, UStaticMesh*>& OutMeshMap);
};
