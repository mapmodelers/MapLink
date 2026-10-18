// Copyright MapModelers. All Rights Reserved.
//
// MapLink Project lifecycle — New (modal dialog + headless scaffolding
// driver), Open (attach to an existing root via its manifest), Unlink
// (detach settings; optionally delete all data).

#include "MapLinkProjectSetup.h"

#include "MapLinkSettings.h"
#include "MapLinkProxyAsset.h"        // Broken-state detection (GetProjectState)
#include "FileHelpers.h"               // FEditorFileUtils::LoadMap
#include "MapLinkBlenderBridge.h"     // RunBlenderScriptGeneric / OpenBlenderGUI / EMapLinkResult
#include "MapLinkSourceControl.h"
#include "MapLinkUtils.h"             // GetPluginBaseDir
#include "MapLinkNotify.h"
#include "MapLinkPipeline.h"          // LogMapLink

#include "Interfaces/IPluginManager.h"
#include "PluginDescriptor.h"

#include "Framework/Application/SlateApplication.h"
#include "Dialog/SCustomDialog.h"     // v1.13 — labeled-button modals
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Styling/AppStyle.h"

#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"

#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/MessageDialog.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"          // v1.12 — Unlink delete-all
#include "EditorAssetLibrary.h"       // v1.12 — Unlink delete-all (/Game/MapLink)
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Modules/ModuleManager.h"

#include "Editor.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"

#define LOCTEXT_NAMESPACE "MapLinkProjectSetup"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace
{
    /** Mirror of maplink_project_setup.py::_sanitize_token — keep alnum,
     *  '_' and '-'; map whitespace to '_'; drop everything else.  Used so the
     *  UE-side preview / paths match exactly what the Python script produces. */
    FString SanitizeToken(const FString& In)
    {
        FString Out;
        Out.Reserve(In.Len());
        FString Trimmed = In;
        Trimmed.TrimStartAndEndInline();
        for (const TCHAR Ch : Trimmed)
        {
            if (FChar::IsAlnum(Ch) || Ch == TEXT('_') || Ch == TEXT('-'))
            {
                Out.AppendChar(Ch);
            }
            else if (FChar::IsWhitespace(Ch))
            {
                Out.AppendChar(TEXT('_'));
            }
            // else: dropped (slashes, dots, etc.)
        }
        return Out;
    }

    FString ResolvePluginVersionName()
    {
        TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MapLink"));
        if (Plugin.IsValid() && !Plugin->GetDescriptor().VersionName.IsEmpty())
        {
            return Plugin->GetDescriptor().VersionName;
        }
        return TEXT("1.9.4");
    }
}

// ---------------------------------------------------------------------------
// Modal dialog widget (private to this translation unit)
// ---------------------------------------------------------------------------

class SMapLinkNewProjectDialog : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SMapLinkNewProjectDialog) {}
        SLATE_ARGUMENT(TSharedPtr<SWindow>, ParentWindow)
        SLATE_ARGUMENT(FString, DefaultSetupName)
        SLATE_ARGUMENT(FString, DefaultRoot)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    bool    WasConfirmed()  const { return bConfirmed; }
    FString GetSetupName()  const { return SetupName; }   // sanitized on OK
    FString GetRootPath()   const { return RootPath; }    // normalized on OK
    FString GetStem()       const { return Stem; }        // LI_<SetupName>, set on OK

private:
    FReply  OnCreateClicked();
    FReply  OnCancelClicked();
    FReply  OnBrowseClicked();
    FText   GetPreviewText() const;
    FString ComputeStem() const;

    /** v1.14 — keep the auto-derived Root (<parent>/<SanitizedName>) in
     *  sync with the Project name field until the user edits Root by hand
     *  (typing or Browse). */
    void    SyncRootToName();

    /** Fields + Create are live unless this project already has a setup and the
     *  user hasn't yet ticked "Accept and create anyway". */
    bool CanEditFields() const { return !bProjectHasSetup || bForceCreateAnyway; }

    TWeakPtr<SWindow>            ParentWindow;
    TSharedPtr<SEditableTextBox> RootBox;

    FString SetupName;
    FString RootPath;
    FString Stem;
    bool    bConfirmed  = false;

    // v1.14 — Root auto-derivation state: the parent dir of the initial
    // default root (e.g. C:/MapLink), a manual-edit latch, and a reentrancy
    // guard so programmatic SetText doesn't count as a manual edit.
    FString RootParentDir;
    bool    bRootManuallyEdited = false;
    bool    bSyncingRootText    = false;

    // Re-setup gate: true when THIS UE project already has a MapLink setup
    // applied; the dialog then disables its fields + Create until the user
    // ticks "Accept and create anyway".
    bool    bProjectHasSetup    = false;
    bool    bForceCreateAnyway  = false;
};

