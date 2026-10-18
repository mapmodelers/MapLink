// Copyright MapModelers. All Rights Reserved.
//
// v1.20 — THE MapLink colour code. One file, both applications.
//
// These five accents identify an asset's TYPE, and they have to mean the same
// thing wherever the artist sees them: the viewport name overlay, the Manager,
// the asset type icons, and Blender's Outliner collection tags. A colour code
// that disagrees with itself across the two halves of the pipeline is worse
// than no colour code, because the artist learns it once and is then wrong.
//
// MIRRORED IN: ue/blender/theme_colors.py (SWATCH_ACCENTS). That file writes
// the same hexes into Blender's collection colour theme. Change one, change
// the other — they are two runtimes, not two opinions.
//
// Authored in sRGB hex because that is what the icon artwork is authored in;
// stored linear because that is what FLinearColor is. The conversion is
// exact and done here so no call site is ever tempted to eyeball it.

#pragma once

#include "CoreMinimal.h"

namespace MapLinkPalette
{
    /** sRGB hex, for documentation, icon authoring, and the Blender mirror. */
    inline const TCHAR* HexLI        = TEXT("#E4872A");   // amber
    inline const TCHAR* HexSM        = TEXT("#18CAB9");   // teal
    inline const TCHAR* HexBP        = TEXT("#2C7EB8");   // azure
    inline const TCHAR* HexAttention = TEXT("#D53C3D");   // red
    inline const TCHAR* HexNoExport  = TEXT("#8B7355");   // drab

    /**
     * The same five as linear FLinearColor.
     *
     * "Attention" rather than "OutOfDate" or "Missing": Unreal spends it on a
     * stale asset and Blender on a broken link, and naming it after either
     * one is how a shared palette entry quietly becomes two.
     */
    inline const FLinearColor LI        (0.775822f, 0.242281f, 0.023153f);
    inline const FLinearColor SM        (0.009134f, 0.590619f, 0.485150f);
    inline const FLinearColor BP        (0.025187f, 0.208637f, 0.479320f);
    inline const FLinearColor Attention (0.665387f, 0.045186f, 0.046665f);
    inline const FLinearColor NoExport  (0.254152f, 0.170138f, 0.099899f);

    /**
     * Bumped whenever a value above changes.
     *
     * UMapLinkSettings' colours are Config properties, so an ini written
     * before this palette existed OVERRIDES the code defaults and keeps
     * overriding them forever — which is exactly what happened: a test project's
     * DefaultGame.ini carried cream / pink / purple from the v1.15.1
     * user-picked defaults, so the shipped colour code never appeared on a
     * project that had ever saved its settings. Comparing this number against
     * the one in the ini is how a stale palette heals itself, once, without
     * trampling a customisation somebody made on purpose.
     */
    inline constexpr int32 Version = 1;
}
