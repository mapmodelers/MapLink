// Copyright MapModelers. All Rights Reserved.

#include "SMapLinkMaterialsTab.h"
#include "MapLinkNotify.h"
#include "SMapLinkMaterialPreviewViewport.h"

#include "MapLinkMaterialExporter.h"   // SyncAfterOp
#include "MapLinkMaterialLibraryExchange.h"  // v1.12 — Export/Import library
#include "MapLinkTextureScanner.h"     // v1.16 — texture health
#include "MapLinkTextureTypes.h"       // v1.16 — status enum
#include "SMapLinkTexturesTab.h"       // v1.16 — drill-through to the Textures tab
#include "MapLinkUtils.h"              // GetMaterialInstancesContentPath
#include "MapLinkPipeline.h"           // LogMapLink

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "EditorAssetLibrary.h"
#include "PackageTools.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"

#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstance.h"     // UMaterialInstance (parent type check)
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"             // UMaterial (base material name)
#include "MaterialTypes.h"                  // FMaterialParameterInfo
#include "Engine/Texture.h"

#include "Editor.h"                          // GEditor
#include "Subsystems/AssetEditorSubsystem.h" // OpenEditorForAsset (native MI editor)

#include "Modules/ModuleManager.h"
#include "PropertyCustomizationHelpers.h"   // SObjectPropertyEntryBox
#include "PropertyEditorModule.h"           // full-property IDetailsView
#include "IDetailsView.h"
#include "AssetThumbnail.h"                 // FAssetThumbnailPool (texture thumbs)
#include "AssetRegistry/AssetData.h"

#include "Framework/Application/SlateApplication.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Input/SComboButton.h"          // v1.17 — "More" overflow
#include "Framework/MultiBox/MultiBoxBuilder.h"  // v1.17 — FMenuBuilder
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "SMapLinkMaterialsTab"

// ───────────────────────────────────────────────────────────────────────
// Modal helpers
// ───────────────────────────────────────────────────────────────────────

namespace
{
    const TCHAR* kNoParentGroup = TEXT("(no parent)");

    /** Simple single-line text prompt.  Returns true on OK with a non-empty
     *  value. */
    bool PromptText(const FText& Title, const FText& Label,
                    const FString& DefaultValue, FString& OutValue)
    {
        bool bAccepted = false;
        FString Value = DefaultValue;

        TSharedRef<SWindow> Window = SNew(SWindow)
            .Title(Title)
            .ClientSize(FVector2D(420.f, 130.f))
            .SupportsMaximize(false)
            .SupportsMinimize(false);

        TSharedRef<SEditableTextBox> Entry = SNew(SEditableTextBox)
            .Text(FText::FromString(DefaultValue))
            .OnTextChanged_Lambda([&Value](const FText& T){ Value = T.ToString(); });

        Window->SetContent(
            SNew(SBorder)
            .BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
            .Padding(10.f)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f)
                [ SNew(STextBlock).Text(Label) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 10.f)
                [ Entry ]
                + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 6.f, 0.f)
                    [
                        SNew(SButton)
                        .Text(LOCTEXT("OK", "OK"))
                        .OnClicked_Lambda([&bAccepted, Window]()
                        {
                            bAccepted = true;
                            Window->RequestDestroyWindow();
                            return FReply::Handled();
                        })
                    ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [
                        SNew(SButton)
                        .Text(LOCTEXT("Cancel", "Cancel"))
                        .OnClicked_Lambda([Window]()
                        {
                            Window->RequestDestroyWindow();
                            return FReply::Handled();
                        })
                    ]
                ]
            ]);

        FSlateApplication::Get().AddModalWindow(Window, nullptr);

        Value.TrimStartAndEndInline();
        if (bAccepted && !Value.IsEmpty())
        {
            OutValue = Value;
            return true;
        }
        return false;
    }

    /** Prompt for a parent material + a name (used by New MI). */
    bool PromptNewMI(UMaterialInterface* DefaultParent,
                     UMaterialInterface*& OutParent, FString& OutName)
    {
        bool bAccepted = false;
        FString Name = TEXT("MI_New");
        FString ParentPath = DefaultParent ? DefaultParent->GetPathName() : FString();

        TSharedRef<SWindow> Window = SNew(SWindow)
            .Title(LOCTEXT("NewMITitle", "New Material Instance"))
            .ClientSize(FVector2D(480.f, 180.f))
            .SupportsMaximize(false)
            .SupportsMinimize(false);

        Window->SetContent(
            SNew(SBorder)
            .BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
            .Padding(10.f)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f)
                [ SNew(STextBlock).Text(LOCTEXT("ParentLabel", "Parent material:")) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 10.f)
                [
                    SNew(SObjectPropertyEntryBox)
                    .AllowedClass(UMaterialInterface::StaticClass())
                    .DisplayThumbnail(false)
                    .OnShouldFilterAsset_Lambda([](const FAssetData& A)
                    {
                        // Restrict to MapLink-managed parents only.
                        return !FMapLinkUtils::IsMapLinkMaterialPath(
                            A.PackageName.ToString());
                    })
                    .ObjectPath_Lambda([&ParentPath](){ return ParentPath; })
                    .OnObjectChanged_Lambda([&ParentPath](const FAssetData& A)
                    {
                        ParentPath = A.GetObjectPathString();
                    })
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f)
                [ SNew(STextBlock).Text(LOCTEXT("NameLabel", "Name:")) ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 10.f)
                [
                    SNew(SEditableTextBox)
                    .Text(FText::FromString(Name))
                    .OnTextChanged_Lambda([&Name](const FText& T){ Name = T.ToString(); })
                ]
                + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 6.f, 0.f)
                    [
                        SNew(SButton)
                        .Text(LOCTEXT("Create", "Create"))
                        .OnClicked_Lambda([&bAccepted, Window]()
                        {
                            bAccepted = true;
                            Window->RequestDestroyWindow();
                            return FReply::Handled();
                        })
                    ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [
                        SNew(SButton)
                        .Text(LOCTEXT("Cancel", "Cancel"))
                        .OnClicked_Lambda([Window]()
                        {
                            Window->RequestDestroyWindow();
                            return FReply::Handled();
                        })
                    ]
                ]
            ]);

        FSlateApplication::Get().AddModalWindow(Window, nullptr);

        Name.TrimStartAndEndInline();
        if (!bAccepted || Name.IsEmpty() || ParentPath.IsEmpty())
        {
            return false;
        }

        OutParent = LoadObject<UMaterialInterface>(nullptr, *ParentPath);
        OutName   = Name;
        return OutParent != nullptr;
    }
}

// ───────────────────────────────────────────────────────────────────────
// Construct
// ───────────────────────────────────────────────────────────────────────

// ───────────────────────────────────────────────────────────────────────
// v1.17 — action overflow
// ───────────────────────────────────────────────────────────────────────

TSharedRef<SWidget> SMapLinkMaterialsTab::BuildMaterialOverflowMenu()
{
    FMenuBuilder Menu(/*bShouldCloseWindowAfterMenuSelection=*/true, nullptr);

    Menu.BeginSection("MapLinkMatSelection",
        LOCTEXT("MatMenuSelection", "Selected Material Instance"));
    {
        Menu.AddMenuEntry(
            LOCTEXT("Duplicate", "Duplicate"),
            LOCTEXT("DuplicateTip", "Duplicate the selected Material Instance."),
            FSlateIcon(),
            FUIAction(
                FExecuteAction::CreateLambda([this]()
                {
                    OnDuplicateClicked();
                }),
                FCanExecuteAction::CreateLambda([this]()
                {
                    return HasSelectedMI();
                })));

        Menu.AddMenuEntry(
            LOCTEXT("Rename", "Rename"),
            LOCTEXT("RenameTip", "Rename the selected Material Instance."),
            FSlateIcon(),
            FUIAction(
                FExecuteAction::CreateLambda([this]()
                {
                    OnRenameClicked();
                }),
                FCanExecuteAction::CreateLambda([this]()
                {
                    return HasSelectedMI();
                })));
    }
    Menu.EndSection();

    Menu.BeginSection("MapLinkMatLibrary",
        LOCTEXT("MatMenuLibrary", "Library"));
    {
        Menu.AddMenuEntry(
            LOCTEXT("ExportLib", "Export…"),
            LOCTEXT("ExportLibTip", "Export Material Instances (all "
                "or a selection, with parents and textures) as a "
                "reusable MapLink material library bundle."),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([this]()
            {
                OnExportLibraryClicked();
            })));

        Menu.AddMenuEntry(
            LOCTEXT("ImportLib", "Import…"),
            LOCTEXT("ImportLibTip", "Import a MapLink material "
                "library bundle exported from another project "
                "(merges into /Game/MapLink/MaterialInstances/)."),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([this]()
            {
                OnImportLibraryClicked();
            })));

        Menu.AddMenuEntry(
            LOCTEXT("RebuildJson", "Rebuild…"),
            LOCTEXT("RebuildJsonTip", "Rebuild Material Instances "
                "from a material_instances.json.\n\n"
                "Not the same as Import: this synthesises assets "
                "from recorded parameter values rather than "
                "restoring .uasset files, so it is only as good as "
                "what the file captured. Parents are matched by "
                "name and textures by filename against this "
                "project; nothing is imported from disk."),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([this]()
            {
                OnRebuildFromJsonClicked();
            })));

        // v1.20 — the one-time thumbnail migration.
        //
        // In the overflow menu rather than the button block for the reason
        // the block was thinned out in v1.17: this is a library-level errand
        // you run rarely, not one of the things you do every day. It is also
        // the only action in MapLink that checks out every material instance
        // in the project, so it should take a deliberate two clicks.
        Menu.AddMenuEntry(
            LOCTEXT("StampThumbs", "Restamp Asset Thumbnails…"),
            LOCTEXT("StampThumbsTip",
                "Re-render every MapLink material instance on the MapLink "
                "calibration cube and cache the picture into its .uasset, so "
                "the Content Browser shows the same thumbnail as the Blender "
                "Asset Browser.\n\n"
                "This checks out and saves every material instance in the "
                "project onto one changelist. Run it once after the look "
                "changes, not as part of a normal sync."),
            FSlateIcon(),
            FUIAction(FExecuteAction::CreateLambda([this]()
            {
                OnStampAssetThumbnailsClicked();
            })));
    }
    Menu.EndSection();

    return Menu.MakeWidget();
}

