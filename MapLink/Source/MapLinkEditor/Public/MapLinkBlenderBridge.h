// Copyright MapModelers. All Rights Reserved.
//
// v1.7.3 — Added OpenBlenderGUI() and OpenInExplorer().

#pragma once

#include "CoreMinimal.h"
#include "MapLinkTypes.h"

/**
 * Manages launching Blender as a subprocess to run maplink_export.py.
 */
class FMapLinkBlenderBridge
{
public:
    /**
     * Run the Blender headless script in the given mode.
     *
     * v1.7.4: Optional Reason string is recorded in the timing JSON
     *         (e.g. "sidecar_miss", "root_import", "reimport_changed").
     */
    static EMapLinkResult RunBlenderScript(
        const FString& BlendFilePath,
        const FString& PluginBaseDir,
        const FString& Mode,
        const FString& Reason = TEXT("unknown"));

    /**
     * Convenience: run analyze then export in sequence.
     *
     * v1.7.4: Optional Reason string is recorded in the timing JSON.
     */
    static EMapLinkResult RunBlenderExport(
        const FString& BlendFilePath,
        const FString& PluginBaseDir,
        const FString& Reason = TEXT("unknown"));

    /**
     * v1.9 — Run "both"-mode exports for many .blend files in PARALLEL.
     *
     * Launches up to UMapLinkSettings::MaxParallelBlenderExports
     * blender.exe processes at once and waits for all of them, with a
     * single progress dialog.  Used as a warm pass before the sequential
     * per-node import loop: every successful export populates the FBX
     * cache (Exports/<stem>/<stem>.fbx + Logs/<stem>/analysis.json +
     * filehash.txt), so the loop then hits IsFbxCacheFresh() and never
     * launches Blender itself.
     *
     * Failures are safe: a failed export simply leaves the cache stale
     * for that node, and the sequential loop falls back to its own
     * single launch (today's behavior) with full error dialogs.
     *
     * @return Number of failed exports (0 = all succeeded).
     */
    static int32 RunBlenderExportsParallel(
        const TArray<FString>& BlendFilePaths,
        const FString& PluginBaseDir,
        const FString& Reason = TEXT("warm_pass"));

    /**
     * Run an arbitrary Blender headless script with custom arguments.
     */
    static EMapLinkResult RunBlenderScriptGeneric(
        const FString& BlendFilePath,
        const FString& ScriptFilename,
        const FString& PluginBaseDir,
        const FString& ExtraArgs,
        const FString& TaskName = TEXT("Blender Script"));

    // -----------------------------------------------------------------
    // v1.7.3 — Interactive Blender / Explorer launch
    // -----------------------------------------------------------------

    /**
     * Open a .blend file in the Blender GUI (non-headless, detached).
     * The Blender process runs independently from UE5.
     *
     * @param BlendFilePath  Absolute path to the .blend file.
     * @return Success or BlenderNotFound.
     */
    static EMapLinkResult OpenBlenderGUI(const FString& BlendFilePath);

    /**
     * Open the folder containing the given file in the OS file explorer.
     *
     * @param FilePath  Absolute path to a file (its parent dir is opened).
     */
    static void OpenInExplorer(const FString& FilePath);

    // -----------------------------------------------------------------
    // v1.21 — Finding Blender, and knowing which Blender
    // -----------------------------------------------------------------

    /** Minimum Blender this build of MapLink supports. */
    static constexpr int32 MinBlenderMajor = 5;
    static constexpr int32 MinBlenderMinor = 0;

    /**
     * Locate blender.exe.  Resolution order:
     *   1. Project Settings > Plugins > MapLink > Blender > Blender Executable
     *   2. The MAPLINK_BLENDER_EXE environment variable
     *   3. Standard install folders under %ProgramFiles% (newest first)
     *   4. The Windows .blend file association
     *   5. blender.exe on PATH
     *   6. Steam library folders
     *
     * Resolved once and cached for the editor session; editing the setting
     * calls InvalidateBlenderCache().  Empty string when nothing was found.
     */
    static FString GetBlenderExecutablePath();

    /** v1.21 — Drop the cached executable and version (the setting changed). */
    static void InvalidateBlenderCache();

    /**
     * v1.21 — Version of the resolved Blender, read once from
     * `blender --version` and cached.  False when Blender could not be run
     * or its output could not be parsed — which is not by itself a reason
     * to refuse the launch.
     */
    static bool GetBlenderVersion(int32& OutMajor, int32& OutMinor,
                                  FString& OutVersionString);

    /**
     * v1.21 — Resolve and validate Blender ONCE, at the top of a run that
     * would otherwise launch it per node.  On failure, fills OutError with
     * a message naming the setting to fix and OutResult with the reason.
     *
     * Call this before a multi-node import/reimport and abort the whole run
     * on false: without it the "Blender not found" dialog fires once per
     * stale node.
     */
    static bool Preflight(FString& OutError, EMapLinkResult& OutResult);

private:
    /** Show the last N lines of Blender stdout on failure. */
    static void ShowBlenderErrorDialog(const FString& StdOutput);

    /** v1.21 — The one "we could not find Blender" message, so the six
     *  launch sites cannot drift apart. */
    static FString BlenderNotFoundMessage();
};
