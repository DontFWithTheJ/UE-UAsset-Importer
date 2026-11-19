#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "ContentBrowserDelegates.h"

DECLARE_LOG_CATEGORY_EXTERN(LogUAssetImporter, Log, All);

class FUAssetImporterModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;

private:
    bool HandleExternalFileDragOver(
        const FAssetViewDragAndDropExtender::FPayload& Payload);

    bool HandleExternalFileDrop(
        const FAssetViewDragAndDropExtender::FPayload& Payload);

    void OpenImportDialogForFiles(const TArray<FString>& SelectedFiles, const FString& InitialDestinationPackagePath);
};