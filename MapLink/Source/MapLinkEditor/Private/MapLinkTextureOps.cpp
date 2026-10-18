// Copyright MapModelers. All Rights Reserved.
//
// v1.16 — Texture operations (see header for the two governing invariants).

#include "MapLinkTextureOps.h"

#include "MapLinkTextureNaming.h"
#include "MapLinkTextureOwnership.h"
#include "MapLinkTexturePolicy.h"
#include "MapLinkTextureScanner.h"
#include "MapLinkTgaWriter.h"
#include "MapLinkEditorState.h"
#include "MapLinkNotify.h"
#include "MapLinkSettings.h"
#include "MapLinkSourceControl.h"
#include "MapLinkUtils.h"
#include "MapLinkPipeline.h"   // LogMapLink

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "EditorAssetLibrary.h"
#include "ObjectTools.h"
#include "EditorFramework/AssetImportData.h"
#include "EditorReimportHandler.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialTypes.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopedSlowTask.h"
#include "RenderingThread.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    /**
     * Batch guard — while non-zero, a per-asset op does NOT flush
     * texture_policy.json and does NOT write a run manifest. The batch owner
     * does both, once, at the end.
     *
     * Measured on a 243-texture Conform All (a 185-asset test project, 2026-08-21): ConformAny
     * calls Promote -> Claim once per texture, and Claim ended with an
     * unconditional FMapLinkTexturePolicy::Save() plus WriteManifest(). That is
     * a full rewrite of a growing JSON file 243 times -- O(n^2) in entries --
     * and 243 separate texture_conform_<timestamp>.json files in the artefact
     * log, one per texture, instead of the one file that describes the run.
     * The log shows the policy going "(161 entries)", "(162 entries)",
     * "(163 entries)" a second and a half apart, each with its own manifest.
     *
     * That is not just waste. The run took over five minutes with the editor
     * pinned in a synchronous loop, and it ended in a driver-level GPU reset
     * (DXGI_ERROR_DEVICE_REMOVED, no breadcrumbs, no page fault, Aftermath
     * unable to start a dump) at texture ~163 of 243. Nothing here caused
     * that -- but time spent in the loop is exposure to it, so cutting the
     * loop's per-item cost is a reliability measure and not only a speed one.
     */
    int32 GOpBatchDepth = 0;

    struct FScopedOpBatch
    {
        FScopedOpBatch()  { ++GOpBatchDepth; }
        ~FScopedOpBatch() { --GOpBatchDepth; }
    };

    bool IsOpBatched() { return GOpBatchDepth > 0; }
}

namespace
{
    IAssetTools& GetAssetTools()
    {
        return FModuleManager::LoadModuleChecked<FAssetToolsModule>(
            TEXT("AssetTools")).Get();
    }

    const IAssetRegistry& GetRegistry()
    {
        return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
            TEXT("AssetRegistry")).Get();
    }

    /** Save a package the way the rest of MapLink does: checkout, save with
     *  errors routed to GLog rather than GError (SavePackage's appError is
     *  FATAL and would take the editor down), then mark for add. */
    bool SaveAssetPackage(UObject* Asset)
    {
        if (!Asset)
        {
            return false;
        }
        UPackage* Package = Asset->GetOutermost();
        if (!Package)
        {
            return false;
        }

        const FString DiskPath = FPackageName::LongPackageNameToFilename(
            Package->GetName(), FPackageName::GetAssetPackageExtension());

        FMapLinkSourceControl::CheckoutBeforeSave(Package);

        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        SaveArgs.Error         = GLog;
        SaveArgs.SaveFlags     = SAVE_NoError;

        const bool bSaved = UPackage::SavePackage(Package, Asset, *DiskPath, SaveArgs);
        if (bSaved)
        {
            FMapLinkSourceControl::AddAfterSave(Package);
        }
        else
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("[Textures] SavePackage failed for '%s'."), *DiskPath);
        }
        return bSaved;
    }

    UTexture2D* LoadTexture(const FString& AssetPath)
    {
        return Cast<UTexture2D>(
            StaticLoadObject(UTexture2D::StaticClass(), nullptr,
                             *FMapLinkTextureNaming::NormalizeAssetPath(AssetPath)));
    }

    /**
     * EnsureFileInChangelist, but only for a file that is actually on disk.
     *
     * p4 has no verb for "reserve a path that does not exist yet". Asking it to
     * open a missing file produces
     *
     *     Failed mark the file ... for add. The file doesn't exist on disk.
     *
     * in red, with no changelist entry to show for it — which reads like the
     * operation failed when the operation had not started. Call this BEFORE a
     * write (checks out an existing file so the write can land) and AGAIN after
     * a successful one (marks a brand-new file for add). The second call is a
     * no-op when the first already opened it.
     *
     * Same guard MapLinkTexturePolicy::Save already carries; this is that fix
     * applied to the other three write sites.
     */
    void OpenInChangelistIfExists(const FString& AbsolutePath)
    {
        if (!AbsolutePath.IsEmpty() && FPaths::FileExists(AbsolutePath))
        {
            FMapLinkSourceControl::EnsureFileInChangelist(
                AbsolutePath, FMapLinkTextureOps::GetChangelistDescription());
        }
    }

    /** Every asset that references this texture, found through the
     *  AssetRegistry so nothing has to already be in memory. */
    void CollectReferencingAssets(const UTexture2D* Texture,
                                  TArray<FAssetData>& OutReferencers)
    {
        OutReferencers.Reset();
        if (!Texture)
        {
            return;
        }

        const FName PackageName = Texture->GetOutermost()->GetFName();

        TArray<FName> ReferencingPackages;
        GetRegistry().GetReferencers(PackageName, ReferencingPackages,
                                     UE::AssetRegistry::EDependencyCategory::Package);

        for (const FName& Package : ReferencingPackages)
        {
            TArray<FAssetData> InPackage;
            GetRegistry().GetAssetsByPackageName(Package, InPackage);
            OutReferencers.Append(InPackage);
        }
    }

    /** Swap every texture parameter on this MI that points at From, to To.
     *  Returns how many parameters changed. */
    int32 RepointMaterialInstance(UMaterialInstanceConstant* MI,
                                  UTexture2D* From, UTexture2D* To)
    {
        if (!MI || !From || !To)
        {
            return 0;
        }

        TArray<FMaterialParameterInfo> ParamInfos;
        TArray<FGuid> ParamGuids;
        MI->GetAllTextureParameterInfo(ParamInfos, ParamGuids);

        int32 Changed = 0;
        for (const FMaterialParameterInfo& Info : ParamInfos)
        {
            UTexture* Value = nullptr;
            if (!MI->GetTextureParameterValue(Info, Value, /*bOveriddenOnly=*/true))
            {
                continue;
            }
            if (Value != From)
            {
                continue;
            }
            MI->SetTextureParameterValueEditorOnly(Info, To);
            ++Changed;
        }

        if (Changed > 0)
        {
            // PostEditChange() alone is NOT enough, and the difference is
            // invisible until you read the saved package.
            //
            // UMaterialInstance keeps CachedReferencedTextures, a serialised
            // list rebuilt by UpdateCachedData(). SetTextureParameterValueEditorOnly
            // changes the parameter; a bare PostEditChange() does not
            // necessarily rebuild that cache, so the OLD texture stays in it,
            // gets written into the package's import table, and remains a
            // package-level dependency of this MI forever.
            //
            // Measured 2026-08-24, MI_Opq_StoneWall2.uasset after a clean
            // repoint of two parameters — the import table held both:
            //     /Game/Maps/Textures/090654_baseTexBaked
            //     /Game/MapLink/Textures/T_Mk_BaseTexBaked
            //
            // Everything downstream reads that dependency and believes it:
            // the scan lists the original as still referenced (243 local-only
            // went to 242 after conforming EIGHT), and Clean Up refuses to
            // touch it because GetReferencers() is non-empty. Two "the tool is
            // broken" bugs, one stale array.
            //
            // Routing through PostEditChangeProperty with the actual property
            // is what UMaterialInstance listens for.
            if (FProperty* Prop = UMaterialInstance::StaticClass()
                    ->FindPropertyByName(GET_MEMBER_NAME_CHECKED(
                        UMaterialInstance, TextureParameterValues)))
            {
                FPropertyChangedEvent Event(Prop, EPropertyChangeType::ValueSet);
                MI->PostEditChangeProperty(Event);
            }
            else
            {
                MI->PostEditChange();
            }
            MI->MarkPackageDirty();

            // Say so if it did not take. A silent stale dependency is what
            // made this cost two days; a line in the log costs nothing.
            for (const FMaterialParameterInfo& Info : ParamInfos)
            {
                UTexture* Check = nullptr;
                if (MI->GetTextureParameterValue(Info, Check, true)
                    && Check == From)
                {
                    UE_LOG(LogMapLink, Warning,
                           TEXT("[Textures] '%s' still points at '%s' after "
                                "repointing. The original will stay referenced "
                                "and Clean Up will refuse it."),
                           *MI->GetPathName(), *From->GetName());
                }
            }
        }
        return Changed;
    }

    FString TimestampForFilename()
    {
        return FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S"));
    }
}

const TCHAR* FMapLinkTextureOps::GetChangelistDescription()
{
    return TEXT("[MapLink] Texture Conform");
}

bool FMapLinkTextureOps::IsDestructive(EMapLinkTextureClaimPreset Preset)
{
    return Preset == EMapLinkTextureClaimPreset::Migrate
        || Preset == EMapLinkTextureClaimPreset::Adopt;
}

// ---------------------------------------------------------------------------
// Conform — §6.1
// ---------------------------------------------------------------------------

