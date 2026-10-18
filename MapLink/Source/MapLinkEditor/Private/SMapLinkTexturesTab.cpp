// Copyright MapModelers. All Rights Reserved.
//
// v1.16 — Textures tab (see header).

#include "SMapLinkTexturesTab.h"
#include "SMapLinkTextureCleanupDialog.h"
#include "SMapLinkMaterialsTab.h"      // RequestSelectMaterials — drill-through
#include "SMapLinkManagerWindow.h"     // ActivateTab — drill-through

#include "MapLinkNotify.h"
#include "MapLinkTextureOps.h"
#include "MapLinkTexturePolicy.h"
#include "MapLinkTextureScanner.h"
#include "MapLinkTextureNaming.h"
#include "MapLinkUtils.h"
#include "MapLinkPipeline.h"           // LogMapLink

#include "Editor.h"                          // GEditor
#include "Subsystems/AssetEditorSubsystem.h" // OpenEditorForAsset
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetThumbnail.h"
#include "Engine/Texture2D.h"

#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"       // ExploreFolder
#include "Misc/MessageDialog.h"
#include "Dialog/SCustomDialog.h"   // v1.17 — conform preview table
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "PropertyEditorModule.h"
#include "Toolkits/IToolkitHost.h"
#include "EditorReimportHandler.h"
#include "Misc/PackageName.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"          // v1.18 — status badge on tiles
#include "Widgets/SNullWidget.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SHyperlink.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "SMapLinkTexturesTab"

TWeakPtr<SMapLinkTexturesTab> SMapLinkTexturesTab::LiveTab;
FString SMapLinkTexturesTab::PendingFocusTexture;

namespace
{
    // ── Column ids ────────────────────────────────────────────────────────
    const FName ColStatus (TEXT("Status"));
    const FName ColName   (TEXT("Name"));
    const FName ColRefs   (TEXT("Refs"));

    /**
     * The status palette, ordered by what each state costs somebody else.
     * Matches MapLinkEditorModule's severity colours so the tool does not
     * grow a second vocabulary of red.
     */
    FLinearColor StatusColour(EMapLinkTextureStatus Status)
    {
        switch (Status)
        {
        case EMapLinkTextureStatus::Conform:      return FLinearColor(0.35f, 0.78f, 0.42f);
        case EMapLinkTextureStatus::Plugin:       return FLinearColor(0.62f, 0.58f, 0.85f);
        case EMapLinkTextureStatus::Portable:     return FLinearColor(0.45f, 0.70f, 0.90f);
        case EMapLinkTextureStatus::LocalOnly:    return FLinearColor(0.94f, 0.55f, 0.13f);
        case EMapLinkTextureStatus::Broken:       return FLinearColor(0.86f, 0.20f, 0.16f);
        case EMapLinkTextureStatus::Unclassified:
        default:                                  return FLinearColor(0.60f, 0.60f, 0.60f);
        }
    }

    /** A glyph per status, because colour alone is not an encoding and this
     *  view is nothing but status. */
    const TCHAR* StatusGlyph(EMapLinkTextureStatus Status)
    {
        switch (Status)
        {
        case EMapLinkTextureStatus::Conform:   return TEXT("●");
        case EMapLinkTextureStatus::Plugin:    return TEXT("◆");
        case EMapLinkTextureStatus::Portable:  return TEXT("○");
        case EMapLinkTextureStatus::LocalOnly: return TEXT("▲");
        case EMapLinkTextureStatus::Broken:    return TEXT("✕");
        default:                               return TEXT("?");
        }
    }

    /**
     * The status, in the model's own vocabulary.
     *
     * MapLink and External are the two words this feature is built on — they
     * are the class names, the console commands, the JSON, and what the user
     * says out loud. Inventing a second set of words for the UI ("this PC
     * only", "ready for the team") made the tool read as if it were explaining
     * itself to a stranger. It should read as if it knows what it is doing.
     */
    FText StatusWords(EMapLinkTextureStatus Status)
    {
        switch (Status)
        {
        case EMapLinkTextureStatus::Conform:      return LOCTEXT("StMapLink", "MapLink");
        case EMapLinkTextureStatus::Plugin:       return LOCTEXT("StPlugin", "Plugin");
        case EMapLinkTextureStatus::Portable:     return LOCTEXT("StProject", "Project");
        case EMapLinkTextureStatus::LocalOnly:    return LOCTEXT("StExternal", "External");
        case EMapLinkTextureStatus::Broken:       return LOCTEXT("StMissing", "Missing");
        case EMapLinkTextureStatus::Unclassified:
        default:                                  return LOCTEXT("StUnclassified", "Unclassified");
        }
    }

    /** One line. The row already carries the state; this says what it means. */
    FText StatusTooltip(EMapLinkTextureStatus Status)
    {
        switch (Status)
        {
        // Every one of these says who can open the file, because that is the
        // only question the status axis answers and the words alone do not
        // carry it -- "Project" and "External" read as ownership until the
        // tooltip tells you they are about location and reach.
        case EMapLinkTextureStatus::Conform:
            return LOCTEXT("TipMapLink",
                "MapLink owns it. Source file in the MapLink project. "
                "Everyone on the team gets it.");
        case EMapLinkTextureStatus::Plugin:
            return LOCTEXT("TipPlugin",
                "Ships with MapLink itself. Identical in every project that "
                "has the plugin, so there is nothing to conform — and "
                "conforming it would edit the plugin's own content.");
        case EMapLinkTextureStatus::Portable:
            return LOCTEXT("TipProject",
                "Someone else's texture, but inside the project tree, so it "
                "reaches teammates as it is. Conform it only if you want "
                "MapLink to own it too.");
        case EMapLinkTextureStatus::LocalOnly:
            return LOCTEXT("TipExternal",
                "Source file outside the project. It opens on this machine "
                "and nowhere else. Conforming copies it in — this is the one "
                "that needs doing.");
        case EMapLinkTextureStatus::Broken:
            return LOCTEXT("TipMissingS",
                "Source file not found on disk.");
        default:
            return LOCTEXT("TipUnclassifiedS", "No class recorded.");
        }
    }

    /** What Conform All acts on. Used by BOTH the button's number and the
     *  button's click, because the v1.19 pair disagreed: the label counted
     *  LocalOnly + Broken + Unclassified (243) and the click took everything
     *  that was neither Conform nor Broken (248, the extra five being the
     *  plugin's own textures sitting at status Portable). Same question, two
     *  answers, and the only place the difference showed was the preview
     *  table — "Conform All (243)" opening on "248 texture(s): 248 to conform".
     *
     *  Portable IS included, and deliberately: conforming it is what the
     *  button is for when you want MapLink to own the whole set. Plugin is
     *  not, and never can be. */
    bool ConformAllTakes(EMapLinkTextureStatus Status)
    {
        return Status != EMapLinkTextureStatus::Conform
            && Status != EMapLinkTextureStatus::Broken
            && Status != EMapLinkTextureStatus::Plugin;
    }

    /** Worst first: the row that will cost a teammate an hour belongs at the
     *  top, not in alphabetical order somewhere in the middle. */
    int32 StatusSeverity(EMapLinkTextureStatus Status)
    {
        switch (Status)
        {
        case EMapLinkTextureStatus::Broken:       return 0;
        case EMapLinkTextureStatus::LocalOnly:    return 1;
        case EMapLinkTextureStatus::Unclassified: return 2;
        case EMapLinkTextureStatus::Portable:     return 3;
        case EMapLinkTextureStatus::Conform:      return 4;
        // Last: it is the only status that is not about this project at all.
        case EMapLinkTextureStatus::Plugin:       return 5;
        default:                                  return 6;
        }
    }

    /** The remedy, for the tooltip and the detail panel. Empty when there is
     *  nothing to do — callers decide what to show in that case. */
    FString RemedyOrEmpty(const TSharedPtr<FMapLinkTextureListItem>& Item)
    {
        if (!Item.IsValid()) { return FString(); }
        if (!Item->Remedy.IsEmpty()) { return Item->Remedy; }
        if (Item->bOrphan) { return TEXT("Unreferenced."); }
        return FString();
    }

    /**
     * ONE tooltip, used by the list row, every cell in it, and the tile.
     *
     * Hovering a name and hovering its thumbnail used to say different things,
     * which makes a user hunt for the spot that tells them what they want.
     * Everything a row knows goes here, and the row itself carries it, so any
     * pixel of the row gives the same answer.
     */
    FText MakeItemTooltip(const TSharedPtr<FMapLinkTextureListItem>& Item)
    {
        if (!Item.IsValid()) { return FText::GetEmpty(); }

        FString T = Item->AssetName;
        T += TEXT("\n") + Item->AssetPath;

        T += TEXT("\n\n") + StatusWords(Item->Status).ToString();
        T += TEXT("  —  ") + StatusTooltip(Item->Status).ToString();

        if (!Item->SourceDiskPath.IsEmpty())
        {
            T += TEXT("\n\nSource: ") + Item->SourceDiskPath;
        }
        if (!Item->RelativeToLibrary.IsEmpty())
        {
            T += TEXT("\nBlender: //") + Item->RelativeToLibrary;
        }

        const int32 N = Item->ReferencingMaterials.Num();
        T += FString::Printf(TEXT("\n\nUsed by %d material instance(s)"), N);

        const FString Remedy = RemedyOrEmpty(Item);
        if (!Remedy.IsEmpty())
        {
            T += TEXT("\n\n") + Remedy;
        }
        return FText::FromString(T);
    }

