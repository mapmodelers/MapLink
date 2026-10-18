// Copyright MapModelers. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MapLinkTypes.h"
#include "Logging/LogMacros.h"

/**
 * v1.8 — Dedicated log category for BP-Reference-Mode lifecycle ops
 *        (create / refresh / delete / rename / duplicate-scrub /
 *        reimport-swap).  Separate from LogMapLink so users can filter
 *        BP stand-in operations without drowning in the rest of the
 *        pipeline's chatter.
 */
DECLARE_LOG_CATEGORY_EXTERN(LogMapLinkBPStandIn, Log, All);

/**
 * Shared utility functions used across multiple MapLink modules.
 * Consolidates previously-duplicated code (MD5 hashing, Blender suffix
 * stripping, swap log writing, safe plugin directory access).
 */
class FMapLinkUtils
{
public:
    // -----------------------------------------------------------------
    // v1.10 — Single MapLink content root (merge seam)
    // -----------------------------------------------------------------

    /** The one canonical UE content root for everything MapLink generates.
     *  Flat namespace, always "/Game/MapLink/" (trailing slash included).
     *  Every MIM content path is composed from this — it is the merge seam,
     *  so it must never be branched per-feature or per-project. */
    static const FString& GetMapLinkContentRoot();

    /** Canonical package path for Material Instances:
     *  "/Game/MapLink/MaterialInstances" (no trailing slash, ready for
     *  FName package-path use / AssetRegistry filters). */
    static FString GetMaterialInstancesContentPath();

    /**
     * True when *Path* resolves inside *Root*.
     *
     * v1.19 — the C++ half of `link_scope.is_under()`. Case-insensitive and
     * separator-normalised, and it compares whole path components: "…/Foo"
     * is NOT under "…/Fo", which a bare StartsWith would say it was.
     */
    static bool IsPathUnder(const FString& Path, const FString& Root);

    /** The MapLink plugin's mounted content root: "/MapLink/" (trailing
     *  slash). Shipped parent materials (M_*) live under here. */
    static const FString& GetMapLinkPluginContentRoot();

    /** True if a material package/object path belongs to MapLink — i.e. it
     *  is under the plugin content root ("/MapLink/") or the project content
     *  root ("/Game/MapLink/").  Used to restrict MI parent pickers to
     *  MapLink-managed parents only. */
    static bool IsMapLinkMaterialPath(const FString& PackageOrObjectPath);

    /** Compute MD5 hash of a file. Returns empty string on failure. */
    static FString ComputeFileMD5(const FString& FilePath);

    /** Strip Blender duplicate suffix (.001, .002, …) from a name.
     *  "LI_Test_01.002" → "LI_Test_01"
     *  "LI_Test_01"     → "LI_Test_01" (unchanged) */
    static FString StripBlenderSuffix(const FString& Name);

    /** Write a swap log JSON to <ArtifactLogDir>/<BlendName>/<LogFileName>. */
    static void WriteSwapLog(
        const FString& BlendName,
        const TArray<FMapLinkStubRecord>& Records,
        const FString& LogFileName);

    /** Get the plugin base directory with null-safety.
     *  Falls back to <ProjectPlugins>/MapLink if the plugin isn't found.
     *  v1.13: for locating shipped files ONLY (Scripts/, blender/, Content).
     *  Never compose generated-artifact paths from this — the plugin dir is
     *  read-only for engine-installed marketplace plugins.  Use the
     *  GetArtifact*Dir() accessors below instead. */
    static FString GetPluginBaseDir();

    // -----------------------------------------------------------------
    // v1.13 — Generated artifacts live under <ProjectSaved>/MapLink/
    // -----------------------------------------------------------------
    // The plugin directory can be read-only (engine-installed marketplace
    // plugin), so every generated artifact (analysis/log/hash caches, FBX
    // exports, lock files) is written under the project's Saved dir:
    //     <ProjectSaved>/MapLink/Logs/...
    //     <ProjectSaved>/MapLink/Export/...
    // Blender-side workers write to the same root: UE passes it as
    // --outdir to maplink_export.py, and feeds the addon's
    // maplink_artifact_root preference at addon install.
    //
    // Migration (one release of grace): reads PREFER the new location and
    // fall back to the legacy <Plugin>/Logs and <Plugin>/Exports (also
    // <Plugin>/Export) paths. Writes NEVER target the old locations.
    //
    // Per-iteration cleanup note: the dev cleanup script should clear
    // <ProjectSaved>/MapLink (plus the legacy <Plugin>/Logs|Export(s)
    // while the grace fallback exists).

