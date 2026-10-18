// Copyright MapModelers. All Rights Reserved.
//
// v1.11 — MapLink Asset Viewer.
//
// Read-only FEditorViewportClient for the Asset Viewer. Two camera modes,
// switchable live via a single toggle:
//   Orbit   — asset centered, continuous yaw spin (realtime on).
//   FreeCam — standard editor flight (WASD + RMB look); realtime only while
//             the user is actively interacting, to avoid idle GPU drain.
//
// The client owns NO assets and mutates NOTHING. It only drives the camera and
// ticks the preview world so lit materials render.

#pragma once

#include "CoreMinimal.h"
#include "EditorViewportClient.h"

class FAdvancedPreviewScene;
class SMapLinkAssetViewport;

enum class EMapLinkViewerCameraMode : uint8
{
    Orbit,
    FreeCam,
};

class FMapLinkAssetViewerViewportClient : public FEditorViewportClient
{
public:
    FMapLinkAssetViewerViewportClient(
        FAdvancedPreviewScene* InPreviewScene,
        const TSharedRef<SMapLinkAssetViewport>& InViewportWidget);

    // ── FEditorViewportClient ──────────────────────────────────────────
    virtual void Tick(float DeltaSeconds) override;
    virtual void TrackingStarted(const struct FInputEventState& InInputState,
                                 bool bIsDraggingWidget,
                                 bool bNudge) override;
    virtual void TrackingStopped() override;
    /** v1.13.1 — mouse-wheel zoom (dolly). Orbit: scales OrbitDistance;
     *  FreeCam: dollies along the view direction. Handled here so the wheel
     *  no longer just changes the hidden flight-speed setting. */
    virtual bool InputKey(const FInputKeyEventArgs& EventArgs) override;

    /** v1.15.3 — 3D asset-name labels (root + linked instances), drawn
     *  when UMapLinkSettings::bShowAssetNames is ON.  Anchors come from
     *  the viewport widget (GetLabelAnchors); colors from
     *  FMapLinkNameDisplay (type color, out-of-date override). */
    virtual void DrawCanvas(FViewport& InViewport, FSceneView& View,
                            FCanvas& Canvas) override;

    // ── Camera mode ────────────────────────────────────────────────────
    EMapLinkViewerCameraMode GetCameraMode() const { return CameraMode; }
    void SetCameraMode(EMapLinkViewerCameraMode InMode);
    void ToggleCameraMode();

    /** Center + size the camera on the given world-space bounds.
     *  In Orbit, sets the spin target and distance; in FreeCam, drops the
     *  camera at a sensible 3/4 vantage. Safe to call with empty bounds. */
    void FrameBounds(const FBoxSphereBounds& InBounds);

    /** Pause: forces realtime off and stops the spin / world tick. Used while
     *  a MapLink reimport is destroying and recreating assets. */
    void SetPaused(bool bInPaused);

    /** v1.12 — camera speed multiplier: scales the orbit spin rate directly
     *  and maps onto the editor FreeCam speed setting (1–8, log2 steps). */
    void SetSpeedMultiplier(float InMultiplier);

private:
    void ApplyOrbitCamera();

    EMapLinkViewerCameraMode CameraMode = EMapLinkViewerCameraMode::Orbit;

    // Orbit state
    FVector OrbitCenter   = FVector::ZeroVector;
    float   OrbitDistance = 500.f;
    float   OrbitYaw      = -135.f;          // degrees, advanced each tick
    float   OrbitPitch    = -20.f;           // fixed look-down
    float   OrbitSpeedDeg = 25.f;            // deg/sec (base, pre-multiplier)

    /** v1.12 — user camera speed multiplier (toolbar spinner).
     *  v1.13.1 — default 0.3 (was 1.0) so the viewer opens at 0.3 even before
     *  the toolbar spinner pushes a value. */
    float   SpeedMultiplier = 0.3f;

    /** v1.13.1 — fractional distance change per mouse-wheel notch (dolly).
     *  0.3 = each notch moves 30% closer / farther. */
    float   ZoomSpeed = 0.3f;

    /** True while the user is dragging/flying (RMB capture) — used to gate
     *  FreeCam realtime so an idle viewport stops redrawing. */
    bool bInteracting = false;

    /** True while a reimport is in progress; suppresses spin + world tick. */
    bool bPaused = false;
};
