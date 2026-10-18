// Copyright MapModelers. All Rights Reserved.
//
// v1.4.5 — Optimisations:
//   - --factory-startup: skip user prefs/addons (faster Blender startup)
//   - No .blend file passed to --background: script opens it once via open_mainfile
//   - RunBlenderExport uses "both" mode: analyze+export in a single Blender session
// v1.7.3 — Added OpenBlenderGUI() and OpenInExplorer().

#include "MapLinkBlenderBridge.h"
#include "MapLinkUtils.h"
#include "MapLinkPipeline.h"
#include "MapLinkTiming.h"
#include "MapLinkSettings.h"   // v1.9 warm-pass worker count
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Misc/FeedbackContext.h"
#include "Misc/ScopedSlowTask.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/MessageDialog.h"

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

EMapLinkResult FMapLinkBlenderBridge::RunBlenderScript(
    const FString& BlendFilePath,
    const FString& PluginBaseDir,
    const FString& Mode,
    const FString& Reason)
{
    // v1.7.4 — timing instrumentation
    const double LaunchT0 = FPlatformTime::Seconds();
    const FString BlendNameForTiming = FPaths::GetBaseFilename(BlendFilePath);

    // v1.21 -- resolve and version-check in one place. A multi-node run
    // calls Preflight() itself before its loop, so this stays the
    // single-launch path and the dialog cannot repeat per node.
    FString PreflightError;
    EMapLinkResult PreflightResult = EMapLinkResult::Success;
    if (!Preflight(PreflightError, PreflightResult))
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(PreflightError));
        FMapLinkTimingSession::Get().LogBlenderLaunch(
            BlendNameForTiming, Reason, Mode,
            (FPlatformTime::Seconds() - LaunchT0) * 1000.0, -1);
        return PreflightResult;
    }
    const FString BlenderExe = GetBlenderExecutablePath();

    const FString ScriptPath = PluginBaseDir / TEXT("Scripts") / TEXT("maplink_export.py");
    if (!FPaths::FileExists(ScriptPath))
    {
        UE_LOG(LogMapLink, Error,
               TEXT("MapLink Python script not found at '%s'."), *ScriptPath);
        FMapLinkTimingSession::Get().LogBlenderLaunch(
            BlendNameForTiming, Reason, Mode,
            (FPlatformTime::Seconds() - LaunchT0) * 1000.0, -1);
        return EMapLinkResult::BlenderScriptFailed;
    }

    // v1.4.5: Launch Blender WITHOUT the .blend on the command line.
    // The Python script opens it once via bpy.ops.wm.open_mainfile.
    // --factory-startup skips user prefs/addons for faster startup.
    // Asset Library data is stored IN the .blend files, not in user prefs,
    // so --factory-startup is safe for the MapLink pipeline.
    const FString Args = FString::Printf(
        TEXT("--background --factory-startup --python-exit-code 1 --python \"%s\" -- --blend \"%s\" --outdir \"%s\" --mode %s"),
        *ScriptPath,
        *BlendFilePath,
        *FMapLinkUtils::GetArtifactRootDir(),
        *Mode);

    UE_LOG(LogMapLink, Log,
           TEXT("Launching Blender: %s %s"), *BlenderExe, *Args);

    // Progress dialog
    FScopedSlowTask SlowTask(1.f,
        FText::FromString(FString::Printf(
            TEXT("MapLink: Running Blender %s pass…"), *Mode)));
    SlowTask.MakeDialog(/*bShowCancelButton=*/false);

    // Launch process
    void* ReadPipe = nullptr;
    void* WritePipe = nullptr;
    FPlatformProcess::CreatePipe(ReadPipe, WritePipe);

    uint32 ProcId = 0;
    FProcHandle Proc = FPlatformProcess::CreateProc(
        *BlenderExe,
        *Args,
        /*bLaunchDetached=*/false,
        /*bLaunchHidden=*/true,
        /*bLaunchReallyHidden=*/true,
        &ProcId,
        /*PriorityModifier=*/0,
        /*OptionalWorkingDirectory=*/nullptr,
        WritePipe);

    if (!Proc.IsValid())
    {
        FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
        UE_LOG(LogMapLink, Error, TEXT("Failed to create Blender process."));
        FMapLinkTimingSession::Get().LogBlenderLaunch(
            BlendNameForTiming, Reason, Mode,
            (FPlatformTime::Seconds() - LaunchT0) * 1000.0, -1);
        return EMapLinkResult::BlenderNotFound;
    }

    // Read stdout while waiting.  v1.9.4 — watchdog timeout + cooperative
    // cancel so a hung Blender can never freeze the editor forever.
    const double WaitT0    = FPlatformTime::Seconds();
    const double TimeoutSec = static_cast<double>(
        FMath::Max(0, UMapLinkSettings::Get()->BlenderTimeoutSeconds));
    EMapLinkResult KillResult = EMapLinkResult::Success;  // Success == not killed
    FString StdOutput;
    while (FPlatformProcess::IsProcRunning(Proc))
    {
        StdOutput += FPlatformProcess::ReadPipe(ReadPipe);
        FPlatformProcess::Sleep(0.1f);

        if (SlowTask.ShouldCancel())
        {
            KillResult = EMapLinkResult::Cancelled;
            break;
        }
        if (TimeoutSec > 0.0 &&
            (FPlatformTime::Seconds() - WaitT0) > TimeoutSec)
        {
            KillResult = EMapLinkResult::BlenderTimedOut;
            break;
        }
    }

    if (KillResult != EMapLinkResult::Success)
    {
        FPlatformProcess::TerminateProc(Proc, /*KillTree=*/true);
        FPlatformProcess::Sleep(0.05f);
    }

    // Read any remaining output
    StdOutput += FPlatformProcess::ReadPipe(ReadPipe);

    int32 ReturnCode = -1;
    FPlatformProcess::GetProcReturnCode(Proc, &ReturnCode);
    FPlatformProcess::CloseProc(Proc);
    FPlatformProcess::ClosePipe(ReadPipe, WritePipe);

    if (KillResult == EMapLinkResult::Cancelled)
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("Blender '%s' cancelled by user — process terminated."),
               *BlendNameForTiming);
        FMapLinkTimingSession::Get().LogBlenderLaunch(
            BlendNameForTiming, Reason, Mode,
            (FPlatformTime::Seconds() - LaunchT0) * 1000.0, -2);
        return EMapLinkResult::Cancelled;
    }
    if (KillResult == EMapLinkResult::BlenderTimedOut)
    {
        UE_LOG(LogMapLink, Error,
               TEXT("Blender '%s' exceeded %.0f s watchdog timeout — "
                    "terminated."), *BlendNameForTiming, TimeoutSec);
        FMapLinkTimingSession::Get().LogBlenderLaunch(
            BlendNameForTiming, Reason, Mode,
            (FPlatformTime::Seconds() - LaunchT0) * 1000.0, -2);
        ShowBlenderErrorDialog(FString::Printf(
            TEXT("Blender did not finish within %.0f s and was terminated.\n\n"
                 "If your scenes are genuinely this large, raise "
                 "'Blender Timeout' in Project Settings > Plugins > MapLink > "
                 "Performance (0 disables the watchdog).\n\nLast output:\n%s"),
            TimeoutSec, *StdOutput));
        return EMapLinkResult::BlenderScriptFailed;
    }

    UE_LOG(LogMapLink, Log,
           TEXT("Blender process exited with code %d."), ReturnCode);

    if (!StdOutput.IsEmpty())
    {
        UE_LOG(LogMapLink, Log, TEXT("Blender stdout:\n%s"), *StdOutput);
    }

    // v1.7.4 — record this launch in the timing session
    FMapLinkTimingSession::Get().LogBlenderLaunch(
        BlendNameForTiming, Reason, Mode,
        (FPlatformTime::Seconds() - LaunchT0) * 1000.0, ReturnCode);

    if (ReturnCode != 0)
    {
        ShowBlenderErrorDialog(StdOutput);
        return EMapLinkResult::BlenderScriptFailed;
    }

    // v1.21 -- a Python traceback that Blender still exits 0 on. Blender
    // honours --python-exit-code only for an uncaught exception, and an
    // older build ignores it entirely, so the output is the second witness.
    if (StdOutput.Contains(TEXT("Traceback (most recent call last)")))
    {
        UE_LOG(LogMapLink, Error,
               TEXT("Blender exited %d but printed a Python traceback - "
                    "treating this run as failed."), ReturnCode);
        ShowBlenderErrorDialog(StdOutput);
        return EMapLinkResult::BlenderScriptFailed;
    }

    SlowTask.EnterProgressFrame(1.f);
    return EMapLinkResult::Success;
}

