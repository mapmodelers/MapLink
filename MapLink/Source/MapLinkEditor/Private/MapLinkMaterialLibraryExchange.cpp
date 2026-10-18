// Copyright MapModelers. All Rights Reserved.
//
// v1.12 — Material Library import/export (see header).

#include "MapLinkMaterialLibraryExchange.h"
#include "MapLinkNotify.h"

#include "MapLinkMaterialExporter.h"   // SyncAfterOp (invisible engine tail)
#include "MapLinkSourceControl.h"
#include "MapLinkUtils.h"              // GetMaterialInstancesContentPath
#include "MapLinkPipeline.h"           // LogMapLink
#include "MapLinkSettings.h"           // v1.16 — MI .blend dir (rebuild default)

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/ARFilter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "EditorAssetLibrary.h"

// v1.16 — rebuild from material_instances.json
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Materials/MaterialInterface.h"
#include "MaterialTypes.h"             // FMaterialParameterInfo
#include "Engine/Texture.h"

#include "Interfaces/IPluginManager.h"
#include "Modules/ModuleManager.h"

#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"

#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Styling/AppStyle.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"    // FindPackage

#define LOCTEXT_NAMESPACE "MapLinkMatLibExchange"

// ---------------------------------------------------------------------------
// Shared helpers (private to this translation unit)
// ---------------------------------------------------------------------------

namespace
{
    constexpr int32 kMatLibSchemaVersion = 1;

    FString GetPluginVersionName()
    {
        TSharedPtr<IPlugin> Plugin =
            IPluginManager::Get().FindPlugin(TEXT("MapLink"));
        return (Plugin.IsValid() && !Plugin->GetDescriptor().VersionName.IsEmpty())
            ? Plugin->GetDescriptor().VersionName : FString(TEXT("unknown"));
    }

    /** One Material Instance as the bundle manifest records it. */
    struct FMapLinkMatLibManifestMI
    {
        FString Package;
        TArray<FString> Deps;
    };

    /** One pickable Material Instance row (shared by export + import UI). */
    struct FMatLibRow
    {
        bool    bSelected = true;
        FString DisplayName;     // MI asset name
        FString SubText;         // export: parent name / import: conflict note
        FString PackageName;     // long package name
    };

    /** Checkbox list + Select All/None + OK/Cancel modal. Returns true when
     *  confirmed with at least one row selected. Rows are edited in place. */
    bool ShowPickerDialog(
        const FText& Title,
        const FText& OkLabel,
        TArray<TSharedRef<FMatLibRow>>& Rows,
        TSharedPtr<SWidget> ExtraTopWidget = nullptr)
    {
        if (!FSlateApplication::IsInitialized())
        {
            return false;
        }

        bool bAccepted = false;

        TSharedRef<SWindow> Window = SNew(SWindow)
            .Title(Title)
            .ClientSize(FVector2D(560.f, 540.f))
            .SupportsMaximize(false)
            .SupportsMinimize(false);

        TSharedRef<SScrollBox> List = SNew(SScrollBox);
        for (const TSharedRef<FMatLibRow>& Row : Rows)
        {
            List->AddSlot()
            .Padding(2.f)
            [
                SNew(SCheckBox)
                .IsChecked_Lambda([Row]()
                {
                    return Row->bSelected ? ECheckBoxState::Checked
                                          : ECheckBoxState::Unchecked;
                })
                .OnCheckStateChanged_Lambda([Row](ECheckBoxState S)
                {
                    Row->bSelected = (S == ECheckBoxState::Checked);
                })
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(4.f, 0.f, 8.f, 0.f)
                    [
                        SNew(STextBlock)
                        .Text(FText::FromString(Row->DisplayName))
                    ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [
                        SNew(STextBlock)
                        .Text(FText::FromString(Row->SubText))
                        .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                    ]
                ]
            ];
        }

        auto SetAll = [&Rows](bool bValue)
        {
            for (const TSharedRef<FMatLibRow>& Row : Rows)
            {
                Row->bSelected = bValue;
            }
        };

        TSharedRef<SVerticalBox> Content = SNew(SVerticalBox);

        if (ExtraTopWidget.IsValid())
        {
            Content->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
            [
                ExtraTopWidget.ToSharedRef()
            ];
        }

        Content->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 6.f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 6.f, 0.f)
            [
                SNew(SButton)
                .Text(LOCTEXT("SelectAll", "Select All"))
                .OnClicked_Lambda([SetAll]()
                {
                    SetAll(true);
                    return FReply::Handled();
                })
            ]
            + SHorizontalBox::Slot().AutoWidth()
            [
                SNew(SButton)
                .Text(LOCTEXT("SelectNone", "Select None"))
                .OnClicked_Lambda([SetAll]()
                {
                    SetAll(false);
                    return FReply::Handled();
                })
            ]
        ];

        Content->AddSlot().FillHeight(1.f)
        [
            SNew(SBorder)
            .BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
            .Padding(4.f)
            [
                List
            ]
        ];

        Content->AddSlot().AutoHeight().Padding(0.f, 10.f, 0.f, 0.f)
        .HAlign(HAlign_Right)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 6.f, 0.f)
            [
                SNew(SButton)
                .Text(OkLabel)
                .OnClicked_Lambda([&bAccepted, Window]()
                {
                    bAccepted = true;
                    Window->RequestDestroyWindow();
                    return FReply::Handled();
                })
            ]
            + SHorizontalBox::Slot().AutoWidth()
            [
                SNew(SButton)
                .Text(LOCTEXT("Cancel", "Cancel"))
                .OnClicked_Lambda([Window]()
                {
                    Window->RequestDestroyWindow();
                    return FReply::Handled();
                })
            ]
        ];

        Window->SetContent(
            SNew(SBorder)
            .BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
            .Padding(10.f)
            [
                Content
            ]);

        FSlateApplication::Get().AddModalWindow(Window, nullptr);

        if (!bAccepted)
        {
            return false;
        }
        for (const TSharedRef<FMatLibRow>& Row : Rows)
        {
            if (Row->bSelected)
            {
                return true;
            }
        }
        MapLinkNotify::Warn(
            LOCTEXT("NothingSelected", "Nothing was selected."));
        return false;
    }

    /** "/Game/MapLink/X/MI_Y" → "Content/MapLink/X/MI_Y.uasset" (bundle-rel,
     *  forward slashes). Empty if the package is not under /Game/. */
    FString PackageToBundleRel(const FString& PackageName, const FString& Ext)
    {
        if (!PackageName.StartsWith(TEXT("/Game/")))
        {
            return FString();
        }
        return FString(TEXT("Content/"))
             + PackageName.RightChop(FCString::Strlen(TEXT("/Game/")))
             + Ext;
    }
}

/**
 * Read a bundle manifest.  Extracted from ShowImportDialog in v1.19 so the
 * non-dialog ImportFromManifest() reads exactly the same file the same way --
 * a second parser is how two entry points quietly stop agreeing about what a
 * bundle is.
 */
