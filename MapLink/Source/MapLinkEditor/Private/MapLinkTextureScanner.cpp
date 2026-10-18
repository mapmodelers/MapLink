// Copyright MapModelers. All Rights Reserved.
//
// v1.16 — The texture scan (see header).

#include "MapLinkTextureScanner.h"

#include "MapLinkTextureNaming.h"
#include "MapLinkTextureOwnership.h"
#include "MapLinkTexturePolicy.h"
#include "MapLinkTgaWriter.h"
#include "MapLinkEditorState.h"
#include "MapLinkSettings.h"
#include "MapLinkUtils.h"
#include "MapLinkPipeline.h"   // LogMapLink

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialTypes.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "HAL/FileManager.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    FMapLinkTextureScanResult GLastResult;
    bool GHasResult = false;

    const IAssetRegistry& GetRegistry()
    {
        const FAssetRegistryModule& Module =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
        return Module.Get();
    }

    /** Package-file mtime, or a zero time when the file is not on disk. */
    FDateTime GetPackageMTime(const FString& PackageName)
    {
        FString Filename;
        if (!FPackageName::DoesPackageExist(PackageName, &Filename))
        {
            return FDateTime(0);
        }
        return IFileManager::Get().GetTimeStamp(*Filename);
    }

    /** True for the reserved preview directories, which every pass ignores. */
    bool IsReservedPreviewPath(const FString& AbsolutePath)
    {
        const FString PreviewDir = FMapLinkUtils::GetTexturePreviewDir();
        if (PreviewDir.IsEmpty() || AbsolutePath.IsEmpty())
        {
            return false;
        }
        return AbsolutePath.StartsWith(PreviewDir + TEXT("/"), ESearchCase::IgnoreCase);
    }
}

// ---------------------------------------------------------------------------
// Pass 1 — AssetRegistry only. No package loads.
// ---------------------------------------------------------------------------

void FMapLinkTextureScanner::EnumerateFast(
    FMapLinkTextureScanResult& Out,
    TMap<FString, TArray<FString>>& OutTextureToMaterials)
{
    const IAssetRegistry& Registry = GetRegistry();

    // The texture set is the union of two terms. The second one is what catches
    // orphans: a texture sitting in MapLink's folder that nothing references
    // would be invisible if we only walked material dependencies.
    TMap<FString, FAssetData> Candidates;   // asset path -> data

    auto ConsiderAsset = [&](const FAssetData& Data) -> bool
    {
        const FString AssetPath = Data.GetObjectPathString();

        // §2.3 — filter out and COUNT anything that is not a plain UTexture2D.
        // Cubes, arrays, render targets and virtual textures all have no
        // FTextureSource story we are willing to tell, and dropping them
        // silently would make the totals lie.
        const FTopLevelAssetPath ClassPath = Data.AssetClassPath;
        if (ClassPath != UTexture2D::StaticClass()->GetClassPathName())
        {
            return false;
        }

        Candidates.Add(AssetPath, Data);
        return true;
    };

    // ── Term 1: textures referenced by MapLink-owned material instances ───
    //
    // Package-level dependencies, not parameter enumeration: this pass must not
    // load anything. The precise pass corrects this with the exporter's own
    // enumeration, so scan and export can never disagree about what a material
    // references — but that costs loads, and this pass is what makes the view
    // open at all.
    TArray<FAssetData> MaterialAssets;
    FMapLinkTextureOwnership::GetMapLinkMaterialInstanceAssets(MaterialAssets);

    for (const FAssetData& MaterialAsset : MaterialAssets)
    {
        const FString MaterialPath = MaterialAsset.GetObjectPathString();

        TArray<FName> Dependencies;
        Registry.GetDependencies(MaterialAsset.PackageName, Dependencies,
                                 UE::AssetRegistry::EDependencyCategory::Package);

        for (const FName& DependencyPackage : Dependencies)
        {
            TArray<FAssetData> InPackage;
            Registry.GetAssetsByPackageName(DependencyPackage, InPackage);

            for (const FAssetData& Data : InPackage)
            {
                if (ConsiderAsset(Data))
                {
                    OutTextureToMaterials.FindOrAdd(Data.GetObjectPathString())
                                         .AddUnique(MaterialPath);
                }
                else if (Data.AssetClassPath.GetAssetName().ToString()
                             .Contains(TEXT("Texture")))
                {
                    Out.SkippedUnsupported.AddUnique(FString::Printf(
                        TEXT("%s (%s)"), *Data.GetObjectPathString(),
                        *Data.AssetClassPath.ToString()));
                }
            }
        }
    }

    // ── Term 2: everything under /Game/MapLink/Textures/ ──────────────────
    {
        FARFilter Filter;
        Filter.bRecursivePaths   = true;
        Filter.bRecursiveClasses = false;
        Filter.ClassPaths.Add(UTexture2D::StaticClass()->GetClassPathName());
        Filter.PackagePaths.Add(*FMapLinkTextureNaming::GetTexturesPackagePath());

        TArray<FAssetData> Owned;
        Registry.GetAssets(Filter, Owned);
        for (const FAssetData& Data : Owned)
        {
            ConsiderAsset(Data);
        }
    }

    // ── Rows ──────────────────────────────────────────────────────────────
    Out.Rows.Reset(Candidates.Num());

    TArray<FString> SortedPaths;
    Candidates.GetKeys(SortedPaths);
    SortedPaths.Sort();

    for (const FString& AssetPath : SortedPaths)
    {
        const FAssetData& Data = Candidates[AssetPath];

        FMapLinkTextureRow Row;
        Row.AssetPath   = AssetPath;
        Row.AssetName   = Data.AssetName.ToString();
        Row.PackagePath = Data.PackagePath.ToString();

        // Class resolves from the policy file immediately — that is the whole
        // point of keeping decisions in a file rather than deriving them.
        Row.TextureClass   = EffectiveClass(AssetPath);
        Row.SuggestedClass = SuggestClass(AssetPath);

        if (const TArray<FString>* Materials = OutTextureToMaterials.Find(AssetPath))
        {
            Row.ReferencingMaterials = *Materials;
        }

        Out.Rows.Add(MoveTemp(Row));
    }

    for (int32 Index = 0; Index < Out.Rows.Num(); ++Index)
    {
        Out.RowIndexByAssetPath.Add(Out.Rows[Index].AssetPath, Index);
    }
}

