// Copyright MapModelers. All Rights Reserved.

#include "MapLinkActorLock.h"
#include "MapLinkSettings.h"
#include "MapLinkPipeline.h"   // LogMapLink

#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Engine/Blueprint.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "Misc/Paths.h"
#include "Containers/Ticker.h"

FDelegateHandle FMapLinkActorLock::MapOpenedHandle;
FDelegateHandle FMapLinkActorLock::ActorAddedHandle;

namespace
{
    const TCHAR* MapLinkContentPrefix = TEXT("/Game/MapLink/");

    /** Strip the object suffix ('.Foo') and answer whether the package
     *  lives under /Game/MapLink/. */
    bool IsMapLinkPackagePath(FString Path, FString* OutStem)
    {
        int32 DotIdx;
        if (Path.FindChar(TEXT('.'), DotIdx))
        {
            Path.LeftInline(DotIdx);
        }
        if (!Path.StartsWith(MapLinkContentPrefix))
        {
            return false;
        }
        if (OutStem)
        {
            // /Game/MapLink/<Stem>/<Asset> — the stem folder is the id.
            FString After = Path.RightChop(FCString::Strlen(MapLinkContentPrefix));
            int32 SlashIdx;
            *OutStem = After.FindChar(TEXT('/'), SlashIdx)
                ? After.Left(SlashIdx) : FPaths::GetBaseFilename(After);
        }
        return true;
    }
}

bool FMapLinkActorLock::IsMapLinkActor(const AActor* Actor, FString* OutStem,
                                       FString* OutInstanceKey,
                                       TOptional<FVector>* OutAssetOrigin)
{
    if (!Actor)
    {
        return false;
    }

    // Path-identified actors: the actor IS the instance (v1.15.2), and
    // its own transform IS the asset origin (v1.15.3).
    auto SetPerActorKey = [Actor, OutInstanceKey, OutAssetOrigin]()
    {
        if (OutInstanceKey)
        {
            *OutInstanceKey = Actor->GetPathName();
        }
        if (OutAssetOrigin)
        {
            *OutAssetOrigin = Actor->GetActorLocation();
        }
    };

    // v1.15.3 — parse a trailing "x,y,z" tag segment into an origin.
    auto ParseOrigin = [OutAssetOrigin](const FString& Segment)
    {
        if (!OutAssetOrigin)
        {
            return;
        }
        TArray<FString> Comps;
        Segment.ParseIntoArray(Comps, TEXT(","), /*CullEmpty=*/false);
        if (Comps.Num() == 3)
        {
            *OutAssetOrigin = FVector(
                FCString::Atof(*Comps[0]),
                FCString::Atof(*Comps[1]),
                FCString::Atof(*Comps[2]));
        }
    };

    // 1) Pipeline tags (WP expansion, SM_ expansion, import placeholders).
    for (const FName& Tag : Actor->Tags)
    {
        const FString TagStr = Tag.ToString();
        const bool bExpanded  = TagStr.StartsWith(TEXT("MapLinkExpanded:"));
        const bool bWPExpand  = TagStr.StartsWith(TEXT("MapLinkWPExpand:"));
        if (bExpanded || bWPExpand)
        {
            if (OutStem || OutInstanceKey || OutAssetOrigin)
            {
                // MapLinkExpanded:<stem>:<idx>[:<x,y,z>]         → label = <stem>
                // MapLinkWPExpand:<comp>:<child>:<idx>[:<x,y,z>] → label = <child>
                // v1.15.2 — <idx> is PER-ACTOR: the instance key drops it
                // so every actor of one broken-open asset shares the key.
                // v1.15.3 — trailing segment = asset origin (see TagActor).
                TArray<FString> Parts;
                TagStr.ParseIntoArray(Parts, TEXT(":"), /*CullEmpty=*/false);
                if (bExpanded && Parts.Num() >= 2)
                {
                    if (OutStem)        { *OutStem = Parts[1]; }
                    if (OutInstanceKey)
                    {
                        // v1.15.6 — fold the ORIGIN segment into the key:
                        // the same asset placed N times under one
                        // composition must yield N widgets (one per
                        // instance), while the mesh actors of ONE
                        // instance share the origin and stay grouped.
                        *OutInstanceKey = Parts[0] + TEXT(":") + Parts[1];
                        if (Parts.Num() >= 4)
                        {
                            *OutInstanceKey += TEXT(":") + Parts[3];
                        }
                    }
                    if (Parts.Num() >= 4) { ParseOrigin(Parts[3]); }
                }
                else if (bWPExpand && Parts.Num() >= 3)
                {
                    if (OutStem)        { *OutStem = Parts[2]; }
                    if (OutInstanceKey)
                    {
                        *OutInstanceKey = Parts[0] + TEXT(":") + Parts[1]
                                        + TEXT(":") + Parts[2];
                        if (Parts.Num() >= 5)
                        {
                            *OutInstanceKey += TEXT(":") + Parts[4];
                        }
                    }
                    if (Parts.Num() >= 5) { ParseOrigin(Parts[4]); }
                }
                else
                {
                    if (OutStem) { OutStem->Reset(); }
                    SetPerActorKey();
                }

                // Legacy-tag rescue: a Level Instance actor's own
                // transform IS the collection transform — usable even
                // when the tag predates the origin segment.
                if (OutAssetOrigin && !OutAssetOrigin->IsSet() &&
                    Cast<ALevelInstance>(Actor))
                {
                    *OutAssetOrigin = Actor->GetActorLocation();
                }
            }
            return true;
        }
        if (TagStr == TEXT("MapLinkPlaceholder"))
        {
            if (OutStem) { OutStem->Reset(); }
            SetPerActorKey();
            return true;
        }
    }

    // 2) Level Instance referencing a /Game/MapLink/ level.
    if (const ALevelInstance* LI = Cast<ALevelInstance>(Actor))
    {
        SetPerActorKey();
        return IsMapLinkPackagePath(LI->GetWorldAsset().ToString(), OutStem);
    }

    // 3) Blueprint-classed actor generated from /Game/MapLink/.
    if (Actor->GetClass() && Actor->GetClass()->ClassGeneratedBy)
    {
        if (const UBlueprint* BP =
                Cast<UBlueprint>(Actor->GetClass()->ClassGeneratedBy))
        {
            FString Stem;
            if (IsMapLinkPackagePath(BP->GetOutermost()->GetName(), &Stem))
            {
                if (OutStem)
                {
                    if (Stem.EndsWith(TEXT("_GP"))) Stem.LeftChopInline(3);
                    *OutStem = Stem;
                }
                SetPerActorKey();
                return true;
            }
        }
        return false;
    }

    // 4) StaticMeshActor whose mesh lives under /Game/MapLink/.
    if (const AStaticMeshActor* SMA = Cast<AStaticMeshActor>(Actor))
    {
        const UStaticMeshComponent* SMC = SMA->GetStaticMeshComponent();
        if (SMC && SMC->GetStaticMesh())
        {
            SetPerActorKey();
            return IsMapLinkPackagePath(
                SMC->GetStaticMesh()->GetOutermost()->GetName(), OutStem);
        }
    }

    return false;
}

