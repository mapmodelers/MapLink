// Copyright MapModelers. All Rights Reserved.
//
// v1.7.3 — Actor context menu, Blender icon, Alt+PageDown/PageUp,
//           dynamic Reimport menu (bShowInReimportMenu on proxies),
//           Auto-Update with active-level cascade, Material Display.

#include "MapLinkEditorModule.h"
#include "MapLinkNotify.h"
#include "MapLinkDiagnostics.h"       // v1.13 — Copy Diagnostics
#include "Dialog/SCustomDialog.h"      // v1.13 — labeled-button modals
#include "Widgets/Text/STextBlock.h"
#include "MapLinkAssetActions.h"
#include "MapLinkMaterialExporter.h"
#include "MapLinkMaterialDisplay.h"
#include "MapLinkBlenderBridge.h"
#include "MapLinkProjectSetup.h"       // v1.12 — Project lifecycle (New/Open/Unlink)
#include "MapLinkPipeline.h"
#include "MapLinkProxyAsset.h"
#include "MapLinkSettings.h"
#include "MapLinkUtils.h"                    // v1.19 — IsPathUnder
#include "MapLinkMaterialLibraryExchange.h"  // v1.19 — ProcessPendingImport
#include "MapLinkEditorState.h"        // v1.12 — favourites in the Reimport menu
#include "SMapLinkManagerWindow.h"          // v1.9
#include "SMapLinkMaterialsTab.h"           // v1.16 — focus materials on an asset
#include "Viewer/SMapLinkAssetViewer.h"      // v1.11 — Asset Viewer
#include "MapLinkWPExpansionHelper.h"        // v1.9 — cascade descendant walk
#include "Framework/Docking/TabManager.h"   // v1.9
#include "Widgets/Docking/SDockTab.h"       // v1.9

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Modules/ModuleManager.h"
#include "ToolMenus.h"
#include "MapLinkContentBrowserExtensions.h"
#include "MapLinkTextureConsole.h"   // v1.16 — texture manager test surface
#include "MapLinkBPLifecycleHooks.h"        // v1.8 D4
#include "MapLinkActorLock.h"               // v1.15 — selection lock
#include "MapLinkNameDisplay.h"             // v1.15 — asset name overlay
#include "ToolMenuSection.h"
#include "Misc/MessageDialog.h"
#include "HAL/PlatformProcess.h"     // FPlatformProcess::LaunchURL
#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/UICommandList.h"

#include "Editor.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Blueprint.h"
#include "Components/StaticMeshComponent.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "LevelInstance/LevelInstanceSubsystem.h"
#include "LevelInstance/LevelInstanceInterface.h"
#include "EngineUtils.h"                 // TActorIterator
#include "Components/MeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "FileHelpers.h"
#include "LevelEditorViewport.h"     // v1.16 — drill: keep camera, force Unlit
#include "Styling/ISlateStyle.h"     // v1.16 — GetOptionalBrush: does this icon exist?
#include "Styling/SlateTypes.h"      // v1.16 — FButtonStyle / FComboButtonStyle
#include "Brushes/SlateColorBrush.h" // v1.16 — the group's background lift
#include "Widgets/SBoxPanel.h"       // v1.16 — SHorizontalBox
#include "Widgets/Layout/SBorder.h"  // v1.16 — the group background
#include "Widgets/Images/SImage.h"   // v1.16 — button icons
#include "Widgets/Input/SComboButton.h"
#include "Widgets/SNullWidget.h"
#include <initializer_list>
#include "Selection.h"
#include "LevelEditor.h"

#include "Interfaces/IPluginManager.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "FMapLinkEditorModule"

// v1.18 — MapLink documentation URL. Empty until the docs are hosted; the
// Help menu entry stays disabled while it is, so the menu never offers an
// action it cannot perform.
static const TCHAR* MapLinkDocsURL = TEXT("https://docs.mapmodelers.com/maplink/");

// ---------------------------------------------------------------------------
// Statics
// ---------------------------------------------------------------------------

TSharedPtr<FSlateStyleSet> FMapLinkEditorModule::StyleSet;
const FName FMapLinkEditorModule::StyleSetName(TEXT("MapLinkStyle"));
TSet<FName> FMapLinkEditorModule::RegisteredIcons;
FString FMapLinkEditorModule::PreviousLevelPath;

// v1.9 — MapLink Manager tab
const FName FMapLinkEditorModule::ManagerTabName(TEXT("MapLinkManager"));
const FName FMapLinkEditorModule::AssetViewerTabName(TEXT("MapLinkAssetViewer"));

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

FMapLinkCommands::FMapLinkCommands()
    : TCommands<FMapLinkCommands>(
        TEXT("MapLinkEditor"),
        NSLOCTEXT("Contexts", "MapLinkEditor", "MapLink Editor"),
        NAME_None,
        FAppStyle::GetAppStyleSetName())
{
}

void FMapLinkCommands::RegisterCommands()
{
    // FInputChord: (Key, bShift, bCtrl, bAlt, bCmd)
    UI_COMMAND(EditLevelStandalone,
        "Edit Level (Standalone)",
        "Open the selected Level Instance as the active editor level.",
        EUserInterfaceActionType::Button,
        FInputChord(EKeys::PageDown, false, false, true, false));

    UI_COMMAND(ReturnToPreviousLevel,
        "Return to Previous Level",
        "Return to the level that was open before Edit Level.",
        EUserInterfaceActionType::Button,
        FInputChord(EKeys::PageUp, false, false, true, false));
}

// ---------------------------------------------------------------------------
// Actor → Proxy reverse resolution
// ---------------------------------------------------------------------------

namespace MapLinkActorResolve
{
    static UMapLinkProxyAsset* ResolveProxyFromActor(AActor* Actor)
    {
        if (!Actor) return nullptr;
        FString Stem;

        if (ALevelInstance* LI = Cast<ALevelInstance>(Actor))
        {
            FString Path = LI->GetWorldAsset().ToString();
            int32 DotIdx;
            if (Path.FindChar(TEXT('.'), DotIdx)) Path = Path.Left(DotIdx);
            if (Path.StartsWith(TEXT("/Game/MapLink/")))
                Stem = FPaths::GetBaseFilename(Path);
        }
        else if (Actor->GetClass() && Actor->GetClass()->ClassGeneratedBy)
        {
            UBlueprint* BP = Cast<UBlueprint>(Actor->GetClass()->ClassGeneratedBy);
            if (BP)
            {
                FString PkgPath = BP->GetOutermost()->GetName();
                if (PkgPath.StartsWith(TEXT("/Game/MapLink/")))
                {
                    Stem = BP->GetName();
                    if (Stem.EndsWith(TEXT("_GP"))) Stem = Stem.LeftChop(3);
                }
            }
        }
        else if (AStaticMeshActor* SMA = Cast<AStaticMeshActor>(Actor))
        {
            UStaticMeshComponent* SMC = SMA->GetStaticMeshComponent();
            if (SMC && SMC->GetStaticMesh())
            {
                FString MeshPath = SMC->GetStaticMesh()->GetOutermost()->GetName();
                if (MeshPath.StartsWith(TEXT("/Game/MapLink/")))
                {
                    FString After = MeshPath.RightChop(
                        FCString::Strlen(TEXT("/Game/MapLink/")));
                    int32 SlashIdx;
                    Stem = After.FindChar(TEXT('/'), SlashIdx)
                        ? After.Left(SlashIdx) : After;
                }
            }
        }

        if (Stem.IsEmpty()) return nullptr;
        FString ProxyPath = FString::Printf(
            TEXT("/Game/MapLink/%s/%s_Proxy"), *Stem, *Stem);
        return LoadObject<UMapLinkProxyAsset>(nullptr, *ProxyPath);
    }

    static FString GetLevelInstanceMapPath(ALevelInstance* LI)
    {
        if (!LI) return FString();
        FString Path = LI->GetWorldAsset().ToString();
        int32 DotIdx;
        if (Path.FindChar(TEXT('.'), DotIdx)) Path = Path.Left(DotIdx);
        return Path;
    }
}

// ---------------------------------------------------------------------------
// Helpers: find active level's proxy, scan all proxies
// ---------------------------------------------------------------------------

namespace
{
    /** If the current editor level is a MapLink level, return its proxy. */
    UMapLinkProxyAsset* GetActiveMapLinkProxy()
    {
        if (!GEditor) return nullptr;
        UWorld* World = GEditor->GetEditorWorldContext().World();
        if (!World) return nullptr;

        FString LevelPath = World->GetOutermost()->GetName();
        if (!LevelPath.StartsWith(TEXT("/Game/MapLink/"))) return nullptr;

        FString Stem = FPaths::GetBaseFilename(LevelPath);
        FString ProxyPath = FString::Printf(
            TEXT("/Game/MapLink/%s/%s_Proxy"), *Stem, *Stem);
        return LoadObject<UMapLinkProxyAsset>(nullptr, *ProxyPath);
    }

    /** Collect all UMapLinkProxyAsset assets from the registry. */
    TArray<UMapLinkProxyAsset*> GetAllProxies()
    {
        TArray<UMapLinkProxyAsset*> Result;
        FAssetRegistryModule& ARModule =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
        IAssetRegistry& AR = ARModule.Get();

        FARFilter Filter;
        Filter.ClassPaths.Add(UMapLinkProxyAsset::StaticClass()->GetClassPathName());
        Filter.PackagePaths.Add(FName(TEXT("/Game/MapLink")));
        Filter.bRecursivePaths = true;

        TArray<FAssetData> Assets;
        AR.GetAssets(Filter, Assets);

        for (const FAssetData& AD : Assets)
        {
            if (UMapLinkProxyAsset* P = Cast<UMapLinkProxyAsset>(AD.GetAsset()))
                Result.Add(P);
        }
        return Result;
    }
}

// ---------------------------------------------------------------------------
// Style registration (MapLink icon brushes)
// ---------------------------------------------------------------------------

