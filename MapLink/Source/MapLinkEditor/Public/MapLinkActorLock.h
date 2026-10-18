// Copyright MapModelers. All Rights Reserved.
//
// v1.15 — Selection lock for MapLink-placed actors + shared actor identity.
//
// MapLink actors in the level viewport stay clickable/selectable but are
// transform-locked (stock editor actor lock — bLockLocation — no custom
// viewport/gizmo interception).  Hand-moved transforms would be stomped by
// the next nuke-and-recreate reimport anyway, so the lock turns a silent
// data-loss trap into a visible editor affordance.  Delete/rename are NOT
// blocked.  Driven by UMapLinkSettings::bLockMapLinkActorTransforms
// (default ON); toggling the setting sweeps the loaded editor world live.
//
// The identity helper (IsMapLinkActor) is deliberately cheap — pure tag and
// path-prefix checks, no asset loads — and is shared with the v1.15 asset
// name display (MapLinkNameDisplay).

#pragma once

#include "CoreMinimal.h"

class AActor;
class UWorld;

class MAPLINKEDITOR_API FMapLinkActorLock
{
public:
    /** Hook OnMapOpened + OnLevelActorAdded.  Call from StartupModule. */
    static void Register();
    static void Unregister();

    /**
     * Cheap MapLink-actor identity — tags and /Game/MapLink/ path prefixes
     * only, never loads an asset:
     *   • tag "MapLinkPlaceholder" or a tag starting with
     *     "MapLinkExpanded:" / "MapLinkWPExpand:",
     *   • ALevelInstance whose world asset lives under /Game/MapLink/,
     *   • Blueprint-classed actor generated from /Game/MapLink/,
     *   • AStaticMeshActor whose mesh lives under /Game/MapLink/.
     * If OutStem is non-null it receives the asset stem for display
     * (best-effort; may be empty for tag-only matches).
     *
     * v1.15.2 — OutInstanceKey (optional): identifier shared by every
     * actor belonging to the SAME placed asset instance and unique
     * across instances.  Expansion tags carry a per-ACTOR index
     * (…:<idx>), so one broken-open asset spawns N tagged actors — the
     * key is the tag minus that index ("MapLinkWPExpand:<comp>:<child>",
     * "MapLinkExpanded:<stem>").  Path-identified actors (LI/SM/BP) key
     * by the actor itself.  Used by the name display to draw ONE label
     * per instance.
     *
     * v1.15.3 — OutAssetOrigin (optional): the placed asset's origin —
     * the Blender collection-instance transform.  Path-identified
     * actors: their own location (the actor IS the instance).
     * Tag-identified actors: parsed from the tag's trailing "x,y,z"
     * segment (written by TagActor since v1.15.3); LEGACY tags without
     * the segment leave it unset — the name display then skips the
     * label rather than guessing.
     */
    static bool IsMapLinkActor(const AActor* Actor, FString* OutStem = nullptr,
                               FString* OutInstanceKey = nullptr,
                               TOptional<FVector>* OutAssetOrigin = nullptr);

    /** Apply the current setting to every MapLink actor in the loaded
     *  editor world (all levels).  Returns the number of actors touched. */
    static int32 ApplyToEditorWorld();

private:
    static void OnMapOpened(const FString& Filename, bool bAsTemplate);
    static void OnLevelActorAdded(AActor* Actor);

    static FDelegateHandle MapOpenedHandle;
    static FDelegateHandle ActorAddedHandle;
};
