// Copyright MapModelers. All Rights Reserved.
//
// v1.7.3 — Added "Edit source .blend" and "Open in Explorer" context menu entries.

#pragma once

#include "CoreMinimal.h"
#include "AssetTypeActions_Base.h"

class UMapLinkProxyAsset;

/**
 * Asset type actions for UMapLinkProxyAsset.
 * Provides custom colour, double-click behaviour, and context-menu entries
 * for Import / Reimport / MapLink Manager / Edit Blend / Open Explorer.
 */
class FAssetTypeActions_MapLinkProxy : public FAssetTypeActions_Base
{
public:
    // FAssetTypeActions_Base interface
    virtual FText GetName() const override;
    virtual FColor GetTypeColor() const override;
    virtual UClass* GetSupportedClass() const override;
    virtual uint32 GetCategories() override;
    virtual void OpenAssetEditor(
        const TArray<UObject*>& InObjects,
        TSharedPtr<class IToolkitHost> EditWithinLevelEditor) override;
    virtual void GetActions(
        const TArray<UObject*>& InObjects,
        struct FToolMenuSection& Section) override;
    virtual bool HasActions(const TArray<UObject*>& InObjects) const override;

private:
    void ExecuteImport(TArray<TWeakObjectPtr<UObject>> Objects);
    void ExecuteReimport(TArray<TWeakObjectPtr<UObject>> Objects);
    // v1.14 — Force Full Reimport (whole tree, hashes ignored, confirmed).
    void ExecuteForceReimport(TArray<TWeakObjectPtr<UObject>> Objects);
    // v1.9 — WP expansion moved to the MapLink Manager window.
    void ExecuteOpenManager(TArray<TWeakObjectPtr<UObject>> Objects);

    // v1.7.3
    void ExecuteEditSourceBlend(TArray<TWeakObjectPtr<UObject>> Objects);
    void ExecuteOpenInExplorer(TArray<TWeakObjectPtr<UObject>> Objects);
};
