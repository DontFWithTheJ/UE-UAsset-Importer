#include "UAssetPackageFactory.h"

#include "ContentBrowserModule.h"
#include "Containers/Ticker.h"
#include "Framework/Application/SlateApplication.h"
#include "IContentBrowserSingleton.h"
#include "Logging/MessageLog.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "SUAssetImporterDialog.h"
#include "UAssetImporterModule.h"
#include "Widgets/SWindow.h"
#include "UObject/UObjectGlobals.h"

#define LOCTEXT_NAMESPACE "UUAssetPackageFactory"

namespace UAssetPackageFactoryPrivate
{
    static const FName MessageLogName(TEXT("UAssetImporter"));
    static const TCHAR* ConfigSection = TEXT("UAssetImporter.ImportOptions");

    static FString NormalizePath(FString Path)
    {
        Path = FPaths::ConvertRelativePathToFull(Path);
        FPaths::NormalizeFilename(Path);
        return Path;
    }

    static bool IsInsideDirectory(const FString& FilePath, const FString& Directory)
    {
        FString NormalizedFile = NormalizePath(FilePath);
        FString NormalizedDirectory = NormalizePath(Directory);

        NormalizedDirectory.ReplaceInline(TEXT("\\"), TEXT("/"));

        if (!NormalizedDirectory.EndsWith(TEXT("/")))
        {
            NormalizedDirectory += TEXT("/");
        }

        return NormalizedFile.StartsWith(
            NormalizedDirectory,
            ESearchCase::IgnoreCase);
    }

    static void QueuePostImportSanitization(
        TArray<FString> PackageNames,
        TArray<FString> ClearAllSources)
    {
        if (PackageNames.IsEmpty())
        {
            return;
        }

        FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateLambda(
                [
                    PackageNames = MoveTemp(PackageNames),
                    ClearAllSources = MoveTemp(ClearAllSources),
                    RemainingAttempts = 4
                ](float) mutable
                {
                    TArray<FString> Problems;

                    const bool bSucceeded =
                        FUAssetImporterService::SanitizeImportedPackages(
                            PackageNames,
                            ClearAllSources,
                            Problems);

                    --RemainingAttempts;

                    if (!bSucceeded && RemainingAttempts <= 0)
                    {
                        FMessageLog MessageLog(MessageLogName);

                        MessageLog.NewPage(
                            LOCTEXT(
                                "FinalSanitizationPage",
                                "UAsset Import - Final metadata cleanup"));

                        for (const FString& Problem : Problems)
                        {
                            UE_LOG(
                                LogUAssetImporter,
                                Error,
                                TEXT("%s"),
                                *Problem);

                            MessageLog.Error(FText::FromString(Problem));
                        }

                        MessageLog.Open(EMessageSeverity::Error, true);
                    }

                    return RemainingAttempts > 0;
                }),
            1.0f);
    }
}

UUAssetPackageFactory::UUAssetPackageFactory()
{
    bCreateNew = false;
    bEditorImport = true;
    bText = false;

    SupportedClass = UObject::StaticClass();
    ImportPriority = 100;

    Formats.Add(TEXT("uasset;Unreal Engine Asset Package"));

    Options.DestinationMode =
        EUAssetImporterDestinationMode::CurrentContentBrowserFolder;
}

bool UUAssetPackageFactory::FactoryCanImport(const FString& Filename)
{
    if (!FPaths::FileExists(Filename)
        || !FPaths::GetExtension(Filename).Equals(
            TEXT("uasset"),
            ESearchCase::IgnoreCase))
    {
        return false;
    }

    if (UAssetPackageFactoryPrivate::IsInsideDirectory(
            Filename,
            FPaths::ProjectContentDir())
        || UAssetPackageFactoryPrivate::IsInsideDirectory(
            Filename,
            FPaths::Combine(FPaths::ProjectDir(), TEXT("Plugins")))
        || UAssetPackageFactoryPrivate::IsInsideDirectory(
            Filename,
            FPaths::EngineContentDir()))
    {
        return false;
    }

    return true;
}

