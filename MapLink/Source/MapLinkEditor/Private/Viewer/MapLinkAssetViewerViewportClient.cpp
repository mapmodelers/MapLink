// Copyright MapModelers. All Rights Reserved.

#include "Viewer/MapLinkAssetViewerViewportClient.h"
#include "Viewer/SMapLinkAssetViewport.h"

#include "AdvancedPreviewScene.h"
#include "EditorModeManager.h"

// v1.15.3 — 3D asset-name labels
#include "MapLinkNameDisplay.h"
#include "MapLinkSettings.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "SceneView.h"

FMapLinkAssetViewerViewportClient::FMapLinkAssetViewerViewportClient(
    FAdvancedPreviewScene* InPreviewScene,
    const TSharedRef<SMapLinkAssetViewport>& InViewportWidget)
    : FEditorViewportClient(
        /*ModeTools=*/nullptr,
        InPreviewScene,
        StaticCastSharedRef<SEditorViewport>(InViewportWidget))
{
    SetViewMode(VMI_Lit);

    EngineShowFlags.SetGrid(false);
    EngineShowFlags.SetSnap(false);

    // No listener; this is a silent preview.
    bSetListenerPosition = false;

    // Read-only viewer: no mode tools were supplied (ModeTools == nullptr), so
    // there is no transform gizmo and no selection-driven editing at all.

    // Start in orbit, realtime on so the spin is visible immediately.
    CameraMode = EMapLinkViewerCameraMode::Orbit;
    SetRealtime(true);

    ApplyOrbitCamera();
}

void FMapLinkAssetViewerViewportClient::SetCameraMode(EMapLinkViewerCameraMode InMode)
{
    if (CameraMode == InMode)
    {
        return;
    }
    CameraMode = InMode;

    if (CameraMode == EMapLinkViewerCameraMode::Orbit)
    {
        // Re-center the spin on the framed target.
        ApplyOrbitCamera();
        SetRealtime(true);
    }
    else
    {
        // FreeCam: keep wherever the orbit left the camera, then idle until the
        // user interacts.
        SetRealtime(false);
    }
    Invalidate();
}

void FMapLinkAssetViewerViewportClient::ToggleCameraMode()
{
    SetCameraMode(CameraMode == EMapLinkViewerCameraMode::Orbit
                      ? EMapLinkViewerCameraMode::FreeCam
                      : EMapLinkViewerCameraMode::Orbit);
}

void FMapLinkAssetViewerViewportClient::FrameBounds(const FBoxSphereBounds& InBounds)
{
    OrbitCenter = InBounds.Origin;

    const float Radius = FMath::Max(InBounds.SphereRadius, 1.f);

    // Distance so the bounding sphere fits the vertical FOV with headroom.
    const float HalfFOVRad = FMath::DegreesToRadians(FMath::Max(ViewFOV, 10.f) * 0.5f);
    const float FitDistance = Radius / FMath::Max(FMath::Tan(HalfFOVRad), 0.05f);
    OrbitDistance = FMath::Max(FitDistance * 1.5f, Radius * 1.25f);

    if (CameraMode == EMapLinkViewerCameraMode::Orbit)
    {
        ApplyOrbitCamera();
    }
    else
    {
        // Drop a 3/4 vantage looking at the center.
        const FRotator ViewRot(OrbitPitch, OrbitYaw, 0.f);
        SetViewRotation(ViewRot);
        SetViewLocation(OrbitCenter - ViewRot.Vector() * OrbitDistance);
    }
    Invalidate();
}

void FMapLinkAssetViewerViewportClient::ApplyOrbitCamera()
{
    const FRotator ViewRot(OrbitPitch, OrbitYaw, 0.f);
    SetViewRotation(ViewRot);
    SetViewLocation(OrbitCenter - ViewRot.Vector() * OrbitDistance);
}

void FMapLinkAssetViewerViewportClient::TrackingStarted(
    const FInputEventState& InInputState, bool bIsDraggingWidget, bool bNudge)
{
    bInteracting = true;
    if (CameraMode == EMapLinkViewerCameraMode::FreeCam)
    {
        SetRealtime(true);
    }
    FEditorViewportClient::TrackingStarted(InInputState, bIsDraggingWidget, bNudge);
}

void FMapLinkAssetViewerViewportClient::TrackingStopped()
{
    bInteracting = false;
    FEditorViewportClient::TrackingStopped();
}

