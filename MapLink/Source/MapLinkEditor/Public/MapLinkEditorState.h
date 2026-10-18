// Copyright MapModelers. All Rights Reserved.
//
// v1.9 — Per-project editor state for the MapLink Manager window.
//
// WP expansion is no longer driven by UPROPERTYs on the proxy assets —
// it is driven from the MapLink Manager window, and the "what is expanded
// where" state lives here, in EditorPerProjectUserSettings (NOT source-
// controlled, so each dev keeps their own expansion setup).

#pragma once

#include "CoreMinimal.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "UObject/Object.h"
#include "UObject/SoftObjectPath.h"
#include "MapLinkEditorState.generated.h"

/** One active WP expansion: a root LI_ blend expanded into a WP level.
 *
 *  v1.17 — the record carries its own identity.  Up to v1.16 the primary key
 *  was RootBlendName, which made three things unrepresentable: the same root
 *  in two levels, the same root twice in one level, and "who placed this".
 *  Worse, re-targeting a root overwrote TargetLevel in place, so the actors
 *  in the OLD level were left behind with no record pointing at them.
 *
 *  Id fixes all of it.  It is appended to every placed actor's tag as a
 *  trailing segment (see FMapLinkWPExpansionHelper::TagActor) — trailing,
 *  because every existing tag parser reads Parts[0..4] behind a
 *  Parts.Num() >= N guard, so v1.16 actors keep working untouched and no
 *  re-tag pass is needed. */
USTRUCT()
struct FMapLinkWPExpansionRecord
{
    GENERATED_BODY()

    /** v1.17 — stable identity for this expansion instance.  Assigned on
     *  creation; legacy records get one from UpgradeExpansionRecords(). */
    UPROPERTY()
    FGuid Id;

    /** Root LI_ blend stem (e.g. "LI_MyLevel"). */
    UPROPERTY()
    FString RootBlendName;

    /** Target World Partition level. */
    UPROPERTY()
    FSoftObjectPath TargetLevel;

    /** How many LI_ levels deep to break open. */
    UPROPERTY()
    int32 ExpandDepth = 1;

    /** v1.17 — display name and outliner folder segment (MapLink/<Label>).
     *  Empty means "use the root stem", which is what v1.16 always did. */
    UPROPERTY()
    FString Label;

    /** v1.17 — re-expand this record when a dependency reimports.  Lets a
     *  heavy expansion be parked without cleaning it out of the level. */
    UPROPERTY()
    bool bReExpandOnImport = true;

    /** v1.17 — actors placed by the last successful expansion (display). */
    UPROPERTY()
    int32 LastPlacedCount = 0;

    /** v1.17 — when this record last expanded (display). */
    UPROPERTY()
    FDateTime LastExpandedUtc = FDateTime(0);

    bool IsValidRecord() const
    {
        return !RootBlendName.IsEmpty() && !TargetLevel.IsNull();
    }

    /** Label, falling back to the root stem. */
    FString GetLabel() const
    {
        return Label.IsEmpty() ? RootBlendName : Label;
    }

    /** Top-level outliner folder this expansion owns. */
    FString GetFolderPath() const
    {
        return FString::Printf(TEXT("MapLink/%s"), *GetLabel());
    }

    /** Short form written into actor tags — 8 hex chars is 2^32 of headroom
     *  for a list that is realistically under a hundred entries. */
    FString GetIdTag() const
    {
        return Id.IsValid()
            ? Id.ToString(EGuidFormats::Digits).Left(8).ToLower()
            : FString();
    }

    FString GetLevelName() const
    {
        return FPaths::GetBaseFilename(TargetLevel.ToString());
    }
};

/** v1.16 — One cached texture scan row. Per-user, regenerable, never
 *  P4-tracked, safe to delete: it exists so the Textures view opens warm on
 *  several thousand textures without loading every package.
 *
 *  Staleness is FTextureSource::GetId() — a GUID the engine already maintains
 *  and bumps on every source edit — not a pixel hash. Hashing mip 0 of a few
 *  thousand 4K textures to answer "did anything change" is the kind of cost
 *  that quietly makes a tab unusable. */
USTRUCT()
struct FMapLinkTextureCacheRecord
{
    GENERATED_BODY()

    UPROPERTY() FString   AssetPath;

    /** AssetImportData's first filename, resolved to a full path. Machine-
     *  specific by nature, which is exactly why it belongs in the per-user
     *  cache and never in the P4-tracked policy file. Caching it is what lets a
     *  warm rescan skip loading the package at all. */
    UPROPERTY() FString   SourceDiskPath;

    UPROPERTY() bool      bSRGB = false;
    UPROPERTY() FString   Compression;

