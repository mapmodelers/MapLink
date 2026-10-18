// Copyright MapModelers. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MapLinkTypes.h"

/**
 * Builds and flattens the dependency graph for a set of interconnected .blend files.
 */
class FMapLinkDependencyGraph
{
public:
    /**
     * Build the full dependency tree rooted at the given .blend file.
     * Runs Blender analysis passes as needed to discover dependencies.
     *
     * @param BlendPath      Absolute path to the root .blend.
     * @param PluginBaseDir  Absolute path to the plugin's base directory.
     * @return  The root node of the tree (owned by this instance).
     */
    FMapLinkDependencyNode* BuildGraph(
        const FString& BlendPath,
        const FString& PluginBaseDir);

    /**
     * Flatten the tree into leaf-first (post-order) processing order.
     * Each BlendPath appears exactly once.
     */
    static FMapLinkProcessOrder FlattenLeafFirst(FMapLinkDependencyNode* Root);

    /** Release all allocated nodes. */
    ~FMapLinkDependencyGraph();

private:
    FMapLinkDependencyNode* BuildGraphInternal(
        const FString& BlendPath,
        const FString& PluginBaseDir,
        TMap<FString, FMapLinkDependencyNode*>& VisitedNodes);

    static void FlattenPostOrder(
        FMapLinkDependencyNode* Node,
        TSet<FString>& Visited,
        FMapLinkProcessOrder& OutOrder);

    /** Parse analysis.json to extract dependency paths. */
    static TArray<FString> ParseDependencyPaths(const FString& AnalysisJsonPath);

    /** All nodes allocated during BuildGraph, for cleanup. */
    TArray<FMapLinkDependencyNode*> AllNodes;
};
