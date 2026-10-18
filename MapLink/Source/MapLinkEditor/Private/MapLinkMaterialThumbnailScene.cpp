// Copyright MapModelers. All Rights Reserved.

#include "MapLinkMaterialThumbnailScene.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "UObject/Package.h"

#include "MapLinkPipeline.h"   // LogMapLink

namespace
{
    // Same convention, and the same two paths, as
    // SMapLinkMaterialPreviewViewport. Kept as literals in both places rather
    // than shared through a header: the viewport is UI and this is an offline
    // renderer, and a shared constant would be the only thing coupling them.
    constexpr int32 kOpaqueSlot = 0;
    constexpr int32 kAlphaSlot  = 1;

    const TCHAR* kCubePath   = TEXT("/MapLink/Materials/mat_cube.mat_cube");
    const TCHAR* kEngineCube = TEXT("/Engine/BasicShapes/Cube.Cube");

    /** Which slot this material's blend mode belongs on. */
    int32 SlotForBlendMode(const UMaterialInterface* InMaterial)
    {
        if (!InMaterial)
        {
            return kOpaqueSlot;
        }
        switch (InMaterial->GetBlendMode())
        {
        case BLEND_Translucent:
        case BLEND_Additive:
        case BLEND_Modulate:
        case BLEND_AlphaComposite:
        case BLEND_AlphaHoldout:
            return kAlphaSlot;
        default:
            // Opaque and Masked. Masked deliberately counts as opaque here: it
            // renders on opaque geometry in the engine too, and putting it on
            // the translucent half would misrepresent how it will look.
            return kOpaqueSlot;
        }
    }
}

FMapLinkMaterialThumbnailScene::FMapLinkMaterialThumbnailScene()
    : FThumbnailPreviewScene(
        FThumbnailPreviewScene::FConstructionValues().SetCreateFloorPlane(false))
{
    // No floor, matching the tab: the cube floats free so it never z-fights a
    // ground plane, and a grid of floating cubes reads cleaner than 87 copies
    // of the same shadow.
    UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, kCubePath);
    if (!Mesh)
    {
        Mesh = LoadObject<UStaticMesh>(nullptr, kEngineCube);
        if (Mesh)
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("[MatThumb] %s not found; falling back to the engine "
                        "cube. It has ONE material slot, so translucent "
                        "materials will preview on opaque geometry."),
                   kCubePath);
        }
    }

    if (!Mesh)
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("[MatThumb] No preview cube available (tried %s and %s) — "
                    "material thumbnails cannot be rendered."),
               kCubePath, kEngineCube);
        return;
    }

    PreviewMeshComp = NewObject<UStaticMeshComponent>(GetTransientPackage());
    PreviewMeshComp->SetStaticMesh(Mesh);
    AddComponent(PreviewMeshComp, FTransform::Identity);
}

void FMapLinkMaterialThumbnailScene::SetMaterialInterface(
    UMaterialInterface* InMaterial)
{
    if (!PreviewMeshComp)
    {
        return;
    }

    const int32 TargetSlot = SlotForBlendMode(InMaterial);
    const int32 NumSlots   = PreviewMeshComp->GetNumMaterials();

    for (int32 Slot = 0; Slot < NumSlots; ++Slot)
    {
        // Null reverts a slot to the mesh's own default material, which is
        // what "if alpha only then 0 is the default mat" asks for.
        UMaterialInterface* ForSlot =
            (InMaterial && Slot == TargetSlot) ? InMaterial : nullptr;
        PreviewMeshComp->SetMaterial(Slot, ForSlot);
    }

    // Centre on the bounds, exactly as FMaterialThumbnailScene does. Without
    // this a mesh whose pivot is not at its centre renders off-frame, and
    // "centred in a square frame" is half of what a thumbnail grid is for.
    FTransform Transform = FTransform::Identity;
    PreviewMeshComp->SetRelativeTransform(Transform);
    PreviewMeshComp->UpdateBounds();

    const float BoundsZOffset = GetBoundsZOffset(PreviewMeshComp->Bounds);
    Transform.SetLocation(-PreviewMeshComp->Bounds.Origin
                          + FVector(0.0, 0.0, BoundsZOffset));
    PreviewMeshComp->SetRelativeTransform(Transform);

    PreviewMeshComp->RecreateRenderState_Concurrent();
}

void FMapLinkMaterialThumbnailScene::GetViewMatrixParameters(
    const float InFOVDegrees,
    FVector& OutOrigin,
    float& OutOrbitPitch,
    float& OutOrbitYaw,
    float& OutOrbitZoom) const
{
    // Framing: the same arithmetic the engine's material scene uses, so the
    // cube sits at the same size in frame as every other UE thumbnail.
    // tan(HalfFOV) = HalfMeshSize / Distance, with 15% headroom so perspective
    // at the corners does not clip the silhouette.
    const float HalfFOVRadians = FMath::DegreesToRadians<float>(InFOVDegrees) * 0.5f;
    const float BoundsMultiplier = 1.15f;

    float HalfMeshSize  = 100.f;
    float BoundsZOffset = 0.f;
    if (PreviewMeshComp)
    {
        HalfMeshSize = static_cast<float>(
            PreviewMeshComp->Bounds.SphereRadius * BoundsMultiplier);
        BoundsZOffset = GetBoundsZOffset(PreviewMeshComp->Bounds);
    }

    const float TargetDistance = HalfMeshSize / FMath::Tan(HalfFOVRadians);

    OutOrigin = FVector(0.0, 0.0, -BoundsZOffset);

    // The Materials tab looks from FRotator(-25, -135, 0). Pitch carries over
    // unchanged; the yaw is that view turned 180° so the camera sits on the
    // opposite corner of the cube, which is the face pair where the blend
    // gradient actually reads. -135 + 180 = 45.
    //
    // If a future cube changes which faces carry the gradient, this is the
    // number to move, and it is the only one.
    OutOrbitPitch = -25.f;
    OutOrbitYaw   = 45.f;
    OutOrbitZoom  = TargetDistance;
}
