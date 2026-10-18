// Copyright MapModelers. All Rights Reserved.

#include "Modules/ModuleManager.h"

class FMapLinkRuntimeModule : public IModuleInterface
{
public:
    virtual void StartupModule() override {}
    virtual void ShutdownModule() override {}
};

IMPLEMENT_MODULE(FMapLinkRuntimeModule, MapLinkRuntime)