FMapLinkTextureOpReport FMapLinkTextureOps::Conform(const TArray<FString>& AssetPaths,
                                                    bool bExecute)
{
    const double Started = FPlatformTime::Seconds();

    FMapLinkTextureOpReport Report;
    Report.OperationName = TEXT("Conform");
    Report.Preset        = TEXT("-");
    Report.bExecuted     = bExecute;

    const UMapLinkSettings* Settings = UMapLinkSettings::Get();
    const bool bAllowReduction = Settings ? Settings->bAllowBitDepthReduction : true;

    FMapLinkTexturePolicy::EnsureLoaded();

    // Names already spoken for, so a batch cannot hand two textures the same
    // one. Seeded from what is on disk, not just from this batch.
    TSet<FString> TakenNames;
    {
        const FString TexturesDir = FMapLinkUtils::GetTextureSourceDir();
        if (!TexturesDir.IsEmpty())
        {
            TArray<FString> Existing;
            IFileManager::Get().FindFiles(Existing, *(TexturesDir / TEXT("*.tga")),
                                          /*Files=*/true, /*Directories=*/false);
            for (const FString& File : Existing)
            {
                TakenNames.Add(FPaths::GetBaseFilename(File));
            }
        }
    }

    for (const FString& RawAssetPath : AssetPaths)
    {
        // The policy is keyed by object path. A path typed at the console, or
        // dragged from the content browser, is usually a package path — and
        // looking one up in the other silently reports SkippedUnclassified for
        // a texture that is classified. Normalise once, here, and every lookup
        // downstream is against the same spelling.
        const FString AssetPath =
            FMapLinkTextureNaming::NormalizeAssetPath(RawAssetPath);

        FMapLinkTextureOpEntry Entry;
        Entry.AssetPathBefore = AssetPath;
        Entry.AssetPathAfter  = AssetPath;

        // v1.16 — effective, not recorded: an unrecorded texture takes its
        // class from its folder rather than blocking on a decision nobody has
        // been asked to make.
        const EMapLinkTextureClass Class =
            FMapLinkTextureScanner::EffectiveClass(AssetPath);
        Entry.TextureClass = Class;

        if (Class == EMapLinkTextureClass::Unclassified)
        {
            Entry.Result = EMapLinkTextureResult::SkippedUnclassified;
            Entry.Warnings.Add(TEXT("no class decision recorded — classify it first"));
            Report.Entries.Add(MoveTemp(Entry));
            ++Report.NumSkipped;
            continue;
        }
        if (Class == EMapLinkTextureClass::External)
        {
            // External originals are read-only to MapLink under every
            // operation, without exception. Not a failure — a correct refusal.
            Entry.Result = EMapLinkTextureResult::SkippedExternal;
            Entry.Warnings.Add(TEXT("External class — MapLink never writes the "
                                    "original. Promote it first if you want "
                                    "MapLink to own it."));
            Report.Entries.Add(MoveTemp(Entry));
            ++Report.NumSkipped;
            continue;
        }

        UTexture2D* Texture = LoadTexture(AssetPath);
        if (!Texture)
        {
            Entry.Result = EMapLinkTextureResult::Failed;
            Entry.Errors.Add(TEXT("the asset could not be loaded"));
            Report.Entries.Add(MoveTemp(Entry));
            ++Report.NumFailed;
            continue;
        }

        Entry.DiskPathBefore = Texture->AssetImportData
            ? FPaths::ConvertRelativePathToFull(
                  Texture->AssetImportData->GetFirstFilename())
            : FString();

        // ── Target name and path ──────────────────────────────────────────
        const FString CurrentName = Texture->GetName();
        FString TargetName = CurrentName;

        if (!FMapLinkTextureNaming::IsMapLinkAssetName(CurrentName))
        {
            const FString Stem = FMapLinkTextureNaming::DeriveStem(CurrentName);
            if (Stem.IsEmpty())
            {
                Entry.Result = EMapLinkTextureResult::Failed;
                Entry.Errors.Add(FString::Printf(
                    TEXT("nothing usable survives sanitising '%s' into a stem"),
                    *CurrentName));
                Report.Entries.Add(MoveTemp(Entry));
                ++Report.NumFailed;
                continue;
            }

            const FString Desired = FMapLinkTextureNaming::MakeAssetName(Stem);
            if (!FMapLinkTextureNaming::ResolveCollision(Desired, TakenNames, TargetName))
            {
                // Never silently merge two textures into one name.
                Entry.Result = EMapLinkTextureResult::Failed;
                Entry.Errors.Add(FString::Printf(
                    TEXT("'%s' collides and all 99 numbered variants are taken"),
                    *Desired));
                Report.Entries.Add(MoveTemp(Entry));
                ++Report.NumFailed;
                continue;
            }
        }
        else
        {
            TakenNames.Add(CurrentName);
        }

        const FString TargetDiskPath =
            FMapLinkTextureNaming::ComposeSourcePath(TargetName);
        if (TargetDiskPath.IsEmpty())
        {
            Entry.Result = EMapLinkTextureResult::Failed;
            Entry.Errors.Add(TEXT("no MapLink project root is configured, so "
                                  "there is nowhere to write the source file"));
            Report.Entries.Add(MoveTemp(Entry));
            ++Report.NumFailed;
            continue;
        }
        Entry.DiskPathAfter = TargetDiskPath;

        {
            FString Reason;
            if (!FMapLinkTextureNaming::ValidateLengths(TargetName, TargetDiskPath, Reason))
            {
                Entry.Result = EMapLinkTextureResult::Failed;
                Entry.Errors.Add(Reason);
                Report.Entries.Add(MoveTemp(Entry));
                ++Report.NumFailed;
                continue;
            }
        }

        const FString TargetPackagePath = FMapLinkTextureNaming::GetTexturesPackagePath();
        const FString CurrentPackagePath = Texture->GetOutermost()->GetName();
        const FString CurrentDir = FPackageName::GetLongPackagePath(CurrentPackagePath);

        const bool bNeedsRename = (TargetName != CurrentName)
                               || (CurrentDir != TargetPackagePath);
        Entry.bRenamed = bNeedsRename;
        Entry.AssetPathAfter = FMapLinkTextureNaming::ComposeAssetPath(TargetName)
                             + TEXT(".") + TargetName;

        // ── Dry run stops here, having computed everything ────────────────
        if (!bExecute)
        {
            FMapLinkTextureSurface Surface;
            FString ExtractError;
            if (!FMapLinkTgaWriter::ExtractSurface(Texture, bAllowReduction,
                                                   Surface, ExtractError))
            {
                Entry.Result = EMapLinkTextureResult::SkippedNoSourceData;
                Entry.Warnings.Add(ExtractError);
                Report.Entries.Add(MoveTemp(Entry));
                ++Report.NumSkipped;
                continue;
            }

            Entry.bLossyReduction = Surface.bReducedBitDepth;
            if (Surface.bHasAlphaChannel && !Surface.bAlphaUniform)
            {
                Entry.Result = EMapLinkTextureResult::AlphaSplit;
                Entry.AlphaAssetPath = FMapLinkTextureNaming::ComposeAssetPath(
                    TargetName + TEXT("_A"));
                Entry.ActionItem = FString::Printf(
                    TEXT("The parent material needs a new texture parameter for "
                         "'%s_A'. MapLink extracts the alpha but must not author "
                         "material parameters."), *TargetName);
                ++Report.NumAlphaSplit;
            }
            else
            {
                Entry.Result = bNeedsRename ? EMapLinkTextureResult::Conformed
                                            : EMapLinkTextureResult::SkippedAlreadyConform;
            }

            if (Entry.bLossyReduction)
            {
                ++Report.NumLossy;
                Entry.Warnings.Add(TEXT("source is above 8 bits per channel and "
                                        "would be reduced — height maps terrace"));
            }
            Report.Entries.Add(MoveTemp(Entry));
            continue;
        }

        // ── Execute ───────────────────────────────────────────────────────

        if (bNeedsRename)
        {
            // UEditorAssetLibrary rather than IAssetTools::RenameAssets:
            // FAssetRenameData lives in a header AssetTools does not expose
            // publicly in 5.7. This takes long package names, leaves a
            // redirector behind exactly the same way, and its module
            // (EditorScriptingUtilities) is already a dependency.
            const FString SourcePackage = CurrentPackagePath;
            const FString DestPackage   = TargetPackagePath / TargetName;

            if (!UEditorAssetLibrary::RenameAsset(SourcePackage, DestPackage))
            {
                Entry.Result = EMapLinkTextureResult::Failed;
                Entry.Errors.Add(FString::Printf(
                    TEXT("could not rename %s to %s — is it checked out?"),
                    *SourcePackage, *DestPackage));
                Report.Entries.Add(MoveTemp(Entry));
                ++Report.NumFailed;
                continue;
            }

            // The rename moves the object in place, so the pointer stays
            // valid — but re-resolve defensively rather than assume it.
            if (UTexture2D* Renamed = LoadTexture(DestPackage + TEXT(".") + TargetName))
            {
                Texture = Renamed;
            }
        }

        // Check the destination out BEFORE the writer needs to replace it —
        // but only if it is there to check out. A first conform is creating
        // this file, and p4 cannot open what does not exist yet.
        OpenInChangelistIfExists(TargetDiskPath);

        const FMapLinkTgaWriteResult WriteResult =
            FMapLinkTgaWriter::WriteFromTexture(Texture, TargetDiskPath, bAllowReduction);

        if (!WriteResult.bSuccess)
        {
            Entry.Result = WriteResult.Error.Contains(TEXT("no editor source data"))
                ? EMapLinkTextureResult::SkippedNoSourceData
                : EMapLinkTextureResult::Failed;
            Entry.Errors.Add(WriteResult.Error);
            if (Entry.Result == EMapLinkTextureResult::Failed) { ++Report.NumFailed; }
            else                                               { ++Report.NumSkipped; }
            Report.Entries.Add(MoveTemp(Entry));
            continue;
        }

        // Now it exists — mark a brand-new file for add. A no-op when the call
        // above already opened it.
        OpenInChangelistIfExists(TargetDiskPath);

        Entry.bSkippedIdentical = WriteResult.bSkippedIdentical;
        Entry.bLossyReduction   = WriteResult.bReducedBitDepth;
        if (!WriteResult.bSkippedIdentical)
        {
            ++Report.NumWrites;
        }
        if (WriteResult.bDroppedUniformAlpha)
        {
            Entry.Warnings.Add(TEXT("alpha channel was uniform and was dropped"));
        }
        if (WriteResult.bGrayscaleCandidate)
        {
            Entry.Warnings.Add(TEXT("all three channels are identical — a "
                                    "grayscale candidate, not converted because "
                                    "that is a compression-settings decision"));
        }
        if (WriteResult.bDiscardedColourChannels)
        {
            Entry.Warnings.Add(TEXT("compression says grayscale but the channels "
                                    "differ; G and B were discarded"));
        }

        // ── Old file cleanup when the extension changed ───────────────────
        //
        // A PNG/PSD/TIF source becomes <basename>.tga in the same directory.
        // p4 move when the old file is tracked, otherwise write-then-delete —
        // which is what already happened, so this is just the delete.
        if (!Entry.DiskPathBefore.IsEmpty()
            && !Entry.DiskPathBefore.Equals(TargetDiskPath, ESearchCase::IgnoreCase)
            && FPaths::FileExists(Entry.DiskPathBefore))
        {
            const FString OldDir = FPaths::GetPath(Entry.DiskPathBefore);
            const FString NewDir = FPaths::GetPath(TargetDiskPath);
            const bool bSameDir = OldDir.Equals(NewDir, ESearchCase::IgnoreCase);

            if (bSameDir)
            {
                if (FMapLinkSourceControl::DeleteFileTracked(
                        Entry.DiskPathBefore, GetChangelistDescription()))
                {
                    Entry.Warnings.Add(FString::Printf(
                        TEXT("replaced %s"),
                        *FPaths::GetCleanFilename(Entry.DiskPathBefore)));
                }
            }
            else
            {
                // Outside MapLink's own folder — never delete somebody else's
                // file just because we made a copy of it.
                Entry.Warnings.Add(FString::Printf(
                    TEXT("previous source %s is outside MapLink's Textures "
                         "folder and was left alone"), *Entry.DiskPathBefore));
            }
        }

        // ── Is there anything left to do? ─────────────────────────────────
        //
        // Byte-identical TGA, no rename, and the asset already points at it:
        // this texture was conform before the command ran. Reimporting and
        // re-saving would rewrite the .uasset and open it in P4 for a run that
        // wrote zero bytes — a diff with no content, which is exactly the churn
        // the idempotence check exists to prevent. Report it and move on.
        const bool bImportPathAlreadyCorrect =
            Texture->AssetImportData
            && FPaths::ConvertRelativePathToFull(
                   Texture->AssetImportData->GetFirstFilename())
               .Equals(TargetDiskPath, ESearchCase::IgnoreCase);

        const bool bNothingChanged = WriteResult.bSkippedIdentical
                                  && !bNeedsRename
                                  && bImportPathAlreadyCorrect;

        if (bNothingChanged)
        {
            // Settings are READ from the asset every pass, so recording them is
            // free of history and cannot dirty a file that already agrees. The
            // conform STAMP is deliberately not touched: re-stamping a no-op
            // would rewrite texture_policy.json on every scan-and-conform.
            FMapLinkTexturePolicy::RecordAssetSettings(
                Entry.AssetPathAfter, Texture->SRGB,
                MapLinkTexture::CompressionToString(Texture->CompressionSettings));

            Entry.Result            = EMapLinkTextureResult::SkippedAlreadyConform;
            Entry.bSkippedIdentical = true;
            if (Entry.bLossyReduction) { ++Report.NumLossy; }
            ++Report.NumSkipped;
            Report.Entries.Add(MoveTemp(Entry));
            continue;
        }

        // ── Point the asset at the file MapLink owns, then reimport ───────
        if (Texture->AssetImportData)
        {
            Texture->AssetImportData->Update(TargetDiskPath);
        }

        // Reimport makes the TGA authoritative and stops UE from asking about
        // a changed source file forever after.
        //
        // NOTE the consequence when a reduction happened: the asset's own
        // source data becomes the 8-bit TGA, so a 16-bit original degrades in
        // UE too. That is what lossy_reduction on the policy entry is warning
        // about, and it is why bAllowBitDepthReduction exists.
        if (WriteResult.bReducedBitDepth)
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("[Textures] '%s' was reduced to 8 bits on the way to "
                        "TGA, and the reimport makes that reduction the asset's "
                        "own source data. Turn off Allow Bit-Depth Reduction in "
                        "MapLink settings if this texture's depth matters."),
                   *TargetName);
        }
        FReimportManager::Instance()->Reimport(
            Texture, /*bAskForNewFileIfMissing=*/false,
            /*bShowNotification=*/false, TargetDiskPath);

        SaveAssetPackage(Texture);

        // ── Alpha split — §5.2 ────────────────────────────────────────────
        if (WriteResult.bHasNonUniformAlpha)
        {
            FMapLinkTextureSurface Surface;
            FString ExtractError;
            if (FMapLinkTgaWriter::ExtractSurface(Texture, bAllowReduction,
                                                  Surface, ExtractError))
            {
                FMapLinkTextureSurface AlphaSurface;
                if (FMapLinkTgaWriter::ExtractAlphaSurface(Surface, AlphaSurface))
                {
                    const FString AlphaName = TargetName + TEXT("_A");
                    const FString AlphaDiskPath =
                        FMapLinkTextureNaming::ComposeSourcePath(AlphaName);

                    OpenInChangelistIfExists(AlphaDiskPath);

                    const FMapLinkTgaWriteResult AlphaWrite =
                        FMapLinkTgaWriter::WriteSurface(AlphaSurface, AlphaDiskPath,
                                                        /*bForceGrayscale=*/true);
                    if (AlphaWrite.bSuccess)
                    {
                        OpenInChangelistIfExists(AlphaDiskPath);

                        TArray<FString> Files;
                        Files.Add(AlphaDiskPath);
                        TArray<UObject*> Imported = GetAssetTools().ImportAssets(
                            Files, FMapLinkTextureNaming::GetTexturesPackagePath());

                        if (Imported.Num() > 0)
                        {
                            if (UTexture2D* AlphaTexture = Cast<UTexture2D>(Imported[0]))
                            {
                                // A mask is never colour data. Setting this on a
                                // texture MapLink just created is authoring, but
                                // it is authoring the only correct answer.
                                AlphaTexture->SRGB = false;
                                AlphaTexture->CompressionSettings = TC_Grayscale;
                                AlphaTexture->PostEditChange();
                                SaveAssetPackage(AlphaTexture);

                                Entry.AlphaAssetPath = AlphaTexture->GetPathName();
                                FMapLinkTexturePolicy::SetClass(
                                    Entry.AlphaAssetPath, EMapLinkTextureClass::MapLink);
                            }
                        }
                        ++Report.NumWrites;
                    }

                    Entry.ActionItem = FString::Printf(
                        TEXT("The parent material needs a new texture parameter "
                             "for '%s'. MapLink extracted the alpha but must not "
                             "author material parameters."), *AlphaName);
                    ++Report.NumAlphaSplit;
                }
            }
        }

        // ── Policy and cache ──────────────────────────────────────────────
        FMapLinkTexturePolicy::StampConform(
            Entry.AssetPathAfter, TEXT("Conform"), WriteResult.bReducedBitDepth);
        FMapLinkTexturePolicy::RecordAssetSettings(
            Entry.AssetPathAfter, Texture->SRGB,
            MapLinkTexture::CompressionToString(Texture->CompressionSettings));
        if (bNeedsRename)
        {
            FMapLinkTexturePolicy::SetClass(Entry.AssetPathAfter,
                                            EMapLinkTextureClass::MapLink);
            FMapLinkTexturePolicy::Remove(AssetPath);
        }

        if (UMapLinkEditorState* State = UMapLinkEditorState::Get())
        {
            State->RemoveTextureCache(AssetPath);
            State->RemoveTextureCache(Entry.AssetPathAfter);
        }

        if (Entry.bLossyReduction) { ++Report.NumLossy; }

        Entry.Result = WriteResult.bSkippedIdentical && !bNeedsRename
            ? EMapLinkTextureResult::SkippedAlreadyConform
            : (WriteResult.bReducedBitDepth ? EMapLinkTextureResult::ReducedLossy
                                            : EMapLinkTextureResult::Conformed);
        if (Entry.Result == EMapLinkTextureResult::SkippedAlreadyConform)
        {
            ++Report.NumSkipped;
        }
        else
        {
            ++Report.NumConformed;
        }

        Report.Entries.Add(MoveTemp(Entry));
    }

    if (bExecute)
    {
        FMapLinkTexturePolicy::Save();
        if (UMapLinkEditorState* State = UMapLinkEditorState::Get())
        {
            State->Save();
        }
    }

    Report.RunSeconds = FPlatformTime::Seconds() - Started;
    WriteManifest(Report);
    return Report;
}