void SMapLinkMaterialsTab::Construct(const FArguments& InArgs)
{
    auto MakeOpButton =
        [this](const FText& Label, const FText& Tip,
               FReply (SMapLinkMaterialsTab::*Handler)(),
               TAttribute<bool> Enabled) -> TSharedRef<SWidget>
    {
        return SNew(SButton)
            .Text(Label)
            .ToolTipText(Tip)
            .IsEnabled(Enabled)
            .OnClicked(this, Handler);
    };

    // Thumbnail pool for texture-parameter pickers (texture thumbnails).
    ThumbnailPool = MakeShared<FAssetThumbnailPool>(24);

    // Sort options for the MI list.
    SortOptions.Add(MakeShared<FString>(TEXT("Name (A-Z)")));
    SortOptions.Add(MakeShared<FString>(TEXT("Name (Z-A)")));
    CurrentSortOption = SortOptions[0];

    // Grouping modes (index → GroupMode).
    GroupModeOptions.Add(MakeShared<FString>(TEXT("Group: Base Material")));
    GroupModeOptions.Add(MakeShared<FString>(TEXT("Group: Parent")));
    GroupModeOptions.Add(MakeShared<FString>(TEXT("List (flat)")));
    CurrentGroupMode = GroupModeOptions[0];

    // "Advanced Parameters" details view — the other MI properties (base
    // property overrides, phys material, etc.), like the Details tab. The
    // exposed parameter value arrays are hidden here since the flat list above
    // already covers them.
    {
        FPropertyEditorModule& PropertyModule =
            FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
        FDetailsViewArgs DetailsArgs;
        DetailsArgs.bUpdatesFromSelection = false;
        DetailsArgs.bLockable             = false;
        DetailsArgs.bAllowSearch          = true;
        DetailsArgs.bHideSelectionTip     = true;
        DetailsArgs.NameAreaSettings      = FDetailsViewArgs::HideNameArea;
        DetailsView = PropertyModule.CreateDetailView(DetailsArgs);
        DetailsView->SetIsPropertyVisibleDelegate(FIsPropertyVisible::CreateLambda(
            [](const FPropertyAndParent& PP) -> bool
            {
                static const TSet<FName> Hidden = {
                    FName(TEXT("ScalarParameterValues")),
                    FName(TEXT("VectorParameterValues")),
                    FName(TEXT("TextureParameterValues")),
                    FName(TEXT("FontParameterValues")),
                    FName(TEXT("RuntimeVirtualTextureParameterValues")) };
                if (Hidden.Contains(PP.Property.GetFName())) { return false; }
                for (const FProperty* Parent : PP.ParentProperties)
                {
                    if (Parent && Hidden.Contains(Parent->GetFName())) { return false; }
                }
                return true;
            }));
        DetailsView->OnFinishedChangingProperties().AddSPLambda(
            this, [this](const FPropertyChangedEvent&) { MarkEditedAndPreview(); });
    }

    ChildSlot
    [
        SNew(SSplitter)
        .Orientation(Orient_Horizontal)

        // ── Left: ops toolbar + parent-grouped MI tree ─────────────────
        + SSplitter::Slot().Value(0.32f)
        [
            SNew(SVerticalBox)

            // v1.13.1 — Material library sync status + manual Resync, moved
            // here from the Manager Settings tab. Sync is automatic after
            // every op; this button is recovery-only (library-dirty flag).
            + SVerticalBox::Slot().AutoHeight().Padding(4.f, 4.f, 4.f, 2.f)
            [
                SNew(SBorder)
                .BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
                .Padding(6.f, 4.f)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
                    [
                        SNew(STextBlock)
                        .AutoWrapText(true)
                        .Text(this, &SMapLinkMaterialsTab::GetMaterialSyncStatusText)
                        .ColorAndOpacity(this, &SMapLinkMaterialsTab::GetMaterialSyncStatusColor)
                    ]
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                      .Padding(6.f, 0.f, 0.f, 0.f)
                    [
                        SNew(SButton)
                        .Text(LOCTEXT("ResyncMaterialLibrary", "Resync"))
                        .ToolTipText(LOCTEXT("ResyncMaterialLibraryTip",
                            "Rebuild the Blender material libraries from the current "
                            "UE Material Instance state (atomic, source-control staged). "
                            "Use this if the libraries were left out of sync by a failed "
                            "auto-sync."))
                        .OnClicked(this, &SMapLinkMaterialsTab::OnResyncMaterialLibraryClicked)
                    ]
                ]
            ]

            // ── v1.16 — texture health ────────────────────────────────
            //
            // The amendment asked for a way to see which materials reference
            // non-conform textures. This is it: one line of prose that says
            // how bad it is, and one button that turns the tree into the
            // answer. Off by default — a user not chasing a portability
            // problem should not pay for the scan or look at the badges.
            + SVerticalBox::Slot().AutoHeight().Padding(4.f, 2.f)
            [
                SNew(SBorder)
                .BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
                .Padding(6.f, 4.f)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
                    [
                        SNew(STextBlock)
                        .AutoWrapText(true)
                        .Text(this, &SMapLinkMaterialsTab::GetTextureHealthText)
                        .ColorAndOpacity(this, &SMapLinkMaterialsTab::GetTextureHealthColour)
                    ]
                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                      .Padding(6.f, 0.f, 0.f, 0.f)
                    [
                        // v1.17 — a toggle, drawn as a toggle.  As a plain
                        // button next to a label reading "Texture health: off"
                        // it was ambiguous which of the two was the state and
                        // which was the control.
                        SNew(SCheckBox)
                        .Style(FAppStyle::Get(), "ToggleButtonCheckbox")
                        .ToolTipText(LOCTEXT("MatTexHealthTip",
                            "Show each material's textures and their status. "
                            "Double-click to open in the Textures tab."))
                        .IsChecked_Lambda([this]()
                        {
                            return bShowTextureHealth
                                ? ECheckBoxState::Checked
                                : ECheckBoxState::Unchecked;
                        })
                        .OnCheckStateChanged_Lambda([this](ECheckBoxState)
                        {
                            OnShowNonConformClicked();
                        })
                        [
                            SNew(SBox).Padding(FMargin(8.f, 3.f))
                            [
                                SNew(STextBlock)
                                .Text(LOCTEXT("MatTexShowHealth", "Texture Health"))
                            ]
                        ]
                    ]
                ]
            ]

            + SVerticalBox::Slot().AutoHeight().Padding(4.f)
            [
                // v1.16 — SWrapBox, not SHorizontalBox.
                //
                // Eight AutoWidth buttons in one horizontal row need about
                // 640px. This column is 0.32 of the window, and AutoWidth
                // slots do not clip — they overflow and paint straight over
                // the parameter list in the middle column, which is what the
                // "broken UI" screenshot shows. A wrap box reflows onto a
                // second line instead, at any window width.
                SNew(SWrapBox)
                .UseAllottedSize(true)
                .InnerSlotPadding(FVector2D(4.f, 4.f))

                + SWrapBox::Slot()
                [ MakeOpButton(LOCTEXT("New", "New"),
                    LOCTEXT("NewTip", "Create a new Material Instance from a parent material."),
                    &SMapLinkMaterialsTab::OnNewClicked, true) ]
                + SWrapBox::Slot()
                [ MakeOpButton(LOCTEXT("CreateChild", "Create Child"),
                    LOCTEXT("CreateChildTip", "Create a new Material Instance parented to "
                        "the selected one (inherits its parameters)."),
                    &SMapLinkMaterialsTab::OnCreateChildClicked,
                    TAttribute<bool>(this, &SMapLinkMaterialsTab::HasSelectedMI)) ]
                + SWrapBox::Slot()
                [ MakeOpButton(LOCTEXT("Refresh", "Refresh"),
                    LOCTEXT("RefreshTip", "Rescan Material Instances."),
                    &SMapLinkMaterialsTab::OnRefreshClicked, true) ]

                // v1.17 — eight equal-weight buttons in a 2x4 block read as
                // eight equally important things to do.  Two of them are:
                // New and Create Child.  Duplicate and Rename act on the
                // selected row (so they also live on its context menu);
                // Export/Import/Rebuild are library-level errands you run
                // rarely.  All five move here.
                + SWrapBox::Slot()
                [
                    SNew(SComboButton)
                    .ToolTipText(LOCTEXT("MatMoreTip",
                        "Duplicate, Rename, and library-level actions."))
                    .ButtonContent()
                    [
                        SNew(STextBlock).Text(LOCTEXT("MatMore", "More"))
                    ]
                    .OnGetMenuContent_Lambda([this]() -> TSharedRef<SWidget>
                    {
                        return BuildMaterialOverflowMenu();
                    })
                ]
            ]
            // Search + sort row
            + SVerticalBox::Slot().AutoHeight().Padding(4.f, 0.f, 4.f, 4.f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().FillWidth(1.f).Padding(0.f, 0.f, 4.f, 0.f)
                [
                    SAssignNew(SearchBox, SSearchBox)
                    .HintText(LOCTEXT("SearchHint", "Search Material Instances…"))
                    .OnTextChanged(this, &SMapLinkMaterialsTab::OnSearchChanged)
                ]
                + SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 4.f, 0.f)
                [
                    SNew(SComboBox<TSharedPtr<FString>>)
                    .OptionsSource(&SortOptions)
                    .InitiallySelectedItem(CurrentSortOption)
                    .OnGenerateWidget_Lambda([](TSharedPtr<FString> In)
                    {
                        return SNew(STextBlock).Text(FText::FromString(*In));
                    })
                    .OnSelectionChanged(this, &SMapLinkMaterialsTab::OnSortChanged)
                    [
                        SNew(STextBlock)
                        .Text_Lambda([this]()
                        {
                            return FText::FromString(
                                CurrentSortOption.IsValid()
                                    ? *CurrentSortOption : FString(TEXT("Sort")));
                        })
                    ]
                ]
                + SHorizontalBox::Slot().AutoWidth()
                [
                    SNew(SComboBox<TSharedPtr<FString>>)
                    .OptionsSource(&GroupModeOptions)
                    .InitiallySelectedItem(CurrentGroupMode)
                    .OnGenerateWidget_Lambda([](TSharedPtr<FString> In)
                    {
                        return SNew(STextBlock).Text(FText::FromString(*In));
                    })
                    .OnSelectionChanged(this, &SMapLinkMaterialsTab::OnGroupModeChanged)
                    .ToolTipText(LOCTEXT("GroupTip",
                        "Group by base material (default), by immediate parent "
                        "(child MIs get their own category), or list flat."))
                    [
                        SNew(STextBlock)
                        .Text_Lambda([this]()
                        {
                            return FText::FromString(
                                CurrentGroupMode.IsValid()
                                    ? *CurrentGroupMode : FString(TEXT("Group")));
                        })
                    ]
                ]
            ]
            + SVerticalBox::Slot().FillHeight(1.f).Padding(4.f, 0.f, 4.f, 4.f)
            [
                SNew(SBorder)
                .BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
                [
                    SAssignNew(TreeView, STreeView<TSharedPtr<FMapLinkMatNode>>)
                    .TreeItemsSource(&RootNodes)
                    .OnGenerateRow(this, &SMapLinkMaterialsTab::OnGenerateRow)
                    .OnGetChildren(this, &SMapLinkMaterialsTab::OnGetChildren)
                    .OnSelectionChanged(this, &SMapLinkMaterialsTab::OnSelectionChanged)
                    .SelectionMode(ESelectionMode::Single)
                    // v1.16 — double-clicking a texture row takes you to the
                    // one place that can fix it. Finding the problem and
                    // fixing it should not be two separate hunts.
                    .OnMouseButtonDoubleClick_Lambda(
                        [](TSharedPtr<FMapLinkMatNode> Node)
                        {
                            if (Node.IsValid() && Node->bIsTextureLeaf)
                            {
                                SMapLinkTexturesTab::RequestFocusOnTexture(
                                    Node->TextureAssetPath);
                            }
                        })
                ]
            ]
        ]

        // ── Middle: param details + Save / Revert ──────────────────────
        + SSplitter::Slot().Value(0.38f)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().Padding(4.f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 4.f, 0.f)
                [ MakeOpButton(LOCTEXT("Save", "Save"),
                    LOCTEXT("SaveTip", "Save edits and resync the Blender libraries."),
                    &SMapLinkMaterialsTab::OnSaveClicked,
                    TAttribute<bool>(this, &SMapLinkMaterialsTab::IsSaveEnabled)) ]
                + SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 4.f, 0.f)
                [ MakeOpButton(LOCTEXT("Revert", "Revert"),
                    LOCTEXT("RevertTip", "Discard unsaved edits (reload from disk)."),
                    &SMapLinkMaterialsTab::OnRevertClicked,
                    TAttribute<bool>(this, &SMapLinkMaterialsTab::IsSaveEnabled)) ]
                + SHorizontalBox::Slot().FillWidth(1.f).HAlign(HAlign_Right)
                [ MakeOpButton(LOCTEXT("OpenInEditor", "Open in Material Editor"),
                    LOCTEXT("OpenInEditorTip", "Open the selected Material Instance in "
                        "UE's native Material Instance editor (full parameter UI with "
                        "override checkboxes)."),
                    &SMapLinkMaterialsTab::OnOpenInEditorClicked,
                    TAttribute<bool>(this, &SMapLinkMaterialsTab::HasSelectedMI)) ]
            ]
            + SVerticalBox::Slot().FillHeight(1.f).Padding(4.f, 0.f, 4.f, 4.f)
            [
                SNew(SSplitter)
                .Orientation(Orient_Vertical)

                // Flat exposed-parameter list (override checkbox + value).
                + SSplitter::Slot().Value(0.62f)
                [
                    SNew(SBorder)
                    .BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
                    .Padding(4.f)
                    [
                        SAssignNew(ParamsBox, SScrollBox)
                    ]
                ]

                // "Advanced Parameters" — the other MI properties (base
                // property overrides, phys material, …), like the Details tab.
                + SSplitter::Slot().Value(0.38f)
                [
                    SNew(SBorder)
                    .BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
                    [
                        SNew(SVerticalBox)
                        + SVerticalBox::Slot().AutoHeight().Padding(4.f, 4.f, 4.f, 2.f)
                        [
                            SNew(STextBlock)
                            .Text(LOCTEXT("AdvancedParams", "Advanced Parameters"))
                            .ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
                        ]
                        + SVerticalBox::Slot().FillHeight(1.f)
                        [
                            DetailsView.ToSharedRef()
                        ]
                    ]
                ]
            ]
        ]

        // ── Right: live preview viewport + secondary-slot editor ───────
        + SSplitter::Slot().Value(0.30f)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().FillHeight(1.f).Padding(2.f)
            [
                SNew(SBorder)
                .BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
                .Padding(2.f)
                [
                    SAssignNew(PreviewViewport, SMapLinkMaterialPreviewViewport)
                ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(2.f, 0.f, 2.f, 2.f)
            [
                SNew(SBorder)
                .BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
                .Padding(4.f)
                [
                    SNew(SVerticalBox)
                    // Slot 0 is the selected material (driven by the list); the
                    // panel below only adds an EXTRA material to slot 1 for
                    // visual context — it never changes what is being edited.
                    + SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 1.f)
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("Slot1", "Slot 1 — extra preview material:"))
                        .ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
                    ]
                    + SVerticalBox::Slot().AutoHeight()
                    [
                        SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().FillWidth(1.f)
                        [
                            SNew(SObjectPropertyEntryBox)
                            .AllowedClass(UMaterialInterface::StaticClass())
                            .ThumbnailPool(ThumbnailPool)
                            .DisplayThumbnail(true)
                            .ObjectPath_Lambda([this]()
                            {
                                UMaterialInterface* M = PreviewViewport.IsValid()
                                    ? PreviewViewport->GetSlotMaterial(1) : nullptr;
                                return M ? M->GetPathName() : FString();
                            })
                            .OnObjectChanged(this, &SMapLinkMaterialsTab::OnSlot1Changed)
                        ]
                        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                          .Padding(4.f, 0.f, 0.f, 0.f)
                        [
                            SNew(SButton)
                            .Text(LOCTEXT("RevertSlot1", "Default"))
                            .ToolTipText(LOCTEXT("RevertSlot1Tip",
                                "Revert slot 1 to the cube's default material."))
                            .OnClicked(this, &SMapLinkMaterialsTab::OnRevertSlot1Clicked)
                        ]
                    ]
                ]
            ]
        ]
    ];

    RebuildTree();
    // v1.16 — publish this instance and take any focus request parked while no
    // tab existed. Done last so the widget tree is fully built first.
    LiveTab = SharedThis(this);
    if (!PendingFocusStem.IsEmpty())
    {
        ApplyFocusStem(PendingFocusStem);
        PendingFocusStem.Empty();
    }
    if (PendingSelectNames.Num() > 0)
    {
        ApplySelectMaterials(PendingSelectNames);
        PendingSelectNames.Empty();
    }

}

