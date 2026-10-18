// Copyright MapModelers. All Rights Reserved.

#include "MapLinkSettings.h"
#include "MapLinkPalette.h"
#include "MapLinkProxyAsset.h"
#include "MapLinkUtils.h"
#include "MapLinkPipeline.h"  // LogMapLink
#include "MapLinkActorLock.h" // v1.15 — live sweep on lock toggle
#include "MapLinkBlenderBridge.h" // v1.21 — drop the cached blender.exe on edit
#include "Editor.h"           // v1.15 — viewport redraw on overlay toggle

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Misc/ConfigCacheIni.h"
#include "HAL/PlatformFileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#define LOCTEXT_NAMESPACE "MapLinkSettings"

// ---------------------------------------------------------------------------
// Defaults
// ---------------------------------------------------------------------------

UMapLinkSettings::UMapLinkSettings()
{
    AutoUpdatePollInterval = 3.0f;
    bShowTimesInLocalTime  = true;   // v1.13.1 — display import times in local time

    // v1.9 — Performance
    bParallelWarmPass         = true;
    MaxParallelBlenderExports = 4;
    BlenderTimeoutSeconds     = 600;   // v1.9.4 — headless Blender watchdog
    bCascadeAutoReimport      = true;
    bShelveBeforeReimport     = false; // v1.13 — opt-in shelve safety net

    // Material library .blend paths are intentionally EMPTY by default.
    // They are populated per-project by "New Project" (which writes them
    // into maplink_project.json and applies that manifest) or set manually
    // in Project Settings. No machine-specific paths are baked in.
    MaterialsLibraryBlend.FilePath  = TEXT("");
    MaterialInstancesBlend.FilePath = TEXT("");
    MaterialInstanceContentPath.Path = TEXT("/Game/MapLink/MaterialInstances/");

    MaterialDisplayScanPath.Path = TEXT("/Game/MapLink/MaterialInstances/");
    MaterialDisplayLevelName = TEXT("L_MaterialInstances");
    MaterialDisplayGridSpacing = 500.f;
    MaterialDisplayGridColumns = 10;

    // v1.14 — Outliner folders.  Defaults reproduce pre-v1.14 behaviour
    // exactly: collections mirror to folders, expanded level only.
    FolderMode  = EMapLinkFolderMode::CollectionFolders;
    FolderScope = EMapLinkFolderScope::ExpandedLevelOnly;

    // v1.15 — Viewport
    bLockMapLinkActorTransforms = true;   // protect placed assets by default
    bShowAssetNames             = false;  // opt-in overlay
    bShowMapLinkToolbar         = true;   // v1.16 — on by default
    bShowChildAssetNames        = false;  // v1.15.4 — opt-in child labels
    MaxNameLabels               = 200;
    MaxNameLabelDistance        = 15000.f;

    // v1.15.1 — per-type name display + state colors
    bShowNamesLI = true;
    bShowNamesSM = true;
    bShowNamesBP = true;
    // v1.20 — from MapLinkPalette, which ue/blender/theme_colors.py mirrors.
    // The literals that used to sit here were correct and still invisible:
    // these are Config properties, so any project whose ini predates the
    // palette kept overriding them. See ApplyPaletteVersionHeal below.
    NameColorLI        = MapLinkPalette::LI;
    NameColorSM        = MapLinkPalette::SM;
    NameColorBP        = MapLinkPalette::BP;
    NameColorOutOfDate = MapLinkPalette::Attention;

    // NameColorPaletteVersion is deliberately NOT stamped here.
    //
    // The constructor runs BEFORE the ini is read, and a key absent from the
    // ini is left at whatever the constructor set. Stamping the current
    // version here therefore told every pre-palette project that it was
    // already up to date — no project has the key, so every project claimed
    // version 1 — and the heal returned early without ever running. It stays
    // at the header's 0 until a heal actually happens and writes it.
    bPruneWPFolders = true;
}

