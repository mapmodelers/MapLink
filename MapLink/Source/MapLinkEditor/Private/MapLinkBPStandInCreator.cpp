// Copyright MapModelers. All Rights Reserved.

#include "MapLinkBPStandInCreator.h"
#include "MapLinkSourceControl.h"   // v1.9

#include "MapLinkBPBinding.h"
#include "MapLinkBlenderBridge.h"
#include "MapLinkSettings.h"
#include "MapLinkStandInReport.h"
#include "MapLinkUtils.h"
#include "MapLinkPipeline.h"  // LogMapLink

#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "Interfaces/IPluginManager.h"

#include "AssetExportTask.h"
#include "Exporters/Exporter.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "ObjectTools.h"           // v1.8 D7 — ObjectTools::DeleteAssets

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlChangelist.h"
#include "SourceControlOperations.h"

#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/MessageDialog.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"

#define LOCTEXT_NAMESPACE "MapLinkBPStandInCreator"

// ===========================================================================
// Anonymous namespace helpers
// ===========================================================================

namespace
{
    constexpr const TCHAR* CL_DESC_CREATE = TEXT("[MapLink] Stand-in Creation");
    constexpr const TCHAR* CL_DESC_DELETE = TEXT("[MapLink] Stand-in Cleanup");
    constexpr const TCHAR* MAPLINK_VERSION = TEXT("1.8");

    // -----------------------------------------------------------------
    // Path / project helpers
    // -----------------------------------------------------------------

    FString GetProjectTempDir()
    {
        return FPaths::ProjectSavedDir() / TEXT("MapLink") / TEXT("Temp");
    }