// ───────────────────────────────────────────────────────────────────────
// Tree
// ───────────────────────────────────────────────────────────────────────

// ---------------------------------------------------------------------------
// v1.16 — texture health
// ---------------------------------------------------------------------------

namespace
{
    /** Same palette as the Textures tab. Duplicated deliberately rather than
     *  exported: two tabs agreeing on five colours is not worth a shared
     *  header, and the day they should differ this is where you change it. */
    FLinearColor MatTextureStatusColour(EMapLinkTextureStatus Status)
    {
        switch (Status)
        {
        case EMapLinkTextureStatus::Conform:   return FLinearColor(0.35f, 0.78f, 0.42f);
        case EMapLinkTextureStatus::Plugin:    return FLinearColor(0.62f, 0.58f, 0.85f);
        case EMapLinkTextureStatus::Portable:  return FLinearColor(0.45f, 0.70f, 0.90f);
        case EMapLinkTextureStatus::LocalOnly: return FLinearColor(0.94f, 0.55f, 0.13f);
        case EMapLinkTextureStatus::Broken:    return FLinearColor(0.86f, 0.20f, 0.16f);
        default:                               return FLinearColor(0.60f, 0.60f, 0.60f);
        }
    }

    const TCHAR* MatTextureStatusGlyph(EMapLinkTextureStatus Status)
    {
        switch (Status)
        {
        case EMapLinkTextureStatus::Conform:   return TEXT("\u25CF");
        case EMapLinkTextureStatus::Plugin:    return TEXT("\u25C6");
        case EMapLinkTextureStatus::Portable:  return TEXT("\u25CB");
        case EMapLinkTextureStatus::LocalOnly: return TEXT("\u25B2");
        case EMapLinkTextureStatus::Broken:    return TEXT("\u2715");
        default:                               return TEXT("?");
        }
    }