void FMapLinkEditorModule::RegisterStyle()
{
    if (StyleSet.IsValid()) return;

    TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MapLink"));
    if (!Plugin.IsValid())
    {
        // Nothing to resolve paths against. Leave StyleSet null so shutdown
        // has nothing to unregister and every call site falls back.
        return;
    }

    StyleSet = MakeShareable(new FSlateStyleSet(StyleSetName));
    RegisteredIcons.Reset();

    // Always through the plugin, never a hardcoded path -- the plugin can sit
    // in the engine directory or the project directory.
    const FString Res = Plugin->GetBaseDir() / TEXT("Resources");
    StyleSet->SetContentRoot(Res);

    // Each brush is registered only if its PNG is actually on disk.
    //
    // This matters more than it looks. A style key pointing at a missing file
    // still resolves, and Slate then draws an empty rectangle -- which means
    // an incomplete icon set produces blank icons rather than the editor's own
    // ones. Leaving the key unset instead lets FClassIconFinder and
    // FSlateIconFinder do their job. Art drops into Resources/ and lights up
    // with no code change; art that is absent costs nothing.
    // SVG first, PNG second.
    //
    // Slate rasterises vector brushes at the DPI it is actually drawing at, so
    // an SVG stays sharp on a high-DPI monitor and at any editor zoom. A PNG
    // authored for 16px is resampled to whatever size the slot wants, which is
    // what makes the Content Browser icons look soft.
    //
    // nanosvg is what UE rasterises Slate SVG with, and it silently drops
    // <style> blocks, CSS classes, <text>, filter, mask, clip-path and
    // <use>/<symbol>. Vectors destined here therefore need flat filled paths
    // with fill="..." attributes -- which is what the authoring templates in
    // authoring_icons/src/ue/ already enforce.
    //
    // Icon128.png stays raster regardless: the Plugins browser loads that file
    // by name and does not accept a vector.
    //
    // No new include: FSlateVectorImageBrush is declared in the same header as
    // FSlateImageBrush (Brushes/SlateImageBrush.h), which this file already
    // compiles against.
    auto SetBrush = [&Res](const FName Key, const TCHAR* Stem, float Px)
    {
        const FVector2D Size(Px, Px);

        const FString Svg = Res / FString::Printf(TEXT("%s.svg"), Stem);
        if (FPaths::FileExists(Svg))
        {
            StyleSet->Set(Key, new FSlateVectorImageBrush(Svg, Size));
            RegisteredIcons.Add(Key);
            return;
        }

        const FString Png = Res / FString::Printf(TEXT("%s.png"), Stem);
        if (FPaths::FileExists(Png))
        {
            StyleSet->Set(Key, new FSlateImageBrush(Png, Size));
            RegisteredIcons.Add(Key);
        }
    };

    // FClassIconFinder looks these up by convention: the suffix is the class
    // name exactly, with no U prefix.
    SetBrush("ClassIcon.MapLinkProxyAsset",      TEXT("ProxyAsset_16"), 16.f);
    SetBrush("ClassThumbnail.MapLinkProxyAsset", TEXT("ProxyAsset_64"), 64.f);

    // Brand mark. 20px is the size Slate draws a toolbar button at; menu
    // entries take 16.
    SetBrush("MapLink.ToolbarButton", TEXT("MapLinkLogo"), 20.f);
    SetBrush("MapLink.MenuIcon",      TEXT("MapLinkLogo"), 16.f);

    // v1.16 — toolbar action icons.
    SetBrush("MapLink.EditBlend", TEXT("Blend_20"),     20.f);
    // Authored and registered but not yet on any surface. Kept because the art
    // ships and the brush costs nothing; delete both lines if either is
    // abandoned rather than leaving the art orphaned.
    SetBrush("MapLink.Warning",   TEXT("Warning_20"),   20.f);  // reserved
    SetBrush("MapLink.Nuke",      TEXT("Nuke_20"),      20.f);  // reserved
    SetBrush("MapLink.DrillDown", TEXT("DrillDown_20"), 20.f);
    SetBrush("MapLink.DrillUp",   TEXT("DrillUp_20"),   20.f);
    SetBrush("MapLink.Folder",    TEXT("Folder_20"),    20.f);
    SetBrush("MapLink.Dots",      TEXT("Dots_20"),      20.f);

    // The Manager button IS the MapLink mark -- it opens the plugin's main
    // window, so the brand is the honest icon for it. Drawn larger than the
    // actions beside it, and it turns red rather than wearing a badge: at this
    // size a corner overlay is a few pixels and easy to miss, whereas the whole
    // mark changing colour is unmissable at a glance.
    SetBrush("MapLink.Manager", TEXT("MapLinkLogo"), 24.f);
    {
        const FString Logo = Res / TEXT("MapLinkLogo.svg");
        if (FPaths::FileExists(Logo))
        {
            StyleSet->Set("MapLink.ManagerStale",
                new FSlateVectorImageBrush(Logo, FVector2D(24.f, 24.f),
                                           FLinearColor(0.86f, 0.20f, 0.16f)));
            RegisteredIcons.Add("MapLink.ManagerStale");
        }
    }

    // v1.16 — the faint lift behind the MapLink toolbar group. Owned here
    // rather than set through FToolMenuEntry::StyleNameOverride: that name is
    // resolved against the EDITOR's style set, so a style registered in
    // MapLinkStyle was never found and Slate silently fell back to a default
    // with no hover or pressed state. Owning the widget is the only way to
    // paint behind a group.
    StyleSet->Set("MapLink.GroupBackground",
        new FSlateColorBrush(FLinearColor(1.f, 1.f, 1.f, 0.055f)));

    // Reimport and Force Full Reimport are the SAME glyph in two tints: grey
    // for the routine one, orange for the destructive one. Same source file,
    // so they cannot drift apart, and the tint is the only thing saying "this
    // is the dangerous sibling".
    //
    // The art is MapLink's rather than an engine brush because tinting means
    // owning the brush -- an FSlateIcon naming an engine brush takes the
    // engine's colour, which cannot be overridden from here.
    {
        const FString ReimportSvg = Res / TEXT("Reimport_20.svg");
        if (FPaths::FileExists(ReimportSvg))
        {
            const FVector2D Sz(20.f, 20.f);

            StyleSet->Set("MapLink.Reimport",
                new FSlateVectorImageBrush(ReimportSvg, Sz,
                                           FLinearColor(0.72f, 0.74f, 0.76f)));
            RegisteredIcons.Add("MapLink.Reimport");

            StyleSet->Set("MapLink.ReimportForce",
                new FSlateVectorImageBrush(ReimportSvg, Sz,
                                           FLinearColor(0.94f, 0.55f, 0.13f)));
            RegisteredIcons.Add("MapLink.ReimportForce");
        }
    }

    FSlateStyleRegistry::RegisterSlateStyle(*StyleSet);
}


FSlateIcon FMapLinkEditorModule::GetIcon(const FName& Key)
{
    if (StyleSet.IsValid() && RegisteredIcons.Contains(Key))
    {
        return FSlateIcon(StyleSetName, Key);
    }
    // Empty icon: the entry draws without one. Naming a brush the style set
    // does not have would log a lookup failure on every draw instead.
    return FSlateIcon();
}

// ---------------------------------------------------------------------------
// Keyboard shortcut actions
// ---------------------------------------------------------------------------

// Defined below with the rest of the drill view handling; declared here
// because the drill actions are what call it.
namespace MapLinkDrillView
{
    void PushAndDescend(const FTransform& LIToWorld);
    void ArriveInChild();
    bool CanAscend();
    FString PeekParentLevel();
    void ArriveInParent();
}

void FMapLinkEditorModule::EditLevelStandaloneAction()
{
    if (!GEditor) return;
    USelection* Sel = GEditor->GetSelectedActors();
    if (!Sel) return;

    ALevelInstance* LIActor = nullptr;
    for (FSelectionIterator It(*Sel); It; ++It)
    {
        LIActor = Cast<ALevelInstance>(*It);
        if (LIActor) break;
    }
    if (!LIActor) return;

    FString MapPath = MapLinkActorResolve::GetLevelInstanceMapPath(LIActor);
    if (MapPath.IsEmpty()) return;

    if (!FEditorFileUtils::SaveDirtyPackages(true, true, true)) return;

    // The camera is captured in PARENT space and re-expressed in the child's
    // space, because the level instance sits somewhere out in the world while
    // its standalone level is authored around the origin. Without the actor's
    // transform the camera would land kilometres from the geometry.
    MapLinkDrillView::PushAndDescend(LIActor->GetActorTransform());

    FEditorFileUtils::LoadMap(MapPath);

    // Unlit on the way down: a generated LI_ level has no built lighting, so
    // Lit renders it black and unworkable. The mode the user had is remembered
    // per level and handed back on the way up.
    MapLinkDrillView::ArriveInChild();

    UE_LOG(LogMapLink, Log, TEXT("[EditLevel] Opened '%s'."), *MapPath);
}

void FMapLinkEditorModule::ReturnToPreviousLevelAction()
{
    if (!MapLinkDrillView::CanAscend()) return;
    if (!FEditorFileUtils::SaveDirtyPackages(true, true, true)) return;

    const FString Target = MapLinkDrillView::PeekParentLevel();
    if (Target.IsEmpty()) return;

    FEditorFileUtils::LoadMap(Target);

    // Takes the camera you actually navigated to inside the child and lifts it
    // back into parent space, so you surface looking at the same thing rather
    // than snapping back to where you left.
    MapLinkDrillView::ArriveInParent();

    UE_LOG(LogMapLink, Log, TEXT("[EditLevel] Returned to '%s'."), *Target);
}

// ---------------------------------------------------------------------------
// v1.16 — Drill camera / view-mode continuity
//
// LoadMap tears down the world but NOT the viewport client, so the camera can
// simply be read before and written after. The view mode is remembered across
// a descend so drilling up hands back whatever the user was in, rather than
// leaving them in the Unlit the descend forced.
// ---------------------------------------------------------------------------

namespace MapLinkDrillView
{
    /** One level of descent. The stack is what makes drilling work more than
     *  one level deep: each frame remembers the level to come back to, the
     *  transform that maps that level's space to the child's, and the view
     *  mode to hand back. */
    struct FDrillFrame
    {
        FString        ParentLevel;
        FTransform     LIToWorld  = FTransform::Identity;
        EViewModeIndex ViewMode   = VMI_Lit;
        bool           bHasViewMode = false;

        /** The parent's camera, exactly as it stood at the moment of descent.
         *
         *  Restored verbatim on the way up rather than transforming the child
         *  camera back out through LIToWorld. Deriving it was wrong in
         *  practice: the level instance's transform is not the only thing
         *  between the two spaces, so the round trip drifted and you surfaced
         *  somewhere arbitrary. Storing the pose removes the arithmetic
         *  entirely -- you come back up exactly where you left. */
        FVector  CamLoc = FVector::ZeroVector;
        FRotator CamRot = FRotator::ZeroRotator;
        float    CamFOV = 90.0f;
        bool     bHasCam = false;
    };

    static TArray<FDrillFrame> Stack;

    /** Camera in flight between the two LoadMap halves, already converted into
     *  the destination's space. */
    static bool     bPending  = false;
    static FVector  PendingLoc = FVector::ZeroVector;
    static FRotator PendingRot = FRotator::ZeroRotator;
    static float    PendingFOV = 90.0f;

    static FLevelEditorViewportClient* ActiveClient()
    {
        // The client the user last interacted with. Null in headless runs and
        // briefly during layout changes, hence every call is guarded.
        return GCurrentLevelEditingViewportClient;
    }

    bool    CanAscend()       { return Stack.Num() > 0; }
    FString PeekParentLevel() { return Stack.Num() ? Stack.Last().ParentLevel : FString(); }

