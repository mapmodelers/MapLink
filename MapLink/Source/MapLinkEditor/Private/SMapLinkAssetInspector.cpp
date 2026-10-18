// Copyright MapModelers. All Rights Reserved.
//
// v1.19 — Assets tab inspector (see header for the design rationale).

#include "SMapLinkAssetInspector.h"

#include "MapLinkEditorState.h"
#include "MapLinkInterchangeHelper.h"
#include "MapLinkPipeline.h"          // LogMapLink, GetMapLinkContentRoot
#include "MapLinkProxyAsset.h"
#include "MapLinkSettings.h"
#include "MapLinkTextureScanner.h"
#include "MapLinkTextureTypes.h"
#include "MapLinkUtils.h"
#include "Viewer/SMapLinkAssetViewport.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstance.h"
#include "Engine/Texture.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

#include "Styling/AppStyle.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SHyperlink.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SMapLinkAssetInspector"

namespace
{
    /** 12345678 -> "12 345 678". Triangle counts are the one number here a
     *  reader compares at a glance, and an ungrouped eight-digit run is
     *  unreadable. */
    FString Grouped(int64 Value)
    {
        FString Digits = FString::Printf(TEXT("%lld"), Value);
        FString Out;
        const int32 Len = Digits.Len();
        for (int32 i = 0; i < Len; ++i)
        {
            if (i > 0 && ((Len - i) % 3) == 0)
            {
                Out += TEXT(" ");
            }
            Out.AppendChar(Digits[i]);
        }
        return Out;
    }

    FString TypeFromStem(const FString& Stem)
    {
        if (Stem.StartsWith(TEXT("LI_"))) return TEXT("LI");
        if (Stem.StartsWith(TEXT("SM_"))) return TEXT("SM");
        if (Stem.StartsWith(TEXT("BP_"))) return TEXT("BP");
        return TEXT("?");
    }

    EMapLinkViewerAssetType ViewerTypeFromStem(const FString& Stem)
    {
        return FMapLinkAssetViewerProxyList::ClassifyStem(Stem);
    }

