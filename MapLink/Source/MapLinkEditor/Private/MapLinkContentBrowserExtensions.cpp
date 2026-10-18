// Copyright MapModelers. All Rights Reserved.

#include "MapLinkContentBrowserExtensions.h"
#include "MapLinkNotify.h"

#include "MapLinkBPBinding.h"
#include "MapLinkBPStandInCreator.h"
#include "MapLinkBlenderBridge.h"
#include "MapLinkSettings.h"
#include "MapLinkStandInReport.h"
#include "MapLinkUtils.h"
#include "MapLinkPipeline.h"  // LogMapLink

#include "ToolMenus.h"
#include "ToolMenuSection.h"
#include "ToolMenuEntry.h"
#include "ToolMenuMisc.h"
#include "ContentBrowserMenuContexts.h"

#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "AssetRegistry/AssetData.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"
#include "Misc/MessageDialog.h"

#define LOCTEXT_NAMESPACE "MapLinkContentBrowserExtensions"

// ===========================================================================
// State / helpers
// ===========================================================================

namespace
{
    static const FName MapLinkMenuOwner = TEXT("MapLinkContentBrowserExtensions");
    static bool bRegistered = false;

    /** Pull the first selected UBlueprint from the context. */
    UBlueprint* GetSelectedBlueprint(const FToolMenuContext& Ctx)
    {
        UContentBrowserAssetContextMenuContext* AssetCtx =
            Ctx.FindContext<UContentBrowserAssetContextMenuContext>();
        if (!AssetCtx) return nullptr;
        for (const FAssetData& AD : AssetCtx->SelectedAssets)
        {
            UObject* Obj = AD.GetAsset();
            if (UBlueprint* BP = Cast<UBlueprint>(Obj))
            {
                return BP;
            }
        }
        return nullptr;
    }

    bool IsActorDerivedBlueprint(UBlueprint* BP)
    {
        return BP
            && BP->ParentClass
            && BP->ParentClass->IsChildOf(AActor::StaticClass());
    }

    bool IsInPluginMapLinkContent(UBlueprint* BP)
    {
        return BP && BP->GetPathName().StartsWith(TEXT("/MapLink/"));
    }

    bool HasBinding(UBlueprint* BP)
    {
        return UMapLinkBPBinding::Get(BP) != nullptr;
    }

    FString GetStandInPath(UBlueprint* BP)
    {
        if (UMapLinkBPBinding* B = UMapLinkBPBinding::Get(BP))
        {
            return B->StandInBlendPath;
        }
        return FString();
    }

    bool StandInExists(UBlueprint* BP)
    {
        const FString P = GetStandInPath(BP);
        return !P.IsEmpty() && FPaths::FileExists(P);
    }

    // -----------------------------------------------------------------
    // Action handlers
    // -----------------------------------------------------------------

