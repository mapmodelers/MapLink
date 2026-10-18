// Copyright MapModelers. All Rights Reserved.
//
// v1.8 — BP Reference Mode.
//
// UMapLinkBPBinding marks a user-authored actor-derived UBlueprint as
// having a MapLink stand-in .blend on disk.  Its presence is the gate
// for the "Refresh", "Open", "Unlink" right-click menu entries (vs.
// the initial "Create").
//
// Storage strategy (UE 5.7):
//   UBlueprint does NOT inherit IInterface_AssetUserData in 5.7, so we
//   can't use UAssetUserData.  Instead, the binding is stored as a
//   top-level UObject inside the BP's UPackage:
//     - RF_Public | RF_Standalone  -> serialized with the package
//     - IsAsset() overridden false -> hidden from Content Browser
//     - Discovered via ForEachObjectWithPackage(BP->GetOutermost(), ...)
//   When the BP is deleted, its package is destroyed → the binding goes
//   with it.  Rename / move operations are handled explicitly by the
//   lifecycle hooks in D4.
//
// Lives in MapLinkRuntime (not MapLinkEditor) so that any BP carrying
// a binding still loads correctly in non-editor / cooked contexts.  All
// mutation, however, is editor-only — see MapLinkBPStandInCreator and
// MapLinkBPLifecycleHooks.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "MapLinkBPBinding.generated.h"

class UBlueprint;

UCLASS()
class MAPLINKRUNTIME_API UMapLinkBPBinding : public UObject
{
    GENERATED_BODY()

public:
    /** Absolute filesystem path to the stand-in .blend authored by
     *  MapLink's headless Blender pass. */
    UPROPERTY(EditAnywhere, Category = "MapLink")
    FString StandInBlendPath;

    /** UTC timestamp of stand-in creation / last refresh (ISO 8601). */
    UPROPERTY(EditAnywhere, Category = "MapLink")
    FDateTime CreatedTimestamp;

    /** MapLink plugin version that authored this binding (e.g. "1.8"). */
    UPROPERTY(EditAnywhere, Category = "MapLink")
    FString CreatedFromMapLinkVersion;

    /** Hide from the Content Browser — this is binding metadata
     *  attached to a UBlueprint, not a user-discoverable asset. */
    virtual bool IsAsset() const override { return false; }

    // -----------------------------------------------------------------
    // Static helpers
    // -----------------------------------------------------------------

    /** Returns the binding stored alongside BP in its package, or
     *  nullptr if none exists. */
    static UMapLinkBPBinding* Get(UBlueprint* BP);

    /** Returns the existing binding, or creates one (RF_Public |
     *  RF_Standalone, outer=BP's package) and marks the BP package
     *  dirty. */
    static UMapLinkBPBinding* GetOrCreate(UBlueprint* BP);

    /** Detaches and garbage-collects the binding if present.  Marks
     *  the BP package dirty on success.  Safe when no binding exists. */
    static void Remove(UBlueprint* BP);

private:
    /** Object name used for the binding within the BP's package.
     *  One binding per package. */
    static FName GetBindingObjectName();
};
