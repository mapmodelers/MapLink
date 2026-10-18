// Copyright MapModelers. All Rights Reserved.

#include "MapLinkNameDisplay.h"
#include "MapLinkActorLock.h"     // IsMapLinkActor + stem
#include "MapLinkSettings.h"
#include "MapLinkProxyAsset.h"    // v1.15.1 — out-of-date heuristic
#include "Viewer/MapLinkAssetViewerProxyList.h"   // ClassifyStem
#include "HAL/FileManager.h"

#include "Debug/DebugDrawService.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "LevelInstance/LevelInstanceSubsystem.h"   // v1.15.3 — inside-LI exclusion
#include "LevelInstance/LevelInstanceActor.h"       // v1.15.4 — child LI labels
#include "LevelInstance/LevelInstanceInterface.h"   // v1.15.6 — owning-stem check
#include "Engine/StaticMeshActor.h"                 // v1.15.6 — SM child labels
#include "Misc/Paths.h"
#include "Engine/Canvas.h"
#include "Engine/Font.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "SceneView.h"
#include "SceneInterface.h"   // Family->Scene->GetWorld()

FDelegateHandle FMapLinkNameDisplay::DrawHandle;

// ---------------------------------------------------------------------------
// v1.15.1 — per-stem out-of-date cache
//
// Same cheap heuristic as the Manager's auto-refresh (.blend mtime vs
// Proxy->ImportedBlendModTime), evaluated at most every ~5 s per stem so
// the per-frame draw stays file-stat free for cached stems.  Proxy
// resolution (LoadObject) is bounded per frame to avoid a hitch when a
// scene shows many stems for the first time.
// ---------------------------------------------------------------------------

namespace
{
    struct FStemState
    {
        TWeakObjectPtr<UMapLinkProxyAsset> Proxy;
        bool   bOutOfDate    = false;
        bool   bProxyMissing = false;   // don't hammer LoadObject
        double NextCheckSec  = 0.0;
    };

    TMap<FString, FStemState> GStemStates;
    int32 GProxyLoadsThisFrame = 0;
    uint64 GLastFrame = 0;

    constexpr double StateTTLSeconds        = 5.0;
    constexpr double MissingProxyTTLSeconds = 60.0;
    constexpr int32  MaxProxyLoadsPerFrame  = 5;
}

bool FMapLinkNameDisplay::IsStemOutOfDate(const FString& Stem)
{
    const double Now = FPlatformTime::Seconds();

    // Reset the per-frame load budget when the frame changes.
    if (GFrameCounter != GLastFrame)
    {
        GLastFrame = GFrameCounter;
        GProxyLoadsThisFrame = 0;
    }

    FStemState& State = GStemStates.FindOrAdd(Stem);
    if (Now < State.NextCheckSec)
    {
        return State.bOutOfDate;
    }

    UMapLinkProxyAsset* Proxy = State.Proxy.Get();
    if (!Proxy && !State.bProxyMissing)
    {
        const FString ProxyPath = FString::Printf(
            TEXT("/Game/MapLink/%s/%s_Proxy"), *Stem, *Stem);
        Proxy = FindObject<UMapLinkProxyAsset>(nullptr, *ProxyPath);
        if (!Proxy && GProxyLoadsThisFrame < MaxProxyLoadsPerFrame)
        {
            ++GProxyLoadsThisFrame;
            Proxy = LoadObject<UMapLinkProxyAsset>(nullptr, *ProxyPath);
        }
        if (Proxy)
        {
            State.Proxy = Proxy;
        }
        else if (GProxyLoadsThisFrame <= MaxProxyLoadsPerFrame)
        {
            State.bProxyMissing = true;
            State.NextCheckSec  = Now + MissingProxyTTLSeconds;
            State.bOutOfDate    = false;
            return false;
        }
        else
        {
            return State.bOutOfDate;   // budget exhausted — retry next frame
        }
    }
    if (!Proxy)
    {
        State.NextCheckSec = Now + MissingProxyTTLSeconds;
        return State.bOutOfDate;
    }

    State.NextCheckSec = Now + StateTTLSeconds;
    State.bOutOfDate   = false;
    if (!Proxy->BlendFilePath.IsEmpty() &&
        Proxy->ImportedBlendModTime.GetTicks() > 0)
    {
        const FDateTime FileTime =
            IFileManager::Get().GetTimeStamp(*Proxy->BlendFilePath);
        State.bOutOfDate = FileTime > Proxy->ImportedBlendModTime;
    }
    return State.bOutOfDate;
}

bool FMapLinkNameDisplay::ShouldShowStem(const FString& Stem)
{
    const UMapLinkSettings* S = UMapLinkSettings::Get();
    if (!S || !S->bShowAssetNames)
    {
        return false;
    }
    switch (FMapLinkAssetViewerProxyList::ClassifyStem(Stem))
    {
    case EMapLinkViewerAssetType::LevelInstance: return S->bShowNamesLI;
    case EMapLinkViewerAssetType::StaticMesh:    return S->bShowNamesSM;
    case EMapLinkViewerAssetType::Blueprint:     return S->bShowNamesBP;
    default:                                     return true;   // unknown: master toggle only
    }
}