    void PushAndDescend(const FTransform& LIToWorld)
    {
        FDrillFrame Frame;
        Frame.LIToWorld = LIToWorld;

        if (UWorld* W = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
        {
            Frame.ParentLevel = W->GetOutermost()->GetName();
        }

        if (FLevelEditorViewportClient* VC = ActiveClient())
        {
            // World space -> the child level's local space.
            const FVector  Loc = VC->GetViewLocation();
            const FRotator Rot = VC->GetViewRotation();
            // Down: world -> the child's local space, so you land on the
            // geometry instead of where the instance sits in the world.
            PendingLoc = LIToWorld.InverseTransformPosition(Loc);
            PendingRot = LIToWorld.InverseTransformRotation(Rot.Quaternion()).Rotator();
            PendingFOV = VC->ViewFOV;
            bPending   = true;

            // Up: no arithmetic at all, just this.
            Frame.CamLoc  = Loc;
            Frame.CamRot  = Rot;
            Frame.CamFOV  = VC->ViewFOV;
            Frame.bHasCam = true;

            // Only remember a mode we did not impose ourselves, so descending
            // from an already-drilled level cannot record our own Unlit as the
            // user's preference.
            if (VC->GetViewMode() != VMI_Unlit)
            {
                Frame.ViewMode     = VC->GetViewMode();
                Frame.bHasViewMode = true;
            }
        }

        Stack.Push(Frame);

        // Kept in sync so anything still reading the old single-slot field
        // (and the Manager's "return" affordances) sees the immediate parent.
        FMapLinkEditorModule::PreviousLevelPath = Frame.ParentLevel;
    }

    static void ApplyPendingCamera(FLevelEditorViewportClient* VC)
    {
        if (!VC || !bPending) return;
        VC->SetViewLocation(PendingLoc);
        VC->SetViewRotation(PendingRot);
        VC->ViewFOV = PendingFOV;
        bPending = false;
    }

    void ArriveInChild()
    {
        FLevelEditorViewportClient* VC = ActiveClient();
        if (!VC) return;
        ApplyPendingCamera(VC);
        VC->SetViewMode(VMI_Unlit);
        VC->Invalidate();
    }

    void ArriveInParent()
    {
        if (Stack.Num() == 0) return;
        const FDrillFrame Frame = Stack.Pop();

        FLevelEditorViewportClient* VC = ActiveClient();
        if (VC)
        {
            if (Frame.bHasCam)
            {
                VC->SetViewLocation(Frame.CamLoc);
                VC->SetViewRotation(Frame.CamRot);
                VC->ViewFOV = Frame.CamFOV;
            }

            if (Frame.bHasViewMode)
            {
                VC->SetViewMode(Frame.ViewMode);
            }
            VC->Invalidate();
        }

        FMapLinkEditorModule::PreviousLevelPath = PeekParentLevel();
    }
}

// ---------------------------------------------------------------------------
// Auto-Update ticker
// ---------------------------------------------------------------------------

// Defined with the toolbar further down; declared here because the ticker is
// what drives the Manager status light.
namespace MapLinkToolbar { void RefreshStatus(); }

bool FMapLinkEditorModule::OnAutoUpdateTick(float DeltaTime)
{
    if (bAutoUpdateInProgress) return true;
    if (!GEditor) return true;

    // Skip during PIE
    if (GEditor->IsPlayingSessionInEditor()) return true;

    // v1.16 — keep the toolbar's Manager light current. Same poll interval,
    // and it only rebuilds widgets when the aggregate state actually flips.
    MapLinkToolbar::RefreshStatus();

    // Scan proxies with bAutoUpdate
    TArray<UMapLinkProxyAsset*> AllProxies = GetAllProxies();

    for (UMapLinkProxyAsset* Proxy : AllProxies)
    {
        if (!Proxy || !Proxy->bAutoUpdate) continue;

        const FString& BlendPath = Proxy->BlendFilePath;
        if (BlendPath.IsEmpty() || !FPaths::FileExists(BlendPath)) continue;

        FDateTime ModTime = IFileManager::Get().GetTimeStamp(*BlendPath);
        FDateTime* LastSeen = LastSeenModTimes.Find(BlendPath);

        if (!LastSeen)
        {
            // First encounter — seed, don't reimport
            LastSeenModTimes.Add(BlendPath, ModTime);
            continue;
        }

        if (ModTime > *LastSeen)
        {
            LastSeenModTimes.Add(BlendPath, ModTime);

            // v1.13.1 — Skip AutoReimport: change ignored (mtime still
            // tracked, so unticking the flag reacts only to NEW changes).
            if (Proxy->bSkipAutoReimport)
            {
                UE_LOG(LogMapLink, Log,
                       TEXT("[AutoUpdate] Change in '%s' ignored — Skip "
                            "AutoReimport is set."),
                       *FPaths::GetBaseFilename(BlendPath));
                continue;
            }

            // v1.9 — debounce: note the change, fire after a quiet period.
            PendingChanges.Add(BlendPath, FPlatformTime::Seconds());
            UE_LOG(LogMapLink, Log,
                   TEXT("[AutoUpdate] Detected change: '%s' — debouncing."),
                   *FPaths::GetBaseFilename(BlendPath));
        }
    }

    // v1.9 — Recursive auto-reimport: a parent with BOTH bAutoUpdate and
    // bAutoUpdateRecursive watches its whole dependency tree.  A changed
    // child queues the PARENT for reimport (RunReimport cascades down +
    // re-swaps).  bCascadeAutoReimport is the project-wide master switch
    // (default on); the per-proxy "Auto Reimport (Recursive)" flag is the
    // real control.
    if (UMapLinkSettings::Get()->bCascadeAutoReimport)
    {
        const double NowCascade = FPlatformTime::Seconds();

        // v1.13.1 — Skip AutoReimport: flagged children never trigger the
        // parent cascade.
        TSet<FString> SkipPaths;
        for (UMapLinkProxyAsset* P : AllProxies)
        {
            if (P && P->bSkipAutoReimport && !P->BlendFilePath.IsEmpty())
            {
                SkipPaths.Add(P->BlendFilePath);
            }
        }

        // Map every known proxy stem → its .blend path so we can stat
        // descendants that may not themselves have Auto-Update enabled.
        TMap<FString, FString> NameToPath;
        for (UMapLinkProxyAsset* Proxy : AllProxies)
        {
            if (Proxy && !Proxy->BlendFilePath.IsEmpty())
            {
                NameToPath.Add(
                    FPaths::GetBaseFilename(Proxy->BlendFilePath),
                    Proxy->BlendFilePath);
            }
        }

        // Rebuild the descendant cache every ~30 s (analysis.json walk).
        if (NowCascade - CascadeCacheBuiltSec > 30.0)
        {
            CascadeDescendantCache.Reset();
            for (UMapLinkProxyAsset* Proxy : AllProxies)
            {
                if (!Proxy || !Proxy->bAutoUpdate ||
                    !Proxy->bAutoUpdateRecursive) continue;
                const FString ParentName =
                    FPaths::GetBaseFilename(Proxy->BlendFilePath);
                if (!ParentName.StartsWith(TEXT("LI_"))) continue;

                TSet<FString> DescNames;
                FMapLinkWPExpansionHelper::CollectDescendantNames(
                    ParentName, DescNames);

                TArray<FString> DescPaths;
                for (const FString& DName : DescNames)
                {
                    if (DName == ParentName) continue;
                    if (const FString* DPath = NameToPath.Find(DName))
                    {
                        DescPaths.Add(*DPath);
                    }
                }
                CascadeDescendantCache.Add(ParentName, DescPaths);
            }
            CascadeCacheBuiltSec = NowCascade;
        }

        for (UMapLinkProxyAsset* Proxy : AllProxies)
        {
            if (!Proxy || !Proxy->bAutoUpdate ||
                !Proxy->bAutoUpdateRecursive) continue;
            const FString ParentName =
                FPaths::GetBaseFilename(Proxy->BlendFilePath);
            const TArray<FString>* DescPaths =
                CascadeDescendantCache.Find(ParentName);
            if (!DescPaths) continue;

            for (const FString& ChildPath : *DescPaths)
            {
                if (ChildPath.IsEmpty() || !FPaths::FileExists(ChildPath))
                {
                    continue;
                }
                const FDateTime ChildMod =
                    IFileManager::Get().GetTimeStamp(*ChildPath);
                FDateTime* ChildSeen = LastSeenModTimes.Find(ChildPath);
                if (!ChildSeen)
                {
                    LastSeenModTimes.Add(ChildPath, ChildMod);
                    continue;
                }
                if (ChildMod > *ChildSeen)
                {
                    LastSeenModTimes.Add(ChildPath, ChildMod);
                    // v1.13.1 — Skip AutoReimport: a flagged child's change
                    // does NOT queue the parent (mtime still tracked).
                    if (SkipPaths.Contains(ChildPath))
                    {
                        UE_LOG(LogMapLink, Log,
                               TEXT("[AutoUpdate] Child '%s' changed but has "
                                    "Skip AutoReimport — parent '%s' NOT "
                                    "queued."),
                               *FPaths::GetBaseFilename(ChildPath),
                               *ParentName);
                        continue;
                    }
                    // Queue the PARENT, not the child.
                    PendingChanges.Add(Proxy->BlendFilePath, NowCascade);
                    UE_LOG(LogMapLink, Log,
                           TEXT("[AutoUpdate] Child '%s' changed — queuing "
                                "parent '%s' (cascade)."),
                           *FPaths::GetBaseFilename(ChildPath),
                           *ParentName);
                }
            }
        }
    }

    // v1.9 — fire pending reimports once a blend has been quiet ~2 s.
    // A still-changing file (export-on-save in progress, multi-file save
    // cascade from Blender) keeps refreshing its entry above and is not
    // fired until the burst settles.
    const double NowSec = FPlatformTime::Seconds();
    for (auto It = PendingChanges.CreateIterator(); It; ++It)
    {
        if (NowSec - It->Value < 2.0)
        {
            continue;
        }

        const FString BlendPath = It->Key;
        It.RemoveCurrent();

        // v1.13.1 — final guard: a Skip-flagged asset never auto-reimports,
        // even if it was queued before the flag was ticked.
        {
            bool bSkipPending = false;
            for (UMapLinkProxyAsset* P : AllProxies)
            {
                if (P && P->BlendFilePath == BlendPath)
                {
                    bSkipPending = P->bSkipAutoReimport;
                    break;
                }
            }
            if (bSkipPending)
            {
                UE_LOG(LogMapLink, Log,
                       TEXT("[AutoUpdate] Pending reimport of '%s' dropped — "
                            "Skip AutoReimport is set."),
                       *FPaths::GetBaseFilename(BlendPath));
                continue;
            }
        }

        UE_LOG(LogMapLink, Log,
               TEXT("[AutoUpdate] Quiet period elapsed — reimporting '%s'."),
               *FPaths::GetBaseFilename(BlendPath));

        bAutoUpdateInProgress = true;

        // ── KEY FIX: reimport from the active editor level ────
        // If the editor has a MapLink level open, reimport from
        // THAT level so the full cascade runs (SM_ reimport +
        // parent LI_ re-swap).  This matches manual reimport
        // behavior which correctly updates in-viewport references.
        UMapLinkProxyAsset* ActiveProxy = GetActiveMapLinkProxy();
        if (ActiveProxy && !ActiveProxy->BlendFilePath.IsEmpty())
        {
            UE_LOG(LogMapLink, Log,
                   TEXT("[AutoUpdate] Reimporting from active level '%s' (cascade)."),
                   *FPaths::GetBaseFilename(ActiveProxy->BlendFilePath));
            FMapLinkPipeline::RunReimport(ActiveProxy->BlendFilePath);
        }
        else
        {
            // Not viewing a MapLink level — reimport the individual file
            FMapLinkPipeline::RunReimport(BlendPath);
        }

        bAutoUpdateInProgress = false;
        break; // One reimport per tick
    }

    return true;
}

// ---------------------------------------------------------------------------
// Module lifecycle
// ---------------------------------------------------------------------------

void FMapLinkEditorModule::StartupModule()
{
    // v1.21 - every Blender MapLink launches inherits this environment.
    // PYTHONUTF8 stops a path with an accent coming back as mojibake in the
    // captured output (and stops UnicodeEncodeError inside the worker on a
    // non-UTF-8 code page). PYTHONUNBUFFERED is what makes the watchdog
    // dialog's "Last output" non-empty: piped stdout is block-buffered
    // otherwise, so a killed Blender took its output with it.
    FPlatformMisc::SetEnvironmentVar(TEXT("PYTHONUTF8"), TEXT("1"));
    FPlatformMisc::SetEnvironmentVar(TEXT("PYTHONUNBUFFERED"), TEXT("1"));

    RegisterStyle();

    IAssetTools& AssetTools =
        FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
    MapLinkProxyActions = MakeShared<FAssetTypeActions_MapLinkProxy>();
    AssetTools.RegisterAssetTypeActions(MapLinkProxyActions.ToSharedRef());

    FMapLinkCommands::Register();

    // v1.8 — BP Reference Mode: Content Browser right-click menu
    FMapLinkContentBrowserExtensions::Register();
    FMapLinkBPLifecycleHooks::Register();    // v1.8 D4

    // v1.15 — selection lock + asset name overlay for MapLink actors
    FMapLinkActorLock::Register();
    FMapLinkNameDisplay::Register();

    // v1.16 — Texture Manager console commands. The data layer ships before
    // any UI does (the Textures tab is Phase 2), so this is how it is driven
    // and tested. Absent -execute, every command is a dry run.
    FMapLinkTextureConsole::Register();

    UToolMenus::RegisterStartupCallback(
        FSimpleMulticastDelegate::FDelegate::CreateRaw(
            this, &FMapLinkEditorModule::RegisterMenus));

    // v1.9 — MapLink Manager window (nomad tab)
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
        ManagerTabName,
        FOnSpawnTab::CreateRaw(
            this, &FMapLinkEditorModule::SpawnMapLinkManagerTab))
        .SetDisplayName(NSLOCTEXT("MapLink", "ManagerTabTitle", "MapLink Manager"))
        .SetTooltipText(NSLOCTEXT("MapLink", "ManagerTabTooltip",
            "Imported .blend files, states, reimport, WP expansion, settings."))
        // v1.16 — the mark belongs on the tab title, where it identifies the
        // window among the user's other tabs.
        .SetIcon(GetIcon("MapLink.MenuIcon"))
        .SetMenuType(ETabSpawnerMenuType::Hidden);

    // v1.11 — MapLink Asset Viewer (read-only preview nomad tab)
    FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
        AssetViewerTabName,
        FOnSpawnTab::CreateRaw(
            this, &FMapLinkEditorModule::SpawnMapLinkAssetViewerTab))
        .SetDisplayName(NSLOCTEXT("MapLink", "AssetViewerTabTitle", "MapLink Asset Viewer"))
        .SetTooltipText(NSLOCTEXT("MapLink", "AssetViewerTabTooltip",
            "Read-only preview of MapLink proxies (LI_/SM_/BP_) with orbit/freecam."))
        .SetMenuType(ETabSpawnerMenuType::Hidden);

    const float PollInterval = UMapLinkSettings::Get()->AutoUpdatePollInterval;
    AutoUpdateTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateRaw(this, &FMapLinkEditorModule::OnAutoUpdateTick),
        PollInterval);

    UE_LOG(LogTemp, Log, TEXT("MapLinkEditor: Module started (auto-update: %.1fs)."),
           PollInterval);
}