void UMapLinkSettings::ApplyPaletteVersionHeal()
{
    if (NameColorPaletteVersion >= MapLinkPalette::Version)
    {
        return;
    }

    NameColorLI        = MapLinkPalette::LI;
    NameColorSM        = MapLinkPalette::SM;
    NameColorBP        = MapLinkPalette::BP;
    NameColorOutOfDate = MapLinkPalette::Attention;
    NameColorPaletteVersion = MapLinkPalette::Version;

    UE_LOG(LogMapLink, Log,
           TEXT("Palette: name colours reset to the MapLink colour code "
                "(%s / %s / %s / %s). The project's saved values predated it."),
           MapLinkPalette::HexLI, MapLinkPalette::HexSM,
           MapLinkPalette::HexBP, MapLinkPalette::HexAttention);

    // Best effort, and deliberately not required. This class is
    // config=Game + defaultconfig, so SaveConfig() targets DefaultGame.ini,
    // which is usually read-only under Perforce until somebody checks it out
    // (the same trap that ate the project paths in v1.15). If the write does
    // not land, the heal simply runs again next launch and the colours are
    // right either way.
    TryUpdateDefaultConfigFile();
}

const UMapLinkSettings* UMapLinkSettings::Get()
{
    return GetDefault<UMapLinkSettings>();
}

// ---------------------------------------------------------------------------
// v1.15 — per-user project paths
//
// Root cause of the "Locate project root… again" bug: this class is
// config=Game + defaultconfig, so SaveConfig() targets DefaultGame.ini —
// source-controlled and usually READ-ONLY under P4 when not checked out.
// The write failed silently and every programmatically-persisted path
// (SetBlenderProjectRoot, ApplyProjectManifest, Unlink) evaporated on
// editor restart.  The five project paths now live in
// EditorPerProjectUserSettings.ini instead: per-user, per-project, always
// writable, never source-controlled.  Shared project truth stays in
// maplink_project.json (the manifest) — the ini is only this machine's
// pointer at it.
// ---------------------------------------------------------------------------

namespace
{
    const TCHAR* UserPathsSection = TEXT("MapLink.UserProjectPaths");

    /** Legacy Config section this class used to serialize into. */
    const TCHAR* LegacySection = TEXT("/Script/MapLinkEditor.MapLinkSettings");

    /** Extract the quoted inner value of a legacy struct-formatted ini
     *  entry — e.g.  (Path="C:/foo")  or  (FilePath="C:/x.blend")  —
     *  returns empty if the entry is absent or unparsable. */
    FString ReadLegacyStructPath(const TCHAR* Key)
    {
        if (!GConfig) return FString();
        FString Raw;
        if (!GConfig->GetString(LegacySection, Key, Raw, GGameIni)
            || Raw.IsEmpty())
        {
            return FString();
        }
        int32 First = INDEX_NONE, Last = INDEX_NONE;
        if (Raw.FindChar(TEXT('"'), First) &&
            Raw.FindLastChar(TEXT('"'), Last) && Last > First)
        {
            return Raw.Mid(First + 1, Last - First - 1);
        }
        return FString();
    }
}

void UMapLinkSettings::SaveUserProjectPaths()
{
    if (!GConfig) return;
    const UMapLinkSettings* S = Get();
    if (!S) return;

    GConfig->SetString(UserPathsSection, TEXT("BlenderExecutable"),
                       *S->BlenderExecutable.FilePath,        GEditorPerProjectIni);
    GConfig->SetString(UserPathsSection, TEXT("BlenderProjectRoot"),
                       *S->BlenderProjectRoot.Path,           GEditorPerProjectIni);
    GConfig->SetString(UserPathsSection, TEXT("MaterialsLibraryBlend"),
                       *S->MaterialsLibraryBlend.FilePath,    GEditorPerProjectIni);
    GConfig->SetString(UserPathsSection, TEXT("MaterialInstancesBlend"),
                       *S->MaterialInstancesBlend.FilePath,   GEditorPerProjectIni);
    GConfig->SetString(UserPathsSection, TEXT("MaterialInstanceContentPath"),
                       *S->MaterialInstanceContentPath.Path,  GEditorPerProjectIni);
    GConfig->SetString(UserPathsSection, TEXT("MaterialDisplayScanPath"),
                       *S->MaterialDisplayScanPath.Path,      GEditorPerProjectIni);
    GConfig->Flush(false, GEditorPerProjectIni);
}

