#include "UAssetImporterModule.h"

#include "ContentBrowserModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Input/DragAndDrop.h"
#include "Logging/MessageLog.h"
#include "MessageLogModule.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "SUAssetImporterDialog.h"
#include "UAssetImporterService.h"
#include "Widgets/SWindow.h"
#include "IContentBrowserSingleton.h"
#include "ContentBrowserItemPath.h"

DEFINE_LOG_CATEGORY(LogUAssetImporter);

#define LOCTEXT_NAMESPACE "FUAssetImporterModule"

namespace UAssetImporterModulePrivate
{
    static const FName MessageLogName(TEXT("UAssetImporter"));

    static bool IsUAssetFile(const FString& FilePath)
    {
        return FPaths::GetExtension(FilePath).Equals(
            TEXT("uasset"),
            ESearchCase::IgnoreCase
        );
    }

    static FString NormalizeFilePath(const FString& Path)
    {
        FString Result = FPaths::ConvertRelativePathToFull(Path);
        FPaths::NormalizeFilename(Result);
        return Result;
    }

    static FString GetCurrentDestinationPath()
    {
        if (!FModuleManager::Get().IsModuleLoaded(TEXT("ContentBrowser")))
        {
            return TEXT("/Game");
        }

        FContentBrowserModule& ContentBrowserModule =
            FModuleManager::GetModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));

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

    static void ReportProblems(const TArray<FString>& Problems)
    {
        if (Problems.IsEmpty())
        {
            return;
        }

        FMessageLog MessageLog(MessageLogName);
        MessageLog.NewPage(LOCTEXT("PreparationPage", "Import preparation"));

        for (const FString& Problem : Problems)
        {
            UE_LOG(LogUAssetImporter, Error, TEXT("%s"), *Problem);
            MessageLog.Error(FText::FromString(Problem));
        }

        MessageLog.Open(EMessageSeverity::Error, true);
    }
}

void FUAssetImporterModule::StartupModule()
{
    FMessageLogInitializationOptions MessageLogOptions;

    FModuleManager::LoadModuleChecked<FMessageLogModule>(TEXT("MessageLog"))
        .RegisterLogListing(
            UAssetImporterModulePrivate::MessageLogName,
            LOCTEXT("UAssetImporterLogLabel", "UAsset Importer"),
            MessageLogOptions
        );

    FContentBrowserModule& ContentBrowserModule =
        FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));

    ContentBrowserModule.GetAssetViewDragAndDropExtenders().Insert(
        FAssetViewDragAndDropExtender(
            FAssetViewDragAndDropExtender::FOnDropDelegate::CreateRaw(
                this,
                &FUAssetImporterModule::HandleExternalFileDrop
            ),
            FAssetViewDragAndDropExtender::FOnDragOverDelegate::CreateRaw(
                this,
                &FUAssetImporterModule::HandleExternalFileDragOver
            )
        ),
        0
    );

    UE_LOG(LogUAssetImporter, Log, TEXT("UAsset Importer initialized."));
}

void FUAssetImporterModule::ShutdownModule()
{
    if (FModuleManager::Get().IsModuleLoaded(TEXT("ContentBrowser")))
    {
        FContentBrowserModule& ContentBrowserModule =
            FModuleManager::GetModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));

        ContentBrowserModule.GetAssetViewDragAndDropExtenders().RemoveAll(
            [this](const FAssetViewDragAndDropExtender& Extender)
            {
                return Extender.OnDropDelegate.IsBoundToObject(this)
                    || Extender.OnDragOverDelegate.IsBoundToObject(this)
                    || Extender.OnDragLeaveDelegate.IsBoundToObject(this);
            }
        );
    }
}

