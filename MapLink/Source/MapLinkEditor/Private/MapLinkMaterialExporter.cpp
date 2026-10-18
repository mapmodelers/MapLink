// Copyright MapModelers. All Rights Reserved.

#include "MapLinkMaterialExporter.h"
#include "MapLinkSettings.h"
#include "MapLinkBlenderBridge.h"
#include "MapLinkPipeline.h"
#include "MapLinkSourceControl.h"   // v1.9
#include "MapLinkUtils.h"           // v1.10 — content root
#include "MapLinkEditorState.h"     // v1.10 — library-dirty flag
#include "MapLinkTextureNaming.h"   // v1.16 — portable path arithmetic
#include "MapLinkTexturePolicy.h"   // v1.16 — texture class decisions
#include "MapLinkTextureTypes.h"    // v1.16 — class/status enums
#include "MapLinkTgaWriter.h"       // v1.20 — MI thumbnails for Blender
#include "ObjectTools.h"            // v1.20 — ThumbnailTools::CacheThumbnail
#include "Misc/ObjectThumbnail.h"   // v1.20 — FObjectThumbnail
#include "MapLinkMaterialThumbnailScene.h"        // v1.20 — our own scene
#include "Editor.h"                              // v1.20 — GEditor scratch RT
#include "Engine/TextureRenderTarget2D.h"
#include "CanvasTypes.h"
#include "SceneView.h"
#include "ShowFlags.h"
#include "Misc/App.h"              // FApp::CanEverRender
#include "CoreGlobals.h"           // GAreScreenMessagesEnabled
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"       // FlushRenderingCommands
// v1.20 — RenderThumbnailFamily copies UThumbnailRenderer::RenderViewFamily's
// body, so it takes that file's include list with it. GetRendererModule() is a
// free function declared in EngineModule.h, NOT in RendererInterface.h, which
// only declares the IRendererModule interface it returns.
#include "RendererInterface.h"
#include "EngineModule.h"
#include "SceneViewExtension.h"
#include "LegacyScreenPercentageDriver.h"
#include "Engine/Engine.h"
#include "Materials/MaterialInterface.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Engine/Texture2D.h"
#include "EditorFramework/AssetImportData.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlChangelist.h"
#include "SourceControlOperations.h"

#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"        // v1.10 — IFileManager (copy/move/replace)
#include "UObject/SavePackage.h"    // v1.20 — thumbnail stamp saves
#include "Misc/PackageName.h"
#include "Interfaces/IPluginManager.h"

// ───────────────────────────────────────────────────────────────────────
// Changelist helper
// ───────────────────────────────────────────────────────────────────────

namespace
{
    /**
     * Find or create a P4 changelist named "MapLink Material Sync".
     * Reuses an existing pending CL with the same description if one exists.
     * Returns the changelist ref, or nullptr if SCC is disabled / doesn't
     * support changelists.
     */
    FSourceControlChangelistPtr GetOrCreateChangelist()
    {
        // v1.9 — delegates to the shared MapLink SCC helper.
        // Description standardized to "[MapLink] Material Sync"
        // (previously "MapLink Material Sync").
        return FMapLinkSourceControl::GetOrCreateChangelist(
            TEXT("[MapLink] Material Sync"));
    }

    /** Checkout or Add a file into the given changelist (or default if null). */
    void SCCCheckOutOrAdd(const FString& FilePath, FSourceControlChangelistPtr Changelist)
    {
        if (!ISourceControlModule::Get().IsEnabled())
        {
            return;
        }

        ISourceControlProvider& SCC = ISourceControlModule::Get().GetProvider();
        FSourceControlStatePtr FileState = SCC.GetState(FilePath, EStateCacheUsage::ForceUpdate);

        TArray<FString> Files = { FilePath };

        // v1.16 — a file already open needs nothing.
        //
        // A file opened for ADD is source-controlled (pending), so the branch
        // below ran `p4 edit` on it and Perforce answered
        //
        //     can't edit (already opened for add)
        //
        // which this logged as "SCC Checkout → FAILED". It was not a failure;
        // the file was already open in a changelist, which is the entire goal.
        // Reporting a correct state as a failure is how a real failure
        // downstream ends up looking like more of the same noise.
        if (FileState.IsValid() && (FileState->IsAdded() || FileState->IsCheckedOut()))
        {
            return;
        }

        if (FileState.IsValid() && FileState->IsSourceControlled())
        {
            // File is known to P4 → checkout
            TSharedRef<FCheckOut> Op = ISourceControlOperation::Create<FCheckOut>();
            ECommandResult::Type R = Changelist.IsValid()
                ? SCC.Execute(Op, Changelist, Files, EConcurrency::Synchronous)
                : SCC.Execute(Op, Files, EConcurrency::Synchronous);

            UE_LOG(LogMapLink, Log, TEXT("[MatSync] SCC Checkout %s → %s"),
                   *FilePath, R == ECommandResult::Succeeded ? TEXT("OK") : TEXT("FAILED"));
        }
        else
        {
            // New file → mark for add
            TSharedRef<FMarkForAdd> Op = ISourceControlOperation::Create<FMarkForAdd>();
            ECommandResult::Type R = Changelist.IsValid()
                ? SCC.Execute(Op, Changelist, Files, EConcurrency::Synchronous)
                : SCC.Execute(Op, Files, EConcurrency::Synchronous);

            UE_LOG(LogMapLink, Log, TEXT("[MatSync] SCC Add %s → %s"),
                   *FilePath, R == ECommandResult::Succeeded ? TEXT("OK") : TEXT("FAILED"));
        }
    }
}

// ───────────────────────────────────────────────────────────────────────
// Public
// ───────────────────────────────────────────────────────────────────────

EMapLinkResult FMapLinkMaterialExporter::SyncToBlender()
{
    FString JsonPath;
    EMapLinkResult Result = RunExport(JsonPath);
    if (Result != EMapLinkResult::Success)
    {
        return Result;
    }

    return RunBlenderMaterialSync(JsonPath);
}

// ───────────────────────────────────────────────────────────────────────
// Phase A — Scan MIs, write unified JSON
// ───────────────────────────────────────────────────────────────────────

// ---------------------------------------------------------------------------
// v1.20 — Asset Browser thumbnails
// ---------------------------------------------------------------------------

namespace
{
    /** Edge length of the thumbnails written for Blender. 128 is what the
     *  Asset Browser draws at its largest tile size; larger costs disk and
     *  render time for pixels nobody sees. */
    constexpr int32 kMapLinkThumbSize = 128;

    const TCHAR* kMapLinkThumbDir = TEXT("Thumbnails");
}

namespace
{
    /** Render one MI through *Scene* into *OutThumb*.  -> false on any failure.
     *
     *  The body is UMaterialInstanceThumbnailRenderer::Draw's, with our scene
     *  in place of FMaterialThumbnailScene and the render target and readback
     *  from ThumbnailTools::RenderThumbnail. Deliberately not clever: the two
     *  engine functions it is stitched together from are the ones the Content
     *  Browser uses every day, so this picture is produced the same way UE
     *  produces its own.
     */
    /** UThumbnailRenderer::RenderViewFamily's body, because that one is
     *  PROTECTED and we are not a UThumbnailRenderer.
     *
     *  Copied rather than worked around: the screen-percentage driver and the
     *  view-extension pass are not optional decoration. Without the driver the
     *  family renders at whatever the last global resolution fraction was, and
     *  without the extension pass anything that hooks SetupView (which on this
     *  project includes the renderer's own passes) never sees the view.
     *  Verified against Engine/Private/ThumbnailRendering/ThumbnailRenderer.cpp.
     */
    void RenderThumbnailFamily(FCanvas* Canvas,
                               FSceneViewFamily* ViewFamily,
                               FSceneView* View)
    {
        if (!ViewFamily || !View)
        {
            return;
        }

        ViewFamily->EngineShowFlags.ScreenPercentage = false;
        ViewFamily->SetScreenPercentageInterface(
            new FLegacyScreenPercentageDriver(*ViewFamily,
                                              /*GlobalResolutionFraction=*/1.0f));

        if (GEngine && GEngine->ViewExtensions)
        {
            ViewFamily->ViewExtensions =
                GEngine->ViewExtensions->GatherActiveExtensions(
                    FSceneViewExtensionContext(ViewFamily->Scene));

            for (const FSceneViewExtensionRef& Extension : ViewFamily->ViewExtensions)
            {
                Extension->SetupViewFamily(*ViewFamily);
                Extension->SetupView(*ViewFamily, *View);
            }
        }

        GetRendererModule().BeginRenderingViewFamily(Canvas, ViewFamily);
    }

