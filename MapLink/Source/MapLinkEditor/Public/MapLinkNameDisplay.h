// Copyright MapModelers. All Rights Reserved.
//
// v1.15 — Asset name display: floating name labels over MapLink actors in
// the level editor viewport, visually in the spirit of a Blueprint vec3
// variable with "Show 3D Widget" enabled (small white text, drop shadow,
// anchored at the actor origin).
//
// Editor-only debug draw registered on the "Editor" show-flag channel —
// nothing is added to levels (no TextRenderComponent: components would
// dirty generated levels, survive into PIE/cooks and violate the clean
// nuke-and-recreate invariant), and game/PIE viewports never draw it.
//
// Driven by UMapLinkSettings: bShowAssetNames (per-user, default OFF),
// MaxNameLabels (nearest-first budget) and MaxNameLabelDistance.
// Actor identity + label stem come from FMapLinkActorLock::IsMapLinkActor.

#pragma once

#include "CoreMinimal.h"

class UCanvas;
class APlayerController;

class MAPLINKEDITOR_API FMapLinkNameDisplay
{
public:
    /** Register the debug-draw delegate.  Call from StartupModule. */
    static void Register();
    static void Unregister();

    /** v1.15.1 — per-type visibility (LI_/SM_/BP_ toggles; unknown stems
     *  follow the master toggle only). */
    static bool ShouldShowStem(const FString& Stem);

    /** v1.15.1 — label color: the stem's TYPE color while up to date,
     *  NameColorOutOfDate when the proxy's .blend is newer than its last
     *  import (same mtime heuristic as the Manager, cached ~5 s per
     *  stem).  Also used by the Asset Viewer overlay. */
    static FLinearColor GetLabelColor(const FString& Stem);

    /** v1.15.4 — child-asset variant: the same color, whitened out to
     *  read as "child" (used when Show Children is ON). */
    static FLinearColor GetChildLabelColor(const FString& Stem);

    /** Cheap cached out-of-date check (mtime vs ImportedBlendModTime).
     *
     *  v1.16 — public: the toolbar's Manager status light asks the same
     *  question the name overlay does, and it must get the same answer from
     *  the same cache. A second implementation would drift. Pure query, no
     *  side effects beyond its own ~5s memo. */
    static bool IsStemOutOfDate(const FString& Stem);

private:
    static void Draw(UCanvas* Canvas, APlayerController* PC);

    static FDelegateHandle DrawHandle;
};