void SMapLinkNewProjectDialog::Construct(const FArguments& InArgs)
{
    ParentWindow = InArgs._ParentWindow;
    SetupName    = InArgs._DefaultSetupName;
    RootPath     = InArgs._DefaultRoot;

    // v1.14 — remember the default root's parent (e.g. C:/MapLink) so the
    // Root field can track Project-name edits until manually overridden.
    RootParentDir = FPaths::GetPath(RootPath);

    // Does THIS UE project already have a MapLink setup applied? (settings
    // carry a BlenderProjectRoot whose manifest exists on disk).
    {
        const UMapLinkSettings* S = UMapLinkSettings::Get();
        const FString ExistingRoot = S ? S->BlenderProjectRoot.Path : FString();
        if (!ExistingRoot.IsEmpty())
        {
            const FString ExistingManifest =
                ExistingRoot / TEXT("maplink_project.json");
            bProjectHasSetup = FPaths::FileExists(ExistingManifest);
        }
    }

    const float LabelW = 120.f;

    ChildSlot
    [
        SNew(SBox)
        .WidthOverride(560.f)
        .Padding(12.f)
        [
            SNew(SVerticalBox)

            // ── Re-setup warning (only when this project already has a setup) ──
            + SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
            [
                SNew(SBorder)
                .Visibility(bProjectHasSetup ? EVisibility::Visible
                                             : EVisibility::Collapsed)
                .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
                .BorderBackgroundColor(FLinearColor(0.40f, 0.18f, 0.06f))
                .Padding(8.f)
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight()
                    [
                        SNew(STextBlock)
                        .AutoWrapText(true)
                        .ColorAndOpacity(FSlateColor(FLinearColor(1.f, 0.85f, 0.6f)))
                        .Text(LOCTEXT("ResetupWarn",
                            "This UE project already has a MapLink setup.\n\n"
                            "Creating another will REPOINT the project to the new "
                            "root: the active authoring root and material library "
                            "switch to it, the previous root's Blender libraries "
                            "and catalog are left orphaned, and UE content only "
                            "coexists if the setup name differs (a same-named "
                            "top-level is overwritten).\n\n"
                            "This does not delete the previous setup's files."))
                    ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
                    [
                        SNew(SCheckBox)
                        .IsChecked_Lambda([this]()
                        {
                            return bForceCreateAnyway ? ECheckBoxState::Checked
                                                      : ECheckBoxState::Unchecked;
                        })
                        .OnCheckStateChanged_Lambda([this](ECheckBoxState S)
                        {
                            bForceCreateAnyway = (S == ECheckBoxState::Checked);
                        })
                        [
                            SNew(STextBlock).Margin(FMargin(4.f, 0.f, 0.f, 0.f))
                            .Text(LOCTEXT("AcceptAnyway", "Accept and create anyway"))
                        ]
                    ]
                ]
            ]

            // Setup name
            + SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [ SNew(SBox).WidthOverride(LabelW)
                  [ SNew(STextBlock).Text(LOCTEXT("SetupLbl", "Project name")) ] ]
                + SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
                [
                    SNew(SEditableTextBox)
                    .Text(FText::FromString(SetupName))
                    .IsEnabled_Lambda([this](){ return CanEditFields(); })
                    .OnTextChanged_Lambda([this](const FText& T)
                    {
                        SetupName = T.ToString();
                        SyncRootToName();   // v1.14 — Root follows the name
                    })
                ]
            ]

            // Root folder + Browse
            + SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [ SNew(SBox).WidthOverride(LabelW)
                  [ SNew(STextBlock).Text(LOCTEXT("RootLbl", "Root folder")) ] ]
                + SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
                [
                    SAssignNew(RootBox, SEditableTextBox)
                    .Text(FText::FromString(RootPath))
                    .IsEnabled_Lambda([this](){ return CanEditFields(); })
                    .OnTextChanged_Lambda([this](const FText& T)
                    {
                        RootPath = T.ToString();
                        // v1.14 — a user edit latches the Root as manual;
                        // programmatic sync (SetText) must not.
                        if (!bSyncingRootText)
                        {
                            bRootManuallyEdited = true;
                        }
                    })
                ]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6.f, 0.f, 0.f, 0.f)
                [
                    SNew(SButton)
                    .Text(LOCTEXT("Browse", "Browse…"))
                    .IsEnabled_Lambda([this](){ return CanEditFields(); })
                    .OnClicked(this, &SMapLinkNewProjectDialog::OnBrowseClicked)
                ]
            ]

            // Preview
            + SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 2.f)
            [ SNew(STextBlock).Text(LOCTEXT("PreviewLbl", "Top-level .blend:")) ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .Text(this, &SMapLinkNewProjectDialog::GetPreviewText)
            ]

            // Buttons
            + SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f).HAlign(HAlign_Right)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 8.f, 0.f)
                [
                    SNew(SButton)
                    .Text(LOCTEXT("Create", "Create Project"))
                    .IsEnabled_Lambda([this](){ return CanEditFields(); })
                    .OnClicked(this, &SMapLinkNewProjectDialog::OnCreateClicked)
                ]
                + SHorizontalBox::Slot().AutoWidth()
                [
                    SNew(SButton)
                    .Text(LOCTEXT("Cancel", "Cancel"))
                    .OnClicked(this, &SMapLinkNewProjectDialog::OnCancelClicked)
                ]
            ]
        ]
    ];
}

void SMapLinkNewProjectDialog::SyncRootToName()
{
    // v1.14 — auto-derive Root = <parent>/<SanitizedName> (deterministic,
    // matches the C:\MapLink\<SetupName> scaffold convention) while the
    // user hasn't touched the Root field.  UI prefill only — nothing is
    // created on disk until Create is confirmed, and manifest paths stay
    // relative.
    if (bRootManuallyEdited || RootParentDir.IsEmpty())
    {
        return;
    }
    const FString Tok = SanitizeToken(SetupName);
    FString NewRoot = Tok.IsEmpty() ? RootParentDir : RootParentDir / Tok;
    FPaths::NormalizeDirectoryName(NewRoot);
    if (NewRoot == RootPath)
    {
        return;
    }
    RootPath = NewRoot;
    if (RootBox.IsValid())
    {
        bSyncingRootText = true;
        RootBox->SetText(FText::FromString(RootPath));
        bSyncingRootText = false;
    }
}

FString SMapLinkNewProjectDialog::ComputeStem() const
{
    const FString Tok = SanitizeToken(SetupName);
    if (Tok.IsEmpty())
    {
        return FString();
    }
    // One deterministic top-level per setup: LI_<SetupName>.
    return FString::Printf(TEXT("LI_%s"), *Tok);
}

FText SMapLinkNewProjectDialog::GetPreviewText() const
{
    const FString S = ComputeStem();
    if (S.IsEmpty())
    {
        return LOCTEXT("PreviewNeedLI", "(enter an LI name)");
    }
    FString Root = RootPath;
    Root.TrimStartAndEndInline();
    FPaths::NormalizeDirectoryName(Root);
    const FString Top = Root / TEXT("LI") / S / (S + TEXT(".blend"));
    return FText::FromString(Top);
}

FReply SMapLinkNewProjectDialog::OnBrowseClicked()
{
    IDesktopPlatform* DP = FDesktopPlatformModule::Get();
    if (DP)
    {
        const void* ParentHandle = nullptr;
        if (TSharedPtr<SWindow> Win = ParentWindow.Pin())
        {
            if (Win->GetNativeWindow().IsValid())
            {
                ParentHandle = Win->GetNativeWindow()->GetOSWindowHandle();
            }
        }

        FString DefaultPath = RootPath;
        DefaultPath.TrimStartAndEndInline();
        if (DefaultPath.IsEmpty() || !FPaths::DirectoryExists(DefaultPath))
        {
            DefaultPath = FPaths::ProjectDir();
        }

        FString Picked;
        if (DP->OpenDirectoryDialog(ParentHandle,
                TEXT("Select MapLink project root folder"),
                DefaultPath, Picked) && !Picked.IsEmpty())
        {
            FPaths::NormalizeDirectoryName(Picked);
            RootPath = Picked;
            bRootManuallyEdited = true;   // v1.14 — Browse latches manual
            if (RootBox.IsValid())
            {
                bSyncingRootText = true;
                RootBox->SetText(FText::FromString(RootPath));
                bSyncingRootText = false;
            }
        }
    }
    return FReply::Handled();
}

