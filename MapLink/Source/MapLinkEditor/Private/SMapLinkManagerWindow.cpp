// Copyright MapModelers. All Rights Reserved.
//
// v1.9 — MapLink Manager window (see header).

#include "SMapLinkManagerWindow.h"
#include "MapLinkNotify.h"
#include "MapLinkDiagnostics.h"   // v1.13 — Copy Diagnostics

#include "MapLinkEditorModule.h"       // v1.11 — OpenAssetViewer entry point
#include "MapLinkPipeline.h"
#include "MapLinkUtils.h"
#include "MapLinkBlenderBridge.h"
#include "MapLinkProxyAsset.h"
#include "MapLinkSettings.h"
#include "MapLinkEditorState.h"
#include "MapLinkWPExpansionHelper.h"
#include "MapLinkBPStandInCreator.h"   // RefreshStandIn (Update Stand-in)
#include "MapLinkStandInReport.h"      // FMapLinkStandInReport
#include "MapLinkProjectSetup.h"       // v1.12 — Project tab (New/Open/Unlink)
#include "MapLinkMaterialDisplay.h"    // v1.12 — MapLink menu (MI display level)
#include "MapLinkMaterialExporter.h"   // v1.10 — Resync (material sync engine)
#include "SMapLinkMaterialsTab.h"      // v1.10 — Materials tab (MIM Drop 2)
#include "SMapLinkTexturesTab.h"      // v1.16 — Textures tab (Texture Manager)
#include "SMapLinkAssetInspector.h"  // v1.19 — Assets tab inspector
#include "MapLinkTextureScanner.h"    // v1.16 — tab badge (last scan, never triggers one)

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "IDetailsView.h"
#include "PropertyCustomizationHelpers.h"

#include "Engine/World.h"
#include "Engine/Blueprint.h"          // Cast<UBlueprint> for stand-in refresh
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"  // OpenEditorForAsset (Proxy button)
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"   // v1.17 — Open Level from an expansion row
#include "FileHelpers.h"        // v1.17 — FEditorFileUtils::LoadMap
#include "Misc/Crc.h"            // v1.9 — FCrc::StrCrc32 for auto-refresh signature
#include "Misc/MessageDialog.h"
#include "Misc/ScopedSlowTask.h"
#include "Dialog/SCustomDialog.h"   // v1.14 — Force Full Reimport confirm

#include "Styling/AppStyle.h"
#include "DesktopPlatformModule.h"              // v1.12 — MapLink menu Import .blend…
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h" // v1.12 — FMenuBuilder
#include "HAL/PlatformProcess.h"                // v1.12 — ExploreFolder
#include "Widgets/SNullWidget.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"   // v1.17 — expansion label
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Images/SImage.h"        // v1.19 — icon buttons
#include "Widgets/Layout/SSplitter.h"     // v1.19 — list | inspector
#include "Widgets/Layout/SWrapBox.h"      // v1.19 — filter bar
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Views/SHeaderRow.h"

#define LOCTEXT_NAMESPACE "SMapLinkManagerWindow"

// ---------------------------------------------------------------------------
// Column ids
// ---------------------------------------------------------------------------

namespace MapLinkManagerColumns
{
    static const FName Favourite(TEXT("Favourite"));
    static const FName Name(TEXT("Name"));
    static const FName Type(TEXT("Type"));
    static const FName State(TEXT("State"));
    static const FName LastImport(TEXT("LastImport"));
    static const FName Auto(TEXT("Auto"));
    static const FName Recursive(TEXT("Recursive"));
    static const FName Expansion(TEXT("Expansion"));   // v1.12
    static const FName Skip(TEXT("Skip"));              // v1.13.1
}

// ---------------------------------------------------------------------------
// v1.17 — small display helpers
// ---------------------------------------------------------------------------

namespace MapLinkManagerUtil
{
    /** "just now" / "14 min ago" / "3 h ago" / "2 d ago" / "2026-08-08".
     *  Takes the UTC instant the proxy stored; compares against UTC now, so
     *  the local-time display preference cannot skew the delta. */
    inline FString RelativeTime(const FDateTime& UtcInstant)
    {
        const FTimespan Delta = FDateTime::UtcNow() - UtcInstant;

        if (Delta.GetTicks() < 0)
        {
            return TEXT("just now");        // clock skew — do not print "-3 h ago"
        }
        const int32 Minutes = static_cast<int32>(Delta.GetTotalMinutes());
        if (Minutes < 1)   { return TEXT("just now"); }
        if (Minutes < 60)  { return FString::Printf(TEXT("%d min ago"), Minutes); }

        const int32 Hours = static_cast<int32>(Delta.GetTotalHours());
        if (Hours < 24)    { return FString::Printf(TEXT("%d h ago"), Hours); }

        const int32 Days = static_cast<int32>(Delta.GetTotalDays());
        if (Days < 7)      { return FString::Printf(TEXT("%d d ago"), Days); }

        return UtcInstant.ToString(TEXT("%Y-%m-%d"));
    }
}

// ---------------------------------------------------------------------------
// Row widget
// ---------------------------------------------------------------------------

class SMapLinkManagerTableRow
    : public SMultiColumnTableRow<TSharedPtr<FMapLinkManagerRow>>
{
public:
    SLATE_BEGIN_ARGS(SMapLinkManagerTableRow) {}
        SLATE_ARGUMENT(TSharedPtr<FMapLinkManagerRow>, Item)
        SLATE_ARGUMENT(TWeakPtr<SMapLinkManagerWindow>, OwnerWindow)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs,
                   const TSharedRef<STableViewBase>& OwnerTable)
    {
        Item        = InArgs._Item;
        OwnerWindow = InArgs._OwnerWindow;
        SMultiColumnTableRow<TSharedPtr<FMapLinkManagerRow>>::Construct(
            FSuperRowType::FArguments(), OwnerTable);
    }

    virtual TSharedRef<SWidget> GenerateWidgetForColumn(
        const FName& ColumnName) override
    {
        if (!Item.IsValid())
        {
            return SNullWidget::NullWidget;
        }

        if (ColumnName == MapLinkManagerColumns::Favourite)
        {
            return SNew(SBox)
                .VAlign(VAlign_Center)
                .HAlign(HAlign_Center)
                [
                    SNew(SButton)
                    .ButtonStyle(FAppStyle::Get(), "HoverHintOnly")
                    .ToolTipText(LOCTEXT("FavTip",
                        "Pin as favourite (favourites stay at the top)."))
                    .OnClicked_Lambda([this]() -> FReply
                    {
                        if (TSharedPtr<SMapLinkManagerWindow> Window =
                                OwnerWindow.Pin())
                        {
                            Window->RowToggleFavourite(Item);
                        }
                        return FReply::Handled();
                    })
                    [
                        SNew(STextBlock)
                        .Text_Lambda([this]()
                        {
                            return Item->bFavourite
                                ? FText::FromString(TEXT("\u2605"))   // ★
                                : FText::FromString(TEXT("\u2606"));  // ☆
                        })
                        .ColorAndOpacity_Lambda([this]()
                        {
                            return Item->bFavourite
                                ? FSlateColor(FLinearColor(1.f, 0.82f, 0.f))
                                : FSlateColor(FLinearColor(0.5f, 0.5f, 0.5f));
                        })
                    ]
                ];
        }

        if (ColumnName == MapLinkManagerColumns::Name)
        {
            return SNew(SBox)
                .VAlign(VAlign_Center)
                .Padding(FMargin(4.f, 2.f))
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(Item->BlendName))
                    .ToolTipText(FText::FromString(Item->BlendPath))
                ];
        }

        if (ColumnName == MapLinkManagerColumns::Type)
        {
            return SNew(SBox)
                .VAlign(VAlign_Center)
                .HAlign(HAlign_Center)
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(Item->TypeStr))
                ];
        }

        if (ColumnName == MapLinkManagerColumns::State)
        {
            // v1.15.1 — up to date reads as REGULAR text (default
            // foreground, not green); only problem states get a color.
            FSlateColor Color = FSlateColor::UseForeground();
            if (Item->bMissing)        Color = FSlateColor(FLinearColor(0.9f, 0.25f, 0.25f));
            else if (Item->bOrphanBP)  Color = FSlateColor(FLinearColor(0.9f, 0.45f, 0.2f));
            else if (Item->bOutOfDate) Color = FSlateColor(FLinearColor(1.0f, 0.62f, 0.3f));  // slight orange

            return SNew(SBox)
                .VAlign(VAlign_Center)
                .Padding(FMargin(4.f, 2.f))
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(Item->StateStr))
                    // v1.18 — the evidence behind the verdict: which two
                    // timestamps were compared, or which hash differed.
                    .ToolTipText(FText::FromString(Item->StateTooltipStr))
                    .ColorAndOpacity(Color)
                ];
        }

        if (ColumnName == MapLinkManagerColumns::LastImport)
        {
            return SNew(SBox)
                .VAlign(VAlign_Center)
                .Padding(FMargin(4.f, 2.f))
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(Item->LastImportStr))
                    // v1.17 — exact instant on hover; the cell itself answers
                    // "is this recent?" without arithmetic.
                    .ToolTipText(FText::FromString(Item->LastImportAbsStr))
                ];
        }

        if (ColumnName == MapLinkManagerColumns::Auto)
        {
            return SNew(SBox)
                .VAlign(VAlign_Center)
                .HAlign(HAlign_Center)
                [
                    SNew(SCheckBox)
                    .ToolTipText(LOCTEXT("AutoTip",
                        "Auto-reimport when the source .blend changes.\n"
                        "Disabled while Skip is on — Skip shields this asset "
                        "from every automatic reimport path."))
                    // v1.17 — Skip overrides Auto, so Auto stops being a live
                    // control when Skip is on.  It used to stay clickable and
                    // do nothing, which is the grid failing to express a rule
                    // that only lived in the tooltip.
                    .IsEnabled_Lambda([this]() -> bool
                    {
                        UMapLinkProxyAsset* Proxy = Item->Proxy.Get();
                        return Proxy && !Proxy->bSkipAutoReimport;
                    })
                    .IsChecked_Lambda([this]() -> ECheckBoxState
                    {
                        UMapLinkProxyAsset* Proxy = Item->Proxy.Get();
                        return (Proxy && Proxy->bAutoUpdate)
                            ? ECheckBoxState::Checked
                            : ECheckBoxState::Unchecked;
                    })
                    .OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
                    {
                        if (TSharedPtr<SMapLinkManagerWindow> Window =
                                OwnerWindow.Pin())
                        {
                            Window->RowSetAutoUpdate(
                                Item, NewState == ECheckBoxState::Checked);
                        }
                    })
                ];
        }

        if (ColumnName == MapLinkManagerColumns::Recursive)
        {
            return SNew(SBox)
                .VAlign(VAlign_Center)
                .HAlign(HAlign_Center)
                [
                    SNew(SCheckBox)
                    .ToolTipText(LOCTEXT("RecursiveTip",
                        "Recursive auto-reimport: also reimport this asset "
                        "when ANY .blend in its dependency tree changes "
                        "(requires Auto)."))
                    .IsEnabled_Lambda([this]() -> bool
                    {
                        UMapLinkProxyAsset* Proxy = Item->Proxy.Get();
                        return Proxy && Proxy->bAutoUpdate &&
                               !Proxy->bSkipAutoReimport;
                    })
                    .IsChecked_Lambda([this]() -> ECheckBoxState
                    {
                        UMapLinkProxyAsset* Proxy = Item->Proxy.Get();
                        return (Proxy && Proxy->bAutoUpdateRecursive)
                            ? ECheckBoxState::Checked
                            : ECheckBoxState::Unchecked;
                    })
                    .OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
                    {
                        if (TSharedPtr<SMapLinkManagerWindow> Window =
                                OwnerWindow.Pin())
                        {
                            Window->RowSetRecursive(
                                Item, NewState == ECheckBoxState::Checked);
                        }
                    })
                ];
        }

        // v1.13.1 — Skip AutoReimport: shields this asset from ALL automatic
        // reimport paths (own Auto watch AND parent Recursive cascades).
        if (ColumnName == MapLinkManagerColumns::Skip)
        {
            return SNew(SBox)
                .VAlign(VAlign_Center)
                .HAlign(HAlign_Center)
                [
                    SNew(SCheckBox)
                    .ToolTipText(LOCTEXT("SkipTip",
                        "Skip AutoReimport: this asset's .blend changes never "
                        "trigger an automatic reimport — not via its own Auto "
                        "watch, and not as a child of a Recursive parent. "
                        "Manual reimport is unaffected."))
                    .IsChecked_Lambda([this]() -> ECheckBoxState
                    {
                        UMapLinkProxyAsset* Proxy = Item->Proxy.Get();
                        return (Proxy && Proxy->bSkipAutoReimport)
                            ? ECheckBoxState::Checked
                            : ECheckBoxState::Unchecked;
                    })
                    .OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
                    {
                        if (TSharedPtr<SMapLinkManagerWindow> Window =
                                OwnerWindow.Pin())
                        {
                            Window->RowSetSkipAutoReimport(
                                Item, NewState == ECheckBoxState::Checked);
                        }
                    })
                ];
        }

        // v1.12 — the per-row Actions button strip was removed. Actions run
        // on the list selection (action bar above the list) and on the row
        // context menu; double-click opens the Asset Viewer.
        // v1.17 — the Expansion cell is interactive.  It was read-only text
        // while expansion lived in a detached panel below the list; now each
        // record is a chip that focuses it in the Expansions tab, and LI_
        // rows carry a "+" that opens the Expand form pre-filled.
        if (ColumnName == MapLinkManagerColumns::Expansion)
        {
            const bool bIsLI = (Item->TypeStr == TEXT("LI"));
            if (!bIsLI)
            {
                return SNullWidget::NullWidget;
            }

            TSharedRef<SHorizontalBox> Chips = SNew(SHorizontalBox);

            for (const FMapLinkManagerRow::FChip& Chip : Item->ExpansionChips)
            {
                const FGuid ChipId = Chip.Id;
                Chips->AddSlot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                .Padding(0.f, 0.f, 3.f, 0.f)
                [
                    SNew(SButton)
                    .ButtonStyle(FAppStyle::Get(), "SimpleButton")
                    .ContentPadding(FMargin(4.f, 0.f))
                    .ToolTipText(LOCTEXT("ExpansionChipTip",
                        "Show this expansion in the Expansions tab."))
                    .OnClicked_Lambda([this, ChipId]()
                    {
                        if (TSharedPtr<SMapLinkManagerWindow> Owner =
                                OwnerWindow.Pin())
                        {
                            Owner->FocusExpansion(ChipId);
                        }
                        return FReply::Handled();
                    })
                    [
                        SNew(STextBlock)
                        .Text(FText::FromString(Chip.Text))
                        .ColorAndOpacity(FSlateColor(
                            FLinearColor(0.45f, 0.75f, 1.f)))
                    ]
                ];
            }

            const FString RootName = Item->BlendName;
            Chips->AddSlot()
            .AutoWidth()
            .VAlign(VAlign_Center)
            [
                SNew(SButton)
                .ButtonStyle(FAppStyle::Get(), "SimpleButton")
                .ContentPadding(FMargin(4.f, 0.f))
                .ToolTipText(LOCTEXT("ExpansionAddTip",
                    "Expand this LI_ into a World Partition level. A root can "
                    "be expanded into several levels, and into one level more "
                    "than once."))
                .OnClicked_Lambda([this, RootName]()
                {
                    if (TSharedPtr<SMapLinkManagerWindow> Owner =
                            OwnerWindow.Pin())
                    {
                        Owner->BeginExpandForRoot(RootName);
                    }
                    return FReply::Handled();
                })
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("ExpansionAdd", "+"))
                    .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                ]
            ];

            return SNew(SBox)
                .VAlign(VAlign_Center)
                .Padding(FMargin(4.f, 2.f))
                [
                    Chips
                ];
        }

        return SNullWidget::NullWidget;
    }

private:
    TSharedPtr<FMapLinkManagerRow>   Item;
    TWeakPtr<SMapLinkManagerWindow>  OwnerWindow;
};

// ---------------------------------------------------------------------------
// Construct
// ---------------------------------------------------------------------------

void SMapLinkManagerWindow::Construct(const FArguments& InArgs)
{
    // v1.9 — one-time migration of legacy per-proxy expansion flags into
    // UMapLinkEditorState records.
    FMapLinkWPExpansionHelper::MigrateLegacyProxyExpansionFlags();

    // v1.17 — give pre-v1.17 expansion records an Id.  Loss-free, idempotent,
    // and it must happen before anything reads a record: the Id is what
    // cleanup matches on.
    UMapLinkEditorState::Get()->UpgradeExpansionRecords();

    // Defaults from editor state
    UMapLinkEditorState* State = UMapLinkEditorState::Get();
    SelectedWPLevelPath = State->LastWPLevel.IsNull()
        ? FString()
        : State->LastWPLevel.ToString();
    ExpandDepth = FMath::Clamp(State->LastExpandDepth, 1, 10);

    // Default sort: problem states first (rank), then name.
    SortColumn = MapLinkManagerColumns::State;
    SortMode   = EColumnSortMode::Ascending;

    // v1.12 — land on the Project tab when nothing is linked yet; on the
    // Assets tab during normal work.
    ActiveTabIndex = static_cast<int32>(
        FMapLinkProjectSetup::IsSetupActive(nullptr) ? ETab::Assets : ETab::Project);

    RefreshRows(/*bVerifyHashes=*/false);

    // v1.9 — auto-refresh timer (mtime heuristic only; never MD5).
    // v1.9.4 — honour the configured poll interval instead of a hardcoded
    // 2 s, so the idle stat-walk frequency is user-tunable (Project Settings
    // > Plugins > MapLink > Auto-Update Poll Interval).  Floor at 2 s.
    const float AutoRefreshPeriod = FMath::Max(2.0f,
        UMapLinkSettings::Get()->AutoUpdatePollInterval);
    RegisterActiveTimer(AutoRefreshPeriod,
        FWidgetActiveTimerDelegate::CreateSP(
            this, &SMapLinkManagerWindow::OnAutoRefreshTimer));

    ChildSlot
    [
        SNew(SVerticalBox)

        // ── Tab strip ────────────────────────────────────────────────
        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(6.f, 6.f, 6.f, 0.f)
        [
            SNew(SHorizontalBox)

            // v1.17 — five near-identical 25-line SCheckBox blocks collapsed
            // into MakeTabButton().  Adding the Expansions tab was the third
            // time this boilerplate had been copied; the sixth copy would
            // have been the one where an index quietly disagreed with the
            // switcher slot below.
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(0.f, 0.f, 4.f, 0.f)
            [
                MakeTabButton(ETab::Project, LOCTEXT("ProjectTab", "Project"))
            ]

            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(0.f, 0.f, 4.f, 0.f)
            [
                // v1.13 — surface the out-of-date count without opening the
                // list: "Assets (3⚠)".  AllRows is kept fresh by the
                // auto-refresh signature pass, so this is a free O(n) count
                // at draw time — no scanning.
                MakeTabButton(ETab::Assets, TAttribute<FText>::Create(
                    TAttribute<FText>::FGetter::CreateLambda([this]()
                    {
                        int32 OOD = 0;
                        for (const TSharedPtr<FMapLinkManagerRow>& Row : AllRows)
                        {
                            if (Row.IsValid() && Row->bOutOfDate && !Row->bMissing)
                            {
                                ++OOD;
                            }
                        }
                        return OOD > 0
                            ? FText::Format(LOCTEXT("AssetsTabOOD",
                                  "Assets ({0}\u26A0)"), FText::AsNumber(OOD))
                            : LOCTEXT("AssetsTab", "Assets");
                    })),
                    TAttribute<FText>::Create(
                        TAttribute<FText>::FGetter::CreateLambda([this]()
                    {
                        int32 OOD = 0;
                        for (const TSharedPtr<FMapLinkManagerRow>& Row : AllRows)
                        {
                            if (Row.IsValid() && Row->bOutOfDate && !Row->bMissing)
                            {
                                ++OOD;
                            }
                        }
                        return FText::Format(LOCTEXT("AssetsTabTip",
                            "{0} imported .blend(s); {1} out of date."),
                            FText::AsNumber(AllRows.Num()),
                            FText::AsNumber(OOD));
                    })))
            ]

            // v1.17 — Expansions is its own tab.  It sits right after Assets
            // because that is the order of the workflow: import the .blend,
            // then decide where it lands in the world.
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(0.f, 0.f, 4.f, 0.f)
            [
                MakeTabButton(ETab::Expansions, TAttribute<FText>::Create(
                    TAttribute<FText>::FGetter::CreateLambda([]()
                    {
                        const int32 Num =
                            UMapLinkEditorState::Get()->ActiveExpansions.Num();
                        return Num > 0
                            ? FText::Format(LOCTEXT("ExpansionsTabN",
                                  "Expansions ({0})"), FText::AsNumber(Num))
                            : LOCTEXT("ExpansionsTab", "Expansions");
                    })))
            ]

            + SHorizontalBox::Slot()
            .AutoWidth()
            [
                MakeTabButton(ETab::Materials,
                              LOCTEXT("MaterialsTab", "Materials"))
            ]

            // v1.16 — Textures sits next to Materials because that is where
            // the problem it solves shows up: a material is broken for a
            // teammate BECAUSE one of its textures is local-only.
            + SHorizontalBox::Slot()
            .AutoWidth()
            [
                // Same badge idiom as the Assets tab: the count of textures
                // that will be missing on somebody else's machine, without
                // having to open the tab. Free — it reads the last scan, it
                // never triggers one.
                MakeTabButton(ETab::Textures, TAttribute<FText>::Create(
                    TAttribute<FText>::FGetter::CreateLambda([]()
                    {
                        if (!FMapLinkTextureScanner::HasResult())
                        {
                            return LOCTEXT("TexturesTab", "Textures");
                        }
                        const FMapLinkTextureScanResult& R =
                            FMapLinkTextureScanner::GetLastResult();
                        const int32 AtRisk = R.NumLocalOnly + R.NumBroken
                                           + R.NumUnclassified;
                        return AtRisk > 0
                            ? FText::Format(LOCTEXT("TexturesTabWarn",
                                  "Textures ({0}\u26A0)"), FText::AsNumber(AtRisk))
                            : LOCTEXT("TexturesTab", "Textures");
                    })),
                    TAttribute<FText>::Create(
                        TAttribute<FText>::FGetter::CreateLambda([]()
                    {
                        if (!FMapLinkTextureScanner::HasResult())
                        {
                            return LOCTEXT("TexturesTabTipNoScan",
                                "No texture scan yet.");
                        }
                        const FMapLinkTextureScanResult& R =
                            FMapLinkTextureScanner::GetLastResult();
                        return FText::Format(LOCTEXT("TexturesTabTip",
                            "{0} textures; {1} would be missing on another "
                            "machine."),
                            FText::AsNumber(R.Total()),
                            FText::AsNumber(R.NumLocalOnly + R.NumBroken
                                          + R.NumUnclassified));
                    })))
            ]

            + SHorizontalBox::Slot()
            .AutoWidth()
            [
                MakeTabButton(ETab::Settings,
                              LOCTEXT("SettingsTab", "Settings"))
            ]

            // v1.12 — the MapLink main-menu entries, reachable even when the
            // Manager floats in its own window (which has no main menu bar).
            + SHorizontalBox::Slot()
            .FillWidth(1.f)
            .HAlign(HAlign_Right)
            .VAlign(VAlign_Center)
            [
                BuildMapLinkMenuButton()
            ]
        ]

        + SVerticalBox::Slot()
        .AutoHeight()
        .Padding(6.f, 4.f)
        [
            SNew(SSeparator)
        ]

        // ── Tab content ─────────────────────────────────────────────
        + SVerticalBox::Slot()
        .FillHeight(1.f)
        [
            SNew(SWidgetSwitcher)
            .WidgetIndex_Lambda([this]() { return ActiveTabIndex; })

            + SWidgetSwitcher::Slot()
            [
                BuildProjectTab()
            ]

            + SWidgetSwitcher::Slot()
            [
                BuildAssetsTab()
            ]

            + SWidgetSwitcher::Slot()
            [
                BuildExpansionsTab()
            ]

            + SWidgetSwitcher::Slot()
            [
                BuildMaterialsTab()
            ]

            // SWidgetSwitcher slots are POSITIONAL — there is no .Index().
            // Slot order MUST match the ETab enum, or every tab button points
            // at the wrong page.  MakeTabButton() takes ETab rather than a
            // raw int so the buttons at least cannot drift among themselves.
            + SWidgetSwitcher::Slot()
            [
                BuildTexturesTab()
            ]

            + SWidgetSwitcher::Slot()
            [
                BuildSettingsTab()
            ]
        ]

        // ── Context strip (v1.18) ────────────────────────────────────
        + SVerticalBox::Slot()
        .AutoHeight()
        [
            BuildContextStrip()
        ]
    ];

    // v1.16 — publish this instance so cross-tab navigation can reach it.
    // Last, so the widget tree is fully built first.
    LiveWindow = SharedThis(this);
}