    /** The same words the Textures tab uses. The enum names stay in the logs
     *  and the manifests, where precision beats familiarity; on screen the
     *  only question is who else can open the file. */
    FText MatStatusWords(EMapLinkTextureStatus Status)
    {
        switch (Status)
        {
        case EMapLinkTextureStatus::Conform:  return LOCTEXT("MatStMapLink",  "MapLink");
        case EMapLinkTextureStatus::Plugin:   return LOCTEXT("MatStPlugin",   "Plugin");
        case EMapLinkTextureStatus::Portable: return LOCTEXT("MatStProject",  "Project");
        case EMapLinkTextureStatus::LocalOnly:return LOCTEXT("MatStExternal", "External");
        case EMapLinkTextureStatus::Broken:   return LOCTEXT("MatStMissing",  "Missing");
        default:                              return LOCTEXT("MatStUnclass",  "Unclassified");
        }
    }

    /** Broken > LocalOnly > Unclassified > Portable > Conform. */
    int32 MatStatusSeverity(EMapLinkTextureStatus Status)
    {
        switch (Status)
        {
        case EMapLinkTextureStatus::Broken:       return 0;
        case EMapLinkTextureStatus::LocalOnly:    return 1;
        case EMapLinkTextureStatus::Unclassified: return 2;
        case EMapLinkTextureStatus::Portable:     return 3;
        case EMapLinkTextureStatus::Conform:      return 4;
        case EMapLinkTextureStatus::Plugin:       return 5;
        default:                                  return 6;
        }
    }
}

void SMapLinkMaterialsTab::AttachTextureChildren(
    const TSharedPtr<FMapLinkMatNode>& Leaf, UMaterialInstanceConstant* MI)
{
    if (!bShowTextureHealth || !Leaf.IsValid() || !MI)
    {
        return;
    }
    if (!FMapLinkTextureScanner::HasResult())
    {
        return;
    }

    const FString MIPath = MI->GetPathName();
    const FMapLinkTextureScanResult& R = FMapLinkTextureScanner::GetLastResult();

    // Walk the scan's reverse index rather than re-enumerating the MI's
    // parameters. The scanner already did that enumeration, matching the
    // exporter exactly; doing it a second time here would be a second opinion
    // that could disagree.
    EMapLinkTextureStatus Worst = EMapLinkTextureStatus::Conform;
    bool bAny = false;

    for (const FMapLinkTextureRow& Row : R.Rows)
    {
        if (!Row.ReferencingMaterials.Contains(MIPath))
        {
            continue;
        }

        TSharedPtr<FMapLinkMatNode> TexNode = MakeShared<FMapLinkMatNode>();
        TexNode->bIsTextureLeaf    = true;
        TexNode->DisplayName       = Row.AssetName;
        TexNode->TextureAssetPath  = Row.AssetPath;
        TexNode->TextureRemedy     = Row.Remedy;
        TexNode->TextureStatus     = Row.Status;
        TexNode->bHasTextureStatus = true;
        Leaf->Children.Add(TexNode);

        bAny = true;
        if (MatStatusSeverity(Row.Status) < MatStatusSeverity(Worst))
        {
            Worst = Row.Status;
        }
    }

    if (bAny)
    {
        Leaf->TextureStatus     = Worst;
        Leaf->bHasTextureStatus = true;

        Leaf->Children.Sort([](const TSharedPtr<FMapLinkMatNode>& A,
                               const TSharedPtr<FMapLinkMatNode>& B)
        {
            const int32 SA = MatStatusSeverity(A->TextureStatus);
            const int32 SB = MatStatusSeverity(B->TextureStatus);
            if (SA != SB) { return SA < SB; }
            return A->DisplayName < B->DisplayName;
        });
    }
}

FText SMapLinkMaterialsTab::GetTextureHealthText() const
{
    if (!bShowTextureHealth)
    {
        return LOCTEXT("MatTexHealthOff", "Texture health: off");
    }
    if (!FMapLinkTextureScanner::HasResult())
    {
        return LOCTEXT("MatTexHealthNoScan", "Texture health: no scan yet");
    }

    const FMapLinkTextureScanResult& R = FMapLinkTextureScanner::GetLastResult();
    const int32 AtRisk = R.NumLocalOnly + R.NumBroken;
    if (AtRisk == 0)
    {
        return FText::Format(LOCTEXT("MatTexHealthClean",
            "Textures: {0}, all conform."), FText::AsNumber(R.Total()));
    }

    return FText::Format(LOCTEXT("MatTexHealthDirty",
        "Textures: {0} of {1} are External and require conforming."),
        FText::AsNumber(AtRisk), FText::AsNumber(R.Total()));
}

FSlateColor SMapLinkMaterialsTab::GetTextureHealthColour() const
{
    if (!bShowTextureHealth || !FMapLinkTextureScanner::HasResult())
    {
        return FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f));
    }
    const FMapLinkTextureScanResult& R = FMapLinkTextureScanner::GetLastResult();
    if (R.NumBroken > 0)
    {
        return FSlateColor(MatTextureStatusColour(EMapLinkTextureStatus::Broken));
    }
    if (R.NumLocalOnly > 0)
    {
        return FSlateColor(MatTextureStatusColour(EMapLinkTextureStatus::LocalOnly));
    }
    return FSlateColor(MatTextureStatusColour(EMapLinkTextureStatus::Conform));
}

FReply SMapLinkMaterialsTab::OnShowNonConformClicked()
{
    bShowTextureHealth = !bShowTextureHealth;

    // Turning it ON is the moment to make sure there is something to show.
    // Scan() reuses a cached result, so this is only expensive the first time.
    if (bShowTextureHealth)
    {
        FMapLinkTextureScanner::Scan(/*bForceFullRecompute=*/false);
    }

    RebuildTree();
    return FReply::Handled();
}

