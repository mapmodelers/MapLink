// Copyright MapModelers. All Rights Reserved.
//
// v1.16 — Console commands for the texture manager.
//
// The codebase contained no FAutoConsoleCommand anywhere before this. Adding
// one is a new pattern, chosen deliberately: the data layer needs to be
// exercisable before any UI exists, and a console command is the smallest
// surface that can do it without committing to a tab layout that Phase 2 will
// want to change anyway.
//
// Registered from FMapLinkEditorModule::StartupModule and torn down in
// ShutdownModule, matching the Register/Unregister discipline every other
// MapLink subsystem already follows.
//
//   MapLink.Textures.Scan
//   MapLink.Textures.Report        [-verbose]
//   MapLink.Textures.Rescan
//   MapLink.Textures.Classify      <AssetPath> <MapLink|External|Unclassified>
//   MapLink.Textures.AutoClassify  [-execute]
//   MapLink.Textures.Conform       <AssetPath|all> [-execute]
//   MapLink.Textures.Claim         <AssetPath> <Fork|Migrate|Adopt|Copy> [-execute]
//   MapLink.Textures.Promote       <AssetPath> [-execute]
//   MapLink.Textures.Orphans       [-delete] [-execute]
//   MapLink.Textures.SelfTest
//
// ABSENT -execute, EVERYTHING IS A DRY RUN.

#pragma once

#include "CoreMinimal.h"

class FMapLinkTextureConsole
{
public:
    /** Idempotent — calling twice is a no-op. */
    static void Register();
    static void Unregister();
};
