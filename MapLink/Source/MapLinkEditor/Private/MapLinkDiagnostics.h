// Copyright MapModelers. All Rights Reserved.
//
// v1.13 — Copy Diagnostics (support cost reduction).
//
// One click gathers everything a support request needs into the clipboard
// as markdown: plugin + addon + engine versions, project name, manifest
// path/content, a UMapLinkSettings snapshot, the last 200 LogMapLink lines
// of the current session (when the log file is accessible), and a proxy
// list with state summary.  Exposed from the MapLink main menu and the
// Manager's MapLink button.  Blender-side counterpart:
// blender/diagnostics.py.

#pragma once

#include "CoreMinimal.h"

class FMapLinkDiagnostics
{
public:
    /** Build the markdown report and copy it to the OS clipboard.
     *  Best-effort: sections that can't be gathered are skipped, never
     *  fatal.  Shows a "Diagnostics copied" toast on completion. */
    static void CopyToClipboard();

    /** The markdown report itself (used by CopyToClipboard). */
    static FString BuildReport();
};
