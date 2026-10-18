// Copyright MapModelers. All Rights Reserved.
//
// v1.13 — Editor feedback policy helpers (see header).

#include "MapLinkNotify.h"

#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"

namespace
{
    void ShowToast(const FText& Message,
                   SNotificationItem::ECompletionState State,
                   float ExpireSec)
    {
        FNotificationInfo Info(Message);
        Info.ExpireDuration  = ExpireSec;
        Info.bFireAndForget  = true;
        Info.bUseThrobber    = false;
        Info.bUseSuccessFailIcons = true;

        TSharedPtr<SNotificationItem> Item =
            FSlateNotificationManager::Get().AddNotification(Info);
        if (Item.IsValid())
        {
            Item->SetCompletionState(State);
        }
    }
}

namespace MapLinkNotify
{
    void Success(const FText& Message)
    {
        ShowToast(Message, SNotificationItem::CS_Success, 4.0f);
    }

    void Warn(const FText& Message)
    {
        // CS_None keeps the neutral icon; warnings shouldn't render the red
        // fail cross, and there is no dedicated warning state on the item.
        ShowToast(Message, SNotificationItem::CS_None, 6.0f);
    }

    void Fail(const FText& Message)
    {
        ShowToast(Message, SNotificationItem::CS_Fail, 8.0f);
    }
}

FText MapLinkResultToText(EMapLinkResult Result)
{
    switch (Result)
    {
    case EMapLinkResult::Success:
        return NSLOCTEXT("MapLink", "ResultSuccess", "Success");
    case EMapLinkResult::BlenderNotFound:
        return NSLOCTEXT("MapLink", "ResultBlenderNotFound",
            "Blender executable not found (set MAPLINK_BLENDER_EXE or install Blender)");
    case EMapLinkResult::BlenderScriptFailed:
        return NSLOCTEXT("MapLink", "ResultBlenderScriptFailed",
            "The headless Blender script failed (see Output Log / LogMapLink)");
    case EMapLinkResult::FbxNotFound:
        return NSLOCTEXT("MapLink", "ResultFbxNotFound",
            "Export artifacts missing (FBX/analysis not produced)");
    case EMapLinkResult::TempLevelMissing:
        return NSLOCTEXT("MapLink", "ResultTempLevelMissing",
            "Temporary import level could not be created or found");
    case EMapLinkResult::InterchangeFailed:
        return NSLOCTEXT("MapLink", "ResultInterchangeFailed",
            "Mesh import (Interchange) failed");
    case EMapLinkResult::AssetNotFound:
        return NSLOCTEXT("MapLink", "ResultAssetNotFound",
            "A required asset could not be found");
    case EMapLinkResult::LevelLocked:
        return NSLOCTEXT("MapLink", "ResultLevelLocked",
            "The target level is locked or read-only");
    case EMapLinkResult::DependencyCycle:
        return NSLOCTEXT("MapLink", "ResultDependencyCycle",
            "Dependency cycle detected between .blend files");
    case EMapLinkResult::BlueprintCompileError:
        return NSLOCTEXT("MapLink", "ResultBlueprintCompileError",
            "Generated Blueprint failed to compile");
    case EMapLinkResult::MaterialLibraryNotFound:
        return NSLOCTEXT("MapLink", "ResultMaterialLibraryNotFound",
            "Material library folder not found");
    case EMapLinkResult::MaterialInstancesBlendNotFound:
        return NSLOCTEXT("MapLink", "ResultMaterialInstancesBlendNotFound",
            "MaterialInstances .blend not found");
    case EMapLinkResult::MaterialSyncFailed:
        return NSLOCTEXT("MapLink", "ResultMaterialSyncFailed",
            "Material library sync failed (dirty flag set — use Resync in Settings)");
    case EMapLinkResult::NoMaterialInstancesFound:
        return NSLOCTEXT("MapLink", "ResultNoMaterialInstancesFound",
            "No Material Instances found under /Game/MapLink/MaterialInstances");
    case EMapLinkResult::MaterialDisplayLevelCreateFailed:
        return NSLOCTEXT("MapLink", "ResultMaterialDisplayLevelCreateFailed",
            "Material display level could not be created");
    case EMapLinkResult::MaterialDisplayMeshNotFound:
        return NSLOCTEXT("MapLink", "ResultMaterialDisplayMeshNotFound",
            "Material display mesh not found");
    case EMapLinkResult::Cancelled:
        return NSLOCTEXT("MapLink", "ResultCancelled",
            "Cancelled by user");
    case EMapLinkResult::BlenderTimedOut:
        return NSLOCTEXT("MapLink", "ResultBlenderTimedOut",
            "Headless Blender exceeded the watchdog timeout and was terminated");
    case EMapLinkResult::TextureConformFailed:
        return NSLOCTEXT("MapLink", "ResultTextureConformFailed",
            "Texture conform failed (see Output Log / LogMapLink)");
    case EMapLinkResult::TextureNoSourceData:
        return NSLOCTEXT("MapLink", "ResultTextureNoSourceData",
            "Texture has no editor source data — it cannot be rewritten "
            "(re-import it from its original file first)");
    case EMapLinkResult::TexturePathTooLong:
        return NSLOCTEXT("MapLink", "ResultTexturePathTooLong",
            "The resulting texture path would exceed the 240-character limit");
    case EMapLinkResult::TextureReferencerLocked:
        return NSLOCTEXT("MapLink", "ResultTextureReferencerLocked",
            "A referencing asset could not be checked out or modified, so the "
            "operation was aborted rather than leave a dangling reference");
    case EMapLinkResult::InvalidAssetName:
        return NSLOCTEXT("MapLink", "ResultInvalidAssetName",
            "The .blend name cannot be used as an Unreal asset name - rename "
            "the file using letters, digits and underscores only");
    }
    return NSLOCTEXT("MapLink", "ResultUnknown", "Unknown result");
}
