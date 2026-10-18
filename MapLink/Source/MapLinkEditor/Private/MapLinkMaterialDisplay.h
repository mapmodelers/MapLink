// Copyright MapModelers. All Rights Reserved.
//
// v1.7.3 — Material Instance display level generator.

#pragma once

#include "CoreMinimal.h"
#include "MapLinkTypes.h"

/**
 * Creates/updates a UE5 level with a grid of display cubes, one per
 * Material Instance found in the configured scan path.  Each cube shows
 * the MI on its outer shell (Element 0).
 *
 * Content in the "Persistent" outliner folder is preserved across updates.
 */
class FMapLinkMaterialDisplay
{
public:
    /**
     * Main entry point — scans for MIs, creates or loads the display level,
     * removes stale display cubes, spawns fresh ones in a grid, and saves.
     *
     * @return Success, NoMaterialInstancesFound, or MaterialDisplay* error codes.
     */
    static EMapLinkResult UpdateMaterialDisplayLevel();
};