    /** The proxy for a stem, or null. Registry lookup, no directory walk. */
    UMapLinkProxyAsset* FindProxyForStem(const FString& Stem)
    {
        FAssetRegistryModule& ARM =
            FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");

        FARFilter Filter;
        Filter.ClassPaths.Add(UMapLinkProxyAsset::StaticClass()->GetClassPathName());
        Filter.bRecursivePaths = true;

        TArray<FAssetData> Assets;
        ARM.Get().GetAssets(Filter, Assets);

        for (const FAssetData& Data : Assets)
        {
            UMapLinkProxyAsset* Proxy = Cast<UMapLinkProxyAsset>(Data.GetAsset());
            if (!Proxy) continue;
            if (FPaths::GetBaseFilename(Proxy->BlendFilePath) == Stem)
            {
                return Proxy;
            }
        }
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Construct
// ---------------------------------------------------------------------------

void SMapLinkAssetInspector::Construct(const FArguments& InArgs)
{
    ChildSlot
    [
        SNew(SSplitter)
        .Orientation(Orient_Vertical)

        // Preview. Fixed-ish share rather than filling: the information is
        // the point, the picture is confirmation.
        + SSplitter::Slot()
        .Value(0.42f)
        [
            SNew(SBorder)
            .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
            .Padding(1.f)
            [
                SAssignNew(Viewport, SMapLinkAssetViewport)
            ]
        ]

        + SSplitter::Slot()
        .Value(0.58f)
        [
            SNew(SBorder)
            .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
            .Padding(2.f)
            [
                SNew(SScrollBox)
                + SScrollBox::Slot()
                [
                    SAssignNew(InfoBox, SVerticalBox)
                ]
            ]
        ]
    ];

    // Constructed asleep. The Assets tab builds every tab page up front
    // (SWidgetSwitcher), so an inspector that started live would spin a
    // preview scene for a tab the user may never open.
    if (Viewport.IsValid())
    {
        Viewport->SetPaused(true);
    }
    RebuildInfoPanel();
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

void SMapLinkAssetInspector::SetInspectedStem(const FString& Stem)
{
    // The Assets list re-broadcasts its selection on every auto-refresh
    // rebuild — several times a minute. Reloading the same preview scene each
    // time would make the tab unusable, so identical is a hard no-op.
    if (Stem == CurrentStem && !bAsleep)
    {
        return;
    }

    CurrentStem = Stem;
    bAsleep     = false;

    Info = FMapLinkAssetInfo();

    if (Stem.IsEmpty())
    {
        if (Viewport.IsValid())
        {
            Viewport->ClearScene();
            Viewport->SetPaused(true);
        }
        RebuildInfoPanel();
        return;
    }

    BuildCheapInfo(Stem, Info);

    // Heavy info is cached per stem, so re-selecting an asset you have
    // already inspected is free. A section that is closed stays uncomputed.
    if (const FMapLinkAssetInfo* Cached = HeavyCache.Find(Stem))
    {
        const FMapLinkAssetInfo& C = *Cached;
        Info.bHeavyValid          = C.bHeavyValid;
        Info.bHeavyFailed         = C.bHeavyFailed;
        Info.NumMeshes            = C.NumMeshes;
        Info.NumTriangles         = C.NumTriangles;
        Info.NumVertices          = C.NumVertices;
        Info.MaxLODs              = C.MaxLODs;
        Info.NumMaterialSlots     = C.NumMaterialSlots;
        Info.MaterialNames        = C.MaterialNames;
        Info.TextureNames         = C.TextureNames;
        Info.NumTexturesNotConform = C.NumTexturesNotConform;
    }

    if (Viewport.IsValid())
    {
        FMapLinkViewerEntry Entry;
        Entry.Stem  = Stem;
        Entry.Type  = ViewerTypeFromStem(Stem);
        Entry.Proxy = FindProxyForStem(Stem);

        Viewport->SetPaused(false);
        Viewport->ShowEntry(Entry);
    }

    EnsureHeavyInfo();
    RebuildInfoPanel();
}

void SMapLinkAssetInspector::Sleep()
{
    if (bAsleep)
    {
        return;
    }
    bAsleep    = true;
    CurrentStem.Reset();

    if (Viewport.IsValid())
    {
        // Both, in this order: drop the spawned components, then stop the
        // client ticking. Pausing alone would leave a whole level's worth of
        // preview components resident for a panel nobody is looking at.
        Viewport->ClearScene();
        Viewport->SetPaused(true);
    }
}

void SMapLinkAssetInspector::InvalidateCache(const FString& Stem)
{
    if (Stem.IsEmpty())
    {
        HeavyCache.Reset();
    }
    else
    {
        HeavyCache.Remove(Stem);
    }
}

void SMapLinkAssetInspector::EnsureHeavyInfo()
{
    if (CurrentStem.IsEmpty() || Info.bHeavyValid || Info.bHeavyFailed)
    {
        return;
    }
    // The gate. Nothing below loads an asset unless the user has opened a
    // section that shows the result.
    if (!bOpenStats && !bOpenMaterials)
    {
        return;
    }

    BuildHeavyInfo(CurrentStem, Info);
    HeavyCache.Add(CurrentStem, Info);
}

// ---------------------------------------------------------------------------
// Cheap info — proxy fields and one analysis.json read
// ---------------------------------------------------------------------------

void SMapLinkAssetInspector::BuildCheapInfo(const FString& Stem,
                                            FMapLinkAssetInfo& Out)
{
    Out.Stem        = Stem;
    Out.TypeStr     = TypeFromStem(Stem);
    Out.ContentPath = FString::Printf(TEXT("/Game/MapLink/%s/"), *Stem);
    Out.bCheapValid = true;

    if (const UMapLinkProxyAsset* Proxy = FindProxyForStem(Stem))
    {
        Out.BlendPath = Proxy->BlendFilePath;

        Out.BlendHashShort = Proxy->BlendFileHash.IsEmpty()
            ? FString(TEXT("—"))
            : Proxy->BlendFileHash.Left(12) + TEXT("…");

        FDateTime LastImport;
        const bool bHasStamp =
            FDateTime::ParseIso8601(*Proxy->LastImportTimestamp, LastImport);

        FDateTime Display = LastImport;
        if (bHasStamp && UMapLinkSettings::Get()->bShowTimesInLocalTime)
        {
            Display += (FDateTime::Now() - FDateTime::UtcNow());
        }
        Out.LastImportStr = bHasStamp
            ? Display.ToString(TEXT("%Y-%m-%d %H:%M:%S"))
            : FString(TEXT("never"));

        // Same verdict the list column reaches, with the same evidence — one
        // source of truth for "is this stale", stated twice on purpose.
        static const TCHAR* Fmt = TEXT("%Y-%m-%d %H:%M:%S");
        const FDateTime FileTime =
            IFileManager::Get().GetTimeStamp(*Proxy->BlendFilePath);

        if (Proxy->BlendFilePath.IsEmpty() ||
            !FPaths::FileExists(Proxy->BlendFilePath))
        {
            Out.StateStr    = TEXT(".blend missing");
            Out.StateDetail = TEXT("The source file is not on disk at the "
                                   "recorded path.");
        }
        else if (Proxy->ImportedBlendModTime.GetTicks() > 0)
        {
            const bool bStale = FileTime > Proxy->ImportedBlendModTime;
            Out.StateStr = bStale ? TEXT("Out of date") : TEXT("Up to date");
            Out.StateDetail = FString::Printf(
                TEXT(".blend modified %s   ·   imported from %s"),
                *FileTime.ToString(Fmt),
                *Proxy->ImportedBlendModTime.ToString(Fmt));
        }
        else if (bHasStamp)
        {
            const bool bStale = FileTime > LastImport;
            Out.StateStr = bStale ? TEXT("Out of date (legacy check)")
                                  : TEXT("Up to date");
            Out.StateDetail = FString::Printf(
                TEXT(".blend modified %s   ·   imported at %s\n")
                TEXT("Legacy comparison — file mtime against the import wall "
                     "clock. Reimport once to switch to the exact check."),
                *FileTime.ToString(Fmt), *LastImport.ToString(Fmt));
        }
        else
        {
            Out.StateStr    = TEXT("Never imported?");
            Out.StateDetail = TEXT("The proxy carries no import timestamp.");
        }
    }
    else
    {
        Out.StateStr    = TEXT("No proxy");
        Out.StateDetail = TEXT("No MapLink proxy asset resolves to this stem.");
    }

    // Expansions this asset is a root of.
    for (const FMapLinkWPExpansionRecord& Rec :
         UMapLinkEditorState::Get()->ActiveExpansions)
    {
        if (Rec.RootBlendName == Stem)
        {
            Out.ExpansionStrs.Add(FString::Printf(TEXT("%s → %s  (depth %d)"),
                *Rec.GetLabel(), *Rec.GetLevelName(), Rec.ExpandDepth));
        }
    }

    // Children, from the same analysis.json the expansion walk reads. Pure
    // file read — no asset loading, which is what keeps this phase cheap.
    const FString AnalysisPath =
        FMapLinkUtils::ResolveArtifactLogFile(Stem / TEXT("analysis.json"));

    FMapLinkAnalysisReport Report;
    if (!FMapLinkInterchangeHelper::ParseAnalysisJson(AnalysisPath, Report))
    {
        Out.bAnalysisMissing = true;
        return;
    }

    Out.NumOwnGeo = Report.OwnGeo.Num();

    // Collapse repeats: a courtyard with forty instances of one bollard is
    // "SM_bollard ×40", not forty rows.
    TMap<FString, int32> Counts;
    TArray<FString> Order;
    for (const FMapLinkLinkedAssetEntry& Linked : Report.LinkedAssets)
    {
        const FString ChildName =
            FPaths::GetBaseFilename(Linked.SourceBlendPath);
        if (ChildName.IsEmpty()) continue;

        if (int32* Existing = Counts.Find(ChildName))
        {
            ++(*Existing);
        }
        else
        {
            Counts.Add(ChildName, 1);
            Order.Add(ChildName);
        }
    }

    Order.Sort();
    for (const FString& Name : Order)
    {
        FMapLinkInspectorChild Child;
        Child.Name          = Name;
        Child.TypeStr       = TypeFromStem(Name);
        Child.InstanceCount = Counts[Name];
        Out.Children.Add(Child);
    }
}

// ---------------------------------------------------------------------------
// Heavy info — this is the part that loads assets
// ---------------------------------------------------------------------------

void SMapLinkAssetInspector::BuildHeavyInfo(const FString& Stem,
                                            FMapLinkAssetInfo& Out)
{
    FAssetRegistryModule& ARM =
        FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
    IAssetRegistry& AR = ARM.Get();

    const FString MeshDir = FString::Printf(TEXT("/Game/MapLink/%s/Meshes"), *Stem);

    FARFilter Filter;
    Filter.PackagePaths.Add(FName(*MeshDir));
    Filter.bRecursivePaths = true;
    Filter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());

    TArray<FAssetData> MeshAssets;
    AR.GetAssets(Filter, MeshAssets);

    if (MeshAssets.Num() == 0)
    {
        // Not a failure: LI_ roots own no meshes of their own, and a BP_
        // reference proxy owns none either. Say so rather than showing zeros
        // that read like a broken import.
        Out.bHeavyValid = true;
        return;
    }

    TSet<FString> Materials;
    TSet<FString> Textures;

    for (const FAssetData& Data : MeshAssets)
    {
        UStaticMesh* Mesh = Cast<UStaticMesh>(Data.GetAsset());
        if (!Mesh) continue;

        ++Out.NumMeshes;
        Out.MaxLODs = FMath::Max(Out.MaxLODs, Mesh->GetNumLODs());

        if (Mesh->GetNumLODs() > 0)
        {
            Out.NumTriangles += Mesh->GetNumTriangles(0);
            Out.NumVertices  += Mesh->GetNumVertices(0);
        }

        const TArray<FStaticMaterial>& Slots = Mesh->GetStaticMaterials();
        Out.NumMaterialSlots += Slots.Num();

        for (const FStaticMaterial& Slot : Slots)
        {
            UMaterialInterface* MI = Slot.MaterialInterface;
            if (!MI) continue;

            Materials.Add(MI->GetName());

            // Textures the material actually references. GetUsedTextures is
            // the engine's own answer; walking parameters by hand would miss
            // everything baked into the parent graph.
            TArray<UTexture*> Used;
            MI->GetUsedTextures(Used, EMaterialQualityLevel::Num,
                                /*bAllQualityLevels=*/true,
                                ERHIFeatureLevel::Num,
                                /*bAllFeatureLevels=*/true);
            for (const UTexture* Tex : Used)
            {
                if (Tex)
                {
                    Textures.Add(Tex->GetPathName());
                }
            }
        }
    }

    Out.MaterialNames = Materials.Array();
    Out.MaterialNames.Sort();

    // Texture names for display, but the conform count is the useful part:
    // it says whether this asset will look the same on a teammate's machine.
    TArray<FString> TexPaths = Textures.Array();
    TexPaths.Sort();
    for (const FString& Path : TexPaths)
    {
        Out.TextureNames.Add(FPaths::GetBaseFilename(Path));
    }

    // The conform verdict comes from the last texture scan — read, never
    // triggered. RowIndexByAssetPath is exactly this lookup, so this stays a
    // hash probe per texture rather than a walk of all 245 rows.
    if (FMapLinkTextureScanner::HasResult())
    {
        const FMapLinkTextureScanResult& Scan =
            FMapLinkTextureScanner::GetLastResult();

        for (const FString& TexPath : Textures)
        {
            const int32* RowIndex = Scan.RowIndexByAssetPath.Find(TexPath);
            if (!RowIndex || !Scan.Rows.IsValidIndex(*RowIndex))
            {
                continue;   // not a MapLink-tracked texture; not our verdict
            }
            const EMapLinkTextureStatus Status = Scan.Rows[*RowIndex].Status;
            if (Status != EMapLinkTextureStatus::Conform &&
                Status != EMapLinkTextureStatus::Portable &&
                Status != EMapLinkTextureStatus::Plugin)
            {
                ++Out.NumTexturesNotConform;
            }
        }
    }

    Out.bHeavyValid = true;
}

// ---------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------

TSharedRef<SWidget> SMapLinkAssetInspector::MakeField(
    const FText& Label, const FString& Value, const FText& Tooltip)
{
    return SNew(SHorizontalBox)
        .ToolTipText(Tooltip)

        + SHorizontalBox::Slot()
        .AutoWidth()
        .VAlign(VAlign_Top)
        [
            SNew(SBox).WidthOverride(108.f)
            [
                SNew(STextBlock)
                .Text(Label)
                .ColorAndOpacity(FSlateColor::UseSubduedForeground())
            ]
        ]

        + SHorizontalBox::Slot()
        .FillWidth(1.f)
        .VAlign(VAlign_Top)
        [
            SNew(STextBlock)
            .Text(FText::FromString(Value))
            .AutoWrapText(true)
        ];
}

TSharedRef<SWidget> SMapLinkAssetInspector::MakeSection(
    const FText& Title, bool* OpenFlag, TSharedRef<SWidget> Body)
{
    return SNew(SExpandableArea)
        .InitiallyCollapsed(!(*OpenFlag))
        .AreaTitle(Title)
        .Padding(FMargin(10.f, 6.f))
        .OnAreaExpansionChanged_Lambda([this, OpenFlag](bool bExpanded)
        {
            *OpenFlag = bExpanded;
            // Opening a heavy section is the user asking for the load. This
            // is the only place asset loading is triggered from the UI.
            EnsureHeavyInfo();
            RebuildInfoPanel();
        })
        .BodyContent()
        [
            Body
        ];
}

void SMapLinkAssetInspector::RebuildInfoPanel()
{
    if (!InfoBox.IsValid())
    {
        return;
    }

    InfoBox->ClearChildren();

    if (CurrentStem.IsEmpty())
    {
        InfoBox->AddSlot()
        .AutoHeight()
        .Padding(12.f)
        [
            SNew(STextBlock)
            .AutoWrapText(true)
            .Text(LOCTEXT("InspectorNoSelection",
                "Select a single asset to inspect it.\n\n"
                "The inspector is deliberately single-selection: a preview of "
                "several assets is not a preview, and summed statistics answer "
                "a question nobody asked."))
            .ColorAndOpacity(FSlateColor::UseSubduedForeground())
        ];
        return;
    }

    // ── Header ──────────────────────────────────────────────────────
    InfoBox->AddSlot()
    .AutoHeight()
    .Padding(10.f, 8.f, 10.f, 4.f)
    [
        SNew(SHorizontalBox)

        + SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
        [
            SNew(STextBlock)
            .Text(FText::FromString(Info.Stem))
            .Font(FAppStyle::GetFontStyle("BoldFont"))
        ]

        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
        [
            SNew(STextBlock)
            .Text(FText::FromString(Info.TypeStr))
            .ColorAndOpacity(FSlateColor::UseSubduedForeground())
        ]
    ];

    // ── Identity & state ────────────────────────────────────────────
    {
        TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);

        const bool bStale = Info.StateStr.StartsWith(TEXT("Out of date"));
        const bool bBad   = Info.StateStr.Contains(TEXT("missing")) ||
                            Info.StateStr == TEXT("No proxy");

        Body->AddSlot().AutoHeight().Padding(0.f, 1.f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth()
            [
                SNew(SBox).WidthOverride(108.f)
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("FieldState", "State"))
                    .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                ]
            ]
            + SHorizontalBox::Slot().FillWidth(1.f)
            [
                SNew(STextBlock)
                .Text(FText::FromString(Info.StateStr))
                .ColorAndOpacity(FSlateColor(
                    bBad   ? FLinearColor(0.9f, 0.25f, 0.25f) :
                    bStale ? FLinearColor(1.0f, 0.62f, 0.30f)
                           : FLinearColor(0.45f, 0.85f, 0.45f)))
            ]
        ];

