// Copyright MapModelers. All Rights Reserved.
//
// v1.16 — Cleanup dialog (see header).

#include "SMapLinkTextureCleanupDialog.h"

#include "Misc/Paths.h"

#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "SMapLinkTextureCleanupDialog"

namespace
{
    const FName ColCheck (TEXT("Check"));
    const FName ColName  (TEXT("Name"));
    const FName ColWhy   (TEXT("Why"));
    const FName ColWhere (TEXT("Where"));
    const FName ColSize  (TEXT("Size"));

    FText PrettyBytes(int64 Bytes)
    {
        if (Bytes <= 0)
        {
            return FText::GetEmpty();
        }
        if (Bytes < 1024 * 1024)
        {
            return FText::Format(LOCTEXT("KB", "{0} KB"),
                                 FText::AsNumber(FMath::Max<int64>(1, Bytes / 1024)));
        }
        return FText::Format(LOCTEXT("MB", "{0} MB"),
                             FText::AsNumber(FMath::Max<int64>(1, Bytes / (1024 * 1024))));
    }

    /** The reason, in words the artist would use. The enum-ish strings the ops
     *  layer produces are for the manifest, not for a person. */
    FText FriendlyReason(const FMapLinkTextureCleanupItem& Item)
    {
        if (Item.Reason == TEXT("Superseded"))
        {
            return FText::Format(LOCTEXT("WhySuperseded",
                "Promoted to {0}"),
                FText::FromString(FPaths::GetBaseFilename(Item.SupersededBy)));
        }
        return LOCTEXT("WhyOrphan", "Orphaned MapLink texture");
    }

    class SCleanupRowWidget : public SMultiColumnTableRow<TSharedPtr<FMapLinkCleanupRow>>
    {
    public:
        SLATE_BEGIN_ARGS(SCleanupRowWidget) {}
            SLATE_ARGUMENT(TSharedPtr<FMapLinkCleanupRow>, Row)
        SLATE_END_ARGS()

        void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwner)
        {
            Row = InArgs._Row;
            SMultiColumnTableRow<TSharedPtr<FMapLinkCleanupRow>>::Construct(
                FSuperRowType::FArguments(), InOwner);
        }

        virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& Column) override
        {
            if (!Row.IsValid())
            {
                return SNullWidget::NullWidget;
            }

            if (Column == ColCheck)
            {
                TSharedPtr<FMapLinkCleanupRow> Captured = Row;
                return SNew(SBox).Padding(FMargin(4.f, 2.f)).VAlign(VAlign_Center)
                    [
                        SNew(SCheckBox)
                        .IsChecked_Lambda([Captured]()
                        {
                            return Captured->bChecked
                                ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                        })
                        .OnCheckStateChanged_Lambda([Captured](ECheckBoxState State)
                        {
                            Captured->bChecked = (State == ECheckBoxState::Checked);
                        })
                    ];
            }
            if (Column == ColName)
            {
                return SNew(SBox).Padding(FMargin(2.f)).VAlign(VAlign_Center)
                    [
                        SNew(STextBlock)
                        .Text(FText::FromString(Row->Item.AssetName))
                        .ToolTipText(FText::FromString(Row->Item.AssetPath))
                    ];
            }
            if (Column == ColWhy)
            {
                return SNew(SBox).Padding(FMargin(2.f)).VAlign(VAlign_Center)
                    [
                        SNew(STextBlock)
                        .Text(FriendlyReason(Row->Item))
                        .ColorAndOpacity(FSlateColor(FLinearColor(0.72f, 0.74f, 0.76f)))
                    ];
            }
            if (Column == ColWhere)
            {
                return SNew(SBox).Padding(FMargin(2.f)).VAlign(VAlign_Center)
                    [
                        SNew(STextBlock)
                        .Text(FText::FromString(Row->Item.PackagePath))
                        .ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
                    ];
            }
            if (Column == ColSize)
            {
                // Blank when the image file is not ours — that is information,
                // not an omission: only the .uasset would go.
                return SNew(SBox).Padding(FMargin(2.f)).VAlign(VAlign_Center)
                    [
                        SNew(STextBlock)
                        .Text(Row->Item.bSourceIsOurs
                            ? PrettyBytes(Row->Item.SourceBytes)
                            : LOCTEXT("FileKept", "kept"))
                        .ToolTipText(Row->Item.bSourceIsOurs
                            ? LOCTEXT("FileGoesTip",
                                "Source file is in the MapLink project and will "
                                "be deleted with the asset.")
                            : LOCTEXT("FileKeptTip",
                                "Source file is outside the MapLink project and "
                                "will be left on disk."))
                        .ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
                    ];
            }
            return SNullWidget::NullWidget;
        }

    private:
        TSharedPtr<FMapLinkCleanupRow> Row;
    };
}

