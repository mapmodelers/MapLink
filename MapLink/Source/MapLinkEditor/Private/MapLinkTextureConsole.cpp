// Copyright MapModelers. All Rights Reserved.
//
// v1.16 — Console commands for the texture manager (see header).

#include "MapLinkTextureConsole.h"

#include "MapLinkTextureNaming.h"
#include "MapLinkTextureOps.h"
#include "MapLinkTexturePolicy.h"
#include "MapLinkTextureScanner.h"
#include "MapLinkTextureTypes.h"
#include "MapLinkTgaWriter.h"
#include "MapLinkNotify.h"
#include "MapLinkPipeline.h"   // LogMapLink

#include "EditorAssetLibrary.h"

#include "HAL/IConsoleManager.h"
#include "Misc/MessageDialog.h"

namespace
{
    TArray<TUniquePtr<FAutoConsoleCommand>> GCommands;

    /** Log a multi-line block one line at a time. A single UE_LOG of a
     *  thousand-line report is unreadable in the Output Log and gets clipped
     *  in some views; per-line keeps it copy-pasteable. */
    void LogBlock(const FString& Text)
    {
        TArray<FString> Lines;
        Text.ParseIntoArrayLines(Lines, /*bCullEmpty=*/false);
        for (const FString& Line : Lines)
        {
            UE_LOG(LogMapLink, Display, TEXT("%s"), *Line);
        }
    }

    bool HasFlag(const TArray<FString>& Args, const TCHAR* Flag)
    {
        for (const FString& Arg : Args)
        {
            if (Arg.Equals(Flag, ESearchCase::IgnoreCase))
            {
                return true;
            }
        }
        return false;
    }

    /** First argument that is not a -flag. */
    FString FirstPositional(const TArray<FString>& Args, int32 Skip = 0)
    {
        int32 Seen = 0;
        for (const FString& Arg : Args)
        {
            if (Arg.StartsWith(TEXT("-")))
            {
                continue;
            }
            if (Seen++ == Skip)
            {
                return Arg;
            }
        }
        return FString();
    }

    /** §11 — destructive operations need an explicit execute flag AND a modal
     *  confirm. The flag alone is a typo away from deleting somebody's work. */
    bool ConfirmDestructive(const FString& What)
    {
        const FText Message = FText::FromString(FString::Printf(
            TEXT("%s\n\nThis deletes assets and/or files. It cannot be undone "
                 "from the editor.\n\nContinue?"), *What));
        return FMessageDialog::Open(EAppMsgType::YesNo, Message,
            NSLOCTEXT("MapLink", "TextureDestructiveTitle",
                      "MapLink — destructive texture operation"))
            == EAppReturnType::Yes;
    }

    void RunAndReport(FMapLinkTextureOpReport&& Report)
    {
        LogBlock(FMapLinkTextureOps::BuildReportText(Report));
        FMapLinkTextureOps::NotifySummary(Report);
    }

    // ── Commands ──────────────────────────────────────────────────────────

    void CmdScan(const TArray<FString>& Args)
    {
        const FMapLinkTextureScanResult& Result = FMapLinkTextureScanner::Scan();
        LogBlock(FMapLinkTextureScanner::BuildReport(Result, HasFlag(Args, TEXT("-verbose"))));

        if (Result.IsClean())
        {
            MapLinkNotify::Success(FText::FromString(FString::Printf(
                TEXT("Textures: %d scanned. All conform."), Result.Total())));
        }
        else
        {
            MapLinkNotify::Warn(FText::FromString(FString::Printf(
                TEXT("Textures: %d scanned — %d External, %d missing. "
                     "See the Output Log."),
                Result.Total(), Result.NumLocalOnly, Result.NumBroken)));
        }
    }

    void CmdReport(const TArray<FString>& Args)
    {
        if (!FMapLinkTextureScanner::HasResult())
        {
            UE_LOG(LogMapLink, Display,
                   TEXT("[Textures] Nothing scanned yet — running a scan first."));
            FMapLinkTextureScanner::Scan();
        }
        LogBlock(FMapLinkTextureScanner::BuildReport(
            FMapLinkTextureScanner::GetLastResult(),
            HasFlag(Args, TEXT("-verbose"))));
    }

    void CmdRescan(const TArray<FString>& Args)
    {
        UE_LOG(LogMapLink, Display,
               TEXT("[Textures] Clearing the per-user cache and recomputing "
                    "everything."));
        const FMapLinkTextureScanResult& Result = FMapLinkTextureScanner::Rescan();
        LogBlock(FMapLinkTextureScanner::BuildReport(Result, HasFlag(Args, TEXT("-verbose"))));
    }