FMapLinkTextureOpReport FMapLinkTextureOps::ConformAll(bool bExecute)
{
    const FMapLinkTextureScanResult& Scan = FMapLinkTextureScanner::HasResult()
        ? FMapLinkTextureScanner::GetLastResult()
        : FMapLinkTextureScanner::Scan();

    TArray<FString> Paths;
    for (const FMapLinkTextureRow& Row : Scan.Rows)
    {
        if (Row.TextureClass == EMapLinkTextureClass::MapLink)
        {
            Paths.Add(Row.AssetPath);
        }
    }
    return Conform(Paths, bExecute);
}

// ---------------------------------------------------------------------------
// Claim — §6.2
// ---------------------------------------------------------------------------

FMapLinkTextureOpReport FMapLinkTextureOps::Claim(const FString& RawAssetPath,
                                                  EMapLinkTextureClaimPreset Preset,
                                                  bool bExecute)
{
    const double Started = FPlatformTime::Seconds();

    // Package path in, object path out — see the note in Conform.
    const FString AssetPath =
        FMapLinkTextureNaming::NormalizeAssetPath(RawAssetPath);

    FMapLinkTextureOpReport Report;
    Report.OperationName = TEXT("Claim");
    Report.Preset        = MapLinkTexture::ToString(Preset);
    Report.bExecuted     = bExecute;

    FMapLinkTextureOpEntry Entry;
    Entry.AssetPathBefore = AssetPath;

    UTexture2D* Original = LoadTexture(AssetPath);
    if (!Original)
    {
        Entry.Result = EMapLinkTextureResult::Failed;
        Entry.Errors.Add(TEXT("the asset could not be loaded"));
        Report.Entries.Add(MoveTemp(Entry));
        ++Report.NumFailed;
        Report.RunSeconds = FPlatformTime::Seconds() - Started;
        WriteManifest(Report);
        return Report;
    }

    Entry.TextureClass = FMapLinkTexturePolicy::GetClass(AssetPath);
    Entry.DiskPathBefore = Original->AssetImportData
        ? FPaths::ConvertRelativePathToFull(
              Original->AssetImportData->GetFirstFilename())
        : FString();

    // ── Target name ───────────────────────────────────────────────────────
    const FString Stem = FMapLinkTextureNaming::DeriveStem(Original->GetName());
    if (Stem.IsEmpty())
    {
        Entry.Result = EMapLinkTextureResult::Failed;
        Entry.Errors.Add(FString::Printf(
            TEXT("nothing usable survives sanitising '%s' into a stem"),
            *Original->GetName()));
        Report.Entries.Add(MoveTemp(Entry));
        ++Report.NumFailed;
        Report.RunSeconds = FPlatformTime::Seconds() - Started;
        WriteManifest(Report);
        return Report;
    }

    TSet<FString> TakenNames;
    {
        FARFilter Filter;
        Filter.bRecursivePaths = false;
        Filter.ClassPaths.Add(UTexture2D::StaticClass()->GetClassPathName());
        Filter.PackagePaths.Add(*FMapLinkTextureNaming::GetTexturesPackagePath());
        TArray<FAssetData> Existing;
        GetRegistry().GetAssets(Filter, Existing);
        for (const FAssetData& Data : Existing)
        {
            TakenNames.Add(Data.AssetName.ToString());
        }
    }

    FString TargetName;
    if (!FMapLinkTextureNaming::ResolveCollision(
            FMapLinkTextureNaming::MakeAssetName(Stem), TakenNames, TargetName))
    {
        Entry.Result = EMapLinkTextureResult::Failed;
        Entry.Errors.Add(TEXT("name collides and all 99 numbered variants are taken"));
        Report.Entries.Add(MoveTemp(Entry));
        ++Report.NumFailed;
        Report.RunSeconds = FPlatformTime::Seconds() - Started;
        WriteManifest(Report);
        return Report;
    }

    const FString TargetPackagePath = FMapLinkTextureNaming::GetTexturesPackagePath();
    const FString TargetDiskPath    = FMapLinkTextureNaming::ComposeSourcePath(TargetName);
    Entry.AssetPathAfter = TargetPackagePath / TargetName + TEXT(".") + TargetName;
    Entry.DiskPathAfter  = TargetDiskPath;

    {
        FString Reason;
        if (TargetDiskPath.IsEmpty()
            || !FMapLinkTextureNaming::ValidateLengths(TargetName, TargetDiskPath, Reason))
        {
            Entry.Result = EMapLinkTextureResult::Failed;
            Entry.Errors.Add(TargetDiskPath.IsEmpty()
                ? TEXT("no MapLink project root is configured")
                : *Reason);
            Report.Entries.Add(MoveTemp(Entry));
            ++Report.NumFailed;
            Report.RunSeconds = FPlatformTime::Seconds() - Started;
            WriteManifest(Report);
            return Report;
        }
    }

    // ── Referencer analysis, done for every preset so a dry run is honest ─
    TArray<FAssetData> Referencers;
    CollectReferencingAssets(Original, Referencers);

    // Four buckets, not two. The first real run reported "would repoint 7" and
    // then repointed 4, because the collection pass counted every MapLink-owned
    // UMaterialInterface and the execute pass only handled the ones that load
    // as UMaterialInstanceConstant. The other three were parent UMaterials —
    // /MapLink/Materials/M_OpqDsp_Simple and siblings — which produced
    // "Failed to find object 'MaterialInstanceConstant /MapLink/...'" warnings
    // and were then silently dropped.
    //
    // A parent UMaterial holds its texture in the material graph as a parameter
    // DEFAULT, not as an instance override. There is no SetTextureParameter
    // that reaches it, and it is shipped plugin content besides. So it is not a
    // repoint that failed; it is a referencer that cannot be repointed, and the
    // dry run has to say so up front.
    TArray<FString> RepointableMaterials;   // MapLink-owned MICs
    TArray<FString> LockedParentMaterials;  // MapLink-owned parent UMaterials
    TArray<FString> ForeignMaterials;       // not MapLink-owned at all
    TArray<FString> OtherReferencers;       // blueprints, levels, data assets

    for (const FAssetData& Data : Referencers)
    {
        UObject* Asset = Data.GetAsset();
        if (!Asset || Asset == Original)
        {
            continue;
        }

        if (UMaterialInterface* Material = Cast<UMaterialInterface>(Asset))
        {
            if (!FMapLinkTextureOwnership::IsMapLinkMaterial(Material))
            {
                ForeignMaterials.Add(Data.GetObjectPathString());
            }
            else if (Cast<UMaterialInstanceConstant>(Asset) != nullptr)
            {
                RepointableMaterials.Add(Data.GetObjectPathString());
            }
            else
            {
                LockedParentMaterials.Add(Data.GetObjectPathString());
            }
        }
        else
        {
            OtherReferencers.Add(Data.GetObjectPathString());
        }
    }

    const bool bDeletesOriginalAsset = IsDestructive(Preset);

    // THE PLACE THE TWO INVARIANTS MEET. A referencer that cannot be repointed
    // blocks a destructive preset, because deleting the original would dangle
    // it. Two different reasons land here and both are refusals, not bugs:
    //
    //   foreign material  — MapLink is forbidden from modifying it
    //   parent UMaterial  — there is no mechanism to modify it
    if (bDeletesOriginalAsset
        && (ForeignMaterials.Num() > 0 || LockedParentMaterials.Num() > 0))
    {
        Entry.Result = EMapLinkTextureResult::Failed;
        Entry.Errors.Add(FString::Printf(
            TEXT("%d referencing material(s) cannot be repointed, so the "
                 "original cannot be deleted without dangling. Use Fork or "
                 "Copy instead."),
            ForeignMaterials.Num() + LockedParentMaterials.Num()));
        for (const FString& Foreign : ForeignMaterials)
        {
            Entry.Errors.Add(FString::Printf(
                TEXT("  blocked by %s — not a MapLink material, MapLink never "
                     "modifies it"), *Foreign));
        }
        for (const FString& Parent : LockedParentMaterials)
        {
            Entry.Errors.Add(FString::Printf(
                TEXT("  blocked by %s — a parent material; its texture is a "
                     "graph default, not an instance override"), *Parent));
        }
        Report.Entries.Add(MoveTemp(Entry));
        ++Report.NumFailed;
        Report.RunSeconds = FPlatformTime::Seconds() - Started;
        WriteManifest(Report);
        return Report;
    }

    // ── Dry run ───────────────────────────────────────────────────────────
    if (!bExecute)
    {
        Entry.Result = EMapLinkTextureResult::Conformed;
        Entry.RepointedMaterials = (Preset == EMapLinkTextureClaimPreset::Copy)
            ? TArray<FString>()
            : RepointableMaterials;

        // Named, not subtracted. "would repoint 4" with three parents listed as
        // untouched is a true statement; "would repoint 7" followed by 4 is not.
        if (Preset != EMapLinkTextureClaimPreset::Copy
            && LockedParentMaterials.Num() > 0)
        {
            Entry.Warnings.Add(FString::Printf(
                TEXT("%d parent material(s) also reference this texture and "
                     "will keep pointing at the original — a parent's texture "
                     "is a graph default and cannot be repointed:"),
                LockedParentMaterials.Num()));
            for (const FString& Parent : LockedParentMaterials)
            {
                Entry.Warnings.Add(FString::Printf(TEXT("  %s"), *Parent));
            }
        }

        if (bDeletesOriginalAsset && OtherReferencers.Num() > 0)
        {
            Entry.Warnings.Add(FString::Printf(
                TEXT("%d non-material referencer(s) would be repointed"),
                OtherReferencers.Num()));
        }
        if (Preset == EMapLinkTextureClaimPreset::Migrate
            && !Entry.DiskPathBefore.IsEmpty())
        {
            Entry.Warnings.Add(FString::Printf(
                TEXT("would DELETE the original file %s"), *Entry.DiskPathBefore));
        }
        if (bDeletesOriginalAsset)
        {
            Entry.Warnings.Add(FString::Printf(
                TEXT("would DELETE the original asset %s"), *AssetPath));
        }

        ++Report.NumConformed;
        Report.Entries.Add(MoveTemp(Entry));
        Report.RunSeconds = FPlatformTime::Seconds() - Started;
        WriteManifest(Report);
        return Report;
    }

    // ── Execute: duplicate ────────────────────────────────────────────────
    //
    // Duplicate rather than create-and-import: it carries sRGB, compression,
    // LOD group, address modes and everything else across untouched. Those are
    // settings somebody chose, and conformance has no business changing them.
    UObject* DuplicatedObject =
        GetAssetTools().DuplicateAsset(TargetName, TargetPackagePath, Original);
    UTexture2D* Claimed = Cast<UTexture2D>(DuplicatedObject);
    if (!Claimed)
    {
        Entry.Result = EMapLinkTextureResult::Failed;
        Entry.Errors.Add(FString::Printf(
            TEXT("could not duplicate into %s/%s"), *TargetPackagePath, *TargetName));
        Report.Entries.Add(MoveTemp(Entry));
        ++Report.NumFailed;
        Report.RunSeconds = FPlatformTime::Seconds() - Started;
        WriteManifest(Report);
        return Report;
    }
    Entry.AssetPathAfter = Claimed->GetPathName();

    const UMapLinkSettings* Settings = UMapLinkSettings::Get();
    const bool bAllowReduction = Settings ? Settings->bAllowBitDepthReduction : true;

    OpenInChangelistIfExists(TargetDiskPath);

    const FMapLinkTgaWriteResult WriteResult =
        FMapLinkTgaWriter::WriteFromTexture(Claimed, TargetDiskPath, bAllowReduction);

    OpenInChangelistIfExists(TargetDiskPath);

    if (!WriteResult.bSuccess)
    {
        Entry.Result = EMapLinkTextureResult::Failed;
        Entry.Errors.Add(WriteResult.Error);
        Report.Entries.Add(MoveTemp(Entry));
        ++Report.NumFailed;
        Report.RunSeconds = FPlatformTime::Seconds() - Started;
        WriteManifest(Report);
        return Report;
    }

    if (!WriteResult.bSkippedIdentical) { ++Report.NumWrites; }
    Entry.bLossyReduction = WriteResult.bReducedBitDepth;
    if (Entry.bLossyReduction) { ++Report.NumLossy; }

    if (Claimed->AssetImportData)
    {
        Claimed->AssetImportData->Update(TargetDiskPath);
    }
    FReimportManager::Instance()->Reimport(
        Claimed, false, false, TargetDiskPath);
    SaveAssetPackage(Claimed);

    // ── Repoint ───────────────────────────────────────────────────────────
    if (Preset != EMapLinkTextureClaimPreset::Copy)
    {
        for (const FString& MaterialPath : RepointableMaterials)
        {
            // Load as UObject and cast. Loading through
            // UMaterialInstanceConstant::StaticClass() was what emitted
            // "Failed to find object 'MaterialInstanceConstant /MapLink/...'"
            // for anything that was not one — a warning about a mismatch the
            // partition above has already handled.
            UMaterialInstanceConstant* MI = Cast<UMaterialInstanceConstant>(
                StaticLoadObject(UObject::StaticClass(), nullptr, *MaterialPath));
            if (!MI)
            {
                Entry.Warnings.Add(FString::Printf(
                    TEXT("%s could not be loaded as a material instance and was "
                         "left pointing at the original"), *MaterialPath));
                continue;
            }
            // The ownership test governs every repoint, re-checked on the
            // loaded object rather than trusted from the earlier pass.
            if (!FMapLinkTextureOwnership::IsMapLinkMaterial(MI))
            {
                continue;
            }
            if (RepointMaterialInstance(MI, Original, Claimed) > 0)
            {
                SaveAssetPackage(MI);
                Entry.RepointedMaterials.Add(MaterialPath);
            }
        }

        // Same list the dry run promised, reported again after the fact so the
        // execute log and the dry-run log agree line for line.
        if (LockedParentMaterials.Num() > 0)
        {
            Entry.Warnings.Add(FString::Printf(
                TEXT("%d parent material(s) still reference the original — a "
                     "parent's texture is a graph default and cannot be "
                     "repointed:"), LockedParentMaterials.Num()));
            for (const FString& Parent : LockedParentMaterials)
            {
                Entry.Warnings.Add(FString::Printf(TEXT("  %s"), *Parent));
            }
        }
    }

    if (bDeletesOriginalAsset && OtherReferencers.Num() > 0)
    {
        // Everything that is not a material: blueprints, levels, data assets.
        // ForceReplaceReferences does the archive walk; it is only reached once
        // the foreign-material check above has passed.
        TArray<UObject*> ToReplace;
        ToReplace.Add(Original);
        ObjectTools::ForceReplaceReferences(Claimed, ToReplace);
        Entry.Warnings.Add(FString::Printf(
            TEXT("repointed %d non-material referencer(s)"), OtherReferencers.Num()));
    }

    // ── Delete, only after every referencer is accounted for ──────────────
    if (bDeletesOriginalAsset)
    {
        TArray<UObject*> ToDelete;
        ToDelete.Add(Original);
        const int32 Deleted = ObjectTools::ForceDeleteObjects(
            ToDelete, /*bShowConfirmation=*/false);
        if (Deleted == 0)
        {
            Entry.Errors.Add(TEXT("the original asset could not be deleted; the "
                                  "MapLink copy exists and the referencers were "
                                  "repointed, so nothing dangles"));
        }

        if (Preset == EMapLinkTextureClaimPreset::Migrate
            && !Entry.DiskPathBefore.IsEmpty())
        {
            FMapLinkSourceControl::DeleteFileTracked(
                Entry.DiskPathBefore, GetChangelistDescription());
        }
    }

    // ── Policy ────────────────────────────────────────────────────────────
    FMapLinkTexturePolicy::SetClass(Entry.AssetPathAfter, EMapLinkTextureClass::MapLink);
    FMapLinkTexturePolicy::RecordAssetSettings(
        Entry.AssetPathAfter, Claimed->SRGB,
        MapLinkTexture::CompressionToString(Claimed->CompressionSettings));
    FMapLinkTexturePolicy::StampConform(
        Entry.AssetPathAfter,
        FString::Printf(TEXT("Claim/%s"), MapLinkTexture::ToString(Preset)),
        WriteResult.bReducedBitDepth);
    if (bDeletesOriginalAsset)
    {
        FMapLinkTexturePolicy::Remove(AssetPath);
    }
    if (!IsOpBatched())
    {
        FMapLinkTexturePolicy::Save();
    }

    Entry.Result = WriteResult.bReducedBitDepth
        ? EMapLinkTextureResult::ReducedLossy
        : EMapLinkTextureResult::Conformed;
    ++Report.NumConformed;
    Report.Entries.Add(MoveTemp(Entry));

    Report.RunSeconds = FPlatformTime::Seconds() - Started;
    if (!IsOpBatched())
    {
        WriteManifest(Report);
    }
    return Report;
}