    // ── List row ──────────────────────────────────────────────────────────
    class SMapLinkTextureRow
        : public SMultiColumnTableRow<TSharedPtr<FMapLinkTextureListItem>>
    {
    public:
        SLATE_BEGIN_ARGS(SMapLinkTextureRow) {}
            SLATE_ARGUMENT(TSharedPtr<FMapLinkTextureListItem>, Item)
        SLATE_END_ARGS()

        void Construct(const FArguments& InArgs,
                       const TSharedRef<STableViewBase>& InOwner)
        {
            Item = InArgs._Item;
            SMultiColumnTableRow<TSharedPtr<FMapLinkTextureListItem>>::Construct(
                FSuperRowType::FArguments(), InOwner);

            // On the ROW, not on each cell: hovering anywhere in the row gives
            // the same answer, and no cell needs its own.
            SetToolTipText(MakeItemTooltip(Item));
        }

        virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& Column) override
        {
            if (!Item.IsValid())
            {
                return SNullWidget::NullWidget;
            }

            if (Column == ColStatus)
            {
                // The glyph IS the status column. Spelling the word out on
                // every row repeated the same six strings 245 times and told
                // the reader nothing the colour and shape had not.
                return SNew(SBox).Padding(FMargin(4.f, 2.f)).VAlign(VAlign_Center)
                    [
                        SNew(STextBlock)
                        .Text(FText::FromString(StatusGlyph(Item->Status)))
                        .ColorAndOpacity(FSlateColor(StatusColour(Item->Status)))
                    ];
            }
            if (Column == ColName)
            {
                return SNew(SBox).Padding(FMargin(2.f)).VAlign(VAlign_Center)
                    [
                        SNew(STextBlock).Text(FText::FromString(Item->AssetName))
                    ];
            }
            if (Column == ColRefs)
            {
                const int32 N = Item->ReferencingMaterials.Num();
                return SNew(SBox).Padding(FMargin(2.f)).VAlign(VAlign_Center)
                    [
                        SNew(STextBlock)
                        .Text(N == 0 ? LOCTEXT("RefsNone", "—") : FText::AsNumber(N))
                        .ColorAndOpacity(FSlateColor(N == 0
                            ? FLinearColor(0.6f, 0.6f, 0.6f)
                            : FLinearColor(0.85f, 0.85f, 0.85f)))
                    ];
            }
            return SNullWidget::NullWidget;
        }

    private:
        TSharedPtr<FMapLinkTextureListItem> Item;
    };
}

// ---------------------------------------------------------------------------
// Construct
// ---------------------------------------------------------------------------

void SMapLinkTexturesTab::Construct(const FArguments& InArgs)
{
    // 64 in flight is enough for a screenful of tiles with room to scroll.
    //
    // The second constructor argument is bAreRealTimeThumbnailsAllowed and it
    // MUST stay defaulted to true. Passing false was why every tile rendered
    // black: a texture has no cached thumbnail on disk, so the pool has to
    // render one, and that render is the thing the flag switches off.
    ThumbnailPool = MakeShared<FAssetThumbnailPool>(64);

    FilterModeOptions.Add(MakeShared<FString>(TEXT("Not Conform")));
    FilterModeOptions.Add(MakeShared<FString>(TEXT("All")));
    FilterModeOptions.Add(MakeShared<FString>(TEXT("External")));
    FilterModeOptions.Add(MakeShared<FString>(TEXT("Unused")));
    FilterModeOptions.Add(MakeShared<FString>(TEXT("MapLink")));
    CurrentFilterMode = FilterModeOptions[0];
    FilterMode = 0;

    SortColumn = ColStatus;
    SortMode   = EColumnSortMode::Ascending;

    ChildSlot
    [
        SNew(SSplitter)
        .Orientation(Orient_Horizontal)

        + SSplitter::Slot().Value(0.70f)
        [
            // Nothing in this column may paint outside it, whatever a child
            // decides its desired size is.
            SNew(SVerticalBox)
            .Clipping(EWidgetClipping::ClipToBounds)

            + SVerticalBox::Slot().AutoHeight().Padding(4.f, 4.f, 4.f, 2.f)
            [
                BuildHealthStrip()
            ]

            + SVerticalBox::Slot().AutoHeight().Padding(4.f, 2.f)
            [
                BuildToolbar()
            ]

            + SVerticalBox::Slot().FillHeight(1.f).Padding(4.f, 2.f, 4.f, 4.f)
            [
                BuildBrowser()
            ]
        ]

        + SSplitter::Slot().Value(0.30f)
        [
            SNew(SBox)
            .Clipping(EWidgetClipping::ClipToBounds)
            [ BuildDetailPanel() ]
        ]
    ];

    // Scan on construct, but never a FORCED one: opening a tab must not cost a
    // full recompute. Refresh is how you ask for fresh.
    RebuildFromScan(/*bForceRescan=*/false);

    LiveTab = SharedThis(this);
    if (!PendingFocusTexture.IsEmpty())
    {
        ApplyFocusTexture(PendingFocusTexture);
        PendingFocusTexture.Empty();
    }
}

// ---------------------------------------------------------------------------
// Health strip — the number the whole feature exists to move
// ---------------------------------------------------------------------------

int32 SMapLinkTexturesTab::CountNotConform() const
{
    if (!FMapLinkTextureScanner::HasResult())
    {
        return 0;
    }
    const FMapLinkTextureScanResult& R = FMapLinkTextureScanner::GetLastResult();
    return R.NumLocalOnly + R.NumBroken + R.NumUnclassified;
}

int32 SMapLinkTexturesTab::CountConformAllTargets() const
{
    int32 N = 0;
    for (const TSharedPtr<FMapLinkTextureListItem>& Item : AllItems)
    {
        if (Item.IsValid() && ConformAllTakes(Item->Status))
        {
            ++N;
        }
    }
    return N;
}

TSharedRef<SWidget> SMapLinkTexturesTab::BuildHealthStrip()
{
    // v1.17 — the most important sentence in the plugin ("237 of 245 textures
    // are External and require conforming") used to be a line of orange text
    // with no explanation and no way to act on it: the button that fixes it
    // sat two rows below, in a wrap box, next to a near-identically named
    // button that does something narrower. State the problem, say what the
    // fix does, and put the fix where the problem is stated.
    return SNew(SBorder)
        .BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
        .Padding(8.f, 6.f)
        [
            SNew(SHorizontalBox)

            + SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
            [
                SNew(SVerticalBox)

                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(STextBlock)
                    .AutoWrapText(true)
                    .Text(this, &SMapLinkTexturesTab::GetHealthText)
                    .ColorAndOpacity(this, &SMapLinkTexturesTab::GetHealthColour)
                ]

                + SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f, 0.f, 0.f)
                [
                    SNew(STextBlock)
                    .AutoWrapText(true)
                    .Visibility_Lambda([this]()
                    {
                        return CountNotConform() > 0
                            ? EVisibility::Visible : EVisibility::Collapsed;
                    })
                    .Text(LOCTEXT("TexHealthExplain",
                        "Conforming copies each source file into the MapLink "
                        "project, renames it to the MapLink convention, fixes "
                        "sRGB and compression, and repoints the material "
                        "instances that use it. Originals outside MapLink are "
                        "copied, never modified."))
                    .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                ]
            ]

            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
              .Padding(8.f, 0.f, 4.f, 0.f)
            [
                SNew(SButton)
                .Visibility_Lambda([this]()
                {
                    return CountConformAllTargets() > 0
                        ? EVisibility::Visible : EVisibility::Collapsed;
                })
                .Text_Lambda([this]()
                {
                    return FText::Format(LOCTEXT("TexConformAllN",
                        "Conform All ({0})"),
                        FText::AsNumber(CountConformAllTargets()));
                })
                .ToolTipText(LOCTEXT("TexConformAllTip",
                    "Conform every texture MapLink does not already own, "
                    "including the ones already inside the project tree. "
                    "Textures that ship with MapLink are left alone. You get a "
                    "per-texture preview before anything is written."))
                .OnClicked(this, &SMapLinkTexturesTab::OnConformAllClicked)
            ]

            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [
                SNew(SButton)
                .Text(LOCTEXT("TexRescan", "Refresh"))
                .ToolTipText(LOCTEXT("TexRescanTip", "Rescan. Read-only."))
                .OnClicked(this, &SMapLinkTexturesTab::OnScanClicked)
            ]
        ];
}

FText SMapLinkTexturesTab::GetHealthText() const
{
    if (!FMapLinkTextureScanner::HasResult())
    {
        return LOCTEXT("TexNoScan", "No scan yet.");
    }

    const FMapLinkTextureScanResult& R = FMapLinkTextureScanner::GetLastResult();

    if (R.Total() == 0)
    {
        return LOCTEXT("TexNone", "No textures referenced by MapLink materials.");
    }

    TArray<FText> Parts;

    if (R.NumLocalOnly > 0)
    {
        // The denominator is the project's textures, not the row count.
        // MapLink's own shipped ones are in the list so you can see them, but
        // counting them here would put five textures nobody can act on into
        // the one sentence whose whole job is to size the work.
        Parts.Add(FText::Format(LOCTEXT("TexExternal",
            "{0} of {1} textures are External and require conforming."),
            FText::AsNumber(R.NumLocalOnly),
            FText::AsNumber(FMath::Max(0, R.Total() - R.NumPlugin))));
    }
    if (R.NumBroken > 0)
    {
        Parts.Add(FText::Format(LOCTEXT("TexBroken",
            "{0} source file(s) missing."), FText::AsNumber(R.NumBroken)));
    }

    if (Parts.Num() == 0)
    {
        return FText::Format(LOCTEXT("TexClean",
            "{0} textures. All conform."), FText::AsNumber(R.Total()));
    }

    return FText::Join(FText::FromString(TEXT("  ")), Parts);
}

FSlateColor SMapLinkTexturesTab::GetHealthColour() const
{
    if (!FMapLinkTextureScanner::HasResult())
    {
        return FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f));
    }
    const FMapLinkTextureScanResult& R = FMapLinkTextureScanner::GetLastResult();
    if (R.NumBroken > 0)
    {
        return FSlateColor(StatusColour(EMapLinkTextureStatus::Broken));
    }
    if (R.NumLocalOnly > 0 || R.NumUnclassified > 0)
    {
        return FSlateColor(StatusColour(EMapLinkTextureStatus::LocalOnly));
    }
    return FSlateColor(StatusColour(EMapLinkTextureStatus::Conform));
}

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------