FLinearColor FMapLinkNameDisplay::GetChildLabelColor(const FString& Stem)
{
    // v1.15.6 — darkened type/state color (white fade read too faint):
    // 60% toward black clearly reads as "child".
    return FMath::Lerp(GetLabelColor(Stem), FLinearColor::Black, 0.6f);
}

FLinearColor FMapLinkNameDisplay::GetLabelColor(const FString& Stem)
{
    const UMapLinkSettings* S = UMapLinkSettings::Get();
    if (!S)
    {
        return FLinearColor::White;
    }
    if (IsStemOutOfDate(Stem))
    {
        return S->NameColorOutOfDate;
    }
    switch (FMapLinkAssetViewerProxyList::ClassifyStem(Stem))
    {
    case EMapLinkViewerAssetType::LevelInstance: return S->NameColorLI;
    case EMapLinkViewerAssetType::StaticMesh:    return S->NameColorSM;
    case EMapLinkViewerAssetType::Blueprint:     return S->NameColorBP;
    default:                                     return FLinearColor::White;
    }
}

void FMapLinkNameDisplay::Register()
{
    // "Editor" show flag: on in level-editor viewports, off in PIE/game —
    // editor-only for free.
    DrawHandle = UDebugDrawService::Register(
        TEXT("Editor"),
        FDebugDrawDelegate::CreateStatic(&FMapLinkNameDisplay::Draw));
}

void FMapLinkNameDisplay::Unregister()
{
    if (DrawHandle.IsValid())
    {
        UDebugDrawService::Unregister(DrawHandle);
        DrawHandle.Reset();
    }
    GStemStates.Empty();
}