void FMapLinkEditorModule::ShutdownModule()
{
    if (AutoUpdateTickerHandle.IsValid())
        FTSTicker::GetCoreTicker().RemoveTicker(AutoUpdateTickerHandle);

    // v1.9
    if (FSlateApplication::IsInitialized())
    {
        FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(ManagerTabName);
        FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(AssetViewerTabName);
    }

    FMapLinkCommands::Unregister();
    UToolMenus::UnRegisterStartupCallback(this);
    UToolMenus::UnregisterOwner(this);

    // v1.8 — BP Reference Mode
    FMapLinkContentBrowserExtensions::Unregister();
    FMapLinkBPLifecycleHooks::Unregister();   // v1.8 D4

    // v1.15
    FMapLinkActorLock::Unregister();
    FMapLinkNameDisplay::Unregister();

    // v1.16
    FMapLinkTextureConsole::Unregister();

    if (FModuleManager::Get().IsModuleLoaded("AssetTools") && MapLinkProxyActions.IsValid())
    {
        IAssetTools& AssetTools =
            FModuleManager::GetModuleChecked<FAssetToolsModule>("AssetTools").Get();
        AssetTools.UnregisterAssetTypeActions(MapLinkProxyActions.ToSharedRef());
    }

    if (StyleSet.IsValid())
    {
        FSlateStyleRegistry::UnRegisterSlateStyle(*StyleSet);
        StyleSet.Reset();
    }
    RegisteredIcons.Reset();
}

// ---------------------------------------------------------------------------
// Keybind registration
// ---------------------------------------------------------------------------

void FMapLinkEditorModule::BindLevelEditorCommands()
{
    if (!FModuleManager::Get().IsModuleLoaded("LevelEditor")) return;

    FLevelEditorModule& LevelEditorModule =
        FModuleManager::LoadModuleChecked<FLevelEditorModule>("LevelEditor");

    TSharedRef<FUICommandList> LevelEditorActions =
        LevelEditorModule.GetGlobalLevelEditorActions();

    LevelEditorActions->MapAction(
        FMapLinkCommands::Get().EditLevelStandalone,
        FExecuteAction::CreateStatic(&FMapLinkEditorModule::EditLevelStandaloneAction));
    LevelEditorActions->MapAction(
        FMapLinkCommands::Get().ReturnToPreviousLevel,
        FExecuteAction::CreateStatic(&FMapLinkEditorModule::ReturnToPreviousLevelAction));
}

// ---------------------------------------------------------------------------
// Menu registration
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// v1.15.5 — MapLink display toggles, shared by the viewport-toolbar combo
// and the MapLink main menu's "Viewport Display" submenu.
// ---------------------------------------------------------------------------

namespace
{
    void FillMapLinkDisplayMenu(UToolMenu* Menu)
    {
        FToolMenuSection& Names = Menu->AddSection(
            "MapLinkNames", LOCTEXT("MapLinkNamesSection", "Asset Names"));

        auto ToggleEntry = [](const FName Id, const FText& Label,
                              const FText& Tip,
                              TFunction<void()> Toggle,
                              TFunction<bool()> IsChecked,
                              TFunction<bool()> CanExecute) -> FToolMenuEntry
        {
            FToolMenuEntry Entry = FToolMenuEntry::InitMenuEntry(
                Id, Label, Tip, FSlateIcon(),
                FUIAction(
                    FExecuteAction::CreateLambda(MoveTemp(Toggle)),
                    FCanExecuteAction::CreateLambda(MoveTemp(CanExecute)),
                    FIsActionChecked::CreateLambda(MoveTemp(IsChecked))),
                EUserInterfaceActionType::ToggleButton);
            return Entry;
        };

        const auto MasterOn = []()
        {
            return UMapLinkSettings::Get()->bShowAssetNames;
        };
        const auto Always = []() { return true; };

        Names.AddEntry(ToggleEntry("MapLinkShowAssetNames",
            LOCTEXT("TglShowNames", "Show Asset Names"),
            LOCTEXT("TglShowNamesTip",
                "Floating 3D name labels over MapLink assets — level "
                "viewport and Asset Viewer (per-user)."),
            []() { UMapLinkSettings::SetShowAssetNames(
                       !UMapLinkSettings::Get()->bShowAssetNames); },
            MasterOn, Always));

        Names.AddEntry(ToggleEntry("MapLinkShowChildren",
            LOCTEXT("TglShowChildren", "Show Children"),
            LOCTEXT("TglShowChildrenTip",
                "Also label child assets (linked instances), whitened."),
            []() { UMapLinkSettings::SetShowChildAssetNames(
                       !UMapLinkSettings::Get()->bShowChildAssetNames); },
            []() { return UMapLinkSettings::Get()->bShowChildAssetNames; },
            MasterOn));

        Names.AddEntry(ToggleEntry("MapLinkShowNamesLI",
            LOCTEXT("TglNamesLI", "LI_ (Level Instances)"),
            LOCTEXT("TglNamesLITip", "Label LI_ assets."),
            []() { UMapLinkSettings::SetShowNamesLI(
                       !UMapLinkSettings::Get()->bShowNamesLI); },
            []() { return UMapLinkSettings::Get()->bShowNamesLI; },
            MasterOn));

        Names.AddEntry(ToggleEntry("MapLinkShowNamesSM",
            LOCTEXT("TglNamesSM", "SM_ (Static Meshes)"),
            LOCTEXT("TglNamesSMTip", "Label SM_ assets."),
            []() { UMapLinkSettings::SetShowNamesSM(
                       !UMapLinkSettings::Get()->bShowNamesSM); },
            []() { return UMapLinkSettings::Get()->bShowNamesSM; },
            MasterOn));

        Names.AddEntry(ToggleEntry("MapLinkShowNamesBP",
            LOCTEXT("TglNamesBP", "BP_ (Blueprints)"),
            LOCTEXT("TglNamesBPTip", "Label BP_ assets."),
            []() { UMapLinkSettings::SetShowNamesBP(
                       !UMapLinkSettings::Get()->bShowNamesBP); },
            []() { return UMapLinkSettings::Get()->bShowNamesBP; },
            MasterOn));

        FToolMenuSection& Actors = Menu->AddSection(
            "MapLinkActors", LOCTEXT("MapLinkActorsSection", "Actors"));

        Actors.AddEntry(ToggleEntry("MapLinkLockTransforms",
            LOCTEXT("TglLock", "Lock MapLink Actor Transforms"),
            LOCTEXT("TglLockTip",
                "MapLink-placed actors stay selectable but transform-"
                "locked (project-wide setting; swept live)."),
            []() { UMapLinkSettings::SetLockMapLinkActorTransforms(
                       !UMapLinkSettings::Get()->bLockMapLinkActorTransforms); },
            []() { return UMapLinkSettings::Get()->bLockMapLinkActorTransforms; },
            Always));
    }
}

// ---------------------------------------------------------------------------
// Level Editor toolbar (v1.16)
// ---------------------------------------------------------------------------

namespace MapLinkToolbar
{
    /** First engine icon name that actually exists, else a MapLink brush.
     *
     *  Slate does not fail loudly on a bad brush name -- it draws a grey
     *  placeholder and logs once -- so an unverified "Icons.Foo" looks exactly
     *  like a broken button. Engine icon names also move between 5.x versions.
     *  Asking the style set whether it has the brush turns a guess into a
     *  fallback chain, and the MapLink brush at the end means a button is
     *  never left blank.
     */
    FSlateIcon EngineIcon(std::initializer_list<const TCHAR*> Candidates,
                          const FName MapLinkFallback = NAME_None)
    {
        const ISlateStyle& App = FAppStyle::Get();
        for (const TCHAR* Name : Candidates)
        {
            const FName N(Name);
            if (App.GetOptionalBrush(N, nullptr, nullptr) != nullptr)
            {
                return FSlateIcon(FAppStyle::GetAppStyleSetName(), N);
            }
        }
        if (MapLinkFallback != NAME_None)
        {
            return FMapLinkEditorModule::GetIcon(MapLinkFallback);
        }
        return FSlateIcon();
    }

    /** One-shot report of what resolved, so a missing icon is a log line
     *  rather than a mystery grey square. */
    void LogIconResolution()
    {
        static bool bDone = false;
        if (bDone) return;
        bDone = true;

        const ISlateStyle& App = FAppStyle::Get();
        for (const TCHAR* Name : { TEXT("Icons.Search"), TEXT("Icons.Visibility"),
                                   TEXT("Icons.Visible"), TEXT("Icons.ChevronDown"),
                                   TEXT("Icons.ChevronUp"), TEXT("Icons.ArrowDown"),
                                   TEXT("Icons.ArrowUp"), TEXT("Icons.Refresh"),
                                   TEXT("Icons.Warning"), TEXT("Icons.Settings") })
        {
            UE_LOG(LogMapLink, Log, TEXT("[Toolbar] icon '%s' : %s"), Name,
                   App.GetOptionalBrush(FName(Name), nullptr, nullptr)
                       ? TEXT("present") : TEXT("MISSING"));
        }
    }

    /** Every proxy reachable from the current actor selection, deduped.
     *
     *  Actors rather than Content Browser assets: the Content Browser already
     *  has its own MapLink menu through FAssetTypeActions_MapLinkProxy, so a
     *  toolbar that also read it would duplicate that surface instead of
     *  covering the one it does not reach.
     */
    TArray<UMapLinkProxyAsset*> SelectedProxies()
    {
        TArray<UMapLinkProxyAsset*> Out;
        if (!GEditor) return Out;
        USelection* Sel = GEditor->GetSelectedActors();
        if (!Sel) return Out;

        for (FSelectionIterator It(*Sel); It; ++It)
        {
            AActor* A = Cast<AActor>(*It);
            if (!A) continue;
            if (UMapLinkProxyAsset* P =
                    MapLinkActorResolve::ResolveProxyFromActor(A))
            {
                Out.AddUnique(P);
            }
        }
        return Out;
    }

    /** Source .blend paths behind the current selection, deduped. */
    TArray<FString> SelectedBlends()
    {
        TArray<FString> Out;
        for (UMapLinkProxyAsset* P : SelectedProxies())
        {
            if (P && !P->BlendFilePath.IsEmpty()) Out.AddUnique(P->BlendFilePath);
        }
        return Out;
    }

    /** The active selection's stem: the LAST actor selected, since the editor
     *  appends to the selection set. Empty when nothing is selected, which the
     *  Asset Viewer already treats as "first proxy". */
    FString ActiveStem()
    {
        const TArray<UMapLinkProxyAsset*> P = SelectedProxies();
        return P.Num() > 0 ? FPaths::GetBaseFilename(P.Last()->BlendFilePath)
                           : FString();
    }

    bool HasSelection() { return SelectedProxies().Num() > 0; }

    /** Every material actually assigned under the selection, recursively.
     *
     *  Walks the WORLD and asks each actor which level instance owns it,
     *  climbing that chain to see whether a selected actor is an ancestor.
     *  That is recursive by construction -- nested level instances included --
     *  and uses only GetOwningLevelInstance, which the name overlay already
     *  relies on. Enumerating each instance's contents directly would need
     *  hand-rolled recursion over APIs this plugin has never exercised.
     *
     *  Assigned materials, not "materials in the asset's folder": what is on
     *  the meshes is the question being asked, and the two differ once an
     *  asset shares a library material.
     */
    TArray<FString> SelectedMaterialNames()
    {
        TSet<FString> Names;
        if (!GEditor) return Names.Array();

        UWorld* World = GEditor->GetEditorWorldContext().World();
        USelection* Sel = GEditor->GetSelectedActors();
        if (!World || !Sel) return Names.Array();

        TSet<AActor*> Roots;
        for (FSelectionIterator It(*Sel); It; ++It)
        {
            if (AActor* A = Cast<AActor>(*It)) Roots.Add(A);
        }
        if (Roots.Num() == 0) return Names.Array();

        ULevelInstanceSubsystem* LIS = World->GetSubsystem<ULevelInstanceSubsystem>();

        auto IsUnderSelection = [&Roots, LIS](AActor* A)
        {
            if (Roots.Contains(A)) return true;
            if (!LIS) return false;

            // Climb the owning-level-instance chain. Bounded so a malformed
            // cycle cannot hang the editor.
            ILevelInstanceInterface* Owner = LIS->GetOwningLevelInstance(A->GetLevel());
            for (int32 Depth = 0; Owner && Depth < 32; ++Depth)
            {
                AActor* OwnerActor = Cast<AActor>(Owner);
                if (!OwnerActor) break;
                if (Roots.Contains(OwnerActor)) return true;
                Owner = LIS->GetOwningLevelInstance(OwnerActor->GetLevel());
            }
            return false;
        };

        for (TActorIterator<AActor> It(World); It; ++It)
        {
            AActor* A = *It;
            if (!A || !IsUnderSelection(A)) continue;

            TInlineComponentArray<UMeshComponent*> Meshes(A);
            for (UMeshComponent* MC : Meshes)
            {
                if (!MC) continue;
                const int32 Num = MC->GetNumMaterials();
                for (int32 i = 0; i < Num; ++i)
                {
                    if (UMaterialInterface* MI = MC->GetMaterial(i))
                    {
                        Names.Add(MI->GetName());
                    }
                }
            }
        }
        return Names.Array();
    }