    bool RenderMIThumbnail(FMapLinkMaterialThumbnailScene& Scene,
                           UMaterialInterface* MI,
                           uint32 Size,
                           FObjectThumbnail& OutThumb)
    {
        if (!MI || !GEditor || !GIsRHIInitialized || !FApp::CanEverRender())
        {
            return false;
        }

        UTextureRenderTarget2D* RT = GEditor->GetScratchRenderTarget(Size);
        FTextureRenderTargetResource* RTR =
            RT ? RT->GameThread_GetRenderTargetResource() : nullptr;
        if (!RTR)
        {
            return false;
        }

        OutThumb.SetImageSize(Size, Size);

        // Kill the on-screen messages for the duration of the render.
        //
        // SceneRendering.cpp draws them INTO the rendered view when
        // GAreScreenMessagesEnabled is set, which is why this project's
        // "Nanite is disabled but fallback meshes were stripped…" banner was
        // baked into every thumbnail — ours and the Content Browser's own
        // spheres alike. It is a property of the project's Nanite settings,
        // not of the material, so it has no business in a picture of a
        // material.
        //
        // Scoped, not assigned: this is a global, the editor is still running
        // around us, and a thrown exception between here and the end of the
        // function must not leave the user's viewport warnings switched off.
        TGuardValue<bool> NoScreenMessages(GAreScreenMessagesEnabled, false);

        FCanvas Canvas(RTR, nullptr, FGameTime::GetTimeSinceAppStart(),
                       GMaxRHIFeatureLevel);
        Canvas.Clear(FLinearColor::Black);

        Scene.SetMaterialInterface(MI);
        {
            FSceneViewFamilyContext ViewFamily(
                FSceneViewFamily::ConstructionValues(
                    RTR, Scene.GetScene(), FEngineShowFlags(ESFIM_Game))
                .SetTime(FGameTime::GetTimeSinceAppStart()));

            // ESFIM_Game plus these is what every engine thumbnail renderer
            // uses. It is also why the red "Nanite is disabled…" banner that
            // was baked into the first attempt cannot appear here: that text
            // is editor-viewport canvas furniture, and nothing in this path
            // draws it.
            ViewFamily.EngineShowFlags.DisableAdvancedFeatures();
            ViewFamily.EngineShowFlags.MotionBlur   = 0;
            ViewFamily.EngineShowFlags.AntiAliasing = 0;

            FSceneView* View = Scene.CreateView(&ViewFamily, 0, 0, Size, Size);
            RenderThumbnailFamily(&Canvas, &ViewFamily, View);
        }
        Scene.SetMaterialInterface(nullptr);

        Canvas.Flush_GameThread();
        FlushRenderingCommands();

        TArray<uint8>& Data = OutThumb.AccessImageData();
        Data.Empty();
        Data.AddUninitialized(Size * Size * sizeof(FColor));
        const FIntRect SrcRect(0, 0, Size, Size);
        if (!RTR->ReadPixelsPtr(reinterpret_cast<FColor*>(Data.GetData()),
                                FReadSurfaceDataFlags(), SrcRect))
        {
            Data.Empty();
            return false;
        }
        return true;
    }
}

FString FMapLinkMaterialExporter::WriteMIThumbnail(
    UMaterialInterface* MI,
    const FString& MIDir,
    const FString& ChangelistDescription,
    FMapLinkMaterialThumbnailScene& Scene,
    bool bCacheIntoPackage,
    bool& bOutPackageDirtied)
{
    bOutPackageDirtied = false;
    if (!MI || MIDir.IsEmpty() || !Scene.HasPreviewMesh())
    {
        return FString();
    }

    FObjectThumbnail Thumb;
    if (!RenderMIThumbnail(Scene, MI, kMapLinkThumbSize, Thumb))
    {
        UE_LOG(LogMapLink, Verbose,
               TEXT("[MatSync] Thumbnail render failed for '%s' — skipped."),
               *MI->GetName());
        return FString();
    }

    const TArray<uint8>& Pixels = Thumb.GetUncompressedImageData();
    const int32 W = Thumb.GetImageWidth();
    const int32 H = Thumb.GetImageHeight();
    if (W <= 0 || H <= 0 || Pixels.Num() < W * H * 4)
    {
        UE_LOG(LogMapLink, Verbose,
               TEXT("[MatSync] Thumbnail for '%s' came back empty — skipped."),
               *MI->GetName());
        return FString();
    }

    // FObjectThumbnail holds FColor, i.e. B,G,R,A top-down — exactly what
    // FMapLinkTextureSurface documents. The writer reverses the rows itself
    // because TGA is bottom-up; do not pre-flip here.
    FMapLinkTextureSurface Surface;
    Surface.Width  = W;
    Surface.Height = H;
    Surface.BGRA.Append(Pixels.GetData(), static_cast<int64>(W) * H * 4);

    // A material preview is opaque and the writer emits 24-bit BGR regardless;
    // saying the source had no alpha keeps bDroppedUniformAlpha off the result
    // so a caller reading it is not told about a channel that never existed.
    Surface.bHasAlphaChannel = false;
    Surface.bAlphaUniform    = true;

    const FString ThumbDirAbs = FPaths::ConvertRelativePathToFull(
        MIDir / kMapLinkThumbDir);
    FPlatformFileManager::Get().GetPlatformFile()
        .CreateDirectoryTree(*ThumbDirAbs);

    const FString FileName = MI->GetName() + TEXT(".tga");
    const FString AbsPath  = ThumbDirAbs / FileName;
    const FString RelPath  = FString(kMapLinkThumbDir) / FileName;

    // Open it for edit BEFORE writing, and clear the read-only bit after —
    // same discipline as the JSON write above, and for the same reason it was
    // needed there: a failed checkout leaves a submitted file read-only and
    // the write silently does nothing. Source control decides what is TRACKED;
    // it does not get to decide whether the tool can write its own output.
    const bool bExisted = FPaths::FileExists(AbsPath);
    if (bExisted)
    {
        FMapLinkSourceControl::EnsureFileInChangelist(AbsPath, ChangelistDescription);
        FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*AbsPath, false);
    }

    const FMapLinkTgaWriteResult Result =
        FMapLinkTgaWriter::WriteSurface(Surface, AbsPath, /*bForceGrayscale=*/false);

    if (!Result.bSuccess)
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("[MatSync] Thumbnail for '%s' not written: %s"),
               *MI->GetName(), *Result.Error);
        return FString();
    }

    // The writer skips a byte-identical file, so an unchanged MI performs zero
    // writes and never touches the changelist. Only a genuinely new file needs
    // marking for add.
    if (!bExisted && !Result.bSkippedIdentical)
    {
        FMapLinkSourceControl::EnsureFileInChangelist(AbsPath, ChangelistDescription);
    }

    // The same picture, cached into the .uasset so the Content Browser shows
    // it too. A cached thumbnail is what UE draws for an UNLOADED asset, so
    // without this the browser keeps rendering its own sphere on demand and
    // the two views of the same material disagree.
    //
    // Off during a normal sync: it dirties the package, and a material sync
    // that silently checks out 87 .uassets is not something anyone asked for.
    // The migration pass turns it on deliberately.
    if (bCacheIntoPackage)
    {
        if (UPackage* Package = MI->GetOutermost())
        {
            ThumbnailTools::CacheThumbnail(MI->GetFullName(), &Thumb, Package);
            Package->MarkPackageDirty();
            bOutPackageDirtied = true;
        }
    }

    return RelPath;
}