// ---------------------------------------------------------------------------
// Project tab (v1.12)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// v1.17 — Project status card
// ---------------------------------------------------------------------------

TSharedRef<SWidget> SMapLinkManagerWindow::MakeStatusLine(
    const FText& Label,
    TAttribute<FText> Value,
    TAttribute<FSlateColor> Colour,
    ETab GoToTab,
    const FText& Tooltip)
{
    const int32 TabIndex = static_cast<int32>(GoToTab);

    return SNew(SHorizontalBox)

        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        [
            SNew(SBox)
            .WidthOverride(130.f)
            [
                SNew(STextBlock)
                .Text(Label)
                .ColorAndOpacity(FSlateColor::UseSubduedForeground())
            ]
        ]

        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        [
            SNew(SButton)
            .ButtonStyle(FAppStyle::Get(), "SimpleButton")
            .ContentPadding(FMargin(2.f, 1.f))
            .ToolTipText(Tooltip)
            .OnClicked_Lambda([this, TabIndex]()
            {
                ActiveTabIndex = TabIndex;
                return FReply::Handled();
            })
            [
                SNew(STextBlock)
                .Text(Value)
                .ColorAndOpacity(Colour)
            ]
        ];
}

TSharedRef<SWidget> SMapLinkManagerWindow::BuildProjectStatusCard()
{
    static const FSlateColor Good(FLinearColor(0.45f, 0.85f, 0.45f));
    static const FSlateColor Warn(FLinearColor(1.f,   0.75f, 0.25f));

    return SNew(SVerticalBox)

        + SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
        [
            SNew(STextBlock)
            .Text(LOCTEXT("ProjectStatus", "Status"))
            .Font(FAppStyle::GetFontStyle("BoldFont"))
        ]

        // Assets
        + SVerticalBox::Slot().AutoHeight().Padding(0.f, 1.f)
        [
            MakeStatusLine(
                LOCTEXT("StatAssets", "Assets"),
                TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateLambda(
                    [this]()
                    {
                        int32 OOD = 0, Missing = 0;
                        for (const TSharedPtr<FMapLinkManagerRow>& Row : AllRows)
                        {
                            if (!Row.IsValid()) continue;
                            if (Row->bMissing)        { ++Missing; }
                            else if (Row->bOutOfDate) { ++OOD; }
                        }
                        FString Text = FString::Printf(TEXT("%d imported"),
                                                       AllRows.Num());
                        if (OOD > 0)
                        {
                            Text += FString::Printf(TEXT(" · %d out of date"), OOD);
                        }
                        if (Missing > 0)
                        {
                            Text += FString::Printf(TEXT(" · %d missing .blend"),
                                                    Missing);
                        }
                        if (OOD == 0 && Missing == 0 && AllRows.Num() > 0)
                        {
                            Text += TEXT(" · all up to date");
                        }
                        return FText::FromString(Text);
                    })),
                TAttribute<FSlateColor>::Create(
                    TAttribute<FSlateColor>::FGetter::CreateLambda([this]()
                    {
                        for (const TSharedPtr<FMapLinkManagerRow>& Row : AllRows)
                        {
                            if (Row.IsValid() && (Row->bOutOfDate || Row->bMissing))
                            {
                                return Warn;
                            }
                        }
                        return Good;
                    })),
                ETab::Assets,
                LOCTEXT("StatAssetsTip", "Open the Assets tab."))
        ]

        // Textures
        + SVerticalBox::Slot().AutoHeight().Padding(0.f, 1.f)
        [
            MakeStatusLine(
                LOCTEXT("StatTextures", "Textures"),
                TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateLambda(
                    []()
                    {
                        if (!FMapLinkTextureScanner::HasResult())
                        {
                            return LOCTEXT("StatTexNoScan", "not scanned yet");
                        }
                        const FMapLinkTextureScanResult& R =
                            FMapLinkTextureScanner::GetLastResult();
                        const int32 AtRisk = R.NumLocalOnly + R.NumBroken
                                           + R.NumUnclassified;
                        return AtRisk > 0
                            ? FText::FromString(FString::Printf(
                                TEXT("%d · %d need conforming"),
                                R.Total(), AtRisk))
                            : FText::FromString(FString::Printf(
                                TEXT("%d · all conform"), R.Total()));
                    })),
                TAttribute<FSlateColor>::Create(
                    TAttribute<FSlateColor>::FGetter::CreateLambda([]()
                    {
                        if (!FMapLinkTextureScanner::HasResult())
                        {
                            return FSlateColor::UseForeground();
                        }
                        const FMapLinkTextureScanResult& R =
                            FMapLinkTextureScanner::GetLastResult();
                        return (R.NumLocalOnly + R.NumBroken + R.NumUnclassified) > 0
                            ? Warn : Good;
                    })),
                ETab::Textures,
                LOCTEXT("StatTexturesTip",
                    "Open the Textures tab. A texture that is not conform is a "
                    "texture your teammate will not see."))
        ]

        // Material library
        + SVerticalBox::Slot().AutoHeight().Padding(0.f, 1.f)
        [
            MakeStatusLine(
                LOCTEXT("StatMaterials", "Material library"),
                TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateLambda(
                    []()
                    {
                        return FMapLinkMaterialExporter::IsLibraryDirty()
                            ? LOCTEXT("StatMatDirty", "OUT OF SYNC — resync")
                            : LOCTEXT("StatMatClean", "in sync with Blender");
                    })),
                TAttribute<FSlateColor>::Create(
                    TAttribute<FSlateColor>::FGetter::CreateLambda([]()
                    {
                        return FMapLinkMaterialExporter::IsLibraryDirty()
                            ? Warn : Good;
                    })),
                ETab::Materials,
                LOCTEXT("StatMaterialsTip", "Open the Materials tab."))
        ]

        // Expansions
        + SVerticalBox::Slot().AutoHeight().Padding(0.f, 1.f)
        [
            MakeStatusLine(
                LOCTEXT("StatExpansions", "Expansions"),
                TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateLambda(
                    []()
                    {
                        const UMapLinkEditorState* State =
                            UMapLinkEditorState::Get();
                        const int32 Num = State->ActiveExpansions.Num();
                        if (Num == 0)
                        {
                            return LOCTEXT("StatExpNone", "none");
                        }
                        TSet<FString> Levels;
                        int32 Paused = 0;
                        for (const FMapLinkWPExpansionRecord& Rec :
                             State->ActiveExpansions)
                        {
                            Levels.Add(Rec.GetLevelName());
                            if (!Rec.bReExpandOnImport) { ++Paused; }
                        }
                        FString Text = FString::Printf(TEXT("%d in %d level%s"),
                            Num, Levels.Num(), Levels.Num() == 1 ? TEXT("") : TEXT("s"));
                        if (Paused > 0)
                        {
                            Text += FString::Printf(TEXT(" · %d paused"), Paused);
                        }
                        return FText::FromString(Text);
                    })),
                TAttribute<FSlateColor>::Create(
                    TAttribute<FSlateColor>::FGetter::CreateLambda([]()
                    {
                        return FSlateColor::UseForeground();
                    })),
                ETab::Expansions,
                LOCTEXT("StatExpansionsTip", "Open the Expansions tab."))
        ];
}

TSharedRef<SWidget> SMapLinkManagerWindow::BuildProjectTab()
{
    return SNew(SVerticalBox)

    // ── Linked-project panel (visible when Active) ──────────────────
    + SVerticalBox::Slot()
    .AutoHeight()
    .Padding(6.f, 8.f)
    [
        SNew(SBorder)
        .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
        .Padding(12.f)
        .Visibility_Lambda([]()
        {
            return FMapLinkProjectSetup::IsSetupActive(nullptr)
                ? EVisibility::Visible : EVisibility::Collapsed;
        })
        [
            SNew(SVerticalBox)

            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(STextBlock)
                .Text(LOCTEXT("ProjectLinked", "MapLink Project"))
                .Font(FAppStyle::GetFontStyle("BoldFont"))
            ]

            + SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .Text_Lambda([]() -> FText
                {
                    FString Root;
                    FMapLinkProjectSetup::IsSetupActive(&Root);
                    return FText::Format(LOCTEXT("ProjectRootFmt",
                        "Root:  {0}\nUE content:  /Game/MapLink/"),
                        FText::FromString(Root));
                })
            ]

            // v1.17 — the status card.  The first tab of the tool was two
            // paths and two buttons: prime real estate answering a question
            // nobody has twice.  Every number here already exists somewhere
            // in the window; the point is that it is on the page you land on,
            // and that clicking it takes you to the tab that fixes it.
            + SVerticalBox::Slot().AutoHeight().Padding(0.f, 12.f, 0.f, 0.f)
            [
                BuildProjectStatusCard()
            ]

            + SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
            [
                SNew(SHorizontalBox)

                + SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 8.f, 0.f)
                [
                    SNew(SButton)
                    .Text(LOCTEXT("ProjectExplore", "Open Root in Explorer"))
                    .OnClicked_Lambda([]() -> FReply
                    {
                        FString Root;
                        if (FMapLinkProjectSetup::IsSetupActive(&Root))
                        {
                            FPlatformProcess::ExploreFolder(*Root);
                        }
                        return FReply::Handled();
                    })
                ]

                + SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 8.f, 0.f)
                [
                    // v1.17 — every support exchange starts with this data.
                    // It lived only in the MapLink menu; it belongs on the
                    // page a confused user lands on.
                    SNew(SButton)
                    .Text(LOCTEXT("ProjectDiag", "Copy Diagnostics"))
                    .ToolTipText(LOCTEXT("ProjectDiagTip",
                        "Copy plugin/addon versions, paths and settings to the "
                        "clipboard — paste this into a bug report."))
                    .OnClicked_Lambda([]() -> FReply
                    {
                        FMapLinkDiagnostics::CopyToClipboard();
                        return FReply::Handled();
                    })
                ]

                + SHorizontalBox::Slot().AutoWidth()
                [
                    SNew(SButton)
                    .Text(LOCTEXT("ProjectUnlink", "Unlink…"))
                    .ToolTipText(LOCTEXT("ProjectUnlinkTip",
                        "Detach this UE project from the MapLink project. "
                        "You choose whether the data stays (default) or is "
                        "deleted entirely for a fresh start."))
                    .OnClicked(this, &SMapLinkManagerWindow::OnUnlinkProjectClicked)
                ]
            ]
        ]
    ]

    // ── No-project / broken panel (visible when NOT Active) ────────
    + SVerticalBox::Slot()
    .AutoHeight()
    .Padding(6.f, 8.f)
    [
        SNew(SBorder)
        .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
        .Padding(12.f)
        .Visibility_Lambda([]()
        {
            return FMapLinkProjectSetup::IsSetupActive(nullptr)
                ? EVisibility::Collapsed : EVisibility::Visible;
        })
        [
            SNew(SVerticalBox)

            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .Text_Lambda([]() -> FText
                {
                    if (FMapLinkProjectSetup::GetProjectState()
                        == EMapLinkProjectState::Broken)
                    {
                        return LOCTEXT("BrokenProjectPanel",
                            "This UE project contains MapLink content, but its "
                            "MapLink project root is missing (moved, deleted or "
                            "not synced). Locate the root folder to re-attach.");
                    }
                    return LOCTEXT("NoProjectPanel",
                        "No MapLink project in this UE project. Create a new "
                        "one, or open an existing MapLink project root.");
                })
            ]

            + SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
            [
                SNew(SHorizontalBox)

                + SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 8.f, 0.f)
                [
                    SNew(SButton)
                    .Text_Lambda([]() -> FText
                    {
                        return FMapLinkProjectSetup::GetProjectState()
                                == EMapLinkProjectState::Broken
                            ? LOCTEXT("LocateProjectRoot", "Locate Project Root…")
                            : LOCTEXT("OpenExistingProject", "Open Existing…");
                    })
                    .ToolTipText(LOCTEXT("OpenExistingTip",
                        "Pick an existing MapLink project root folder (the one "
                        "containing maplink_project.json); it is validated and "
                        "its settings are applied automatically."))
                    .OnClicked(this, &SMapLinkManagerWindow::OnOpenProjectClicked)
                ]

                + SHorizontalBox::Slot().AutoWidth()
                [
                    SNew(SButton)
                    .Text(LOCTEXT("NewProjectPanelBtn", "New MapLink Project…"))
                    .ToolTipText(LOCTEXT("NewProjectPanelTip",
                        "Scaffold a fresh MapLink authoring tree and link this "
                        "UE project to it."))
                    .OnClicked(this, &SMapLinkManagerWindow::OnNewProjectClicked)
                ]
            ]
        ]
    ]

    // filler
    + SVerticalBox::Slot().FillHeight(1.f) [ SNullWidget::NullWidget ];
}

// ---------------------------------------------------------------------------
// MapLink menu button (v1.12)
// ---------------------------------------------------------------------------

TSharedRef<SWidget> SMapLinkManagerWindow::BuildMapLinkMenuButton()
{
    return SNew(SComboButton)
        .ButtonContent()
        [
            SNew(STextBlock).Text(LOCTEXT("MapLinkMenuBtn", "MapLink"))
        ]
        .OnGetMenuContent_Lambda([this]()
        {
            FMenuBuilder MB(/*bCloseAfterSelection=*/true, nullptr);

            MB.BeginSection(NAME_None, LOCTEXT("MenuImport", "Import"));
            MB.AddMenuEntry(
                LOCTEXT("MenuImportBlend", "Import .blend…"),
                LOCTEXT("MenuImportBlendTip", "Import a .blend file via MapLink."),
                FSlateIcon(),
                FUIAction(FExecuteAction::CreateLambda([]()
                {
                    TArray<FString> Filenames;
                    if (FDesktopPlatformModule::Get()->OpenFileDialog(
                            FSlateApplication::Get()
                                .FindBestParentWindowHandleForDialogs(nullptr),
                            TEXT("Select .blend file"), FPaths::ProjectDir(),
                            TEXT(""), TEXT("Blender files (*.blend)|*.blend"),
                            EFileDialogFlags::None, Filenames)
                        && Filenames.Num() > 0)
                    {
                        FMapLinkPipeline::RunImport(Filenames[0]);
                    }
                })));
            MB.EndSection();

            MB.BeginSection(NAME_None, LOCTEXT("MenuMaterials", "Materials"));
            MB.AddMenuEntry(
                LOCTEXT("MenuUpdateMIDisplay", "Update L_MaterialInstances"),
                LOCTEXT("MenuUpdateMIDisplayTip", "Create/update the MI display level."),
                FSlateIcon(),
                FUIAction(FExecuteAction::CreateLambda([]()
                {
                    FMapLinkMaterialDisplay::UpdateMaterialDisplayLevel();
                })));
            MB.AddMenuEntry(
                LOCTEXT("MenuOpenMIBlend", "Open Material Instance Blend"),
                LOCTEXT("MenuOpenMIBlendTip",
                    "Open Library_MaterialInstances.blend in Blender."),
                FSlateIcon(),
                FUIAction(FExecuteAction::CreateLambda([]()
                {
                    FMapLinkProjectSetup::OpenMaterialInstanceLibrary();
                })));
            MB.AddMenuEntry(
                LOCTEXT("MenuOpenMILevel", "Open L_MaterialInstances (Level)"),
                LOCTEXT("MenuOpenMILevelTip",
                    "Open the MI display level in the editor."),
                FSlateIcon(),
                FUIAction(FExecuteAction::CreateLambda([]()
                {
                    FMapLinkProjectSetup::OpenMaterialInstancesLevel();
                })));
            MB.EndSection();

            MB.BeginSection(NAME_None, LOCTEXT("MenuHelp", "Help"));
            MB.AddMenuEntry(
                LOCTEXT("MenuInstallAddon", "Install / Update Blender Addon"),
                LOCTEXT("MenuInstallAddonTip",
                    "Install or update the bundled MapLink Blender addon."),
                FSlateIcon(),
                FUIAction(FExecuteAction::CreateLambda([]()
                {
                    FMapLinkProjectSetup::InstallBlenderAddon();
                })));
            MB.AddMenuEntry(
                LOCTEXT("MenuCopyDiag", "Copy Diagnostics"),
                LOCTEXT("MenuCopyDiagTip",
                    "Copy a markdown support report to the clipboard."),
                FSlateIcon(),
                FUIAction(FExecuteAction::CreateLambda([]()
                {
                    FMapLinkDiagnostics::CopyToClipboard();
                })));
            MB.EndSection();

            return MB.MakeWidget();
        });
}

// ---------------------------------------------------------------------------
// Assets tab
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// v1.19 — Assets toolbar and filter bar
// ---------------------------------------------------------------------------

TSharedRef<SWidget> SMapLinkManagerWindow::MakeIconButton(
    const FSlateIcon& Icon,
    const FText& Label,
    const FText& Tooltip,
    FOnClicked OnClicked,
    TAttribute<bool> IsEnabled,
    bool bAlwaysShowLabel)
{
    // FMapLinkEditorModule::GetIcon returns an EMPTY FSlateIcon when the art
    // is not on disk — deliberately, so a missing file falls back rather than
    // logging a lookup failure every frame. That makes icon-only buttons a
    // trap: an absent SVG becomes a blank clickable rectangle. So the label
    // is dropped only when there is genuinely an icon to drop it for.
    const bool bHasIcon  = Icon.IsSet();
    const bool bShowText = bAlwaysShowLabel || !bHasIcon;

    TSharedRef<SHorizontalBox> Content = SNew(SHorizontalBox);

    if (bHasIcon)
    {
        Content->AddSlot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        .Padding(0.f, 0.f, bShowText ? 4.f : 0.f, 0.f)
        [
            SNew(SImage)
            .Image(Icon.GetIcon())
            .ColorAndOpacity(FSlateColor::UseForeground())
        ];
    }
    if (bShowText)
    {
        Content->AddSlot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        [
            SNew(STextBlock).Text(Label)
        ];
    }

    return SNew(SButton)
        .ButtonStyle(FAppStyle::Get(), "SimpleButton")
        .ContentPadding(FMargin(6.f, 3.f))
        // Icon-only buttons live or die by the tooltip, so it always carries
        // the label as well as the explanation.
        .ToolTipText(bShowText
            ? Tooltip
            : FText::Format(NSLOCTEXT("MapLink", "IconBtnTipFmt", "{0}\n\n{1}"),
                            Label, Tooltip))
        .IsEnabled(IsEnabled)
        .OnClicked(OnClicked)
        [
            Content
        ];
}

TSharedRef<SWidget> SMapLinkManagerWindow::MakeToolbarSeparator()
{
    return SNew(SBox)
        .Padding(FMargin(5.f, 2.f))
        [
            SNew(SSeparator).Orientation(Orient_Vertical)
        ];
}

bool SMapLinkManagerWindow::IsInspectorVisible() const
{
    return UMapLinkEditorState::Get()->bShowAssetInspector;
}

void SMapLinkManagerWindow::SyncInspectorToSelection()
{
    if (!AssetInspector.IsValid())
    {
        return;
    }

    if (!IsInspectorVisible())
    {
        AssetInspector->Sleep();
        return;
    }

    // Single-selection only, by design: a preview of seven assets is not a
    // preview, and summed statistics answer a question nobody asked. The
    // panel says so itself rather than showing the first of the seven.
    const TArray<TSharedPtr<FMapLinkManagerRow>> Selected = GetSelectedRows();
    AssetInspector->SetInspectedStem(
        Selected.Num() == 1 && Selected[0].IsValid()
            ? Selected[0]->BlendName
            : FString());
}

