// Copyright MapModelers. All Rights Reserved.
//
// v1.8 — BP Reference Mode — Content Browser right-click integration.
//
// Adds a "MapLink" submenu to the Blueprint asset context menu with five
// entries whose visibility is computed per-asset:
//
//   - Create MapLink Stand-in    (visible: actor-derived BP, no binding,
//                                          not under /MapLink/)
//   - Refresh MapLink Stand-in   (visible: binding exists AND .blend exists)
//   - Open Stand-in in Blender   (visible: binding exists AND .blend exists)
//   - Unlink MapLink Stand-in    (visible: binding exists)
//   - Show Stand-in in Explorer  (visible: .blend exists on disk)
//
// Registered from MapLinkEditorModule::StartupModule via
// FMapLinkContentBrowserExtensions::Register().

#pragma once

#include "CoreMinimal.h"

class FMapLinkContentBrowserExtensions
{
public:
    /** Hook into UToolMenus to add the MapLink submenu.  Idempotent —
     *  calling Register twice is a no-op. */
    static void Register();

    /** Detach from UToolMenus. */
    static void Unregister();
};
