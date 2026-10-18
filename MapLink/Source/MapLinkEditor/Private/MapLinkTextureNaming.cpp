// Copyright MapModelers. All Rights Reserved.
//
// v1.16 — Texture naming and path arithmetic (see header).

#include "MapLinkTextureNaming.h"

#include "MapLinkUtils.h"
#include "MapLinkSettings.h"
#include "MapLinkPipeline.h"   // LogMapLink

#include "Misc/Paths.h"

namespace
{
    /** Trailing type suffixes preserved verbatim through truncation. Ordered
     *  longest-first so "_ORM" is not matched as "_R" would-be. */
    const TCHAR* const TypeSuffixes[] =
    {
        TEXT("_ORM"), TEXT("_AO"), TEXT("_D"), TEXT("_N"),
        TEXT("_H"),   TEXT("_R"),  TEXT("_M"),
    };

    /** Split a stem into (body, type suffix). Suffix is empty when none. */
    void SplitTypeSuffix(const FString& Stem, FString& OutBody, FString& OutSuffix)
    {
        for (const TCHAR* Suffix : TypeSuffixes)
        {
            if (Stem.EndsWith(Suffix, ESearchCase::CaseSensitive))
            {
                const int32 SuffixLen = FCString::Strlen(Suffix);
                OutBody   = Stem.Left(Stem.Len() - SuffixLen);
                OutSuffix = Suffix;
                return;
            }
        }
        OutBody   = Stem;
        OutSuffix.Reset();
    }

    /** Drive letter, or empty when there isn't one (UNC, relative, POSIX). */
    FString VolumeOf(const FString& AbsolutePath)
    {
        if (AbsolutePath.Len() >= 2 && AbsolutePath[1] == TEXT(':'))
        {
            return AbsolutePath.Left(1).ToUpper();
        }
        return FString();
    }

    FString NormalizeDir(const FString& In)
    {
        if (In.IsEmpty())
        {
            return FString();
        }
        FString Out = FPaths::ConvertRelativePathToFull(In);
        FPaths::NormalizeDirectoryName(Out);
        return Out;
    }

    bool IsUnder(const FString& NormalizedPath, const FString& NormalizedDir)
    {
        if (NormalizedDir.IsEmpty())
        {
            return false;
        }
        const FString DirWithSlash = NormalizedDir + TEXT("/");
        return NormalizedPath.StartsWith(DirWithSlash, ESearchCase::IgnoreCase);
    }
}

const TCHAR* FMapLinkTextureNaming::GetPrefix()
{
    return TEXT("T_Mk_");
}

const TCHAR* FMapLinkTextureNaming::GetExternalPreviewPrefix()
{
    return TEXT("T_Mk_Ext_");
}

// ---------------------------------------------------------------------------
// Stem derivation — §4.2
// ---------------------------------------------------------------------------