FReply SMapLinkNewProjectDialog::OnCreateClicked()
{
    // Re-setup gate (belt-and-suspenders; the Create button is also disabled
    // via CanEditFields until the box is ticked): refuse to proceed on a
    // project that already has a setup unless the user accepted the warning.
    if (bProjectHasSetup && !bForceCreateAnyway)
    {
        FMessageDialog::Open(EAppMsgType::Ok,
            LOCTEXT("ResetupBlocked",
                "This UE project already has a MapLink setup.\n\n"
                "Tick 'Accept and create anyway' to proceed."));
        return FReply::Handled();
    }

    const FString Name = SanitizeToken(SetupName);
    const FString NewStem = ComputeStem();
    FString Root = RootPath;
    Root.TrimStartAndEndInline();
    FPaths::NormalizeDirectoryName(Root);

    if (Name.IsEmpty() || Root.IsEmpty() || NewStem.IsEmpty())
    {
        FMessageDialog::Open(EAppMsgType::Ok,
            LOCTEXT("FieldsRequired",
                "Setup name and root folder are both required "
                "(the setup name must contain letters or digits)."));
        return FReply::Handled();
    }

    // Guard: never clobber an existing MapLink set at this root.
    const FString Manifest = Root / TEXT("maplink_project.json");
    if (FPaths::FileExists(Manifest))
    {
        FMessageDialog::Open(EAppMsgType::Ok,
            FText::Format(LOCTEXT("ManifestExists",
                "A MapLink setup already exists at:\n\n{0}\n\n"
                "Choose a different root folder."),
                FText::FromString(Manifest)));
        return FReply::Handled();
    }

    // Guard: bundled resources must be locatable.
    if (UMapLinkSettings::GetPluginBlenderResourcesDir().IsEmpty())
    {
        FMessageDialog::Open(EAppMsgType::Ok,
            LOCTEXT("NoResourcesDlg",
                "Could not locate the MapLink plugin's bundled Blender "
                "resources directory (<plugin>/blender/resources)."));
        return FReply::Handled();
    }

    // Commit canonical values for the caller.
    SetupName = Name;
    RootPath  = Root;
    Stem      = NewStem;
    bConfirmed = true;

    if (TSharedPtr<SWindow> Win = ParentWindow.Pin())
    {
        Win->RequestDestroyWindow();
    }
    return FReply::Handled();
}

FReply SMapLinkNewProjectDialog::OnCancelClicked()
{
    bConfirmed = false;
    if (TSharedPtr<SWindow> Win = ParentWindow.Pin())
    {
        Win->RequestDestroyWindow();
    }
    return FReply::Handled();
}

// ---------------------------------------------------------------------------
// Scaffold driver + post-steps
// ---------------------------------------------------------------------------

