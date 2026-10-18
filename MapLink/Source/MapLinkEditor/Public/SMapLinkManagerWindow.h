// Copyright MapModelers. All Rights Reserved.
//
// v1.9 — MapLink Manager window.
// v1.17 — Expansions promoted from a panel to a tab.
//
// Dockable nomad tab, six pages (ETab, which the SWidgetSwitcher slot order
// must match):
//   Project    — link / create / unlink a MapLink project.
//   Assets     — every imported .blend (proxy) with live state (Up to date /
//                Out of date / Missing .blend / Orphan BP), selection actions,
//                per-row Auto / Recursive / Skip, batch "Reimport
//                Out-of-Date" and "Warm FBX Cache", and totals.
//   Expansions — v1.17.  Create form (root + WP level + depth + label, with a
//                dry-run preview) over the list of every expansion that
//                exists: state, actor counts, pause, re-expand, cleanup.
//                Up to v1.16 this was a fixed panel under the Assets table
//                that could only ever describe ONE expansion per root,
//                because that is all UMapLinkEditorState could store.
//   Materials  — material instance manager (v1.10).
//   Textures   — texture manager & conformer (v1.16).
//   Settings   — UMapLinkSettings details view (shared settings →
//                DefaultGame.ini; project paths → per-user ini, v1.15).

#pragma once

#include "CoreMinimal.h"
#include "MapLinkWPExpansionHelper.h"   // v1.17 — FMapLinkExpansionPreview
#include "Types/SlateEnums.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SComboBox.h"

class UMapLinkProxyAsset;
class SMapLinkAssetInspector;   // v1.19
class IDetailsView;
class SSearchBox;
template<typename T> class SSpinBox;

/** One row in the Assets list. */
struct FMapLinkManagerRow
{
    FString BlendName;                       // stem, e.g. "LI_MyLevel"
    FString BlendPath;                       // absolute .blend path
    FString TypeStr;                         // "LI" / "SM" / "BP"
    FString StateStr;                        // display state
    /** v1.18 — why the state says what it says: the two timestamps compared,
     *  or which hash differed. "Out of date" a hundred times over is a claim
     *  with no evidence, and the reflex is to distrust the column. */
    FString StateTooltipStr;
    FString LastImportStr;                   // v1.17 — relative ("3 h ago")
    FString LastImportAbsStr;                // v1.17 — absolute, for the tooltip
    FDateTime LastImportTime;                // parsed (for sorting)
    bool    bHasTimestamp = false;
    int32   StateRank   = 3;                 // 0=missing 1=orphan 2=ood 3=ok
    bool    bOutOfDate  = false;
    bool    bMissing    = false;
    bool    bOrphanBP   = false;
    bool    bFavourite  = false;
    bool    bAutoUpdate = false;
    bool    bAutoUpdateRecursive = false;
    bool    bSkipAutoReimport = false;       // v1.13.1
    // v1.12 — WP expansion info (LI roots only): "L_World · d2", else empty.
    // v1.17 — a root may now be expanded into several levels, and more than
    // once into one, so the cell holds a chip per record.  ExpansionStr is
    // the joined form, kept for sorting and tooltips.
    bool    bExpanded    = false;
    FString ExpansionStr;
    struct FChip
    {
        FGuid   Id;
        FString Text;      // "WP_Game d2"
    };
    TArray<FChip> ExpansionChips;
    TWeakObjectPtr<UMapLinkProxyAsset> Proxy;
};

/** v1.17 — one row in the Expansions list (a UMapLinkEditorState record,
 *  plus the derived state the record cannot store: whether its level still
 *  exists, and whether a dependency has changed since it last expanded). */
struct FMapLinkExpansionRow
{
    FGuid           Id;
    FString         Label;
    FString         RootBlendName;
    FSoftObjectPath TargetLevel;
    FString         LevelName;
    int32           ExpandDepth = 1;
    int32           PlacedCount = 0;
    bool            bReExpandOnImport = true;
    FString         LastExpandedStr;

