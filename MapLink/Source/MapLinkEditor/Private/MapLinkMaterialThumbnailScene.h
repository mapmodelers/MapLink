// Copyright MapModelers. All Rights Reserved.
//
// v1.20 — the thumbnail scene behind MapLink's material-instance previews.
//
// WHY THIS EXISTS AT ALL
//
// The engine's own FMaterialThumbnailScene very nearly does this job, and for
// a while the plan was to use it: setting `ThumbnailInfo->PreviewMesh` swaps
// its sphere for the MapLink calibration cube, and OrbitPitch/OrbitYaw aim the
// camera, all without a line of rendering code.  It falls down on ONE detail —
// it only ever assigns the material to slot 0:
//
//     PreviewActor->GetStaticMeshComponent()->SetMaterial(0, InMaterial);
//
// mat_cube carries two slots by design (0 = opaque geometry, 1 = translucent),
// so an alpha material rendered through the engine scene lands on the opaque
// half and reads nothing like the Materials tab.  Matching that tab is the
// whole requirement, so the scene is ours.
//
// Everything else is inherited: FThumbnailPreviewScene already centres on
// bounds, fits the object in a square frame and builds the FSceneView.
// GetViewMatrixParameters is the only geometry we supply.

#pragma once

#include "CoreMinimal.h"
#include "ThumbnailHelpers.h"

class UMaterialInterface;
class UStaticMesh;
class UStaticMeshComponent;

class FMapLinkMaterialThumbnailScene : public FThumbnailPreviewScene
{
public:
    FMapLinkMaterialThumbnailScene();

    /** Route *InMaterial* onto the slot its blend mode belongs to.
     *
     *  Opaque and masked go to slot 0, the translucent family to slot 1, and
     *  the slot that did NOT get the material reverts to the cube's own
     *  default so the other half of the geometry still reads as the
     *  calibration cube.  Passing null clears both.
     */
    void SetMaterialInterface(UMaterialInterface* InMaterial);

    /** True when a cube mesh was found — false means every render is empty and
     *  the caller should say so once rather than write 87 black squares. */
    bool HasPreviewMesh() const { return PreviewMeshComp != nullptr; }

protected:
    // ── FThumbnailPreviewScene ─────────────────────────────────────────
    virtual void GetViewMatrixParameters(const float InFOVDegrees,
                                         FVector& OutOrigin,
                                         float& OutOrbitPitch,
                                         float& OutOrbitYaw,
                                         float& OutOrbitZoom) const override;

    /** 30°, matching SMapLinkMaterialPreviewViewport.
     *
     *  The default is 90°, which is a first-person game camera rather than a
     *  product shot: at that field of view the near face splays out and the
     *  material's tiling — the only thing the picture exists to show — is
     *  distorted differently in every corner.  The tab already settled this;
     *  the thumbnail has to agree with it or the two stop matching. */
    virtual float GetFOV() const override { return 30.f; }

private:
    UStaticMeshComponent* PreviewMeshComp = nullptr;
};