namespace
{
    /**
     * Best-effort build of the MaterialInstances library level: a non-WP level
     * at /Game/MapLink/MaterialInstances/L_MaterialInstances holding a grid of
     * `mat_cube` (/MapLink/Materials/mat_cube) actors — one per existing project
     * Material Instance, assigned as the cube's material. On a fresh setup no MIs
     * exist yet, so a single base cube is placed as a seed and the grid is
     * (re)populated later by the Material Display tool, which refreshes THIS SAME
     * level in place (one canonical name, no per-project duplicate). Non-fatal:
     * logs and returns on any failure. Creates a STANDARD (non-WP) level.
     */
    void BuildMaterialInstanceLibraryLevel(const FString& SetupName)
    {
        const FString PackageName =
            TEXT("/Game/MapLink/MaterialInstances/L_MaterialInstances");

        const FString DiskPath = FPackageName::LongPackageNameToFilename(
            PackageName, FPackageName::GetMapPackageExtension());
        if (FPaths::FileExists(DiskPath))
        {
            UE_LOG(LogMapLink, Log,
                   TEXT("[ProjectSetup] MI library level already exists, skipping: %s"),
                   *PackageName);
            return;
        }

        // CRITICAL: UPackage::SavePackage calls appError() (FATAL — crashes the
        // editor) if it cannot create its temporary file, which lives in the
        // project's Saved/ dir. When the project is under OneDrive / Controlled
        // Folder Access, that create can fail with WinError 2/5. Probe both the
        // Saved temp area and the target Content dir up front and SKIP the level
        // build (non-fatal) rather than letting SavePackage take the editor down.
        // The .blend scaffold (the important part) has already succeeded.
        {
            IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
            auto DirWritable = [&PF](const FString& Dir) -> bool
            {
                if (Dir.IsEmpty() || !PF.CreateDirectoryTree(*Dir))
                {
                    return false;
                }
                const FString Probe = Dir / TEXT(".maplink_write_test");
                if (!FFileHelper::SaveStringToFile(TEXT("ok"), *Probe,
                        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
                {
                    return false;
                }
                PF.DeleteFile(*Probe);
                return true;
            };

            const FString SavedDir   = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
            const FString ContentDir = FPaths::GetPath(DiskPath);
            if (!DirWritable(SavedDir) || !DirWritable(ContentDir))
            {
                UE_LOG(LogMapLink, Warning,
                       TEXT("[ProjectSetup] Skipping MaterialInstances library level: the "
                            "project save area is not writable (Saved='%s', Content='%s'). "
                            "This is typically a OneDrive/Controlled-Folder-Access restriction "
                            "on the UE project folder. The .blend scaffold is unaffected; build "
                            "the level later once the project folder is writable."),
                       *SavedDir, *ContentDir);
                return;
            }
        }

        UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(
            nullptr, TEXT("/MapLink/Materials/mat_cube.mat_cube"));
        if (!CubeMesh)
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("[ProjectSetup] mat_cube not found at /MapLink/Materials/mat_cube; "
                        "skipping MI library level."));
            return;
        }

        // Collect existing project Material Instances under the content path
        // (sorted by name so the grid is stable across rebuilds).
        TArray<UMaterialInterface*> Instances;
        {
            FString ContentPath = UMapLinkSettings::Get()->MaterialInstanceContentPath.Path;
            if (ContentPath.IsEmpty())
            {
                ContentPath = TEXT("/Game/MapLink/MaterialInstances/");
            }
            while (ContentPath.Len() > 1 && ContentPath.EndsWith(TEXT("/")))
            {
                ContentPath.LeftChopInline(1);
            }

            FAssetRegistryModule& ARM =
                FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
            FARFilter Filter;
            Filter.bRecursivePaths = true;
            Filter.bRecursiveClasses = true;
            Filter.PackagePaths.Add(FName(*ContentPath));
            Filter.ClassPaths.Add(UMaterialInstanceConstant::StaticClass()->GetClassPathName());

            TArray<FAssetData> Found;
            ARM.Get().GetAssets(Filter, Found);
            Found.Sort([](const FAssetData& A, const FAssetData& B)
            {
                return A.AssetName.LexicalLess(B.AssetName);
            });
            for (const FAssetData& AD : Found)
            {
                if (UMaterialInterface* MI = Cast<UMaterialInterface>(AD.GetAsset()))
                {
                    Instances.Add(MI);
                }
            }
        }

        IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
        PF.CreateDirectoryTree(*FPaths::GetPath(DiskPath));

        UPackage* Pkg = CreatePackage(*PackageName);
        if (!Pkg)
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("[ProjectSetup] CreatePackage failed for MI library level '%s'."),
                   *PackageName);
            return;
        }
        Pkg->MarkAsFullyLoaded();
        Pkg->MarkPackageDirty();

        const FString LevelName = FPackageName::GetShortName(PackageName);
        UWorld* World = UWorld::CreateWorld(
            EWorldType::Editor, /*bInformEngineOfWorld=*/false,
            FName(*LevelName), Pkg);
        if (!World)
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("[ProjectSetup] CreateWorld failed for MI library level '%s'."),
                   *PackageName);
            return;
        }
        World->SetFlags(RF_Public | RF_Standalone);

        auto SpawnCube = [World, CubeMesh](const FVector& Loc,
                                           UMaterialInterface* MI,
                                           const FString& Label)
        {
            FActorSpawnParameters SpawnParams;
            SpawnParams.SpawnCollisionHandlingOverride =
                ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            SpawnParams.OverrideLevel = World->PersistentLevel;
            AStaticMeshActor* A = World->SpawnActor<AStaticMeshActor>(
                AStaticMeshActor::StaticClass(), FTransform(Loc), SpawnParams);
            if (A)
            {
                A->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
                if (MI)
                {
                    A->GetStaticMeshComponent()->SetMaterial(0, MI);
                }
                A->SetActorLabel(Label);
            }
        };

        const float Spacing = 200.f;
        const int32 Cols = 8;
        if (Instances.Num() == 0)
        {
            // Fresh setup: seed one reference cube; user fills the grid later.
            SpawnCube(FVector::ZeroVector, nullptr, TEXT("mat_cube_base"));
        }
        else
        {
            for (int32 i = 0; i < Instances.Num(); ++i)
            {
                const FVector Loc((i % Cols) * Spacing, -(i / Cols) * Spacing, 0.f);
                SpawnCube(Loc, Instances[i], Instances[i]->GetName());
            }
        }

        FMapLinkSourceControl::CheckoutBeforeSave(Pkg);
        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        // v1.13.1 — never route save errors to GError (fatal appError).
        SaveArgs.Error     = GLog;
        SaveArgs.SaveFlags = SAVE_NoError;
        const bool bSaved = UPackage::SavePackage(Pkg, World, *DiskPath, SaveArgs);

        if (bSaved)
        {
            FMapLinkSourceControl::AddAfterSave(Pkg);
            FAssetRegistryModule::AssetCreated(World);
            UE_LOG(LogMapLink, Log,
                   TEXT("[ProjectSetup] MI library level '%s' built (%d cube(s)) -> %s"),
                   *PackageName, FMath::Max(1, Instances.Num()), *DiskPath);
        }
        else
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("[ProjectSetup] SavePackage failed for MI library level '%s' -> %s"),
                   *PackageName, *DiskPath);
        }

        World->DestroyWorld(false);
        CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
    }

    /** Add every .blend / .json / .txt under RootPath to the project-setup
     *  changelist.  Graceful no-op when source control is disabled. */
    void AddTreeToSourceControl(const FString& RootPath)
    {
        IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
        TArray<FString> Files;
        PF.FindFilesRecursively(Files, *RootPath, TEXT(""));

        const FString Desc = TEXT("[MapLink] Project Setup");
        int32 Processed = 0;
        for (const FString& File : Files)
        {
            const FString Ext = FPaths::GetExtension(File).ToLower();
            if (Ext == TEXT("blend") || Ext == TEXT("json") || Ext == TEXT("txt"))
            {
                if (FMapLinkSourceControl::EnsureFileInChangelist(File, Desc))
                {
                    ++Processed;
                }
            }
        }
        UE_LOG(LogMapLink, Log,
               TEXT("[ProjectSetup] Source control processed %d project file(s)."),
               Processed);
    }

    /** Verify the bundled resources dir exists and physically contains every
     *  template the scaffolder consumes. On failure, fills OutError with a
     *  message naming the resolved path and the missing file(s) so deployment
     *  gaps surface clearly instead of as a cryptic Blender "cannot read file"
     *  error. */
    bool ValidateResourcesDir(const FString& ResourcesDir, FString& OutError)
    {
        static const TCHAR* const Required[] = {
            TEXT("LI_Template.blend"),
            TEXT("SM_Template.blend"),
            TEXT("BP_Template.blend"),
            TEXT("Library_Materials.blend"),
            TEXT("Library_MaterialInstances.blend"),
        };

        if (ResourcesDir.IsEmpty())
        {
            OutError = TEXT("The MapLink plugin could not be located, so its "
                            "bundled Blender resources can't be resolved.");
            return false;
        }
        if (!FPaths::DirectoryExists(ResourcesDir))
        {
            OutError = FString::Printf(
                TEXT("The plugin's bundled resources folder is missing:\n\n%s\n\n"
                     "Copy the plugin's 'blender' folder (with its 'resources' "
                     "templates) into the installed plugin and rebuild/restart."),
                *ResourcesDir);
            return false;
        }

        TArray<FString> Missing;
        for (const TCHAR* const Name : Required)
        {
            if (!FPaths::FileExists(ResourcesDir / Name))
            {
                Missing.Add(Name);
            }
        }
        if (Missing.Num() > 0)
        {
            OutError = FString::Printf(
                TEXT("Required template file(s) are missing from:\n\n%s\n\n  %s\n\n"
                     "Copy the plugin's 'blender/resources' contents into the "
                     "installed plugin."),
                *ResourcesDir, *FString::Join(Missing, TEXT("\n  ")));
            return false;
        }
        return true;
    }

    void RunScaffold(const FString& SetupName, const FString& RootPath,
                     const FString& Stem)
    {
        const FString ResourcesDir = UMapLinkSettings::GetPluginBlenderResourcesDir();

        // Pre-flight: fail loudly and clearly if the templates aren't deployed,
        // rather than letting Blender choke on a missing --background file.
        FString ResErr;
        if (!ValidateResourcesDir(ResourcesDir, ResErr))
        {
            FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(ResErr));
            UE_LOG(LogMapLink, Error, TEXT("[ProjectSetup] %s"),
                   *ResErr.Replace(TEXT("\n"), TEXT(" ")));
            return;
        }

        const FString PluginBaseDir = FMapLinkUtils::GetPluginBaseDir();
        const FString VersionName   = ResolvePluginVersionName();

        // Pre-flight: the script must also be on disk (the bridge returns
        // BlenderScriptFailed with no dialog if it isn't).
        // v1.19 - the scaffolder lives in the addon package now, so the
        // Blender addon installer delivers it and a Blender-only artist can
        // run "New MapLink Project..." with no UE at all. It still imports no
        // addon module and still runs under --factory-startup, so living
        // beside them costs nothing.
        const FString ScriptRelPath = TEXT("blender/maplink_project_setup.py");
        const FString ScriptPath    = PluginBaseDir / ScriptRelPath;
        if (!FPaths::FileExists(ScriptPath))
        {
            FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
                LOCTEXT("ScriptMissing",
                    "MapLink can't start because the setup script is missing:\n\n{0}\n\n"
                    "Ensure '<Plugin>/blender/maplink_project_setup.py' is deployed."),
                FText::FromString(ScriptPath)));
            UE_LOG(LogMapLink, Error,
                   TEXT("[ProjectSetup] Aborting: script missing at %s"), *ScriptPath);
            return;
        }

        // Pre-flight: the chosen root must be creatable AND writable. On many
        // machines Documents is OneDrive-synced (Known-Folder-Move) or under
        // Controlled Folder Access, which blocks the external blender.exe from
        // creating folders there and surfaces as a confusing Blender error.
        // Test it natively up front and abort with a clear message before we
        // ever spawn Blender.
        {
            IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
            bool bWritable = PF.CreateDirectoryTree(*RootPath);
            if (bWritable)
            {
                const FString Probe = RootPath / TEXT(".maplink_write_test");
                bWritable = FFileHelper::SaveStringToFile(TEXT("ok"), *Probe,
                    FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
                if (bWritable)
                {
                    PF.DeleteFile(*Probe);
                }
            }
            if (!bWritable)
            {
                FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
                    LOCTEXT("RootNotWritable",
                        "MapLink can't create or write the chosen root folder:\n\n{0}\n\n"
                        "If this path is under OneDrive/Documents or Controlled Folder "
                        "Access, choose a local, non-synced folder (e.g. C:\\MapLink\\<name>)."),
                    FText::FromString(RootPath)));
                UE_LOG(LogMapLink, Error,
                       TEXT("[ProjectSetup] Root not creatable/writable: %s"), *RootPath);
                return;
            }
        }

        // The bridge requires a positional .blend for --background; the setup
        // script ignores it (it drives its own open/save), so we pass the
        // bundled LI_Template — verified present above — as a throwaway.
        const FString ThrowawayBlend = ResourcesDir / TEXT("LI_Template.blend");

        FString Args;
        Args += FString::Printf(TEXT("--root \"%s\""), *RootPath);
        Args += FString::Printf(TEXT(" --setup-name \"%s\""), *SetupName);
        Args += FString::Printf(TEXT(" --li-stem \"%s\""), *Stem);
        Args += FString::Printf(TEXT(" --resources-dir \"%s\""), *ResourcesDir);
        Args += TEXT(" --unit-scale 1.0");
        Args += FString::Printf(TEXT(" --maplink-version \"%s\""), *VersionName);
        Args += TEXT(" --mi-collection \"Library_Material_Instances\"");
        Args += TEXT(" --instance-content-path \"/Game/MapLink/MaterialInstances/\"");

        // SCC: everything created in this op stacks into one named changelist.
        FMapLinkSourceControl::SetSessionDescription(TEXT("[MapLink] Project Setup"));

        // --- Phase A (Blender): scaffold tree + libs + templates + manifest ---
        const EMapLinkResult Result = FMapLinkBlenderBridge::RunBlenderScriptGeneric(
            ThrowawayBlend,
            ScriptRelPath,
            PluginBaseDir,
            Args,
            TEXT("New MapLink Project"));

        if (Result != EMapLinkResult::Success)
        {
            // The bridge already surfaced a Blender-stdout error dialog.
            UE_LOG(LogMapLink, Error,
                   TEXT("[ProjectSetup] Scaffolding failed (bridge result=%d)."),
                   (int32)Result);
            return;
        }

        // --- Phase B (UE side, best-effort, each step non-fatal) ---
        const FString ManifestPath = RootPath / TEXT("maplink_project.json");
        const bool bApplied = UMapLinkSettings::LoadProjectManifest(ManifestPath);

        BuildMaterialInstanceLibraryLevel(SetupName);

        AddTreeToSourceControl(RootPath);

        const FString TopBlend =
            RootPath / TEXT("LI") / Stem / (Stem + TEXT(".blend"));

        // Import the freshly-scaffolded top-level so it lands in UE content
        // (proxy + assets; RunImport is placement-free, so nothing is dropped
        // into the active level) and flag it for the Reimport menu — one click
        // to update once the user authors geometry into it. Non-fatal.
        if (FPaths::FileExists(TopBlend))
        {
            const EMapLinkResult ImpRes = FMapLinkPipeline::RunImport(TopBlend);
            if (ImpRes == EMapLinkResult::Success)
            {
                FMapLinkPipeline::SetProxyShowInReimport(TopBlend, true);
            }
            else
            {
                UE_LOG(LogMapLink, Warning,
                    TEXT("[ProjectSetup] auto-import of %s returned code %d"),
                    *TopBlend, static_cast<int32>(ImpRes));
            }
        }

        const FText DoneMsg = FText::Format(
            LOCTEXT("ProjectSetupDone",
                "MapLink project '{0}' created at:\n\n{1}\n\n{2}"
                "Open the top-level .blend in Blender now?"),
            FText::FromString(SetupName),
            FText::FromString(RootPath),
            bApplied
                ? FText::GetEmpty()
                : LOCTEXT("ManifestWarn",
                    "(Warning: the manifest could not be applied to settings "
                    "automatically — set the material library paths in Project "
                    "Settings if needed.)\n\n"));

        if (FMessageDialog::Open(EAppMsgType::YesNo, DoneMsg) == EAppReturnType::Yes)
        {
            FMapLinkBlenderBridge::OpenBlenderGUI(TopBlend);
        }
    }
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

