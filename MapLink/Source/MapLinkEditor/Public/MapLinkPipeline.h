// Copyright MapModelers. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MapLinkTypes.h"

class UMapLinkProxyAsset;

DECLARE_LOG_CATEGORY_EXTERN(LogMapLink, Log, All);

/** v1.11 — Broadcast around the destructive reimport (nuke-and-recreate).
 *  Payload is the root .blend stem being reimported. Consumers (e.g. the
 *  Asset Viewer) pause on Begin and refresh on End. */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnMapLinkReimport, const FString& /*Stem*/);

/**
 * v1.21 - the failures of one import or reimport run.
 *
 * A node that fails does not abort the run: the remaining nodes still
 * import, which is the behaviour that makes a broken dependency survivable.
 * What it must not do is disappear. Before this, every per-node failure was
 * logged, skipped, and then the run returned Success - so the user got
 * "Reimport complete", a green badge in Blender, and no idea a file had
 * died. Failures are collected here, the run returns the first one, and one
 * summary names them all.
 */
class FMapLinkRunFailures
{
public:
    /** Start a run. Nested runs are counted, not restarted. */
    static void Begin();

    /** End a run; the list clears when the outermost run ends. */
    static void End();

    /** Record one failed node. */
    static void Add(const FString& NodeName, EMapLinkResult Code,
                    const FString& Detail);

    static bool  HasAny();
    static int32 Num();

    /** The first failure of the run - what RunImport/RunReimport returns. */
    static EMapLinkResult FirstResult();

    /** One modal naming every failed node. No-op when there are none. */
    static void ShowSummary(const FString& Operation, const FString& Subject);

    /** True when ShowSummary() has already told the user about this run.
     *  A caller with its own "it failed" dialog should skip it rather than
     *  open a second one over the same failure. Cleared by the next Begin(). */
    static bool WasSummaryShown();
};

/** RAII wrapper for FMapLinkRunFailures::Begin/End. */
struct FMapLinkRunFailureScope
{
    FMapLinkRunFailureScope()  { FMapLinkRunFailures::Begin(); }
    ~FMapLinkRunFailureScope() { FMapLinkRunFailures::End();   }
};

/**
 * Orchestrates the full MapLink import pipeline:
 *   dependency graph → Phase 1 (Blender) → Phase 2 (FBX/BP)
 *   → Phase 2C/2D (swap) → Phase 3 (post-import).
 */
class FMapLinkPipeline
{
public:
    static EMapLinkResult RunImport(const FString& BlendFilePath);
    static EMapLinkResult RunReimport(const FString& BlendFilePath);
    static void OpenPrimaryAsset(const FString& BlendName, bool bIsLevel);

    /** v1.11 — reimport begin/end signals (see FOnMapLinkReimport). */
    static FOnMapLinkReimport& OnReimportBegin();
    static FOnMapLinkReimport& OnReimportEnd();

    /** Get the plugin base directory. Public for use by Reimport. */
    static FString GetPluginBaseDir();

    /**
     * Copy a .blend from outside the project — with everything it links —
     * into *DestRoot*, by running the Blender addon's vendor-in operator
     * headlessly.
     *
     * v1.19. UE does not reimplement the walk: `vendor_in.py` already owns
     * "which folders does this asset need", the conflict policy, the
     * post-copy path conforming and the material-instance reporting. A second
     * implementation in C++ would agree with it until the first time somebody
     * fixed a bug in one of them. So this launches Blender and calls the
     * operator, exactly as an artist would.
     *
     * @param OutVendoredBlend  Path of the copy, to import instead of the original.
     * @return true on success; OutError carries the reason otherwise.
     */
    static bool VendorBlendIntoProject(
        const FString& SourceBlend,
        const FString& DestRoot,
        bool bOverwrite,
        FString& OutVendoredBlend,
        FString& OutError);

    /** Save a proxy asset package to disk. Public for use by Reimport. */
    static void SaveProxyPackage(UMapLinkProxyAsset* Proxy);

    /**
     * Find the proxy created for the given .blend (deterministic path
     * /Game/MapLink/<stem>/<stem>_Proxy) and set its bShowInReimportMenu flag,
     * persisting via SaveProxyPackage (P4-routed when connected). No-op if the
     * proxy doesn't exist yet. Returns true if a proxy was found and updated.
     */
    static bool SetProxyShowInReimport(const FString& BlendFilePath, bool bShow);

private:
    /** Internal reimport implementation (defined in MapLinkReimport.cpp). */
    static EMapLinkResult RunReimportImpl(const FString& BlendFilePath);

    /** v1.13 — import body; RunImport() wraps it with the UE-status file
     *  writes (importing → ok/failed) for the Blender addon badge. */
    static EMapLinkResult RunImportImpl(const FString& BlendFilePath);

    static void LogError(const FString& BlendName, const FString& Phase,
                         EMapLinkResult Code, const FString& Message);
    static FString ReadHashFromLogFile(const FString& BlendName);
};
