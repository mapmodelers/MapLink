// Copyright MapModelers. All Rights Reserved.

#include "SMapLinkMaterialPreviewViewport.h"

#include "AdvancedPreviewScene.h"
#include "EditorViewportClient.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "UObject/Package.h"

#include "MapLinkPipeline.h"   // LogMapLink

// ───────────────────────────────────────────────────────────────────────
// Cube material-slot convention (set by the imported mat_cube SM)
// ───────────────────────────────────────────────────────────────────────

namespace
{
    constexpr int32 kOpaqueSlot = 0;   // opaque / masked geometry
    constexpr int32 kAlphaSlot  = 1;   // translucent (alpha) geometry

    const TCHAR* kCubePath   = TEXT("/MapLink/Materials/mat_cube.mat_cube");
    const TCHAR* kEngineCube = TEXT("/Engine/BasicShapes/Cube.Cube");

    /** Does this material belong on the cube's translucent geometry?
     *  Kept identical to FMapLinkMaterialThumbnailScene::SlotForBlendMode —
     *  if the two ever disagree, the tab and the thumbnail disagree. */
    bool IsTranslucentBlendMode(const UMaterialInterface* Material)
    {
        if (!Material)
        {
            return false;
        }
        switch (Material->GetBlendMode())
        {
        case BLEND_Translucent:
        case BLEND_Additive:
        case BLEND_Modulate:
        case BLEND_AlphaComposite:
        case BLEND_AlphaHoldout:
            return true;
        default:
            return false;   // Opaque and Masked render on opaque geometry.
        }
    }
}

// ───────────────────────────────────────────────────────────────────────
// Viewport client
// ───────────────────────────────────────────────────────────────────────

class FMapLinkMaterialPreviewViewportClient : public FEditorViewportClient
{
public:
    FMapLinkMaterialPreviewViewportClient(
        FAdvancedPreviewScene* InPreviewScene,
        const TSharedRef<SMapLinkMaterialPreviewViewport>& InViewportWidget)
        : FEditorViewportClient(
            /*ModeTools=*/nullptr,
            InPreviewScene,
            StaticCastSharedRef<SEditorViewport>(InViewportWidget))
    {
        // Lit material preview, framed on the cube, spinning lighting rig from
        // the advanced preview scene.
        SetViewMode(VMI_Lit);
        SetRealtime(true);

        // v1.19 — a narrow lens.
        //
        // FEditorViewportClient defaults to 90°, which is a first-person game
        // camera, not a product shot: at that field of view a cube 190 units
        // away is drawn with violent perspective, the near face splays out and
        // the tiling of the material — the only thing this viewport exists to
        // show — is distorted differently in every corner. 30° is close to a
        // long lens and reads almost orthographic, so a repeating texture
        // looks the same across the face.
        //
        // The camera moves back to match. Framing is distance × tan(FOV/2), so
        // dropping 90° → 30° means multiplying the distance by
        // tan(45°)/tan(15°) ≈ 3.73 to keep the cube the same size on screen.
        ViewFOV  = 30.f;
        FOVAngle = ViewFOV;

        SetViewLocation(FVector(448.f, 448.f, 336.f));
        SetViewRotation(FRotator(-25.f, -135.f, 0.f));

        EngineShowFlags.SetGrid(false);
        EngineShowFlags.SetSnap(false);

        bSetListenerPosition = false;
    }

    virtual void Tick(float DeltaSeconds) override
    {
        FEditorViewportClient::Tick(DeltaSeconds);

        // Advance the preview world so realtime material updates render.
        if (PreviewScene)
        {
            PreviewScene->GetWorld()->Tick(LEVELTICK_All, DeltaSeconds);
        }
    }
};

// ───────────────────────────────────────────────────────────────────────
// Widget
// ───────────────────────────────────────────────────────────────────────