    FString GetReportPathForCreate(const FString& BPName)
    {
        const FString LogDir =
            FMapLinkUtils::GetArtifactLogDir() / TEXT("bp_standin");
        FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*LogDir);
        const FString Stamp =
            FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S"));
        return LogDir / FString::Printf(
            TEXT("%s_blender_%s.json"), *Stamp, *BPName);
    }

    bool IsInPluginMapLinkContent(const UBlueprint* BP)
    {
        if (!BP) return true;
        const FString Path = BP->GetPathName();
        return Path.StartsWith(TEXT("/MapLink/"));
    }

    // -----------------------------------------------------------------
    // SCC helpers
    // -----------------------------------------------------------------

    FSourceControlChangelistPtr GetOrCreateChangelist(const FString& Description)
    {
        // v1.9 — delegates to the shared MapLink SCC helper
        // (find-or-create pending CL by exact description, cached).
        return FMapLinkSourceControl::GetOrCreateChangelist(Description);
    }

    /** Add a file to a CL.  Returns true on success.  If SCC is off or
     *  fails, returns true anyway (the file write is what matters). */
    bool SCCMarkForAdd(const FString& FilePath,
                      FSourceControlChangelistPtr Changelist)
    {
        if (!ISourceControlModule::Get().IsEnabled())
        {
            return true;
        }

        ISourceControlProvider& SCC = ISourceControlModule::Get().GetProvider();
        FSourceControlStatePtr State =
            SCC.GetState(FilePath, EStateCacheUsage::ForceUpdate);
        TArray<FString> Files = { FilePath };

        if (State.IsValid() && State->IsSourceControlled())
        {
            // Already in P4 — just check it out
            TSharedRef<FCheckOut> Op =
                ISourceControlOperation::Create<FCheckOut>();
            ECommandResult::Type R = Changelist.IsValid()
                ? SCC.Execute(Op, Changelist, Files, EConcurrency::Synchronous)
                : SCC.Execute(Op, Files, EConcurrency::Synchronous);
            UE_LOG(LogMapLinkBPStandIn, Log,
                   TEXT("SCC checkout %s → %s"),
                   *FilePath, R == ECommandResult::Succeeded ? TEXT("OK") : TEXT("FAILED"));
            return R == ECommandResult::Succeeded;
        }

        // Not in P4 — mark for add
        TSharedRef<FMarkForAdd> Op =
            ISourceControlOperation::Create<FMarkForAdd>();
        ECommandResult::Type R = Changelist.IsValid()
            ? SCC.Execute(Op, Changelist, Files, EConcurrency::Synchronous)
            : SCC.Execute(Op, Files, EConcurrency::Synchronous);
        UE_LOG(LogMapLinkBPStandIn, Log,
               TEXT("SCC add %s → %s"),
               *FilePath, R == ECommandResult::Succeeded ? TEXT("OK") : TEXT("FAILED"));
        return R == ECommandResult::Succeeded;
    }

    bool SCCMarkForDelete(const FString& FilePath,
                         FSourceControlChangelistPtr Changelist)
    {
        if (!ISourceControlModule::Get().IsEnabled())
        {
            // Plain filesystem delete fallback (last resort)
            return FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*FilePath);
        }

        ISourceControlProvider& SCC = ISourceControlModule::Get().GetProvider();
        FSourceControlStatePtr State =
            SCC.GetState(FilePath, EStateCacheUsage::ForceUpdate);
        TArray<FString> Files = { FilePath };

        // v1.8 D7 hotfix1 — Handle "marked for add" files:
        // p4 delete on a file that's marked-for-add (never submitted)
        // is a no-op AND returns failure.  The correct op is p4 revert,
        // which undoes the add and removes the local file as a side
        // effect.  After revert, we also do a defensive filesystem
        // delete in case the local file is still on disk.
        if (State.IsValid() && State->IsAdded())
        {
            TSharedRef<FRevert> RevertOp =
                ISourceControlOperation::Create<FRevert>();
            ECommandResult::Type R =
                SCC.Execute(RevertOp, Files, EConcurrency::Synchronous);
            // Whether revert succeeded or not, defensively remove the
            // local file — for a marked-for-add asset there's no depot
            // copy to lose.
            if (FPaths::FileExists(FilePath))
            {
                FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*FilePath);
            }
            UE_LOG(LogMapLinkBPStandIn, Log,
                   TEXT("  p4 revert (was 'marked for add'): %s — local file removed."),
                   *FilePath);
            return R == ECommandResult::Succeeded ||
                   !FPaths::FileExists(FilePath);
        }

        // Already marked-for-delete in some other CL — accept as ok
        if (State.IsValid() && State->IsDeleted())
        {
            UE_LOG(LogMapLinkBPStandIn, Log,
                   TEXT("  p4: %s already marked for delete."), *FilePath);
            return true;
        }

        if (State.IsValid() && State->IsSourceControlled())
        {
            // If file is checked-out for edit, revert that first so
            // FDelete doesn't refuse.
            if (State->IsCheckedOut())
            {
                TSharedRef<FRevert> RevertOp =
                    ISourceControlOperation::Create<FRevert>();
                SCC.Execute(RevertOp, Files, EConcurrency::Synchronous);
            }
            TSharedRef<FDelete> Op =
                ISourceControlOperation::Create<FDelete>();
            ECommandResult::Type R = Changelist.IsValid()
                ? SCC.Execute(Op, Changelist, Files, EConcurrency::Synchronous)
                : SCC.Execute(Op, Files, EConcurrency::Synchronous);
            if (R == ECommandResult::Succeeded)
            {
                UE_LOG(LogMapLinkBPStandIn, Log,
                       TEXT("  p4 delete (submitted file): %s"), *FilePath);
                return true;
            }
            UE_LOG(LogMapLinkBPStandIn, Warning,
                   TEXT("  p4 delete FAILED for %s — falling back to "
                        "plain filesystem delete."), *FilePath);
            // Fall through to plain delete
        }

        // Not in P4 (or fallback) — plain delete
        const bool bDeleted = FPlatformFileManager::Get()
            .GetPlatformFile().DeleteFile(*FilePath);
        if (bDeleted)
        {
            UE_LOG(LogMapLinkBPStandIn, Log,
                   TEXT("  Plain filesystem delete: %s"), *FilePath);
        }
        else if (FPaths::FileExists(FilePath))
        {
            UE_LOG(LogMapLinkBPStandIn, Warning,
                   TEXT("  Failed to delete file: %s"), *FilePath);
        }
        // If the file doesn't exist at all, that's success (target state reached).
        return bDeleted || !FPaths::FileExists(FilePath);
    }

    // -----------------------------------------------------------------
    // SCS walk helpers
    // -----------------------------------------------------------------

    /** USCS_Node has no GetSCSParent() in UE 5.7 — only the downward
     *  ChildNodes array.  Build a child→parent lookup once per SCS so
     *  we can climb the hierarchy upward when composing transforms. */
    TMap<USCS_Node*, USCS_Node*> BuildSCSParentMap(
        USimpleConstructionScript* SCS)
    {
        TMap<USCS_Node*, USCS_Node*> Map;
        if (!SCS) return Map;
        const TArray<USCS_Node*> AllNodes = SCS->GetAllNodes();
        for (USCS_Node* Parent : AllNodes)
        {
            if (!Parent) continue;
            for (USCS_Node* Child : Parent->GetChildNodes())
            {
                if (Child) Map.Add(Child, Parent);
            }
        }
        return Map;
    }

    /** Compose the cumulative relative transform of an SCS node by
     *  walking up its SCS parent chain.  UE convention: 
     *     T_world = T_p1 * T_p2 * ... * T_pn * T_node
     *  When climbing up from node, prepend each parent: 
     *     accum = parent_local * accum. */
    FTransform ComputeCumulativeRelativeTransform(
        USCS_Node* Node,
        const TMap<USCS_Node*, USCS_Node*>& ParentMap)
    {
        USceneComponent* Self = Cast<USceneComponent>(Node->ComponentTemplate);
        FTransform Accum = Self
            ? FTransform(Self->GetRelativeRotation(),
                         Self->GetRelativeLocation(),
                         Self->GetRelativeScale3D())
            : FTransform::Identity;

        USCS_Node* Cursor = ParentMap.FindRef(Node);
        while (Cursor)
        {
            USceneComponent* ParentTpl =
                Cast<USceneComponent>(Cursor->ComponentTemplate);
            if (ParentTpl)
            {
                const FTransform ParentLocal(
                    ParentTpl->GetRelativeRotation(),
                    ParentTpl->GetRelativeLocation(),
                    ParentTpl->GetRelativeScale3D());
                Accum = ParentLocal * Accum;
            }
            Cursor = ParentMap.FindRef(Cursor);
        }
        return Accum;
    }

    /** Walk SCS nodes from this BP and all parent BPs.  Returns flat
     *  list of FMapLinkBPStandInComponent (one per UStaticMeshComponent). */
    void WalkSCSCollectComponents(
        UBlueprint* BP,
        TArray<FMapLinkBPStandInComponent>& OutComponents)
    {
        UBlueprint* Cursor = BP;
        int32 SafetyDepth = 0;
        while (Cursor && SafetyDepth++ < 32)
        {
            if (Cursor->SimpleConstructionScript)
            {
                // Build parent map for THIS BP's SCS — UE 5.7 only
                // exposes downward ChildNodes, so we invert.
                const TMap<USCS_Node*, USCS_Node*> ParentMap =
                    BuildSCSParentMap(Cursor->SimpleConstructionScript);

                TArray<USCS_Node*> Nodes =
                    Cursor->SimpleConstructionScript->GetAllNodes();
                for (USCS_Node* Node : Nodes)
                {
                    if (!Node || !Node->ComponentTemplate) continue;

                    UStaticMeshComponent* SMC =
                        Cast<UStaticMeshComponent>(Node->ComponentTemplate);
                    if (!SMC) continue;

                    UStaticMesh* Mesh = SMC->GetStaticMesh();
                    if (!Mesh) continue;

                    FMapLinkBPStandInComponent Comp;
                    Comp.ObjectName = Node->GetVariableName().ToString();
                    if (Comp.ObjectName.IsEmpty())
                    {
                        Comp.ObjectName = FString::Printf(
                            TEXT("Component_%d"), OutComponents.Num());
                    }
                    Comp.StaticMesh = Mesh;
                    Comp.RelativeTransform =
                        ComputeCumulativeRelativeTransform(Node, ParentMap);
                    Comp.MeshUAssetPath = Mesh->GetPathName();
                    OutComponents.Add(MoveTemp(Comp));
                }
            }

            // Climb to parent BP (inherited components)
            UClass* ParentClass = Cursor->ParentClass;
            if (!ParentClass) break;
            UBlueprint* ParentBP =
                Cast<UBlueprint>(ParentClass->ClassGeneratedBy);
            if (!ParentBP || ParentBP == Cursor) break;
            Cursor = ParentBP;
        }
    }

    /** Estimate a bounds fallback extent (in cm) from the CDO. */
    FVector ComputeBoundsFallbackExtent(UBlueprint* BP)
    {
        const FVector Default(200.f, 200.f, 200.f);
        if (!BP || !BP->GeneratedClass) return Default;

        AActor* CDO = BP->GeneratedClass->GetDefaultObject<AActor>();
        if (!CDO) return Default;

        FVector Origin, Extent;
        CDO->GetActorBounds(/*bOnlyCollidingComponents=*/false, Origin, Extent,
                            /*bIncludeFromChildActors=*/true);
        // GetActorBounds returns BoxExtent (half-size).  We want full extent.
        FVector Full = Extent * 2.0f;
        if (Full.IsNearlyZero(1.0f))
        {
            return Default;
        }
        return Full;
    }

    // -----------------------------------------------------------------
    // FBX export (per UStaticMesh)
    // -----------------------------------------------------------------

    /** Export a UStaticMesh to FBX.  Returns true on success. */
    bool ExportStaticMeshToFBX(UStaticMesh* Mesh, const FString& OutFbxPath)
    {
        if (!Mesh) return false;

        UExporter* FoundExporter =
            UExporter::FindExporter(Mesh, TEXT("fbx"));
        if (!FoundExporter)
        {
            UE_LOG(LogMapLinkBPStandIn, Error,
                   TEXT("No FBX exporter registered for UStaticMesh."));
            return false;
        }

        UAssetExportTask* Task = NewObject<UAssetExportTask>();
        Task->Object           = Mesh;
        Task->Exporter         = FoundExporter;
        Task->Filename         = OutFbxPath;
        Task->bSelected        = false;
        Task->bReplaceIdentical = true;
        Task->bPrompt          = false;
        Task->bUseFileArchive  = false;
        Task->bWriteEmptyFiles = false;
        Task->bAutomated       = true;
        Task->Options          = nullptr;

        const bool bOk = UExporter::RunAssetExportTask(Task);
        if (!bOk)
        {
            UE_LOG(LogMapLinkBPStandIn, Error,
                   TEXT("FBX export FAILED for '%s' → '%s'"),
                   *Mesh->GetName(), *OutFbxPath);
            return false;
        }
        if (!FPaths::FileExists(OutFbxPath))
        {
            UE_LOG(LogMapLinkBPStandIn, Error,
                   TEXT("FBX export reported success but file missing: '%s'"),
                   *OutFbxPath);
            return false;
        }
        return true;
    }

    // -----------------------------------------------------------------
    // Spec JSON writer
    // -----------------------------------------------------------------

    /** Add a [x,y,z] number array to a JSON object. */
    void AddVectorField(const TSharedRef<FJsonObject>& Obj,
                        const FString& Name, const FVector& V)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Add(MakeShared<FJsonValueNumber>(V.X));
        Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
        Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
        Obj->SetArrayField(Name, Arr);
    }

    /** Write spec JSON for the headless Blender script. */
    bool WriteSpecJson(
        const FString& OutPath,
        const FString& BPPath,
        const FString& BPName,
        const FString& BPClassName,
        const FString& TargetBlend,
        const FString& TemplateBlend,
        EMapLinkBPStandInMode Mode,
        const FVector& BoundsExtent,
        const TArray<FMapLinkBPStandInComponent>& Components,
        const TMap<UStaticMesh*, FString>& MeshFbxPaths,
        const FString& ReportPath)
    {
        TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetStringField(TEXT("blueprint_path"), BPPath);
        Root->SetStringField(TEXT("blueprint_name"), BPName);
        Root->SetStringField(TEXT("blueprint_class"), BPClassName);
        Root->SetStringField(TEXT("target_blend"), TargetBlend);
        Root->SetStringField(TEXT("template_blend"), TemplateBlend);

        const TCHAR* ModeStr = TEXT("marker");
        switch (Mode)
        {
            case EMapLinkBPStandInMode::Extracted: ModeStr = TEXT("extracted"); break;
            case EMapLinkBPStandInMode::Bounds:    ModeStr = TEXT("bounds");    break;
            case EMapLinkBPStandInMode::Marker:    ModeStr = TEXT("marker");    break;
        }
        Root->SetStringField(TEXT("mode"), ModeStr);

        // bounds_fallback_extent in BLENDER units (m) per spec.  UE
        // computes in cm → divide by 100.
        AddVectorField(Root, TEXT("bounds_fallback_extent"),
                       BoundsExtent * 0.01f);

        // components[]
        TArray<TSharedPtr<FJsonValue>> CompArr;
        for (const FMapLinkBPStandInComponent& C : Components)
        {
            TSharedRef<FJsonObject> CObj = MakeShared<FJsonObject>();
            CObj->SetStringField(TEXT("object_name"), C.ObjectName);
            const FString* FbxPath = MeshFbxPaths.Find(C.StaticMesh);
            CObj->SetStringField(TEXT("fbx_path"),
                                 FbxPath ? *FbxPath : FString());
            CObj->SetStringField(TEXT("mesh_uasset_path"),
                                 C.MeshUAssetPath);

            // relative_transform — convert UE cm to Blender m (location only)
            TSharedRef<FJsonObject> RT = MakeShared<FJsonObject>();
            AddVectorField(RT, TEXT("location"),
                           C.RelativeTransform.GetLocation() * 0.01f);
            const FRotator Rot = C.RelativeTransform.Rotator();
            AddVectorField(RT, TEXT("rotation_euler_deg"),
                           FVector(Rot.Roll, Rot.Pitch, Rot.Yaw));
            AddVectorField(RT, TEXT("scale"),
                           C.RelativeTransform.GetScale3D());
            CObj->SetObjectField(TEXT("relative_transform"), RT);

            CompArr.Add(MakeShared<FJsonValueObject>(CObj));
        }
        Root->SetArrayField(TEXT("components"), CompArr);

        Root->SetStringField(TEXT("report_path"), ReportPath);

        FString Out;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
        FJsonSerializer::Serialize(Root, Writer);
        // v1.21 - UTF-8, always. The default encoding turns UTF-16 the
        // moment a path carries an accent, and maplink_create_bp_standin.py
        // opens this file as UTF-8.
        return FFileHelper::SaveStringToFile(Out, *OutPath,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }

    // -----------------------------------------------------------------
    // Blender subprocess
    // -----------------------------------------------------------------

    /** Returns the absolute path of the headless script. */
    FString GetHeadlessScriptPath()
    {
        const FString PluginDir = FMapLinkUtils::GetPluginBaseDir();
        return PluginDir / TEXT("Scripts") / TEXT("maplink_create_bp_standin.py");
    }

    /** Run blender headless against the spec JSON.  Returns true on
     *  successful subprocess exit (0).  bOutWroteReport indicates
     *  whether the script wrote its report file (it might fail before
     *  reaching report write). */
    bool RunHeadlessBlender(const FString& SpecPath, int32& OutExitCode,
                            FString& OutCombinedOutput)
    {
        // v1.21 -- resolve and version-check through the shared preflight so
        // this path refuses an old Blender for the same reason import does.
        FString PreflightError;
        EMapLinkResult PreflightResult = EMapLinkResult::Success;
        if (!FMapLinkBlenderBridge::Preflight(PreflightError, PreflightResult))
        {
            UE_LOG(LogMapLinkBPStandIn, Error, TEXT("%s"),
                   *PreflightError.Replace(TEXT("\n"), TEXT(" ")));
            OutCombinedOutput += PreflightError;
            OutExitCode = -1;
            return false;
        }
        const FString BlenderExe =
            FMapLinkBlenderBridge::GetBlenderExecutablePath();

        const FString Script = GetHeadlessScriptPath();
        if (!FPaths::FileExists(Script))
        {
            UE_LOG(LogMapLinkBPStandIn, Error,
                   TEXT("Headless script missing: '%s'"), *Script);
            OutExitCode = -1;
            return false;
        }

        const FString Args = FString::Printf(
            TEXT("--background --factory-startup --python-exit-code 1 ")
            TEXT("--python \"%s\" -- --spec \"%s\""),
            *Script, *SpecPath);

        void* PipeRead  = nullptr;
        void* PipeWrite = nullptr;
        FPlatformProcess::CreatePipe(PipeRead, PipeWrite);

        FProcHandle Proc = FPlatformProcess::CreateProc(
            *BlenderExe, *Args,
            /*bLaunchDetached=*/false,
            /*bLaunchHidden=*/true,
            /*bLaunchReallyHidden=*/true,
            /*OutProcessID=*/nullptr,
            /*PriorityModifier=*/0,
            /*OptionalWorkingDirectory=*/nullptr,
            PipeWrite);

        if (!Proc.IsValid())
        {
            FPlatformProcess::ClosePipe(PipeRead, PipeWrite);
            UE_LOG(LogMapLinkBPStandIn, Error,
                   TEXT("CreateProc failed for Blender headless."));
            OutExitCode = -1;
            return false;
        }

        // Drain stdout until child exits
        OutCombinedOutput.Reset();
        while (FPlatformProcess::IsProcRunning(Proc))
        {
            const FString Chunk = FPlatformProcess::ReadPipe(PipeRead);
            if (!Chunk.IsEmpty()) OutCombinedOutput += Chunk;
            FPlatformProcess::Sleep(0.05f);
        }
        // Final read after exit
        const FString Tail = FPlatformProcess::ReadPipe(PipeRead);
        if (!Tail.IsEmpty()) OutCombinedOutput += Tail;

        int32 ExitCode = -1;
        FPlatformProcess::GetProcReturnCode(Proc, &ExitCode);
        FPlatformProcess::CloseProc(Proc);
        FPlatformProcess::ClosePipe(PipeRead, PipeWrite);

        OutExitCode = ExitCode;
        UE_LOG(LogMapLinkBPStandIn, Log,
               TEXT("Blender headless exit=%d, %d bytes captured."),
               ExitCode, OutCombinedOutput.Len());
        return ExitCode == 0;
    }

    /** Parse the child report's result field.  Returns true if
     *  result=="success", false otherwise (including parse fail). */
    bool ReadChildReportResult(const FString& ReportPath, FString& OutResult)
    {
        OutResult.Reset();
        FString JsonStr;
        if (!FFileHelper::LoadFileToString(JsonStr, *ReportPath))
        {
            return false;
        }
        TSharedPtr<FJsonObject> Root;
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
        if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
        {
            return false;
        }
        Root->TryGetStringField(TEXT("result"), OutResult);
        return OutResult == TEXT("success");
    }

    // -----------------------------------------------------------------
    // Temp dir cleanup
    // -----------------------------------------------------------------

    void CleanupTempDir()
    {
        const FString TempDir = GetProjectTempDir();
        if (!FPaths::DirectoryExists(TempDir)) return;
        IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
        // Remove contents but not the dir itself
        TArray<FString> Files;
        PF.FindFilesRecursively(Files, *TempDir, TEXT(""));
        for (const FString& F : Files)
        {
            PF.DeleteFile(*F);
        }
    }
}

