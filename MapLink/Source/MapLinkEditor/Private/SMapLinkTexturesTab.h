// Copyright MapModelers. All Rights Reserved.
//
// v1.16 — Textures tab (Texture Manager & Conformer).
//
// The question this tab answers is one sentence long: "will the person I send
// this project to see the same thing I see?" Everything on screen is either
// that answer, or the button that changes it.
//
// Two views over the same rows, because texture work is two different jobs:
//   Thumbnails  you are looking for a texture and you know what it looks like
//   List        you are auditing, and you want to sort by what is wrong
//
// A detail panel on the right carries what a row cannot: a big preview, the
// file on disk, and every material that uses it — clickable, because "which
// materials break if I touch this" is the question that follows every other
// question here.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STileView.h"
#include "Styling/SlateColor.h"

#include "MapLinkTextureTypes.h"

class ITableRow;
class STableViewBase;
class SSearchBox;
class SBox;
class SVerticalBox;
class FAssetThumbnailPool;
class FAssetThumbnail;

/** One row. A copy of the scan row, not a pointer into it: Rescan() replaces
 *  the scan result wholesale, and a view holding pointers into the old one
 *  would be reading freed memory the moment anybody hit Refresh. */
struct FMapLinkTextureListItem
{
    FString AssetPath;
    FString AssetName;
    FString PackagePath;
    FString SourceDiskPath;
    FString RelativeToLibrary;
    FString Remedy;
    FString Compression;

    EMapLinkTextureClass  TextureClass   = EMapLinkTextureClass::Unclassified;
    EMapLinkTextureClass  SuggestedClass = EMapLinkTextureClass::Unclassified;
    EMapLinkTextureStatus Status         = EMapLinkTextureStatus::Unclassified;

    bool bSRGB   = false;
    bool bOrphan = false;

    /** Material instances that use this texture. THE reverse index — one bad
     *  texture on a shared parent is fifteen bad materials, and this is what
     *  turns that back into one fix. */
    TArray<FString> ReferencingMaterials;

    /** Built lazily on first draw, then cached — a thumbnail per row for 245
     *  rows up front would load every texture in the project. */
    TSharedPtr<FAssetThumbnail> Thumbnail;
};

class SMapLinkTexturesTab : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SMapLinkTexturesTab) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    /** Focus this tab on one texture, from the Materials tab's drill-through.
     *  Parked when no tab exists yet. */
    static void RequestFocusOnTexture(const FString& TextureAssetPath);