    /** v1.18 — this row was read out of the open LEVEL, not the ini: an
     *  expansion physically present that this machine has no record of.
     *  Id/TargetLevel/Depth above are unset for these. */
    bool            bDiscovered  = false;
    FString         DiscoveredIdTag;      // empty = pre-v1.17 actors
    FString         DiscoveredRootGuess;
    bool            bStreamedOut = false;

    /** Derived — see EState below. */
    enum class EState : uint8
    {
        InSync,        // level exists, nothing changed since last expand
        Stale,         // a dependency reimported after LastExpandedUtc
        NeverExpanded, // record exists, nothing placed yet
        Paused,        // bReExpandOnImport off
        LevelMissing,  // TargetLevel no longer resolves
        RootMissing,   // the root .blend/proxy is gone
        NotInRecords,  // v1.18 — in the open level, not in your ini
    };
    EState State = EState::InSync;

    FText   GetStateText() const;
    FLinearColor GetStateColour() const;
};

class SMapLinkManagerWindow : public SCompoundWidget
{
public:
    /** Tab indices, so cross-tab navigation is not a magic number.
     *  MUST match the SWidgetSwitcher slot order in Construct(). */
    enum class ETab : int32
    {
        Project = 0, Assets = 1, Expansions = 2,
        Materials = 3, Textures = 4, Settings = 5
    };

    SLATE_BEGIN_ARGS(SMapLinkManagerWindow) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

private:
    // ── Tabs (v1.12; v1.17 — Expansions) ─────────────────────────────
    // Construct() starts on Project when no MapLink project is linked.
    int32 ActiveTabIndex = 1;

    /** v1.17 — one tab-strip toggle. Replaces five copies of the same
     *  25-line SCheckBox block.
     *  v1.18 — takes a tooltip, because "Assets (106⚠)" does not say whether
     *  106 is the number of assets or the number of problems. */
    TSharedRef<SWidget> MakeTabButton(ETab Tab, const TAttribute<FText>& Label,
                                      const TAttribute<FText>& Tooltip =
                                          TAttribute<FText>());

    /** v1.18 — the strip along the bottom: what this window is pointed at.
     *  Every destructive button here raises "what am I about to affect?" and
     *  nothing on screen answered it. */
    TSharedRef<SWidget> BuildContextStrip();
    FText GetContextStripText() const;

    // ── Assets list ──────────────────────────────────────────────────
    TArray<TSharedPtr<FMapLinkManagerRow>> AllRows;
    TArray<TSharedPtr<FMapLinkManagerRow>> FilteredRows;
    TSharedPtr<SListView<TSharedPtr<FMapLinkManagerRow>>> ListView;
    TSharedPtr<SSearchBox> SearchBox;
    FString FilterText;
    /** v1.19 — FilterText split on whitespace. A row must contain EVERY
     *  token, in any order: "bzn bar" finds LI_bzn_barn_01. */
    TArray<FString> FilterTokens;

    // ── Filter chips (v1.13) ─────────────────────────────────────────
    // Type toggles (LI/SM/BP) and state toggles (Out-of-date / Missing /
    // OK), combined with FilterText in ApplyFilter().  All default ON.
    bool bFilterTypeLI  = true;
    bool bFilterTypeSM  = true;
    bool bFilterTypeBP  = true;
    bool bFilterStateOOD     = true;
    bool bFilterStateMissing = true;
    bool bFilterStateOK      = true;
    /** v1.17 — which count a chip displays next to its label. */
    enum class EChipCount : uint8
    {
        None, TypeLI, TypeSM, TypeBP, StateOOD, StateMissing, StateOK
    };
    int32 CountForChip(EChipCount Which) const;

    /** One filter chip bound to *Flag; re-applies the filter.
     *  v1.17 — carries a live count ("Out-of-date  106").
     *  v1.19.1 — ONE style for all six chips. The two-weight split (discreet
     *  type chips, filled state chips) is gone: it made one row of controls
     *  read as two kinds of thing. Urgency now rides on the COUNT's colour
     *  rather than on the chip's fill, so "Missing 3" still catches the eye
     *  while "Missing 0" is as quiet as "LI". */
    TSharedRef<SWidget> MakeFilterChip(const FText& Label, bool* Flag,
                                       const FText& Tooltip,
                                       EChipCount Which = EChipCount::None);

