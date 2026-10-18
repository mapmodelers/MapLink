// Copyright MapModelers. All Rights Reserved.
//
// v1.8 — BP Reference Mode — UE-side creator.
//
// FMapLinkBPStandInCreator orchestrates the end-to-end creation of a
// BP stand-in: validate the BP, resolve target paths, walk its SCS,
// export per-component FBXs, write the spec JSON, launch the headless
// Blender script (maplink_create_bp_standin.py), P4-add the outputs,
// stamp a UMapLinkBPBinding on the BP, and clean up temp files.
//
// All operations write to an FMapLinkStandInReport so the Content
// Browser extension can surface staged diagnostics in the post-op
// dialog.

#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class FMapLinkStandInReport;

/** Per-component data captured during the SCS walk.  One entry per
 *  UStaticMeshComponent encountered (after deduplication by mesh). */
struct FMapLinkBPStandInComponent
{
    /** Variable name of the SCS node, or "Component_<idx>" fallback. */
    FString ObjectName;
    /** The UStaticMesh referenced by the SCS node. */
    UStaticMesh* StaticMesh = nullptr;
    /** Cumulative relative transform from the SCS root (in UE cm). */
    FTransform RelativeTransform;
    /** /Game/... path of the static mesh (cached for spec output). */
    FString MeshUAssetPath;
};

/** Mode chosen by determine_mode stage. */
enum class EMapLinkBPStandInMode : uint8
{
    Extracted,  // at least one SMC found
    Bounds,     // no SMCs but CDO has non-zero bounds
    Marker,     // pure-logic BP — fallback 200³ wireframe cube
};

class FMapLinkBPStandInCreator
{
public:
    /** Full create flow.  Returns true on overall success.
     *  @param BP                  The Blueprint to create a stand-in for.
     *  @param bForceOverwrite     If false and target .blend exists,
     *                             a YesNo dialog asks before clobbering.
     *  @param Report              Caller-owned report; stages append. */
    static bool CreateStandIn(
        UBlueprint* BP,
        bool bForceOverwrite,
        FMapLinkStandInReport& Report);

    /** Refresh — pre-checks that a binding exists, then calls
     *  CreateStandIn(bForceOverwrite=true). */
    static bool RefreshStandIn(
        UBlueprint* BP,
        FMapLinkStandInReport& Report);

    /** Delete — used by D4 lifecycle hooks.  P4-deletes the .blend
     *  + sidecars, removes the binding from the BP.  If bAlreadyConfirmed
     *  is false, presents a final YesNo dialog first. */
    static bool DeleteStandIn(
        UBlueprint* BP,
        bool bAlreadyConfirmed,
        FMapLinkStandInReport& Report);
};