// ---------------------------------------------------------------------------
// Pass 2 — load only what is stale, then evaluate
// ---------------------------------------------------------------------------

void FMapLinkTextureScanner::ResolvePrecise(FMapLinkTextureScanResult& InOut,
                                            bool bForceFullRecompute)
{
    UMapLinkEditorState* State = UMapLinkEditorState::Get();
    const UMapLinkSettings* Settings = UMapLinkSettings::Get();
    const bool bAllowReduction = Settings ? Settings->bAllowBitDepthReduction : true;
    (void)bAllowReduction;   // consumed by the ops layer, recorded here for parity

    // ── Precise reverse index ─────────────────────────────────────────────
    //
    // The fast pass used package dependencies, which include the parent
    // material and anything else the MI happens to reference. Here we enumerate
    // texture parameters EXACTLY as FMapLinkMaterialExporter::RunExport does —
    // GetAllTextureParameterInfo + GetTextureParameterValue with
    // bOverriddenOnly — so the scan and the export can never disagree about
    // what a material references. Anything the fast pass over-collected is
    // corrected here.
    TArray<FAssetData> MaterialAssets;
    FMapLinkTextureOwnership::GetMapLinkMaterialInstanceAssets(MaterialAssets);

    TMap<FString, TArray<FString>> PreciseTextureToMaterials;

    for (const FAssetData& MaterialAsset : MaterialAssets)
    {
        UMaterialInstanceConstant* MI =
            Cast<UMaterialInstanceConstant>(MaterialAsset.GetAsset());
        if (!MI)
        {
            continue;
        }
        ++InOut.NumPackagesLoaded;

        // Guard the whole feature on the ownership test, even though the
        // registry query already filtered by path: a material that merely SITS
        // under /Game/MapLink/ without a MapLink parent is still MapLink's by
        // the stated rule, and one test governing every decision is the point.
        if (!FMapLinkTextureOwnership::IsMapLinkMaterial(MI))
        {
            continue;
        }

        const FString MaterialPath = MaterialAsset.GetObjectPathString();

        TArray<FMaterialParameterInfo> ParamInfos;
        TArray<FGuid> ParamGuids;
        MI->GetAllTextureParameterInfo(ParamInfos, ParamGuids);

        for (const FMaterialParameterInfo& Info : ParamInfos)
        {
            UTexture* TexValue = nullptr;
            if (!MI->GetTextureParameterValue(Info, TexValue, /*bOveriddenOnly=*/true)
                || !TexValue)
            {
                continue;
            }

            const FString TexturePath = TexValue->GetPathName();

            if (!Cast<UTexture2D>(TexValue))
            {
                InOut.SkippedUnsupported.AddUnique(FString::Printf(
                    TEXT("%s (%s, referenced by %s as '%s')"),
                    *TexturePath, *TexValue->GetClass()->GetName(),
                    *MaterialPath, *Info.Name.ToString()));
                continue;
            }

            PreciseTextureToMaterials.FindOrAdd(TexturePath).AddUnique(MaterialPath);
        }
    }

    // ── Per-texture resolution ────────────────────────────────────────────
    FScopedSlowTask SlowTask(static_cast<float>(InOut.Rows.Num()),
        NSLOCTEXT("MapLink", "TextureScanProgress", "Scanning MapLink textures…"));
    SlowTask.MakeDialogDelayed(1.0f, /*bShowCancelButton=*/false);

    for (FMapLinkTextureRow& Row : InOut.Rows)
    {
        SlowTask.EnterProgressFrame(1.f);

        // The precise index replaces the provisional one wholesale. A texture
        // the fast pass attributed to a material that does not actually
        // reference it through a parameter loses that edge here.
        if (const TArray<FString>* Precise = PreciseTextureToMaterials.Find(Row.AssetPath))
        {
            Row.ReferencingMaterials = *Precise;
        }
        else
        {
            Row.ReferencingMaterials.Reset();
        }

        const FString PackageName = FPackageName::ObjectPathToPackageName(Row.AssetPath);
        const FDateTime PackageMTime = GetPackageMTime(PackageName);

        FMapLinkTextureCacheRecord* Cached =
            State ? State->FindTextureCache(Row.AssetPath) : nullptr;

        const bool bCacheUsable =
            !bForceFullRecompute
            && Cached
            && Cached->PackageMTime == PackageMTime
            && PackageMTime != FDateTime(0);

        if (bCacheUsable)
        {
            // Warm path: nothing is loaded. This is the branch that has to hold
            // for several thousand textures, so it must not touch the package.
            Row.SourceDiskPath = Cached->SourceDiskPath;
            Row.bSRGB          = Cached->bSRGB;
            Row.Compression    = Cached->Compression;
            Row.SourceId       = Cached->SourceId;
        }
        else
        {
            UTexture2D* Texture = Cast<UTexture2D>(
                StaticLoadObject(UTexture2D::StaticClass(), nullptr, *Row.AssetPath));
            if (!Texture)
            {
                Row.Status = EMapLinkTextureStatus::Broken;
                Row.Issues.Add(TEXT("the asset could not be loaded"));
                Row.Remedy = TEXT("Check the package is present and not corrupt.");
                continue;
            }
            ++InOut.NumPackagesLoaded;

            Row.bSRGB       = Texture->SRGB;
            Row.Compression = MapLinkTexture::CompressionToString(
                Texture->CompressionSettings);
            Row.SourceId    = Texture->Source.IsValid()
                            ? Texture->Source.GetId() : FGuid();

            if (Texture->AssetImportData)
            {
                const FString First = Texture->AssetImportData->GetFirstFilename();
                if (!First.IsEmpty())
                {
                    Row.SourceDiskPath = FPaths::ConvertRelativePathToFull(First);
                }
            }

            // Both values are READ from the asset and recorded, never changed.
            FMapLinkTexturePolicy::RecordAssetSettings(
                Row.AssetPath, Row.bSRGB, Row.Compression);

            if (State)
            {
                FMapLinkTextureCacheRecord Record;
                Record.AssetPath      = Row.AssetPath;
                Record.SourceDiskPath = Row.SourceDiskPath;
                Record.bSRGB          = Row.bSRGB;
                Record.Compression    = Row.Compression;
                Record.SourceId       = Row.SourceId;
                Record.PackageMTime   = PackageMTime;

                const int64 Size = Row.SourceDiskPath.IsEmpty()
                    ? 0 : IFileManager::Get().FileSize(*Row.SourceDiskPath);
                Record.DiskSize = Size;
                Record.DiskMTime = Row.SourceDiskPath.IsEmpty()
                    ? FDateTime(0)
                    : IFileManager::Get().GetTimeStamp(*Row.SourceDiskPath);

                // No Save() here. See the note on SetTextureCache: one write at
                // the end of the pass, not one per row.
                State->SetTextureCache(Record);
            }
        }

        EvaluateRow(Row);
    }

    if (State)
    {
        State->Save();
    }
}

