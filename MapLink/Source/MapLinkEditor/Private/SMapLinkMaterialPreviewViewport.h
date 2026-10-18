// Copyright MapModelers. All Rights Reserved.
//
// v1.10 (MIM Drop 2) — embedded live material preview viewport.
//
// A self-contained SEditorViewport that previews a single UMaterialInterface
// on the MapLink calibration cube static mesh (/MapLink/Materials/mat_cube).
// The cube carries two material slots by design:
//   slot 0 → opaque geometry   slot 1 → alpha (translucent) geometry
// so the selected MI is assigned to the slot matching its blend mode and the
// preview reads correctly for both opaque and translucent materials.

#pragma once

#include "CoreMinimal.h"
#include "SEditorViewport.h"
#include "UObject/GCObject.h"

class FAdvancedPreviewScene;
class FMapLinkMaterialPreviewViewportClient;
class UMaterialInterface;
class UStaticMesh;
class UStaticMeshComponent;

class SMapLinkMaterialPreviewViewport
    : public SEditorViewport
    , public FGCObject
{
public:
    SLATE_BEGIN_ARGS(SMapLinkMaterialPreviewViewport) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    virtual ~SMapLinkMaterialPreviewViewport() override;

    /** Assign the material being edited — always slot 0. */
    void SetPreviewMaterial(UMaterialInterface* Material);

    /** Assign a material to an explicit slot (0 = edited material,
     *  1 = extra preview material). Passing null reverts that slot to the
     *  cube's built-in default material. */
    void SetSlotMaterial(int32 Slot, UMaterialInterface* Material);

    /** Current material on a slot (null = cube default). */
    UMaterialInterface* GetSlotMaterial(int32 Slot) const;

    // ── FGCObject ──────────────────────────────────────────────────────
    virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
    virtual FString GetReferencerName() const override
    {
        return TEXT("SMapLinkMaterialPreviewViewport");
    }

protected:
    // ── SEditorViewport ────────────────────────────────────────────────
    // NOTE (UE 5.7): MakeViewportToolbar() is `final` on SEditorViewport and
    // OnFloatingButtonClicked() is no longer a virtual here, so neither is
    // overridden — the default (empty) viewport toolbar is used.
    virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override;

private:
    /** Load the MapLink cube SM; falls back to the engine cube if missing. */
    UStaticMesh* LoadPreviewMesh() const;

    /** Push SlotMaterials[0..1] onto the cube's two material slots. */
    void ApplySlots();

    TSharedPtr<FAdvancedPreviewScene>                  PreviewScene;
    TSharedPtr<FMapLinkMaterialPreviewViewportClient>  ViewportClient;

    /** GC-protected: owned by the preview scene, referenced here defensively. */
    TObjectPtr<UStaticMeshComponent> PreviewMeshComp = nullptr;
    TObjectPtr<UMaterialInterface>   SlotMaterials[2] = { nullptr, nullptr };
};