static bool ParseMatLibManifest(
    const FString& ManifestPath,
    TMap<FString, FString>& OutPackageFiles,
    TArray<FMapLinkMatLibManifestMI>& OutMIs,
    FString& OutError)
{
    OutPackageFiles.Reset();
    OutMIs.Reset();

    FString Json;
    TSharedPtr<FJsonObject> RootJson;
    if (!FFileHelper::LoadFileToString(Json, *ManifestPath))
    {
        OutError = FString::Printf(TEXT("Could not read:\n%s"), *ManifestPath);
        return false;
    }
    {
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
        if (!FJsonSerializer::Deserialize(Reader, RootJson) || !RootJson.IsValid())
        {
            OutError = FString::Printf(
                TEXT("Not a valid MapLink material library manifest:\n%s"),
                *ManifestPath);
            return false;
        }
    }

    int32 Schema = 0;
    RootJson->TryGetNumberField(TEXT("schema_version"), Schema);
    if (Schema > kMatLibSchemaVersion)
    {
        OutError = FString::Printf(
            TEXT("This bundle uses manifest schema v%d; this plugin supports "
                 "up to v%d. Update MapLink to import it."),
            Schema, kMatLibSchemaVersion);
        return false;
    }

    // package → bundle-relative file
    TMap<FString, FString>& PackageFiles = OutPackageFiles;
    {
        const TArray<TSharedPtr<FJsonValue>>* PackagesJson = nullptr;
        if (RootJson->TryGetArrayField(TEXT("packages"), PackagesJson))
        {
            for (const TSharedPtr<FJsonValue>& V : *PackagesJson)
            {
                const TSharedPtr<FJsonObject>* Obj = nullptr;
                if (V->TryGetObject(Obj) && Obj && Obj->IsValid())
                {
                    const FString Pkg  = (*Obj)->GetStringField(TEXT("package"));
                    const FString File = (*Obj)->GetStringField(TEXT("file"));
                    if (!Pkg.IsEmpty() && !File.IsEmpty())
                    {
                        PackageFiles.Add(Pkg, File);
                    }
                }
            }
        }
    }

    // MI list + per-MI deps.
    TArray<FMapLinkMatLibManifestMI>& ManifestMIs = OutMIs;
    {
        const TArray<TSharedPtr<FJsonValue>>* MIsJson = nullptr;
        if (RootJson->TryGetArrayField(TEXT("material_instances"), MIsJson))
        {
            for (const TSharedPtr<FJsonValue>& V : *MIsJson)
            {
                const TSharedPtr<FJsonObject>* Obj = nullptr;
                if (!V->TryGetObject(Obj) || !Obj || !Obj->IsValid())
                {
                    continue;
                }
                FMapLinkMatLibManifestMI MI;
                MI.Package = (*Obj)->GetStringField(TEXT("package"));
                const TArray<TSharedPtr<FJsonValue>>* DepsJson = nullptr;
                if ((*Obj)->TryGetArrayField(TEXT("deps"), DepsJson))
                {
                    for (const TSharedPtr<FJsonValue>& D : *DepsJson)
                    {
                        MI.Deps.Add(D->AsString());
                    }
                }
                if (!MI.Package.IsEmpty())
                {
                    ManifestMIs.Add(MoveTemp(MI));
                }
            }
        }
    }

    if (ManifestMIs.Num() == 0 || PackageFiles.Num() == 0)
    {
        OutError = TEXT("The manifest lists no Material Instances.");
        return false;
    }

    return true;
}


const TCHAR* FMapLinkMaterialLibraryExchange::GetManifestFilename()
{
    return TEXT("matlib_manifest.json");
}

void FMapLinkMaterialLibraryExchange::CollectPackageClosure(
    const FName PackageName, TSet<FName>& InOutClosure)
{
    if (InOutClosure.Contains(PackageName))
    {
        return;
    }
    const FString NameStr = PackageName.ToString();
    if (!NameStr.StartsWith(TEXT("/Game/")))
    {
        // Plugin ("/MapLink/"), engine and script packages never travel.
        return;
    }
    InOutClosure.Add(PackageName);

    IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
        "AssetRegistry").Get();

    TArray<FName> Deps;
    AR.GetDependencies(PackageName, Deps,
                       UE::AssetRegistry::EDependencyCategory::Package);
    for (const FName Dep : Deps)
    {
        CollectPackageClosure(Dep, InOutClosure);
    }
}

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------

/**
 * Write the bundle: copy the /Game/ closure of *SelectedMIs* into *BundleDir*
 * and emit the manifest.  v1.19 -- extracted from ShowExportDialog so the
 * dialog and ExportForAssets() produce byte-identical bundles by construction
 * rather than by two people remembering to keep them in step.
 */