FString FMapLinkTextureNaming::DeriveStem(const FString& SourceAssetName)
{
    // Blender's ".001" duplicate tail first, so it never survives into the
    // sanitiser as "_001" and masquerades as meaningful.
    FString Work = FMapLinkUtils::StripBlenderSuffix(SourceAssetName);

    // A leading "T_" is the texture-type marker we are about to re-add as part
    // of "T_Mk_". Keeping it would produce T_Mk_T_Foo.
    if (Work.StartsWith(TEXT("T_"), ESearchCase::CaseSensitive))
    {
        Work.RightChopInline(2, EAllowShrinking::No);
    }

    // Sanitise to [A-Za-z0-9_], collapsing every run of separators to one
    // underscore. Anything that is not alphanumeric counts as a separator, so
    // "Grass04_2.5x2.5_4K" and "Grass04 2-5x2-5 4K" land on the same stem.
    FString Sanitised;
    Sanitised.Reserve(Work.Len());
    bool bPendingSeparator = false;
    for (const TCHAR Ch : Work)
    {
        const bool bKeep = (Ch >= TEXT('A') && Ch <= TEXT('Z'))
                        || (Ch >= TEXT('a') && Ch <= TEXT('z'))
                        || (Ch >= TEXT('0') && Ch <= TEXT('9'));
        if (bKeep)
        {
            if (bPendingSeparator && Sanitised.Len() > 0)
            {
                Sanitised.AppendChar(TEXT('_'));
            }
            bPendingSeparator = false;
            Sanitised.AppendChar(Ch);
        }
        else
        {
            bPendingSeparator = true;
        }
    }

    // LEADING DIGITS ARE KEPT. This used to strip them, guarding a rule that
    // does not apply: "a UE asset name may not start with a digit" is true of
    // the ASSET NAME, and the asset name is always GetPrefix() + Stem, so it
    // always starts with 'T'. The stem is never on its own. The guard was
    // written for a bare stem and applied to one that is always composed.
    //
    // What it cost, measured 2026-08-24 on a photogrammetry set where the
    // leading number IS the scan identity:
    //
    //     090654_baseTexBaked -> T_Mk_BaseTexBaked
    //     093452_baseTexBaked -> T_Mk_BaseTexBaked_01
    //     094212_baseTexBaked -> T_Mk_BaseTexBaked_02
    //
    // Three distinct scans collapsed onto one name and were then told apart by
    // a collision suffix in whatever order the run happened to reach them.
    // Silently discarding the only part of a name that identified the asset,
    // and replacing it with an ordering artefact, is the worst thing a
    // renaming tool can do — the artist cannot get it back from the result.
    //
    // A stem that is ALL digits still cannot stand alone if the prefix ever
    // becomes empty, so keep that one narrow case honest rather than assuming.
    if (Sanitised.Len() > 0 && FString(GetPrefix()).IsEmpty())
    {
        int32 FirstNonDigit = 0;
        while (FirstNonDigit < Sanitised.Len()
               && Sanitised[FirstNonDigit] >= TEXT('0')
               && Sanitised[FirstNonDigit] <= TEXT('9'))
        {
            ++FirstNonDigit;
        }
        if (FirstNonDigit > 0)
        {
            Sanitised.RightChopInline(FirstNonDigit, EAllowShrinking::No);
        }
    }

    while (Sanitised.StartsWith(TEXT("_"), ESearchCase::CaseSensitive))
    {
        Sanitised.RightChopInline(1, EAllowShrinking::No);
    }
    while (Sanitised.EndsWith(TEXT("_"), ESearchCase::CaseSensitive))
    {
        Sanitised.LeftChopInline(1, EAllowShrinking::No);
    }

    // Capitalise the first character.
    //
    // A stem like "red" composes into T_Mk_red — which is what the first real
    // run produced. UE asset names are PascalCase by convention, and
    // §4.5 makes the asset name the filename, so a lowercase stem puts a
    // lowercase file on disk permanently. A stem now allowed to start with a
    // digit is simply left alone by this. Only the FIRST character is touched:
    // "grass_4K_D" must not become "GRASS_4K_D", and interior casing an artist
    // chose is theirs to keep.
    if (Sanitised.Len() > 0)
    {
        Sanitised[0] = FChar::ToUpper(Sanitised[0]);
    }

    // §4.3 — truncate from the END, preserving the type suffix, because "_N" is
    // the difference between a normal map and everything else.
    if (Sanitised.Len() > MaxStemLength)
    {
        FString Body, Suffix;
        SplitTypeSuffix(Sanitised, Body, Suffix);

        const int32 Budget = MaxStemLength - Suffix.Len();
        if (Budget <= 0)
        {
            Sanitised = Sanitised.Left(MaxStemLength);
        }
        else
        {
            Body = Body.Left(Budget);
            while (Body.EndsWith(TEXT("_"), ESearchCase::CaseSensitive))
            {
                Body.LeftChopInline(1, EAllowShrinking::No);
            }
            Sanitised = Body + Suffix;
        }
    }

    return Sanitised;
}

FString FMapLinkTextureNaming::MakeAssetName(const FString& Stem)
{
    return Stem.IsEmpty() ? FString() : FString(GetPrefix()) + Stem;
}