void SMapLinkMaterialPreviewViewport::Construct(const FArguments& InArgs)
{
    PreviewScene = MakeShared<FAdvancedPreviewScene>(
        FPreviewScene::ConstructionValues());

    // No ground plane — the cube floats free so it never z-fights / overlaps
    // a floor (per request).
    PreviewScene->SetFloorVisibility(false);

    // Spawn the preview cube into the scene.
    if (UStaticMesh* Mesh = LoadPreviewMesh())
    {
        PreviewMeshComp = NewObject<UStaticMeshComponent>(GetTransientPackage());
        PreviewMeshComp->SetStaticMesh(Mesh);
        PreviewScene->AddComponent(PreviewMeshComp, FTransform::Identity);
    }
    else
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("[MatPreview] Could not load a preview cube mesh "
                    "(tried %s and %s)."), kCubePath, kEngineCube);
    }

    SEditorViewport::Construct(SEditorViewport::FArguments());
}

SMapLinkMaterialPreviewViewport::~SMapLinkMaterialPreviewViewport()
{
    if (ViewportClient.IsValid())
    {
        ViewportClient->Viewport = nullptr;
    }
}

TSharedRef<FEditorViewportClient>
SMapLinkMaterialPreviewViewport::MakeEditorViewportClient()
{
    ViewportClient = MakeShared<FMapLinkMaterialPreviewViewportClient>(
        PreviewScene.Get(), SharedThis(this));
    return ViewportClient.ToSharedRef();
}

UStaticMesh* SMapLinkMaterialPreviewViewport::LoadPreviewMesh() const
{
    if (UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, kCubePath))
    {
        return Cube;
    }
    // Fallback so the viewport is never empty on projects without the plugin
    // content mounted (engine cube has a single slot — opaque preview only).
    return LoadObject<UStaticMesh>(nullptr, kEngineCube);
}

void SMapLinkMaterialPreviewViewport::SetPreviewMaterial(UMaterialInterface* Material)
{
    // v1.20 — route by blend mode, which is what this class's header has
    // claimed since v1.10 ("the selected MI is assigned to the slot matching
    // its blend mode") and what SMapLinkMaterialsTab's call site comments
    // describe ("opaque -> 0, alpha -> 1"). The code did neither: it always
    // used slot 0, so a translucent material previewed on the opaque half of
    // the cube.
    //
    // It matters now because the Asset Browser thumbnails render through
    // FMapLinkMaterialThumbnailScene, which DOES route by blend mode. Leaving
    // this as it was would mean the tab and the thumbnail of the same material
    // showed two different pictures.
    const int32 Slot = IsTranslucentBlendMode(Material) ? kAlphaSlot : kOpaqueSlot;
    const int32 Other = (Slot == kAlphaSlot) ? kOpaqueSlot : kAlphaSlot;

    SlotMaterials[Slot] = Material;
    // The slot that did not get it reverts to the cube's own default, so the
    // rest of the geometry still reads as the calibration cube rather than
    // keeping whatever was previewed before.
    SlotMaterials[Other] = nullptr;
    ApplySlots();
}

void SMapLinkMaterialPreviewViewport::SetSlotMaterial(int32 Slot, UMaterialInterface* Material)
{
    if (Slot != kOpaqueSlot && Slot != kAlphaSlot)
    {
        return;
    }
    SlotMaterials[Slot] = Material;
    ApplySlots();
}

UMaterialInterface* SMapLinkMaterialPreviewViewport::GetSlotMaterial(int32 Slot) const
{
    return (Slot == kOpaqueSlot || Slot == kAlphaSlot) ? SlotMaterials[Slot] : nullptr;
}

void SMapLinkMaterialPreviewViewport::ApplySlots()
{
    if (!PreviewMeshComp)
    {
        return;
    }

    const int32 NumSlots = PreviewMeshComp->GetNumMaterials();
    for (int32 Slot = 0; Slot < 2; ++Slot)
    {
        if (NumSlots > Slot)
        {
            PreviewMeshComp->SetMaterial(Slot, SlotMaterials[Slot]);
        }
    }

    PreviewMeshComp->MarkRenderStateDirty();
    if (ViewportClient.IsValid())
    {
        ViewportClient->Invalidate();
    }
}

void SMapLinkMaterialPreviewViewport::AddReferencedObjects(
    FReferenceCollector& Collector)
{
    Collector.AddReferencedObject(PreviewMeshComp);
    Collector.AddReferencedObject(SlotMaterials[0]);
    Collector.AddReferencedObject(SlotMaterials[1]);
}