TSharedRef<SWidget> SMapLinkManagerWindow::BuildAssetsToolbar()
{
    // v1.19 — the top bar is what you reach for while working a list: the
    // per-selection verbs. Reimport Out-of-Date, Refresh, Verify, Warm Cache
    // and Auto moved to the maintenance bar under the list — they are things
    // you do to the PROJECT, occasionally, not to the row you just clicked.
    return SNew(SHorizontalBox)

    // ── Contextual: appears only with a selection ────────────────────
    //
    // Eight permanently-greyed buttons read as a wall waiting to activate.
    // Showing the group only when it applies is the difference between a
    // toolbar that looks designed and one that looks unfinished.
    + SHorizontalBox::Slot().AutoWidth()
    [
        SNew(SHorizontalBox)
        .Visibility_Lambda([this]()
        {
            return HasAnySelection() ? EVisibility::Visible
                                     : EVisibility::Collapsed;
        })

        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
          .Padding(0.f, 0.f, 6.f, 0.f)
        [
            SNew(STextBlock)
            .Text_Lambda([this]()
            {
                return FText::Format(LOCTEXT("BarSelFmt", "{0} selected"),
                    FText::AsNumber(GetSelectedRows().Num()));
            })
            .ColorAndOpacity(FSlateColor::UseSubduedForeground())
        ]

        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
        [
            MakeIconButton(
                FMapLinkEditorModule::GetIcon("MapLink.Reimport"),
                LOCTEXT("SelReimport", "Reimport"),
                LOCTEXT("SelReimportTip",
                    "Reimport every selected .blend (changed dependencies "
                    "only)."),
                FOnClicked::CreateSP(this, &SMapLinkManagerWindow::OnSelReimport),
                TAttribute<bool>(this, &SMapLinkManagerWindow::HasAnySelection))
        ]

        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
        [
            MakeIconButton(
                FMapLinkEditorModule::GetIcon("MapLink.ReimportForce"),
                LOCTEXT("SelForce", "Force Full Reimport"),
                LOCTEXT("SelForceTip",
                    "Re-import every file in each selected tree, ignoring "
                    "hashes (nuke-and-recreate). Destructive — it confirms "
                    "first."),
                FOnClicked::CreateSP(this,
                    &SMapLinkManagerWindow::OnSelForceReimport),
                TAttribute<bool>(this, &SMapLinkManagerWindow::HasAnySelection))
        ]

        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
        [
            MakeIconButton(
                FMapLinkEditorModule::GetIcon("MapLink.EditBlend"),
                LOCTEXT("SelBlender", "Open in Blender"),
                LOCTEXT("SelBlenderTip", "Open this asset's .blend in Blender."),
                FOnClicked::CreateSP(this, &SMapLinkManagerWindow::OnSelBlender),
                TAttribute<bool>(this, &SMapLinkManagerWindow::HasSingleSelection))
        ]

        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
        [
            MakeIconButton(
                FMapLinkEditorModule::GetIcon("MapLink.Folder"),
                LOCTEXT("SelExplorer", "Show in Explorer"),
                LOCTEXT("SelExplorerTip",
                    "Show the source .blend in the file browser."),
                FOnClicked::CreateSP(this, &SMapLinkManagerWindow::OnSelExplorer),
                TAttribute<bool>(this, &SMapLinkManagerWindow::HasSingleSelection))
        ]

        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
        [
            MakeIconButton(
                FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Search"),
                LOCTEXT("SelBrowse", "Browse to Asset"),
                LOCTEXT("SelBrowseTip",
                    "Select the generated assets in the Content Browser."),
                FOnClicked::CreateSP(this, &SMapLinkManagerWindow::OnSelBrowse),
                TAttribute<bool>(this, &SMapLinkManagerWindow::HasSingleSelection))
        ]

        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
        [
            MakeIconButton(
                FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Visibility"),
                LOCTEXT("SelViewer", "Open in Asset Viewer"),
                LOCTEXT("SelViewerTip",
                    "Open the standalone Asset Viewer — the full-size version "
                    "of the inspector, with camera options."),
                FOnClicked::CreateSP(this, &SMapLinkManagerWindow::OnSelViewer),
                TAttribute<bool>(this, &SMapLinkManagerWindow::HasAnySelection))
        ]

        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
        [
            MakeIconButton(
                // The gear. Three dots means "more actions" everywhere else
                // in this editor, and this is not a menu — it opens the proxy
                // asset, the object that holds this asset's settings.
                FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Settings"),
                LOCTEXT("SelProxy", "Open Proxy Asset"),
                LOCTEXT("SelProxyTip",
                    "Open the MapLink proxy asset itself."),
                FOnClicked::CreateSP(this, &SMapLinkManagerWindow::OnSelProxy),
                TAttribute<bool>(this, &SMapLinkManagerWindow::HasSingleSelection))
        ]

        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
        [
            MakeIconButton(
                FSlateIcon(),
                LOCTEXT("SelStandIn", "Update Stand-in"),
                LOCTEXT("SelStandInTip",
                    "Refresh the Blueprint stand-ins of the selected BP_ "
                    "assets."),
                FOnClicked::CreateSP(this,
                    &SMapLinkManagerWindow::OnSelUpdateStandIn),
                TAttribute<bool>(this, &SMapLinkManagerWindow::SelectionAllBP))
        ]
    ]

    // Nothing selected: say what the bar is for rather than showing a blank
    // strip. An empty toolbar reads as broken; an empty toolbar with one
    // grey sentence reads as waiting.
    + SHorizontalBox::Slot()
    .AutoWidth()
    .VAlign(VAlign_Center)
    [
        SNew(STextBlock)
        .Text(LOCTEXT("BarNoSelection", "Select an asset to act on it"))
        .ColorAndOpacity(FSlateColor::UseSubduedForeground())
        .Visibility_Lambda([this]()
        {
            return HasAnySelection() ? EVisibility::Collapsed
                                     : EVisibility::Visible;
        })
    ]

    // ── Right: the inspector toggle ──────────────────────────────────
    + SHorizontalBox::Slot()
    .FillWidth(1.f)
    .HAlign(HAlign_Right)
    .VAlign(VAlign_Center)
    [
        SNew(SCheckBox)
        .Style(FAppStyle::Get(), "ToggleButtonCheckbox")
        .ToolTipText(LOCTEXT("BarInspectorTip",
            "Inspector\n\nA 3D preview and an information panel for the "
            "selected asset.\n\nOff by default and remembered per user: the "
            "preview loads assets, so an always-on inspector would make "
            "clicking down a hundred-row list expensive. Single selection "
            "only."))
        .IsChecked_Lambda([this]()
        {
            return IsInspectorVisible() ? ECheckBoxState::Checked
                                        : ECheckBoxState::Unchecked;
        })
        .OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
        {
            UMapLinkEditorState* State = UMapLinkEditorState::Get();
            State->bShowAssetInspector = (NewState == ECheckBoxState::Checked);
            State->Save();
            SyncInspectorToSelection();
        })
        [
            SNew(SBox).Padding(FMargin(8.f, 3.f))
            [ SNew(STextBlock).Text(LOCTEXT("BarInspector", "Inspector")) ]
        ]
    ];
}

TSharedRef<SWidget> SMapLinkManagerWindow::BuildAssetsMaintenanceBar()
{
    // v1.19 — project-wide maintenance, under the list, sharing the row with
    // the totals it acts on: "106 out of date" and the button that fixes it
    // read as one sentence, which they were never able to do while the button
    // sat at the top of the tab and the count at the bottom.
    //
    // These left the top bar because they are not what you do while working a
    // list. They are what you do to the whole project, occasionally, and they
    // were crowding out the per-selection verbs that ARE the moment-to-moment
    // work.
    return SNew(SHorizontalBox)

    + SHorizontalBox::Slot()
    .FillWidth(1.f)
    .VAlign(VAlign_Center)
    [
        SNew(STextBlock)
        .Text(this, &SMapLinkManagerWindow::GetAssetTotalsText)
        .ColorAndOpacity(FSlateColor::UseSubduedForeground())
    ]

    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
    [
        MakeIconButton(
            FMapLinkEditorModule::GetIcon("MapLink.Reimport"),
            LOCTEXT("BarReimportOOD", "Reimport Out-of-Date"),
            LOCTEXT("BarReimportOODTip",
                "Reimport every asset whose source .blend has changed."),
            FOnClicked::CreateSP(this,
                &SMapLinkManagerWindow::OnReimportOutOfDateClicked),
            true, /*bAlwaysShowLabel=*/true)
    ]

    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
    [
        MakeIconButton(
            FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Refresh"),
            LOCTEXT("BarRefresh", "Refresh"),
            LOCTEXT("BarRefreshTip",
                "Re-scan proxy state. Read-only, and cheap — the mtime "
                "heuristic, not MD5."),
            FOnClicked::CreateSP(this,
                &SMapLinkManagerWindow::OnManualRefreshClicked),
            true, /*bAlwaysShowLabel=*/true)
    ]

    + SHorizontalBox::Slot().AutoWidth()[ MakeToolbarSeparator() ]

        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
    [
        MakeIconButton(
            // Not the gear: the gear now marks "open the proxy ASSET", which
            // is the settings-shaped thing here. Verify is a comparison, so
            // it takes the warning glyph — it is the action that tells you
            // something is wrong.
            FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Warning"),
            LOCTEXT("BarVerify", "Verify (MD5)"),
            LOCTEXT("BarVerifyTip",
                "Hash every source .blend and compare against the hash "
                "recorded at import. Exact, and slow — the State column's "
                "mtime check is the fast approximation of this."),
            FOnClicked::CreateSP(this, &SMapLinkManagerWindow::OnVerifyClicked),
            true)
    ]

    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
    [
        MakeIconButton(
            // No icon on purpose: the only engine keys this plugin verifies
            // at startup are Search/Visibility/Chevron/Arrow/Refresh/Warning/
            // Settings, and an unverified key renders as a blank button.
            // MakeIconButton falls back to the label, which is the right
            // outcome for a rare, heavy action anyway.
            FSlateIcon(),
            LOCTEXT("BarWarm", "Warm FBX Cache"),
            LOCTEXT("BarWarmTip",
                "Export every cache-stale .blend to FBX in parallel, so the "
                "next import skips all Blender launches."),
            FOnClicked::CreateSP(this, &SMapLinkManagerWindow::OnWarmCacheClicked),
            true)
    ]

    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
    [
        // Auto-refresh is a preference, not an action — it belongs with the
        // controls, but as a toggle, and it does not need a full label.
        SNew(SCheckBox)
        .Style(FAppStyle::Get(), "ToggleButtonCheckbox")
        .ToolTipText(LOCTEXT("BarAutoRefreshTip",
            "Auto-refresh\n\nRe-scan on a timer while this tab is open. The "
            "scan is skipped unless a cheap content hash changed, so an idle "
            "window never rebuilds."))
        .IsChecked_Lambda([this]()
        {
            return bAutoRefresh ? ECheckBoxState::Checked
                                : ECheckBoxState::Unchecked;
        })
        .OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
        {
            bAutoRefresh = (NewState == ECheckBoxState::Checked);
        })
        [
            SNew(SBox).Padding(FMargin(6.f, 3.f))
            [ SNew(STextBlock).Text(LOCTEXT("BarAutoRefresh", "Auto")) ]
        ]
    ]
    ;
}

TSharedRef<SWidget> SMapLinkManagerWindow::BuildAssetsFilterBar()
{
    // A bordered strip, so the eye can tell at a glance which band it is
    // looking at. The chips used to sit inline with the action buttons, which
    // is how "Warm FBX Cache" and "Out-of-date" came to look like the same
    // kind of control.
    return SNew(SBorder)
        .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
        .Padding(FMargin(6.f, 3.f))
        [
            SNew(SWrapBox)
            .UseAllottedSize(true)
            .InnerSlotPadding(FVector2D(4.f, 3.f))

            + SWrapBox::Slot().FillEmptySpace(false)
            [
                SNew(SBox).MinDesiredWidth(180.f).VAlign(VAlign_Center)
                [
                    SAssignNew(SearchBox, SSearchBox)
                    .HintText(LOCTEXT("FilterHint", "Filter by name…"))
                    .OnTextChanged_Lambda([this](const FText& NewText)
                    {
                        FilterText = NewText.ToString();
                        ApplyFilter();
                    })
                ]
            ]

            + SWrapBox::Slot()
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 2.f, 0.f)
                [
                    MakeFilterChip(LOCTEXT("ChipLI", "LI"), &bFilterTypeLI,
                        LOCTEXT("ChipLITip", "Show Level Instance (LI_) entries."),
                        EChipCount::TypeLI)
                ]
                + SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 2.f, 0.f)
                [
                    MakeFilterChip(LOCTEXT("ChipSM", "SM"), &bFilterTypeSM,
                        LOCTEXT("ChipSMTip", "Show StaticMesh-only (SM_) entries."),
                        EChipCount::TypeSM)
                ]
                + SHorizontalBox::Slot().AutoWidth()
                [
                    MakeFilterChip(LOCTEXT("ChipBP", "BP"), &bFilterTypeBP,
                        LOCTEXT("ChipBPTip", "Show Blueprint reference (BP_) entries."),
                        EChipCount::TypeBP)
                ]
            ]

            + SWrapBox::Slot()
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 2.f, 0.f)
                [
                    MakeFilterChip(LOCTEXT("ChipOOD", "Out-of-date"),
                        &bFilterStateOOD,
                        LOCTEXT("ChipOODTip", "Show out-of-date entries."),
                        EChipCount::StateOOD)
                ]
                + SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 2.f, 0.f)
                [
                    MakeFilterChip(LOCTEXT("ChipMissing", "Missing"),
                        &bFilterStateMissing,
                        LOCTEXT("ChipMissingTip",
                            "Show entries whose .blend is missing on disk."),
                        EChipCount::StateMissing)
                ]
                + SHorizontalBox::Slot().AutoWidth()
                [
                    MakeFilterChip(LOCTEXT("ChipOK", "OK"), &bFilterStateOK,
                        LOCTEXT("ChipOKTip", "Show up-to-date entries."),
                        EChipCount::StateOK)
                ]
            ]

            + SWrapBox::Slot()
            [
                SNew(SButton)
                .ButtonStyle(FAppStyle::Get(), "SimpleButton")
                .ContentPadding(FMargin(6.f, 2.f))
                .ToolTipText(LOCTEXT("ChipResetTip",
                    "Clear the type/state filters and the name filter."))
                .Visibility_Lambda([this]()
                {
                    const bool bFiltering =
                        !bFilterTypeLI || !bFilterTypeSM || !bFilterTypeBP ||
                        !bFilterStateOOD || !bFilterStateMissing ||
                        !bFilterStateOK || !FilterText.IsEmpty();
                    return bFiltering ? EVisibility::Visible
                                      : EVisibility::Collapsed;
                })
                .OnClicked_Lambda([this]()
                {
                    bFilterTypeLI = bFilterTypeSM = bFilterTypeBP = true;
                    bFilterStateOOD = bFilterStateMissing = bFilterStateOK = true;
                    FilterText.Reset();
                    if (SearchBox.IsValid())
                    {
                        SearchBox->SetText(FText::GetEmpty());
                    }
                    ApplyFilter();
                    return FReply::Handled();
                })
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("ChipReset", "Clear filters"))
                    .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                ]
            ]
        ];
}

TSharedRef<SWidget> SMapLinkManagerWindow::BuildAssetsTab()
{
    // v1.19 — three bands, each with one job.
    //
    // Up to v1.18 this tab opened with a single strip carrying an
    // auto-refresh checkbox, four action buttons, six filter chips and a
    // search box, and a second strip of eight selection buttons under it:
    // roughly twenty controls, mixing "do something" with "show me less",
    // before the first row of data. Doing and filtering are different
    // intents and now live in different bands:
    //
    //   Toolbar     actions only, icon-first, with the contextual selection
    //               group appearing only when something is selected.
    //   Filter bar  a visually distinct strip: search, type chips, state
    //               chips, and the way out of a filtered view.
    //   Body        the list, optionally split with the inspector.
    //   Totals      one line of answer.
    return SNew(SVerticalBox)

    + SVerticalBox::Slot().AutoHeight().Padding(6.f, 4.f, 6.f, 2.f)
    [
        BuildAssetsToolbar()
    ]

    + SVerticalBox::Slot().AutoHeight().Padding(6.f, 0.f, 6.f, 4.f)
    [
        BuildAssetsFilterBar()
    ]

    // ── Body: list, or list | inspector ─────────────────────────────
    + SVerticalBox::Slot()
    .FillHeight(1.f)
    .Padding(6.f, 0.f)
    [
        SNew(SSplitter)
        .Orientation(Orient_Horizontal)

        + SSplitter::Slot()
        .Value(0.62f)
        [
            SNew(SBorder)
            .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
            [
                SAssignNew(ListView, SListView<TSharedPtr<FMapLinkManagerRow>>)
                .ListItemsSource(&FilteredRows)
                .OnGenerateRow(this, &SMapLinkManagerWindow::OnGenerateRow)
                .SelectionMode(ESelectionMode::Multi)                       // v1.12
                .OnContextMenuOpening(this, &SMapLinkManagerWindow::OnRowContextMenu)
                .OnMouseButtonDoubleClick(this, &SMapLinkManagerWindow::OnRowDoubleClick)
                // v1.19 — the inspector follows the selection. It no-ops on an
                // unchanged stem, which matters: this fires again on every
                // auto-refresh rebuild.
                .OnSelectionChanged_Lambda(
                    [this](TSharedPtr<FMapLinkManagerRow>, ESelectInfo::Type)
                    {
                        SyncInspectorToSelection();
                    })
                .HeaderRow
                (
                    // v1.12 — CanSelectGeneratedColumn: right-click the header
                    // to show/hide columns.
                    SNew(SHeaderRow)
                    .CanSelectGeneratedColumn(true)

                    + SHeaderRow::Column(MapLinkManagerColumns::Favourite)
                    .DefaultLabel(FText::FromString(TEXT("\u2605")))
                    .FixedWidth(30.f)

                    + SHeaderRow::Column(MapLinkManagerColumns::Name)
                    .DefaultLabel(LOCTEXT("ColName", "Name"))
                    .FillWidth(0.24f)
                    .SortMode(this, &SMapLinkManagerWindow::GetColumnSortMode,
                              MapLinkManagerColumns::Name)
                    .OnSort(this, &SMapLinkManagerWindow::OnColumnSort)

                    + SHeaderRow::Column(MapLinkManagerColumns::Type)
                    .DefaultLabel(LOCTEXT("ColType", "Type"))
                    .FixedWidth(46.f)
                    .SortMode(this, &SMapLinkManagerWindow::GetColumnSortMode,
                              MapLinkManagerColumns::Type)
                    .OnSort(this, &SMapLinkManagerWindow::OnColumnSort)

                    + SHeaderRow::Column(MapLinkManagerColumns::State)
                    .DefaultLabel(LOCTEXT("ColState", "State"))
                    .FillWidth(0.16f)
                    .SortMode(this, &SMapLinkManagerWindow::GetColumnSortMode,
                              MapLinkManagerColumns::State)
                    .OnSort(this, &SMapLinkManagerWindow::OnColumnSort)

                    + SHeaderRow::Column(MapLinkManagerColumns::LastImport)
                    .DefaultLabel(LOCTEXT("ColLastImport", "Last Import"))
                    .DefaultTooltip(LOCTEXT("ColLastImportTip",
                        "How long ago this asset last imported. Hover a cell for "
                        "the exact instant."))
                    .FillWidth(0.18f)
                    .SortMode(this, &SMapLinkManagerWindow::GetColumnSortMode,
                              MapLinkManagerColumns::LastImport)
                    .OnSort(this, &SMapLinkManagerWindow::OnColumnSort)

                    + SHeaderRow::Column(MapLinkManagerColumns::Auto)
                    .DefaultLabel(LOCTEXT("ColAuto", "Auto"))
                    .FixedWidth(46.f)
                    .SortMode(this, &SMapLinkManagerWindow::GetColumnSortMode,
                              MapLinkManagerColumns::Auto)
                    .OnSort(this, &SMapLinkManagerWindow::OnColumnSort)

                    + SHeaderRow::Column(MapLinkManagerColumns::Recursive)
                    .DefaultLabel(LOCTEXT("ColRecursive", "Rec."))
                    .DefaultTooltip(LOCTEXT("ColRecursiveTip",
                        "Auto Reimport (Recursive) — reimport when any "
                        "dependency .blend changes (requires Auto). Children "
                        "do NOT need their own Auto tick."))
                    .FixedWidth(46.f)
                    .SortMode(this, &SMapLinkManagerWindow::GetColumnSortMode,
                              MapLinkManagerColumns::Recursive)
                    .OnSort(this, &SMapLinkManagerWindow::OnColumnSort)

                    // v1.13.1 — Skip AutoReimport shield
                    + SHeaderRow::Column(MapLinkManagerColumns::Skip)
                    .DefaultLabel(LOCTEXT("ColSkip", "Skip"))
                    .DefaultTooltip(LOCTEXT("ColSkipTip",
                        "Skip AutoReimport — this asset never auto-reimports, "
                        "even under a Recursive parent. Manual reimport is "
                        "unaffected."))
                    .FixedWidth(46.f)
                    .SortMode(this, &SMapLinkManagerWindow::GetColumnSortMode,
                              MapLinkManagerColumns::Skip)
                    .OnSort(this, &SMapLinkManagerWindow::OnColumnSort)

                    // v1.12 — the per-row Actions button strip was removed;
                    // actions run on the selection (action bar / context menu).
                    + SHeaderRow::Column(MapLinkManagerColumns::Expansion)
                    .DefaultLabel(LOCTEXT("ColExpansion", "Expansion"))
                    .DefaultTooltip(LOCTEXT("ColExpansionTip",
                        "World Partition expansion: the WP level this LI_ root "
                        "is expanded into, and the depth."))
                    .FillWidth(0.20f)
                    .SortMode(this, &SMapLinkManagerWindow::GetColumnSortMode,
                              MapLinkManagerColumns::Expansion)
                    .OnSort(this, &SMapLinkManagerWindow::OnColumnSort)
                )
            ]
        ]

        // The inspector is a real splitter slot rather than a collapsed
        // widget: SSplitter has no per-slot visibility, so a hidden panel
        // would keep its share of the width and the list would never reclaim
        // it. SizeToContent collapses the slot instead.
        + SSplitter::Slot()
        .SizeRule(TAttribute<SSplitter::ESizeRule>::Create(
            TAttribute<SSplitter::ESizeRule>::FGetter::CreateLambda([this]()
            {
                return IsInspectorVisible()
                    ? SSplitter::FractionOfParent
                    : SSplitter::SizeToContent;
            })))
        .Value(0.38f)
        [
            SNew(SBox)
            .Visibility_Lambda([this]()
            {
                return IsInspectorVisible()
                    ? EVisibility::Visible : EVisibility::Collapsed;
            })
            [
                SAssignNew(AssetInspector, SMapLinkAssetInspector)
            ]
        ]
    ]

    // ── Totals + maintenance (v1.17 / v1.19) ────────────────────────
    + SVerticalBox::Slot()
    .AutoHeight()
    .Padding(8.f, 4.f, 8.f, 6.f)
    [
        BuildAssetsMaintenanceBar()
    ];
}

