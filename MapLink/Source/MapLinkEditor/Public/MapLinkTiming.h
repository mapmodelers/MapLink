// Copyright MapModelers. All Rights Reserved.
//
// v1.7.4 — Timing instrumentation for import/reimport profiling.
//
// One JSON dump per session, written to:
//     <Plugin>/Logs/timing/maplink_timing_<UTC>_<Session>_<Subject>.json
//
// Phase tree mirrors the call stack. Aggregated counters and per-launch
// records (Blender, sidecar resolves) are kept flat for quick scanning.
//
// Usage (RAII):
//     {
//         FMapLinkTimingSessionGuard Guard(TEXT("Import"), BlendName, PluginDir);
//         MAPLINK_SCOPED_PHASE("Phase0_BuildGraph");
//         ...
//     }  // <-- JSON flushed here
//
// All calls are no-ops when no session is active, so it's safe to leave
// scoped phases in code paths that may execute outside an import run.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

struct FMapLinkTimingPhase
{
    FString  Name;
    FString  Key;
    double   StartSec   = 0.0;
    double   DurationMs = 0.0;
    TArray<TSharedPtr<FMapLinkTimingPhase>> Children;
    TWeakPtr<FMapLinkTimingPhase> Parent;
};

class FMapLinkTimingSession
{
public:
    static FMapLinkTimingSession& Get();

    /** Open a session. Subsequent scoped phases attach to this session. */
    void Begin(const FString& InSessionName,
               const FString& InSubject,
               const FString& InPluginBaseDir);

    /** Close the session and flush JSON to disk. No-op if not active. */
    void End();

    bool IsActive() const { return RootPhase.IsValid(); }

    /** Push a new scoped phase onto the stack. Returns a raw pointer used by
     *  FMapLinkScopedPhase. nullptr when no session is active. */
    FMapLinkTimingPhase* PushPhase(const FString& Name, const FString& Key);

    /** Pop the given phase (must match the current top of stack). */
    void PopPhase(FMapLinkTimingPhase* Phase);

    /** Bump a named counter. Safe to call with no active session. */
    void IncrementCounter(const FString& CounterName, int32 Delta = 1);

    /** Record a Blender subprocess launch (one entry per launch). */
    void LogBlenderLaunch(const FString& BlendName,
                          const FString& Reason,
                          const FString& Mode,
                          double         DurationMs,
                          int32          ReturnCode);

    /** Record how a dependency's analysis was resolved.
     *  Source ∈ { "sidecar", "plugin_cache", "blender" }. */
    void LogDependencyResolve(const FString& BlendName, const FString& Source);

    /** Free-form string metadata (e.g. fbx size, mode). */
    void AddNote(const FString& Key, const FString& Value);

private:
    FMapLinkTimingSession() = default;

    TSharedPtr<FJsonObject> PhaseToJson(
        const TSharedPtr<FMapLinkTimingPhase>& Phase) const;

    TSharedPtr<FMapLinkTimingPhase> RootPhase;
    TSharedPtr<FMapLinkTimingPhase> CurrentPhase;
    FString    SessionName;
    FString    Subject;
    FString    PluginBaseDir;
    FDateTime  StartTime;
    double     StartSec = 0.0;

    TMap<FString, int32>            Counters;
    TArray<TSharedPtr<FJsonObject>> BlenderLaunches;
    TArray<TSharedPtr<FJsonObject>> DependencyResolves;
    TMap<FString, FString>          Notes;
};

/** RAII scoped phase. Pushes on construct, pops on destruct. */
struct FMapLinkScopedPhase
{
    explicit FMapLinkScopedPhase(const TCHAR* InName, const TCHAR* InKey = nullptr);
    ~FMapLinkScopedPhase();

    FMapLinkScopedPhase(const FMapLinkScopedPhase&) = delete;
    FMapLinkScopedPhase& operator=(const FMapLinkScopedPhase&) = delete;

private:
    FMapLinkTimingPhase* Phase = nullptr;
};

/** RAII session guard. Begins on construct, ends (and writes JSON) on destruct. */
struct FMapLinkTimingSessionGuard
{
    FMapLinkTimingSessionGuard(const TCHAR*   InSessionName,
                               const FString& InSubject,
                               const FString& InPluginBaseDir);
    ~FMapLinkTimingSessionGuard();
};

#define MAPLINK_TIMING_CONCAT_IMPL(a, b) a##b
#define MAPLINK_TIMING_CONCAT(a, b)      MAPLINK_TIMING_CONCAT_IMPL(a, b)

#define MAPLINK_SCOPED_PHASE(Name) \
    FMapLinkScopedPhase MAPLINK_TIMING_CONCAT(MLPhase_, __LINE__)(TEXT(Name))

#define MAPLINK_SCOPED_PHASE_KEY(Name, KeyExpr) \
    FMapLinkScopedPhase MAPLINK_TIMING_CONCAT(MLPhase_, __LINE__)(TEXT(Name), *(KeyExpr))