int32 FMapLinkActorLock::ApplyToEditorWorld()
{
    if (!GEditor)
    {
        return 0;
    }
    UWorld* World = GEditor->GetEditorWorldContext().World();
    if (!World || World->WorldType != EWorldType::Editor)
    {
        return 0;
    }

    const bool bLock = UMapLinkSettings::Get()->bLockMapLinkActorTransforms;

    int32 Touched = 0;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (!IsMapLinkActor(Actor))
        {
            continue;
        }
        if (Actor->IsLockLocation() != bLock)
        {
            // Deliberately NO Modify(): the lock flag must not dirty level
            // packages on its own — it simply rides along the next pipeline
            // save (which is P4-routed).
            Actor->SetLockLocation(bLock);
            ++Touched;
        }
    }

    if (Touched > 0)
    {
        UE_LOG(LogMapLink, Log,
               TEXT("[ActorLock] %s %d MapLink actor(s) in '%s'."),
               bLock ? TEXT("Locked") : TEXT("Unlocked"),
               Touched, *World->GetName());
    }
    return Touched;
}

void FMapLinkActorLock::Register()
{
    MapOpenedHandle = FEditorDelegates::OnMapOpened.AddStatic(
        &FMapLinkActorLock::OnMapOpened);

    if (GEngine)
    {
        ActorAddedHandle = GEngine->OnLevelActorAdded().AddStatic(
            &FMapLinkActorLock::OnLevelActorAdded);
    }

    // Initial sweep — the module loads at PostEngineInit, a map may
    // already be up (or will trigger OnMapOpened right after).
    ApplyToEditorWorld();
}

void FMapLinkActorLock::Unregister()
{
    FEditorDelegates::OnMapOpened.Remove(MapOpenedHandle);
    if (GEngine)
    {
        GEngine->OnLevelActorAdded().Remove(ActorAddedHandle);
    }
    MapOpenedHandle.Reset();
    ActorAddedHandle.Reset();
}

void FMapLinkActorLock::OnMapOpened(const FString& /*Filename*/, bool /*bAsTemplate*/)
{
    ApplyToEditorWorld();
}

void FMapLinkActorLock::OnLevelActorAdded(AActor* Actor)
{
    // Pipeline tags and drag-drop asset references are applied AFTER the
    // spawn this delegate observes — defer the identity check one tick.
    if (!Actor || !UMapLinkSettings::Get()->bLockMapLinkActorTransforms)
    {
        return;
    }
    UWorld* World = Actor->GetWorld();
    if (!World || World->WorldType != EWorldType::Editor)
    {
        return;   // never touch PIE/game worlds
    }

    TWeakObjectPtr<AActor> WeakActor(Actor);
    FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([WeakActor](float) -> bool
        {
            AActor* A = WeakActor.Get();
            if (A && !A->IsLockLocation() &&
                UMapLinkSettings::Get()->bLockMapLinkActorTransforms &&
                FMapLinkActorLock::IsMapLinkActor(A))
            {
                A->SetLockLocation(true);
            }
            return false;   // one-shot
        }));
}
