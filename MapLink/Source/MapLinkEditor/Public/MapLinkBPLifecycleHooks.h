// Copyright MapModelers. All Rights Reserved.
//
// v1.8 — BP Reference Mode lifecycle hooks.
//
// Watches UE asset events on UBlueprint assets that participate in
// the BP-Reference-Mode workflow, and keeps proxy state consistent
// when users rename or duplicate bound Blueprints:
//
//   RENAME    → propagate the new path to every UMapLinkProxyAsset
//               whose OriginalBoundBlueprintPath / BoundBlueprint
//               referenced the old path
//   ADD       → duplicate-detection: if a newly-added BP has a binding
//               whose stand-in sidecar's bound_blueprint_path doesn't
//               match THIS BP's path, scrub the binding (it was
//               inherited from the source BP via package duplication
//               and would otherwise corrupt the original's stand-in)
//
// v1.8 D7 — REMOVED the OnAssetsPreDelete / OnAssetRemoved cascade
// hooks.  Stand-in deletion is now handled exclusively by the
// explicit "Delete MapLink Stand-in" menu entry, which consolidates
// everything (binding removal, p4-delete, /Game/MapLink/<name>/
// purge) into one user-initiated action.  If the user deletes a
// bound BP via UE's regular Delete, the stand-in stays on disk —
// harmless; the next reimport with a missing bound BP falls back to
// BP_Deprecated, and the user can cleanly run "Delete MapLink
// Stand-in" before deleting the BP if cleanup is desired.
//
// All hooks are no-ops when:
//   - the asset isn't a UBlueprint
//   - the BP has no UMapLinkBPBinding
//   - the stand-in .blend referenced by the binding doesn't exist

#pragma once

#include "CoreMinimal.h"
#include "Delegates/IDelegateInstance.h"

struct FAssetData;

class FMapLinkBPLifecycleHooks
{
public:
    /** Register all delegates.  Call once at editor module startup. */
    static void Register();

    /** Unregister all delegates.  Call at editor module shutdown. */
    static void Unregister();

private:
    // Cached delegate handles for clean unregistration
    static FDelegateHandle RenamedHandle;
    static FDelegateHandle AddedHandle;

    // Hook implementations
    static void OnAssetRenamed(const FAssetData& Asset, const FString& OldObjectPath);
    static void OnAssetAdded(const FAssetData& Asset);
};