    /** Cached "something needs attention", so the toolbar light does not walk
     *  the asset registry on every widget rebuild. Refreshed by the module's
     *  existing auto-update ticker. */
    static bool bAnythingOutOfDate = false;

    bool AnythingOutOfDate() { return bAnythingOutOfDate; }

    /** Recompute the light. Cheap enough for the poll interval: it asks the
     *  same question the name overlay already answers per stem. */
    void RefreshStatus()
    {
        const bool bWas = bAnythingOutOfDate;
        bool bNow = false;

        for (UMapLinkProxyAsset* P : GetAllProxies())
        {
            if (!P) continue;
            const FString Stem = FPaths::GetBaseFilename(P->BlendFilePath);
            if (Stem.IsEmpty()) continue;
            if (FMapLinkNameDisplay::IsStemOutOfDate(Stem))
            {
                bNow = true;
                break;
            }
        }

        bAnythingOutOfDate = bNow;
        if (bWas != bNow)
        {
            // Only when it actually changes -- rebuilding every widget on a
            // timer would fight the user's own menu interactions.
            UToolMenus::Get()->RefreshAllWidgets();
        }
    }
}


void FMapLinkEditorModule::FillMapLinkActionsMenu(UToolMenu* Menu)
{
    FToolMenuSection& Sel = Menu->AddSection(
        "Selection", LOCTEXT("ToolbarSelection", "Selected Asset(s)"));

    const TArray<UMapLinkProxyAsset*> Proxies = MapLinkToolbar::SelectedProxies();

    if (Proxies.Num() == 0)
    {
        Sel.AddEntry(FToolMenuEntry::InitMenuEntry(
            "MapLink_NoSelection",
            LOCTEXT("ToolbarNoSel", "No MapLink asset selected"),
            LOCTEXT("ToolbarNoSelTip",
                    "Select a MapLink actor in the level to act on it."),
            FSlateIcon(), FUIAction()));
        return;
    }

    // Say what it resolved to, so the menu is never ambiguous about its target.
    const FString Label = Proxies.Num() == 1
        ? FPaths::GetBaseFilename(Proxies[0]->BlendFilePath)
        : FString::Printf(TEXT("%d assets"), Proxies.Num());
    Sel.AddEntry(FToolMenuEntry::InitMenuEntry(
        "MapLink_SelLabel", FText::FromString(Label),
        LOCTEXT("ToolbarSelLabelTip", "What these actions will act on."),
        GetIcon("MapLink.MenuIcon"), FUIAction()));

    TArray<FString> Blends;
    TArray<TWeakObjectPtr<UObject>> ProxyObjects;
    for (UMapLinkProxyAsset* P : Proxies)
    {
        Blends.AddUnique(P->BlendFilePath);
        ProxyObjects.Add(P);
    }

    // ── Find it ──────────────────────────────────────────────────────────
    FToolMenuSection& Find = Menu->AddSection(
        "Find", LOCTEXT("ToolbarFind", "Find"));

    Find.AddMenuEntry("MapLink_FindProxy",
        LOCTEXT("FindProxy", "Find Proxy in Content Browser"),
        LOCTEXT("FindProxyTip", "Select the proxy asset in the Content Browser."),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateLambda([ProxyObjects]()
        {
            TArray<UObject*> Objects;
            for (const TWeakObjectPtr<UObject>& W : ProxyObjects)
            {
                if (UObject* O = W.Get()) Objects.Add(O);
            }
            if (Objects.Num() > 0 && GEditor)
            {
                GEditor->SyncBrowserToObjects(Objects);
            }
        })));

    Find.AddMenuEntry("MapLink_FindInManager",
        LOCTEXT("FindInManager", "Find in Manager"),
        LOCTEXT("FindInManagerTip", "Open the MapLink Manager on this asset."),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateLambda([]()
        {
            FGlobalTabmanager::Get()->TryInvokeTab(FTabId(TEXT("MapLinkManager")));
        })));

    const FString FirstStem = FPaths::GetBaseFilename(Blends[0]);
    Find.AddMenuEntry("MapLink_OpenInViewer",
        LOCTEXT("OpenInViewer", "Open in Asset Viewer"),
        LOCTEXT("OpenInViewerTip", "Preview this asset in the MapLink viewer."),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateLambda([FirstStem]()
        {
            if (FMapLinkEditorModule* M =
                    FModuleManager::GetModulePtr<FMapLinkEditorModule>("MapLinkEditor"))
            {
                M->OpenAssetViewer(FirstStem);
            }
        })));

    Find.AddMenuEntry("MapLink_SelectMaterials",
        LOCTEXT("SelectMaterials", "Select Materials in Material Manager"),
        LOCTEXT("SelectMaterialsTip",
                "Open the Manager's Materials tab with this asset's material "
                "instances selected."),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateLambda([]()
        {
            // v1.16 — this used to only open the Manager and leave the user to
            // find the materials themselves. Order matters: the request is
            // filed BEFORE the tab is invoked, so a Manager that has to be
            // created still picks it up on construct.
            SMapLinkMaterialsTab::RequestSelectMaterials(
                MapLinkToolbar::SelectedMaterialNames());
            FGlobalTabmanager::Get()->TryInvokeTab(FTabId(TEXT("MapLinkManager")));
        })));

    // ── Source ───────────────────────────────────────────────────────────
    FToolMenuSection& Src = Menu->AddSection(
        "Source", LOCTEXT("ToolbarSource", "Source"));

    Src.AddMenuEntry("MapLink_EditBlend",
        LOCTEXT("ToolbarEditBlend", "Edit source .blend"),
        LOCTEXT("ToolbarEditBlendTip", "Open in Blender."),
        GetIcon("MapLink.MenuIcon"),
        FUIAction(FExecuteAction::CreateLambda([Blends]()
        {
            for (const FString& B : Blends)
            {
                FMapLinkBlenderBridge::OpenBlenderGUI(B);
            }
        })));

    Src.AddMenuEntry("MapLink_ExploreBlend",
        LOCTEXT("ToolbarExplore", "Find source .blend in Explorer"),
        LOCTEXT("ToolbarExploreTip", "Open the containing folder."),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateLambda([Blends]()
        {
            for (const FString& B : Blends)
            {
                FMapLinkBlenderBridge::OpenInExplorer(B);
            }
        })));

    // ── Reimport ─────────────────────────────────────────────────────────
    FToolMenuSection& Re = Menu->AddSection(
        "Reimport", LOCTEXT("ToolbarReimport", "Reimport"));

    Re.AddMenuEntry("MapLink_Reimport",
        LOCTEXT("ToolbarReimportEntry", "Reimport"),
        LOCTEXT("ToolbarReimportTip", "Reimport changed dependencies."),
        GetIcon("MapLink.Reimport"),
        FUIAction(FExecuteAction::CreateLambda([Blends]()
        {
            for (const FString& B : Blends) FMapLinkPipeline::RunReimport(B);
        })));

    Re.AddMenuEntry("MapLink_ForceReimport",
        LOCTEXT("ToolbarForceReimport", "Force full-reimport"),
        LOCTEXT("ToolbarForceReimportTip",
                "Every file in each selected asset's tree is re-imported "
                "whether or not it changed. Confirmation is shown first."),
        GetIcon("MapLink.ReimportForce"),
        FUIAction(FExecuteAction::CreateLambda([Blends]()
        {
            TSharedRef<SCustomDialog> Dlg =
                SNew(SCustomDialog)
                .Title(LOCTEXT("ToolbarForceDlgTitle",
                               "MapLink Force Full Reimport"))
                .Content()
                [
                    SNew(STextBlock)
                    .WrapTextAt(520.0f)
                    .Text(FText::Format(LOCTEXT("ToolbarForceDlgFmt",
                        "Force Full Reimport {0} asset(s)?\n\n"
                        "Every file in each tree is re-imported, ignoring "
                        "hashes (nuke-and-recreate)."),
                        FText::AsNumber(Blends.Num())))
                ]
                .Buttons({
                    SCustomDialog::FButton(LOCTEXT("ToolbarForceGo",
                                                   "Force Full Reimport")),
                    SCustomDialog::FButton(LOCTEXT("ToolbarForceCancel",
                                                   "Cancel"))
                });
            if (Dlg->ShowModal() != 0) return;
            for (const FString& B : Blends) FMapLinkPipeline::RunImport(B);
        })));

    // Per-proxy flag, so the toggle reflects and sets what the pipeline reads.
    const bool bAllSkipping = [&Proxies]()
    {
        for (UMapLinkProxyAsset* P : Proxies)
        {
            if (!P->bSkipAutoReimport) return false;
        }
        return Proxies.Num() > 0;
    }();

    Re.AddMenuEntry("MapLink_ToggleAutoReimport",
        LOCTEXT("ToolbarAutoReimport", "Auto-Reimport"),
        LOCTEXT("ToolbarAutoReimportTip",
                "When off, .blend changes to this asset never trigger an "
                "automatic reimport."),
        FSlateIcon(),
        FUIAction(
            FExecuteAction::CreateLambda([ProxyObjects, bAllSkipping]()
            {
                for (const TWeakObjectPtr<UObject>& W : ProxyObjects)
                {
                    if (UMapLinkProxyAsset* P =
                            Cast<UMapLinkProxyAsset>(W.Get()))
                    {
                        P->bSkipAutoReimport = !bAllSkipping ? true : false;
                        P->MarkPackageDirty();
                    }
                }
            }),
            FCanExecuteAction(),
            FIsActionChecked::CreateLambda([bAllSkipping]()
            {
                return !bAllSkipping;
            })),
        EUserInterfaceActionType::ToggleButton);
}


// ---------------------------------------------------------------------------
// v1.16 — the MapLink toolbar group
//
// Built as ONE owned widget rather than nine FToolMenuEntry blocks. The only
// reason is the background: StyleNameOverride resolves against the editor's
// style set, so a MapLink toolbar style is never found and Slate falls back to
// a style with no hover. Painting behind a group means owning the group.
//
// Everything the entries did is preserved: live enable state, live tooltips,
// the badged Manager icon, and both menus still come from the same
// Fill*Menu functions the viewport combo and main menu use.
// ---------------------------------------------------------------------------

namespace MapLinkToolbar
{
    /** A MapLink brush, or null when its art was not shipped. */
    const FSlateBrush* MLBrush(const FName Key)
    {
        return (FMapLinkEditorModule::StyleSet.IsValid() &&
                FMapLinkEditorModule::RegisteredIcons.Contains(Key))
            ? FMapLinkEditorModule::StyleSet->GetBrush(Key)
            : nullptr;
    }

    /** First engine brush that exists, else a MapLink one. Same reasoning as
     *  EngineIcon: a bad name draws a grey square instead of failing. */
    const FSlateBrush* EngineBrush(std::initializer_list<const TCHAR*> Candidates,
                                   const FName Fallback = NAME_None)
    {
        const ISlateStyle& App = FAppStyle::Get();
        for (const TCHAR* N : Candidates)
        {
            if (const FSlateBrush* B =
                    App.GetOptionalBrush(FName(N), nullptr, nullptr))
            {
                return B;
            }
        }
        return Fallback != NAME_None ? MLBrush(Fallback) : nullptr;
    }

    /** Menu names registered once so the combos can generate their content
     *  from the existing fill functions instead of a second copy of them. */
    static const FName ActionsMenuName("MapLink.ToolbarActions");
    static const FName DisplayMenuName("MapLink.ToolbarDisplay");

    void RegisterComboMenus()
    {
        UToolMenus* TM = UToolMenus::Get();
        if (!TM) return;

        if (!TM->IsMenuRegistered(ActionsMenuName))
        {
            UToolMenu* M = TM->RegisterMenu(ActionsMenuName);
            M->AddDynamicSection("Dyn", FNewSectionConstructChoice(
                FNewToolMenuDelegate::CreateLambda([](UToolMenu* InMenu)
                { FMapLinkEditorModule::FillMapLinkActionsMenu(InMenu); })));
        }
        if (!TM->IsMenuRegistered(DisplayMenuName))
        {
            UToolMenu* M = TM->RegisterMenu(DisplayMenuName);
            M->AddDynamicSection("Dyn", FNewSectionConstructChoice(
                FNewToolMenuDelegate::CreateLambda([](UToolMenu* InMenu)
                { FillMapLinkDisplayMenu(InMenu); })));
        }
    }
}