// ---------------------------------------------------------------------------
// v1.17 — Assets tab totals
// ---------------------------------------------------------------------------

FText SMapLinkManagerWindow::GetAssetTotalsText() const
{
    int32 OOD = 0, Missing = 0, Orphan = 0;
    for (const TSharedPtr<FMapLinkManagerRow>& Row : AllRows)
    {
        if (!Row.IsValid()) continue;
        if (Row->bMissing)        { ++Missing; }
        else if (Row->bOrphanBP)  { ++Orphan;  }
        else if (Row->bOutOfDate) { ++OOD;     }
    }

    const int32 Expansions =
        UMapLinkEditorState::Get()->ActiveExpansions.Num();

    TArray<FString> Parts;
    Parts.Add(FString::Printf(TEXT("%d asset%s"),
        AllRows.Num(), AllRows.Num() == 1 ? TEXT("") : TEXT("s")));
    if (FilteredRows.Num() != AllRows.Num())
    {
        Parts.Add(FString::Printf(TEXT("%d shown"), FilteredRows.Num()));
    }
    Parts.Add(FString::Printf(TEXT("%d out of date"), OOD));
    if (Missing > 0) { Parts.Add(FString::Printf(TEXT("%d missing"), Missing)); }
    if (Orphan  > 0) { Parts.Add(FString::Printf(TEXT("%d orphan BP"), Orphan)); }
    if (Expansions > 0)
    {
        Parts.Add(FString::Printf(TEXT("%d expansion%s"),
            Expansions, Expansions == 1 ? TEXT("") : TEXT("s")));
    }

    return FText::FromString(FString::Join(Parts, TEXT("  ·  ")));
}

// ---------------------------------------------------------------------------
// v1.17 — Expansions tab
// ---------------------------------------------------------------------------
//
// Up to v1.16 this was a fixed-height panel under the Assets table: one root
// combo, one level picker, one depth spinner, and a grey one-line summary of
// every active expansion joined with "|".  That shape was not a UI choice —
// UMapLinkEditorState keyed expansion records by root, so one root could only
// ever be in one level, and the panel had nothing plural to show.
//
// With FMapLinkWPExpansionRecord::Id (v1.17) the model is plural, so this tab
// is: the same create form at the top (it was never the problem), and below it
// the list the panel could not draw.

namespace MapLinkExpansionColumns
{
    static const FName Label(TEXT("Label"));
    static const FName Root(TEXT("Root"));
    static const FName Level(TEXT("Level"));
    static const FName Depth(TEXT("Depth"));
    static const FName Actors(TEXT("Actors"));
    static const FName LastExpanded(TEXT("LastExpanded"));
    static const FName Auto(TEXT("Auto"));
    static const FName State(TEXT("State"));
}

FText FMapLinkExpansionRow::GetStateText() const
{
    switch (State)
    {
    case EState::InSync:        return NSLOCTEXT("MapLink", "ExpInSync", "In sync");
    case EState::Stale:         return NSLOCTEXT("MapLink", "ExpStale", "Stale");
    case EState::NeverExpanded: return NSLOCTEXT("MapLink", "ExpNever", "Not expanded yet");
    case EState::Paused:        return NSLOCTEXT("MapLink", "ExpPaused", "Paused");
    case EState::LevelMissing:  return NSLOCTEXT("MapLink", "ExpNoLevel", "Level missing");
    case EState::RootMissing:   return NSLOCTEXT("MapLink", "ExpNoRoot", "Root missing");
    case EState::NotInRecords:  return NSLOCTEXT("MapLink", "ExpNotMine",
                                    "In level, not in your records");
    }
    return FText::GetEmpty();
}

FLinearColor FMapLinkExpansionRow::GetStateColour() const
{
    switch (State)
    {
    case EState::InSync:        return FLinearColor(0.45f, 0.85f, 0.45f);
    case EState::Stale:         return FLinearColor(1.f,   0.75f, 0.25f);
    case EState::NeverExpanded: return FLinearColor(0.7f,  0.7f,  0.7f);
    case EState::Paused:        return FLinearColor(0.6f,  0.6f,  0.6f);
    case EState::LevelMissing:  return FLinearColor(1.f,   0.35f, 0.35f);
    case EState::RootMissing:   return FLinearColor(1.f,   0.35f, 0.35f);
    case EState::NotInRecords:  return FLinearColor(0.55f, 0.75f, 1.f);
    }
    return FLinearColor::White;
}

// ── Row widget ─────────────────────────────────────────────────────────────

class SMapLinkExpansionTableRow
    : public SMultiColumnTableRow<TSharedPtr<FMapLinkExpansionRow>>
{
public:
    SLATE_BEGIN_ARGS(SMapLinkExpansionTableRow) {}
        SLATE_ARGUMENT(TSharedPtr<FMapLinkExpansionRow>, Item)
        SLATE_ARGUMENT(TWeakPtr<SMapLinkManagerWindow>, OwnerWindow)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs,
                   const TSharedRef<STableViewBase>& OwnerTable)
    {
        Item        = InArgs._Item;
        OwnerWindow = InArgs._OwnerWindow;
        SMultiColumnTableRow<TSharedPtr<FMapLinkExpansionRow>>::Construct(
            FSuperRowType::FArguments(), OwnerTable);
    }

    virtual TSharedRef<SWidget> GenerateWidgetForColumn(
        const FName& ColumnName) override
    {
        if (!Item.IsValid())
        {
            return SNullWidget::NullWidget;
        }

        auto Text = [](const FString& In, const FSlateColor& Colour)
        {
            return SNew(SBox)
                .VAlign(VAlign_Center)
                .Padding(FMargin(6.f, 2.f))
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(In))
                    .ColorAndOpacity(Colour)
                ];
        };

        if (ColumnName == MapLinkExpansionColumns::Label)
        {
            return Text(Item->Label, FSlateColor::UseForeground());
        }
        if (ColumnName == MapLinkExpansionColumns::Root)
        {
            return Text(Item->RootBlendName, FSlateColor::UseSubduedForeground());
        }
        if (ColumnName == MapLinkExpansionColumns::Level)
        {
            return Text(Item->LevelName, FSlateColor::UseForeground());
        }
        if (ColumnName == MapLinkExpansionColumns::Depth)
        {
            // Editable. The record carries ExpandDepth, ExpandRecord cleans up
            // its own prior placement before re-expanding, and CleanupWPForExpansion
            // is matched by record Id — so "3 to 2" is this box plus Re-expand.
            // A discovered (untracked) row has no record to write to.
            if (Item->bDiscovered)
            {
                return Text(FString::Printf(TEXT("%d"), Item->ExpandDepth),
                            FSlateColor::UseSubduedForeground());
            }
            return SNew(SBox)
                .VAlign(VAlign_Center)
                .Padding(FMargin(4.f, 1.f))
                [
                    SNew(SSpinBox<int32>)
                    .MinValue(1).MaxValue(10)
                    .MinSliderValue(1).MaxSliderValue(10)
                    .ToolTipText(NSLOCTEXT("MapLink", "ExpDepthEditTip",
                        "How many LI_ levels deep to break open. Change it, "
                        "then press Re-expand: the previous placement for this "
                        "expansion is removed and rebuilt at the new depth. "
                        "Nothing has to be deleted by hand."))
                    .Value(Item->ExpandDepth)
                    .OnValueCommitted_Lambda(
                        [this](int32 NewValue, ETextCommit::Type)
                        {
                            if (TSharedPtr<SMapLinkManagerWindow> Owner = OwnerWindow.Pin())
                            {
                                Owner->OnSetExpansionDepth(Item, NewValue);
                            }
                        })
                ];
        }
        if (ColumnName == MapLinkExpansionColumns::Actors)
        {
            return Text(Item->PlacedCount > 0
                            ? FString::Printf(TEXT("%d"), Item->PlacedCount)
                            : FString(TEXT("—")),
                        FSlateColor::UseSubduedForeground());
        }
        if (ColumnName == MapLinkExpansionColumns::LastExpanded)
        {
            return Text(Item->LastExpandedStr, FSlateColor::UseSubduedForeground());
        }
        if (ColumnName == MapLinkExpansionColumns::Auto)
        {
            // The pause valve.  Without it the only way to stop a heavy
            // expansion from re-running on every dependency save was to
            // delete it out of the level and rebuild it later.
            return SNew(SBox)
                .HAlign(HAlign_Center)
                .VAlign(VAlign_Center)
                [
                    SNew(SCheckBox)
                    .ToolTipText(NSLOCTEXT("MapLink", "ExpAutoTip",
                        "Re-expand this expansion when one of its .blend "
                        "dependencies reimports. Off parks the expansion: the "
                        "actors stay in the level, they just stop updating."))
                    // v1.18 — an untracked row has no record to toggle.
                    .IsEnabled(!Item->bDiscovered)
                    .IsChecked(Item->bReExpandOnImport
                        ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
                    .OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
                    {
                        if (TSharedPtr<SMapLinkManagerWindow> Owner = OwnerWindow.Pin())
                        {
                            Owner->OnSetExpansionPaused(
                                Item, NewState != ECheckBoxState::Checked);
                        }
                    })
                ];
        }
        if (ColumnName == MapLinkExpansionColumns::State)
        {
            return SNew(SBox)
                .VAlign(VAlign_Center)
                .Padding(FMargin(6.f, 2.f))
                [
                    SNew(STextBlock)
                    .Text(Item->GetStateText())
                    .ColorAndOpacity(FSlateColor(Item->GetStateColour()))
                ];
        }

        return SNullWidget::NullWidget;
    }

private:
    TSharedPtr<FMapLinkExpansionRow> Item;
    TWeakPtr<SMapLinkManagerWindow>  OwnerWindow;
};

// ── Tab ────────────────────────────────────────────────────────────────────

TSharedRef<SWidget> SMapLinkManagerWindow::BuildExpansionsTab()
{
    RebuildLIRootOptions();
    RefreshExpansionPreview();
    RefreshExpansionRows();

    return SNew(SVerticalBox)

    // ── Create form ────────────────────────────────────────────────
    + SVerticalBox::Slot()
    .AutoHeight()
    .Padding(6.f)
    [
        SNew(SBorder)
        .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
        .Padding(8.f)
        [
            SNew(SVerticalBox)

            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 0.f, 0.f, 6.f)
            [
                SNew(STextBlock)
                .Text(LOCTEXT("WPHeader", "New World Partition Expansion"))
                .Font(FAppStyle::GetFontStyle("BoldFont"))
            ]

            + SVerticalBox::Slot()
            .AutoHeight()
            [
                SNew(SHorizontalBox)

                // Root proxy selector
                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                .Padding(0.f, 0.f, 4.f, 0.f)
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("WPRoot", "Root LI_:"))
                ]

                + SHorizontalBox::Slot()
                .FillWidth(0.28f)
                .VAlign(VAlign_Center)
                .Padding(0.f, 0.f, 12.f, 0.f)
                [
                    SAssignNew(RootCombo, SComboBox<TSharedPtr<FString>>)
                    .OptionsSource(&LIRootOptions)
                    .OnGenerateWidget_Lambda(
                        [](TSharedPtr<FString> Option)
                        {
                            return SNew(STextBlock)
                                .Text(FText::FromString(
                                    Option.IsValid() ? *Option : FString()));
                        })
                    .OnSelectionChanged_Lambda(
                        [this](TSharedPtr<FString> NewSel, ESelectInfo::Type)
                        {
                            SelectedRoot = NewSel;
                            RefreshExpansionPreview();
                        })
                    [
                        SNew(STextBlock)
                        .Text(this, &SMapLinkManagerWindow::GetSelectedRootText)
                    ]
                ]

                // WP level picker — v1.17: World Partition levels only.
                // v1.16 allowed any UWorld and only checked for World
                // Partition inside ExpandIntoWP, AFTER the level had been
                // opened; picking wrong cost a full level load to find out.
                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                .Padding(0.f, 0.f, 4.f, 0.f)
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("WPLevel", "WP Level:"))
                ]

                + SHorizontalBox::Slot()
                .FillWidth(0.34f)
                .VAlign(VAlign_Center)
                .Padding(0.f, 0.f, 12.f, 0.f)
                [
                    SNew(SObjectPropertyEntryBox)
                    .AllowedClass(UWorld::StaticClass())
                    .AllowClear(true)
                    .DisplayThumbnail(false)
                    .ObjectPath(this, &SMapLinkManagerWindow::GetWPLevelObjectPath)
                    .OnObjectChanged(this, &SMapLinkManagerWindow::OnWPLevelChanged)
                    .OnShouldFilterAsset(this,
                        &SMapLinkManagerWindow::ShouldFilterOutLevel)
                ]

                // Depth
                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                .Padding(0.f, 0.f, 4.f, 0.f)
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("WPDepth", "Depth:"))
                ]

                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                .Padding(0.f, 0.f, 4.f, 0.f)
                [
                    SNew(SBox)
                    .WidthOverride(56.f)
                    [
                        SNew(SSpinBox<int32>)
                        .MinValue(1)
                        .MaxValue(10)
                        .Value_Lambda([this]() { return ExpandDepth; })
                        .OnValueChanged_Lambda([this](int32 NewValue)
                        {
                            ExpandDepth = FMath::Clamp(NewValue, 1, 10);
                            RefreshExpansionPreview();
                        })
                    ]
                ]

                // "of 4" — the real depth of this root's hierarchy, so the
                // spinner is a bounded choice instead of a guess.
                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                .Padding(0.f, 0.f, 12.f, 0.f)
                [
                    SNew(STextBlock)
                    .Text(this, &SMapLinkManagerWindow::GetDepthSuffixText)
                    .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                ]

                // Label
                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                .Padding(0.f, 0.f, 4.f, 0.f)
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("WPLabel", "Label:"))
                ]

                + SHorizontalBox::Slot()
                .FillWidth(0.22f)
                .VAlign(VAlign_Center)
                .Padding(0.f, 0.f, 12.f, 0.f)
                [
                    SNew(SEditableTextBox)
                    .HintText(LOCTEXT("WPLabelHint", "(root stem)"))
                    .ToolTipText(LOCTEXT("WPLabelTip",
                        "Names the outliner folder (MapLink/<Label>) and this "
                        "row in the list. Leave blank to use the root stem — "
                        "which is exactly what every pre-v1.17 expansion did. "
                        "Set it when you expand the same root twice."))
                    .Text_Lambda([this]()
                    {
                        return FText::FromString(NewExpansionLabel);
                    })
                    .OnTextChanged_Lambda([this](const FText& NewText)
                    {
                        NewExpansionLabel = NewText.ToString();
                    })
                ]

                + SHorizontalBox::Slot()
                .AutoWidth()
                .VAlign(VAlign_Center)
                [
                    SNew(SButton)
                    .Text(LOCTEXT("WPExpand", "Expand"))
                    .IsEnabled(this, &SMapLinkManagerWindow::CanExpandNow)
                    .ToolTipText(this, &SMapLinkManagerWindow::GetExpandTooltip)
                    .OnClicked(this, &SMapLinkManagerWindow::OnExpandClicked)
                ]
            ]

            // Dry run.  Expansion writes hundreds of actors into a
            // source-controlled level with no undo; the user should not have
            // to run it to find out what it does.
            + SVerticalBox::Slot()
            .AutoHeight()
            .Padding(0.f, 6.f, 0.f, 0.f)
            [
                SNew(STextBlock)
                .Text(this, &SMapLinkManagerWindow::GetExpansionPreviewText)
                .ColorAndOpacity(FSlateColor::UseSubduedForeground())
            ]
        ]
    ]

    // ── List toolbar ───────────────────────────────────────────────
    + SVerticalBox::Slot()
    .AutoHeight()
    .Padding(8.f, 2.f, 8.f, 4.f)
    [
        SNew(SHorizontalBox)

        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        .Padding(0.f, 0.f, 8.f, 0.f)
        [
            SNew(STextBlock)
            .Text(LOCTEXT("ExpListHeader", "Active expansions"))
            .Font(FAppStyle::GetFontStyle("BoldFont"))
        ]

        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        .Padding(0.f, 0.f, 4.f, 0.f)
        [
            SNew(SButton)
            .Text(LOCTEXT("ExpReExpandSel", "Re-expand"))
            .ToolTipText(LOCTEXT("ExpReExpandSelTip",
                "Rebuild the selected expansions from their current .blend "
                "hierarchies."))
            .IsEnabled_Lambda([this]()
            {
                return GetSelectedExpansions().Num() > 0;
            })
            .OnClicked(this, &SMapLinkManagerWindow::OnReExpandSelected)
        ]

        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        .Padding(0.f, 0.f, 4.f, 0.f)
        [
            SNew(SButton)
            .Text(LOCTEXT("ExpReExpandStale", "Re-expand stale"))
            .ToolTipText(LOCTEXT("ExpReExpandStaleTip",
                "Rebuild every expansion whose .blend dependencies have "
                "changed since it was last expanded."))
            .IsEnabled_Lambda([this]()
            {
                for (const TSharedPtr<FMapLinkExpansionRow>& Row : ExpansionRows)
                {
                    if (Row.IsValid() &&
                        Row->State == FMapLinkExpansionRow::EState::Stale)
                    {
                        return true;
                    }
                }
                return false;
            })
            .OnClicked(this, &SMapLinkManagerWindow::OnReExpandStale)
        ]

        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        .Padding(0.f, 0.f, 4.f, 0.f)
        [
            SNew(SButton)
            .Text(LOCTEXT("ExpCleanupSel", "Cleanup"))
            .ToolTipText(LOCTEXT("ExpCleanupSelTip",
                "Remove the selected expansions' actors and folders from "
                "their levels, and drop the records."))
            .IsEnabled_Lambda([this]()
            {
                return GetSelectedExpansions().Num() > 0;
            })
            .OnClicked(this, &SMapLinkManagerWindow::OnCleanupSelectedExpansions)
        ]

        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Center)
        .Padding(0.f, 0.f, 4.f, 0.f)
        [
            // FMapLinkWPExpansionHelper::CleanupAllWPExpansion has existed
            // since v1.9 with no caller and no button.  This is it.
            SNew(SButton)
            .Text(LOCTEXT("ExpCleanupAll", "Cleanup All in Level…"))
            .ToolTipText(LOCTEXT("ExpCleanupAllTip",
                "Remove EVERY MapLink expansion from the level selected "
                "above — including expansions placed by a teammate — and "
                "drop the matching records."))
            .OnClicked(this, &SMapLinkManagerWindow::OnCleanupAllInLevel)
        ]

        + SHorizontalBox::Slot()
        .FillWidth(1.f)
        .HAlign(HAlign_Right)
        .VAlign(VAlign_Center)
        [
            SNew(STextBlock)
            .Text(this, &SMapLinkManagerWindow::GetExpansionSummaryText)
            .ColorAndOpacity(FSlateColor::UseSubduedForeground())
        ]
    ]

    // ── List ───────────────────────────────────────────────────────
    + SVerticalBox::Slot()
    .FillHeight(1.f)
    .Padding(6.f, 0.f, 6.f, 6.f)
    [
        SNew(SBorder)
        .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
        .Padding(2.f)
        [
            SAssignNew(ExpansionListView, SListView<TSharedPtr<FMapLinkExpansionRow>>)
            .ListItemsSource(&ExpansionRows)
            .SelectionMode(ESelectionMode::Multi)
            .OnGenerateRow(this, &SMapLinkManagerWindow::OnGenerateExpansionRow)
            .OnContextMenuOpening(this, &SMapLinkManagerWindow::OnExpansionContextMenu)
            .OnMouseButtonDoubleClick(this, &SMapLinkManagerWindow::OnExpansionDoubleClick)
            .HeaderRow
            (
                SNew(SHeaderRow)

                + SHeaderRow::Column(MapLinkExpansionColumns::Label)
                .DefaultLabel(LOCTEXT("ExpColLabel", "Expansion"))
                .FillWidth(0.22f)

                + SHeaderRow::Column(MapLinkExpansionColumns::Root)
                .DefaultLabel(LOCTEXT("ExpColRoot", "Root"))
                .FillWidth(0.20f)

                + SHeaderRow::Column(MapLinkExpansionColumns::Level)
                .DefaultLabel(LOCTEXT("ExpColLevel", "Level"))
                .FillWidth(0.18f)

                + SHeaderRow::Column(MapLinkExpansionColumns::Depth)
                .DefaultLabel(LOCTEXT("ExpColDepth", "Depth"))
                .FixedWidth(56.f)

                + SHeaderRow::Column(MapLinkExpansionColumns::Actors)
                .DefaultLabel(LOCTEXT("ExpColActors", "Actors"))
                .DefaultTooltip(LOCTEXT("ExpColActorsTip",
                    "Actors placed by the last successful expansion."))
                .FixedWidth(66.f)

                + SHeaderRow::Column(MapLinkExpansionColumns::LastExpanded)
                .DefaultLabel(LOCTEXT("ExpColLast", "Last expanded"))
                .FillWidth(0.16f)

                + SHeaderRow::Column(MapLinkExpansionColumns::Auto)
                .DefaultLabel(LOCTEXT("ExpColAuto", "Auto"))
                .DefaultTooltip(LOCTEXT("ExpColAutoTip",
                    "Re-expand on dependency reimport."))
                .FixedWidth(46.f)

                + SHeaderRow::Column(MapLinkExpansionColumns::State)
                .DefaultLabel(LOCTEXT("ExpColState", "State"))
                .FillWidth(0.16f)
            )
        ]
    ];
}

