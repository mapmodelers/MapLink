// Copyright MapModelers. All Rights Reserved.
//
// v1.10 (MIM Drop 2) — Materials tab for the MapLink Manager window.
//
// Parent-grouped Material Instance tree + IDetailsView param panel + live
// preview viewport (MapLink cube) + the single-MI authoring ops (New /
// Duplicate / Rename / Edit-Save / Revert).  Every mutating op ends in
// FMapLinkMaterialExporter::SyncAfterOp(...) so the Blender libraries stay in
// sync through the invisible auto-sync engine (Drop 1).

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STreeView.h"
#include "Styling/SlateColor.h"   // v1.13.1 — GetMaterialSyncStatusColor
#include "MapLinkTextureTypes.h"  // v1.16 — EMapLinkTextureStatus on tree nodes

class SScrollBox;
class IDetailsView;
class ITableRow;
class STableViewBase;
class SMapLinkMaterialPreviewViewport;
class UMaterialInstanceConstant;
class UMaterialInterface;
class FAssetThumbnailPool;
struct FAssetData;

/** One node in the parent-grouped MI tree: either a parent group (has
 *  children, no MI) or an MI leaf (has MI, no children). */
struct FMapLinkMatNode
{
    bool    bIsParentGroup = false;
    FString DisplayName;                  // parent name (group) or MI name (leaf)
    TWeakObjectPtr<UMaterialInstanceConstant> MI;          // leaf only
    TWeakObjectPtr<UMaterialInterface>        GroupParent; // group only
    TArray<TSharedPtr<FMapLinkMatNode>>       Children;

    // ── v1.16 — the third level: an MI's texture parameters ──────────────
    //
    // A material is not broken for a teammate on its own; it is broken
    // BECAUSE one of its textures resolves only on this machine. Making that
    // a child row is what turns "something is wrong with this material" into
    // "this parameter, this texture, this fix".
    bool    bIsTextureLeaf = false;
    FString ParamName;                    // texture leaf only
    FString TextureAssetPath;             // texture leaf only
    FString TextureRemedy;                // texture leaf only

    /** For a texture leaf, that texture's status. For an MI leaf or a group,
     *  the WORST status among everything underneath — which is the only
     *  summary that does not lie: one local-only texture in a material makes
     *  the whole material local-only for the person opening it. */
    EMapLinkTextureStatus TextureStatus = EMapLinkTextureStatus::Unclassified;

    /** False when nothing underneath has a texture status worth showing, so a
     *  clean project renders exactly as it did before v1.16. */
    bool bHasTextureStatus = false;
};

class SMapLinkMaterialsTab : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SMapLinkMaterialsTab) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    /** v1.16 — focus the tab on one asset's material instances.
     *
     *  Two paths because the Manager may or may not already be open: a live
     *  tab is filtered immediately, otherwise the stem is parked and consumed
     *  by the next Construct. Same pattern as the Asset Viewer's
     *  PendingViewerStem. */
    static void RequestFocusOnStem(const FString& Stem);

    /** v1.16 — select the given material instances by name.
     *
     *  Takes names rather than object pointers so the request survives being
     *  parked while no tab exists, and so it cannot keep assets alive. */
    static void RequestSelectMaterials(const TArray<FString>& MINames);

private:
    /** Parked filter for a tab that does not exist yet. */
    static FString PendingFocusStem;
    static TArray<FString> PendingSelectNames;
    void ApplySelectMaterials(const TArray<FString>& MINames);
    /** The live tab, if one is constructed. */
    static TWeakPtr<SMapLinkMaterialsTab> LiveTab;
    /** Reflects the filter into the search box as well as the tree, so the
     *  user can see why the list is narrowed and can clear it. */
    TSharedPtr<class SSearchBox> SearchBox;
    void ApplyFocusStem(const FString& Stem);

public:

private:
    // ── Tree + search/sort ─────────────────────────────────────────────
    TArray<TSharedPtr<FMapLinkMatNode>> RootNodes;     // parent groups
    TSharedPtr<STreeView<TSharedPtr<FMapLinkMatNode>>> TreeView;

    FString FilterText;                                // MI name filter
    TArray<TSharedPtr<FString>> SortOptions;           // "Name (A-Z)" / "(Z-A)"
    TSharedPtr<FString>         CurrentSortOption;

    /** 0 = group by base Material (default), 1 = group by immediate parent
     *  (so child MIs get their own category under their parent MI),
     *  2 = flat list (all MIs, mixed). */
    int32                       GroupMode = 0;
    TArray<TSharedPtr<FString>> GroupModeOptions;
    TSharedPtr<FString>         CurrentGroupMode;

    void OnSearchChanged(const FText& Text);
    void OnSortChanged(TSharedPtr<FString> NewSort, ESelectInfo::Type SelectInfo);
    void OnGroupModeChanged(TSharedPtr<FString> NewMode, ESelectInfo::Type SelectInfo);

    // ── v1.16 — texture health ────────────────────────────────────────
    //
    // Off by default. The third tree level costs a scan lookup per MI, and a
    // user who is not chasing a portability problem should not pay for it or
    // look at it.
    bool bShowTextureHealth = false;

    FText       GetTextureHealthText() const;
    FSlateColor GetTextureHealthColour() const;
    FReply      OnShowNonConformClicked();

    /** Fill an MI leaf's children from the texture scan's reverse index and
     *  roll the worst status upward. No-op when bShowTextureHealth is off. */
    void AttachTextureChildren(const TSharedPtr<FMapLinkMatNode>& Leaf,
                               UMaterialInstanceConstant* MI);

    void RebuildTree();
    TSharedRef<ITableRow> OnGenerateRow(
        TSharedPtr<FMapLinkMatNode> Node,
        const TSharedRef<STableViewBase>& OwnerTable);
    void OnGetChildren(
        TSharedPtr<FMapLinkMatNode> Node,
        TArray<TSharedPtr<FMapLinkMatNode>>& OutChildren);
    void OnSelectionChanged(
        TSharedPtr<FMapLinkMatNode> Node, ESelectInfo::Type SelectInfo);

    // ── Params + preview ───────────────────────────────────────────────
    // The embedded view is a standard details view of the MI (parent, base
    // property overrides, parameter override arrays). For the full native
    // parameter UI with override checkboxes, "Open in Material Editor" launches
    // UE's actual Material Instance editor for the selected MI.
    TSharedPtr<SScrollBox>                       ParamsBox;     // exposed params
    TSharedPtr<IDetailsView>                     DetailsView;   // "Advanced Parameters"
    TSharedPtr<SMapLinkMaterialPreviewViewport>  PreviewViewport;
    TSharedPtr<FAssetThumbnailPool>              ThumbnailPool;

    TWeakObjectPtr<UMaterialInstanceConstant> SelectedMI;
    bool bHasUnsavedEdits = false;

    /** Tree-select an MI: route it to slot 0 + focus for edit. */
    void SelectMI(UMaterialInstanceConstant* MI);

    /** Point the param panel + details view at a material (being edited). */
    void FocusForEdit(UMaterialInstanceConstant* MI);

    /** Build the inline exposed-parameter editor for an MI: every scalar /
     *  vector / texture parameter of the parent, editable in place (editing a
     *  value creates the override, matching MI behaviour). Override toggling /
     *  reset lives in the native editor via "Open in Material Editor". */
    void BuildParamPanel(UMaterialInstanceConstant* MI);

    /** Open the selected MI in UE's native Material Instance editor. */
    FReply OnOpenInEditorClicked();

    /** Flag unsaved + refresh the live preview after a parameter edit. */
    void MarkEditedAndPreview();

    /** Reparent the focused MI (MapLink-filtered) → rebuild + tail sync. */
    void OnReparent(const FAssetData& Asset);

    /** Slot 1 is a VIEWPORT-ONLY extra material; it never changes the MI we
     *  are editing (that is always the list selection on slot 0). */
    void   OnSlot1Changed(const FAssetData& Asset);
    FReply OnRevertSlot1Clicked();   // clear slot 1 → cube default

    /** Resolve the currently-relevant parent material for a new MI:
     *  the selected leaf's parent, or the selected group's parent. */
    UMaterialInterface* GetContextParent() const;

    // ── Library exchange (v1.12) ────────────────────────────────────────
    FReply OnExportLibraryClicked();
    FReply OnImportLibraryClicked();
    FReply OnRebuildFromJsonClicked();   // v1.16 — material_instances.json
    FReply OnStampAssetThumbnailsClicked();  // v1.20 — .uasset thumbnails

    // ── Material library sync status + manual Resync (v1.13.1, moved here
    //    from the Manager Settings tab). Sync is automatic after every op;
    //    this button is for recovery when the library-dirty flag is raised.
    FText       GetMaterialSyncStatusText() const;
    FSlateColor GetMaterialSyncStatusColor() const;
    FReply      OnResyncMaterialLibraryClicked();

    // ── Ops (each ends in SyncAfterOp) ──────────────────────────────────
    FReply OnNewClicked();
    FReply OnCreateChildClicked();   // new MI parented to the selected MI
    FReply OnDuplicateClicked();
    FReply OnRenameClicked();
    /** v1.17 — Duplicate / Rename / Export / Import / Rebuild, demoted out
     *  of the eight-button block into a "More" menu. */
    TSharedRef<SWidget> BuildMaterialOverflowMenu();

    FReply OnSaveClicked();
    FReply OnRevertClicked();
    FReply OnRefreshClicked();

    bool   HasSelectedMI() const { return SelectedMI.IsValid(); }
    bool   IsSaveEnabled() const { return SelectedMI.IsValid() && bHasUnsavedEdits; }

    /** Run the auto-sync tail step under a per-op changelist and report. */
    void   RunTailSync(const FString& OpName);
};