int32 FMapLinkMaterialExporter::PruneStaleThumbnails(
    const FString& MIDir,
    const TSet<FString>& LiveMINames,
    const FString& ChangelistDescription)
{
    if (MIDir.IsEmpty())
    {
        return 0;
    }

    const FString ThumbDirAbs = FPaths::ConvertRelativePathToFull(
        MIDir / kMapLinkThumbDir);
    if (!FPaths::DirectoryExists(ThumbDirAbs))
    {
        return 0;
    }

    TArray<FString> Found;
    IFileManager::Get().FindFiles(Found, *(ThumbDirAbs / TEXT("*.tga")), true, false);

    int32 Removed = 0;
    for (const FString& File : Found)
    {
        const FString Stem = FPaths::GetBaseFilename(File);
        if (LiveMINames.Contains(Stem))
        {
            continue;
        }
        const FString AbsPath = ThumbDirAbs / File;
        if (FMapLinkSourceControl::DeleteFileTracked(AbsPath, ChangelistDescription))
        {
            ++Removed;
            UE_LOG(LogMapLink, Log,
                   TEXT("[MatSync] Removed thumbnail for deleted MI '%s'."), *Stem);
        }
    }
    return Removed;
}

int32 FMapLinkMaterialExporter::SaveDirtyMIPackages(
    const TArray<FAssetData>& MIAssets,
    const FString& ChangelistDescription)
{
    FMapLinkSourceControl::GetOrCreateChangelist(ChangelistDescription);

    int32 Saved = 0;
    for (const FAssetData& AssetData : MIAssets)
    {
        UObject* Asset = AssetData.FastGetAsset(false);
        UPackage* Package = Asset ? Asset->GetOutermost() : nullptr;
        if (!Package || !Package->IsDirty())
        {
            continue;
        }

        const FString DiskPath = FPackageName::LongPackageNameToFilename(
            Package->GetName(), FPackageName::GetAssetPackageExtension());

        FMapLinkSourceControl::CheckoutBeforeSave(Package);

        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        SaveArgs.Error         = GLog;
        SaveArgs.SaveFlags     = SAVE_NoError;

        if (UPackage::SavePackage(Package, Asset, *DiskPath, SaveArgs))
        {
            FMapLinkSourceControl::AddAfterSave(Package);
            ++Saved;
        }
        else
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("[MatSync] SavePackage failed for '%s' — its Content "
                        "Browser thumbnail keeps the old picture."), *DiskPath);
        }
    }
    return Saved;
}

EMapLinkResult FMapLinkMaterialExporter::StampAssetThumbnails()
{
    FString UnusedJsonPath;
    return RunExport(UnusedJsonPath,
                     TEXT("[MapLink] Material thumbnails"),
                     /*bStampAssetThumbnails=*/true);
}