void UMapLinkSettings::SetShowAssetNames(bool bShow)
{
    UMapLinkSettings* Mutable = GetMutableDefault<UMapLinkSettings>();
    if (!Mutable || Mutable->bShowAssetNames == bShow)
    {
        return;
    }
    Mutable->bShowAssetNames = bShow;
    if (GConfig)
    {
        GConfig->SetBool(TEXT("MapLink.Viewport"), TEXT("bShowAssetNames"),
                         bShow, GEditorPerProjectIni);
        GConfig->Flush(false, GEditorPerProjectIni);
    }
    if (GEditor)
    {
        GEditor->RedrawLevelEditingViewports(/*bInvalidateHitProxies=*/false);
    }
}

namespace
{
    /** v1.15.4 — shared writer for the per-user viewport bool prefs. */
    void SetViewportBoolPref(const TCHAR* Key, bool& Member, bool bNewValue)
    {
        if (Member == bNewValue)
        {
            return;
        }
        Member = bNewValue;
        if (GConfig)
        {
            GConfig->SetBool(TEXT("MapLink.Viewport"), Key, bNewValue,
                             GEditorPerProjectIni);
            GConfig->Flush(false, GEditorPerProjectIni);
        }
        if (GEditor)
        {
            GEditor->RedrawLevelEditingViewports(
                /*bInvalidateHitProxies=*/false);
        }
    }
}

void UMapLinkSettings::SetShowChildAssetNames(bool bShow)
{
    if (UMapLinkSettings* Mutable = GetMutableDefault<UMapLinkSettings>())
    {
        SetViewportBoolPref(TEXT("bShowChildAssetNames"),
                            Mutable->bShowChildAssetNames, bShow);
    }
}

void UMapLinkSettings::SetShowNamesLI(bool bShow)
{
    if (UMapLinkSettings* Mutable = GetMutableDefault<UMapLinkSettings>())
    {
        SetViewportBoolPref(TEXT("bShowNamesLI"),
                            Mutable->bShowNamesLI, bShow);
    }
}

void UMapLinkSettings::SetShowNamesSM(bool bShow)
{
    if (UMapLinkSettings* Mutable = GetMutableDefault<UMapLinkSettings>())
    {
        SetViewportBoolPref(TEXT("bShowNamesSM"),
                            Mutable->bShowNamesSM, bShow);
    }
}

void UMapLinkSettings::SetShowNamesBP(bool bShow)
{
    if (UMapLinkSettings* Mutable = GetMutableDefault<UMapLinkSettings>())
    {
        SetViewportBoolPref(TEXT("bShowNamesBP"),
                            Mutable->bShowNamesBP, bShow);
    }
}

void UMapLinkSettings::SetLockMapLinkActorTransforms(bool bLock)
{
    UMapLinkSettings* Mutable = GetMutableDefault<UMapLinkSettings>();
    if (!Mutable || Mutable->bLockMapLinkActorTransforms == bLock)
    {
        return;
    }
    Mutable->bLockMapLinkActorTransforms = bLock;
    FMapLinkActorLock::ApplyToEditorWorld();

    // Best-effort Config persistence (project-wide setting).  May no-op
    // when DefaultGame.ini is read-only and not checked out — the value
    // still applies for this session, and the sweep already ran.
    Mutable->TryUpdateDefaultConfigFile();
}

