#include "SUAssetImporterDialog.h"

#include "Styling/SlateBrush.h"
#include "Styling/SlateIconFinder.h"
#include "ClassIconFinder.h"
#include "Widgets/Views/STreeView.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/SOverlay.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "ContentBrowserModule.h"
#include "ContentBrowserDelegates.h"
#include "Framework/Application/SlateApplication.h"
#include "IContentBrowserSingleton.h"
#include "Logging/MessageLog.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SUAssetImporterDialog"

namespace SUAssetImporterDialogPrivate
{
    static FString NormalizePackageFolder(FString Path)
    {
        Path.TrimStartAndEndInline();
        Path.ReplaceInline(TEXT("\\"), TEXT("/"));
        while (Path.Len() > 1 && Path.EndsWith(TEXT("/")))
        {
            Path.LeftChopInline(1);
        }
        return Path;
    }

    static const FSlateBrush* FindClassIconBrush(const FAssetData& AssetData)
    {
        if (AssetData.IsValid())
        {
            if (const UClass* IconClass = FClassIconFinder::GetIconClassForAssetData(AssetData, nullptr))
            {
                if (const FSlateBrush* Brush = FSlateIconFinder::FindIconBrushForClass(
                    IconClass,
                    FName(TEXT("ClassIcon.Object"))))
                {
                    return Brush;
                }
            }
        }

        if (const FSlateBrush* Brush = FSlateIconFinder::FindIconBrushForClass(
            UObject::StaticClass(),
            FName(TEXT("ClassIcon.Object"))))
        {
            return Brush;
        }
        return FAppStyle::GetDefaultBrush();
    }

    static const FSlateBrush* FindFolderIconBrush()
    {
        const FName FolderIconName(TEXT("ContentBrowser.AssetTreeFolderClosed"));
        if (const FSlateBrush* Brush = FAppStyle::GetBrush(FolderIconName))
        {
            return Brush;
        }
        return FAppStyle::GetDefaultBrush();
    }
}

class SUAssetImporterDependencyTreeRow final : public STableRow<TSharedPtr<FUAssetImporterDependencyTreeNode>>
{
public:
    SLATE_BEGIN_ARGS(SUAssetImporterDependencyTreeRow) {}
        SLATE_ARGUMENT(TSharedPtr<FUAssetImporterDependencyTreeNode>, Item)
        SLATE_ARGUMENT(bool, ShowCheckBox)
        SLATE_ARGUMENT(bool, CheckBoxEnabled)
        SLATE_ARGUMENT(ECheckBoxState, CheckState)
        SLATE_EVENT(FOnCheckStateChanged, OnCheckStateChanged)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTable)
    {
        const TSharedPtr<FUAssetImporterDependencyTreeNode> Item = InArgs._Item;
        const FSlateBrush* IconBrush = Item.IsValid() && Item->IconBrush
            ? Item->IconBrush
            : FAppStyle::GetDefaultBrush();
        FSlateColor StatusColor = FSlateColor::UseSubduedForeground();
        if (Item.IsValid() && Item->bMissing)
        {
            StatusColor = FSlateColor(FLinearColor(0.9f, 0.25f, 0.2f));
        }
        else if (Item.IsValid() && Item->bIsSelectable && !Item->bIsSelected)
        {
            StatusColor = FSlateColor(FLinearColor(0.9f, 0.65f, 0.2f));
        }

        STableRow<TSharedPtr<FUAssetImporterDependencyTreeNode>>::Construct(
            STableRow<TSharedPtr<FUAssetImporterDependencyTreeNode>>::FArguments()
            .ShowSelection(false)
            .Padding(FMargin(2.0f, 2.0f))
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 5.0f, 0.0f)
                [
                    SNew(SBox).WidthOverride(18.0f).HeightOverride(18.0f)
                    [
                        SNew(SCheckBox)
                        .Visibility(InArgs._ShowCheckBox ? EVisibility::Visible : EVisibility::Hidden)
                        .IsEnabled(InArgs._CheckBoxEnabled)
                        .IsChecked(InArgs._CheckState)
                        .OnCheckStateChanged(InArgs._OnCheckStateChanged)
                    ]
                ]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 5.0f, 0.0f)
                [ SNew(SBox).WidthOverride(16.0f).HeightOverride(16.0f)[SNew(SImage).Image(IconBrush)] ]
                + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(Item.IsValid() ? Item->DisplayName : FString()))
                    .Font(Item.IsValid() && Item->bIsFolder
                        ? FAppStyle::Get().GetFontStyle(TEXT("BoldFont"))
                        : FAppStyle::Get().GetFontStyle(TEXT("NormalFont")))
                    .ToolTipText(FText::FromString(Item.IsValid() ? Item->Tooltip : FString()))
                ]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.0f, 0.0f, 2.0f, 0.0f)
                [
                    SNew(STextBlock)
                    .Text(FText::FromString(Item.IsValid() ? Item->StatusText : FString()))
                    .ColorAndOpacity(StatusColor)
                ]
            ],
            OwnerTable);
    }
};