void SMapLinkMaterialsTab::RebuildTree()
{
    RootNodes.Reset();

    FAssetRegistryModule& ARModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AR = ARModule.Get();

    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*FMapLinkUtils::GetMaterialInstancesContentPath()));
    Filter.bRecursivePaths = true;
    Filter.ClassPaths.Add(UMaterialInstanceConstant::StaticClass()->GetClassPathName());

    TArray<FAssetData> MIAssets;
    AR.GetAssets(Filter, MIAssets);

    // Group by parent name.
    TMap<FString, TSharedPtr<FMapLinkMatNode>> Groups;

    const FString NameFilter = FilterText.TrimStartAndEnd();
    const bool bDescending =
        CurrentSortOption.IsValid() && CurrentSortOption->Contains(TEXT("Z-A"));

    for (const FAssetData& Data : MIAssets)
    {
        UMaterialInstanceConstant* MI =
            Cast<UMaterialInstanceConstant>(Data.GetAsset());
        if (!MI) continue;

        const FString MIName = MI->GetName();

        // Name search filter (case-insensitive substring).
        if (!NameFilter.IsEmpty() && !MIName.Contains(NameFilter))
        {
            continue;
        }

        TSharedPtr<FMapLinkMatNode> Leaf = MakeShared<FMapLinkMatNode>();
        Leaf->bIsParentGroup = false;
        Leaf->DisplayName    = MIName;
        Leaf->MI             = MI;

        AttachTextureChildren(Leaf, MI);   // v1.16 — no-op when health is off

        if (GroupMode == 2)
        {
            RootNodes.Add(Leaf);   // flat: all MIs, mixed parents
        }
        else
        {
            // GroupMode 0 → the base Material (top parent of the whole chain),
            //               so child-of-child MIs still roll up under their
            //               root Material and MI parents get NO category.
            // GroupMode 1 → the immediate parent (a parent that is itself an
            //               MI gets its own category, holding its child MIs).
            UMaterialInterface* GroupKey = (GroupMode == 0)
                ? static_cast<UMaterialInterface*>(MI->GetMaterial())
                : MI->Parent.Get();
            const FString GroupName =
                GroupKey ? GroupKey->GetName() : FString(kNoParentGroup);

            TSharedPtr<FMapLinkMatNode>& Group = Groups.FindOrAdd(GroupName);
            if (!Group.IsValid())
            {
                Group = MakeShared<FMapLinkMatNode>();
                Group->bIsParentGroup = true;
                Group->DisplayName    = GroupName;
                Group->GroupParent    = GroupKey;
            }
            Group->Children.Add(Leaf);

            // v1.16 — the worst news underneath, surfaced on the collapsed
            // group. One local-only texture on a shared parent shows up as
            // fifteen bad materials; the group badge is what makes it obvious
            // they are all the same problem.
            if (Leaf->bHasTextureStatus)
            {
                if (!Group->bHasTextureStatus
                    || MatStatusSeverity(Leaf->TextureStatus)
                       < MatStatusSeverity(Group->TextureStatus))
                {
                    Group->TextureStatus     = Leaf->TextureStatus;
                    Group->bHasTextureStatus = true;
                }
            }
        }
    }

    auto Order = [bDescending](const TSharedPtr<FMapLinkMatNode>& A,
                               const TSharedPtr<FMapLinkMatNode>& B)
    {
        return bDescending ? (A->DisplayName > B->DisplayName)
                           : (A->DisplayName < B->DisplayName);
    };

    if (GroupMode == 2)
    {
        RootNodes.Sort(Order);
    }
    else
    {
        Groups.GenerateValueArray(RootNodes);
        RootNodes.Sort(Order);
        for (TSharedPtr<FMapLinkMatNode>& Group : RootNodes)
        {
            Group->Children.Sort(Order);
        }
    }

    if (TreeView.IsValid())
    {
        TreeView->RequestTreeRefresh();
        if (GroupMode != 2)
        {
            for (const TSharedPtr<FMapLinkMatNode>& Group : RootNodes)
            {
                TreeView->SetItemExpansion(Group, true);
            }
        }
    }
}

void SMapLinkMaterialsTab::OnGroupModeChanged(
    TSharedPtr<FString> NewMode, ESelectInfo::Type /*SelectInfo*/)
{
    if (!NewMode.IsValid())
    {
        return;
    }
    CurrentGroupMode = NewMode;
    const int32 Idx = GroupModeOptions.IndexOfByKey(NewMode);
    GroupMode = (Idx != INDEX_NONE) ? Idx : 0;
    RebuildTree();
}

void SMapLinkMaterialsTab::OnSearchChanged(const FText& Text)
{
    FilterText = Text.ToString();
    RebuildTree();
}

// ---------------------------------------------------------------------------
// v1.16 — "Select Materials in Material Manager"
// ---------------------------------------------------------------------------

FString SMapLinkMaterialsTab::PendingFocusStem;
TWeakPtr<SMapLinkMaterialsTab> SMapLinkMaterialsTab::LiveTab;

void SMapLinkMaterialsTab::RequestFocusOnStem(const FString& Stem)
{
    if (TSharedPtr<SMapLinkMaterialsTab> Tab = LiveTab.Pin())
    {
        Tab->ApplyFocusStem(Stem);
        return;
    }
    PendingFocusStem = Stem;
}

TArray<FString> SMapLinkMaterialsTab::PendingSelectNames;

void SMapLinkMaterialsTab::RequestSelectMaterials(const TArray<FString>& MINames)
{
    if (TSharedPtr<SMapLinkMaterialsTab> Tab = LiveTab.Pin())
    {
        Tab->ApplySelectMaterials(MINames);
        return;
    }
    PendingSelectNames = MINames;
}

void SMapLinkMaterialsTab::ApplySelectMaterials(const TArray<FString>& MINames)
{
    if (!TreeView.IsValid()) return;

    // Clear any text filter first: a name filter and an explicit selection
    // fight each other, and the filter would hide most of what was asked for.
    FilterText.Empty();
    if (SearchBox.IsValid())
    {
        SearchBox->SetText(FText::GetEmpty());   // rebuilds the tree
    }
    else
    {
        RebuildTree();
    }

    const TSet<FString> Wanted(MINames);
    TreeView->ClearSelection();

    int32 Hits = 0;
    TSharedPtr<FMapLinkMatNode> FirstHit;

    for (const TSharedPtr<FMapLinkMatNode>& Group : RootNodes)
    {
        if (!Group.IsValid()) continue;

        bool bGroupHasHit = false;
        for (const TSharedPtr<FMapLinkMatNode>& Leaf : Group->Children)
        {
            if (!Leaf.IsValid() || !Wanted.Contains(Leaf->DisplayName)) continue;

            TreeView->SetItemSelection(Leaf, true);
            bGroupHasHit = true;
            ++Hits;
            if (!FirstHit.IsValid()) FirstHit = Leaf;
        }

        // Expand only groups that contain something, so the tree opens on the
        // answer instead of opening everything.
        if (bGroupHasHit) TreeView->SetItemExpansion(Group, true);
    }

    if (FirstHit.IsValid()) TreeView->RequestScrollIntoView(FirstHit);

    UE_LOG(LogMapLink, Log,
           TEXT("[Materials] Selected %d of %d requested material(s)."),
           Hits, MINames.Num());

    if (Hits == 0 && MINames.Num() > 0)
    {
        // Silence here would look like the button did nothing.
        MapLinkNotify::Warn(FText::FromString(FString::Printf(
            TEXT("None of the %d assigned material(s) are MapLink material "
                 "instances."), MINames.Num())));
    }
}

void SMapLinkMaterialsTab::ApplyFocusStem(const FString& Stem)
{
    FilterText = Stem;

    // Driven through the search box rather than FilterText alone: a filtered
    // list with an empty search field looks like a bug, and leaves the user no
    // obvious way back to the full set.
    if (SearchBox.IsValid())
    {
        SearchBox->SetText(FText::FromString(Stem));   // fires OnSearchChanged
    }
    else
    {
        RebuildTree();
    }
}

void SMapLinkMaterialsTab::OnSortChanged(
    TSharedPtr<FString> NewSort, ESelectInfo::Type /*SelectInfo*/)
{
    if (NewSort.IsValid())
    {
        CurrentSortOption = NewSort;
        RebuildTree();
    }
}

TSharedRef<ITableRow> SMapLinkMaterialsTab::OnGenerateRow(
    TSharedPtr<FMapLinkMatNode> Node,
    const TSharedRef<STableViewBase>& OwnerTable)
{
    const FSlateFontInfo Font = Node->bIsParentGroup
        ? FAppStyle::Get().GetFontStyle("BoldFont")
        : FAppStyle::Get().GetFontStyle("NormalFont");

    // ── v1.16 — a texture parameter row ───────────────────────────────────
    //
    // Glyph, name, status word. Nothing else on the row: the remedy is the
    // same sentence for every External texture, and printing it 239 times
    // beside 239 identical status words is noise, not information. It lives
    // in the tooltip, where it is there when it is wanted.
    if (Node->bIsTextureLeaf)
    {
        const FLinearColor Colour = MatTextureStatusColour(Node->TextureStatus);

        FString Tip = Node->TextureAssetPath;
        Tip += TEXT("\n\n") + MatStatusWords(Node->TextureStatus).ToString();
        if (!Node->TextureRemedy.IsEmpty())
        {
            Tip += TEXT("\n") + Node->TextureRemedy;
        }
        Tip += TEXT("\n\nDouble-click to open in the Textures tab.");

        return SNew(STableRow<TSharedPtr<FMapLinkMatNode>>, OwnerTable)
            .ToolTipText(FText::FromString(Tip))
            [
                SNew(SBox).Padding(FMargin(2.f, 2.f))
                [
                    SNew(SHorizontalBox)

                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                      .Padding(0.f, 0.f, 4.f, 0.f)
                    [
                        SNew(STextBlock)
                        .Text(FText::FromString(MatTextureStatusGlyph(Node->TextureStatus)))
                        .ColorAndOpacity(FSlateColor(Colour))
                    ]

                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                    [
                        SNew(STextBlock)
                        .Text(FText::FromString(Node->DisplayName))
                    ]

                    + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                      .Padding(8.f, 0.f, 0.f, 0.f)
                    [
                        SNew(STextBlock)
                        .Text(MatStatusWords(Node->TextureStatus))
                        .ColorAndOpacity(FSlateColor(Colour))
                    ]
                ]
            ];
    }

    // ── Group or MI row, with a status badge when health is on ────────────
    return SNew(STableRow<TSharedPtr<FMapLinkMatNode>>, OwnerTable)
    [
        SNew(SBox).Padding(FMargin(2.f, 2.f))
        [
            SNew(SHorizontalBox)

            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
              .Padding(0.f, 0.f, 4.f, 0.f)
            [
                SNew(STextBlock)
                .Visibility(Node->bHasTextureStatus
                    ? EVisibility::Visible : EVisibility::Collapsed)
                .Text(FText::FromString(MatTextureStatusGlyph(Node->TextureStatus)))
                .ColorAndOpacity(FSlateColor(
                    MatTextureStatusColour(Node->TextureStatus)))
                .ToolTipText(FText::Format(
                    LOCTEXT("MatWorstTexture",
                        "Worst texture status: {0}"),
                    MatStatusWords(Node->TextureStatus)))
            ]

            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [
                SNew(STextBlock)
                .Text(FText::FromString(Node->DisplayName))
                .Font(Font)
            ]
        ]
    ];
}

