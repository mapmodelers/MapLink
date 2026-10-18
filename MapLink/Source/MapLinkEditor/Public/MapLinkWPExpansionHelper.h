// Copyright MapModelers. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/SoftObjectPath.h"
#include "MapLinkTypes.h"
#include "MapLinkEditorState.h"

class UMapLinkProxyAsset;

/**
 * v1.17 — result of a dry-run walk of a root's hierarchy at a given depth.
 *
 * Expansion is the one MapLink operation that writes hundreds of actors into
 * a source-controlled level with no undo, driven by a single integer the user
 * has no way to reason about.  This turns "Depth 2" into "breaks 3 LI, places
 * 47 SM and 2 BP" BEFORE the level is opened.
 */
struct FMapLinkExpansionPreview
{
    /** True depth of the root's LI_ hierarchy (1 = no nested LI_). */
    int32 MaxDepth = 1;

    /** Counts at the requested depth. */
    int32 NumBrokenLI = 0;   // LI_ opened up (recursed into)
    int32 NumLI       = 0;   // LI_ left as ALevelInstance at the boundary
    int32 NumSM       = 0;
    int32 NumBP       = 0;

    /** analysis.json missing for one or more nodes — counts are a lower
     *  bound and those nodes fall back to LevelInstance placement. */
    bool bIncomplete = false;

    int32 TotalPlacements() const { return NumLI + NumSM + NumBP; }
};

/**
 * v1.18 — an expansion discovered by reading a LEVEL, not the ini.
 *
 * Expansion records live in EditorPerProjectUserSettings — correct, because an
 * expansion setup is personal. But the ACTORS land in a source-controlled
 * .umap. Those two facts together mean the ini is not the whole truth: a
 * teammate's expansion is in your level and invisible to you, and so is any
 * expansion your own records lost track of (every one the pre-v1.17 re-target
 * bug stranded, for instance).
 *
 * Reading the tags back turns "invisible" into a row you can act on.
 */
struct FMapLinkDiscoveredExpansion
{
    /** 8-hex id from the tag. Empty for pre-v1.17 actors, which carry no id
     *  segment — those are identified by their folder alone. */
    FString IdTag;

    /** Folder segment under MapLink/ — the expansion's label. */
    FString Label;

    /** Best-effort root stem, from the tag's composition segments. The
     *  outliner folder is named after the LABEL, which defaults to the root
     *  stem but need not equal it, so this is a suggestion the user confirms
     *  when adopting — not a fact. */
    FString RootGuess;

    /** Loaded actors carrying this expansion's tag. */
    int32 ActorCount = 0;

    /** A MapLink/<Label> folder exists in the world partition's actor
     *  descriptors, but no LOADED actor carries the tag — the expansion is
     *  real and simply streamed out. Reported rather than hidden, because
     *  "nothing here" and "not loaded right now" are very different answers. */
    bool bStreamedOut = false;
};

/**
 * v1.7 — World Partition Expansion.
 * v1.9 — Window-driven: expansion parameters come from the MapLink Manager
 *        window (root blend + target level + depth) and active expansions
 *        are recorded in UMapLinkEditorState — the old per-proxy
 *        bExpandIntoWP / TargetWPLevel / ExpandDepth properties are
 *        deprecated.  Placed actors are organised into a clean outliner
 *        hierarchy:
 *
 *            MapLink/<Root>/                    ← main parent folder
 *            MapLink/<Root>/<LI_child>/         ← subfolder per broken LI_
 *            MapLink/<Root>/.../<SM_composite>/ ← subfolder per composite SM_
 *
 * Each placed actor is additionally tagged for selective cleanup:
 *   MapLinkWPExpand:<source_composition>:<child_asset>:<index>
 *
 * v1.17 — the tag grew two trailing segments, both append-only because every
 * parser in the codebase reads Parts[0..4] behind a Parts.Num() >= N guard:
 *
 *   MapLinkWPExpand:<comp>:<child>:<index>:<x,y,z>:<expansion-id8>
 *                                          ^v1.15.3  ^v1.17
 *
 * The Id is what makes expansion plural.  Up to v1.16 the record key was the
 * root blend name, so cleanup could only ask "does this actor belong to root
 * X?" — which cannot separate two expansions of the same root, and cannot
 * find the actors of an expansion whose record has been re-pointed at another
 * level.  Matching on the Id answers "does this actor belong to THIS
 * expansion?" exactly.  Pre-v1.17 actors have no Id segment and are still
 * matched the old way (composition set + folder), so nothing already in a
 * level is stranded by the upgrade.
 */
