// Copyright MapModelers. All Rights Reserved.
//
// v1.16 — Cleanup dialog for the Textures tab.
//
// Deleting assets is the one thing in this feature that cannot be undone from
// the editor, so it gets a real window rather than a message box pointing at
// the Output Log. You see every asset, why it is on the list, what it would
// cost to keep it, and you tick the ones you mean.
//
// Nothing is pre-ticked. The dialog opens with an empty selection and a
// disabled Delete button, so the fastest possible sequence of clicks deletes
// nothing at all.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

#include "MapLinkTextureOps.h"

class SWindow;
class ITableRow;
class STableViewBase;

/** One tickable row. */
struct FMapLinkCleanupRow
{
    FMapLinkTextureCleanupItem Item;
    bool bChecked = false;
};

class SMapLinkTextureCleanupDialog : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SMapLinkTextureCleanupDialog) {}
        SLATE_ARGUMENT(TSharedPtr<SWindow>, ParentWindow)
        SLATE_ARGUMENT(TArray<FMapLinkTextureCleanupItem>, Items)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    /** Empty when the user cancelled or ticked nothing. */
    const TArray<FString>& GetConfirmedAssetPaths() const { return Confirmed; }

    /**
     * Open modally. Returns the asset paths the user ticked and confirmed;
     * empty on cancel. The caller does the deleting — this window's only job
     * is to be sure about what.
     */
    static TArray<FString> Show(const TArray<FMapLinkTextureCleanupItem>& Items);

private:
    TWeakPtr<SWindow> ParentWindow;
    TArray<TSharedPtr<FMapLinkCleanupRow>> Rows;
    TSharedPtr<SListView<TSharedPtr<FMapLinkCleanupRow>>> ListView;
    TArray<FString> Confirmed;

    TSharedRef<ITableRow> OnGenerateRow(TSharedPtr<FMapLinkCleanupRow> Row,
                                        const TSharedRef<STableViewBase>& OwnerTable);

    int32 NumChecked() const;
    int64 BytesChecked() const;

    FText  GetSummaryText() const;
    bool   IsDeleteEnabled() const;
    FReply OnSelectAll();
    FReply OnSelectNone();
    FReply OnSelectSuperseded();
    FReply OnDelete();
    FReply OnCancel();
};
