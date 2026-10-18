// Copyright MapModelers. All Rights Reserved.
//
// v1.8 — BP Reference Mode — reimport / swap-time helpers.
//
// One central namespace consulted by every code path that previously
// assumed BP_ → "/Game/MapLink/<name>/<name>" synthesis.  In reference
// mode the synthesis is skipped: at swap / expansion time, the bound
// user-authored Blueprint is substituted instead, or BP_Deprecated as
// a fallback when the bound BP is missing.
//
// Three states a BP_ node can be in:
//   - LEGACY        : sidecar lacks bound_blueprint_path → existing pipeline
//   - REFERENCE_OK  : bound BP resolves to a loadable UClass with GeneratedClass
//   - REFERENCE_MISSING : bound BP path is set but no loadable class → fall back
//                         to BP_Deprecated, tag the actor MapLinkMissingBP:<path>

#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class UClass;
class UMapLinkProxyAsset;

namespace FMapLinkBPReferenceMode
{
    // -----------------------------------------------------------------
    // Constants
    // -----------------------------------------------------------------

    /** Canonical path to the deprecated-BP fallback under the plugin's
     *  Content/Blueprints/.  Used when a bound BP can't be resolved. */
    extern const TCHAR* BP_DEPRECATED_PRIMARY_PATH;

    /** Secondary path checked if the canonical one isn't found — lets
     *  early testing use a project-side BP_Deprecated.uasset before
     *  it's moved into the plugin Content/. */
    extern const TCHAR* BP_DEPRECATED_FALLBACK_PATH;

    // -----------------------------------------------------------------
    // Sidecar parsing (used at Pipeline::RunImport time)
    // -----------------------------------------------------------------

    /** Parse the sidecar <blend_dir>/<stem>_analysis.json for the
     *  v1.8 fields bound_blueprint_path + standin_mode.
     *  @return true if the sidecar exists, parses, AND contains a
     *          non-empty bound_blueprint_path. */
    bool ParseSidecarForBoundBP(
        const FString& BlendFilePath,
        FString&       OutBoundBPPath,
        FString&       OutStandInMode);

    // -----------------------------------------------------------------
    // Proxy inspection (used at swap / expansion / reimport time)
    // -----------------------------------------------------------------

    /** Load the proxy at /Game/MapLink/<NodeBlendName>/<NodeBlendName>_Proxy
     *  and check whether it's a reference-mode proxy.
     *  @return true if proxy exists AND bIsBPReference == true. */
    bool IsProxyReferenceMode(
        const FString& NodeBlendName,
        FString&       OutBoundBPPath,
        UMapLinkProxyAsset** OutProxy = nullptr);

    /** Resolve a reference-mode proxy to the actor UClass to spawn.
     *  Tries Proxy->BoundBlueprint first; falls back to BP_Deprecated.
     *  @return UClass to spawn, or nullptr if even BP_Deprecated is
     *          missing (caller should leave the placeholder).
     *  @param OutbWasDeprecated set true if the fallback was used. */
    UClass* ResolveBoundClass(
        UMapLinkProxyAsset* Proxy,
        bool&               OutbWasDeprecated);

    /** Same as above but takes the bound BP path directly — handy when
     *  caller has the path but no proxy in hand (e.g. early in pipeline). */
    UClass* ResolveBoundClassFromPath(
        const FString& BoundBPPath,
        bool&          OutbWasDeprecated);

    // -----------------------------------------------------------------
    // Placement reporting
    // -----------------------------------------------------------------

    enum class EPlacementStatus : uint8
    {
        Ok,                   // bound BP resolved cleanly
        DeprecatedSubstituted,// BP_Deprecated stood in for a missing BP
        Missing,              // not even BP_Deprecated could be resolved
    };

    struct FPlacement
    {
        FString ProxyPath;          // /Game/MapLink/<name>/<name>_Proxy
        FString BoundBlueprintPath; // /Game/.../BP_X
        FString PlacementContext;   // "level_swap" / "bp_nested" / "wp_expansion"
        FString OwnerAssetPath;     // /Game/MapLink/<owner_level>  or  <owner_BP>
        EPlacementStatus Status = EPlacementStatus::Ok;
    };

    /** Append a placement record to the per-import accumulator. */
    void RecordPlacement(const FPlacement& Placement);

    /** Flush all accumulated placements to a JSON file under the
     *  plugin's Logs dir, then clear the accumulator.  Called once at
     *  end of FMapLinkPipeline::RunImport.  No-op if no placements
     *  were recorded. */
    void WriteAndClearPlacementsReport(const FString& RootBlendName);
}