bool FMapLinkProjectSetup::IsSetupActive(FString* OutRoot)
{
    const UMapLinkSettings* Settings = UMapLinkSettings::Get();
    if (!Settings)
    {
        return false;
    }

    const FString Root = Settings->BlenderProjectRoot.Path;
    if (Root.IsEmpty())
    {
        return false;
    }

    // The manifest must still exist on disk. If the authoring root was moved or
    // deleted, treat the setup as inactive so the user can re-establish it.
    const FString Manifest = Root / TEXT("maplink_project.json");
    if (!FPaths::FileExists(Manifest))
    {
        return false;
    }

    if (OutRoot)
    {
        *OutRoot = Root;
    }
    return true;
}

// ---------------------------------------------------------------------------
// v1.12 — lifecycle state / Open / Unlink
// ---------------------------------------------------------------------------

EMapLinkProjectState FMapLinkProjectSetup::GetProjectState(FString* OutRoot)
{
    if (IsSetupActive(OutRoot))
    {
        return EMapLinkProjectState::Active;
    }

    // No valid root/manifest.  If MapLink proxies exist in content, the
    // project was authored here (or synced) and the root link is broken —
    // recovery is Open (locate root), not New.
    //
    // UI predicates poll this per frame, so throttle the registry query:
    // the NoProject/Broken distinction changes rarely (import/delete/open).
    static double LastQuerySec = -1000.0;
    static bool   bLastHadProxies = false;
    const double NowSec = FPlatformTime::Seconds();
    if (NowSec - LastQuerySec > 1.0)
    {
        LastQuerySec = NowSec;

        FAssetRegistryModule& ARModule =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");

        FARFilter Filter;
        Filter.ClassPaths.Add(
            UMapLinkProxyAsset::StaticClass()->GetClassPathName());
        Filter.bRecursivePaths = true;

        TArray<FAssetData> ProxyAssets;
        ARModule.Get().GetAssets(Filter, ProxyAssets);
        bLastHadProxies = ProxyAssets.Num() > 0;
    }

    return bLastHadProxies ? EMapLinkProjectState::Broken
                           : EMapLinkProjectState::NoProject;
}

void FMapLinkProjectSetup::ShowOpenProjectDialog()
{
    FString ActiveRoot;
    if (IsSetupActive(&ActiveRoot))
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Printf(
            TEXT("This UE project is already linked to a MapLink project at:\n%s\n\n"
                 "Use MapLink → Unlink Project first to attach a different one."),
            *ActiveRoot)));
        return;
    }

    IDesktopPlatform* DP = FDesktopPlatformModule::Get();
    if (!DP)
    {
        return;
    }

    // Sensible starting point: last configured root's parent, else C:\MapLink
    // (the default scaffold location), else the UE project dir.
    FString DefaultPath;
    if (const UMapLinkSettings* S = UMapLinkSettings::Get())
    {
        if (!S->BlenderProjectRoot.Path.IsEmpty())
        {
            DefaultPath = FPaths::GetPath(S->BlenderProjectRoot.Path);
        }
    }