    /** Object path, plus a hard stop when it names nothing.
     *
     *  Classifying a path with a typo used to succeed: the policy gained an
     *  entry for an asset that does not exist, P4 gained a diff, and the
     *  texture the user meant stayed Unclassified. Refusing costs one lookup. */
    bool ResolveExistingAsset(const FString& Raw, FString& OutObjectPath)
    {
        OutObjectPath = FMapLinkTextureNaming::NormalizeAssetPath(Raw);
        if (OutObjectPath.IsEmpty())
        {
            return false;
        }
        if (!UEditorAssetLibrary::DoesAssetExist(OutObjectPath))
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("[Textures] No asset at '%s'. Right-click the texture "
                        "in the Content Browser > Copy Reference and paste that."),
                   *OutObjectPath);
            return false;
        }
        return true;
    }

    void CmdClassify(const TArray<FString>& Args)
    {
        const FString RawPath   = FirstPositional(Args, 0);
        const FString ClassName = FirstPositional(Args, 1);

        if (RawPath.IsEmpty() || ClassName.IsEmpty())
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("[Textures] Usage: MapLink.Textures.Classify "
                        "<AssetPath> <MapLink|External|Unclassified>"));
            return;
        }

        FString AssetPath;
        if (!ResolveExistingAsset(RawPath, AssetPath))
        {
            return;
        }

        EMapLinkTextureClass NewClass;
        if (!MapLinkTexture::ParseClass(ClassName, NewClass))
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("[Textures] '%s' is not a class. Use MapLink, External "
                        "or Unclassified."), *ClassName);
            return;
        }

        // A classification IS the decision, so it writes immediately and lands
        // in the P4-tracked policy file. That file is the audit trail.
        FMapLinkTexturePolicy::SetClass(AssetPath, NewClass);
        bool bWrote = false;
        if (FMapLinkTexturePolicy::Save(&bWrote))
        {
            UE_LOG(LogMapLink, Display, TEXT("[Textures] %s -> %s%s"),
                   *AssetPath, MapLinkTexture::ToString(NewClass),
                   bWrote ? TEXT("") : TEXT(" (already recorded)"));
            MapLinkNotify::Success(FText::FromString(FString::Printf(
                TEXT("Texture classified: %s"), MapLinkTexture::ToString(NewClass))));
        }
    }

    void CmdAutoClassify(const TArray<FString>& Args)
    {
        const bool bExecute = HasFlag(Args, TEXT("-execute"));

        TArray<FString> Changed;
        const int32 Count = FMapLinkTextureScanner::ApplySuggestions(bExecute, Changed);

        UE_LOG(LogMapLink, Display,
               TEXT("[Textures] AutoClassify (%s): %d suggestion(s)"),
               bExecute ? TEXT("execute") : TEXT("DRY RUN"), Count);

        // Same reasoning as the scan report: 244 identical lines is not
        // information. -verbose prints them all when you actually want to
        // audit the list.
        const bool bVerbose = HasFlag(Args, TEXT("-verbose"));
        const int32 Cap = bVerbose ? Changed.Num() : FMath::Min(Changed.Num(), 25);
        for (int32 i = 0; i < Cap; ++i)
        {
            UE_LOG(LogMapLink, Display, TEXT("  %s"), *Changed[i]);
        }
        if (Cap < Changed.Num())
        {
            UE_LOG(LogMapLink, Display,
                   TEXT("  … and %d more (add -verbose to list them all)"),
                   Changed.Num() - Cap);
        }

        if (!bExecute && Count > 0)
        {
            UE_LOG(LogMapLink, Display,
                   TEXT("[Textures] Nothing was written. Re-run with -execute "
                        "to accept these into texture_policy.json."));
        }
    }

    void CmdConform(const TArray<FString>& Args)
    {
        const FString Target = FirstPositional(Args, 0);
        const bool bExecute  = HasFlag(Args, TEXT("-execute"));

        if (Target.IsEmpty())
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("[Textures] Usage: MapLink.Textures.Conform "
                        "<AssetPath|all> [-execute]"));
            return;
        }

        if (Target.Equals(TEXT("all"), ESearchCase::IgnoreCase))
        {
            RunAndReport(FMapLinkTextureOps::ConformAll(bExecute));
        }
        else
        {
            FString AssetPath;
            if (!ResolveExistingAsset(Target, AssetPath))
            {
                return;
            }
            TArray<FString> Paths;
            Paths.Add(AssetPath);
            RunAndReport(FMapLinkTextureOps::Conform(Paths, bExecute));
        }
    }

    void CmdClaim(const TArray<FString>& Args)
    {
        const FString RawPath    = FirstPositional(Args, 0);
        const FString PresetName = FirstPositional(Args, 1);
        const bool    bExecute   = HasFlag(Args, TEXT("-execute"));

        if (RawPath.IsEmpty())
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("[Textures] Usage: MapLink.Textures.Claim <AssetPath> "
                        "<Fork|Migrate|Adopt|Copy> [-execute]"));
            return;
        }

        FString AssetPath;
        if (!ResolveExistingAsset(RawPath, AssetPath))
        {
            return;
        }

        EMapLinkTextureClaimPreset Preset = EMapLinkTextureClaimPreset::Fork;
        if (!PresetName.IsEmpty() && !MapLinkTexture::ParsePreset(PresetName, Preset))
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("[Textures] '%s' is not a preset. Use Fork, Migrate, "
                        "Adopt or Copy."), *PresetName);
            return;
        }

        if (bExecute && FMapLinkTextureOps::IsDestructive(Preset))
        {
            if (!ConfirmDestructive(FString::Printf(
                    TEXT("Claim '%s' with preset %s."),
                    *AssetPath, MapLinkTexture::ToString(Preset))))
            {
                UE_LOG(LogMapLink, Display, TEXT("[Textures] Cancelled."));
                return;
            }
        }

        RunAndReport(FMapLinkTextureOps::Claim(AssetPath, Preset, bExecute));
    }

    void CmdPromote(const TArray<FString>& Args)
    {
        const FString RawPath  = FirstPositional(Args, 0);
        const bool    bExecute = HasFlag(Args, TEXT("-execute"));

        if (RawPath.IsEmpty())
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("[Textures] Usage: MapLink.Textures.Promote "
                        "<AssetPath> [-execute]"));
            return;
        }

        FString AssetPath;
        if (!ResolveExistingAsset(RawPath, AssetPath))
        {
            return;
        }
        RunAndReport(FMapLinkTextureOps::Promote(AssetPath, bExecute));
    }

    void CmdOrphans(const TArray<FString>& Args)
    {
        const bool bDelete  = HasFlag(Args, TEXT("-delete"));
        const bool bExecute = HasFlag(Args, TEXT("-execute"));

        if (bDelete && bExecute)
        {
            if (!ConfirmDestructive(TEXT("Delete every orphaned MapLink texture "
                                         "and its source file.")))
            {
                UE_LOG(LogMapLink, Display, TEXT("[Textures] Cancelled."));
                return;
            }
        }

        RunAndReport(FMapLinkTextureOps::Orphans(bDelete, bExecute));
    }

    void CmdSelfTest(const TArray<FString>& Args)
    {
        // Both halves always run — an early return would hide the second
        // failure behind the first, and they are independent.
        const bool bTga    = FMapLinkTgaWriter::RunSelfTest();
        const bool bNaming = FMapLinkTextureNaming::RunSelfTest();

        if (bTga && bNaming)
        {
            MapLinkNotify::Success(NSLOCTEXT("MapLink", "TextureSelfTestPass",
                "Texture self-tests passed (TGA writer + naming)."));
        }
        else
        {
            MapLinkNotify::Fail(NSLOCTEXT("MapLink", "TextureSelfTestFail",
                "Texture self-test FAILED — see the Output Log."));
        }
    }

    void Add(const TCHAR* Name, const TCHAR* Help,
             void (*Handler)(const TArray<FString>&))
    {
        GCommands.Add(MakeUnique<FAutoConsoleCommand>(
            Name, Help,
            FConsoleCommandWithArgsDelegate::CreateStatic(Handler)));
    }
}

