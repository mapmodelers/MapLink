// Copyright MapModelers. All Rights Reserved.

#include "MapLinkImportFactory.h"
#include "MapLinkProxyAsset.h"
#include "MapLinkPipeline.h"

UMapLinkImportFactory::UMapLinkImportFactory()
{
    SupportedClass = UMapLinkProxyAsset::StaticClass();
    Formats.Add(TEXT("blend;Blender Scene"));
    bCreateNew = false;
    bEditorImport = true;
    bText = false;
    ImportPriority = DefaultImportPriority;
}

bool UMapLinkImportFactory::FactoryCanImport(const FString& Filename)
{
    return Filename.EndsWith(TEXT(".blend"));
}

UObject* UMapLinkImportFactory::FactoryCreateFile(
    UClass* InClass,
    UObject* InParent,
    FName InName,
    EObjectFlags Flags,
    const FString& Filename,
    const TCHAR* Parms,
    FFeedbackContext* Warn,
    bool& bOutOperationCanceled)
{
    const EMapLinkResult Result = FMapLinkPipeline::RunImport(Filename);

    if (Result != EMapLinkResult::Success)
    {
        bOutOperationCanceled = true;
        if (Result == EMapLinkResult::Cancelled)
        {
            UE_LOG(LogMapLink, Warning,
                   TEXT("MapLink import cancelled by user for '%s'."), *Filename);
        }
        else
        {
            UE_LOG(LogMapLink, Error,
                   TEXT("MapLink import failed for '%s' with code %d."),
                   *Filename, static_cast<int32>(Result));
        }
        return nullptr;
    }

    // Return the proxy asset so the Content Browser shows it
    const FString BlendName = FPaths::GetBaseFilename(Filename);
    const FString ProxyPath = FString::Printf(
        TEXT("/Game/MapLink/%s/%s_Proxy"), *BlendName, *BlendName);

    return LoadObject<UMapLinkProxyAsset>(nullptr, *ProxyPath);
}
