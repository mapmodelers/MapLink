// Copyright MapModelers. All Rights Reserved.
//
// v1.7 — WP Expansion
// v1.9 — Window-driven expansion (root + level + depth from the MapLink
//        Manager window), expansion records in UMapLinkEditorState,
//        clean outliner folder hierarchy (MapLink/<Root>/<sub>/...),
//        Cleanup WP (actors + folders + record), legacy-flag migration.
//
// Recursively resolves a root LI_ hierarchy and places individual actors
// directly into a World Partition level.  Each placed actor is tagged
// MapLinkWPExpand:<source_composition>:<child_asset>:<index> for selective
// cleanup and re-expansion, and foldered under MapLink/<Root>/.

#include "MapLinkWPExpansionHelper.h"
#include "MapLinkPipeline.h"
#include "MapLinkUtils.h"            // v1.8 — LogMapLinkBPStandIn declaration
#include "MapLinkInterchangeHelper.h"
#include "MapLinkLevelHelper.h"
#include "MapLinkProxyAsset.h"
#include "MapLinkBPReferenceMode.h"  // v1.8
#include "MapLinkEditorState.h"      // v1.9
#include "MapLinkSourceControl.h"    // v1.9
#include "MapLinkSettings.h"         // v1.14 — FolderMode
#include "MapLinkNotify.h"           // v1.17 — success toasts

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Blueprint.h"
#include "Components/StaticMeshComponent.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"
#include "FileHelpers.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "LevelInstance/LevelInstanceSubsystem.h"   // v1.14 — in-place LI reload
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorActorFolders.h"      // v1.9 — outliner folder management
#include "Folder.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"     // v1.9 — package-name → filename for reopen
#include "Misc/FileHelper.h"
#include "Misc/ScopedSlowTask.h"
#include "Interfaces/IPluginManager.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionHelpers.h"           // v1.15.1 — desc-based prune
#include "WorldPartition/WorldPartitionActorDescInstance.h" // v1.15.1

// ---------------------------------------------------------------------------
// Tagging + foldering
// ---------------------------------------------------------------------------

namespace
{
    // v1.14 — folder-mode helpers.  The mandatory MapLink/<Root> base folder
    // is created in every mode; these only gate the sub-structure below it.

    /** WP outliner folders are actor-backed UObjects whose names cannot
     *  contain '.', but Blender instance names carry '.NNN' suffixes
     *  ("SM_untruc.001").  An unsanitized segment creates an EMPTY folder
     *  object while the actors' dotted folder path never binds to it —
     *  so map '.' → '_' (same convention as FBX mesh naming). */
    FString SanitizeFolderSeg(const FString& In)
    {
        return In.Replace(TEXT("."), TEXT("_"));
    }

    /** Base / Sub, except in mode None (everything stays in the base). */
    FString ComposeFolder(const FString& Base, const FString& Sub)
    {
        if (Sub.IsEmpty() ||
            UMapLinkSettings::GetFolderMode() == EMapLinkFolderMode::None)
        {
            return Base;
        }
        return Base / Sub;
    }

    /** Per-instance folder for instanced collections placed as instances
     *  (leaf SM_, BP_, boundary LI_) — only in
     *  "Collection + instanced collection = folders" mode. */
    FString ComposeInstanceFolder(const FString& Base, const FString& AssetName)
    {
        if (UMapLinkSettings::GetFolderMode() ==
            EMapLinkFolderMode::CollectionAndInstanceFolders &&
            !AssetName.IsEmpty())
        {
            return Base / SanitizeFolderSeg(AssetName);
        }
        return Base;
    }

    // ── v1.17 — expansion identity ────────────────────────────────────────
    //
    // The Id of the expansion currently being placed.  A file-scope value
    // rather than a parameter threaded through ExpandLIRecursive →
    // PlaceSM/BP/LI → TagActor: expansion is a synchronous, single-threaded
    // editor operation, and four extra signatures would buy nothing.
    // Always set through FScopedExpansionId so an early return cannot leak it
    // into the next expansion.
    FString GCurrentExpansionIdTag;

    struct FScopedExpansionId
    {
        explicit FScopedExpansionId(const FString& IdTag)
        {
            GCurrentExpansionIdTag = IdTag;
        }
        ~FScopedExpansionId()
        {
            GCurrentExpansionIdTag.Reset();
        }
    };

    /** Id segment of a MapLinkWPExpand tag, or empty for pre-v1.17 actors.
     *  Layout: MapLinkWPExpand:<comp>:<child>:<idx>:<x,y,z>:<id8> */
    FString ParseExpansionIdTag(const FString& TagStr)
    {
        if (!TagStr.StartsWith(TEXT("MapLinkWPExpand:")))
        {
            return FString();
        }
        TArray<FString> Parts;
        TagStr.ParseIntoArray(Parts, TEXT(":"), /*CullEmpty=*/false);
        return Parts.Num() >= 6 ? Parts[5] : FString();
    }

    /** First MapLinkWPExpand tag on an actor, or empty. */
    FString FindExpansionTag(const AActor* Actor)
    {
        if (!Actor) return FString();
        for (const FName& Tag : Actor->Tags)
        {
            const FString TagStr = Tag.ToString();
            if (TagStr.StartsWith(TEXT("MapLinkWPExpand:")))
            {
                return TagStr;
            }
        }
        return FString();
    }
}

void FMapLinkWPExpansionHelper::TagActor(
    AActor* Actor,
    const FString& SourceComposition,
    const FString& ChildAsset,
    int32 Index,
    const FString& FolderPath,
    const FVector& AssetOrigin)
{
    if (!Actor) return;

    // v1.15.3 — trailing segment = the placed asset's origin (collection
    // instance transform), consumed by the name display.  Extra segment
    // is backward-compatible with every existing parser.
    // v1.17 — one more trailing segment: the expansion record Id.  Also
    // append-only for the same reason (every parser is Parts.Num() >= N
    // guarded and reads Parts[0..4]), so v1.16 actors need no re-tagging and
    // cleanup can tell two expansions of the same root apart.
    FString TagStr = FString::Printf(
        TEXT("MapLinkWPExpand:%s:%s:%d:%.2f,%.2f,%.2f"),
        *SourceComposition, *ChildAsset, Index,
        AssetOrigin.X, AssetOrigin.Y, AssetOrigin.Z);
    if (!GCurrentExpansionIdTag.IsEmpty())
    {
        TagStr += TEXT(":") + GCurrentExpansionIdTag;
    }
    Actor->Tags.Add(FName(*TagStr));

    // v1.9 — clean outliner hierarchy.  Create the folder tree explicitly
    // first (SetFolderPath alone does not reliably create actor-backed WP
    // folders), then assign the actor to it.
    if (!FolderPath.IsEmpty())
    {
        EnsureFolderHierarchy(Actor->GetWorld(), FolderPath);
        Actor->SetFolderPath(FName(*FolderPath));
    }
}

// ---------------------------------------------------------------------------
// Folder hierarchy creation (v1.9)
// ---------------------------------------------------------------------------

namespace
{
    /**
     * Folder paths this expansion run has already ensured.
     *
     * THE DUPLICATE-UActorFolder BUG. EnsureFolderHierarchy is called from
     * TagActor — once per placed actor, not once per folder. Twenty-four
     * actors sharing one folder called it twenty-four times, and in a World
     * Partition world ContainsFolder did not report the folder the previous
     * call had just created: a WP folder is a UActorFolder in its own external
     * package, and it is not visible to the in-memory query until something
     * flushes. So every call created another one.
     *
     * Measured on WP_Game, 2026-08-26: 185 UActorFolder packages holding 24
     * folders labelled LINK, 20 labelled LINK_LI, 16 labelled LINK_SM — the
     * actor counts, exactly. The editor logged 188 "Merging duplicate actor
     * folder" lines on load, 187 of them under MapLink/.
     *
     * Those duplicates are what makes deleting a folder assert the outliner
     * (ActorFolderTreeItem.cpp:287): merged folders leave tree items holding a
     * path that no longer matches their UActorFolder. v1.15 responded by
     * disarming DeleteFolderTree in WP worlds, which stopped MapLink tripping
     * it — and left the cause running, so the crash still waits for anyone who
     * deletes a folder by hand.
     *
     * The memo answers the question ContainsFolder cannot: "did WE already
     * ensure this path in this run?". Cleared at the start of every expansion,
     * so a folder the user deleted between runs is still recreated.
     */
    TSet<FString> GEnsuredFolderPaths;
    int32 GEnsuredFolderHits = 0;
}

void FMapLinkWPExpansionHelper::ResetEnsuredFolderCache()
{
    if (GEnsuredFolderHits > 0)
    {
        UE_LOG(LogMapLink, Log,
               TEXT("WP Folders: %d duplicate folder-create call(s) suppressed "
                    "in the last run (%d distinct path(s) ensured)."),
               GEnsuredFolderHits, GEnsuredFolderPaths.Num());
    }
    GEnsuredFolderPaths.Reset();
    GEnsuredFolderHits = 0;
}

void FMapLinkWPExpansionHelper::EnsureFolderHierarchy(
    UWorld* World, const FString& FolderPath)
{
    if (!World || FolderPath.IsEmpty()) return;

    const FFolder::FRootObject RootObj =
        FFolder::GetWorldRootFolder(World).GetRootObject();

    FActorFolders& Folders = FActorFolders::Get();

    TArray<FString> Segments;
    FolderPath.ParseIntoArray(Segments, TEXT("/"), /*CullEmpty=*/true);

    FString Accum;
    for (const FString& Seg : Segments)
    {
        Accum = Accum.IsEmpty() ? Seg : (Accum + TEXT("/") + Seg);

        // Keyed by world too: two levels can hold the same folder path and
        // they are different folders.
        const FString MemoKey = World->GetPathName() + TEXT("|") + Accum;
        if (GEnsuredFolderPaths.Contains(MemoKey))
        {
            ++GEnsuredFolderHits;
            continue;
        }
        GEnsuredFolderPaths.Add(MemoKey);

        const FFolder Folder(RootObj, FName(*Accum));
        if (!Folders.ContainsFolder(*World, Folder))
        {
            Folders.CreateFolder(*World, Folder);
        }
    }
}

// ---------------------------------------------------------------------------
// Descendant resolution (shared by cleanup + re-expand)
// ---------------------------------------------------------------------------