// ===========================================================================
// CreateStandIn — 10 stages
// ===========================================================================

bool FMapLinkBPStandInCreator::CreateStandIn(
    UBlueprint* BP, bool bForceOverwrite, FMapLinkStandInReport& Report)
{
    // -----------------------------------------------------------------
    // Stage 1 — validate
    // -----------------------------------------------------------------
    Report.BeginStage(TEXT("validate"));
    if (!BP)
    {
        Report.AddError(TEXT("Null Blueprint"));
        Report.EndStage(EMapLinkReportStatus::Error, nullptr,
                        TEXT("null blueprint"));
        Report.SetResult(EMapLinkReportStatus::Error);
        return false;
    }
    if (!BP->ParentClass ||
        !BP->ParentClass->IsChildOf(AActor::StaticClass()))
    {
        Report.AddError(TEXT("Blueprint is not actor-derived"));
        Report.EndStage(EMapLinkReportStatus::Error, nullptr,
                        TEXT("not actor-derived"));
        Report.SetResult(EMapLinkReportStatus::Error);
        return false;
    }
    if (IsInPluginMapLinkContent(BP))
    {
        Report.AddError(TEXT("Blueprint lives under /MapLink/ — plugin "
                             "content can't have a stand-in"));
        Report.EndStage(EMapLinkReportStatus::Error, nullptr,
                        TEXT("/MapLink/ content"));
        Report.SetResult(EMapLinkReportStatus::Error);
        return false;
    }
    const FString BPName = BP->GetName();
    if (BPName.Equals(TEXT("BP_Deprecated"), ESearchCase::IgnoreCase))
    {
        Report.AddError(TEXT("Cannot create stand-in for BP_Deprecated"));
        Report.EndStage(EMapLinkReportStatus::Error, nullptr,
                        TEXT("BP_Deprecated rejected"));
        Report.SetResult(EMapLinkReportStatus::Error);
        return false;
    }
    Report.SetBlueprintClass(BP->ParentClass->GetName());
    {
        TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
        D->SetStringField(TEXT("bp_name"),   BPName);
        D->SetStringField(TEXT("parent"),    BP->ParentClass->GetName());
        D->SetStringField(TEXT("bp_path"),   BP->GetPathName());
        Report.EndStage(EMapLinkReportStatus::Ok, D, TEXT(""));
    }

    // -----------------------------------------------------------------
    // Stage 2 — resolve_target
    // -----------------------------------------------------------------
    Report.BeginStage(TEXT("resolve_target"));
    const FString BlenderRoot =
        UMapLinkSettings::ResolveBlenderProjectRoot(true, true);
    if (BlenderRoot.IsEmpty())
    {
        Report.AddError(TEXT("BlenderProjectRoot not set / not resolvable"));
        Report.EndStage(EMapLinkReportStatus::Error, nullptr,
                        TEXT("no BlenderProjectRoot"));
        Report.SetResult(EMapLinkReportStatus::Error);
        return false;
    }
    const FString TemplateBlend =
        UMapLinkSettings::GetBPTemplateBlendPath(BlenderRoot);
    if (!FPaths::FileExists(TemplateBlend))
    {
        Report.AddError(FString::Printf(
            TEXT("BP_Template.blend missing at '%s'"), *TemplateBlend));
        Report.EndStage(EMapLinkReportStatus::Error, nullptr,
                        TEXT("template missing"));
        Report.SetResult(EMapLinkReportStatus::Error);
        return false;
    }
    const FString TargetBlend =
        UMapLinkSettings::GetBPStandInBlendPath(BlenderRoot, BPName);
    if (FPaths::FileExists(TargetBlend) && !bForceOverwrite)
    {
        const FText Msg = FText::Format(
            LOCTEXT("OverwriteConfirm",
                    "A stand-in already exists at:\n\n{0}\n\n"
                    "Overwrite it?"),
            FText::FromString(TargetBlend));
        if (FMessageDialog::Open(EAppMsgType::YesNo, Msg) !=
            EAppReturnType::Yes)
        {
            Report.EndStage(EMapLinkReportStatus::Skip, nullptr,
                            TEXT("user declined overwrite"));
            Report.SetResult(EMapLinkReportStatus::Skip);
            return false;
        }
    }
    Report.SetStandInBlendPath(TargetBlend);
    {
        TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
        D->SetStringField(TEXT("blender_root"),    BlenderRoot);
        D->SetStringField(TEXT("template_blend"),  TemplateBlend);
        D->SetStringField(TEXT("target_blend"),    TargetBlend);
        Report.EndStage(EMapLinkReportStatus::Ok, D, TEXT(""));
    }

    // -----------------------------------------------------------------
    // Stage 3 — scs_walk
    // -----------------------------------------------------------------
    Report.BeginStage(TEXT("scs_walk"));
    TArray<FMapLinkBPStandInComponent> Components;
    WalkSCSCollectComponents(BP, Components);
    {
        TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
        D->SetNumberField(TEXT("component_count"), Components.Num());
        Report.EndStage(EMapLinkReportStatus::Ok, D, TEXT(""));
    }

    // -----------------------------------------------------------------
    // Stage 4 — determine_mode
    // -----------------------------------------------------------------
    Report.BeginStage(TEXT("determine_mode"));
    EMapLinkBPStandInMode Mode = EMapLinkBPStandInMode::Marker;
    FVector BoundsExtent(200.f, 200.f, 200.f);
    if (Components.Num() > 0)
    {
        Mode = EMapLinkBPStandInMode::Extracted;
    }
    else
    {
        const FVector Estimated = ComputeBoundsFallbackExtent(BP);
        if (!Estimated.IsNearlyZero(1.0f))
        {
            Mode = EMapLinkBPStandInMode::Bounds;
            BoundsExtent = Estimated;
        }
    }
    {
        const TCHAR* ModeStr =
            Mode == EMapLinkBPStandInMode::Extracted ? TEXT("extracted") :
            Mode == EMapLinkBPStandInMode::Bounds    ? TEXT("bounds")    :
                                                       TEXT("marker");
        TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
        D->SetStringField(TEXT("mode"), ModeStr);
        Report.EndStage(EMapLinkReportStatus::Ok, D, TEXT(""));
    }

    // -----------------------------------------------------------------
    // Pre-flight confirmation dialog (after we know everything)
    // -----------------------------------------------------------------
    if (!bForceOverwrite)
    {
        const TCHAR* ModeStr =
            Mode == EMapLinkBPStandInMode::Extracted ? TEXT("extracted") :
            Mode == EMapLinkBPStandInMode::Bounds    ? TEXT("bounds")    :
                                                       TEXT("marker");
        const FText Msg = FText::Format(
            LOCTEXT("PreflightConfirm",
                    "Create MapLink stand-in for:\n  {0}\n\n"
                    "Target:      {1}\n"
                    "Mode:        {2}\n"
                    "Components:  {3}\n"
                    "P4 CL:       {4}\n\n"
                    "Proceed?"),
            FText::FromString(BP->GetName()),
            FText::FromString(TargetBlend),
            FText::FromString(ModeStr),
            FText::AsNumber(Components.Num()),
            FText::FromString(CL_DESC_CREATE));
        if (FMessageDialog::Open(EAppMsgType::YesNo, Msg) !=
            EAppReturnType::Yes)
        {
            Report.SetResult(EMapLinkReportStatus::Skip);
            Report.AddWarning(TEXT("user cancelled at preflight"));
            return false;
        }
    }

    // -----------------------------------------------------------------
    // Stage 5 — fbx_export
    // -----------------------------------------------------------------
    Report.BeginStage(TEXT("fbx_export"));
    const FString TempDir = GetProjectTempDir();
    FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*TempDir);

    TMap<UStaticMesh*, FString> MeshFbxPaths;
    if (Mode == EMapLinkBPStandInMode::Extracted)
    {
        TSet<UStaticMesh*> UniqueMeshes;
        for (const FMapLinkBPStandInComponent& C : Components)
        {
            if (C.StaticMesh) UniqueMeshes.Add(C.StaticMesh);
        }
        for (UStaticMesh* Mesh : UniqueMeshes)
        {
            const FString MeshName = Mesh->GetName();
            const FString FbxPath = TempDir / (MeshName + TEXT(".fbx"));
            if (ExportStaticMeshToFBX(Mesh, FbxPath))
            {
                MeshFbxPaths.Add(Mesh, FbxPath);
            }
            else
            {
                Report.AddWarning(FString::Printf(
                    TEXT("FBX export failed for mesh '%s'"), *MeshName));
            }
        }
    }
    {
        TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
        D->SetNumberField(TEXT("exported_meshes"), MeshFbxPaths.Num());
        Report.EndStage(EMapLinkReportStatus::Ok, D, TEXT(""));
    }

    // -----------------------------------------------------------------
    // Stage 6 — spec_write
    // -----------------------------------------------------------------
    Report.BeginStage(TEXT("spec_write"));
    const FString SpecPath = TempDir / (BPName + TEXT("_spec.json"));
    const FString ChildReportPath = GetReportPathForCreate(BPName);
    if (!WriteSpecJson(
            SpecPath,
            BP->GetPathName(),
            BPName,
            BP->ParentClass->GetName(),
            TargetBlend,
            TemplateBlend,
            Mode,
            BoundsExtent,
            Components,
            MeshFbxPaths,
            ChildReportPath))
    {
        Report.AddError(FString::Printf(
            TEXT("Failed to write spec JSON: %s"), *SpecPath));
        Report.EndStage(EMapLinkReportStatus::Error, nullptr,
                        TEXT("spec write failed"));
        Report.SetResult(EMapLinkReportStatus::Error);
        return false;
    }
    {
        TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
        D->SetStringField(TEXT("spec_path"),   SpecPath);
        D->SetStringField(TEXT("child_report"), ChildReportPath);
        Report.EndStage(EMapLinkReportStatus::Ok, D, TEXT(""));
    }

    // -----------------------------------------------------------------
    // Stage 7 — blender_run
    // -----------------------------------------------------------------
    Report.BeginStage(TEXT("blender_run"));
    int32 ExitCode = -1;
    FString StdoutCombined;
    const bool bRanOk = RunHeadlessBlender(SpecPath, ExitCode, StdoutCombined);
    FString ChildResult;
    const bool bChildOk = ReadChildReportResult(ChildReportPath, ChildResult);
    if (!bRanOk || !bChildOk || !FPaths::FileExists(TargetBlend))
    {
        Report.AddError(FString::Printf(
            TEXT("Blender headless failed (exit=%d, child_result='%s', "
                 "target_exists=%d)"),
            ExitCode, *ChildResult, FPaths::FileExists(TargetBlend) ? 1 : 0));
        TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
        D->SetNumberField(TEXT("exit_code"),    ExitCode);
        D->SetStringField(TEXT("child_result"), ChildResult);
        D->SetStringField(TEXT("child_report"), ChildReportPath);
        Report.EndStage(EMapLinkReportStatus::Error, D,
                        TEXT("blender run failed"));
        Report.SetResult(EMapLinkReportStatus::Error);
        return false;
    }
    {
        TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
        D->SetNumberField(TEXT("exit_code"),    ExitCode);
        D->SetStringField(TEXT("child_result"), ChildResult);
        D->SetStringField(TEXT("child_report"), ChildReportPath);
        Report.EndStage(EMapLinkReportStatus::Ok, D, TEXT(""));
    }

    // -----------------------------------------------------------------
    // Stage 8 — p4_add
    // -----------------------------------------------------------------
    Report.BeginStage(TEXT("p4_add"));
    const FString BlendDir = FPaths::GetPath(TargetBlend);
    const FString StemAnalysis = BlendDir / (BPName + TEXT("_analysis.json"));
    const FString StemHash     = BlendDir / (BPName + TEXT("_filehash.txt"));

    Report.SetOutput(TEXT("blend"),            TargetBlend);
    Report.SetOutput(TEXT("sidecar_analysis"), StemAnalysis);
    Report.SetOutput(TEXT("sidecar_hash"),     StemHash);

    FSourceControlChangelistPtr CL = GetOrCreateChangelist(CL_DESC_CREATE);
    bool bAllAddedOk = true;
    bAllAddedOk &= SCCMarkForAdd(TargetBlend,  CL);
    bAllAddedOk &= SCCMarkForAdd(StemAnalysis, CL);
    bAllAddedOk &= SCCMarkForAdd(StemHash,     CL);
    {
        TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
        D->SetBoolField(TEXT("scc_enabled"),
                        ISourceControlModule::Get().IsEnabled());
        D->SetBoolField(TEXT("all_ops_ok"), bAllAddedOk);
        D->SetStringField(TEXT("cl_description"), CL_DESC_CREATE);
        Report.EndStage(
            bAllAddedOk ? EMapLinkReportStatus::Ok : EMapLinkReportStatus::Warn,
            D,
            bAllAddedOk ? TEXT("") : TEXT("some SCC ops returned non-ok"));
    }

    // -----------------------------------------------------------------
    // Stage 9 — bind
    // -----------------------------------------------------------------
    Report.BeginStage(TEXT("bind"));
    UMapLinkBPBinding* Binding = UMapLinkBPBinding::GetOrCreate(BP);
    if (!Binding)
    {
        Report.AddError(TEXT("Failed to create UMapLinkBPBinding"));
        Report.EndStage(EMapLinkReportStatus::Error, nullptr,
                        TEXT("binding creation failed"));
        Report.SetResult(EMapLinkReportStatus::Error);
        return false;
    }
    Binding->StandInBlendPath          = TargetBlend;
    Binding->CreatedTimestamp          = FDateTime::UtcNow();
    Binding->CreatedFromMapLinkVersion = MAPLINK_VERSION;

    BP->MarkPackageDirty();
    {
        TArray<UPackage*> ToSave = { BP->GetOutermost() };
        FEditorFileUtils::PromptForCheckoutAndSave(
            ToSave,
            /*bCheckDirty=*/false,
            /*bPromptToSave=*/false);
    }
    {
        TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
        D->SetStringField(TEXT("standin_blend_path"), TargetBlend);
        Report.EndStage(EMapLinkReportStatus::Ok, D, TEXT(""));
    }

    // -----------------------------------------------------------------
    // Stage 10 — cleanup_temp
    // -----------------------------------------------------------------
    Report.BeginStage(TEXT("cleanup_temp"));
    CleanupTempDir();
    Report.EndStage(EMapLinkReportStatus::Ok, nullptr, TEXT(""));

    Report.SetResult(EMapLinkReportStatus::Ok);
    UE_LOG(LogMapLinkBPStandIn, Log,
           TEXT("CreateStandIn SUCCESS for '%s' → '%s'"),
           *BPName, *TargetBlend);
    return true;
}