bool FUAssetImporterModule::HandleExternalFileDragOver(
    const FAssetViewDragAndDropExtender::FPayload& Payload)
{
    if (!Payload.DragDropOp.IsValid()
        || !Payload.DragDropOp->IsOfType<FExternalDragOperation>())
    {
        return false;
    }

    const TSharedPtr<FExternalDragOperation> ExternalOperation =
        StaticCastSharedPtr<FExternalDragOperation>(Payload.DragDropOp);

    if (!ExternalOperation.IsValid() || !ExternalOperation->HasFiles())
    {
        return false;
    }

    for (const FString& FilePath : ExternalOperation->GetFiles())
    {
        if (UAssetImporterModulePrivate::IsUAssetFile(FilePath))
        {
            return true;
        }
    }

    return false;
}

bool FUAssetImporterModule::HandleExternalFileDrop(
    const FAssetViewDragAndDropExtender::FPayload& Payload)
{
    if (!Payload.DragDropOp.IsValid()
        || !Payload.DragDropOp->IsOfType<FExternalDragOperation>())
    {
        return false;
    }

    const TSharedPtr<FExternalDragOperation> ExternalOperation =
        StaticCastSharedPtr<FExternalDragOperation>(Payload.DragDropOp);

    if (!ExternalOperation.IsValid() || !ExternalOperation->HasFiles())
    {
        return false;
    }

    TArray<FString> UAssetFiles;

    for (const FString& FilePath : ExternalOperation->GetFiles())
    {
        if (UAssetImporterModulePrivate::IsUAssetFile(FilePath)
            && FPaths::FileExists(FilePath))
        {
            UAssetFiles.AddUnique(
                UAssetImporterModulePrivate::NormalizeFilePath(FilePath));
        }
    }

    if (UAssetFiles.IsEmpty())
    {
        return false;
    }

    UE_LOG(
        LogUAssetImporter,
        Log,
        TEXT("Intercepted external .uasset drop: %d file(s)."),
        UAssetFiles.Num()
    );

    OpenImportDialogForFiles(UAssetFiles);
    return true;
}

void FUAssetImporterModule::OpenImportDialogForFiles(
    const TArray<FString>& SelectedFiles)
{
    TSharedRef<FUAssetImporterService> Service =
        MakeShared<FUAssetImporterService>();

    TArray<TSharedPtr<FUAssetImporterAssetItem>> Items;
    TArray<FString> PreparationProblems;

    if (!Service->Prepare(SelectedFiles, Items, PreparationProblems))
    {
        if (PreparationProblems.IsEmpty())
        {
            PreparationProblems.Add(
                TEXT("Import preparation failed without an error description.")
            );
        }

        UAssetImporterModulePrivate::ReportProblems(PreparationProblems);
        Service->Cleanup();
        return;
    }

    const FString InitialDestinationPackagePath =
        UAssetImporterModulePrivate::GetCurrentDestinationPath();

    TSharedRef<SWindow> Window = SNew(SWindow)
        .Title(LOCTEXT("ImportWindowTitle", "UAsset Importer"))
        .ClientSize(FVector2D(820.0f, 760.0f))
        .SupportsMinimize(false)
        .SupportsMaximize(true)
        .SizingRule(ESizingRule::UserSized);

    Window->SetContent(
        SNew(SUAssetImporterDialog)
        .Service(Service)
        .Items(Items)
        .InitialDestinationPackagePath(InitialDestinationPackagePath)
        .OnFinished(FSimpleDelegate::CreateLambda(
            [WindowWeak = TWeakPtr<SWindow>(Window)]()
            {
                if (TSharedPtr<SWindow> PinnedWindow = WindowWeak.Pin())
                {
                    PinnedWindow->RequestDestroyWindow();
                }
            }
        ))
    );

    Window->SetOnWindowClosed(FOnWindowClosed::CreateLambda(
        [Service](const TSharedRef<SWindow>&)
        {
            Service->Cleanup();
        }
    ));

    FSlateApplication::Get().AddModalWindow(
        Window,
        FSlateApplication::Get().GetActiveTopLevelWindow(),
        false
    );
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FUAssetImporterModule, UAssetImporter)