        Body->AddSlot().AutoHeight().Padding(108.f, 1.f, 0.f, 4.f)
        [
            SNew(STextBlock)
            .AutoWrapText(true)
            .Text(FText::FromString(Info.StateDetail))
            .ColorAndOpacity(FSlateColor::UseSubduedForeground())
        ];

        Body->AddSlot().AutoHeight().Padding(0.f, 1.f)
        [ MakeField(LOCTEXT("FieldLastImport", "Last import"), Info.LastImportStr) ];

        Body->AddSlot().AutoHeight().Padding(0.f, 1.f)
        [ MakeField(LOCTEXT("FieldHash", "Blend hash"), Info.BlendHashShort,
            LOCTEXT("FieldHashTip",
                "MD5 recorded at import. Verify (MD5) compares the file "
                "against this.")) ];

        Body->AddSlot().AutoHeight().Padding(0.f, 1.f)
        [ MakeField(LOCTEXT("FieldContent", "UE content"), Info.ContentPath) ];

        // The source path is long and the useful thing to do with it is open
        // it, so it is a link rather than a line of truncated text.
        if (!Info.BlendPath.IsEmpty())
        {
            const FString BlendPath = Info.BlendPath;
            Body->AddSlot().AutoHeight().Padding(0.f, 1.f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth()
                [
                    SNew(SBox).WidthOverride(108.f)
                    [
                        SNew(STextBlock)
                        .Text(LOCTEXT("FieldSource", "Source .blend"))
                        .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                    ]
                ]
                + SHorizontalBox::Slot().FillWidth(1.f)
                [
                    SNew(SHyperlink)
                    .Text(FText::FromString(BlendPath))
                    .ToolTipText(LOCTEXT("FieldSourceTip",
                        "Show this .blend in Explorer."))
                    .OnNavigate_Lambda([BlendPath]()
                    {
                        FPlatformProcess::ExploreFolder(*BlendPath);
                    })
                ]
            ];
        }

