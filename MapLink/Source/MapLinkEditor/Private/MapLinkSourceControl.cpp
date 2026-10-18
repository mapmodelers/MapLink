// Copyright MapModelers. All Rights Reserved.
//
// v1.9 — Unified source-control helper (see header).

#include "MapLinkSourceControl.h"
#include "MapLinkPipeline.h"   // LogMapLink
#include "MapLinkNotify.h"     // v1.13 — shelve toasts
#include "MapLinkSettings.h"   // v1.13 — bShelveBeforeReimport
#include "MapLinkUtils.h"      // v1.13 — GetMapLinkContentRoot

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"

#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlState.h"
#include "ISourceControlChangelistState.h"
#include "SourceControlOperations.h"

#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "UObject/Package.h"

FString FMapLinkSourceControl::SessionDescription =
    TEXT("[MapLink] Import");

TMap<FString, FSourceControlChangelistPtr>
    FMapLinkSourceControl::ChangelistCache;

// ---------------------------------------------------------------------------
// Changelist resolution
// ---------------------------------------------------------------------------

FString FMapLinkSourceControl::NormalizeChangelistDescription(
    const FString& Description)
{
    // v1.13 — Robust find-or-create key.  Perforce round-trips descriptions
    // with altered whitespace (trailing newline appended on submit forms,
    // CRLF conversion, per-line indentation from `p4 change` templates),
    // which previously made the exact-compare miss and spawn duplicate CLs.
    // Normalization: per-line trim, drop trailing empty lines, '\n' joins.
    // Comparison remains case-SENSITIVE.
    // Mirrored by _normalize_description() in blender/p4_integration.py.
    TArray<FString> Lines;
    Description.Replace(TEXT("\r\n"), TEXT("\n"))
               .ParseIntoArray(Lines, TEXT("\n"), /*bCullEmpty=*/false);
    for (FString& Line : Lines)
    {
        Line.TrimStartAndEndInline();
    }
    while (Lines.Num() > 0 && Lines.Last().IsEmpty())
    {
        Lines.Pop();
    }
    return FString::Join(Lines, TEXT("\n"));
}

FSourceControlChangelistPtr FMapLinkSourceControl::GetOrCreateChangelist(
    const FString& Description)
{
    if (!ISourceControlModule::Get().IsEnabled())
    {
        return nullptr;
    }

    ISourceControlProvider& SCC = ISourceControlModule::Get().GetProvider();
    if (!SCC.UsesChangelists())
    {
        return nullptr;
    }

    const FString NormalizedTarget =
        NormalizeChangelistDescription(Description);

    // Session cache hit — verify the CL still exists and is pending before
    // trusting it (it may have been submitted/deleted externally in P4V).
    if (FSourceControlChangelistPtr* Cached = ChangelistCache.Find(Description))
    {
        bool bStillPending = false;
        if (Cached->IsValid())
        {
            // Cheap targeted refresh of just this CL, then re-check state.
            TSharedRef<FUpdatePendingChangelistsStatus> VerifyOp =
                ISourceControlOperation::Create<FUpdatePendingChangelistsStatus>();
            VerifyOp->SetChangelistsToUpdate(
                TArray<FSourceControlChangelistRef>{ Cached->ToSharedRef() });
            SCC.Execute(VerifyOp, EConcurrency::Synchronous);

            FSourceControlChangelistStatePtr CachedState =
                SCC.GetState(Cached->ToSharedRef(), EStateCacheUsage::Use);
            if (CachedState.IsValid())
            {
                const FString CachedDesc = NormalizeChangelistDescription(
                    CachedState->GetDescriptionText().ToString());
                bStillPending = (CachedDesc == NormalizedTarget);
            }
        }

        if (bStillPending)
        {
            return *Cached;
        }

        UE_LOG(LogMapLink, Log,
               TEXT("[SCC] Cached CL for '%s' no longer pending — "
                    "dropping cache entry and re-searching."),
               *Description);
        ChangelistCache.Remove(Description);
    }

    // Cache miss: refresh ALL pending CLs of the current workspace, every
    // time — the provider's state cache goes stale when CLs are created or
    // submitted outside this editor session.
    TSharedRef<FUpdatePendingChangelistsStatus> Op =
        ISourceControlOperation::Create<FUpdatePendingChangelistsStatus>();
    Op->SetUpdateAllChangelists(true);
    SCC.Execute(Op, EConcurrency::Synchronous);

    // Search by NORMALIZED description — stacking behaviour: a pending CL
    // with the same normalized description is ALWAYS reused, never
    // duplicated.
    TArray<FSourceControlChangelistRef> AllCLs =
        SCC.GetChangelists(EStateCacheUsage::Use);
    for (const FSourceControlChangelistRef& CLRef : AllCLs)
    {
        FSourceControlChangelistStatePtr CLState =
            SCC.GetState(CLRef, EStateCacheUsage::Use);
        if (CLState.IsValid())
        {
            const FString Desc = NormalizeChangelistDescription(
                CLState->GetDescriptionText().ToString());
            if (Desc == NormalizedTarget)
            {
                UE_LOG(LogMapLink, Log,
                       TEXT("[SCC] Reusing existing pending CL: %s"),
                       *Description);
                ChangelistCache.Add(Description, CLRef);
                return CLRef;
            }
        }
    }

    // Create new
    TSharedRef<FNewChangelist> NewCLOp =
        ISourceControlOperation::Create<FNewChangelist>();
    NewCLOp->SetDescription(FText::FromString(Description));

    if (SCC.Execute(NewCLOp, EConcurrency::Synchronous) ==
        ECommandResult::Succeeded)
    {
        FSourceControlChangelistPtr NewCL = NewCLOp->GetNewChangelist();
        if (NewCL.IsValid())
        {
            UE_LOG(LogMapLink, Log,
                   TEXT("[SCC] Created pending CL: %s"), *Description);
            ChangelistCache.Add(Description, NewCL);
            return NewCL;
        }
    }

    UE_LOG(LogMapLink, Warning,
           TEXT("[SCC] Could not create CL '%s' — falling back to default."),
           *Description);
    return nullptr;
}