// ---------------------------------------------------------------------------
// Materials tab (v1.10 — MIM Drop 2)
// ---------------------------------------------------------------------------

TSharedRef<SWidget> SMapLinkManagerWindow::BuildMaterialsTab()
{
    return SNew(SMapLinkMaterialsTab);
}

// ---------------------------------------------------------------------------
// Textures tab (v1.16 — Texture Manager & Conformer)
// ---------------------------------------------------------------------------

TSharedRef<SWidget> SMapLinkManagerWindow::BuildTexturesTab()
{
    return SNew(SMapLinkTexturesTab);
}

TWeakPtr<SMapLinkManagerWindow> SMapLinkManagerWindow::LiveWindow;

void SMapLinkManagerWindow::ActivateTab(ETab Tab)
{
    if (TSharedPtr<SMapLinkManagerWindow> Window = LiveWindow.Pin())
    {
        Window->ActiveTabIndex = static_cast<int32>(Tab);
    }
}

// ---------------------------------------------------------------------------
// Settings tab
// ---------------------------------------------------------------------------

TSharedRef<SWidget> SMapLinkManagerWindow::BuildSettingsTab()
{
    FPropertyEditorModule& PropertyModule =
        FModuleManager::LoadModuleChecked<FPropertyEditorModule>(
            "PropertyEditor");

    FDetailsViewArgs DetailsArgs;
    DetailsArgs.bUpdatesFromSelection = false;
    DetailsArgs.bLockable             = false;
    DetailsArgs.bAllowSearch          = true;
    DetailsArgs.bHideSelectionTip     = true;
    DetailsArgs.NameAreaSettings      = FDetailsViewArgs::HideNameArea;

    SettingsView = PropertyModule.CreateDetailView(DetailsArgs);
    SettingsView->SetObject(GetMutableDefault<UMapLinkSettings>());
    SettingsView->OnFinishedChangingProperties().AddSP(
        this, &SMapLinkManagerWindow::OnSettingsChanged);

    return SNew(SVerticalBox)

    + SVerticalBox::Slot()
    .AutoHeight()
    .Padding(8.f, 6.f)
    [
        // v1.18 — the rule, and the marker that makes it visible per row.
        // The sentence alone was doing work the layout should do: two
        // adjacent rows in one category can land in different files, and
        // putting a machine-local path in the source-controlled one breaks
        // every teammate, silently.
        SNew(STextBlock)
        .AutoWrapText(true)
        .Text(LOCTEXT("SettingsNote",
            "Settings without a marker are written to Config/DefaultGame.ini "
            "— they are SOURCE CONTROLLED and change for the whole team.\n"
            "Settings marked [this machine] are yours alone (Blender root, "
            "material library paths, viewport display); they live in your "
            "per-user editor ini and never touch source control."))
        .ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
    ]

    // v1.13.1 — the Material library sync status + Resync button moved to the
    // Materials tab (see SMapLinkMaterialsTab), where the other material ops
    // live. Settings keeps only the project-settings details view below.
    + SVerticalBox::Slot()
    .FillHeight(1.f)
    [
        SettingsView.ToSharedRef()
    ];
}

// v1.13.1 — Material library sync UI (status text + Resync button) moved to
// the Materials tab (SMapLinkMaterialsTab). See there for the handlers.

void SMapLinkManagerWindow::OnSettingsChanged(
    const FPropertyChangedEvent& Event)
{
    UMapLinkSettings* Settings = GetMutableDefault<UMapLinkSettings>();
    Settings->TryUpdateDefaultConfigFile();
}

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

TArray<UMapLinkProxyAsset*> SMapLinkManagerWindow::CollectAllProxies()
{
    TArray<UMapLinkProxyAsset*> Result;

    FAssetRegistryModule& ARModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
            "AssetRegistry");
    IAssetRegistry& AR = ARModule.Get();

    FARFilter Filter;
    Filter.ClassPaths.Add(
        UMapLinkProxyAsset::StaticClass()->GetClassPathName());
    Filter.bRecursivePaths = true;

    TArray<FAssetData> ProxyAssets;
    AR.GetAssets(Filter, ProxyAssets);

    for (const FAssetData& AssetData : ProxyAssets)
    {
        if (UMapLinkProxyAsset* Proxy =
                Cast<UMapLinkProxyAsset>(AssetData.GetAsset()))
        {
            Result.Add(Proxy);
        }
    }
    return Result;
}

void SMapLinkManagerWindow::RefreshRows(bool bVerifyHashes)
{
    // Preserve selection across the rebuild.
    FString PrevSelected;
    if (ListView.IsValid())
    {
        TArray<TSharedPtr<FMapLinkManagerRow>> Sel = ListView->GetSelectedItems();
        if (Sel.Num() > 0 && Sel[0].IsValid())
        {
            PrevSelected = Sel[0]->BlendName;
        }
    }

    AllRows.Reset();

    UMapLinkEditorState* EditorState = UMapLinkEditorState::Get();
    TArray<UMapLinkProxyAsset*> Proxies = CollectAllProxies();

    TUniquePtr<FScopedSlowTask> VerifyTask;
    if (bVerifyHashes && Proxies.Num() > 0)
    {
        VerifyTask = MakeUnique<FScopedSlowTask>(
            static_cast<float>(Proxies.Num()),
            LOCTEXT("VerifyTask", "MapLink: Verifying .blend hashes…"));
        VerifyTask->MakeDialog(false);
    }

    for (UMapLinkProxyAsset* Proxy : Proxies)
    {
        if (VerifyTask.IsValid())
        {
            VerifyTask->EnterProgressFrame(1.f);
        }

        TSharedPtr<FMapLinkManagerRow> Row = MakeShared<FMapLinkManagerRow>();
        Row->Proxy       = Proxy;
        Row->BlendPath   = Proxy->BlendFilePath;
        Row->BlendName   = FPaths::GetBaseFilename(Proxy->BlendFilePath);
        Row->bAutoUpdate = Proxy->bAutoUpdate;
        Row->bFavourite  = EditorState->IsFavourite(Row->BlendName);
        Row->bAutoUpdate = Proxy->bAutoUpdate;
        Row->bAutoUpdateRecursive = Proxy->bAutoUpdateRecursive;
        Row->bSkipAutoReimport = Proxy->bSkipAutoReimport;   // v1.13.1

        if (Row->BlendName.StartsWith(TEXT("LI_")))      Row->TypeStr = TEXT("LI");
        else if (Row->BlendName.StartsWith(TEXT("SM_"))) Row->TypeStr = TEXT("SM");
        else                                              Row->TypeStr = TEXT("BP");

        // v1.12 — WP expansion column: "L_World · d2" for expanded LI roots.
        // v1.17 — one chip per record: a root can be in several levels, and
        // in one level more than once.
        for (const FMapLinkWPExpansionRecord& Rec : EditorState->ActiveExpansions)
        {
            if (Rec.RootBlendName != Row->BlendName)
            {
                continue;
            }
            FMapLinkManagerRow::FChip Chip;
            Chip.Id   = Rec.Id;
            Chip.Text = FString::Printf(TEXT("%s d%d"),
                *Rec.TargetLevel.GetAssetName(), Rec.ExpandDepth);
            if (!Rec.bReExpandOnImport)
            {
                Chip.Text += TEXT(" (paused)");
            }
            Row->ExpansionChips.Add(MoveTemp(Chip));
        }
        if (Row->ExpansionChips.Num() > 0)
        {
            Row->bExpanded = true;
            TArray<FString> Parts;
            for (const FMapLinkManagerRow::FChip& Chip : Row->ExpansionChips)
            {
                Parts.Add(Chip.Text);
            }
            Row->ExpansionStr = FString::Join(Parts, TEXT(" · "));
        }

        // Friendly last-import timestamp
        FDateTime LastImport;
        const bool bHasTimestamp = FDateTime::ParseIso8601(
            *Proxy->LastImportTimestamp, LastImport);
        Row->bHasTimestamp = bHasTimestamp;
        Row->LastImportTime = LastImport;
        // v1.13.1 — the stored instant is UTC; show it in local time by default
        // (Settings → General → "Show Times In Local Time").
        FDateTime DisplayTime = LastImport;
        if (UMapLinkSettings::Get()->bShowTimesInLocalTime)
        {
            DisplayTime += (FDateTime::Now() - FDateTime::UtcNow());
        }
        // v1.17 — the column showed an absolute UTC stamp, which makes the
        // reader do timezone arithmetic to answer the only question they ever
        // ask of it: "is this recent?".  Relative in the cell, absolute in the
        // tooltip.
        Row->LastImportAbsStr = bHasTimestamp
            ? DisplayTime.ToString(TEXT("%Y-%m-%d %H:%M"))
            : Proxy->LastImportTimestamp;
        Row->LastImportStr = bHasTimestamp
            ? MapLinkManagerUtil::RelativeTime(LastImport)
            : Proxy->LastImportTimestamp;

        // ── State ────────────────────────────────────────────────────
        if (Row->BlendPath.IsEmpty() || !FPaths::FileExists(Row->BlendPath))
        {
            Row->bMissing  = true;
            Row->StateRank = 0;
            Row->StateStr  = TEXT(".blend missing");
        }
        else if (Proxy->bIsBPReference &&
                 Proxy->OriginalBoundBlueprintPath.IsEmpty())
        {
            Row->bOrphanBP = true;
            Row->StateRank = 1;
            Row->StateStr  = TEXT("Orphan BP (no binding)");
        }
        else if (bVerifyHashes)
        {
            const FString CurrentMD5 =
                FMapLinkUtils::ComputeFileMD5(Row->BlendPath);
            if (!Proxy->BlendFileHash.IsEmpty() &&
                CurrentMD5.Equals(Proxy->BlendFileHash, ESearchCase::IgnoreCase))
            {
                Row->StateRank = 3;
                Row->StateStr  = TEXT("Up to date (MD5)");
                Row->StateTooltipStr = LOCTEXT("StateMD5OkTip",
                    "File contents hash to the value recorded at import — "
                    "identical, whatever the timestamps say.").ToString();
            }
            else
            {
                Row->bOutOfDate = true;
                Row->StateRank  = 2;
                Row->StateStr   = TEXT("Out of date (MD5)");
                Row->StateTooltipStr = LOCTEXT("StateMD5StaleTip",
                    "File contents differ from the hash recorded at import. "
                    "This is the exact check — the asset really has changed.")
                    .ToString();
            }
        }
        else
        {
            // Fast heuristic: source .blend modified since last import → stale.
            //
            // v1.18 — the cell now explains itself. "Out of date" repeated a
            // hundred times is a claim with no evidence, and the first
            // reaction to a whole list of it is to distrust the column and
            // reach for Verify (MD5). The two timestamps being compared are
            // right here; showing them turns "is this real?" into a fact the
            // reader can check.
            //
            // v1.13.1 — compare mtime-vs-mtime (ONE clock). We stamp
            // ImportedBlendModTime = GetTimeStamp(.blend) at import, so an
            // unchanged file reads exactly equal here and never trips. The old
            // check compared this file mtime against LastImportTimestamp
            // (FDateTime::UtcNow at import) — a wall-clock instant on a
            // different clock than the filesystem mtime — which made a
            // freshly-edited cascade child (e.g. LI_uneplace) read "Out of
            // date" even though the reimport succeeded and MD5 verify agreed.
            const FDateTime FileTime =
                IFileManager::Get().GetTimeStamp(*Row->BlendPath);
            static const TCHAR* Fmt = TEXT("%Y-%m-%d %H:%M:%S");

            if (Proxy->ImportedBlendModTime.GetTicks() > 0)
            {
                if (FileTime > Proxy->ImportedBlendModTime)
                {
                    Row->bOutOfDate = true;
                    Row->StateRank  = 2;
                    Row->StateStr   = TEXT("Out of date");
                    Row->StateTooltipStr = FString::Printf(
                        TEXT(".blend modified   %s\n")
                        TEXT("imported from     %s\n\n")
                        TEXT("Both are file modification times, so an "
                             "untouched .blend reads exactly equal. Verify "
                             "(MD5) compares contents instead — use it if the "
                             "file was touched but not actually edited."),
                        *FileTime.ToString(Fmt),
                        *Proxy->ImportedBlendModTime.ToString(Fmt));
                }
                else
                {
                    Row->StateRank = 3;
                    Row->StateStr  = TEXT("Up to date");
                    Row->StateTooltipStr = FString::Printf(
                        TEXT(".blend unchanged since import (%s)."),
                        *FileTime.ToString(Fmt));
                }
            }
            // Legacy proxy (imported before v1.13.1, no mtime stamped): fall
            // back to the original wall-clock comparison until its next import.
            else if (bHasTimestamp && FileTime > LastImport)
            {
                Row->bOutOfDate = true;
                Row->StateRank  = 2;
                Row->StateStr   = TEXT("Out of date (legacy check)");
                Row->StateTooltipStr = FString::Printf(
                    TEXT(".blend modified   %s\n")
                    TEXT("imported at       %s\n\n")
                    TEXT("This proxy predates mtime stamping, so the "
                         "comparison is against the import's WALL CLOCK, not "
                         "the file's modification time — two different clocks. "
                         "It over-reports. Reimport once and this asset "
                         "switches to the exact check."),
                    *FileTime.ToString(Fmt), *LastImport.ToString(Fmt));
            }
            else if (!bHasTimestamp)
            {
                Row->bOutOfDate = true;
                Row->StateRank  = 2;
                Row->StateStr   = TEXT("Never imported?");
                Row->StateTooltipStr = LOCTEXT("StateNeverImportedTip",
                    "The proxy carries no import timestamp at all. Reimport "
                    "it to establish one.").ToString();
            }
            else
            {
                Row->StateRank = 3;
                Row->StateStr  = TEXT("Up to date");
                Row->StateTooltipStr = FString::Printf(
                    TEXT(".blend unchanged since import (%s)."),
                    *FileTime.ToString(Fmt));
            }
        }

        AllRows.Add(Row);
    }

    SortRows();
    RebuildLIRootOptions();
    ApplyFilter();

    // v1.19 — a full-verify pass follows a reimport often enough that this is
    // the honest place to drop cached mesh statistics: the generated assets
    // have been recreated, so triangle counts and material lists are stale.
    if (bVerifyHashes && AssetInspector.IsValid())
    {
        AssetInspector->InvalidateCache();
    }

    // Restore selection by name.
    if (!PrevSelected.IsEmpty() && ListView.IsValid())
    {
        for (const TSharedPtr<FMapLinkManagerRow>& Row : FilteredRows)
        {
            if (Row->BlendName == PrevSelected)
            {
                ListView->SetSelection(Row, ESelectInfo::Direct);
                break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Sorting (v1.9)
// ---------------------------------------------------------------------------

EColumnSortMode::Type SMapLinkManagerWindow::GetColumnSortMode(
    const FName ColumnId) const
{
    return (ColumnId == SortColumn) ? SortMode : EColumnSortMode::None;
}

void SMapLinkManagerWindow::OnColumnSort(
    EColumnSortPriority::Type /*Priority*/,
    const FName& ColumnId,
    EColumnSortMode::Type NewMode)
{
    SortColumn = ColumnId;
    SortMode   = NewMode;
    SortRows();
    ApplyFilter();
}

void SMapLinkManagerWindow::SortRows()
{
    const FName Col = SortColumn;
    const bool  bAsc = (SortMode != EColumnSortMode::Descending);

    AllRows.Sort([Col, bAsc](const TSharedPtr<FMapLinkManagerRow>& A,
                             const TSharedPtr<FMapLinkManagerRow>& B)
    {
        // Favourites always pinned to the top, regardless of sort column.
        if (A->bFavourite != B->bFavourite)
        {
            return A->bFavourite;
        }

        int32 Cmp = 0;
        if (Col == MapLinkManagerColumns::Name)
        {
            Cmp = A->BlendName.Compare(B->BlendName);
        }
        else if (Col == MapLinkManagerColumns::Type)
        {
            Cmp = A->TypeStr.Compare(B->TypeStr);
        }
        else if (Col == MapLinkManagerColumns::State)
        {
            Cmp = A->StateRank - B->StateRank;
        }
        else if (Col == MapLinkManagerColumns::LastImport)
        {
            if (A->LastImportTime == B->LastImportTime) Cmp = 0;
            else Cmp = (A->LastImportTime < B->LastImportTime) ? -1 : 1;
        }
        else if (Col == MapLinkManagerColumns::Auto)
        {
            Cmp = (A->bAutoUpdate == B->bAutoUpdate) ? 0
                  : (A->bAutoUpdate ? 1 : -1);
        }
        else if (Col == MapLinkManagerColumns::Recursive)
        {
            Cmp = (A->bAutoUpdateRecursive == B->bAutoUpdateRecursive) ? 0
                  : (A->bAutoUpdateRecursive ? 1 : -1);
        }
        else if (Col == MapLinkManagerColumns::Skip)
        {
            Cmp = (A->bSkipAutoReimport == B->bSkipAutoReimport) ? 0
                  : (A->bSkipAutoReimport ? 1 : -1);
        }
        else if (Col == MapLinkManagerColumns::Favourite)
        {
            Cmp = (A->bFavourite == B->bFavourite) ? 0
                  : (A->bFavourite ? 1 : -1);
        }
        else if (Col == MapLinkManagerColumns::Expansion)
        {
            // Expanded rows first among equals, then by expansion text.
            if (A->bExpanded != B->bExpanded)
            {
                Cmp = A->bExpanded ? -1 : 1;
            }
            else
            {
                Cmp = A->ExpansionStr.Compare(B->ExpansionStr);
            }
        }

        if (Cmp == 0)
        {
            // Stable tiebreak by name (always ascending).
            return A->BlendName < B->BlendName;
        }
        return bAsc ? (Cmp < 0) : (Cmp > 0);
    });
}

int32 SMapLinkManagerWindow::CountForChip(EChipCount Which) const
{
    int32 Count = 0;
    for (const TSharedPtr<FMapLinkManagerRow>& Row : AllRows)
    {
        if (!Row.IsValid()) continue;
        switch (Which)
        {
        case EChipCount::TypeLI:  Count += (Row->TypeStr == TEXT("LI")); break;
        case EChipCount::TypeSM:  Count += (Row->TypeStr == TEXT("SM")); break;
        case EChipCount::TypeBP:  Count += (Row->TypeStr == TEXT("BP")); break;
        case EChipCount::StateOOD:
            Count += (Row->bOutOfDate && !Row->bMissing); break;
        case EChipCount::StateMissing:
            Count += (Row->bMissing ? 1 : 0); break;
        case EChipCount::StateOK:
            Count += (!Row->bOutOfDate && !Row->bMissing && !Row->bOrphanBP);
            break;
        default: break;
        }
    }
    return Count;
}

TSharedRef<SWidget> SMapLinkManagerWindow::MakeFilterChip(
    const FText& Label, bool* Flag, const FText& Tooltip, EChipCount Which)
{
    // v1.13 — simple chip, combined with the text filter in ApplyFilter().
    // v1.17 — the chip carries its count.
    // v1.19 — two weights: discreet type chips, filled state chips.
    // v1.19.1 — ONE weight.  The split was right about the problem and wrong
    // about the fix.
    //
    // Six filled blue pills made a screen with nothing filtered look
    // aggressively filtered, so v1.19 made LI/SM/BP discreet and left the
    // state chips filled.  That traded one problem for another: the six
    // controls of a single filter row became two different widgets, in two
    // fonts, with two paddings and two ways of showing the same on/off state.
    //
    // Now every chip is the same control — SimpleButton, no fill at any
    // state, brightness carries inclusion.  The reason the state chips were
    // loud is preserved by moving urgency onto the DATA: the count is
    // coloured, the chip is not.  "Missing 3" still catches the eye, "Missing
    // 0" is as quiet as "LI 40", and a clean project shows no colour in the
    // strip at all — which is what a clean project should look like.
    const bool bHasCount = (Which != EChipCount::None);

    // Included reads as body text; excluded fades towards the panel.
    const FLinearColor OnGrey (0.78f, 0.78f, 0.78f);
    const FLinearColor OffGrey(0.34f, 0.34f, 0.34f);

    return SNew(SButton)
        .ButtonStyle(FAppStyle::Get(), "SimpleButton")
        .ContentPadding(FMargin(5.f, 2.f))
        .ToolTipText(Tooltip)
        .OnClicked_Lambda([this, Flag]()
        {
            *Flag = !*Flag;
            ApplyFilter();
            return FReply::Handled();
        })
        [
            SNew(SHorizontalBox)

            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [
                SNew(STextBlock)
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                .Text(Label)
                .ColorAndOpacity_Lambda([Flag, OnGrey, OffGrey]()
                {
                    return FSlateColor(*Flag ? OnGrey : OffGrey);
                })
            ]

            // The count.  Neutral by default; amber or red only when a state
            // chip has something to report AND that chip is included — a chip
            // the user has switched off must not shout about rows it hides.
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
              .Padding(4.f, 0.f, 0.f, 0.f)
            [
                SNew(STextBlock)
                .Visibility(bHasCount ? EVisibility::Visible
                                      : EVisibility::Collapsed)
                .Font(FCoreStyle::GetDefaultFontStyle("Regular", 9))
                .Text_Lambda([this, Which, bHasCount]()
                {
                    return bHasCount
                        ? FText::AsNumber(CountForChip(Which))
                        : FText::GetEmpty();
                })
                .ColorAndOpacity_Lambda([this, Flag, Which, OnGrey, OffGrey]()
                {
                    if (!*Flag)
                    {
                        return FSlateColor(OffGrey);
                    }
                    if (CountForChip(Which) > 0)
                    {
                        if (Which == EChipCount::StateMissing)
                        {
                            return FSlateColor(
                                FLinearColor(0.93f, 0.31f, 0.26f));
                        }
                        if (Which == EChipCount::StateOOD)
                        {
                            return FSlateColor(
                                FLinearColor(0.96f, 0.68f, 0.22f));
                        }
                    }
                    return FSlateColor(OnGrey);
                })
            ]
        ];
}

void SMapLinkManagerWindow::ApplyFilter()
{
    // Tokenised once per filter change, not once per row.
    FilterTokens.Reset();
    FilterText.TrimStartAndEnd().ParseIntoArrayWS(FilterTokens);

    FilteredRows.Reset();
    for (const TSharedPtr<FMapLinkManagerRow>& Row : AllRows)
    {
        // Text filter — v1.19: every whitespace-separated word must appear,
        // in any order. "bzn bar" finds LI_bzn_barn_01 because it contains
        // both, which is how people actually search a list of stems: they
        // remember two fragments of a name, not the substring between them.
        // Substring per token, not prefix — "bar" has to match "barn".
        if (!FilterTokens.IsEmpty())
        {
            bool bAllTokensMatch = true;
            for (const FString& Token : FilterTokens)
            {
                if (!Row->BlendName.Contains(Token, ESearchCase::IgnoreCase))
                {
                    bAllTokensMatch = false;
                    break;
                }
            }
            if (!bAllTokensMatch)
            {
                continue;
            }
        }

        // v1.13 — type chips (unknown types are always shown)
        const bool bKnownType = (Row->TypeStr == TEXT("LI") ||
                                 Row->TypeStr == TEXT("SM") ||
                                 Row->TypeStr == TEXT("BP"));
        if (bKnownType &&
            !((Row->TypeStr == TEXT("LI") && bFilterTypeLI) ||
              (Row->TypeStr == TEXT("SM") && bFilterTypeSM) ||
              (Row->TypeStr == TEXT("BP") && bFilterTypeBP)))
        {
            continue;
        }

        // v1.13 — state chips (precedence: Missing > Out-of-date > OK)
        const bool bStateOn =
            Row->bMissing   ? bFilterStateMissing :
            Row->bOutOfDate ? bFilterStateOOD
                            : bFilterStateOK;
        if (!bStateOn)
        {
            continue;
        }

        FilteredRows.Add(Row);
    }

    if (ListView.IsValid())
    {
        ListView->RequestListRefresh();
    }
}

TSharedRef<ITableRow> SMapLinkManagerWindow::OnGenerateRow(
    TSharedPtr<FMapLinkManagerRow> Item,
    const TSharedRef<STableViewBase>& OwnerTable)
{
    return SNew(SMapLinkManagerTableRow, OwnerTable)
        .Item(Item)
        .OwnerWindow(SharedThis(this));
}

// ---------------------------------------------------------------------------
// Toolbar actions
// ---------------------------------------------------------------------------

EActiveTimerReturnType SMapLinkManagerWindow::OnAutoRefreshTimer(
    double InCurrentTime, float /*InDeltaTime*/)
{
    // Only the cheap mtime heuristic ever runs automatically — never MD5.
    // Skip while the Settings tab is up.  Rebuild the list only when a
    // cheap signature actually changed, so an idle window never flickers
    // or drops the current selection.
    if (bAutoRefresh && ActiveTabIndex == static_cast<int32>(ETab::Assets))
    {
        const uint32 Sig = ComputeStateSignature();
        if (Sig != LastSignature)
        {
            LastSignature = Sig;
            RefreshRows(/*bVerifyHashes=*/false);
        }
    }
    return EActiveTimerReturnType::Continue;
}

uint32 SMapLinkManagerWindow::ComputeStateSignature() const
{
    uint32 Hash = 0;
    UMapLinkEditorState* EditorState = UMapLinkEditorState::Get();

    TArray<UMapLinkProxyAsset*> Proxies = CollectAllProxies();
    Hash = HashCombine(Hash, ::GetTypeHash(Proxies.Num()));

    for (UMapLinkProxyAsset* Proxy : Proxies)
    {
        if (!Proxy) continue;
        const FString& Path = Proxy->BlendFilePath;

        Hash = HashCombine(Hash, FCrc::StrCrc32(*Path));
        Hash = HashCombine(Hash, ::GetTypeHash((uint8)(Proxy->bAutoUpdate ? 1 : 0)));
        Hash = HashCombine(Hash, ::GetTypeHash((uint8)(Proxy->bAutoUpdateRecursive ? 1 : 0)));

        const FString Name = FPaths::GetBaseFilename(Path);
        Hash = HashCombine(Hash,
            ::GetTypeHash((uint8)(EditorState->IsFavourite(Name) ? 1 : 0)));

        // v1.13.1 — fold the proxy's import stamp into the signature so a
        // COMPLETED (re)import re-triggers the auto-refresh. A reimport
        // re-stamps the proxy but does NOT change the source .blend's mtime,
        // so without this the open Assets tab refreshed once at save time
        // (showing the PRE-reimport row: "Out of date" + a stale last-import
        // time) and then froze — the reimport's success never reached the UI
        // until a manual refresh.
        Hash = HashCombine(Hash, FCrc::StrCrc32(*Proxy->LastImportTimestamp));
        Hash = HashCombine(Hash, ::GetTypeHash(Proxy->ImportedBlendModTime.GetTicks()));

        if (!Path.IsEmpty() && FPaths::FileExists(Path))
        {
            const FDateTime Mod = IFileManager::Get().GetTimeStamp(*Path);
            Hash = HashCombine(Hash, ::GetTypeHash(Mod.GetTicks()));
        }
        else
        {
            // Missing files still contribute (so appearing/disappearing
            // is detected).
            Hash = HashCombine(Hash, 0xDEADu);
        }
    }

    // Active-expansion set affects the WP panel summary text.
    Hash = HashCombine(Hash,
        ::GetTypeHash(EditorState->ActiveExpansions.Num()));

    return Hash;
}

FReply SMapLinkManagerWindow::OnVerifyClicked()
{
    RefreshRows(/*bVerifyHashes=*/true);
    return FReply::Handled();
}

FReply SMapLinkManagerWindow::OnManualRefreshClicked()
{
    // v1.15 — unconditional cheap re-scan; sync the signature so the
    // auto-refresh timer doesn't immediately rebuild a second time.
    RefreshRows(/*bVerifyHashes=*/false);
    LastSignature = ComputeStateSignature();
    return FReply::Handled();
}

FReply SMapLinkManagerWindow::OnNewProjectClicked()
{
    FMapLinkProjectSetup::ShowNewProjectDialog();
    RefreshRows(false);
    return FReply::Handled();
}

FReply SMapLinkManagerWindow::OnOpenProjectClicked()
{
    FMapLinkProjectSetup::ShowOpenProjectDialog();
    RefreshRows(false);
    return FReply::Handled();
}

FReply SMapLinkManagerWindow::OnUnlinkProjectClicked()
{
    FMapLinkProjectSetup::UnlinkProject();
    RefreshRows(false);
    return FReply::Handled();
}

// ---------------------------------------------------------------------------
// Selection-based actions (v1.12)
// ---------------------------------------------------------------------------

TArray<TSharedPtr<FMapLinkManagerRow>>
SMapLinkManagerWindow::GetSelectedRows() const
{
    return ListView.IsValid()
        ? ListView->GetSelectedItems()
        : TArray<TSharedPtr<FMapLinkManagerRow>>();
}

bool SMapLinkManagerWindow::HasAnySelection() const
{
    return GetSelectedRows().Num() > 0;
}

bool SMapLinkManagerWindow::HasSingleSelection() const
{
    return GetSelectedRows().Num() == 1;
}

bool SMapLinkManagerWindow::SelectionAllBP() const
{
    const TArray<TSharedPtr<FMapLinkManagerRow>> Rows = GetSelectedRows();
    if (Rows.Num() == 0)
    {
        return false;
    }
    for (const TSharedPtr<FMapLinkManagerRow>& Row : Rows)
    {
        if (!Row.IsValid() || Row->TypeStr != TEXT("BP"))
        {
            return false;
        }
    }
    return true;
}

FReply SMapLinkManagerWindow::OnSelReimport()
{
    for (const TSharedPtr<FMapLinkManagerRow>& Row : GetSelectedRows())
    {
        if (Row.IsValid() && !Row->bMissing)
        {
            RowReimport(Row);
        }
    }
    return FReply::Handled();
}

FReply SMapLinkManagerWindow::OnSelForceReimport()
{
    // v1.14 — same semantics as "Force Full Reimport" in the MapLink >
    // Reimport menu: RunImport, ignoring hashes.  One explicit labeled
    // confirm for the whole selection (v1.13 dialog convention —
    // destructive semantics never map onto Yes/No).
    TArray<TSharedPtr<FMapLinkManagerRow>> Targets;
    for (const TSharedPtr<FMapLinkManagerRow>& Row : GetSelectedRows())
    {
        if (Row.IsValid() && !Row->bMissing && !Row->BlendPath.IsEmpty())
        {
            Targets.Add(Row);
        }
    }
    if (Targets.Num() == 0)
    {
        return FReply::Handled();
    }

    TSharedRef<SCustomDialog> Dlg =
        SNew(SCustomDialog)
        .Title(LOCTEXT("ForceReimportDlgTitle", "MapLink Force Full Reimport"))
        .Content()
        [
            SNew(STextBlock)
            .WrapTextAt(520.0f)
            .Text(FText::Format(LOCTEXT("ForceReimportDlgFmt",
                "Force Full Reimport {0} selected asset(s)?\n\n"
                "Every file in each tree is re-imported, ignoring hashes "
                "(nuke-and-recreate)."),
                FText::AsNumber(Targets.Num())))
        ]
        .Buttons({
            SCustomDialog::FButton(LOCTEXT("ForceReimportGoBtn",
                                           "Force Full Reimport")),
            SCustomDialog::FButton(LOCTEXT("ForceReimportCancelBtn", "Cancel"))
        });
    if (Dlg->ShowModal() != 0)
    {
        return FReply::Handled();
    }

    for (const TSharedPtr<FMapLinkManagerRow>& Row : Targets)
    {
        FMapLinkPipeline::RunImport(Row->BlendPath);
    }
    RefreshRows(false);
    return FReply::Handled();
}

FReply SMapLinkManagerWindow::OnSelUpdateStandIn()
{
    for (const TSharedPtr<FMapLinkManagerRow>& Row : GetSelectedRows())
    {
        if (Row.IsValid() && Row->TypeStr == TEXT("BP"))
        {
            RowUpdateStandIn(Row);
        }
    }
    return FReply::Handled();
}

FReply SMapLinkManagerWindow::OnSelBlender()
{
    const TArray<TSharedPtr<FMapLinkManagerRow>> Rows = GetSelectedRows();
    if (Rows.Num() > 0)
    {
        RowOpenBlender(Rows[0]);
    }
    return FReply::Handled();
}

FReply SMapLinkManagerWindow::OnSelBrowse()
{
    const TArray<TSharedPtr<FMapLinkManagerRow>> Rows = GetSelectedRows();
    if (Rows.Num() > 0)
    {
        RowOpenContentBrowser(Rows[0]);
    }
    return FReply::Handled();
}

FReply SMapLinkManagerWindow::OnSelExplorer()
{
    const TArray<TSharedPtr<FMapLinkManagerRow>> Rows = GetSelectedRows();
    if (Rows.Num() > 0)
    {
        RowOpenExplorer(Rows[0]);
    }
    return FReply::Handled();
}

FReply SMapLinkManagerWindow::OnSelProxy()
{
    const TArray<TSharedPtr<FMapLinkManagerRow>> Rows = GetSelectedRows();
    if (Rows.Num() > 0)
    {
        RowOpenProxy(Rows[0]);
    }
    return FReply::Handled();
}

FReply SMapLinkManagerWindow::OnSelViewer()
{
    const TArray<TSharedPtr<FMapLinkManagerRow>> Rows = GetSelectedRows();
    RowOpenInViewer(Rows.Num() > 0 ? Rows[0] : nullptr);
    return FReply::Handled();
}

void SMapLinkManagerWindow::OnRowDoubleClick(TSharedPtr<FMapLinkManagerRow> Item)
{
    RowOpenInViewer(Item);
}

TSharedPtr<SWidget> SMapLinkManagerWindow::OnRowContextMenu()
{
    const TArray<TSharedPtr<FMapLinkManagerRow>> Rows = GetSelectedRows();
    if (Rows.Num() == 0)
    {
        return nullptr;
    }

    const bool bSingle = (Rows.Num() == 1);

    FMenuBuilder MB(/*bCloseAfterSelection=*/true, nullptr);

    MB.BeginSection(NAME_None, FText::Format(
        LOCTEXT("CtxHeaderFmt", "Selection ({0})"),
        FText::AsNumber(Rows.Num())));

    MB.AddMenuEntry(
        LOCTEXT("CtxReimport", "Reimport"),
        LOCTEXT("CtxReimportTip",
            "Reimport every selected .blend (changed dependencies only)."),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateLambda([this]()
        {
            OnSelReimport();
        })));

    MB.AddMenuEntry(
        LOCTEXT("CtxForceReimport", "Force Full Reimport"),
        LOCTEXT("CtxForceReimportTip",
            "Re-import the whole tree of every selected .blend, ignoring "
            "hashes (confirmation shown)."),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateLambda([this]()
        {
            OnSelForceReimport();
        })));

    MB.AddMenuEntry(
        LOCTEXT("CtxViewer", "Open in Asset Viewer"),
        LOCTEXT("CtxViewerTip", "Preview in the read-only Asset Viewer."),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateLambda([this]()
        {
            OnSelViewer();
        })));

    MB.AddMenuEntry(
        LOCTEXT("CtxFavourite", "Toggle Favourite"),
        LOCTEXT("CtxFavouriteTip",
            "Pin / unpin the selected assets as favourites (favourites stay "
            "at the top and appear in the MapLink Reimport menu)."),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateLambda([this]()
        {
            for (const TSharedPtr<FMapLinkManagerRow>& Row : GetSelectedRows())
            {
                RowToggleFavourite(Row);
            }
        })));

    if (SelectionAllBP())
    {
        MB.AddMenuEntry(
            LOCTEXT("CtxStandIn", "Update Stand-in"),
            LOCTEXT("CtxStandInTip",
                "Refresh / recreate the Blueprint stand-ins (BP_ only)."),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([this]()
            {
                OnSelUpdateStandIn();
            })));
    }

    MB.EndSection();

    if (bSingle)
    {
        MB.BeginSection(NAME_None, LOCTEXT("CtxOpen", "Open"));
        MB.AddMenuEntry(
            LOCTEXT("CtxBlender", "Open in Blender"),
            LOCTEXT("CtxBlenderTip", "Open the source .blend in the Blender GUI."),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([this]()
            {
                OnSelBlender();
            })));
        MB.AddMenuEntry(
            LOCTEXT("CtxBrowse", "Show in Content Browser"),
            LOCTEXT("CtxBrowseTip", "Reveal the generated asset."),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([this]()
            {
                OnSelBrowse();
            })));
        MB.AddMenuEntry(
            LOCTEXT("CtxExplorer", "Show in Explorer"),
            LOCTEXT("CtxExplorerTip", "Open the .blend's folder."),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([this]()
            {
                OnSelExplorer();
            })));
        MB.AddMenuEntry(
            LOCTEXT("CtxProxy", "Open Proxy Asset"),
            LOCTEXT("CtxProxyTip", "Open the MapLink proxy in its editor."),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([this]()
            {
                OnSelProxy();
            })));
        MB.EndSection();
    }

    return MB.MakeWidget();
}