    void ExecCreate(const FToolMenuContext Ctx)
    {
        UBlueprint* BP = GetSelectedBlueprint(Ctx);
        if (!BP) return;
        FMapLinkStandInReport Report(TEXT("create"), BP->GetPathName());
        const bool bOk = FMapLinkBPStandInCreator::CreateStandIn(
            BP, /*bForceOverwrite=*/false, Report);
        if (bOk)
        {
            MapLinkNotify::Success(FText::Format(
                LOCTEXT("CreatedOk",
                        "MapLink stand-in created.\n"
                        "Blend: {0}"),
                FText::FromString(GetStandInPath(BP))));
        }
        else
        {
            FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
                LOCTEXT("CreatedFailed",
                        "MapLink stand-in creation finished with issues.\n\n"
                        "See report:\n{0}"),
                FText::FromString(Report.GetReportPath())));
        }
    }

    void ExecRefresh(const FToolMenuContext Ctx)
    {
        UBlueprint* BP = GetSelectedBlueprint(Ctx);
        if (!BP) return;
        FMapLinkStandInReport Report(TEXT("refresh"), BP->GetPathName());
        const bool bOk = FMapLinkBPStandInCreator::RefreshStandIn(BP, Report);
        if (bOk)
        {
            MapLinkNotify::Success(
                LOCTEXT("RefreshOkTitle", "Stand-in refreshed"));
        }
        else
        {
            FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
                LOCTEXT("RefreshDetail", "{0}\n\nReport:\n{1}"),
                LOCTEXT("RefreshFailedTitle", "Refresh finished with issues"),
                FText::FromString(Report.GetReportPath())));
        }
    }

    void ExecOpenInBlender(const FToolMenuContext Ctx)
    {
        UBlueprint* BP = GetSelectedBlueprint(Ctx);
        if (!BP) return;
        const FString Blend = GetStandInPath(BP);
        if (Blend.IsEmpty() || !FPaths::FileExists(Blend))
        {
            FMessageDialog::Open(EAppMsgType::Ok,
                LOCTEXT("OpenMissing",
                        "Stand-in .blend not found on disk."));
            return;
        }
        const FString BlenderExe =
            FMapLinkBlenderBridge::GetBlenderExecutablePath();
        if (BlenderExe.IsEmpty() || !FPaths::FileExists(BlenderExe))
        {
            FMessageDialog::Open(EAppMsgType::Ok,
                LOCTEXT("OpenNoBlender",
                        "Blender executable not configured."));
            return;
        }
        const FString Args = FString::Printf(TEXT("\"%s\""), *Blend);
        FProcHandle Proc = FPlatformProcess::CreateProc(
            *BlenderExe, *Args,
            /*bLaunchDetached=*/true,
            /*bLaunchHidden=*/false,
            /*bLaunchReallyHidden=*/false,
            nullptr, 0, nullptr, nullptr);
        if (Proc.IsValid())
        {
            FPlatformProcess::CloseProc(Proc);
            UE_LOG(LogMapLinkBPStandIn, Log,
                   TEXT("Launched Blender (detached) with: %s"), *Blend);
        }
    }

    void ExecDelete(const FToolMenuContext Ctx)
    {
        UBlueprint* BP = GetSelectedBlueprint(Ctx);
        if (!BP) return;
        FMapLinkStandInReport Report(
            TEXT("delete"), BP ? BP->GetPathName() : FString());
        const bool bOk = FMapLinkBPStandInCreator::DeleteStandIn(
            BP, /*bAlreadyConfirmed=*/false, Report);
        Report.WriteToFile();
        UE_LOG(LogMapLinkBPStandIn, Log,
               TEXT("Delete MapLink stand-in for '%s': result=%s"),
               *BP->GetName(),
               bOk ? TEXT("ok") : TEXT("failed_or_skipped"));
    }

    void ExecShowInExplorer(const FToolMenuContext Ctx)
    {
        UBlueprint* BP = GetSelectedBlueprint(Ctx);
        if (!BP) return;
        const FString Blend = GetStandInPath(BP);
        if (Blend.IsEmpty() || !FPaths::FileExists(Blend))
        {
            FMessageDialog::Open(EAppMsgType::Ok,
                LOCTEXT("ShowMissing",
                        "Stand-in .blend not found on disk."));
            return;
        }
        FPlatformProcess::ExploreFolder(*Blend);
    }

    // -----------------------------------------------------------------
    // Visibility predicates wrapping the per-asset checks
    // -----------------------------------------------------------------

    bool VisCreate(const FToolMenuContext& Ctx)
    {
        UBlueprint* BP = GetSelectedBlueprint(Ctx);
        return IsActorDerivedBlueprint(BP)
            && !IsInPluginMapLinkContent(BP)
            && !HasBinding(BP)
            && BP->GetName() != TEXT("BP_Deprecated");
    }
    bool VisRefresh(const FToolMenuContext& Ctx)
    {
        UBlueprint* BP = GetSelectedBlueprint(Ctx);
        return BP && StandInExists(BP);
    }
    bool VisOpen(const FToolMenuContext& Ctx)    { return VisRefresh(Ctx); }
    bool VisDelete(const FToolMenuContext& Ctx)
    {
        UBlueprint* BP = GetSelectedBlueprint(Ctx);
        return BP && HasBinding(BP);
    }
    bool VisShowExplorer(const FToolMenuContext& Ctx)
    {
        UBlueprint* BP = GetSelectedBlueprint(Ctx);
        return BP && StandInExists(BP);
    }

    // -----------------------------------------------------------------
    // Submenu builder
    // -----------------------------------------------------------------

    void BuildMapLinkSubmenu(UToolMenu* InMenu)
    {
        FToolMenuSection& Sec = InMenu->AddSection(
            TEXT("MapLinkBPStandIn"),
            LOCTEXT("MapLinkBPSection", "BP Stand-in"));

        // 1) Create
        Sec.AddDynamicEntry(
            TEXT("Create"),
            FNewToolMenuSectionDelegate::CreateLambda(
                [](FToolMenuSection& Section)
                {
                    if (!VisCreate(Section.Context)) return;
                    Section.AddMenuEntry(
                        TEXT("MapLink_CreateStandIn"),
                        LOCTEXT("Create_Label",   "Create MapLink Stand-in"),
                        LOCTEXT("Create_Tooltip",
                                "Generate a Blender stand-in .blend that "
                                "represents this BP for MapLink import."),
                        FSlateIcon(),
                        FExecuteAction::CreateStatic(&ExecCreate, Section.Context));
                }));

        // 2) Refresh
        Sec.AddDynamicEntry(
            TEXT("Refresh"),
            FNewToolMenuSectionDelegate::CreateLambda(
                [](FToolMenuSection& Section)
                {
                    if (!VisRefresh(Section.Context)) return;
                    Section.AddMenuEntry(
                        TEXT("MapLink_RefreshStandIn"),
                        LOCTEXT("Refresh_Label",  "Refresh MapLink Stand-in"),
                        LOCTEXT("Refresh_Tooltip",
                                "Re-author the stand-in .blend from the BP's "
                                "current SCS (overwrites the existing file)."),
                        FSlateIcon(),
                        FExecuteAction::CreateStatic(&ExecRefresh, Section.Context));
                }));

        // 3) Open in Blender
        Sec.AddDynamicEntry(
            TEXT("Open"),
            FNewToolMenuSectionDelegate::CreateLambda(
                [](FToolMenuSection& Section)
                {
                    if (!VisOpen(Section.Context)) return;
                    Section.AddMenuEntry(
                        TEXT("MapLink_OpenStandIn"),
                        LOCTEXT("Open_Label",     "Open Stand-in in Blender"),
                        LOCTEXT("Open_Tooltip",
                                "Launch Blender (detached) with the stand-in "
                                ".blend loaded."),
                        FSlateIcon(),
                        FExecuteAction::CreateStatic(&ExecOpenInBlender, Section.Context));
                }));

        // 4) Delete  (v1.8 D7 — replaces "Unlink"; does the full cleanup)
        Sec.AddDynamicEntry(
            TEXT("Delete"),
            FNewToolMenuSectionDelegate::CreateLambda(
                [](FToolMenuSection& Section)
                {
                    if (!VisDelete(Section.Context)) return;
                    Section.AddMenuEntry(
                        TEXT("MapLink_DeleteStandIn"),
                        LOCTEXT("Delete_Label",   "Delete MapLink Stand-in"),
                        LOCTEXT("Delete_Tooltip",
                                "Remove the binding, p4-delete the stand-in "
                                ".blend + sidecars, and purge every asset "
                                "under /Game/MapLink/<BPName>/ (proxy + any "
                                "orphan SM folder + any synthesized BP)."),
                        FSlateIcon(),
                        FExecuteAction::CreateStatic(&ExecDelete, Section.Context));
                }));

        // 5) Show in Explorer
        Sec.AddDynamicEntry(
            TEXT("ShowExplorer"),
            FNewToolMenuSectionDelegate::CreateLambda(
                [](FToolMenuSection& Section)
                {
                    if (!VisShowExplorer(Section.Context)) return;
                    Section.AddMenuEntry(
                        TEXT("MapLink_ShowInExplorer"),
                        LOCTEXT("Show_Label",     "Show Stand-in in Explorer"),
                        LOCTEXT("Show_Tooltip",
                                "Open the folder containing the stand-in "
                                ".blend in your file manager."),
                        FSlateIcon(),
                        FExecuteAction::CreateStatic(&ExecShowInExplorer, Section.Context));
                }));
    }

    /** Visibility for the parent "MapLink" submenu — hide entirely if
     *  no entries would be visible (avoids an empty submenu). */
    bool VisAnyEntry(const FToolMenuContext& Ctx)
    {
        return VisCreate(Ctx) || VisRefresh(Ctx) || VisOpen(Ctx)
            || VisDelete(Ctx) || VisShowExplorer(Ctx);
    }
}

