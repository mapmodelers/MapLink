// Copyright MapModelers. All Rights Reserved.
//
// v1.7.3 — Added bAutoUpdate, bShowInReimportMenu.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UObject/SoftObjectPtr.h"
#include "MapLinkProxyAsset.generated.h"

class UBlueprint;

UCLASS()
class MAPLINKRUNTIME_API UMapLinkProxyAsset : public UObject
{
    GENERATED_BODY()

public:
    /** Absolute path to the source .blend file */
    UPROPERTY(VisibleAnywhere, Category = "MapLink")
    FString BlendFilePath;

    /** MD5 hex string of the .blend at last successful import.
     *  (Must stay MD5 — it is compared against FMapLinkUtils::ComputeFileMD5
     *  and the Blender-side hashlib.md5 sidecar; an algorithm mismatch here
     *  is what caused the historic 100% FBX cache-miss bug.) */
    UPROPERTY(VisibleAnywhere, Category = "MapLink")
    FString BlendFileHash;

    /** UTC timestamp of last successful import (ISO 8601). Display only —
     *  do NOT use for staleness: it is a wall-clock instant and skews against
     *  a file's mtime clock. Use ImportedBlendModTime for staleness. */
    UPROPERTY(VisibleAnywhere, Category = "MapLink")
    FString LastImportTimestamp;

    /** v1.13.1 — Modification time (IFileManager::GetTimeStamp) of the source
     *  .blend AT last successful import. The Manager's fast staleness heuristic
     *  compares the .blend's CURRENT mtime against THIS value — mtime-vs-mtime,
     *  one clock — instead of against the wall-clock LastImportTimestamp, which
     *  skewed and made a freshly-edited cascade child read "Out of date" until
     *  an MD5 verify. Ticks()==0 means "unset" (legacy proxy → fall back). */
    UPROPERTY(VisibleAnywhere, Category = "MapLink")
    FDateTime ImportedBlendModTime = FDateTime(0);

    /** Soft reference to the primary generated asset (umap or uasset) */
    UPROPERTY(VisibleAnywhere, Category = "MapLink")
    TSoftObjectPtr<UObject> PrimaryAsset;

    /** Whether the primary asset is a Level (true) or Blueprint (false) */
    UPROPERTY(VisibleAnywhere, Category = "MapLink")
    bool bIsLevel = false;

    // -----------------------------------------------------------------
    // WP Expansion (v1.7) — DEPRECATED in v1.9
    //
    // Expansion is now driven from the MapLink Manager window and the
    // active state lives in UMapLinkEditorState (per-project user config).
    // These properties are kept only so old assets deserialize cleanly and
    // can be migrated (FMapLinkWPExpansionHelper::MigrateLegacyProxyExpansionFlags).
    // Do not write to them.
    // -----------------------------------------------------------------

    UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "MapLink|WP Expansion (Deprecated)")
    TSoftObjectPtr<UWorld> TargetWPLevel;

    UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "MapLink|WP Expansion (Deprecated)")
    bool bExpandIntoWP = false;

    UPROPERTY(VisibleAnywhere, AdvancedDisplay, Category = "MapLink|WP Expansion (Deprecated)")
    int32 ExpandDepth = 1;

    // -----------------------------------------------------------------
    // v1.7.3
    // -----------------------------------------------------------------

    /** Automatically reimport when the source .blend file changes.
     *  When viewing a MapLink level, reimports from the active level
     *  (full cascade) so parent LI_ levels refresh correctly. */
    UPROPERTY(EditAnywhere, Category = "MapLink|Auto-Update")
    bool bAutoUpdate = false;

    /** v1.9 — Recursive auto-reimport: when ON (and bAutoUpdate is ON),
     *  this asset also auto-reimports whenever ANY .blend in its
     *  dependency tree changes — not just its own source file.  Intended
     *  for parent LI_ composites so leaf edits propagate even when the
     *  parent's level isn't open.  Ignored unless bAutoUpdate is true. */
    UPROPERTY(EditAnywhere, Category = "MapLink|Auto-Update",
              meta=(EditCondition="bAutoUpdate"))
    bool bAutoUpdateRecursive = false;

    /** v1.13.1 — Skip AutoReimport: this asset's .blend changes NEVER
     *  trigger an automatic reimport — not through its own Auto-Update
     *  watch, and not as a child of a parent with Auto Reimport
     *  (Recursive).  Use for WIP assets you save often but don't want
     *  pulled into UE yet.  Manual reimports (and cascades triggered by
     *  OTHER changed files) still rebuild it when its hash changed. */
    UPROPERTY(EditAnywhere, Category = "MapLink|Auto-Update")
    bool bSkipAutoReimport = false;

    /** Add "Reimport" and "Force Reimport" entries for this asset
     *  in the MapLink top-level menu.  Typically enabled on root
     *  levels (e.g. LI_World). */
    UPROPERTY(EditAnywhere, Category = "MapLink|Reimport Menu")
    bool bShowInReimportMenu = false;

    // -----------------------------------------------------------------
    // v1.8 — BP Reference Mode
    // -----------------------------------------------------------------
    //
    // When bIsBPReference is true, this proxy represents a MapLink-side
    // stand-in for a user-authored Blueprint that lives OUTSIDE
    // /Game/MapLink/.  The pipeline does NOT synthesize a BP under
    // /Game/MapLink/<Stem>/ — it resolves instances at swap time via
    // BoundBlueprint.  When the bound BP is missing (deleted, renamed
    // out from under us, or never loaded), the level swap substitutes
    // /MapLink/Blueprints/BP_Deprecated and tags the actor with
    // MapLinkMissingBP:<OriginalBoundBlueprintPath>.
    //
    // The gate for reference-mode behavior at reimport time is the
    // presence of "bound_blueprint_path" in the sidecar analysis JSON,
    // not these properties (which only exist after the first import
    // populates them).

    /** Whether this proxy points at a user-authored Blueprint via
     *  BoundBlueprint instead of a synthesized /Game/MapLink BP. */
    UPROPERTY(EditAnywhere, Category = "MapLink|BP Reference")
    bool bIsBPReference = false;

    /** Soft reference to the actor-derived Blueprint this stand-in
     *  represents. Resolved at swap/expansion time. */
    UPROPERTY(EditAnywhere, Category = "MapLink|BP Reference")
    TSoftObjectPtr<UBlueprint> BoundBlueprint;

    /** Frozen copy of the bound BP's path at last import.  Survives
     *  the BP being deleted from disk so the missing-BP diagnostic
     *  ("MapLinkMissingBP:<path>") can still report a meaningful target. */
    UPROPERTY(EditAnywhere, Category = "MapLink|BP Reference")
    FString OriginalBoundBlueprintPath;
};