void SMapLinkMaterialsTab::OnGetChildren(
    TSharedPtr<FMapLinkMatNode> Node,
    TArray<TSharedPtr<FMapLinkMatNode>>& OutChildren)
{
    if (Node.IsValid())
    {
        OutChildren = Node->Children;
    }
}

void SMapLinkMaterialsTab::OnSelectionChanged(
    TSharedPtr<FMapLinkMatNode> Node, ESelectInfo::Type /*SelectInfo*/)
{
    if (Node.IsValid() && !Node->bIsParentGroup && Node->MI.IsValid())
    {
        SelectMI(Node->MI.Get());
    }
}

// ───────────────────────────────────────────────────────────────────────
// Selection / details / preview
// ───────────────────────────────────────────────────────────────────────

void SMapLinkMaterialsTab::SelectMI(UMaterialInstanceConstant* MI)
{
    if (PreviewViewport.IsValid())
    {
        // Route to its blend-mode slot (opaque → 0, alpha → 1).
        PreviewViewport->SetPreviewMaterial(MI);
    }
    FocusForEdit(MI);
}

void SMapLinkMaterialsTab::FocusForEdit(UMaterialInstanceConstant* MI)
{
    SelectedMI = MI;
    bHasUnsavedEdits = false;
    BuildParamPanel(MI);
    if (DetailsView.IsValid())
    {
        DetailsView->SetObject(MI, /*bForceRefresh=*/true);
    }
}

FReply SMapLinkMaterialsTab::OnOpenInEditorClicked()
{
    if (SelectedMI.IsValid() && GEditor)
    {
        // Launch UE's native Material Instance editor (full parameter UI with
        // working override checkboxes, parameter groups, base overrides).
        GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()
            ->OpenEditorForAsset(SelectedMI.Get());
    }
    return FReply::Handled();
}

void SMapLinkMaterialsTab::MarkEditedAndPreview()
{
    bHasUnsavedEdits = true;
    if (PreviewViewport.IsValid() && SelectedMI.IsValid())
    {
        PreviewViewport->SetPreviewMaterial(SelectedMI.Get());
    }
}

// ── Flat exposed-parameter panel ────────────────────────────────────────
// Override state is read and cleared straight from the MI's own override
// arrays (matched by parameter name), so the checkbox can never desync from
// the value — clearing reverts to the inherited (parent) value and unticks.
void SMapLinkMaterialsTab::BuildParamPanel(UMaterialInstanceConstant* MI)
{
    if (!ParamsBox.IsValid()) { return; }
    ParamsBox->ClearChildren();
    if (!MI) { return; }

    TWeakObjectPtr<UMaterialInstanceConstant> WeakMI = MI;
    const FSlateFontInfo BoldFont   = FAppStyle::Get().GetFontStyle("BoldFont");
    const FSlateFontInfo NormalFont = FAppStyle::Get().GetFontStyle("NormalFont");

    auto AddSection = [this, BoldFont](const FString& Title)
    {
        ParamsBox->AddSlot().Padding(2.f, 8.f, 2.f, 2.f)
        [ SNew(STextBlock).Text(FText::FromString(Title)).Font(BoldFont) ];
    };

    auto AddRow = [this, BoldFont, NormalFont](
        const FString& Name, TFunction<bool()> IsOver,
        TFunction<void(bool)> SetOver, const TSharedRef<SWidget>& Editor)
    {
        ParamsBox->AddSlot().Padding(2.f, 1.f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 4.f, 0.f)
            [
                SNew(SCheckBox)
                .ToolTipText(LOCTEXT("OvTip", "Override this parameter on the instance"))
                .IsChecked_Lambda([IsOver]()
                {
                    return IsOver() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
                })
                .OnCheckStateChanged_Lambda([this, SetOver](ECheckBoxState S)
                {
                    SetOver(S == ECheckBoxState::Checked);
                    MarkEditedAndPreview();
                })
            ]
            + SHorizontalBox::Slot().FillWidth(0.45f).VAlign(VAlign_Center)
            [
                SNew(STextBlock).Text(FText::FromString(Name))
                .Font_Lambda([BoldFont, NormalFont, IsOver]()
                {
                    return IsOver() ? BoldFont : NormalFont;
                })
            ]
            + SHorizontalBox::Slot().FillWidth(0.55f).VAlign(VAlign_Center)
            [
                SNew(SBox).IsEnabled_Lambda([IsOver]() { return IsOver(); })
                [ Editor ]
            ]
        ];
    };

    // ── Parent + base material ───────────────────────────────────────────
    ParamsBox->AddSlot().Padding(2.f, 2.f, 2.f, 2.f)
    [
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 6.f, 0.f)
        [ SNew(STextBlock).Text(LOCTEXT("ParentLbl", "Parent:")).Font(BoldFont) ]
        + SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
        [
            SNew(SObjectPropertyEntryBox)
            .AllowedClass(UMaterialInterface::StaticClass())
            .ThumbnailPool(ThumbnailPool)
            .DisplayThumbnail(true)
            .OnShouldFilterAsset_Lambda([](const FAssetData& A)
            {
                return !FMapLinkUtils::IsMapLinkMaterialPath(A.PackageName.ToString());
            })
            .ObjectPath_Lambda([WeakMI]()
            {
                UMaterialInstanceConstant* M = WeakMI.Get();
                return (M && M->Parent) ? M->Parent->GetPathName() : FString();
            })
            .OnObjectChanged(this, &SMapLinkMaterialsTab::OnReparent)
        ]
    ];

    // Base (top) material — shown only when the parent is itself an MI.
    if (UMaterialInterface* ParentMtl = MI->Parent)
    {
        if (ParentMtl->IsA(UMaterialInstance::StaticClass()))
        {
            if (UMaterial* Base = MI->GetMaterial())
            {
                ParamsBox->AddSlot().Padding(2.f, 0.f, 2.f, 4.f)
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(FString::Printf(
                        TEXT("Base Material:  %s"), *Base->GetName())))
                    .ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f)))
                ];
            }
        }
    }

    // ── Scalars ──────────────────────────────────────────────────────────
    {
        TArray<FMaterialParameterInfo> Infos; TArray<FGuid> Guids;
        MI->GetAllScalarParameterInfo(Infos, Guids);
        if (Infos.Num() > 0) { AddSection(TEXT("Scalars")); }
        for (const FMaterialParameterInfo& Info : Infos)
        {
            const FName PName = Info.Name;
            TFunction<bool()> IsOver = [WeakMI, PName]() -> bool
            {
                if (UMaterialInstanceConstant* M = WeakMI.Get())
                    for (const FScalarParameterValue& E : M->ScalarParameterValues)
                        if (E.ParameterInfo.Name == PName) { return true; }
                return false;
            };
            TFunction<void(bool)> SetOver = [WeakMI, Info, PName](bool bOn)
            {
                UMaterialInstanceConstant* M = WeakMI.Get(); if (!M) { return; }
                if (bOn)
                {
                    float V = 0.f; M->GetScalarParameterValue(Info, V);
                    M->SetScalarParameterValueEditorOnly(Info, V);
                }
                else
                {
                    M->ScalarParameterValues.RemoveAll(
                        [PName](const FScalarParameterValue& E){ return E.ParameterInfo.Name == PName; });
                }
                M->PostEditChange();
            };
            AddRow(Info.Name.ToString(), IsOver, SetOver,
                SNew(SSpinBox<float>)
                .MinDesiredWidth(80.f)
                .Value_Lambda([WeakMI, Info]()
                {
                    float V = 0.f;
                    if (UMaterialInstanceConstant* M = WeakMI.Get()) { M->GetScalarParameterValue(Info, V); }
                    return V;
                })
                .OnValueCommitted_Lambda([this, WeakMI, Info](float NewV, ETextCommit::Type)
                {
                    if (UMaterialInstanceConstant* M = WeakMI.Get())
                    {
                        M->SetScalarParameterValueEditorOnly(Info, NewV);
                        M->PostEditChange();
                        MarkEditedAndPreview();
                    }
                }));
        }
    }

    // ── Vectors (RGBA) ───────────────────────────────────────────────────
    {
        TArray<FMaterialParameterInfo> Infos; TArray<FGuid> Guids;
        MI->GetAllVectorParameterInfo(Infos, Guids);
        if (Infos.Num() > 0) { AddSection(TEXT("Vectors (R G B A)")); }
        for (const FMaterialParameterInfo& Info : Infos)
        {
            const FName PName = Info.Name;
            TFunction<bool()> IsOver = [WeakMI, PName]() -> bool
            {
                if (UMaterialInstanceConstant* M = WeakMI.Get())
                    for (const FVectorParameterValue& E : M->VectorParameterValues)
                        if (E.ParameterInfo.Name == PName) { return true; }
                return false;
            };
            TFunction<void(bool)> SetOver = [WeakMI, Info, PName](bool bOn)
            {
                UMaterialInstanceConstant* M = WeakMI.Get(); if (!M) { return; }
                if (bOn)
                {
                    FLinearColor C = FLinearColor::Black; M->GetVectorParameterValue(Info, C);
                    M->SetVectorParameterValueEditorOnly(Info, C);
                }
                else
                {
                    M->VectorParameterValues.RemoveAll(
                        [PName](const FVectorParameterValue& E){ return E.ParameterInfo.Name == PName; });
                }
                M->PostEditChange();
            };
            auto MakeCh = [this, WeakMI, Info](int32 Ch) -> TSharedRef<SWidget>
            {
                return SNew(SSpinBox<float>).MinDesiredWidth(44.f)
                    .Value_Lambda([WeakMI, Info, Ch]()
                    {
                        FLinearColor C = FLinearColor::Black;
                        if (UMaterialInstanceConstant* M = WeakMI.Get()) { M->GetVectorParameterValue(Info, C); }
                        return C.Component(Ch);
                    })
                    .OnValueCommitted_Lambda([this, WeakMI, Info, Ch](float NewV, ETextCommit::Type)
                    {
                        if (UMaterialInstanceConstant* M = WeakMI.Get())
                        {
                            FLinearColor C = FLinearColor::Black; M->GetVectorParameterValue(Info, C);
                            C.Component(Ch) = NewV;
                            M->SetVectorParameterValueEditorOnly(Info, C);
                            M->PostEditChange();
                            MarkEditedAndPreview();
                        }
                    });
            };
            AddRow(Info.Name.ToString(), IsOver, SetOver,
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().FillWidth(1.f).Padding(1.f, 0.f)[ MakeCh(0) ]
                + SHorizontalBox::Slot().FillWidth(1.f).Padding(1.f, 0.f)[ MakeCh(1) ]
                + SHorizontalBox::Slot().FillWidth(1.f).Padding(1.f, 0.f)[ MakeCh(2) ]
                + SHorizontalBox::Slot().FillWidth(1.f).Padding(1.f, 0.f)[ MakeCh(3) ]);
        }
    }

    // ── Textures ─────────────────────────────────────────────────────────
    {
        TArray<FMaterialParameterInfo> Infos; TArray<FGuid> Guids;
        MI->GetAllTextureParameterInfo(Infos, Guids);
        if (Infos.Num() > 0) { AddSection(TEXT("Textures")); }
        for (const FMaterialParameterInfo& Info : Infos)
        {
            const FName PName = Info.Name;
            TFunction<bool()> IsOver = [WeakMI, PName]() -> bool
            {
                if (UMaterialInstanceConstant* M = WeakMI.Get())
                    for (const FTextureParameterValue& E : M->TextureParameterValues)
                        if (E.ParameterInfo.Name == PName) { return true; }
                return false;
            };
            TFunction<void(bool)> SetOver = [WeakMI, Info, PName](bool bOn)
            {
                UMaterialInstanceConstant* M = WeakMI.Get(); if (!M) { return; }
                if (bOn)
                {
                    UTexture* T = nullptr; M->GetTextureParameterValue(Info, T);
                    M->SetTextureParameterValueEditorOnly(Info, T);
                }
                else
                {
                    M->TextureParameterValues.RemoveAll(
                        [PName](const FTextureParameterValue& E){ return E.ParameterInfo.Name == PName; });
                }
                M->PostEditChange();
            };
            AddRow(Info.Name.ToString(), IsOver, SetOver,
                SNew(SObjectPropertyEntryBox)
                .AllowedClass(UTexture::StaticClass())
                .ThumbnailPool(ThumbnailPool)
                .DisplayThumbnail(true)
                .ObjectPath_Lambda([WeakMI, Info]()
                {
                    UTexture* T = nullptr;
                    if (UMaterialInstanceConstant* M = WeakMI.Get()) { M->GetTextureParameterValue(Info, T); }
                    return T ? T->GetPathName() : FString();
                })
                .OnObjectChanged_Lambda([this, WeakMI, Info](const FAssetData& A)
                {
                    if (UMaterialInstanceConstant* M = WeakMI.Get())
                    {
                        M->SetTextureParameterValueEditorOnly(Info, Cast<UTexture>(A.GetAsset()));
                        M->PostEditChange();
                        MarkEditedAndPreview();
                    }
                }));
        }
    }
}