EMapLinkResult FMapLinkBlenderBridge::RunBlenderExport(
    const FString& BlendFilePath,
    const FString& PluginBaseDir,
    const FString& Reason)
{
    // v1.4.5: Single Blender launch using "both" mode.
    // Previously this launched Blender TWICE (analyze + export).
    // The Python script now handles both in one session with one file open.
    return RunBlenderScript(BlendFilePath, PluginBaseDir, TEXT("both"), Reason);
}

// ---------------------------------------------------------------------------
// v1.9 — Parallel warm pass
// ---------------------------------------------------------------------------

int32 FMapLinkBlenderBridge::RunBlenderExportsParallel(
    const TArray<FString>& BlendFilePaths,
    const FString& PluginBaseDir,
    const FString& Reason)
{
    if (BlendFilePaths.Num() == 0)
    {
        return 0;
    }

    // v1.21 -- same resolve + version gate, but silent: the sequential
    // loop that follows reports the failure with a dialog.
    {
        FString WarmError;
        EMapLinkResult WarmResult = EMapLinkResult::Success;
        if (!Preflight(WarmError, WarmResult))
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("[WarmPass] Skipped - %s"), *WarmError.Replace(TEXT("\n"), TEXT(" ")));
            return BlendFilePaths.Num();
        }
    }
    const FString BlenderExe = GetBlenderExecutablePath();

    const FString ScriptPath =
        PluginBaseDir / TEXT("Scripts") / TEXT("maplink_export.py");
    if (!FPaths::FileExists(ScriptPath))
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("[WarmPass] maplink_export.py not found — skipping."));
        return BlendFilePaths.Num();
    }

    const int32 MaxWorkers = FMath::Clamp(
        UMapLinkSettings::Get()->MaxParallelBlenderExports, 1, 16);

    UE_LOG(LogMapLink, Log,
           TEXT("[WarmPass] Exporting %d .blend file(s) with %d parallel "
                "worker(s) (reason=%s)."),
           BlendFilePaths.Num(), MaxWorkers, *Reason);

    const double PassT0 = FPlatformTime::Seconds();

    struct FWarmJob
    {
        FString     BlendPath;
        FString     BlendName;
        FProcHandle Proc;
        void*       ReadPipe  = nullptr;
        void*       WritePipe = nullptr;
        FString     StdOutput;
        double      StartSec  = 0.0;
    };

    FScopedSlowTask SlowTask(
        static_cast<float>(BlendFilePaths.Num()),
        FText::FromString(FString::Printf(
            TEXT("MapLink: Exporting %d files from Blender (%d parallel)…"),
            BlendFilePaths.Num(), MaxWorkers)));
    SlowTask.MakeDialog(/*bShowCancelButton=*/false);

    int32 NextIndex = 0;
    int32 Completed = 0;
    int32 Failed    = 0;
    TArray<TUniquePtr<FWarmJob>> Active;

    auto LaunchNext = [&]() -> bool
    {
        if (NextIndex >= BlendFilePaths.Num()) return false;

        TUniquePtr<FWarmJob> Job = MakeUnique<FWarmJob>();
        Job->BlendPath = BlendFilePaths[NextIndex++];
        Job->BlendName = FPaths::GetBaseFilename(Job->BlendPath);
        Job->StartSec  = FPlatformTime::Seconds();

        const FString Args = FString::Printf(
            TEXT("--background --factory-startup --python-exit-code 1 --python \"%s\" -- ")
            TEXT("--blend \"%s\" --outdir \"%s\" --mode both --lock"),
            *ScriptPath, *Job->BlendPath,
            *FMapLinkUtils::GetArtifactRootDir());

        FPlatformProcess::CreatePipe(Job->ReadPipe, Job->WritePipe);

        uint32 ProcId = 0;
        Job->Proc = FPlatformProcess::CreateProc(
            *BlenderExe, *Args,
            /*bLaunchDetached=*/false,
            /*bLaunchHidden=*/true,
            /*bLaunchReallyHidden=*/true,
            &ProcId,
            /*PriorityModifier=*/0,
            /*OptionalWorkingDirectory=*/nullptr,
            Job->WritePipe);

        if (!Job->Proc.IsValid())
        {
            FPlatformProcess::ClosePipe(Job->ReadPipe, Job->WritePipe);
            UE_LOG(LogMapLink, Error,
                   TEXT("[WarmPass] Failed to create Blender process for "
                        "'%s'."), *Job->BlendName);
            ++Failed;
            ++Completed;
            SlowTask.EnterProgressFrame(1.f);
            return true;    // slot still free — caller keeps launching
        }

        UE_LOG(LogMapLink, Log,
               TEXT("[WarmPass] Launched export %d/%d: %s"),
               NextIndex, BlendFilePaths.Num(), *Job->BlendName);

        Active.Add(MoveTemp(Job));
        return true;
    };

    // v1.9.4 — per-worker watchdog + global cancel.  Without this a single
    // hung worker would stall the whole warm pass (and the editor) forever.
    const double TimeoutSec = static_cast<double>(
        FMath::Max(0, UMapLinkSettings::Get()->BlenderTimeoutSeconds));
    bool bCancelled = false;

    // Prime the pool
    while (Active.Num() < MaxWorkers && LaunchNext()) {}

    // Poll loop
    while (Active.Num() > 0)
    {
        // Global cancel: stop launching new work and kill what's running.
        if (!bCancelled && SlowTask.ShouldCancel())
        {
            bCancelled = true;
            NextIndex  = BlendFilePaths.Num();  // LaunchNext() no-ops hereafter
            UE_LOG(LogMapLink, Warning,
                   TEXT("[WarmPass] Cancelled by user — terminating %d active "
                        "worker(s)."), Active.Num());
        }

        for (int32 i = Active.Num() - 1; i >= 0; --i)
        {
            FWarmJob& Job = *Active[i];
            Job.StdOutput += FPlatformProcess::ReadPipe(Job.ReadPipe);

            const bool bRunning = FPlatformProcess::IsProcRunning(Job.Proc);
            const bool bTimedOut = (TimeoutSec > 0.0) &&
                ((FPlatformTime::Seconds() - Job.StartSec) > TimeoutSec);

            if (bRunning && !bTimedOut && !bCancelled)
            {
                continue;
            }

            // Kill if still running (timeout / cancel), then finalize.
            if (bRunning)
            {
                FPlatformProcess::TerminateProc(Job.Proc, /*KillTree=*/true);
                FPlatformProcess::Sleep(0.02f);
            }

            // Finished — drain pipe, collect return code
            Job.StdOutput += FPlatformProcess::ReadPipe(Job.ReadPipe);

            int32 ReturnCode = -1;
            FPlatformProcess::GetProcReturnCode(Job.Proc, &ReturnCode);
            FPlatformProcess::CloseProc(Job.Proc);
            FPlatformProcess::ClosePipe(Job.ReadPipe, Job.WritePipe);

            const double Ms =
                (FPlatformTime::Seconds() - Job.StartSec) * 1000.0;
            FMapLinkTimingSession::Get().LogBlenderLaunch(
                Job.BlendName, Reason, TEXT("both"), Ms,
                (bTimedOut || bCancelled) ? -2 : ReturnCode);

            if (bTimedOut)
            {
                ++Failed;
                UE_LOG(LogMapLink, Error,
                       TEXT("[WarmPass] Export TIMED OUT (%.0f s): %s — "
                            "terminated; sequential loop will retry."),
                       TimeoutSec, *Job.BlendName);
            }
            else if (bCancelled)
            {
                ++Failed;
                UE_LOG(LogMapLink, Warning,
                       TEXT("[WarmPass] Export cancelled: %s"), *Job.BlendName);
            }
            else if (ReturnCode != 0)
            {
                ++Failed;
                // No modal dialog here — the sequential loop will retry
                // this node (cache stays stale) and surface the error.
                UE_LOG(LogMapLink, Warning,
                       TEXT("[WarmPass] Export FAILED (code %d): %s\n%s"),
                       ReturnCode, *Job.BlendName, *Job.StdOutput);
            }
            else if (Job.StdOutput.Contains(
                         TEXT("Traceback (most recent call last)")))
            {
                // Exit 0 with a traceback: the export did not finish. Count
                // it as failed so the sequential loop re-runs the node.
                ++Failed;
                UE_LOG(LogMapLink, Warning,
                       TEXT("[WarmPass] Export printed a Python traceback "
                            "(exit 0): %s\n%s"),
                       *Job.BlendName, *Job.StdOutput);
            }
            else
            {
                UE_LOG(LogMapLink, Log,
                       TEXT("[WarmPass] Export OK (%.0f ms): %s"),
                       Ms, *Job.BlendName);
            }

            ++Completed;
            SlowTask.EnterProgressFrame(1.f,
                FText::FromString(FString::Printf(
                    TEXT("MapLink: Exported %d/%d…"),
                    Completed, BlendFilePaths.Num())));

            Active.RemoveAt(i);
            while (Active.Num() < MaxWorkers && LaunchNext()) {}
        }

        if (Active.Num() > 0)
        {
            FPlatformProcess::Sleep(0.05f);
        }
    }

    const double TotalMs = (FPlatformTime::Seconds() - PassT0) * 1000.0;
    FMapLinkTimingSession::Get().AddNote(
        TEXT("warm_pass"),
        FString::Printf(TEXT("%d files / %d workers / %.0f ms / %d failed"),
                        BlendFilePaths.Num(), MaxWorkers, TotalMs, Failed));

    UE_LOG(LogMapLink, Log,
           TEXT("[WarmPass] Done: %d exported, %d failed, %.1f s total."),
           BlendFilePaths.Num() - Failed, Failed, TotalMs / 1000.0);

    return Failed;
}