// ---------------------------------------------------------------------------
// Promote — §6.3
// ---------------------------------------------------------------------------

FMapLinkTextureOpReport FMapLinkTextureOps::Promote(const FString& AssetPath,
                                                    bool bExecute)
{
    // Identical to Fork by definition; the only addition is the class flip,
    // which Claim already does. Kept as a named entry point because "promote
    // this External texture" is a distinct intent from "claim it with preset
    // X", and the console exposes both.
    FMapLinkTextureOpReport Report =
        Claim(AssetPath, EMapLinkTextureClaimPreset::Fork, bExecute);
    Report.OperationName = TEXT("Promote");

    return Report;
}

// ---------------------------------------------------------------------------
// Unified conform — v1.16
// ---------------------------------------------------------------------------

namespace
{
    /** Fold Src's entries and counters into Dst. */
    void MergeReport(FMapLinkTextureOpReport& Dst, FMapLinkTextureOpReport&& Src)
    {
        Dst.Entries.Append(MoveTemp(Src.Entries));
        Dst.NumConformed  += Src.NumConformed;
        Dst.NumSkipped    += Src.NumSkipped;
        Dst.NumFailed     += Src.NumFailed;
        Dst.NumLossy      += Src.NumLossy;
        Dst.NumAlphaSplit += Src.NumAlphaSplit;
        Dst.NumWrites     += Src.NumWrites;
    }
}

