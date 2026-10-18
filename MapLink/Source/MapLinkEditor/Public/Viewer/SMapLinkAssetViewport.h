// Copyright MapModelers. All Rights Reserved.
//
// v1.11 — MapLink Asset Viewer.
//
// SEditorViewport that owns the single, reused preview scene and swaps the
// displayed proxy in place. Renderable resolution per type:
//   SM_ → /Game/MapLink/<stem>/Meshes/* (composite chain via ExpandSM when a
//         fresh analysis.json exists; otherwise the stable /Meshes/ assets).
//   LI_ → the generated UWorld, replicated as components (all child actors,
//         recursive into nested level instances, depth-capped). Stable across
//         the per-iteration clean because it reads the asset, not Logs/.
//   BP_ → bound Blueprint's static-mesh component visuals, or BP_Deprecated.
//
// Strictly read-only: every spawned object is transient and owned by the
// preview scene; nothing here can mutate a source asset.

#pragma once

#include "CoreMinimal.h"
#include "SEditorViewport.h"
#include "UObject/GCObject.h"
#include "Engine/StreamableManager.h"
#include "Viewer/MapLinkAssetViewerProxyList.h"
#include "Viewer/MapLinkAssetViewerViewportClient.h"

class FAdvancedPreviewScene;
class UStaticMesh;
class UStaticMeshComponent;
class USceneComponent;
class UWorld;
class UClass;

class SMapLinkAssetViewport
    : public SEditorViewport
    , public FGCObject
{
public:
    SLATE_BEGIN_ARGS(SMapLinkAssetViewport) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    virtual ~SMapLinkAssetViewport() override;

    // ── Viewer API (called by the host widget) ─────────────────────────
    /** Load + display the given entry (async, placeholder while pending). */
    void ShowEntry(const FMapLinkViewerEntry& Entry);

    /** Tear down the displayed proxy: cancel any in-flight load, detach and
     *  destroy all spawned components, drop refs. Used on pause / reselect. */
    void ClearScene();

    void ToggleCameraMode();
    EMapLinkViewerCameraMode GetCameraMode() const;
    void Reframe();

    /** v1.12 — camera speed multiplier (orbit spin rate + FreeCam flight). */
    void SetCameraSpeed(float Multiplier);

    /** Pause/resume the viewport client (realtime off, no spin) for reimport. */
    void SetPaused(bool bPaused);

    /** v1.15.3 — 3D name-label anchor: root asset at (0,0,0) or a linked
     *  collection instance (bChild=true, whitened tint, gated by
     *  Show Children) at its transform. */
    struct FLabelAnchor
    {
        FString Stem;
        FVector Position;
        bool    bChild = false;
    };

    /** Drawn by the viewport client when bShowAssetNames is ON. */
    const TArray<FLabelAnchor>& GetLabelAnchors() const
    {
        return LabelAnchors;
    }

    /** Fires (host thread) when a ShowEntry finishes populating the scene. */
    FSimpleDelegate OnPopulated;

    // ── FGCObject ──────────────────────────────────────────────────────
    virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
    virtual FString GetReferencerName() const override
    {
        return TEXT("SMapLinkAssetViewport");
    }

protected:
    virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override;

    /** v1.15 — asset-name overlay (mirrors the level viewport's name
     *  display; driven by UMapLinkSettings::bShowAssetNames). */
    virtual void PopulateViewportOverlays(TSharedRef<SOverlay> Overlay) override;

private:
    // Population entry points (begin async load)
    void BeginShowStaticMesh(const FMapLinkViewerEntry& Entry, int32 Gen);
    void BeginShowLevel(const FMapLinkViewerEntry& Entry, int32 Gen);
    void BeginShowBlueprint(const FMapLinkViewerEntry& Entry, int32 Gen);

    // Load completions
    void OnMeshesLoaded(int32 Gen);
    void OnLevelLoaded(int32 Gen);
    void OnBlueprintLoaded(int32 Gen);

    // Scene helpers
    UStaticMeshComponent* AddStaticMesh(UStaticMesh* Mesh,
                                        const TArray<class UMaterialInterface*>& Overrides,
                                        const FTransform& WorldXf);
    void ReplicateWorld(UWorld* World, const FTransform& ParentXf,
                        int32 Depth, TSet<FString>& VisitedWorlds);
    void CollectBlueprintMeshes(UClass* GeneratedClass, const FTransform& ParentXf);
    void FinishPopulate(int32 Gen);

    static FTransform ComputeComponentWorldTransform(const USceneComponent* Comp);

    /** v1.15.3 — see GetLabelAnchors. */
    TArray<FLabelAnchor> LabelAnchors;

    TSharedPtr<FAdvancedPreviewScene>                 PreviewScene;
    TSharedPtr<FMapLinkAssetViewerViewportClient>     ViewportClient;

    /** GC-protected: spawned preview components (and the meshes they hold). */
    TArray<TObjectPtr<UStaticMeshComponent>> SpawnedComps;

    // In-flight async load state (one load at a time, gated by generation).
    TSharedPtr<FStreamableHandle> PendingLoadHandle;
    int32   LoadGeneration = 0;
    FString CurrentStem;

    FSoftObjectPath InFlightWorldPath;
    FSoftObjectPath InFlightBPPath;
    TArray<TPair<FSoftObjectPath, FTransform>> InFlightMeshPlacements;

    static constexpr int32 kMaxLevelDepth = 8;
    static constexpr int32 kMaxInstancesPerComp = 4096;
};