void FMapLinkNameDisplay::Draw(UCanvas* Canvas, APlayerController* /*PC*/)
{
    const UMapLinkSettings* S = UMapLinkSettings::Get();
    if (!S || !S->bShowAssetNames)
    {
        return;
    }
    if (!Canvas || !Canvas->SceneView || !Canvas->Canvas || !GEditor)
    {
        return;
    }

    // Only draw into views of the actual editor world — the "Editor" show
    // flag is also on in preview-scene viewports (Asset Viewer, material
    // editor…), which must not project the level's actors.
    UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
    if (!EditorWorld || EditorWorld->WorldType != EWorldType::Editor)
    {
        return;
    }
    // v1.16 — drop the per-stem cache when the level changes.
    //
    // GStemStates keys on stem only, so after a drill the entries survive
    // into a world where those stems mean something else (or nothing): the
    // cached proxy weak-pointers go stale and labels render with the wrong
    // colour until the 5s TTL happens to expire. A world-pointer compare is
    // one branch per frame and makes the cache exactly as long-lived as the
    // world it describes.
    static TWeakObjectPtr<UWorld> LastWorld;
    if (LastWorld.Get() != EditorWorld)
    {
        GStemStates.Empty();
        LastWorld = EditorWorld;
    }

    const FSceneView* View = Canvas->SceneView;
    if (View->Family && View->Family->Scene &&
        View->Family->Scene->GetWorld() != EditorWorld)
    {
        return;
    }

    const FVector ViewOrigin  = View->ViewMatrices.GetViewOrigin();
    const float   MaxDistSq   = FMath::Square(S->MaxNameLabelDistance);
    const int32   MaxLabels   = FMath::Max(1, S->MaxNameLabels);

    // v1.15.3 — ONE label per placed asset instance, anchored at the
    // ASSET's origin (the Blender collection-instance transform — like a
    // linked collection in Blender), never per mesh actor.  The origin
    // comes from the expansion tag (TagActor writes it since v1.15.3) or
    // from the actor's own transform for path-identified/LI actors.
    // Tag groups WITHOUT an origin (legacy, pre-1.15.3 expansion) draw
    // nothing — re-expand/reimport restores their labels.
    struct FLabelAccum
    {
        TOptional<FVector> Origin;
        FString Text;
        bool bChild = false;   // v1.15.4 — whitened tint
    };
    TMap<FString, FLabelAccum> Groups;

    const bool bShowChildren = S->bShowChildAssetNames;

    // v1.15.3 fix — TActorIterator also yields the actors INSIDE loaded
    // Level Instances (the generated LI levels' own mesh actors).  Those
    // must NEVER label individually: the ONE label of a placed LI comes
    // from its ALevelInstance actor (which lives in the main world and
    // is not LI-owned).  Without this check every mesh of a loaded LI
    // spawned its own path-identified label.
    ULevelInstanceSubsystem* LISubsystem =
        EditorWorld->GetSubsystem<ULevelInstanceSubsystem>();

    for (TActorIterator<AActor> It(EditorWorld); It; ++It)
    {
        AActor* Actor = *It;

        ILevelInstanceInterface* OwningLI = LISubsystem
            ? LISubsystem->GetOwningLevelInstance(Actor->GetLevel())
            : nullptr;
        const bool bInsideLI = (OwningLI != nullptr);
        if (bInsideLI && !bShowChildren)
        {
            continue;
        }

        FString Stem, InstanceKey;
        TOptional<FVector> AssetOrigin;
        if (!FMapLinkActorLock::IsMapLinkActor(Actor, &Stem, &InstanceKey,
                                               &AssetOrigin))
        {
            continue;
        }

        bool bChild = false;
        if (bInsideLI)
        {
            // Lives inside a Level Instance.  v1.15.6 — children are:
            //  • nested ALevelInstance actors (LI children), and
            //  • path-identified leaf SM actors whose mesh belongs to a
            //    DIFFERENT stem than the owning level (linked SM child
            //    instances).  Tag-matched actors (composite expansions)
            //    and same-stem meshes (the LI's own geometry) never
            //    label — that would be one widget per mesh again.
            if (!Cast<ALevelInstance>(Actor))
            {
                const bool bTagMatched =
                    InstanceKey.StartsWith(TEXT("MapLink"));
                if (bTagMatched || !Cast<AStaticMeshActor>(Actor))
                {
                    continue;
                }
                FString OwningStem;
                {
                    FString OwnPath =
                        OwningLI->GetWorldAsset().ToSoftObjectPath().ToString();
                    int32 DotIdx;
                    if (OwnPath.FindChar(TEXT('.'), DotIdx))
                    {
                        OwnPath.LeftInline(DotIdx);
                    }
                    OwningStem = FPaths::GetBaseFilename(OwnPath);
                }
                if (Stem.IsEmpty() || Stem == OwningStem)
                {
                    continue;   // the LI's own geometry
                }
            }
            bChild = true;
        }

        // v1.15.1 — per-type filter (LI_/SM_/BP_ toggles).
        if (!ShouldShowStem(Stem))
        {
            continue;
        }

        if (Stem.IsEmpty())
        {
            Stem = Actor->GetActorLabel();
        }

        FLabelAccum& Accum = Groups.FindOrAdd(InstanceKey);
        if (!Accum.Origin.IsSet() && AssetOrigin.IsSet())
        {
            Accum.Origin = AssetOrigin;
        }
        if (Accum.Text.IsEmpty())
        {
            Accum.Text = MoveTemp(Stem);
        }
        Accum.bChild = bChild;
    }

    if (Groups.Num() == 0)
    {
        return;
    }

    struct FLabel
    {
        FVector WorldPos;
        FString Text;
        float   DistSq;
        bool    bChild;
    };
    TArray<FLabel> Labels;
    Labels.Reserve(Groups.Num());

    for (TPair<FString, FLabelAccum>& Pair : Groups)
    {
        if (!Pair.Value.Origin.IsSet())
        {
            continue;   // legacy tags without origin — no label
        }
        const FVector Pos = Pair.Value.Origin.GetValue();
        const float DistSq = FVector::DistSquared(Pos, ViewOrigin);
        if (DistSq > MaxDistSq)
        {
            continue;
        }
        Labels.Add({ Pos, MoveTemp(Pair.Value.Text), DistSq,
                     Pair.Value.bChild });
    }

    if (Labels.Num() == 0)
    {
        return;
    }

    // Nearest first; clamp to budget.
    Labels.Sort([](const FLabel& A, const FLabel& B)
    {
        return A.DistSq < B.DistSq;
    });
    if (Labels.Num() > MaxLabels)
    {
        Labels.SetNum(MaxLabels);
    }

    UFont* Font = GEngine ? GEngine->GetSmallFont() : nullptr;
    if (!Font)
    {
        return;
    }

    for (const FLabel& Label : Labels)
    {
        // Project: behind-camera results have W <= 0.
        const FVector Screen = Canvas->Project(Label.WorldPos);
        if (Screen.Z <= 0.f)
        {
            continue;
        }

        const FText Text = FText::FromString(Label.Text);

        // Center horizontally over the anchor, like the vec3 3D widget.
        float W = 0.f, H = 0.f;
        Canvas->TextSize(Font, Label.Text, W, H);

        // v1.15.1 — type color; out-of-date overrides.  v1.15.4 —
        // children draw whitened.  (Empty stems → plain white.)
        const FLinearColor LabelColor = Label.Text.IsEmpty()
            ? FLinearColor::White
            : (Label.bChild ? GetChildLabelColor(Label.Text)
                            : GetLabelColor(Label.Text));

        FCanvasTextItem Item(
            FVector2D(Screen.X - W * 0.5f, Screen.Y - H * 0.5f),
            Text, Font, LabelColor);
        Item.EnableShadow(FLinearColor::Black);
        Item.Scale = FVector2D(1.f, 1.f);
        Canvas->DrawItem(Item);
    }
}
