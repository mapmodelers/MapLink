// Copyright MapModelers. All Rights Reserved.
//
// v1.13 — Editor feedback policy helpers.
//
// POLICY: non-decision feedback goes through editor toasts, NOT modal
// dialogs.  Modal dialogs (FMessageDialog / SCustomDialog) are reserved
// for exactly three cases:
//   1. Decisions            (the user picks between courses of action)
//   2. Destructive confirms (delete / nuke / unlink)
//   3. Hard failures that BLOCK an action the user just requested
// Everything else — success confirmations, progress endpoints, non-fatal
// warnings with no user choice — uses MapLinkNotify::Success/Warn/Fail.
//
// MapLinkResultToText() is the single source of human-readable strings for
// EMapLinkResult. Use it wherever a result code reaches the user — never
// show a raw "(code %d)" again.

#pragma once

#include "CoreMinimal.h"
#include "MapLinkTypes.h"

namespace MapLinkNotify
{
    /** Green-check toast, ~4 s. Success confirmations. */
    void Success(const FText& Message);

    /** Yellow warning toast, ~6 s. Non-fatal warnings with no user choice. */
    void Warn(const FText& Message);

    /** Red fail toast, ~8 s. Failures the user should notice but that do
     *  not block a requested action (blocking failures stay modal). */
    void Fail(const FText& Message);
}

/** Human-readable text for an EMapLinkResult (single switch — keep complete;
 *  the compiler flags missing entries via -Wswitch on enum class). */
FText MapLinkResultToText(EMapLinkResult Result);
