// Copyright MapModelers. All Rights Reserved.
//
// v1.16 — texture_policy.json (see header).

#include "MapLinkTexturePolicy.h"

#include "MapLinkTextureNaming.h"
#include "MapLinkUtils.h"
#include "MapLinkSettings.h"
#include "MapLinkSourceControl.h"
#include "MapLinkPipeline.h"   // LogMapLink

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"

namespace
{
    /** Asset path -> entry. Sorted only on the way out; a map keeps lookups
     *  cheap during a scan that touches every row. */
    TMap<FString, FMapLinkTexturePolicyEntry> GEntries;
    bool GLoaded = false;

    const TCHAR* KeySchemaVersion = TEXT("schema_version");
    const TCHAR* KeyTextures      = TEXT("textures");
    const TCHAR* KeyAssetPath     = TEXT("asset_path");
    const TCHAR* KeyClass         = TEXT("class");
    const TCHAR* KeySRGB          = TEXT("srgb");
    const TCHAR* KeyCompression   = TEXT("compression");
    const TCHAR* KeyPreviewMaxRes = TEXT("preview_max_res");
    const TCHAR* KeyNote          = TEXT("note");
    const TCHAR* KeyLastConform   = TEXT("last_conform");
    const TCHAR* KeyLossy         = TEXT("lossy_reduction");

    /** Serialise the whole store. `textures` is an ARRAY sorted by asset path,
     *  not an object: FJsonObject is backed by a TMap and would emit its keys in
     *  hash order, so an object would produce a different byte sequence on every
     *  run and churn a P4-tracked file for no reason. An array we sort ourselves
     *  is deterministic and diffs cleanly. */
    FString SerializeToString()
    {
        TArray<FString> Keys;
        GEntries.GetKeys(Keys);
        Keys.Sort();

        TArray<TSharedPtr<FJsonValue>> Items;
        Items.Reserve(Keys.Num());

        for (const FString& Key : Keys)
        {
            const FMapLinkTexturePolicyEntry& Entry = GEntries[Key];

            // A row with no decision, no note, no conform history and no
            // override says nothing a reader could act on. Writing one per
            // texture would make a read-only scan dirty a P4-tracked file and
            // open a changelist nobody asked for, on a project where that is
            // 244 rows of "we looked at this and formed no opinion".
            //
            // The sRGB and compression values are re-read from the asset on
            // every scan, so nothing is lost by omitting them until there is a
            // decision to hang them on.
            const bool bHasSomethingToSay =
                   Entry.TextureClass != EMapLinkTextureClass::Unclassified
                || !Entry.Note.IsEmpty()
                || !Entry.LastConform.IsEmpty()
                || Entry.PreviewMaxRes > 0
                || Entry.bLossyReduction;
            if (!bHasSomethingToSay)
            {
                continue;
            }

            TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
            Obj->SetStringField(KeyAssetPath, Entry.AssetPath);
            Obj->SetStringField(KeyClass,
                MapLinkTexture::ToString(Entry.TextureClass));
            Obj->SetBoolField  (KeySRGB, Entry.bSRGB);
            Obj->SetStringField(KeyCompression, Entry.Compression);

            // Optional fields are omitted when they carry no information, so a
            // freshly classified project produces a small readable file rather
            // than a wall of defaults.
            if (Entry.PreviewMaxRes > 0)
            {
                Obj->SetNumberField(KeyPreviewMaxRes, Entry.PreviewMaxRes);
            }
            if (!Entry.Note.IsEmpty())
            {
                Obj->SetStringField(KeyNote, Entry.Note);
            }
            if (!Entry.LastConform.IsEmpty())
            {
                Obj->SetStringField(KeyLastConform, Entry.LastConform);
            }
            if (Entry.bLossyReduction)
            {
                Obj->SetBoolField(KeyLossy, true);
            }

            Items.Add(MakeShared<FJsonValueObject>(Obj));
        }

        TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
        Root->SetNumberField(KeySchemaVersion, FMapLinkTexturePolicy::SchemaVersion);
        Root->SetArrayField(KeyTextures, Items);

        FString Out;
        TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
        FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
        return Out;
    }
}

const TCHAR* FMapLinkTexturePolicy::GetChangelistDescription()
{
    return TEXT("[MapLink] Texture Policy");
}