void SMapLinkMaterialsTab::OnReparent(const FAssetData& Asset)
{
    if (!SelectedMI.IsValid())
    {
        return;
    }
    UMaterialInterface* NewParent = Cast<UMaterialInterface>(Asset.GetAsset());
    UMaterialInstanceConstant* MI = SelectedMI.Get();
    if (!NewParent || MI->Parent == NewParent)
    {
        return;
    }

    MI->SetParentEditorOnly(NewParent);
    MI->PostEditChange();
    UEditorAssetLibrary::SaveLoadedAsset(MI, /*bOnlyIfDirty=*/false);

    FocusForEdit(MI);                    // refresh native panel for the new parent
    if (PreviewViewport.IsValid())
    {
        PreviewViewport->SetPreviewMaterial(MI);
    }
    RebuildTree();                       // regroup the MI under its new parent
    bHasUnsavedEdits = false;
    RunTailSync(TEXT("Reparent"));
}

void SMapLinkMaterialsTab::OnSlot1Changed(const FAssetData& Asset)
{
    // Viewport-only: set slot 1; never change the material being edited.
    if (PreviewViewport.IsValid())
    {
        PreviewViewport->SetSlotMaterial(1, Cast<UMaterialInterface>(Asset.GetAsset()));
    }
}

FReply SMapLinkMaterialsTab::OnRevertSlot1Clicked()
{
    if (PreviewViewport.IsValid())
    {
        PreviewViewport->SetSlotMaterial(1, nullptr);   // → cube default material
    }
    return FReply::Handled();
}

// (Custom parameter panel removed in v1.10 — the embedded view is a standard
// MI details view; full native parameter editing is via "Open in Material
// Editor". See FocusForEdit / OnOpenInEditorClicked.)


UMaterialInterface* SMapLinkMaterialsTab::GetContextParent() const
{
    if (!TreeView.IsValid())
    {
        return nullptr;
    }
    TArray<TSharedPtr<FMapLinkMatNode>> Sel = TreeView->GetSelectedItems();
    if (Sel.Num() == 0 || !Sel[0].IsValid())
    {
        return nullptr;
    }
    const TSharedPtr<FMapLinkMatNode>& Node = Sel[0];
    if (Node->bIsParentGroup)
    {
        return Node->GroupParent.Get();
    }
    return Node->MI.IsValid() ? Node->MI->Parent : nullptr;
}

// ───────────────────────────────────────────────────────────────────────
// Ops — each ends in SyncAfterOp via RunTailSync
// ───────────────────────────────────────────────────────────────────────

void SMapLinkMaterialsTab::RunTailSync(const FString& OpName)
{
    const FString CL = FString::Printf(TEXT("[MapLink] Material %s"), *OpName);
    const EMapLinkResult R = FMapLinkMaterialExporter::SyncAfterOp(CL);

    if (R != EMapLinkResult::Success && R != EMapLinkResult::NoMaterialInstancesFound)
    {
        MapLinkNotify::Warn(FText::Format(
            NSLOCTEXT("MapLink", "SyncTailWarnFmt",
                "'{0}' applied in UE, but the Blender library sync did not "
                "complete: {1}\nThe library is marked out of sync — use "
                "Settings → Resync Material Library to recover."),
            FText::FromString(OpName), MapLinkResultToText(R)));
    }
}

FReply SMapLinkMaterialsTab::OnExportLibraryClicked()
{
    FMapLinkMaterialLibraryExchange::ShowExportDialog();
    return FReply::Handled();
}

FReply SMapLinkMaterialsTab::OnImportLibraryClicked()
{
    // The import flow ends in SyncAfterOp internally (invisible engine tail).
    FMapLinkMaterialLibraryExchange::ShowImportDialog();
    RebuildTree();
    return FReply::Handled();
}

FReply SMapLinkMaterialsTab::OnStampAssetThumbnailsClicked()
{
    const EAppReturnType::Type Answer = FMessageDialog::Open(
        EAppMsgType::YesNo,
        LOCTEXT("StampThumbsConfirm",
            "Re-render every MapLink material instance's thumbnail and cache "
            "it into its .uasset?\n\n"
            "This checks out and saves every material instance in the project "
            "onto one changelist. The Content Browser will then show the same "
            "picture as the Blender Asset Browser."));

    if (Answer != EAppReturnType::Yes)
    {
        return FReply::Handled();
    }

    const EMapLinkResult Result = FMapLinkMaterialExporter::StampAssetThumbnails();
    if (Result == EMapLinkResult::Success
        || Result == EMapLinkResult::NoMaterialInstancesFound)
    {
        MapLinkNotify::Success(LOCTEXT("StampThumbsDone",
            "Asset thumbnails restamped. The counts are in the log."));
    }
    else
    {
        MapLinkNotify::Fail(FText::Format(
            LOCTEXT("StampThumbsFailedFmt",
                "Restamping thumbnails failed: {0}"),
            MapLinkResultToText(Result)));
    }
    return OnRefreshClicked();
}