        if (Info.ExpansionStrs.Num() > 0)
        {
            Body->AddSlot().AutoHeight().Padding(0.f, 6.f, 0.f, 1.f)
            [ MakeField(LOCTEXT("FieldExpansions", "Expanded into"),
                        FString::Join(Info.ExpansionStrs, TEXT("\n"))) ];
        }

        InfoBox->AddSlot().AutoHeight().Padding(4.f, 2.f)
        [ MakeSection(LOCTEXT("SecIdentity", "Identity & state"),
                      &bOpenIdentity, Body) ];
    }

    // ── Children ────────────────────────────────────────────────────
    {
        TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);

        if (Info.bAnalysisMissing)
        {
            Body->AddSlot().AutoHeight()
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .Text(LOCTEXT("ChildrenNoAnalysis",
                    "No analysis.json for this asset — reimport it to record "
                    "its dependency list. Expansion falls back to placing it "
                    "whole while this is missing."))
                .ColorAndOpacity(FSlateColor(FLinearColor(1.f, 0.75f, 0.25f)))
            ];
        }
        else
        {
            Body->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f)
            [
                SNew(STextBlock)
                .Text(FText::FromString(FString::Printf(
                    TEXT("%d own mesh(es)   ·   %d linked asset(s)"),
                    Info.NumOwnGeo, Info.Children.Num())))
                .ColorAndOpacity(FSlateColor::UseSubduedForeground())
            ];

            for (const FMapLinkInspectorChild& Child : Info.Children)
            {
                Body->AddSlot().AutoHeight().Padding(0.f, 1.f)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth()
                    [
                        SNew(SBox).WidthOverride(30.f)
                        [
                            SNew(STextBlock)
                            .Text(FText::FromString(Child.TypeStr))
                            .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                        ]
                    ]
                    + SHorizontalBox::Slot().FillWidth(1.f)
                    [
                        SNew(STextBlock).Text(FText::FromString(Child.Name))
                    ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [
                        SNew(STextBlock)
                        .Text(FText::FromString(Child.InstanceCount > 1
                            ? FString::Printf(TEXT("×%d"), Child.InstanceCount)
                            : FString()))
                        .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                    ]
                ];
            }

            if (Info.Children.Num() == 0 && Info.NumOwnGeo == 0)
            {
                Body->AddSlot().AutoHeight()
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("ChildrenNone", "Nothing linked."))
                    .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                ];
            }
        }

        InfoBox->AddSlot().AutoHeight().Padding(4.f, 2.f)
        [ MakeSection(LOCTEXT("SecChildren", "Children"), &bOpenChildren, Body) ];
    }

    // ── Generated assets (heavy) ────────────────────────────────────
    {
        TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);

        if (!Info.bHeavyValid)
        {
            Body->AddSlot().AutoHeight()
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .Text(LOCTEXT("StatsNotLoaded",
                    "Expand this section to measure the generated meshes. It "
                    "loads them, so it is not done until you ask."))
                .ColorAndOpacity(FSlateColor::UseSubduedForeground())
            ];
        }
        else if (Info.NumMeshes == 0)
        {
            Body->AddSlot().AutoHeight()
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .Text(LOCTEXT("StatsNoMeshes",
                    "This asset owns no generated meshes of its own — an LI_ "
                    "composes children, a BP_ reference binds a Blueprint. "
                    "See Children."))
                .ColorAndOpacity(FSlateColor::UseSubduedForeground())
            ];
        }
        else
        {
            Body->AddSlot().AutoHeight().Padding(0.f, 1.f)
            [ MakeField(LOCTEXT("FieldMeshes", "Meshes"),
                        FString::Printf(TEXT("%d"), Info.NumMeshes)) ];
            Body->AddSlot().AutoHeight().Padding(0.f, 1.f)
            [ MakeField(LOCTEXT("FieldTris", "Triangles"),
                        Grouped(Info.NumTriangles),
                        LOCTEXT("FieldTrisTip", "LOD 0, summed across meshes.")) ];
            Body->AddSlot().AutoHeight().Padding(0.f, 1.f)
            [ MakeField(LOCTEXT("FieldVerts", "Vertices"),
                        Grouped(Info.NumVertices)) ];
            Body->AddSlot().AutoHeight().Padding(0.f, 1.f)
            [ MakeField(LOCTEXT("FieldLODs", "LODs (max)"),
                        FString::Printf(TEXT("%d"), Info.MaxLODs)) ];
            Body->AddSlot().AutoHeight().Padding(0.f, 1.f)
            [ MakeField(LOCTEXT("FieldSlots", "Material slots"),
                        FString::Printf(TEXT("%d"), Info.NumMaterialSlots)) ];
        }

        InfoBox->AddSlot().AutoHeight().Padding(4.f, 2.f)
        [ MakeSection(LOCTEXT("SecStats", "Generated assets"),
                      &bOpenStats, Body) ];
    }

    // ── Materials & textures (heavy) ────────────────────────────────
    {
        TSharedRef<SVerticalBox> Body = SNew(SVerticalBox);

        if (!Info.bHeavyValid)
        {
            Body->AddSlot().AutoHeight()
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .Text(LOCTEXT("MatsNotLoaded",
                    "Expand this section to read the materials and textures "
                    "this asset uses. It loads the meshes to do it."))
                .ColorAndOpacity(FSlateColor::UseSubduedForeground())
            ];
        }
        else
        {
            if (Info.NumTexturesNotConform > 0)
            {
                Body->AddSlot().AutoHeight().Padding(0.f, 0.f, 0.f, 4.f)
                [
                    SNew(STextBlock)
                    .AutoWrapText(true)
                    .Text(FText::Format(LOCTEXT("MatsNotConformFmt",
                        "{0} of this asset's textures are not conform — it "
                        "will not look the same on a teammate's machine."),
                        FText::AsNumber(Info.NumTexturesNotConform)))
                    .ColorAndOpacity(FSlateColor(FLinearColor(1.f, 0.75f, 0.25f)))
                ];
            }

            Body->AddSlot().AutoHeight().Padding(0.f, 1.f)
            [ MakeField(LOCTEXT("FieldMats", "Materials"),
                Info.MaterialNames.Num() > 0
                    ? FString::Join(Info.MaterialNames, TEXT("\n"))
                    : FString(TEXT("—"))) ];

            Body->AddSlot().AutoHeight().Padding(0.f, 4.f, 0.f, 1.f)
            [ MakeField(LOCTEXT("FieldTexes", "Textures"),
                Info.TextureNames.Num() > 0
                    ? FString::Join(Info.TextureNames, TEXT("\n"))
                    : FString(TEXT("—"))) ];
        }

        InfoBox->AddSlot().AutoHeight().Padding(4.f, 2.f)
        [ MakeSection(LOCTEXT("SecMaterials", "Materials & textures"),
                      &bOpenMaterials, Body) ];
    }
}

#undef LOCTEXT_NAMESPACE