void UMapLinkSettings::PostInitProperties()
{
    Super::PostInitProperties();

    // Only the CDO carries the settings; skip transient duplicates.
    if (!HasAnyFlags(RF_ClassDefaultObject) || !GConfig)
    {
        return;
    }

    // Before anything else reads a colour: an ini older than the palette wins
    // over the constructor, so the shipped colour code only ever reached a
    // project that had never saved its settings.
    ApplyPaletteVersionHeal();

    // 1) Per-user layer (authoritative since v1.15).
    bool bAnyUserValue = false;
    auto LoadKey = [&bAnyUserValue](const TCHAR* Key, FString& Out)
    {
        FString V;
        if (GConfig->GetString(UserPathsSection, Key, V, GEditorPerProjectIni))
        {
            Out = V;               // may legitimately be empty (Unlink)
            bAnyUserValue = true;
        }
    };
    LoadKey(TEXT("BlenderExecutable"),           BlenderExecutable.FilePath);
    LoadKey(TEXT("BlenderProjectRoot"),          BlenderProjectRoot.Path);
    LoadKey(TEXT("MaterialsLibraryBlend"),       MaterialsLibraryBlend.FilePath);
    LoadKey(TEXT("MaterialInstancesBlend"),      MaterialInstancesBlend.FilePath);
    LoadKey(TEXT("MaterialInstanceContentPath"), MaterialInstanceContentPath.Path);
    LoadKey(TEXT("MaterialDisplayScanPath"),     MaterialDisplayScanPath.Path);

    // v1.15 — per-user viewport preferences (not paths, same layer).
    GConfig->GetBool(TEXT("MapLink.Viewport"), TEXT("bShowAssetNames"),
                     bShowAssetNames, GEditorPerProjectIni);
    GConfig->GetBool(TEXT("MapLink.Viewport"), TEXT("bShowMapLinkToolbar"),
                     bShowMapLinkToolbar, GEditorPerProjectIni);
    GConfig->GetBool(TEXT("MapLink.Viewport"), TEXT("bShowChildAssetNames"),
                     bShowChildAssetNames, GEditorPerProjectIni);
    GConfig->GetBool(TEXT("MapLink.Viewport"), TEXT("bShowNamesLI"),
                     bShowNamesLI, GEditorPerProjectIni);
    GConfig->GetBool(TEXT("MapLink.Viewport"), TEXT("bShowNamesSM"),
                     bShowNamesSM, GEditorPerProjectIni);
    GConfig->GetBool(TEXT("MapLink.Viewport"), TEXT("bShowNamesBP"),
                     bShowNamesBP, GEditorPerProjectIni);

    if (bAnyUserValue)
    {
        return;
    }

    // 2) One-shot migration: no per-user values yet — adopt whatever the
    //    old Config layer (Game.ini hierarchy, incl. DefaultGame.ini)
    //    still holds, then persist it to the per-user layer so the legacy
    //    entries never need to be read (or written) again.
    const FString LegacyRoot      = ReadLegacyStructPath(TEXT("BlenderProjectRoot"));
    const FString LegacyMats      = ReadLegacyStructPath(TEXT("MaterialsLibraryBlend"));
    const FString LegacyInst      = ReadLegacyStructPath(TEXT("MaterialInstancesBlend"));
    const FString LegacyContent   = ReadLegacyStructPath(TEXT("MaterialInstanceContentPath"));
    const FString LegacyScan      = ReadLegacyStructPath(TEXT("MaterialDisplayScanPath"));

    if (!LegacyRoot.IsEmpty())      BlenderProjectRoot.Path          = LegacyRoot;
    if (!LegacyMats.IsEmpty())      MaterialsLibraryBlend.FilePath   = LegacyMats;
    if (!LegacyInst.IsEmpty())      MaterialInstancesBlend.FilePath  = LegacyInst;
    if (!LegacyContent.IsEmpty())   MaterialInstanceContentPath.Path = LegacyContent;
    if (!LegacyScan.IsEmpty())      MaterialDisplayScanPath.Path     = LegacyScan;

    if (!LegacyRoot.IsEmpty() || !LegacyMats.IsEmpty() ||
        !LegacyInst.IsEmpty() || !LegacyContent.IsEmpty() ||
        !LegacyScan.IsEmpty())
    {
        SaveUserProjectPaths();
        UE_LOG(LogMapLink, Log,
               TEXT("[Settings] Migrated legacy project paths from the Game "
                    "config layer to EditorPerProjectUserSettings.ini "
                    "(root='%s')."), *BlenderProjectRoot.Path);
    }
}