class FMapLinkWPExpansionHelper
{
public:
    /**
     * v1.9 — Expand a root LI_ hierarchy into a World Partition level.
     * Cleans up any previous expansion for this root first, then records
     * the expansion in UMapLinkEditorState so reimports re-expand it.
     *
     * @param RootBlendName    LI_ blend stem (e.g. "LI_MyLevel").
     * @param TargetLevelPath  The World Partition level to expand into.
     * @param ExpandDepth      How many LI_ levels deep to break open (>=1).
     */
    static EMapLinkResult ExpandIntoWP(
        const FString& RootBlendName,
        const FSoftObjectPath& TargetLevelPath,
        int32 ExpandDepth);

    // ── v1.17 — record-driven expansion ──────────────────────────────────

    /**
     * v1.17 — expand one record.  This is the real entry point; ExpandIntoWP
     * above is now a thin wrapper that finds-or-creates the record for
     * (root, level).
     *
     * Cleans up THIS record's previous placement (matched by the record Id in
     * the actor tag, falling back to the folder for pre-v1.17 actors), places
     * the hierarchy, and writes the record back with LastPlacedCount /
     * LastExpandedUtc.  InOutRecord gains an Id if it did not have one.
     *
     * @param bSaveAndReopen  false when a caller is expanding several records
     *                        into the same level and will save once at the end.
     */
    static EMapLinkResult ExpandRecord(
        FMapLinkWPExpansionRecord& InOutRecord,
        bool bSaveAndReopen = true);

    /** v1.17 — clean up exactly one expansion by Id: removes its actors and
     *  folders from its target level, drops the record, saves. */
    static EMapLinkResult CleanupExpansion(const FGuid& ExpansionId);

    /** v1.17 — remove every MapLink expansion from one level and drop the
     *  matching records.  The button behind CleanupAllWPExpansion, which has
     *  existed since v1.9 with no caller. */
    static EMapLinkResult CleanupAllExpansionsInLevel(
        const FSoftObjectPath& TargetLevelPath);

    /** v1.17 — remove the actors of one record from an already-open world.
     *  Matches on the record Id segment of the tag; pre-v1.17 actors (no Id
     *  segment) are matched by folder + composition as before.  Returns the
     *  number of actors removed. */
    static int32 CleanupWPForExpansion(
        UWorld* TargetWorld,
        const FMapLinkWPExpansionRecord& Record);

    /**
     * v1.17 — dry run.  Walks analysis.json exactly as ExpandLIRecursive
     * would, without loading the level or spawning anything.  Cheap enough
     * to call on every depth change in the UI.
     */
    static FMapLinkExpansionPreview PreviewExpansion(
        const FString& RootBlendName,
        int32 ExpandDepth);

    // ── v1.18 — level-as-source-of-truth ─────────────────────────────────

    /**
     * v1.18 — every MapLink expansion physically present in a world, read
     * from actor tags and outliner folders. Independent of the ini, so it
     * sees expansions this machine has no record of.
     *
     * Tag discovery uses TActorIterator, i.e. LOADED actors — the same set
     * every cleanup path in this file acts on, so what is discovered is
     * exactly what can be cleaned. World Partition descriptors are consulted
     * separately for folder presence, so a fully streamed-out expansion is
     * reported (bStreamedOut) instead of silently missing.
     */
    static TArray<FMapLinkDiscoveredExpansion> ScanExpansionsInWorld(
        UWorld* World);

