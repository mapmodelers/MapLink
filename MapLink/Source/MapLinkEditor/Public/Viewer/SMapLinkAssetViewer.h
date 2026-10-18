// Copyright MapModelers. All Rights Reserved.
//
// v1.11 — MapLink Asset Viewer (host SDockTab content).
//
// Toolbar (Prev / index label / Next / jump dropdown / single camera toggle /
// Reframe), the read-only viewport, and a non-interactive overlay used for the
// "Loading…", reimport-pause, and "no asset" states. Subscribes to the MapLink
// reimport begin/end signals to pause + refresh safely across the destructive
// recreate. Identity is the BlendName stem (MapLink has no per-asset GUID).

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SComboBox.h"
#include "Viewer/MapLinkAssetViewerProxyList.h"

class SMapLinkAssetViewport;

class SMapLinkAssetViewer : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SMapLinkAssetViewer) {}
        /** Stem to show first; empty → first proxy in the sorted list. */
        SLATE_ARGUMENT(FString, InitialStem)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    virtual ~SMapLinkAssetViewer() override;

    /** Bring the viewer to a given stem (called by the Manager entry point on
     *  an already-open tab). Falls back to the first proxy if absent. */
    void SelectByStem(const FString& Stem);

    // SWidget — host-level key handling for nav/camera.
    virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
    virtual bool SupportsKeyboardFocus() const override { return true; }

private:
    enum class EOverlay : uint8 { Hidden, Loading, Paused, Empty };

    // Data / selection
    void RebuildEntries();
    void SelectIndex(int32 Index);
    void GoPrev();
    void GoNext();
    void GoFirst();
    void GoLast();
    void Reframe();
    void ToggleCamera();

    // Reimport signals
    void HandleReimportBegin(const FString& Stem);
    void HandleReimportEnd(const FString& Stem);

    // UI binders
    FText GetIndexLabel() const;
    FText GetCameraButtonText() const;
    FText GetOverlayText() const;
    EVisibility GetOverlayVisibility() const;
    bool IsNavEnabled() const;

    void OnPopulated();

    // ── State ──────────────────────────────────────────────────────────
    TArray<TSharedPtr<FMapLinkViewerEntry>> Entries;
    int32   CurrentIndex = INDEX_NONE;
    FString CurrentStem;

    TSharedPtr<SMapLinkAssetViewport> Viewport;
    TSharedPtr<SComboBox<TSharedPtr<FMapLinkViewerEntry>>> JumpCombo;

    /** v1.12 — camera speed multiplier (orbit spin + FreeCam flight).
     *  v1.13.1 — default 0.3 (was 1.0). */
    float CameraSpeed = 0.3f;

    EOverlay OverlayState = EOverlay::Empty;

    FDelegateHandle ReimportBeginHandle;
    FDelegateHandle ReimportEndHandle;
};