void FMapLinkSourceControl::ResetChangelistCache()
{
    ChangelistCache.Empty();
}

// ---------------------------------------------------------------------------
// Session description
// ---------------------------------------------------------------------------

void FMapLinkSourceControl::SetSessionDescription(const FString& Description)
{
    SessionDescription = Description;
}

FString FMapLinkSourceControl::GetSessionDescription()
{
    return SessionDescription;
}

// ---------------------------------------------------------------------------
// File / package ops
// ---------------------------------------------------------------------------

bool FMapLinkSourceControl::EnsureFileInChangelist(
    const FString& AbsoluteFilePath,
    const FString& Description)
{
    if (!ISourceControlModule::Get().IsEnabled())
    {
        return true;
    }

    ISourceControlProvider& SCC = ISourceControlModule::Get().GetProvider();
    FSourceControlStatePtr State =
        SCC.GetState(AbsoluteFilePath, EStateCacheUsage::ForceUpdate);
    if (!State.IsValid())
    {
        return false;
    }

    // Already open for add/edit — nothing to do (file is already
    // stacked in whichever CL it was opened in).
    if (State->IsAdded() || State->IsCheckedOut())
    {
        return true;
    }

    FSourceControlChangelistPtr CL = GetOrCreateChangelist(Description);
    TArray<FString> Files = { AbsoluteFilePath };

    if (State->IsSourceControlled())
    {
        TSharedRef<FCheckOut> Op =
            ISourceControlOperation::Create<FCheckOut>();
        ECommandResult::Type R = CL.IsValid()
            ? SCC.Execute(Op, CL, Files, EConcurrency::Synchronous)
            : SCC.Execute(Op, Files, EConcurrency::Synchronous);
        UE_LOG(LogMapLink, Log,
               TEXT("[SCC] checkout '%s' → %s  (CL: %s)"),
               *FPaths::GetCleanFilename(AbsoluteFilePath),
               R == ECommandResult::Succeeded ? TEXT("OK") : TEXT("FAILED"),
               *Description);
        return R == ECommandResult::Succeeded;
    }

    TSharedRef<FMarkForAdd> Op =
        ISourceControlOperation::Create<FMarkForAdd>();
    ECommandResult::Type R = CL.IsValid()
        ? SCC.Execute(Op, CL, Files, EConcurrency::Synchronous)
        : SCC.Execute(Op, Files, EConcurrency::Synchronous);
    UE_LOG(LogMapLink, Log,
           TEXT("[SCC] add '%s' → %s  (CL: %s)"),
           *FPaths::GetCleanFilename(AbsoluteFilePath),
           R == ECommandResult::Succeeded ? TEXT("OK") : TEXT("FAILED"),
           *Description);
    return R == ECommandResult::Succeeded;
}