FMapLinkTextureOpReport FMapLinkTextureOps::ConformAny(
    const TArray<FString>& AssetPaths, bool bExecute)
{
    const double Started = FPlatformTime::Seconds();

    FMapLinkTextureOpReport Report;
    Report.OperationName = TEXT("Conform");
    Report.Preset        = TEXT("-");
    Report.bExecuted     = bExecute;

    // One policy write and one manifest for the whole run, not one per
    // texture. See FScopedOpBatch.
    FScopedOpBatch Batch;

    // Split by what the asset already is, not by what the user asked for.
    TArray<FString> Direct;    // MapLink already owns it: rename + write
    TArray<FString> Promote;   // somebody else's: duplicate in first

    for (const FString& RawPath : AssetPaths)
    {
        const FString AssetPath =
            FMapLinkTextureNaming::NormalizeAssetPath(RawPath);

        // MapLink's own shipped content is refused here, not filtered by the
        // caller, because there are three callers -- Conform, Conform All, and
        // MapLink.Textures.* from the console -- and a rule enforced in the UI
        // is a rule the console does not have. Promoting a /MapLink/ texture
        // would copy the plugin's own file into <Root>/Textures and repoint
        // the plugin's own shipped material instances at the copy, which is
        // the plugin rewriting itself inside a customer project.
        if (AssetPath.StartsWith(FMapLinkUtils::GetMapLinkPluginContentRoot(),
                                 ESearchCase::IgnoreCase))
        {
            FMapLinkTextureOpEntry Entry;
            Entry.AssetPathBefore = AssetPath;
            Entry.Result = EMapLinkTextureResult::SkippedPluginContent;
            ++Report.NumSkipped;
            Report.Entries.Add(MoveTemp(Entry));
            continue;
        }

        const FString PackagePath =
            FPackageName::GetLongPackagePath(
                FPackageName::ObjectPathToPackageName(AssetPath));
        const FString AssetName = FPaths::GetBaseFilename(
            FPackageName::ObjectPathToPackageName(AssetPath));

        const bool bAlreadyOurs =
            FMapLinkTextureNaming::IsTexturesPackagePath(PackagePath)
            && FMapLinkTextureScanner::EffectiveClass(AssetPath)
                   == EMapLinkTextureClass::MapLink;

        if (bAlreadyOurs) { Direct.Add(AssetPath); }
        else              { Promote.Add(AssetPath); }
    }

    // ── Pacing ───────────────────────────────────────────────────────────
    //
    // A conform run is the heaviest sustained thing MapLink asks of a machine:
    // per texture, a TGA rewrite, an Interchange reimport, a full texture build
    // (~350 MB for a 4K source) and two package saves, in a synchronous loop
    // that never reaches an idle frame. 243 of those ran for five unbroken
    // minutes on 2026-08-21 and ended in a driver-level GPU reset at ~163.
    //
    // The reset was not MapLink's doing and this does not prevent it. What it
    // does is three things worth having on their own merits:
    //
    //   * the run reaches idle regularly instead of once, at the end;
    //   * the render thread and the object graph are drained at each boundary
    //     rather than accumulating across 243 texture builds;
    //   * there is a progress dialog with a cancel button, so a run that is
    //     going badly can be stopped by the person watching it.
    //
    // Cancelling is safe and losing the run is cheap, because conform is
    // idempotent: everything finished is recorded in texture_policy.json and
    // reads back as already-conform, so a second run resumes where this one
    // stopped. That property is what makes pacing a protection rather than a
    // decoration -- it turns "the editor died at 163 of 243" into "run it
    // again", which is exactly what happened.
    const UMapLinkSettings* Settings = UMapLinkSettings::Get();
    const int32 ChunkSize = Settings
        ? FMath::Clamp(Settings->TextureConformChunkSize, 0, 256) : 16;
    const float Breather = Settings
        ? FMath::Clamp(Settings->TextureConformBreatherSeconds, 0.f, 10.f) : 0.5f;

    // Dry runs touch nothing and finish in a moment: no dialog, no pauses.
    const bool bPace = bExecute && ChunkSize > 0 && Promote.Num() > ChunkSize;

    FScopedSlowTask Task(
        static_cast<float>(Promote.Num()),
        FText::FromString(FString::Printf(
            TEXT("Conforming %d texture(s)…"), Promote.Num())));
    if (bExecute && Promote.Num() > 1)
    {
        Task.MakeDialog(/*bShowCancelButton=*/true);
    }

    if (Direct.Num() > 0)
    {
        MergeReport(Report, Conform(Direct, bExecute));
    }

    int32 Done = 0;
    bool bCancelled = false;
    for (const FString& AssetPath : Promote)
    {
        if (bExecute && Task.ShouldCancel())
        {
            bCancelled = true;
        }

        if (bCancelled)
        {
            // Named individually rather than summarised: "43 skipped" does not
            // tell you WHICH 43 still need doing, and the manifest is the
            // record you come back to.
            FMapLinkTextureOpEntry Entry;
            Entry.AssetPathBefore = AssetPath;
            Entry.Result = EMapLinkTextureResult::SkippedCancelled;
            ++Report.NumSkipped;
            Report.Entries.Add(MoveTemp(Entry));
            continue;
        }

        if (bExecute && Promote.Num() > 1)
        {
            Task.EnterProgressFrame(1.f, FText::FromString(FString::Printf(
                TEXT("%d / %d — %s"), Done + 1, Promote.Num(),
                *FPackageName::ObjectPathToPackageName(AssetPath))));
        }

        MergeReport(Report, FMapLinkTextureOps::Promote(AssetPath, bExecute));
        ++Done;

        if (bPace && (Done % ChunkSize) == 0 && Done < Promote.Num())
        {
            // Drain before pausing, not after: a sleep with the last chunk's
            // work still queued on the render thread is a sleep that protects
            // nothing.
            FlushRenderingCommands();
            CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
            if (Breather > 0.f)
            {
                FPlatformProcess::Sleep(Breather);
            }
            UE_LOG(LogMapLink, Display,
                   TEXT("[Textures] Conform paced: %d/%d done, drained and "
                        "paused %.1fs."), Done, Promote.Num(), Breather);
        }
    }

    if (bCancelled)
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("[Textures] Conform CANCELLED after %d of %d. Everything "
                    "finished is recorded and reads as conform; run it again "
                    "to pick up the rest."), Done, Promote.Num());
    }

    // The batch's single flush. A dry run recorded no decisions, so it has
    // nothing to persist. A cancelled run persists what it finished -- that is
    // the whole point of it being safe to cancel.
    if (bExecute)
    {
        FMapLinkTexturePolicy::Save();
    }

    Report.RunSeconds = FPlatformTime::Seconds() - Started;
    WriteManifest(Report);
    return Report;
}