void SUAssetImporterDialog::Construct(const FArguments& InArgs)
{
    Service = InArgs._Service;
    Items = InArgs._Items;
    Options.BatchAlreadyProcessedPackageNames = InArgs._BatchAlreadyProcessedPackageNames;
    OnFinished = InArgs._OnFinished;
    InitialDestinationPackagePath = SUAssetImporterDialogPrivate::NormalizePackageFolder(InArgs._InitialDestinationPackagePath);
    if (InitialDestinationPackagePath.IsEmpty())
    {
        InitialDestinationPackagePath = TEXT("/Game");
    }

    Options.DestinationPackagePath = InitialDestinationPackagePath;
    LoadOptions();
    if (Options.DestinationMode == EUAssetImporterDestinationMode::CurrentContentBrowserFolder)
    {
        Options.DestinationPackagePath = InitialDestinationPackagePath;
    }
    if (Options.DestinationMode == EUAssetImporterDestinationMode::PreserveOriginalPaths)
    {
        Options.DependencyPlacement = EUAssetImporterDependencyPlacement::KeepOriginalPaths;
    }

    if (UTexture2D* CheckerboardTexture = UThumbnailManager::Get().CheckerboardTexture)
    {
        PreviewBackgroundBrush = MakeShared<FSlateBrush>();
        PreviewBackgroundBrush->SetResourceObject(CheckerboardTexture);
        PreviewBackgroundBrush->Tiling = ESlateBrushTileType::Both;
        PreviewBackgroundBrush->ImageSize = FVector2D(
            CheckerboardTexture->GetSizeX(),
            CheckerboardTexture->GetSizeY());
    }

    ChildSlot
    [
        SNew(SBorder)
        .BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
        .Padding(10.0f)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().FillWidth(1.0f)
                [
                    SNew(STextBlock)
                    .Text(LOCTEXT("AssetsHeading", "Assets to Import"))
                    .Font(FAppStyle::Get().GetFontStyle(TEXT("HeadingSmall")))
                ]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
                [
                    SNew(SButton)
                    .Text(LOCTEXT("LoadPreviews", "Load selected previews"))
                    .ToolTipText(LOCTEXT("LoadPreviewsTip", "Load the selected source packages and their dependencies to render live thumbnails."))
                    .IsEnabled_Lambda([this]() { return Service.IsValid() && GetSelectedCount() > 0; })
                    .OnClicked_Lambda([this]()
                    {
                        Service->LoadSelectedPreviews(Items);
                        RefreshAssetRows();
                        return FReply::Handled();
                    })
                ]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [
                    SNew(STextBlock)
                    .Text(this, &SUAssetImporterDialog::GetSelectedCountText)
                    .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
            [
                SNew(SBox)
                .HeightOverride(150.0f)
                [
                    SAssignNew(AssetList, SScrollBox)
                ]
            ]
            + SVerticalBox::Slot().FillHeight(1.0f).Padding(0.0f, 0.0f, 0.0f, 8.0f)
            [
                SNew(SScrollBox)
                + SScrollBox::Slot()
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
                    [
                        SNew(SBorder)
                        .BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
                        .Padding(FMargin(12.0f, 10.0f))
                        [
                            SNew(SVerticalBox)
                            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
                            [
                                SNew(SHorizontalBox)
                                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
                                [ SNew(SImage).Image(SUAssetImporterDialogPrivate::FindFolderIconBrush()) ]
                                + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
                                [
                                    SNew(STextBlock)
                                    .Text(LOCTEXT("DependencyPreviewHeading", "Dependencies"))
                                    .Font(FAppStyle::Get().GetFontStyle(TEXT("HeadingSmall")))
                                ]
                                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                                [
                                    SNew(STextBlock)
                                    .Text(this, &SUAssetImporterDialog::GetDependencyCountText)
                                    .Font(FAppStyle::Get().GetFontStyle(TEXT("BoldFont")))
                                    .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                                ]
                            ]
                            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
                            [ SNew(SSeparator) ]
                            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
                            [
                                SNew(SHorizontalBox)
                                + SHorizontalBox::Slot().FillWidth(1.0f)[ SNew(SSpacer) ]
                                + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 6.0f, 0.0f)
                                [
                                    SNew(SButton)
                                    .Text(LOCTEXT("SelectAllDependencies", "Select all"))
                                    .IsEnabled_Lambda([this]() { return DependencyImportableCount > 0 && DependencySelectedCount < DependencyImportableCount; })
                                    .OnClicked_Lambda([this]() { SelectAllDependencies(true); return FReply::Handled(); })
                                ]
                                + SHorizontalBox::Slot().AutoWidth()
                                [
                                    SNew(SButton)
                                    .Text(LOCTEXT("DeselectAllDependencies", "Deselect all"))
                                    .IsEnabled_Lambda([this]() { return DependencySelectedCount > 0; })
                                    .OnClicked_Lambda([this]() { SelectAllDependencies(false); return FReply::Handled(); })
                                ]
                            ]
                            + SVerticalBox::Slot().AutoHeight()
                            [
                                SNew(SExpandableArea)
                                .InitiallyCollapsed(true)
                                .HeaderContent()
                                [
                                    SNew(STextBlock)
                                    .Text(LOCTEXT("DependencyTreeHeader", "Review dependency files"))
                                    .Font(FAppStyle::Get().GetFontStyle(TEXT("BoldFont")))
                                ]
                                .BodyContent()
                                [
                                    SNew(SBorder)
                                    .BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
                                    .Padding(4.0f)
                                    [
                                        SNew(SBox)
                                        .HeightOverride(160.0f)
                                        [
                                            SAssignNew(DependencyTreeView, STreeView<FDependencyTreeNodePtr>)
                                            .TreeItemsSource(&DependencyTreeRoots)
                                            .OnGenerateRow(this, &SUAssetImporterDialog::GenerateDependencyTreeRow)
                                            .OnGetChildren(this, &SUAssetImporterDialog::GetDependencyTreeChildren)
                                            .SelectionMode(ESelectionMode::None)
                                        ]
                                    ]
                                ]
                            ]
                        ]
                    ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 10.0f)
                    [
                        SNew(SBorder)
                        .BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
                        .Padding(FMargin(12.0f, 10.0f))
                        [
                            SNew(SVerticalBox)
                            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
                            [
                                SNew(STextBlock)
                                .Text(LOCTEXT("OptionsHeading", "Import Options"))
                                .Font(FAppStyle::Get().GetFontStyle(TEXT("HeadingSmall")))
                            ]
                            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
                            [ SNew(SSeparator) ]
                            + SVerticalBox::Slot().AutoHeight()
                            [
                                SNew(SHorizontalBox)
                                + SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 16.0f, 0.0f)
                                [
                                    SNew(SVerticalBox)
                                    + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
                                    [
                                        SNew(SCheckBox)
                                        .IsChecked_Lambda([this]() { return Options.bAbortIfDependencyMissing ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                                        .OnCheckStateChanged_Lambda([this](ECheckBoxState State) { Options.bAbortIfDependencyMissing = State == ECheckBoxState::Checked; })
                                        [ SNew(STextBlock).Text(LOCTEXT("AbortMissing", "Abort if a required dependency is missing")) ]
                                    ]
                                    + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
                                    [
                                        SNew(SCheckBox)
                                        .IsChecked_Lambda([this]() { return Options.bIgnoreSoftReferenceErrors ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                                        .OnCheckStateChanged_Lambda([this](ECheckBoxState State) { Options.bIgnoreSoftReferenceErrors = State == ECheckBoxState::Checked; })
                                        [ SNew(STextBlock).Text(LOCTEXT("IgnoreSoft", "Ignore missing soft references")) ]
                                    ]
                                    + SVerticalBox::Slot().AutoHeight()
                                    [
                                        SNew(SCheckBox)
                                        .IsChecked_Lambda([this]() { return Options.bOverwriteExistingAssets ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                                        .OnCheckStateChanged_Lambda([this](ECheckBoxState State) { Options.bOverwriteExistingAssets = State == ECheckBoxState::Checked; })
                                        [ SNew(STextBlock).Text(LOCTEXT("Overwrite", "Overwrite existing assets")) ]
                                    ]
                                ]
                                + SHorizontalBox::Slot().FillWidth(1.0f)
                                [
                                    SNew(SVerticalBox)
                                    + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
                                    [
                                        SNew(SCheckBox)
                                        .IsChecked_Lambda([this]() { return Options.bRollbackOnFailure ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                                        .OnCheckStateChanged_Lambda([this](ECheckBoxState State) { Options.bRollbackOnFailure = State == ECheckBoxState::Checked; })
                                        [ SNew(STextBlock).Text(LOCTEXT("Rollback", "Roll back copied files if import fails")) ]
                                    ]
                                    + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
                                    [
                                        SNew(SCheckBox)
                                        .IsChecked_Lambda([this]() { return Options.bSyncImportedAssets ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                                        .OnCheckStateChanged_Lambda([this](ECheckBoxState State) { Options.bSyncImportedAssets = State == ECheckBoxState::Checked; })
                                        [ SNew(STextBlock).Text(LOCTEXT("SyncAssets", "Select imported assets in Content Browser")) ]
                                    ]
                                    + SVerticalBox::Slot().AutoHeight()
                                    [
                                        SNew(SCheckBox)
                                        .IsChecked_Lambda([this]() { return Options.bLoadImportedRootAssets ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                                        .OnCheckStateChanged_Lambda([this](ECheckBoxState State) { Options.bLoadImportedRootAssets = State == ECheckBoxState::Checked; })
                                        [ SNew(STextBlock).Text(LOCTEXT("LoadAssets", "Load selected assets after import")) ]
                                    ]
                                ]
                            ]
                        ]
                    ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
                    [
                        SNew(SBorder)
                        .BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
                        .Padding(FMargin(12.0f, 10.0f))
                        [
                            SNew(SVerticalBox)
                            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
                            [
                                SNew(STextBlock)
                                .Text(LOCTEXT("ImportPlacementHeading", "Import Placement"))
                                .Font(FAppStyle::Get().GetFontStyle(TEXT("HeadingSmall")))
                            ]
                            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
                            [ SNew(SSeparator) ]
                            + SVerticalBox::Slot().AutoHeight()
                            [
                                SNew(SHorizontalBox)
                                + SHorizontalBox::Slot().FillWidth(1.0f).Padding(0.0f, 0.0f, 12.0f, 0.0f)
                                [
                                    SNew(SVerticalBox)
                                    + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 5.0f)
                                    [
                                        SNew(STextBlock)
                                        .Text(LOCTEXT("DependencyPlacementHeading", "Dependency placement"))
                                        .Font(FAppStyle::Get().GetFontStyle(TEXT("BoldFont")))
                                    ]
                                    + SVerticalBox::Slot().AutoHeight()
                                    [
                                        SAssignNew(DependencyPlacementComboButton, SComboButton)
                                        .IsEnabled_Lambda([this]() { return Options.DestinationMode != EUAssetImporterDestinationMode::PreserveOriginalPaths; })
                                        .OnGetMenuContent(this, &SUAssetImporterDialog::MakeDependencyPlacementMenu)
                                        .ButtonContent()
                                        [ SNew(STextBlock).Text(this, &SUAssetImporterDialog::GetDependencyPlacementLabel) ]
                                    ]
                                ]
                                + SHorizontalBox::Slot().FillWidth(1.0f)
                                [
                                    SNew(SVerticalBox)
                                    + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 5.0f)
                                    [
                                        SNew(STextBlock)
                                        .Text(LOCTEXT("DestinationHeading", "Destination"))
                                        .Font(FAppStyle::Get().GetFontStyle(TEXT("BoldFont")))
                                    ]
                                    + SVerticalBox::Slot().AutoHeight()
                                    [
                                        SAssignNew(DestinationComboButton, SComboButton)
                                        .OnGetMenuContent(this, &SUAssetImporterDialog::MakeDestinationMenu)
                                        .ButtonContent()
                                        [ SNew(STextBlock).Text(this, &SUAssetImporterDialog::GetDestinationLabel) ]
                                    ]
                                ]
                            ]
                        ]
                    ]
                ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
            [ SNew(SSeparator) ]
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().FillWidth(1.0f)[ SNew(SSpacer) ]
                + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
                [
                    SNew(SButton)
                    .Text(LOCTEXT("CancelButton", "Cancel"))
                    .IsEnabled_Lambda([this]() { return !bIsImporting; })
                    .OnClicked(this, &SUAssetImporterDialog::CancelImport)
                ]
                + SHorizontalBox::Slot().AutoWidth()
                [
                    SNew(SButton)
                    .Text(LOCTEXT("ImportButton", "Import"))
                    .ButtonStyle(FAppStyle::Get(), TEXT("PrimaryButton"))
                    .IsEnabled_Lambda([this]() { return !bIsImporting && GetSelectedCount() > 0; })
                    .OnClicked(this, &SUAssetImporterDialog::StartImport)
                ]
            ]
        ]
    ];

    RefreshAssetRows();
    RebuildDependencyTree();
}

void SUAssetImporterDialog::RefreshAssetRows()
{
    AssetList->ClearChildren();
    PreviewBrushes.Reset();
    for (const TSharedPtr<FUAssetImporterAssetItem>& Item : Items)
    {
        AssetList->AddSlot().Padding(0.0f, 0.0f, 0.0f, 5.0f)[MakeAssetRow(Item)];
    }
}

const FUAssetImporterResult& SUAssetImporterDialog::GetImportResult() const
{
    return ImportResult;
}

const FUAssetImporterOptions& SUAssetImporterDialog::GetOptions() const
{
    return Options;
}

bool SUAssetImporterDialog::DidCompleteImport() const
{
    return bImportCompleted;
}

TSharedRef<SWidget> SUAssetImporterDialog::MakeDestinationMenu()
{
    return SNew(SBorder).Padding(4.0f)
    [
        SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight()
        [
            SNew(SButton).ButtonStyle(FAppStyle::Get(), TEXT("Menu.Button"))
            .Text(LOCTEXT("DestinationCurrent", "Current Content Browser folder"))
            .OnClicked_Lambda([this]() { SelectDestinationMode(EUAssetImporterDestinationMode::CurrentContentBrowserFolder); if (DestinationComboButton.IsValid()) DestinationComboButton->SetIsOpen(false); return FReply::Handled(); })
        ]
        + SVerticalBox::Slot().AutoHeight()
        [
            SNew(SButton).ButtonStyle(FAppStyle::Get(), TEXT("Menu.Button"))
            .Text(LOCTEXT("DestinationRoot", "Project Content root (/Game)"))
            .OnClicked_Lambda([this]() { SelectDestinationMode(EUAssetImporterDestinationMode::ProjectContentRoot); if (DestinationComboButton.IsValid()) DestinationComboButton->SetIsOpen(false); return FReply::Handled(); })
        ]
        + SVerticalBox::Slot().AutoHeight()
        [
            SNew(SButton).ButtonStyle(FAppStyle::Get(), TEXT("Menu.Button"))
            .Text(LOCTEXT("DestinationOriginal", "Preserve original package paths"))
            .OnClicked_Lambda([this]() { SelectDestinationMode(EUAssetImporterDestinationMode::PreserveOriginalPaths); if (DestinationComboButton.IsValid()) DestinationComboButton->SetIsOpen(false); return FReply::Handled(); })
        ]
        + SVerticalBox::Slot().AutoHeight()
        [
            SNew(SButton).ButtonStyle(FAppStyle::Get(), TEXT("Menu.Button"))
            .Text(LOCTEXT("DestinationChoose", "Choose folder..."))
            .OnClicked_Lambda([this]() { if (DestinationComboButton.IsValid()) DestinationComboButton->SetIsOpen(false); OpenDestinationPicker(); return FReply::Handled(); })
        ]
    ];
}

TSharedRef<SWidget> SUAssetImporterDialog::MakeDependencyPlacementMenu()
{
    return SNew(SBorder).Padding(4.0f)
    [
        SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight()
        [
            SNew(SButton).ButtonStyle(FAppStyle::Get(), TEXT("Menu.Button"))
            .Text(LOCTEXT("DependencyKeepOriginal", "Keep dependencies at original paths"))
            .OnClicked_Lambda([this]() { SelectDependencyPlacement(EUAssetImporterDependencyPlacement::KeepOriginalPaths); if (DependencyPlacementComboButton.IsValid()) DependencyPlacementComboButton->SetIsOpen(false); return FReply::Handled(); })
        ]
        + SVerticalBox::Slot().AutoHeight()
        [
            SNew(SButton).ButtonStyle(FAppStyle::Get(), TEXT("Menu.Button"))
            .Text(LOCTEXT("DependencySubfolder", "Dependencies folder (preserve source structure)"))
            .OnClicked_Lambda([this]() { SelectDependencyPlacement(EUAssetImporterDependencyPlacement::DependenciesFolder); if (DependencyPlacementComboButton.IsValid()) DependencyPlacementComboButton->SetIsOpen(false); return FReply::Handled(); })
        ]
        + SVerticalBox::Slot().AutoHeight()
        [
            SNew(SButton).ButtonStyle(FAppStyle::Get(), TEXT("Menu.Button"))
            .Text(LOCTEXT("DependencyFlat", "One Dependencies folder (flatten)"))
            .OnClicked_Lambda([this]() { SelectDependencyPlacement(EUAssetImporterDependencyPlacement::FlatDependenciesFolder); if (DependencyPlacementComboButton.IsValid()) DependencyPlacementComboButton->SetIsOpen(false); return FReply::Handled(); })
        ]
        + SVerticalBox::Slot().AutoHeight()
        [
            SNew(SButton).ButtonStyle(FAppStyle::Get(), TEXT("Menu.Button"))
            .Text(LOCTEXT("DependencySameFolder", "Same folder as imported assets"))
            .OnClicked_Lambda([this]() { SelectDependencyPlacement(EUAssetImporterDependencyPlacement::SameFolderAsAssets); if (DependencyPlacementComboButton.IsValid()) DependencyPlacementComboButton->SetIsOpen(false); return FReply::Handled(); })
        ]
    ];
}

TSharedRef<SWidget> SUAssetImporterDialog::MakeAssetRow(const TSharedPtr<FUAssetImporterAssetItem>& Item)
{
    TSharedPtr<FSlateBrush> PreviewBrush;
    const FSlateBrush* FallbackIconBrush = Item.IsValid()
        ? SUAssetImporterDialogPrivate::FindClassIconBrush(Item->AssetData)
        : FAppStyle::GetDefaultBrush();
    TSharedRef<SWidget> ThumbnailWidget = StaticCastSharedRef<SWidget>(
        SNew(SBox).WidthOverride(80.0f).HeightOverride(80.0f).HAlign(HAlign_Center).VAlign(VAlign_Center)
        [
            SNew(SBox).WidthOverride(48.0f).HeightOverride(48.0f)
            [ SNew(SImage).Image(FallbackIconBrush) ]
        ]);

    if (Item.IsValid() && Item->PreviewTexture.IsValid())
    {
        PreviewBrush = MakeShared<FSlateBrush>();
        PreviewBrush->SetResourceObject(Item->PreviewTexture.Get());
        PreviewBrush->ImageSize = FVector2D(80.0f, 80.0f);
        PreviewBrushes.Add(PreviewBrush);

        if (PreviewBackgroundBrush.IsValid())
        {
            ThumbnailWidget = StaticCastSharedRef<SWidget>(
                SNew(SOverlay)
                + SOverlay::Slot()
                [
                    SNew(SImage).Image(PreviewBackgroundBrush.Get())
                ]
                + SOverlay::Slot()
                [
                    SNew(SImage).Image(PreviewBrush.Get())
                ]);
        }
        else
        {
            ThumbnailWidget = StaticCastSharedRef<SWidget>(
                SNew(SImage).Image(PreviewBrush.Get()));
        }
    }

    const FString FilePath = Item.IsValid() ? Item->SourceFile : FString();
    const FString AssetName = Item.IsValid() ? Item->AssetName : FString();
    const FString AssetClass = Item.IsValid() ? Item->AssetClass : FString();
    const FString AssetPath = Item.IsValid() ? Item->DisplayPath : FString();
    const FString EngineVersion = Item.IsValid() ? Item->SavedEngineVersion : FString();
    const int64 FileSize = Item.IsValid() ? Item->FileSize : 0;
    const bool bMetadataValid = Item.IsValid() && Item->bMetadataValid;
    const FString EngineVersionText = EngineVersion.IsEmpty() ? TEXT("Saved engine version unavailable") : FString::Printf(TEXT("Saved with UE %s"), *EngineVersion);
    const FString FileSizeText = FileSize > 0 ? FText::AsMemory(FileSize).ToString() : TEXT("File size unavailable");

    const FText MetadataText = FText::FromString(FString::Printf(
        TEXT("%s  |  %s\n%s  |  %s  |  %s"),
        *AssetClass,
        bMetadataValid ? TEXT("Metadata available") : TEXT("Metadata unavailable"),
        *AssetPath,
        *EngineVersionText,
        *FileSizeText));

    const FText TooltipText = FText::FromString(FString::Printf(
        TEXT("%s\nClass: %s\nPackage: %s\n%s\nSize: %s\nSource: %s"),
        *AssetName,
        *AssetClass,
        *AssetPath,
        *EngineVersionText,
        *FileSizeText,
        *FilePath));

    return SNew(SBorder)
        .BorderImage(FAppStyle::GetBrush(TEXT("ToolPanel.GroupBorder")))
        .Padding(6.0f)
        .ToolTipText(TooltipText)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 8.0f, 0.0f)
            [
                SNew(SCheckBox)
                .IsChecked_Lambda([Item]() { return Item.IsValid() && Item->bSelected ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                .OnCheckStateChanged_Lambda([this, Item](ECheckBoxState State)
                {
                    if (Item.IsValid())
                    {
                        Item->bSelected = State == ECheckBoxState::Checked;
                        RebuildDependencyTree();
                    }
                })
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.0f, 0.0f, 10.0f, 0.0f)
            [ SNew(SBox).WidthOverride(80.0f).HeightOverride(80.0f)[ThumbnailWidget] ]
            + SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 3.0f)
                [ SNew(STextBlock).Text(FText::FromString(AssetName)).Font(FAppStyle::Get().GetFontStyle(TEXT("BoldFont"))).ToolTipText(FText::FromString(FilePath)) ]
                + SVerticalBox::Slot().AutoHeight()
                [ SNew(STextBlock).Text(MetadataText).AutoWrapText(true).ColorAndOpacity(bMetadataValid ? FSlateColor::UseSubduedForeground() : FSlateColor(FLinearColor::Red)) ]
            ]
        ];
}

TSharedRef<ITableRow> SUAssetImporterDialog::GenerateDependencyTreeRow(
    FDependencyTreeNodePtr Item,
    const TSharedRef<STableViewBase>& OwnerTable)
{
    const bool bShowCheckBox = Item.IsValid()
        && (Item->bIsFolder ? HasSelectableDependency(Item) : Item->bIsSelectable);

    return SNew(SUAssetImporterDependencyTreeRow, OwnerTable)
        .Item(Item)
        .ShowCheckBox(bShowCheckBox)
        .CheckBoxEnabled(bShowCheckBox)
        .CheckState(GetDependencyNodeCheckState(Item))
        .OnCheckStateChanged_Lambda([this, Item](ECheckBoxState State)
        {
            SetDependencyNodeSelection(Item, State == ECheckBoxState::Checked);
        });
}

void SUAssetImporterDialog::GetDependencyTreeChildren(
    FDependencyTreeNodePtr Item,
    TArray<FDependencyTreeNodePtr>& OutChildren) const
{
    OutChildren.Reset();
    if (Item.IsValid())
    {
        OutChildren = Item->Children;
    }
}

void SUAssetImporterDialog::RebuildDependencyTree()
{
    DependencyTreeRoots.Reset();
    DependencyCount = 0;
    DependencyImportableCount = 0;
    DependencySelectedCount = 0;
    DependencyReusedCount = 0;
    DependencyMissingCount = 0;

    if (!Service.IsValid())
    {
        if (DependencyTreeView.IsValid())
        {
            DependencyTreeView->RequestTreeRefresh();
        }
        return;
    }

    TArray<FUAssetImporterDependencyItem> Dependencies;
    Service->GetDependencyItems(Items, Dependencies);
    DependencyCount = Dependencies.Num();

    TSet<FString> CurrentDependencyPackages;
    for (const FUAssetImporterDependencyItem& Dependency : Dependencies)
    {
        CurrentDependencyPackages.Add(Dependency.OriginalPackageName.ToLower());
        if (Dependency.bAlreadyInProject)
        {
            ++DependencyReusedCount;
        }
        if (Dependency.bMissing)
        {
            ++DependencyMissingCount;
        }
        if (Dependency.bHasSourceFile && Dependency.bHasMetadata && !Dependency.bAlreadyInProject && !Dependency.bMissing)
        {
            ++DependencyImportableCount;
        }
    }

    for (auto It = ExcludedDependencyPackageNames.CreateIterator(); It; ++It)
    {
        if (!CurrentDependencyPackages.Contains(*It))
        {
            It.RemoveCurrent();
        }
    }

    TMap<FString, FDependencyTreeNodePtr> FolderNodesByPath;
    const FSlateBrush* FolderBrush = SUAssetImporterDialogPrivate::FindFolderIconBrush();

    for (const FUAssetImporterDependencyItem& Dependency : Dependencies)
    {
        const FString PackagePath = FPackageName::GetLongPackagePath(Dependency.OriginalPackageName);
        TArray<FString> FolderSegments;
        PackagePath.ParseIntoArray(FolderSegments, TEXT("/"), true);
        if (!FolderSegments.IsEmpty() && FolderSegments[0].Equals(TEXT("Game"), ESearchCase::IgnoreCase))
        {
            FolderSegments.RemoveAt(0);
        }

        FDependencyTreeNodePtr ParentFolder;
        FString CurrentFolderPath;
        for (const FString& FolderSegment : FolderSegments)
        {
            CurrentFolderPath = CurrentFolderPath.IsEmpty()
                ? FolderSegment
                : CurrentFolderPath + TEXT("/") + FolderSegment;
            const FString FolderKey = CurrentFolderPath.ToLower();
            FDependencyTreeNodePtr* ExistingFolder = FolderNodesByPath.Find(FolderKey);
            if (ExistingFolder)
            {
                ParentFolder = *ExistingFolder;
                continue;
            }

            FDependencyTreeNodePtr FolderNode = MakeShared<FUAssetImporterDependencyTreeNode>();
            FolderNode->DisplayName = FolderSegment;
            FolderNode->PackageName = CurrentFolderPath;
            FolderNode->Tooltip = CurrentFolderPath;
            FolderNode->IconBrush = FolderBrush;
            FolderNode->bIsFolder = true;

            if (ParentFolder.IsValid())
            {
                ParentFolder->Children.Add(FolderNode);
            }
            else
            {
                DependencyTreeRoots.Add(FolderNode);
            }

            FolderNodesByPath.Add(FolderKey, FolderNode);
            ParentFolder = MoveTemp(FolderNode);
        }

        FDependencyTreeNodePtr AssetNode = MakeShared<FUAssetImporterDependencyTreeNode>();
        AssetNode->DisplayName = Dependency.AssetName;
        AssetNode->PackageName = Dependency.OriginalPackageName;
        AssetNode->IconBrush = SUAssetImporterDialogPrivate::FindClassIconBrush(Dependency.AssetData);
        AssetNode->bMissing = Dependency.bMissing;
        AssetNode->bAlreadyInProject = Dependency.bAlreadyInProject;
        AssetNode->bIsSelectable = Dependency.bHasSourceFile
            && Dependency.bHasMetadata
            && !Dependency.bAlreadyInProject
            && !Dependency.bMissing;
        AssetNode->bIsSelected = AssetNode->bIsSelectable
            ? !ExcludedDependencyPackageNames.Contains(Dependency.OriginalPackageName.ToLower())
            : Dependency.bAlreadyInProject;

        if (AssetNode->bIsSelectable)
        {
            if (AssetNode->bIsSelected)
            {
                AssetNode->StatusText = TEXT("Selected for import");
                ++DependencySelectedCount;
            }
            else
            {
                AssetNode->StatusText = TEXT("Excluded by user");
            }
        }
        else if (Dependency.bMissing)
        {
            AssetNode->StatusText = TEXT("Missing");
        }
        else if (Dependency.bAlreadyInProject)
        {
            AssetNode->StatusText = TEXT("Reused from project");
        }
        else
        {
            AssetNode->StatusText = TEXT("Metadata unavailable");
        }

        AssetNode->Tooltip = FString::Printf(
            TEXT("%s\nClass: %s\nReference: %s\nPackage: %s\nSource: %s"),
            *Dependency.AssetName,
            *Dependency.AssetClass,
            Dependency.bSoftOnly ? TEXT("Soft") : TEXT("Hard or mixed"),
            *Dependency.OriginalPackageName,
            Dependency.SourceFile.IsEmpty() ? TEXT("No source file found") : *Dependency.SourceFile);
        if (AssetNode->bIsSelectable && !AssetNode->bIsSelected)
        {
            AssetNode->Tooltip += TEXT("\nThis dependency is excluded from import. References to it may remain unresolved.");
        }

        if (ParentFolder.IsValid())
        {
            ParentFolder->Children.Add(AssetNode);
        }
        else
        {
            DependencyTreeRoots.Add(AssetNode);
        }
    }

    if (DependencyTreeView.IsValid())
    {
        DependencyTreeView->RequestTreeRefresh();
    }
}

void SUAssetImporterDialog::OpenDestinationPicker()
{
    if (!FModuleManager::Get().IsModuleLoaded(TEXT("ContentBrowser")))
    {
        FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
    }

    FContentBrowserModule& ContentBrowserModule = FModuleManager::GetModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
    FString SelectedPath = Options.DestinationPackagePath.IsEmpty() ? InitialDestinationPackagePath : Options.DestinationPackagePath;
    bool bAccepted = false;

    FPathPickerConfig PathPickerConfig;
    PathPickerConfig.DefaultPath = SelectedPath.IsEmpty() ? TEXT("/Game") : SelectedPath;
    PathPickerConfig.bAllowClassesFolder = false;
    PathPickerConfig.bAllowReadOnlyFolders = false;
    PathPickerConfig.bOnPathSelectedPassesVirtualPaths = false;
    PathPickerConfig.bNotifyDefaultPathSelected = false;
    PathPickerConfig.OnPathSelected = FOnPathSelected::CreateLambda([&SelectedPath](const FString& Path) { SelectedPath = Path; });

    TSharedRef<SWindow> PickerWindow = SNew(SWindow)
        .Title(LOCTEXT("ChooseDestinationTitle", "Choose Import Destination"))
        .ClientSize(FVector2D(420.0f, 560.0f))
        .SupportsMinimize(false)
        .SupportsMaximize(false)
        .SizingRule(ESizingRule::UserSized);

    PickerWindow->SetContent(
        SNew(SBorder).Padding(8.0f)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().FillHeight(1.0f)
            [ ContentBrowserModule.Get().CreatePathPicker(PathPickerConfig) ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 8.0f, 0.0f, 0.0f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().FillWidth(1.0f)[SNew(SSpacer)]
                + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
                [ SNew(SButton).Text(LOCTEXT("DestinationPickerCancel", "Cancel")).OnClicked_Lambda([&PickerWindow]() { PickerWindow->RequestDestroyWindow(); return FReply::Handled(); }) ]
                + SHorizontalBox::Slot().AutoWidth()
                [
                    SNew(SButton).ButtonStyle(FAppStyle::Get(), TEXT("PrimaryButton"))
                    .IsEnabled_Lambda([&SelectedPath]() { return FPackageName::IsValidLongPackageName(SelectedPath, true); })
                    .Text(LOCTEXT("DestinationPickerUse", "Use Selected Folder"))
                    .OnClicked_Lambda([&PickerWindow, &SelectedPath, &bAccepted]()
                    {
                        if (!FPackageName::IsValidLongPackageName(SelectedPath, true))
                        {
                            return FReply::Handled();
                        }
                        bAccepted = true;
                        PickerWindow->RequestDestroyWindow();
                        return FReply::Handled();
                    })
                ]
            ]
        ]);

    FSlateApplication::Get().AddModalWindow(PickerWindow, FSlateApplication::Get().GetActiveTopLevelWindow(), false);
    if (bAccepted)
    {
        Options.DestinationMode = EUAssetImporterDestinationMode::CustomFolder;
        Options.DestinationPackagePath = SUAssetImporterDialogPrivate::NormalizePackageFolder(SelectedPath);
    }
}

void SUAssetImporterDialog::SelectDestinationMode(EUAssetImporterDestinationMode Mode)
{
    Options.DestinationMode = Mode;
    if (Mode == EUAssetImporterDestinationMode::CurrentContentBrowserFolder)
    {
        Options.DestinationPackagePath = InitialDestinationPackagePath;
    }
    else if (Mode == EUAssetImporterDestinationMode::ProjectContentRoot)
    {
        Options.DestinationPackagePath = TEXT("/Game");
    }
    else if (Mode == EUAssetImporterDestinationMode::PreserveOriginalPaths)
    {
        Options.DependencyPlacement = EUAssetImporterDependencyPlacement::KeepOriginalPaths;
    }
}

void SUAssetImporterDialog::SelectDependencyPlacement(const EUAssetImporterDependencyPlacement Placement)
{
    Options.DependencyPlacement = Placement;
}

FText SUAssetImporterDialog::GetDependencyPlacementLabel() const
{
    switch (Options.DependencyPlacement)
    {
    case EUAssetImporterDependencyPlacement::DependenciesFolder:
        return LOCTEXT("DependencyLabelSubfolder", "Dependencies folder (preserve source structure)");
    case EUAssetImporterDependencyPlacement::FlatDependenciesFolder:
        return LOCTEXT("DependencyLabelFlat", "One Dependencies folder (flatten)");
    case EUAssetImporterDependencyPlacement::SameFolderAsAssets:
        return LOCTEXT("DependencyLabelSameFolder", "Same folder as imported assets");
    default:
        return LOCTEXT("DependencyLabelOriginal", "Keep dependencies at original paths");
    }
}

FText SUAssetImporterDialog::GetDependencyCountText() const
{
    if (DependencyCount == 0)
    {
        return LOCTEXT("DependencyCountNone", "No dependencies");
    }

    return FText::FromString(FString::Printf(
        TEXT("%d total  |  %d to import  |  %d reused  |  %d missing"),
        DependencyCount,
        DependencySelectedCount,
        DependencyReusedCount,
        DependencyMissingCount));
}

void SUAssetImporterDialog::CountSelectableDependencyNodes(
    FDependencyTreeNodePtr Item,
    int32& OutTotal,
    int32& OutSelected) const
{
    if (!Item.IsValid())
    {
        return;
    }

    if (Item->bIsFolder)
    {
        for (const FDependencyTreeNodePtr& Child : Item->Children)
        {
            CountSelectableDependencyNodes(Child, OutTotal, OutSelected);
        }
        return;
    }

    if (Item->bIsSelectable)
    {
        ++OutTotal;
        if (Item->bIsSelected)
        {
            ++OutSelected;
        }
    }
}

bool SUAssetImporterDialog::HasSelectableDependency(FDependencyTreeNodePtr Item) const
{
    if (!Item.IsValid())
    {
        return false;
    }
    if (!Item->bIsFolder)
    {
        return Item->bIsSelectable;
    }
    for (const FDependencyTreeNodePtr& Child : Item->Children)
    {
        if (HasSelectableDependency(Child))
        {
            return true;
        }
    }
    return false;
}

ECheckBoxState SUAssetImporterDialog::GetDependencyNodeCheckState(FDependencyTreeNodePtr Item) const
{
    if (!Item.IsValid())
    {
        return ECheckBoxState::Unchecked;
    }

    if (!Item->bIsFolder)
    {
        return Item->bIsSelected ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
    }

    int32 Total = 0;
    int32 Selected = 0;
    CountSelectableDependencyNodes(Item, Total, Selected);
    if (Total == 0 || Selected == Total)
    {
        return ECheckBoxState::Checked;
    }
    if (Selected == 0)
    {
        return ECheckBoxState::Unchecked;
    }
    return ECheckBoxState::Undetermined;
}

void SUAssetImporterDialog::SetDependencyNodeSelectionRecursive(FDependencyTreeNodePtr Item, const bool bSelected)
{
    if (!Item.IsValid())
    {
        return;
    }

    if (Item->bIsFolder)
    {
        for (const FDependencyTreeNodePtr& Child : Item->Children)
        {
            SetDependencyNodeSelectionRecursive(Child, bSelected);
        }
        return;
    }

    if (!Item->bIsSelectable)
    {
        return;
    }

    Item->bIsSelected = bSelected;
    const FString PackageKey = Item->PackageName.ToLower();
    if (bSelected)
    {
        ExcludedDependencyPackageNames.Remove(PackageKey);
        Item->StatusText = TEXT("Selected for import");
    }
    else
    {
        ExcludedDependencyPackageNames.Add(PackageKey);
        Item->StatusText = TEXT("Excluded by user");
    }
}

void SUAssetImporterDialog::SetDependencyNodeSelection(FDependencyTreeNodePtr Item, const bool bSelected)
{
    SetDependencyNodeSelectionRecursive(Item, bSelected);
    UpdateDependencySelectionCounts();
    if (DependencyTreeView.IsValid())
    {
        DependencyTreeView->RequestTreeRefresh();
    }
}

void SUAssetImporterDialog::SelectAllDependencies(const bool bSelected)
{
    for (const FDependencyTreeNodePtr& Root : DependencyTreeRoots)
    {
        SetDependencyNodeSelectionRecursive(Root, bSelected);
    }
    UpdateDependencySelectionCounts();
    if (DependencyTreeView.IsValid())
    {
        DependencyTreeView->RequestTreeRefresh();
    }
}

void SUAssetImporterDialog::UpdateDependencySelectionCounts()
{
    DependencySelectedCount = 0;
    for (const FDependencyTreeNodePtr& Root : DependencyTreeRoots)
    {
        int32 Total = 0;
        int32 Selected = 0;
        CountSelectableDependencyNodes(Root, Total, Selected);
        DependencySelectedCount += Selected;
    }
}

void SUAssetImporterDialog::CollectSelectedDependencyPackages(
    FDependencyTreeNodePtr Item,
    TArray<FString>& OutPackageNames) const
{
    if (!Item.IsValid())
    {
        return;
    }

    if (Item->bIsFolder)
    {
        for (const FDependencyTreeNodePtr& Child : Item->Children)
        {
            CollectSelectedDependencyPackages(Child, OutPackageNames);
        }
        return;
    }

    if (Item->bIsSelectable && Item->bIsSelected)
    {
        OutPackageNames.AddUnique(Item->PackageName);
    }
}

FText SUAssetImporterDialog::GetDestinationLabel() const
{
    switch (Options.DestinationMode)
    {
    case EUAssetImporterDestinationMode::ProjectContentRoot:
        return LOCTEXT("DestinationLabelRoot", "Project Content root (/Game)");
    case EUAssetImporterDestinationMode::PreserveOriginalPaths:
        return LOCTEXT("DestinationLabelOriginal", "Preserve original package paths");
    case EUAssetImporterDestinationMode::CustomFolder:
        return FText::Format(LOCTEXT("DestinationLabelCustom", "Custom folder: {0}"), FText::FromString(Options.DestinationPackagePath));
    default:
        return FText::Format(LOCTEXT("DestinationLabelCurrent", "Current Content Browser folder: {0}"), FText::FromString(InitialDestinationPackagePath));
    }
}

void SUAssetImporterDialog::LoadOptions()
{
    if (!GConfig)
    {
        return;
    }

    const TCHAR* Section = TEXT("UAssetImporter.ImportOptions");
    GConfig->GetBool(Section, TEXT("AbortIfDependencyMissing"), Options.bAbortIfDependencyMissing, GEditorPerProjectIni);
    GConfig->GetBool(Section, TEXT("IgnoreSoftReferenceErrors"), Options.bIgnoreSoftReferenceErrors, GEditorPerProjectIni);
    GConfig->GetBool(Section, TEXT("OverwriteExistingAssets"), Options.bOverwriteExistingAssets, GEditorPerProjectIni);
    GConfig->GetBool(Section, TEXT("RollbackOnFailure"), Options.bRollbackOnFailure, GEditorPerProjectIni);
    GConfig->GetBool(Section, TEXT("SyncImportedAssets"), Options.bSyncImportedAssets, GEditorPerProjectIni);
    GConfig->GetBool(Section, TEXT("LoadImportedRootAssets"), Options.bLoadImportedRootAssets, GEditorPerProjectIni);

    int32 DependencyPlacementValue = static_cast<int32>(Options.DependencyPlacement);
    if (!GConfig->GetInt(Section, TEXT("DependencyPlacementMode"), DependencyPlacementValue, GEditorPerProjectIni))
    {
        bool bLegacyPlaceDependencies = false;
        bool bLegacyFlattenDependencies = false;
        GConfig->GetBool(Section, TEXT("PlaceDependenciesInDestination"), bLegacyPlaceDependencies, GEditorPerProjectIni);
        GConfig->GetBool(Section, TEXT("FlattenDependenciesInOneFolder"), bLegacyFlattenDependencies, GEditorPerProjectIni);
        if (bLegacyPlaceDependencies)
        {
            DependencyPlacementValue = static_cast<int32>(bLegacyFlattenDependencies
                ? EUAssetImporterDependencyPlacement::FlatDependenciesFolder
                : EUAssetImporterDependencyPlacement::DependenciesFolder);
        }
        else
        {
            DependencyPlacementValue = static_cast<int32>(EUAssetImporterDependencyPlacement::KeepOriginalPaths);
        }
    }
    if (DependencyPlacementValue >= 0 && DependencyPlacementValue <= static_cast<int32>(EUAssetImporterDependencyPlacement::SameFolderAsAssets))
    {
        Options.DependencyPlacement = static_cast<EUAssetImporterDependencyPlacement>(DependencyPlacementValue);
    }

    int32 DestinationModeValue = static_cast<int32>(Options.DestinationMode);
    GConfig->GetInt(Section, TEXT("DestinationMode"), DestinationModeValue, GEditorPerProjectIni);
    if (DestinationModeValue >= 0 && DestinationModeValue <= static_cast<int32>(EUAssetImporterDestinationMode::CustomFolder))
    {
        Options.DestinationMode = static_cast<EUAssetImporterDestinationMode>(DestinationModeValue);
    }
    GConfig->GetString(Section, TEXT("DestinationPackagePath"), Options.DestinationPackagePath, GEditorPerProjectIni);
}

void SUAssetImporterDialog::SaveOptions() const
{
    if (!GConfig)
    {
        return;
    }

    const TCHAR* Section = TEXT("UAssetImporter.ImportOptions");
    GConfig->SetBool(Section, TEXT("AbortIfDependencyMissing"), Options.bAbortIfDependencyMissing, GEditorPerProjectIni);
    GConfig->SetBool(Section, TEXT("IgnoreSoftReferenceErrors"), Options.bIgnoreSoftReferenceErrors, GEditorPerProjectIni);
    GConfig->SetBool(Section, TEXT("OverwriteExistingAssets"), Options.bOverwriteExistingAssets, GEditorPerProjectIni);
    GConfig->SetBool(Section, TEXT("RollbackOnFailure"), Options.bRollbackOnFailure, GEditorPerProjectIni);
    GConfig->SetBool(Section, TEXT("SyncImportedAssets"), Options.bSyncImportedAssets, GEditorPerProjectIni);
    GConfig->SetBool(Section, TEXT("LoadImportedRootAssets"), Options.bLoadImportedRootAssets, GEditorPerProjectIni);
    GConfig->SetInt(Section, TEXT("DependencyPlacementMode"), static_cast<int32>(Options.DependencyPlacement), GEditorPerProjectIni);
    GConfig->SetInt(Section, TEXT("DestinationMode"), static_cast<int32>(Options.DestinationMode), GEditorPerProjectIni);
    GConfig->SetString(Section, TEXT("DestinationPackagePath"), *Options.DestinationPackagePath, GEditorPerProjectIni);
    GConfig->Flush(false, GEditorPerProjectIni);
}

FReply SUAssetImporterDialog::StartImport()
{
    if (bIsImporting || !Service.IsValid() || GetSelectedCount() <= 0)
    {
        return FReply::Handled();
    }

    bIsImporting = true;
    Options.bClearImportSourceData = true;
    Options.SelectedDependencyPackageNames.Reset();
    for (const FDependencyTreeNodePtr& Root : DependencyTreeRoots)
    {
        CollectSelectedDependencyPackages(Root, Options.SelectedDependencyPackageNames);
    }
    Options.ExcludedDependencyPackageNames.Reset();
    for (const FString& PackageName : ExcludedDependencyPackageNames)
    {
        Options.ExcludedDependencyPackageNames.Add(PackageName);
    }
    if (Options.DestinationMode == EUAssetImporterDestinationMode::PreserveOriginalPaths)
    {
        Options.DependencyPlacement = EUAssetImporterDependencyPlacement::KeepOriginalPaths;
    }
    SaveOptions();
    ImportResult = Service->Import(Items, Options);
    bImportCompleted = true;
    Service->Cleanup();

    if (OnFinished.IsBound())
    {
        OnFinished.Execute();
    }
    return FReply::Handled();
}

FReply SUAssetImporterDialog::CancelImport()
{
    if (bIsImporting)
    {
        return FReply::Handled();
    }
    if (Service.IsValid())
    {
        Service->Cleanup();
    }
    if (OnFinished.IsBound())
    {
        OnFinished.Execute();
    }
    return FReply::Handled();
}

int32 SUAssetImporterDialog::GetSelectedCount() const
{
    int32 Count = 0;
    for (const TSharedPtr<FUAssetImporterAssetItem>& Item : Items)
    {
        if (Item.IsValid() && Item->bSelected)
        {
            ++Count;
        }
    }
    return Count;
}

FText SUAssetImporterDialog::GetSelectedCountText() const
{
    return FText::Format(LOCTEXT("SelectedCountFormat", "{0} selected"), FText::AsNumber(GetSelectedCount()));
}

#undef LOCTEXT_NAMESPACE