bool FMapLinkSourceControl::DeleteFileTracked(
    const FString& AbsoluteFilePath,
    const FString& Description)
{
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();

    auto DeleteFromDisk = [&PF, &AbsoluteFilePath]() -> bool
    {
        if (!FPaths::FileExists(AbsoluteFilePath))
        {
            return true;
        }
        PF.SetReadOnly(*AbsoluteFilePath, false);
        return IFileManager::Get().Delete(*AbsoluteFilePath, false, true);
    };

    if (AbsoluteFilePath.IsEmpty())
    {
        return true;
    }

    if (!ISourceControlModule::Get().IsEnabled())
    {
        return DeleteFromDisk();
    }

    ISourceControlProvider& SCC = ISourceControlModule::Get().GetProvider();
    FSourceControlStatePtr State =
        SCC.GetState(AbsoluteFilePath, EStateCacheUsage::ForceUpdate);
    if (!State.IsValid())
    {
        return DeleteFromDisk();
    }

    TArray<FString> Files = { AbsoluteFilePath };

    // Opened for add and never submitted. Deleting it while the add is pending
    // leaves the changelist unsubmittable, so revert first — that returns the
    // file to untracked, and then it is just a file.
    if (State->IsAdded())
    {
        TSharedRef<FRevert> Op = ISourceControlOperation::Create<FRevert>();
        const ECommandResult::Type R =
            SCC.Execute(Op, Files, EConcurrency::Synchronous);
        UE_LOG(LogMapLink, Log,
               TEXT("[SCC] un-add '%s' → %s  (was never submitted)"),
               *FPaths::GetCleanFilename(AbsoluteFilePath),
               R == ECommandResult::Succeeded ? TEXT("OK") : TEXT("FAILED"));
        return DeleteFromDisk();
    }

    // In the depot. p4 delete opens it for delete AND removes it from disk, so
    // there is nothing left to unlink afterwards.
    if (State->IsSourceControlled())
    {
        FSourceControlChangelistPtr CL = GetOrCreateChangelist(Description);

        // A file already open for edit cannot be opened for delete; revert the
        // checkout first so the delete is the only pending action on it.
        if (State->IsCheckedOut())
        {
            TSharedRef<FRevert> Revert = ISourceControlOperation::Create<FRevert>();
            SCC.Execute(Revert, Files, EConcurrency::Synchronous);
        }

        TSharedRef<FDelete> Op = ISourceControlOperation::Create<FDelete>();
        const ECommandResult::Type R = CL.IsValid()
            ? SCC.Execute(Op, CL, Files, EConcurrency::Synchronous)
            : SCC.Execute(Op, Files, EConcurrency::Synchronous);
        UE_LOG(LogMapLink, Log,
               TEXT("[SCC] delete '%s' → %s  (CL: %s)"),
               *FPaths::GetCleanFilename(AbsoluteFilePath),
               R == ECommandResult::Succeeded ? TEXT("OK") : TEXT("FAILED"),
               *Description);

        // Belt and braces: some providers open for delete without unlinking.
        return DeleteFromDisk();
    }

    // Never tracked. Nothing for source control to know about.
    return DeleteFromDisk();
}

FString FMapLinkSourceControl::GetPackageFilename(UPackage* Package)
{
    if (!Package) return FString();

    const FString Ext = Package->ContainsMap()
        ? FPackageName::GetMapPackageExtension()
        : FPackageName::GetAssetPackageExtension();

    FString Filename;
    if (FPackageName::TryConvertLongPackageNameToFilename(
            Package->GetName(), Filename, Ext))
    {
        return FPaths::ConvertRelativePathToFull(Filename);
    }
    return FString();
}

void FMapLinkSourceControl::CheckoutBeforeSave(UPackage* Package)
{
    if (!Package || !ISourceControlModule::Get().IsEnabled()) return;

    const FString Filename = GetPackageFilename(Package);
    if (Filename.IsEmpty() || !FPaths::FileExists(Filename))
    {
        // First save — nothing on disk to check out.  AddAfterSave will
        // mark the new file for add.
        return;
    }

    EnsureFileInChangelist(Filename, SessionDescription);
}

// ---------------------------------------------------------------------------
// v1.13 — Pre-reimport shelve (opt-in safety net around nuke-and-recreate)
// ---------------------------------------------------------------------------