// v1.18 — the stripe under each thumbnail encodes status in colour, and
// colour with no key is decoration. StatusGlyph and StatusWords already
// existed for the list view; the thumbnail view just never showed them. One
// legend teaches the whole wall of tiles, so the stripe starts working.
TSharedRef<SWidget> SMapLinkTexturesTab::BuildStatusLegend()
{
    static const EMapLinkTextureStatus Order[] = {
        EMapLinkTextureStatus::Conform,
        EMapLinkTextureStatus::Portable,
        EMapLinkTextureStatus::Plugin,
        EMapLinkTextureStatus::LocalOnly,
        EMapLinkTextureStatus::Broken,
        EMapLinkTextureStatus::Unclassified,
    };

    TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);

    for (const EMapLinkTextureStatus Status : Order)
    {
        const FLinearColor Colour = StatusColour(Status);

        Row->AddSlot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        .Padding(0.f, 0.f, 10.f, 0.f)
        [
            SNew(SHorizontalBox)

            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
              .Padding(0.f, 0.f, 3.f, 0.f)
            [
                // Glyph, not just a swatch — it is what the tile badge shows,
                // and a legend that keys only the colour leaves the badge
                // unexplained.
                SNew(STextBlock)
                .Text(FText::FromString(StatusGlyph(Status)))
                .ColorAndOpacity(FSlateColor(Colour))
            ]

            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [
                SNew(STextBlock)
                .Text(StatusWords(Status))
                .ToolTipText(StatusTooltip(Status))
                .ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
            ]
        ];
    }

    return SNew(SBox)
        .VAlign(VAlign_Center)
        // Only in the thumbnail view: the list view already spells the status
        // out in its own column, where a legend would be noise.
        .Visibility_Lambda([this]()
        {
            return bListView ? EVisibility::Collapsed : EVisibility::Visible;
        })
        [
            Row
        ];
}

TSharedRef<SWidget> SMapLinkTexturesTab::BuildToolbar()
{
    auto OpButton = [this](const FText& Label, const FText& Tip,
                           FOnClicked OnClick, bool bNeedsSelection)
        -> TSharedRef<SWidget>
    {
        return SNew(SButton)
            .Text(Label)
            .ToolTipText(Tip)
            .OnClicked(OnClick)
            .IsEnabled_Lambda([this, bNeedsSelection]()
            {
                return !bNeedsSelection || HasSelection();
            });
    };

    return SNew(SVerticalBox)

        // ── Filter / view row ─────────────────────────────────────────────
        //
        // A wrap box, like the action row below it. An SHorizontalBox of
        // AutoWidth slots has a minimum desired width — search + filter combo
        // + two view toggles + a count is roughly 560px — and when the panel
        // is narrower than that the box does not shrink, it overflows and
        // paints outside its slot. That is the clipped toolbar in the
        // screenshot. Wrapping costs a second line at narrow widths and
        // cannot overflow at any width.
        + SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f)
        [
            SNew(SWrapBox)
            .UseAllottedSize(true)
            .InnerSlotPadding(FVector2D(6.f, 4.f))

            + SWrapBox::Slot().FillEmptySpace(true).FillLineWhenSizeLessThan(420.f)
            [
                SNew(SBox).MinDesiredWidth(140.f)
                [
                    SAssignNew(SearchBox, SSearchBox)
                    .HintText(LOCTEXT("TexSearchHint", "Search…"))
                    .OnTextChanged(this, &SMapLinkTexturesTab::OnSearchChanged)
                ]
            ]

            + SWrapBox::Slot()
            [
                SNew(SBox).MinDesiredWidth(140.f).VAlign(VAlign_Center)
                [
                    SNew(SComboBox<TSharedPtr<FString>>)
                    .OptionsSource(&FilterModeOptions)
                    .InitiallySelectedItem(CurrentFilterMode)
                    .OnSelectionChanged(this, &SMapLinkTexturesTab::OnFilterModeChanged)
                    .OnGenerateWidget_Lambda([](TSharedPtr<FString> In)
                    {
                        return SNew(STextBlock).Text(FText::FromString(*In));
                    })
                    [
                        SNew(STextBlock)
                        .Text_Lambda([this]()
                        {
                            return CurrentFilterMode.IsValid()
                                ? FText::FromString(*CurrentFilterMode)
                                : FText::GetEmpty();
                        })
                    ]
                ]
            ]

            + SWrapBox::Slot()
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [
                    SNew(SCheckBox)
                    .Style(FAppStyle::Get(), "ToggleButtonCheckbox")
                    .ToolTipText(LOCTEXT("ViewThumbsTip", "Thumbnail view."))
                    .IsChecked_Lambda([this]()
                    {
                        return bListView ? ECheckBoxState::Unchecked
                                         : ECheckBoxState::Checked;
                    })
                    .OnCheckStateChanged_Lambda([this](ECheckBoxState)
                    {
                        bListView = false;
                        RefreshViews();
                    })
                    [
                        SNew(SBox).Padding(FMargin(8.f, 3.f))
                        [ SNew(STextBlock).Text(LOCTEXT("ViewThumbs", "Thumbnails")) ]
                    ]
                ]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [
                    SNew(SCheckBox)
                    .Style(FAppStyle::Get(), "ToggleButtonCheckbox")
                    .ToolTipText(LOCTEXT("ViewListTip",
                        "List view. Click a column heading to sort."))
                    .IsChecked_Lambda([this]()
                    {
                        return bListView ? ECheckBoxState::Checked
                                         : ECheckBoxState::Unchecked;
                    })
                    .OnCheckStateChanged_Lambda([this](ECheckBoxState)
                    {
                        bListView = true;
                        RefreshViews();
                    })
                    [
                        SNew(SBox).Padding(FMargin(8.f, 3.f))
                        [ SNew(STextBlock).Text(LOCTEXT("ViewList", "List")) ]
                    ]
                ]
            ]

            + SWrapBox::Slot()
            [
                SNew(SBox).VAlign(VAlign_Center)
                [
                    SNew(STextBlock)
                    .Text_Lambda([this]()
                    {
                        return FText::Format(LOCTEXT("TexShowing", "{0} shown"),
                                             FText::AsNumber(VisibleItems.Num()));
                    })
                    .ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
                ]
            ]

            + SWrapBox::Slot()
            [
                BuildStatusLegend()
            ]
        ]

        // ── Action row ────────────────────────────────────────────────────
        + SVerticalBox::Slot().AutoHeight()
        [
            SNew(SWrapBox)
            .UseAllottedSize(true)
            .InnerSlotPadding(FVector2D(4.f, 4.f))

            + SWrapBox::Slot()
            [
                OpButton(
                    LOCTEXT("TexNew", "Import…"),
                    LOCTEXT("TexNewTip",
                        "Import an image file into MapLink and conform it."),
                    FOnClicked::CreateSP(this, &SMapLinkTexturesTab::OnImportClicked),
                    false)
            ]
            + SWrapBox::Slot()
            [
                // v1.17 — the label carries the selection count, so "Conform"
                // and "Conform All" can no longer be misread for each other:
                // one says (5), the other says (237) and lives in the banner
                // beside the sentence that explains why 237 need it.
                SNew(SButton)
                .ToolTipText(LOCTEXT("TexConformTip",
                    "Make the SELECTED textures MapLink textures, with their "
                    "source files in the MapLink project. Shows a per-texture "
                    "preview first."))
                .IsEnabled_Lambda([this]() { return HasSelection(); })
                .OnClicked(this, &SMapLinkTexturesTab::OnConformClicked)
                .Text_Lambda([this]()
                {
                    const int32 N = GetSelectedItems().Num();
                    return N > 0
                        ? FText::Format(LOCTEXT("TexConformN", "Conform ({0})"),
                                        FText::AsNumber(N))
                        : LOCTEXT("TexConform", "Conform");
                })
            ]
            + SWrapBox::Slot()
            [
                SNew(SBox).Padding(FMargin(6.f, 0.f))
                [ SNew(SSeparator).Orientation(Orient_Vertical) ]
            ]
            + SWrapBox::Slot()
            [
                OpButton(
                    LOCTEXT("TexCleanUp", "Clean Up…"),
                    LOCTEXT("TexCleanUpTip",
                        "Review and delete textures nothing references."),
                    FOnClicked::CreateSP(this, &SMapLinkTexturesTab::OnCleanUpClicked),
                    false)
            ]
        ];
}

// ---------------------------------------------------------------------------
// Browser — thumbnails or list, same rows
// ---------------------------------------------------------------------------