bool UUAssetPackageFactory::ConfigureProperties()
{
    LoadOptions();

    InitialContentBrowserPath = GetCurrentContentBrowserPath();

    if (Options.DestinationMode ==
        EUAssetImporterDestinationMode::CurrentContentBrowserFolder)
    {
        Options.DestinationPackagePath = InitialContentBrowserPath;
    }

    return true;
}

FString UUAssetPackageFactory::GetCurrentContentBrowserPath() const
{
    if (!FModuleManager::Get().IsModuleLoaded(TEXT("ContentBrowser")))
    {
        return TEXT("/Game");
    }

    FContentBrowserModule& ContentBrowserModule =
        FModuleManager::GetModuleChecked<FContentBrowserModule>(
            TEXT("ContentBrowser"));

    const FString CurrentPath =
        ContentBrowserModule.Get().GetCurrentPath().GetInternalPathString();

    if (FPackageName::IsValidLongPackageName(CurrentPath, false)
        && (CurrentPath.Equals(TEXT("/Game"), ESearchCase::IgnoreCase)
            || CurrentPath.StartsWith(TEXT("/Game/"), ESearchCase::IgnoreCase)))
    {
        return CurrentPath;
    }

    return TEXT("/Game");
}

void UUAssetPackageFactory::LoadOptions()
{
    if (!GConfig)
    {
        return;
    }

    GConfig->GetBool(
        UAssetPackageFactoryPrivate::ConfigSection,
        TEXT("AbortIfDependencyMissing"),
        Options.bAbortIfDependencyMissing,
        GEditorPerProjectIni);

    GConfig->GetBool(
        UAssetPackageFactoryPrivate::ConfigSection,
        TEXT("IgnoreSoftReferenceErrors"),
        Options.bIgnoreSoftReferenceErrors,
        GEditorPerProjectIni);

    GConfig->GetBool(
        UAssetPackageFactoryPrivate::ConfigSection,
        TEXT("OverwriteExistingAssets"),
        Options.bOverwriteExistingAssets,
        GEditorPerProjectIni);

    GConfig->GetBool(
        UAssetPackageFactoryPrivate::ConfigSection,
        TEXT("RollbackOnFailure"),
        Options.bRollbackOnFailure,
        GEditorPerProjectIni);

    GConfig->GetBool(
        UAssetPackageFactoryPrivate::ConfigSection,
        TEXT("SyncImportedAssets"),
        Options.bSyncImportedAssets,
        GEditorPerProjectIni);

    GConfig->GetBool(
        UAssetPackageFactoryPrivate::ConfigSection,
        TEXT("LoadImportedRootAssets"),
        Options.bLoadImportedRootAssets,
        GEditorPerProjectIni);

    int32 DependencyPlacementValue =
        static_cast<int32>(Options.DependencyPlacement);

    if (!GConfig->GetInt(
        UAssetPackageFactoryPrivate::ConfigSection,
        TEXT("DependencyPlacementMode"),
        DependencyPlacementValue,
        GEditorPerProjectIni))
    {
        bool bLegacyPlaceDependencies = false;
        bool bLegacyFlattenDependencies = false;

        GConfig->GetBool(
            UAssetPackageFactoryPrivate::ConfigSection,
            TEXT("PlaceDependenciesInDestination"),
            bLegacyPlaceDependencies,
            GEditorPerProjectIni);

        GConfig->GetBool(
            UAssetPackageFactoryPrivate::ConfigSection,
            TEXT("FlattenDependenciesInOneFolder"),
            bLegacyFlattenDependencies,
            GEditorPerProjectIni);

        if (bLegacyPlaceDependencies)
        {
            DependencyPlacementValue = static_cast<int32>(
                bLegacyFlattenDependencies
                    ? EUAssetImporterDependencyPlacement::FlatDependenciesFolder
                    : EUAssetImporterDependencyPlacement::DependenciesFolder);
        }
    }

    if (DependencyPlacementValue >= 0
        && DependencyPlacementValue <= static_cast<int32>(
            EUAssetImporterDependencyPlacement::SameFolderAsAssets))
    {
        Options.DependencyPlacement =
            static_cast<EUAssetImporterDependencyPlacement>(
                DependencyPlacementValue);
    }

    int32 ModeValue = static_cast<int32>(Options.DestinationMode);

    GConfig->GetInt(
        UAssetPackageFactoryPrivate::ConfigSection,
        TEXT("DestinationMode"),
        ModeValue,
        GEditorPerProjectIni);

    if (ModeValue >= 0
        && ModeValue <= static_cast<int32>(
            EUAssetImporterDestinationMode::CustomFolder))
    {
        Options.DestinationMode =
            static_cast<EUAssetImporterDestinationMode>(ModeValue);
    }

    GConfig->GetString(
        UAssetPackageFactoryPrivate::ConfigSection,
        TEXT("DestinationPackagePath"),
        Options.DestinationPackagePath,
        GEditorPerProjectIni);
}

