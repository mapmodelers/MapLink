// Copyright MapModelers. All Rights Reserved.
//
// MapLink Project lifecycle — New / Open / Unlink.
//
// "New MapLink Project…" is the UE-driven scaffolding of a fresh, portable,
// marketplace-clean set of MapLink .blends from the plugin's bundled
// templates: a modal dialog (project name / root), then
// maplink_project_setup.py headless via FMapLinkBlenderBridge lays down the
// directory tree (LI/SM/BP/MaterialInstances/Textures), the top-level
// LI_<SetupName>, the seeded SM_/BP_ templates, the project-local material
// libraries and the manifest.  On success it applies the manifest to
// UMapLinkSettings, best-effort builds a non-WP MaterialInstances library
// level (a mat_cube grid), and adds the new tree to source control.  It does
// NOT touch the user's gameplay level — the user picks that themselves.
//
// "Open MapLink Project…" attaches this UE project to an EXISTING MapLink
// root (new machine, P4 sync, moved root): pick the root folder, find the
// maplink_project.json manifest, apply it to settings.
//
// "Unlink Project" detaches the UE project — keeping all data (default) or
// optionally deleting the Blender root and /Game/MapLink content entirely.

#pragma once

#include "CoreMinimal.h"

/** v1.12 — lifecycle state of the MapLink project link in this UE project. */
enum class EMapLinkProjectState : uint8
{
    /** No root configured and no MapLink proxies in content — clean slate. */
    NoProject,
    /** Root configured and its manifest exists on disk — fully linked. */
    Active,
    /** No valid root/manifest, BUT MapLink proxies exist in content — the
     *  authoring root was moved, deleted or not synced. Recover via
     *  "Open MapLink Project…" (locate the root), not via New. */
    Broken,
};

class FMapLinkProjectSetup
{
public:
    /**
     * Show the modal "New MapLink Project" dialog and, on confirmation, run
     * the scaffolding.  Safe to call from a menu lambda on the game thread.
     * All user feedback (success / failure) is surfaced via dialogs; nothing
     * is returned.
     */
    static void ShowNewProjectDialog();

    /**
     * v1.12 — "Open MapLink Project…": directory picker → locate
     * maplink_project.json (walks up from the picked folder) → apply it to
     * UMapLinkSettings (root + material library paths, persisted).  All
     * feedback via dialogs.  Safe to call from a menu lambda.
     */
    static void ShowOpenProjectDialog();

    /**
     * v1.12 — "Unlink Project".  Prompts with two modes:
     *   Unlink only        — clear the settings; every file stays; re-attach
     *                        anytime with ShowOpenProjectDialog().
     *   Unlink + delete all — additionally delete the Blender root tree on
     *                        disk and all /Game/MapLink content (double
     *                        confirmation; refused while a /Game/MapLink
     *                        level is open).
     */
    static void UnlinkProject();

    /**
     * v1.12 — full lifecycle state (NoProject / Active / Broken).  Broken =
     * no valid root/manifest but MapLink proxy assets exist in content.
     * Optionally returns the active root (Active only).  Non-prompting; the
     * asset-registry probe is throttled, so it is safe in UI predicates.
     */
    static EMapLinkProjectState GetProjectState(FString* OutRoot = nullptr);

    /**
     * True if this UE project already has a MapLink setup applied: the
     * BlenderProjectRoot setting is populated AND its maplink_project.json
     * manifest exists on disk. Returns false (setup re-enabled) when the root
     * is unset or the manifest is missing — e.g. the authoring root was moved
     * or deleted — so a stale link never bricks setup. Optionally returns the
     * active root. Non-prompting, cheap, safe to call from UI predicates.
     */
    static bool IsSetupActive(FString* OutRoot = nullptr);

    /**
     * Install (or update in place) and enable the MapLink Blender addon into
     * the user's Blender, then enable it and set its UE-plugin-dir preference.
     *
     * Resolves the Blender executable the same way as the rest of the plugin
     * (FMapLinkBlenderBridge::GetBlenderExecutablePath — MAPLINK_BLENDER_EXE env
     * var, then default install locations), and derives the addon source and
     * plugin paths from the plugin's own base dir — so there is nothing machine-
     * or user-specific.  Runs the bundled Scripts/maplink_install_addon.py
     * headless WITHOUT --factory-startup (saving prefs under factory-startup
     * would clobber the user's settings).  All feedback is via dialogs.
     */
    static void InstallBlenderAddon();

    /**
     * Open the project's Material Instance library .blend
     * (Library_MaterialInstances.blend) in the Blender GUI. Resolves the path
     * from settings (Material Instances .blend), falling back to
     * <ProjectRoot>/MaterialInstances/Library_MaterialInstances.blend.
     */
    static void OpenMaterialInstanceLibrary();

    /**
     * Open the Material Instance display level
     * (/Game/MapLink/MaterialInstances/L_MaterialInstances.umap) in the editor,
     * prompting to save any modified packages first.
     */
    static void OpenMaterialInstancesLevel();
};
