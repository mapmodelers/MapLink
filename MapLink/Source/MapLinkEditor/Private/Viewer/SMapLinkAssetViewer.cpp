// Copyright MapModelers. All Rights Reserved.

#include "Viewer/SMapLinkAssetViewer.h"
#include "Viewer/SMapLinkAssetViewport.h"

#include "MapLinkPipeline.h"   // OnReimportBegin / OnReimportEnd

#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"   // v1.15.2 — Show Asset Names tick
#include "Widgets/Input/SSpinBox.h"
#include "MapLinkSettings.h"           // v1.15.2
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "MapLinkAssetViewer"

void SMapLinkAssetViewer::Construct(const FArguments& InArgs)
{
    CurrentStem = InArgs._InitialStem;

    RebuildEntries();

    ChildSlot
    [
        SNew(SVerticalBox)

        // ── Toolbar ────────────────────────────────────────────────────
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(6.f, 4.f)
        [
            SNew(SHorizontalBox)

            + SHorizontalBox::Slot().AutoWidth().Padding(2.f, 0.f)
            [
                SNew(SButton)
                .Text(LOCTEXT("First", "|◀"))
                .ToolTipText(LOCTEXT("FirstTip", "First proxy (Home)"))
                .IsEnabled_Raw(this, &SMapLinkAssetViewer::IsNavEnabled)
                .OnClicked_Lambda([this]() { GoFirst(); return FReply::Handled(); })
            ]
            + SHorizontalBox::Slot().AutoWidth().Padding(2.f, 0.f)
            [
                SNew(SButton)
                .Text(LOCTEXT("Prev", "◀ Prev"))
                .ToolTipText(LOCTEXT("PrevTip", "Previous proxy (Left / [ )"))
                .IsEnabled_Raw(this, &SMapLinkAssetViewer::IsNavEnabled)
                .OnClicked_Lambda([this]() { GoPrev(); return FReply::Handled(); })
            ]
            + SHorizontalBox::Slot().AutoWidth().Padding(2.f, 0.f)
            [
                SNew(SButton)
                .Text(LOCTEXT("Next", "Next ▶"))
                .ToolTipText(LOCTEXT("NextTip", "Next proxy (Right / ] )"))
                .IsEnabled_Raw(this, &SMapLinkAssetViewer::IsNavEnabled)
                .OnClicked_Lambda([this]() { GoNext(); return FReply::Handled(); })
            ]
            + SHorizontalBox::Slot().AutoWidth().Padding(2.f, 0.f)
            [
                SNew(SButton)
                .Text(LOCTEXT("Last", "▶|"))
                .ToolTipText(LOCTEXT("LastTip", "Last proxy (End)"))
                .IsEnabled_Raw(this, &SMapLinkAssetViewer::IsNavEnabled)
                .OnClicked_Lambda([this]() { GoLast(); return FReply::Handled(); })
            ]

            // v1.12 — asset counter + name AFTER the nav buttons.
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            .Padding(10.f, 0.f)
            [
                SNew(STextBlock)
                .MinDesiredWidth(160.f)
                .Text_Raw(this, &SMapLinkAssetViewer::GetIndexLabel)
            ]

            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            .Padding(8.f, 0.f)
            [
                SAssignNew(JumpCombo, SComboBox<TSharedPtr<FMapLinkViewerEntry>>)
                .OptionsSource(&Entries)
                .OnGenerateWidget_Lambda([](TSharedPtr<FMapLinkViewerEntry> E)
                {
                    return SNew(STextBlock)
                        .Text(FText::FromString(E.IsValid() ? E->Stem : FString()));
                })
                .OnSelectionChanged_Lambda(
                    [this](TSharedPtr<FMapLinkViewerEntry> E, ESelectInfo::Type Info)
                {
                    if (Info != ESelectInfo::Direct && E.IsValid())
                    {
                        const int32 Idx = Entries.IndexOfByKey(E);
                        if (Idx != INDEX_NONE)
                        {
                            SelectIndex(Idx);
                        }
                    }
                })
                [
                    SNew(STextBlock).Text(LOCTEXT("Jump", "Jump to…"))
                ]
            ]

            // spacer
            + SHorizontalBox::Slot().FillWidth(1.f) [ SNullWidget::NullWidget ]

            // v1.15.2 — mirror of the global "Show Asset Names" toggle
            // (same per-user setting as the level viewport overlay).
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            .Padding(2.f, 0.f, 8.f, 0.f)
            [
                SNew(SCheckBox)
                .IsChecked_Lambda([]()
                {
                    return UMapLinkSettings::Get()->bShowAssetNames
                        ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                })
                .OnCheckStateChanged_Lambda([](ECheckBoxState NewState)
                {
                    UMapLinkSettings::SetShowAssetNames(
                        NewState == ECheckBoxState::Checked);
                })
                .ToolTipText(LOCTEXT("ShowNamesTip",
                    "Show asset name labels — here and in the level "
                    "viewport (per-user setting)."))
                [
                    SNew(SBox).Padding(FMargin(4.f, 2.f))
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("ShowNames", "Show Asset Names"))
                    ]
                ]
            ]

            // v1.15.4 — child labels (whitened) toggle.
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            .Padding(0.f, 0.f, 8.f, 0.f)
            [
                SNew(SCheckBox)
                .IsEnabled_Lambda([]()
                {
                    return UMapLinkSettings::Get()->bShowAssetNames;
                })
                .IsChecked_Lambda([]()
                {
                    return UMapLinkSettings::Get()->bShowChildAssetNames
                        ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                })
                .OnCheckStateChanged_Lambda([](ECheckBoxState NewState)
                {
                    UMapLinkSettings::SetShowChildAssetNames(
                        NewState == ECheckBoxState::Checked);
                })
                .ToolTipText(LOCTEXT("ShowChildrenTip",
                    "Also label child assets (linked instances) in a "
                    "whitened tint — here and in the level viewport "
                    "(per-user setting)."))
                [
                    SNew(SBox).Padding(FMargin(4.f, 2.f))
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("ShowChildren", "Children"))
                    ]
                ]
            ]

            // v1.12 — camera speed multiplier (orbit spin + FreeCam flight).
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            .Padding(2.f, 0.f, 4.f, 0.f)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("SpeedLbl", "Speed"))
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            .Padding(0.f, 0.f, 8.f, 0.f)
            [
                SNew(SBox)
                .WidthOverride(70.f)
                [
                    SNew(SSpinBox<float>)
                    .MinValue(0.1f)
                    .MaxValue(10.f)
                    .Delta(0.1f)
                    .Value_Lambda([this]() { return CameraSpeed; })
                    .ToolTipText(LOCTEXT("SpeedTip",
                        "Camera speed multiplier — scales the orbit spin rate "
                        "and the FreeCam flight speed."))
                    .OnValueChanged_Lambda([this](float NewValue)
                    {
                        CameraSpeed = NewValue;
                        if (Viewport.IsValid())
                        {
                            Viewport->SetCameraSpeed(CameraSpeed);
                        }
                    })
                ]
            ]

            + SHorizontalBox::Slot().AutoWidth().Padding(2.f, 0.f)
            [
                SNew(SButton)
                .Text_Raw(this, &SMapLinkAssetViewer::GetCameraButtonText)
                .ToolTipText(LOCTEXT("CamTip", "Toggle Orbit / FreeCam (C)"))
                .OnClicked_Lambda([this]() { ToggleCamera(); return FReply::Handled(); })
            ]
            + SHorizontalBox::Slot().AutoWidth().Padding(2.f, 0.f)
            [
                SNew(SButton)
                .Text(LOCTEXT("Reframe", "Reframe (F)"))
                .ToolTipText(LOCTEXT("ReframeTip", "Re-center the camera on the asset (F)"))
                .OnClicked_Lambda([this]() { Reframe(); return FReply::Handled(); })
            ]
        ]

        // ── Viewport + overlay ─────────────────────────────────────────
        + SVerticalBox::Slot()
        .FillHeight(1.f)
        .Padding(6.f, 0.f, 6.f, 6.f)
        [
            SNew(SOverlay)

            + SOverlay::Slot()
            [
                SAssignNew(Viewport, SMapLinkAssetViewport)
            ]

            + SOverlay::Slot()
            .HAlign(HAlign_Center)
            .VAlign(VAlign_Center)
            [
                SNew(SBorder)
                .Visibility_Raw(this, &SMapLinkAssetViewer::GetOverlayVisibility)
                .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
                .BorderBackgroundColor(FLinearColor(0.f, 0.f, 0.f, 0.65f))
                .Padding(FMargin(24.f, 14.f))
                [
                    SNew(STextBlock)
                    .Text_Raw(this, &SMapLinkAssetViewer::GetOverlayText)
                    .ColorAndOpacity(FLinearColor::White)
                ]
            ]
        ]
    ];

    if (Viewport.IsValid())
    {
        Viewport->OnPopulated.BindSP(this, &SMapLinkAssetViewer::OnPopulated);
    }

    // Subscribe to the destructive-reimport signals.
    ReimportBeginHandle = FMapLinkPipeline::OnReimportBegin().AddSP(
        this, &SMapLinkAssetViewer::HandleReimportBegin);
    ReimportEndHandle = FMapLinkPipeline::OnReimportEnd().AddSP(
        this, &SMapLinkAssetViewer::HandleReimportEnd);

    // Initial selection: requested stem if present, else the first proxy.
    int32 InitIdx = INDEX_NONE;
    if (!CurrentStem.IsEmpty())
    {
        InitIdx = Entries.IndexOfByPredicate(
            [this](const TSharedPtr<FMapLinkViewerEntry>& E)
            { return E.IsValid() && E->Stem == CurrentStem; });
    }
    SelectIndex(InitIdx == INDEX_NONE ? 0 : InitIdx);
}