void FMapLinkWPExpansionHelper::CollectDescendantNames(
    const FString& RootBlendName, TSet<FString>& OutNames)
{
    FString PluginBaseDir = FMapLinkPipeline::GetPluginBaseDir();

    TArray<FString> Queue;
    Queue.Add(RootBlendName);

    while (Queue.Num() > 0)
    {
        FString Current = Queue.Pop(EAllowShrinking::No);
        if (OutNames.Contains(Current)) continue;
        OutNames.Add(Current);

        FString AnalysisPath = FMapLinkUtils::ResolveArtifactLogFile(
            Current / TEXT("analysis.json"));
        FMapLinkAnalysisReport Report;
        if (FMapLinkInterchangeHelper::ParseAnalysisJson(AnalysisPath, Report))
        {
            for (const FMapLinkLinkedAssetEntry& Linked : Report.LinkedAssets)
            {
                FString ChildName = FPaths::GetBaseFilename(Linked.SourceBlendPath);
                if (!OutNames.Contains(ChildName))
                {
                    Queue.Add(ChildName);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// v1.15.2 — Clean-outliner reopen (shared by Expand / Remove / re-expand)
//
// RedrawAllViewports is not enough for World Partition outliner folders; a
// full blank-then-reopen of the current editor level is what actually
// rebuilds the tree (v1.9/v1.13.1 lessons — LoadMap no-ops on the already-
// open level, hence the blank map first).
// ---------------------------------------------------------------------------

namespace
{
    void ReopenEditorLevelForOutliner()
    {
        UWorld* ReloadWorld =
            GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        if (!ReloadWorld)
        {
            return;
        }

        const FString PackageName = ReloadWorld->GetOutermost()->GetName();
        if (PackageName.StartsWith(TEXT("/Temp/")))
        {
            return;   // untitled/transient — nothing to reopen
        }

        FString MapFilename;
        const bool bResolved =
            FPackageName::TryConvertLongPackageNameToFilename(
                PackageName, MapFilename,
                FPackageName::GetMapPackageExtension()) &&
            FPaths::FileExists(MapFilename);

        UE_LOG(LogMapLink, Log,
               TEXT("WP: Reopening level '%s' to refresh outliner folders."),
               *PackageName);

        GEditor->CreateNewMapForEditing();
        FEditorFileUtils::LoadMap(bResolved ? MapFilename : PackageName);
    }
}

// ---------------------------------------------------------------------------
// Folder deletion (best-effort)
// ---------------------------------------------------------------------------

void FMapLinkWPExpansionHelper::DeleteFolderTree(
    UWorld* TargetWorld, const FString& TopFolderPath)
{
    if (!TargetWorld || TopFolderPath.IsEmpty()) return;

    // v1.15 — DISARMED in World Partition worlds.  Deleting UActorFolder-
    // backed folders here asserted the SceneOutliner three times in one
    // day (ActorFolderTreeItem.cpp:287) across two different call paths,
    // surviving both a root-object-identity fix and a post-LoadMap revert.
    // The common denominator is folder DELETION in a WP world whose
    // on-disk folder set carries duplicates (UE logs "Merging duplicate
    // actor folder" on every load of the affected map).  Folder deletion
    // is cosmetic; a crashed editor mid-reimport is not.  Re-expansion
    // reuses existing folder paths (EnsureFolderHierarchy is
    // find-or-create), so skipping deletion only leaves empty folders
    // behind.  Non-WP worlds keep the old behaviour (string-based
    // folders, no UActorFolder objects, never crashed).
    if (TargetWorld->GetWorldPartition() != nullptr)
    {
        UE_LOG(LogMapLink, Log,
               TEXT("WP Cleanup: SKIPPING folder-tree deletion under '%s' "
                    "(World Partition world — outliner-assert guard, "
                    "v1.15). Empty folders may remain; they are reused on "
                    "the next expansion."),
               *TopFolderPath);
        return;
    }

    // Collect every known folder under (and including) TopFolderPath,
    // then delete deepest-first so parents empty out as we go.
    TArray<FFolder> ToDelete;

    FActorFolders& Folders = FActorFolders::Get();
    const FFolder WorldRoot = FFolder::GetWorldRootFolder(TargetWorld);

    Folders.ForEachFolder(*TargetWorld,
        [&ToDelete, &TopFolderPath, &WorldRoot](const FFolder& Folder)
        {
            // v1.15 — CRASH FIX: only the target world's OWN folders.
            // ForEachFolder also yields folders living under OTHER root
            // objects — embedded Level Instance containers expose their
            // internal folders here (visible since the v1.14 AllLevels
            // folder scope put MapLink/ folders inside generated LI
            // levels).  The old code collected those paths and deleted
            // them against the WORLD's root object, which desyncs the
            // outliner's folder items (Assertion ActorFolderTreeItem.cpp
            // :287, observed live — twice).  Filter by root object and
            // delete each folder with its own identity.
            if (Folder.GetRootObject() != WorldRoot.GetRootObject())
            {
                return true;
            }
            const FString Path = Folder.GetPath().ToString();
            if (Path == TopFolderPath ||
                Path.StartsWith(TopFolderPath + TEXT("/")))
            {
                ToDelete.Add(Folder);
            }
            return true; // continue iteration
        });

    ToDelete.Sort([](const FFolder& A, const FFolder& B)
    {
        return A.GetPath().ToString().Len() > B.GetPath().ToString().Len();
    });

    for (const FFolder& Folder : ToDelete)
    {
        Folders.DeleteFolder(*TargetWorld, Folder);
    }

    if (ToDelete.Num() > 0)
    {
        UE_LOG(LogMapLink, Log,
               TEXT("WP Cleanup: deleted %d outliner folder(s) under '%s'."),
               ToDelete.Num(), *TopFolderPath);
    }
}

void FMapLinkWPExpansionHelper::PruneEmptyMapLinkFolders(UWorld* TargetWorld)
{
    if (!TargetWorld) return;

    UWorldPartition* WorldPartition = TargetWorld->GetWorldPartition();

    // v1.15.1 — emergency valve (see UMapLinkSettings::bPruneWPFolders).
    if (WorldPartition && !UMapLinkSettings::Get()->bPruneWPFolders)
    {
        UE_LOG(LogMapLink, Log,
               TEXT("WP Prune: disabled by setting (bPruneWPFolders=false) "
                    "— skipping."));
        return;
    }

    FActorFolders& Folders = FActorFolders::Get();
    const FFolder WorldRoot = FFolder::GetWorldRootFolder(TargetWorld);
    static const FString Root = TEXT("MapLink");

    // 1. Every folder at/under "MapLink" — the target world's OWN folders
    //    only (v1.15 crash fix: same root-object filtering as
    //    DeleteFolderTree; folders inside embedded Level Instance
    //    containers must never be touched from here).
    TArray<FFolder> MapLinkFolders;
    Folders.ForEachFolder(*TargetWorld,
        [&MapLinkFolders, &WorldRoot](const FFolder& Folder)
        {
            if (Folder.GetRootObject() != WorldRoot.GetRootObject())
            {
                return true;
            }
            const FString Path = Folder.GetPath().ToString();
            if (Path == Root || Path.StartsWith(Root + TEXT("/")))
            {
                MapLinkFolders.Add(Folder);
            }
            return true;
        });
    if (MapLinkFolders.Num() == 0) return;

    // 2. Folder paths that hold at least one actor — mark the actor's folder
    //    AND all of its ancestors as "used", so a non-empty deep folder keeps
    //    its parent chain.
    TSet<FString> UsedFolders;
    auto MarkUsedWithAncestors = [&UsedFolders](FString FP)
    {
        while (!FP.IsEmpty())
        {
            UsedFolders.Add(FP);
            int32 Slash = INDEX_NONE;
            if (FP.FindLastChar(TEXT('/'), Slash))
            {
                FP = FP.Left(Slash);
            }
            else
            {
                break;
            }
        }
    };

    // 2a. Loaded actors (covers transient/non-WP-stored actors too).
    for (TActorIterator<AActor> It(TargetWorld); It; ++It)
    {
        MarkUsedWithAncestors(It->GetFolderPath().ToString());
    }

    // 2b. v1.15.1 — In a World Partition world, ALSO consult the actor
    //     DESCRIPTORS: unloaded cells keep their actors out of the
    //     TActorIterator sweep, so with only (2a) a folder whose actors
    //     are simply not streamed in right now looks empty and would be
    //     deleted out from under them (the exact hazard that forced the
    //     v1.15 post-restore prune revert).  Descriptors enumerate EVERY
    //     actor, loaded or not, with its folder path — emptiness is now
    //     valid regardless of streaming state.
    if (WorldPartition)
    {
        FWorldPartitionHelpers::ForEachActorDescInstance(WorldPartition,
            [&MarkUsedWithAncestors]
            (const FWorldPartitionActorDescInstance* DescInstance) -> bool
            {
                if (DescInstance)
                {
                    MarkUsedWithAncestors(
                        DescInstance->GetActorDesc()->GetFolderPath().ToString());
                }
                return true;
            });
    }

    // 3. Delete the MapLink folders that hold no actors, deepest-first
    //    (each with its own FFolder identity — v1.15).
    TArray<FFolder> ToDelete;
    for (const FFolder& F : MapLinkFolders)
    {
        if (!UsedFolders.Contains(F.GetPath().ToString()))
        {
            ToDelete.Add(F);
        }
    }
    ToDelete.Sort([](const FFolder& A, const FFolder& B)
    {
        return A.GetPath().ToString().Len() > B.GetPath().ToString().Len();
    });
    for (const FFolder& F : ToDelete)
    {
        Folders.DeleteFolder(*TargetWorld, F);
    }

    if (ToDelete.Num() > 0)
    {
        UE_LOG(LogMapLink, Log,
               TEXT("WP Cleanup: pruned %d empty MapLink outliner folder(s)."),
               ToDelete.Num());
    }
}

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------

int32 FMapLinkWPExpansionHelper::CleanupWPForRoot(
    UWorld* TargetWorld,
    const FString& RootBlendName)
{
    if (!TargetWorld) return 0;

    const FString TopFolder = FString::Printf(TEXT("MapLink/%s"), *RootBlendName);

    // The tag records the *source composition* (deepest broken LI_), which
    // for a root expansion is the root or any descendant LI_.  Build the
    // descendant set so legacy actors (pre-v1.9, no folder path) are still
    // matched by composition.
    TSet<FString> Compositions;
    CollectDescendantNames(RootBlendName, Compositions);

    TArray<AActor*> ToRemove;
    for (TActorIterator<AActor> It(TargetWorld); It; ++It)
    {
        AActor* Actor = *It;

        bool bIsExpanded = false;
        for (const FName& Tag : Actor->Tags)
        {
            FString TagStr = Tag.ToString();
            if (!TagStr.StartsWith(TEXT("MapLinkWPExpand:"))) continue;

            // MapLinkWPExpand:<comp>:<child>:<idx>
            TArray<FString> Parts;
            TagStr.ParseIntoArray(Parts, TEXT(":"), /*CullEmpty=*/false);
            if (Parts.Num() >= 2 && Compositions.Contains(Parts[1]))
            {
                bIsExpanded = true;
            }
            break;
        }

        if (!bIsExpanded)
        {
            // v1.9 folder match (catches anything the composition walk
            // missed, e.g. analysis.json no longer on disk)
            const FString ActorFolder = Actor->GetFolderPath().ToString();
            if (ActorFolder == TopFolder ||
                ActorFolder.StartsWith(TopFolder + TEXT("/")))
            {
                for (const FName& Tag : Actor->Tags)
                {
                    if (Tag.ToString().StartsWith(TEXT("MapLinkWPExpand:")))
                    {
                        bIsExpanded = true;
                        break;
                    }
                }
            }
        }

        if (bIsExpanded)
        {
            ToRemove.Add(Actor);
        }
    }

    for (AActor* Actor : ToRemove)
    {
        TargetWorld->DestroyActor(Actor);
    }

    DeleteFolderTree(TargetWorld, TopFolder);

    // v1.15.1 — WP worlds: the desc-based prune replaces the disarmed
    // tree deletion.  Runs after the actor sweep so folders emptied by
    // THIS cleanup are collected; folders still referenced by unloaded
    // actors are protected by the descriptor pass.
    PruneEmptyMapLinkFolders(TargetWorld);

    if (ToRemove.Num() > 0)
    {
        UE_LOG(LogMapLink, Log,
               TEXT("WP Cleanup: removed %d actors for root '%s'."),
               ToRemove.Num(), *RootBlendName);
    }

    return ToRemove.Num();
}

// ---------------------------------------------------------------------------
// v1.17 — per-record cleanup
// ---------------------------------------------------------------------------

int32 FMapLinkWPExpansionHelper::CleanupWPForExpansion(
    UWorld* TargetWorld,
    const FMapLinkWPExpansionRecord& Record)
{
    if (!TargetWorld) return 0;

    const FString IdTag     = Record.GetIdTag();
    const FString TopFolder = Record.GetFolderPath();

    // Pre-v1.17 actors carry no Id segment.  They are still matched the old
    // way — composition set + folder — so upgrading does not strand anything
    // already in the level.
    TSet<FString> Compositions;
    CollectDescendantNames(Record.RootBlendName, Compositions);

    TArray<AActor*> ToRemove;
    for (TActorIterator<AActor> It(TargetWorld); It; ++It)
    {
        AActor* Actor = *It;

        const FString TagStr = FindExpansionTag(Actor);
        if (TagStr.IsEmpty())
        {
            continue;   // not a MapLink expansion actor at all
        }

        const FString ActorIdTag = ParseExpansionIdTag(TagStr);

        bool bMine = false;
        if (!ActorIdTag.IsEmpty())
        {
            // v1.17 actor: the Id is authoritative and exact.  This is what
            // lets two expansions of the same root coexist in one level.
            bMine = (!IdTag.IsEmpty() && ActorIdTag == IdTag);
        }
        else
        {
            // Legacy actor: composition or folder, as v1.16 did.
            TArray<FString> Parts;
            TagStr.ParseIntoArray(Parts, TEXT(":"), /*CullEmpty=*/false);
            const bool bCompMatch =
                Parts.Num() >= 2 && Compositions.Contains(Parts[1]);

            const FString ActorFolder = Actor->GetFolderPath().ToString();
            const bool bFolderMatch =
                ActorFolder == TopFolder ||
                ActorFolder.StartsWith(TopFolder + TEXT("/"));

            bMine = bCompMatch || bFolderMatch;
        }

        if (bMine)
        {
            ToRemove.Add(Actor);
        }
    }

    for (AActor* Actor : ToRemove)
    {
        TargetWorld->DestroyActor(Actor);
    }

    DeleteFolderTree(TargetWorld, TopFolder);
    PruneEmptyMapLinkFolders(TargetWorld);

    if (ToRemove.Num() > 0)
    {
        UE_LOG(LogMapLink, Log,
               TEXT("WP Cleanup: removed %d actors for expansion '%s' (%s)."),
               ToRemove.Num(), *Record.GetLabel(), *IdTag);
    }

    return ToRemove.Num();
}

int32 FMapLinkWPExpansionHelper::CleanupAllWPExpansion(UWorld* TargetWorld)
{
    if (!TargetWorld) return 0;

    TArray<AActor*> ToRemove;
    for (TActorIterator<AActor> It(TargetWorld); It; ++It)
    {
        for (const FName& Tag : (*It)->Tags)
        {
            if (Tag.ToString().StartsWith(TEXT("MapLinkWPExpand:")))
            {
                ToRemove.Add(*It);
                break;
            }
        }
    }

    for (AActor* Actor : ToRemove)
    {
        TargetWorld->DestroyActor(Actor);
    }

    DeleteFolderTree(TargetWorld, TEXT("MapLink"));

    // v1.15.1 — DeleteFolderTree is disarmed in WP worlds; the desc-based
    // prune is what actually keeps the outliner clean there (deletes only
    // folders no actor — loaded OR unloaded — references).
    PruneEmptyMapLinkFolders(TargetWorld);

    if (ToRemove.Num() > 0)
    {
        UE_LOG(LogMapLink, Log,
               TEXT("WP Cleanup: removed ALL %d WP-expanded actors."),
               ToRemove.Num());
    }

    return ToRemove.Num();
}

// ---------------------------------------------------------------------------
// Actor placement helpers
// ---------------------------------------------------------------------------

void FMapLinkWPExpansionHelper::PlaceSMInWP(
    const FString& AssetName,
    const FTransform& WorldTransform,
    const FString& SourceComposition,
    const FString& FolderPath,
    UWorld* TargetWorld,
    int32& OutPlacedCount,
    const FString& InstanceName)
{
    FString PluginBaseDir = FMapLinkPipeline::GetPluginBaseDir();

    // v1.14 — in "Collection + instanced collection = folders" mode, EVERY
    // placed instance gets its own folder named after the Blender instance
    // object ("SM_untruc", "SM_untruc_001", …) — one folder PER instanced
    // collection, not per asset type.  Dots sanitized: WP folder objects
    // cannot carry '.' in their names.
    const FString InstSeg = SanitizeFolderSeg(
        InstanceName.IsEmpty() ? AssetName : InstanceName);
    const bool bInstanceFolders =
        UMapLinkSettings::GetFolderMode() ==
        EMapLinkFolderMode::CollectionAndInstanceFolders;

    // Check if composite or leaf (same logic as MapLinkLevelHelper swap phase)
    FString AnalysisPath = FMapLinkUtils::ResolveArtifactLogFile(
        AssetName / TEXT("analysis.json"));
    FMapLinkAnalysisReport SubReport;
    bool bNeedsExpansion = false;
    if (FMapLinkInterchangeHelper::ParseAnalysisJson(AnalysisPath, SubReport))
    {
        bNeedsExpansion = (SubReport.LinkedAssets.Num() > 0 ||
                           SubReport.OwnGeo.Num() > 1);
    }

    if (bNeedsExpansion)
    {
        // Composite SM_ — recursive expansion via existing ExpandSM.
        // v1.9: composite contents get their own subfolder per .blend.
        // v1.14 — gated by the Folder Creation Mode (None → flat); in
        // instance-folders mode the subfolder is PER INSTANCE (InstSeg).
        const FString CompositeFolder = bInstanceFolders
            ? (FolderPath / InstSeg)
            : ComposeFolder(FolderPath, AssetName);

        TSet<FString> Visited;
        TArray<FExpandedMesh> Expanded =
            FMapLinkLevelHelper::ExpandSM(AssetName, WorldTransform, Visited);

        for (int32 i = 0; i < Expanded.Num(); ++i)
        {
            // v1.8.1 — nested BP_ reference expanded from the composite
            if (Expanded[i].Kind == EMapLinkExpandedKind::BPReference)
            {
                AActor* BPActor = FMapLinkLevelHelper::SpawnExpandedBPReference(
                    TargetWorld,
                    Expanded[i].BPNodeName,
                    Expanded[i].Transform,
                    SourceComposition,
                    TEXT("wp_expansion"),
                    NAME_None);
                if (BPActor)
                {
                    TagActor(BPActor, SourceComposition,
                             Expanded[i].BPNodeName, OutPlacedCount,
                             CompositeFolder,
                             Expanded[i].Transform.GetLocation());
                    ++OutPlacedCount;
                }
                continue;
            }

            UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *Expanded[i].MeshPath);
            if (!Mesh)
            {
                UE_LOG(LogMapLink, Warning,
                       TEXT("WP: ExpandSM mesh not found: '%s'"), *Expanded[i].MeshPath);
                continue;
            }

            FActorSpawnParameters SpawnParams;
            SpawnParams.SpawnCollisionHandlingOverride =
                ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

            AStaticMeshActor* SMActor = TargetWorld->SpawnActor<AStaticMeshActor>(
                AStaticMeshActor::StaticClass(),
                Expanded[i].Transform, SpawnParams);
            if (SMActor)
            {
                SMActor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
                SMActor->SetActorScale3D(Expanded[i].Transform.GetScale3D());
                SMActor->SetActorLabel(FString::Printf(
                    TEXT("%s_exp_%d"), *AssetName, i));

                TagActor(SMActor, SourceComposition, AssetName, OutPlacedCount,
                         CompositeFolder, WorldTransform.GetLocation());
                ++OutPlacedCount;
            }
        }

        UE_LOG(LogMapLink, Log,
               TEXT("WP: Expanded SM_ composite '%s' → %d mesh actors."),
               *AssetName, Expanded.Num());
    }
    else
    {
        // Leaf SM_ — single mesh
        FString MeshPath = FString::Printf(
            TEXT("/Game/MapLink/%s/Meshes/%s"), *AssetName, *AssetName);
        UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);

        if (Mesh)
        {
            FActorSpawnParameters SpawnParams;
            SpawnParams.SpawnCollisionHandlingOverride =
                ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

            AStaticMeshActor* SMActor = TargetWorld->SpawnActor<AStaticMeshActor>(
                AStaticMeshActor::StaticClass(),
                WorldTransform, SpawnParams);
            if (SMActor)
            {
                SMActor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
                SMActor->SetActorScale3D(WorldTransform.GetScale3D());
                SMActor->SetActorLabel(AssetName);

                // v1.14 — a leaf SM_ instance is an instanced collection:
                // it gets its own PER-INSTANCE folder only in the
                // "Collection + instanced collection = folders" mode.
                TagActor(SMActor, SourceComposition, AssetName, OutPlacedCount,
                         ComposeInstanceFolder(FolderPath, InstSeg),
                         WorldTransform.GetLocation());
                ++OutPlacedCount;
            }

            UE_LOG(LogMapLink, Log,
                   TEXT("WP: Placed SM_ leaf '%s'."), *AssetName);
        }
        else
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("WP: SM_ mesh not found at '%s'."), *MeshPath);
        }
    }
}

void FMapLinkWPExpansionHelper::PlaceBPInWP(
    const FString& AssetName,
    const FTransform& WorldTransform,
    const FString& SourceComposition,
    const FString& FolderPath,
    UWorld* TargetWorld,
    int32& OutPlacedCount,
    const FString& InstanceName)
{
    // v1.14 — a placed BP_ instance is an instanced collection: it gets its
    // own PER-INSTANCE folder only in "Collection + instanced collection =
    // folders" mode.
    const FString PlacedFolder = ComposeInstanceFolder(
        FolderPath, InstanceName.IsEmpty() ? AssetName : InstanceName);

    // ----- v1.8 reference-mode short-circuit ----------------------
    FString RefBoundBPPath;
    UMapLinkProxyAsset* RefProxy = nullptr;
    const bool bRefMode =
        FMapLinkBPReferenceMode::IsProxyReferenceMode(
            AssetName, RefBoundBPPath, &RefProxy);

    if (bRefMode)
    {
        bool bWasDeprecated = false;
        UClass* SpawnClass = FMapLinkBPReferenceMode::ResolveBoundClass(
            RefProxy, bWasDeprecated);

        if (SpawnClass)
        {
            FActorSpawnParameters SpawnParams;
            SpawnParams.SpawnCollisionHandlingOverride =
                ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

            AActor* SpawnedActor = TargetWorld->SpawnActor<AActor>(
                SpawnClass, WorldTransform, SpawnParams);
            if (SpawnedActor)
            {
                SpawnedActor->SetActorScale3D(WorldTransform.GetScale3D());
                if (bWasDeprecated)
                {
                    SpawnedActor->SetActorLabel(
                        FString::Printf(TEXT("MISSING_%s"), *AssetName));
                    SpawnedActor->Tags.Add(FName(*FString::Printf(
                        TEXT("MapLinkMissingBP:%s"), *RefBoundBPPath)));
                }
                else
                {
                    SpawnedActor->SetActorLabel(AssetName);
                }

                // Standard WP expansion tag — applied in addition to
                // MapLinkMissingBP: when relevant — so the cleanup pass
                // can still find these by composition.
                TagActor(SpawnedActor, SourceComposition, AssetName,
                         OutPlacedCount, PlacedFolder,
                         WorldTransform.GetLocation());
                ++OutPlacedCount;
            }

            FMapLinkBPReferenceMode::FPlacement Rec;
            Rec.ProxyPath          = RefProxy
                ? RefProxy->GetPathName() : FString();
            Rec.BoundBlueprintPath = RefBoundBPPath;
            Rec.PlacementContext   = TEXT("wp_expansion");
            Rec.OwnerAssetPath     = TargetWorld
                ? TargetWorld->GetPathName() : FString();
            Rec.Status             = bWasDeprecated
                ? FMapLinkBPReferenceMode::EPlacementStatus::DeprecatedSubstituted
                : FMapLinkBPReferenceMode::EPlacementStatus::Ok;
            FMapLinkBPReferenceMode::RecordPlacement(Rec);

            UE_LOG(LogMapLink, Log,
                   TEXT("WP: Placed BP_ '%s'%s (reference-mode bound='%s')."),
                   *AssetName,
                   bWasDeprecated ? TEXT(" [DEPRECATED_FALLBACK]") : TEXT(""),
                   *RefBoundBPPath);
        }
        else
        {
            UE_LOG(LogMapLinkBPStandIn, Error,
                   TEXT("WP: BP_ '%s' reference-mode: neither bound BP "
                        "nor BP_Deprecated loadable — skipping placement."),
                   *AssetName);

            FMapLinkBPReferenceMode::FPlacement Rec;
            Rec.ProxyPath          = RefProxy
                ? RefProxy->GetPathName() : FString();
            Rec.BoundBlueprintPath = RefBoundBPPath;
            Rec.PlacementContext   = TEXT("wp_expansion");
            Rec.OwnerAssetPath     = TargetWorld
                ? TargetWorld->GetPathName() : FString();
            Rec.Status             = FMapLinkBPReferenceMode::EPlacementStatus::Missing;
            FMapLinkBPReferenceMode::RecordPlacement(Rec);
        }
        return;
    }

    // ----- legacy path -------------------------------------------------
    // Check for _GP wrapper first (same logic as swap phase)
    FString GPPath = FString::Printf(
        TEXT("/Game/MapLink/%s/%s_GP"), *AssetName, *AssetName);
    UBlueprint* GPBP = LoadObject<UBlueprint>(nullptr, *GPPath);

    FString MainPath = FString::Printf(
        TEXT("/Game/MapLink/%s/%s"), *AssetName, *AssetName);
    UBlueprint* MainBP = LoadObject<UBlueprint>(nullptr, *MainPath);

    UBlueprint* SpawnBP = nullptr;
    if (GPBP && GPBP->GeneratedClass)
    {
        SpawnBP = GPBP;
        UE_LOG(LogMapLink, Log,
               TEXT("WP: Using _GP wrapper for '%s'."), *AssetName);
    }
    else if (MainBP && MainBP->GeneratedClass)
    {
        SpawnBP = MainBP;
    }

    if (SpawnBP)
    {
        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride =
            ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

        TSubclassOf<AActor> ActorClass =
            static_cast<TSubclassOf<AActor>>(SpawnBP->GeneratedClass);

        AActor* SpawnedActor = TargetWorld->SpawnActor<AActor>(
            ActorClass, WorldTransform, SpawnParams);
        if (SpawnedActor)
        {
            SpawnedActor->SetActorScale3D(WorldTransform.GetScale3D());
            SpawnedActor->SetActorLabel(AssetName);

            TagActor(SpawnedActor, SourceComposition, AssetName,
                     OutPlacedCount, PlacedFolder,
                     WorldTransform.GetLocation());
            ++OutPlacedCount;
        }

        UE_LOG(LogMapLink, Log,
               TEXT("WP: Placed BP_ '%s'."), *AssetName);
    }
    else
    {
        // v1.8 D7 hotfix2 — Legacy WP placement found no synth BP.
        // Fall back to BP_Deprecated and tag the actor so it's
        // visible/queryable in the WP outliner.
        bool bDummyDeprecated = false;
        UClass* DeprecatedClass =
            FMapLinkBPReferenceMode::ResolveBoundClassFromPath(
                FString(), bDummyDeprecated);

        if (DeprecatedClass)
        {
            FActorSpawnParameters SpawnParams;
            SpawnParams.SpawnCollisionHandlingOverride =
                ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

            AActor* SpawnedActor = TargetWorld->SpawnActor<AActor>(
                DeprecatedClass, WorldTransform, SpawnParams);
            if (SpawnedActor)
            {
                SpawnedActor->SetActorScale3D(WorldTransform.GetScale3D());
                SpawnedActor->SetActorLabel(
                    FString::Printf(TEXT("MISSING_%s"), *AssetName));
                SpawnedActor->Tags.Add(FName(*FString::Printf(
                    TEXT("MapLinkMissingBP:%s"), *AssetName)));

                // Standard WP expansion tag so cleanup still finds it.
                TagActor(SpawnedActor, SourceComposition, AssetName,
                         OutPlacedCount, PlacedFolder,
                         WorldTransform.GetLocation());
                ++OutPlacedCount;
            }

            FMapLinkBPReferenceMode::FPlacement Rec;
            Rec.ProxyPath          = FString();
            Rec.BoundBlueprintPath = FString();
            Rec.PlacementContext   = TEXT("wp_expansion");
            Rec.OwnerAssetPath     = TargetWorld
                ? TargetWorld->GetPathName() : FString();
            Rec.Status             =
                FMapLinkBPReferenceMode::EPlacementStatus::DeprecatedSubstituted;
            FMapLinkBPReferenceMode::RecordPlacement(Rec);

            UE_LOG(LogMapLinkBPStandIn, Warning,
                   TEXT("WP: BP_ '%s' -> BP_Deprecated substituted "
                        "(no proxy, no synth BP, no ref-mode binding "
                        "found).  Actor labeled 'MISSING_%s'."),
                   *AssetName, *AssetName);
        }
        else
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("WP: No BP asset found for '%s' AND BP_Deprecated "
                        "could not be loaded — placement skipped."),
                   *AssetName);
        }
    }
}

