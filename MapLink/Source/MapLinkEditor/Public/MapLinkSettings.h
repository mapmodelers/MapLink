// Copyright MapModelers. All Rights Reserved.
//
// v1.7.3 — Material Display settings + Auto-Update interval.
// v1.8   — Added BlenderProjectRoot + BP-stand-in path helpers.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "MapLinkSettings.generated.h"

/** v1.14 — how Blender collections map to UE outliner / World Partition
 *  folders when MapLink places actors.  These are OUTLINER folders only:
 *  all content still routes through GetMapLinkContentRoot() → /Game/MapLink/. */
UENUM()
enum class EMapLinkFolderMode : uint8
{
    /** No directories except the mandatory MapLink/<Root> dir. */
    None                UMETA(DisplayName="None"),

    /** One Blender collection → one UE folder.  DEFAULT — reproduces the
     *  pre-v1.14 behaviour, so existing projects are unaffected on upgrade. */
    CollectionFolders   UMETA(DisplayName="Collection = folder"),

    /** Instanced collections also become folders, nested under their parent
     *  collection's directory. */
    CollectionAndInstanceFolders
                        UMETA(DisplayName="Collection + instanced collection = folders"),
};

/** v1.14 — where folder creation applies (orthogonal to the mode). */
UENUM()
enum class EMapLinkFolderScope : uint8
{
    /** Only in the currently expanded level (pre-v1.14 behaviour, default). */
    ExpandedLevelOnly   UMETA(DisplayName="Expanded level only"),

    /** Across all created levels (expanded level + the individual generated
     *  LI_ levels). */
    AllLevels           UMETA(DisplayName="All created levels"),

    /** Hidden alias of AllLevels — both spellings exist in shipped configs
     *  (the v1.14 iterations wrote each), so config import must accept
     *  either without a LoadConfig warning. */
    AllCreatedLevels = AllLevels    UMETA(Hidden),
};