FReply SMapLinkManagerWindow::OnReimportOutOfDateClicked()
{
    TArray<FString> Targets;
    for (const TSharedPtr<FMapLinkManagerRow>& Row : AllRows)
    {
        if (Row->bOutOfDate && !Row->bMissing)
        {
            Targets.Add(Row->BlendPath);
        }
    }

    if (Targets.Num() == 0)
    {
        MapLinkNotify::Success(
            LOCTEXT("NothingOOD", "Everything is up to date."));
        return FReply::Handled();
    }

    for (const FString& BlendPath : Targets)
    {
        FMapLinkPipeline::RunReimport(BlendPath);
    }

    RefreshRows(false);
    return FReply::Handled();
}

FReply SMapLinkManagerWindow::OnWarmCacheClicked()
{
    const FString PluginBaseDir = FMapLinkPipeline::GetPluginBaseDir();

    TArray<FString> WarmList;
    for (const TSharedPtr<FMapLinkManagerRow>& Row : AllRows)
    {
        if (Row->bMissing) continue;
        if (Row->TypeStr != TEXT("LI") && Row->TypeStr != TEXT("SM")) continue;

        FMapLinkUtils::WaitForExportLock(Row->BlendName);
        if (FMapLinkUtils::IsFbxCacheFresh(
                Row->BlendPath, Row->BlendName))
        {
            continue;
        }
        WarmList.Add(Row->BlendPath);
    }

    if (WarmList.Num() == 0)
    {
        MapLinkNotify::Success(
            LOCTEXT("CacheWarm", "FBX cache is already warm for every entry."));
        return FReply::Handled();
    }

    const int32 Failed = FMapLinkBlenderBridge::RunBlenderExportsParallel(
        WarmList, PluginBaseDir, TEXT("manual_warm"));

    const FText WarmMsg = FText::FromString(FString::Printf(
        TEXT("Warm pass complete: %d exported, %d failed."),
        WarmList.Num() - Failed, Failed));
    if (Failed == 0)
        MapLinkNotify::Success(WarmMsg);
    else
        MapLinkNotify::Warn(WarmMsg);

    RefreshRows(false);
    return FReply::Handled();
}

// ---------------------------------------------------------------------------
// Row actions
// ---------------------------------------------------------------------------

void SMapLinkManagerWindow::RowReimport(TSharedPtr<FMapLinkManagerRow> Row)
{
    if (!Row.IsValid() || Row->BlendPath.IsEmpty()) return;
    FMapLinkPipeline::RunReimport(Row->BlendPath);
    RefreshRows(false);
}

void SMapLinkManagerWindow::RowOpenBlender(TSharedPtr<FMapLinkManagerRow> Row)
{
    if (!Row.IsValid()) return;
    FMapLinkBlenderBridge::OpenBlenderGUI(Row->BlendPath);
}

void SMapLinkManagerWindow::RowOpenExplorer(TSharedPtr<FMapLinkManagerRow> Row)
{
    if (!Row.IsValid()) return;
    FMapLinkBlenderBridge::OpenInExplorer(Row->BlendPath);
}

void SMapLinkManagerWindow::RowOpenContentBrowser(
    TSharedPtr<FMapLinkManagerRow> Row)
{
    if (!Row.IsValid()) return;
    UMapLinkProxyAsset* Proxy = Row->Proxy.Get();
    if (!Proxy) return;

    // Prefer the generated primary asset (LI_ world / SM mesh / BP); fall
    // back to the proxy itself.
    UObject* Target = Proxy->PrimaryAsset.LoadSynchronous();
    if (!Target)
    {
        Target = Proxy;
    }

    if (Target && GEditor)
    {
        TArray<UObject*> Objects = { Target };
        GEditor->SyncBrowserToObjects(Objects);
    }
}

void SMapLinkManagerWindow::RowOpenProxy(TSharedPtr<FMapLinkManagerRow> Row)
{
    if (!Row.IsValid()) return;
    UMapLinkProxyAsset* Proxy = Row->Proxy.Get();
    if (!Proxy || !GEditor) return;

    if (UAssetEditorSubsystem* AES =
            GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
    {
        AES->OpenEditorForAsset(Proxy);
    }
}

void SMapLinkManagerWindow::RowOpenInViewer(TSharedPtr<FMapLinkManagerRow> Row)
{
    // Read-only preview. Identity is the BlendName stem.
    const FString Stem = Row.IsValid() ? Row->BlendName : FString();
    if (FMapLinkEditorModule* Module =
            FModuleManager::GetModulePtr<FMapLinkEditorModule>("MapLinkEditor"))
    {
        Module->OpenAssetViewer(Stem);
    }
}

void SMapLinkManagerWindow::RowUpdateStandIn(TSharedPtr<FMapLinkManagerRow> Row)
{
    if (!Row.IsValid()) return;
    UMapLinkProxyAsset* Proxy = Row->Proxy.Get();
    if (!Proxy)
    {
        return;
    }

    // The stand-in is authored on the Blueprint that this proxy points at.
    UObject* Primary = Proxy->PrimaryAsset.LoadSynchronous();
    UBlueprint* BP = Cast<UBlueprint>(Primary);
    if (!BP)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Printf(
            TEXT("'%s' has no Blueprint stand-in to update.\n\n"
                 "Update Stand-in applies to BP_ stand-in assets."),
            *Row->BlendName)));
        return;
    }

    FMapLinkStandInReport Report(TEXT("refresh"), BP->GetPathName());
    const bool bOK = FMapLinkBPStandInCreator::RefreshStandIn(BP, Report);

    if (bOK)
        MapLinkNotify::Success(FText::FromString(FString::Printf(
            TEXT("Stand-in for %s updated."), *Row->BlendName)));
    else
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Printf(
            TEXT("Stand-in update for %s failed (see Output Log)."),
            *Row->BlendName)));

    RefreshRows(false);
}

void SMapLinkManagerWindow::RowToggleFavourite(
    TSharedPtr<FMapLinkManagerRow> Row)
{
    if (!Row.IsValid()) return;

    UMapLinkEditorState::Get()->ToggleFavourite(Row->BlendName);
    Row->bFavourite = !Row->bFavourite;

    // Re-pin: favourites move to the top immediately.
    SortRows();
    ApplyFilter();
}

void SMapLinkManagerWindow::RowSetAutoUpdate(
    TSharedPtr<FMapLinkManagerRow> Row, bool bEnabled)
{
    if (!Row.IsValid()) return;
    UMapLinkProxyAsset* Proxy = Row->Proxy.Get();
    if (!Proxy) return;

    if (Proxy->bAutoUpdate != bEnabled)
    {
        Proxy->bAutoUpdate = bEnabled;
        Proxy->MarkPackageDirty();
        FMapLinkPipeline::SaveProxyPackage(Proxy);
    }
}

void SMapLinkManagerWindow::RowSetRecursive(
    TSharedPtr<FMapLinkManagerRow> Row, bool bEnabled)
{
    if (!Row.IsValid()) return;
    UMapLinkProxyAsset* Proxy = Row->Proxy.Get();
    if (!Proxy) return;

    if (Proxy->bAutoUpdateRecursive != bEnabled)
    {
        Proxy->bAutoUpdateRecursive = bEnabled;
        Proxy->MarkPackageDirty();
        FMapLinkPipeline::SaveProxyPackage(Proxy);
    }
}

