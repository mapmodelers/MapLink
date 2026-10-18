// Copyright MapModelers. All Rights Reserved.
//
// v1.19 — Assets tab inspector: a 3D preview over an information panel, for
// the single selected asset.
//
// WHY THIS IS A SEPARATE WIDGET, AND OFF BY DEFAULT
//
// The Manager's Assets tab is a list of a hundred-odd rows that has to stay
// instant: the auto-refresh ticker rebuilds it on a content hash, and anything
// that makes selection expensive makes the whole tab feel slow. A live preview
// scene plus mesh statistics is the opposite of cheap — it loads assets.
//
// So the inspector is:
//   * opt-in, persisted per user in UMapLinkEditorState::bShowAssetInspector;
//   * single-selection only — a preview of "seven things" is not a preview,
//     and the stats would be a sum nobody asked for;
//   * two-phase. The cheap phase (identity, state evidence, children from
//     analysis.json) is proxy + JSON reads and runs on every selection. The
//     heavy phase (mesh statistics, materials, textures) LOADS assets and runs
//     only when its section is expanded, and caches per stem.
//
// The viewport is SMapLinkAssetViewport, the same widget the standalone Asset
// Viewer uses — not a second implementation. It already knows how to resolve
// SM_/LI_/BP_ to renderable components, and it already has SetPaused(), which
// is what lets a hidden inspector cost nothing.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Viewer/MapLinkAssetViewerProxyList.h"

class SMapLinkAssetViewport;
class SVerticalBox;
class UMapLinkProxyAsset;

/** One child of the inspected asset, from its analysis.json. */
struct FMapLinkInspectorChild
{
    FString Name;          // stem, e.g. "SM_p_gate1a"
    FString TypeStr;       // "LI" / "SM" / "BP" / "GEO"
    int32   InstanceCount = 1;
};

/** Everything the panel shows. Split by cost, because the split is the whole
 *  performance design: Cheap runs per selection, Heavy runs on demand. */
struct FMapLinkAssetInfo
{
    // ── Cheap: proxy fields + one JSON read ──────────────────────────────
    bool    bCheapValid = false;

    FString Stem;
    FString TypeStr;
    FString StateStr;
    FString StateDetail;        // the two timestamps behind the verdict
    FString BlendPath;
    FString LastImportStr;
    FString BlendHashShort;
    FString ContentPath;        // /Game/MapLink/<stem>/
    TArray<FString> ExpansionStrs;

    TArray<FMapLinkInspectorChild> Children;
    int32 NumOwnGeo = 0;
    bool  bAnalysisMissing = false;

    // ── Heavy: loads generated assets ────────────────────────────────────
    bool    bHeavyValid = false;
    bool    bHeavyFailed = false;

    int32   NumMeshes      = 0;
    int64   NumTriangles   = 0;   // LOD0, summed
    int32   NumVertices    = 0;
    int32   MaxLODs        = 0;
    int32   NumMaterialSlots = 0;

    TArray<FString> MaterialNames;      // unique, sorted
    TArray<FString> TextureNames;       // unique, sorted
    int32   NumTexturesNotConform = 0;
};

class SMapLinkAssetInspector : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SMapLinkAssetInspector) {}
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    /**
     * Show one asset, or nothing.
     *
     * Pass an empty stem for "no selection" and for multi-selection — the
     * caller decides, because only the caller knows how many rows are
     * selected. Repeating the current stem is a no-op, which matters: the
     * Assets list re-broadcasts selection on every auto-refresh rebuild, and
     * reloading a preview scene three times a second would be unusable.
     */
    void SetInspectedStem(const FString& Stem);

    /** Release the preview scene and stop ticking it. Called when the
     *  inspector is collapsed, so an unused inspector costs nothing. */
    void Sleep();

    /** Drop cached heavy info for a stem (or all stems, if empty) — used
     *  after a reimport, when the generated assets have been recreated. */
    void InvalidateCache(const FString& Stem = FString());

private:
    TSharedPtr<SMapLinkAssetViewport> Viewport;
    TSharedPtr<SVerticalBox>          InfoBox;

    FString CurrentStem;
    bool    bAsleep = true;

    /** Heavy info, keyed by stem. Cleared on reimport. */
    TMap<FString, FMapLinkAssetInfo> HeavyCache;

    FMapLinkAssetInfo Info;

    /** Which sections the user has open. Persisted for the session only —
     *  a section the user closed should stay closed as they click down the
     *  list, and the heavy sections must not silently re-open and start
     *  loading assets again. */
    bool bOpenIdentity  = true;
    bool bOpenChildren  = true;
    bool bOpenStats     = false;
    bool bOpenMaterials = false;

    void RebuildInfoPanel();
    void EnsureHeavyInfo();

    static void BuildCheapInfo(const FString& Stem, FMapLinkAssetInfo& Out);
    static void BuildHeavyInfo(const FString& Stem, FMapLinkAssetInfo& Out);

    TSharedRef<SWidget> MakeSection(const FText& Title, bool* OpenFlag,
                                    TSharedRef<SWidget> Body);
    TSharedRef<SWidget> MakeField(const FText& Label, const FString& Value,
                                  const FText& Tooltip = FText::GetEmpty());
};