    // ── Auto-refresh (v1.9) ──────────────────────────────────────────
    bool bAutoRefresh = true;
    double LastAutoRefreshSec = 0.0;
    uint32 LastSignature = 0;
    /** Cheap content hash (proxy set + mtimes + flags + favourites).
     *  The auto-refresh timer rebuilds the list only when this changes,
     *  so an idle window never flickers or loses selection. */
    uint32 ComputeStateSignature() const;
    EActiveTimerReturnType OnAutoRefreshTimer(double InCurrentTime, float InDeltaTime);

    // ── Sorting (v1.9) ───────────────────────────────────────────────
    FName SortColumn;                          // set in Construct
    EColumnSortMode::Type SortMode = EColumnSortMode::Ascending;
    EColumnSortMode::Type GetColumnSortMode(const FName ColumnId) const;
    void OnColumnSort(EColumnSortPriority::Type Priority,
                      const FName& ColumnId,
                      EColumnSortMode::Type NewMode);
    void SortRows();

    /** Rebuild AllRows from the asset registry.
     *  @param bVerifyHashes  true → full MD5 compare against the stored
     *         BlendFileHash (slow but exact); false → mtime heuristic. */
    void RefreshRows(bool bVerifyHashes);
    void ApplyFilter();

    TSharedRef<ITableRow> OnGenerateRow(
        TSharedPtr<FMapLinkManagerRow> Item,
        const TSharedRef<STableViewBase>& OwnerTable);

    // ── Toolbar actions ──────────────────────────────────────────────
    FReply OnVerifyClicked();
    /** v1.15 — manual counterpart of the auto-refresh timer (cheap re-scan). */
    FReply OnManualRefreshClicked();
    FReply OnReimportOutOfDateClicked();
    FReply OnWarmCacheClicked();
    /** v1.17 — "106 assets · 106 out of date · 2 expansions" under the list.
     *  The Assets tab could report per-row state and a tab badge, but never
     *  the totals the badge is derived from. */
    FText  GetAssetTotalsText() const;
    // v1.12 — Project tab actions (New / Open-Locate / Unlink).
    FReply OnNewProjectClicked();
    FReply OnOpenProjectClicked();
    FReply OnUnlinkProjectClicked();

    // ── Selection-based actions (v1.12) ──────────────────────────────
    // The per-row button strip was removed; actions run on the list
    // selection from the action bar and the row context menu.
    TArray<TSharedPtr<FMapLinkManagerRow>> GetSelectedRows() const;
    bool HasAnySelection() const;
    bool HasSingleSelection() const;
    bool SelectionAllBP() const;
    FReply OnSelReimport();      // all selected
    FReply OnSelForceReimport(); // all selected — v1.14: Force Full Reimport
    FReply OnSelUpdateStandIn(); // all selected (BP only)
    FReply OnSelBlender();       // single
    FReply OnSelBrowse();        // single
    FReply OnSelExplorer();      // single
    FReply OnSelProxy();         // single
    FReply OnSelViewer();        // single (or first)
    TSharedPtr<SWidget> OnRowContextMenu();
    void OnRowDoubleClick(TSharedPtr<FMapLinkManagerRow> Item);

    // ── Row actions (called by the row widget) ───────────────────────
    void RowReimport(TSharedPtr<FMapLinkManagerRow> Row);
    void RowOpenBlender(TSharedPtr<FMapLinkManagerRow> Row);
    void RowOpenExplorer(TSharedPtr<FMapLinkManagerRow> Row);
    void RowOpenContentBrowser(TSharedPtr<FMapLinkManagerRow> Row);
    void RowOpenProxy(TSharedPtr<FMapLinkManagerRow> Row);
    void RowOpenInViewer(TSharedPtr<FMapLinkManagerRow> Row);   // v1.11
    void RowUpdateStandIn(TSharedPtr<FMapLinkManagerRow> Row);
    void RowSetAutoUpdate(TSharedPtr<FMapLinkManagerRow> Row, bool bEnabled);
    void RowSetRecursive(TSharedPtr<FMapLinkManagerRow> Row, bool bEnabled);
    void RowSetSkipAutoReimport(TSharedPtr<FMapLinkManagerRow> Row,
                                bool bEnabled);   // v1.13.1
    void RowToggleFavourite(TSharedPtr<FMapLinkManagerRow> Row);