#if PLATFORM_WINDOWS
    if (DefaultPath.IsEmpty() || !FPaths::DirectoryExists(DefaultPath))
    {
        DefaultPath = TEXT("C:/MapLink");
    }
#endif
    if (DefaultPath.IsEmpty() || !FPaths::DirectoryExists(DefaultPath))
    {
        DefaultPath = FPaths::ProjectDir();
    }

    FString Picked;
    if (!DP->OpenDirectoryDialog(
            FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr),
            TEXT("Open MapLink Project — select the project root folder"),
            DefaultPath, Picked) || Picked.IsEmpty())
    {
        return;
    }
    FPaths::NormalizeDirectoryName(Picked);

    // Locate the manifest (walks up from the picked folder, so picking a
    // subfolder inside the tree also works).
    const FString ManifestPath = UMapLinkSettings::FindProjectManifest(Picked);
    if (ManifestPath.IsEmpty())
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
            LOCTEXT("OpenNoManifest",
                "No MapLink project found.\n\n"
                "maplink_project.json was not found in (or above):\n{0}\n\n"
                "Select the MapLink project root folder — the one created by "
                "New MapLink Project (it contains the LI/ SM/ BP/ folders and "
                "the manifest)."),
            FText::FromString(Picked)));
        return;
    }

    // Read name + version for the confirmation (best-effort; Apply re-reads).
    FString ProjectName = FPaths::GetBaseFilename(FPaths::GetPath(ManifestPath));
    FString ManifestVersion;
    {
        FString Json;
        TSharedPtr<FJsonObject> RootObj;
        if (FFileHelper::LoadFileToString(Json, *ManifestPath))
        {
            TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
            if (FJsonSerializer::Deserialize(Reader, RootObj) && RootObj.IsValid())
            {
                RootObj->TryGetStringField(TEXT("project_name"), ProjectName);
                RootObj->TryGetStringField(TEXT("maplink_version"), ManifestVersion);
            }
        }
    }

    if (!UMapLinkSettings::ApplyProjectManifest(ManifestPath))
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
            LOCTEXT("OpenApplyFailed",
                "Found the manifest but could not apply it:\n{0}\n\n"
                "The file may be unreadable or contain invalid JSON. See the "
                "Output Log (LogMapLink) for details."),
            FText::FromString(ManifestPath)));
        return;
    }

    // Non-fatal version note: manifest written by a different plugin version.
    FText VersionNote = FText::GetEmpty();
    if (!ManifestVersion.IsEmpty())
    {
        const FString PluginVersion = ResolvePluginVersionName();
        if (ManifestVersion != PluginVersion)
        {
            VersionNote = FText::Format(
                LOCTEXT("OpenVersionNote",
                    "\n\nNote: the project was created with MapLink {0} "
                    "(this plugin is {1})."),
                FText::FromString(ManifestVersion),
                FText::FromString(PluginVersion));
        }
    }

    MapLinkNotify::Success(FText::Format(
        LOCTEXT("OpenDone",
            "MapLink project '{0}' opened.\nRoot: {1}{2}"),
        FText::FromString(ProjectName),
        FText::FromString(FPaths::GetPath(ManifestPath)),
        VersionNote));

    UE_LOG(LogMapLink, Log,
           TEXT("[ProjectSetup] Opened MapLink project '%s' (manifest: %s)"),
           *ProjectName, *ManifestPath);
}

