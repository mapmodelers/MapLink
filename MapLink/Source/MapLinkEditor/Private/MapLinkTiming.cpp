// Copyright MapModelers. All Rights Reserved.
//
// v1.7.4 — Timing instrumentation implementation.

#include "MapLinkTiming.h"
#include "MapLinkUtils.h"
#include "MapLinkPipeline.h"   // LogMapLink

#include "HAL/PlatformTime.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/DateTime.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

// ---------------------------------------------------------------------------
// FMapLinkTimingSession
// ---------------------------------------------------------------------------

FMapLinkTimingSession& FMapLinkTimingSession::Get()
{
    static FMapLinkTimingSession Instance;
    return Instance;
}

void FMapLinkTimingSession::Begin(const FString& InSessionName,
                                  const FString& InSubject,
                                  const FString& InPluginBaseDir)
{
    if (RootPhase.IsValid())
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("[Timing] Previous session '%s' not ended cleanly. Flushing."),
               *SessionName);
        End();
    }

    SessionName   = InSessionName;
    Subject       = InSubject;
    PluginBaseDir = InPluginBaseDir;
    StartTime     = FDateTime::UtcNow();
    StartSec      = FPlatformTime::Seconds();

    Counters.Reset();
    BlenderLaunches.Reset();
    DependencyResolves.Reset();
    Notes.Reset();

    RootPhase = MakeShared<FMapLinkTimingPhase>();
    RootPhase->Name     = FString::Printf(TEXT("%s:%s"), *SessionName, *Subject);
    RootPhase->StartSec = StartSec;
    CurrentPhase        = RootPhase;

    UE_LOG(LogMapLink, Log, TEXT("[Timing] Session begin: %s"), *RootPhase->Name);
}

void FMapLinkTimingSession::End()
{
    if (!RootPhase.IsValid())
    {
        return;
    }

    const double NowSec = FPlatformTime::Seconds();
    RootPhase->DurationMs = (NowSec - StartSec) * 1000.0;

    // Defensive: auto-close any phase still on the stack.
    while (CurrentPhase.IsValid() && CurrentPhase != RootPhase)
    {
        CurrentPhase->DurationMs = (NowSec - CurrentPhase->StartSec) * 1000.0;
        UE_LOG(LogMapLink, Warning,
               TEXT("[Timing] Phase '%s' was not popped — auto-closing."),
               *CurrentPhase->Name);
        TSharedPtr<FMapLinkTimingPhase> Parent = CurrentPhase->Parent.Pin();
        CurrentPhase = Parent;
    }

    // ----- Build JSON -----
    TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("session"),     SessionName);
    Root->SetStringField(TEXT("subject"),     Subject);
    Root->SetStringField(TEXT("started_utc"), StartTime.ToIso8601());
    Root->SetNumberField(TEXT("total_ms"),    RootPhase->DurationMs);

    {
        TSharedPtr<FJsonObject> CountersObj = MakeShared<FJsonObject>();
        for (const TPair<FString, int32>& Pair : Counters)
        {
            CountersObj->SetNumberField(Pair.Key, Pair.Value);
        }
        Root->SetObjectField(TEXT("counters"), CountersObj);
    }

    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Reserve(BlenderLaunches.Num());
        for (const TSharedPtr<FJsonObject>& Obj : BlenderLaunches)
        {
            Arr.Add(MakeShared<FJsonValueObject>(Obj));
        }
        Root->SetArrayField(TEXT("blender_launches"), Arr);
    }

    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Reserve(DependencyResolves.Num());
        for (const TSharedPtr<FJsonObject>& Obj : DependencyResolves)
        {
            Arr.Add(MakeShared<FJsonValueObject>(Obj));
        }
        Root->SetArrayField(TEXT("dependency_resolves"), Arr);
    }

    if (Notes.Num() > 0)
    {
        TSharedPtr<FJsonObject> NotesObj = MakeShared<FJsonObject>();
        for (const TPair<FString, FString>& Pair : Notes)
        {
            NotesObj->SetStringField(Pair.Key, Pair.Value);
        }
        Root->SetObjectField(TEXT("notes"), NotesObj);
    }

    Root->SetObjectField(TEXT("phases"), PhaseToJson(RootPhase));

    // ----- Serialize -----
    FString OutString;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutString);
    FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

    // ----- Path: <ArtifactLogDir>/timing/maplink_timing_<UTC>_<Session>_<Subject>.json -----
    const FString TimeStr  = StartTime.ToString(TEXT("%Y%m%d_%H%M%S"));
    const FString FileName = FString::Printf(
        TEXT("maplink_timing_%s_%s_%s.json"),
        *TimeStr, *SessionName, *Subject);
    const FString TimingDir = FMapLinkUtils::GetArtifactLogDir() / TEXT("timing");
    const FString FilePath  = TimingDir / FileName;

    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    PF.CreateDirectoryTree(*TimingDir);

    if (FFileHelper::SaveStringToFile(OutString, *FilePath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        UE_LOG(LogMapLink, Log,
               TEXT("[Timing] Session end: %s (total %.1f ms) — wrote %s"),
               *RootPhase->Name, RootPhase->DurationMs, *FilePath);
    }
    else
    {
        UE_LOG(LogMapLink, Error,
               TEXT("[Timing] Failed to write JSON: %s"), *FilePath);
    }

    // Reset session state
    RootPhase.Reset();
    CurrentPhase.Reset();
    SessionName.Empty();
    Subject.Empty();
    PluginBaseDir.Empty();
    Counters.Reset();
    BlenderLaunches.Reset();
    DependencyResolves.Reset();
    Notes.Reset();
}