    // ── Expansions tab (v1.9 panel → v1.17 tab) ──────────────────────
    //
    // The create form keeps the shape of the v1.16 panel (root + level +
    // depth + Expand), because that part was never the problem.  What it
    // gains: a WP-filtered level picker, a live dry-run preview, an optional
    // label, and — below it — the list of every expansion that exists, which
    // is the thing the singleton panel could not show.

    TArray<TSharedPtr<FString>> LIRootOptions;
    TSharedPtr<FString> SelectedRoot;
    TSharedPtr<SComboBox<TSharedPtr<FString>>> RootCombo;
    FString SelectedWPLevelPath;     // object path of the target UWorld
    int32   ExpandDepth = 1;
    FString NewExpansionLabel;       // v1.17 — blank = use the root stem

    /** v1.17 — dry run for the current (root, depth), refreshed whenever
     *  either changes.  Never touches the level. */
    FMapLinkExpansionPreview PreviewCache;
    FString PreviewKey;              // "<root>|<depth>" the cache was built for
    void    RefreshExpansionPreview();
    FText   GetExpansionPreviewText() const;
    FText   GetDepthSuffixText() const;

    void RebuildLIRootOptions();
    FText GetSelectedRootText() const;
    FText GetExpansionSummaryText() const;
    FString GetWPLevelObjectPath() const;
    void OnWPLevelChanged(const struct FAssetData& AssetData);
    bool ShouldFilterOutLevel(const struct FAssetData& AssetData) const;
    bool CanExpandNow() const;
    FText GetExpandTooltip() const;
    FReply OnExpandClicked();
    FReply OnCleanupClicked();

    // ── Expansions list (v1.17) ──────────────────────────────────────
    TArray<TSharedPtr<FMapLinkExpansionRow>> ExpansionRows;
    TSharedPtr<SListView<TSharedPtr<FMapLinkExpansionRow>>> ExpansionListView;

    void RefreshExpansionRows();
    TSharedRef<ITableRow> OnGenerateExpansionRow(
        TSharedPtr<FMapLinkExpansionRow> Item,
        const TSharedRef<STableViewBase>& OwnerTable);
    TSharedPtr<SWidget> OnExpansionContextMenu();
    void OnExpansionDoubleClick(TSharedPtr<FMapLinkExpansionRow> Item);
    TArray<TSharedPtr<FMapLinkExpansionRow>> GetSelectedExpansions() const;

    FReply OnReExpandSelected();
    FReply OnReExpandStale();
    /** Shared by both: group by level, expand, save each level once. */
    FReply ReExpandByIds(const TArray<FGuid>& Ids);
    FReply OnCleanupSelectedExpansions();
    FReply OnCleanupAllInLevel();
    void   OnOpenExpansionLevel(TSharedPtr<FMapLinkExpansionRow> Item);
    void   OnRenameExpansion(TSharedPtr<FMapLinkExpansionRow> Item);
    /** v1.20 — set an expansion's depth in place. The record already carries
     *  ExpandDepth and ExpandRecord already cleans up its own previous
     *  placement before re-expanding, so changing depth here and pressing
     *  Re-expand is the whole "go from 3 to 2" operation. Before this the
     *  depth was display-only and the only route was deleting every actor and
     *  every folder by hand — which is what walked into the outliner assert. */
    void   OnSetExpansionDepth(TSharedPtr<FMapLinkExpansionRow> Item,
                               int32 NewDepth);

    void   OnSetExpansionPaused(TSharedPtr<FMapLinkExpansionRow> Item,
                                bool bPaused);