static bool WriteMatLibBundle(
    const TArray<FString>& SelectedMIs,
    const TSet<FName>& Closure,
    const TMap<FString, TArray<FString>>& PerMIDeps,
    const FString& BundleDir,
    const FString& LibraryName,
    int32& OutCopied,
    TArray<FString>& OutFailed,
    FString& OutManifestPath,
    FString& OutError)
{
    OutCopied = 0;
    OutFailed.Reset();

    // ── Save dirty members, then copy files ───────────────────────────────
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    int32& Copied = OutCopied;
    TArray<FString>& Failed = OutFailed;
    TArray<TSharedPtr<FJsonValue>> PackagesJson;

    for (const FName PkgName : Closure)
    {
        const FString Pkg = PkgName.ToString();

        // Persist in-memory edits so the bundle matches what the user sees.
        if (UPackage* Loaded = FindPackage(nullptr, *Pkg))
        {
            if (Loaded->IsDirty())
            {
                UEditorAssetLibrary::SaveAsset(Pkg, /*bOnlyIfIsDirty=*/true);
            }
        }

        FString SrcFile;
        if (!FPackageName::DoesPackageExist(Pkg, &SrcFile))
        {
            Failed.Add(Pkg + TEXT(" (no file on disk)"));
            continue;
        }

        const FString Ext = FPaths::GetExtension(SrcFile, /*bIncludeDot=*/true);
        const FString Rel = PackageToBundleRel(Pkg, Ext);
        if (Rel.IsEmpty())
        {
            continue;
        }

        const FString DestFile = BundleDir / Rel;
        PF.CreateDirectoryTree(*FPaths::GetPath(DestFile));
        if (IFileManager::Get().Copy(*DestFile, *SrcFile,
                /*bReplace=*/true, /*bEvenIfReadOnly=*/true) != COPY_OK)
        {
            Failed.Add(Pkg);
            continue;
        }
        ++Copied;

        TSharedPtr<FJsonObject> PkgJson = MakeShared<FJsonObject>();
        PkgJson->SetStringField(TEXT("package"), Pkg);
        PkgJson->SetStringField(TEXT("file"), Rel);
        PackagesJson.Add(MakeShared<FJsonValueObject>(PkgJson));
    }

    // ── Manifest ──────────────────────────────────────────────────────────
    {
        TSharedPtr<FJsonObject> RootJson = MakeShared<FJsonObject>();
        RootJson->SetNumberField(TEXT("schema_version"), kMatLibSchemaVersion);
        RootJson->SetStringField(TEXT("maplink_version"), GetPluginVersionName());
        RootJson->SetStringField(TEXT("library_name"), LibraryName);
        RootJson->SetStringField(TEXT("source_project"), FApp::GetProjectName());
        RootJson->SetStringField(TEXT("exported_at"),
            FDateTime::UtcNow().ToIso8601());

        TArray<TSharedPtr<FJsonValue>> MIsJson;
        for (const FString& MI : SelectedMIs)
        {
            TSharedPtr<FJsonObject> MIJson = MakeShared<FJsonObject>();
            MIJson->SetStringField(TEXT("package"), MI);

            TArray<TSharedPtr<FJsonValue>> DepsJson;
            if (const TArray<FString>* Deps = PerMIDeps.Find(MI))
            {
                for (const FString& Dep : *Deps)
                {
                    DepsJson.Add(MakeShared<FJsonValueString>(Dep));
                }
            }
            MIJson->SetArrayField(TEXT("deps"), DepsJson);
            MIsJson.Add(MakeShared<FJsonValueObject>(MIJson));
        }
        RootJson->SetArrayField(TEXT("material_instances"), MIsJson);
        RootJson->SetArrayField(TEXT("packages"), PackagesJson);

        FString Out;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
        FJsonSerializer::Serialize(RootJson.ToSharedRef(), Writer);

        OutManifestPath = BundleDir
            / FMapLinkMaterialLibraryExchange::GetManifestFilename();
        const FString& ManifestPath = OutManifestPath;
        if (!FFileHelper::SaveStringToFile(Out, *ManifestPath,
                FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
        {
            OutError = FString::Printf(
                TEXT("Failed to write the bundle manifest:\n%s"), *ManifestPath);
            return false;
        }
    }

    return true;
}

/**
 * Copy a resolved package set out of a bundle.  v1.19 -- extracted from
 * ShowImportDialog, tail sync and all, so the scriptable path cannot skip the
 * SyncAfterOp that keeps the Blender libraries honest.
 */
static FMapLinkMatLibImportResult CopyBundlePackages(
    const TSet<FString>& ToCopy,
    const TMap<FString, FString>& PackageFiles,
    const FString& BundleDir,
    bool bOverwrite)
{
    FMapLinkMatLibImportResult Result;
    Result.bOk = true;

    // ── Copy ──────────────────────────────────────────────────────────────
    const FString CLDesc = TEXT("[MapLink] Import Material Library");
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();

    int32& Imported = Result.Imported;
    int32& Overwritten = Result.Overwritten;
    int32& Skipped = Result.Skipped;
    TArray<FString>& Failed = Result.Failed;

    for (const FString& Pkg : ToCopy)
    {
        const FString* Rel = PackageFiles.Find(Pkg);
        if (!Rel)
        {
            Failed.Add(Pkg + TEXT(" (not in bundle)"));
            continue;
        }
        const FString SrcFile = BundleDir / *Rel;
        if (!FPaths::FileExists(SrcFile))
        {
            Failed.Add(Pkg + TEXT(" (bundle file missing)"));
            continue;
        }

        const bool bExists = FPackageName::DoesPackageExist(Pkg);
        if (bExists && !bOverwrite)
        {
            ++Skipped;
            continue;
        }
        if (bExists && FindPackage(nullptr, *Pkg))
        {
            // Loaded in this editor session — overwriting the file under a
            // live UPackage invites corruption. Skip and report.
            Failed.Add(Pkg + TEXT(" (loaded in editor — restart and re-import "
                                  "to overwrite)"));
            continue;
        }

        const FString Ext = FPaths::GetExtension(*Rel, /*bIncludeDot=*/true);
        FString DestFile;
        if (bExists)
        {
            FPackageName::DoesPackageExist(Pkg, &DestFile);
        }
        else
        {
            DestFile = FPackageName::LongPackageNameToFilename(Pkg, Ext);
        }
        if (DestFile.IsEmpty())
        {
            Failed.Add(Pkg);
            continue;
        }

        PF.CreateDirectoryTree(*FPaths::GetPath(DestFile));
        if (bExists)
        {
            // Check out before the replace so P4 sees an edit, not a stomp.
            FMapLinkSourceControl::EnsureFileInChangelist(DestFile, CLDesc);
        }
        if (IFileManager::Get().Copy(*DestFile, *SrcFile,
                /*bReplace=*/true, /*bEvenIfReadOnly=*/true) != COPY_OK)
        {
            Failed.Add(Pkg);
            continue;
        }
        if (!bExists)
        {
            FMapLinkSourceControl::EnsureFileInChangelist(DestFile, CLDesc);
        }
        bExists ? ++Overwritten : ++Imported;
    }

    // ── Register + tail sync ──────────────────────────────────────────────
    if (Imported > 0 || Overwritten > 0)
    {
        IAssetRegistry& AR =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
                "AssetRegistry").Get();
        TArray<FString> ScanPaths;
        ScanPaths.Add(TEXT("/Game/MapLink"));
        AR.ScanPathsSynchronous(ScanPaths, /*bForceRescan=*/true);

        // Invisible engine tail — regenerates the Blender libraries into the
        // same changelist. Never a user-facing sync button.
        const EMapLinkResult R = FMapLinkMaterialExporter::SyncAfterOp(CLDesc);
        if (R != EMapLinkResult::Success &&
            R != EMapLinkResult::NoMaterialInstancesFound)
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("[MatLibExchange] import tail sync returned code %d"),
                   static_cast<int32>(R));
        }
    }

    for (const FString& F : Failed)
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("[MatLibExchange] import failed for: %s"), *F);
    }
    UE_LOG(LogMapLink, Log,
           TEXT("[MatLibExchange] Import done: %d new, %d overwritten, "
                "%d skipped, %d failed (bundle: %s)"),
           Imported, Overwritten, Skipped, Failed.Num(), *BundleDir);

    return Result;
}

// The /Game/ closure of each MI, plus the per-MI dep list the manifest records
// so an importer can take one MI without taking all of them.
void FMapLinkMaterialLibraryExchange::BuildClosureForMIs(
    const TArray<FString>& MIPackages,
    TSet<FName>& OutClosure,
    TMap<FString, TArray<FString>>& OutPerMIDeps)
{
    for (const FString& MIPkg : MIPackages)
    {
        TSet<FName> MIClosure;
        CollectPackageClosure(FName(*MIPkg), MIClosure);
        OutClosure.Append(MIClosure);

        TArray<FString>& Deps = OutPerMIDeps.Add(MIPkg);
        for (const FName Pkg : MIClosure)
        {
            if (Pkg.ToString() != MIPkg)
            {
                Deps.Add(Pkg.ToString());
            }
        }
        Deps.Sort();
    }
}

