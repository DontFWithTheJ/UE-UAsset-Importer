#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "UAssetImporterService.h"
#include "UAssetPackageFactory.generated.h"

UCLASS()
class UASSETIMPORTER_API UUAssetPackageFactory : public UFactory
{
	GENERATED_BODY()

public:
	UUAssetPackageFactory();

	virtual bool FactoryCanImport(const FString& Filename) override;
	virtual bool ConfigureProperties() override;

	virtual UObject* FactoryCreateFile(
		UClass* InClass,
		UObject* InParent,
		FName InName,
		EObjectFlags Flags,
		const FString& Filename,
		const TCHAR* Parms,
		FFeedbackContext* Warn,
		bool& bOutOperationCanceled) override;

	virtual void CleanUp() override;

private:
	void LoadOptions();
	FString GetCurrentContentBrowserPath() const;
	void ReportBatchProblems();

	FUAssetImporterOptions Options;
	FString InitialContentBrowserPath;

	TArray<FString> BatchSourceFiles;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UObject>> BatchPlaceholderObjects;

	TArray<FString> BatchPackageNames;
	TArray<FString> BatchErrors;
	TArray<FString> BatchWarnings;

	int32 BatchImportedCount = 0;
	int32 BatchSkippedCount = 0;
	int32 BatchProblemCount = 0;

	bool bHasBatchActivity = false;
};