EMapLinkResult FMapLinkMaterialExporter::RunExport(
    FString& OutJsonPath, const FString& ChangelistDescription,
    bool bStampAssetThumbnails)
{
    const UMapLinkSettings* Settings = UMapLinkSettings::Get();
    const FString ContentPath = Settings->MaterialInstanceContentPath.Path;

    UE_LOG(LogMapLink, Log,
           TEXT("[MatSync] Starting Material Instance export. ContentPath: %s"), *ContentPath);

    // ── 1. Scan Asset Registry for UMaterialInstanceConstant ──────
    FAssetRegistryModule& ARModule =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AR = ARModule.Get();

    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*ContentPath));
    Filter.bRecursivePaths = true;
    Filter.ClassPaths.Add(UMaterialInstanceConstant::StaticClass()->GetClassPathName());

    TArray<FAssetData> MIAssets;
    AR.GetAssets(Filter, MIAssets);

    if (MIAssets.Num() == 0)
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("[MatSync] No Material Instances found in %s"), *ContentPath);
        return EMapLinkResult::NoMaterialInstancesFound;
    }

    UE_LOG(LogMapLink, Log,
           TEXT("[MatSync] Found %d Material Instance(s)."), MIAssets.Num());

    // ── Progress dialog ───────────────────────────────────────────
    FScopedSlowTask SlowTask(
        static_cast<float>(MIAssets.Num()) + 1.f,
        FText::FromString(TEXT("MapLink: Exporting Material Instances…")));
    SlowTask.MakeDialog(/*bShowCancelButton=*/false);

    // ── 1b. The library anchor — v1.16 ────────────────────────────
    //
    // Every relative texture path in this JSON is expressed against the
    // directory the material library .blend FINALLY lives in. This has to be
    // resolved before the loop, because each texture needs it.
    //
    // It must be the FINAL directory, never bpy.data.filepath on the Blender
    // side: AtomicRegenLibrary regenerates into <LibDir>/.maplink_sync_tmp/,
    // one level deeper, then replaces upward. A path relativised against the
    // open temp file would validate during the sync and break the instant the
    // file lands. Computing it here, in UE, against the real destination is
    // the only place that fact is knowable.
    FString LibraryAnchorDir;
    {
        if (Settings && !Settings->MaterialInstancesBlend.FilePath.IsEmpty())
        {
            LibraryAnchorDir = FPaths::GetPath(FPaths::ConvertRelativePathToFull(
                Settings->MaterialInstancesBlend.FilePath));
        }
        if (LibraryAnchorDir.IsEmpty())
        {
            const FString Root =
                UMapLinkSettings::ResolveBlenderProjectRoot(false, false);
            if (!Root.IsEmpty())
            {
                LibraryAnchorDir = Root / TEXT("MaterialInstances");
            }
        }
    }
    FMapLinkTexturePolicy::EnsureLoaded();

    // ── 2. Process each MI ────────────────────────────────────────
    TArray<TSharedPtr<FJsonValue>> AllMIJsonValues;
    TArray<TSharedPtr<FJsonValue>> LogResults;
    TSet<FString> LiveMINames;              // v1.20 — for the thumbnail prune
    int32 ThumbnailCount = 0;
    int32 DirtiedPackages = 0;

    // One scene for the whole run. Building it per MI would rebuild the
    // preview world, its lighting rig and the cube 87 times over.
    FMapLinkMaterialThumbnailScene ThumbScene;
    if (!ThumbScene.HasPreviewMesh())
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("[MatSync] No preview cube — material thumbnails skipped "
                    "for this run."));
    }
    int32 WarningCount = 0;
    int32 PortableCount = 0;
    int32 LocalOnlyCount = 0;

    for (const FAssetData& AssetData : MIAssets)
    {
        SlowTask.EnterProgressFrame(1.f);

        UMaterialInstanceConstant* MI =
            Cast<UMaterialInstanceConstant>(AssetData.GetAsset());
        if (!MI) continue;

        const FString MIName = MI->GetName();
        const FString ParentName = MI->Parent ? MI->Parent->GetName() : TEXT("NONE");
        const FString MIAssetPath = AssetData.GetObjectPathString();

        // --- Build JSON object ---
        TSharedPtr<FJsonObject> MIJson = MakeShared<FJsonObject>();
        MIJson->SetStringField(TEXT("MaterialParent"), ParentName);
        MIJson->SetStringField(TEXT("MaterialInstance"), MIName);
        MIJson->SetStringField(TEXT("UnrealSoftReference"), MIAssetPath);

        // v1.20 — the Asset Browser thumbnail. Optional by construction: the
        // Blender reader accepts its absence forever, and a missing picture
        // must never be the reason a material sync fails.
        LiveMINames.Add(MIName);
        {
            bool bDirtied = false;
            const FString ThumbRel = WriteMIThumbnail(
                MI, LibraryAnchorDir, ChangelistDescription, ThumbScene,
                bStampAssetThumbnails, bDirtied);
            if (bDirtied)
            {
                ++DirtiedPackages;
            }
            if (!ThumbRel.IsEmpty())
            {
                // Stored with forward slashes: this string is read by Python
                // on the other side and joined against the MI directory.
                MIJson->SetStringField(TEXT("thumbnail"),
                                       ThumbRel.Replace(TEXT("\\"), TEXT("/")));
                ++ThumbnailCount;
            }
        }

        // Per-MI log entry
        TSharedPtr<FJsonObject> LogEntry = MakeShared<FJsonObject>();
        LogEntry->SetStringField(TEXT("mi"), MIName);
        LogEntry->SetStringField(TEXT("parent"), ParentName);
        LogEntry->SetStringField(TEXT("asset_path"), MIAssetPath);

        TArray<TSharedPtr<FJsonValue>> TexIssues;

        // ── Scalar parameters ─────────────────────────────────────
        TSharedPtr<FJsonObject> ScalarsJson = MakeShared<FJsonObject>();
        int32 ScalarCount = 0;
        {
            TArray<FMaterialParameterInfo> ParamInfos;
            TArray<FGuid> ParamGuids;
            MI->GetAllScalarParameterInfo(ParamInfos, ParamGuids);

            for (const FMaterialParameterInfo& Info : ParamInfos)
            {
                float Value = 0.f;
                if (MI->GetScalarParameterValue(Info, Value, /*bOveriddenOnly=*/true))
                {
                    ScalarsJson->SetNumberField(Info.Name.ToString(), Value);
                    ++ScalarCount;
                }
            }
        }
        MIJson->SetObjectField(TEXT("ScalarParameters"), ScalarsJson);

        // ── Texture parameters ────────────────────────────────────
        TSharedPtr<FJsonObject> TexturesJson = MakeShared<FJsonObject>();
        int32 TextureCount = 0;
        {
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

                FString DiskPath;
                bool bFileExists = false;

                if (TexValue->AssetImportData)
                {
                    DiskPath = TexValue->AssetImportData->GetFirstFilename();
                    if (!DiskPath.IsEmpty())
                    {
                        DiskPath = FPaths::ConvertRelativePathToFull(DiskPath);
                        bFileExists = FPaths::FileExists(DiskPath);
                    }
                }

                if (DiskPath.IsEmpty())
                {
                    ++WarningCount;
                    TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
                    Issue->SetStringField(TEXT("param"), Info.Name.ToString());
                    Issue->SetStringField(TEXT("issue"), TEXT("source_path_missing"));
                    TexIssues.Add(MakeShared<FJsonValueObject>(Issue));
                    continue;
                }

                if (!bFileExists)
                {
                    ++WarningCount;
                    TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
                    Issue->SetStringField(TEXT("param"), Info.Name.ToString());
                    Issue->SetStringField(TEXT("issue"), TEXT("file_not_found"));
                    TexIssues.Add(MakeShared<FJsonValueObject>(Issue));
                }

                // ── v1.16 — the value is an OBJECT, not a bare path ──────
                //
                // A path alone cannot say what colour space the pixels are in,
                // so Blender guessed from the extension and got TGA wrong: it
                // assumes sRGB, which is right for albedo and wrong for every
                // mask, blend weight and normal map. That guess is the bug
                // this drop exists to fix, and the fix is that UE — which
                // KNOWS, because the flag is on the asset — says so.
                //
                //   path        absolute, the authoring machine's truth
                //   rel         "//"-style relative to the library .blend,
                //               present only when it resolves for everyone
                //   anchor      what rel is relative to, so a reader can
                //               verify rather than assume
                //   srgb        the asset's own SRGB flag
                //   compression the UE compression setting, for diagnosis
                //   class       MapLink / External / Unclassified
                //   status      Conform / Portable / LocalOnly / Broken
                //
                // schema_version rides on every entry rather than the file,
                // because the file's root is a bare array with nowhere to hang
                // a header — and a per-entry version is what a partial reader
                // needs anyway.
                const FString TexAssetPath = TexValue->GetPathName();

                TSharedPtr<FJsonObject> TexJson = MakeShared<FJsonObject>();
                TexJson->SetNumberField(TEXT("schema_version"), 2);
                TexJson->SetStringField(TEXT("path"), DiskPath);

                FString RelPath;
                const bool bPortable = !LibraryAnchorDir.IsEmpty()
                    && FMapLinkTextureNaming::ComputePortablePathFrom(
                           DiskPath, LibraryAnchorDir, RelPath);

                if (bPortable)
                {
                    // "//" is Blender's own marker for "relative to this
                    // .blend". Emitting it here means the Python side assigns
                    // the string verbatim instead of re-deriving it against
                    // whatever file happens to be open.
                    TexJson->SetStringField(TEXT("rel"), TEXT("//") + RelPath);
                    TexJson->SetStringField(TEXT("anchor"), LibraryAnchorDir);
                    ++PortableCount;
                }
                else
                {
                    ++LocalOnlyCount;
                    TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
                    Issue->SetStringField(TEXT("param"), Info.Name.ToString());
                    Issue->SetStringField(TEXT("issue"), TEXT("local_only"));
                    Issue->SetStringField(TEXT("path"), DiskPath);
                    TexIssues.Add(MakeShared<FJsonValueObject>(Issue));
                }

                if (UTexture2D* Tex2D = Cast<UTexture2D>(TexValue))
                {
                    TexJson->SetBoolField(TEXT("srgb"), Tex2D->SRGB != 0);
                    TexJson->SetStringField(TEXT("compression"),
                        MapLinkTexture::CompressionToString(Tex2D->CompressionSettings));
                }
                else
                {
                    // Not a 2D texture (a render target, a cube). SRGB still
                    // exists on UTexture, so the colour-space answer is still
                    // available — only the compression enum is 2D-specific.
                    TexJson->SetBoolField(TEXT("srgb"), TexValue->SRGB != 0);
                }

                const EMapLinkTextureClass TexClass =
                    FMapLinkTexturePolicy::GetClass(TexAssetPath);
                TexJson->SetStringField(TEXT("class"),
                    MapLinkTexture::ToString(TexClass));
                TexJson->SetStringField(TEXT("status"),
                    !bFileExists ? TEXT("Broken")
                                 : (bPortable ? TEXT("Portable") : TEXT("LocalOnly")));
                TexJson->SetStringField(TEXT("asset"), TexAssetPath);

                TexturesJson->SetObjectField(Info.Name.ToString(), TexJson);
                ++TextureCount;
            }
        }
        MIJson->SetObjectField(TEXT("TextureParameters"), TexturesJson);

        // ── Vector parameters ─────────────────────────────────────
        TSharedPtr<FJsonObject> VectorsJson = MakeShared<FJsonObject>();
        int32 VectorCount = 0;
        {
            TArray<FMaterialParameterInfo> ParamInfos;
            TArray<FGuid> ParamGuids;
            MI->GetAllVectorParameterInfo(ParamInfos, ParamGuids);

            for (const FMaterialParameterInfo& Info : ParamInfos)
            {
                FLinearColor Value;
                if (MI->GetVectorParameterValue(Info, Value, /*bOveriddenOnly=*/true))
                {
                    TArray<TSharedPtr<FJsonValue>> RGBA;
                    RGBA.Add(MakeShared<FJsonValueNumber>(Value.R));
                    RGBA.Add(MakeShared<FJsonValueNumber>(Value.G));
                    RGBA.Add(MakeShared<FJsonValueNumber>(Value.B));
                    RGBA.Add(MakeShared<FJsonValueNumber>(Value.A));
                    VectorsJson->SetArrayField(Info.Name.ToString(), RGBA);
                    ++VectorCount;
                }
            }
        }
        MIJson->SetObjectField(TEXT("VectorParameters"), VectorsJson);

        UE_LOG(LogMapLink, Log,
               TEXT("[MatSync] MI: %s | Parent: %s | Scalars: %d | Textures: %d | Vectors: %d"),
               *MIName, *ParentName, ScalarCount, TextureCount, VectorCount);

        LogEntry->SetNumberField(TEXT("scalar_count"), ScalarCount);
        LogEntry->SetNumberField(TEXT("texture_count"), TextureCount);
        LogEntry->SetNumberField(TEXT("vector_count"), VectorCount);
        LogEntry->SetArrayField(TEXT("texture_issues"), TexIssues);
        LogResults.Add(MakeShared<FJsonValueObject>(LogEntry));

        AllMIJsonValues.Add(MakeShared<FJsonValueObject>(MIJson));
    }

    // ── 2b. Thumbnails: drop the ones whose MI is gone ────────────
    //
    // After the loop, because LiveMINames is only complete then. A thumbnail
    // left behind for a deleted MI is not harmful in Blender — nothing reads
    // it — but it accumulates in the depot forever and it is exactly the kind
    // of file nobody ever goes looking for.
    if (!LibraryAnchorDir.IsEmpty())
    {
        const int32 Pruned = PruneStaleThumbnails(
            LibraryAnchorDir, LiveMINames, ChangelistDescription);
        UE_LOG(LogMapLink, Log,
               TEXT("[MatSync] Thumbnails: %d written/current, %d stale "
                    "removed, %d .uasset(s) restamped."),
               ThumbnailCount, Pruned, DirtiedPackages);
    }

    // The migration pass saves what it dirtied. Nothing else in this function
    // touches a .uasset, so this is the only place packages are written, and
    // it only runs when the caller asked for the stamp.
    if (bStampAssetThumbnails && DirtiedPackages > 0)
    {
        const int32 Saved = SaveDirtyMIPackages(MIAssets, ChangelistDescription);
        UE_LOG(LogMapLink, Log,
               TEXT("[MatSync] Asset thumbnails: %d package(s) saved."), Saved);
    }

    // ── 3. Write unified JSON ─────────────────────────────────────
    SlowTask.EnterProgressFrame(1.f, FText::FromString(TEXT("Writing material_instances.json…")));

    // Resolve disk path for the JSON.
    //
    // v1.16 — this lives in the MapLink (Blender) project, not in UE Content.
    // It is consumed by the Blender material sync, so it belongs next to the
    // library .blend it feeds, at <MapLinkProjectRoot>/MaterialInstances/.
    // Putting it under Content made it an asset-adjacent file that UE does not
    // own and Perforce tracked alongside .uassets for no reason.
    //
    // Preferred anchor is the configured Material Instances .blend, because
    // that is already <Root>/MaterialInstances/Library_MaterialInstances.blend
    // and it follows whatever the user pointed the setting at. Falling back to
    // the project root keeps a first run working before the library exists.
    //
    // v1.16 — this is LibraryAnchorDir, resolved before the loop because the
    // relative texture paths needed it. Reusing the same value is not just
    // tidiness: it is the invariant that makes those paths correct. The JSON
    // and the .blend that reads it must sit in the same directory, or every
    // "//" in the file points somewhere else.
    const FString MIDir = LibraryAnchorDir;
    if (MIDir.IsEmpty())
    {
        UE_LOG(LogMapLink, Error,
               TEXT("[MatSync] No MapLink project root and no Material "
                    "Instances .blend configured, so there is nowhere to "
                    "write material_instances.json. Set the project in "
                    "MapLink > Project."));
        return EMapLinkResult::MaterialSyncFailed;
    }

    const FString JsonDiskPath = FPaths::ConvertRelativePathToFull(
        MIDir / TEXT("material_instances.json"));

    FPlatformFileManager::Get().GetPlatformFile()
        .CreateDirectoryTree(*FPaths::GetPath(JsonDiskPath));

    // One-shot pointer at the file the old layout left behind, so a stale copy
    // under Content is never mistaken for the live one.
    {
        FString LegacyRel = ContentPath;
        LegacyRel.RemoveFromStart(TEXT("/Game/"));
        LegacyRel.RemoveFromStart(TEXT("/Game"));
        const FString LegacyPath = FPaths::ConvertRelativePathToFull(
            FPaths::ProjectContentDir() / LegacyRel
            / TEXT("material_instances.json"));
        if (LegacyPath != JsonDiskPath
            && FPaths::FileExists(LegacyPath))
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("[MatSync] A material_instances.json from the old "
                        "layout is still at '%s'. It is no longer read; the "
                        "live file is '%s'. Delete the old one when "
                        "convenient."),
                   *LegacyPath, *JsonDiskPath);
        }
    }

    // Serialise
    FString NewJsonString;
    {
        TSharedRef<TJsonWriter<>> Writer =
            TJsonWriterFactory<>::Create(&NewJsonString);
        FJsonSerializer::Serialize(AllMIJsonValues, Writer);
    }

    // Compare with existing to avoid unnecessary SCC churn
    FString ExistingJsonString;
    bool bExists = FFileHelper::LoadFileToString(ExistingJsonString, *JsonDiskPath);
    bool bChanged = !bExists || (ExistingJsonString != NewJsonString);

    if (bChanged)
    {
        // Create P4 changelist and checkout/add the file.  v1.10 — staged
        // into the op's CL (Resync / SyncAfterOp pass the op description), so
        // the JSON and the regenerated libraries land in ONE changelist.
        FSourceControlChangelistPtr Changelist =
            FMapLinkSourceControl::GetOrCreateChangelist(ChangelistDescription);

        if (bExists)
        {
            SCCCheckOutOrAdd(JsonDiskPath, Changelist);
        }

        // v1.16 — clear the read-only bit before writing.
        //
        // When the file is submitted and the checkout FAILS (it is open in
        // another changelist, the workspace is out of date, the server is
        // unreachable), it stays read-only, SaveStringToFile fails, and the
        // export silently writes nothing. Perforce marks submitted files
        // read-only, so this is not a rare corner: it is what happens every
        // time a checkout does not go through.
        //
        // Same discipline MapLinkTexturePolicy::Save already carries. Source
        // control decides what is TRACKED; it does not get to decide whether
        // the tool can write its own output.
        if (bExists)
        {
            FPlatformFileManager::Get().GetPlatformFile()
                .SetReadOnly(*JsonDiskPath, false);
        }

        // v1.16 — and CHECK it.
        //
        // This return value was ignored, and the log said "JSON UPDATED"
        // unconditionally. A read-only file therefore produced a clean-looking
        // export that wrote nothing, and the library regen below then rebuilt
        // Blender's material library from a JSON that was WEEKS old — which
        // presents as "I conformed a texture and Blender still points at the
        // old path", with no error anywhere to explain it.
        const bool bWrote = FFileHelper::SaveStringToFile(
            NewJsonString, *JsonDiskPath,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

        if (!bWrote)
        {
            UE_LOG(LogMapLink, Error,
                   TEXT("[MatSync] Could NOT write '%s'. The file is most "
                        "likely still read-only because the Perforce checkout "
                        "failed (see the SCC line above). Nothing downstream "
                        "is regenerated, because rebuilding the Blender "
                        "library from the previous JSON would look like it "
                        "worked and would be wrong. Check the file out by "
                        "hand, or clear its read-only flag, and re-run."),
                   *JsonDiskPath);
            return EMapLinkResult::MaterialSyncFailed;
        }

        if (!bExists)
        {
            SCCCheckOutOrAdd(JsonDiskPath, Changelist);
        }

        UE_LOG(LogMapLink, Log, TEXT("[MatSync] JSON %s: %s"),
               bExists ? TEXT("UPDATED") : TEXT("CREATED"), *JsonDiskPath);
    }
    else
    {
        UE_LOG(LogMapLink, Log, TEXT("[MatSync] JSON UNCHANGED: %s"), *JsonDiskPath);
    }

    OutJsonPath = JsonDiskPath;

    UE_LOG(LogMapLink, Log,
           TEXT("[MatSync] Export complete: %d MIs scanned, %d warnings"),
           MIAssets.Num(), WarningCount);

    // ── 4. Write structured export_log.json ───────────────────────
    // v1.16 — the number that matters for portability, stated plainly. A team
    // member opening this library gets exactly PortableCount textures; the
    // LocalOnly ones resolve on this machine and nowhere else.
    if (PortableCount + LocalOnlyCount > 0)
    {
        UE_LOG(LogMapLink, Display,
               TEXT("[MatSync] Texture portability: %d portable, %d local-only "
                    "(anchor: %s)%s"),
               PortableCount, LocalOnlyCount,
               MIDir.IsEmpty() ? TEXT("<unresolved>") : *MIDir,
               LocalOnlyCount > 0
                   ? TEXT(" — run MapLink.Textures.Conform on the local-only "
                          "ones to make them portable.")
                   : TEXT(""));
    }

    WriteExportLog(ContentPath, LogResults, MIAssets.Num(), WarningCount);

    return EMapLinkResult::Success;
}