UObject* UUAssetPackageFactory::FactoryCreateFile(
    UClass* InClass,
    UObject* InParent,
    FName InName,
    EObjectFlags Flags,
    const FString& Filename,
    const TCHAR* Parms,
    FFeedbackContext* Warn,
    bool& bOutOperationCanceled)
{
    bOutOperationCanceled = false;

    if (!FactoryCanImport(Filename))
    {
        BatchErrors.Add(FString::Printf(
            TEXT("The selected file cannot be imported: %s"),
            *Filename));

        ++BatchProblemCount;
        bOutOperationCanceled = true;
        return nullptr;
    }

    const FString NormalizedFilename =
        UAssetPackageFactoryPrivate::NormalizePath(Filename);

    if (!BatchSourceFiles.ContainsByPredicate(
        [&NormalizedFilename](const FString& ExistingPath)
        {
            return ExistingPath.Equals(
                NormalizedFilename,
                ESearchCase::IgnoreCase);
        }))
    {
        BatchSourceFiles.Add(NormalizedFilename);
    }

    bHasBatchActivity = true;

    UObject* BatchPlaceholder = NewObject<UObject>(
        GetTransientPackage(),
        NAME_None,
        RF_Transient);

    if (!BatchPlaceholder)
    {
        BatchErrors.Add(FString::Printf(
            TEXT("Could not create a temporary import placeholder for '%s'."),
            *Filename));

        ++BatchProblemCount;
        bOutOperationCanceled = true;
        return nullptr;
    }

    BatchPlaceholderObjects.Add(BatchPlaceholder);

    UE_LOG(
        LogUAssetImporter,
        Display,
        TEXT("[Content Browser Import] Queued source file %d: '%s'."),
        BatchSourceFiles.Num(),
        *NormalizedFilename);

    return BatchPlaceholder;
}

void UUAssetPackageFactory::ReportBatchProblems()
{
    if (BatchErrors.IsEmpty() && BatchWarnings.IsEmpty())
    {
        return;
    }

    FMessageLog MessageLog(
        UAssetPackageFactoryPrivate::MessageLogName);

    MessageLog.NewPage(
        LOCTEXT("StandardImportProblems", "UAsset Import - Standard Import"));

    for (const FString& Warning : BatchWarnings)
    {
        MessageLog.Warning(FText::FromString(Warning));
    }

    for (const FString& Error : BatchErrors)
    {
        MessageLog.Error(FText::FromString(Error));
    }

    MessageLog.Open(
        BatchErrors.IsEmpty()
            ? EMessageSeverity::Warning
            : EMessageSeverity::Error,
        true);
}