// ---------------------------------------------------------------------------

void SMapLinkTextureCleanupDialog::Construct(const FArguments& InArgs)
{
    ParentWindow = InArgs._ParentWindow;

    for (const FMapLinkTextureCleanupItem& Item : InArgs._Items)
    {
        TSharedPtr<FMapLinkCleanupRow> Row = MakeShared<FMapLinkCleanupRow>();
        Row->Item = Item;
        Row->bChecked = false;   // never pre-ticked
        Rows.Add(Row);
    }

    ChildSlot
    [
        SNew(SVerticalBox)

        // ── What this is ──────────────────────────────────────────────────
        + SVerticalBox::Slot().AutoHeight().Padding(8.f, 8.f, 8.f, 4.f)
        [
            SNew(STextBlock)
            .AutoWrapText(true)
            .Text(LOCTEXT("CleanupIntro",
                "Nothing in the project references these textures. Select the "
                "ones to delete."))
        ]

        + SVerticalBox::Slot().AutoHeight().Padding(8.f, 0.f, 8.f, 6.f)
        [
            SNew(STextBlock)
            .AutoWrapText(true)
            .ColorAndOpacity(FSlateColor(FLinearColor(0.94f, 0.55f, 0.13f)))
            .Text(LOCTEXT("CleanupWarn",
                "Cannot be undone from the editor. Deletions land in the "
                "MapLink changelist."))
        ]

        // ── Bulk ticks ────────────────────────────────────────────────────
        + SVerticalBox::Slot().AutoHeight().Padding(8.f, 0.f, 8.f, 4.f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 4.f, 0.f)
            [
                SNew(SButton)
                .Text(LOCTEXT("TickSuperseded", "Select Promoted Originals"))
                .ToolTipText(LOCTEXT("TickSupersededTip",
                    "Originals that have been promoted to MapLink."))
                .OnClicked(this, &SMapLinkTextureCleanupDialog::OnSelectSuperseded)
            ]
            + SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 4.f, 0.f)
            [
                SNew(SButton)
                .Text(LOCTEXT("TickAll", "Select All"))
                .OnClicked(this, &SMapLinkTextureCleanupDialog::OnSelectAll)
            ]
            + SHorizontalBox::Slot().AutoWidth()
            [
                SNew(SButton)
                .Text(LOCTEXT("TickNone", "Select None"))
                .OnClicked(this, &SMapLinkTextureCleanupDialog::OnSelectNone)
            ]
        ]

        // ── The list ──────────────────────────────────────────────────────
        + SVerticalBox::Slot().FillHeight(1.f).Padding(8.f, 2.f)
        [
            SNew(SBorder)
            .BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
            [
                SAssignNew(ListView, SListView<TSharedPtr<FMapLinkCleanupRow>>)
                .ListItemsSource(&Rows)
                .OnGenerateRow(this, &SMapLinkTextureCleanupDialog::OnGenerateRow)
                .SelectionMode(ESelectionMode::None)
                .HeaderRow(
                    SNew(SHeaderRow)
                    + SHeaderRow::Column(ColCheck).DefaultLabel(FText::GetEmpty())
                      .FixedWidth(28.f)
                    + SHeaderRow::Column(ColName)
                      .DefaultLabel(LOCTEXT("HdrName", "Texture")).FillWidth(0.28f)
                    + SHeaderRow::Column(ColWhy)
                      .DefaultLabel(LOCTEXT("HdrWhy", "Reason")).FillWidth(0.38f)
                    + SHeaderRow::Column(ColWhere)
                      .DefaultLabel(LOCTEXT("HdrWhere", "Folder")).FillWidth(0.24f)
                    + SHeaderRow::Column(ColSize)
                      .DefaultLabel(LOCTEXT("HdrSize", "File")).FillWidth(0.10f)
                )
            ]
        ]

        + SVerticalBox::Slot().AutoHeight().Padding(8.f, 4.f)
        [
            SNew(SSeparator)
        ]

        // ── Footer ────────────────────────────────────────────────────────
        + SVerticalBox::Slot().AutoHeight().Padding(8.f, 0.f, 8.f, 8.f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
            [
                SNew(STextBlock)
                .Text(this, &SMapLinkTextureCleanupDialog::GetSummaryText)
            ]
            + SHorizontalBox::Slot().AutoWidth().Padding(4.f, 0.f, 0.f, 0.f)
            [
                SNew(SButton)
                .Text(LOCTEXT("Cancel", "Cancel"))
                .OnClicked(this, &SMapLinkTextureCleanupDialog::OnCancel)
            ]
            + SHorizontalBox::Slot().AutoWidth().Padding(4.f, 0.f, 0.f, 0.f)
            [
                SNew(SButton)
                .Text(LOCTEXT("DeleteTicked", "Delete Selected"))
                .IsEnabled(this, &SMapLinkTextureCleanupDialog::IsDeleteEnabled)
                .OnClicked(this, &SMapLinkTextureCleanupDialog::OnDelete)
            ]
        ]
    ];
}