EMapLinkResult FMapLinkBlenderBridge::RunBlenderScriptGeneric(
    const FString& BlendFilePath,
    const FString& ScriptFilename,
    const FString& PluginBaseDir,
    const FString& ExtraArgs,
    const FString& TaskName)
{
    // v1.7.4 — timing instrumentation
    const double LaunchT0 = FPlatformTime::Seconds();
    const FString BlendNameForTiming = FPaths::GetBaseFilename(BlendFilePath);

    FString PreflightError;
    EMapLinkResult PreflightResult = EMapLinkResult::Success;
    if (!Preflight(PreflightError, PreflightResult))
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(PreflightError));
        FMapLinkTimingSession::Get().LogBlenderLaunch(
            BlendNameForTiming, TaskName, TEXT("generic"),
            (FPlatformTime::Seconds() - LaunchT0) * 1000.0, -1);
        return PreflightResult;
    }
    const FString BlenderExe = GetBlenderExecutablePath();

    // v1.19 - ScriptFilename may carry a subdirectory ("blender/foo.py").
    // maplink_project_setup.py moved into the addon package so the Blender
    // addon installer ships it: MapLink_UpdateAddon.cmd mirrors ue\blender and
    // nothing else, so a Blender-only artist had the bundled templates but not
    // the script that uses them. <Plugin>/Scripts stays the default location;
    // this is a fallback, not a replacement.
    FString ScriptPath = PluginBaseDir / TEXT("Scripts") / ScriptFilename;
    if (!FPaths::FileExists(ScriptPath))
    {
        const FString AltPath = PluginBaseDir / ScriptFilename;
        if (FPaths::FileExists(AltPath))
        {
            ScriptPath = AltPath;
        }
    }
    if (!FPaths::FileExists(ScriptPath))
    {
        UE_LOG(LogMapLink, Error,
               TEXT("MapLink script not found at '%s'."), *ScriptPath);
        FMapLinkTimingSession::Get().LogBlenderLaunch(
            BlendNameForTiming, TaskName, TEXT("generic"),
            (FPlatformTime::Seconds() - LaunchT0) * 1000.0, -1);
        return EMapLinkResult::BlenderScriptFailed;
    }

    // Open the .blend via --background, run the script, pass extra args after --
    const FString Args = FString::Printf(
        TEXT("--background \"%s\" --factory-startup --python-exit-code 1 --python \"%s\" -- %s"),
        *BlendFilePath,
        *ScriptPath,
        *ExtraArgs);

    UE_LOG(LogMapLink, Log,
           TEXT("Launching Blender [%s]: %s %s"), *TaskName, *BlenderExe, *Args);

    FScopedSlowTask SlowTask(1.f,
        FText::FromString(FString::Printf(
            TEXT("MapLink: %s…"), *TaskName)));
    SlowTask.MakeDialog(/*bShowCancelButton=*/false);

    void* ReadPipe = nullptr;
    void* WritePipe = nullptr;
    FPlatformProcess::CreatePipe(ReadPipe, WritePipe);

    uint32 ProcId = 0;
    FProcHandle Proc = FPlatformProcess::CreateProc(
        *BlenderExe, *Args,
        /*bLaunchDetached=*/false, /*bLaunchHidden=*/true,
        /*bLaunchReallyHidden=*/true, &ProcId, 0, nullptr, WritePipe);

    if (!Proc.IsValid())
    {
        FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
        UE_LOG(LogMapLink, Error, TEXT("Failed to create Blender process for %s."), *TaskName);
        FMapLinkTimingSession::Get().LogBlenderLaunch(
            BlendNameForTiming, TaskName, TEXT("generic"),
            (FPlatformTime::Seconds() - LaunchT0) * 1000.0, -1);
        return EMapLinkResult::BlenderNotFound;
    }

    // v1.9.4 — watchdog timeout + cooperative cancel (see RunBlenderScript).
    const double WaitT0    = FPlatformTime::Seconds();
    const double TimeoutSec = static_cast<double>(
        FMath::Max(0, UMapLinkSettings::Get()->BlenderTimeoutSeconds));
    EMapLinkResult KillResult = EMapLinkResult::Success;
    FString StdOutput;
    while (FPlatformProcess::IsProcRunning(Proc))
    {
        StdOutput += FPlatformProcess::ReadPipe(ReadPipe);
        FPlatformProcess::Sleep(0.1f);

        if (SlowTask.ShouldCancel())
        {
            KillResult = EMapLinkResult::Cancelled;
            break;
        }
        if (TimeoutSec > 0.0 &&
            (FPlatformTime::Seconds() - WaitT0) > TimeoutSec)
        {
            KillResult = EMapLinkResult::BlenderTimedOut;
            break;
        }
    }

    if (KillResult != EMapLinkResult::Success)
    {
        FPlatformProcess::TerminateProc(Proc, /*KillTree=*/true);
        FPlatformProcess::Sleep(0.05f);
    }

    StdOutput += FPlatformProcess::ReadPipe(ReadPipe);

    int32 ReturnCode = -1;
    FPlatformProcess::GetProcReturnCode(Proc, &ReturnCode);
    FPlatformProcess::CloseProc(Proc);
    FPlatformProcess::ClosePipe(ReadPipe, WritePipe);

    if (KillResult == EMapLinkResult::Cancelled)
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("Blender [%s] cancelled by user — process terminated."),
               *TaskName);
        return EMapLinkResult::Cancelled;
    }
    if (KillResult == EMapLinkResult::BlenderTimedOut)
    {
        UE_LOG(LogMapLink, Error,
               TEXT("Blender [%s] exceeded %.0f s watchdog timeout — "
                    "terminated."), *TaskName, TimeoutSec);
        ShowBlenderErrorDialog(FString::Printf(
            TEXT("Blender task '%s' did not finish within %.0f s and was "
                 "terminated.\n\nRaise 'Blender Timeout' in Project Settings > "
                 "Plugins > MapLink > Performance if needed (0 disables it).\n\n"
                 "Last output:\n%s"),
            *TaskName, TimeoutSec, *StdOutput));
        return EMapLinkResult::BlenderScriptFailed;
    }

    // Write stdout to log file
    const FString LogDir = FMapLinkUtils::GetArtifactLogDir() / TEXT("material_sync");
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    PF.CreateDirectoryTree(*LogDir);
    FFileHelper::SaveStringToFile(StdOutput,
        *(LogDir / TEXT("blender_stdout.log")),
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

    UE_LOG(LogMapLink, Log,
           TEXT("Blender [%s] exited with code %d."), *TaskName, ReturnCode);

    // v1.7.4 — record this launch in the timing session
    FMapLinkTimingSession::Get().LogBlenderLaunch(
        BlendNameForTiming, TaskName, TEXT("generic"),
        (FPlatformTime::Seconds() - LaunchT0) * 1000.0, ReturnCode);

    if (ReturnCode != 0)
    {
        // Log last 30 lines to UE_LOG
        TArray<FString> Lines;
        StdOutput.ParseIntoArrayLines(Lines);
        const int32 StartIdx = FMath::Max(0, Lines.Num() - 30);
        FString Tail;
        for (int32 i = StartIdx; i < Lines.Num(); ++i)
        {
            Tail += Lines[i] + TEXT("\n");
        }
        UE_LOG(LogMapLink, Error,
               TEXT("[%s] Blender script failed. Last output:\n%s"), *TaskName, *Tail);

        ShowBlenderErrorDialog(StdOutput);
        return EMapLinkResult::BlenderScriptFailed;
    }

    // v1.21 -- a Python traceback that Blender still exits 0 on. Blender
    // honours --python-exit-code only for an uncaught exception, and an
    // older build ignores it entirely, so the output is the second witness.
    if (StdOutput.Contains(TEXT("Traceback (most recent call last)")))
    {
        UE_LOG(LogMapLink, Error,
               TEXT("Blender exited %d but printed a Python traceback - "
                    "treating this run as failed."), ReturnCode);
        ShowBlenderErrorDialog(StdOutput);
        return EMapLinkResult::BlenderScriptFailed;
    }

    SlowTask.EnterProgressFrame(1.f);
    return EMapLinkResult::Success;
}

