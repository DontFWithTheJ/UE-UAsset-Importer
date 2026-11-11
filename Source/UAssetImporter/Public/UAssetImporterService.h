#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "UObject/StrongObjectPtr.h"
#include "Engine/Texture2D.h"

enum class EUAssetImporterDestinationMode : uint8
{
    CurrentContentBrowserFolder,
    ProjectContentRoot,
    PreserveOriginalPaths,
    CustomFolder
};

enum class EUAssetImporterDependencyPlacement : uint8
{
    KeepOriginalPaths,
    DependenciesFolder,
    FlatDependenciesFolder,
    SameFolderAsAssets
};

struct FUAssetImporterOptions
{
    bool bAbortIfDependencyMissing = true;
    bool bIgnoreSoftReferenceErrors = true;
    bool bOverwriteExistingAssets = false;
    bool bRollbackOnFailure = true;
    bool bSyncImportedAssets = true;
    bool bLoadImportedRootAssets = false;
    bool bClearImportSourceData = true;
    bool bLogSummary = true;
    bool bShowProblemLog = true;
    bool bDeferFinalization = false;
    EUAssetImporterDependencyPlacement DependencyPlacement = EUAssetImporterDependencyPlacement::KeepOriginalPaths;
    TArray<FString> SelectedDependencyPackageNames;
    TArray<FString> ExcludedDependencyPackageNames;
    TArray<FString> BatchAlreadyProcessedPackageNames;
    EUAssetImporterDestinationMode DestinationMode = EUAssetImporterDestinationMode::CurrentContentBrowserFolder;
    FString DestinationPackagePath = TEXT("/Game");
};

struct FUAssetImporterAssetItem
{
    FString SourceFile;
    FString ContentRoot;
    FString OriginalPackageName;
    FString TemporaryPackageName;
    FString AssetName;
    FString AssetClass;
    FString DisplayPath;
    FString SavedEngineVersion;
    int64 FileSize = 0;
    FAssetData AssetData;
    TStrongObjectPtr<UTexture2D> PreviewTexture;
    bool bSelected = true;
    bool bMetadataValid = false;
};


struct FUAssetImporterDependencyItem
{
    FString OriginalPackageName;
    FString TemporaryPackageName;
    FString AssetName;
    FString AssetClass;
    FString SourceFile;
    FAssetData AssetData;
    bool bHasSourceFile = false;
    bool bHasMetadata = false;
    bool bAlreadyInProject = false;
    bool bMissing = false;
    bool bSoftOnly = false;
};

struct FUAssetImporterResult
{
    int32 ImportedCount = 0;
    int32 SkippedCount = 0;
    int32 ProblemCount = 0;
    bool bSucceeded = false;
    TArray<FString> Warnings;
    TArray<FString> Errors;
    TArray<FString> AffectedPackageNames;
    TArray<FString> CopiedPackageNames;
    TArray<FString> ImportedRootPackageNames;
};

class FUAssetImporterService : public TSharedFromThis<FUAssetImporterService>
{
public:
    bool Prepare(
        const TArray<FString>& SelectedFiles,
        TArray<TSharedPtr<FUAssetImporterAssetItem>>& OutItems,
        TArray<FString>& OutProblems);

    FUAssetImporterResult Import(
        const TArray<TSharedPtr<FUAssetImporterAssetItem>>& Items,
        const FUAssetImporterOptions& Options);

    void GetDependencyItems(
        const TArray<TSharedPtr<FUAssetImporterAssetItem>>& Items,
        TArray<FUAssetImporterDependencyItem>& OutDependencies) const;

    static bool SanitizeImportedPackages(
        const TArray<FString>& PackageNames,
        const TArray<FString>& PackageNamesToClearAllSources,
        TArray<FString>& OutProblems);

    void Cleanup();

private:
    struct FSourceRoot
    {
        FString ContentRoot;
        FString OriginalVirtualRoot;
        FString TemporaryVirtualRoot;
        bool bIsPluginRoot = false;
        bool bMountRegistered = false;
        TArray<FString> Files;
    };

    struct FPlannedAsset
    {
        FString SourceFile;
        FString TemporaryPackageName;
        FString OriginalPackageName;
        FString DestinationPackageName;
        bool bIsSelectedRoot = false;
    };

    struct FCopyOperation
    {
        FPlannedAsset Asset;
        FString DestinationBase;
        FString DestinationRoot;
        bool bDestinationExists = false;
    };

    FString FindContentRoot(const FString& FilePath) const;
    FString DetermineOriginalVirtualRoot(const FString& ContentRoot, bool& bOutIsPluginRoot) const;
    FString MakePackageName(const FString& VirtualRoot, const FString& ContentRoot, const FString& FilePath) const;
    FString MakeTemporaryPackageName(const FSourceRoot& Root, const FString& FilePath) const;
    FString NormalizeFilePath(const FString& Path) const;
    FString FindSourceFileForPackage(const FString& PackageName) const;
    FString FindTemporaryPackageForPackage(const FString& PackageName) const;
    bool IsTargetPackageAvailable(const FString& PackageName) const;
    bool GetDestinationBasePath(const FString& PackageName, FString& OutBasePath, FString& OutDestinationRoot, FString& OutProblem) const;
    TArray<FString> GetPackageFiles(const FString& BasePath) const;
    bool CopyFileVerified(const FString& Source, const FString& Destination, FString& OutProblem) const;
    void ReportIssues(const TArray<FString>& Warnings, const TArray<FString>& Errors, const FUAssetImporterResult& Result, bool bShowMessageLog, bool bLogSummary) const;

    TArray<FSourceRoot> SourceRoots;
    TMap<FString, FString> OriginalPackageToFile;
    TMap<FString, FString> OriginalPackageToTemporaryPackage;
    TMap<FString, FString> TemporaryPackageToFile;
    TMap<FString, FString> TemporaryPackageToOriginalPackage;
    TMap<FString, FAssetData> AssetDataByTemporaryPackage;
    TMap<FString, TArray<FString>> HardDependenciesByTemporaryPackage;
    TMap<FString, TArray<FString>> SoftDependenciesByTemporaryPackage;
};