void FMapLinkWPExpansionHelper::PlaceLIInWP(
    const FString& AssetName,
    const FTransform& WorldTransform,
    const FString& SourceComposition,
    const FString& FolderPath,
    UWorld* TargetWorld,
    int32& OutPlacedCount)
{
    FString AssetPath = FString::Printf(
        TEXT("/Game/MapLink/%s/%s"), *AssetName, *AssetName);
    UWorld* LIWorld = LoadObject<UWorld>(nullptr, *AssetPath);

    if (LIWorld)
    {
        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride =
            ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

        ALevelInstance* LIActor = TargetWorld->SpawnActor<ALevelInstance>(
            ALevelInstance::StaticClass(),
            WorldTransform, SpawnParams);
        if (LIActor)
        {
            LIActor->SetActorScale3D(WorldTransform.GetScale3D());
            LIActor->SetActorLabel(AssetName);
            LIActor->SetWorldAsset(
                TSoftObjectPtr<UWorld>(FSoftObjectPath(AssetPath)));

            TagActor(LIActor, SourceComposition, AssetName,
                     OutPlacedCount, FolderPath,
                     WorldTransform.GetLocation());
            ++OutPlacedCount;
        }

        UE_LOG(LogMapLink, Log,
               TEXT("WP: Placed LI_ '%s' as ALevelInstance (depth boundary)."),
               *AssetName);
    }
    else
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("WP: No level asset found for '%s'."), *AssetPath);
    }
}

