// Copyright MapModelers. All Rights Reserved.
//
// v1.8 — BP Reference Mode.
//
// FMapLinkStandInReport is the staged-operation JSON reporter for all
// BP-Reference-Mode lifecycle operations (create, refresh, delete,
// lifecycle_delete, lifecycle_rename, lifecycle_duplicate_scrub,
// reimport_swap).
//
// Each operation creates one report.  Each pipeline stage opens with
// BeginStage() and closes with EndStage(); errors/warnings can be added
// at any time.  The destructor writes the JSON to disk if WriteToFile()
// was not called explicitly — this guarantees that a crash mid-stage
// still leaves a forensic trail.
//
// Files land in:
//   <PluginDir>/Logs/bp_standin/<YYYYMMDD_HHMMSS>_<operation>_<sanitized_bp_name>.json

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Templates/SharedPointer.h"

/** Stage / overall-result status enum.  Maps to JSON strings:
 *    Ok    -> "ok"
 *    Warn  -> "warn"
 *    Error -> "error"
 *    Skip  -> "skip"
 *
 *  Result mapping for SetResult:
 *    Ok    -> "success"
 *    Warn  -> "partial"
 *    Error -> "failure"
 *    Skip  -> "partial"   (treated as partial when used as overall result) */
enum class EMapLinkReportStatus : uint8
{
    Ok,
    Warn,
    Error,
    Skip
};

class FMapLinkStandInReport
{
public:
    /** Construct a new report.
     *  @param Operation       One of: "create", "refresh", "delete",
     *                         "lifecycle_delete", "lifecycle_rename",
     *                         "lifecycle_duplicate_scrub",
     *                         "reimport_swap"
     *  @param BlueprintPath   "/Game/..." path of the BP this report
     *                         pertains to.  May be empty for failed
     *                         operations that never resolved a BP. */
    FMapLinkStandInReport(const FString& Operation,
                          const FString& BlueprintPath);

    /** Auto-writes the report to disk if WriteToFile() hasn't been
     *  called.  Crash-safe forensic trail. */
    ~FMapLinkStandInReport();

    // -----------------------------------------------------------------
    // Stage management
    // -----------------------------------------------------------------

    /** Open a new stage.  If a stage is already open, it is auto-closed
     *  with status Warn and a "auto_closed" message (so callers can't
     *  silently drop a stage). */
    void BeginStage(const FString& Name);

    /** Close the currently-open stage.  No-op if no stage is open. */
    void EndStage(EMapLinkReportStatus Status,
                  const TSharedPtr<FJsonObject>& Details,
                  const FString& Message);

    // -----------------------------------------------------------------
    // Top-level fields
    // -----------------------------------------------------------------

    void AddError(const FString& Error);
    void AddWarning(const FString& Warning);

    /** Set an output file path.  Common roles: "blend",
     *  "sidecar_analysis", "sidecar_hash". */
    void SetOutput(const FString& Role, const FString& Path);

    /** Set the overall result.  Defaults to Error until called.
     *  Errors[] non-empty also forces failure status at write time. */
    void SetResult(EMapLinkReportStatus Result);

    /** Optional: stamp the BP's parent class name (e.g. "AStaticMeshActor"). */
    void SetBlueprintClass(const FString& ClassName);

    /** Optional: stamp the stand-in .blend path. */
    void SetStandInBlendPath(const FString& Path);

    // -----------------------------------------------------------------
    // Persistence
    // -----------------------------------------------------------------

    /** Write the report to disk.  Idempotent — calling twice writes once. */
    bool WriteToFile();

    /** Absolute filesystem path the report will be / was written to. */
    const FString& GetReportPath() const { return ReportPath; }

private:
    static FString StatusToString(EMapLinkReportStatus S);
    static FString SanitizeForFilename(const FString& In);

    FString Operation;
    FString BlueprintPath;
    FString BlueprintClass;
    FString StandInBlendPath;
    FString TimestampUtc;        // ISO 8601, set in ctor
    FString FilenameTimestamp;   // YYYYMMDD_HHMMSS, set in ctor
    FString ReportPath;          // computed in ctor

    TArray<TSharedPtr<FJsonValue>> StagesJson;
    TArray<FString>                Errors;
    TArray<FString>                Warnings;
    TSharedPtr<FJsonObject>        Outputs;

    EMapLinkReportStatus           Result      = EMapLinkReportStatus::Error;
    bool                           bResultSet  = false;
    bool                           bWritten    = false;

    // Currently-open stage (if any)
    FString                        OpenStageName;
    double                         OpenStageStartSec = 0.0;
    bool                           bStageOpen        = false;
};