private:
    // ── Data ──────────────────────────────────────────────────────────────
    TArray<TSharedPtr<FMapLinkTextureListItem>> AllItems;
    TArray<TSharedPtr<FMapLinkTextureListItem>> VisibleItems;

    TSharedPtr<SListView<TSharedPtr<FMapLinkTextureListItem>>> ListView;
    TSharedPtr<STileView<TSharedPtr<FMapLinkTextureListItem>>> TileView;
    TSharedPtr<SSearchBox>          SearchBox;
    TSharedPtr<FAssetThumbnailPool> ThumbnailPool;

    /** Drop every live thumbnail and free the pool's GPU resources.
     *  Called before a long conform run — see the definition. */
    void        ReleaseThumbnails();

    // ── Detail panel ──────────────────────────────────────────────────────
    TSharedPtr<FMapLinkTextureListItem> DetailItem;
    TSharedPtr<SBox>            DetailPreviewBox;   // swapped, not rebuilt
    TSharedPtr<FAssetThumbnail> DetailThumbnail;    // held: the widget is a view onto it
    TSharedPtr<SVerticalBox>    UsedByBox;          // one hyperlink per material

    FString FilterText;

    /** 0 = Needs Attention, 1 = All, 2 = External, 3 = Unused, 4 = MapLink. */
    int32 FilterMode = 0;
    TArray<TSharedPtr<FString>> FilterModeOptions;
    TSharedPtr<FString>         CurrentFilterMode;

    /** false = thumbnails, true = list. Thumbnails first because finding a
     *  texture by eye is the commoner errand. */
    bool bListView = false;

    // ── Sorting (list view) ───────────────────────────────────────────────
    FName SortColumn;
    EColumnSortMode::Type SortMode = EColumnSortMode::Ascending;

    EColumnSortMode::Type GetSortModeForColumn(FName Column) const;
    void OnSortColumn(EColumnSortPriority::Type Priority, const FName& Column,
                      EColumnSortMode::Type NewMode);
    void SortVisibleItems();

    static TWeakPtr<SMapLinkTexturesTab> LiveTab;
    static FString PendingFocusTexture;
    void ApplyFocusTexture(const FString& TextureAssetPath);

    // ── Build ─────────────────────────────────────────────────────────────
    TSharedRef<SWidget> BuildHealthStrip();
    TSharedRef<SWidget> BuildToolbar();
    /** v1.18 — colour key for the thumbnail status stripe. Thumbnail view
     *  only; the list view names the status in a column. */
    TSharedRef<SWidget> BuildStatusLegend();
    TSharedRef<SWidget> BuildBrowser();
    TSharedRef<SWidget> BuildDetailPanel();

    void RebuildFromScan(bool bForceRescan);

    /**
     * Scroll-into-view has to wait a frame.
     *
     * RequestListRefresh does not regenerate rows until the next Slate tick,
     * so a scroll request issued in the same call finds no widget for the item
     * and is dropped. That is why a drill-through switched to this tab and
     * then sat at the top of the list. Parked here, consumed in Tick.
     */
    TSharedPtr<FMapLinkTextureListItem> PendingScrollTo;
    virtual void Tick(const FGeometry& AllottedGeometry,
                      const double InCurrentTime,
                      const float InDeltaTime) override;
    void ApplyFilter();
    void RefreshViews();

    TSharedRef<ITableRow> OnGenerateListRow(
        TSharedPtr<FMapLinkTextureListItem> Item,
        const TSharedRef<STableViewBase>& OwnerTable);
    TSharedRef<ITableRow> OnGenerateTile(
        TSharedPtr<FMapLinkTextureListItem> Item,
        const TSharedRef<STableViewBase>& OwnerTable);

    void OnSearchChanged(const FText& Text);
    void OnFilterModeChanged(TSharedPtr<FString> NewMode, ESelectInfo::Type SelectInfo);
    void OnSelectionChanged(TSharedPtr<FMapLinkTextureListItem> Item,
                            ESelectInfo::Type SelectInfo);

    // ── Health strip ──────────────────────────────────────────────────────
    FText       GetHealthText() const;
    FSlateColor GetHealthColour() const;
    /** v1.17 — how many textures the last scan says are not conform. Drives
     *  the banner's Conform All label and its visibility. */
    int32       CountNotConform() const;

    /** How many rows Conform All would act on. THE SAME predicate the click
     *  uses (ConformAllTakes) — a button that says (243) and then acts on 248
     *  is a bug the user only discovers in the confirm dialog. */
    int32       CountConformAllTargets() const;

    // ── Actions ───────────────────────────────────────────────────────────
    FReply OnScanClicked();
    void   RunConform(const TArray<FString>& Paths, const FText& Title);

    // ── Context menu ──────────────────────────────────────────────────────
    TSharedPtr<SWidget> OnContextMenu();
    TArray<UObject*> LoadSelectedAssets() const;
    void CopyToClipboard(const FString& Text) const;
    FReply OnConformClicked();
    void   OnConformClickedVoid();       // selection — promotes or writes as needed
    FReply OnConformAllClicked();    // every non-conform texture
    FReply OnImportClicked();        // file on disk -> MapLink, conformed
    FReply OnCleanUpClicked();
    FReply OnRevealSelectedClicked();
    FReply OnOpenAssetClicked();

    TArray<TSharedPtr<FMapLinkTextureListItem>> GetSelectedItems() const;
    TArray<FString> GetSelectedAssetPaths() const;
    bool HasSelection() const;
    bool HasSingleSelection() const;

    void RunOpAndRefresh(struct FMapLinkTextureOpReport&& Report);
};
