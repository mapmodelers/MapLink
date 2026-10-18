// Copyright MapModelers. All Rights Reserved.
//
// v1.16 — Texture Manager vocabulary: the single place enum names are spelled.
//
// The policy file, the run manifest, the console commands and the Materials tab
// all round-trip through these functions, so none of them can drift from the
// others. Spelling a status string inline anywhere else is a bug waiting for a
// rename.

#include "MapLinkTextureTypes.h"

#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace MapLinkTexture
{

const TCHAR* ToString(EMapLinkTextureClass In)
{
    switch (In)
    {
    case EMapLinkTextureClass::MapLink:      return TEXT("MapLink");
    case EMapLinkTextureClass::External:     return TEXT("External");
    case EMapLinkTextureClass::Unclassified: break;
    }
    return TEXT("Unclassified");
}

const TCHAR* ToString(EMapLinkTextureStatus In)
{
    switch (In)
    {
    case EMapLinkTextureStatus::Conform:      return TEXT("Conform");
    case EMapLinkTextureStatus::Plugin:       return TEXT("Plugin");
    case EMapLinkTextureStatus::Portable:     return TEXT("Portable");
    case EMapLinkTextureStatus::LocalOnly:    return TEXT("LocalOnly");
    case EMapLinkTextureStatus::Broken:       return TEXT("Broken");
    case EMapLinkTextureStatus::Unclassified: break;
    }
    return TEXT("Unclassified");
}

const TCHAR* ToString(EMapLinkTextureResult In)
{
    switch (In)
    {
    case EMapLinkTextureResult::Conformed:              return TEXT("Conformed");
    case EMapLinkTextureResult::SkippedAlreadyConform:  return TEXT("SkippedAlreadyConform");
    case EMapLinkTextureResult::SkippedExternal:        return TEXT("SkippedExternal");
    case EMapLinkTextureResult::SkippedPluginContent:   return TEXT("SkippedPluginContent");
    case EMapLinkTextureResult::SkippedCancelled:       return TEXT("SkippedCancelled");
    case EMapLinkTextureResult::SkippedUnclassified:    return TEXT("SkippedUnclassified");
    case EMapLinkTextureResult::SkippedNoSourceData:    return TEXT("SkippedNoSourceData");
    case EMapLinkTextureResult::SkippedUnsupportedType: return TEXT("SkippedUnsupportedType");
    case EMapLinkTextureResult::ReducedLossy:           return TEXT("ReducedLossy");
    case EMapLinkTextureResult::AlphaSplit:             return TEXT("AlphaSplit");
    case EMapLinkTextureResult::Orphan:                 return TEXT("Orphan");
    case EMapLinkTextureResult::Failed:                 break;
    }
    return TEXT("Failed");
}

const TCHAR* ToString(EMapLinkTextureClaimPreset In)
{
    switch (In)
    {
    case EMapLinkTextureClaimPreset::Migrate: return TEXT("Migrate");
    case EMapLinkTextureClaimPreset::Adopt:   return TEXT("Adopt");
    case EMapLinkTextureClaimPreset::Copy:    return TEXT("Copy");
    case EMapLinkTextureClaimPreset::Fork:    break;
    }
    return TEXT("Fork");
}

bool ParseClass(const FString& In, EMapLinkTextureClass& Out)
{
    if (In.Equals(TEXT("MapLink"), ESearchCase::IgnoreCase))
    {
        Out = EMapLinkTextureClass::MapLink;      return true;
    }
    if (In.Equals(TEXT("External"), ESearchCase::IgnoreCase))
    {
        Out = EMapLinkTextureClass::External;     return true;
    }
    if (In.Equals(TEXT("Unclassified"), ESearchCase::IgnoreCase)
        || In.Equals(TEXT("None"), ESearchCase::IgnoreCase))
    {
        Out = EMapLinkTextureClass::Unclassified; return true;
    }
    return false;
}

bool ParsePreset(const FString& In, EMapLinkTextureClaimPreset& Out)
{
    if (In.Equals(TEXT("Fork"),    ESearchCase::IgnoreCase))
    {
        Out = EMapLinkTextureClaimPreset::Fork;    return true;
    }
    if (In.Equals(TEXT("Migrate"), ESearchCase::IgnoreCase))
    {
        Out = EMapLinkTextureClaimPreset::Migrate; return true;
    }
    if (In.Equals(TEXT("Adopt"),   ESearchCase::IgnoreCase))
    {
        Out = EMapLinkTextureClaimPreset::Adopt;   return true;
    }
    if (In.Equals(TEXT("Copy"),    ESearchCase::IgnoreCase))
    {
        Out = EMapLinkTextureClaimPreset::Copy;    return true;
    }
    return false;
}

// ── Compression ───────────────────────────────────────────────────────────
//
// Through the reflected enum rather than a hand-written switch, so an engine
// upgrade that adds a compression mode records its real name instead of
// silently collapsing it to TC_Default. A name we cannot resolve on read comes
// back as TC_Default and the caller logs it — losing the setting is bad, but
// pretending an unknown string is a known mode is worse.

FString CompressionToString(TextureCompressionSettings In)
{
    if (const UEnum* Enum = StaticEnum<TextureCompressionSettings>())
    {
        const FString Name = Enum->GetNameStringByValue(static_cast<int64>(In));
        if (!Name.IsEmpty())
        {
            return Name;
        }
    }
    return TEXT("TC_Default");
}

TextureCompressionSettings CompressionFromString(const FString& In)
{
    if (In.IsEmpty())
    {
        return TC_Default;
    }
    if (const UEnum* Enum = StaticEnum<TextureCompressionSettings>())
    {
        const int64 Value = Enum->GetValueByNameString(In);
        if (Value != INDEX_NONE)
        {
            return static_cast<TextureCompressionSettings>(Value);
        }
    }
    return TC_Default;
}

// ── Human text ────────────────────────────────────────────────────────────
//
// One line each, stating the fact. The UI carries the short form; this is the
// long form for logs and tooltips.

FText StatusText(EMapLinkTextureStatus In)
{
    switch (In)
    {
    case EMapLinkTextureStatus::Conform:
        return NSLOCTEXT("MapLink", "TexStatusConform",
            "Conform — MapLink owns it; source file in the MapLink project");
    case EMapLinkTextureStatus::Plugin:
        return NSLOCTEXT("MapLink", "TexStatusPlugin",
            "Plugin — ships with MapLink; identical everywhere, never conformed");
    case EMapLinkTextureStatus::Portable:
        return NSLOCTEXT("MapLink", "TexStatusPortable",
            "Project — not MapLink's, but inside the project tree; reaches teammates");
    case EMapLinkTextureStatus::LocalOnly:
        return NSLOCTEXT("MapLink", "TexStatusLocalOnly",
            "External — source file outside the project; resolves on this machine only");
    case EMapLinkTextureStatus::Broken:
        return NSLOCTEXT("MapLink", "TexStatusBroken",
            "Missing — no source data, file not found, or path too long");
    case EMapLinkTextureStatus::Unclassified:
        break;
    }
    return NSLOCTEXT("MapLink", "TexStatusUnclassified",
        "Unclassified — no class recorded");
}

} // namespace MapLinkTexture
