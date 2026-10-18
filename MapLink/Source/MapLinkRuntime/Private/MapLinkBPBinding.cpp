// Copyright MapModelers. All Rights Reserved.

#include "MapLinkBPBinding.h"

#include "Engine/Blueprint.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

FName UMapLinkBPBinding::GetBindingObjectName()
{
    // One binding object per package, with a stable name so a re-create
    // after Remove() doesn't collide with a still-renaming-out instance.
    static const FName Name(TEXT("MapLinkBPBinding"));
    return Name;
}

// ---------------------------------------------------------------------------
// Get
// ---------------------------------------------------------------------------

UMapLinkBPBinding* UMapLinkBPBinding::Get(UBlueprint* BP)
{
    if (!BP)
    {
        return nullptr;
    }

    UPackage* Pkg = BP->GetOutermost();
    if (!Pkg)
    {
        return nullptr;
    }

    // Iterate top-level objects in the BP's package.  At most one
    // UMapLinkBPBinding lives there.
    UMapLinkBPBinding* Found = nullptr;
    ForEachObjectWithPackage(
        Pkg,
        [&Found](UObject* Obj) -> bool
        {
            if (UMapLinkBPBinding* Binding = Cast<UMapLinkBPBinding>(Obj))
            {
                Found = Binding;
                return false; // stop iteration
            }
            return true; // keep scanning
        },
        /*bIncludeNestedObjects=*/false);

    return Found;
}

// ---------------------------------------------------------------------------
// GetOrCreate
// ---------------------------------------------------------------------------

UMapLinkBPBinding* UMapLinkBPBinding::GetOrCreate(UBlueprint* BP)
{
    if (!BP)
    {
        return nullptr;
    }

    if (UMapLinkBPBinding* Existing = Get(BP))
    {
        return Existing;
    }

    UPackage* Pkg = BP->GetOutermost();
    if (!Pkg)
    {
        return nullptr;
    }

    // RF_Public  — the binding can be referenced (e.g. by the
    //              MapLinkProxyAsset side via FSoftObjectPath).
    // RF_Standalone — package save preserves it even with no hard refs.
    // Outer = Pkg — top-level in the BP's package; IsAsset() override
    //               keeps it out of the Content Browser regardless.
    UMapLinkBPBinding* NewBinding = NewObject<UMapLinkBPBinding>(
        Pkg,
        UMapLinkBPBinding::StaticClass(),
        GetBindingObjectName(),
        RF_Public | RF_Standalone);

    BP->MarkPackageDirty();
    return NewBinding;
}

// ---------------------------------------------------------------------------
// Remove
// ---------------------------------------------------------------------------

void UMapLinkBPBinding::Remove(UBlueprint* BP)
{
    if (!BP)
    {
        return;
    }

    UMapLinkBPBinding* Existing = Get(BP);
    if (!Existing)
    {
        return;
    }

    // Strip the "save with package" / "publicly referenceable" flags
    // and move the object into the transient package so it stops
    // surviving package saves.  REN_DontCreateRedirectors prevents a
    // stale link from being left in the BP's package on next save.
    Existing->ClearFlags(RF_Public | RF_Standalone);
    Existing->Rename(
        nullptr,
        GetTransientPackage(),
        REN_DontCreateRedirectors | REN_DoNotDirty);
    Existing->MarkAsGarbage();

    BP->MarkPackageDirty();
}