TSharedRef<ITableRow> SMapLinkTextureCleanupDialog::OnGenerateRow(
    TSharedPtr<FMapLinkCleanupRow> Row, const TSharedRef<STableViewBase>& OwnerTable)
{
    return SNew(SCleanupRowWidget, OwnerTable).Row(Row);
}

// ---------------------------------------------------------------------------

int32 SMapLinkTextureCleanupDialog::NumChecked() const
{
    int32 N = 0;
    for (const TSharedPtr<FMapLinkCleanupRow>& Row : Rows)
    {
        if (Row->bChecked) { ++N; }
    }
    return N;
}

int64 SMapLinkTextureCleanupDialog::BytesChecked() const
{
    int64 B = 0;
    for (const TSharedPtr<FMapLinkCleanupRow>& Row : Rows)
    {
        if (Row->bChecked && Row->Item.bSourceIsOurs)
        {
            B += Row->Item.SourceBytes;
        }
    }
    return B;
}

FText SMapLinkTextureCleanupDialog::GetSummaryText() const
{
    const int32 N = NumChecked();
    if (N == 0)
    {
        return FText::Format(LOCTEXT("CleanupNoneTicked",
            "{0} unreferenced texture(s)."), FText::AsNumber(Rows.Num()));
    }

    const int64 B = BytesChecked();
    if (B > 0)
    {
        return FText::Format(LOCTEXT("CleanupTicked",
            "{0} selected — {1} of source files."),
            FText::AsNumber(N), PrettyBytes(B));
    }
    return FText::Format(LOCTEXT("CleanupTickedNoBytes",
        "{0} selected."), FText::AsNumber(N));
}

bool SMapLinkTextureCleanupDialog::IsDeleteEnabled() const
{
    return NumChecked() > 0;
}

FReply SMapLinkTextureCleanupDialog::OnSelectAll()
{
    for (TSharedPtr<FMapLinkCleanupRow>& Row : Rows) { Row->bChecked = true; }
    if (ListView.IsValid()) { ListView->RequestListRefresh(); }
    return FReply::Handled();
}

FReply SMapLinkTextureCleanupDialog::OnSelectNone()
{
    for (TSharedPtr<FMapLinkCleanupRow>& Row : Rows) { Row->bChecked = false; }
    if (ListView.IsValid()) { ListView->RequestListRefresh(); }
    return FReply::Handled();
}

FReply SMapLinkTextureCleanupDialog::OnSelectSuperseded()
{
    for (TSharedPtr<FMapLinkCleanupRow>& Row : Rows)
    {
        Row->bChecked = Row->Item.Reason == TEXT("Superseded");
    }
    if (ListView.IsValid()) { ListView->RequestListRefresh(); }
    return FReply::Handled();
}

FReply SMapLinkTextureCleanupDialog::OnDelete()
{
    Confirmed.Reset();
    for (const TSharedPtr<FMapLinkCleanupRow>& Row : Rows)
    {
        if (Row->bChecked)
        {
            Confirmed.Add(Row->Item.AssetPath);
        }
    }
    if (TSharedPtr<SWindow> Window = ParentWindow.Pin())
    {
        Window->RequestDestroyWindow();
    }
    return FReply::Handled();
}

FReply SMapLinkTextureCleanupDialog::OnCancel()
{
    Confirmed.Reset();
    if (TSharedPtr<SWindow> Window = ParentWindow.Pin())
    {
        Window->RequestDestroyWindow();
    }
    return FReply::Handled();
}

// ---------------------------------------------------------------------------

TArray<FString> SMapLinkTextureCleanupDialog::Show(
    const TArray<FMapLinkTextureCleanupItem>& Items)
{
    if (Items.Num() == 0)
    {
        return TArray<FString>();
    }

    TSharedRef<SWindow> Window = SNew(SWindow)
        .Title(LOCTEXT("CleanupTitle", "MapLink — Clean Up Textures"))
        .ClientSize(FVector2D(920.f, 560.f))
        .SupportsMaximize(false)
        .SupportsMinimize(false);

    TSharedRef<SMapLinkTextureCleanupDialog> Content =
        SNew(SMapLinkTextureCleanupDialog)
        .ParentWindow(Window)
        .Items(Items);

    Window->SetContent(Content);
    FSlateApplication::Get().AddModalWindow(Window, nullptr);

    return Content->GetConfirmedAssetPaths();
}

#undef LOCTEXT_NAMESPACE