FMapLinkTextureOpReport FMapLinkTextureOps::ImportAndConform(
    const TArray<FString>& SourceFiles, bool bExecute)
{
    const double Started = FPlatformTime::Seconds();

    FMapLinkTextureOpReport Report;
    Report.OperationName = TEXT("Import");
    Report.Preset        = TEXT("-");
    Report.bExecuted     = bExecute;

    if (!bExecute)
    {
        // A dry run cannot import, so it reports the intent. Importing is not
        // destructive and the file picker is itself the confirmation.
        for (const FString& File : SourceFiles)
        {
            FMapLinkTextureOpEntry Entry;
            Entry.DiskPathBefore = File;
            Entry.Result = EMapLinkTextureResult::Conformed;
            Entry.Warnings.Add(FString::Printf(
                TEXT("would import and conform %s"),
                *FPaths::GetCleanFilename(File)));
            ++Report.NumConformed;
            Report.Entries.Add(MoveTemp(Entry));
        }
        Report.RunSeconds = FPlatformTime::Seconds() - Started;
        return Report;
    }

    TArray<FString> ImportedPaths;

    for (const FString& File : SourceFiles)
    {
        if (!FPaths::FileExists(File))
        {
            FMapLinkTextureOpEntry Entry;
            Entry.DiskPathBefore = File;
            Entry.Result = EMapLinkTextureResult::Failed;
            Entry.Errors.Add(TEXT("file not found"));
            ++Report.NumFailed;
            Report.Entries.Add(MoveTemp(Entry));
            continue;
        }

        TArray<FString> One;
        One.Add(File);

        TArray<UObject*> Imported = GetAssetTools().ImportAssets(
            One, FMapLinkTextureNaming::GetTexturesPackagePath());

        UTexture2D* Texture = Imported.Num() > 0
            ? Cast<UTexture2D>(Imported[0]) : nullptr;

        if (!Texture)
        {
            FMapLinkTextureOpEntry Entry;
            Entry.DiskPathBefore = File;
            Entry.Result = EMapLinkTextureResult::Failed;
            Entry.Errors.Add(FString::Printf(
                TEXT("'%s' did not import as a 2D texture"),
                *FPaths::GetCleanFilename(File)));
            ++Report.NumFailed;
            Report.Entries.Add(MoveTemp(Entry));
            continue;
        }

        // Claim it before conforming, so the conform pass takes the
        // rename-and-write path rather than duplicating what it just imported.
        const FString AssetPath = Texture->GetPathName();
        FMapLinkTexturePolicy::SetClass(AssetPath, EMapLinkTextureClass::MapLink);
        SaveAssetPackage(Texture);
        ImportedPaths.Add(AssetPath);
    }

    if (ImportedPaths.Num() > 0)
    {
        FMapLinkTexturePolicy::Save();
        MergeReport(Report, Conform(ImportedPaths, /*bExecute=*/true));
        FMapLinkTextureScanner::Rescan();
    }

    Report.RunSeconds = FPlatformTime::Seconds() - Started;
    WriteManifest(Report);
    return Report;
}

// ---------------------------------------------------------------------------
// Cleanup — §6.6 (v1.16)
// ---------------------------------------------------------------------------