#if WITH_EDITOR
void UMapLinkSettings::PostEditChangeProperty(
    FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    const FName Name = PropertyChangedEvent.GetPropertyName();
    const FName Member =
        PropertyChangedEvent.MemberProperty
            ? PropertyChangedEvent.MemberProperty->GetFName() : Name;

    static const FName UserPathProps[] =
    {
        GET_MEMBER_NAME_CHECKED(UMapLinkSettings, BlenderExecutable),
        GET_MEMBER_NAME_CHECKED(UMapLinkSettings, BlenderProjectRoot),
        GET_MEMBER_NAME_CHECKED(UMapLinkSettings, MaterialsLibraryBlend),
        GET_MEMBER_NAME_CHECKED(UMapLinkSettings, MaterialInstancesBlend),
        GET_MEMBER_NAME_CHECKED(UMapLinkSettings, MaterialInstanceContentPath),
        GET_MEMBER_NAME_CHECKED(UMapLinkSettings, MaterialDisplayScanPath),
    };
    for (const FName& Prop : UserPathProps)
    {
        if (Member == Prop)
        {
            SaveUserProjectPaths();
            break;
        }
    }

    // v1.21 — the resolved blender.exe is cached for the editor session, so
    // a new path here has to drop it or the old one keeps being launched.
    if (Member == GET_MEMBER_NAME_CHECKED(UMapLinkSettings, BlenderExecutable))
    {
        FMapLinkBlenderBridge::InvalidateBlenderCache();
    }

    // v1.15 — Viewport reactions.
    if (Member == GET_MEMBER_NAME_CHECKED(UMapLinkSettings,
                                          bLockMapLinkActorTransforms))
    {
        // Sweep the loaded editor world live so the toggle acts instantly.
        FMapLinkActorLock::ApplyToEditorWorld();
    }
    else if (Member == GET_MEMBER_NAME_CHECKED(UMapLinkSettings,
                                               bShowAssetNames) ||
             Member == GET_MEMBER_NAME_CHECKED(UMapLinkSettings,
                                               bShowChildAssetNames) ||
             Member == GET_MEMBER_NAME_CHECKED(UMapLinkSettings, bShowNamesLI) ||
             Member == GET_MEMBER_NAME_CHECKED(UMapLinkSettings, bShowNamesSM) ||
             Member == GET_MEMBER_NAME_CHECKED(UMapLinkSettings, bShowNamesBP) ||
             Member == GET_MEMBER_NAME_CHECKED(UMapLinkSettings,
                                               bShowMapLinkToolbar))
    {
        // Per-user preferences — same writable layer as the project paths.
        if (GConfig)
        {
            GConfig->SetBool(TEXT("MapLink.Viewport"), TEXT("bShowAssetNames"),
                             bShowAssetNames, GEditorPerProjectIni);
            GConfig->SetBool(TEXT("MapLink.Viewport"),
                             TEXT("bShowMapLinkToolbar"),
                             bShowMapLinkToolbar, GEditorPerProjectIni);
            GConfig->SetBool(TEXT("MapLink.Viewport"),
                             TEXT("bShowChildAssetNames"),
                             bShowChildAssetNames, GEditorPerProjectIni);
            GConfig->SetBool(TEXT("MapLink.Viewport"), TEXT("bShowNamesLI"),
                             bShowNamesLI, GEditorPerProjectIni);
            GConfig->SetBool(TEXT("MapLink.Viewport"), TEXT("bShowNamesSM"),
                             bShowNamesSM, GEditorPerProjectIni);
            GConfig->SetBool(TEXT("MapLink.Viewport"), TEXT("bShowNamesBP"),
                             bShowNamesBP, GEditorPerProjectIni);
            GConfig->Flush(false, GEditorPerProjectIni);
        }
        if (GEditor)
        {
            GEditor->RedrawLevelEditingViewports(/*bInvalidateHitProxies=*/false);
        }
    }
}
#endif

EMapLinkFolderMode UMapLinkSettings::GetFolderMode()
{
    const UMapLinkSettings* S = Get();
    return S ? S->FolderMode : EMapLinkFolderMode::CollectionFolders;
}

EMapLinkFolderScope UMapLinkSettings::GetFolderScope()
{
    const UMapLinkSettings* S = Get();
    return S ? S->FolderScope : EMapLinkFolderScope::ExpandedLevelOnly;
}