    /** Artifact root: <ProjectSaved>/MapLink (absolute). */
    static FString GetArtifactRootDir();

    /** Artifact log dir: <ProjectSaved>/MapLink/Logs (absolute). */
    static FString GetArtifactLogDir();

    /** Artifact export dir: <ProjectSaved>/MapLink/Export (absolute). */
    static FString GetArtifactExportDir();

    /** Resolve a log-artifact file for READING with migration grace:
     *  returns <ArtifactLogDir>/RelPath if it exists, else the legacy
     *  <Plugin>/Logs/RelPath if that exists, else the new path.
     *  WRITES must always use GetArtifactLogDir() directly. */
    static FString ResolveArtifactLogFile(const FString& RelPath);

    /** Resolve an export-artifact file for READING with migration grace:
     *  new <ArtifactExportDir>/RelPath first, then legacy
     *  <Plugin>/Exports/RelPath, then <Plugin>/Export/RelPath.
     *  WRITES must always use GetArtifactExportDir() directly. */
    static FString ResolveArtifactExportFile(const FString& RelPath);

    // -----------------------------------------------------------------
    // v1.16 — Texture Manager directories (inside the MapLink project)
    // -----------------------------------------------------------------
    // These live under the MapLink *Blender project* root
    // (UMapLinkSettings::BlenderProjectRoot), NOT under Saved/ and NOT under
    // the plugin dir: they are P4-tracked production data the whole team
    // syncs, not per-machine generated artifacts.
    //
    // Both return an empty string when the project root is unresolved, and
    // neither ever prompts — they call ResolveBlenderProjectRoot(false,false)
    // so a background scan can never pop a directory picker.

    /** <Root>/Textures — authoritative MapLink-owned .tga files.
     *  Empty string if the MapLink project root is unresolved. */
    static FString GetTextureSourceDir();

    /** <Root>/TexturePreviews — Blender-facing derivative PNGs.
     *  Empty string if the MapLink project root is unresolved.
     *
     *  NOTE the name: the word "Proxy" is reserved for UMapLinkProxyAsset and
     *  the WP expansion system.  These files are previews, never proxies, and
     *  no symbol in the texture feature spells "Proxy" in any form. */
    static FString GetTexturePreviewDir();

    /** <Root>/TexturePreviews/External — previews of External-class textures
     *  (Phase 2 populates this; v1.16 only reserves and ignores it). */
    static FString GetExternalTexturePreviewDir();

    /** <Root>/TexturePreviews/MapLink — reserved for a possible future
     *  all-PNG mode.  Stays empty in v1.16 and is ignored by every scan. */
    static FString GetMapLinkTexturePreviewDir();

    /** v1.15 — true if <BlendName>'s analysis.json declares own geometry
     *  (non-empty blend_own_geo array).  Returns false when the analysis
     *  is missing or unparsable — callers must combine with their own
     *  analysis validity check. */
    static bool AnalysisDeclaresOwnGeo(const FString& BlendName);

    // -----------------------------------------------------------------
    // v1.7.5 — FBX-export-on-save cache
    // -----------------------------------------------------------------

    /** Return true if the Blender artifacts in <ArtifactLogDir>/<BlendName>/
     *  (legacy plugin dir honoured for reads, v1.13) are up-to-date with the
     *  current .blend file:
     *    - filehash.txt matches MD5(BlendPath)
     *    - analysis.json exists
     *  When true, the caller may skip the Phase1 Blender launch entirely;
     *  Phase2 (ImportAndCollectResults) will read the cached artifacts.
     *  These artifacts are normally produced either by a previous UE-side
     *  Blender launch, or by the MapLink_Blender addon v1.4.0+ save_post
     *  subprocess (which is what eliminates cold-import launches). */
    static bool IsFbxCacheFresh(
        const FString& BlendPath,
        const FString& BlendName);

    /** If <ArtifactExportDir>/<BlendName>/.export.lock (or its legacy
     *  in-plugin location) exists and is fresh (<60 s old), block up to
     *  TimeoutSec waiting for it to disappear.
     *  Stale locks are deleted; missing locks return immediately. */
    static void WaitForExportLock(
        const FString& BlendName,
        double         TimeoutSec = 8.0);
};