TSharedRef<SWidget> FMapLinkEditorModule::BuildToolbarGroup()
{
    using namespace MapLinkToolbar;

    const ISlateStyle& App = FAppStyle::Get();

    // A flat toolbar-style button where available, the ordinary one otherwise.
    // Taken from the editor's own style so hover and pressed match every other
    // button on the bar rather than being hand-approximated.
    const FButtonStyle* Btn =
        App.HasWidgetStyle<FButtonStyle>("SimpleButton")
            ? &App.GetWidgetStyle<FButtonStyle>("SimpleButton")
            : &App.GetWidgetStyle<FButtonStyle>("Button");

    TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);

    auto AddButton = [&Row, Btn](TAttribute<const FSlateBrush*> Icon,
                                 TAttribute<FText> Tip,
                                 TFunction<void()> Do,
                                 bool bNeedsSelection)
    {
        Row->AddSlot().AutoWidth().Padding(0.f)
        [
            SNew(SButton)
            .ButtonStyle(Btn)
            .ContentPadding(FMargin(3.f, 1.f))       // tighter than stock
            .ToolTipText(Tip)
            .IsEnabled_Lambda([bNeedsSelection]()
            {
                return !bNeedsSelection || MapLinkToolbar::HasSelection();
            })
            .OnClicked_Lambda([Do]() { Do(); return FReply::Handled(); })
            [
                SNew(SImage)
                .Image(Icon)
                .ColorAndOpacity(FSlateColor::UseForeground())
            ]
        ];
    };

    const FComboButtonStyle* ComboStyle =
        App.HasWidgetStyle<FComboButtonStyle>("SimpleComboButton")
            ? &App.GetWidgetStyle<FComboButtonStyle>("SimpleComboButton")
            : &App.GetWidgetStyle<FComboButtonStyle>("ComboButton");

    auto AddCombo = [&Row, ComboStyle](const FName MenuName,
                                       TAttribute<const FSlateBrush*> Icon,
                                       TAttribute<FText> Tip, bool bArrowOnly,
                                       bool bDownArrow)
    {
        Row->AddSlot().AutoWidth().Padding(0.f)
        [
            SNew(SComboButton)
            .ComboButtonStyle(ComboStyle)
            .ContentPadding(FMargin(3.f, 1.f))
            .ToolTipText(Tip)
            .HasDownArrow(bDownArrow)
            .ButtonContent()
            [
                bArrowOnly
                    ? SNullWidget::NullWidget
                    : StaticCastSharedRef<SWidget>(
                        SNew(SImage).Image(Icon)
                        .ColorAndOpacity(FSlateColor::UseForeground()))
            ]
            .OnGetMenuContent_Lambda([MenuName]()
            {
                FToolMenuContext Ctx;
                return UToolMenus::Get()->GenerateWidget(MenuName, Ctx);
            })
        ];
    };

    // ── Manager: the MapLink mark, red when anything is stale ────────────
    AddButton(
        TAttribute<const FSlateBrush*>::CreateLambda([]()
        {
            // Red when anything is stale -- the whole mark, not a badge.
            return MapLinkToolbar::AnythingOutOfDate()
                ? MLBrush("MapLink.ManagerStale") : MLBrush("MapLink.Manager");
        }),
        TAttribute<FText>::CreateLambda([]()
        {
            return MapLinkToolbar::AnythingOutOfDate()
                ? LOCTEXT("TbManagerTipStale",
                          "MapLink Manager — assets are out of date.")
                : LOCTEXT("TbManagerTip",
                          "MapLink Manager — everything up to date.");
        }),
        []() { FGlobalTabmanager::Get()->TryInvokeTab(FTabId(TEXT("MapLinkManager"))); },
        false);

    // ── Find proxy in the Content Browser ────────────────────────────────
    AddButton(
        EngineBrush({ TEXT("Icons.Search"), TEXT("Icons.Find") }, "MapLink.MenuIcon"),
        LOCTEXT("TbFindTip", "Select the proxy in the Content Browser."),
        []()
        {
            TArray<UObject*> Objects;
            for (UMapLinkProxyAsset* P : MapLinkToolbar::SelectedProxies())
            {
                if (P) Objects.Add(P);
            }
            if (Objects.Num() > 0 && GEditor) GEditor->SyncBrowserToObjects(Objects);
        }, true);

    // ── Show the source .blend in Explorer ───────────────────────────────
    AddButton(MLBrush("MapLink.Folder"),
        LOCTEXT("TbExploreTip",
                "Show the source .blend in Explorer (selects the file)."),
        []()
        {
            for (const FString& B : MapLinkToolbar::SelectedBlends())
            {
                if (FPaths::FileExists(B))
                {
                    // Selects the file in its folder rather than opening the
                    // folder blind, which matters when a tree holds many .blends.
                    FPlatformProcess::ExploreFolder(*B);
                }
            }
        }, true);

    // ── Edit source .blend ───────────────────────────────────────────────
    AddButton(MLBrush("MapLink.EditBlend"),
        LOCTEXT("TbEditBlendTip", "Open the source .blend in Blender."),
        []()
        {
            for (const FString& B : MapLinkToolbar::SelectedBlends())
            {
                FMapLinkBlenderBridge::OpenBlenderGUI(B);
            }
        }, true);

    // ── Asset names / display: eye only, no arrow ────────────────────────
    //
    // Reimport and Force Full Reimport are deliberately NOT on the bar: they
    // are in the overflow menu, which is enough for how often they are used
    // and keeps the destructive one out of easy mis-click range.
    AddCombo(MapLinkToolbar::DisplayMenuName,
        EngineBrush({ TEXT("Icons.Visibility"), TEXT("Icons.Visible") },
                    "MapLink.MenuIcon"),
        LOCTEXT("TbDisplayTip",
                "Asset names, child names, per-type filters and label limits."),
        /*bArrowOnly=*/false, /*bDownArrow=*/false);

    // ── Drill ────────────────────────────────────────────────────────────
    AddButton(MLBrush("MapLink.DrillDown"),
        LOCTEXT("TbDrillDownTip",
                "Open the selected Level Instance as a standalone level "
                "(Alt+PageDown)."),
        []() { FMapLinkEditorModule::EditLevelStandaloneAction(); }, false);

    AddButton(MLBrush("MapLink.DrillUp"),
        LOCTEXT("TbDrillUpTip",
                "Return to the level open before drilling (Alt+PageUp)."),
        []() { FMapLinkEditorModule::ReturnToPreviousLevelAction(); }, false);

    // ── Overflow: three dots, no chevron ─────────────────────────────────
    AddCombo(MapLinkToolbar::ActionsMenuName, MLBrush("MapLink.Dots"),
        LOCTEXT("TbMapLinkTip",
                "More MapLink actions for the selected asset(s)."),
        /*bArrowOnly=*/false, /*bDownArrow=*/false);

    // The lift that makes the group read as one set, present at rest and not
    // only on hover.
    const FSlateBrush* Bg = StyleSet.IsValid()
        ? StyleSet->GetBrush("MapLink.GroupBackground") : nullptr;

    return SNew(SBorder)
        .BorderImage(Bg)
        .Padding(FMargin(3.f, 1.f))
        .VAlign(VAlign_Center)
        [ Row ];
}

void FMapLinkEditorModule::RegisterToolbar()
{
    const UMapLinkSettings* S = UMapLinkSettings::Get();
    if (S && !S->bShowMapLinkToolbar)
    {
        return;
    }

    MapLinkToolbar::RegisterComboMenus();

    // ONE menu name. Extending several "harmless" candidates was not harmless:
    // three of them exist in 5.7 and all render into the same visible bar, so
    // the section appeared three times.
    UToolMenu* Bar = UToolMenus::Get()->ExtendMenu(
        TEXT("LevelEditor.LevelEditorToolBar.User"));
    if (!Bar)
    {
        return;
    }

    FToolMenuSection& Section = Bar->FindOrAddSection("MapLink");
    Section.Label = LOCTEXT("TbSection", "MapLink");

    Section.AddDynamicEntry("MapLinkToolbarDynamic",
        FNewToolMenuSectionDelegate::CreateLambda([](FToolMenuSection& In)
    {
        MapLinkToolbar::LogIconResolution();

        In.AddEntry(FToolMenuEntry::InitWidget(
            "MapLinkGroup",
            FMapLinkEditorModule::BuildToolbarGroup(),
            FText::GetEmpty(),
            /*bNoIndent=*/true,
            /*bSearchable=*/false,
            /*bNoPadding=*/true));
    }));
}


