// Copyright MapModelers. All Rights Reserved.
//
// v1.7.3 — Added "Edit source .blend" and "Open in Explorer" context menu entries.
// v1.9   — WP expansion moved to the MapLink Manager window; context menu
//          entries replaced by "Open MapLink Manager".

#include "MapLinkAssetActions.h"
#include "MapLinkProxyAsset.h"
#include "MapLinkPipeline.h"
#include "MapLinkBlenderBridge.h"
#include "MapLinkEditorModule.h"
#include "Framework/Docking/TabManager.h"   // v1.9 — open Manager tab
#include "ToolMenus.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Misc/MessageDialog.h"
#include "Dialog/SCustomDialog.h"   // v1.14 — Force Full Reimport confirm
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "MapLinkAssetActions"

FText FAssetTypeActions_MapLinkProxy::GetName() const
{
    return LOCTEXT("Name", "MapLink Proxy");
}

FColor FAssetTypeActions_MapLinkProxy::GetTypeColor() const
{
    // Slate #7B8391, the same value the Blender side uses for PROXY.
    //
    // Was a saturated orange, which sat right on top of the LI_ amber the type
    // icons use and read as "Level" at a glance. A proxy is not a level; it is
    // the handle onto one. Slate is deliberately the quietest colour in the
    // palette, because the proxy is the one type whose row you are meant to
    // look past.
    return FColor(0x7B, 0x83, 0x91);
}

UClass* FAssetTypeActions_MapLinkProxy::GetSupportedClass() const
{
    return UMapLinkProxyAsset::StaticClass();
}

uint32 FAssetTypeActions_MapLinkProxy::GetCategories()
{
    return EAssetTypeCategories::Misc;
}

void FAssetTypeActions_MapLinkProxy::OpenAssetEditor(
    const TArray<UObject*>& InObjects,
    TSharedPtr<IToolkitHost> EditWithinLevelEditor)
{
    TArray<UObject*> ProxiesToOpenAsProperties;

    for (UObject* Obj : InObjects)
    {
        UMapLinkProxyAsset* Proxy = Cast<UMapLinkProxyAsset>(Obj);
        if (!Proxy) continue;

        // v1.8 — Reference-mode proxies always open in the property
        // editor.  The user can click the BoundBlueprint soft reference
        // inside the property panel to navigate to the actual bound BP.
        // We do NOT follow PrimaryAsset here even if it's set (stale
        // pre-hotfix2 proxies may still have it pointed at the bound BP).
        if (Proxy->bIsBPReference)
        {
            ProxiesToOpenAsProperties.Add(Obj);
            continue;
        }

        // Legacy path: follow PrimaryAsset (synthesized BP/Level)
        if (Proxy->PrimaryAsset.IsValid())
        {
            UObject* Primary = Proxy->PrimaryAsset.LoadSynchronous();
            if (Primary && !Primary->IsA<UMapLinkProxyAsset>())
            {
                GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Primary);
                continue;
            }
        }

        // Fallback: open proxy in the simple property editor
        // (shows WP Expansion settings, blend path, hash, etc.)
        ProxiesToOpenAsProperties.Add(Obj);
    }

    // Base class opens FSimpleAssetEditor — no recursion
    if (ProxiesToOpenAsProperties.Num() > 0)
    {
        FAssetTypeActions_Base::OpenAssetEditor(ProxiesToOpenAsProperties, EditWithinLevelEditor);
    }
}

bool FAssetTypeActions_MapLinkProxy::HasActions(const TArray<UObject*>& InObjects) const
{
    return true;
}

void FAssetTypeActions_MapLinkProxy::GetActions(
    const TArray<UObject*>& InObjects,
    FToolMenuSection& Section)
{
    TArray<TWeakObjectPtr<UObject>> WeakObjects;
    for (UObject* Obj : InObjects)
    {
        WeakObjects.Add(Obj);
    }

    // ── v1.7.3 — Edit source .blend / Open in Explorer ───────────
    // Available for all proxy types (LI_, BP_, SM_)

    // Empty when the art isn't present; the entries then draw without an icon.
    const FSlateIcon MenuIcon =
        FMapLinkEditorModule::GetIcon("MapLink.MenuIcon");

    Section.AddMenuEntry(
        "MapLink_EditSourceBlend",
        LOCTEXT("EditSourceBlend", "Edit source .blend"),
        LOCTEXT("EditSourceBlendTooltip",
                "Open the source .blend file in Blender (GUI)."),
        MenuIcon,
        FUIAction(FExecuteAction::CreateRaw(
            this, &FAssetTypeActions_MapLinkProxy::ExecuteEditSourceBlend, WeakObjects))
    );

    Section.AddMenuEntry(
        "MapLink_OpenInExplorer",
        LOCTEXT("OpenInExplorer", "Open source .blend in Explorer"),
        LOCTEXT("OpenInExplorerTooltip",
                "Open the folder containing the source .blend in Windows Explorer."),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateRaw(
            this, &FAssetTypeActions_MapLinkProxy::ExecuteOpenInExplorer, WeakObjects))
    );

    // ── Import / Reimport ─────────────────────────────────────────

    Section.AddMenuEntry(
        "MapLink_Import",
        LOCTEXT("Import", "Import"),
        LOCTEXT("ImportTooltip", "Run a full MapLink import for this .blend file."),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateRaw(
            this, &FAssetTypeActions_MapLinkProxy::ExecuteImport, WeakObjects))
    );

    Section.AddMenuEntry(
        "MapLink_Reimport",
        LOCTEXT("Reimport", "Reimport"),
        LOCTEXT("ReimportTooltip",
                "Reimport this .blend, updating only changed dependencies."),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateRaw(
            this, &FAssetTypeActions_MapLinkProxy::ExecuteReimport, WeakObjects))
    );

    // v1.14 — same semantics as MapLink menu > Reimport > Force Full
    // Reimport (whole tree, hashes ignored), with an explicit confirm.
    Section.AddMenuEntry(
        "MapLink_ForceReimport",
        LOCTEXT("ForceReimport", "Force Full Reimport"),
        LOCTEXT("ForceReimportTooltip",
                "Re-import this .blend's whole tree, ignoring hashes "
                "(nuke-and-recreate; confirmation shown)."),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateRaw(
            this, &FAssetTypeActions_MapLinkProxy::ExecuteForceReimport,
            WeakObjects))
    );

    // ── MapLink Manager (v1.9) ──────────────────────────────
    // WP expansion is driven from the Manager window now.

    Section.AddMenuEntry(
        "MapLink_OpenManager",
        LOCTEXT("OpenManager", "Open MapLink Manager"),
        LOCTEXT("OpenManagerTooltip",
                "Open the MapLink Manager window (asset states, reimport,\n"
                "WP expansion, settings)."),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateRaw(
            this, &FAssetTypeActions_MapLinkProxy::ExecuteOpenManager, WeakObjects))
    );
}