TSharedRef<SWidget> SMapLinkTexturesTab::BuildBrowser()
{
    return SNew(SBorder)
        .BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
        [
            SNew(SVerticalBox)

            + SVerticalBox::Slot().FillHeight(1.f)
            [
                SAssignNew(TileView, STileView<TSharedPtr<FMapLinkTextureListItem>>)
                .Visibility_Lambda([this]()
                {
                    return bListView ? EVisibility::Collapsed : EVisibility::Visible;
                })
                .ListItemsSource(&VisibleItems)
                .OnGenerateTile(this, &SMapLinkTexturesTab::OnGenerateTile)
                .OnSelectionChanged(this, &SMapLinkTexturesTab::OnSelectionChanged)
                .SelectionMode(ESelectionMode::Multi)
                .ItemWidth(148.f)
                .ItemHeight(180.f)
                .OnMouseButtonDoubleClick_Lambda(
                    [this](TSharedPtr<FMapLinkTextureListItem>) { OnOpenAssetClicked(); })
                .OnContextMenuOpening(this, &SMapLinkTexturesTab::OnContextMenu)
            ]

            + SVerticalBox::Slot().FillHeight(1.f)
            [
                SAssignNew(ListView, SListView<TSharedPtr<FMapLinkTextureListItem>>)
                .Visibility_Lambda([this]()
                {
                    return bListView ? EVisibility::Visible : EVisibility::Collapsed;
                })
                .ListItemsSource(&VisibleItems)
                .OnGenerateRow(this, &SMapLinkTexturesTab::OnGenerateListRow)
                .OnSelectionChanged(this, &SMapLinkTexturesTab::OnSelectionChanged)
                .SelectionMode(ESelectionMode::Multi)
                .OnMouseButtonDoubleClick_Lambda(
                    [this](TSharedPtr<FMapLinkTextureListItem>) { OnOpenAssetClicked(); })
                .OnContextMenuOpening(this, &SMapLinkTexturesTab::OnContextMenu)
                .HeaderRow(
                    SNew(SHeaderRow)
                    + SHeaderRow::Column(ColStatus)
                      .DefaultLabel(LOCTEXT("HdrStatus", "Status"))
                      .FixedWidth(64.f)
                      .HAlignHeader(HAlign_Center)
                      .HAlignCell(HAlign_Center)
                      .SortMode_Lambda([this]() { return GetSortModeForColumn(ColStatus); })
                      .OnSort(this, &SMapLinkTexturesTab::OnSortColumn)
                    + SHeaderRow::Column(ColName)
                      .DefaultLabel(LOCTEXT("HdrName", "Texture")).FillWidth(0.78f)
                      .SortMode_Lambda([this]() { return GetSortModeForColumn(ColName); })
                      .OnSort(this, &SMapLinkTexturesTab::OnSortColumn)
                    + SHeaderRow::Column(ColRefs)
                      .DefaultLabel(LOCTEXT("HdrRefs", "Used By")).FillWidth(0.22f)
                      .SortMode_Lambda([this]() { return GetSortModeForColumn(ColRefs); })
                      .OnSort(this, &SMapLinkTexturesTab::OnSortColumn)
                )
            ]
        ];
}

TSharedRef<ITableRow> SMapLinkTexturesTab::OnGenerateListRow(
    TSharedPtr<FMapLinkTextureListItem> Item,
    const TSharedRef<STableViewBase>& OwnerTable)
{
    return SNew(SMapLinkTextureRow, OwnerTable).Item(Item);
}

TSharedRef<ITableRow> SMapLinkTexturesTab::OnGenerateTile(
    TSharedPtr<FMapLinkTextureListItem> Item,
    const TSharedRef<STableViewBase>& OwnerTable)
{
    if (!Item.IsValid())
    {
        return SNew(STableRow<TSharedPtr<FMapLinkTextureListItem>>, OwnerTable);
    }

    // Built on first draw and cached on the item. Creating 245 thumbnails up
    // front would load every texture in the project to fill a screen that
    // shows twenty.
    if (!Item->Thumbnail.IsValid() && ThumbnailPool.IsValid())
    {
        FAssetRegistryModule& ARM =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
        const FAssetData Data = ARM.Get().GetAssetByObjectPath(
            FSoftObjectPath(Item->AssetPath));
        if (Data.IsValid())
        {
            Item->Thumbnail = MakeShared<FAssetThumbnail>(
                Data, 96, 96, ThumbnailPool);
        }
    }

    const FLinearColor Colour = StatusColour(Item->Status);

    TSharedRef<SWidget> Image = Item->Thumbnail.IsValid()
        ? Item->Thumbnail->MakeThumbnailWidget()
        : StaticCastSharedRef<SWidget>(SNew(SBox));

    return SNew(STableRow<TSharedPtr<FMapLinkTextureListItem>>, OwnerTable)
        .Padding(2.f)
        .ToolTipText(MakeItemTooltip(Item))
        [
            SNew(SBorder)
            .BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
            .Padding(3.f)
            [
                SNew(SVerticalBox)

                + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
                [
                    SNew(SBox).WidthOverride(128.f).HeightOverride(128.f)
                    [
                        // v1.18 — the status glyph, on the thumbnail itself.
                        // The stripe below encodes status as colour, which is
                        // not an encoding on its own: a colour-blind reader
                        // gets nothing from it, and on a wall of 245 tiles the
                        // eye lands on the picture, not on a 2px bar under it.
                        // Same glyph the list view has always used.
                        SNew(SOverlay)

                        + SOverlay::Slot()
                        [ Image ]

                        + SOverlay::Slot()
                        .HAlign(HAlign_Right)
                        .VAlign(VAlign_Top)
                        .Padding(2.f)
                        [
                            SNew(SBorder)
                            // Dark plate: the glyph has to stay legible over a
                            // white texture and over a black one.
                            .BorderImage(FAppStyle::Get().GetBrush("WhiteBrush"))
                            .BorderBackgroundColor(
                                FSlateColor(FLinearColor(0.f, 0.f, 0.f, 0.55f)))
                            .Padding(FMargin(3.f, 0.f))
                            .ToolTipText(StatusTooltip(Item->Status))
                            [
                                SNew(STextBlock)
                                .Text(FText::FromString(
                                    StatusGlyph(Item->Status)))
                                .ColorAndOpacity(FSlateColor(Colour))
                            ]
                        ]
                    ]
                ]

                // Status stripe. On a wall of tiles this is what you read;
                // the name is confirmation.
                + SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f, 0.f, 2.f)
                [
                    SNew(SBorder)
                    .BorderImage(FAppStyle::Get().GetBrush("WhiteBrush"))
                    .BorderBackgroundColor(FSlateColor(Colour))
                    .Padding(0.f, 1.f)
                    [ SNew(SBox).HeightOverride(2.f) ]
                ]

                // Two lines of wrapped name. Texture names are long and
                // T_Mk_-prefixed, so a single clipped line showed the prefix
                // and nothing that distinguishes one row from the next.
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(SBox).WidthOverride(136.f).HeightOverride(32.f)
                    [
                        SNew(STextBlock)
                        .Text(FText::FromString(Item->AssetName))
                        .Justification(ETextJustify::Center)
                        .WrapTextAt(134.f)
                        // THE reason wrapping did nothing: Slate breaks lines
                        // at word boundaries, and "093452_baseTexBaked" has
                        // none. Underscore is not a break opportunity, so a
                        // texture name is one unbreakable word and the wrap
                        // width was never reachable. Per-character wrapping is
                        // the policy that exists for exactly this.
                        .WrappingPolicy(ETextWrappingPolicy::AllowPerCharacterWrapping)
                        // Two lines is the budget; anything past that gets an
                        // ellipsis rather than silently vanishing. The full
                        // name is in the tooltip either way.
                        .OverflowPolicy(ETextOverflowPolicy::Ellipsis)
                        .ColorAndOpacity(FSlateColor(FLinearColor(0.85f, 0.85f, 0.85f)))
                    ]
                ]
            ]
        ];
}

// ---------------------------------------------------------------------------
// Detail panel — what a row cannot carry
// ---------------------------------------------------------------------------

TSharedRef<SWidget> SMapLinkTexturesTab::BuildDetailPanel()
{
    return SNew(SBorder)
        .BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
        .Padding(6.f)
        [
            SNew(SScrollBox)

            + SScrollBox::Slot()
            [
                SNew(SVerticalBox)

                // ── Nothing selected ──────────────────────────────────────
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(STextBlock)
                    .AutoWrapText(true)
                    .Visibility_Lambda([this]()
                    {
                        return DetailItem.IsValid() ? EVisibility::Collapsed
                                                    : EVisibility::Visible;
                    })
                    .ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
                    .Text(LOCTEXT("NoSelection", "No texture selected."))
                ]

                // ── Preview ───────────────────────────────────────────────
                + SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
                [
                    SNew(SBox)
                    .HeightOverride(192.f)
                    .Visibility_Lambda([this]()
                    {
                        return DetailItem.IsValid() ? EVisibility::Visible
                                                    : EVisibility::Collapsed;
                    })
                    [
                        SNew(SBorder)
                        .BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
                        .Padding(2.f)
                        [
                            SNew(SBox)
                            .HAlign(HAlign_Center).VAlign(VAlign_Center)
                            [
                                SAssignNew(DetailPreviewBox, SBox)
                                .WidthOverride(176.f).HeightOverride(176.f)
                            ]
                        ]
                    ]
                ]

                // ── Name + state ──────────────────────────────────────────
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(STextBlock)
                    .AutoWrapText(true)
                    .Font(FAppStyle::Get().GetFontStyle("BoldFont"))
                    .Text_Lambda([this]()
                    {
                        return DetailItem.IsValid()
                            ? FText::FromString(DetailItem->AssetName)
                            : FText::GetEmpty();
                    })
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
                [
                    SNew(STextBlock)
                    .AutoWrapText(true)
                    .Text_Lambda([this]()
                    {
                        return DetailItem.IsValid()
                            ? StatusWords(DetailItem->Status) : FText::GetEmpty();
                    })
                    .ColorAndOpacity_Lambda([this]()
                    {
                        return FSlateColor(DetailItem.IsValid()
                            ? StatusColour(DetailItem->Status)
                            : FLinearColor(0.6f, 0.6f, 0.6f));
                    })
                ]

                // ── Where ─────────────────────────────────────────────────
                + SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 2.f)
                [
                    SNew(STextBlock)
                    .AutoWrapText(true)
                    .ColorAndOpacity(FSlateColor(FLinearColor(0.72f, 0.74f, 0.76f)))
                    .Text_Lambda([this]() -> FText
                    {
                        if (!DetailItem.IsValid()) { return FText::GetEmpty(); }
                        return FText::Format(LOCTEXT("DetailFolder", "Folder: {0}"),
                            FText::FromString(DetailItem->PackagePath));
                    })
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
                [
                    SNew(STextBlock)
                    .AutoWrapText(true)
                    .ColorAndOpacity(FSlateColor(FLinearColor(0.72f, 0.74f, 0.76f)))
                    .Text_Lambda([this]() -> FText
                    {
                        if (!DetailItem.IsValid()) { return FText::GetEmpty(); }
                        return DetailItem->SourceDiskPath.IsEmpty()
                            ? LOCTEXT("DetailNoFile", "Source file: none on record")
                            : FText::Format(LOCTEXT("DetailFile", "Source file: {0}"),
                                FText::FromString(DetailItem->SourceDiskPath));
                    })
                ]

                // ── Buttons ───────────────────────────────────────────────
                + SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
                [
                    SNew(SWrapBox)
                    .UseAllottedSize(true)
                    .InnerSlotPadding(FVector2D(4.f, 4.f))
                    .Visibility_Lambda([this]()
                    {
                        return DetailItem.IsValid() ? EVisibility::Visible
                                                    : EVisibility::Collapsed;
                    })
                    + SWrapBox::Slot()
                    [
                        SNew(SButton)
                        .Text(LOCTEXT("DetailOpen", "Open texture"))
                        .OnClicked(this, &SMapLinkTexturesTab::OnOpenAssetClicked)
                    ]
                    + SWrapBox::Slot()
                    [
                        SNew(SButton)
                        .Text(LOCTEXT("DetailShowFile", "Show file"))
                        .ToolTipText(LOCTEXT("DetailShowFileTip",
                            "Open the containing folder."))
                        .OnClicked(this, &SMapLinkTexturesTab::OnRevealSelectedClicked)
                    ]
                ]

                // ── Used by ───────────────────────────────────────────────
                + SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f)
                [
                    SNew(STextBlock)
                    .Font(FAppStyle::Get().GetFontStyle("BoldFont"))
                    .Visibility_Lambda([this]()
                    {
                        return DetailItem.IsValid() ? EVisibility::Visible
                                                    : EVisibility::Collapsed;
                    })
                    .Text_Lambda([this]() -> FText
                    {
                        if (!DetailItem.IsValid()) { return FText::GetEmpty(); }
                        const int32 N = DetailItem->ReferencingMaterials.Num();
                        return N == 0
                            ? LOCTEXT("UsedByNone", "Used By: none")
                            : FText::Format(LOCTEXT("UsedBy", "Used By ({0})"),
                                            FText::AsNumber(N));
                    })
                ]
                + SVerticalBox::Slot().AutoHeight()
                [
                    SAssignNew(UsedByBox, SVerticalBox)
                ]
            ]
        ];
}

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