// ===========================================================================
// Register / Unregister
// ===========================================================================

void FMapLinkContentBrowserExtensions::Register()
{
    if (bRegistered) return;

    UToolMenus::RegisterStartupCallback(
        FSimpleMulticastDelegate::FDelegate::CreateLambda([]()
        {
            FToolMenuOwnerScoped OwnerScope(MapLinkMenuOwner);

            UToolMenu* BPContext = UToolMenus::Get()->ExtendMenu(
                TEXT("ContentBrowser.AssetContextMenu.Blueprint"));
            if (!BPContext) return;

            FToolMenuSection& Section = BPContext->FindOrAddSection(
                TEXT("MapLink"));

            Section.AddDynamicEntry(
                TEXT("MapLinkSubmenu"),
                FNewToolMenuSectionDelegate::CreateLambda(
                    [](FToolMenuSection& Sec)
                    {
                        if (!VisAnyEntry(Sec.Context)) return;
                        Sec.AddSubMenu(
                            TEXT("MapLink"),
                            LOCTEXT("MapLinkSubmenu_Label",   "MapLink"),
                            LOCTEXT("MapLinkSubmenu_Tooltip",
                                    "MapLink BP Reference Mode actions"),
                            FNewToolMenuDelegate::CreateStatic(
                                &BuildMapLinkSubmenu));
                    }));
        }));

    bRegistered = true;
    UE_LOG(LogMapLinkBPStandIn, Log,
           TEXT("Content Browser extensions registered."));
}

void FMapLinkContentBrowserExtensions::Unregister()
{
    if (!bRegistered) return;
    UToolMenus::UnregisterOwner(MapLinkMenuOwner);
    bRegistered = false;
    UE_LOG(LogMapLinkBPStandIn, Log,
           TEXT("Content Browser extensions unregistered."));
}

#undef LOCTEXT_NAMESPACE
