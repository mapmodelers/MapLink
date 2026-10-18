// Copyright MapModelers. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MapLinkTypes.h"

class UWorld;
class AActor;

/**
 * Handles placeholder swap in Level Instance (.umap) assets:
 * replaces empty-proxy actors with the correct LevelInstance, Blueprint,
 * or StaticMesh actors.  Supports SM_ leaf, SM_ composite (recursive
 * expansion), _GP wrapper lookup for BP_, and expanded-tag cleanup.
 */
class FMapLinkLevelHelper
{
public:
    /**
     * Swap all placeholder actors in the given level.
     *
     * Resolution order per placeholder:
     *   SM_ → AStaticMeshActor (leaf) or N × AStaticMeshActor (composite)
     *   BP_ → check for _GP wrapper first, then raw BP
     *   LI_ → ALevelInstance
     */
    static EMapLinkResult SwapPlaceholdersInLevel(
        const FString& LevelPath,
        const FString& BlendName);

    /** Write the swap log JSON for a level. */
    static void WriteSwapLog(
        const FString& BlendName,
        const TArray<FMapLinkStubRecord>& Records,
        const FString& LogFileName);

    /**
     * Recursively expand an SM_ composite into individual mesh placements.
     * Reads analysis.json for the given AssetName and composes transforms.
     * Cycle detection via VisitedSet.
     */
    static TArray<FExpandedMesh> ExpandSM(
        const FString& AssetName,
        const FTransform& ParentTransform,
        TSet<FString>& VisitedSet);

    /**
     * v1.8.1 — Spawn the bound (reference-mode) Blueprint for a BP_ node
     * that was expanded out of an SM_ composite.  Mirrors the top-level
     * BP_ swap path: resolves the proxy, falls back to BP_Deprecated,
     * tags/labels the actor and records a placement.
     * @return the spawned actor, or nullptr if it couldn't be resolved.
     */
    static AActor* SpawnExpandedBPReference(
        UWorld* World,
        const FString& BPNodeName,
        const FTransform& WorldTransform,
        const FString& OwnerAssetPath,
        const FString& PlacementContext,
        const FName& CleanupTag);

private:
    static FString StripBlenderSuffix(const FString& Name);
};
