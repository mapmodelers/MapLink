// Copyright MapModelers. All Rights Reserved.
//
// v1.16 — Texture naming and path arithmetic.
//
// Two jobs that both come down to "what should this be called and where does it
// live", kept together because the bijection in §4.5 is what ties them:
//
//     asset name  T_Mk_Foo
//     source file <Root>/Textures/T_Mk_Foo.tga
//     preview     <Root>/TexturePreviews/<Class>/T_Mk_[Ext_]Foo.png
//
// Each is derivable from the others. Any violation is a conformance failure,
// which is only checkable because the derivation lives in exactly one place.

#pragma once

#include "CoreMinimal.h"

class MAPLINKEDITOR_API FMapLinkTextureNaming
{
public:
    /** "T_Mk_" — MapLink-class asset and source-file prefix. */
    static const TCHAR* GetPrefix();

    /** "T_Mk_Ext_" — External preview PNG prefix.
     *
     *  The class is encoded in the filename even though the folder already
     *  says it, because Blender shows image NAMES far more often than folders —
     *  image editor header, outliner, N-panel, the file-relocation dialog. A
     *  name that reads "derived preview of a third-party texture, do not edit"
     *  at a glance earns its redundancy. */
    static const TCHAR* GetExternalPreviewPrefix();

    /** Longest permitted stem, truncated from the end while preserving the
     *  type suffix. */
    static constexpr int32 MaxStemLength = 48;

    /** Longest permitted full asset name. */
    static constexpr int32 MaxAssetNameLength = 64;

    /** P4 workspace roots are deep and MAX_PATH still bites. A composed source
     *  path longer than this is rejected and reported rather than written. */
    static constexpr int32 MaxAbsolutePathLength = 240;

    /**
     * Source asset name -> stem, per §4.2:
     *   strip a leading "T_", preserve trailing type suffixes (_D _N _H _ORM
     *   _R _M _AO), sanitise to [A-Za-z0-9_] collapsing separator runs to one
     *   underscore, strip leading digits, strip leading/trailing underscores,
     *   and drop any Blender ".001" tail through FMapLinkUtils::StripBlenderSuffix.
     *
     * Returns an empty string when nothing survives (a name that was entirely
     * punctuation and digits), which the caller must treat as a failure rather
     * than composing "T_Mk_".
     */
    static FString DeriveStem(const FString& SourceAssetName);

    /** "T_Mk_<Stem>". Empty when the stem is empty. */
    static FString MakeAssetName(const FString& Stem);

    /**
     * Accept whatever the user actually has in hand and return the object path
     * the policy is keyed by.
     *
     *   /Game/MapLink/Textures/T_Mk_Red              (package path, typed)
     *   Texture2D'/Game/.../T_Mk_Red.T_Mk_Red'       (Copy Reference)
     *   /Game/MapLink/Textures/T_Mk_Red.T_Mk_Red     (object path, already fine)
     *
     * all normalise to the last form. Without this a typed path misses every
     * policy lookup and the operation reports SkippedUnclassified for a texture
     * that is in fact classified — the failure is indistinguishable from a real
     * one, which is what makes it worth a function.
     *
     * Empty in, empty out. No validation: this is spelling, not existence.
     */
    static FString NormalizeAssetPath(const FString& InPath);

    /** "T_Mk_Ext_<Stem>". Empty when the stem is empty. */
    static FString MakeExternalPreviewName(const FString& Stem);

    /** True when the name is exactly T_Mk_[A-Za-z0-9_]+ — the §4 pattern, case
     *  exact. "t_mk_foo" and "T_MK_Foo" both fail. */
    static bool IsMapLinkAssetName(const FString& AssetName);

    /**
     * Resolve a collision by appending _01, _02, … up to _99.
     *
     * Taken is consulted AND updated, so a batch pass cannot hand the same
     * resolved name to two textures. Returns false when 99 variants are all
     * taken, which is a report-and-stop, never a silent merge: two different
     * textures collapsing into one name is data loss that looks like success.
     */
    static bool ResolveCollision(const FString& DesiredAssetName,
                                 TSet<FString>& Taken,
                                 FString&       OutResolved);

    // ── Path composition ──────────────────────────────────────────────────

    /** <Root>/Textures/<AssetName>.tga. Empty when the project root is
     *  unresolved. */
    static FString ComposeSourcePath(const FString& AssetName);

    /** /Game/MapLink/Textures/<AssetName>. */
    static FString ComposeAssetPath(const FString& AssetName);

    /** /Game/MapLink/Textures — the one legal package path for MapLink class. */
    static FString GetTexturesPackagePath();

    /** True when PackagePath is exactly the textures package path — not a
     *  subfolder of it. MapLink's texture namespace is flat by design; a
     *  texture in /Game/MapLink/Textures/Rocks/ is not conform. */
    static bool IsTexturesPackagePath(const FString& PackagePath);

    /** False with a reason when composing this path would exceed the length
     *  limits in §4.3. */
    static bool ValidateLengths(const FString& AssetName,
                                const FString& AbsoluteSourcePath,
                                FString&       OutReason);

    // ── Portability (the whole reason a teammate's Blender works) ──────────

    /**
     * The directory the material library .blend lives in. Every relative
     * texture path is expressed against THIS, because that is what Blender's
     * "//" prefix means.
     *
     * Anchored on UMapLinkSettings::MaterialInstancesBlend when configured,
     * falling back to <Root>/MaterialInstances. Empty when neither resolves.
     */
    static FString GetLibraryAnchorDir();

    /**
     * Can a teammate open this file?
     *
     * Returns true and fills OutRelative (forward slashes, no "//" prefix,
     * relative to the library anchor) when the source sits inside the synced
     * tree — under the MapLink project root, or under the UE project and on the
     * same volume as the anchor. Those are the two cases where the bytes are on
     * every machine and only the absolute prefix differs.
     *
     * Returns false when no relative form can express it: a different volume, a
     * scratch drive, a network share, anything outside both trees. That is
     * exactly EMapLinkTextureStatus::LocalOnly, and conforming — copying the
     * pixels in — is the only thing that fixes it.
     */
    static bool ComputePortablePath(const FString& AbsoluteSourcePath,
                                    FString&       OutRelative);

    /**
     * Assert the derivations above against a fixed table of inputs, logging
     * every failure. Wired into MapLink.Textures.SelfTest.
     *
     * Naming is pure string arithmetic with no project state, so it is the one
     * part of the texture manager that can be proven correct without a texture,
     * an editor world, or Perforce. Every case in the table is one that has
     * been wrong at least once.
     */
    static bool RunSelfTest();

    /** ComputePortablePath against an explicit anchor, so the exporter can pass
     *  the FINAL library path rather than the temp copy the headless sync is
     *  actually running against. AtomicRegenLibrary regenerates into
     *  <LibDir>/.maplink_sync_tmp/, one directory deeper, then replaces upward —
     *  relativising against the open file would produce paths that validate in
     *  the temp and break the instant it lands. */
    static bool ComputePortablePathFrom(const FString& AbsoluteSourcePath,
                                        const FString& AnchorDir,
                                        FString&       OutRelative);
};