FString FMapLinkTextureNaming::NormalizeAssetPath(const FString& InPath)
{
    FString Work = InPath.TrimStartAndEnd();
    if (Work.IsEmpty())
    {
        return FString();
    }

    // Right-click > Copy Reference yields Texture2D'/Game/.../T_Mk_Red.T_Mk_Red'.
    // Peel the class and the quotes so a pasted reference is accepted verbatim
    // rather than failing with a message about a texture that plainly exists.
    int32 QuoteIdx = INDEX_NONE;
    if (Work.FindChar(TEXT('\''), QuoteIdx))
    {
        Work.RightChopInline(QuoteIdx + 1, EAllowShrinking::No);
    }
    Work.ReplaceInline(TEXT("'"), TEXT(""), ESearchCase::CaseSensitive);
    Work.ReplaceInline(TEXT("\""), TEXT(""), ESearchCase::CaseSensitive);
    Work.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
    Work.TrimStartAndEndInline();

    while (Work.EndsWith(TEXT("/"), ESearchCase::CaseSensitive))
    {
        Work.LeftChopInline(1, EAllowShrinking::No);
    }
    if (Work.IsEmpty())
    {
        return FString();
    }

    int32 LastSlash = INDEX_NONE;
    Work.FindLastChar(TEXT('/'), LastSlash);
    const FString Leaf = (LastSlash == INDEX_NONE)
        ? Work
        : Work.RightChop(LastSlash + 1);

    // A dot in the leaf means it is already an object path. Anywhere earlier in
    // the string it is part of a folder name and none of our business.
    if (Leaf.Contains(TEXT("."), ESearchCase::CaseSensitive))
    {
        return Work;
    }
    return Work + TEXT(".") + Leaf;
}

FString FMapLinkTextureNaming::MakeExternalPreviewName(const FString& Stem)
{
    return Stem.IsEmpty() ? FString()
                          : FString(GetExternalPreviewPrefix()) + Stem;
}

bool FMapLinkTextureNaming::IsMapLinkAssetName(const FString& AssetName)
{
    const FString Prefix = GetPrefix();
    if (!AssetName.StartsWith(Prefix, ESearchCase::CaseSensitive))
    {
        return false;
    }
    if (AssetName.Len() <= Prefix.Len())
    {
        return false;   // bare "T_Mk_" is not a name
    }
    for (int32 i = Prefix.Len(); i < AssetName.Len(); ++i)
    {
        const TCHAR Ch = AssetName[i];
        const bool bLegal = (Ch >= TEXT('A') && Ch <= TEXT('Z'))
                         || (Ch >= TEXT('a') && Ch <= TEXT('z'))
                         || (Ch >= TEXT('0') && Ch <= TEXT('9'))
                         || Ch == TEXT('_');
        if (!bLegal)
        {
            return false;
        }
    }
    return true;
}

bool FMapLinkTextureNaming::ResolveCollision(const FString& DesiredAssetName,
                                             TSet<FString>& Taken,
                                             FString&       OutResolved)
{
    if (!Taken.Contains(DesiredAssetName))
    {
        Taken.Add(DesiredAssetName);
        OutResolved = DesiredAssetName;
        return true;
    }

    for (int32 Suffix = 1; Suffix <= 99; ++Suffix)
    {
        const FString Candidate = FString::Printf(TEXT("%s_%02d"),
                                                  *DesiredAssetName, Suffix);
        if (Candidate.Len() > MaxAssetNameLength)
        {
            break;
        }
        if (!Taken.Contains(Candidate))
        {
            Taken.Add(Candidate);
            OutResolved = Candidate;
            return true;
        }
    }

    // Never silently merge two textures into one name. Report and stop.
    OutResolved.Reset();
    return false;
}

// ---------------------------------------------------------------------------
// Path composition
// ---------------------------------------------------------------------------

FString FMapLinkTextureNaming::GetTexturesPackagePath()
{
    FString Root = FMapLinkUtils::GetMapLinkContentRoot();   // "/Game/MapLink/"
    Root.RemoveFromEnd(TEXT("/"));
    return Root / TEXT("Textures");                          // "/Game/MapLink/Textures"
}

bool FMapLinkTextureNaming::IsTexturesPackagePath(const FString& PackagePath)
{
    FString Normalized = PackagePath;
    Normalized.RemoveFromEnd(TEXT("/"));
    return Normalized.Equals(GetTexturesPackagePath(), ESearchCase::CaseSensitive);
}

FString FMapLinkTextureNaming::ComposeAssetPath(const FString& AssetName)
{
    return AssetName.IsEmpty() ? FString()
                               : GetTexturesPackagePath() / AssetName;
}