// ---------------------------------------------------------------------------
// Recursive expansion core
// ---------------------------------------------------------------------------

void FMapLinkWPExpansionHelper::ExpandLIRecursive(
    const FString& BlendName,
    const FTransform& ParentTransform,
    const FString& SourceComposition,
    const FString& FolderPath,
    int32 CurrentDepth,
    int32 MaxExpandDepth,
    UWorld* TargetWorld,
    TSet<FString>& VisitedSet,
    int32& OutPlacedCount)
{
    // Depth boundary reached — place as ALevelInstance (stops recursion)
    if (CurrentDepth >= MaxExpandDepth)
    {
        PlaceLIInWP(BlendName, ParentTransform, SourceComposition,
                     FolderPath, TargetWorld, OutPlacedCount);
        return;
    }

    // Cycle detection
    if (VisitedSet.Contains(BlendName))
    {
        UE_LOG(LogMapLink, Error,
               TEXT("WP ExpandLIRecursive: cycle detected for '%s' — aborting."),
               *BlendName);
        return;
    }
    VisitedSet.Add(BlendName);

    // Read analysis.json for this LI_
    FString PluginBaseDir = FMapLinkPipeline::GetPluginBaseDir();
    FString AnalysisPath = FMapLinkUtils::ResolveArtifactLogFile(
        BlendName / TEXT("analysis.json"));

    FMapLinkAnalysisReport Report;
    if (!FMapLinkInterchangeHelper::ParseAnalysisJson(AnalysisPath, Report))
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("WP: Failed to read analysis.json for '%s'. Placing as LevelInstance."),
               *BlendName);
        // Fallback: place as ALevelInstance since we can't resolve contents
        PlaceLIInWP(BlendName, ParentTransform, SourceComposition,
                     FolderPath, TargetWorld, OutPlacedCount);
        VisitedSet.Remove(BlendName);
        return;
    }

    // The "source composition" for tagging is the deepest broken LI_.
    FString ChildComposition = BlendName;

    // 1. Own geometry → place as individual StaticMeshActors
    for (const FMapLinkOwnGeoEntry& Geo : Report.OwnGeo)
    {
        // Compose transform: child local * parent world
        FTransform ComposedTransform = Geo.WorldTransform * ParentTransform;

        // v1.13.1 — nest under the artist's Blender collection chain.
        // v1.14 — gated by the Folder Creation Mode (None → flat).
        const FString GeoFolder = ComposeFolder(FolderPath, Geo.CollectionPath);

        // Resolve mesh path (same naming as FBX import).
        // v1.13.1 FIX: FbxFactory names a SINGLE mesh node after the FBX
        // file itself (no _<node> suffix); only multi-mesh imports produce
        // <file>_<node> assets. The old unconditional <stem>_<node>
        // composition made every single-own-geo LI_ — e.g. the seeded LI_
        // template of a fresh project — resolve to a non-existent mesh,
        // so WP expansion placed 0 actors ("it won't show up").
        FString SanitizedGeoName = Geo.Name.Replace(TEXT("."), TEXT("_"));
        FString MeshAssetName = (Report.OwnGeo.Num() == 1)
            ? BlendName
            : FString::Printf(TEXT("%s_%s"), *BlendName, *SanitizedGeoName);
        FString MeshPath = FString::Printf(
            TEXT("/Game/MapLink/%s/Meshes/%s"), *BlendName, *MeshAssetName);

        UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *MeshPath);
        if (!Mesh)
        {
            // Robustness: try the other convention before giving up
            // (assets imported by other plugin versions / edge cases).
            const FString AltName = (Report.OwnGeo.Num() == 1)
                ? FString::Printf(TEXT("%s_%s"), *BlendName, *SanitizedGeoName)
                : BlendName;
            const FString AltPath = FString::Printf(
                TEXT("/Game/MapLink/%s/Meshes/%s"), *BlendName, *AltName);
            Mesh = LoadObject<UStaticMesh>(nullptr, *AltPath);
            if (Mesh)
            {
                MeshPath = AltPath;
            }
        }
        if (Mesh)
        {
            FActorSpawnParameters SpawnParams;
            SpawnParams.SpawnCollisionHandlingOverride =
                ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

            AStaticMeshActor* SMActor = TargetWorld->SpawnActor<AStaticMeshActor>(
                AStaticMeshActor::StaticClass(),
                ComposedTransform, SpawnParams);
            if (SMActor)
            {
                SMActor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
                SMActor->SetActorScale3D(ComposedTransform.GetScale3D());
                SMActor->SetActorLabel(FString::Printf(
                    TEXT("%s_%s"), *BlendName, *Geo.Name));

                TagActor(SMActor, ChildComposition, BlendName,
                         OutPlacedCount, GeoFolder,
                         ParentTransform.GetLocation());
                ++OutPlacedCount;
            }
        }
        else
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("WP: Own-geo mesh not found at '%s' for '%s'."),
                   *MeshPath, *BlendName);
        }
    }

    // 2. Linked assets → dispatch by prefix
    for (const FMapLinkLinkedAssetEntry& Linked : Report.LinkedAssets)
    {
        FString ChildName = FPaths::GetBaseFilename(Linked.SourceBlendPath);
        FTransform ChildTransform = Linked.WorldTransform * ParentTransform;

        // v1.13.1 — nest this instance under the artist's Blender collection
        // chain (e.g. FolderPath/Layer1); the per-asset subfolder lands below.
        // v1.14 — gated by the Folder Creation Mode (None → flat).
        const FString EntryFolder =
            ComposeFolder(FolderPath, Linked.CollectionPath);

        if (ChildName.StartsWith(TEXT("LI_")))
        {
            // v1.9 — broken-open child LI_ gets its own subfolder.
            // v1.14 — at the depth boundary the LI_ stays an instanced
            // collection: it only gets a PER-INSTANCE folder (named after
            // the Blender instance object, e.g. "LI_uneplace.002") in
            // "Collection + instanced collection = folders" mode.
            const FString ChildFolder = ComposeFolder(EntryFolder, ChildName);
            const bool bBoundary = (CurrentDepth + 1 >= MaxExpandDepth);

            // Recurse deeper (or place as LevelInstance at boundary)
            ExpandLIRecursive(
                ChildName,
                ChildTransform,
                ChildComposition,
                bBoundary ? ComposeInstanceFolder(EntryFolder, Linked.Name)
                          : ChildFolder,
                CurrentDepth + 1,
                MaxExpandDepth,
                TargetWorld,
                VisitedSet,
                OutPlacedCount);
        }
        else if (ChildName.StartsWith(TEXT("SM_")))
        {
            PlaceSMInWP(ChildName, ChildTransform, ChildComposition,
                        EntryFolder, TargetWorld, OutPlacedCount,
                        /*InstanceName=*/Linked.Name);
        }
        else if (ChildName.StartsWith(TEXT("BP_")))
        {
            PlaceBPInWP(ChildName, ChildTransform, ChildComposition,
                        EntryFolder, TargetWorld, OutPlacedCount,
                        /*InstanceName=*/Linked.Name);
        }
        else
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("WP: Unknown prefix for child '%s' — skipping."),
                   *ChildName);
        }
    }

    VisitedSet.Remove(BlendName);
}