void SMapLinkTexturesTab::RebuildFromScan(bool bForceRescan)
{
    const FMapLinkTextureScanResult& R = bForceRescan
        ? FMapLinkTextureScanner::Rescan()
        : FMapLinkTextureScanner::Scan(/*bForceFullRecompute=*/false);

    // Keep the selected asset path across a rebuild — losing your place every
    // time you press a button makes a batch of small fixes miserable.
    const FString KeepSelected = DetailItem.IsValid() ? DetailItem->AssetPath : FString();
    DetailItem.Reset();

    AllItems.Reset(R.Rows.Num());
    for (const FMapLinkTextureRow& Row : R.Rows)
    {
        TSharedPtr<FMapLinkTextureListItem> Item = MakeShared<FMapLinkTextureListItem>();
        Item->AssetPath            = Row.AssetPath;
        Item->AssetName            = Row.AssetName;
        Item->PackagePath          = Row.PackagePath;
        Item->SourceDiskPath       = Row.SourceDiskPath;
        Item->RelativeToLibrary    = Row.RelativeToLibrary;
        Item->Remedy               = Row.Remedy;
        Item->Compression          = Row.Compression;
        Item->TextureClass         = Row.TextureClass;
        Item->SuggestedClass       = Row.SuggestedClass;
        Item->Status               = Row.Status;
        Item->bSRGB                = Row.bSRGB;
        Item->bOrphan              = Row.bOrphan;
        Item->ReferencingMaterials = Row.ReferencingMaterials;
        AllItems.Add(Item);
    }

    ApplyFilter();

    if (!KeepSelected.IsEmpty())
    {
        for (const TSharedPtr<FMapLinkTextureListItem>& Item : VisibleItems)
        {
            if (Item->AssetPath.Equals(KeepSelected, ESearchCase::IgnoreCase))
            {
                OnSelectionChanged(Item, ESelectInfo::Direct);
                break;
            }
        }
    }
}

void SMapLinkTexturesTab::ApplyFilter()
{
    VisibleItems.Reset();

    const FString Needle = FilterText.TrimStartAndEnd();

    for (const TSharedPtr<FMapLinkTextureListItem>& Item : AllItems)
    {
        bool bModeOk = true;
        switch (FilterMode)
        {
        case 0: bModeOk = Item->Status != EMapLinkTextureStatus::Conform
                       && Item->Status != EMapLinkTextureStatus::Portable
                       && Item->Status != EMapLinkTextureStatus::Plugin; break;
        case 1: bModeOk = true; break;
        case 2: bModeOk = Item->Status == EMapLinkTextureStatus::LocalOnly; break;
        case 3: bModeOk = Item->ReferencingMaterials.Num() == 0 || Item->bOrphan; break;
        case 4: bModeOk = Item->Status == EMapLinkTextureStatus::Conform; break;
        default: bModeOk = true; break;
        }
        if (!bModeOk) { continue; }

        if (!Needle.IsEmpty()
            && !Item->AssetName.Contains(Needle)
            && !Item->PackagePath.Contains(Needle)
            && !Item->SourceDiskPath.Contains(Needle))
        {
            continue;
        }

        VisibleItems.Add(Item);
    }

    SortVisibleItems();
    RefreshViews();
}

void SMapLinkTexturesTab::RefreshViews()
{
    if (ListView.IsValid()) { ListView->RequestListRefresh(); }
    if (TileView.IsValid()) { TileView->RequestListRefresh(); }
}

// ---------------------------------------------------------------------------
// Sorting
// ---------------------------------------------------------------------------

EColumnSortMode::Type SMapLinkTexturesTab::GetSortModeForColumn(FName Column) const
{
    return SortColumn == Column ? SortMode : EColumnSortMode::None;
}

void SMapLinkTexturesTab::OnSortColumn(EColumnSortPriority::Type /*Priority*/,
                                       const FName& Column,
                                       EColumnSortMode::Type NewMode)
{
    SortColumn = Column;
    SortMode   = NewMode;
    SortVisibleItems();
    RefreshViews();
}

