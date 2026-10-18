// Copyright MapModelers. All Rights Reserved.

#include "MapLinkStandInReport.h"
#include "MapLinkUtils.h"
#include "MapLinkPipeline.h"  // LogMapLink

#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"

// ---------------------------------------------------------------------------
// ctor / dtor
// ---------------------------------------------------------------------------

FMapLinkStandInReport::FMapLinkStandInReport(const FString& InOperation,
                                             const FString& InBlueprintPath)
    : Operation(InOperation)
    , BlueprintPath(InBlueprintPath)
{
    const FDateTime Now = FDateTime::UtcNow();
    TimestampUtc      = Now.ToIso8601();
    FilenameTimestamp = Now.ToString(TEXT("%Y%m%d_%H%M%S"));

    Outputs = MakeShared<FJsonObject>();

    // Compose target path
    const FString LogDir        = FMapLinkUtils::GetArtifactLogDir() / TEXT("bp_standin");

    // Sanitize BP name for filename
    FString BPLeaf;
    if (!BlueprintPath.IsEmpty())
    {
        // e.g. "/Game/UltimateSurvivalPack/Blueprints/Buildings/Final/BP_WoodenDoors.BP_WoodenDoors"
        // → "BP_WoodenDoors"
        FString Path = BlueprintPath;
        int32 DotIdx;
        if (Path.FindChar(TEXT('.'), DotIdx))
        {
            Path = Path.Left(DotIdx);
        }
        BPLeaf = FPaths::GetCleanFilename(Path);
    }
    if (BPLeaf.IsEmpty())
    {
        BPLeaf = TEXT("unknown");
    }
    BPLeaf = SanitizeForFilename(BPLeaf);

    ReportPath = LogDir / FString::Printf(
        TEXT("%s_%s_%s.json"),
        *FilenameTimestamp,
        *Operation,
        *BPLeaf);

    // Ensure the directory exists up front so a write at any time succeeds.
    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    PF.CreateDirectoryTree(*LogDir);
}

FMapLinkStandInReport::~FMapLinkStandInReport()
{
    if (!bWritten)
    {
        // Auto-close any still-open stage so the JSON is well-formed.
        if (bStageOpen)
        {
            EndStage(EMapLinkReportStatus::Warn, nullptr,
                     TEXT("auto_closed_in_destructor"));
        }
        WriteToFile();
    }
}

// ---------------------------------------------------------------------------
// Stage management
// ---------------------------------------------------------------------------

void FMapLinkStandInReport::BeginStage(const FString& Name)
{
    if (bStageOpen)
    {
        // Auto-close the prior stage with a warning.
        EndStage(EMapLinkReportStatus::Warn, nullptr,
                 TEXT("auto_closed_by_new_begin_stage"));
    }
    OpenStageName     = Name;
    OpenStageStartSec = FPlatformTime::Seconds();
    bStageOpen        = true;
}

void FMapLinkStandInReport::EndStage(EMapLinkReportStatus Status,
                                     const TSharedPtr<FJsonObject>& Details,
                                     const FString& Message)
{
    if (!bStageOpen)
    {
        return;
    }

    const double DurationMs =
        (FPlatformTime::Seconds() - OpenStageStartSec) * 1000.0;

    TSharedRef<FJsonObject> Stage = MakeShared<FJsonObject>();
    Stage->SetStringField(TEXT("name"),    OpenStageName);
    Stage->SetStringField(TEXT("status"),  StatusToString(Status));
    Stage->SetNumberField(TEXT("duration_ms"),
                          FMath::FloorToDouble(DurationMs));
    Stage->SetStringField(TEXT("message"), Message);
    Stage->SetObjectField(TEXT("details"),
                          Details.IsValid() ? Details : MakeShared<FJsonObject>());

    StagesJson.Add(MakeShared<FJsonValueObject>(Stage));

    // Mirror to UE log for live diagnostics.
    UE_LOG(LogMapLink, Log,
           TEXT("[BPStandIn:%s] stage=%s status=%s (%.0fms) %s"),
           *Operation, *OpenStageName, *StatusToString(Status),
           DurationMs, *Message);

    bStageOpen = false;
    OpenStageName.Empty();
}

// ---------------------------------------------------------------------------
// Top-level setters
// ---------------------------------------------------------------------------

void FMapLinkStandInReport::AddError(const FString& Error)
{
    Errors.Add(Error);
    UE_LOG(LogMapLink, Error,
           TEXT("[BPStandIn:%s] ERROR: %s"), *Operation, *Error);
}

void FMapLinkStandInReport::AddWarning(const FString& Warning)
{
    Warnings.Add(Warning);
    UE_LOG(LogMapLink, Warning,
           TEXT("[BPStandIn:%s] WARN: %s"), *Operation, *Warning);
}

void FMapLinkStandInReport::SetOutput(const FString& Role, const FString& Path)
{
    if (Outputs.IsValid())
    {
        Outputs->SetStringField(Role, Path);
    }
}

void FMapLinkStandInReport::SetResult(EMapLinkReportStatus InResult)
{
    Result     = InResult;
    bResultSet = true;
}