// ===========================================================================
// RefreshStandIn
// ===========================================================================

bool FMapLinkBPStandInCreator::RefreshStandIn(
    UBlueprint* BP, FMapLinkStandInReport& Report)
{
    if (!BP)
    {
        Report.AddError(TEXT("Null BP"));
        Report.SetResult(EMapLinkReportStatus::Error);
        return false;
    }
    if (!UMapLinkBPBinding::Get(BP))
    {
        Report.AddError(TEXT("Refresh requested but no binding exists"));
        Report.SetResult(EMapLinkReportStatus::Error);
        return false;
    }
    return CreateStandIn(BP, /*bForceOverwrite=*/true, Report);
}

// ===========================================================================
// DeleteStandIn
// ===========================================================================

bool FMapLinkBPStandInCreator::DeleteStandIn(
    UBlueprint* BP, bool bAlreadyConfirmed, FMapLinkStandInReport& Report)
{
    Report.BeginStage(TEXT("resolve_target"));
    if (!BP)
    {
        Report.AddError(TEXT("Null BP"));
        Report.EndStage(EMapLinkReportStatus::Error, nullptr, TEXT("null"));
        Report.SetResult(EMapLinkReportStatus::Error);
        return false;
    }
    UMapLinkBPBinding* Binding = UMapLinkBPBinding::Get(BP);
    if (!Binding || Binding->StandInBlendPath.IsEmpty())
    {
        Report.AddError(TEXT("No binding / stand-in path to delete"));
        Report.EndStage(EMapLinkReportStatus::Error, nullptr,
                        TEXT("no binding"));
        Report.SetResult(EMapLinkReportStatus::Error);
        return false;
    }
    const FString Blend = Binding->StandInBlendPath;
    const FString Dir = FPaths::GetPath(Blend);
    const FString Stem = FPaths::GetBaseFilename(Blend);
    const FString Analysis = Dir / (Stem + TEXT("_analysis.json"));
    const FString Hash     = Dir / (Stem + TEXT("_filehash.txt"));
    Report.SetStandInBlendPath(Blend);
    Report.EndStage(EMapLinkReportStatus::Ok, nullptr, TEXT(""));

    if (!bAlreadyConfirmed)
    {
        const FText Msg = FText::Format(
            LOCTEXT("DeleteConfirm",
                    "Delete MapLink stand-in for '{0}'?\n\n"
                    "This will:\n"
                    "  • remove the binding from the Blueprint\n"
                    "  • p4-delete:\n"
                    "      {1}\n"
                    "      + sidecar JSON / filehash\n"
                    "    into CL '{2}'\n"
                    "  • delete every asset under /Game/MapLink/{3}/\n"
                    "    (proxy + any orphan SM folder + any synth BP)\n\n"
                    "Continue?"),
            FText::FromString(BP->GetName()),
            FText::FromString(Blend),
            FText::FromString(CL_DESC_DELETE),
            FText::FromString(BP->GetName()));
        if (FMessageDialog::Open(EAppMsgType::YesNo, Msg) !=
            EAppReturnType::Yes)
        {
            Report.SetResult(EMapLinkReportStatus::Skip);
            Report.AddWarning(TEXT("user declined delete"));
            return false;
        }
    }

    Report.BeginStage(TEXT("p4_delete"));
    FSourceControlChangelistPtr CL = GetOrCreateChangelist(CL_DESC_DELETE);
    bool bOk = true;
    bOk &= SCCMarkForDelete(Blend,    CL);
    bOk &= SCCMarkForDelete(Analysis, CL);
    bOk &= SCCMarkForDelete(Hash,     CL);
    Report.EndStage(
        bOk ? EMapLinkReportStatus::Ok : EMapLinkReportStatus::Warn,
        nullptr,
        bOk ? TEXT("") : TEXT("some SCC delete ops failed"));

    Report.BeginStage(TEXT("unbind"));
    UMapLinkBPBinding::Remove(BP);
    BP->MarkPackageDirty();
    {
        TArray<UPackage*> ToSave = { BP->GetOutermost() };
        FEditorFileUtils::PromptForCheckoutAndSave(
            ToSave, false, false);
    }
    Report.EndStage(EMapLinkReportStatus::Ok, nullptr, TEXT(""));

    // -----------------------------------------------------------------
    // v1.8 D7 — Purge /Game/MapLink/<BPName>/ folder.
    // This removes the Proxy AND any orphan SM folder / synthesized BP
    // that might have been created by a pre-D7 reimport when the
    // sidecar got transiently stripped.  We use the BP's name as the
    // folder name (matches the stand-in stem convention).
    // -----------------------------------------------------------------
    Report.BeginStage(TEXT("purge_proxy_folder"));
    {
        const FString BPName       = BP->GetName();
        const FString ProxyFolder  = FString::Printf(
            TEXT("/Game/MapLink/%s"), *BPName);

        FAssetRegistryModule& AssetRegistryModule =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
        IAssetRegistry& AR = AssetRegistryModule.Get();

        TArray<FAssetData> AssetsUnderFolder;
        AR.GetAssetsByPath(FName(*ProxyFolder), AssetsUnderFolder,
                           /*bRecursive=*/true,
                           /*bIncludeOnlyOnDiskAssets=*/false);

        TArray<UObject*> AssetsToDelete;
        for (const FAssetData& Data : AssetsUnderFolder)
        {
            if (UObject* Obj = Data.GetAsset())
            {
                AssetsToDelete.Add(Obj);
            }
        }

        int32 NumDeleted = 0;
        if (AssetsToDelete.Num() > 0)
        {
            // bShowConfirmation = false: we already confirmed the
            // overall delete operation upstream.
            NumDeleted = ObjectTools::DeleteObjects(
                AssetsToDelete, /*bShowConfirmation=*/false);
            UE_LOG(LogMapLinkBPStandIn, Log,
                   TEXT("[Delete] Purged %d asset(s) under '%s'."),
                   NumDeleted, *ProxyFolder);
        }
        else
        {
            UE_LOG(LogMapLinkBPStandIn, Log,
                   TEXT("[Delete] No assets found under '%s' — nothing to purge."),
                   *ProxyFolder);
        }

        // Remove the empty folder from the Content Browser tree.
        // Best-effort — if the folder still has unsaved redirectors
        // or undeletable assets, this no-ops without error.
        if (NumDeleted > 0 || AssetsToDelete.Num() == 0)
        {
            AR.RemovePath(ProxyFolder);
        }
    }
    Report.EndStage(EMapLinkReportStatus::Ok, nullptr, TEXT(""));

    Report.SetResult(bOk ? EMapLinkReportStatus::Ok
                         : EMapLinkReportStatus::Warn);
    return bOk;
}

#undef LOCTEXT_NAMESPACE