void SMapLinkTexturesTab::SortVisibleItems()
{
    const bool bAscending = SortMode != EColumnSortMode::Descending;

    VisibleItems.Sort([this, bAscending](
        const TSharedPtr<FMapLinkTextureListItem>& A,
        const TSharedPtr<FMapLinkTextureListItem>& B)
    {
        auto Result = [bAscending](int32 Cmp) { return bAscending ? Cmp < 0 : Cmp > 0; };

        if (SortColumn == ColName)
        {
            return Result(A->AssetName.Compare(B->AssetName, ESearchCase::IgnoreCase));
        }
        if (SortColumn == ColRefs)
        {
            const int32 NA = A->ReferencingMaterials.Num();
            const int32 NB = B->ReferencingMaterials.Num();
            if (NA != NB) { return Result(NA < NB ? -1 : 1); }
            return A->AssetName < B->AssetName;
        }
        // Status and State both sort by severity: worst first ascending,
        // because "sort by status" means "show me the problems".
        const int32 SA = StatusSeverity(A->Status);
        const int32 SB = StatusSeverity(B->Status);
        if (SA != SB) { return Result(SA < SB ? -1 : 1); }
        return A->AssetName < B->AssetName;
    });
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

void SMapLinkTexturesTab::OnSelectionChanged(
    TSharedPtr<FMapLinkTextureListItem> Item, ESelectInfo::Type /*SelectInfo*/)
{
    DetailItem = Item;

    if (DetailPreviewBox.IsValid())
    {
        if (Item.IsValid() && ThumbnailPool.IsValid())
        {
            FAssetRegistryModule& ARM =
                FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
            const FAssetData Data = ARM.Get().GetAssetByObjectPath(
                FSoftObjectPath(Item->AssetPath));
            if (Data.IsValid())
            {
                DetailThumbnail = MakeShared<FAssetThumbnail>(Data, 176, 176, ThumbnailPool);
                DetailPreviewBox->SetContent(DetailThumbnail->MakeThumbnailWidget());
            }
            else
            {
                DetailPreviewBox->SetContent(SNullWidget::NullWidget);
            }
        }
        else
        {
            DetailPreviewBox->SetContent(SNullWidget::NullWidget);
        }
    }

    // Rebuild the "used by" list. Each entry is a link: the question after
    // "which materials use this" is always "show me that material".
    if (UsedByBox.IsValid())
    {
        UsedByBox->ClearChildren();

        if (Item.IsValid())
        {
            for (const FString& MatPath : Item->ReferencingMaterials)
            {
                const FString MatName = FPaths::GetBaseFilename(MatPath);
                UsedByBox->AddSlot().AutoHeight().Padding(0.f, 1.f)
                [
                    SNew(SHyperlink)
                    .Text(FText::FromString(MatName))
                    .ToolTipText(LOCTEXT("ShowInMaterials", "Show in Materials tab"))
                    .OnNavigate_Lambda([MatName]()
                    {
                        // Switch first, then select — selecting a row on a
                        // page the switcher is not showing is invisible.
                        SMapLinkManagerWindow::ActivateTab(
                            SMapLinkManagerWindow::ETab::Materials);
                        TArray<FString> Names;
                        Names.Add(MatName);
                        SMapLinkMaterialsTab::RequestSelectMaterials(Names);
                    })
                ];
            }
        }
    }
}

TArray<TSharedPtr<FMapLinkTextureListItem>> SMapLinkTexturesTab::GetSelectedItems() const
{
    if (bListView && ListView.IsValid())
    {
        return ListView->GetSelectedItems();
    }
    if (TileView.IsValid())
    {
        return TileView->GetSelectedItems();
    }
    return TArray<TSharedPtr<FMapLinkTextureListItem>>();
}

TArray<FString> SMapLinkTexturesTab::GetSelectedAssetPaths() const
{
    TArray<FString> Out;
    for (const TSharedPtr<FMapLinkTextureListItem>& Item : GetSelectedItems())
    {
        if (Item.IsValid()) { Out.Add(Item->AssetPath); }
    }
    return Out;
}

bool SMapLinkTexturesTab::HasSelection() const
{
    return GetSelectedItems().Num() > 0;
}

bool SMapLinkTexturesTab::HasSingleSelection() const
{
    return GetSelectedItems().Num() == 1;
}

// ---------------------------------------------------------------------------
// Focus request (drill-through from the Materials tab)
// ---------------------------------------------------------------------------

void SMapLinkTexturesTab::RequestFocusOnTexture(const FString& TextureAssetPath)
{
    const FString Normalised =
        FMapLinkTextureNaming::NormalizeAssetPath(TextureAssetPath);

    // Switch to this tab FIRST. Selecting a row in a page the switcher is not
    // showing is invisible, which is exactly what made the Materials tab's
    // double-click look like it did nothing.
    SMapLinkManagerWindow::ActivateTab(SMapLinkManagerWindow::ETab::Textures);

    if (TSharedPtr<SMapLinkTexturesTab> Tab = LiveTab.Pin())
    {
        Tab->ApplyFocusTexture(Normalised);
    }
    else
    {
        PendingFocusTexture = Normalised;
    }
}

void SMapLinkTexturesTab::ApplyFocusTexture(const FString& TextureAssetPath)
{
    // Show everything first: a focus request for a healthy texture would
    // otherwise land on an empty list under the default filter and look broken.
    FilterMode = 1;
    CurrentFilterMode = FilterModeOptions.IsValidIndex(1)
        ? FilterModeOptions[1] : CurrentFilterMode;
    FilterText.Empty();
    if (SearchBox.IsValid()) { SearchBox->SetText(FText::GetEmpty()); }
    ApplyFilter();

    for (const TSharedPtr<FMapLinkTextureListItem>& Item : VisibleItems)
    {
        if (Item->AssetPath.Equals(TextureAssetPath, ESearchCase::IgnoreCase))
        {
            OnSelectionChanged(Item, ESelectInfo::Direct);

            // Park the scroll. ApplyFilter has just called RequestListRefresh,
            // and the view will not have a widget for this item until the next
            // tick — scrolling now is a request the view drops on the floor.
            PendingScrollTo = Item;
            return;
        }
    }
}

void SMapLinkTexturesTab::Tick(const FGeometry& AllottedGeometry,
                               const double InCurrentTime,
                               const float InDeltaTime)
{
    SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);

    if (!PendingScrollTo.IsValid())
    {
        return;
    }

    TSharedPtr<FMapLinkTextureListItem> Target = PendingScrollTo;
    PendingScrollTo.Reset();

    if (bListView && ListView.IsValid())
    {
        ListView->SetSelection(Target);
        ListView->RequestScrollIntoView(Target);
    }
    else if (TileView.IsValid())
    {
        TileView->SetSelection(Target);
        TileView->RequestScrollIntoView(Target);
    }
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void SMapLinkTexturesTab::ReleaseThumbnails()
{
    // A conform run is a synchronous loop that reimports and rebuilds every
    // texture it touches; on a 243-texture run that is minutes of the editor
    // never returning to an idle frame. Throughout it, this tab is still
    // holding up to 64 FAssetThumbnails with real-time rendering enabled —
    // and a real-time thumbnail of an asset whose source data has just been
    // replaced re-renders. That is sustained GPU work queued behind a game
    // thread that is not pumping, on top of whatever the texture build is
    // doing. 2026-08-21: a 243-texture run died at ~163 with
    // DXGI_ERROR_DEVICE_REMOVED — RTX 2080 Max-Q, 2.1 GB of a 7.2 GB budget in
    // use, so not VRAM; no breadcrumbs, no DRED page fault, and Aftermath
    // timed out trying to dump, which is a driver reset and not a fault in
    // anything MapLink queued. This does not fix that. It removes the one
    // source of GPU work MapLink itself keeps alive across the whole run.
    //
    // The pool itself must keep bAreRealTimeThumbnailsAllowed — see Construct,
    // where turning it off is what made every tile render black. So the fix is
    // to have nothing to render for the duration, not to render it slower.
    // RebuildFromScan repopulates AllItems afterwards and the tiles come back
    // lazily, one per first draw.
    for (const TSharedPtr<FMapLinkTextureListItem>& Item : AllItems)
    {
        if (Item.IsValid())
        {
            Item->Thumbnail.Reset();
        }
    }
    DetailThumbnail.Reset();

    // Replace the pool rather than releasing it: FAssetThumbnailPool's
    // ReleaseResources() is private in 5.7, and its destructor does the same
    // job. Dropping the last reference here frees the render targets; a fresh
    // pool is ready for the tiles that come back after RebuildFromScan.
    //
    // Row widgets built before this call keep their own reference to the
    // thumbnails they were handed, and through them to the old pool, so those
    // survive until the list rebuilds. This reduces the live set to what is
    // actually on screen rather than emptying it — enough to matter over a run
    // of minutes, and it does not reach into private engine API to get there.
    ThumbnailPool = MakeShared<FAssetThumbnailPool>(64);
}

void SMapLinkTexturesTab::RunOpAndRefresh(FMapLinkTextureOpReport&& Report)
{
    // The full report goes to the Output Log because it is the copy-pasteable
    // artefact. The toast is the headline.
    TArray<FString> Lines;
    FMapLinkTextureOps::BuildReportText(Report).ParseIntoArrayLines(Lines, false);
    for (const FString& Line : Lines)
    {
        UE_LOG(LogMapLink, Display, TEXT("%s"), *Line);
    }

    FMapLinkTextureOps::NotifySummary(Report);

    // Tell the AssetRegistry to re-read what the operation touched BEFORE
    // rescanning.
    //
    // The scan derives its texture set from package-level dependencies, and
    // those come from the registry's cached view of each package header. A
    // material instance saved a moment ago still reads as depending on the
    // texture it used to point at, so a promoted original stayed on the list
    // until something else made the registry catch up — which is why it took
    // a manual Refresh to disappear.
    {
        TArray<FString> ModifiedFiles;
        auto AddPackageOf = [&ModifiedFiles](const FString& ObjectPath)
        {
            if (ObjectPath.IsEmpty()) { return; }
            const FString PackageName =
                FPackageName::ObjectPathToPackageName(ObjectPath);
            FString Filename;
            if (FPackageName::DoesPackageExist(PackageName, &Filename))
            {
                ModifiedFiles.AddUnique(Filename);
            }
        };

        for (const FMapLinkTextureOpEntry& Entry : Report.Entries)
        {
            AddPackageOf(Entry.AssetPathBefore);
            AddPackageOf(Entry.AssetPathAfter);
            for (const FString& Material : Entry.RepointedMaterials)
            {
                AddPackageOf(Material);
            }
        }

        if (ModifiedFiles.Num() > 0)
        {
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry")
                .Get().ScanModifiedAssetFiles(ModifiedFiles);
        }
    }

    RebuildFromScan(/*bForceRescan=*/true);
}

FReply SMapLinkTexturesTab::OnScanClicked()
{
    RebuildFromScan(/*bForceRescan=*/true);

    const FMapLinkTextureScanResult& R = FMapLinkTextureScanner::GetLastResult();
    const int32 NotConform = R.Total() - R.NumConform - R.NumPortable;
    if (NotConform == 0)
    {
        MapLinkNotify::Success(FText::Format(LOCTEXT("TexScanClean",
            "{0} textures scanned. All conform."), FText::AsNumber(R.Total())));
    }
    else
    {
        MapLinkNotify::Warn(FText::Format(LOCTEXT("TexScanDirty",
            "{0} of {1} textures are not conform."),
            FText::AsNumber(NotConform), FText::AsNumber(R.Total())));
    }
    return FReply::Handled();
}

/** Dry run, confirm, execute. Shared by Conform and Conform All so the two
 *  behave identically. */
// ── v1.17 — conform preview table ──────────────────────────────────────────
//
// The dry run was already the right idea: ConformAny(paths, false) produces a
// full per-texture report and nothing is written until the user says yes. What
// was wrong was the presentation. BuildReportText() flattened that report into
// a newline-joined blob, half of it went to the Output Log and the rest into an
// FMessageDialog as four summary numbers. At 237 textures "230 to conform" is
// not a preview — it is a number you either trust or do not, with no way to
// check which five are the ones that will fail.
//
// Same data, one table, failures first.

namespace
{
    namespace ConformPreviewColumns
    {
        static const FName Texture(TEXT("Texture"));
        static const FName Change(TEXT("Change"));
        static const FName Materials(TEXT("Materials"));
    }

    struct FConformPreviewRow
    {
        FString      Texture;      // asset name before
        FString      Change;       // human-readable delta
        FString      Materials;    // "3 repointed" / "—"
        FLinearColor Colour = FLinearColor::White;
        int32        Rank   = 2;   // 0 = fails, 1 = action needed, 2 = normal, 3 = skipped
    };

    class SConformPreviewRow
        : public SMultiColumnTableRow<TSharedPtr<FConformPreviewRow>>
    {
    public:
        SLATE_BEGIN_ARGS(SConformPreviewRow) {}
            SLATE_ARGUMENT(TSharedPtr<FConformPreviewRow>, Item)
        SLATE_END_ARGS()

        void Construct(const FArguments& InArgs,
                       const TSharedRef<STableViewBase>& OwnerTable)
        {
            Item = InArgs._Item;
            SMultiColumnTableRow<TSharedPtr<FConformPreviewRow>>::Construct(
                FSuperRowType::FArguments(), OwnerTable);
        }

        virtual TSharedRef<SWidget> GenerateWidgetForColumn(
            const FName& ColumnName) override
        {
            if (!Item.IsValid())
            {
                return SNullWidget::NullWidget;
            }

            FString Text;
            if      (ColumnName == ConformPreviewColumns::Texture)   { Text = Item->Texture; }
            else if (ColumnName == ConformPreviewColumns::Change)    { Text = Item->Change; }
            else if (ColumnName == ConformPreviewColumns::Materials) { Text = Item->Materials; }
            else { return SNullWidget::NullWidget; }

            return SNew(SBox)
                .VAlign(VAlign_Center)
                .Padding(FMargin(6.f, 2.f))
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(Text))
                    .ColorAndOpacity(FSlateColor(
                        ColumnName == ConformPreviewColumns::Change
                            ? Item->Colour
                            : FLinearColor(0.85f, 0.85f, 0.85f)))
                ];
        }

    private:
        TSharedPtr<FConformPreviewRow> Item;
    };

    /** One report entry → one table row. */
    TSharedPtr<FConformPreviewRow> MakePreviewRow(const FMapLinkTextureOpEntry& E)
    {
        TSharedPtr<FConformPreviewRow> Row = MakeShared<FConformPreviewRow>();
        Row->Texture = FPaths::GetBaseFilename(
            E.AssetPathBefore.IsEmpty() ? E.DiskPathBefore : E.AssetPathBefore);

        const FString NameAfter = FPaths::GetBaseFilename(E.AssetPathAfter);

        TArray<FString> Bits;

        if (!E.Errors.IsEmpty())
        {
            Row->Rank   = 0;
            Row->Colour = FLinearColor(1.f, 0.35f, 0.35f);
            Bits.Add(FString::Join(E.Errors, TEXT("; ")));
        }
        else if (E.Result == EMapLinkTextureResult::SkippedPluginContent)
        {
            Row->Rank   = 3;
            Row->Colour = FLinearColor(0.55f, 0.55f, 0.55f);
            Bits.Add(TEXT("ships with MapLink — not conformed"));
        }
        else if (E.bSkippedIdentical)
        {
            Row->Rank   = 3;
            Row->Colour = FLinearColor(0.55f, 0.55f, 0.55f);
            Bits.Add(TEXT("already conform — no write"));
        }
        else
        {
            if (E.bRenamed && !NameAfter.IsEmpty() && NameAfter != Row->Texture)
            {
                Bits.Add(FString::Printf(TEXT("rename → %s"), *NameAfter));
            }
            if (!E.DiskPathAfter.IsEmpty() && E.DiskPathAfter != E.DiskPathBefore)
            {
                Bits.Add(TEXT("source copied into the project"));
            }
            if (E.bLossyReduction)
            {
                Row->Colour = FLinearColor(1.f, 0.75f, 0.25f);
                Bits.Add(TEXT("LOSSY: reduced to 8-bit"));
            }
            if (!E.AlphaAssetPath.IsEmpty())
            {
                Bits.Add(FString::Printf(TEXT("alpha split → %s"),
                    *FPaths::GetBaseFilename(E.AlphaAssetPath)));
            }
            if (Bits.IsEmpty())
            {
                Bits.Add(TEXT("conform"));
            }
        }

        if (!E.ActionItem.IsEmpty())
        {
            // Work the tool must NOT do for you: never let it hide behind a
            // summary count.
            Row->Rank   = FMath::Min(Row->Rank, 1);
            Row->Colour = FLinearColor(1.f, 0.75f, 0.25f);
            Bits.Add(FString::Printf(TEXT("YOU: %s"), *E.ActionItem));
        }
        for (const FString& W : E.Warnings)
        {
            Bits.Add(FString::Printf(TEXT("warning: %s"), *W));
        }

        Row->Change    = FString::Join(Bits, TEXT("  ·  "));
        Row->Materials = E.RepointedMaterials.Num() > 0
            ? FString::Printf(TEXT("%d repointed"), E.RepointedMaterials.Num())
            : FString(TEXT("—"));
        return Row;
    }
}