FString FMapLinkTexturePolicy::GetPolicyFilePath()
{
    const FString Root = UMapLinkSettings::ResolveBlenderProjectRoot(
        /*bPromptOnAutoDetect=*/false, /*bPromptOnMissing=*/false);
    if (Root.IsEmpty())
    {
        return FString();
    }
    return FPaths::ConvertRelativePathToFull(Root / TEXT("texture_policy.json"));
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

bool FMapLinkTexturePolicy::Load()
{
    GEntries.Reset();
    GLoaded = true;

    const FString Path = GetPolicyFilePath();
    if (Path.IsEmpty())
    {
        UE_LOG(LogMapLink, Verbose,
               TEXT("[Textures] No MapLink project root, so there is no policy "
                    "file to load yet."));
        return true;
    }

    if (!FPaths::FileExists(Path))
    {
        // Not an error. It means nobody has recorded a decision yet, which is
        // the correct state for a project that has never run a scan.
        return true;
    }

    FString Raw;
    if (!FFileHelper::LoadFileToString(Raw, *Path))
    {
        UE_LOG(LogMapLink, Error,
               TEXT("[Textures] Could not read '%s'."), *Path);
        return false;
    }

    TSharedPtr<FJsonObject> Root;
    TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
    if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
    {
        UE_LOG(LogMapLink, Error,
               TEXT("[Textures] '%s' is not valid JSON. No decisions were "
                    "loaded — fix or delete the file rather than letting a "
                    "scan re-suggest everything."), *Path);
        return false;
    }

    int32 FileSchema = 0;
    Root->TryGetNumberField(KeySchemaVersion, FileSchema);
    if (FileSchema > SchemaVersion)
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("[Textures] '%s' is schema %d and this build understands "
                    "%d. Reading what is recognisable; update MapLink before "
                    "saving or newer fields will be dropped."),
               *Path, FileSchema, SchemaVersion);
    }

    const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
    if (!Root->TryGetArrayField(KeyTextures, Items) || !Items)
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("[Textures] '%s' has no 'textures' array."), *Path);
        return true;
    }

    int32 Rejected = 0;
    for (const TSharedPtr<FJsonValue>& Item : *Items)
    {
        const TSharedPtr<FJsonObject>* Obj = nullptr;
        if (!Item.IsValid() || !Item->TryGetObject(Obj) || !Obj)
        {
            ++Rejected;
            continue;
        }

        FMapLinkTexturePolicyEntry Entry;
        if (!(*Obj)->TryGetStringField(KeyAssetPath, Entry.AssetPath)
            || Entry.AssetPath.IsEmpty())
        {
            ++Rejected;
            continue;
        }

        FString ClassName;
        if ((*Obj)->TryGetStringField(KeyClass, ClassName))
        {
            MapLinkTexture::ParseClass(ClassName, Entry.TextureClass);
        }

        (*Obj)->TryGetBoolField  (KeySRGB,        Entry.bSRGB);
        (*Obj)->TryGetStringField(KeyCompression, Entry.Compression);
        (*Obj)->TryGetNumberField(KeyPreviewMaxRes, Entry.PreviewMaxRes);
        (*Obj)->TryGetStringField(KeyNote,        Entry.Note);
        (*Obj)->TryGetStringField(KeyLastConform, Entry.LastConform);
        (*Obj)->TryGetBoolField  (KeyLossy,       Entry.bLossyReduction);

        FString Reason;
        if (!ValidateEntry(Entry, Reason))
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("[Textures] Policy entry for '%s' %s. Loaded anyway "
                        "with the offending value normalised."),
                   *Entry.AssetPath, *Reason);
        }

        GEntries.Add(Entry.AssetPath, Entry);
    }

    if (Rejected > 0)
    {
        UE_LOG(LogMapLink, Warning,
               TEXT("[Textures] %d policy entries in '%s' had no usable "
                    "asset_path and were skipped."), Rejected, *Path);
    }

    UE_LOG(LogMapLink, Log,
           TEXT("[Textures] Loaded %d texture policy entries from '%s'."),
           GEntries.Num(), *Path);
    return true;
}

void FMapLinkTexturePolicy::EnsureLoaded()
{
    if (!GLoaded)
    {
        Load();
    }
}

void FMapLinkTexturePolicy::Invalidate()
{
    GEntries.Reset();
    GLoaded = false;
}

// ---------------------------------------------------------------------------
// Save — atomic, change-compared, source-controlled
// ---------------------------------------------------------------------------