SMapLinkAssetViewer::~SMapLinkAssetViewer()
{
    FMapLinkPipeline::OnReimportBegin().Remove(ReimportBeginHandle);
    FMapLinkPipeline::OnReimportEnd().Remove(ReimportEndHandle);
}

// v1.12 — the "Navigate" and "Camera" dropdown menus were removed: they
// duplicated the toolbar buttons one-for-one.

// ───────────────────────────────────────────────────────────────────────
// Data / selection
// ───────────────────────────────────────────────────────────────────────

void SMapLinkAssetViewer::RebuildEntries()
{
    Entries.Reset();

    TArray<FMapLinkViewerEntry> Raw;
    FMapLinkAssetViewerProxyList::Enumerate(Raw);
    for (FMapLinkViewerEntry& E : Raw)
    {
        Entries.Add(MakeShared<FMapLinkViewerEntry>(MoveTemp(E)));
    }

    if (JumpCombo.IsValid())
    {
        JumpCombo->RefreshOptions();
    }
}

void SMapLinkAssetViewer::SelectIndex(int32 Index)
{
    if (Entries.Num() == 0)
    {
        CurrentIndex = INDEX_NONE;
        OverlayState = EOverlay::Empty;
        if (Viewport.IsValid())
        {
            Viewport->ClearScene();
        }
        return;
    }

    Index = FMath::Clamp(Index, 0, Entries.Num() - 1);
    CurrentIndex = Index;
    CurrentStem  = Entries[Index]->Stem;
    OverlayState = EOverlay::Loading;

    if (Viewport.IsValid())
    {
        Viewport->ShowEntry(*Entries[Index]);
    }
    if (JumpCombo.IsValid())
    {
        JumpCombo->SetSelectedItem(Entries[Index]);
    }
}