TArray<FMapLinkTextureCleanupItem> FMapLinkTextureOps::FindCleanupCandidates()
{
    TArray<FMapLinkTextureCleanupItem> Out;

    FMapLinkTexturePolicy::EnsureLoaded();
    const FMapLinkTextureScanResult& Scan = FMapLinkTextureScanner::Scan(false);

    // Index every T_Mk_ texture by stem, so "does a MapLink twin exist for
    // this original" is a lookup rather than a second registry walk.
    TMap<FString, FString> MapLinkByStem;
    {
        FARFilter Filter;
        Filter.bRecursivePaths = false;
        Filter.ClassPaths.Add(UTexture2D::StaticClass()->GetClassPathName());
        Filter.PackagePaths.Add(*FMapLinkTextureNaming::GetTexturesPackagePath());

        TArray<FAssetData> MapLinkTextures;
        GetRegistry().GetAssets(Filter, MapLinkTextures);

        const FString Prefix = FMapLinkTextureNaming::GetPrefix();
        for (const FAssetData& Data : MapLinkTextures)
        {
            FString Name = Data.AssetName.ToString();
            if (Name.StartsWith(Prefix, ESearchCase::CaseSensitive))
            {
                Name.RightChopInline(Prefix.Len(), EAllowShrinking::No);
                MapLinkByStem.Add(Name, Data.GetObjectPathString());
            }
        }
    }

    const FString MapLinkRoot = FMapLinkUtils::GetTextureSourceDir();

    for (const FMapLinkTextureRow& Row : Scan.Rows)
    {
        // THE GATE. Nothing else in this function can override it.
        //
        // Row.ReferencingMaterials is MapLink's own view; it deliberately does
        // not know about levels, blueprints or data assets. Deleting on that
        // basis alone would take out a texture a level still uses. Ask the
        // registry for the truth instead.
        const FName PackageName(
            *FPackageName::ObjectPathToPackageName(Row.AssetPath));

        TArray<FName> Referencers;
        GetRegistry().GetReferencers(PackageName, Referencers,
                                     UE::AssetRegistry::EDependencyCategory::Package);
        if (Referencers.Num() > 0)
        {
            continue;
        }

        FMapLinkTextureCleanupItem Item;
        Item.AssetPath      = Row.AssetPath;
        Item.AssetName      = Row.AssetName;
        Item.PackagePath    = Row.PackagePath;
        Item.SourceDiskPath = Row.SourceDiskPath;

        const bool bIsMapLinkAsset =
            FMapLinkTextureNaming::IsMapLinkAssetName(Row.AssetName)
            && FMapLinkTextureNaming::IsTexturesPackagePath(Row.PackagePath);

        if (bIsMapLinkAsset)
        {
            Item.Reason = TEXT("Orphan");
        }
        else
        {
            // Superseded: a T_Mk_ twin exists for this original's stem.
            const FString Stem = FMapLinkTextureNaming::DeriveStem(Row.AssetName);
            const FString* Twin = Stem.IsEmpty() ? nullptr : MapLinkByStem.Find(Stem);
            if (!Twin)
            {
                // Unreferenced, but MapLink never made a copy of it, so its
                // being unused is not MapLink's business and not MapLink's
                // call. Someone imported it and has not used it yet.
                continue;
            }
            Item.Reason       = TEXT("Superseded");
            Item.SupersededBy = *Twin;
        }

        // Is the source file ours to delete? Only if it lives in the MapLink
        // project's Textures folder.
        if (!Item.SourceDiskPath.IsEmpty() && !MapLinkRoot.IsEmpty())
        {
            const FString NormSource =
                FPaths::ConvertRelativePathToFull(Item.SourceDiskPath);
            FString NormRoot = FPaths::ConvertRelativePathToFull(MapLinkRoot);
            FPaths::NormalizeDirectoryName(NormRoot);

            Item.bSourceIsOurs = NormSource.StartsWith(NormRoot + TEXT("/"),
                                                       ESearchCase::IgnoreCase);
            if (Item.bSourceIsOurs)
            {
                Item.SourceBytes = IFileManager::Get().FileSize(*NormSource);
                if (Item.SourceBytes < 0) { Item.SourceBytes = 0; }
            }
        }

        Out.Add(MoveTemp(Item));
    }

    // Superseded first — those are the ones the user just created by copying
    // into MapLink, so they are what they came here for.
    Out.Sort([](const FMapLinkTextureCleanupItem& A,
                const FMapLinkTextureCleanupItem& B)
    {
        if (A.Reason != B.Reason) { return A.Reason > B.Reason; }  // S before O
        return A.AssetName < B.AssetName;
    });

    return Out;
}

FMapLinkTextureOpReport FMapLinkTextureOps::DeleteCleanupItems(
    const TArray<FString>& AssetPaths, bool bExecute)
{
    const double Started = FPlatformTime::Seconds();

    FMapLinkTextureOpReport Report;
    Report.OperationName = TEXT("Cleanup");
    Report.Preset        = TEXT("-");
    Report.bExecuted     = bExecute;

    for (const FString& RawPath : AssetPaths)
    {
        const FString AssetPath =
            FMapLinkTextureNaming::NormalizeAssetPath(RawPath);

        FMapLinkTextureOpEntry Entry;
        Entry.AssetPathBefore = AssetPath;
        Entry.AssetPathAfter  = FString();

        UTexture2D* Texture = LoadTexture(AssetPath);
        if (!Texture)
        {
            Entry.Result = EMapLinkTextureResult::Failed;
            Entry.Errors.Add(TEXT("the asset could not be loaded"));
            ++Report.NumFailed;
            Report.Entries.Add(MoveTemp(Entry));
            continue;
        }

        Entry.TextureClass   = FMapLinkTexturePolicy::GetClass(AssetPath);
        Entry.DiskPathBefore = Texture->AssetImportData
            ? FPaths::ConvertRelativePathToFull(
                  Texture->AssetImportData->GetFirstFilename())
            : FString();

        // Re-check the gate. The list may have sat in a dialog for minutes
        // while the user assigned this texture to something.
        TArray<FName> Referencers;
        GetRegistry().GetReferencers(Texture->GetOutermost()->GetFName(), Referencers,
                                     UE::AssetRegistry::EDependencyCategory::Package);
        if (Referencers.Num() > 0)
        {
            Entry.Result = EMapLinkTextureResult::SkippedUnclassified;
            Entry.Warnings.Add(FString::Printf(
                TEXT("%d asset(s) now reference this texture — it is in use "
                     "again and was left alone"), Referencers.Num()));
            ++Report.NumSkipped;
            Report.Entries.Add(MoveTemp(Entry));
            continue;
        }

        if (!bExecute)
        {
            Entry.Result = EMapLinkTextureResult::Conformed;
            Entry.Warnings.Add(TEXT("would be deleted"));
            ++Report.NumConformed;
            Report.Entries.Add(MoveTemp(Entry));
            continue;
        }

        // Only OUR file gets removed with the asset.
        const FString MapLinkRoot = FMapLinkUtils::GetTextureSourceDir();
        bool bSourceIsOurs = false;
        if (!Entry.DiskPathBefore.IsEmpty() && !MapLinkRoot.IsEmpty())
        {
            FString NormRoot = FPaths::ConvertRelativePathToFull(MapLinkRoot);
            FPaths::NormalizeDirectoryName(NormRoot);
            bSourceIsOurs = Entry.DiskPathBefore.StartsWith(
                NormRoot + TEXT("/"), ESearchCase::IgnoreCase);
        }

        TArray<UObject*> ToDelete;
        ToDelete.Add(Texture);
        const int32 Deleted = ObjectTools::ForceDeleteObjects(
            ToDelete, /*bShowConfirmation=*/false);

        if (Deleted == 0)
        {
            Entry.Result = EMapLinkTextureResult::Failed;
            Entry.Errors.Add(TEXT("the asset could not be deleted"));
            ++Report.NumFailed;
            Report.Entries.Add(MoveTemp(Entry));
            continue;
        }

        if (bSourceIsOurs)
        {
            FMapLinkSourceControl::DeleteFileTracked(
                Entry.DiskPathBefore, GetChangelistDescription());
        }
        else if (!Entry.DiskPathBefore.IsEmpty())
        {
            Entry.Warnings.Add(FString::Printf(
                TEXT("the source file %s is outside the MapLink project and "
                     "was left on disk"),
                *FPaths::GetCleanFilename(Entry.DiskPathBefore)));
        }

        FMapLinkTexturePolicy::Remove(AssetPath);

        if (UMapLinkEditorState* State = UMapLinkEditorState::Get())
        {
            State->RemoveTextureCache(AssetPath);
        }

        Entry.Result = EMapLinkTextureResult::Conformed;
        ++Report.NumConformed;
        Report.Entries.Add(MoveTemp(Entry));
    }

    if (bExecute && Report.NumConformed > 0)
    {
        FMapLinkTexturePolicy::Save();
        FMapLinkTextureScanner::Rescan();
    }

    Report.RunSeconds = FPlatformTime::Seconds() - Started;
    WriteManifest(Report);
    return Report;
}

// ---------------------------------------------------------------------------
// Orphans — §6.5
// ---------------------------------------------------------------------------

