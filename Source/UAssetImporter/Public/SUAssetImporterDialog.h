#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Views/STreeView.h"
#include "UAssetImporterService.h"

struct FSlateBrush;
class SScrollBox;
class SComboButton;
class STableViewBase;

struct FUAssetImporterDependencyTreeNode
{
    FString DisplayName;
    FString Tooltip;
    FString StatusText;
    FString PackageName;
    const FSlateBrush* IconBrush = nullptr;
    bool bIsFolder = false;
    bool bMissing = false;
    bool bIsSelectable = false;
    bool bIsSelected = true;
    bool bAlreadyInProject = false;
    TArray<TSharedPtr<FUAssetImporterDependencyTreeNode>> Children;
};

class SUAssetImporterDialog : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SUAssetImporterDialog) {}
        SLATE_ARGUMENT(TSharedPtr<FUAssetImporterService>, Service)
        SLATE_ARGUMENT(TArray<TSharedPtr<FUAssetImporterAssetItem>>, Items)
        SLATE_ARGUMENT(FString, InitialDestinationPackagePath)
        SLATE_ARGUMENT(TArray<FString>, BatchAlreadyProcessedPackageNames)
        SLATE_EVENT(FSimpleDelegate, OnFinished)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);

    const FUAssetImporterResult& GetImportResult() const;
    const FUAssetImporterOptions& GetOptions() const;
    bool DidCompleteImport() const;

private:
    using FDependencyTreeNodePtr = TSharedPtr<FUAssetImporterDependencyTreeNode>;

    TSharedRef<SWidget> MakeAssetRow(const TSharedPtr<FUAssetImporterAssetItem>& Item);
    TSharedRef<SWidget> MakeDestinationMenu();
    TSharedRef<SWidget> MakeDependencyPlacementMenu();
    TSharedRef<ITableRow> GenerateDependencyTreeRow(FDependencyTreeNodePtr Item, const TSharedRef<STableViewBase>& OwnerTable);
    void GetDependencyTreeChildren(FDependencyTreeNodePtr Item, TArray<FDependencyTreeNodePtr>& OutChildren) const;
    void RefreshAssetRows();
    void RebuildDependencyTree();
    void SetDependencyNodeSelection(FDependencyTreeNodePtr Item, bool bSelected);
    void SetDependencyNodeSelectionRecursive(FDependencyTreeNodePtr Item, bool bSelected);
    void CountSelectableDependencyNodes(FDependencyTreeNodePtr Item, int32& OutTotal, int32& OutSelected) const;
    bool HasSelectableDependency(FDependencyTreeNodePtr Item) const;
    void SelectAllDependencies(bool bSelected);
    void UpdateDependencySelectionCounts();
    void CollectSelectedDependencyPackages(FDependencyTreeNodePtr Item, TArray<FString>& OutPackageNames) const;
    ECheckBoxState GetDependencyNodeCheckState(FDependencyTreeNodePtr Item) const;
    void LoadOptions();
    void SaveOptions() const;
    void OpenDestinationPicker();
    void SelectDestinationMode(EUAssetImporterDestinationMode Mode);
    void SelectDependencyPlacement(EUAssetImporterDependencyPlacement Placement);
    FReply StartImport();
    FReply CancelImport();
    int32 GetSelectedCount() const;
    FText GetSelectedCountText() const;
    FText GetDependencyCountText() const;
    FText GetDestinationLabel() const;
    FText GetDependencyPlacementLabel() const;

    TSharedPtr<FUAssetImporterService> Service;
    TArray<TSharedPtr<FUAssetImporterAssetItem>> Items;
    TSharedPtr<SScrollBox> AssetList;
    TSharedPtr<SComboButton> DestinationComboButton;
    TSharedPtr<SComboButton> DependencyPlacementComboButton;
    TSharedPtr<STreeView<FDependencyTreeNodePtr>> DependencyTreeView;
    TArray<FDependencyTreeNodePtr> DependencyTreeRoots;
    int32 DependencyCount = 0;
    int32 DependencyImportableCount = 0;
    int32 DependencySelectedCount = 0;
    int32 DependencyReusedCount = 0;
    int32 DependencyMissingCount = 0;
    TSet<FString> ExcludedDependencyPackageNames;
    TSharedPtr<FSlateBrush> PreviewBackgroundBrush;
    TArray<TSharedPtr<FSlateBrush>> PreviewBrushes;
    FSimpleDelegate OnFinished;
    FUAssetImporterOptions Options;
    FUAssetImporterResult ImportResult;
    FString InitialDestinationPackagePath;
    bool bIsImporting = false;
    bool bImportCompleted = false;
};