void SMapLinkAssetViewer::SelectByStem(const FString& Stem)
{
    RebuildEntries();
    const int32 Idx = Entries.IndexOfByPredicate(
        [&Stem](const TSharedPtr<FMapLinkViewerEntry>& E)
        { return E.IsValid() && E->Stem == Stem; });
    SelectIndex(Idx == INDEX_NONE ? 0 : Idx);
}

void SMapLinkAssetViewer::GoPrev()  { if (CurrentIndex != INDEX_NONE) SelectIndex(CurrentIndex - 1); }
void SMapLinkAssetViewer::GoNext()  { if (CurrentIndex != INDEX_NONE) SelectIndex(CurrentIndex + 1); }
void SMapLinkAssetViewer::GoFirst() { SelectIndex(0); }
void SMapLinkAssetViewer::GoLast()  { SelectIndex(Entries.Num() - 1); }

void SMapLinkAssetViewer::Reframe()
{
    if (Viewport.IsValid()) Viewport->Reframe();
}

void SMapLinkAssetViewer::ToggleCamera()
{
    if (Viewport.IsValid()) Viewport->ToggleCameraMode();
}

// ───────────────────────────────────────────────────────────────────────
// Reimport pause / resume
// ───────────────────────────────────────────────────────────────────────

void SMapLinkAssetViewer::HandleReimportBegin(const FString& /*Stem*/)
{
    OverlayState = EOverlay::Paused;
    if (Viewport.IsValid())
    {
        Viewport->ClearScene();   // drop all refs to assets about to be destroyed
        Viewport->SetPaused(true);
    }
}