// ───────────────────────────────────────────────────────────────────────
// Phase B — Invoke Blender
// ───────────────────────────────────────────────────────────────────────

EMapLinkResult FMapLinkMaterialExporter::RunBlenderMaterialSync(const FString& JsonPath)
{
    const UMapLinkSettings* Settings = UMapLinkSettings::Get();

    if (!FPaths::FileExists(Settings->MaterialsLibraryBlend.FilePath))
    {
        UE_LOG(LogMapLink, Error, TEXT("[MatSync] Materials library .blend not found: %s"),
               *Settings->MaterialsLibraryBlend.FilePath);
        return EMapLinkResult::MaterialLibraryNotFound;
    }

    if (!FPaths::FileExists(Settings->MaterialInstancesBlend.FilePath))
    {
        UE_LOG(LogMapLink, Error, TEXT("[MatSync] Material instances .blend not found: %s"),
               *Settings->MaterialInstancesBlend.FilePath);
        return EMapLinkResult::MaterialInstancesBlendNotFound;
    }

    // ── Ensure Library_MaterialInstances.blend is checked out (or marked
    //    for add) in the "MapLink Material Sync" changelist BEFORE Blender
    //    runs. Without this, the file is read-only on disk (P4) and
    //    Blender's save_mainfile() silently fails — leaving the new MI
    //    missing from the .blend and absent from P4.
    {
        const FString BlendDiskPath = FPaths::ConvertRelativePathToFull(
            Settings->MaterialInstancesBlend.FilePath);

        FSourceControlChangelistPtr Changelist = GetOrCreateChangelist();
        SCCCheckOutOrAdd(BlendDiskPath, Changelist);

        // Diagnostic: warn loudly if the file is still read-only after the
        // SCC operation, because Blender's save will fail in that case.
        IPlatformFile& PFCheck = FPlatformFileManager::Get().GetPlatformFile();
        if (PFCheck.IsReadOnly(*BlendDiskPath))
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("[MatSync] %s is still read-only after SCC operation. "
                        "Blender save will fail. Check it out manually in P4V "
                        "and retry."),
                   *BlendDiskPath);
        }
    }

    const FString PluginBaseDir = FMapLinkPipeline::GetPluginBaseDir();

    const FString LogDir = FMapLinkUtils::GetArtifactLogDir() / TEXT("material_sync");
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    PF.CreateDirectoryTree(*LogDir);

    EMapLinkResult Result = FMapLinkBlenderBridge::RunBlenderScriptGeneric(
        Settings->MaterialInstancesBlend.FilePath,
        TEXT("maplink_material_sync.py"),
        PluginBaseDir,
        FString::Printf(
            TEXT("--json \"%s\" --materials-lib \"%s\" --logdir \"%s\" "
                 "--mode execute --materials-lib-rel \"%s\""),
            *JsonPath,
            *Settings->MaterialsLibraryBlend.FilePath,
            *LogDir,
            *MakeLibraryRelativePath(
                FPaths::GetPath(FPaths::ConvertRelativePathToFull(
                    Settings->MaterialInstancesBlend.FilePath)),
                Settings->MaterialsLibraryBlend.FilePath)),
        TEXT("Material Sync"));

    if (Result != EMapLinkResult::Success)
    {
        return EMapLinkResult::MaterialSyncFailed;
    }

    return EMapLinkResult::Success;
}