void FMapLinkEditorModule::RegisterMenus()
{
    FToolMenuOwnerScoped OwnerScoped(this);
    BindLevelEditorCommands();

    RegisterToolbar();

    const FSlateIcon MenuIcon = GetIcon("MapLink.MenuIcon");

    // ── MapLink top-level menu ────────────────────────────────────
    UToolMenu* MainMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu");
    MainMenu->AddSubMenu("MainMenu", NAME_None, "MapLink",
        LOCTEXT("MapLinkMenu", "MapLink"),
        LOCTEXT("MapLinkMenuTooltip", "MapLink pipeline tools"));

    UToolMenu* MapLinkMenu = UToolMenus::Get()->ExtendMenu(
        "LevelEditor.MainMenu.MapLink");

    // ── v1.18 — menu order is frequency order ─────────────────────
    //
    // Sections render in registration order, so this IS the order the user
    // reads. It used to be Display, Project, Manager, Import, Reimport,
    // Materials, Help — which put project lifecycle (used roughly once) above
    // the Manager and the starred reimport (used every few minutes), and left
    // the single most-clicked entry in the menu sitting seventh.
    //
    // Now: Manager and Import first, then the ★ reimport list, then the things
    // you reach for occasionally, then Help. Import .blend… joined the Manager
    // section rather than keeping a header to itself — one entry is not a
    // section.

    // ── Manager (v1.9) ───────────────────────────────────
    FToolMenuSection& ManagerSection = MapLinkMenu->AddSection(
        "Manager", LOCTEXT("ManagerSection", "Manager"));

    ManagerSection.AddMenuEntry("OpenMapLinkManager",
        LOCTEXT("OpenMapLinkManager", "MapLink Manager…"),
        LOCTEXT("OpenMapLinkManagerTooltip",
            "All imported .blend files, their state, reimport, WP expansion "
            "and MapLink settings."),
        MenuIcon,
        FUIAction(FExecuteAction::CreateLambda([]()
        {
            FGlobalTabmanager::Get()->TryInvokeTab(
                FTabId(FMapLinkEditorModule::ManagerTabName));
        })));

    // ── Import ────────────────────────────────────────────────────
    // v1.18 — no "Import" header: the entry joins Manager, immediately above.
    FToolMenuSection* ManagerSectionPtr = MapLinkMenu->FindSection("Manager");
    FToolMenuSection& ImportSection = ManagerSectionPtr
        ? *ManagerSectionPtr
        : MapLinkMenu->AddSection("Manager",
              LOCTEXT("ManagerSection", "Manager"));

    ImportSection.AddMenuEntry("ImportBlend",
        LOCTEXT("ImportBlend", "Import .blend..."),
        LOCTEXT("ImportBlendTooltip", "Import a .blend file via MapLink."),
        MenuIcon,
        FUIAction(FExecuteAction::CreateLambda([]()
        {
            TArray<FString> Filenames;
            if (!FDesktopPlatformModule::Get()->OpenFileDialog(
                    FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr),
                    TEXT("Select .blend file"), FPaths::ProjectDir(), TEXT(""),
                    TEXT("Blender files (*.blend)|*.blend"),
                    EFileDialogFlags::None, Filenames) || Filenames.Num() == 0)
            {
                return;
            }

            FString Picked = FPaths::ConvertRelativePathToFull(Filenames[0]);

            // v1.19 — a .blend from outside the project root imports fine here
            // and nowhere else: the source is not in the synced tree, so a
            // teammate reimporting it has nothing to reimport from. This is the
            // same defect the Blender sidecar gate refuses, reached through the
            // other door, so it gets the same answer — copy it in first.
            //
            // One branch on the existing entry, not a second "Import external
            // .blend…" button whose difference from this one nobody could
            // state.
            const FString Root =
                UMapLinkSettings::ResolveBlenderProjectRoot(false, false);
            const bool bOutside =
                !Root.IsEmpty() && !FMapLinkUtils::IsPathUnder(Picked, Root);

            if (bOutside)
            {
                const EAppReturnType::Type Answer = FMessageDialog::Open(
                    EAppMsgType::YesNoCancel,
                    FText::Format(NSLOCTEXT("MapLink", "ImportOutsideRoot",
                        "This .blend is outside your MapLink project:\n\n{0}\n\n"
                        "Project root:\n{1}\n\n"
                        "Importing it as-is works on this machine only — the "
                        "source is not in the synced tree, so nobody else can "
                        "reimport it.\n\n"
                        "Copy it into the project first? (Yes copies the asset "
                        "and everything it links, then imports the copy.)"),
                        FText::FromString(Picked),
                        FText::FromString(Root)));

                if (Answer == EAppReturnType::Cancel)
                {
                    return;
                }
                if (Answer == EAppReturnType::Yes)
                {
                    FString Vendored, VendorError;
                    if (!FMapLinkPipeline::VendorBlendIntoProject(
                            Picked, Root, /*bOverwrite=*/false,
                            Vendored, VendorError))
                    {
                        FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
                            NSLOCTEXT("MapLink", "VendorFailedFmt",
                                "Could not copy the asset into the project:\n\n{0}"),
                            FText::FromString(VendorError)));
                        return;
                    }
                    if (!Vendored.IsEmpty())
                    {
                        Picked = Vendored;
                    }
                }
            }

            const EMapLinkResult R = FMapLinkPipeline::RunImport(Picked);
            // v1.21 - the run already names every failed file when it shows
            // its own summary; a second, vaguer dialog on top of it is noise.
            if (R != EMapLinkResult::Success &&
                R != EMapLinkResult::Cancelled &&
                !FMapLinkRunFailures::WasSummaryShown())
            {
                FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
                    NSLOCTEXT("MapLink", "ImportFailedFmt",
                              "Import failed: {0}"),
                    MapLinkResultToText(R)));
            }
        })));

    // ── Reimport (dynamic from bShowInReimportMenu proxies) ──────
    FToolMenuSection& ReimportSection = MapLinkMenu->AddSection(
        "Reimport", LOCTEXT("ReimportSection", "Reimport"));

    ReimportSection.AddDynamicEntry("MapLink_ReimportDynamic",
        FNewToolMenuSectionDelegate::CreateLambda(
            [](FToolMenuSection& InSection)
    {
        TArray<UMapLinkProxyAsset*> AllProxies = GetAllProxies();

        // Collect proxies flagged for the reimport menu, plus (v1.12) every
        // Manager favourite — favourites ARE the quick-access set.
        UMapLinkEditorState* EditorState = UMapLinkEditorState::Get();
        TArray<UMapLinkProxyAsset*> MenuProxies;
        for (UMapLinkProxyAsset* P : AllProxies)
        {
            if (!P)
            {
                continue;
            }
            const FString Stem = FPaths::GetBaseFilename(P->BlendFilePath);
            if (P->bShowInReimportMenu || EditorState->IsFavourite(Stem))
                MenuProxies.Add(P);
        }

        if (MenuProxies.Num() == 0)
        {
            InSection.AddMenuEntry("ReimportNone",
                LOCTEXT("ReimportNone", "(No favourites or flagged proxies)"),
                LOCTEXT("ReimportNoneTip",
                    "Mark an asset as favourite (★) in the MapLink Manager, or "
                    "enable 'Show In Reimport Menu' on a proxy asset."),
                FSlateIcon(), FUIAction());
            return;
        }

        // Favourites first, then alphabetical.
        MenuProxies.Sort([EditorState](const UMapLinkProxyAsset& A,
                                       const UMapLinkProxyAsset& B)
        {
            const FString StemA = FPaths::GetBaseFilename(A.BlendFilePath);
            const FString StemB = FPaths::GetBaseFilename(B.BlendFilePath);
            const bool bFavA = EditorState->IsFavourite(StemA);
            const bool bFavB = EditorState->IsFavourite(StemB);
            if (bFavA != bFavB)
            {
                return bFavA;
            }
            return StemA < StemB;
        });

        for (int32 i = 0; i < MenuProxies.Num(); ++i)
        {
            FString BlendPath = MenuProxies[i]->BlendFilePath;
            FString Stem = FPaths::GetBaseFilename(BlendPath);
            const bool bFav = EditorState->IsFavourite(Stem);

            // v1.12: 5.7 checked format strings forbid conditional format
            // literals — build the label, then prefix the favourite star.
            FString EntryLabel = FString::Printf(TEXT("Reimport %s"), *Stem);
            if (bFav)
            {
                EntryLabel = FString(TEXT("★ ")) + EntryLabel;   // ★
            }

            // Single entry per proxy: prompt for regular vs. force on click.
            InSection.AddMenuEntry(
                FName(*FString::Printf(TEXT("Reimport_%d"), i)),
                FText::FromString(EntryLabel),
                FText::FromString(FString::Printf(
                    TEXT("Reimport %s. You'll be asked for a regular (changed "
                         "files only) or a full force reimport."), *Stem)),
                FSlateIcon(),
                FUIAction(FExecuteAction::CreateLambda([BlendPath, Stem]()
                {
                    if (!FPaths::FileExists(BlendPath))
                    {
                        FMessageDialog::Open(EAppMsgType::Ok,
                            FText::FromString(FString::Printf(
                                TEXT("File not found:\n%s"), *BlendPath)));
                        return;
                    }

                    // v1.13 — explicit labeled buttons (destructive or
                    // surprising semantics are never mapped onto Yes/No).
                    TSharedRef<SCustomDialog> ReimportDialog =
                        SNew(SCustomDialog)
                        .Title(NSLOCTEXT("MapLink", "ReimportDlgTitle",
                                         "MapLink Reimport"))
                        .Content()
                        [
                            SNew(STextBlock)
                            .WrapTextAt(520.0f)
                            .Text(FText::Format(NSLOCTEXT("MapLink",
                                "ReimportDlgPromptFmt",
                                "Reimport {0}?\n\n"
                                "Reimport Changed re-imports only files whose "
                                "hash changed.\nForce Full Reimport re-imports "
                                "every file in the tree, ignoring hashes."),
                                FText::FromString(Stem)))
                        ]
                        .Buttons({
                            SCustomDialog::FButton(NSLOCTEXT("MapLink",
                                "ReimportChangedBtn", "Reimport Changed")),
                            SCustomDialog::FButton(NSLOCTEXT("MapLink",
                                "ReimportForceBtn", "Force Full Reimport")),
                            SCustomDialog::FButton(NSLOCTEXT("MapLink",
                                "ReimportCancelBtn", "Cancel"))
                        });
                    const int32 Choice = ReimportDialog->ShowModal();
                    if (Choice != 0 && Choice != 1)
                    {
                        return;   // Cancel button or window closed (-1)
                    }

                    const bool bForce = (Choice == 1);
                    const EMapLinkResult R = bForce
                        ? FMapLinkPipeline::RunImport(BlendPath)     // full
                        : FMapLinkPipeline::RunReimport(BlendPath);  // changed only

                    const TCHAR* Mode = bForce ? TEXT("Force reimport") : TEXT("Reimport");
                    if (R == EMapLinkResult::Success)
                    {
                        MapLinkNotify::Success(FText::FromString(FString::Printf(
                            TEXT("%s %s complete."), Mode, *Stem)));
                    }
                    else if (R == EMapLinkResult::Cancelled)
                    {
                        MapLinkNotify::Warn(FText::FromString(FString::Printf(
                            TEXT("%s %s cancelled (completed files were saved)."),
                            Mode, *Stem)));
                    }
                    else if (!FMapLinkRunFailures::WasSummaryShown())
                    {
                        FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
                            NSLOCTEXT("MapLink", "ReimportFailedFmt",
                                      "{0} {1} failed: {2}"),
                            FText::FromString(Mode), FText::FromString(Stem),
                            MapLinkResultToText(R)));
                    }
                })));
        }
    }));

    // ── Materials (Sync + Display) ────────────────────────────────
    FToolMenuSection& MatSection = MapLinkMenu->AddSection(
        "Materials", LOCTEXT("MaterialsSection", "Materials"));

    // v1.10 — The manual "Sync Materials to Blender" entry was REMOVED:
    // material sync is an invariant invisible tail step of every material op,
    // never a user-facing button.  The only sanctioned manual trigger is the
    // recovery "Resync Material Library" button on the Manager → Settings tab.
    // FMapLinkMaterialExporter::SyncToBlender() is retained as a dev-only
    // diagnostic (single library, in-place) and is no longer surfaced here.

    MatSection.AddMenuEntry("UpdateMaterialDisplay",
        LOCTEXT("UpdateMaterialDisplay", "Update L_MaterialInstances"),
        LOCTEXT("UpdateMaterialDisplayTip", "Create/update the MI display level."),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateLambda([]()
        {
            EMapLinkResult R = FMapLinkMaterialDisplay::UpdateMaterialDisplayLevel();
            if (R == EMapLinkResult::Success)
            {
                const UMapLinkSettings* S = UMapLinkSettings::Get();
                MapLinkNotify::Success(
                    FText::FromString(FString::Printf(
                        TEXT("Material display updated: %s/%s"),
                        *S->MaterialDisplayScanPath.Path,
                        *S->MaterialDisplayLevelName)));
            }
            else if (R == EMapLinkResult::NoMaterialInstancesFound)
                MapLinkNotify::Warn(
                    FText::FromString(TEXT("No MIs found. Check MapLink settings.")));
            else
                FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
                    NSLOCTEXT("MapLink", "MatDisplayFailedFmt",
                              "Material display update failed: {0}"),
                    MapLinkResultToText(R)));
        })));

    MatSection.AddMenuEntry("OpenMaterialInstanceLibrary",
        LOCTEXT("OpenMaterialInstanceLibrary", "Open Material Instance Blend"),
        LOCTEXT("OpenMaterialInstanceLibraryTooltip",
            "Open the project's Library_MaterialInstances.blend in Blender. "
            "Resolves from MapLink settings or the project manifest location."),
        MenuIcon,
        FUIAction(FExecuteAction::CreateLambda([]()
        {
            FMapLinkProjectSetup::OpenMaterialInstanceLibrary();
        })));

    MatSection.AddMenuEntry("OpenMaterialInstancesLevel",
        LOCTEXT("OpenMaterialInstancesLevel", "Open L_MaterialInstances (Level)"),
        LOCTEXT("OpenMaterialInstancesLevelTooltip",
            "Open the Material Instance display level "
            "(/Game/MapLink/MaterialInstances/L_MaterialInstances.umap) in the "
            "editor. Prompts to save any modified packages first."),
        MenuIcon,
        FUIAction(FExecuteAction::CreateLambda([]()
        {
            FMapLinkProjectSetup::OpenMaterialInstancesLevel();
        })));

    // ── v1.15.5 — Viewport Display submenu (same content as the
    //    viewport-toolbar combo below) ─────────────────────────────
    {
        FToolMenuSection& DisplaySection = MapLinkMenu->AddSection(
            "Display", LOCTEXT("DisplaySection", "Display"));
        DisplaySection.AddSubMenu("MapLinkViewportDisplay",
            LOCTEXT("ViewportDisplay", "Viewport Display"),
            LOCTEXT("ViewportDisplayTip",
                "Asset name labels, per-type filters, child labels, "
                "actor transform lock."),
            FNewToolMenuDelegate::CreateLambda(
                [](UToolMenu* Menu) { FillMapLinkDisplayMenu(Menu); }));
    }

    // ── v1.15.5 — MapLink combo on the level-viewport toolbar.
    //    "LevelEditor.ViewportToolbar" = the 5.4+ toolbar; the legacy
    //    Show menu is extended too so older toolbar modes get the
    //    section as well.  Extending an unused menu name is harmless.
    {
        if (UToolMenu* ViewportToolbar = UToolMenus::Get()->ExtendMenu(
                "LevelEditor.ViewportToolbar"))
        {
            FToolMenuSection& Section =
                ViewportToolbar->FindOrAddSection("MapLink");
            Section.AddEntry(FToolMenuEntry::InitComboButton(
                "MapLinkViewportCombo",
                FUIAction(),
                FNewToolMenuChoice(FNewToolMenuDelegate::CreateLambda(
                    [](UToolMenu* Menu) { FillMapLinkDisplayMenu(Menu); })),
                LOCTEXT("MapLinkCombo", "MapLink"),
                LOCTEXT("MapLinkComboTip",
                    "MapLink viewport display settings."),
                GetIcon("MapLink.ToolbarButton")));
        }

        if (UToolMenu* LegacyShow = UToolMenus::Get()->ExtendMenu(
                "LevelEditor.LevelViewportToolBar.Show"))
        {
            FToolMenuSection& Section =
                LegacyShow->FindOrAddSection("MapLink");
            Section.AddSubMenu("MapLinkShowSub",
                LOCTEXT("MapLinkCombo", "MapLink"),
                LOCTEXT("MapLinkComboTip",
                    "MapLink viewport display settings."),
                FNewToolMenuDelegate::CreateLambda(
                    [](UToolMenu* Menu) { FillMapLinkDisplayMenu(Menu); }));
        }
    }

    // ── Project (v1.12) — New / Open / Unlink ───────────────────
    FToolMenuSection& ProjectSection = MapLinkMenu->AddSection(
        "Project", LOCTEXT("ProjectSection", "Project"));

    ProjectSection.AddMenuEntry("NewMapLinkProject",
        LOCTEXT("NewMapLinkProject", "New MapLink Project…"),
        LOCTEXT("NewMapLinkProjectTooltip",
            "Create a fresh MapLink project: scaffold the Blender authoring "
            "tree (LI/SM/BP + material libraries) and link this UE project "
            "to it. One MapLink project per UE project."),
        MenuIcon,
        FUIAction(
            FExecuteAction::CreateLambda([]()
            {
                FMapLinkProjectSetup::ShowNewProjectDialog();
            }),
            FCanExecuteAction::CreateLambda([]()
            {
                return !FMapLinkProjectSetup::IsSetupActive(nullptr);
            })));

    ProjectSection.AddMenuEntry("OpenMapLinkProject",
        LOCTEXT("OpenMapLinkProject", "Open MapLink Project…"),
        LOCTEXT("OpenMapLinkProjectTooltip",
            "Link this UE project to an existing MapLink project: pick its "
            "root folder (the one containing maplink_project.json) and the "
            "settings are applied automatically."),
        MenuIcon,
        FUIAction(
            FExecuteAction::CreateLambda([]()
            {
                FMapLinkProjectSetup::ShowOpenProjectDialog();
            }),
            FCanExecuteAction::CreateLambda([]()
            {
                return !FMapLinkProjectSetup::IsSetupActive(nullptr);
            })));

    ProjectSection.AddMenuEntry("UnlinkMapLinkProject",
        LOCTEXT("UnlinkMapLinkProject", "Unlink Project"),
        LOCTEXT("UnlinkMapLinkProjectTooltip",
            "Detach this UE project from its MapLink project. You choose "
            "whether the data stays (re-attach anytime) or is deleted "
            "entirely for a fresh start."),
        FSlateIcon(),
        FUIAction(
            FExecuteAction::CreateLambda([]()
            {
                FMapLinkProjectSetup::UnlinkProject();
            }),
            FCanExecuteAction::CreateLambda([]()
            {
                // Enabled when linked OR half-linked (root set, manifest gone).
                const UMapLinkSettings* S = UMapLinkSettings::Get();
                return S && !S->BlenderProjectRoot.Path.IsEmpty();
            })));

    // ── Help ──────────────────────────────────────────────────────
    FToolMenuSection& HelpSection = MapLinkMenu->AddSection(
        "Help", LOCTEXT("HelpSection", "Help"));

    HelpSection.AddMenuEntry("InstallBlenderAddon",
        LOCTEXT("InstallBlenderAddon", "Install / Update Blender Addon"),
        LOCTEXT("InstallBlenderAddonTooltip",
            "Install (or update in place) and enable the MapLink Blender addon "
            "in your Blender, and set its UE-plugin-dir preference. Resolves "
            "Blender automatically and uses the addon bundled with this plugin "
            "— close any open Blender session first so files aren't locked."),
        MenuIcon,
        FUIAction(FExecuteAction::CreateLambda([]()
        {
            FMapLinkProjectSetup::InstallBlenderAddon();
        })));

    HelpSection.AddMenuEntry("CopyDiagnostics",
        LOCTEXT("CopyDiagnostics", "Copy Diagnostics"),
        LOCTEXT("CopyDiagnosticsTooltip",
            "Copy a markdown support report to the clipboard: plugin/addon/"
            "engine versions, project manifest, settings snapshot, proxy "
            "list, and the session's LogMapLink tail."),
        FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Settings"),
        FUIAction(FExecuteAction::CreateLambda([]()
        {
            FMapLinkDiagnostics::CopyToClipboard();
        })));

    // v1.18 — the docs URL lives in ONE place, and the entry greys out while
    // it is empty instead of looking live and then apologising. A menu item
    // that can never succeed should not be clickable; when docs ship, set
    // MapLinkDocsURL and the entry lights up on its own.
    HelpSection.AddMenuEntry("MapLinkDocumentation",
        LOCTEXT("MapLinkDocumentation", "Documentation"),
        TAttribute<FText>::Create(TAttribute<FText>::FGetter::CreateLambda([]()
        {
            return FString(MapLinkDocsURL).IsEmpty()
                ? NSLOCTEXT("MapLink", "MapLinkDocumentationTooltipUnset",
                    "MapLink documentation is not published yet.")
                : NSLOCTEXT("MapLink", "MapLinkDocumentationTooltip",
                    "Open the MapLink documentation in your browser.");
        })),
        FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Help"),
        FUIAction(
            FExecuteAction::CreateLambda([]()
            {
                FPlatformProcess::LaunchURL(MapLinkDocsURL, nullptr, nullptr);
            }),
            FCanExecuteAction::CreateLambda([]()
            {
                return !FString(MapLinkDocsURL).IsEmpty();
            })));

    // ── Actor context menu ────────────────────────────────────────
    RegisterActorContextMenu();
}