void SMapLinkAssetViewer::HandleReimportEnd(const FString& /*Stem*/)
{
    if (Viewport.IsValid())
    {
        Viewport->SetPaused(false);
    }

    RebuildEntries();

    if (Entries.Num() == 0)
    {
        // Content root drained / asset gone — show a clear, non-stale message
        // rather than rendering dangling state.
        CurrentIndex = INDEX_NONE;
        OverlayState = EOverlay::Empty;
        return;
    }

    const int32 Idx = Entries.IndexOfByPredicate(
        [this](const TSharedPtr<FMapLinkViewerEntry>& E)
        { return E.IsValid() && E->Stem == CurrentStem; });
    SelectIndex(Idx == INDEX_NONE ? 0 : Idx);
}

void SMapLinkAssetViewer::OnPopulated()
{
    if (OverlayState != EOverlay::Paused)
    {
        OverlayState = EOverlay::Hidden;
    }
}

// ───────────────────────────────────────────────────────────────────────
// Keys + binders
// ───────────────────────────────────────────────────────────────────────

FReply SMapLinkAssetViewer::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
    if (OverlayState == EOverlay::Paused)
    {
        return FReply::Unhandled();
    }

    const FKey Key = InKeyEvent.GetKey();
    if (Key == EKeys::Left  || Key == EKeys::LeftBracket)  { GoPrev();  return FReply::Handled(); }
    if (Key == EKeys::Right || Key == EKeys::RightBracket) { GoNext();  return FReply::Handled(); }
    if (Key == EKeys::Home)                                { GoFirst(); return FReply::Handled(); }
    if (Key == EKeys::End)                                 { GoLast();  return FReply::Handled(); }
    if (Key == EKeys::F)                                   { Reframe(); return FReply::Handled(); }
    if (Key == EKeys::C)                                   { ToggleCamera(); return FReply::Handled(); }

    return FReply::Unhandled();
}

bool SMapLinkAssetViewer::IsNavEnabled() const
{
    return Entries.Num() > 0 && OverlayState != EOverlay::Paused;
}

FText SMapLinkAssetViewer::GetIndexLabel() const
{
    if (CurrentIndex == INDEX_NONE || Entries.Num() == 0)
    {
        return LOCTEXT("NoSel", "0 / 0");
    }
    return FText::FromString(FString::Printf(TEXT("%d / %d   %s"),
        CurrentIndex + 1, Entries.Num(), *CurrentStem));
}

FText SMapLinkAssetViewer::GetCameraButtonText() const
{
    const EMapLinkViewerCameraMode Mode =
        Viewport.IsValid() ? Viewport->GetCameraMode() : EMapLinkViewerCameraMode::Orbit;
    return (Mode == EMapLinkViewerCameraMode::Orbit)
        ? LOCTEXT("CamOrbit", "Camera: Orbit (C)")
        : LOCTEXT("CamFree",  "Camera: FreeCam (C)");
}

FText SMapLinkAssetViewer::GetOverlayText() const
{
    switch (OverlayState)
    {
    case EOverlay::Loading: return LOCTEXT("Loading", "Loading…");
    case EOverlay::Paused:  return LOCTEXT("Paused",  "Reimport in progress — viewer paused.");
    case EOverlay::Empty:   return LOCTEXT("Empty",   "No MapLink asset to display.");
    default:                return FText::GetEmpty();
    }
}

EVisibility SMapLinkAssetViewer::GetOverlayVisibility() const
{
    return (OverlayState == EOverlay::Hidden)
        ? EVisibility::Collapsed
        : EVisibility::HitTestInvisible;
}

#undef LOCTEXT_NAMESPACE