// ───────────────────────────────────────────────────────────────────────
// Structured export log
// ───────────────────────────────────────────────────────────────────────

void FMapLinkMaterialExporter::WriteExportLog(
    const FString& ContentPath,
    const TArray<TSharedPtr<FJsonValue>>& Results,
    int32 TotalScanned, int32 Warnings)
{
    const FString PluginBaseDir = FMapLinkPipeline::GetPluginBaseDir();
    const FString LogDir = FMapLinkUtils::GetArtifactLogDir() / TEXT("material_sync");
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    PF.CreateDirectoryTree(*LogDir);

    TSharedPtr<FJsonObject> Log = MakeShared<FJsonObject>();
    Log->SetStringField(TEXT("timestamp"), FDateTime::UtcNow().ToIso8601());
    Log->SetStringField(TEXT("content_path"), ContentPath);
    Log->SetNumberField(TEXT("total_scanned"), TotalScanned);
    Log->SetNumberField(TEXT("warnings"), Warnings);
    Log->SetArrayField(TEXT("results"), Results);

    FString LogString;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&LogString);
    FJsonSerializer::Serialize(Log.ToSharedRef(), Writer);

    FFileHelper::SaveStringToFile(LogString,
        *(LogDir / TEXT("export_log.json")),
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

// ═══════════════════════════════════════════════════════════════════════
// v1.10 — Invisible Material Instance Manager auto-sync engine
// ═══════════════════════════════════════════════════════════════════════

bool FMapLinkMaterialExporter::IsLibraryDirty()
{
    return UMapLinkEditorState::Get()->IsMaterialLibraryDirty();
}

void FMapLinkMaterialExporter::MarkLibraryDirty(bool bDirty)
{
    UMapLinkEditorState::Get()->SetMaterialLibraryDirty(bDirty);
}

EMapLinkResult FMapLinkMaterialExporter::Resync()
{
    // Manual recovery escape hatch — full engine under the Material Sync CL.
    return RunSyncEngine(TEXT("[MapLink] Material Sync"));
}

EMapLinkResult FMapLinkMaterialExporter::SyncAfterOp(
    const FString& OpChangelistDescription)
{
    // Invisible tail step for material ops.  Stages the library regen into
    // the op's own CL so an op is one self-contained changelist.
    const FString Desc = OpChangelistDescription.IsEmpty()
        ? FString(TEXT("[MapLink] Material Sync"))
        : OpChangelistDescription;
    return RunSyncEngine(Desc);
}

// ── Sibling library resolution ─────────────────────────────────────────
//
// Invariant 2: two permanent sibling libraries, both regenerated by the same
// sync so they never drift —
//   Library_MaterialInstances.blend      (diffuse-only, default)
//   Library_MaterialInstances_PBR.blend  (full maps)
// The PBR sibling and its channel-scoped regen land in Phase 2 (Texture
// Conform).  Until then we regenerate every sibling that actually exists on
// disk, so the libraries present on this host stay perfectly in sync.

bool FMapLinkMaterialExporter::ResolveLibraryTargets(
    TArray<FLibraryTarget>& OutTargets)
{
    OutTargets.Reset();

    const UMapLinkSettings* Settings = UMapLinkSettings::Get();
    const FString DiffusePath =
        FPaths::ConvertRelativePathToFull(Settings->MaterialInstancesBlend.FilePath);

    if (DiffusePath.IsEmpty() || !FPaths::FileExists(DiffusePath))
    {
        return false;   // diffuse library is mandatory
    }

    OutTargets.Add(FLibraryTarget{ DiffusePath, /*bIsPBR=*/false });

    // Derive the "_PBR" sibling: same directory, "<stem>_PBR.blend".
    const FString Dir  = FPaths::GetPath(DiffusePath);
    const FString Stem = FPaths::GetBaseFilename(DiffusePath);
    const FString PBRPath = Dir / (Stem + TEXT("_PBR.blend"));

    if (FPaths::FileExists(PBRPath))
    {
        OutTargets.Add(FLibraryTarget{ PBRPath, /*bIsPBR=*/true });
    }
    else
    {
        UE_LOG(LogMapLink, Log,
               TEXT("[MatSync] PBR sibling not present yet (%s) — regenerating "
                    "diffuse library only. PBR lands in Phase 2."), *PBRPath);
    }

    return true;
}

// ── Library-relative path arithmetic (v1.19) ───────────────────────────
//
// Measured on a 185-asset production project: 180 of its 900 library links were
// the SAME edge -- Library_MaterialInstances -> Library_Materials -- stored
// absolute, in the one file 182 of the 185 assets depend on. A teammate syncing
// that project got a missing library at the root of its material graph, and no
// Blender-side auto-fix could ever reach it: UE regenerates that file headless
// with the addon unloaded.
//
// It was not an accident either. The regen runs against
// <LibDir>/.maplink_sync_tmp/<Stem>.tmp.blend, and a '//'-relative link
// resolves into THAT directory while the worker is open -- which is exactly
// the "Cannot find lib '...\.maplink_sync_tmp\Library_Materials.blend'" the
// sync log has been printing. Absolute was the workaround.
//
// The fix is the one v1.16 already applied to textures: compute the relative
// form here, against the FINAL directory, and have the worker stamp it just
// before it saves.

FString FMapLinkMaterialExporter::MakeLibraryRelativePath(
    const FString& FinalLibDir, const FString& TargetPath)
{
    if (FinalLibDir.IsEmpty() || TargetPath.IsEmpty())
    {
        return FString();
    }

    FString Dir = FPaths::ConvertRelativePathToFull(FinalLibDir);
    FString Tgt = FPaths::ConvertRelativePathToFull(TargetPath);
    FPaths::NormalizeDirectoryName(Dir);
    FPaths::NormalizeFilename(Tgt);

    // Different volume => no relative form exists. Say so by returning empty;
    // the caller keeps the absolute path rather than emitting something that
    // looks portable and is not.
    if (Dir.Len() >= 2 && Tgt.Len() >= 2
        && FChar::ToUpper(Dir[0]) != FChar::ToUpper(Tgt[0])
        && Dir[1] == TEXT(':') && Tgt[1] == TEXT(':'))
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("[MatSync] '%s' is on a different volume from the library "
                    "directory '%s'; no portable path exists."), *Tgt, *Dir);
        return FString();
    }

    FString Rel = Tgt;
    if (!FPaths::MakePathRelativeTo(Rel, *(Dir / TEXT(""))))
    {
        return FString();
    }

    // Blender's own separator convention for a relative library path.
    Rel.ReplaceInline(TEXT("\\"), TEXT("/"));
    return TEXT("//") + Rel;
}

