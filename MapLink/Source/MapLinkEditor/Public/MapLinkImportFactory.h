// Copyright MapModelers. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "MapLinkImportFactory.generated.h"

/**
 * UFactory subclass that handles .blend file drag-drop into the Content Browser.
 * Delegates entirely to FMapLinkPipeline::RunImport.
 */
UCLASS()
class UMapLinkImportFactory : public UFactory
{
    GENERATED_BODY()

public:
    UMapLinkImportFactory();

    // UFactory interface
    virtual UObject* FactoryCreateFile(
        UClass* InClass,
        UObject* InParent,
        FName InName,
        EObjectFlags Flags,
        const FString& Filename,
        const TCHAR* Parms,
        FFeedbackContext* Warn,
        bool& bOutOperationCanceled) override;

    virtual bool FactoryCanImport(const FString& Filename) override;
};