// ---------------------------------------------------------------------------
// v1.8 — BlenderProjectRoot resolution
// ---------------------------------------------------------------------------

namespace
{
    /** Get a parent window pointer for modal dialogs. */
    void* GetActiveWindowHandle()
    {
        if (!FSlateApplication::IsInitialized())
        {
            return nullptr;
        }
        TSharedPtr<SWindow> ActiveWin =
            FSlateApplication::Get().GetActiveTopLevelWindow();
        if (!ActiveWin.IsValid())
        {
            return nullptr;
        }
        return ActiveWin->GetNativeWindow()->GetOSWindowHandle();
    }

    /** Normalize and validate a candidate root.  Returns the normalized
     *  path on success, empty on failure. */
    FString NormalizeRoot(const FString& In)
    {
        if (In.IsEmpty()) return FString();
        FString N = In;
        FPaths::NormalizeDirectoryName(N);
        if (!FPaths::DirectoryExists(N)) return FString();
        return N;
    }
}

FString UMapLinkSettings::AutoDetectBlenderProjectRoot()
{
    // Scan asset registry for any UMapLinkProxyAsset with a populated
    // BlendFilePath.  Strip the /<PREFIX>/<STEM>/<STEM>.blend tail.
    FAssetRegistryModule& ARM =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
            TEXT("AssetRegistry"));
    IAssetRegistry& AR = ARM.Get();

    TArray<FAssetData> ProxyAssets;
    AR.GetAssetsByClass(
        UMapLinkProxyAsset::StaticClass()->GetClassPathName(),
        ProxyAssets);

    UE_LOG(LogMapLink, Log,
           TEXT("[Settings] AutoDetectBlenderProjectRoot scanning %d proxy(ies)…"),
           ProxyAssets.Num());

    for (const FAssetData& AD : ProxyAssets)
    {
        UMapLinkProxyAsset* Proxy = Cast<UMapLinkProxyAsset>(AD.GetAsset());
        if (!Proxy || Proxy->BlendFilePath.IsEmpty()) continue;

        FString Path = Proxy->BlendFilePath;
        FPaths::NormalizeFilename(Path);

        // .../<root>/<PREFIX>/<STEM>/<STEM>.blend
        // Pop the file:    .../<root>/<PREFIX>/<STEM>
        // Pop the stem:    .../<root>/<PREFIX>
        // Pop the prefix:  .../<root>
        FString DirStem   = FPaths::GetPath(Path);
        FString DirPrefix = FPaths::GetPath(DirStem);
        FString Root      = FPaths::GetPath(DirPrefix);

        const FString PrefixLeaf = FPaths::GetCleanFilename(DirPrefix);
        if (PrefixLeaf != TEXT("LI") &&
            PrefixLeaf != TEXT("BP") &&
            PrefixLeaf != TEXT("SM"))
        {
            continue;
        }

        if (!FPaths::DirectoryExists(Root)) continue;

        UE_LOG(LogMapLink, Log,
               TEXT("[Settings] Auto-detected BlenderProjectRoot: '%s' "
                    "(from proxy '%s')"),
               *Root, *AD.AssetName.ToString());
        return Root;
    }

    UE_LOG(LogMapLink, Log,
           TEXT("[Settings] AutoDetectBlenderProjectRoot found no candidate."));
    return FString();
}

void UMapLinkSettings::SetBlenderProjectRoot(const FString& NewPath)
{
    UMapLinkSettings* Mutable = GetMutableDefault<UMapLinkSettings>();
    if (!Mutable) return;

    Mutable->BlenderProjectRoot.Path = NewPath;
    SaveUserProjectPaths();   // v1.15 — per-user ini, not DefaultGame.ini

    UE_LOG(LogMapLink, Log,
           TEXT("[Settings] BlenderProjectRoot set to: '%s'"),
           *NewPath);
}