    /**
     * v1.18 — take ownership of a discovered expansion: mint a record for it
     * and RE-TAG its actors with the new record's Id.
     *
     * Re-tagging is the whole operation, not a side effect. The tag carries
     * only the first 8 hex of the Guid, which is one-way — a record built
     * from a discovered tag could never match its own actors again. Stamping
     * the actors with the new Id is what makes the adopted expansion
     * cleanable, and it is also what "adopt" honestly means.
     *
     * Depth cannot be recovered from a level (nothing records how deep the
     * walk went), so the caller supplies it; it only matters at the next
     * re-expansion.
     */
    static EMapLinkResult AdoptDiscoveredExpansion(
        UWorld* World,
        const FMapLinkDiscoveredExpansion& Discovered,
        const FString& RootBlendName,
        int32 ExpandDepth,
        const FSoftObjectPath& TargetLevelPath);

    /** v1.18 — remove a discovered expansion's actors and folders without
     *  ever having had a record for it. Matches on the tag id when there is
     *  one, on the folder when there is not. */
    static int32 RemoveDiscoveredExpansion(
        UWorld* World,
        const FMapLinkDiscoveredExpansion& Discovered);

    /**
     * v1.17 — is this level a World Partition map, answered from asset
     * registry tags WITHOUT loading it.  Up to v1.16 the check lived inside
     * ExpandIntoWP, after LoadAndOpenTargetWorld had already opened the
     * level — so picking a non-WP level cost a full level load to find out.
     */
    static bool IsWorldPartitionLevel(const FSoftObjectPath& LevelPath);

    /**
     * v1.9 — Cleanup WP: remove every actor placed for a root expansion,
     * delete the MapLink/<Root> outliner folder tree, remove the expansion
     * record, and save the level.  Loads/opens the target level if needed.
     */
    static EMapLinkResult CleanupWP(
        const FString& RootBlendName,
        const FSoftObjectPath& TargetLevelPath);

    /**
     * Remove all actors placed for a specific root (tag + folder match)
     * from an already-open world, and delete the emptied outliner folders.
     * Does NOT touch the expansion record.  Returns actors removed.
     */
    static int32 CleanupWPForRoot(UWorld* TargetWorld, const FString& RootBlendName);

    /**
     * Remove ALL actors placed by any WP expansion (tagged MapLinkWPExpand:*)
     * and the entire MapLink outliner folder tree.  Returns actors removed.
     */
    static int32 CleanupAllWPExpansion(UWorld* TargetWorld);

    /**
     * After a reimport, re-expand every recorded expansion whose root or
     * any descendant appears in ChangedBlendNames.
     */
    static void ReExpandIfNeeded(const TArray<FString>& ChangedBlendNames);

    /**
     * v1.14 — In-place outliner repair after the nuke-and-recreate pass
     * (replaces the v1.13.x deferred close→reopen of the editor level).
     * On the CURRENTLY OPEN editor world:
     *   1. force-reloads every loaded ALevelInstance whose /Game/MapLink/
     *      target level was recreated (they otherwise keep pointing at the
     *      destroyed in-memory world — the "unloaded Level Instance" rows),
     *   2. prunes outliner folders left empty by the destructive pass
     *      (the "phantom folder" rows), and
     *   3. broadcasts the actor-list change so the outliner and viewports
     *      rebuild.
     * Pass the changed blend stems to limit the Level-Instance reload; an
     * empty array reloads every /Game/MapLink/ Level Instance.
     */
    static void RefreshEditorWorldAfterReimport(
        const TArray<FString>& ChangedBlendNames);

    /**
     * v1.9 — One-time migration: scan proxies still carrying the legacy
     * bExpandIntoWP flag, convert them into UMapLinkEditorState records,
     * and clear the flags.  Called when the Manager window opens.
     * Returns the number of proxies migrated.
     */
    static int32 MigrateLegacyProxyExpansionFlags();

    /**
     * v1.9 — Collect the root + every descendant LI_/SM_/BP_ stem by
     * walking analysis.json files (linked_assets only).  Public so the
     * auto-update ticker can watch a parent's whole dependency tree.
     */
    static void CollectDescendantNames(
        const FString& RootBlendName, TSet<FString>& OutNames);