void SMapLinkTexturesTab::RunConform(const TArray<FString>& Paths, const FText& Title)
{
    if (Paths.Num() == 0)
    {
        MapLinkNotify::Success(LOCTEXT("TexNothingToConform",
            "Nothing to conform."));
        return;
    }

    FMapLinkTextureOpReport Dry = FMapLinkTextureOps::ConformAny(Paths, false);

    TArray<FString> Lines;
    FMapLinkTextureOps::BuildReportText(Dry).ParseIntoArrayLines(Lines, false);
    for (const FString& Line : Lines)
    {
        UE_LOG(LogMapLink, Display, TEXT("%s"), *Line);
    }

    int32 Repointed = 0;
    for (const FMapLinkTextureOpEntry& E : Dry.Entries)
    {
        Repointed += E.RepointedMaterials.Num();
    }

    // Local, because ShowModal() blocks: the array outlives the list widget
    // that points at it.
    TArray<TSharedPtr<FConformPreviewRow>> PreviewRows;
    PreviewRows.Reserve(Dry.Entries.Num());
    for (const FMapLinkTextureOpEntry& E : Dry.Entries)
    {
        PreviewRows.Add(MakePreviewRow(E));
    }
    PreviewRows.Sort([](const TSharedPtr<FConformPreviewRow>& A,
                        const TSharedPtr<FConformPreviewRow>& B)
    {
        if (A->Rank != B->Rank) { return A->Rank < B->Rank; }
        return A->Texture < B->Texture;
    });

    const FText Summary = FText::FromString(FString::Printf(
        TEXT("%d texture(s): %d to conform, %d already conform, %d would fail. ")
        TEXT("%d material instance(s) repointed."),
        Paths.Num(), Dry.NumConformed, Dry.NumSkipped, Dry.NumFailed, Repointed));

    const TSharedRef<SCustomDialog> Dialog =
        SNew(SCustomDialog)
        .Title(Title)
        .Content()
        [
            SNew(SBox)
            .WidthOverride(880.f)
            .HeightOverride(460.f)
            [
                SNew(SVerticalBox)

                + SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f)
                [
                    SNew(STextBlock).Text(Summary).AutoWrapText(true)
                ]

                + SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
                [
                    SNew(STextBlock)
                    .AutoWrapText(true)
                    .Text(FText::Format(LOCTEXT("TexConformPreviewNote",
                        "Originals outside MapLink are copied, not modified. "
                        "Failures and items needing your attention are listed "
                        "first.{0}"),
                        Dry.NumConformed >= 50
                            ? LOCTEXT("TexConformPreviewLong",
                                "\n\nThis is a long run: every texture is "
                                "rewritten and reimported, and the editor does "
                                "not respond until it finishes. Do not switch "
                                "away from it.")
                            : FText::GetEmpty()))
                    .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                ]

                + SVerticalBox::Slot().FillHeight(1.f)
                [
                    SNew(SBorder)
                    .BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
                    .Padding(2.f)
                    [
                        SNew(SListView<TSharedPtr<FConformPreviewRow>>)
                        .ListItemsSource(&PreviewRows)
                        .SelectionMode(ESelectionMode::None)
                        .OnGenerateRow_Lambda(
                            [](TSharedPtr<FConformPreviewRow> Item,
                               const TSharedRef<STableViewBase>& Owner)
                            -> TSharedRef<ITableRow>
                            {
                                return SNew(SConformPreviewRow, Owner).Item(Item);
                            })
                        .HeaderRow
                        (
                            SNew(SHeaderRow)
                            + SHeaderRow::Column(ConformPreviewColumns::Texture)
                              .DefaultLabel(LOCTEXT("PrevColTex", "Texture"))
                              .FillWidth(0.28f)
                            + SHeaderRow::Column(ConformPreviewColumns::Change)
                              .DefaultLabel(LOCTEXT("PrevColChange", "What changes"))
                              .FillWidth(0.56f)
                            + SHeaderRow::Column(ConformPreviewColumns::Materials)
                              .DefaultLabel(LOCTEXT("PrevColMats", "Materials"))
                              .FillWidth(0.16f)
                        )
                    ]
                ]

                + SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(FString::Printf(
                        TEXT("Manifest: %s"), *Dry.ManifestPath)))
                    .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                ]
            ]
        ]
        .Buttons({
            SCustomDialog::FButton(LOCTEXT("TexConformGo", "Conform")),
            SCustomDialog::FButton(LOCTEXT("TexConformCancel", "Cancel"))
        });

    if (Dialog->ShowModal() != 0)
    {
        return;
    }

    ReleaseThumbnails();
    RunOpAndRefresh(FMapLinkTextureOps::ConformAny(Paths, true));
}

void SMapLinkTexturesTab::OnConformClickedVoid()
{
    OnConformClicked();
}

FReply SMapLinkTexturesTab::OnConformClicked()
{
    RunConform(GetSelectedAssetPaths(), LOCTEXT("TexConformTitle", "MapLink — Conform"));
    return FReply::Handled();
}

FReply SMapLinkTexturesTab::OnConformAllClicked()
{
    // Everything the scan says is not conform, not everything on screen: the
    // filter is a view, not a selection.
    TArray<FString> Paths;
    for (const TSharedPtr<FMapLinkTextureListItem>& Item : AllItems)
    {
        if (Item.IsValid() && ConformAllTakes(Item->Status))
        {
            Paths.Add(Item->AssetPath);
        }
    }
    RunConform(Paths, LOCTEXT("TexConformAllTitle", "MapLink — Conform All"));
    return FReply::Handled();
}

FReply SMapLinkTexturesTab::OnImportClicked()
{
    IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
    if (!Desktop)
    {
        return FReply::Handled();
    }

    const void* ParentHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(
        SharedThis(this));

    TArray<FString> Files;
    const bool bPicked = Desktop->OpenFileDialog(
        ParentHandle,
        TEXT("Import Texture"),
        FPaths::ProjectDir(),
        TEXT(""),
        TEXT("Images (*.tga;*.png;*.jpg;*.jpeg;*.bmp;*.psd;*.tif;*.tiff;*.exr)|")
        TEXT("*.tga;*.png;*.jpg;*.jpeg;*.bmp;*.psd;*.tif;*.tiff;*.exr"),
        EFileDialogFlags::Multiple,
        Files);

    if (!bPicked || Files.Num() == 0)
    {
        return FReply::Handled();
    }

    ReleaseThumbnails();
    RunOpAndRefresh(FMapLinkTextureOps::ImportAndConform(Files, true));
    return FReply::Handled();
}

