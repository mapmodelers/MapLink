// Copyright MapModelers. All Rights Reserved.
//
// v1.13 — Close-the-loop status file (UE → Blender).
//
// UE writes a tiny per-stem JSON next to the source .blend:
//     <blend_dir>/<stem>_uestatus.json
//   begin:  { "state": "importing", "utc": "..." }
//   end:    { "state": "ok"|"failed", "code": "<EMapLinkResult name>",
//             "message": "<short human text>", "utc": "..." }
//
// The Blender addon (blender/ue_status.py) shows this as a sidebar badge:
// "UE: importing…" / "UE: ✓ up to date" / "UE: ✗ failed — <message>".
//
// Status files are transient and machine-local: they are .gitignore'd and
// must be P4-ignored (never added to any changelist).
//
// All writes are BEST-EFFORT: failures are logged and never fatal — the
// import/reimport result is what matters.

#pragma once

#include "CoreMinimal.h"
#include "MapLinkTypes.h"

namespace MapLinkUEStatus
{
    /** Write { state: "importing" } next to the .blend. Best-effort. */
    void WriteImporting(const FString& BlendFilePath);

    /** Write the final { state: "ok"|"failed", code, message }. Best-effort.
     *  "ok" for Success; everything else (including Cancelled) is "failed"
     *  with the human message from MapLinkResultToText(). */
    void WriteResult(const FString& BlendFilePath, EMapLinkResult Result);
}