bool FMapLinkTexturePolicy::Save(bool* bOutWrote)
{
    if (bOutWrote)
    {
        *bOutWrote = false;
    }

    const FString Path = GetPolicyFilePath();
    if (Path.IsEmpty())
    {
        UE_LOG(LogMapLink, Error,
               TEXT("[Textures] Cannot save the texture policy: no MapLink "
                    "project root is configured. Set it in MapLink > Project."));
        return false;
    }

    const FString NewJson = SerializeToString();

    // Change-compare BEFORE touching source control. A rescan that changed no
    // decisions must not check a P4-tracked file out — otherwise every artist
    // ends up with a pending changelist they did not ask for.
    FString ExistingJson;
    const bool bExists = FFileHelper::LoadFileToString(ExistingJson, *Path);
    if (bExists && ExistingJson.Equals(NewJson, ESearchCase::CaseSensitive))
    {
        UE_LOG(LogMapLink, Verbose,
               TEXT("[Textures] Texture policy unchanged: %s"), *Path);
        return true;
    }

    IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
    IFileManager&  FM = IFileManager::Get();

    PF.CreateDirectoryTree(*FPaths::GetPath(Path));

    // ── Temp on the same volume, validated, then replaced ─────────────────
    //
    // Same pattern as FMapLinkMaterialExporter::AtomicRegenLibrary. A
    // half-written policy file is worse than no policy file: it would silently
    // reclassify textures on the next load, and the classification is the part
    // a human decided.
    const FString TempPath = Path + TEXT(".maplinktmp");
    FM.Delete(*TempPath, /*RequireExists=*/false, /*EvenReadOnly=*/true);

    if (!FFileHelper::SaveStringToFile(
            NewJson, *TempPath,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        UE_LOG(LogMapLink, Error,
               TEXT("[Textures] Could not write the temporary policy file "
                    "'%s'. The real file was left untouched."), *TempPath);
        return false;
    }

    // Prove the temp parses before it is allowed anywhere near the real file.
    {
        FString Verify;
        TSharedPtr<FJsonObject> VerifyRoot;
        if (!FFileHelper::LoadFileToString(Verify, *TempPath))
        {
            FM.Delete(*TempPath, false, true);
            UE_LOG(LogMapLink, Error,
                   TEXT("[Textures] Temporary policy file '%s' could not be "
                        "read back. The real file was left untouched."),
                   *TempPath);
            return false;
        }
        TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Verify);
        if (!FJsonSerializer::Deserialize(Reader, VerifyRoot) || !VerifyRoot.IsValid())
        {
            FM.Delete(*TempPath, false, true);
            UE_LOG(LogMapLink, Error,
                   TEXT("[Textures] Temporary policy file '%s' did not parse "
                        "back. The real file was left untouched."), *TempPath);
            return false;
        }
    }

    // Check the real file out before replacing it — but ONLY if it exists.
    // Calling this on a path with nothing at it makes the provider attempt a
    // MarkForAdd, which fails with a red "the file doesn't exist on disk"
    // error before the add below quietly succeeds. Two log lines, one of them
    // alarming, for a completely normal first run.
    //
    // Graceful no-op without a source-control provider, per the standing
    // contract for marketplace users without Perforce.
    if (bExists)
    {
        FMapLinkSourceControl::EnsureFileInChangelist(
            Path, GetChangelistDescription());
        PF.SetReadOnly(*Path, false);
    }

    if (!FM.Move(*Path, *TempPath,
                 /*bReplace=*/true, /*bEvenIfReadOnly=*/true,
                 /*bAttributes=*/false, /*bDoNotRetryOrError=*/false))
    {
        FM.Delete(*TempPath, false, true);
        UE_LOG(LogMapLink, Error,
               TEXT("[Textures] Could not replace '%s'. Is it checked out by "
                    "someone else?"), *Path);
        return false;
    }

    // A brand-new file needs marking for add AFTER it exists.
    if (!bExists)
    {
        FMapLinkSourceControl::EnsureFileInChangelist(
            Path, GetChangelistDescription());
    }

    if (bOutWrote)
    {
        *bOutWrote = true;
    }

    UE_LOG(LogMapLink, Log,
           TEXT("[Textures] Texture policy %s: %s (%d entries)"),
           bExists ? TEXT("UPDATED") : TEXT("CREATED"), *Path, GEntries.Num());
    return true;
}

// ---------------------------------------------------------------------------
// Access
// ---------------------------------------------------------------------------

// Every public accessor keys through this. The policy is keyed by OBJECT path
// ("/Game/MapLink/Textures/T_Mk_Red.T_Mk_Red") because that is what the scanner
// reads out of the AssetRegistry — but a human types the PACKAGE path, and a
// lookup of one against the other misses. That miss is invisible: GetClass
// returns Unclassified, which is a real state, so the operation reports
// "classify it first" for a texture that is already classified.
//
// Normalising here rather than at each call site means a future UI, or a
// scripted caller, cannot reintroduce the bug.
static FString PolicyKey(const FString& AssetPath)
{
    return FMapLinkTextureNaming::NormalizeAssetPath(AssetPath);
}