bool FMapLinkAssetViewerViewportClient::InputKey(const FInputKeyEventArgs& EventArgs)
{
    // v1.13.1 — mouse wheel = zoom (dolly), instead of the base class's
    // hidden flight-speed change. MouseScrollUp = zoom in, Down = zoom out.
    if (EventArgs.Event == IE_Pressed &&
        (EventArgs.Key == EKeys::MouseScrollUp ||
         EventArgs.Key == EKeys::MouseScrollDown))
    {
        const bool bIn = (EventArgs.Key == EKeys::MouseScrollUp);
        const float Factor = bIn ? (1.f - ZoomSpeed) : (1.f + ZoomSpeed);

        if (CameraMode == EMapLinkViewerCameraMode::Orbit)
        {
            OrbitDistance = FMath::Max(OrbitDistance * Factor, 1.f);
            ApplyOrbitCamera();
        }
        else
        {
            // FreeCam: dolly along the current view direction.
            const FVector Fwd = GetViewRotation().Vector();
            const float Step = OrbitDistance * ZoomSpeed;
            SetViewLocation(GetViewLocation() + Fwd * (bIn ? Step : -Step));
        }
        Invalidate();
        return true;   // consumed — don't fall through to flight-speed logic
    }

    return FEditorViewportClient::InputKey(EventArgs);
}

void FMapLinkAssetViewerViewportClient::SetSpeedMultiplier(float InMultiplier)
{
    SpeedMultiplier = FMath::Clamp(InMultiplier, 0.1f, 10.f);

    // FreeCam flight speed — UE 5.7 float-based camera speed API
    // (FEditorViewportCameraSpeedSettings; the integer speed-setting API is
    // deprecated). Default CurrentSpeed is 1.0, so the toolbar multiplier
    // maps directly onto it.
    FEditorViewportCameraSpeedSettings Speed = GetCameraSpeedSettings();
    Speed.SetCurrentSpeed(SpeedMultiplier);
    SetCameraSpeedSettings(Speed);
}

void FMapLinkAssetViewerViewportClient::SetPaused(bool bInPaused)
{
    bPaused = bInPaused;
    if (bPaused)
    {
        SetRealtime(false);
        Invalidate();
    }
}

void FMapLinkAssetViewerViewportClient::Tick(float DeltaSeconds)
{
    FEditorViewportClient::Tick(DeltaSeconds);

    if (bPaused)
    {
        SetRealtime(false);
        return;   // no spin, no preview-world tick while paused
    }

    // Realtime is on continuously in Orbit (spin); in FreeCam only while the
    // user is actively flying. Otherwise the viewport idles on-demand.
    SetRealtime(CameraMode == EMapLinkViewerCameraMode::Orbit || bInteracting);

    if (CameraMode == EMapLinkViewerCameraMode::Orbit)
    {
        OrbitYaw = FMath::Fmod(
            OrbitYaw + OrbitSpeedDeg * SpeedMultiplier * DeltaSeconds, 360.f);
        ApplyOrbitCamera();
    }

    // Advance the preview world so realtime material/lighting renders.
    if (PreviewScene)
    {
        PreviewScene->GetWorld()->Tick(LEVELTICK_All, DeltaSeconds);
    }
}

void FMapLinkAssetViewerViewportClient::DrawCanvas(
    FViewport& InViewport, FSceneView& View, FCanvas& Canvas)
{
    FEditorViewportClient::DrawCanvas(InViewport, View, Canvas);

    // v1.15.3 — 3D asset-name labels: root asset + linked collection
    // instances at their transforms (mirrors the level viewport's name
    // display; same toggles, same palette).
    if (!UMapLinkSettings::Get()->bShowAssetNames)
    {
        return;
    }

    TSharedPtr<SEditorViewport> WidgetPin = GetEditorViewportWidget();
    if (!WidgetPin.IsValid())
    {
        return;
    }
    const SMapLinkAssetViewport* AssetViewport =
        static_cast<const SMapLinkAssetViewport*>(WidgetPin.Get());

    UFont* Font = GEngine ? GEngine->GetSmallFont() : nullptr;
    if (!Font)
    {
        return;
    }

    const bool bShowChildren =
        UMapLinkSettings::Get()->bShowChildAssetNames;

    // v1.15.4 — WorldToPixel returns BACKBUFFER pixels; DrawCanvas
    // operates in DPI-scaled canvas units.  Without the divide, every
    // label drifts down/right proportionally on >100% Windows scaling
    // (the "way under" offset).
    const float DPIScale = FMath::Max(Canvas.GetDPIScale(), 0.01f);

    for (const SMapLinkAssetViewport::FLabelAnchor& Anchor :
             AssetViewport->GetLabelAnchors())
    {
        if (Anchor.bChild && !bShowChildren)
        {
            continue;
        }

        // v1.15.5 — same per-type filters (LI_/SM_/BP_) as the level
        // viewport: full parity between both label systems.
        if (!FMapLinkNameDisplay::ShouldShowStem(Anchor.Stem))
        {
            continue;
        }

        FVector2D Pixel;
        if (!View.WorldToPixel(Anchor.Position, Pixel))
        {
            continue;   // behind the camera
        }
        Pixel /= DPIScale;

        FCanvasTextItem Item(
            Pixel, FText::FromString(Anchor.Stem), Font,
            Anchor.bChild
                ? FMapLinkNameDisplay::GetChildLabelColor(Anchor.Stem)
                : FMapLinkNameDisplay::GetLabelColor(Anchor.Stem));
        Item.bCentreX = true;
        Item.bCentreY = true;
        Item.EnableShadow(FLinearColor::Black);
        Canvas.DrawItem(Item);
    }
}