// ---------------------------------------------------------------------------
// Target world resolution
// ---------------------------------------------------------------------------

UWorld* FMapLinkWPExpansionHelper::LoadAndOpenTargetWorld(
    const FSoftObjectPath& TargetLevelPath)
{
    if (TargetLevelPath.IsNull())
    {
        UE_LOG(LogMapLink, Error, TEXT("WP: No target level set."));
        return nullptr;
    }

    UWorld* TargetWorld = Cast<UWorld>(TargetLevelPath.TryLoad());
    if (!TargetWorld)
    {
        // Try loading via map path (strip the asset name portion)
        FString WPLevelPath = TargetLevelPath.ToString();
        int32 DotIdx;
        if (WPLevelPath.FindChar(TEXT('.'), DotIdx))
        {
            WPLevelPath = WPLevelPath.Left(DotIdx);
        }
        TargetWorld = LoadObject<UWorld>(nullptr, *WPLevelPath);
    }

    if (!TargetWorld)
    {
        UE_LOG(LogMapLink, Error,
               TEXT("WP: Failed to load target level '%s'."),
               *TargetLevelPath.ToString());
        return nullptr;
    }

    // Open in the editor if it's not the current level
    UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (EditorWorld != TargetWorld)
    {
        FString MapPath = TargetWorld->GetOutermost()->GetName();
        UE_LOG(LogMapLink, Log,
               TEXT("WP: Loading target level '%s'."), *MapPath);
        FEditorFileUtils::LoadMap(MapPath);
        TargetWorld = GEditor->GetEditorWorldContext().World();
        if (!TargetWorld)
        {
            UE_LOG(LogMapLink, Error,
                   TEXT("WP: Failed to load target level into editor."));
            return nullptr;
        }
    }

    return TargetWorld;
}

// ---------------------------------------------------------------------------
// Public entry point: ExpandIntoWP (v1.9 — window-driven)
// ---------------------------------------------------------------------------

EMapLinkResult FMapLinkWPExpansionHelper::ExpandIntoWP(
    const FString& RootBlendName,
    const FSoftObjectPath& TargetLevelPath,
    int32 ExpandDepth)
{
    // v1.17 — thin wrapper: find-or-create the record for this (root, level)
    // pair and expand it.  Note the pair, not the root: re-expanding a root
    // into a DIFFERENT level now creates a second record instead of silently
    // overwriting the first and stranding its actors.
    UMapLinkEditorState* State = UMapLinkEditorState::Get();

    FMapLinkWPExpansionRecord Record;
    if (const FMapLinkWPExpansionRecord* Existing =
            State->FindExpansion(RootBlendName, TargetLevelPath))
    {
        Record = *Existing;
    }
    else
    {
        Record.RootBlendName = RootBlendName;
        Record.TargetLevel   = TargetLevelPath;
    }
    Record.ExpandDepth = ExpandDepth;

    return ExpandRecord(Record);
}