// ── Headless rebuild against an explicit (temp) target ─────────────────

EMapLinkResult FMapLinkMaterialExporter::RunBlenderMaterialSyncToTarget(
    const FString& TargetBlendPath,
    const FString& JsonPath,
    const FString& MaterialsLibPath,
    const FString& LogDir,
    const FString& MaterialsLibRelPath)
{
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    PF.CreateDirectoryTree(*LogDir);

    const FString PluginBaseDir = FMapLinkPipeline::GetPluginBaseDir();

    // The target here is an UNTRACKED temp copy on the same volume as the real
    // library, so there is no SCC checkout on it (that happens on the REAL
    // file just before the atomic replace).  Routes through the v1.9.4 bridge,
    // which already enforces the watchdog timeout + cooperative cancel.
    FString Args = FString::Printf(
        TEXT("--json \"%s\" --materials-lib \"%s\" --logdir \"%s\" --mode execute"),
        *JsonPath,
        *MaterialsLibPath,
        *LogDir);
    if (!MaterialsLibRelPath.IsEmpty())
    {
        Args += FString::Printf(TEXT(" --materials-lib-rel \"%s\""),
                                *MaterialsLibRelPath);
    }

    return FMapLinkBlenderBridge::RunBlenderScriptGeneric(
        TargetBlendPath,
        TEXT("maplink_material_sync.py"),
        PluginBaseDir,
        Args,
        TEXT("Material Sync"));
}

// ── Atomic regeneration of a single library ─────────────────────────────