FReply SMapLinkTexturesTab::OnCleanUpClicked()
{
    const TArray<FMapLinkTextureCleanupItem> Items =
        FMapLinkTextureOps::FindCleanupCandidates();

    if (Items.Num() == 0)
    {
        MapLinkNotify::Success(LOCTEXT("TexCleanNone", "Nothing to clean up."));
        return FReply::Handled();
    }

    const TArray<FString> Chosen = SMapLinkTextureCleanupDialog::Show(Items);
    if (Chosen.Num() == 0)
    {
        return FReply::Handled();
    }

    RunOpAndRefresh(FMapLinkTextureOps::DeleteCleanupItems(Chosen, true));
    return FReply::Handled();
}

FReply SMapLinkTexturesTab::OnOpenAssetClicked()
{
    if (DetailItem.IsValid() && GEditor)
    {
        if (UObject* Asset = UEditorAssetLibrary::LoadAsset(DetailItem->AssetPath))
        {
            GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()
                   ->OpenEditorForAsset(Asset);
        }
    }
    return FReply::Handled();
}

FReply SMapLinkTexturesTab::OnRevealSelectedClicked()
{
    if (!DetailItem.IsValid())
    {
        return FReply::Handled();
    }
    if (DetailItem->SourceDiskPath.IsEmpty())
    {
        MapLinkNotify::Warn(LOCTEXT("TexNoSource", "No source file on record."));
        return FReply::Handled();
    }
    if (!FPaths::FileExists(DetailItem->SourceDiskPath))
    {
        MapLinkNotify::Warn(FText::Format(LOCTEXT("TexNoFile",
            "Source file not on disk:\n{0}"),
            FText::FromString(DetailItem->SourceDiskPath)));
        return FReply::Handled();
    }
    FPlatformProcess::ExploreFolder(*DetailItem->SourceDiskPath);
    return FReply::Handled();
}

// ---------------------------------------------------------------------------
// Context menu
// ---------------------------------------------------------------------------

void SMapLinkTexturesTab::CopyToClipboard(const FString& Text) const
{
    FPlatformApplicationMisc::ClipboardCopy(*Text);
}

TArray<UObject*> SMapLinkTexturesTab::LoadSelectedAssets() const
{
    TArray<UObject*> Objects;
    for (const TSharedPtr<FMapLinkTextureListItem>& Item : GetSelectedItems())
    {
        if (Item.IsValid())
        {
            if (UObject* Asset = UEditorAssetLibrary::LoadAsset(Item->AssetPath))
            {
                Objects.Add(Asset);
            }
        }
    }
    return Objects;
}

TSharedPtr<SWidget> SMapLinkTexturesTab::OnContextMenu()
{
    const TArray<TSharedPtr<FMapLinkTextureListItem>> Selected = GetSelectedItems();
    if (Selected.Num() == 0)
    {
        return nullptr;
    }

    const bool bSingle = Selected.Num() == 1;
    TSharedPtr<FMapLinkTextureListItem> First = Selected[0];

    FMenuBuilder Menu(/*bCloseAfterSelection=*/true, nullptr);

    Menu.BeginSection(TEXT("MapLinkTextureOps"), LOCTEXT("SecOps", "MapLink"));
    {
        Menu.AddMenuEntry(
            LOCTEXT("CtxConform", "Conform"),
            LOCTEXT("CtxConformTip",
                "Make these MapLink textures with their source files in the "
                "MapLink project."),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateSP(
                this, &SMapLinkTexturesTab::OnConformClickedVoid)));
    }
    Menu.EndSection();

    Menu.BeginSection(TEXT("MapLinkTextureAsset"), LOCTEXT("SecAsset", "Asset"));
    {
        Menu.AddMenuEntry(
            bSingle ? LOCTEXT("CtxEdit", "Edit") : LOCTEXT("CtxEditN", "Edit All"),
            LOCTEXT("CtxEditTip", "Open in the texture editor."),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([this]()
            {
                TArray<UObject*> Objects = LoadSelectedAssets();
                if (Objects.Num() > 0 && GEditor)
                {
                    GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()
                           ->OpenEditorForAssets(Objects);
                }
            })));

        Menu.AddMenuEntry(
            LOCTEXT("CtxMatrix", "Edit Selection in Property Matrix"),
            LOCTEXT("CtxMatrixTip", "Edit every selected texture's properties side by side."),
            FSlateIcon(),
            FUIAction(
                FExecuteAction::CreateLambda([this]()
                {
                    TArray<UObject*> Objects = LoadSelectedAssets();
                    if (Objects.Num() == 0) { return; }
                    FModuleManager::LoadModuleChecked<FPropertyEditorModule>(
                        "PropertyEditor").CreatePropertyEditorToolkit(
                            TSharedPtr<IToolkitHost>(), Objects);
                }),
                FCanExecuteAction::CreateLambda([this]()
                {
                    return GetSelectedItems().Num() > 1;
                })));

        Menu.AddMenuEntry(
            LOCTEXT("CtxBrowse", "Show in Content Browser"),
            LOCTEXT("CtxBrowseTip", "Select these assets in the Content Browser."),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([this]()
            {
                TArray<FAssetData> Assets;
                FAssetRegistryModule& ARM =
                    FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
                for (const TSharedPtr<FMapLinkTextureListItem>& Item : GetSelectedItems())
                {
                    const FAssetData Data = ARM.Get().GetAssetByObjectPath(
                        FSoftObjectPath(Item->AssetPath));
                    if (Data.IsValid()) { Assets.Add(Data); }
                }
                if (Assets.Num() > 0)
                {
                    FModuleManager::LoadModuleChecked<FContentBrowserModule>(
                        "ContentBrowser").Get().SyncBrowserToAssets(Assets);
                }
            })));

        Menu.AddMenuEntry(
            LOCTEXT("CtxReimport", "Reimport"),
            LOCTEXT("CtxReimportTip", "Re-read each texture from its source file."),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([this]()
            {
                for (UObject* Asset : LoadSelectedAssets())
                {
                    FReimportManager::Instance()->Reimport(
                        Asset, /*bAskForNewFileIfMissing=*/false,
                        /*bShowNotification=*/true);
                }
                RebuildFromScan(/*bForceRescan=*/true);
            })));
    }
    Menu.EndSection();

    Menu.BeginSection(TEXT("MapLinkTextureFile"), LOCTEXT("SecFile", "Source File"));
    {
        Menu.AddMenuEntry(
            LOCTEXT("CtxShowFile", "Show in Explorer"),
            LOCTEXT("CtxShowFileTip", "Open the containing folder."),
            FSlateIcon(),
            FUIAction(
                FExecuteAction::CreateLambda([this]()
                {
                    OnRevealSelectedClicked();
                }),
                FCanExecuteAction::CreateLambda([this, First]()
                {
                    return First.IsValid() && !First->SourceDiskPath.IsEmpty();
                })));

        Menu.AddMenuEntry(
            LOCTEXT("CtxExternalEdit", "Open in External Editor"),
            LOCTEXT("CtxExternalEditTip",
                "Open the source file in whatever the system uses for it."),
            FSlateIcon(),
            FUIAction(
                FExecuteAction::CreateLambda([First]()
                {
                    if (First.IsValid() && FPaths::FileExists(First->SourceDiskPath))
                    {
                        FPlatformProcess::LaunchFileInDefaultExternalApplication(
                            *First->SourceDiskPath);
                    }
                }),
                FCanExecuteAction::CreateLambda([First]()
                {
                    return First.IsValid() && !First->SourceDiskPath.IsEmpty()
                        && FPaths::FileExists(First->SourceDiskPath);
                })));
    }
    Menu.EndSection();

    Menu.BeginSection(TEXT("MapLinkTextureCopy"), LOCTEXT("SecCopy", "Copy"));
    {
        // Multi-select copies one per line, so a selection pastes as a list.
        Menu.AddMenuEntry(
            LOCTEXT("CtxCopyName", "Copy Name"),
            FText::GetEmpty(), FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([this]()
            {
                TArray<FString> Lines;
                for (const TSharedPtr<FMapLinkTextureListItem>& I : GetSelectedItems())
                {
                    Lines.Add(I->AssetName);
                }
                CopyToClipboard(FString::Join(Lines, TEXT("\r\n")));
            })));

        Menu.AddMenuEntry(
            LOCTEXT("CtxCopyPath", "Copy Asset Path"),
            FText::GetEmpty(), FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([this]()
            {
                TArray<FString> Lines;
                for (const TSharedPtr<FMapLinkTextureListItem>& I : GetSelectedItems())
                {
                    Lines.Add(I->AssetPath);
                }
                CopyToClipboard(FString::Join(Lines, TEXT("\r\n")));
            })));

        Menu.AddMenuEntry(
            LOCTEXT("CtxCopyFile", "Copy Source File Path"),
            FText::GetEmpty(), FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([this]()
            {
                TArray<FString> Lines;
                for (const TSharedPtr<FMapLinkTextureListItem>& I : GetSelectedItems())
                {
                    if (!I->SourceDiskPath.IsEmpty()) { Lines.Add(I->SourceDiskPath); }
                }
                CopyToClipboard(FString::Join(Lines, TEXT("\r\n")));
            })));
    }
    Menu.EndSection();

    return Menu.MakeWidget();
}

void SMapLinkTexturesTab::OnSearchChanged(const FText& Text)
{
    FilterText = Text.ToString();
    ApplyFilter();
}

void SMapLinkTexturesTab::OnFilterModeChanged(TSharedPtr<FString> NewMode,
                                              ESelectInfo::Type)
{
    if (!NewMode.IsValid()) { return; }
    CurrentFilterMode = NewMode;
    FilterMode = FilterModeOptions.IndexOfByKey(NewMode);
    if (FilterMode == INDEX_NONE) { FilterMode = 1; }
    ApplyFilter();
}

#undef LOCTEXT_NAMESPACE