EMapLinkResult FMapLinkWPExpansionHelper::ExpandRecord(
    FMapLinkWPExpansionRecord& InOutRecord,
    bool bSaveAndReopen)
{
    const FString RootBlendName = InOutRecord.RootBlendName;

    if (RootBlendName.IsEmpty() || !RootBlendName.StartsWith(TEXT("LI_")))
    {
        UE_LOG(LogMapLink, Error,
               TEXT("WP Expand: '%s' is not an LI_ root."), *RootBlendName);
        return EMapLinkResult::AssetNotFound;
    }

    const int32 Depth = FMath::Clamp(InOutRecord.ExpandDepth, 1, 10);
    InOutRecord.ExpandDepth = Depth;

    // Identity first: the Id is written into every actor tag, so it has to
    // exist before a single actor is placed.
    if (!InOutRecord.Id.IsValid())
    {
        InOutRecord.Id = FGuid::NewGuid();
    }

    const FSoftObjectPath TargetLevelPath = InOutRecord.TargetLevel;

    UE_LOG(LogMapLink, Log,
           TEXT("=== WP Expansion: %s [%s] → %s (depth=%d) ==="),
           *RootBlendName, *InOutRecord.GetIdTag(),
           *TargetLevelPath.ToString(), Depth);

    UWorld* TargetWorld = LoadAndOpenTargetWorld(TargetLevelPath);
    if (!TargetWorld)
    {
        return EMapLinkResult::TempLevelMissing;
    }

    // Verify World Partition is enabled.  The UI now filters the picker to WP
    // levels (IsWorldPartitionLevel), so reaching this is a scripted call or a
    // level that lost WP after the record was made — still worth the guard.
    if (!TargetWorld->GetWorldPartition())
    {
        UE_LOG(LogMapLink, Error,
               TEXT("WP Expand: Target level '%s' does not have World Partition enabled."),
               *TargetLevelPath.ToString());
        return EMapLinkResult::TempLevelMissing;
    }

    // Clean up THIS record's previous placement (actors + folders).  Matched
    // by Id, so a sibling expansion of the same root in the same level is
    // left alone — which v1.16's root-wide sweep could not do.
    CleanupWPForExpansion(TargetWorld, InOutRecord);

    // Fresh memo per run: within the run a folder is created once, across runs
    // it is re-verified, so a folder deleted by hand still comes back.
    ResetEnsuredFolderCache();

    // Run recursive expansion
    FScopedSlowTask ExpandTask(1.f,
        FText::FromString(FString::Printf(
            TEXT("MapLink: Expanding %s into WP"), *InOutRecord.GetLabel())));
    ExpandTask.MakeDialog(false);

    // v1.9 — main parent folder for the whole expansion.
    // v1.17 — named after the record label (defaults to the root stem, so
    // single-expansion projects see exactly the v1.16 folder).
    const FString RootFolder = InOutRecord.GetFolderPath();

    // Create the parent folder up-front so the hierarchy exists even before
    // the first actor lands (and so an empty expansion still shows the node).
    EnsureFolderHierarchy(TargetWorld, RootFolder);

    TSet<FString> VisitedSet;
    int32 PlacedCount = 0;

    {
        // Every TagActor call inside this scope stamps the record Id.
        FScopedExpansionId IdScope(InOutRecord.GetIdTag());

        ExpandLIRecursive(
            RootBlendName,
            FTransform::Identity,
            RootBlendName,
            RootFolder,
            0,
            Depth,
            TargetWorld,
            VisitedSet,
            PlacedCount);
    }

    ExpandTask.EnterProgressFrame(1.f);

    // Record the expansion so reimports re-expand it
    InOutRecord.LastPlacedCount = PlacedCount;
    InOutRecord.LastExpandedUtc = FDateTime::UtcNow();
    UMapLinkEditorState::Get()->AddOrUpdateExpansion(InOutRecord);

    // v1.13.1 — remove stale/empty "MapLink/" outliner folders (orphans left
    // by expansion roots that no longer place anything, e.g. LI_projet_test)
    // before saving, so the persisted level is clean.
    PruneEmptyMapLinkFolders(TargetWorld);

    // Write expansion log
    WriteExpansionLog(RootBlendName, PlacedCount);

    if (bSaveAndReopen)
    {
        // Save the level
        FEditorFileUtils::SaveCurrentLevel();

        // v1.9 — Reopen the level so the WP outliner rebuilds and the new
        // MapLink/<Root>/ folder tree is visible.  v1.15.2 — extracted to
        // ReopenEditorLevelForOutliner so EVERY expansion-related action
        // (Expand, Remove, re-expand) ends with the same clean-outliner
        // reopen the user otherwise performs by hand.
        // v1.17 — a batch caller (ReExpandIfNeeded) passes false and does
        // this ONCE per level instead of once per record.
        ReopenEditorLevelForOutliner();
    }

    UE_LOG(LogMapLink, Log,
           TEXT("=== WP Expansion Complete: %s — %d actors placed ==="),
           *RootBlendName, PlacedCount);

    return EMapLinkResult::Success;
}

// ---------------------------------------------------------------------------
// v1.18 — reading expansions out of a level
// ---------------------------------------------------------------------------

namespace
{
    /** "MapLink/Foo/Bar" -> "Foo"; "" for anything not under MapLink/. */
    FString TopMapLinkFolderSegment(const FString& FolderPath)
    {
        static const FString Root = TEXT("MapLink");
        if (FolderPath == Root)
        {
            return FString();
        }
        if (!FolderPath.StartsWith(Root + TEXT("/")))
        {
            return FString();
        }
        FString Rest = FolderPath.RightChop(Root.Len() + 1);
        int32 Slash = INDEX_NONE;
        if (Rest.FindChar(TEXT('/'), Slash))
        {
            Rest = Rest.Left(Slash);
        }
        return Rest;
    }
}

TArray<FMapLinkDiscoveredExpansion>
FMapLinkWPExpansionHelper::ScanExpansionsInWorld(UWorld* World)
{
    TArray<FMapLinkDiscoveredExpansion> Out;
    if (!World) return Out;

    // Key: the tag id when present, else "folder:<label>" so pre-v1.17
    // expansions still group into exactly one entry.
    TMap<FString, int32> IndexByKey;
    TMap<int32, TSet<FString>> CompositionsByIndex;

    for (TActorIterator<AActor> It(World); It; ++It)
    {
        const FString TagStr = FindExpansionTag(*It);
        if (TagStr.IsEmpty())
        {
            continue;
        }

        TArray<FString> Parts;
        TagStr.ParseIntoArray(Parts, TEXT(":"), /*CullEmpty=*/false);

        const FString ActorIdTag = ParseExpansionIdTag(TagStr);
        const FString Label      =
            TopMapLinkFolderSegment(It->GetFolderPath().ToString());

        const FString Key = !ActorIdTag.IsEmpty()
            ? ActorIdTag
            : (TEXT("folder:") + Label);

        int32* Existing = IndexByKey.Find(Key);
        if (!Existing)
        {
            FMapLinkDiscoveredExpansion New;
            New.IdTag = ActorIdTag;
            New.Label = Label;
            const int32 NewIndex = Out.Add(New);
            IndexByKey.Add(Key, NewIndex);
            Existing = IndexByKey.Find(Key);
        }

        FMapLinkDiscoveredExpansion& Entry = Out[*Existing];
        ++Entry.ActorCount;
        if (Entry.Label.IsEmpty() && !Label.IsEmpty())
        {
            Entry.Label = Label;
        }
        if (Parts.Num() >= 2 && !Parts[1].IsEmpty())
        {
            CompositionsByIndex.FindOrAdd(*Existing).Add(Parts[1]);
        }
    }

    // Root guess. The folder is named after the LABEL, and the label defaults
    // to the root stem — so if a composition matches the label, that is the
    // root. Otherwise fall back to the shortest composition: the root is an
    // ancestor of every other node in its own expansion, and ancestors are
    // reached first, but nothing in the tag records depth, so this stays a
    // suggestion the user confirms.
    for (int32 Index = 0; Index < Out.Num(); ++Index)
    {
        const TSet<FString>* Comps = CompositionsByIndex.Find(Index);
        if (!Comps || Comps->Num() == 0)
        {
            Out[Index].RootGuess = Out[Index].Label;
            continue;
        }
        if (Comps->Contains(Out[Index].Label))
        {
            Out[Index].RootGuess = Out[Index].Label;
            continue;
        }
        FString Shortest;
        for (const FString& Comp : *Comps)
        {
            if (Shortest.IsEmpty() || Comp.Len() < Shortest.Len())
            {
                Shortest = Comp;
            }
        }
        Out[Index].RootGuess = Shortest;
    }

    // Folders that exist but whose actors are all streamed out.  Without this
    // a fully unloaded expansion reads as "not there", which is the one
    // answer that is actively misleading.
    if (UWorldPartition* WorldPartition = World->GetWorldPartition())
    {
        TSet<FString> KnownLabels;
        for (const FMapLinkDiscoveredExpansion& Entry : Out)
        {
            KnownLabels.Add(Entry.Label);
        }

        TSet<FString> DescLabels;
        FWorldPartitionHelpers::ForEachActorDescInstance(WorldPartition,
            [&DescLabels](const FWorldPartitionActorDescInstance* DescInstance) -> bool
            {
                if (DescInstance && DescInstance->GetActorDesc())
                {
                    const FString Seg = TopMapLinkFolderSegment(
                        DescInstance->GetActorDesc()->GetFolderPath().ToString());
                    if (!Seg.IsEmpty())
                    {
                        DescLabels.Add(Seg);
                    }
                }
                return true;
            });

        for (const FString& Label : DescLabels)
        {
            if (!KnownLabels.Contains(Label))
            {
                FMapLinkDiscoveredExpansion Streamed;
                Streamed.Label        = Label;
                Streamed.RootGuess    = Label;
                Streamed.bStreamedOut = true;
                Out.Add(Streamed);
            }
        }
    }

    return Out;
}

int32 FMapLinkWPExpansionHelper::RemoveDiscoveredExpansion(
    UWorld* World,
    const FMapLinkDiscoveredExpansion& Discovered)
{
    if (!World) return 0;

    const FString TopFolder =
        FString::Printf(TEXT("MapLink/%s"), *Discovered.Label);

    TArray<AActor*> ToRemove;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;

        const FString TagStr = FindExpansionTag(Actor);
        if (TagStr.IsEmpty())
        {
            continue;
        }

        bool bMine = false;
        if (!Discovered.IdTag.IsEmpty())
        {
            bMine = (ParseExpansionIdTag(TagStr) == Discovered.IdTag);
        }
        else
        {
            // Pre-v1.17 actors: the folder is all the identity there is.
            const FString ActorFolder = Actor->GetFolderPath().ToString();
            bMine = ParseExpansionIdTag(TagStr).IsEmpty() &&
                    (ActorFolder == TopFolder ||
                     ActorFolder.StartsWith(TopFolder + TEXT("/")));
        }

        if (bMine)
        {
            ToRemove.Add(Actor);
        }
    }

    for (AActor* Actor : ToRemove)
    {
        World->DestroyActor(Actor);
    }

    DeleteFolderTree(World, TopFolder);
    PruneEmptyMapLinkFolders(World);

    UE_LOG(LogMapLink, Log,
           TEXT("WP Cleanup: removed %d actors for discovered expansion "
                "'%s' (%s)."),
           ToRemove.Num(), *Discovered.Label,
           Discovered.IdTag.IsEmpty() ? TEXT("legacy") : *Discovered.IdTag);

    return ToRemove.Num();
}