FString FMapLinkTextureNaming::ComposeSourcePath(const FString& AssetName)
{
    const FString Dir = FMapLinkUtils::GetTextureSourceDir();
    if (Dir.IsEmpty() || AssetName.IsEmpty())
    {
        return FString();
    }
    return Dir / (AssetName + TEXT(".tga"));
}

bool FMapLinkTextureNaming::ValidateLengths(const FString& AssetName,
                                            const FString& AbsoluteSourcePath,
                                            FString&       OutReason)
{
    if (AssetName.Len() > MaxAssetNameLength)
    {
        OutReason = FString::Printf(
            TEXT("asset name is %d characters, over the %d limit"),
            AssetName.Len(), MaxAssetNameLength);
        return false;
    }
    if (AbsoluteSourcePath.Len() > MaxAbsolutePathLength)
    {
        OutReason = FString::Printf(
            TEXT("source path would be %d characters, over the %d limit — P4 "
                 "workspace roots are deep and MAX_PATH still bites"),
            AbsoluteSourcePath.Len(), MaxAbsolutePathLength);
        return false;
    }
    OutReason.Reset();
    return true;
}

// ---------------------------------------------------------------------------
// Portability
// ---------------------------------------------------------------------------

FString FMapLinkTextureNaming::GetLibraryAnchorDir()
{
    if (const UMapLinkSettings* Settings = UMapLinkSettings::Get())
    {
        const FString Configured = Settings->MaterialInstancesBlend.FilePath;
        if (!Configured.IsEmpty())
        {
            return NormalizeDir(FPaths::GetPath(
                FPaths::ConvertRelativePathToFull(Configured)));
        }
    }

    const FString Root = UMapLinkSettings::ResolveBlenderProjectRoot(
        /*bPromptOnAutoDetect=*/false, /*bPromptOnMissing=*/false);
    if (Root.IsEmpty())
    {
        return FString();
    }
    return NormalizeDir(Root / TEXT("MaterialInstances"));
}

bool FMapLinkTextureNaming::ComputePortablePath(const FString& AbsoluteSourcePath,
                                                FString&       OutRelative)
{
    return ComputePortablePathFrom(AbsoluteSourcePath,
                                   GetLibraryAnchorDir(), OutRelative);
}