const FMapLinkTexturePolicyEntry* FMapLinkTexturePolicy::Find(const FString& AssetPath)
{
    EnsureLoaded();
    return GEntries.Find(PolicyKey(AssetPath));
}

EMapLinkTextureClass FMapLinkTexturePolicy::GetClass(const FString& AssetPath)
{
    const FMapLinkTexturePolicyEntry* Entry = Find(AssetPath);
    return Entry ? Entry->TextureClass : EMapLinkTextureClass::Unclassified;
}

void FMapLinkTexturePolicy::SetClass(const FString& AssetPath,
                                     EMapLinkTextureClass NewClass)
{
    EnsureLoaded();
    const FString Key = PolicyKey(AssetPath);
    if (Key.IsEmpty())
    {
        return;
    }

    FMapLinkTexturePolicyEntry& Entry = GEntries.FindOrAdd(Key);
    Entry.AssetPath    = Key;
    Entry.TextureClass = NewClass;
}

void FMapLinkTexturePolicy::Set(const FMapLinkTexturePolicyEntry& InEntry)
{
    EnsureLoaded();
    const FString Key = PolicyKey(InEntry.AssetPath);
    if (Key.IsEmpty())
    {
        return;
    }
    FMapLinkTexturePolicyEntry Stored = InEntry;
    Stored.AssetPath = Key;
    GEntries.Add(Key, MoveTemp(Stored));
}

void FMapLinkTexturePolicy::RecordAssetSettings(const FString& AssetPath,
                                                bool bSRGB,
                                                const FString& Compression)
{
    EnsureLoaded();
    const FString Key = PolicyKey(AssetPath);
    if (Key.IsEmpty())
    {
        return;
    }

    FMapLinkTexturePolicyEntry& Entry = GEntries.FindOrAdd(Key);
    Entry.AssetPath   = Key;
    Entry.bSRGB       = bSRGB;
    Entry.Compression = Compression;
    // Class, note, conform history and the lossy flag are deliberately not
    // touched: these two values are READ from the asset every scan, the rest
    // are decisions and history.
}

void FMapLinkTexturePolicy::StampConform(const FString& AssetPath,
                                         const FString& OperationName,
                                         bool bLossyReduction)
{
    EnsureLoaded();
    const FString Key = PolicyKey(AssetPath);
    if (Key.IsEmpty())
    {
        return;
    }

    FMapLinkTexturePolicyEntry& Entry = GEntries.FindOrAdd(Key);
    Entry.AssetPath = Key;
    Entry.LastConform = FString::Printf(TEXT("%s %s"),
        *FDateTime::UtcNow().ToIso8601(), *OperationName);
    Entry.bLossyReduction = bLossyReduction;
}

void FMapLinkTexturePolicy::Remove(const FString& AssetPath)
{
    EnsureLoaded();
    GEntries.Remove(PolicyKey(AssetPath));
}

void FMapLinkTexturePolicy::GetAll(TArray<FMapLinkTexturePolicyEntry>& OutEntries)
{
    EnsureLoaded();

    TArray<FString> Keys;
    GEntries.GetKeys(Keys);
    Keys.Sort();

    OutEntries.Reset(Keys.Num());
    for (const FString& Key : Keys)
    {
        OutEntries.Add(GEntries[Key]);
    }
}

int32 FMapLinkTexturePolicy::Num()
{
    EnsureLoaded();
    return GEntries.Num();
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

bool FMapLinkTexturePolicy::ValidateEntry(const FMapLinkTexturePolicyEntry& Entry,
                                          FString& OutReason)
{
    if (Entry.PreviewMaxRes < 0 || Entry.PreviewMaxRes > 16384)
    {
        OutReason = FString::Printf(
            TEXT("has preview_max_res %d, which is outside 0..16384 (0 means "
                 "use the project default)"), Entry.PreviewMaxRes);
        return false;
    }

    if (!Entry.Compression.IsEmpty())
    {
        // Round-trip through the reflected enum. A name that does not resolve
        // comes back as TC_Default, which would silently change how a texture
        // is treated — worth one warning rather than years of confusion.
        const TextureCompressionSettings Parsed =
            MapLinkTexture::CompressionFromString(Entry.Compression);
        const FString RoundTripped = MapLinkTexture::CompressionToString(Parsed);
        if (!RoundTripped.Equals(Entry.Compression, ESearchCase::CaseSensitive))
        {
            OutReason = FString::Printf(
                TEXT("names compression '%s', which this engine version does "
                     "not have (it will be read as '%s')"),
                *Entry.Compression, *RoundTripped);
            return false;
        }
    }

    OutReason.Reset();
    return true;
}