void UUAssetPackageFactory::CleanUp()
{
    if (bHasBatchActivity && !BatchSourceFiles.IsEmpty())
    {
        UE_LOG(
            LogUAssetImporter,
            Display,
            TEXT("[Content Browser Import] Collected %d .uasset file(s). Starting one shared import session."),
            BatchSourceFiles.Num());

        TSharedRef<FUAssetImporterService> Service =
            MakeShared<FUAssetImporterService>();

        TArray<TSharedPtr<FUAssetImporterAssetItem>> Items;
        TArray<FString> PreparationProblems;

        if (!Service->Prepare(
                BatchSourceFiles,
                Items,
                PreparationProblems)
            || Items.IsEmpty())
        {
            if (PreparationProblems.IsEmpty())
            {
                PreparationProblems.Add(
                    TEXT("Could not prepare the selected .uasset files for batch import."));
            }

            BatchErrors.Append(PreparationProblems);
            BatchProblemCount += PreparationProblems.Num();

            Service->Cleanup();
        }
        else
        {
            InitialContentBrowserPath =
                GetCurrentContentBrowserPath();

            TSharedPtr<SUAssetImporterDialog> ImportDialog;

            TSharedRef<SWindow> Window = SNew(SWindow)
                .Title(LOCTEXT("ImportWindowTitle", "UAsset Importer"))
                .ClientSize(FVector2D(820.0f, 680.0f))
                .SupportsMinimize(false)
                .SupportsMaximize(true)
                .SizingRule(ESizingRule::UserSized);

            Window->SetContent(
                SAssignNew(ImportDialog, SUAssetImporterDialog)
                .Service(Service)
                .Items(Items)
                .InitialDestinationPackagePath(InitialContentBrowserPath)
                .OnFinished(FSimpleDelegate::CreateLambda(
                    [WindowWeak = TWeakPtr<SWindow>(Window)]()
                    {
                        if (TSharedPtr<SWindow> PinnedWindow = WindowWeak.Pin())
                        {
                            PinnedWindow->RequestDestroyWindow();
                        }
                    })));

            Window->SetOnWindowClosed(
                FOnWindowClosed::CreateLambda(
                    [Service](const TSharedRef<SWindow>&)
                    {
                        Service->Cleanup();
                    }));

            FSlateApplication::Get().AddModalWindow(
                Window,
                FSlateApplication::Get().GetActiveTopLevelWindow(),
                false);

            if (ImportDialog.IsValid()
                && ImportDialog->DidCompleteImport())
            {
                const FUAssetImporterResult ImportResult =
                    ImportDialog->GetImportResult();

                BatchPackageNames.Append(
                    ImportResult.CopiedPackageNames);

                BatchImportedCount = ImportResult.ImportedCount;
                BatchSkippedCount = ImportResult.SkippedCount;
                BatchProblemCount = ImportResult.ProblemCount;

                UE_LOG(
                    LogUAssetImporter,
                    Display,
                    TEXT("[Content Browser Import] Batch finished: %d imported, %d skipped, %d problems."),
                    BatchImportedCount,
                    BatchSkippedCount,
                    BatchProblemCount);
            }
            else
            {
                BatchWarnings.Add(
                    TEXT("The batch .uasset import was canceled."));
            }

            Service->Cleanup();
        }
    }

    if (!BatchPackageNames.IsEmpty())
    {
        UAssetPackageFactoryPrivate::QueuePostImportSanitization(
            BatchPackageNames,
            BatchPackageNames);
    }

    ReportBatchProblems();

    BatchSourceFiles.Reset();
    BatchPlaceholderObjects.Reset();
    BatchPackageNames.Reset();
    BatchErrors.Reset();
    BatchWarnings.Reset();

    BatchImportedCount = 0;
    BatchSkippedCount = 0;
    BatchProblemCount = 0;

    bHasBatchActivity = false;

    Super::CleanUp();
}

#undef LOCTEXT_NAMESPACE