void FMapLinkMaterialLibraryExchange::ShowExportDialog()
{
    const FString ContentPath = FMapLinkUtils::GetMaterialInstancesContentPath();

    // ── Collect the project's Material Instances ─────────────────────────
    IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
        "AssetRegistry").Get();

    FARFilter Filter;
    Filter.bRecursivePaths = true;
    Filter.bRecursiveClasses = true;
    Filter.PackagePaths.Add(FName(*ContentPath));
    Filter.ClassPaths.Add(
        UMaterialInstanceConstant::StaticClass()->GetClassPathName());

    TArray<FAssetData> Found;
    AR.GetAssets(Filter, Found);

    if (Found.Num() == 0)
    {
        MapLinkNotify::Warn(FText::Format(
            LOCTEXT("ExportNoMIs",
                "No Material Instances found under {0} — nothing to export."),
            FText::FromString(ContentPath)));
        return;
    }

    Found.Sort([](const FAssetData& A, const FAssetData& B)
    {
        return A.AssetName.LexicalLess(B.AssetName);
    });

    TArray<TSharedRef<FMatLibRow>> Rows;
    Rows.Reserve(Found.Num());
    for (const FAssetData& AD : Found)
    {
        TSharedRef<FMatLibRow> Row = MakeShared<FMatLibRow>();
        Row->DisplayName = AD.AssetName.ToString();
        Row->PackageName = AD.PackageName.ToString();

        // Parent name for context (registry tag only — no asset load).
        FString ParentName;
        {
            FString ParentTag;
            if (AD.GetTagValue(FName(TEXT("Parent")), ParentTag)
                && !ParentTag.IsEmpty())
            {
                // "/Game/X/M_Y.M_Y" or "M_Y'... '" → short object name.
                int32 DotPos = INDEX_NONE;
                ParentTag.FindLastChar(TEXT('.'), DotPos);
                ParentName = (DotPos != INDEX_NONE)
                    ? ParentTag.Mid(DotPos + 1) : ParentTag;
                ParentName.RemoveFromEnd(TEXT("'"));
            }
        }
        Row->SubText = ParentName.IsEmpty()
            ? FString()
            : FString::Printf(TEXT("(%s)"), *ParentName);
        Rows.Add(Row);
    }

    // Library name field.
    FString LibraryName = FString(FApp::GetProjectName()) + TEXT("_MatLib");
    TSharedRef<SWidget> NameWidget =
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
          .Padding(0.f, 0.f, 8.f, 0.f)
        [
            SNew(STextBlock)
            .Text(LOCTEXT("LibNameLbl", "Library name"))
        ]
        + SHorizontalBox::Slot().FillWidth(1.f)
        [
            SNew(SEditableTextBox)
            .Text(FText::FromString(LibraryName))
            .OnTextChanged_Lambda([&LibraryName](const FText& T)
            {
                LibraryName = T.ToString();
            })
        ];

    if (!ShowPickerDialog(
            LOCTEXT("ExportTitle", "Export Material Library"),
            LOCTEXT("ExportOk", "Export…"),
            Rows, NameWidget))
    {
        return;
    }

    LibraryName.TrimStartAndEndInline();
    if (LibraryName.IsEmpty())
    {
        LibraryName = FString(FApp::GetProjectName()) + TEXT("_MatLib");
    }

    // ── Destination folder ────────────────────────────────────────────────
    IDesktopPlatform* DP = FDesktopPlatformModule::Get();
    FString DestBase;
    if (!DP || !DP->OpenDirectoryDialog(
            FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr),
            TEXT("Select destination folder for the material library bundle"),
            FPaths::ProjectDir(), DestBase) || DestBase.IsEmpty())
    {
        return;
    }
    FPaths::NormalizeDirectoryName(DestBase);
    const FString BundleDir = DestBase / (LibraryName + TEXT("_MapLinkMatLib"));

    // ── Which MIs were ticked ─────────────────────────────────────────────
    TArray<FString> SelectedMIs;
    for (const TSharedRef<FMatLibRow>& Row : Rows)
    {
        if (Row->bSelected)
        {
            SelectedMIs.Add(Row->PackageName);
        }
    }

    TSet<FName> Closure;
    TMap<FString, TArray<FString>> PerMIDeps;
    BuildClosureForMIs(SelectedMIs, Closure, PerMIDeps);

    int32 Copied = 0;
    TArray<FString> Failed;
    FString ManifestPath, ExportError;
    if (!WriteMatLibBundle(SelectedMIs, Closure, PerMIDeps, BundleDir,
                           LibraryName, Copied, Failed, ManifestPath,
                           ExportError))
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(ExportError));
        return;
    }

    UE_LOG(LogMapLink, Log,
           TEXT("[MatLibExchange] Exported %d MIs (%d packages) to %s (%d failures)"),
           SelectedMIs.Num(), Copied, *BundleDir, Failed.Num());

    FString FailNote;
    if (Failed.Num() > 0)
    {
        FailNote = FString::Printf(
            TEXT("\n\n%d package(s) could not be copied — see the Output Log."),
            Failed.Num());
        for (const FString& F : Failed)
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("[MatLibExchange] export failed for: %s"), *F);
        }
    }

    MapLinkNotify::Success(FText::Format(
        LOCTEXT("ExportDone",
            "Material library '{0}' exported.\n\n"
            "{1} Material Instances ({2} packages including parents and "
            "textures) written to:\n{3}{4}"),
        FText::FromString(LibraryName),
        FText::AsNumber(SelectedMIs.Num()),
        FText::AsNumber(Copied),
        FText::FromString(BundleDir),
        FText::FromString(FailNote)));
}

// ---------------------------------------------------------------------------
// Import
// ---------------------------------------------------------------------------

void FMapLinkMaterialLibraryExchange::ShowImportDialog()
{
    // ── Pick and parse the manifest ───────────────────────────────────────
    IDesktopPlatform* DP = FDesktopPlatformModule::Get();
    TArray<FString> PickedFiles;
    if (!DP || !DP->OpenFileDialog(
            FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr),
            TEXT("Open MapLink material library (matlib_manifest.json)"),
            FPaths::ProjectDir(), TEXT(""),
            TEXT("MapLink material library|matlib_manifest.json|JSON files (*.json)|*.json"),
            EFileDialogFlags::None, PickedFiles) || PickedFiles.Num() == 0)
    {
        return;
    }
    const FString ManifestPath = PickedFiles[0];
    const FString BundleDir = FPaths::GetPath(ManifestPath);

    FString ParseError;
    TMap<FString, FString> PackageFiles;
    TArray<FMapLinkMatLibManifestMI> ManifestMIs;
    if (!ParseMatLibManifest(ManifestPath, PackageFiles, ManifestMIs, ParseError))
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(ParseError));
        return;
    }

    // ── Picker (with conflict notes + policy) ─────────────────────────────
    TArray<TSharedRef<FMatLibRow>> Rows;
    Rows.Reserve(ManifestMIs.Num());
    for (const FMapLinkMatLibManifestMI& MI : ManifestMIs)
    {
        TSharedRef<FMatLibRow> Row = MakeShared<FMatLibRow>();
        Row->PackageName = MI.Package;
        Row->DisplayName = FPackageName::GetShortName(MI.Package);
        const bool bExists = FPackageName::DoesPackageExist(MI.Package);
        Row->SubText   = bExists ? TEXT("(already exists)") : FString();
        Row->bSelected = !bExists;   // default: new ones ticked
        Rows.Add(Row);
    }
    Rows.Sort([](const TSharedRef<FMatLibRow>& A, const TSharedRef<FMatLibRow>& B)
    {
        return A->DisplayName < B->DisplayName;
    });

    // Conflict policy combo. Rename-on-import is deliberately not offered:
    // package identity is baked inside a .uasset, so a file-level bundle can
    // only restore to the original package path.
    TSharedPtr<TArray<TSharedPtr<FString>>> PolicyOptions =
        MakeShared<TArray<TSharedPtr<FString>>>();
    PolicyOptions->Add(MakeShared<FString>(TEXT("Skip existing (default)")));
    PolicyOptions->Add(MakeShared<FString>(TEXT("Overwrite existing")));
    TSharedPtr<FString> CurrentPolicy = (*PolicyOptions)[0];

    TSharedRef<SWidget> PolicyWidget =
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
          .Padding(0.f, 0.f, 8.f, 0.f)
        [
            SNew(STextBlock)
            .Text(LOCTEXT("PolicyLbl", "If an asset already exists"))
        ]
        + SHorizontalBox::Slot().AutoWidth()
        [
            SNew(SComboBox<TSharedPtr<FString>>)
            .OptionsSource(PolicyOptions.Get())
            .InitiallySelectedItem(CurrentPolicy)
            .OnGenerateWidget_Lambda([](TSharedPtr<FString> In)
            {
                return SNew(STextBlock).Text(FText::FromString(*In));
            })
            .OnSelectionChanged_Lambda(
                [&CurrentPolicy](TSharedPtr<FString> New, ESelectInfo::Type)
            {
                if (New.IsValid())
                {
                    CurrentPolicy = New;
                }
            })
            [
                SNew(STextBlock)
                .Text_Lambda([&CurrentPolicy]()
                {
                    return FText::FromString(
                        CurrentPolicy.IsValid() ? *CurrentPolicy
                                                : FString(TEXT("Policy")));
                })
            ]
        ];

    if (!ShowPickerDialog(
            LOCTEXT("ImportTitle", "Import Material Library"),
            LOCTEXT("ImportOk", "Import"),
            Rows, PolicyWidget))
    {
        return;
    }

    const bool bOverwrite =
        CurrentPolicy.IsValid() && *CurrentPolicy == TEXT("Overwrite existing");

    // ── Resolve the package set to copy ───────────────────────────────────
    TSet<FString> ToCopy;
    for (const FMapLinkMatLibManifestMI& MI : ManifestMIs)
    {
        const TSharedRef<FMatLibRow>* Row = Rows.FindByPredicate(
            [&MI](const TSharedRef<FMatLibRow>& R)
            {
                return R->PackageName == MI.Package;
            });
        if (!Row || !(*Row)->bSelected)
        {
            continue;
        }
        ToCopy.Add(MI.Package);
        for (const FString& Dep : MI.Deps)
        {
            ToCopy.Add(Dep);
        }
    }

    const FMapLinkMatLibImportResult Result =
        CopyBundlePackages(ToCopy, PackageFiles, BundleDir, bOverwrite);
    const int32 Imported = Result.Imported;
    const int32 Overwritten = Result.Overwritten;
    const int32 Skipped = Result.Skipped;
    const TArray<FString>& Failed = Result.Failed;

    MapLinkNotify::Success(FText::Format(
        LOCTEXT("ImportDone",
            "Material library import finished.\n\n"
            "New: {0}\nOverwritten: {1}\nSkipped (already exist): {2}\n"
            "Failed: {3}{4}"),
        FText::AsNumber(Imported),
        FText::AsNumber(Overwritten),
        FText::AsNumber(Skipped),
        FText::AsNumber(Failed.Num()),
        Failed.Num() > 0
            ? LOCTEXT("ImportFailNote", "\n\nSee the Output Log (LogMapLink).")
            : FText::GetEmpty()));
}