FReply SMapLinkMaterialsTab::OnRebuildFromJsonClicked()
{
    // Same contract as import: the flow ends in SyncAfterOp internally.
    FMapLinkMaterialLibraryExchange::ShowRebuildFromJsonDialog();
    RebuildTree();
    return FReply::Handled();
}

FReply SMapLinkMaterialsTab::OnNewClicked()
{
    UMaterialInterface* Parent = nullptr;
    FString Name;
    if (!PromptNewMI(GetContextParent(), Parent, Name))
    {
        return FReply::Handled();
    }

    const FString PackagePath = FMapLinkUtils::GetMaterialInstancesContentPath();

    IAssetTools& AssetTools =
        FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

    UMaterialInstanceConstantFactoryNew* Factory =
        NewObject<UMaterialInstanceConstantFactoryNew>();
    Factory->InitialParent = Parent;

    UObject* NewObj = AssetTools.CreateAsset(
        Name, PackagePath, UMaterialInstanceConstant::StaticClass(), Factory);

    UMaterialInstanceConstant* NewMI = Cast<UMaterialInstanceConstant>(NewObj);
    if (!NewMI)
    {
        FMessageDialog::Open(EAppMsgType::Ok,
            FText::FromString(TEXT("Failed to create the Material Instance.")));
        return FReply::Handled();
    }

    UEditorAssetLibrary::SaveLoadedAsset(NewMI, /*bOnlyIfDirty=*/false);

    RebuildTree();
    SelectMI(NewMI);
    RunTailSync(TEXT("Create"));
    return FReply::Handled();
}

FReply SMapLinkMaterialsTab::OnCreateChildClicked()
{
    if (!SelectedMI.IsValid())
    {
        return FReply::Handled();
    }

    FString Name;
    if (!PromptText(LOCTEXT("ChildTitle", "Create Child Material Instance"),
                    LOCTEXT("ChildLabel", "New name:"),
                    SelectedMI->GetName() + TEXT("_Child"), Name))
    {
        return FReply::Handled();
    }

    const FString PackagePath = FMapLinkUtils::GetMaterialInstancesContentPath();

    IAssetTools& AssetTools =
        FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

    UMaterialInstanceConstantFactoryNew* Factory =
        NewObject<UMaterialInstanceConstantFactoryNew>();
    Factory->InitialParent = SelectedMI.Get();   // child inherits the selected MI

    UObject* NewObj = AssetTools.CreateAsset(
        Name, PackagePath, UMaterialInstanceConstant::StaticClass(), Factory);

    UMaterialInstanceConstant* ChildMI = Cast<UMaterialInstanceConstant>(NewObj);
    if (!ChildMI)
    {
        FMessageDialog::Open(EAppMsgType::Ok,
            FText::FromString(TEXT("Failed to create the child Material Instance.")));
        return FReply::Handled();
    }

    // A child inherits everything and overrides nothing — guarantee a clean
    // override set so every parameter starts un-ticked (inherited), exactly
    // like making an instance from an instance in UE.
    ChildMI->ScalarParameterValues.Empty();
    ChildMI->VectorParameterValues.Empty();
    ChildMI->TextureParameterValues.Empty();
    ChildMI->PostEditChange();

    UEditorAssetLibrary::SaveLoadedAsset(ChildMI, /*bOnlyIfDirty=*/false);

    RebuildTree();
    SelectMI(ChildMI);
    RunTailSync(TEXT("Create Child"));
    return FReply::Handled();
}

FReply SMapLinkMaterialsTab::OnDuplicateClicked()
{
    if (!SelectedMI.IsValid())
    {
        return FReply::Handled();
    }

    FString NewName;
    if (!PromptText(LOCTEXT("DupTitle", "Duplicate Material Instance"),
                    LOCTEXT("DupLabel", "New name:"),
                    SelectedMI->GetName() + TEXT("_Copy"), NewName))
    {
        return FReply::Handled();
    }

    const FString PackagePath = FMapLinkUtils::GetMaterialInstancesContentPath();
    const FString SrcPath     = SelectedMI->GetPackage()->GetName();
    const FString DestPath    = PackagePath / NewName;

    UObject* Dup = UEditorAssetLibrary::DuplicateAsset(SrcPath, DestPath);
    UMaterialInstanceConstant* DupMI = Cast<UMaterialInstanceConstant>(Dup);
    if (!DupMI)
    {
        FMessageDialog::Open(EAppMsgType::Ok,
            FText::FromString(TEXT("Duplicate failed.")));
        return FReply::Handled();
    }

    UEditorAssetLibrary::SaveLoadedAsset(DupMI, /*bOnlyIfDirty=*/false);

    RebuildTree();
    SelectMI(DupMI);
    RunTailSync(TEXT("Duplicate"));
    return FReply::Handled();
}

FReply SMapLinkMaterialsTab::OnRenameClicked()
{
    if (!SelectedMI.IsValid())
    {
        return FReply::Handled();
    }

    const FString OldName = SelectedMI->GetName();
    FString NewName;
    if (!PromptText(LOCTEXT("RenameTitle", "Rename Material Instance"),
                    LOCTEXT("RenameLabel", "New name:"), OldName, NewName)
        || NewName == OldName)
    {
        return FReply::Handled();
    }

    const FString PackagePath = FMapLinkUtils::GetMaterialInstancesContentPath();
    const FString OldPath     = SelectedMI->GetPackage()->GetName();
    const FString NewPath     = PackagePath / NewName;

    if (!UEditorAssetLibrary::RenameAsset(OldPath, NewPath))
    {
        FMessageDialog::Open(EAppMsgType::Ok,
            FText::FromString(TEXT("Rename failed.")));
        return FReply::Handled();
    }

    UMaterialInstanceConstant* RenamedMI =
        Cast<UMaterialInstanceConstant>(UEditorAssetLibrary::LoadAsset(NewPath));

    RebuildTree();
    if (RenamedMI)
    {
        SelectMI(RenamedMI);
    }
    RunTailSync(TEXT("Rename"));
    return FReply::Handled();
}

FReply SMapLinkMaterialsTab::OnSaveClicked()
{
    if (!SelectedMI.IsValid())
    {
        return FReply::Handled();
    }

    UEditorAssetLibrary::SaveLoadedAsset(SelectedMI.Get(), /*bOnlyIfDirty=*/false);
    bHasUnsavedEdits = false;
    RunTailSync(TEXT("Edit"));
    return FReply::Handled();
}

FReply SMapLinkMaterialsTab::OnRevertClicked()
{
    if (!SelectedMI.IsValid())
    {
        return FReply::Handled();
    }

    UPackage* Pkg = SelectedMI->GetPackage();
    if (Pkg)
    {
        TArray<UPackage*> Packages = { Pkg };
        UPackageTools::ReloadPackages(Packages);
    }

    // The MI object may have been replaced by the reload — rebuild & clear.
    bHasUnsavedEdits = false;
    SelectMI(nullptr);
    RebuildTree();
    return FReply::Handled();
}

FReply SMapLinkMaterialsTab::OnRefreshClicked()
{
    RebuildTree();
    return FReply::Handled();
}

// ── Material library sync UI (v1.13.1, moved from the Manager Settings tab) ──

FText SMapLinkMaterialsTab::GetMaterialSyncStatusText() const
{
    // v1.16 — state, not an explanation. What Resync is for belongs in the
    // documentation; the strip's job is to say whether anything is wrong.
    return FMapLinkMaterialExporter::IsLibraryDirty()
        ? LOCTEXT("MatLibDirty",  "Material library: OUT OF SYNC")
        : LOCTEXT("MatLibClean",  "Material library: in sync");
}

FSlateColor SMapLinkMaterialsTab::GetMaterialSyncStatusColor() const
{
    return FMapLinkMaterialExporter::IsLibraryDirty()
        ? FSlateColor(FLinearColor(0.95f, 0.55f, 0.15f))   // amber warning
        : FSlateColor(FLinearColor(0.6f, 0.6f, 0.6f));      // muted grey
}

FReply SMapLinkMaterialsTab::OnResyncMaterialLibraryClicked()
{
    const EMapLinkResult R = FMapLinkMaterialExporter::Resync();

    if (R == EMapLinkResult::Success)
        MapLinkNotify::Success(FText::FromString(
            TEXT("Material libraries resynced — both siblings up to date.")));
    else if (R == EMapLinkResult::Cancelled)
        MapLinkNotify::Warn(FText::FromString(
            TEXT("Resync cancelled. The libraries may still be out of sync.")));
    else if (R == EMapLinkResult::NoMaterialInstancesFound)
        MapLinkNotify::Warn(FText::FromString(
            TEXT("No Material Instances found — nothing to sync.")));
    else
        FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
            NSLOCTEXT("MapLink", "ResyncFailedFmt",
                "Resync failed: {0}\nThe libraries are still marked out of "
                "sync; check the Output Log for details."),
            MapLinkResultToText(R)));

    // Refresh the status label immediately.
    RebuildTree();
    return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