    /** v1.13.1 — Delete every outliner folder at/under "MapLink/" that holds
     *  no actors (deepest-first). Clears stale orphan folders left by removed
     *  expansion roots (e.g. an empty "MapLink/LI_projet_test").
     *  v1.15: root-object aware — only touches the target world's OWN
     *  folders (folders inside embedded Level Instance containers are
     *  skipped; deleting their paths against the world root desynced the
     *  outliner — Assertion ActorFolderTreeItem.cpp:287).
     *  v1.15.1: in WP worlds, emptiness consults the actor DESCRIPTORS
     *  (every actor, loaded or unloaded) — valid regardless of streaming
     *  state, so it may run after a fresh LoadMap.  Gated by
     *  UMapLinkSettings::bPruneWPFolders (emergency valve, default ON).
     *  This prune — not DeleteFolderTree, disarmed in WP — is what keeps
     *  the outliner free of dead MapLink folders. */
    static void PruneEmptyMapLinkFolders(UWorld* TargetWorld);

private:
    /** Load the target level, open it in the editor if needed, verify WP.
     *  Returns nullptr on failure. */
    static UWorld* LoadAndOpenTargetWorld(const FSoftObjectPath& TargetLevelPath);

    /**
     * v1.9 — Ensure every segment of an outliner folder path exists as a
     * real folder in the world (creates each ancestor).  In World
     * Partition levels folders are actor-backed and are NOT reliably
     * created by SetFolderPath() alone, so we create them explicitly.
     */
    static void EnsureFolderHierarchy(UWorld* World, const FString& FolderPath);

    /** Clear the per-run folder memo EnsureFolderHierarchy keeps, and report
     *  how many duplicate creates it suppressed. Called at the start of every
     *  expansion — see the definition for what the memo is preventing. */
    static void ResetEnsuredFolderCache();


    /** Delete an outliner folder subtree (deepest-first), best-effort. */
    static void DeleteFolderTree(UWorld* TargetWorld, const FString& TopFolderPath);

    /**
     * Recursive core: resolve one LI_ node and either break it open
     * (if depth < ExpandDepth) or place it as an ALevelInstance actor
     * (if depth >= ExpandDepth).
     */
    static void ExpandLIRecursive(
        const FString& BlendName,
        const FTransform& ParentTransform,
        const FString& SourceComposition,
        const FString& FolderPath,
        int32 CurrentDepth,
        int32 ExpandDepth,
        UWorld* TargetWorld,
        TSet<FString>& VisitedSet,
        int32& OutPlacedCount);

    /** Place an SM_ asset (leaf or composite) into the target WP world.
     *  v1.14 — InstanceName is the Blender instance-object name (e.g.
     *  "SM_untruc.001"); in "Collection + instanced collection = folders"
     *  mode each placed instance gets its OWN folder named after it. */
    static void PlaceSMInWP(
        const FString& AssetName,
        const FTransform& WorldTransform,
        const FString& SourceComposition,
        const FString& FolderPath,
        UWorld* TargetWorld,
        int32& OutPlacedCount,
        const FString& InstanceName = FString());

    /** Place a BP_ asset into the target WP world (InstanceName: see
     *  PlaceSMInWP). */
    static void PlaceBPInWP(
        const FString& AssetName,
        const FTransform& WorldTransform,
        const FString& SourceComposition,
        const FString& FolderPath,
        UWorld* TargetWorld,
        int32& OutPlacedCount,
        const FString& InstanceName = FString());

    /** Place an LI_ asset as an ALevelInstance at the depth boundary. */
    static void PlaceLIInWP(
        const FString& AssetName,
        const FTransform& WorldTransform,
        const FString& SourceComposition,
        const FString& FolderPath,
        UWorld* TargetWorld,
        int32& OutPlacedCount);

    /** Apply a MapLinkWPExpand tag + outliner folder to an actor.
     *  v1.15.3 — AssetOrigin: world-space origin of the placed asset
     *  (the Blender collection-instance transform), appended to the tag
     *  as a trailing "x,y,z" segment.  All existing tag parsers use
     *  StartsWith + Parts.Num() >= N, so the extra segment is
     *  backward-compatible; the name display anchors ONE label per
     *  asset instance at this origin instead of one per mesh actor. */
    static void TagActor(
        AActor* Actor,
        const FString& SourceComposition,
        const FString& ChildAsset,
        int32 Index,
        const FString& FolderPath,
        const FVector& AssetOrigin);

    /** Write a JSON log of all placed actors. */
    static void WriteExpansionLog(
        const FString& RootBlendName,
        int32 PlacedCount);
};