// ---------------------------------------------------------------------------
// Status evaluation
// ---------------------------------------------------------------------------

void FMapLinkTextureScanner::EvaluateRow(FMapLinkTextureRow& Row)
{
    // ── Plugin ────────────────────────────────────────────────────────────
    //
    // Before everything, including Broken. A texture on MapLink's own /MapLink/
    // mount ships with the plugin: it is the same file for every customer, it
    // is not in anybody's Perforce depot, and the host project has no say over
    // it. Every question further down this function -- is the source inside
    // the synced tree, is the name conform, has a class been recorded -- is a
    // question about the PROJECT's content, and asking it about the plugin's
    // own gets an answer that is meaningless at best and acted on at worst.
    //
    // The measured symptom: the plugin's five debug textures (32_red,
    // 32_blue, 32_normal, 32_grayscale_white, 128_noise, referenced by the
    // shipped DBG_MI instances) came back Portable, showed as "Project", and
    // were swept into Conform All along with 243 real ones -- which would have
    // copied MapLink's own textures into the host project and repointed
    // MapLink's own shipped materials at the copies.
    if (FMapLinkUtils::GetMapLinkPluginContentRoot().Len() > 0
        && Row.AssetPath.StartsWith(FMapLinkUtils::GetMapLinkPluginContentRoot(),
                                    ESearchCase::IgnoreCase))
    {
        Row.Status = EMapLinkTextureStatus::Plugin;
        Row.RelativeToLibrary.Reset();
        Row.Remedy = TEXT("Ships with MapLink. Nothing to do.");
        return;
    }

    // Portability first, because it is what a teammate experiences and it is
    // meaningful for every class.
    Row.RelativeToLibrary.Reset();
    const bool bPortable = !Row.SourceDiskPath.IsEmpty()
        && FMapLinkTextureNaming::ComputePortablePath(
               Row.SourceDiskPath, Row.RelativeToLibrary);

    // ── Unclassified ──────────────────────────────────────────────────────
    if (Row.TextureClass == EMapLinkTextureClass::Unclassified)
    {
        Row.Status = EMapLinkTextureStatus::Unclassified;
        Row.Issues.Add(TEXT("no class decision recorded"));
        Row.Remedy = TEXT("No class recorded.");
        return;
    }

    // ── Broken ────────────────────────────────────────────────────────────
    if (Row.SourceDiskPath.IsEmpty())
    {
        Row.Status = EMapLinkTextureStatus::Broken;
        Row.Issues.Add(TEXT("no source file is recorded on the asset"));
        Row.Remedy = TEXT("No source file recorded. Re-import or conform.");
        return;
    }
    if (!FPaths::FileExists(Row.SourceDiskPath))
    {
        Row.Status = EMapLinkTextureStatus::Broken;
        Row.Issues.Add(FString::Printf(TEXT("source file is missing: %s"),
                                       *Row.SourceDiskPath));
        Row.Remedy = TEXT("Source file missing. Restore it or conform, which "
                          "rewrites it from the asset's own source data.");
        return;
    }
    {
        FString Reason;
        if (!FMapLinkTextureNaming::ValidateLengths(
                Row.AssetName, Row.SourceDiskPath, Reason))
        {
            Row.Status = EMapLinkTextureStatus::Broken;
            Row.Issues.Add(Reason);
            Row.Remedy = TEXT("Path too long. Shorten the name, or move the project closer to "
                              "the drive root.");
            return;
        }
    }

    // ── External ──────────────────────────────────────────────────────────
    //
    // Path and name legality, plus the recorded settings — and nothing else.
    // External originals are read-only to MapLink under every operation,
    // without exception, so there is no container check to fail.
    if (Row.TextureClass == EMapLinkTextureClass::External)
    {
        Row.Status = bPortable ? EMapLinkTextureStatus::Portable
                               : EMapLinkTextureStatus::LocalOnly;

        // Phase 1 records the preview requirement and reports it as pending.

        if (!bPortable)
        {
            Row.Issues.Add(TEXT("source file is outside the synced tree"));
            Row.Remedy = TEXT("Source file outside the project. Conform to "
                              "bring it into MapLink.");
        }
        return;
    }

    // ── MapLink class — the full check list ───────────────────────────────
    bool bAllPassed = true;
    auto Fail = [&Row, &bAllPassed](const FString& Issue)
    {
        Row.Issues.Add(Issue);
        bAllPassed = false;
    };

    if (!FMapLinkTextureNaming::IsTexturesPackagePath(Row.PackagePath))
    {
        Fail(FString::Printf(
            TEXT("lives in %s, not directly under %s"),
            *Row.PackagePath, *FMapLinkTextureNaming::GetTexturesPackagePath()));
    }

    if (!FMapLinkTextureNaming::IsMapLinkAssetName(Row.AssetName))
    {
        Fail(FString::Printf(
            TEXT("name '%s' does not match T_Mk_[A-Za-z0-9_]+"), *Row.AssetName));
    }

    // §4.5 bijection: the disk path must be derivable from the asset path.
    const FString ExpectedDiskPath =
        FMapLinkTextureNaming::ComposeSourcePath(Row.AssetName);
    if (ExpectedDiskPath.IsEmpty())
    {
        Fail(TEXT("no MapLink project root is configured, so the expected "
                  "source path cannot be composed"));
    }
    else
    {
        if (!FPaths::FileExists(ExpectedDiskPath))
        {
            Fail(FString::Printf(TEXT("no source file at %s"), *ExpectedDiskPath));
        }
        else
        {
            FString ContainerReason;
            if (!FMapLinkTgaWriter::IsConformContainer(ExpectedDiskPath, ContainerReason))
            {
                Fail(FString::Printf(TEXT("the TGA at %s %s"),
                                     *ExpectedDiskPath, *ContainerReason));
            }
        }

        // AssetImportData must point at that exact file, otherwise a reimport
        // would pull pixels from somewhere MapLink does not own.
        const FString Normalized = FPaths::ConvertRelativePathToFull(Row.SourceDiskPath);
        if (!Normalized.Equals(ExpectedDiskPath, ESearchCase::IgnoreCase))
        {
            Fail(FString::Printf(
                TEXT("import path is %s but should be %s"),
                *Normalized, *ExpectedDiskPath));
        }
    }

    // Staleness: the recorded source GUID versus the live one. UE maintains
    // this and bumps it on every source edit, so it answers "has anything
    // changed" without hashing a single pixel.
    if (const FMapLinkTexturePolicyEntry* Entry =
            FMapLinkTexturePolicy::Find(Row.AssetPath))
    {
        if (Entry->LastConform.IsEmpty())
        {
            Fail(TEXT("has never been conformed"));
        }
    }
    else
    {
        Fail(TEXT("has no policy entry"));
    }

    if (bAllPassed)
    {
        Row.Status = EMapLinkTextureStatus::Conform;
        Row.Remedy.Reset();
    }
    else
    {
        // A MapLink-class texture that fails its checks is still reachable if
        // its file is inside the synced tree — it is not conform, but it is not
        // a teammate's problem either. Reporting it as LocalOnly would be a lie.
        Row.Status = bPortable ? EMapLinkTextureStatus::Portable
                               : EMapLinkTextureStatus::LocalOnly;
        Row.Remedy = TEXT("Not conform.");
    }
}