void FMapLinkProjectSetup::UnlinkProject()
{
    FString ActiveRoot;
    const bool bActive = IsSetupActive(&ActiveRoot);
    if (!bActive)
    {
        // Also allow unlinking a half-configured state (root set, manifest
        // gone) — that is exactly when a user wants a clean detach.
        const UMapLinkSettings* S = UMapLinkSettings::Get();
        ActiveRoot = S ? S->BlenderProjectRoot.Path : FString();
        if (ActiveRoot.IsEmpty())
        {
            MapLinkNotify::Warn(
                LOCTEXT("UnlinkNothing",
                    "This UE project is not linked to a MapLink project."));
            return;
        }
    }

    // v1.13 — explicit labeled buttons; the destructive path is its own
    // clearly-labeled button (never mapped onto "No" again).
    TSharedRef<SCustomDialog> UnlinkDialog =
        SNew(SCustomDialog)
        .Title(LOCTEXT("UnlinkDlgTitle", "Unlink MapLink Project"))
        .Content()
        [
            SNew(STextBlock)
            .WrapTextAt(540.0f)
            .Text(FText::Format(
                LOCTEXT("UnlinkConfirm",
                    "Unlink this UE project from the MapLink project at:\n\n"
                    "{0}\n\n"
                    "Unlink Only — all Blender files and /Game/MapLink content "
                    "stay; re-attach anytime with Open MapLink Project.\n\n"
                    "Delete Everything… — removes the Blender root above AND "
                    "all /Game/MapLink content (a second confirmation "
                    "follows)."),
                FText::FromString(ActiveRoot)))
        ]
        .Buttons({
            SCustomDialog::FButton(LOCTEXT("UnlinkOnlyBtn", "Unlink Only")),
            SCustomDialog::FButton(LOCTEXT("UnlinkDeleteBtn",
                                           "Delete Everything…")),
            SCustomDialog::FButton(LOCTEXT("UnlinkCancelBtn", "Cancel"))
        });
    const int32 Choice = UnlinkDialog->ShowModal();
    if (Choice != 0 && Choice != 1)
    {
        return;   // Cancel button or window closed (-1)
    }
    const bool bDeleteAll = (Choice == 1);

    if (bDeleteAll)
    {
        // Refuse while a /Game/MapLink level is open — deleting the packages
        // under the active world invites a crash.
        if (GEditor)
        {
            UWorld* World = GEditor->GetEditorWorldContext().World();
            const FString WorldPkg =
                (World && World->GetPackage())
                    ? World->GetPackage()->GetName() : FString();
            if (WorldPkg.StartsWith(TEXT("/Game/MapLink")))
            {
                FMessageDialog::Open(EAppMsgType::Ok,
                    LOCTEXT("UnlinkLevelOpen",
                        "The currently open level lives under /Game/MapLink. "
                        "Open another level first, then run Unlink again."));
                return;
            }
        }

        // Second, explicit confirmation — destructive, and gated behind an
        // "I understand" checkbox: the delete button stays disabled until
        // the user ticks it (v1.13).
        bool bDeleteConfirmed = false;
        {
            TSharedPtr<SCheckBox> UnderstandCheck;

            TSharedRef<SWindow> ConfirmWindow = SNew(SWindow)
                .Title(LOCTEXT("UnlinkDeleteDlgTitle",
                               "Delete ALL MapLink Data"))
                .SizingRule(ESizingRule::Autosized)
                .SupportsMaximize(false)
                .SupportsMinimize(false);

            ConfirmWindow->SetContent(
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(16.f, 16.f, 16.f, 8.f)
                [
                    SNew(STextBlock)
                    .WrapTextAt(520.0f)
                    .Text(FText::Format(
                        LOCTEXT("UnlinkDeleteConfirm",
                            "PERMANENTLY DELETE all MapLink data?\n\n"
                            "  •  {0}  (entire folder, all .blends)\n"
                            "  •  /Game/MapLink  (all generated UE content)\n\n"
                            "If these files are under source control, "
                            "reconcile/revert the deletion there afterwards."),
                        FText::FromString(ActiveRoot)))
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(16.f, 8.f)
                [
                    SAssignNew(UnderstandCheck, SCheckBox)
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("UnlinkUnderstandChk",
                                      "I understand this cannot be undone"))
                    ]
                ]
                + SVerticalBox::Slot().AutoHeight()
                  .Padding(16.f, 8.f, 16.f, 16.f).HAlign(HAlign_Right)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(4.f, 0.f)
                    [
                        SNew(SButton)
                        .Text(LOCTEXT("UnlinkDeleteFinalBtn",
                                      "Delete Everything"))
                        .IsEnabled_Lambda([&UnderstandCheck]()
                        {
                            return UnderstandCheck.IsValid()
                                && UnderstandCheck->IsChecked();
                        })
                        .OnClicked_Lambda([&bDeleteConfirmed, &ConfirmWindow]()
                        {
                            bDeleteConfirmed = true;
                            ConfirmWindow->RequestDestroyWindow();
                            return FReply::Handled();
                        })
                    ]
                    + SHorizontalBox::Slot().AutoWidth().Padding(4.f, 0.f)
                    [
                        SNew(SButton)
                        .Text(LOCTEXT("UnlinkDeleteCancelBtn", "Cancel"))
                        .OnClicked_Lambda([&ConfirmWindow]()
                        {
                            ConfirmWindow->RequestDestroyWindow();
                            return FReply::Handled();
                        })
                    ]
                ]);

            // Blocks until the window closes; lambdas capture locals by
            // reference, which is safe for a modal.
            FSlateApplication::Get().AddModalWindow(
                ConfirmWindow, TSharedPtr<const SWidget>());
        }
        if (!bDeleteConfirmed)
        {
            return;
        }
    }

    // ── Delete data (optional) ────────────────────────────────────────
    bool bContentDeleted = true, bDiskDeleted = true;
    if (bDeleteAll)
    {
        if (UEditorAssetLibrary::DoesDirectoryExist(TEXT("/Game/MapLink")))
        {
            bContentDeleted =
                UEditorAssetLibrary::DeleteDirectory(TEXT("/Game/MapLink"));
        }

        if (FPaths::DirectoryExists(ActiveRoot))
        {
            bDiskDeleted = IFileManager::Get().DeleteDirectory(
                *ActiveRoot, /*RequireExists=*/false, /*Tree=*/true);
        }
    }

    // ── Clear settings ────────────────────────────────────────────────
    UMapLinkSettings* Mutable = GetMutableDefault<UMapLinkSettings>();
    if (!Mutable)
    {
        return;
    }
    Mutable->BlenderProjectRoot.Path.Empty();
    Mutable->MaterialsLibraryBlend.FilePath.Empty();
    Mutable->MaterialInstancesBlend.FilePath.Empty();
    Mutable->MaterialInstanceContentPath.Path.Empty();
    // v1.15 — per-user ini (SaveConfig targeted the read-only
    // DefaultGame.ini and silently failed to persist the unlink).
    UMapLinkSettings::SaveUserProjectPaths();

    UE_LOG(LogMapLink, Log,
           TEXT("[ProjectSetup] Unlinked MapLink project (was: %s, deleteAll=%d)"),
           *ActiveRoot, bDeleteAll ? 1 : 0);

    if (!bDeleteAll)
    {
        MapLinkNotify::Success(
            LOCTEXT("UnlinkDone",
                "Unlinked. This UE project no longer points at a MapLink "
                "project. Nothing was deleted."));
    }
    else if (bContentDeleted && bDiskDeleted)
    {
        MapLinkNotify::Success(
            LOCTEXT("UnlinkDeleteDone",
                "Unlinked and deleted. The Blender root and /Game/MapLink "
                "content are gone — this project is a clean slate."));
    }
    else
    {
        MapLinkNotify::Warn(FText::Format(
            LOCTEXT("UnlinkDeletePartial",
                "Unlinked, but deletion was incomplete:\n\n"
                "  •  /Game/MapLink content deleted: {0}\n"
                "  •  Blender root deleted: {1}\n\n"
                "Files may be locked (Blender / P4 read-only) or referenced "
                "by open editors. See the Output Log (LogMapLink)."),
            bContentDeleted ? LOCTEXT("YesLbl", "yes") : LOCTEXT("NoLbl", "NO"),
            bDiskDeleted    ? LOCTEXT("YesLbl", "yes") : LOCTEXT("NoLbl", "NO")));
    }
}

void FMapLinkProjectSetup::ShowNewProjectDialog()
{
    // Defence in depth: the menu entry is greyed when a project is active,
    // but guard the entry point too in case it's invoked another way.
    FString ActiveRoot;
    if (IsSetupActive(&ActiveRoot))
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Printf(
            TEXT("This UE project is already linked to a MapLink project at:\n%s\n\n"
                 "Only one MapLink project is allowed per UE project. Use "
                 "MapLink → Unlink Project to detach it first, then create "
                 "or open another."),
            *ActiveRoot)));
        return;
    }

    // Broken state: content carries MapLink proxies but the root is gone.
    // Creating a NEW project would collide with the existing /Game/MapLink
    // content — steer the user to Open (locate the root) instead.
    if (GetProjectState() == EMapLinkProjectState::Broken)
    {
        const EAppReturnType::Type BrokenChoice = FMessageDialog::Open(
            EAppMsgType::YesNo,
            LOCTEXT("BrokenStateNewWarn",
                "This UE project already contains MapLink content, but its "
                "MapLink project root is missing (moved, deleted or not "
                "synced).\n\n"
                "The recommended fix is Open MapLink Project… (locate the "
                "existing root).\n\n"
                "Create a NEW project anyway? Its content may collide with "
                "the existing /Game/MapLink assets."));
        if (BrokenChoice != EAppReturnType::Yes)
        {
            return;
        }
    }

    if (!FSlateApplication::IsInitialized())
    {
        return;
    }

    FString DefaultSetupName = FApp::GetProjectName();
    if (DefaultSetupName.IsEmpty())
    {
        DefaultSetupName = TEXT("MapLink");
    }

    // Default to a local, non-synced location. Documents is frequently
    // OneDrive Known-Folder-Move / Controlled Folder Access protected, which
    // blocks the external blender.exe (and even temp-file saves) — so prefer a
    // plain local folder. UI prefill only; manifest paths are stored relative.
#if PLATFORM_WINDOWS
    FString DefaultRoot = FPaths::Combine(TEXT("C:/MapLink"), DefaultSetupName);
#else
    FString DefaultRoot = FPaths::Combine(
        FPlatformProcess::UserDir(), TEXT("MapLink"), DefaultSetupName);