    UPROPERTY() FGuid     SourceId;        // FTextureSource::GetId()
    UPROPERTY() FDateTime PackageMTime;
    UPROPERTY() FDateTime DiskMTime;
    UPROPERTY() int64     DiskSize = 0;
    UPROPERTY() FString   DiskMD5;
    UPROPERTY() uint8     LastResult = 0;  // EMapLinkTextureResult
};

UCLASS(config=EditorPerProjectUserSettings)
class UMapLinkEditorState : public UObject
{
    GENERATED_BODY()

public:
    /** Stem of the last .blend that went through Import/Reimport.
     *  The Manager window defaults its proxy selector to this. */
    UPROPERTY(config)
    FString LastImportedBlendName;

    /** Last WP level used in the Manager window's expansion section. */
    UPROPERTY(config)
    FSoftObjectPath LastWPLevel;

    /** Last expansion depth used. */
    UPROPERTY(config)
    int32 LastExpandDepth = 1;

    /** Currently-active WP expansions.  Reimport consults this list to
     *  decide what to re-expand (replaces the old per-proxy
     *  bExpandIntoWP / TargetWPLevel / ExpandDepth properties). */
    UPROPERTY(config)
    TArray<FMapLinkWPExpansionRecord> ActiveExpansions;

    /** v1.19 — Assets tab inspector (3D preview + info panel) visible.
     *  Per-user and OFF by default: the preview loads assets, so an always-on
     *  inspector would make clicking down a hundred-row list expensive. */
    UPROPERTY(config)
    bool bShowAssetInspector = false;

    /** v1.9 — Pinned favourites (blend stems) for the Manager window.
     *  Per-user; favourites sort to the top of the asset list. */
    UPROPERTY(config)
    TArray<FString> FavouriteBlendNames;

    /** v1.10 — Material library out-of-sync flag.  Set when the invisible
     *  auto-sync engine fails to regenerate the Blender material libraries
     *  (P4 lock, disk/permission, headless crash/timeout) AFTER the UE-side
     *  material op has already committed.  UE is authoritative, so the op is
     *  never rolled back; instead this flag is raised and the Settings tab
     *  surfaces a "library out of sync — Resync" prompt.  Cleared on the next
     *  successful full sync (auto-sync tail step or manual Resync). */
    UPROPERTY(config)
    bool bMaterialLibraryDirty = false;

    /** v1.16 — Texture scan cache. Per-user, regenerable, never P4-tracked.
     *  Deleting it costs one full rescan and nothing else. */
    UPROPERTY(config)
    TArray<FMapLinkTextureCacheRecord> TextureCache;

    // ── Access ─────────────────────────────────────────────────────────

    static UMapLinkEditorState* Get()
    {
        return GetMutableDefault<UMapLinkEditorState>();
    }

    void Save()
    {
        SaveConfig();
    }

    // ── Expansions (v1.17 — keyed by Id, plural per root and per level) ──

    /** v1.17 — assign an Id/Label to any record written by v1.16 or earlier.
     *  Loss-free and idempotent; returns the number upgraded.  Called once
     *  when the Manager window opens. */
    int32 UpgradeExpansionRecords()
    {
        int32 Upgraded = 0;
        for (FMapLinkWPExpansionRecord& Rec : ActiveExpansions)
        {
            if (!Rec.Id.IsValid())
            {
                Rec.Id = FGuid::NewGuid();
                ++Upgraded;
            }
            if (Rec.ExpandDepth < 1)
            {
                Rec.ExpandDepth = 1;
            }
        }
        if (Upgraded > 0)
        {
            Save();
        }
        return Upgraded;
    }

    FMapLinkWPExpansionRecord* FindExpansionById(const FGuid& Id)
    {
        if (!Id.IsValid()) return nullptr;
        for (FMapLinkWPExpansionRecord& Rec : ActiveExpansions)
        {
            if (Rec.Id == Id)
            {
                return &Rec;
            }
        }
        return nullptr;
    }

    /** Exact (root, level) pair — the natural "is this already expanded
     *  here?" question the Expand form asks before creating a duplicate. */
    FMapLinkWPExpansionRecord* FindExpansion(const FString& RootBlendName,
                                             const FSoftObjectPath& TargetLevel)
    {
        for (FMapLinkWPExpansionRecord& Rec : ActiveExpansions)
        {
            if (Rec.RootBlendName == RootBlendName &&
                Rec.TargetLevel == TargetLevel)
            {
                return &Rec;
            }
        }
        return nullptr;
    }