// ---------------------------------------------------------------------------
// Actor context menu (Viewport + Outliner)
// ---------------------------------------------------------------------------

void FMapLinkEditorModule::RegisterActorContextMenu()
{
    UToolMenu* ActorMenu = UToolMenus::Get()->ExtendMenu(
        "LevelEditor.ActorContextMenu");
    if (!ActorMenu) return;

    FToolMenuSection& Section = ActorMenu->AddSection(
        "MapLink",
        LOCTEXT("MapLinkActorSection", "MapLink"),
        FToolMenuInsert(NAME_None, EToolMenuInsertType::First));

    const FSlateIcon MenuIcon = GetIcon("MapLink.MenuIcon");

    Section.AddDynamicEntry("MapLink_ActorDynamic",
        FNewToolMenuSectionDelegate::CreateLambda(
            [MenuIcon](FToolMenuSection& InSection)
    {
        if (!GEditor) return;
        USelection* Sel = GEditor->GetSelectedActors();
        if (!Sel) return;

        TArray<AActor*> SelectedActors;
        for (FSelectionIterator It(*Sel); It; ++It)
            if (AActor* A = Cast<AActor>(*It)) SelectedActors.Add(A);
        if (SelectedActors.Num() == 0) return;

        AActor* First = SelectedActors[0];

        // ── MapLink proxy entries ────────────────────────────────
        UMapLinkProxyAsset* Proxy =
            MapLinkActorResolve::ResolveProxyFromActor(First);

        if (Proxy)
        {
            FString BlendPath = Proxy->BlendFilePath;

            InSection.AddMenuEntry("MapLink_Actor_EditBlend",
                LOCTEXT("ActorEditBlend", "Edit source .blend"),
                LOCTEXT("ActorEditBlendTip", "Open in Blender."),
                MenuIcon,
                FUIAction(FExecuteAction::CreateLambda([BlendPath]()
                { FMapLinkBlenderBridge::OpenBlenderGUI(BlendPath); })));

            InSection.AddMenuEntry("MapLink_Actor_OpenExplorer",
                LOCTEXT("ActorOpenExplorer", "Open source .blend in Explorer"),
                LOCTEXT("ActorOpenExplorerTip", "Open the .blend folder."),
                FSlateIcon(),
                FUIAction(FExecuteAction::CreateLambda([BlendPath]()
                { FMapLinkBlenderBridge::OpenInExplorer(BlendPath); })));

            InSection.AddMenuEntry("MapLink_Actor_Reimport",
                LOCTEXT("ActorReimport", "Reimport"),
                LOCTEXT("ActorReimportTip", "Reimport from source .blend."),
                FSlateIcon(),
                FUIAction(FExecuteAction::CreateLambda([BlendPath]()
                { FMapLinkPipeline::RunReimport(BlendPath); })));

            // v1.14 — same semantics as MapLink menu > Reimport > Force
            // Full Reimport (whole tree, hashes ignored), confirmed.
            InSection.AddMenuEntry("MapLink_Actor_ForceReimport",
                LOCTEXT("ActorForceReimport", "Force Full Reimport"),
                LOCTEXT("ActorForceReimportTip",
                    "Re-import the whole tree from source .blend, ignoring "
                    "hashes (nuke-and-recreate; confirmation shown)."),
                FSlateIcon(),
                FUIAction(FExecuteAction::CreateLambda([BlendPath]()
                {
                    const FString Stem = FPaths::GetBaseFilename(BlendPath);
                    TSharedRef<SCustomDialog> Dlg =
                        SNew(SCustomDialog)
                        .Title(LOCTEXT("ActorForceReimportDlgTitle",
                                       "MapLink Force Full Reimport"))
                        .Content()
                        [
                            SNew(STextBlock)
                            .WrapTextAt(520.0f)
                            .Text(FText::Format(
                                LOCTEXT("ActorForceReimportDlgFmt",
                                "Force Full Reimport {0}?\n\n"
                                "Every file in the tree is re-imported, "
                                "ignoring hashes (nuke-and-recreate)."),
                                FText::FromString(Stem)))
                        ]
                        .Buttons({
                            SCustomDialog::FButton(
                                LOCTEXT("ActorForceReimportGoBtn",
                                        "Force Full Reimport")),
                            SCustomDialog::FButton(
                                LOCTEXT("ActorForceReimportCancelBtn",
                                        "Cancel"))
                        });
                    if (Dlg->ShowModal() != 0)
                    {
                        return;
                    }
                    FMapLinkPipeline::RunImport(BlendPath);
                })));
        }

        // ── Edit Level (LevelInstance) ────────────────────────────
        ALevelInstance* LIActor = Cast<ALevelInstance>(First);
        if (LIActor)
        {
            FString MapPath = MapLinkActorResolve::GetLevelInstanceMapPath(LIActor);
            if (!MapPath.IsEmpty())
            {
                InSection.AddMenuEntry("MapLink_Actor_EditLevel",
                    LOCTEXT("ActorEditLevel", "Edit Level (Standalone)"),
                    LOCTEXT("ActorEditLevelTip",
                        "Open as active level. Alt+PageDown / Alt+PageUp."),
                    FSlateIcon(),
                    // Goes through the same action as Alt+PageDown and the
                    // toolbar rather than loading the map itself: this entry
                    // used to set PreviousLevelPath directly, which bypassed
                    // the drill stack and left the camera and the return path
                    // disagreeing after the first level.
                    FUIAction(FExecuteAction::CreateStatic(
                        &FMapLinkEditorModule::EditLevelStandaloneAction)));
            }
        }
    }));
}

// ---------------------------------------------------------------------------
// v1.9 — MapLink Manager tab
// ---------------------------------------------------------------------------

TSharedRef<SDockTab> FMapLinkEditorModule::SpawnMapLinkManagerTab(
    const FSpawnTabArgs& Args)
{
    // v1.19 — vendor-in can leave a record of Material Instances the
    // destination project does not have. Opening the Manager is the first
    // thing anyone does in a project, so it is where UE notices.
    FMapLinkMaterialLibraryExchange::ProcessPendingImport();

    return SNew(SDockTab)
        .TabRole(ETabRole::NomadTab)
        [
            SNew(SMapLinkManagerWindow)
        ];
}

// ---------------------------------------------------------------------------
// v1.11 — MapLink Asset Viewer tab
// ---------------------------------------------------------------------------

TSharedRef<SDockTab> FMapLinkEditorModule::SpawnMapLinkAssetViewerTab(
    const FSpawnTabArgs& Args)
{
    TSharedRef<SMapLinkAssetViewer> Viewer =
        SNew(SMapLinkAssetViewer).InitialStem(PendingViewerStem);

    ActiveViewer = Viewer;          // weak — viewer destroyed with the tab
    PendingViewerStem.Reset();

    return SNew(SDockTab)
        .TabRole(ETabRole::NomadTab)
        [
            Viewer
        ];
}

void FMapLinkEditorModule::OpenAssetViewer(const FString& InitialStem)
{
    // Already-open viewer → just retarget it.
    if (TSharedPtr<SMapLinkAssetViewer> Live = ActiveViewer.Pin())
    {
        Live->SelectByStem(InitialStem);
        FGlobalTabmanager::Get()->TryInvokeTab(FTabId(AssetViewerTabName));
        return;
    }

    // Otherwise seed the stem and spawn the tab (spawn reads PendingViewerStem).
    PendingViewerStem = InitialStem;
    FGlobalTabmanager::Get()->TryInvokeTab(FTabId(AssetViewerTabName));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMapLinkEditorModule, MapLinkEditor)