bool FMapLinkTextureNaming::ComputePortablePathFrom(
    const FString& AbsoluteSourcePath,
    const FString& AnchorDir,
    FString&       OutRelative)
{
    OutRelative.Reset();

    if (AbsoluteSourcePath.IsEmpty() || AnchorDir.IsEmpty())
    {
        return false;
    }

    const FString Source = FPaths::ConvertRelativePathToFull(AbsoluteSourcePath);
    const FString Anchor = NormalizeDir(AnchorDir);

    // A relative path cannot cross volumes. On Windows this is the single most
    // common reason a texture is machine-local: the project on C:, a scratch
    // library on D:.
    const FString SourceVolume = VolumeOf(Source);
    const FString AnchorVolume = VolumeOf(Anchor);
    if (SourceVolume != AnchorVolume)
    {
        return false;
    }

    // Tier 1 — inside the MapLink project root. P4-synced, everyone has it.
    const FString ProjectRoot = NormalizeDir(
        UMapLinkSettings::ResolveBlenderProjectRoot(false, false));
    bool bInsideSyncedTree = IsUnder(Source, ProjectRoot);

    // Tier 2 — inside the UE project. Also P4-synced, also on everyone's disk,
    // just under a different root. This is the mode where an artist points
    // Blender at a UE project texture by hand: it works today only on the
    // machine that ran the sync, and a relative path is all it takes to make it
    // work for the team.
    if (!bInsideSyncedTree)
    {
        const FString UEProjectDir = NormalizeDir(FPaths::ProjectDir());
        bInsideSyncedTree = IsUnder(Source, UEProjectDir);
    }

    if (!bInsideSyncedTree)
    {
        // Tier 3 — a scratch drive, Documents, a network share, a Megascans
        // install outside the workspace. No path expressible in a shipped
        // .blend reaches a file that is not on the reader's disk. Conforming is
        // the only fix, and the caller reports it as LocalOnly.
        return false;
    }

    FString Relative = Source;
    if (!FPaths::MakePathRelativeTo(Relative, *(Anchor + TEXT("/"))))
    {
        return false;
    }

    Relative.ReplaceInline(TEXT("\\"), TEXT("/"));
    if (Relative.IsEmpty())
    {
        return false;
    }

    OutRelative = Relative;
    return true;
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------

bool FMapLinkTextureNaming::RunSelfTest()
{
    bool bOk = true;
    auto ExpectStr = [&bOk](const FString& Actual, const TCHAR* Expected,
                            const TCHAR* What)
    {
        if (!Actual.Equals(Expected, ESearchCase::CaseSensitive))
        {
            UE_LOG(LogMapLink, Error,
                   TEXT("[Textures] Naming self-test FAILED: %s — expected "
                        "'%s', got '%s'"), What, Expected, *Actual);
            bOk = false;
        }
    };
    auto Expect = [&bOk](bool bCondition, const TCHAR* What)
    {
        if (!bCondition)
        {
            UE_LOG(LogMapLink, Error,
                   TEXT("[Textures] Naming self-test FAILED: %s"), What);
            bOk = false;
        }
    };

    // ── Stem derivation ───────────────────────────────────────────────────

    // The one from the first real run: leading digits go, and what is left has
    // to come back capitalised or the asset is named T_Mk_red.
    ExpectStr(DeriveStem(TEXT("32_red")), TEXT("Red"), TEXT("leading digits + case"));
    ExpectStr(MakeAssetName(DeriveStem(TEXT("32_red"))), TEXT("T_Mk_Red"),
              TEXT("composed asset name"));

    // A leading T_ is the marker we re-add as part of T_Mk_.
    ExpectStr(DeriveStem(TEXT("T_Grass_D")), TEXT("Grass_D"), TEXT("T_ prefix strip"));

    // Separator runs collapse; interior case is the artist's.
    ExpectStr(DeriveStem(TEXT("Grass04 2-5x2-5 4K")), TEXT("Grass04_2_5x2_5_4K"),
              TEXT("separator collapse"));

    // Blender's duplicate tail never survives as "_001".
    ExpectStr(DeriveStem(TEXT("Rock.001")), TEXT("Rock"), TEXT("blender suffix"));

    // Nothing usable is empty, NOT "T_Mk_".
    ExpectStr(DeriveStem(TEXT("1234")), TEXT(""), TEXT("all-digit name"));
    ExpectStr(MakeAssetName(TEXT("")), TEXT(""), TEXT("empty stem composes to empty"));

    // Already-capital first character is left alone, and only the first.
    ExpectStr(DeriveStem(TEXT("grass_4K_D")), TEXT("Grass_4K_D"),
              TEXT("first char only"));

    // ── Asset-name recognition ────────────────────────────────────────────
    Expect(IsMapLinkAssetName(TEXT("T_Mk_Red")),  TEXT("T_Mk_Red is a MapLink name"));
    Expect(!IsMapLinkAssetName(TEXT("t_mk_Red")), TEXT("case is exact"));
    Expect(!IsMapLinkAssetName(TEXT("T_Mk_")),    TEXT("bare prefix is not a name"));
    Expect(!IsMapLinkAssetName(TEXT("T_Red")),    TEXT("missing Mk"));

    // ── Path normalisation ────────────────────────────────────────────────
    const TCHAR* const Object = TEXT("/Game/MapLink/Textures/T_Mk_Red.T_Mk_Red");

    // The one that produced SkippedUnclassified for a classified texture.
    ExpectStr(NormalizeAssetPath(TEXT("/Game/MapLink/Textures/T_Mk_Red")),
              Object, TEXT("package path -> object path"));
    ExpectStr(NormalizeAssetPath(Object), Object, TEXT("object path is idempotent"));
    ExpectStr(NormalizeAssetPath(
                  TEXT("Texture2D'/Game/MapLink/Textures/T_Mk_Red.T_Mk_Red'")),
              Object, TEXT("Copy Reference form"));
    ExpectStr(NormalizeAssetPath(TEXT("  /Game/MapLink/Textures/T_Mk_Red/  ")),
              Object, TEXT("whitespace and trailing slash"));
    ExpectStr(NormalizeAssetPath(TEXT("")), TEXT(""), TEXT("empty in, empty out"));

    if (bOk)
    {
        UE_LOG(LogMapLink, Display,
               TEXT("[Textures] Naming self-test passed."));
    }
    return bOk;
}
