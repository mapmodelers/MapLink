// Copyright MapModelers. All Rights Reserved.
//
// v1.7.3 — Added MaterialDisplay error codes.

#pragma once

#include "CoreMinimal.h"
#include "MapLinkTypes.generated.h"

// ---------------------------------------------------------------------------
// Error codes
// ---------------------------------------------------------------------------

UENUM()
enum class EMapLinkResult : uint8
{
    Success,
    BlenderNotFound,
    BlenderScriptFailed,
    FbxNotFound,
    TempLevelMissing,
    InterchangeFailed,
    AssetNotFound,
    LevelLocked,
    DependencyCycle,
    BlueprintCompileError,

    // Material Sync errors
    MaterialLibraryNotFound,
    MaterialInstancesBlendNotFound,
    MaterialSyncFailed,
    NoMaterialInstancesFound,

    // Material Display errors (v1.7.3)
    MaterialDisplayLevelCreateFailed,
    MaterialDisplayMeshNotFound,

    // v1.9.4 — user aborted a long-running op (import/reimport/Blender wait)
    Cancelled,

    // v1.9.4 — headless Blender exceeded the watchdog timeout and was killed
    BlenderTimedOut,

    // Texture Manager errors (v1.16)
    TextureConformFailed,
    TextureNoSourceData,
    TexturePathTooLong,
    TextureReferencerLocked,

    // v1.21 - the .blend stem cannot be an Unreal asset name (a space, an
    // apostrophe, an accent). Caught before anything is created.
    InvalidAssetName,
};

// ---------------------------------------------------------------------------
// Dependency graph
// ---------------------------------------------------------------------------

/** One node in the dependency tree. */
struct MAPLINKRUNTIME_API FMapLinkDependencyNode
{
    /** Absolute path to this .blend file */
    FString BlendPath;

    /** Filename stem (e.g. "LI_test01" for LI_test01.blend) */
    FString BlendName;

    /** Direct child dependencies */
    TArray<FMapLinkDependencyNode*> Dependencies;

    ~FMapLinkDependencyNode()
    {
        // Nodes are owned by the graph builder; do not double-free.
    }
};

/** Flat leaf-first processing order derived from the graph. */
using FMapLinkProcessOrder = TArray<FString>; // BlendPath values, leaves first

// ---------------------------------------------------------------------------
// Import intermediates
// ---------------------------------------------------------------------------

/** A mesh imported from the FBX via Interchange. */
struct FMapLinkImportedMesh
{
    FString Name;
    class UStaticMesh* StaticMesh = nullptr;
    FTransform Transform;

    /** v1.14 — Blender collection chain (from analysis.json own_geo), used
     *  for outliner folder creation inside generated levels when
     *  FolderScope == AllLevels. */
    FString CollectionPath;
};

/** An empty imported from the FBX (proxy for a linked asset). */
struct FMapLinkImportedEmpty
{
    FString Name;
    FTransform Transform;

    /** v1.14 — Blender collection chain (from analysis.json linked_assets),
     *  see FMapLinkImportedMesh::CollectionPath. */
    FString CollectionPath;
};

// ---------------------------------------------------------------------------
// Stub record (reimport)
// ---------------------------------------------------------------------------

/** Saved reference that will be stubbed and later restored during reimport. */
USTRUCT()
struct MAPLINKRUNTIME_API FMapLinkStubRecord
{
    GENERATED_BODY()

    /** The BP or Level that holds the reference */
    UPROPERTY()
    FString OwnerAssetPath;

    /** Original Blender object name, e.g. "LI_Test_01.002" */
    UPROPERTY()
    FString ActorLabel;

    /** Suffix-stripped asset name, e.g. "LI_Test_01" */
    UPROPERTY()
    FString AssetName;

    /** Full transform — location, rotation, scale (world for levels, relative for BPs) */
    UPROPERTY()
    FTransform SavedTransform;

    /** true = LI_, false = BP_ */
    UPROPERTY()
    bool bOwnerIsLevel = false;
};

// ---------------------------------------------------------------------------
// Linked asset analysis entry (mirrors analysis.json schema)
// ---------------------------------------------------------------------------

struct FMapLinkLinkedAssetEntry
{
    FString Name;
    FString SourceBlendPath;
    FString AssetName;
    FTransform WorldTransform;

    /** v1.13.1 — the artist's Blender collection chain for this instance
     *  (e.g. "Layer1/Props"), relative to the blend root. Empty = object sat
     *  directly in the master collection. Used to mirror the Blender outliner
     *  org as UE WP outliner sub-folders. */
    FString CollectionPath;
};

struct FMapLinkOwnGeoEntry
{
    FString Name;
    FTransform WorldTransform;

    /** v1.13.1 — Blender collection chain for this mesh (see above). */
    FString CollectionPath;
};

struct FMapLinkAnalysisReport
{
    TArray<FMapLinkOwnGeoEntry> OwnGeo;
    TArray<FMapLinkLinkedAssetEntry> LinkedAssets;
    TArray<FString> LibraryPaths;

    /** Blender scene unit scale (0.01 = cm, 1.0 = m). Default 1.0. */
    double UnitScale = 1.0;
};

// ---------------------------------------------------------------------------
// SM_ composite expansion (v1.6)
// ---------------------------------------------------------------------------

/** v1.8.1 — what an expansion entry represents. Defaults to StaticMesh
 *  so all existing producers/consumers keep their behaviour. */
enum class EMapLinkExpandedKind : uint8
{
    StaticMesh,   // MeshPath -> UStaticMesh (AStaticMeshActor / UStaticMeshComponent)
    BPReference,  // BPNodeName -> reference-mode proxy (bound BP / UChildActorComponent)
};

/** One placement produced by recursive SM_ composite expansion. */
struct FExpandedMesh
{
    /** v1.8.1 — discriminates StaticMesh vs BPReference entries. */
    EMapLinkExpandedKind Kind = EMapLinkExpandedKind::StaticMesh;

    /** [StaticMesh] Full UE5 asset path to the UStaticMesh, e.g.
     *  /Game/MapLink/SM_window1/Meshes/SM_window1 */
    FString MeshPath;

    /** [BPReference] The BP_ node's blend stem (e.g. "BP_wallwindow2d"),
     *  used to locate its reference-mode proxy at placement time. */
    FString BPNodeName;

    /** Composed world/relative transform for this placement. */
    FTransform Transform;

    /** The SM_ composite that owns this entry (for tagging). */
    FString SourceComposite;

    /** Index within the expansion (for tagging). */
    int32 Index = 0;
};