FString UMapLinkSettings::ResolveBlenderProjectRoot(
    bool bPromptOnAutoDetect,
    bool bPromptOnMissing)
{
    // 1) Configured setting wins if valid.
    const UMapLinkSettings* S = Get();
    {
        FString Configured = NormalizeRoot(S->BlenderProjectRoot.Path);
        if (!Configured.IsEmpty())
        {
            return Configured;
        }
    }

    // 2) Auto-detect from existing proxies.
    {
        FString Auto = AutoDetectBlenderProjectRoot();
        if (!Auto.IsEmpty())
        {
            bool bAdopt = true;
            if (bPromptOnAutoDetect)
            {
                const FText Msg = FText::Format(
                    LOCTEXT("AutoDetectConfirm",
                            "MapLink detected this Blender project root from "
                            "your existing imports:\n\n{0}\n\nUse it as the "
                            "BlenderProjectRoot setting?"),
                    FText::FromString(Auto));
                bAdopt = (FMessageDialog::Open(EAppMsgType::YesNo, Msg)
                          == EAppReturnType::Yes);
            }
            if (bAdopt)
            {
                SetBlenderProjectRoot(Auto);
                return Auto;
            }
        }
    }

    // 3) Directory picker.
    if (bPromptOnMissing)
    {
        IDesktopPlatform* DP = FDesktopPlatformModule::Get();
        if (DP)
        {
            FString Picked;
            const bool bOk = DP->OpenDirectoryDialog(
                GetActiveWindowHandle(),
                TEXT("Select Blender Project Root (folder containing BP/ LI/ SM/)"),
                FPaths::ProjectDir(),
                Picked);
            if (bOk && !Picked.IsEmpty())
            {
                FString Norm = NormalizeRoot(Picked);
                if (!Norm.IsEmpty())
                {
                    SetBlenderProjectRoot(Norm);
                    return Norm;
                }
            }
        }
    }

    return FString();
}

// ---------------------------------------------------------------------------
// Path composition
// ---------------------------------------------------------------------------

FString UMapLinkSettings::GetBPStandInBlendPath(
    const FString& BlenderProjectRoot, const FString& BPName)
{
    if (BlenderProjectRoot.IsEmpty() || BPName.IsEmpty())
    {
        return FString();
    }
    FString Root = BlenderProjectRoot;
    FPaths::NormalizeDirectoryName(Root);
    return Root / TEXT("BP") / BPName / (BPName + TEXT(".blend"));
}

FString UMapLinkSettings::GetBPTemplateBlendPath(
    const FString& BlenderProjectRoot)
{
    if (BlenderProjectRoot.IsEmpty())
    {
        return FString();
    }
    FString Root = BlenderProjectRoot;
    FPaths::NormalizeDirectoryName(Root);
    // Canonical MapLink convention: <root>/<PREFIX>/<STEM>/<STEM>.blend
    // — same layout as per-BP stand-ins and what the rename tool
    // produces.  The template is treated as a regular stem under BP/.
    return Root / TEXT("BP") / TEXT("BP_Template") / TEXT("BP_Template.blend");
}

// ---------------------------------------------------------------------------
// Project scaffolding — bundled resources dir + manifest reader (New Project)
// ---------------------------------------------------------------------------

FString UMapLinkSettings::GetPluginBlenderResourcesDir()
{
    TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MapLink"));
    if (!Plugin.IsValid())
    {
        UE_LOG(LogMapLink, Error,
               TEXT("[Settings] MapLink plugin not found; cannot resolve the "
                    "bundled Blender resources directory."));
        return FString();
    }
    FString Dir = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir())
                  / TEXT("blender") / TEXT("resources");
    FPaths::NormalizeDirectoryName(Dir);
    return Dir;
}

FString UMapLinkSettings::FindProjectManifest(const FString& RootOrBlendPath)
{
    if (RootOrBlendPath.IsEmpty())
    {
        return FString();
    }

    FString Path = RootOrBlendPath;
    FPaths::NormalizeFilename(Path);

    // If handed a file (e.g. a .blend inside the tree), start from its dir.
    if (FPaths::FileExists(Path))
    {
        Path = FPaths::GetPath(Path);
    }
    FPaths::NormalizeDirectoryName(Path);

    // Walk up a bounded number of parents looking for the manifest.
    const int32 MaxDepth = 8;
    for (int32 i = 0; i < MaxDepth && !Path.IsEmpty(); ++i)
    {
        const FString Candidate = Path / TEXT("maplink_project.json");
        if (FPaths::FileExists(Candidate))
        {
            return Candidate;
        }
        const FString Parent = FPaths::GetPath(Path);
        if (Parent == Path)
        {
            break;  // reached filesystem root
        }
        Path = Parent;
    }
    return FString();
}

