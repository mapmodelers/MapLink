// Copyright MapModelers. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MapLinkTypes.h"

/**
 * Creates Blueprint actors from imported meshes/empties
 * and swaps placeholder components with ChildActorComponents,
 * StaticMeshComponents (for SM_), or _GP wrappers (for BP_ with _GP).
 *
 * v1.5: SM_ leaf resolution, _GP wrapper lookup
 * v1.6: SM_ composite recursive expansion with tagging
 */
class FMapLinkBlueprintHelper
{
public:
    static EMapLinkResult CreateBlueprint(
        const FString& BlendName,
        const TArray<FMapLinkImportedMesh>& ImportedMeshes,
        const TArray<FMapLinkImportedEmpty>& ImportedEmpties);

    static EMapLinkResult SwapPlaceholdersInBlueprint(
        const FString& BlendName);

    static void WriteSwapLog(
        const FString& BlendName,
        const TArray<FMapLinkStubRecord>& Records,
        const FString& LogFileName);

private:
    static FString StripBlenderSuffix(const FString& Name);
    static void SaveBPPackage(UBlueprint* BP);
};