void SMapLinkManagerWindow::RowSetSkipAutoReimport(
    TSharedPtr<FMapLinkManagerRow> Row, bool bEnabled)
{
    if (!Row.IsValid()) return;
    UMapLinkProxyAsset* Proxy = Row->Proxy.Get();
    if (!Proxy) return;

    if (Proxy->bSkipAutoReimport != bEnabled)
    {
        Proxy->bSkipAutoReimport = bEnabled;
        Row->bSkipAutoReimport = bEnabled;
        Proxy->MarkPackageDirty();
        FMapLinkPipeline::SaveProxyPackage(Proxy);
    }
}

// ---------------------------------------------------------------------------
// v1.18 — Context strip
// ---------------------------------------------------------------------------
//
// Every destructive button in this window — Reimport, Force Full Reimport,
// Conform All, Expand, Cleanup All in Level — raises the same question, and
// nothing on screen answered it: what am I about to affect? The project root
// was on the Project tab, the open level was nowhere, and the number of
// expansions living in that level was three clicks away.

FText SMapLinkManagerWindow::GetContextStripText() const
{
    TArray<FString> Parts;

    FString Root;
    if (FMapLinkProjectSetup::IsSetupActive(&Root))
    {
        Parts.Add(FString::Printf(TEXT("Project: %s"),
            *FPaths::GetCleanFilename(Root.TrimEnd())));
    }
    else
    {
        Parts.Add(TEXT("Project: none linked"));
    }

    const UWorld* World =
        GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (World)
    {
        const FString LevelName =
            FPaths::GetBaseFilename(World->GetOutermost()->GetName());
        Parts.Add(FString::Printf(TEXT("Level: %s%s"),
            *LevelName,
            World->GetWorldPartition() ? TEXT(" (WP)") : TEXT("")));

        // Expansions targeting the level that is actually open — the ones a
        // reimport or a cleanup here would touch.
        const FSoftObjectPath OpenLevel(World->GetPathName());
        int32 Here = 0;
        for (const FMapLinkWPExpansionRecord& Rec :
             UMapLinkEditorState::Get()->ActiveExpansions)
        {
            if (Rec.TargetLevel == OpenLevel)
            {
                ++Here;
            }
        }
        if (Here > 0)
        {
            Parts.Add(FString::Printf(TEXT("%d expansion%s here"),
                Here, Here == 1 ? TEXT("") : TEXT("s")));
        }
    }

    return FText::FromString(FString::Join(Parts, TEXT("   ·   ")));
}

TSharedRef<SWidget> SMapLinkManagerWindow::BuildContextStrip()
{
    return SNew(SBorder)
        .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
        .Padding(FMargin(8.f, 3.f))
        [
            SNew(STextBlock)
            .Text(this, &SMapLinkManagerWindow::GetContextStripText)
            .ColorAndOpacity(FSlateColor::UseSubduedForeground())
        ];
}

// ---------------------------------------------------------------------------
// v1.17 — Tab strip helper
// ---------------------------------------------------------------------------

TSharedRef<SWidget> SMapLinkManagerWindow::MakeTabButton(
    ETab Tab, const TAttribute<FText>& Label, const TAttribute<FText>& Tooltip)
{
    const int32 Index = static_cast<int32>(Tab);

    return SNew(SCheckBox)
        .Style(FAppStyle::Get(), "ToggleButtonCheckbox")
        .ToolTipText(Tooltip)
        .IsChecked_Lambda([this, Index]()
        {
            return ActiveTabIndex == Index
                ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
        })
        .OnCheckStateChanged_Lambda([this, Index](ECheckBoxState)
        {
            ActiveTabIndex = Index;
        })
        [
            SNew(SBox).Padding(FMargin(12.f, 4.f))
            [
                SNew(STextBlock).Text(Label)
            ]
        ];
}

// ---------------------------------------------------------------------------
// Expansions — create form
// ---------------------------------------------------------------------------

void SMapLinkManagerWindow::RebuildLIRootOptions()
{
    const FString PreviousSelection =
        SelectedRoot.IsValid() ? *SelectedRoot : FString();

    LIRootOptions.Reset();
    for (const TSharedPtr<FMapLinkManagerRow>& Row : AllRows)
    {
        if (Row.IsValid() && Row->TypeStr == TEXT("LI"))
        {
            LIRootOptions.Add(MakeShared<FString>(Row->BlendName));
        }
    }

    // Selection priority:
    //   1. keep the user's previous selection if still valid
    //   2. default to the last imported .blend (if it's an LI_)
    //   3. first option
    SelectedRoot.Reset();

    auto SelectByName = [this](const FString& Name) -> bool
    {
        for (const TSharedPtr<FString>& Option : LIRootOptions)
        {
            if (*Option == Name)
            {
                SelectedRoot = Option;
                return true;
            }
        }
        return false;
    };

    bool bSelected = false;
    if (!PreviousSelection.IsEmpty())
    {
        bSelected = SelectByName(PreviousSelection);
    }
    if (!bSelected)
    {
        const FString& LastImported =
            UMapLinkEditorState::Get()->LastImportedBlendName;
        if (!LastImported.IsEmpty())
        {
            bSelected = SelectByName(LastImported);
        }
    }
    if (!bSelected && LIRootOptions.Num() > 0)
    {
        SelectedRoot = LIRootOptions[0];
    }

    if (RootCombo.IsValid())
    {
        RootCombo->RefreshOptions();
        RootCombo->SetSelectedItem(SelectedRoot);
    }
}

FText SMapLinkManagerWindow::GetSelectedRootText() const
{
    return FText::FromString(
        SelectedRoot.IsValid() ? *SelectedRoot
                               : TEXT("<no LI_ proxies>"));
}

FString SMapLinkManagerWindow::GetWPLevelObjectPath() const
{
    return SelectedWPLevelPath;
}

void SMapLinkManagerWindow::OnWPLevelChanged(const FAssetData& AssetData)
{
    SelectedWPLevelPath = AssetData.IsValid()
        ? AssetData.GetObjectPathString()
        : FString();
}

bool SMapLinkManagerWindow::ShouldFilterOutLevel(const FAssetData& AssetData) const
{
    // v1.17 — hide non-World-Partition levels from the picker.  Expansion
    // only works in WP maps; making that a pick-time constraint instead of a
    // post-load error is the difference between "not offered" and "you just
    // waited thirty seconds to be told no".
    return !FMapLinkWPExpansionHelper::IsWorldPartitionLevel(
        FSoftObjectPath(AssetData.GetObjectPathString()));
}

void SMapLinkManagerWindow::RefreshExpansionPreview()
{
    const FString Root = SelectedRoot.IsValid() ? *SelectedRoot : FString();
    const FString Key  = FString::Printf(TEXT("%s|%d"), *Root, ExpandDepth);

    if (Key == PreviewKey)
    {
        return;   // nothing changed — the walk is cheap, but not free
    }
    PreviewKey   = Key;
    PreviewCache = FMapLinkWPExpansionHelper::PreviewExpansion(Root, ExpandDepth);
}

FText SMapLinkManagerWindow::GetDepthSuffixText() const
{
    if (!SelectedRoot.IsValid())
    {
        return FText::GetEmpty();
    }
    return FText::Format(LOCTEXT("DepthOfFmt", "of {0}"),
        FText::AsNumber(PreviewCache.MaxDepth));
}

FText SMapLinkManagerWindow::GetExpansionPreviewText() const
{
    if (!SelectedRoot.IsValid())
    {
        return LOCTEXT("PreviewNoRoot",
            "Select an LI_ root to see what will be placed.");
    }

    const FMapLinkExpansionPreview& P = PreviewCache;

    if (P.TotalPlacements() == 0 && P.NumBrokenLI == 0)
    {
        return LOCTEXT("PreviewNothing",
            "Nothing to place — no analysis.json for this root yet. "
            "Reimport it first.");
    }

    FString Text = FString::Printf(
        TEXT("Will break open %d LI_ and place %d actor(s):  %d SM_ · %d BP_"),
        P.NumBrokenLI, P.TotalPlacements(), P.NumSM, P.NumBP);

    if (P.NumLI > 0)
    {
        Text += FString::Printf(
            TEXT(" · %d LI_ left as Level Instances at the depth boundary"),
            P.NumLI);
    }
    if (P.bIncomplete)
    {
        Text += TEXT("   (incomplete — some analysis.json files are missing, "
                     "those assets fall back to Level Instances)");
    }

    return FText::FromString(Text);
}

bool SMapLinkManagerWindow::CanExpandNow() const
{
    return SelectedRoot.IsValid() && !SelectedWPLevelPath.IsEmpty();
}

FText SMapLinkManagerWindow::GetExpandTooltip() const
{
    if (!SelectedRoot.IsValid())
    {
        return LOCTEXT("ExpandTipNoRoot", "Select a root LI_ first.");
    }
    if (SelectedWPLevelPath.IsEmpty())
    {
        return LOCTEXT("ExpandTipNoLevel",
            "Select a target World Partition level first.");
    }
    return LOCTEXT("WPExpandTip",
        "Expand the root hierarchy into the WP level under outliner folder "
        "MapLink/<Label>, and record it so reimports keep it up to date.\n\n"
        "This writes actors into a source-controlled level and cannot be "
        "undone with Ctrl+Z — use Cleanup to remove them.");
}

FReply SMapLinkManagerWindow::OnExpandClicked()
{
    if (!CanExpandNow())
    {
        return FReply::Handled();
    }

    const FString         Root = *SelectedRoot;
    const FSoftObjectPath LevelPath(SelectedWPLevelPath);
    const FString         Label = NewExpansionLabel.TrimStartAndEnd();

    UMapLinkEditorState* State = UMapLinkEditorState::Get();

    // v1.17 — an existing (root, level) pair with the SAME label is an
    // update; a different label is a second, independent expansion.  v1.16
    // could only ever do the former, and did it silently even when the level
    // differed — which is how actors got stranded.
    FMapLinkWPExpansionRecord Record;
    bool bReplacing = false;
    for (const FMapLinkWPExpansionRecord& Existing : State->ActiveExpansions)
    {
        if (Existing.RootBlendName == Root &&
            Existing.TargetLevel == LevelPath &&
            Existing.Label == Label)
        {
            Record     = Existing;
            bReplacing = true;
            break;
        }
    }
    if (!bReplacing)
    {
        Record.RootBlendName = Root;
        Record.TargetLevel   = LevelPath;
        Record.Label         = Label;
    }
    Record.ExpandDepth = ExpandDepth;

    const FMapLinkExpansionPreview Preview =
        FMapLinkWPExpansionHelper::PreviewExpansion(Root, ExpandDepth);

    // Destructive confirm — the one modal this flow keeps, because it is a
    // decision: hundreds of actors into a source-controlled level, no undo.
    const FText Message = FText::FromString(FString::Printf(
        TEXT("%s '%s' into %s at depth %d?\n\n")
        TEXT("Breaks open %d LI_ and places about %d actor(s) "
             "(%d SM_, %d BP_, %d LI_).\n")
        TEXT("Outliner folder: %s\n\n")
        TEXT("The level is saved on completion. This cannot be undone with "
             "Ctrl+Z — use Cleanup to remove the expansion."),
        bReplacing ? TEXT("Re-expand") : TEXT("Expand"),
        *Record.GetLabel(),
        *FPaths::GetBaseFilename(LevelPath.ToString()),
        ExpandDepth,
        Preview.NumBrokenLI, Preview.TotalPlacements(),
        Preview.NumSM, Preview.NumBP, Preview.NumLI,
        *Record.GetFolderPath()));

    if (FMessageDialog::Open(EAppMsgType::YesNo, Message,
            LOCTEXT("WPExpandTitle", "MapLink — Expand")) != EAppReturnType::Yes)
    {
        return FReply::Handled();
    }

    const EMapLinkResult Result =
        FMapLinkWPExpansionHelper::ExpandRecord(Record);

    if (Result == EMapLinkResult::Success)
    {
        // v1.17 — success is a toast, not silence.  v1.16 raised a modal on
        // failure and said nothing at all on success, which is backwards.
        MapLinkNotify::Success(FText::FromString(FString::Printf(
            TEXT("Expanded '%s' into %s — %d actor(s) placed."),
            *Record.GetLabel(), *Record.GetLevelName(),
            Record.LastPlacedCount)));
        NewExpansionLabel.Reset();
    }
    else
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
            NSLOCTEXT("MapLink", "WPExpandFailedFmt",
                "WP Expansion failed for '{0}': {1}\n"
                "Check Output Log for details."),
            FText::FromString(Root), MapLinkResultToText(Result)));
    }

    RefreshRows(false);
    RefreshExpansionRows();
    return FReply::Handled();
}

FReply SMapLinkManagerWindow::OnCleanupClicked()
{
    // Kept for the legacy "clean this root out of the picked level" gesture.
    if (!SelectedRoot.IsValid())
    {
        MapLinkNotify::Warn(LOCTEXT("NoRootClean",
            "Select a root LI_ first."));
        return FReply::Handled();
    }

    const FString Root = *SelectedRoot;

    FSoftObjectPath LevelPath;
    if (const FMapLinkWPExpansionRecord* Rec =
            UMapLinkEditorState::Get()->FindFirstExpansionForRoot(Root))
    {
        LevelPath = Rec->TargetLevel;
    }
    if (LevelPath.IsNull() && !SelectedWPLevelPath.IsEmpty())
    {
        LevelPath = FSoftObjectPath(SelectedWPLevelPath);
    }

    if (LevelPath.IsNull())
    {
        MapLinkNotify::Warn(LOCTEXT("NoLevelClean",
            "No recorded expansion for this root — select the WP level to "
            "clean in the picker."));
        return FReply::Handled();
    }

    const EMapLinkResult Result =
        FMapLinkWPExpansionHelper::CleanupWP(Root, LevelPath);

    if (Result != EMapLinkResult::Success)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
            NSLOCTEXT("MapLink", "WPCleanFailedFmt",
                "WP Cleanup failed for '{0}': {1}\n"
                "Check Output Log for details."),
            FText::FromString(Root), MapLinkResultToText(Result)));
    }

    RefreshRows(false);
    RefreshExpansionRows();
    return FReply::Handled();
}

// ---------------------------------------------------------------------------
// Expansions — list
// ---------------------------------------------------------------------------

void SMapLinkManagerWindow::RefreshExpansionRows()
{
    ExpansionRows.Reset();

    UMapLinkEditorState* EditorState = UMapLinkEditorState::Get();

    // Everything below is derived from data the Assets list already holds,
    // so building this costs one pass over AllRows and no disk access.
    TSet<FString>            KnownStems;
    TMap<FString, FDateTime> LastImportByStem;
    for (const TSharedPtr<FMapLinkManagerRow>& Row : AllRows)
    {
        if (!Row.IsValid()) continue;
        KnownStems.Add(Row->BlendName);
        if (Row->bHasTimestamp)
        {
            LastImportByStem.Add(Row->BlendName, Row->LastImportTime);
        }
    }

    const IAssetRegistry& AR =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
            "AssetRegistry").Get();

    for (const FMapLinkWPExpansionRecord& Rec : EditorState->ActiveExpansions)
    {
        TSharedPtr<FMapLinkExpansionRow> Row = MakeShared<FMapLinkExpansionRow>();
        Row->Id                = Rec.Id;
        Row->Label             = Rec.GetLabel();
        Row->RootBlendName     = Rec.RootBlendName;
        Row->TargetLevel       = Rec.TargetLevel;
        Row->LevelName         = Rec.GetLevelName();
        Row->ExpandDepth       = Rec.ExpandDepth;
        Row->PlacedCount       = Rec.LastPlacedCount;
        Row->bReExpandOnImport = Rec.bReExpandOnImport;

        const bool bEverExpanded = (Rec.LastExpandedUtc.GetTicks() != 0);
        Row->LastExpandedStr = bEverExpanded
            ? Rec.LastExpandedUtc.ToString(TEXT("%Y-%m-%d %H:%M"))
            : TEXT("—");

        // State, most-severe first.
        if (!KnownStems.Contains(Rec.RootBlendName))
        {
            Row->State = FMapLinkExpansionRow::EState::RootMissing;
        }
        else if (Rec.TargetLevel.IsNull() ||
                 !AR.GetAssetByObjectPath(Rec.TargetLevel).IsValid())
        {
            Row->State = FMapLinkExpansionRow::EState::LevelMissing;
        }
        else if (!bEverExpanded)
        {
            Row->State = FMapLinkExpansionRow::EState::NeverExpanded;
        }
        else if (!Rec.bReExpandOnImport)
        {
            Row->State = FMapLinkExpansionRow::EState::Paused;
        }
        else
        {
            // Stale = some dependency reimported after we last expanded.
            TSet<FString> Descendants;
            FMapLinkWPExpansionHelper::CollectDescendantNames(
                Rec.RootBlendName, Descendants);

            Row->State = FMapLinkExpansionRow::EState::InSync;
            for (const FString& Stem : Descendants)
            {
                if (const FDateTime* Imported = LastImportByStem.Find(Stem))
                {
                    if (*Imported > Rec.LastExpandedUtc)
                    {
                        Row->State = FMapLinkExpansionRow::EState::Stale;
                        break;
                    }
                }
            }
        }

        ExpansionRows.Add(Row);
    }

    // v1.18 — the ini is not the whole truth: the actors are in a
    // source-controlled level. Append whatever is physically there.
    AppendDiscoveredExpansionRows();

    ExpansionRows.Sort(
        [](const TSharedPtr<FMapLinkExpansionRow>& A,
           const TSharedPtr<FMapLinkExpansionRow>& B)
        {
            // Unrecorded expansions first — they are the ones that need a
            // decision; everything else is already under control.
            const bool ADisc = A->bDiscovered, BDisc = B->bDiscovered;
            if (ADisc != BDisc)
            {
                return ADisc;
            }
            if (A->LevelName != B->LevelName)
            {
                return A->LevelName < B->LevelName;
            }
            return A->Label < B->Label;
        });

    if (ExpansionListView.IsValid())
    {
        ExpansionListView->RequestListRefresh();
    }
}

// ── v1.18 — expansions found in the level, not in the ini ─────────────────

void SMapLinkManagerWindow::AppendDiscoveredExpansionRows()
{
    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        return;
    }

    const FSoftObjectPath OpenLevel(World->GetPathName());
    const FString OpenLevelName = FPaths::GetBaseFilename(OpenLevel.ToString());

    // What this machine already claims in the level that is open.
    TSet<FString> RecordedIdTags;
    TSet<FString> RecordedLabels;
    for (const FMapLinkWPExpansionRecord& Rec :
         UMapLinkEditorState::Get()->ActiveExpansions)
    {
        if (Rec.TargetLevel != OpenLevel)
        {
            continue;
        }
        RecordedIdTags.Add(Rec.GetIdTag());
        // Also by label: a record whose actors were placed before v1.17
        // carries an Id the actors do not, so the id match cannot find it.
        RecordedLabels.Add(Rec.GetLabel());
    }

    for (const FMapLinkDiscoveredExpansion& Found :
         FMapLinkWPExpansionHelper::ScanExpansionsInWorld(World))
    {
        if (!Found.IdTag.IsEmpty() && RecordedIdTags.Contains(Found.IdTag))
        {
            continue;   // mine, already listed
        }
        if (Found.IdTag.IsEmpty() && RecordedLabels.Contains(Found.Label))
        {
            continue;   // mine, placed pre-v1.17
        }

        TSharedPtr<FMapLinkExpansionRow> Row = MakeShared<FMapLinkExpansionRow>();
        Row->bDiscovered         = true;
        Row->DiscoveredIdTag     = Found.IdTag;
        Row->DiscoveredRootGuess = Found.RootGuess;
        Row->bStreamedOut        = Found.bStreamedOut;
        Row->Label               = Found.Label;
        Row->RootBlendName       = Found.RootGuess;
        Row->TargetLevel         = OpenLevel;
        Row->LevelName           = OpenLevelName;
        Row->PlacedCount         = Found.ActorCount;
        Row->bReExpandOnImport   = false;
        Row->LastExpandedStr     = Found.bStreamedOut
            ? TEXT("(streamed out)") : TEXT("—");
        Row->State = FMapLinkExpansionRow::EState::NotInRecords;

        ExpansionRows.Add(Row);
    }
}

void SMapLinkManagerWindow::OnAdoptExpansion(
    TSharedPtr<FMapLinkExpansionRow> Item)
{
    if (!Item.IsValid() || !Item->bDiscovered)
    {
        return;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        return;
    }

    if (Item->bStreamedOut)
    {
        MapLinkNotify::Warn(LOCTEXT("AdoptStreamedOut",
            "That expansion's actors are streamed out. Load its cells in the "
            "outliner first — adopting now would produce a record matching no "
            "actor."));
        return;
    }

    // The root the record will re-expand from. The folder name is a good
    // guess and usually right, but it is a LABEL, so confirm it against the
    // known LI_ proxies and fall back to the form's selection.
    FString Root = Item->DiscoveredRootGuess;
    bool bKnownRoot = false;
    for (const TSharedPtr<FString>& Option : LIRootOptions)
    {
        if (Option.IsValid() && *Option == Root)
        {
            bKnownRoot = true;
            break;
        }
    }
    if (!bKnownRoot && SelectedRoot.IsValid())
    {
        Root = *SelectedRoot;
    }
    if (Root.IsEmpty())
    {
        MapLinkNotify::Warn(LOCTEXT("AdoptNoRoot",
            "Pick the root LI_ this expansion came from in the form above, "
            "then adopt again."));
        return;
    }

    const FText Message = FText::FromString(FString::Printf(
        TEXT("Adopt '%s' in %s?\n\n")
        TEXT("%d actor(s) will be re-tagged with a new expansion id so this "
             "machine can track, re-expand and clean them.\n\n")
        TEXT("Root:  %s%s\n")
        TEXT("Depth: %d  (not recoverable from a level — this is what the "
             "next re-expansion will use)\n\n")
        TEXT("The level is saved on completion."),
        *Item->Label, *Item->LevelName, Item->PlacedCount,
        *Root,
        bKnownRoot ? TEXT("") : TEXT("   (guessed — from the form above)"),
        ExpandDepth));

    if (FMessageDialog::Open(EAppMsgType::YesNo, Message,
            LOCTEXT("AdoptTitle", "MapLink — Adopt Expansion"))
        != EAppReturnType::Yes)
    {
        return;
    }

    FMapLinkDiscoveredExpansion Discovered;
    Discovered.IdTag        = Item->DiscoveredIdTag;
    Discovered.Label        = Item->Label;
    Discovered.RootGuess    = Item->DiscoveredRootGuess;
    Discovered.ActorCount   = Item->PlacedCount;
    Discovered.bStreamedOut = Item->bStreamedOut;

    const EMapLinkResult Result =
        FMapLinkWPExpansionHelper::AdoptDiscoveredExpansion(
            World, Discovered, Root, ExpandDepth, Item->TargetLevel);

    if (Result != EMapLinkResult::Success)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
            NSLOCTEXT("MapLink", "AdoptFailedFmt",
                "Adopt failed for '{0}': {1}\nCheck Output Log for details."),
            FText::FromString(Item->Label), MapLinkResultToText(Result)));
    }

    RefreshRows(false);
    RefreshExpansionRows();
}