// ---------------------------------------------------------------------------
// v1.19 — the same operations, without a dialog
// ---------------------------------------------------------------------------

const TCHAR* FMapLinkMaterialLibraryExchange::GetPendingImportFilename()
{
    return TEXT("pending_matlib_import.json");
}

FString FMapLinkMaterialLibraryExchange::ResolveMIPackage(
    const FString& NameOrPackage)
{
    if (NameOrPackage.StartsWith(TEXT("/")))
    {
        return NameOrPackage;
    }
    const FString ContentPath = FMapLinkUtils::GetMaterialInstancesContentPath();
    const FString Candidate = ContentPath / NameOrPackage;
    if (FPackageName::DoesPackageExist(Candidate))
    {
        return Candidate;
    }

    // Fall back to the registry: the MI may sit in a subfolder of the content
    // path, and a name that resolves nowhere is better reported by the caller
    // than silently turned into a path that cannot exist.
    IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
        "AssetRegistry").Get();
    FARFilter Filter;
    Filter.bRecursivePaths = true;
    Filter.bRecursiveClasses = true;
    Filter.PackagePaths.Add(FName(*ContentPath));
    Filter.ClassPaths.Add(
        UMaterialInstanceConstant::StaticClass()->GetClassPathName());
    TArray<FAssetData> Found;
    AR.GetAssets(Filter, Found);
    for (const FAssetData& AD : Found)
    {
        if (AD.AssetName.ToString() == NameOrPackage)
        {
            return AD.PackageName.ToString();
        }
    }
    return FString();
}

bool FMapLinkMaterialLibraryExchange::ExportForAssets(
    const TArray<FString>& MINames,
    const FString& OutDir,
    const FString& LibraryName,
    FString& OutManifestPath,
    FString& OutError)
{
    OutManifestPath.Reset();
    OutError.Reset();

    if (MINames.Num() == 0)
    {
        OutError = TEXT("No Material Instances were named.");
        return false;
    }
    if (OutDir.IsEmpty())
    {
        OutError = TEXT("No destination folder.");
        return false;
    }

    TArray<FString> Packages;
    TArray<FString> Unresolved;
    for (const FString& Name : MINames)
    {
        const FString Pkg = ResolveMIPackage(Name);
        if (Pkg.IsEmpty() || !FPackageName::DoesPackageExist(Pkg))
        {
            Unresolved.Add(Name);
            continue;
        }
        Packages.AddUnique(Pkg);
    }
    if (Packages.Num() == 0)
    {
        OutError = FString::Printf(
            TEXT("None of the %d named Material Instance(s) exist in this "
                 "project."), MINames.Num());
        return false;
    }
    for (const FString& U : Unresolved)
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("[MatLibExchange] ExportForAssets: '%s' not found; skipped."),
               *U);
    }

    FString Name = LibraryName;
    Name.TrimStartAndEndInline();
    if (Name.IsEmpty())
    {
        Name = FString(FApp::GetProjectName()) + TEXT("_MatLib");
    }
    const FString BundleDir = OutDir / (Name + TEXT("_MapLinkMatLib"));

    TSet<FName> Closure;
    TMap<FString, TArray<FString>> PerMIDeps;
    BuildClosureForMIs(Packages, Closure, PerMIDeps);

    int32 Copied = 0;
    TArray<FString> Failed;
    if (!WriteMatLibBundle(Packages, Closure, PerMIDeps, BundleDir, Name,
                           Copied, Failed, OutManifestPath, OutError))
    {
        return false;
    }

    UE_LOG(LogMapLink, Log,
           TEXT("[MatLibExchange] ExportForAssets: %d MI(s), %d package(s) -> %s "
                "(%d failure(s), %d unresolved)"),
           Packages.Num(), Copied, *BundleDir, Failed.Num(), Unresolved.Num());
    return true;
}

FMapLinkMatLibImportResult FMapLinkMaterialLibraryExchange::ImportFromManifest(
    const FString& ManifestPath,
    EMapLinkMatLibConflict Policy,
    const TArray<FString>& OnlyMIs)
{
    FMapLinkMatLibImportResult Result;

    TMap<FString, FString> PackageFiles;
    TArray<FMapLinkMatLibManifestMI> ManifestMIs;
    if (!ParseMatLibManifest(ManifestPath, PackageFiles, ManifestMIs,
                             Result.Error))
    {
        return Result;
    }

    // An empty OnlyMIs means "everything the manifest lists" -- the scripted
    // equivalent of Select All, which is the only sane default when nobody is
    // there to tick boxes.
    TSet<FString> Wanted;
    for (const FString& Name : OnlyMIs)
    {
        Wanted.Add(Name);
    }

    TSet<FString> ToCopy;
    int32 Requested = 0;
    for (const FMapLinkMatLibManifestMI& MI : ManifestMIs)
    {
        if (Wanted.Num() > 0
            && !Wanted.Contains(MI.Package)
            && !Wanted.Contains(FPackageName::GetShortName(MI.Package)))
        {
            continue;
        }
        ++Requested;
        ToCopy.Add(MI.Package);
        for (const FString& Dep : MI.Deps)
        {
            ToCopy.Add(Dep);
        }
    }

    if (ToCopy.Num() == 0)
    {
        Result.Error = TEXT("Nothing in the bundle matched the requested "
                            "Material Instances.");
        return Result;
    }

    const FString BundleDir = FPaths::GetPath(ManifestPath);
    Result = CopyBundlePackages(
        ToCopy, PackageFiles, BundleDir,
        Policy == EMapLinkMatLibConflict::Overwrite);

    UE_LOG(LogMapLink, Log,
           TEXT("[MatLibExchange] ImportFromManifest: %d MI(s) requested; "
                "%d new, %d overwritten, %d skipped, %d failed"),
           Requested, Result.Imported, Result.Overwritten, Result.Skipped,
           Result.Failed.Num());
    return Result;
}

