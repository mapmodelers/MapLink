// Copyright MapModelers. All Rights Reserved.
//
// v1.7.3 — Actor context menu, menu icons, keyboard shortcuts,
//           Material Display, LI_World reimport, Auto-Update ticker.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Framework/Commands/Commands.h"
#include "Styling/SlateStyle.h"
#include "Containers/Ticker.h"
// FSlateIcon (returned by value from GetIcon below) arrives via
// Framework/Commands/Commands.h -> UICommandInfo.h. Do not drop that include.

class FAssetTypeActions_MapLinkProxy;
class UToolMenu;                    // v1.16 — toolbar / actions menu
class SWidget;
class FSpawnTabArgs;
class SDockTab;
class SMapLinkAssetViewer;   // v1.11 — Asset Viewer

// ---------------------------------------------------------------------------
// Keyboard shortcuts
// ---------------------------------------------------------------------------

class FMapLinkCommands : public TCommands<FMapLinkCommands>
{
public:
    FMapLinkCommands();
    virtual void RegisterCommands() override;

    /** Alt+PageDown — Open selected LevelInstance as standalone level. */
    TSharedPtr<FUICommandInfo> EditLevelStandalone;

    /** Alt+PageUp — Return to the level that was open before. */
    TSharedPtr<FUICommandInfo> ReturnToPreviousLevel;
};

// ---------------------------------------------------------------------------
// Module
// ---------------------------------------------------------------------------

class FMapLinkEditorModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

    /** Style set owning the MapLink icon brushes. */
    static TSharedPtr<FSlateStyleSet> StyleSet;

    /** Style set name, shared by every FSlateIcon that resolves against it. */
    static const FName StyleSetName;

    /** Keys actually registered in RegisterStyle, i.e. those whose PNG was
     *  present. Tracked here rather than queried back out of the style set so
     *  GetIcon needs no lookup API. */
    static TSet<FName> RegisteredIcons;

    /** FSlateIcon for a MapLink style key, or an empty icon when that brush
     *  was not registered because its PNG is absent.
     *
     *  Always go through this rather than constructing
     *  FSlateIcon("MapLinkStyle", Key) directly: an FSlateIcon naming a brush
     *  the style set does not have logs a lookup failure on every draw, and
     *  the art ships incrementally, so absent is a normal state. */
    static FSlateIcon GetIcon(const FName& Key);

    /** Previous level path for Alt+PageUp navigation. */
    static FString PreviousLevelPath;

    /** v1.9 — MapLink Manager nomad tab id. */
    static const FName ManagerTabName;

    /** v1.11 — MapLink Asset Viewer nomad tab id. */
    static const FName AssetViewerTabName;

    /** v1.11 — Open the read-only Asset Viewer on the given proxy stem.
     *  Empty stem → first proxy. Reuses an already-open viewer tab. */
    void OpenAssetViewer(const FString& InitialStem);

private:
    TSharedPtr<FAssetTypeActions_MapLinkProxy> MapLinkProxyActions;

    // ── Auto-Update ──────────────────────────────────────────────
    FTSTicker::FDelegateHandle AutoUpdateTickerHandle;
    TMap<FString, FDateTime> LastSeenModTimes;
    /** v1.9 — debounce: blend path → last-change time (seconds).
     *  Reimport fires only after a quiet period, so a burst of saves
     *  (e.g. linked-file save cascade in Blender) triggers ONE reimport. */
    TMap<FString, double> PendingChanges;
    /** v1.9 — cascade cache: parent LI_ stem → descendant .blend paths.
     *  Rebuilt periodically so the ticker stays cheap. */
    TMap<FString, TArray<FString>> CascadeDescendantCache;
    double CascadeCacheBuiltSec = 0.0;
    bool bAutoUpdateInProgress = false;

    bool OnAutoUpdateTick(float DeltaTime);

    // ── Menus & keybinds ─────────────────────────────────────────
    void RegisterMenus();

    /** v1.16 — MapLink section on the Level Editor toolbar. Gated on the
     *  per-user "Show MapLink Toolbar" preference. */
    void RegisterToolbar();

    /** v1.16 — the toolbar combo's contents: actions for the selected
     *  MapLink actor(s). Static so the menu delegate needs no instance. */
public:
    static void FillMapLinkActionsMenu(UToolMenu* Menu);

    /** v1.16 — the whole MapLink toolbar group as one widget. Owned rather
     *  than assembled from FToolMenuEntry blocks so the group can carry its
     *  own background: StyleNameOverride resolves against the editor's style
     *  set, not ours. */
    static TSharedRef<class SWidget> BuildToolbarGroup();
private:
    /** v1.9 — spawn the MapLink Manager nomad tab. */
    TSharedRef<SDockTab> SpawnMapLinkManagerTab(const FSpawnTabArgs& Args);
    /** v1.11 — spawn the MapLink Asset Viewer nomad tab. */
    TSharedRef<SDockTab> SpawnMapLinkAssetViewerTab(const FSpawnTabArgs& Args);
    /** v1.11 — stem to seed the viewer with on next spawn; live viewer ref. */
    FString PendingViewerStem;
    TWeakPtr<SMapLinkAssetViewer> ActiveViewer;
    void RegisterActorContextMenu();
    void RegisterStyle();
    void BindLevelEditorCommands();

    static void EditLevelStandaloneAction();
    static void ReturnToPreviousLevelAction();
};