// ---------------------------------------------------------------------------
// v1.7.3 — Interactive Blender / Explorer launch
// ---------------------------------------------------------------------------

EMapLinkResult FMapLinkBlenderBridge::OpenBlenderGUI(const FString& BlendFilePath)
{
    FString PreflightError;
    EMapLinkResult PreflightResult = EMapLinkResult::Success;
    if (!Preflight(PreflightError, PreflightResult))
    {
        FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(PreflightError));
        return PreflightResult;
    }
    const FString BlenderExe = GetBlenderExecutablePath();

    if (!FPaths::FileExists(BlendFilePath))
    {
        FMessageDialog::Open(EAppMsgType::Ok,
            FText::FromString(FString::Printf(
                TEXT("Source .blend file not found:\n%s"), *BlendFilePath)));
        return EMapLinkResult::FbxNotFound;
    }

    // Launch Blender GUI (non-headless, detached).
    const FString Args = FString::Printf(TEXT("\"%s\""), *BlendFilePath);

    UE_LOG(LogMapLink, Log,
           TEXT("Opening Blender GUI: %s %s"), *BlenderExe, *Args);

    FPlatformProcess::CreateProc(
        *BlenderExe,
        *Args,
        /*bLaunchDetached=*/true,
        /*bLaunchHidden=*/false,
        /*bLaunchReallyHidden=*/false,
        /*OutProcessID=*/nullptr,
        /*PriorityModifier=*/0,
        /*OptionalWorkingDirectory=*/nullptr,
        /*PipeWriteChild=*/nullptr);

    return EMapLinkResult::Success;
}