EMapLinkResult FMapLinkWPExpansionHelper::AdoptDiscoveredExpansion(
    UWorld* World,
    const FMapLinkDiscoveredExpansion& Discovered,
    const FString& RootBlendName,
    int32 ExpandDepth,
    const FSoftObjectPath& TargetLevelPath)
{
    if (!World || RootBlendName.IsEmpty())
    {
        return EMapLinkResult::AssetNotFound;
    }
    if (Discovered.bStreamedOut)
    {
        // Nothing loaded to re-tag. Adopting now would produce a record whose
        // Id matches no actor — exactly the broken state this feature exists
        // to clean up.
        UE_LOG(LogMapLink, Warning,
               TEXT("WP Adopt: '%s' is streamed out — load the cells (or open "
                    "the level fully) before adopting."), *Discovered.Label);
        return EMapLinkResult::TempLevelMissing;
    }

    FMapLinkWPExpansionRecord Record;
    Record.Id                = FGuid::NewGuid();
    Record.RootBlendName     = RootBlendName;
    Record.TargetLevel       = TargetLevelPath;
    Record.ExpandDepth       = FMath::Clamp(ExpandDepth, 1, 10);
    Record.Label             = Discovered.Label;
    Record.bReExpandOnImport = true;
    Record.LastExpandedUtc   = FDateTime::UtcNow();

    const FString NewIdTag  = Record.GetIdTag();
    const FString TopFolder = Record.GetFolderPath();

    // Re-tag: append the new id to tags that have none, replace it on tags
    // that do. This is what makes the record and the actors agree.
    int32 Retagged = 0;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;

        const FString TagStr = FindExpansionTag(Actor);
        if (TagStr.IsEmpty())
        {
            continue;
        }

        const FString ActorIdTag = ParseExpansionIdTag(TagStr);

        bool bMine = false;
        if (!Discovered.IdTag.IsEmpty())
        {
            bMine = (ActorIdTag == Discovered.IdTag);
        }
        else
        {
            const FString ActorFolder = Actor->GetFolderPath().ToString();
            bMine = ActorIdTag.IsEmpty() &&
                    (ActorFolder == TopFolder ||
                     ActorFolder.StartsWith(TopFolder + TEXT("/")));
        }
        if (!bMine)
        {
            continue;
        }

        TArray<FString> Parts;
        TagStr.ParseIntoArray(Parts, TEXT(":"), /*CullEmpty=*/false);

        FString NewTag;
        if (Parts.Num() >= 6)
        {
            Parts[5] = NewIdTag;
            NewTag   = FString::Join(Parts, TEXT(":"));
        }
        else
        {
            NewTag = TagStr + TEXT(":") + NewIdTag;
        }

        Actor->Modify();
        Actor->Tags.Remove(FName(*TagStr));
        Actor->Tags.Add(FName(*NewTag));
        Actor->MarkPackageDirty();
        ++Retagged;
    }

    Record.LastPlacedCount = Retagged;
    UMapLinkEditorState::Get()->AddOrUpdateExpansion(Record);

    FEditorFileUtils::SaveCurrentLevel();

    UE_LOG(LogMapLink, Log,
           TEXT("WP Adopt: '%s' -> record %s (root '%s'), %d actor(s) "
                "re-tagged."),
           *Discovered.Label, *NewIdTag, *RootBlendName, Retagged);

    MapLinkNotify::Success(FText::FromString(FString::Printf(
        TEXT("Adopted '%s' — %d actor(s) now tracked."),
        *Record.GetLabel(), Retagged)));

    return EMapLinkResult::Success;
}

// ---------------------------------------------------------------------------
// v1.17 — dry run + WP validation
// ---------------------------------------------------------------------------

namespace
{
    /** Mirror of ExpandLIRecursive that counts instead of spawning.
     *  Kept deliberately adjacent to it: if the placement rules change, both
     *  change, and a preview that lies is worse than no preview. */
    void PreviewLIRecursive(
        const FString& BlendName,
        int32 CurrentDepth,
        int32 MaxExpandDepth,
        TSet<FString>& VisitedSet,
        FMapLinkExpansionPreview& Out)
    {
        Out.MaxDepth = FMath::Max(Out.MaxDepth, CurrentDepth + 1);

        if (CurrentDepth >= MaxExpandDepth)
        {
            ++Out.NumLI;              // placed as ALevelInstance at boundary
            return;
        }

        if (VisitedSet.Contains(BlendName))
        {
            return;                   // cycle — ExpandLIRecursive aborts here too
        }
        VisitedSet.Add(BlendName);

        const FString AnalysisPath = FMapLinkUtils::ResolveArtifactLogFile(
            BlendName / TEXT("analysis.json"));

        FMapLinkAnalysisReport Report;
        if (!FMapLinkInterchangeHelper::ParseAnalysisJson(AnalysisPath, Report))
        {
            // Same fallback as the real walk: place as a LevelInstance.
            ++Out.NumLI;
            Out.bIncomplete = true;
            VisitedSet.Remove(BlendName);
            return;
        }

        ++Out.NumBrokenLI;
        Out.NumSM += Report.OwnGeo.Num();

        for (const FMapLinkLinkedAssetEntry& Linked : Report.LinkedAssets)
        {
            const FString ChildName =
                FPaths::GetBaseFilename(Linked.SourceBlendPath);

            if (ChildName.StartsWith(TEXT("LI_")))
            {
                PreviewLIRecursive(ChildName, CurrentDepth + 1,
                                   MaxExpandDepth, VisitedSet, Out);
            }
            else if (ChildName.StartsWith(TEXT("SM_")))
            {
                ++Out.NumSM;
            }
            else if (ChildName.StartsWith(TEXT("BP_")))
            {
                ++Out.NumBP;
            }
        }

        VisitedSet.Remove(BlendName);
    }
}

FMapLinkExpansionPreview FMapLinkWPExpansionHelper::PreviewExpansion(
    const FString& RootBlendName,
    int32 ExpandDepth)
{
    FMapLinkExpansionPreview Preview;

    if (RootBlendName.IsEmpty() || !RootBlendName.StartsWith(TEXT("LI_")))
    {
        return Preview;
    }

    // Counts at the requested depth.
    {
        TSet<FString> Visited;
        PreviewLIRecursive(RootBlendName, 0, FMath::Clamp(ExpandDepth, 1, 10),
                           Visited, Preview);
    }

    // True hierarchy depth, so the UI can say "Depth 2 of 4" and offer a
    // meaningful Full.  A second walk at max depth; both are pure JSON reads.
    {
        FMapLinkExpansionPreview DepthProbe;
        TSet<FString> Visited;
        PreviewLIRecursive(RootBlendName, 0, 10, Visited, DepthProbe);
        // MaxDepth counts LI_ NESTING LEVELS, which is exactly what the depth
        // spinner means: depth N breaks open N levels of LI_.  A root with no
        // nested LI_ is depth 1.
        Preview.MaxDepth = FMath::Max(1, DepthProbe.MaxDepth);
    }

    return Preview;
}

bool FMapLinkWPExpansionHelper::IsWorldPartitionLevel(
    const FSoftObjectPath& LevelPath)
{
    if (LevelPath.IsNull())
    {
        return false;
    }

    // Already loaded (typically the open level) — ask it directly.
    if (const UWorld* Loaded = Cast<UWorld>(LevelPath.ResolveObject()))
    {
        return Loaded->GetWorldPartition() != nullptr;
    }

    // Otherwise the asset registry knows without loading the map.  UWorld
    // exposes a "LevelIsPartitioned" tag; absence of the tag is treated as
    // "unknown", and unknown must not block the user — ExpandRecord still
    // guards after the load.
    FAssetRegistryModule& ARModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    const FAssetData AssetData =
        ARModule.Get().GetAssetByObjectPath(LevelPath);

    if (!AssetData.IsValid())
    {
        return false;
    }

    FString TagValue;
    if (AssetData.GetTagValue(TEXT("LevelIsPartitioned"), TagValue))
    {
        return TagValue == TEXT("1") || TagValue.ToBool();
    }

    return true;   // unknown — let the post-load guard decide
}

// ---------------------------------------------------------------------------
// Public entry point: CleanupWP (v1.9)
// ---------------------------------------------------------------------------

EMapLinkResult FMapLinkWPExpansionHelper::CleanupWP(
    const FString& RootBlendName,
    const FSoftObjectPath& TargetLevelPath)
{
    UWorld* TargetWorld = LoadAndOpenTargetWorld(TargetLevelPath);
    if (!TargetWorld)
    {
        return EMapLinkResult::TempLevelMissing;
    }

    const int32 Removed = CleanupWPForRoot(TargetWorld, RootBlendName);

    // Remove the records that targeted THIS level — the level is genuinely
    // clean and stays clean: reimports will no longer re-expand it here.
    // v1.17: records pointing at other levels survive, because their actors
    // do too.  (v1.16 dropped every record for the root regardless.)
    UMapLinkEditorState* State = UMapLinkEditorState::Get();
    TArray<FGuid> ToDrop;
    for (const FMapLinkWPExpansionRecord& Rec : State->ActiveExpansions)
    {
        if (Rec.RootBlendName == RootBlendName &&
            Rec.TargetLevel == TargetLevelPath)
        {
            ToDrop.Add(Rec.Id);
        }
    }
    for (const FGuid& Id : ToDrop)
    {
        State->RemoveExpansionById(Id);
    }

    FEditorFileUtils::SaveCurrentLevel();

    // v1.15.2 — same clean-outliner reopen as Expand: removal deletes
    // actors and empties folders, and the WP outliner only truly resyncs
    // on a reopen (the manual step the user was stuck doing every time).
    ReopenEditorLevelForOutliner();

    UE_LOG(LogMapLink, Log,
           TEXT("=== WP Cleanup Complete: %s — %d actors removed ==="),
           *RootBlendName, Removed);

    return EMapLinkResult::Success;
}

// ---------------------------------------------------------------------------
// v1.17 — cleanup by record / by level
// ---------------------------------------------------------------------------

EMapLinkResult FMapLinkWPExpansionHelper::CleanupExpansion(
    const FGuid& ExpansionId)
{
    UMapLinkEditorState* State = UMapLinkEditorState::Get();

    const FMapLinkWPExpansionRecord* Found = State->FindExpansionById(ExpansionId);
    if (!Found)
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("WP Cleanup: no expansion record for id %s."),
               *ExpansionId.ToString());
        return EMapLinkResult::AssetNotFound;
    }

    // Copy: the state array is mutated below.
    const FMapLinkWPExpansionRecord Record = *Found;

    UWorld* TargetWorld = LoadAndOpenTargetWorld(Record.TargetLevel);
    if (!TargetWorld)
    {
        return EMapLinkResult::TempLevelMissing;
    }

    const int32 Removed = CleanupWPForExpansion(TargetWorld, Record);

    State->RemoveExpansionById(ExpansionId);

    FEditorFileUtils::SaveCurrentLevel();
    ReopenEditorLevelForOutliner();

    UE_LOG(LogMapLink, Log,
           TEXT("=== WP Cleanup Complete: %s — %d actors removed ==="),
           *Record.GetLabel(), Removed);

    MapLinkNotify::Success(FText::FromString(FString::Printf(
        TEXT("Removed expansion '%s' from %s — %d actor(s)."),
        *Record.GetLabel(), *Record.GetLevelName(), Removed)));

    return EMapLinkResult::Success;
}

EMapLinkResult FMapLinkWPExpansionHelper::CleanupAllExpansionsInLevel(
    const FSoftObjectPath& TargetLevelPath)
{
    UWorld* TargetWorld = LoadAndOpenTargetWorld(TargetLevelPath);
    if (!TargetWorld)
    {
        return EMapLinkResult::TempLevelMissing;
    }

    const int32 Removed = CleanupAllWPExpansion(TargetWorld);

    // Drop every record that targeted this level.
    UMapLinkEditorState* State = UMapLinkEditorState::Get();
    TArray<FGuid> ToDrop;
    for (const FMapLinkWPExpansionRecord& Rec : State->ActiveExpansions)
    {
        if (Rec.TargetLevel == TargetLevelPath)
        {
            ToDrop.Add(Rec.Id);
        }
    }
    for (const FGuid& Id : ToDrop)
    {
        State->RemoveExpansionById(Id);
    }

    FEditorFileUtils::SaveCurrentLevel();
    ReopenEditorLevelForOutliner();

    MapLinkNotify::Success(FText::FromString(FString::Printf(
        TEXT("Removed all MapLink expansions from %s — %d actor(s), "
             "%d record(s)."),
        *FPaths::GetBaseFilename(TargetLevelPath.ToString()),
        Removed, ToDrop.Num())));

    return EMapLinkResult::Success;
}