// ---------------------------------------------------------------------------
// Action handlers
// ---------------------------------------------------------------------------

void FAssetTypeActions_MapLinkProxy::ExecuteEditSourceBlend(
    TArray<TWeakObjectPtr<UObject>> Objects)
{
    for (const TWeakObjectPtr<UObject>& WeakObj : Objects)
    {
        if (UMapLinkProxyAsset* Proxy = Cast<UMapLinkProxyAsset>(WeakObj.Get()))
        {
            FMapLinkBlenderBridge::OpenBlenderGUI(Proxy->BlendFilePath);
        }
    }
}

void FAssetTypeActions_MapLinkProxy::ExecuteOpenInExplorer(
    TArray<TWeakObjectPtr<UObject>> Objects)
{
    for (const TWeakObjectPtr<UObject>& WeakObj : Objects)
    {
        if (UMapLinkProxyAsset* Proxy = Cast<UMapLinkProxyAsset>(WeakObj.Get()))
        {
            FMapLinkBlenderBridge::OpenInExplorer(Proxy->BlendFilePath);
        }
    }
}

void FAssetTypeActions_MapLinkProxy::ExecuteImport(
    TArray<TWeakObjectPtr<UObject>> Objects)
{
    for (const TWeakObjectPtr<UObject>& WeakObj : Objects)
    {
        if (UMapLinkProxyAsset* Proxy = Cast<UMapLinkProxyAsset>(WeakObj.Get()))
        {
            FMapLinkPipeline::RunImport(Proxy->BlendFilePath);
        }
    }
}

void FAssetTypeActions_MapLinkProxy::ExecuteReimport(
    TArray<TWeakObjectPtr<UObject>> Objects)
{
    for (const TWeakObjectPtr<UObject>& WeakObj : Objects)
    {
        if (UMapLinkProxyAsset* Proxy = Cast<UMapLinkProxyAsset>(WeakObj.Get()))
        {
            FMapLinkPipeline::RunReimport(Proxy->BlendFilePath);
        }
    }
}

void FAssetTypeActions_MapLinkProxy::ExecuteForceReimport(
    TArray<TWeakObjectPtr<UObject>> Objects)
{
    // v1.14 — explicit labeled confirm (v1.13 convention), then RunImport
    // (the force path — whole tree, hashes ignored) per proxy.
    int32 Count = 0;
    for (const TWeakObjectPtr<UObject>& WeakObj : Objects)
    {
        if (Cast<UMapLinkProxyAsset>(WeakObj.Get()))
        {
            ++Count;
        }
    }
    if (Count == 0)
    {
        return;
    }

    TSharedRef<SCustomDialog> Dlg =
        SNew(SCustomDialog)
        .Title(LOCTEXT("ForceReimportDlgTitle", "MapLink Force Full Reimport"))
        .Content()
        [
            SNew(STextBlock)
            .WrapTextAt(520.0f)
            .Text(FText::Format(LOCTEXT("ForceReimportDlgFmt",
                "Force Full Reimport {0} asset(s)?\n\n"
                "Every file in each tree is re-imported, ignoring hashes "
                "(nuke-and-recreate)."),
                FText::AsNumber(Count)))
        ]
        .Buttons({
            SCustomDialog::FButton(LOCTEXT("ForceReimportGoBtn",
                                           "Force Full Reimport")),
            SCustomDialog::FButton(LOCTEXT("ForceReimportCancelBtn", "Cancel"))
        });
    if (Dlg->ShowModal() != 0)
    {
        return;
    }

    for (const TWeakObjectPtr<UObject>& WeakObj : Objects)
    {
        if (UMapLinkProxyAsset* Proxy = Cast<UMapLinkProxyAsset>(WeakObj.Get()))
        {
            FMapLinkPipeline::RunImport(Proxy->BlendFilePath);
        }
    }
}

void FAssetTypeActions_MapLinkProxy::ExecuteOpenManager(
    TArray<TWeakObjectPtr<UObject>> Objects)
{
    // v1.9 — the Manager window owns WP expansion now.
    FGlobalTabmanager::Get()->TryInvokeTab(FTabId(TEXT("MapLinkManager")));
}

#undef LOCTEXT_NAMESPACE