// ---------------------------------------------------------------------------
// Pass 2b — forget what nothing uses
// ---------------------------------------------------------------------------

void FMapLinkTextureScanner::DropReleasedRows(FMapLinkTextureScanResult& InOut)
{
    // The two enumeration passes can disagree, and until now the row survived
    // the disagreement.
    //
    // EnumerateFast term 1 proposes a texture because a MapLink material
    // instance's PACKAGE names it. ResolvePrecise then loads every MapLink
    // material and enumerates its texture PARAMETERS, and replaces the
    // provisional attribution wholesale -- a texture no parameter points at
    // ends with ReferencingMaterials empty. That verdict is authoritative:
    // the precise index is built from all MapLink materials unconditionally,
    // not from the mtime cache, so an empty list means no MapLink material
    // uses this texture, full stop.
    //
    // A conformed original lands in exactly that state, and it is not a rare
    // corner: a package keeps naming a texture after the parameter pointing at
    // it has moved (UMaterialInstance's serialised CachedReferencedTextures is
    // the usual reason). So the tab went on listing ten superseded originals
    // as "External and require conforming" while their own detail panel said
    // "Used By: none" -- the view contradicting itself on one screen, and the
    // count in the header sized the work wrong.
    //
    // Term 2 rows are exempt. Everything under <MapLink>/Textures/ belongs to
    // MapLink whether or not anything references it -- that is what an orphan
    // IS, and finding orphans is the reason term 2 exists at all.
    TArray<FMapLinkTextureRow> Kept;
    Kept.Reserve(InOut.Rows.Num());

    for (FMapLinkTextureRow& Row : InOut.Rows)
    {
        const bool bMapLinkOwned =
            FMapLinkTextureNaming::IsTexturesPackagePath(Row.PackagePath);

        if (!bMapLinkOwned && Row.ReferencingMaterials.Num() == 0)
        {
            ++InOut.NumReleased;
            UE_LOG(LogMapLink, Verbose,
                   TEXT("[Textures] '%s' released — no MapLink material uses "
                        "it. Dropping it from the view."), *Row.AssetPath);
            continue;
        }
        Kept.Add(MoveTemp(Row));
    }

    if (InOut.NumReleased > 0)
    {
        InOut.Rows = MoveTemp(Kept);
        InOut.RowIndexByAssetPath.Reset();
        for (int32 Index = 0; Index < InOut.Rows.Num(); ++Index)
        {
            InOut.RowIndexByAssetPath.Add(InOut.Rows[Index].AssetPath, Index);
        }
    }
}