FMapLinkTimingPhase* FMapLinkTimingSession::PushPhase(
    const FString& Name, const FString& Key)
{
    if (!RootPhase.IsValid() || !CurrentPhase.IsValid())
    {
        return nullptr;  // No active session — scoped phases become no-ops.
    }

    TSharedPtr<FMapLinkTimingPhase> Phase = MakeShared<FMapLinkTimingPhase>();
    Phase->Name     = Name;
    Phase->Key      = Key;
    Phase->StartSec = FPlatformTime::Seconds();
    Phase->Parent   = CurrentPhase;

    CurrentPhase->Children.Add(Phase);
    CurrentPhase = Phase;
    return Phase.Get();
}

void FMapLinkTimingSession::PopPhase(FMapLinkTimingPhase* Phase)
{
    if (!Phase || !CurrentPhase.IsValid())
    {
        return;
    }
    if (CurrentPhase.Get() != Phase)
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("[Timing] Phase mismatch on pop: expected '%s', got '%s'"),
               *CurrentPhase->Name, *Phase->Name);
        return;
    }
    Phase->DurationMs = (FPlatformTime::Seconds() - Phase->StartSec) * 1000.0;
    TSharedPtr<FMapLinkTimingPhase> Parent = CurrentPhase->Parent.Pin();
    CurrentPhase = Parent;
}

void FMapLinkTimingSession::IncrementCounter(const FString& CounterName, int32 Delta)
{
    if (!RootPhase.IsValid()) return;
    Counters.FindOrAdd(CounterName) += Delta;
}

void FMapLinkTimingSession::LogBlenderLaunch(
    const FString& BlendName,
    const FString& Reason,
    const FString& Mode,
    double         DurationMs,
    int32          ReturnCode)
{
    if (!RootPhase.IsValid()) return;

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("blend"),       BlendName);
    Obj->SetStringField(TEXT("reason"),      Reason);
    Obj->SetStringField(TEXT("mode"),        Mode);
    Obj->SetNumberField(TEXT("duration_ms"), DurationMs);
    Obj->SetNumberField(TEXT("return_code"), ReturnCode);
    BlenderLaunches.Add(Obj);

    Counters.FindOrAdd(TEXT("blender_launches")) += 1;
}

void FMapLinkTimingSession::LogDependencyResolve(
    const FString& BlendName, const FString& Source)
{
    if (!RootPhase.IsValid()) return;

    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("blend"),  BlendName);
    Obj->SetStringField(TEXT("source"), Source);
    DependencyResolves.Add(Obj);

    if      (Source == TEXT("sidecar"))      Counters.FindOrAdd(TEXT("sidecar_hits"))      += 1;
    else if (Source == TEXT("plugin_cache")) Counters.FindOrAdd(TEXT("plugin_cache_hits")) += 1;
    else if (Source == TEXT("blender"))      Counters.FindOrAdd(TEXT("sidecar_misses"))    += 1;
}

void FMapLinkTimingSession::AddNote(const FString& Key, const FString& Value)
{
    if (!RootPhase.IsValid()) return;
    Notes.Add(Key, Value);
}

TSharedPtr<FJsonObject> FMapLinkTimingSession::PhaseToJson(
    const TSharedPtr<FMapLinkTimingPhase>& Phase) const
{
    TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
    Obj->SetStringField(TEXT("name"), Phase->Name);
    if (!Phase->Key.IsEmpty())
    {
        Obj->SetStringField(TEXT("key"), Phase->Key);
    }
    Obj->SetNumberField(TEXT("duration_ms"), Phase->DurationMs);

    if (Phase->Children.Num() > 0)
    {
        TArray<TSharedPtr<FJsonValue>> Arr;
        Arr.Reserve(Phase->Children.Num());
        for (const TSharedPtr<FMapLinkTimingPhase>& Child : Phase->Children)
        {
            Arr.Add(MakeShared<FJsonValueObject>(PhaseToJson(Child)));
        }
        Obj->SetArrayField(TEXT("children"), Arr);
    }
    return Obj;
}

// ---------------------------------------------------------------------------
// FMapLinkScopedPhase
// ---------------------------------------------------------------------------

FMapLinkScopedPhase::FMapLinkScopedPhase(const TCHAR* InName, const TCHAR* InKey)
{
    Phase = FMapLinkTimingSession::Get().PushPhase(
        InName ? FString(InName) : FString(),
        InKey  ? FString(InKey)  : FString());
}

FMapLinkScopedPhase::~FMapLinkScopedPhase()
{
    if (Phase)
    {
        FMapLinkTimingSession::Get().PopPhase(Phase);
    }
}

// ---------------------------------------------------------------------------
// FMapLinkTimingSessionGuard
// ---------------------------------------------------------------------------

FMapLinkTimingSessionGuard::FMapLinkTimingSessionGuard(
    const TCHAR*   InSessionName,
    const FString& InSubject,
    const FString& InPluginBaseDir)
{
    FMapLinkTimingSession::Get().Begin(
        FString(InSessionName), InSubject, InPluginBaseDir);
}

FMapLinkTimingSessionGuard::~FMapLinkTimingSessionGuard()
{
    FMapLinkTimingSession::Get().End();
}