void FMapLinkBlenderBridge::OpenInExplorer(const FString& FilePath)
{
    const FString DirPath = FPaths::GetPath(FilePath);
    if (FPaths::DirectoryExists(DirPath))
    {
        FPlatformProcess::ExploreFolder(*DirPath);
    }
    else
    {
        FMessageDialog::Open(EAppMsgType::Ok,
            FText::FromString(FString::Printf(
                TEXT("Directory not found:\n%s"), *DirPath)));
    }
}

// ---------------------------------------------------------------------------
// Private
// ---------------------------------------------------------------------------

namespace
{
    // v1.21 -- resolved once per editor session; InvalidateBlenderCache()
    // clears it when the setting changes.
    bool    GBlenderResolved     = false;
    FString GBlenderExe;
    bool    GBlenderVersionKnown = false;
    int32   GBlenderMajor        = 0;
    int32   GBlenderMinor        = 0;
    FString GBlenderVersionString;

    /** blender-launcher.exe starts blender.exe and exits immediately, so a
     *  launcher path would make every wait-for-exit in this file return the
     *  instant Blender started. Map it to the real binary beside it. */
    FString PreferRealBlenderBinary(const FString& InPath)
    {
        if (FPaths::GetCleanFilename(InPath).Equals(
                TEXT("blender-launcher.exe"), ESearchCase::IgnoreCase))
        {
            const FString Real = FPaths::GetPath(InPath) / TEXT("blender.exe");
            if (FPaths::FileExists(Real))
            {
                return Real;
            }
        }
        return InPath;
    }