void FMapLinkStandInReport::SetBlueprintClass(const FString& ClassName)
{
    BlueprintClass = ClassName;
}

void FMapLinkStandInReport::SetStandInBlendPath(const FString& Path)
{
    StandInBlendPath = Path;
}

// ---------------------------------------------------------------------------
// WriteToFile
// ---------------------------------------------------------------------------

bool FMapLinkStandInReport::WriteToFile()
{
    if (bWritten)
    {
        return true;
    }

    // If no explicit result was set, derive one from errors[].
    EMapLinkReportStatus FinalResult = Result;
    if (!bResultSet)
    {
        FinalResult = Errors.Num() > 0
            ? EMapLinkReportStatus::Error
            : (Warnings.Num() > 0
               ? EMapLinkReportStatus::Warn
               : EMapLinkReportStatus::Ok);
    }
    else if (Errors.Num() > 0 && FinalResult == EMapLinkReportStatus::Ok)
    {
        // An explicit Ok with errors present is downgraded to partial.
        FinalResult = EMapLinkReportStatus::Warn;
    }

    // Map result enum to schema strings.
    FString ResultStr;
    switch (FinalResult)
    {
        case EMapLinkReportStatus::Ok:    ResultStr = TEXT("success"); break;
        case EMapLinkReportStatus::Warn:  ResultStr = TEXT("partial"); break;
        case EMapLinkReportStatus::Skip:  ResultStr = TEXT("partial"); break;
        case EMapLinkReportStatus::Error: ResultStr = TEXT("failure"); break;
    }

    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("operation"),     Operation);
    Root->SetStringField(TEXT("feature"),       TEXT("bp_reference_mode"));
    Root->SetStringField(TEXT("version"),       TEXT("1.8"));
    Root->SetStringField(TEXT("timestamp_utc"), TimestampUtc);
    Root->SetStringField(TEXT("blueprint_path"), BlueprintPath);

    if (BlueprintClass.IsEmpty())
    {
        Root->SetField(TEXT("blueprint_class"), MakeShared<FJsonValueNull>());
    }
    else
    {
        Root->SetStringField(TEXT("blueprint_class"), BlueprintClass);
    }

    if (StandInBlendPath.IsEmpty())
    {
        Root->SetField(TEXT("standin_blend_path"), MakeShared<FJsonValueNull>());
    }
    else
    {
        Root->SetStringField(TEXT("standin_blend_path"), StandInBlendPath);
    }

    Root->SetArrayField(TEXT("stages"), StagesJson);
    Root->SetStringField(TEXT("result"), ResultStr);

    {
        TArray<TSharedPtr<FJsonValue>> ErrArr;
        for (const FString& E : Errors)
        {
            ErrArr.Add(MakeShared<FJsonValueString>(E));
        }
        Root->SetArrayField(TEXT("errors"), ErrArr);
    }
    {
        TArray<TSharedPtr<FJsonValue>> WarnArr;
        for (const FString& W : Warnings)
        {
            WarnArr.Add(MakeShared<FJsonValueString>(W));
        }
        Root->SetArrayField(TEXT("warnings"), WarnArr);
    }
    Root->SetObjectField(TEXT("outputs"),
                         Outputs.IsValid() ? Outputs : MakeShared<FJsonObject>());

    FString Out;
    TSharedRef<TJsonWriter<>> Writer =
        TJsonWriterFactory<>::Create(&Out);
    FJsonSerializer::Serialize(Root, Writer);

    const bool bOk = FFileHelper::SaveStringToFile(Out, *ReportPath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    if (bOk)
    {
        UE_LOG(LogMapLink, Log,
               TEXT("[BPStandIn:%s] Report written: %s (result=%s)"),
               *Operation, *ReportPath, *ResultStr);
        bWritten = true;
    }
    else
    {
        UE_LOG(LogMapLink, Error,
               TEXT("[BPStandIn:%s] FAILED to write report: %s"),
               *Operation, *ReportPath);
    }
    return bOk;
}

// ---------------------------------------------------------------------------
// Statics / helpers
// ---------------------------------------------------------------------------

FString FMapLinkStandInReport::StatusToString(EMapLinkReportStatus S)
{
    switch (S)
    {
        case EMapLinkReportStatus::Ok:    return TEXT("ok");
        case EMapLinkReportStatus::Warn:  return TEXT("warn");
        case EMapLinkReportStatus::Error: return TEXT("error");
        case EMapLinkReportStatus::Skip:  return TEXT("skip");
    }
    return TEXT("ok");
}

FString FMapLinkStandInReport::SanitizeForFilename(const FString& In)
{
    FString Out;
    Out.Reserve(In.Len());
    for (TCHAR C : In)
    {
        const bool bAllowed =
            (C >= TEXT('A') && C <= TEXT('Z')) ||
            (C >= TEXT('a') && C <= TEXT('z')) ||
            (C >= TEXT('0') && C <= TEXT('9')) ||
            C == TEXT('_') || C == TEXT('-');
        Out.AppendChar(bAllowed ? C : TEXT('_'));
    }
    if (Out.IsEmpty())
    {
        Out = TEXT("unknown");
    }
    return Out;
}