    /** First record for a root, in list order.  Only for legacy/display
     *  paths — a root may now have several. */
    FMapLinkWPExpansionRecord* FindFirstExpansionForRoot(const FString& RootBlendName)
    {
        for (FMapLinkWPExpansionRecord& Rec : ActiveExpansions)
        {
            if (Rec.RootBlendName == RootBlendName)
            {
                return &Rec;
            }
        }
        return nullptr;
    }

    int32 CountExpansionsForRoot(const FString& RootBlendName) const
    {
        int32 Count = 0;
        for (const FMapLinkWPExpansionRecord& Rec : ActiveExpansions)
        {
            if (Rec.RootBlendName == RootBlendName)
            {
                ++Count;
            }
        }
        return Count;
    }

    /** Insert or update by Id.  An invalid Id creates a new record (and is
     *  written back into InOutRecord so the caller keeps the handle). */
    FGuid AddOrUpdateExpansion(FMapLinkWPExpansionRecord& InOutRecord)
    {
        if (!InOutRecord.Id.IsValid())
        {
            InOutRecord.Id = FGuid::NewGuid();
        }
        if (FMapLinkWPExpansionRecord* Existing = FindExpansionById(InOutRecord.Id))
        {
            *Existing = InOutRecord;
        }
        else
        {
            ActiveExpansions.Add(InOutRecord);
        }
        LastWPLevel     = InOutRecord.TargetLevel;
        LastExpandDepth = InOutRecord.ExpandDepth;
        Save();
        return InOutRecord.Id;
    }

    void RemoveExpansionById(const FGuid& Id)
    {
        ActiveExpansions.RemoveAll(
            [&Id](const FMapLinkWPExpansionRecord& Rec)
            {
                return Rec.Id == Id;
            });
        Save();
    }

    /** Drop every record for a root, whatever level it targets.  Used by the
     *  legacy CleanupWP(Root, Level) path and by proxy deletion. */
    void RemoveExpansionsForRoot(const FString& RootBlendName)
    {
        ActiveExpansions.RemoveAll(
            [&RootBlendName](const FMapLinkWPExpansionRecord& Rec)
            {
                return Rec.RootBlendName == RootBlendName;
            });
        Save();
    }

    // ── Favourites (v1.9) ──────────────────────────────────────────────

    bool IsFavourite(const FString& BlendName) const
    {
        return FavouriteBlendNames.Contains(BlendName);
    }

    void SetFavourite(const FString& BlendName, bool bFavourite)
    {
        const bool bHas = FavouriteBlendNames.Contains(BlendName);
        if (bFavourite && !bHas)
        {
            FavouriteBlendNames.Add(BlendName);
            Save();
        }
        else if (!bFavourite && bHas)
        {
            FavouriteBlendNames.Remove(BlendName);
            Save();
        }
    }

    void ToggleFavourite(const FString& BlendName)
    {
        SetFavourite(BlendName, !IsFavourite(BlendName));
    }

    // ── Texture scan cache (v1.16) ─────────────────────────────────────
    //
    // NOTE the deliberate deviation from the accessors above: these do NOT
    // Save() per mutation. Every existing setter here writes the whole ini on
    // every call, which is fine for a handful of expansions and catastrophic
    // for a scan touching several thousand texture rows. The scanner mutates
    // freely and calls Save() once when the pass completes.

    FMapLinkTextureCacheRecord* FindTextureCache(const FString& AssetPath)
    {
        for (FMapLinkTextureCacheRecord& Rec : TextureCache)
        {
            if (Rec.AssetPath == AssetPath)
            {
                return &Rec;
            }
        }
        return nullptr;
    }

    /** Insert or update. Does NOT save — call Save() once per scan. */
    void SetTextureCache(const FMapLinkTextureCacheRecord& Record)
    {
        if (FMapLinkTextureCacheRecord* Existing = FindTextureCache(Record.AssetPath))
        {
            *Existing = Record;
        }
        else
        {
            TextureCache.Add(Record);
        }
    }

    /** Does NOT save — call Save() once per scan. */
    void RemoveTextureCache(const FString& AssetPath)
    {
        TextureCache.RemoveAll(
            [&AssetPath](const FMapLinkTextureCacheRecord& Rec)
            {
                return Rec.AssetPath == AssetPath;
            });
    }

    /** Manual rescan forces a full recompute (§8.5). */
    void ClearTextureCache()
    {
        TextureCache.Reset();
        Save();
    }

    // ── Material library sync state (v1.10) ────────────────────────────

    bool IsMaterialLibraryDirty() const { return bMaterialLibraryDirty; }

    void SetMaterialLibraryDirty(bool bDirty)
    {
        if (bMaterialLibraryDirty != bDirty)
        {
            bMaterialLibraryDirty = bDirty;
            Save();
        }
    }
};