void SMapLinkManagerWindow::OnRemoveDiscoveredExpansion(
    TSharedPtr<FMapLinkExpansionRow> Item)
{
    if (!Item.IsValid() || !Item->bDiscovered)
    {
        return;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        return;
    }

    const FText Message = FText::FromString(FString::Printf(
        TEXT("Remove '%s' from %s?\n\n")
        TEXT("%d loaded actor(s) and the MapLink/%s folder tree are deleted, "
             "and the level is saved.\n\n")
        TEXT("This expansion is not in your records — it was placed by "
             "another machine, or stranded by an older MapLink version. "
             "Deleting it removes it for everyone."),
        *Item->Label, *Item->LevelName, Item->PlacedCount, *Item->Label));

    if (FMessageDialog::Open(EAppMsgType::YesNo, Message,
            LOCTEXT("RemoveDiscTitle", "MapLink — Remove Untracked Expansion"))
        != EAppReturnType::Yes)
    {
        return;
    }

    FMapLinkDiscoveredExpansion Discovered;
    Discovered.IdTag        = Item->DiscoveredIdTag;
    Discovered.Label        = Item->Label;
    Discovered.ActorCount   = Item->PlacedCount;
    Discovered.bStreamedOut = Item->bStreamedOut;

    const int32 Removed =
        FMapLinkWPExpansionHelper::RemoveDiscoveredExpansion(World, Discovered);

    FEditorFileUtils::SaveCurrentLevel();

    MapLinkNotify::Success(FText::FromString(FString::Printf(
        TEXT("Removed untracked expansion '%s' — %d actor(s)."),
        *Item->Label, Removed)));

    RefreshRows(false);
    RefreshExpansionRows();
}

TSharedRef<ITableRow> SMapLinkManagerWindow::OnGenerateExpansionRow(
    TSharedPtr<FMapLinkExpansionRow> Item,
    const TSharedRef<STableViewBase>& OwnerTable)
{
    return SNew(SMapLinkExpansionTableRow, OwnerTable)
        .Item(Item)
        .OwnerWindow(SharedThis(this));
}

TArray<TSharedPtr<FMapLinkExpansionRow>>
SMapLinkManagerWindow::GetSelectedExpansions() const
{
    return ExpansionListView.IsValid()
        ? ExpansionListView->GetSelectedItems()
        : TArray<TSharedPtr<FMapLinkExpansionRow>>();
}

FText SMapLinkManagerWindow::GetExpansionSummaryText() const
{
    if (ExpansionRows.Num() == 0)
    {
        return LOCTEXT("NoExpansions", "No expansions yet");
    }

    int32 Stale = 0, Paused = 0, Broken = 0, Actors = 0;
    int32 Mine = 0, Untracked = 0;
    TSet<FString> Levels;
    for (const TSharedPtr<FMapLinkExpansionRow>& Row : ExpansionRows)
    {
        if (!Row.IsValid()) continue;
        Levels.Add(Row->LevelName);
        Actors += Row->PlacedCount;
        if (Row->bDiscovered)
        {
            ++Untracked;
            continue;
        }
        ++Mine;
        switch (Row->State)
        {
        case FMapLinkExpansionRow::EState::Stale:        ++Stale;  break;
        case FMapLinkExpansionRow::EState::Paused:       ++Paused; break;
        case FMapLinkExpansionRow::EState::LevelMissing:
        case FMapLinkExpansionRow::EState::RootMissing:  ++Broken; break;
        default: break;
        }
    }

    TArray<FString> Parts;
    Parts.Add(FString::Printf(TEXT("%d expansion%s in %d level%s"),
        Mine, Mine == 1 ? TEXT("") : TEXT("s"),
        Levels.Num(), Levels.Num() == 1 ? TEXT("") : TEXT("s")));
    Parts.Add(FString::Printf(TEXT("%d actor(s)"), Actors));
    if (Stale  > 0) { Parts.Add(FString::Printf(TEXT("%d stale"), Stale)); }
    if (Paused > 0) { Parts.Add(FString::Printf(TEXT("%d paused"), Paused)); }
    if (Broken > 0) { Parts.Add(FString::Printf(TEXT("%d broken"), Broken)); }
    if (Untracked > 0)
    {
        Parts.Add(FString::Printf(TEXT("%d in level but not yours"), Untracked));
    }

    return FText::FromString(FString::Join(Parts, TEXT("  ·  ")));
}

TSharedPtr<SWidget> SMapLinkManagerWindow::OnExpansionContextMenu()
{
    const TArray<TSharedPtr<FMapLinkExpansionRow>> Selected = GetSelectedExpansions();
    if (Selected.Num() == 0)
    {
        return nullptr;
    }

    const bool bSingle = (Selected.Num() == 1);
    TSharedPtr<FMapLinkExpansionRow> First = Selected[0];

    FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection=*/true, nullptr);

    // v1.18 — a discovered row has no record behind it, so none of the
    // record-driven verbs apply. It gets the two that do.
    if (bSingle && First.IsValid() && First->bDiscovered)
    {
        Menu.BeginSection("MapLinkExpansionDiscovered",
            LOCTEXT("ExpMenuDiscovered", "Untracked expansion"));
        {
            Menu.AddMenuEntry(
                LOCTEXT("ExpMenuAdopt", "Adopt…"),
                LOCTEXT("ExpMenuAdoptTip",
                    "Take ownership: mint a record for this expansion and "
                    "re-tag its actors so you can re-expand and clean it."),
                FSlateIcon(),
                FUIAction(FExecuteAction::CreateLambda([this, First]()
                {
                    OnAdoptExpansion(First);
                })));

            Menu.AddMenuEntry(
                LOCTEXT("ExpMenuRemoveDisc", "Remove from Level…"),
                LOCTEXT("ExpMenuRemoveDiscTip",
                    "Delete this expansion's actors and folders. It is not "
                    "yours — deleting it removes it for everyone."),
                FSlateIcon(),
                FUIAction(FExecuteAction::CreateLambda([this, First]()
                {
                    OnRemoveDiscoveredExpansion(First);
                })));

            Menu.AddMenuEntry(
                LOCTEXT("ExpMenuOpenLevel", "Open Level"),
                LOCTEXT("ExpMenuOpenLevelTip",
                    "Open the target level in the editor."),
                FSlateIcon(),
                FUIAction(FExecuteAction::CreateLambda([this, First]()
                {
                    OnOpenExpansionLevel(First);
                })));
        }
        Menu.EndSection();
        return Menu.MakeWidget();
    }

    Menu.BeginSection("MapLinkExpansionActions",
        LOCTEXT("ExpMenuActions", "Expansion"));
    {
        Menu.AddMenuEntry(
            LOCTEXT("ExpMenuReExpand", "Re-expand"),
            LOCTEXT("ExpMenuReExpandTip",
                "Rebuild from the current .blend hierarchy."),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([this]()
            {
                OnReExpandSelected();
            })));

        Menu.AddMenuEntry(
            LOCTEXT("ExpMenuCleanup", "Cleanup"),
            LOCTEXT("ExpMenuCleanupTip",
                "Remove this expansion's actors and folders, drop the record."),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([this]()
            {
                OnCleanupSelectedExpansions();
            })));

        if (bSingle)
        {
            Menu.AddMenuEntry(
                LOCTEXT("ExpMenuRename", "Rename…"),
                LOCTEXT("ExpMenuRenameTip",
                    "Change the label. The outliner folder is renamed on the "
                    "next expansion."),
                FSlateIcon(),
                FUIAction(FExecuteAction::CreateLambda([this, First]()
                {
                    OnRenameExpansion(First);
                })));

            Menu.AddMenuEntry(
                LOCTEXT("ExpMenuOpenLevel", "Open Level"),
                LOCTEXT("ExpMenuOpenLevelTip",
                    "Open the target level in the editor."),
                FSlateIcon(),
                FUIAction(FExecuteAction::CreateLambda([this, First]()
                {
                    OnOpenExpansionLevel(First);
                })));
        }
    }
    Menu.EndSection();

    Menu.BeginSection("MapLinkExpansionAuto",
        LOCTEXT("ExpMenuAuto", "Re-expand on import"));
    {
        Menu.AddMenuEntry(
            LOCTEXT("ExpMenuPause", "Pause"),
            LOCTEXT("ExpMenuPauseTip",
                "Stop re-expanding on dependency reimport. The actors stay "
                "in the level."),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([this, Selected]()
            {
                for (const TSharedPtr<FMapLinkExpansionRow>& Row : Selected)
                {
                    OnSetExpansionPaused(Row, true);
                }
            })));

        Menu.AddMenuEntry(
            LOCTEXT("ExpMenuResume", "Resume"),
            LOCTEXT("ExpMenuResumeTip",
                "Re-expand again on dependency reimport."),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([this, Selected]()
            {
                for (const TSharedPtr<FMapLinkExpansionRow>& Row : Selected)
                {
                    OnSetExpansionPaused(Row, false);
                }
            })));
    }
    Menu.EndSection();

    return Menu.MakeWidget();
}

void SMapLinkManagerWindow::OnExpansionDoubleClick(
    TSharedPtr<FMapLinkExpansionRow> Item)
{
    OnOpenExpansionLevel(Item);
}

void SMapLinkManagerWindow::OnOpenExpansionLevel(
    TSharedPtr<FMapLinkExpansionRow> Item)
{
    if (!Item.IsValid() || Item->TargetLevel.IsNull())
    {
        return;
    }

    FString PackageName = Item->TargetLevel.GetLongPackageName();
    if (PackageName.IsEmpty())
    {
        return;
    }

    FString MapFilename;
    if (FPackageName::TryConvertLongPackageNameToFilename(
            PackageName, MapFilename, FPackageName::GetMapPackageExtension()) &&
        FPaths::FileExists(MapFilename))
    {
        FEditorFileUtils::LoadMap(MapFilename);
    }
    else
    {
        MapLinkNotify::Fail(FText::Format(
            LOCTEXT("ExpOpenLevelFailed",
                "Level '{0}' could not be found on disk."),
            FText::FromString(Item->LevelName)));
    }
}

void SMapLinkManagerWindow::OnRenameExpansion(
    TSharedPtr<FMapLinkExpansionRow> Item)
{
    if (!Item.IsValid()) return;

    UMapLinkEditorState* State = UMapLinkEditorState::Get();
    FMapLinkWPExpansionRecord* Rec = State->FindExpansionById(Item->Id);
    if (!Rec) return;

    // The Manager has no inline-edit affordance in the list yet; the create
    // form's Label field is the editor.  Load the record into it and take the
    // user there — one field, one place it is edited.
    SelectedWPLevelPath = Rec->TargetLevel.ToString();
    ExpandDepth         = Rec->ExpandDepth;
    NewExpansionLabel   = Rec->GetLabel();
    for (const TSharedPtr<FString>& Option : LIRootOptions)
    {
        if (*Option == Rec->RootBlendName)
        {
            SelectedRoot = Option;
            if (RootCombo.IsValid())
            {
                RootCombo->SetSelectedItem(SelectedRoot);
            }
            break;
        }
    }
    RefreshExpansionPreview();

    MapLinkNotify::Success(LOCTEXT("ExpRenameLoaded",
        "Loaded into the form above — edit the Label and press Expand to "
        "rebuild under the new folder name."));
}

void SMapLinkManagerWindow::OnSetExpansionDepth(
    TSharedPtr<FMapLinkExpansionRow> Item, int32 NewDepth)
{
    if (!Item.IsValid()) return;

    NewDepth = FMath::Clamp(NewDepth, 1, 10);
    if (NewDepth == Item->ExpandDepth) return;

    UMapLinkEditorState* State = UMapLinkEditorState::Get();
    if (FMapLinkWPExpansionRecord* Rec = State->FindExpansionById(Item->Id))
    {
        const int32 Old = Rec->ExpandDepth;
        Rec->ExpandDepth = NewDepth;
        State->Save();

        // Stored, not applied. Re-expanding rewrites the level and can take
        // minutes; doing that because somebody nudged a spinner would be the
        // tool deciding on their behalf. The row's state already reads stale.
        UE_LOG(LogMapLink, Log,
               TEXT("WP Expand: '%s' depth %d -> %d. Press Re-expand to apply."),
               *Item->Label, Old, NewDepth);
    }
    RefreshExpansionRows();
}

void SMapLinkManagerWindow::OnSetExpansionPaused(
    TSharedPtr<FMapLinkExpansionRow> Item, bool bPaused)
{
    if (!Item.IsValid()) return;

    UMapLinkEditorState* State = UMapLinkEditorState::Get();
    if (FMapLinkWPExpansionRecord* Rec = State->FindExpansionById(Item->Id))
    {
        Rec->bReExpandOnImport = !bPaused;
        State->Save();
    }
    RefreshExpansionRows();
}

FReply SMapLinkManagerWindow::OnReExpandSelected()
{
    TArray<FGuid> Ids;
    for (const TSharedPtr<FMapLinkExpansionRow>& Row : GetSelectedExpansions())
    {
        if (Row.IsValid())
        {
            Ids.Add(Row->Id);
        }
    }
    return ReExpandByIds(Ids);
}

FReply SMapLinkManagerWindow::OnReExpandStale()
{
    TArray<FGuid> Ids;
    for (const TSharedPtr<FMapLinkExpansionRow>& Row : ExpansionRows)
    {
        if (Row.IsValid() && Row->State == FMapLinkExpansionRow::EState::Stale)
        {
            Ids.Add(Row->Id);
        }
    }
    return ReExpandByIds(Ids);
}

FReply SMapLinkManagerWindow::ReExpandByIds(const TArray<FGuid>& Ids)
{
    if (Ids.Num() == 0)
    {
        return FReply::Handled();
    }

    UMapLinkEditorState* State = UMapLinkEditorState::Get();

    // Group by level so a level is opened, expanded into and saved once —
    // the same rule ReExpandIfNeeded now follows on the reimport path.
    TMap<FSoftObjectPath, TArray<FMapLinkWPExpansionRecord>> ByLevel;
    for (const FGuid& Id : Ids)
    {
        if (const FMapLinkWPExpansionRecord* Rec = State->FindExpansionById(Id))
        {
            ByLevel.FindOrAdd(Rec->TargetLevel).Add(*Rec);
        }
    }

    int32 Succeeded = 0, Actors = 0;
    for (TPair<FSoftObjectPath, TArray<FMapLinkWPExpansionRecord>>& Pair : ByLevel)
    {
        const int32 Count = Pair.Value.Num();
        for (int32 Index = 0; Index < Count; ++Index)
        {
            FMapLinkWPExpansionRecord& Rec = Pair.Value[Index];
            const bool bLast = (Index == Count - 1);
            if (FMapLinkWPExpansionHelper::ExpandRecord(Rec, bLast) ==
                EMapLinkResult::Success)
            {
                ++Succeeded;
                Actors += Rec.LastPlacedCount;
            }
        }
    }

    MapLinkNotify::Success(FText::FromString(FString::Printf(
        TEXT("Re-expanded %d of %d expansion(s) across %d level(s) — "
             "%d actor(s)."),
        Succeeded, Ids.Num(), ByLevel.Num(), Actors)));

    RefreshRows(false);
    RefreshExpansionRows();
    return FReply::Handled();
}

FReply SMapLinkManagerWindow::OnCleanupSelectedExpansions()
{
    const TArray<TSharedPtr<FMapLinkExpansionRow>> Selected = GetSelectedExpansions();
    if (Selected.Num() == 0)
    {
        return FReply::Handled();
    }

    // v1.18 — discovered rows have no record to drop; they are removed
    // through Remove from Level…, which says out loud that they are not yours.
    TArray<TSharedPtr<FMapLinkExpansionRow>> Tracked;
    int32 SkippedDiscovered = 0;
    for (const TSharedPtr<FMapLinkExpansionRow>& Row : Selected)
    {
        if (!Row.IsValid()) continue;
        if (Row->bDiscovered) { ++SkippedDiscovered; continue; }
        Tracked.Add(Row);
    }
    if (Tracked.Num() == 0)
    {
        MapLinkNotify::Warn(LOCTEXT("CleanupOnlyDiscovered",
            "Those expansions are not in your records — use "
            "\"Remove from Level…\" on the row instead."));
        return FReply::Handled();
    }
    if (SkippedDiscovered > 0)
    {
        MapLinkNotify::Warn(FText::Format(
            LOCTEXT("CleanupSkippedDiscovered",
                "Skipping {0} untracked expansion(s) — use \"Remove from "
                "Level…\" on those rows."),
            FText::AsNumber(SkippedDiscovered)));
    }

    TArray<FString> Names;
    for (const TSharedPtr<FMapLinkExpansionRow>& Row : Tracked)
    {
        Names.Add(FString::Printf(TEXT("  %s → %s (%d actors)"),
            *Row->Label, *Row->LevelName, Row->PlacedCount));
    }

    const FText Message = FText::FromString(FString::Printf(
        TEXT("Remove %d expansion(s)?\n\n%s\n\n"
             "Their actors and outliner folders are deleted and the levels "
             "are saved."),
        Tracked.Num(), *FString::Join(Names, TEXT("\n"))));

    if (FMessageDialog::Open(EAppMsgType::YesNo, Message,
            LOCTEXT("ExpCleanupTitle", "MapLink — Cleanup Expansions"))
        != EAppReturnType::Yes)
    {
        return FReply::Handled();
    }

    int32 Removed = 0;
    for (const TSharedPtr<FMapLinkExpansionRow>& Row : Tracked)
    {
        if (FMapLinkWPExpansionHelper::CleanupExpansion(Row->Id) ==
                EMapLinkResult::Success)
        {
            ++Removed;
        }
    }

    if (Removed < Tracked.Num())
    {
        MapLinkNotify::Fail(FText::Format(
            LOCTEXT("ExpCleanupPartial",
                "Removed {0} of {1} expansion(s) — see the Output Log."),
            FText::AsNumber(Removed), FText::AsNumber(Tracked.Num())));
    }

    RefreshRows(false);
    RefreshExpansionRows();
    return FReply::Handled();
}

FReply SMapLinkManagerWindow::OnCleanupAllInLevel()
{
    if (SelectedWPLevelPath.IsEmpty())
    {
        MapLinkNotify::Warn(LOCTEXT("CleanAllNoLevel",
            "Pick the level to clean in the WP Level box above."));
        return FReply::Handled();
    }

    const FSoftObjectPath LevelPath(SelectedWPLevelPath);
    const FString LevelName = FPaths::GetBaseFilename(LevelPath.ToString());

    int32 InLevel = 0;
    for (const TSharedPtr<FMapLinkExpansionRow>& Row : ExpansionRows)
    {
        if (Row.IsValid() && Row->TargetLevel == LevelPath)
        {
            ++InLevel;
        }
    }

    const FText Message = FText::FromString(FString::Printf(
        TEXT("Remove EVERY MapLink expansion from '%s'?\n\n"
             "This deletes every actor tagged MapLinkWPExpand in that level — "
             "including expansions created by a teammate, which do not appear "
             "in your list — and the whole MapLink/ outliner folder tree.\n\n"
             "%d of your %d record(s) target this level."),
        *LevelName, InLevel, ExpansionRows.Num()));

    if (FMessageDialog::Open(EAppMsgType::YesNo, Message,
            LOCTEXT("CleanAllTitle", "MapLink — Cleanup All in Level"))
        != EAppReturnType::Yes)
    {
        return FReply::Handled();
    }

    const EMapLinkResult Result =
        FMapLinkWPExpansionHelper::CleanupAllExpansionsInLevel(LevelPath);

    if (Result != EMapLinkResult::Success)
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
            NSLOCTEXT("MapLink", "CleanAllFailedFmt",
                "Cleanup All failed for '{0}': {1}\n"
                "Check Output Log for details."),
            FText::FromString(LevelName), MapLinkResultToText(Result)));
    }

    RefreshRows(false);
    RefreshExpansionRows();
    return FReply::Handled();
}

// ── Cross-tab entry points (the Assets list's Expansion chips) ─────────────

void SMapLinkManagerWindow::BeginExpandForRoot(const FString& RootBlendName)
{
    RebuildLIRootOptions();
    for (const TSharedPtr<FString>& Option : LIRootOptions)
    {
        if (*Option == RootBlendName)
        {
            SelectedRoot = Option;
            if (RootCombo.IsValid())
            {
                RootCombo->SetSelectedItem(SelectedRoot);
            }
            break;
        }
    }
    NewExpansionLabel.Reset();
    RefreshExpansionPreview();
    RefreshExpansionRows();
    ActiveTabIndex = static_cast<int32>(ETab::Expansions);
}

void SMapLinkManagerWindow::FocusExpansion(const FGuid& Id)
{
    RefreshExpansionRows();
    ActiveTabIndex = static_cast<int32>(ETab::Expansions);

    if (!ExpansionListView.IsValid())
    {
        return;
    }
    for (const TSharedPtr<FMapLinkExpansionRow>& Row : ExpansionRows)
    {
        if (Row.IsValid() && Row->Id == Id)
        {
            ExpansionListView->SetSelection(Row);
            ExpansionListView->RequestScrollIntoView(Row);
            break;
        }
    }
}

#undef LOCTEXT_NAMESPACE