void FMapLinkSourceControl::ShelveReimportSnapshot(
    const TArray<FString>& ChangedBlendPaths)
{
    if (!UMapLinkSettings::Get()->bShelveBeforeReimport)
    {
        return;
    }
    if (!ISourceControlModule::Get().IsEnabled())
    {
        return;
    }
    ISourceControlProvider& SCC = ISourceControlModule::Get().GetProvider();
    if (!SCC.UsesChangelists())
    {
        UE_LOG(LogMapLink, Log,
               TEXT("[Shelve] Provider has no changelist support — skipping "
                    "pre-reimport shelve."));
        return;
    }
    if (ChangedBlendPaths.Num() == 0)
    {
        return;
    }

    const FString Desc = TEXT("[MapLink] Reimport");

    // 1. Collect the files the snapshot must capture.
    TArray<FString> Files;

    IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
        TEXT("AssetRegistry")).Get();

    for (const FString& BlendPath : ChangedBlendPaths)
    {
        const FString Stem = FPaths::GetBaseFilename(BlendPath);
        if (Stem.IsEmpty())
        {
            continue;
        }

        // a. Every /Game/MapLink/<stem>/... package about to be nuked.
        FString Root = FMapLinkUtils::GetMapLinkContentRoot(); // "/Game/MapLink/"
        Root.RemoveFromEnd(TEXT("/"));
        const FString StemPath = Root / Stem;                  // "/Game/MapLink/<stem>"

        TArray<FAssetData> Assets;
        AR.GetAssetsByPath(FName(*StemPath), Assets, /*bRecursive=*/true);
        for (const FAssetData& Asset : Assets)
        {
            FString Filename;
            if (FPackageName::DoesPackageExist(
                    Asset.PackageName.ToString(), &Filename))
            {
                Files.AddUnique(FPaths::ConvertRelativePathToFull(Filename));
            }
        }

        // b. The source .blend and its P4-tracked sidecars — so the shelf
        //    captures the exact .blend versions the assets came from.
        const FString BlendDir = FPaths::GetPath(BlendPath);
        const FString Sidecars[] = {
            BlendPath,
            BlendDir / (Stem + TEXT("_analysis.json")),
            BlendDir / (Stem + TEXT("_filehash.txt")),
        };
        for (const FString& F : Sidecars)
        {
            if (FPaths::FileExists(F))
            {
                Files.AddUnique(FPaths::ConvertRelativePathToFull(F));
            }
        }
    }

    if (Files.Num() == 0)
    {
        UE_LOG(LogMapLink, Log,
               TEXT("[Shelve] Nothing on disk to snapshot — skipping."));
        return;
    }

    // 2. Ensure every file is opened in the "[MapLink] Reimport" CL.
    int32 EnsureFailures = 0;
    for (const FString& F : Files)
    {
        if (!EnsureFileInChangelist(F, Desc))
        {
            ++EnsureFailures;
        }
    }

    FSourceControlChangelistPtr CL = GetOrCreateChangelist(Desc);
    if (!CL.IsValid())
    {
        MapLinkNotify::Warn(NSLOCTEXT("MapLink", "ShelveNoCL",
            "Shelve skipped: could not resolve the [MapLink] Reimport "
            "changelist. Reimport continues."));
        return;
    }

    // 3. Shelve the CL via the provider (FShelve is the supported route in
    //    UE 5.x — the Changelists window uses the same operation).
    TSharedRef<FShelve> ShelveOp = ISourceControlOperation::Create<FShelve>();
    const ECommandResult::Type R = SCC.Execute(
        ShelveOp, CL, TArray<FString>(), EConcurrency::Synchronous);

    if (R == ECommandResult::Succeeded)
    {
        const FString CLId = CL->GetIdentifier();
        UE_LOG(LogMapLink, Log,
               TEXT("[Shelve] Snapshot shelved in CL %s (%d files%s)."),
               *CLId, Files.Num(),
               EnsureFailures > 0 ? TEXT(", some checkouts failed") : TEXT(""));
        MapLinkNotify::Success(FText::Format(
            NSLOCTEXT("MapLink", "ShelveDoneFmt",
                      "Snapshot shelved in CL {0} (assets + .blend)"),
            FText::FromString(CLId)));
    }
    else
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("[Shelve] Shelve of CL failed — reimport continues "
                    "without a snapshot."));
        MapLinkNotify::Warn(NSLOCTEXT("MapLink", "ShelveFailed",
            "Pre-reimport shelve failed (see Output Log). "
            "Reimport continues."));
    }
}

void FMapLinkSourceControl::AddAfterSave(UPackage* Package)
{
    if (!Package || !ISourceControlModule::Get().IsEnabled()) return;

    const FString Filename = GetPackageFilename(Package);
    if (Filename.IsEmpty() || !FPaths::FileExists(Filename))
    {
        return;
    }

    EnsureFileInChangelist(Filename, SessionDescription);
}