UCLASS(config=Game, defaultconfig, meta=(DisplayName="MapLink"))
class MAPLINKEDITOR_API UMapLinkSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    // ── v1.18 — shared vs per-user ────────────────────────────────────
    //
    // Two storage layers live in this one settings object and the difference
    // matters more than anything else on the page:
    //
    //   UPROPERTY(Config, ...)              -> Config/DefaultGame.ini
    //                                          SOURCE CONTROLLED. Changing one
    //                                          changes it for the whole team.
    //   UPROPERTY(EditAnywhere, Transient)  -> EditorPerProjectUserSettings.ini
    //                                          (written by hand -- see the
    //                                          per-user project paths block in
    //                                          MapLinkSettings.cpp). Yours alone.
    //
    // The second group is every machine-local path: a .blend library on your
    // D: drive, a Blender project root. Writing one of those into
    // DefaultGame.ini is the most damaging mistake this page allows, because
    // it breaks on every other machine and it breaks silently.
    //
    // Up to v1.17 the distinction was stated once, in a sentence above the
    // search box, and then never shown again -- two adjacent rows in the same
    // category could land in different files with nothing to tell them apart.
    // Every per-user property now carries "[this machine]" in its display
    // name, so the row itself says where it goes. Keep the marker on any new
    // Transient property, and off any new Config one.

    UMapLinkSettings();

    virtual FName GetContainerName() const override { return TEXT("Project"); }
    virtual FName GetCategoryName() const override  { return TEXT("Plugins"); }
    virtual FName GetSectionName() const override   { return TEXT("MapLink"); }

    // ── Blender ────────────────────────────────────────────────

    /** Full path to blender.exe.  Leave this empty and MapLink finds
     *  Blender itself: it checks this setting, then the
     *  MAPLINK_BLENDER_EXE environment variable, then the standard
     *  install folders, the Windows file association and PATH.
     *
     *  Set it when Blender lives somewhere MapLink cannot guess — a
     *  second drive, a Steam copy, a portable install.  MapLink needs
     *  Blender 5.0 or newer and refuses anything older, naming the
     *  version it found.
     *
     *  Per-machine, like every path marked [this machine]: it is stored
     *  in EditorPerProjectUserSettings.ini, never in the
     *  source-controlled DefaultGame.ini. */
    UPROPERTY(EditAnywhere, Transient, Category="Blender",
              meta=(DisplayName="Blender Executable  [this machine]",
                    FilePathFilter="Blender executable (blender.exe)|blender.exe"))
    FFilePath BlenderExecutable;

    // ── General ───────────────────────────────────────────────────────

    /** Auto-Update polling interval in seconds. */
    UPROPERTY(Config, EditAnywhere, Category="General",
              meta=(DisplayName="Auto-Update Poll Interval (s)",
                    ClampMin="1.0", ClampMax="30.0"))
    float AutoUpdatePollInterval;

    /** v1.13.1 — Show import timestamps in the Manager in LOCAL time (ON,
     *  default) instead of raw UTC. Proxies store the import instant in UTC;
     *  this only affects display. */
    UPROPERTY(Config, EditAnywhere, Category="General",
              meta=(DisplayName="Show Times In Local Time"))
    bool bShowTimesInLocalTime;

    // ── Performance (v1.9) ────────────────────────────────────────────

    /** Run Phase-1 Blender exports for all cache-stale nodes in parallel
     *  BEFORE the sequential import loop.  Cuts multi-node import times
     *  roughly by the worker count (Blender startup dominates).  Failed
     *  exports safely fall back to the sequential per-node launch. */
    UPROPERTY(Config, EditAnywhere, Category="Performance",
              meta=(DisplayName="Parallel FBX Warm Pass"))
    bool bParallelWarmPass;

    /** How many blender.exe processes the warm pass may run at once. */
    UPROPERTY(Config, EditAnywhere, Category="Performance",
              meta=(DisplayName="Max Parallel Blender Exports",
                    ClampMin="1", ClampMax="16",
                    EditCondition="bParallelWarmPass"))
    int32 MaxParallelBlenderExports;

    /** v1.9.4 — Hard watchdog (seconds) on every headless Blender launch.
     *  If a blender.exe doesn't exit within this many seconds it is
     *  terminated and the launch is reported as failed, so a hung Blender
     *  (corrupt .blend, modal popup, addon exception) can never freeze the
     *  editor indefinitely.  Set 0 to disable the watchdog (legacy
     *  behaviour — wait forever).  Default 600 (10 min). */
    UPROPERTY(Config, EditAnywhere, Category="Performance",
              meta=(DisplayName="Blender Timeout (s, 0 = off)",
                    ClampMin="0", ClampMax="7200"))
    int32 BlenderTimeoutSeconds;

    /** Project-wide master switch for the per-proxy "Auto Reimport
     *  (Recursive)" flag.  When ON (default), a proxy that has BOTH
     *  Auto-Update and Auto Reimport (Recursive) enabled also watches
     *  every .blend in its dependency tree — editing a child SM_/LI_/BP_
     *  then reimports the parent (which cascades the change down).  When
     *  OFF, recursive watching is globally disabled regardless of the
     *  per-proxy flag. */
    // v1.17 — category is "AutoUpdate", not "Auto-Update".  UE derives the
    // section header from the category FName via NameToDisplayString, which
    // inserts a space after the hyphen: the panel rendered "Auto- Update".
    UPROPERTY(Config, EditAnywhere, Category="AutoUpdate",
              meta=(DisplayName="Enable Recursive Auto-Reimport (master)"))
    bool bCascadeAutoReimport;

    // ── Source Control (v1.13) ────────────────────────────────────────

    /** v1.13 — Safety net around nuke-and-recreate.  When ON and a P4
     *  provider with changelist support is active, reimport shelves a
     *  snapshot BEFORE its destructive phase: every /Game/MapLink/<stem>/
     *  package about to be nuked, plus the source .blend file(s) of the
     *  reimported tree and their P4-tracked sidecars (<stem>_analysis.json,
     *  <stem>_filehash.txt) — so the shelf captures the UE assets AND the
     *  exact .blend versions they came from.  Shelve failures are
     *  NON-FATAL: a toast warning is shown and the reimport proceeds. */
    UPROPERTY(Config, EditAnywhere, Category="Source Control",
              meta=(DisplayName="Shelve Before Reimport"))
    bool bShelveBeforeReimport;

    // ── BP Reference Mode (v1.8) ──────────────────────────────────────

    /** Root of the Blender project on disk — the directory that
     *  contains the BP/ LI/ SM/ subfolders.  BP stand-ins land at
     *      <BlenderProjectRoot>/BP/<BPName>/<BPName>.blend
     *  and the headless builder loads its template from
     *      <BlenderProjectRoot>/BP/BP_Template.blend
     *
     *  v1.15 — per-machine path, persisted PER-USER (see the
     *  "user project paths" block below), deliberately NOT a Config
     *  property: the old Config flag targeted DefaultGame.ini, which is
     *  source-controlled and usually read-only under P4, so SaveConfig()
     *  silently dropped the value and every editor session re-prompted
     *  "Locate project root".  If left empty, MapLink will auto-detect by
     *  sampling existing UMapLinkProxyAsset.BlendFilePaths on first use. */
    UPROPERTY(EditAnywhere, Transient, Category="BP Reference Mode",
              meta=(DisplayName="Blender Project Root  [this machine]"))
    FDirectoryPath BlenderProjectRoot;

    // ── Outliner Folders (v1.14) ──────────────────────────────────────

    /** How Blender collections map to UE outliner/WP folders during
     *  expansion and (per FolderScope) level creation.  Default
     *  "Collection = folder" preserves the pre-v1.14 behaviour. */
    UPROPERTY(Config, EditAnywhere, Category="Outliner Folders",
              meta=(DisplayName="Folder Creation Mode"))
    EMapLinkFolderMode FolderMode;

    /** Whether folder creation applies only to the expanded level (default,
     *  pre-v1.14 behaviour) or also inside every generated LI_ level. */
    UPROPERTY(Config, EditAnywhere, Category="Outliner Folders",
              meta=(DisplayName="Folder Creation Scope"))
    EMapLinkFolderScope FolderScope;

    /** Null-safe accessors (fall back to the pre-v1.14 defaults). */
    static EMapLinkFolderMode  GetFolderMode();
    static EMapLinkFolderScope GetFolderScope();

    // ── Viewport (v1.15) ──────────────────────────────────────────────

    /** Transform-lock MapLink-placed actors in the level viewport.
     *  Actors stay clickable/selectable but the gizmo (and the Details
     *  transform) won't move them — hand-edits would be stomped by the
     *  next reimport anyway.  Uses the stock editor actor lock
     *  (bLockLocation), no custom viewport interception.  Applied to
     *  MapLink actors on map open, on placement, and swept live when
     *  this setting changes.  Delete/rename are NOT blocked. */
    UPROPERTY(Config, EditAnywhere, Category="Viewport",
              meta=(DisplayName="Lock MapLink Actor Transforms"))
    bool bLockMapLinkActorTransforms;

    /** Draw floating asset-name labels over MapLink actors in the level
     *  viewport (same idea as a Blueprint vec3 "Show 3D Widget" label).
     *  Editor-only debug draw — nothing is added to the level, nothing
     *  shows in PIE.  Per-user preference, persisted per-user like the
     *  project paths (never written to DefaultGame.ini). */
    UPROPERTY(EditAnywhere, Transient, Category="Viewport",
              meta=(DisplayName="Show Asset Names  [this machine]"))
    bool bShowAssetNames;

    /** v1.16 — MapLink section on the Level Editor toolbar (Asset Viewer,
     *  Manager with its status light, and the MapLink actions menu).
     *  Per-user like the other display preferences: one artist hiding it must
     *  not change the toolbar for the rest of the team. */
    UPROPERTY(EditAnywhere, Transient, Category="Viewport",
              meta=(DisplayName="Show MapLink Toolbar  [this machine]"))
    bool bShowMapLinkToolbar;

    /** v1.15.4 — also label CHILD assets: nested Level Instances inside
     *  loaded LIs (level viewport) and linked instances in the Asset
     *  Viewer.  Children draw in a whitened tint of their type color.
     *  Per-user preference (same layer as bShowAssetNames). */
    UPROPERTY(EditAnywhere, Transient, Category="Viewport",
              meta=(DisplayName="Show Children (Asset Names)  [this machine]",
                    EditCondition="bShowAssetNames"))
    bool bShowChildAssetNames;

    /** v1.15.1 — per-type visibility of the name labels.
     *  v1.15.4 — per-user (display preference, same layer as
     *  bShowAssetNames), so the viewport dropdown can toggle them
     *  without touching DefaultGame.ini. */
    UPROPERTY(EditAnywhere, Transient, Category="Viewport",
              meta=(DisplayName="Show Names: LI_ (Level Instances)  [this machine]",
                    EditCondition="bShowAssetNames"))
    bool bShowNamesLI;

    UPROPERTY(EditAnywhere, Transient, Category="Viewport",
              meta=(DisplayName="Show Names: SM_ (Static Meshes)  [this machine]",
                    EditCondition="bShowAssetNames"))
    bool bShowNamesSM;

    UPROPERTY(EditAnywhere, Transient, Category="Viewport",
              meta=(DisplayName="Show Names: BP_ (Blueprints)  [this machine]",
                    EditCondition="bShowAssetNames"))
    bool bShowNamesBP;

    /** v1.15.1 — label colors.  A label uses its TYPE color while the
     *  asset is up to date; an OUT-OF-DATE asset overrides to
     *  NameColorOutOfDate (same yellow as the Manager's state column). */
    UPROPERTY(Config, EditAnywhere, Category="Viewport",
              meta=(DisplayName="Name Color: LI_",
                    EditCondition="bShowAssetNames"))
    FLinearColor NameColorLI;

    UPROPERTY(Config, EditAnywhere, Category="Viewport",
              meta=(DisplayName="Name Color: SM_",
                    EditCondition="bShowAssetNames"))
    FLinearColor NameColorSM;

    UPROPERTY(Config, EditAnywhere, Category="Viewport",
              meta=(DisplayName="Name Color: BP_",
                    EditCondition="bShowAssetNames"))
    FLinearColor NameColorBP;

    UPROPERTY(Config, EditAnywhere, Category="Viewport",
              meta=(DisplayName="Name Color: Out of Date",
                    EditCondition="bShowAssetNames"))
    FLinearColor NameColorOutOfDate;

    /** Which revision of the MapLink colour code this project's ini holds.
     *  Below MapLinkPalette::Version, the four colours above are reset once
     *  on load — see ApplyPaletteVersionHeal. Hidden: it is bookkeeping, not
     *  a setting. */
    UPROPERTY(Config)
    int32 NameColorPaletteVersion = 0;

    /** Reset the name colours to MapLinkPalette when the ini predates it. */
    void ApplyPaletteVersionHeal();

    /** Max labels drawn per frame — nearest first (dense-scene guard). */
    UPROPERTY(Config, EditAnywhere, Category="Viewport",
              meta=(DisplayName="Max Name Labels",
                    ClampMin="1", ClampMax="2000",
                    EditCondition="bShowAssetNames"))
    int32 MaxNameLabels;

    /** Labels beyond this distance (UU) from the camera are culled. */
    UPROPERTY(Config, EditAnywhere, Category="Viewport",
              meta=(DisplayName="Max Name Label Distance (UU)",
                    ClampMin="500", ClampMax="500000",
                    EditCondition="bShowAssetNames"))
    float MaxNameLabelDistance;

    /** v1.15.1 — emergency valve for the World Partition folder prune
     *  (actor-descriptor based).  Leave ON: this is what keeps the
     *  outliner free of dead empty MapLink folders.  Turn OFF only if an
     *  outliner assert is ever traced to the prune — that diagnosis is
     *  exactly what this switch exists to isolate. */
    UPROPERTY(Config, EditAnywhere, Category="Viewport",
              meta=(DisplayName="Prune Empty MapLink Folders (WP)"))
    bool bPruneWPFolders;

    // ── Material Sync ─────────────────────────────────────────────────
    // v1.15 — like BlenderProjectRoot these are per-user project paths
    // (manifest-applied absolute paths / manifest-derived content paths),
    // persisted to EditorPerProjectUserSettings.ini, NOT DefaultGame.ini.

    UPROPERTY(EditAnywhere, Transient, Category="Material Sync",
              meta=(DisplayName="Materials Library .blend  [this machine]",
                    FilePathFilter="Blender files (*.blend)|*.blend"))
    FFilePath MaterialsLibraryBlend;

    UPROPERTY(EditAnywhere, Transient, Category="Material Sync",
              meta=(DisplayName="Material Instances .blend  [this machine]",
                    FilePathFilter="Blender files (*.blend)|*.blend"))
    FFilePath MaterialInstancesBlend;

    UPROPERTY(EditAnywhere, Transient, Category="Material Sync",
              meta=(DisplayName="Material Instance Content Path  [this machine]", ContentDir))
    FDirectoryPath MaterialInstanceContentPath;

    // ── Material Display ──────────────────────────────────────────────

    UPROPERTY(EditAnywhere, Transient, Category="Material Display",
              meta=(DisplayName="Material Display Scan Path  [this machine]", ContentDir))
    FDirectoryPath MaterialDisplayScanPath;

    UPROPERTY(Config, EditAnywhere, Category="Material Display",
              meta=(DisplayName="Material Display Level Name"))
    FString MaterialDisplayLevelName;

    UPROPERTY(Config, EditAnywhere, Category="Material Display",
              meta=(DisplayName="Grid Spacing", ClampMin="50", ClampMax="2000"))
    float MaterialDisplayGridSpacing;

    UPROPERTY(Config, EditAnywhere, Category="Material Display",
              meta=(DisplayName="Grid Columns", ClampMin="1", ClampMax="50"))
    int32 MaterialDisplayGridColumns;

    // ── v1.16 — Textures ──────────────────────────────────────────────

    /**
     * Sources above 8 bits per channel (RGBA16, G16, RGBA32F, R16F, R32F) are
     * accepted and scaled down to the 8-bit TGA container. Every reduction is
     * recorded in the run manifest and sets lossy_reduction on the policy
     * entry, because height maps terrace visibly when they lose depth.
     *
     * Turn this OFF and such textures are skipped and reported instead — the
     * right choice for a project whose displacement maps matter more than
     * having every texture conform.
     */
    UPROPERTY(Config, EditAnywhere, Category="Textures",
              meta=(DisplayName="Allow bit-depth reduction to 8-bit"))
    bool bAllowBitDepthReduction = true;

    /**
     * Default long-edge cap for the Blender-facing preview PNGs written under
     * <Root>/TexturePreviews/. Overridable per texture through the policy
     * file's preview_max_res.
     *
     * v1.16 stores and validates this; FMapLinkTexturePreviewGen consumes it in
     * Phase 2. It is here now so a project can be configured before the
     * generator exists rather than after.
     */
    UPROPERTY(Config, EditAnywhere, Category="Textures",
              meta=(DisplayName="Preview PNG maximum resolution",
                    ClampMin="64", ClampMax="8192"))
    int32 TexturePreviewMaxResolution = 1024;

    /**
     * Warn when a texture's source file sits outside the synced tree, so no
     * relative path can reach it and it resolves only on the machine that ran
     * the sync. Leaving this on is strongly recommended: a LocalOnly texture
     * costs whoever opens the library next an afternoon, and it costs them
     * silently.
     */
    UPROPERTY(Config, EditAnywhere, Category="Textures",
              meta=(DisplayName="Warn about machine-local texture paths"))
    bool bWarnOnLocalOnlyTextures = true;

    /**
     * How many textures a conform run does before it stops for breath.
     *
     * Conforming is a synchronous loop: each texture is rewritten as a TGA,
     * reimported through Interchange, rebuilt (a 4K source is a ~350 MB build)
     * and saved, and the editor does not reach an idle frame until the run
     * ends. A 243-texture run is over five minutes of that, unbroken.
     *
     * At each boundary the run drains the render thread, collects garbage, and
     * pauses for the breather below, so the machine gets regular idle moments
     * instead of one continuous block. Smaller is gentler and slower.
     *
     * 0 disables pacing entirely — the pre-v1.20 behaviour.
     */
    UPROPERTY(Config, EditAnywhere, Category="Textures",
              meta=(DisplayName="Conform: textures per chunk",
                    ClampMin="0", ClampMax="256"))
    int32 TextureConformChunkSize = 16;

    /**
     * Seconds to idle at each chunk boundary.
     *
     * This is not a fix for anything — a GPU that resets under sustained load
     * is a driver or thermal problem and belongs to the machine, not to
     * MapLink. It is a concession to the fact that a long conform is the
     * heaviest sustained thing this plugin ever asks a workstation to do, and
     * that a run which pauses is a run that can be cancelled and resumed.
     *
     * 0 means no pause; the chunk boundary still flushes and collects.
     */
    UPROPERTY(Config, EditAnywhere, Category="Textures",
              meta=(DisplayName="Conform: pause at each chunk (seconds)",
                    ClampMin="0.0", ClampMax="10.0"))
    float TextureConformBreatherSeconds = 0.5f;

    static const UMapLinkSettings* Get();

    // ── v1.15 — per-user project paths ────────────────────────────────
    // BlenderProjectRoot, MaterialsLibraryBlend, MaterialInstancesBlend,
    // MaterialInstanceContentPath and MaterialDisplayScanPath live in
    // EditorPerProjectUserSettings.ini (section [MapLink.UserProjectPaths]):
    // per-user AND per-project, always writable, never source-controlled.
    // Loaded once on CDO init (with a one-shot migration of any legacy
    // values from the old Config layer in the Game.ini hierarchy), saved
    // by every writer (Set/Apply/Unlink/Project Settings edits).

    /** Persist the five user project paths from the CDO to the per-user
     *  ini.  Replaces every previous SaveConfig() call on this class for
     *  those fields. */
    static void SaveUserProjectPaths();

    /** v1.15.1 — programmatic toggle for the name overlay (used by the
     *  Asset Viewer checkbox): sets the value, persists it per-user and
     *  redraws the level viewports. */
    static void SetShowAssetNames(bool bShow);

    /** v1.15.4 — same contract for the child-labels toggle. */
    static void SetShowChildAssetNames(bool bShow);

    /** v1.15.4 — per-type filter setters (per-user, redraw). */
    static void SetShowNamesLI(bool bShow);
    static void SetShowNamesSM(bool bShow);
    static void SetShowNamesBP(bool bShow);

    /** v1.15.4 — lock toggle for the viewport dropdown: sets the value,
     *  sweeps the loaded editor world live, and best-effort persists the
     *  Config default (TryUpdateDefaultConfigFile — may no-op if
     *  DefaultGame.ini is read-only; the session value still applies). */
    static void SetLockMapLinkActorTransforms(bool bLock);

    virtual void PostInitProperties() override;
