// Copyright MapModelers. All Rights Reserved.
//
// v1.12 — Material Library import/export (cross-project reuse).
//
// Export: pick Material Instances (checkbox list, Select All) → a bundle
// folder "<Name>_MapLinkMatLib/" containing the copied .uasset files of the
// MIs + their /Game/ dependency closure (parents, textures) at their
// /Game/-relative paths, plus a matlib_manifest.json.
//
// Import: pick a matlib_manifest.json → pick MIs → merge the required
// packages into this project at their canonical /Game/MapLink/... paths,
// with a Skip-existing (default) or Overwrite-existing conflict policy.
// The import ends in FMapLinkMaterialExporter::SyncAfterOp so the Blender
// libraries regenerate through the normal invisible engine — sync is never
// a button.
//
// Format notes (deliberate):
//  - uasset copy == full fidelity, UE-version bound (same as UE Migrate).
//  - Package identity is baked inside a .uasset, so bundles restore to the
//    SAME package path; an "import renamed" policy is impossible at file
//    level and is intentionally not offered.
//  - Packages under the plugin mount ("/MapLink/...") ship with the plugin
//    and are never bundled — only /Game/ packages travel.

#pragma once

#include "CoreMinimal.h"

/** What to do when an incoming package already exists in this project. */
enum class EMapLinkMatLibConflict : uint8
{
    SkipExisting,
    Overwrite,
};

/** Outcome of a non-dialog import. */
struct FMapLinkMatLibImportResult
{
    bool bOk = false;
    int32 Imported = 0;
    int32 Overwritten = 0;
    int32 Skipped = 0;
    TArray<FString> Failed;
    FString Error;
};

class FMapLinkMaterialLibraryExchange
{
public:
    /** Modal export flow. All feedback via dialogs. Game thread only. */
    static void ShowExportDialog();

    /** Modal import flow. All feedback via dialogs. Game thread only. */
    static void ShowImportDialog();

    /**
     * Modal "rebuild Material Instances from material_instances.json" flow.
     *
     * A different operation from Import, and deliberately a separate entry.
     * Import restores .uasset files: full fidelity, nothing is recomputed.
     * This synthesises assets from recorded parameter VALUES, so it can only
     * ever be as good as what the JSON captured -- parents are matched by
     * name, textures by filename against what is already in the project, and
     * anything unresolved keeps the parent's default.
     *
     * Textures are never imported from the disk paths in the file. Those are
     * absolute paths from whichever machine wrote it, so honouring them would
     * make the result depend on a folder layout the project does not own.
     */
    static void ShowRebuildFromJsonDialog();

    // ── v1.19 — the same two operations, without a dialog ──────────────
    //
    // Export and import existed only as modal flows, which made "give this
    // asset's materials to that project" a thing a human had to drive through
    // two file pickers and a checkbox tree. These are the same code paths with
    // the UI lifted off: the dialogs now call them, so there is one
    // implementation rather than a scriptable copy that drifts.

    /**
     * Bundle exactly the named Material Instances (plus their /Game/ closure)
     * into OutDir.  MINames may be bare asset names or long package names;
     * bare names are resolved against the project's Material Instances path.
     *
     * Scoped to the MIs one asset actually uses, instead of the checkbox tree.
     *
     * @return true on success, with OutManifestPath set.
     */
    static bool ExportForAssets(
        const TArray<FString>& MINames,
        const FString& OutDir,
        const FString& LibraryName,
        FString& OutManifestPath,
        FString& OutError);

    /**
     * Import a bundle without a dialog.
     *
     * @param OnlyMIs  Restrict to these MIs (bare or long names). Empty = all
     *                 the manifest lists.
     */
    static FMapLinkMatLibImportResult ImportFromManifest(
        const FString& ManifestPath,
        EMapLinkMatLibConflict Policy,
        const TArray<FString>& OnlyMIs = TArray<FString>());

    /**
     * Surface <BlenderRoot>/MaterialInstances/pending_matlib_import.json, if
     * the Blender addon left one there.
     *
     * Vendor-in copies an asset between MapLink roots as files; the Material
     * Instances it uses are UE assets and cannot travel that way, so the addon
     * records the names the destination is missing. This is UE noticing.
     * Nothing is imported automatically -- the names alone do not say which
     * project to take them from.
     */
    static void ProcessPendingImport();

    /** Bundle manifest filename ("matlib_manifest.json"). */
    static const TCHAR* GetManifestFilename();

    /** Filename the Blender addon writes for vendor-in leftovers. */
    static const TCHAR* GetPendingImportFilename();

private:
    /** Recursive /Game/-only hard package dependency closure of a package
     *  (includes the package itself). */
    static void CollectPackageClosure(
        const FName PackageName, TSet<FName>& InOutClosure);

    /** Closure of every MI in *MIPackages*, plus the per-MI dep list. */
    static void BuildClosureForMIs(
        const TArray<FString>& MIPackages,
        TSet<FName>& OutClosure,
        TMap<FString, TArray<FString>>& OutPerMIDeps);

    /** Bare asset name -> long package name under the MI content path.
     *  Passes a long package name through untouched. */
    static FString ResolveMIPackage(const FString& NameOrPackage);
};