EMapLinkResult FMapLinkMaterialExporter::AtomicRegenLibrary(
    const FLibraryTarget& Target,
    const FString& JsonPath,
    const FString& MaterialsLibPath,
    const FString& ChangelistDescription,
    FString& OutError)
{
    IPlatformFile& PF  = FPlatformFileManager::Get().GetPlatformFile();
    IFileManager&  FM  = IFileManager::Get();

    const FString RealPath = Target.BlendPath;
    const FString Stem     = FPaths::GetBaseFilename(RealPath);

    // ── 1. Temp on the SAME volume ─────────────────────────────────────
    // Phase-1 risk: ReplaceFile/MoveFileEx is only atomic within one volume.
    // Putting the temp in a subfolder OF the library's own directory
    // guarantees same-volume (same drive) regardless of where C:\ temp is.
    const FString LibDir   = FPaths::GetPath(RealPath);
    const FString TempDir  = LibDir / TEXT(".maplink_sync_tmp");
    const FString TempBlend = TempDir / (Stem + TEXT(".tmp.blend"));
    const FString LogDir   = FMapLinkUtils::GetArtifactLogDir()
                             / TEXT("material_sync") / Stem;

    PF.CreateDirectoryTree(*TempDir);

    // Clear any stale temp from a previous aborted run.
    if (PF.FileExists(*TempBlend))
    {
        PF.SetReadOnly(*TempBlend, false);
        FM.Delete(*TempBlend, /*RequireExists=*/false, /*EvenReadOnly=*/true);
    }

    // ── 2. Seed the temp with the real library's content ───────────────
    // Blender rebuilds the MI collection IN the file it opens, so the temp
    // must start as a full copy of the real library (linked parents, the
    // mat_cube template, the Library_Material_Instances collection, etc.).
    if (FM.Copy(*TempBlend, *RealPath, /*Replace=*/true,
                /*EvenIfReadOnly=*/true, /*Attributes=*/false) != COPY_OK)
    {
        OutError = FString::Printf(TEXT("Failed to copy '%s' → temp '%s'."),
                                   *RealPath, *TempBlend);
        return EMapLinkResult::MaterialSyncFailed;
    }
    PF.SetReadOnly(*TempBlend, false);   // temp must be writable for Blender save

    // ── 3. Regenerate INTO the temp (headless, watchdog-guarded) ───────
    // LibDir, not TempDir: the worker is open one level deeper, but the file
    // it produces lands here, and this is the position the stored path has to
    // describe.
    const FString MaterialsLibRel =
        MakeLibraryRelativePath(LibDir, MaterialsLibPath);
    if (MaterialsLibRel.IsEmpty())
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("[MatSync] No portable path from '%s' to '%s'; the "
                    "absolute link stays and this library will not resolve "
                    "on a teammate's machine."), *LibDir, *MaterialsLibPath);
    }

    const EMapLinkResult RegenResult = RunBlenderMaterialSyncToTarget(
        TempBlend, JsonPath, MaterialsLibPath, LogDir, MaterialsLibRel);

    if (RegenResult == EMapLinkResult::Cancelled)
    {
        FM.Delete(*TempBlend, false, true);
        OutError = TEXT("Cancelled by user.");
        return EMapLinkResult::Cancelled;   // distinct: not a dirty-state failure
    }

    if (RegenResult != EMapLinkResult::Success)
    {
        FM.Delete(*TempBlend, false, true);
        OutError = FString::Printf(
            TEXT("Headless regen failed for '%s' (code %d)."),
            *Stem, static_cast<int32>(RegenResult));
        return EMapLinkResult::MaterialSyncFailed;
    }

    // ── 4. Validate the temp opened and saved ──────────────────────────
    // The worker only reaches a clean exit by opening the .blend and saving
    // it (it pre-flights writability and exits non-zero otherwise), so a
    // Success return already proves "it opens".  We additionally gate on a
    // sane file size and the worker's own save_failed flag.
    const int64 TempSize = FM.FileSize(*TempBlend);
    if (TempSize < 1024)
    {
        FM.Delete(*TempBlend, false, true);
        OutError = FString::Printf(
            TEXT("Regenerated temp '%s' is missing or too small (%lld bytes)."),
            *TempBlend, TempSize);
        return EMapLinkResult::MaterialSyncFailed;
    }

    {
        FString ReportStr;
        const FString ReportPath = LogDir / TEXT("material_sync_report.json");
        if (FFileHelper::LoadFileToString(ReportStr, *ReportPath))
        {
            TSharedPtr<FJsonObject> Report;
            TSharedRef<TJsonReader<>> Reader =
                TJsonReaderFactory<>::Create(ReportStr);
            if (FJsonSerializer::Deserialize(Reader, Report) && Report.IsValid())
            {
                bool bSaveFailed = false;
                if (Report->TryGetBoolField(TEXT("save_failed"), bSaveFailed)
                    && bSaveFailed)
                {
                    FM.Delete(*TempBlend, false, true);
                    OutError = FString::Printf(
                        TEXT("Worker reported save_failed for '%s'."), *Stem);
                    return EMapLinkResult::MaterialSyncFailed;
                }
            }
        }
    }

    // ── 5. Stage the REAL file into the op's changelist ────────────────
    // Check the real library out (P4 makes tracked files read-only) BEFORE we
    // overwrite it, so the file is writable and the change is captured in the
    // CL.  Degrades to a clean no-op when no SCC provider is enabled.
    {
        FSourceControlChangelistPtr Changelist =
            FMapLinkSourceControl::GetOrCreateChangelist(ChangelistDescription);
        SCCCheckOutOrAdd(RealPath, Changelist);
    }
    PF.SetReadOnly(*RealPath, false);   // belt-and-suspenders ahead of replace

    // ── 6. Atomic replace: real ← temp ─────────────────────────────────
    // Same-volume Move uses MoveFileEx(REPLACE_EXISTING) → atomic swap.  The
    // real library is never in a half-written state: it is either the old
    // file (on any earlier failure) or the fully-validated new file.
    if (!FM.Move(*RealPath, *TempBlend, /*Replace=*/true,
                 /*EvenIfReadOnly=*/true, /*Attributes=*/false,
                 /*bDoNotRetryOrError=*/false))
    {
        // Real file is untouched; clean up the temp and report.
        FM.Delete(*TempBlend, false, true);
        OutError = FString::Printf(
            TEXT("Atomic replace failed for '%s' (temp left at '%s' was removed)."),
            *RealPath, *TempBlend);
        return EMapLinkResult::MaterialSyncFailed;
    }

    // Best-effort scratch cleanup (Move already consumed TempBlend).
    FM.DeleteDirectory(*TempDir, /*RequireExists=*/false, /*Tree=*/true);

    UE_LOG(LogMapLink, Log,
           TEXT("[MatSync] Atomic regen OK: %s (%s)"),
           *Stem, Target.bIsPBR ? TEXT("PBR") : TEXT("diffuse"));
    return EMapLinkResult::Success;
}

// ── Engine entry point ──────────────────────────────────────────────────

EMapLinkResult FMapLinkMaterialExporter::RunSyncEngine(
    const FString& ChangelistDescription)
{
    // ── 1. Scan UE MI state → unified JSON (staged into the op CL) ──────
    FString JsonPath;
    const EMapLinkResult ExportResult = RunExport(JsonPath, ChangelistDescription);

    if (ExportResult == EMapLinkResult::NoMaterialInstancesFound)
    {
        // Nothing to sync.  Treat as success and clear any dirty flag — we do
        // NOT wipe existing library content on an empty/un-indexed scan (that
        // edge — shrinking to zero MIs — is handled when the ops drop lands).
        MarkLibraryDirty(false);
        UE_LOG(LogMapLink, Log, TEXT("[MatSync] No Material Instances — nothing to sync."));
        return EMapLinkResult::Success;
    }
    if (ExportResult != EMapLinkResult::Success)
    {
        return ExportResult;   // export failed before any library was touched
    }

    // ── 2. Resolve inputs ──────────────────────────────────────────────
    const UMapLinkSettings* Settings = UMapLinkSettings::Get();
    const FString MaterialsLibPath =
        FPaths::ConvertRelativePathToFull(Settings->MaterialsLibraryBlend.FilePath);
    if (MaterialsLibPath.IsEmpty() || !FPaths::FileExists(MaterialsLibPath))
    {
        UE_LOG(LogMapLink, Error,
               TEXT("[MatSync] Materials library .blend not found: %s"),
               *MaterialsLibPath);
        MarkLibraryDirty(true);
        return EMapLinkResult::MaterialLibraryNotFound;
    }

    TArray<FLibraryTarget> Targets;
    if (!ResolveLibraryTargets(Targets))
    {
        UE_LOG(LogMapLink, Error,
               TEXT("[MatSync] Material Instances .blend not found / unconfigured."));
        MarkLibraryDirty(true);
        return EMapLinkResult::MaterialInstancesBlendNotFound;
    }

    // ── 3. Atomic regen of every sibling library, one batched op ────────
    FScopedSlowTask SlowTask(
        static_cast<float>(Targets.Num()),
        FText::FromString(TEXT("MapLink: Syncing material libraries…")));
    SlowTask.MakeDialog(/*bShowCancelButton=*/false);

    bool bAnyFailed = false;
    FString FirstError;

    for (const FLibraryTarget& Target : Targets)
    {
        SlowTask.EnterProgressFrame(1.f);

        FString Error;
        const EMapLinkResult R = AtomicRegenLibrary(
            Target, JsonPath, MaterialsLibPath, ChangelistDescription, Error);

        if (R == EMapLinkResult::Cancelled)
        {
            // User abort: leave libraries as-is, flag dirty so the state is
            // honest, and stop (do not keep launching Blender).
            MarkLibraryDirty(true);
            UE_LOG(LogMapLink, Warning, TEXT("[MatSync] Sync cancelled: %s"), *Error);
            return EMapLinkResult::Cancelled;
        }
        if (R != EMapLinkResult::Success)
        {
            bAnyFailed = true;
            if (FirstError.IsEmpty()) FirstError = Error;
            UE_LOG(LogMapLink, Error, TEXT("[MatSync] %s"), *Error);
            // Keep going: regenerate whatever siblings we still can, but the
            // overall op is a failure and the libraries are marked dirty.
        }
    }

    // ── 4. Outcome (UE work is already committed by the caller) ─────────
    if (bAnyFailed)
    {
        MarkLibraryDirty(true);
        UE_LOG(LogMapLink, Error,
               TEXT("[MatSync] Library sync FAILED — marked out of sync. "
                    "UE material state is committed; recover via Settings → "
                    "Resync Material Library. First error: %s"), *FirstError);
        return EMapLinkResult::MaterialSyncFailed;
    }

    MarkLibraryDirty(false);
    UE_LOG(LogMapLink, Log,
           TEXT("[MatSync] Library sync complete — %d sibling library(ies) in sync."),
           Targets.Num());
    return EMapLinkResult::Success;
}