#if WITH_EDITOR
    virtual void PostEditChangeProperty(
        FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

    // ── v1.8 path helpers ─────────────────────────────────────────────

    /**
     * Resolve the BlenderProjectRoot path.  Resolution order:
     *   1. The configured BlenderProjectRoot setting.
     *   2. Auto-detect from any UMapLinkProxyAsset.BlendFilePath in the
     *      asset registry (strip /<prefix>/<stem>/<stem>.blend tail).
     *      If bPromptOnAutoDetect, dialog to confirm before adopting.
     *   3. If bPromptOnMissing, open a directory picker dialog.
     *
     * On successful resolution via path 2 or 3, the result is persisted
     * back into the settings.  Returns an empty string if no root could
     * be determined.
     */
    static FString ResolveBlenderProjectRoot(
        bool bPromptOnAutoDetect = true,
        bool bPromptOnMissing    = true);

    /** Auto-detect from asset registry only.  No dialogs.  Empty
     *  string if no detection possible. */
    static FString AutoDetectBlenderProjectRoot();

    /** Compose <BlenderProjectRoot>/BP/<BPName>/<BPName>.blend. */
    static FString GetBPStandInBlendPath(
        const FString& BlenderProjectRoot, const FString& BPName);

    /** Compose <BlenderProjectRoot>/BP/BP_Template.blend. */
    static FString GetBPTemplateBlendPath(
        const FString& BlenderProjectRoot);

    /** Persist a new BlenderProjectRoot value to the config file. */
    static void SetBlenderProjectRoot(const FString& NewPath);

    // ── Project scaffolding (New Project) ─────────────────────────────

    /**
     * Absolute path to the plugin's bundled Blender resources directory
     * (<plugin>/blender/resources), which ships the template .blend files
     * and project-local material libraries consumed by "New Project".
     * Returns empty string if the MapLink plugin cannot be located.
     */
    static FString GetPluginBlenderResourcesDir();

    /**
     * Locate the maplink_project.json manifest given either a project
     * root directory OR the path to a .blend inside the project tree
     * (walks parent directories up to a sane depth looking for the
     * manifest). Returns empty string if not found.
     */
    static FString FindProjectManifest(const FString& RootOrBlendPath);

    /**
     * Read a maplink_project.json from disk and apply its values to the
     * settings: BlenderProjectRoot (= manifest dir), MaterialsLibraryBlend,
     * MaterialInstancesBlend, MaterialInstanceContentPath. All relative
     * paths in the manifest are resolved against the manifest directory.
     * Persists via SaveUserProjectPaths (v1.15). Returns true on success.
     */
    static bool ApplyProjectManifest(const FString& ManifestPath);

    /**
     * Convenience: FindProjectManifest then ApplyProjectManifest.
     * Returns true if a manifest was found and applied.
     */
    static bool LoadProjectManifest(const FString& RootOrBlendPath);
};