    // ── Discovered expansions (v1.18) ────────────────────────────────
    /** Append rows for expansions found in the OPEN level that no record
     *  covers. Called at the tail of RefreshExpansionRows. */
    void AppendDiscoveredExpansionRows();
    void OnAdoptExpansion(TSharedPtr<FMapLinkExpansionRow> Item);
    void OnRemoveDiscoveredExpansion(TSharedPtr<FMapLinkExpansionRow> Item);

    /** Jump to the Expansions tab with the Expand form pre-filled for a row
     *  from the Assets list — the "+" chip in the Expansion column. */
    void BeginExpandForRoot(const FString& RootBlendName);
    /** Select a record in the Expansions list and show the tab. */
    void FocusExpansion(const FGuid& Id);

    // ── Settings tab ─────────────────────────────────────────────────
    TSharedPtr<IDetailsView> SettingsView;
    void OnSettingsChanged(const struct FPropertyChangedEvent& Event);

    // v1.13.1 — Resync material library UI (status + button) moved to
    // SMapLinkMaterialsTab.

    // ── helpers ──────────────────────────────────────────────────────
    TSharedRef<SWidget> BuildProjectTab();     // v1.12 — project lifecycle
    /** v1.17 — the Project tab's status card: counts that drill through to
     *  the tab that fixes them. */
    TSharedRef<SWidget> BuildProjectStatusCard();
    TSharedRef<SWidget> MakeStatusLine(const FText& Label,
                                       TAttribute<FText> Value,
                                       TAttribute<FSlateColor> Colour,
                                       ETab GoToTab,
                                       const FText& Tooltip);
    TSharedRef<SWidget> BuildAssetsTab();
    /** v1.19 — the Assets tab in three bands. Actions and filters were one
     *  strip of ~20 mixed controls; doing and filtering are different intents
     *  and now sit in different bands. */
    TSharedRef<SWidget> BuildAssetsToolbar();
    TSharedRef<SWidget> BuildAssetsFilterBar();
    /** v1.19 — project-wide maintenance, under the list, sharing a row with
     *  the totals it acts on. */
    TSharedRef<SWidget> BuildAssetsMaintenanceBar();
    /** Icon button that degrades to a text button when the icon is unset —
     *  FMapLinkEditorModule::GetIcon returns an empty FSlateIcon for art that
     *  is not on disk, so icon-only would silently render blank rectangles. */
    TSharedRef<SWidget> MakeIconButton(const FSlateIcon& Icon,
                                       const FText& Label,
                                       const FText& Tooltip,
                                       FOnClicked OnClicked,
                                       TAttribute<bool> IsEnabled,
                                       bool bAlwaysShowLabel = false);
    TSharedRef<SWidget> MakeToolbarSeparator();

    // ── Inspector (v1.19) ────────────────────────────────────────────
    TSharedPtr<SMapLinkAssetInspector> AssetInspector;
    bool IsInspectorVisible() const;
    /** Push the current selection at the inspector, or clear it when the
     *  selection is not exactly one row. */
    void SyncInspectorToSelection();
    TSharedRef<SWidget> BuildExpansionsTab();  // v1.17 — WP expansions
    TSharedRef<SWidget> BuildMaterialsTab();   // v1.10 (MIM Drop 2)
    TSharedRef<SWidget> BuildTexturesTab();    // v1.16 — Texture Manager

public:
    /**
     * v1.16 — bring a tab to the front.
     *
     * SWidgetSwitcher builds every slot up front, so a drill-through target
     * can select its row and still be invisible because the switcher is
     * showing a different page. Selecting without switching is what made
     * double-clicking a texture in the Materials tab look like it did
     * nothing. No-op when no Manager window is open.
     */
    static void ActivateTab(ETab Tab);

private:
    static TWeakPtr<SMapLinkManagerWindow> LiveWindow;
    TSharedRef<SWidget> BuildSettingsTab();
    /** v1.12 — "MapLink" menu button content: mirrors the main-menu
     *  MapLink entries so they stay reachable from a floating Manager. */
    TSharedRef<SWidget> BuildMapLinkMenuButton();
    static TArray<UMapLinkProxyAsset*> CollectAllProxies();

    friend class SMapLinkManagerTableRow;
    friend class SMapLinkExpansionTableRow;   // v1.17
};