#endif
    FPaths::NormalizeDirectoryName(DefaultRoot);

    TSharedRef<SWindow> Window = SNew(SWindow)
        .Title(LOCTEXT("NewProjectTitle", "New MapLink Project"))
        .SizingRule(ESizingRule::Autosized)
        .SupportsMinimize(false)
        .SupportsMaximize(false);

    TSharedRef<SMapLinkNewProjectDialog> Dialog =
        SNew(SMapLinkNewProjectDialog)
        .ParentWindow(Window)
        .DefaultSetupName(DefaultSetupName)
        .DefaultRoot(DefaultRoot);

    Window->SetContent(Dialog);

    TSharedPtr<SWindow> ParentWin =
        FSlateApplication::Get().GetActiveTopLevelWindow();
    FSlateApplication::Get().AddModalWindow(Window, ParentWin);

    if (!Dialog->WasConfirmed())
    {
        return;
    }

    RunScaffold(Dialog->GetSetupName(), Dialog->GetRootPath(), Dialog->GetStem());
}


// ---------------------------------------------------------------------------
// Install / Update the MapLink Blender addon
// ---------------------------------------------------------------------------
void FMapLinkProjectSetup::InstallBlenderAddon()
{
    // Resolve Blender the same way the rest of the plugin does — env var
    // MAPLINK_BLENDER_EXE first, then default install locations.  Nothing
    // user-specific.
    FString PreflightError;
    EMapLinkResult PreflightResult = EMapLinkResult::Success;
    if (!FMapLinkBlenderBridge::Preflight(PreflightError, PreflightResult))
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(PreflightError));
        return;
    }
    const FString BlenderExe = FMapLinkBlenderBridge::GetBlenderExecutablePath();

    // Derive the addon source + installer script from the plugin's own base
    // dir — portable across machines and users.
    const FString PluginBaseDir = FMapLinkUtils::GetPluginBaseDir();
    if (PluginBaseDir.IsEmpty())
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT(
            "MapLink: could not locate the plugin base directory.")));
        return;
    }

    const FString AddonDir = FPaths::Combine(PluginBaseDir, TEXT("blender"));
    const FString ScriptPath = FPaths::Combine(
        PluginBaseDir, TEXT("Scripts"), TEXT("maplink_install_addon.py"));

    if (!FPaths::FileExists(FPaths::Combine(AddonDir, TEXT("__init__.py"))))
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Printf(
            TEXT("MapLink: addon source not found at:\n%s"), *AddonDir)));
        return;
    }
    if (!FPaths::FileExists(ScriptPath))
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Printf(
            TEXT("MapLink: installer script not found at:\n%s"), *ScriptPath)));
        return;
    }

    // IMPORTANT: no --factory-startup.  The installer saves user preferences;
    // under factory-startup that would overwrite the user's real settings with
    // defaults (the script also refuses to run under factory-startup as a
    // guard).
    const FString Params = FString::Printf(
        TEXT("--background --python-exit-code 1 --python \"%s\" -- ")
        TEXT("--addon-dir \"%s\" ")
        TEXT("--plugin-dir \"%s\" --artifact-root \"%s\""),
        *ScriptPath, *AddonDir, *PluginBaseDir,
        *FMapLinkUtils::GetArtifactRootDir());

    UE_LOG(LogMapLink, Log,
           TEXT("[InstallAddon] Launching Blender: %s %s"), *BlenderExe, *Params);

    int32 ReturnCode = -1;
    FString StdOut;
    FString StdErr;
    const bool bLaunched = FPlatformProcess::ExecProcess(
        *BlenderExe, *Params, &ReturnCode, &StdOut, &StdErr);

    if (!StdOut.IsEmpty())
    {
        UE_LOG(LogMapLink, Log, TEXT("[InstallAddon] stdout:\n%s"), *StdOut);
    }
    if (!StdErr.IsEmpty())
    {
        UE_LOG(LogMapLink, Warning, TEXT("[InstallAddon] stderr:\n%s"), *StdErr);
    }

    const bool bOk = bLaunched && StdOut.Contains(TEXT("MAPLINK_ADDON_OK"));

    if (bOk)
    {
        MapLinkNotify::Success(FText::FromString(FString::Printf(TEXT(
            "MapLink Blender addon installed / updated and enabled.\n"
            "Restart any open Blender session to pick up the new version.\n"
            "Addon source: %s"),
            *AddonDir)));
    }
    else
    {
        FString Detail = StdErr.IsEmpty() ? StdOut : StdErr;
        Detail.TrimStartAndEndInline();
        const int32 MaxLen = 1200;
        if (Detail.Len() > MaxLen)
        {
            Detail = FString(TEXT("...")) + Detail.Right(MaxLen);
        }
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Printf(TEXT(
            "MapLink: Blender addon install FAILED (rc=%d).\n\n"
            "Make sure Blender is fully closed (an open session can lock files), "
            "then try again.\n\nOutput:\n%s"),
            ReturnCode, *Detail)));
    }
}


// ---------------------------------------------------------------------------
// Open the project's Material Instance library .blend
// ---------------------------------------------------------------------------
void FMapLinkProjectSetup::OpenMaterialInstanceLibrary()
{
    const UMapLinkSettings* Settings = UMapLinkSettings::Get();
    FString MiBlend = Settings ? Settings->MaterialInstancesBlend.FilePath : FString();

    // Fall back to <ProjectRoot>/MaterialInstances/Library_MaterialInstances.blend.
    if (MiBlend.IsEmpty() || !FPaths::FileExists(MiBlend))
    {
        const FString Root = UMapLinkSettings::ResolveBlenderProjectRoot(
            /*bPromptOnAutoDetect*/ false, /*bPromptOnMissing*/ false);
        if (!Root.IsEmpty())
        {
            const FString Candidate = FPaths::Combine(
                Root, TEXT("MaterialInstances"),
                TEXT("Library_MaterialInstances.blend"));
            if (FPaths::FileExists(Candidate))
            {
                MiBlend = Candidate;
            }
        }
    }

    if (MiBlend.IsEmpty() || !FPaths::FileExists(MiBlend))
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT(
            "MapLink: Material Instance library .blend not found.\n\n"
            "Set 'Material Instances .blend' in Project Settings > MapLink (or "
            "apply a project manifest), then try again.")));
        return;
    }

    UE_LOG(LogMapLink, Log, TEXT("[OpenMILib] Opening %s"), *MiBlend);
    FMapLinkBlenderBridge::OpenBlenderGUI(MiBlend);
}


// ---------------------------------------------------------------------------
// Open the Material Instance display level (L_MaterialInstances.umap)
// ---------------------------------------------------------------------------
void FMapLinkProjectSetup::OpenMaterialInstancesLevel()
{
    const FString PackageName =
        TEXT("/Game/MapLink/MaterialInstances/L_MaterialInstances");
    const FString DiskPath = FPackageName::LongPackageNameToFilename(
        PackageName, FPackageName::GetMapPackageExtension());

    if (!FPaths::FileExists(DiskPath))
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(FString::Printf(TEXT(
            "MapLink: '%s' doesn't exist yet.\n\nRun Project Setup or Sync "
            "Materials to build it first."), *PackageName)));
        return;
    }

    // LoadMap prompts to save modified packages before switching maps.
    FEditorFileUtils::LoadMap(PackageName);
    UE_LOG(LogMapLink, Log, TEXT("[OpenMILevel] Loaded %s"), *PackageName);
}

#undef LOCTEXT_NAMESPACE