FMapLinkTextureOpReport FMapLinkTextureOps::Orphans(bool bDelete, bool bExecute)
{
    const double Started = FPlatformTime::Seconds();

    FMapLinkTextureOpReport Report;
    Report.OperationName = bDelete ? TEXT("Orphans/Delete") : TEXT("Orphans");
    Report.Preset        = TEXT("-");
    Report.bExecuted     = bExecute;

    const FMapLinkTextureScanResult& Scan = FMapLinkTextureScanner::HasResult()
        ? FMapLinkTextureScanner::GetLastResult()
        : FMapLinkTextureScanner::Scan();

    for (const FMapLinkTextureRow& Row : Scan.Rows)
    {
        if (!Row.bOrphan)
        {
            continue;
        }

        FMapLinkTextureOpEntry Entry;
        Entry.AssetPathBefore = Row.AssetPath;
        Entry.AssetPathAfter  = Row.AssetPath;
        Entry.DiskPathBefore  = Row.SourceDiskPath;
        Entry.TextureClass    = Row.TextureClass;
        Entry.Result          = EMapLinkTextureResult::Orphan;

        if (!bDelete)
        {
            Entry.Warnings.Add(TEXT("no MapLink material references this "
                                    "texture — status only, nothing was done"));
            Report.Entries.Add(MoveTemp(Entry));
            ++Report.NumSkipped;
            continue;
        }

        if (!bExecute)
        {
            Entry.Warnings.Add(TEXT("would be deleted"));
            Report.Entries.Add(MoveTemp(Entry));
            ++Report.NumSkipped;
            continue;
        }

        UTexture2D* Texture = LoadTexture(Row.AssetPath);
        if (!Texture)
        {
            Entry.Result = EMapLinkTextureResult::Failed;
            Entry.Errors.Add(TEXT("could not be loaded"));
            Report.Entries.Add(MoveTemp(Entry));
            ++Report.NumFailed;
            continue;
        }

        // Re-check referencers at the moment of deletion rather than trusting a
        // scan that may be minutes old. An orphan that gained a referencer
        // since the scan is not an orphan.
        TArray<FAssetData> Referencers;
        CollectReferencingAssets(Texture, Referencers);
        if (Referencers.Num() > 0)
        {
            Entry.Result = EMapLinkTextureResult::Failed;
            Entry.Errors.Add(FString::Printf(
                TEXT("gained %d referencer(s) since the scan — not deleted"),
                Referencers.Num()));
            Report.Entries.Add(MoveTemp(Entry));
            ++Report.NumFailed;
            continue;
        }

        TArray<UObject*> ToDelete;
        ToDelete.Add(Texture);
        if (ObjectTools::ForceDeleteObjects(ToDelete, false) > 0)
        {
            if (!Row.SourceDiskPath.IsEmpty())
            {
                // DeleteFileTracked, not EnsureFileInChangelist. An orphan's
                // TGA is very often a file MapLink added and never submitted;
                // marking it for add and then unlinking it left the changelist
                // holding a pending add for a path that no longer exists.
                FMapLinkSourceControl::DeleteFileTracked(
                    Row.SourceDiskPath, GetChangelistDescription());
            }
            FMapLinkTexturePolicy::Remove(Row.AssetPath);
            ++Report.NumConformed;
        }
        else
        {
            Entry.Result = EMapLinkTextureResult::Failed;
            Entry.Errors.Add(TEXT("delete failed"));
            ++Report.NumFailed;
        }

        Report.Entries.Add(MoveTemp(Entry));
    }

    if (bExecute && bDelete)
    {
        FMapLinkTexturePolicy::Save();
    }

    Report.RunSeconds = FPlatformTime::Seconds() - Started;
    WriteManifest(Report);
    return Report;
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------

FString FMapLinkTextureOps::WriteManifest(FMapLinkTextureOpReport& Report)
{
    const FString Dir = FMapLinkUtils::GetArtifactLogDir() / TEXT("TextureConform");
    FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*Dir);

    const FString Path = Dir / FString::Printf(
        TEXT("texture_conform_%s.json"), *TimestampForFilename());

    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("timestamp"), FDateTime::UtcNow().ToIso8601());
    Root->SetStringField(TEXT("operation"), Report.OperationName);
    Root->SetStringField(TEXT("preset"), Report.Preset);
    Root->SetStringField(TEXT("mode"), Report.bExecuted ? TEXT("execute") : TEXT("dry-run"));
    Root->SetNumberField(TEXT("run_seconds"), Report.RunSeconds);
    Root->SetNumberField(TEXT("conformed"), Report.NumConformed);
    Root->SetNumberField(TEXT("skipped"), Report.NumSkipped);
    Root->SetNumberField(TEXT("failed"), Report.NumFailed);
    Root->SetNumberField(TEXT("lossy"), Report.NumLossy);
    Root->SetNumberField(TEXT("alpha_split"), Report.NumAlphaSplit);
    Root->SetNumberField(TEXT("disk_writes"), Report.NumWrites);

    TArray<TSharedPtr<FJsonValue>> Items;
    for (const FMapLinkTextureOpEntry& Entry : Report.Entries)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("asset_before"), Entry.AssetPathBefore);
        Obj->SetStringField(TEXT("asset_after"),  Entry.AssetPathAfter);
        Obj->SetStringField(TEXT("disk_before"),  Entry.DiskPathBefore);
        Obj->SetStringField(TEXT("disk_after"),   Entry.DiskPathAfter);
        Obj->SetStringField(TEXT("class"),  MapLinkTexture::ToString(Entry.TextureClass));
        Obj->SetStringField(TEXT("action"), MapLinkTexture::ToString(Entry.Result));
        Obj->SetBoolField(TEXT("lossy_reduction"), Entry.bLossyReduction);
        Obj->SetBoolField(TEXT("renamed"), Entry.bRenamed);
        Obj->SetBoolField(TEXT("skipped_identical"), Entry.bSkippedIdentical);

        if (!Entry.AlphaAssetPath.IsEmpty())
        {
            Obj->SetStringField(TEXT("alpha_asset"), Entry.AlphaAssetPath);
        }
        if (!Entry.ActionItem.IsEmpty())
        {
            Obj->SetStringField(TEXT("action_item"), Entry.ActionItem);
        }

        auto AddStrings = [&Obj](const TCHAR* Field, const TArray<FString>& Values)
        {
            if (Values.Num() == 0) { return; }
            TArray<TSharedPtr<FJsonValue>> Array;
            for (const FString& Value : Values)
            {
                Array.Add(MakeShared<FJsonValueString>(Value));
            }
            Obj->SetArrayField(Field, Array);
        };
        AddStrings(TEXT("warnings"), Entry.Warnings);
        AddStrings(TEXT("errors"),   Entry.Errors);
        AddStrings(TEXT("repointed_materials"), Entry.RepointedMaterials);

        Items.Add(MakeShared<FJsonValueObject>(Obj));
    }
    Root->SetArrayField(TEXT("entries"), Items);

    FString Out;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
    FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

    FFileHelper::SaveStringToFile(Out, *Path,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

    Report.ManifestPath = Path;
    UE_LOG(LogMapLink, Log, TEXT("[Textures] Manifest: %s"), *Path);
    return Path;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

FString FMapLinkTextureOps::BuildReportText(const FMapLinkTextureOpReport& Report)
{
    TStringBuilder<8192> Out;
    Out.Appendf(TEXT("%s (%s%s) — %d conformed, %d skipped, %d failed, "
                     "%d disk write(s) in %.2fs\n"),
                *Report.OperationName,
                Report.bExecuted ? TEXT("execute") : TEXT("DRY RUN"),
                Report.Preset == TEXT("-") ? TEXT("")
                                           : *FString::Printf(TEXT(", %s"), *Report.Preset),
                Report.NumConformed, Report.NumSkipped, Report.NumFailed,
                Report.NumWrites, Report.RunSeconds);

    for (const FMapLinkTextureOpEntry& Entry : Report.Entries)
    {
        Out.Appendf(TEXT("  [%s] %s\n"),
                    MapLinkTexture::ToString(Entry.Result), *Entry.AssetPathBefore);
        if (Entry.bRenamed && Entry.AssetPathAfter != Entry.AssetPathBefore)
        {
            Out.Appendf(TEXT("      -> %s\n"), *Entry.AssetPathAfter);
        }
        for (const FString& Warning : Entry.Warnings)
        {
            Out.Appendf(TEXT("      · %s\n"), *Warning);
        }
        for (const FString& Error : Entry.Errors)
        {
            Out.Appendf(TEXT("      ! %s\n"), *Error);
        }
        if (!Entry.ActionItem.IsEmpty())
        {
            Out.Appendf(TEXT("      TODO: %s\n"), *Entry.ActionItem);
        }
        if (Entry.RepointedMaterials.Num() > 0)
        {
            Out.Appendf(TEXT("      repointed %d material instance(s)\n"),
                        Entry.RepointedMaterials.Num());
        }
    }

    if (!Report.ManifestPath.IsEmpty())
    {
        Out.Appendf(TEXT("\nManifest: %s\n"), *Report.ManifestPath);
    }
    return Out.ToString();
}

void FMapLinkTextureOps::NotifySummary(const FMapLinkTextureOpReport& Report)
{
    // Toast, never a modal. Modals are for decisions, destructive confirms and
    // hard failures that block a requested action; a run summary is none of
    // those, however much there is to say.
    const FString Mode = Report.bExecuted ? TEXT("") : TEXT(" (dry run)");

    if (Report.HasFailures())
    {
        MapLinkNotify::Fail(FText::FromString(FString::Printf(
            TEXT("Textures%s: %d failed, %d conformed. See the Output Log."),
            *Mode, Report.NumFailed, Report.NumConformed)));
        return;
    }

    if (Report.NumLossy > 0 || Report.NumAlphaSplit > 0)
    {
        MapLinkNotify::Warn(FText::FromString(FString::Printf(
            TEXT("Textures%s: %d conformed, %d reduced to 8-bit, %d alpha "
                 "split. Check the manifest."),
            *Mode, Report.NumConformed, Report.NumLossy, Report.NumAlphaSplit)));
        return;
    }

    MapLinkNotify::Success(FText::FromString(FString::Printf(
        TEXT("Textures%s: %d conformed, %d already correct, %d write(s)."),
        *Mode, Report.NumConformed, Report.NumSkipped, Report.NumWrites)));
}