void FMapLinkTextureConsole::Register()
{
    if (GCommands.Num() > 0)
    {
        return;   // idempotent
    }

    Add(TEXT("MapLink.Textures.Scan"),
        TEXT("Scan every MapLink texture and report status. [-verbose]"),
        &CmdScan);

    Add(TEXT("MapLink.Textures.Report"),
        TEXT("Re-print the last scan without recomputing. [-verbose]"),
        &CmdReport);

    Add(TEXT("MapLink.Textures.Rescan"),
        TEXT("Clear the per-user cache and recompute everything."),
        &CmdRescan);

    Add(TEXT("MapLink.Textures.Classify"),
        TEXT("MapLink.Textures.Classify <AssetPath> <MapLink|External|Unclassified>"),
        &CmdClassify);

    Add(TEXT("MapLink.Textures.AutoClassify"),
        TEXT("Show suggested classes. [-execute] writes them to texture_policy.json."),
        &CmdAutoClassify);

    Add(TEXT("MapLink.Textures.Conform"),
        TEXT("MapLink.Textures.Conform <AssetPath|all> [-execute]. MapLink-class only; the Textures tab conforms anything. Dry run without -execute."),
        &CmdConform);

    Add(TEXT("MapLink.Textures.Claim"),
        TEXT("MapLink.Textures.Claim <AssetPath> <Fork|Migrate|Adopt|Copy> [-execute]."),
        &CmdClaim);

    Add(TEXT("MapLink.Textures.Promote"),
        TEXT("MapLink.Textures.Promote <AssetPath> [-execute]. External becomes MapLink."),
        &CmdPromote);

    Add(TEXT("MapLink.Textures.Orphans"),
        TEXT("List MapLink textures nothing references. [-delete] [-execute]."),
        &CmdOrphans);

    Add(TEXT("MapLink.Textures.SelfTest"),
        TEXT("Synthetic 4x4 round-trip over the TGA writer's byte layout."),
        &CmdSelfTest);

    UE_LOG(LogMapLink, Log,
           TEXT("[Textures] %d console command(s) registered. Type "
                "MapLink.Textures. in the console to list them."),
           GCommands.Num());
}

void FMapLinkTextureConsole::Unregister()
{
    // TUniquePtr<FAutoConsoleCommand> unregisters on destruction.
    GCommands.Empty();
}