void FMapLinkMaterialLibraryExchange::ProcessPendingImport()
{
    const FString Root =
        UMapLinkSettings::ResolveBlenderProjectRoot(false, false);
    if (Root.IsEmpty())
    {
        return;
    }
    const FString PendingPath =
        Root / TEXT("MaterialInstances") / GetPendingImportFilename();
    if (!FPaths::FileExists(PendingPath))
    {
        return;
    }

    FString Json;
    TSharedPtr<FJsonObject> RootJson;
    if (!FFileHelper::LoadFileToString(Json, *PendingPath))
    {
        return;
    }
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    if (!FJsonSerializer::Deserialize(Reader, RootJson) || !RootJson.IsValid())
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("[MatLibExchange] unreadable %s"), *PendingPath);
        return;
    }

    TArray<FString> Names;
    const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
    if (RootJson->TryGetArrayField(TEXT("material_instances"), Arr))
    {
        for (const TSharedPtr<FJsonValue>& V : *Arr)
        {
            const FString N = V->AsString();
            if (!N.IsEmpty())
            {
                Names.Add(N);
            }
        }
    }

    // Anything that has arrived since the addon wrote the file is no longer
    // pending. Re-checking here rather than trusting the record means an
    // import through any route -- bundle, rebuild, hand copy -- clears it.
    TArray<FString> StillMissing;
    for (const FString& N : Names)
    {
        const FString Pkg = ResolveMIPackage(N);
        if (Pkg.IsEmpty() || !FPackageName::DoesPackageExist(Pkg))
        {
            StillMissing.Add(N);
        }
    }

    if (StillMissing.Num() == 0)
    {
        // Resolved. Remove the record rather than leaving a note that will
        // read as an outstanding problem forever.
        IFileManager::Get().Delete(*PendingPath, false, true);
        UE_LOG(LogMapLink, Log,
               TEXT("[MatLibExchange] pending material instances all present; "
                    "removed %s"), *PendingPath);
        return;
    }

    FString List;
    for (int32 i = 0; i < StillMissing.Num() && i < 6; ++i)
    {
        List += (i ? TEXT(", ") : TEXT("")) + StillMissing[i];
    }
    if (StillMissing.Num() > 6)
    {
        List += FString::Printf(TEXT(", +%d more"), StillMissing.Num() - 6);
    }

    // Deliberately NOT an automatic import. The record carries names, and
    // names do not say which project to take them from -- only the person who
    // vendored the asset in knows that. Stating the gap is the honest move;
    // guessing at a source project is not.
    UE_LOG(LogMapLink, Warning,
           TEXT("[MatLibExchange] %d material instance(s) referenced by "
                "vendored-in assets are not in this project: %s"),
           StillMissing.Num(), *List);

    MapLinkNotify::Warn(FText::Format(
        LOCTEXT("PendingMatLib",
            "{0} material instance(s) used by assets copied into this project "
            "are missing:\n{1}\n\nUse Materials ▸ More ▸ Import… with a "
            "bundle exported from the project they came from."),
        FText::AsNumber(StillMissing.Num()),
        FText::FromString(List)));
}

// ---------------------------------------------------------------------------
// Rebuild from material_instances.json
//
// Not an import. Import restores .uasset files byte for byte; this builds
// assets out of recorded parameter values, so it is lossy by construction and
// is kept as its own entry point so the two are never confused.
//
// What the JSON gives us, per entry:
//   MaterialParent       bare asset name, e.g. "M_Opq_Lerp2"
//   MaterialInstance     bare asset name
//   ScalarParameters     name -> number
//   TextureParameters    name -> { path, rel, anchor, srgb, compression,
//                        class, status, asset, schema_version } as of v1.16.
//                        A bare string (the pre-v1.16 shape) is still read.
//   VectorParameters     name -> [r, g, b, a]
//   UnrealSoftReference   original package path, which may be from an older
//                        project layout and is therefore NOT used as the
//                        destination -- rebuilt assets go to the configured
//                        Material Instances content path.
// ---------------------------------------------------------------------------

namespace
{
    /** name -> FAssetData for every asset of a class, across the project. */
    void IndexAssetsByName(
        IAssetRegistry& AR, UClass* Class, TMap<FString, FAssetData>& Out,
        TSet<FString>* OutAmbiguous = nullptr)
    {
        FARFilter Filter;
        Filter.bRecursiveClasses = true;
        Filter.ClassPaths.Add(Class->GetClassPathName());

        TArray<FAssetData> Assets;
        AR.GetAssets(Filter, Assets);
        for (const FAssetData& A : Assets)
        {
            const FString Name = A.AssetName.ToString();
            if (Out.Contains(Name))
            {
                if (OutAmbiguous)
                {
                    OutAmbiguous->Add(Name);
                }
                continue;   // first one wins, deterministically
            }
            Out.Add(Name, A);
        }
    }

    /** Texture basename as the JSON records it: strip folder and extension. */
    FString TextureKeyFromPath(const FString& DiskPath)
    {
        // The recorded paths are absolute and sometimes malformed
        // ("C:/../SomeUser/..."), so only the leaf is trustworthy.
        FString Leaf = FPaths::GetCleanFilename(DiskPath.Replace(TEXT("\\"), TEXT("/")));
        return FPaths::GetBaseFilename(Leaf);
    }

    /**
     * v1.16 — read a TextureParameters value in EITHER shape.
     *
     * Before v1.16 the value was a bare path string. It is now an object
     * carrying colour space, class and a portable relative path. Both shapes
     * are accepted forever: a .json written by an older plugin is still a
     * valid thing to rebuild from, and silently dropping every texture
     * parameter because the shape changed is precisely the failure this
     * function exists to prevent — it looks like "the materials came back
     * untextured", not like "the reader is out of date".
     *
     * Only "path" is read here. "rel" is Blender's business; UE resolves
     * against its own asset registry, for which the absolute path's leaf is
     * the key.
     */
    bool ReadTexturePathValue(const TSharedPtr<FJsonValue>& Value, FString& OutPath)
    {
        if (!Value.IsValid())
        {
            return false;
        }

        // Old shape: "T_Albedo": "C:/.../foo.tga"
        if (Value->TryGetString(OutPath))
        {
            return !OutPath.IsEmpty();
        }

        // New shape: "T_Albedo": { "path": "C:/.../foo.tga", "srgb": true, ... }
        const TSharedPtr<FJsonObject>* TexObj = nullptr;
        if (Value->TryGetObject(TexObj) && TexObj && TexObj->IsValid())
        {
            if ((*TexObj)->TryGetStringField(TEXT("path"), OutPath)
                && !OutPath.IsEmpty())
            {
                return true;
            }
            // A file written by a machine where the texture was portable but
            // the absolute path was elided: fall back to the relative form's
            // leaf, which is all FindTextureByKey needs anyway.
            FString Rel;
            if ((*TexObj)->TryGetStringField(TEXT("rel"), Rel) && !Rel.IsEmpty())
            {
                OutPath = Rel;
                return true;
            }
        }
        return false;
    }

    /** Find a texture by the JSON's filename key.
     *
     *  A source filename can contain characters that are not legal in a UE
     *  asset name -- "TCom_Ground_Grass04_2.5x2.5_4K_albedo.jpg" imports as
     *  "..._2_5x2_5_4K_albedo", because dots separate object paths. Matching
     *  the raw leaf alone would silently miss every one of those, so the
     *  sanitised form is tried as well.
     */
    const FAssetData* FindTextureByKey(
        const TMap<FString, FAssetData>& Textures, const FString& Key)
    {
        if (const FAssetData* Hit = Textures.Find(Key))
        {
            return Hit;
        }
        FString Sanitised = Key;
        Sanitised.ReplaceInline(TEXT("."), TEXT("_"));
        Sanitised.ReplaceInline(TEXT(" "), TEXT("_"));
        return (Sanitised != Key) ? Textures.Find(Sanitised) : nullptr;
    }

    /** One MI entry lifted out of the JSON. */
    struct FRebuildEntry
    {
        FString                 Name;
        FString                 ParentName;
        TMap<FString, double>   Scalars;
        TMap<FString, FString>  Textures;   // param -> texture key
        TMap<FString, FLinearColor> Vectors;
    };
}