    void AddBlenderCandidate(TArray<FString>& Out, FString Path)
    {
        Path.TrimStartAndEndInline();
        Path.TrimQuotesInline();
        Path.TrimStartAndEndInline();
        if (Path.IsEmpty())
        {
            return;
        }
        FPaths::NormalizeFilename(Path);
        Path = PreferRealBlenderBinary(Path);
        if (FPaths::FileExists(Path))
        {
            Out.AddUnique(Path);
        }
    }

    /** Run a short command and return its stdout. Used for the Windows file
     *  association and the PATH lookup: cheaper in compile risk than pulling
     *  the Win32 registry headers into this module. */
    FString RunAndCapture(const FString& Exe, const FString& Args)
    {
        int32 RC = -1;
        FString StdOut, StdErr;
        FPlatformProcess::ExecProcess(*Exe, *Args, &RC, &StdOut, &StdErr);
        return StdOut;
    }

    /** Pull the executable out of a line like
     *  blendfile="C:\Program Files\...\blender-launcher.exe" "%1" */
    FString ExtractExeFromCommandLine(const FString& In)
    {
        int32 Q1 = INDEX_NONE;
        if (In.FindChar(TEXT('"'), Q1))
        {
            const int32 Q2 = In.Find(TEXT("\""), ESearchCase::CaseSensitive,
                                     ESearchDir::FromStart, Q1 + 1);
            if (Q2 > Q1)
            {
                return In.Mid(Q1 + 1, Q2 - Q1 - 1);
            }
        }
        const int32 ExeIdx = In.Find(TEXT(".exe"), ESearchCase::IgnoreCase);
        if (ExeIdx != INDEX_NONE)
        {
            int32 Eq = INDEX_NONE;
            In.FindChar(TEXT('='), Eq);
            const int32 Start = (Eq != INDEX_NONE) ? Eq + 1 : 0;
            return In.Mid(Start, ExeIdx + 4 - Start);
        }
        return FString();
    }
}

void FMapLinkBlenderBridge::InvalidateBlenderCache()
{
    GBlenderResolved     = false;
    GBlenderVersionKnown = false;
    GBlenderMajor        = 0;
    GBlenderMinor        = 0;
    GBlenderExe.Empty();
    GBlenderVersionString.Empty();
    UE_LOG(LogMapLink, Log,
           TEXT("[Blender] Cached executable cleared - it will be resolved "
                "again on the next launch."));
}

FString FMapLinkBlenderBridge::BlenderNotFoundMessage()
{
    return TEXT(
        "MapLink could not find Blender.\n\n"
        "Set the full path to blender.exe here:\n"
        "    Project Settings > Plugins > MapLink > Blender > Blender Executable\n\n"
        "MapLink looked in that setting, the MAPLINK_BLENDER_EXE environment "
        "variable, the standard Blender install folders, the Windows .blend file "
        "association, PATH, and the Steam library folders.\n\n"
        "MapLink needs Blender 5.0 or newer.");
}