// ---------------------------------------------------------------------------
// Reimport integration (v1.9 — record-based; v1.17 — grouped per level)
// ---------------------------------------------------------------------------

void FMapLinkWPExpansionHelper::ReExpandIfNeeded(
    const TArray<FString>& ChangedBlendNames)
{
    if (ChangedBlendNames.Num() == 0) return;

    UMapLinkEditorState* State = UMapLinkEditorState::Get();
    if (State->ActiveExpansions.Num() == 0) return;

    // Copy — ExpandRecord touches the state object.
    TArray<FMapLinkWPExpansionRecord> Records = State->ActiveExpansions;

    // Normalise the changed set once instead of per record.
    TSet<FString> ChangedStems;
    for (const FString& Changed : ChangedBlendNames)
    {
        ChangedStems.Add(FPaths::GetBaseFilename(Changed));
    }

    // v1.17 — bucket the affected records BY TARGET LEVEL.  v1.16 called
    // ExpandIntoWP per record, and each call opened, saved and reopened its
    // level: three expansions in one level meant three level reloads, and
    // two levels meant the editor bounced back and forth.  Now each level is
    // opened once, every record in it is expanded, and the save + outliner
    // reopen happen once at the end of that level's batch.
    TMap<FSoftObjectPath, TArray<FMapLinkWPExpansionRecord>> ByLevel;

    for (const FMapLinkWPExpansionRecord& Rec : Records)
    {
        if (!Rec.IsValidRecord())
        {
            continue;
        }
        if (!Rec.bReExpandOnImport)
        {
            UE_LOG(LogMapLink, Verbose,
                   TEXT("WP: '%s' is paused — skipping re-expansion."),
                   *Rec.GetLabel());
            continue;
        }

        TSet<FString> DescendantNames;
        CollectDescendantNames(Rec.RootBlendName, DescendantNames);

        bool bAffected = false;
        for (const FString& Stem : ChangedStems)
        {
            if (DescendantNames.Contains(Stem))
            {
                bAffected = true;
                break;
            }
        }

        if (bAffected)
        {
            ByLevel.FindOrAdd(Rec.TargetLevel).Add(Rec);
        }
    }

    if (ByLevel.Num() == 0) return;

    // Remember where the user was, so a re-expansion into other levels does
    // not silently leave them somewhere else.
    const UWorld* StartWorld =
        GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    const FSoftObjectPath StartLevel = StartWorld
        ? FSoftObjectPath(StartWorld->GetPathName())
        : FSoftObjectPath();

    int32 TotalRecords = 0;
    int32 TotalActors  = 0;

    for (TPair<FSoftObjectPath, TArray<FMapLinkWPExpansionRecord>>& Pair : ByLevel)
    {
        const int32 Count = Pair.Value.Num();
        for (int32 Index = 0; Index < Count; ++Index)
        {
            FMapLinkWPExpansionRecord& Rec = Pair.Value[Index];
            const bool bLast = (Index == Count - 1);

            UE_LOG(LogMapLink, Log,
                   TEXT("WP: Re-expanding '%s' in %s due to changed dependency."),
                   *Rec.GetLabel(), *Rec.GetLevelName());

            // Only the last record in a level pays for the save + reopen.
            if (ExpandRecord(Rec, /*bSaveAndReopen=*/bLast) ==
                EMapLinkResult::Success)
            {
                ++TotalRecords;
                TotalActors += Rec.LastPlacedCount;
            }
        }
    }

    // Return the user to the level they started in.
    if (!StartLevel.IsNull())
    {
        const UWorld* NowWorld =
            GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
        const FSoftObjectPath NowLevel = NowWorld
            ? FSoftObjectPath(NowWorld->GetPathName())
            : FSoftObjectPath();

        if (NowLevel != StartLevel)
        {
            UE_LOG(LogMapLink, Log,
                   TEXT("WP: Returning to '%s' after re-expansion."),
                   *StartLevel.ToString());
            LoadAndOpenTargetWorld(StartLevel);
        }
    }

    if (TotalRecords > 0)
    {
        MapLinkNotify::Success(FText::FromString(FString::Printf(
            TEXT("Re-expanded %d expansion(s) across %d level(s) — "
                 "%d actor(s) placed."),
            TotalRecords, ByLevel.Num(), TotalActors)));
    }
}

// ---------------------------------------------------------------------------
// v1.14 — In-place post-reimport outliner repair (no level reopen)
// ---------------------------------------------------------------------------

void FMapLinkWPExpansionHelper::RefreshEditorWorldAfterReimport(
    const TArray<FString>& ChangedBlendNames)
{
    if (!GEditor)
    {
        return;
    }
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World)
    {
        return;
    }

    TSet<FString> Changed;
    for (const FString& Name : ChangedBlendNames)
    {
        Changed.Add(FPaths::GetBaseFilename(Name));
    }

    // 1. Force-reload every loaded Level Instance whose /Game/MapLink/
    //    target level was nuked-and-recreated.  The instance actor still
    //    references the destroyed in-memory world — that is what showed up
    //    as "unloaded Level Instance" rows until a manual reopen.
    ULevelInstanceSubsystem* LISubsystem =
        World->GetSubsystem<ULevelInstanceSubsystem>();
    TArray<ALevelInstance*> ToReload;
    for (TActorIterator<ALevelInstance> It(World); It; ++It)
    {
        const FString Pkg =
            It->GetWorldAsset().ToSoftObjectPath().GetLongPackageName();
        if (!Pkg.StartsWith(TEXT("/Game/MapLink/")))
        {
            continue;
        }
        const FString Stem = FPackageName::GetShortName(Pkg);
        if (Changed.Num() == 0 || Changed.Contains(Stem))
        {
            ToReload.Add(*It);
        }
    }

    int32 Reloaded = 0;
    if (LISubsystem && ToReload.Num() > 0)
    {
        for (ALevelInstance* LI : ToReload)
        {
            if (LISubsystem->IsLoaded(LI))
            {
                LISubsystem->RequestUnloadLevelInstance(LI);
            }
        }

        // Release the destroyed worlds' packages before reloading fresh
        // from disk — the same reason the old close→reopen needed a GC
        // between the blank map and the LoadMap: without it the reload
        // re-associates the stale in-memory world.
        FlushAsyncLoading();
        CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

        for (ALevelInstance* LI : ToReload)
        {
            LISubsystem->RequestLoadLevelInstance(LI, /*bForce=*/true);
            ++Reloaded;
        }
    }

    // 2. Phantom folders: the destructive pass deletes actors but folder
    //    registrations survive in FActorFolders — drop every MapLink/
    //    folder that no longer holds an actor.
    PruneEmptyMapLinkFolders(World);

    // 3. Rebuild the outliner + viewports in place.
    if (GEngine)
    {
        GEngine->BroadcastLevelActorListChanged();
    }
    GEditor->RedrawLevelEditingViewports(/*bInvalidateHitProxies=*/true);

    UE_LOG(LogMapLink, Log,
           TEXT("[Reimport] In-place outliner refresh: %d Level Instance(s) "
                "reloaded, empty MapLink folders pruned, actor list "
                "rebroadcast (no level reopen)."), Reloaded);
}

// ---------------------------------------------------------------------------
// v1.9 — Legacy proxy-flag migration
// ---------------------------------------------------------------------------

int32 FMapLinkWPExpansionHelper::MigrateLegacyProxyExpansionFlags()
{
    FAssetRegistryModule& ARModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AR = ARModule.Get();

    FARFilter Filter;
    Filter.ClassPaths.Add(UMapLinkProxyAsset::StaticClass()->GetClassPathName());
    Filter.bRecursivePaths = true;

    TArray<FAssetData> ProxyAssets;
    AR.GetAssets(Filter, ProxyAssets);

    int32 Migrated = 0;
    for (const FAssetData& AssetData : ProxyAssets)
    {
        UMapLinkProxyAsset* Proxy = Cast<UMapLinkProxyAsset>(AssetData.GetAsset());
        if (!Proxy || !Proxy->bExpandIntoWP)
        {
            continue;
        }

        const FString RootBlendName =
            FPaths::GetBaseFilename(Proxy->BlendFilePath);

        if (!Proxy->TargetWPLevel.IsNull() && Proxy->ExpandDepth > 0)
        {
            const FSoftObjectPath LevelPath =
                Proxy->TargetWPLevel.ToSoftObjectPath();

            UMapLinkEditorState* State = UMapLinkEditorState::Get();

            FMapLinkWPExpansionRecord Record;
            if (const FMapLinkWPExpansionRecord* Existing =
                    State->FindExpansion(RootBlendName, LevelPath))
            {
                Record = *Existing;
            }
            else
            {
                Record.RootBlendName = RootBlendName;
                Record.TargetLevel   = LevelPath;
            }
            Record.ExpandDepth = Proxy->ExpandDepth;
            State->AddOrUpdateExpansion(Record);

            UE_LOG(LogMapLink, Log,
                   TEXT("WP Migration: '%s' → expansion record "
                        "(level='%s', depth=%d)."),
                   *RootBlendName,
                   *Proxy->TargetWPLevel.ToString(),
                   Proxy->ExpandDepth);
        }

        // Clear the legacy flag either way so we never migrate twice.
        Proxy->bExpandIntoWP = false;
        Proxy->MarkPackageDirty();
        FMapLinkPipeline::SaveProxyPackage(Proxy);
        ++Migrated;
    }

    if (Migrated > 0)
    {
        UE_LOG(LogMapLink, Log,
               TEXT("WP Migration: %d legacy proxy expansion flag(s) "
                    "converted to MapLink Manager records."), Migrated);
    }
    return Migrated;
}

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------

void FMapLinkWPExpansionHelper::WriteExpansionLog(
    const FString& RootBlendName,
    int32 PlacedCount)
{
    FString PluginBaseDir = FMapLinkPipeline::GetPluginBaseDir();
    FString ExpLogFilePath = FMapLinkUtils::GetArtifactLogDir() / RootBlendName /
        TEXT("wp_expansion.json");

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("root_blend"), RootBlendName);
    Root->SetNumberField(TEXT("placed_actors"), PlacedCount);
    Root->SetStringField(TEXT("timestamp"), FDateTime::UtcNow().ToIso8601());

    FString OutputString;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
    FJsonSerializer::Serialize(Root, Writer);

    IFileManager::Get().MakeDirectory(*FPaths::GetPath(ExpLogFilePath), true);
    FFileHelper::SaveStringToFile(OutputString, *ExpLogFilePath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