void FMapLinkMaterialLibraryExchange::ShowRebuildFromJsonDialog()
{
    const UMapLinkSettings* Settings = UMapLinkSettings::Get();

    // ── Pick the file ────────────────────────────────────────────────────
    FString StartDir = FPaths::ProjectDir();
    if (Settings && !Settings->MaterialInstancesBlend.FilePath.IsEmpty())
    {
        // Defaults next to the library .blend, which is where the exporter
        // writes it.
        StartDir = FPaths::GetPath(Settings->MaterialInstancesBlend.FilePath);
    }

    IDesktopPlatform* DP = FDesktopPlatformModule::Get();
    TArray<FString> PickedFiles;
    if (!DP || !DP->OpenFileDialog(
            FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr),
            TEXT("Rebuild Material Instances from material_instances.json"),
            StartDir, TEXT(""),
            TEXT("MapLink material instances|material_instances.json|"
                 "JSON files (*.json)|*.json"),
            EFileDialogFlags::None, PickedFiles) || PickedFiles.Num() == 0)
    {
        return;
    }
    const FString JsonPath = PickedFiles[0];

    FString Raw;
    if (!FFileHelper::LoadFileToString(Raw, *JsonPath))
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
            LOCTEXT("RebuildReadFail", "Could not read:\n{0}"),
            FText::FromString(JsonPath)));
        return;
    }

    // material_instances.json is a top-level ARRAY. Wrapping it in an object
    // lets the whole parse go through the same object + TryGetArrayField path
    // the manifest reader uses, rather than a second deserialise overload.
    {
        FString Trimmed = Raw;
        Trimmed.TrimStartAndEndInline();
        if (Trimmed.StartsWith(TEXT("{")))
        {
            FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
                LOCTEXT("RebuildIsManifest",
                    "{0}\n\nThis looks like a material library bundle "
                    "manifest, not a material_instances.json.\n\n"
                    "Use Import for that."),
                FText::FromString(FPaths::GetCleanFilename(JsonPath))));
            return;
        }
        Raw = FString(TEXT("{\"entries\":")) + Trimmed + TEXT("}");
    }

    TSharedPtr<FJsonObject> RootJson;
    {
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
        if (!FJsonSerializer::Deserialize(Reader, RootJson) || !RootJson.IsValid())
        {
            FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
                LOCTEXT("RebuildBadJson", "Not valid JSON:\n{0}"),
                FText::FromString(JsonPath)));
            return;
        }
    }

    const TArray<TSharedPtr<FJsonValue>>* EntriesJson = nullptr;
    if (!RootJson->TryGetArrayField(TEXT("entries"), EntriesJson) || !EntriesJson)
    {
        FMessageDialog::Open(EAppMsgType::Ok,
            LOCTEXT("RebuildNoEntries",
                    "That file contains no Material Instance entries."));
        return;
    }

    // ── Lift the entries ─────────────────────────────────────────────────
    TArray<FRebuildEntry> Entries;
    for (const TSharedPtr<FJsonValue>& V : *EntriesJson)
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (!V->TryGetObject(Obj) || !Obj || !Obj->IsValid())
        {
            continue;
        }

        FRebuildEntry E;
        (*Obj)->TryGetStringField(TEXT("MaterialInstance"), E.Name);
        (*Obj)->TryGetStringField(TEXT("MaterialParent"), E.ParentName);
        if (E.Name.IsEmpty() || E.ParentName.IsEmpty())
        {
            continue;
        }

        const TSharedPtr<FJsonObject>* Sub = nullptr;
        if ((*Obj)->TryGetObjectField(TEXT("ScalarParameters"), Sub) && Sub)
        {
            for (const auto& KV : (*Sub)->Values)
            {
                double D = 0.0;
                if (KV.Value.IsValid() && KV.Value->TryGetNumber(D))
                {
                    E.Scalars.Add(KV.Key, D);
                }
            }
        }
        if ((*Obj)->TryGetObjectField(TEXT("TextureParameters"), Sub) && Sub)
        {
            for (const auto& KV : (*Sub)->Values)
            {
                // v1.16 — string OR object. See ReadTexturePathValue.
                FString P;
                if (ReadTexturePathValue(KV.Value, P))
                {
                    E.Textures.Add(KV.Key, TextureKeyFromPath(P));
                }
            }
        }
        if ((*Obj)->TryGetObjectField(TEXT("VectorParameters"), Sub) && Sub)
        {
            for (const auto& KV : (*Sub)->Values)
            {
                const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
                if (KV.Value.IsValid() && KV.Value->TryGetArray(Arr)
                    && Arr && Arr->Num() >= 3)
                {
                    auto At = [Arr](int32 i, double Def) -> double
                    {
                        double D = Def;
                        if (Arr->IsValidIndex(i) && (*Arr)[i].IsValid())
                        {
                            (*Arr)[i]->TryGetNumber(D);
                        }
                        return D;
                    };
                    E.Vectors.Add(KV.Key, FLinearColor(
                        static_cast<float>(At(0, 0.0)),
                        static_cast<float>(At(1, 0.0)),
                        static_cast<float>(At(2, 0.0)),
                        static_cast<float>(At(3, 1.0))));
                }
            }
        }
        Entries.Add(MoveTemp(E));
    }

    if (Entries.Num() == 0)
    {
        FMessageDialog::Open(EAppMsgType::Ok,
            LOCTEXT("RebuildNoUsable",
                    "No usable Material Instance entries in that file."));
        return;
    }

    // ── Index what the project already has ───────────────────────────────
    IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
        "AssetRegistry").Get();

    TMap<FString, FAssetData> Parents, Textures, ExistingMIs;
    TSet<FString> AmbiguousParents, AmbiguousTextures;
    IndexAssetsByName(AR, UMaterialInterface::StaticClass(), Parents,
                      &AmbiguousParents);
    IndexAssetsByName(AR, UTexture::StaticClass(), Textures, &AmbiguousTextures);

    const FString MIContentPath = FMapLinkUtils::GetMaterialInstancesContentPath();
    {
        FARFilter Filter;
        Filter.bRecursivePaths = true;
        Filter.bRecursiveClasses = true;
        Filter.PackagePaths.Add(FName(*MIContentPath));
        Filter.ClassPaths.Add(
            UMaterialInstanceConstant::StaticClass()->GetClassPathName());
        TArray<FAssetData> Found;
        AR.GetAssets(Filter, Found);
        for (const FAssetData& A : Found)
        {
            ExistingMIs.Add(A.AssetName.ToString(), A);
        }
    }

    // ── Rows ─────────────────────────────────────────────────────────────
    // Anything whose parent is missing cannot be built at all, so it is listed
    // unticked with the reason rather than silently dropped.
    TArray<TSharedRef<FMatLibRow>> Rows;
    TMap<FString, const FRebuildEntry*> ByName;
    int32 NoParent = 0;
    for (const FRebuildEntry& E : Entries)
    {
        ByName.Add(E.Name, &E);

        TSharedRef<FMatLibRow> Row = MakeShared<FMatLibRow>();
        Row->DisplayName = E.Name;
        Row->PackageName = MIContentPath / E.Name;

        const bool bHasParent = Parents.Contains(E.ParentName);
        const bool bExists    = ExistingMIs.Contains(E.Name);
        if (!bHasParent)
        {
            Row->bSelected = false;
            Row->SubText = FString::Printf(
                TEXT("  parent '%s' not in this project — cannot rebuild"),
                *E.ParentName);
            ++NoParent;
        }
        else if (bExists)
        {
            Row->SubText = FString::Printf(
                TEXT("  exists — will be rebuilt from file (%s)"), *E.ParentName);
        }
        else
        {
            Row->SubText = FString::Printf(TEXT("  new (%s)"), *E.ParentName);
        }
        Rows.Add(Row);
    }

    TSharedRef<SWidget> Header = SNew(STextBlock)
        .AutoWrapText(true)
        .Text(FText::Format(LOCTEXT("RebuildHeader",
            "{0} entries. Ticked ones are created, or rebuilt in place if they "
            "already exist — every overridden parameter is cleared first, so "
            "the file wins.\n\n"
            "Textures bind to assets already in this project, matched by "
            "filename. Nothing is imported from disk; unmatched parameters "
            "keep the parent's default.{1}"),
            FText::AsNumber(Entries.Num()),
            NoParent > 0
                ? FText::Format(LOCTEXT("RebuildNoParentNote",
                      "\n\n{0} entry(s) name a parent material this project "
                      "does not have. They are unticked and cannot be built."),
                      FText::AsNumber(NoParent))
                : FText::GetEmpty()));

    if (!ShowPickerDialog(
            LOCTEXT("RebuildTitle", "Rebuild Material Instances from JSON"),
            LOCTEXT("RebuildOk", "Rebuild"), Rows, Header))
    {
        return;
    }

    // ── Apply ────────────────────────────────────────────────────────────
    IAssetTools& AssetTools =
        FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

    int32 Created = 0, Rebuilt = 0, Failed = 0;
    int32 ParamsSet = 0, TexUnresolved = 0, ParamUnknown = 0;
    TArray<FString> Problems;

    for (const TSharedRef<FMatLibRow>& Row : Rows)
    {
        if (!Row->bSelected)
        {
            continue;
        }
        const FRebuildEntry* const* Found = ByName.Find(Row->DisplayName);
        if (!Found || !*Found)
        {
            continue;
        }
        const FRebuildEntry& E = **Found;

        const FAssetData* ParentData = Parents.Find(E.ParentName);
        if (!ParentData)
        {
            ++Failed;
            Problems.Add(FString::Printf(TEXT("%s: parent '%s' not found"),
                                         *E.Name, *E.ParentName));
            continue;
        }
        UMaterialInterface* Parent =
            Cast<UMaterialInterface>(ParentData->GetAsset());
        if (!Parent)
        {
            ++Failed;
            Problems.Add(FString::Printf(TEXT("%s: parent '%s' failed to load"),
                                         *E.Name, *E.ParentName));
            continue;
        }

        UMaterialInstanceConstant* MI = nullptr;
        if (const FAssetData* Existing = ExistingMIs.Find(E.Name))
        {
            MI = Cast<UMaterialInstanceConstant>(Existing->GetAsset());
            if (MI)
            {
                // Rebuild means the file is authoritative: drop every existing
                // override so a parameter the file no longer carries does not
                // survive as a stale value.
                MI->ScalarParameterValues.Empty();
                MI->TextureParameterValues.Empty();
                MI->VectorParameterValues.Empty();
                if (MI->Parent != Parent)
                {
                    MI->SetParentEditorOnly(Parent);
                }
                ++Rebuilt;
            }
        }
        else
        {
            UMaterialInstanceConstantFactoryNew* Factory =
                NewObject<UMaterialInstanceConstantFactoryNew>();
            Factory->InitialParent = Parent;
            MI = Cast<UMaterialInstanceConstant>(AssetTools.CreateAsset(
                E.Name, MIContentPath,
                UMaterialInstanceConstant::StaticClass(), Factory));
            if (MI)
            {
                ++Created;
            }
        }

        if (!MI)
        {
            ++Failed;
            Problems.Add(FString::Printf(TEXT("%s: could not create or load"),
                                         *E.Name));
            continue;
        }

        // Parameters are matched against the ones the parent actually exposes,
        // by enumerating them off the instance the same way the exporter does
        // when writing this file. A name in the JSON that the parent no longer
        // has is reported rather than set.
        {
            TArray<FMaterialParameterInfo> Infos;
            TArray<FGuid> Guids;

            MI->GetAllScalarParameterInfo(Infos, Guids);
            TSet<FString> Seen;
            for (const FMaterialParameterInfo& Info : Infos)
            {
                const FString N = Info.Name.ToString();
                if (const double* D = E.Scalars.Find(N))
                {
                    MI->SetScalarParameterValueEditorOnly(
                        Info, static_cast<float>(*D));
                    Seen.Add(N);
                    ++ParamsSet;
                }
            }
            for (const auto& KV : E.Scalars)
            {
                if (!Seen.Contains(KV.Key)) { ++ParamUnknown; }
            }

            Infos.Reset(); Guids.Reset(); Seen.Reset();
            MI->GetAllVectorParameterInfo(Infos, Guids);
            for (const FMaterialParameterInfo& Info : Infos)
            {
                const FString N = Info.Name.ToString();
                if (const FLinearColor* C = E.Vectors.Find(N))
                {
                    MI->SetVectorParameterValueEditorOnly(Info, *C);
                    Seen.Add(N);
                    ++ParamsSet;
                }
            }
            for (const auto& KV : E.Vectors)
            {
                if (!Seen.Contains(KV.Key)) { ++ParamUnknown; }
            }

            Infos.Reset(); Guids.Reset(); Seen.Reset();
            MI->GetAllTextureParameterInfo(Infos, Guids);
            for (const FMaterialParameterInfo& Info : Infos)
            {
                const FString N = Info.Name.ToString();
                const FString* Key = E.Textures.Find(N);
                if (!Key)
                {
                    continue;
                }
                Seen.Add(N);
                const FAssetData* TexData = FindTextureByKey(Textures, *Key);
                UTexture* Tex = TexData
                    ? Cast<UTexture>(TexData->GetAsset()) : nullptr;
                if (Tex)
                {
                    MI->SetTextureParameterValueEditorOnly(Info, Tex);
                    ++ParamsSet;
                }
                else
                {
                    ++TexUnresolved;
                    Problems.Add(FString::Printf(
                        TEXT("%s.%s: no texture named '%s' in this project"),
                        *E.Name, *N, **Key));
                }
            }
            for (const auto& KV : E.Textures)
            {
                if (!Seen.Contains(KV.Key)) { ++ParamUnknown; }
            }
        }

        MI->PostEditChange();
        MI->MarkPackageDirty();
        UEditorAssetLibrary::SaveLoadedAsset(MI, /*bOnlyIfDirty=*/false);
    }

    if (Created == 0 && Rebuilt == 0)
    {
        MapLinkNotify::Success(LOCTEXT("RebuildNothing",
            "Nothing was rebuilt."));
        return;
    }

    // Invisible engine tail, same contract as import: the Blender libraries
    // regenerate into the operation's changelist. Sync is never a button.
    {
        const FString CLDesc = TEXT("[MapLink] Material Instance Rebuild");
        const EMapLinkResult R = FMapLinkMaterialExporter::SyncAfterOp(CLDesc);
        if (R != EMapLinkResult::Success &&
            R != EMapLinkResult::NoMaterialInstancesFound)
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("[MatRebuild] tail sync returned code %d"),
                   static_cast<int32>(R));
        }
    }

    for (const FString& P : Problems)
    {
        UE_LOG(LogMapLink, Warning, TEXT("[MatRebuild] %s"), *P);
    }
    if (AmbiguousParents.Num() > 0 || AmbiguousTextures.Num() > 0)
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("[MatRebuild] %d parent name(s) and %d texture name(s) are "
                    "not unique in this project; the first match was used."),
               AmbiguousParents.Num(), AmbiguousTextures.Num());
    }
    UE_LOG(LogMapLink, Log,
           TEXT("[MatRebuild] Done: %d created, %d rebuilt, %d failed, "
                "%d params set, %d textures unresolved (source: %s)"),
           Created, Rebuilt, Failed, ParamsSet, TexUnresolved, *JsonPath);

    MapLinkNotify::Success(FText::Format(
        LOCTEXT("RebuildDone",
            "Material Instance rebuild finished.\n\n"
            "Created: {0}\nRebuilt: {1}\nFailed: {2}\n\n"
            "Parameters set: {3}\nTextures not found in project: {4}\n"
            "Parameters the parent no longer has: {5}{6}"),
        FText::AsNumber(Created),
        FText::AsNumber(Rebuilt),
        FText::AsNumber(Failed),
        FText::AsNumber(ParamsSet),
        FText::AsNumber(TexUnresolved),
        FText::AsNumber(ParamUnknown),
        Problems.Num() > 0
            ? LOCTEXT("RebuildSeeLog", "\n\nSee the Output Log (LogMapLink).")
            : FText::GetEmpty()));
}

#undef LOCTEXT_NAMESPACE