bool UMapLinkSettings::ApplyProjectManifest(const FString& ManifestPath)
{
    if (ManifestPath.IsEmpty() || !FPaths::FileExists(ManifestPath))
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("[Settings] ApplyProjectManifest: manifest not found: '%s'"),
               *ManifestPath);
        return false;
    }

    FString Json;
    if (!FFileHelper::LoadFileToString(Json, *ManifestPath))
    {
        UE_LOG(LogMapLink, Error,
               TEXT("[Settings] ApplyProjectManifest: failed to read '%s'"),
               *ManifestPath);
        return false;
    }

    TSharedPtr<FJsonObject> Root;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        UE_LOG(LogMapLink, Error,
               TEXT("[Settings] ApplyProjectManifest: invalid JSON in '%s'"),
               *ManifestPath);
        return false;
    }

    // The manifest directory is the project root; all rel paths resolve here.
    const FString ManifestDir = FPaths::GetPath(ManifestPath);

    UMapLinkSettings* Mutable = GetMutableDefault<UMapLinkSettings>();
    if (!Mutable)
    {
        return false;
    }

    {
        FString RootDir = ManifestDir;
        FPaths::NormalizeDirectoryName(RootDir);
        Mutable->BlenderProjectRoot.Path = RootDir;
    }

    // Material library block — paths are RELATIVE to the manifest dir.
    const TSharedPtr<FJsonObject>* MatObj = nullptr;
    if (Root->TryGetObjectField(TEXT("material_library"), MatObj)
        && MatObj && MatObj->IsValid())
    {
        FString Rel;

        if ((*MatObj)->TryGetStringField(TEXT("materials_blend_rel"), Rel)
            && !Rel.IsEmpty())
        {
            FString Abs = FPaths::ConvertRelativePathToFull(ManifestDir, Rel);
            FPaths::NormalizeFilename(Abs);
            Mutable->MaterialsLibraryBlend.FilePath = Abs;
        }

        if ((*MatObj)->TryGetStringField(TEXT("instances_blend_rel"), Rel)
            && !Rel.IsEmpty())
        {
            FString Abs = FPaths::ConvertRelativePathToFull(ManifestDir, Rel);
            FPaths::NormalizeFilename(Abs);
            Mutable->MaterialInstancesBlend.FilePath = Abs;
        }

        FString ContentPath;
        if ((*MatObj)->TryGetStringField(TEXT("instance_content_path"), ContentPath)
            && !ContentPath.IsEmpty())
        {
            // Single source of truth: MI sync target and MI display scan path are
            // the same place (where the project's Material Instances live).
            Mutable->MaterialInstanceContentPath.Path = ContentPath;
            Mutable->MaterialDisplayScanPath.Path = ContentPath;
        }
    }

    SaveUserProjectPaths();   // v1.15 — per-user ini, not DefaultGame.ini

    UE_LOG(LogMapLink, Log,
           TEXT("[Settings] Applied project manifest '%s' (root='%s', "
                "materials='%s', instances='%s')."),
           *ManifestPath,
           *Mutable->BlenderProjectRoot.Path,
           *Mutable->MaterialsLibraryBlend.FilePath,
           *Mutable->MaterialInstancesBlend.FilePath);
    return true;
}

bool UMapLinkSettings::LoadProjectManifest(const FString& RootOrBlendPath)
{
    const FString ManifestPath = FindProjectManifest(RootOrBlendPath);
    if (ManifestPath.IsEmpty())
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("[Settings] LoadProjectManifest: no manifest found from '%s'"),
               *RootOrBlendPath);
        return false;
    }
    return ApplyProjectManifest(ManifestPath);
}

#undef LOCTEXT_NAMESPACE