FString FMapLinkBlenderBridge::GetBlenderExecutablePath()
{
    if (GBlenderResolved)
    {
        return GBlenderExe;
    }
    GBlenderResolved = true;
    GBlenderExe.Empty();

    TArray<FString> Candidates;

    // 1. The setting - the documented place to put it.
    if (const UMapLinkSettings* Settings = UMapLinkSettings::Get())
    {
        AddBlenderCandidate(Candidates, Settings->BlenderExecutable.FilePath);
    }

    // 2. Environment variable. It predates the setting and is still honoured.
    AddBlenderCandidate(Candidates,
        FPlatformMisc::GetEnvironmentVariable(TEXT("MAPLINK_BLENDER_EXE")));

#if PLATFORM_WINDOWS
    // 3. Standard install folders, newest first. %ProgramFiles% is read from
    //    the environment: a hardcoded C: misses every install on another drive.
    {
        TArray<FString> Roots;
        auto AddRoot = [&Roots](const FString& InRoot)
        {
            if (!InRoot.IsEmpty())
            {
                Roots.AddUnique(InRoot);
            }
        };
        AddRoot(FPlatformMisc::GetEnvironmentVariable(TEXT("ProgramW6432")));
        AddRoot(FPlatformMisc::GetEnvironmentVariable(TEXT("ProgramFiles")));
        AddRoot(FPlatformMisc::GetEnvironmentVariable(TEXT("ProgramFiles(x86)")));

        // Down to 3.6: an older Blender is refused by the version gate, and
        // being refused by name beats "Blender not found".
        for (const FString& Root : Roots)
        {
            for (int32 Major = 5; Major >= 3; --Major)
            {
                const int32 MinorStart = (Major == 3) ? 6 : 9;
                const int32 MinorEnd   = (Major == 3) ? 6 : 0;
                for (int32 Minor = MinorStart; Minor >= MinorEnd; --Minor)
                {
                    AddBlenderCandidate(Candidates, FString::Printf(
                        TEXT("%s/Blender Foundation/Blender %d.%d/blender.exe"),
                        *Root, Major, Minor));
                }
            }
        }
    }

    // 4. The Windows .blend association - the one reliable pointer at an
    //    install nobody would think to guess.
    AddBlenderCandidate(Candidates, ExtractExeFromCommandLine(
        RunAndCapture(TEXT("cmd.exe"), TEXT("/c ftype blendfile"))));

    // 5. PATH.
    {
        TArray<FString> Lines;
        RunAndCapture(TEXT("cmd.exe"), TEXT("/c where blender.exe"))
            .ParseIntoArrayLines(Lines);
        for (const FString& Line : Lines)
        {
            AddBlenderCandidate(Candidates, Line);
        }
    }

    // 6. Steam, including its secondary library folders.
    {
        TArray<FString> SteamRoots;
        const FString PF86 =
            FPlatformMisc::GetEnvironmentVariable(TEXT("ProgramFiles(x86)"));
        if (!PF86.IsEmpty())
        {
            SteamRoots.Add(PF86 / TEXT("Steam"));
        }

        // libraryfolders.vdf lists every extra library; its "path" lines are
        // all we need. The array grows as they are read, so index by hand.
        for (int32 i = 0; i < SteamRoots.Num(); ++i)
        {
            const FString Vdf =
                SteamRoots[i] / TEXT("steamapps/libraryfolders.vdf");
            FString Text;
            if (!FFileHelper::LoadFileToString(Text, *Vdf))
            {
                continue;
            }
            TArray<FString> Lines;
            Text.ParseIntoArrayLines(Lines);
            for (const FString& Line : Lines)
            {
                const int32 Key = Line.Find(TEXT("\"path\""));
                if (Key == INDEX_NONE)
                {
                    continue;
                }
                FString Tail = Line.Mid(Key + 6);
                Tail.TrimStartAndEndInline();
                Tail.TrimQuotesInline();
                Tail.ReplaceInline(TEXT("\\\\"), TEXT("/"));
                Tail.TrimStartAndEndInline();
                if (!Tail.IsEmpty())
                {
                    SteamRoots.AddUnique(Tail);
                }
            }
        }

        for (const FString& Root : SteamRoots)
        {
            AddBlenderCandidate(Candidates,
                Root / TEXT("steamapps/common/Blender/blender.exe"));
        }
    }
#elif PLATFORM_MAC
    AddBlenderCandidate(Candidates,
        TEXT("/Applications/Blender.app/Contents/MacOS/Blender"));
    AddBlenderCandidate(Candidates,
        TEXT("/Applications/Blender.app/Contents/MacOS/blender"));
#elif PLATFORM_LINUX
    AddBlenderCandidate(Candidates, TEXT("/usr/bin/blender"));
    AddBlenderCandidate(Candidates, TEXT("/usr/local/bin/blender"));
    AddBlenderCandidate(Candidates,
        RunAndCapture(TEXT("/usr/bin/which"), TEXT("blender")));
#endif

    if (Candidates.Num() > 0)
    {
        GBlenderExe = Candidates[0];
        UE_LOG(LogMapLink, Log,
               TEXT("[Blender] Using '%s' (%d candidate(s) found)."),
               *GBlenderExe, Candidates.Num());
    }
    else
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("[Blender] No blender.exe found. Set it in Project "
                    "Settings > Plugins > MapLink > Blender."));
    }

    return GBlenderExe;
}