// ---------------------------------------------------------------------------
// Aggregates
// ---------------------------------------------------------------------------

void FMapLinkTextureScanner::BuildAggregates(FMapLinkTextureScanResult& InOut)
{
    InOut.MaterialAggregates.Reset();

    for (const FMapLinkTextureRow& Row : InOut.Rows)
    {
        switch (Row.Status)
        {
        case EMapLinkTextureStatus::Conform:      ++InOut.NumConform;      break;
        case EMapLinkTextureStatus::Plugin:       ++InOut.NumPlugin;       break;
        case EMapLinkTextureStatus::Portable:     ++InOut.NumPortable;     break;
        case EMapLinkTextureStatus::LocalOnly:    ++InOut.NumLocalOnly;    break;
        case EMapLinkTextureStatus::Unclassified: ++InOut.NumUnclassified; break;
        case EMapLinkTextureStatus::Broken:       ++InOut.NumBroken;       break;
        }

        for (const FString& MaterialPath : Row.ReferencingMaterials)
        {
            FMapLinkTextureAggregate& Agg =
                InOut.MaterialAggregates.FindOrAdd(MaterialPath);
            switch (Row.Status)
            {
            case EMapLinkTextureStatus::Conform:      ++Agg.NumConform;      break;
            case EMapLinkTextureStatus::Plugin:       ++Agg.NumPlugin;       break;
            case EMapLinkTextureStatus::Portable:     ++Agg.NumPortable;     break;
            case EMapLinkTextureStatus::LocalOnly:    ++Agg.NumLocalOnly;    break;
            case EMapLinkTextureStatus::Unclassified: ++Agg.NumUnclassified; break;
            case EMapLinkTextureStatus::Broken:       ++Agg.NumBroken;       break;
            }
        }
    }

    // §6.5 — a MapLink-class texture under the textures folder that no
    // MapLink-owned MI references. Non-blocking status, never auto-deleted.
    for (FMapLinkTextureRow& Row : InOut.Rows)
    {
        Row.bOrphan = Row.TextureClass == EMapLinkTextureClass::MapLink
                   && Row.ReferencingMaterials.Num() == 0
                   && FMapLinkTextureNaming::IsTexturesPackagePath(Row.PackagePath);
        if (Row.bOrphan)
        {
            ++InOut.NumOrphans;
        }
    }
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

const FMapLinkTextureScanResult& FMapLinkTextureScanner::Scan(bool bForceFullRecompute)
{
    const double Started = FPlatformTime::Seconds();

    FMapLinkTexturePolicy::EnsureLoaded();

    GLastResult = FMapLinkTextureScanResult();

    TMap<FString, TArray<FString>> ProvisionalIndex;
    EnumerateFast(GLastResult, ProvisionalIndex);
    ResolvePrecise(GLastResult, bForceFullRecompute);
    DropReleasedRows(GLastResult);
    BuildAggregates(GLastResult);

    // The scan records the sRGB and compression it read; persist those so the
    // policy file stays a complete picture even for textures nobody has
    // classified yet. With no project root (a fresh project, before MapLink >
    // Project is set) there is nowhere to persist to; the scan is still valid,
    // so stay quiet instead of logging an error on every editor start. User
    // actions that need the file still report the missing root via Save().
    if (!FMapLinkTexturePolicy::GetPolicyFilePath().IsEmpty())
    {
        FMapLinkTexturePolicy::Save();
    }

    GLastResult.ScanSeconds  = FPlatformTime::Seconds() - Started;
    GLastResult.ScannedAtUtc = FDateTime::UtcNow();
    GHasResult = true;

    UE_LOG(LogMapLink, Log,
           TEXT("[Textures] Scan: %d textures in %.2fs — %d conform, "
                "%d portable, %d local-only, %d unclassified, %d broken, "
                "%d orphans (%d packages loaded)"),
           GLastResult.Total(), GLastResult.ScanSeconds,
           GLastResult.NumConform, GLastResult.NumPortable,
           GLastResult.NumLocalOnly, GLastResult.NumUnclassified,
           GLastResult.NumBroken, GLastResult.NumOrphans,
           GLastResult.NumPackagesLoaded);

    if (GLastResult.NumReleased > 0)
    {
        UE_LOG(LogMapLink, Log,
               TEXT("[Textures] %d texture(s) released: a package still names "
                    "them but no MapLink material uses them. Not listed. They "
                    "remain in the Content Browser, where they belong to "
                    "whoever owns that folder."), GLastResult.NumReleased);
    }

    return GLastResult;
}

const FMapLinkTextureScanResult& FMapLinkTextureScanner::Rescan()
{
    if (UMapLinkEditorState* State = UMapLinkEditorState::Get())
    {
        State->ClearTextureCache();
    }
    FMapLinkTexturePolicy::Invalidate();
    return Scan(/*bForceFullRecompute=*/true);
}

const FMapLinkTextureScanResult& FMapLinkTextureScanner::GetLastResult()
{
    return GLastResult;
}

bool FMapLinkTextureScanner::HasResult()
{
    return GHasResult;
}

const FMapLinkTextureRow* FMapLinkTextureScanner::FindRow(const FString& TextureAssetPath)
{
    if (const int32* Index = GLastResult.RowIndexByAssetPath.Find(TextureAssetPath))
    {
        return GLastResult.Rows.IsValidIndex(*Index) ? &GLastResult.Rows[*Index] : nullptr;
    }
    return nullptr;
}

FMapLinkTextureAggregate FMapLinkTextureScanner::GetMaterialAggregate(
    const FString& MaterialAssetPath)
{
    if (const FMapLinkTextureAggregate* Found =
            GLastResult.MaterialAggregates.Find(MaterialAssetPath))
    {
        return *Found;
    }
    return FMapLinkTextureAggregate();
}

// ---------------------------------------------------------------------------
// Suggestion — §6.4
// ---------------------------------------------------------------------------

EMapLinkTextureClass FMapLinkTextureScanner::EffectiveClass(const FString& AssetPath)
{
    const EMapLinkTextureClass Recorded = FMapLinkTexturePolicy::GetClass(AssetPath);
    return Recorded != EMapLinkTextureClass::Unclassified
        ? Recorded
        : SuggestClass(AssetPath);
}

EMapLinkTextureClass FMapLinkTextureScanner::SuggestClass(const FString& AssetPath)
{
    // Under MapLink's own content root, MapLink owns it.
    if (AssetPath.StartsWith(FMapLinkUtils::GetMapLinkContentRoot(), ESearchCase::IgnoreCase))
    {
        return EMapLinkTextureClass::MapLink;
    }

    // EVERYTHING ELSE IS EXTERNAL.
    //
    // §6.4 originally said "/Engine/, /Megascans/ or a plugin mount ->
    // External; everything else -> Unclassified". Against a real project that
    // rule suggests nothing: production textures live in /Game/<whatever>/,
    // which is neither MapLink's folder nor a plugin mount, so the suggester
    // returned Unclassified for 242 of 244 textures and left the user with no
    // route other than classifying each one by hand.
    //
    // The corrected rule matches what the classes actually mean. MapLink owns
    // what is in MapLink's folder; every other texture is somebody else's until
    // a human decides to claim it, which is precisely External — "third-party,
    // accepted as-is, MapLink never writes the original". Suggesting that is
    // both true and safe: External is the read-only class, so accepting every
    // suggestion cannot damage anything.
    //
    // Unclassified stays what it always was — the state before anyone decided —
    // rather than doubling as a suggestion, which it never usefully was.
    return EMapLinkTextureClass::External;
}

int32 FMapLinkTextureScanner::ApplySuggestions(bool bExecute,
                                               TArray<FString>& OutChanged)
{
    OutChanged.Reset();

    if (!GHasResult)
    {
        Scan();
    }

    for (const FMapLinkTextureRow& Row : GLastResult.Rows)
    {
        if (Row.SuggestedClass == EMapLinkTextureClass::Unclassified)
        {
            continue;
        }
        if (Row.TextureClass == Row.SuggestedClass)
        {
            continue;
        }

        OutChanged.Add(FString::Printf(TEXT("%s: %s -> %s"),
            *Row.AssetPath,
            MapLinkTexture::ToString(Row.TextureClass),
            MapLinkTexture::ToString(Row.SuggestedClass)));

        if (bExecute)
        {
            FMapLinkTexturePolicy::SetClass(Row.AssetPath, Row.SuggestedClass);
        }
    }

    if (bExecute && OutChanged.Num() > 0)
    {
        FMapLinkTexturePolicy::Save();
    }

    return OutChanged.Num();
}

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------

FString FMapLinkTextureScanner::BuildReport(const FMapLinkTextureScanResult& Result,
                                            bool bVerbose)
{
    TStringBuilder<8192> Out;

    Out.Appendf(TEXT("MapLink textures — %d total, scanned in %.2fs\n"),
                Result.Total(), Result.ScanSeconds);
    Out.Appendf(TEXT("  %4d conform       (MapLink owns it, container correct)\n"),
                Result.NumConform);
    Out.Appendf(TEXT("  %4d plugin        (ships with MapLink, never conformed)\n"),
                Result.NumPlugin);
    Out.Appendf(TEXT("  %4d portable      (inside the project tree)\n"),
                Result.NumPortable);
    Out.Appendf(TEXT("  %4d LOCAL ONLY    (opens on this machine and nowhere else)\n"),
                Result.NumLocalOnly);
    Out.Appendf(TEXT("  %4d unclassified  (no decision recorded)\n"),
                Result.NumUnclassified);
    Out.Appendf(TEXT("  %4d broken        (no source data or file missing)\n"),
                Result.NumBroken);
    Out.Appendf(TEXT("  %4d orphans       (MapLink-class, referenced by nothing)\n"),
                Result.NumOrphans);

    if (Result.SkippedUnsupported.Num() > 0)
    {
        Out.Appendf(TEXT("\n%d asset(s) skipped as unsupported types:\n"),
                    Result.SkippedUnsupported.Num());
        for (const FString& Skipped : Result.SkippedUnsupported)
        {
            Out.Appendf(TEXT("  - %s\n"), *Skipped);
        }
    }

    // The reverse index, as a work plan rather than a list of symptoms. One bad
    // texture on a shared parent shows up as fifteen bad materials; this is
    // what turns that back into one fix.
    TArray<const FMapLinkTextureRow*> Problems;
    for (const FMapLinkTextureRow& Row : Result.Rows)
    {
        if (Row.Status == EMapLinkTextureStatus::LocalOnly
            || Row.Status == EMapLinkTextureStatus::Broken
            || Row.Status == EMapLinkTextureStatus::Unclassified)
        {
            Problems.Add(&Row);
        }
    }
    Problems.Sort([](const FMapLinkTextureRow& A, const FMapLinkTextureRow& B)
    {
        return A.ReferencingMaterials.Num() > B.ReferencingMaterials.Num();
    });

    if (Problems.Num() > 0)
    {
        // The Output Log is not a report surface. On a real project this list
        // is hundreds of rows deep, and four lines each buries everything else
        // that happened in the session. Show the head — which is the part that
        // matters, because the list is sorted by how many materials each
        // texture costs — and say plainly how much was left out. Silent
        // truncation would read as "that's all of them".
        const int32 DetailCap = bVerbose ? Problems.Num()
                                         : FMath::Min(Problems.Num(), 25);

        Out.Appendf(TEXT("\nNeeds attention, most-referenced first"));
        if (DetailCap < Problems.Num())
        {
            Out.Appendf(TEXT(" (showing %d of %d — add -verbose for all)"),
                        DetailCap, Problems.Num());
        }
        Out.Appendf(TEXT(":\n"));

        Problems.SetNum(DetailCap);
        for (const FMapLinkTextureRow* Row : Problems)
        {
            Out.Appendf(TEXT("  [%s] %s\n"),
                        MapLinkTexture::ToString(Row->Status), *Row->AssetPath);
            if (Row->ReferencingMaterials.Num() > 0)
            {
                Out.Appendf(TEXT("      referenced by %d material instance(s) — "
                                 "fix once\n"),
                            Row->ReferencingMaterials.Num());
            }
            if (!Row->SourceDiskPath.IsEmpty())
            {
                Out.Appendf(TEXT("      source: %s\n"), *Row->SourceDiskPath);
            }
            if (!Row->Remedy.IsEmpty())
            {
                Out.Appendf(TEXT("      %s\n"), *Row->Remedy);
            }
            if (bVerbose)
            {
                for (const FString& Issue : Row->Issues)
                {
                    Out.Appendf(TEXT("      · %s\n"), *Issue);
                }
                for (const FString& Material : Row->ReferencingMaterials)
                {
                    Out.Appendf(TEXT("        <- %s\n"), *Material);
                }
            }
        }
    }

    if (Result.IsClean())
    {
        Out.Appendf(TEXT("\nAll textures conform.\n"));
    }

    return Out.ToString();
}
