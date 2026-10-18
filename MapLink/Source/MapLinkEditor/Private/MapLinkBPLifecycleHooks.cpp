// Copyright MapModelers. All Rights Reserved.
//
// v1.8 D7 — Trimmed lifecycle hooks.  Drops the OnAssetsPreDelete /
// OnAssetRemoved cascade flow that was tied to UE's own Delete-asset
// path; stand-in deletion now goes exclusively through the explicit
// "Delete MapLink Stand-in" menu entry which calls
// FMapLinkBPStandInCreator::DeleteStandIn.

#include "MapLinkBPLifecycleHooks.h"
#include "MapLinkBPBinding.h"
#include "MapLinkBPReferenceMode.h"
#include "MapLinkProxyAsset.h"
#include "MapLinkUtils.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Blueprint.h"
#include "Modules/ModuleManager.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "MapLinkBPLifecycleHooks"

// =====================================================================
// Static state
// =====================================================================

FDelegateHandle FMapLinkBPLifecycleHooks::RenamedHandle;
FDelegateHandle FMapLinkBPLifecycleHooks::AddedHandle;

namespace
{
    bool IsBlueprintAsset(const FAssetData& Asset)
    {
        UClass* Cls = Asset.GetClass();
        return Cls && Cls->IsChildOf<UBlueprint>();
    }
}

// =====================================================================
// Register / Unregister
// =====================================================================

void FMapLinkBPLifecycleHooks::Register()
{
    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AR = AssetRegistryModule.Get();

    RenamedHandle = AR.OnAssetRenamed().AddStatic(
        &FMapLinkBPLifecycleHooks::OnAssetRenamed);
    AddedHandle   = AR.OnAssetAdded().AddStatic(
        &FMapLinkBPLifecycleHooks::OnAssetAdded);

    UE_LOG(LogMapLinkBPStandIn, Log,
           TEXT("[Lifecycle] Hooks registered (Renamed / Added).  "
                "Delete cascade is handled by the explicit "
                "'Delete MapLink Stand-in' menu entry."));
}

void FMapLinkBPLifecycleHooks::Unregister()
{
    if (FModuleManager::Get().IsModuleLoaded("AssetRegistry"))
    {
        FAssetRegistryModule& AssetRegistryModule =
            FModuleManager::GetModuleChecked<FAssetRegistryModule>("AssetRegistry");
        IAssetRegistry& AR = AssetRegistryModule.Get();

        if (RenamedHandle.IsValid())
        {
            AR.OnAssetRenamed().Remove(RenamedHandle);
            RenamedHandle.Reset();
        }
        if (AddedHandle.IsValid())
        {
            AR.OnAssetAdded().Remove(AddedHandle);
            AddedHandle.Reset();
        }
    }

    UE_LOG(LogMapLinkBPStandIn, Log,
           TEXT("[Lifecycle] Hooks unregistered."));
}

// =====================================================================
// OnAssetRenamed — propagate the new BP path to every ref-mode proxy
// =====================================================================

void FMapLinkBPLifecycleHooks::OnAssetRenamed(
    const FAssetData& Asset, const FString& OldObjectPath)
{
    if (!IsBlueprintAsset(Asset)) return;

    const FString NewPath = Asset.GetObjectPathString();
    if (OldObjectPath == NewPath) return;

    UE_LOG(LogMapLinkBPStandIn, Log,
           TEXT("[Lifecycle] Rename: '%s' → '%s' — scanning proxies"),
           *OldObjectPath, *NewPath);

    FAssetRegistryModule& AssetRegistryModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AR = AssetRegistryModule.Get();

    TArray<FAssetData> ProxyAssets;
    AR.GetAssetsByClass(
        UMapLinkProxyAsset::StaticClass()->GetClassPathName(),
        ProxyAssets);

    int32 Updated = 0;
    for (const FAssetData& Data : ProxyAssets)
    {
        UMapLinkProxyAsset* Proxy = Cast<UMapLinkProxyAsset>(Data.GetAsset());
        if (!Proxy || !Proxy->bIsBPReference) continue;
        if (Proxy->OriginalBoundBlueprintPath != OldObjectPath) continue;

        Proxy->OriginalBoundBlueprintPath = NewPath;
        Proxy->BoundBlueprint =
            TSoftObjectPtr<UBlueprint>(FSoftObjectPath(NewPath));
        Proxy->MarkPackageDirty();
        ++Updated;

        UE_LOG(LogMapLinkBPStandIn, Log,
               TEXT("  Updated proxy '%s'"), *Proxy->GetPathName());
    }

    if (Updated > 0)
    {
        UE_LOG(LogMapLinkBPStandIn, Log,
               TEXT("[Lifecycle] Rename: %d proxy(s) updated."), Updated);
    }

    UBlueprint* BP = Cast<UBlueprint>(Asset.GetAsset());
    if (BP && UMapLinkBPBinding::Get(BP))
    {
        BP->MarkPackageDirty();
    }
}

// =====================================================================
// OnAssetAdded — duplicate-detection + binding scrub
// =====================================================================

void FMapLinkBPLifecycleHooks::OnAssetAdded(const FAssetData& Asset)
{
    if (!IsBlueprintAsset(Asset)) return;

    UBlueprint* BP = Cast<UBlueprint>(Asset.GetAsset());
    if (!BP) return;

    UMapLinkBPBinding* Binding = UMapLinkBPBinding::Get(BP);
    if (!Binding) return;
    if (Binding->StandInBlendPath.IsEmpty()) return;
    if (!FPaths::FileExists(Binding->StandInBlendPath)) return;

    FString StandInBoundBPPath;
    FString StandInMode;
    if (!FMapLinkBPReferenceMode::ParseSidecarForBoundBP(
            Binding->StandInBlendPath, StandInBoundBPPath, StandInMode))
    {
        return;  // sidecar absent / malformed — leave binding alone
    }

    const FString ThisBPPath = BP->GetPathName();
    if (StandInBoundBPPath == ThisBPPath) return;  // legitimate owner

    UE_LOG(LogMapLinkBPStandIn, Warning,
           TEXT("[Lifecycle] Duplicate detected: BP '%s' carries a binding "
                "pointing at stand-in owned by '%s'. "
                "Scrubbing binding from duplicate."),
           *ThisBPPath, *StandInBoundBPPath);

    Binding->ClearFlags(RF_Public | RF_Standalone);
    Binding->SetFlags(RF_Transient);
    Binding->Rename(nullptr, GetTransientPackage(),
                    REN_DontCreateRedirectors | REN_NonTransactional);
    Binding->MarkAsGarbage();
    BP->MarkPackageDirty();
}

#undef LOCTEXT_NAMESPACE