bool FMapLinkBlenderBridge::GetBlenderVersion(
    int32& OutMajor, int32& OutMinor, FString& OutVersionString)
{
    if (GBlenderVersionKnown)
    {
        OutMajor         = GBlenderMajor;
        OutMinor         = GBlenderMinor;
        OutVersionString = GBlenderVersionString;
        return GBlenderMajor > 0;
    }

    const FString Exe = GetBlenderExecutablePath();
    if (Exe.IsEmpty())
    {
        return false;
    }

    GBlenderVersionKnown = true;

    int32 RC = -1;
    FString StdOut, StdErr;
    FPlatformProcess::ExecProcess(*Exe, TEXT("--version"), &RC, &StdOut, &StdErr);

    // The first line is "Blender 5.0.1".
    TArray<FString> Lines;
    StdOut.ParseIntoArrayLines(Lines);
    for (const FString& Line : Lines)
    {
        const FString Trimmed = Line.TrimStartAndEnd();
        if (!Trimmed.StartsWith(TEXT("Blender ")))
        {
            continue;
        }
        const FString Ver = Trimmed.RightChop(8).TrimStartAndEnd();
        TArray<FString> Parts;
        Ver.ParseIntoArray(Parts, TEXT("."), true);
        if (Parts.Num() >= 2)
        {
            GBlenderMajor         = FCString::Atoi(*Parts[0]);
            GBlenderMinor         = FCString::Atoi(*Parts[1]);
            GBlenderVersionString = Ver;
            break;
        }
    }

    if (GBlenderMajor <= 0)
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("[Blender] Could not read a version from '%s --version' "
                    "(exit %d) - continuing without the version check."),
               *Exe, RC);
        return false;
    }

    UE_LOG(LogMapLink, Log, TEXT("[Blender] '%s' reports version %s."),
           *Exe, *GBlenderVersionString);

    OutMajor         = GBlenderMajor;
    OutMinor         = GBlenderMinor;
    OutVersionString = GBlenderVersionString;
    return true;
}

bool FMapLinkBlenderBridge::Preflight(FString& OutError, EMapLinkResult& OutResult)
{
    OutResult = EMapLinkResult::Success;
    OutError.Empty();

    const FString Exe = GetBlenderExecutablePath();
    if (Exe.IsEmpty())
    {
        OutResult = EMapLinkResult::BlenderNotFound;
        OutError  = BlenderNotFoundMessage();
        return false;
    }

    int32 Major = 0;
    int32 Minor = 0;
    FString VersionString;
    if (!GetBlenderVersion(Major, Minor, VersionString))
    {
        // Blender is there but would not say which one it is. Not enough to
        // refuse the run - the launch itself reports whatever goes wrong.
        return true;
    }

    const bool bTooOld =
        (Major <  MinBlenderMajor) ||
        (Major == MinBlenderMajor && Minor < MinBlenderMinor);

    if (bTooOld)
    {
        OutResult = EMapLinkResult::BlenderNotFound;
        OutError  = FString::Printf(TEXT(
            "MapLink needs Blender %d.%d or newer.\n\n"
            "Found Blender %s at:\n    %s\n\n"
            "Install Blender %d.%d or newer, or point MapLink at it here:\n"
            "    Project Settings > Plugins > MapLink > Blender > "
            "Blender Executable"),
            MinBlenderMajor, MinBlenderMinor, *VersionString, *Exe,
            MinBlenderMajor, MinBlenderMinor);
        return false;
    }

    return true;
}

void FMapLinkBlenderBridge::ShowBlenderErrorDialog(const FString& StdOutput)
{
    TArray<FString> Lines;
    StdOutput.ParseIntoArrayLines(Lines);

    // v1.9.4 — surface a concise one-line cause at the top of the dialog so
    // the user isn't left scanning a wall of Blender output.  Prefer the
    // last Python "Error:/Exception/Traceback" style line; fall back to the
    // last non-empty line.
    FString Summary;
    for (int32 i = Lines.Num() - 1; i >= 0; --i)
    {
        const FString& L = Lines[i];
        if (L.Contains(TEXT("Error")) || L.Contains(TEXT("Exception")) ||
            L.Contains(TEXT("Traceback")) || L.Contains(TEXT("Failed")))
        {
            Summary = L.TrimStartAndEnd();
            break;
        }
    }
    if (Summary.IsEmpty())
    {
        for (int32 i = Lines.Num() - 1; i >= 0; --i)
        {
            if (!Lines[i].TrimStartAndEnd().IsEmpty())
            {
                Summary = Lines[i].TrimStartAndEnd();
                break;
            }
        }
    }

    // Last 20 lines for the detail block.
    const int32 StartIdx = FMath::Max(0, Lines.Num() - 20);
    FString Tail;
    for (int32 i = StartIdx; i < Lines.Num(); ++i)
    {
        Tail += Lines[i] + TEXT("\n");
    }

    UE_LOG(LogMapLink, Error,
           TEXT("Blender script failed. Cause: %s\nLast output:\n%s"),
           *Summary, *Tail);

    FMessageDialog::Open(EAppMsgType::Ok,
        FText::FromString(FString::Printf(
            TEXT("MapLink: Blender script failed.\n\n"
                 "Likely cause:\n%s\n\n"
                 "Full output (last 20 lines):\n%s"),
            *Summary, *Tail)));
}
