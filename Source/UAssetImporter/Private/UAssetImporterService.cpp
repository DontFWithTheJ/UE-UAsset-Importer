#include "UAssetImporterService.h"

#include "UAssetImporterModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AutoReimport/AutoReimportManager.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/Texture2D.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "ImageCore.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Logging/MessageLog.h"
#include "ObjectTools.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ObjectThumbnail.h"
#include "Misc/ScopedSlowTask.h"
#include "UObject/LinkerInstancingContext.h"
#include "UObject/Package.h"
#include "UObject/CoreRedirects.h"
#include "Misc/ScopeExit.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "MaterialShaderPrecompileMode.h"
#include "ShaderCompiler.h"
#include "RenderingThread.h"

namespace UAssetImporterPrivate
{
    static const FName MessageLogName(TEXT("UAssetImporter"));

    static FString NormalizePackageName(FString PackageName)
    {
        PackageName.ReplaceInline(TEXT("\\"), TEXT("/"));
        while (PackageName.EndsWith(TEXT("/")))
        {
            PackageName.LeftChopInline(1);
        }
        return PackageName;
    }

    static FString EnsureTrailingSlash(FString Path)
    {
        Path.ReplaceInline(TEXT("\\"), TEXT("/"));
        if (!Path.EndsWith(TEXT("/")))
        {
            Path += TEXT("/");
        }
        return Path;
    }

    static bool IsNativeOrEnginePackage(const FString& PackageName)
    {
        return PackageName.StartsWith(TEXT("/Script/"), ESearchCase::CaseSensitive)
            || PackageName.StartsWith(TEXT("/Memory/"), ESearchCase::CaseSensitive)
            || PackageName.StartsWith(TEXT("/Temp/"), ESearchCase::CaseSensitive)
            || PackageName.StartsWith(TEXT("/Engine/"), ESearchCase::CaseSensitive);
    }

    static bool IsInvalidReimportSource(const FString& Filename)
    {
        if (Filename.IsEmpty())
        {
            return true;
        }

        const FString Extension = FPaths::GetExtension(Filename);
        if (Extension.Equals(TEXT("uasset"), ESearchCase::IgnoreCase)
            || Extension.Equals(TEXT("mhpkg"), ESearchCase::IgnoreCase))
        {
            return true;
        }

        return !FPaths::FileExists(Filename);
    }

    static void IgnoreAutoReimportChange(const FString& Filename, const bool bExistedBefore)
    {
        FString FullPath = FPaths::ConvertRelativePathToFull(Filename);
        FPaths::NormalizeFilename(FullPath);

        for (TObjectIterator<UAutoReimportManager> It; It; ++It)
        {
            UAutoReimportManager* Manager = *It;
            if (!IsValid(Manager))
            {
                continue;
            }

            if (bExistedBefore)
            {
                Manager->IgnoreFileModification(FullPath);
            }
            else
            {
                Manager->IgnoreNewFile(FullPath);
            }
        }
    }

    static void IgnoreAutoReimportDelete(const FString& Filename)
    {
        FString FullPath = FPaths::ConvertRelativePathToFull(Filename);
        FPaths::NormalizeFilename(FullPath);

        for (TObjectIterator<UAutoReimportManager> It; It; ++It)
        {
            UAutoReimportManager* Manager = *It;
            if (IsValid(Manager))
            {
                Manager->IgnoreDeletedFile(FullPath);
            }
        }
    }

    static void IgnoreAutoReimportWrite(const FString& Filename)
    {
        FString FullPath = FPaths::ConvertRelativePathToFull(Filename);
        FPaths::NormalizeFilename(FullPath);

        for (TObjectIterator<UAutoReimportManager> It; It; ++It)
        {
            UAutoReimportManager* Manager = *It;
            if (IsValid(Manager))
            {
                Manager->IgnoreNewFile(FullPath);
                Manager->IgnoreFileModification(FullPath);
            }
        }
    }

    static bool ClearLegacySourcePaths(UObject* Object, const bool bClearAllSources)
    {
        if (!Object)
        {
            return false;
        }

        bool bChanged = false;
        for (TFieldIterator<FStrProperty> It(Object->GetClass()); It; ++It)
        {
            FStrProperty* Property = *It;
            if (!Property)
            {
                continue;
            }

            const FString PropertyName = Property->GetName();
            if (!PropertyName.Equals(TEXT("SourceFilePath"), ESearchCase::IgnoreCase)
                && !PropertyName.Equals(TEXT("SourceFilePath_DEPRECATED"), ESearchCase::IgnoreCase)
                && !PropertyName.Equals(TEXT("ReimportSourceFile"), ESearchCase::IgnoreCase))
            {
                continue;
            }

            const FString CurrentValue = Property->GetPropertyValue_InContainer(Object);
            if (CurrentValue.IsEmpty())
            {
                continue;
            }

            if (bClearAllSources || IsInvalidReimportSource(CurrentValue))
            {
                Property->SetPropertyValue_InContainer(Object, FString());
                bChanged = true;
            }
        }

        return bChanged;
    }

    static bool SanitizeImportDataObject(
        UObject* Asset,
        bool& bOutChanged,
        const bool bClearAllSources)
    {
        if (!Asset)
        {
            return false;
        }

        bool bChanged = false;
        for (TFieldIterator<FObjectPropertyBase> It(Asset->GetClass()); It; ++It)
        {
            FObjectPropertyBase* ObjectProperty = *It;
            if (!ObjectProperty || !ObjectProperty->PropertyClass->IsChildOf(UAssetImportData::StaticClass()))
            {
                continue;
            }

            UAssetImportData* ImportData = Cast<UAssetImportData>(
                ObjectProperty->GetObjectPropertyValue_InContainer(Asset));
            if (!ImportData)
            {
                continue;
            }

            const TArray<FAssetImportInfo::FSourceFile>& SourceFiles = ImportData->GetSourceData().SourceFiles;
            const TArray<FString> ResolvedFilenames = ImportData->ExtractFilenames();
            TArray<FAssetImportInfo::FSourceFile> ValidSourceFiles;
            ValidSourceFiles.Reserve(SourceFiles.Num());

            for (int32 Index = 0; Index < SourceFiles.Num(); ++Index)
            {
                const FString RawFilename = SourceFiles[Index].RelativeFilename;
                const FString ResolvedFilename = ResolvedFilenames.IsValidIndex(Index)
                    ? ResolvedFilenames[Index]
                    : RawFilename;

                const bool bRawPathIsPackage =
                    FPaths::GetExtension(RawFilename).Equals(TEXT("uasset"), ESearchCase::IgnoreCase)
                    || FPaths::GetExtension(RawFilename).Equals(TEXT("mhpkg"), ESearchCase::IgnoreCase);

                if (!bClearAllSources && !bRawPathIsPackage && !IsInvalidReimportSource(ResolvedFilename))
                {
                    ValidSourceFiles.Add(SourceFiles[Index]);
                }
            }

            if (ValidSourceFiles.Num() != SourceFiles.Num())
            {
                ImportData->SetSourceFiles(MoveTemp(ValidSourceFiles));
                bChanged = true;
            }

            bChanged |= ClearLegacySourcePaths(ImportData, bClearAllSources);
        }

        bChanged |= ClearLegacySourcePaths(Asset, bClearAllSources);

        if (bChanged)
        {
            Asset->MarkPackageDirty();
            if (UPackage* Package = Asset->GetOutermost())
            {
                Package->MarkPackageDirty();
            }
        }

        bOutChanged |= bChanged;
        return bChanged;
    }

    static bool SavePackageWithoutPrompt(UPackage* Package, const FString& Filename, FString& OutError)
    {
        if (!Package || Filename.IsEmpty())
        {
            OutError = TEXT("Cannot save an invalid package or filename.");
            return false;
        }

        IgnoreAutoReimportWrite(Filename);

        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        SaveArgs.SaveFlags = SAVE_NoError;

        if (!UPackage::SavePackage(Package, nullptr, *Filename, SaveArgs))
        {
            OutError = FString::Printf(
                TEXT("Failed to save package '%s' to '%s'. Check file permissions and source control."),
                *Package->GetName(),
                *Filename);
            return false;
        }

        IgnoreAutoReimportWrite(Filename);
        Package->SetDirtyFlag(false);
        return true;
    }

    static FString GetPackagePathRelativeToMount(const FString& PackageName)
    {
        FString Normalized = NormalizePackageName(PackageName);
        if (Normalized.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive))
        {
            return Normalized.Mid(6);
        }

        if (Normalized.StartsWith(TEXT("/"), ESearchCase::CaseSensitive))
        {
            const int32 MountEnd = Normalized.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1);
            if (MountEnd != INDEX_NONE)
            {
                const FString MountName = Normalized.Mid(1, MountEnd - 1);
                const FString Relative = Normalized.Mid(MountEnd + 1);
                return MountName / Relative;
            }
        }

        return FPaths::GetCleanFilename(Normalized);
    }

    static FString MakeObjectPath(const FString& PackageName, const FString& ObjectName)
    {
        return FString::Printf(TEXT("%s.%s"), *PackageName, *ObjectName);
    }

    static bool IsPackageNameAlreadyProcessed(const TArray<FString>& Names, const FString& Candidate)
    {
        return Names.ContainsByPredicate([&Candidate](const FString& Name)
        {
            return Name.Equals(Candidate, ESearchCase::IgnoreCase);
        });
    }

    static UTexture2D* CreateTextureFromThumbnail(const FObjectThumbnail& Thumbnail)
    {
        if (!Thumbnail.HasValidImageData())
        {
            return nullptr;
        }

        const int32 Width = Thumbnail.GetImageWidth();
        const int32 Height = Thumbnail.GetImageHeight();
        const TArray<uint8>& Pixels = Thumbnail.GetUncompressedImageData();
        const int64 ExpectedByteCount = static_cast<int64>(Width) * Height * 4;
        if (Width <= 0 || Height <= 0 || ExpectedByteCount <= 0 || Pixels.Num() != ExpectedByteCount)
        {
            return nullptr;
        }

        UTexture2D* PreviewTexture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
        if (!PreviewTexture || !PreviewTexture->GetPlatformData() || PreviewTexture->GetPlatformData()->Mips.IsEmpty())
        {
            return nullptr;
        }

        FTexture2DMipMap& Mip = PreviewTexture->GetPlatformData()->Mips[0];
        void* TextureData = Mip.BulkData.Lock(LOCK_READ_WRITE);
        if (!TextureData)
        {
            return nullptr;
        }

        FMemory::Memcpy(TextureData, Pixels.GetData(), Pixels.Num());
        Mip.BulkData.Unlock();
        PreviewTexture->SRGB = true;
        PreviewTexture->NeverStream = true;
        PreviewTexture->UpdateResource();
        return PreviewTexture;
    }

    static UTexture2D* CreateTextureSourcePreview(UTexture2D* SourceTexture)
    {
        if (!IsValid(SourceTexture) || !SourceTexture->Source.IsValid())
        {
            return nullptr;
        }

        FImage SourceImage;
        if (!SourceTexture->Source.GetMipImage(SourceImage, 0))
        {
            return nullptr;
        }

        const EGammaSpace GammaSpace = SourceTexture->SRGB ? EGammaSpace::sRGB : EGammaSpace::Linear;
        SourceImage.ChangeFormat(ERawImageFormat::BGRA8, GammaSpace);

        if (SourceImage.SizeX <= 0 || SourceImage.SizeY <= 0)
        {
            return nullptr;
        }

        constexpr int32 MaxPreviewDimension = 256;
        const float Scale = FMath::Min(
            1.0f,
            FMath::Min(
                static_cast<float>(MaxPreviewDimension) / static_cast<float>(SourceImage.SizeX),
                static_cast<float>(MaxPreviewDimension) / static_cast<float>(SourceImage.SizeY)));

        if (Scale < 1.0f)
        {
            const int32 PreviewWidth = FMath::Max(1, FMath::RoundToInt(static_cast<float>(SourceImage.SizeX) * Scale));
            const int32 PreviewHeight = FMath::Max(1, FMath::RoundToInt(static_cast<float>(SourceImage.SizeY) * Scale));
            FImageCore::ResizeImageInPlace(
                SourceImage,
                PreviewWidth,
                PreviewHeight,
                ERawImageFormat::BGRA8,
                GammaSpace,
                FImageCore::EResizeImageFilter::Default);
        }

        const int32 Width = SourceImage.SizeX;
        const int32 Height = SourceImage.SizeY;
        const int64 ExpectedByteCount = static_cast<int64>(Width) * Height * 4;
        if (Width <= 0 || Height <= 0 || ExpectedByteCount <= 0 || SourceImage.RawData.Num() != ExpectedByteCount)
        {
            return nullptr;
        }

        UTexture2D* PreviewTexture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
        if (!PreviewTexture || !PreviewTexture->GetPlatformData() || PreviewTexture->GetPlatformData()->Mips.IsEmpty())
        {
            return nullptr;
        }

        FTexture2DMipMap& Mip = PreviewTexture->GetPlatformData()->Mips[0];
        void* TextureData = Mip.BulkData.Lock(LOCK_READ_WRITE);
        if (!TextureData)
        {
            return nullptr;
        }

        FMemory::Memcpy(TextureData, SourceImage.RawData.GetData(), static_cast<SIZE_T>(ExpectedByteCount));
        Mip.BulkData.Unlock();

        PreviewTexture->SRGB = SourceTexture->SRGB;
        PreviewTexture->NeverStream = true;
        PreviewTexture->UpdateResource();
        return PreviewTexture;
    }

    static void EnsurePreviewMaterialsReady(UObject* PreviewObject)
    {
        if (!IsValid(PreviewObject))
        {
            return;
        }

        TSet<UMaterialInterface*> Materials;

        if (UMaterialInterface* Material = Cast<UMaterialInterface>(PreviewObject))
        {
            Materials.Add(Material);
        }

        if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(PreviewObject))
        {
            for (int32 MaterialIndex = 0; MaterialIndex < StaticMesh->GetStaticMaterials().Num(); ++MaterialIndex)
            {
                if (UMaterialInterface* Material = StaticMesh->GetMaterial(MaterialIndex))
                {
                    Materials.Add(Material);
                }
            }
        }

        for (UMaterialInterface* Material : Materials)
        {
            if (IsValid(Material))
            {
                Material->EnsureIsComplete();
            }
        }
    }

    static UTexture2D* CreateRenderedPreviewTexture(UObject* PreviewObject)
    {
        if (!IsValid(PreviewObject))
        {
            return nullptr;
        }

        if (UTexture2D* SourceTexture = Cast<UTexture2D>(PreviewObject))
        {
            if (UTexture2D* SourcePreview = CreateTextureSourcePreview(SourceTexture))
            {
                return SourcePreview;
            }
        }

        EnsurePreviewMaterialsReady(PreviewObject);

        FObjectThumbnail Thumbnail;
        ThumbnailTools::RenderThumbnail(
            PreviewObject,
            256,
            256,
            ThumbnailTools::EThumbnailTextureFlushMode::AlwaysFlush,
            nullptr,
            &Thumbnail);

        if (!Thumbnail.HasValidImageData())
        {
            return nullptr;
        }

        return CreateTextureFromThumbnail(Thumbnail);
    }

    static bool ReleaseEmptyInMemoryPackage(const FString& PackageName, FString& OutProblem)
    {
        UPackage* Package = FindPackage(nullptr, *PackageName);
        if (!Package)
        {
            return true;
        }

        FString ExistingFilename;
        if (FPackageName::DoesPackageExist(PackageName, &ExistingFilename, false)
            && !ExistingFilename.IsEmpty()
            && FPaths::FileExists(ExistingFilename))
        {
            return true;
        }

        TArray<UObject*> Objects;
        GetObjectsWithOuter(Package, Objects, false);
        for (UObject* Object : Objects)
        {
            if (!Object || Object->GetClass()->GetFName() == FName(TEXT("MetaData")) || Object->HasAnyFlags(RF_Transient))
            {
                continue;
            }

            OutProblem = FString::Printf(
                TEXT("The destination package '%s' is loaded in memory without a package file and contains object '%s'. It cannot be replaced safely while loaded."),
                *PackageName,
                *Object->GetPathName());
            return false;
        }

        const FString DetachedName = FString::Printf(
            TEXT("UAssetImporterPlaceholder_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));

        if (!Package->Rename(*DetachedName, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional))
        {
            OutProblem = FString::Printf(
                TEXT("Could not detach the empty in-memory placeholder package '%s'. Close the editor and retry."),
                *PackageName);
            return false;
        }

        Package->ClearFlags(RF_Public | RF_Standalone | RF_Transactional);
        Package->SetDirtyFlag(false);
        return true;
    }
}

bool FUAssetImporterService::Prepare(
    const TArray<FString>& SelectedFiles,
    TArray<TSharedPtr<FUAssetImporterAssetItem>>& OutItems,
    TArray<FString>& OutProblems)
{
    Cleanup();
    if (!SourceRoots.IsEmpty())
    {
        OutItems.Reset();
        OutProblems.Reset();
        OutProblems.Add(TEXT("A previous import session still has temporary source packages loaded. Close the import window and retry after Unreal releases those packages."));
        return false;
    }

    SourceRoots.Reset();
    OriginalPackageToFile.Reset();
    OriginalPackageToTemporaryPackage.Reset();
    TemporaryPackageToFile.Reset();
    TemporaryPackageToOriginalPackage.Reset();
    AssetDataByTemporaryPackage.Reset();
    HardDependenciesByTemporaryPackage.Reset();
    SoftDependenciesByTemporaryPackage.Reset();
    OutItems.Reset();
    OutProblems.Reset();

    if (SelectedFiles.IsEmpty())
    {
        OutProblems.Add(TEXT("No .uasset files were selected."));
        return false;
    }

    TArray<FString> NormalizedSelectedFiles;
    for (const FString& SelectedFile : SelectedFiles)
    {
        const FString FullPath = NormalizeFilePath(SelectedFile);
        if (!FPaths::FileExists(FullPath))
        {
            OutProblems.Add(FString::Printf(TEXT("The source file does not exist: %s"), *FullPath));
            continue;
        }

        if (!FPaths::GetExtension(FullPath).Equals(TEXT("uasset"), ESearchCase::IgnoreCase))
        {
            OutProblems.Add(FString::Printf(TEXT("Only .uasset files are supported: %s"), *FullPath));
            continue;
        }

        NormalizedSelectedFiles.AddUnique(FullPath);
    }

    if (NormalizedSelectedFiles.IsEmpty() || !OutProblems.IsEmpty())
    {
        return false;
    }

    TMap<FString, int32> RootIndexByPath;
    for (const FString& SelectedFile : NormalizedSelectedFiles)
    {
        const FString ContentRoot = FindContentRoot(SelectedFile);
        const FString RootKey = ContentRoot.ToLower();
        if (RootIndexByPath.Contains(RootKey))
        {
            continue;
        }

        FSourceRoot Root;
        Root.ContentRoot = ContentRoot;
        Root.OriginalVirtualRoot = DetermineOriginalVirtualRoot(ContentRoot, Root.bIsPluginRoot);
        Root.TemporaryVirtualRoot = FString::Printf(
    TEXT("/UAssetImporterSource_%s"),
    *FGuid::NewGuid().ToString(EGuidFormats::Digits));

        RootIndexByPath.Add(RootKey, SourceRoots.Add(MoveTemp(Root)));
    }

    TSet<FString> RegisteredTemporaryMountPoints;

    auto RegisterSourceFileMount = [this, &RegisteredTemporaryMountPoints](const FString& SourceFile) -> bool
    {
        for (FSourceRoot& Root : SourceRoots)
        {
            const FString ContentRootPrefix =
                UAssetImporterPrivate::EnsureTrailingSlash(Root.ContentRoot);

            if (!SourceFile.StartsWith(ContentRootPrefix, ESearchCase::IgnoreCase))
            {
                continue;
            }

            const FString VirtualRoot =
                UAssetImporterPrivate::EnsureTrailingSlash(Root.TemporaryVirtualRoot);

            const FString PhysicalRoot =
                UAssetImporterPrivate::EnsureTrailingSlash(
                    NormalizeFilePath(Root.ContentRoot));

            const FString MountKey = VirtualRoot.ToLower();

            if (!RegisteredTemporaryMountPoints.Contains(MountKey))
            {
                FPackageName::RegisterMountPoint(VirtualRoot, PhysicalRoot);
                RegisteredTemporaryMountPoints.Add(MountKey);
            }

            Root.bMountRegistered = true;
            return true;
        }

        return false;
    };

    for (const FString& SelectedFile : NormalizedSelectedFiles)
    {
        const FString ContentRoot = FindContentRoot(SelectedFile);
        const int32* RootIndex = RootIndexByPath.Find(ContentRoot.ToLower());
        if (!RootIndex || !SourceRoots.IsValidIndex(*RootIndex))
        {
            OutProblems.Add(FString::Printf(TEXT("Could not create a source mount for '%s'."), *SelectedFile));
            continue;
        }

        FSourceRoot& Root = SourceRoots[*RootIndex];
        Root.Files.AddUnique(SelectedFile);
        if (!RegisterSourceFileMount(SelectedFile))
        {
            OutProblems.Add(FString::Printf(TEXT("Could not register a temporary mount for source file '%s'."), *SelectedFile));
            continue;
        }

        const FString OriginalPackage = MakePackageName(Root.OriginalVirtualRoot, Root.ContentRoot, SelectedFile);
        const FString TemporaryPackage = MakeTemporaryPackageName(Root, SelectedFile);

        TemporaryPackageToFile.Add(TemporaryPackage, SelectedFile);
        TemporaryPackageToOriginalPackage.Add(TemporaryPackage, OriginalPackage);

        if (const FString* ExistingSource = OriginalPackageToFile.Find(OriginalPackage))
        {
            if (!ExistingSource->Equals(SelectedFile, ESearchCase::IgnoreCase))
            {
                OutProblems.Add(FString::Printf(
                    TEXT("Two different source files map to the same package path '%s': '%s' and '%s'. Import them separately."),
                    *OriginalPackage,
                    **ExistingSource,
                    *SelectedFile));
            }
        }
        else
        {
            OriginalPackageToFile.Add(OriginalPackage, SelectedFile);
            OriginalPackageToTemporaryPackage.Add(OriginalPackage, TemporaryPackage);
        }
    }

    if (!OutProblems.IsEmpty())
    {
        return false;
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    auto CacheAssetData = [this, &AssetRegistry](const FString& TemporaryPackage)
    {
        TArray<FAssetData> FoundAssets;
        AssetRegistry.GetAssetsByPackageName(FName(*TemporaryPackage), FoundAssets);
        for (const FAssetData& AssetData : FoundAssets)
        {
            AssetDataByTemporaryPackage.Add(AssetData.PackageName.ToString(), AssetData);
        }
        return AssetDataByTemporaryPackage.Contains(TemporaryPackage);
    };

    TArray<FString> DependencyMetadataFailures;
    auto RegisterDependencyPackage = [this, &AssetRegistry, &CacheAssetData, &DependencyMetadataFailures, &RegisterSourceFileMount](const FString& DependencyPackage) -> FString
    {
        FString OriginalDependencyPackage = UAssetImporterPrivate::NormalizePackageName(DependencyPackage);
        for (const FSourceRoot& Root : SourceRoots)
        {
            const FString TemporaryPrefix = UAssetImporterPrivate::EnsureTrailingSlash(Root.TemporaryVirtualRoot);
            if (OriginalDependencyPackage.StartsWith(TemporaryPrefix, ESearchCase::IgnoreCase))
            {
                const FString RelativePackagePath = OriginalDependencyPackage.Mid(TemporaryPrefix.Len());
                OriginalDependencyPackage = UAssetImporterPrivate::NormalizePackageName(Root.OriginalVirtualRoot + TEXT("/") + RelativePackagePath);
                break;
            }
        }

        const FString SourceFile = FindSourceFileForPackage(OriginalDependencyPackage);
        const FString TemporaryPackage = FindTemporaryPackageForPackage(OriginalDependencyPackage);
        if (SourceFile.IsEmpty() || TemporaryPackage.IsEmpty() || !FPaths::FileExists(SourceFile))
        {
            return FString();
        }

        OriginalPackageToFile.Add(OriginalDependencyPackage, SourceFile);
        OriginalPackageToTemporaryPackage.Add(OriginalDependencyPackage, TemporaryPackage);
        TemporaryPackageToFile.Add(TemporaryPackage, SourceFile);
        TemporaryPackageToOriginalPackage.Add(TemporaryPackage, OriginalDependencyPackage);

        for (FSourceRoot& Root : SourceRoots)
        {
            if (SourceFile.StartsWith(UAssetImporterPrivate::EnsureTrailingSlash(Root.ContentRoot), ESearchCase::IgnoreCase))
            {
                Root.Files.AddUnique(SourceFile);
                if (!RegisterSourceFileMount(SourceFile))
                {
                    return FString();
                }
                break;
            }
        }

        if (!AssetDataByTemporaryPackage.Contains(TemporaryPackage))
        {
            TArray<FString> FilesToScan = { SourceFile };
            AssetRegistry.ScanFilesSynchronous(FilesToScan, true);
            if (!CacheAssetData(TemporaryPackage))
            {
                DependencyMetadataFailures.AddUnique(OriginalDependencyPackage);
            }
        }

        return TemporaryPackage;
    };

    for (const FString& SelectedFile : NormalizedSelectedFiles)
    {
        FString TemporaryPackage;
        for (const FSourceRoot& Root : SourceRoots)
        {
            if (SelectedFile.StartsWith(UAssetImporterPrivate::EnsureTrailingSlash(Root.ContentRoot), ESearchCase::IgnoreCase))
            {
                TemporaryPackage = MakeTemporaryPackageName(Root, SelectedFile);
                break;
            }
        }

        if (TemporaryPackage.IsEmpty())
        {
            OutProblems.Add(FString::Printf(TEXT("Could not map the selected file to a temporary package: %s"), *SelectedFile));
            continue;
        }

        TArray<FString> FilesToScan = { SelectedFile };
        AssetRegistry.ScanFilesSynchronous(FilesToScan, true);
        if (!CacheAssetData(TemporaryPackage))
        {
            OutProblems.Add(FString::Printf(TEXT("Unreal could not read package metadata for '%s'. The file may be cooked, corrupt, or incompatible with this engine version."), *SelectedFile));
        }
    }

    if (!OutProblems.IsEmpty())
    {
        return false;
    }

    TArray<FString> PendingPackages;
    for (const FString& SelectedFile : NormalizedSelectedFiles)
    {
        for (const FSourceRoot& Root : SourceRoots)
        {
            if (SelectedFile.StartsWith(UAssetImporterPrivate::EnsureTrailingSlash(Root.ContentRoot), ESearchCase::IgnoreCase))
            {
                PendingPackages.AddUnique(MakeTemporaryPackageName(Root, SelectedFile));
                break;
            }
        }
    }

    auto GetOriginalDependencyPackage = [this](FString DependencyPackage)
    {
        DependencyPackage = UAssetImporterPrivate::NormalizePackageName(MoveTemp(DependencyPackage));
        for (const FSourceRoot& Root : SourceRoots)
        {
            const FString TemporaryPrefix = UAssetImporterPrivate::EnsureTrailingSlash(Root.TemporaryVirtualRoot);
            if (DependencyPackage.StartsWith(TemporaryPrefix, ESearchCase::IgnoreCase))
            {
                const FString RelativePackagePath = DependencyPackage.Mid(TemporaryPrefix.Len());
                return UAssetImporterPrivate::NormalizePackageName(Root.OriginalVirtualRoot + TEXT("/") + RelativePackagePath);
            }
        }
        return DependencyPackage;
    };

    TSet<FString> VisitedPackages;
    FScopedSlowTask ScanProgress(0.0f, NSLOCTEXT("UAssetImporter", "ScanMetadata", "Reading package dependencies..."));
    ScanProgress.MakeDialogDelayed(0.5f, true);
    while (!PendingPackages.IsEmpty())
    {
        ScanProgress.EnterProgressFrame(0.0f);
        if (ScanProgress.ShouldCancel())
        {
            OutProblems.Add(TEXT("Import preparation canceled."));
            return false;
        }
        const FString TemporaryPackage = PendingPackages.Pop(EAllowShrinking::No);
        if (VisitedPackages.Contains(TemporaryPackage))
        {
            continue;
        }
        VisitedPackages.Add(TemporaryPackage);

        TArray<FName> Dependencies;
        UE::AssetRegistry::FDependencyQuery HardQuery;
        HardQuery.Required = UE::AssetRegistry::EDependencyProperty::Hard;
        AssetRegistry.GetDependencies(FName(*TemporaryPackage), Dependencies, UE::AssetRegistry::EDependencyCategory::Package, HardQuery);

        TArray<FName> SoftDependencies;
        UE::AssetRegistry::FDependencyQuery SoftQuery;
        SoftQuery.Excluded = UE::AssetRegistry::EDependencyProperty::Hard;
        AssetRegistry.GetDependencies(FName(*TemporaryPackage), SoftDependencies, UE::AssetRegistry::EDependencyCategory::Package, SoftQuery);

        TArray<FString>& CachedHardDependencies = HardDependenciesByTemporaryPackage.FindOrAdd(TemporaryPackage);
        TArray<FString>& CachedSoftDependencies = SoftDependenciesByTemporaryPackage.FindOrAdd(TemporaryPackage);

        for (const FName& DependencyName : Dependencies)
        {
            const FString DependencyPackage = GetOriginalDependencyPackage(DependencyName.ToString());
            if (DependencyPackage.IsEmpty() || UAssetImporterPrivate::IsNativeOrEnginePackage(DependencyPackage))
            {
                continue;
            }

            CachedHardDependencies.AddUnique(DependencyPackage);

            const FString TemporaryDependency = RegisterDependencyPackage(DependencyPackage);
            if (!TemporaryDependency.IsEmpty() && AssetDataByTemporaryPackage.Contains(TemporaryDependency))
            {
                PendingPackages.AddUnique(TemporaryDependency);
            }
        }

        for (const FName& DependencyName : SoftDependencies)
        {
            const FString DependencyPackage = GetOriginalDependencyPackage(DependencyName.ToString());
            if (DependencyPackage.IsEmpty() || UAssetImporterPrivate::IsNativeOrEnginePackage(DependencyPackage))
            {
                continue;
            }

            CachedSoftDependencies.AddUnique(DependencyPackage);

            const FString TemporaryDependency = RegisterDependencyPackage(DependencyPackage);
            if (!TemporaryDependency.IsEmpty() && AssetDataByTemporaryPackage.Contains(TemporaryDependency))
            {
                PendingPackages.AddUnique(TemporaryDependency);
            }
        }
    }

    if (!DependencyMetadataFailures.IsEmpty())
    {
        UE_LOG(LogUAssetImporter, Warning,
            TEXT("Asset Registry metadata could not be read for %d dependency package(s). First: %s"),
            DependencyMetadataFailures.Num(), *DependencyMetadataFailures[0]);
    }

for (const FString& SelectedFile : NormalizedSelectedFiles)
{
    FString TemporaryPackage;
    FString OriginalPackage;
    FString RootForFile;

    for (const FSourceRoot& Root : SourceRoots)
    {
        if (SelectedFile.StartsWith(
            UAssetImporterPrivate::EnsureTrailingSlash(Root.ContentRoot),
            ESearchCase::IgnoreCase))
        {
            RootForFile = Root.ContentRoot;
            TemporaryPackage = MakeTemporaryPackageName(Root, SelectedFile);
            OriginalPackage = MakePackageName(
                Root.OriginalVirtualRoot,
                Root.ContentRoot,
                SelectedFile);
            break;
        }
    }

    if (TemporaryPackage.IsEmpty())
    {
        OutProblems.Add(FString::Printf(
            TEXT("Could not map the selected file to a package path: %s"),
            *SelectedFile));
        continue;
    }

    TSharedPtr<FUAssetImporterAssetItem> Item =
        MakeShared<FUAssetImporterAssetItem>();

    Item->SourceFile = SelectedFile;
    Item->ContentRoot = RootForFile;
    Item->OriginalPackageName = OriginalPackage;
    Item->TemporaryPackageName = TemporaryPackage;
    Item->AssetName = FPaths::GetBaseFilename(SelectedFile);
    Item->DisplayPath = OriginalPackage;
    Item->bSelected = true;
    Item->FileSize = IFileManager::Get().FileSize(*SelectedFile);

    if (const FAssetData* AssetData = AssetDataByTemporaryPackage.Find(TemporaryPackage))
    {
        Item->AssetData = *AssetData;
        Item->AssetClass = AssetData->AssetClassPath.GetAssetName().ToString();
        Item->AssetName = AssetData->AssetName.ToString();
        Item->bMetadataValid = true;

        FString SavedEngineVersion;

        if (AssetData->GetTagValue(
            FName(TEXT("SavedByEngineVersion")),
            SavedEngineVersion))
        {
            Item->SavedEngineVersion = SavedEngineVersion;
        }
    }
    else
    {
        Item->AssetClass = TEXT("Metadata unavailable");
        Item->bMetadataValid = false;

        OutProblems.Add(FString::Printf(
            TEXT("Unreal could not read package metadata for '%s'. The file may be cooked, corrupt, or incompatible with this engine version."),
            *SelectedFile));
    }

    OutItems.Add(Item);
}

    return OutProblems.IsEmpty() && !OutItems.IsEmpty();
}

void FUAssetImporterService::LoadSelectedPreviews(const TArray<TSharedPtr<FUAssetImporterAssetItem>>& Items)
{
    IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    FLinkerInstancingContext PreviewRemapContext(true);
    for (const TPair<FString, FString>& Mapping : OriginalPackageToTemporaryPackage)
    {
        PreviewRemapContext.AddPackageMapping(FName(*Mapping.Key), FName(*Mapping.Value));

        const FString OriginalObjectPath =
            Mapping.Key + TEXT(".") + FPackageName::GetShortName(Mapping.Key);

        const FString TemporaryObjectPath =
            Mapping.Value + TEXT(".") + FPackageName::GetShortName(Mapping.Value);

        PreviewRemapContext.AddPathMapping(
            FSoftObjectPath(OriginalObjectPath),
            FSoftObjectPath(TemporaryObjectPath));
    }

    TSet<FString> SelectedTemporaryPackages;

    for (const auto& Item : Items)
    {
        if (Item.IsValid() && Item->bSelected && Item->bMetadataValid && !Item->AssetClass.StartsWith(TEXT("Niagara"), ESearchCase::IgnoreCase))
        {
            SelectedTemporaryPackages.Add(Item->TemporaryPackageName);
        }
    }

    TArray<FString> OrderedPreviewDependencyPackages;
    TSet<FString> PreviewPackagesVisited;
    TSet<FString> PreviewPackagesBeingVisited;

    TFunction<void(const FString&)> VisitPreviewPackage;

    VisitPreviewPackage = [this, &AssetRegistry, &SelectedTemporaryPackages,
        &OrderedPreviewDependencyPackages, &PreviewPackagesVisited,
        &PreviewPackagesBeingVisited, &VisitPreviewPackage](const FString& PackageName)
    {
        if (PackageName.IsEmpty()
            || PreviewPackagesVisited.Contains(PackageName)
            || PreviewPackagesBeingVisited.Contains(PackageName))
        {
            return;
        }

        PreviewPackagesBeingVisited.Add(PackageName);

        TArray<FName> PackageDependencies;

        UE::AssetRegistry::FDependencyQuery HardQuery;
        HardQuery.Required = UE::AssetRegistry::EDependencyProperty::Hard;

        AssetRegistry.GetDependencies(
            FName(*PackageName),
            PackageDependencies,
            UE::AssetRegistry::EDependencyCategory::Package,
            HardQuery);

        TArray<FName> SoftDependencies;

        UE::AssetRegistry::FDependencyQuery SoftQuery;
        SoftQuery.Excluded = UE::AssetRegistry::EDependencyProperty::Hard;

        AssetRegistry.GetDependencies(
            FName(*PackageName),
            SoftDependencies,
            UE::AssetRegistry::EDependencyCategory::Package,
            SoftQuery);

        PackageDependencies.Append(SoftDependencies);

        for (const FName& DependencyName : PackageDependencies)
        {
            const FString DependencyPackage =
                UAssetImporterPrivate::NormalizePackageName(DependencyName.ToString());

            if (DependencyPackage.IsEmpty()
                || UAssetImporterPrivate::IsNativeOrEnginePackage(DependencyPackage))
            {
                continue;
            }

            const FString TemporaryDependency =
                FindTemporaryPackageForPackage(DependencyPackage);

            if (!TemporaryDependency.IsEmpty()
                && TemporaryPackageToFile.Contains(TemporaryDependency))
            {
                VisitPreviewPackage(TemporaryDependency);
            }
        }

        PreviewPackagesBeingVisited.Remove(PackageName);
        PreviewPackagesVisited.Add(PackageName);

        if (!SelectedTemporaryPackages.Contains(PackageName))
        {
            OrderedPreviewDependencyPackages.AddUnique(PackageName);
        }
    };

    for (const FString& SelectedPackageName : SelectedTemporaryPackages)
    {
        VisitPreviewPackage(SelectedPackageName);
    }

    TArray<FString> PreviewDependencyLoadFailures;

    for (const FString& DependencyPackageName : OrderedPreviewDependencyPackages)
    {
        if (FindPackage(nullptr, *DependencyPackageName))
        {
            continue;
        }

        if (!LoadPackage(
            nullptr,
            *DependencyPackageName,
            LOAD_None,
            nullptr,
            &PreviewRemapContext))
        {
            PreviewDependencyLoadFailures.AddUnique(DependencyPackageName);
        }
    }
    TArray<FString> PreviewRenderFailures;

    for (const TSharedPtr<FUAssetImporterAssetItem>& Item : Items)
    {
        if (!Item.IsValid()
            || !Item->bSelected
            || Item->PreviewTexture.IsValid()
            || !Item->bMetadataValid
            || Item->AssetClass.StartsWith(TEXT("Niagara"), ESearchCase::IgnoreCase))
        {
            continue;
        }

        UPackage* PreviewPackage = FindPackage(nullptr, *Item->TemporaryPackageName);
        if (!PreviewPackage)
        {
            PreviewPackage = LoadPackage(
                nullptr,
                *Item->TemporaryPackageName,
                LOAD_None,
                nullptr,
                &PreviewRemapContext);
        }

        if (!PreviewPackage)
        {
            PreviewRenderFailures.AddUnique(
                Item->AssetName + TEXT(" (package load failed)"));
            continue;
        }

        UObject* PreviewObject = FindObject<UObject>(
            PreviewPackage,
            *Item->AssetName);

        if (!PreviewObject)
        {
            PreviewRenderFailures.AddUnique(
                Item->AssetName + TEXT(" (asset object not found in loaded package)"));
            continue;
        }

        UE_LOG(
            LogUAssetImporter,
            Display,
            TEXT("Rendering live preview: Asset='%s', Class='%s', Package='%s'."),
            *PreviewObject->GetName(),
            *PreviewObject->GetClass()->GetName(),
            *PreviewPackage->GetName());

        if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(PreviewObject))
        {
            for (int32 MaterialIndex = 0;
                MaterialIndex < StaticMesh->GetStaticMaterials().Num();
                ++MaterialIndex)
            {
                UMaterialInterface* Material = StaticMesh->GetMaterial(MaterialIndex);

                UE_LOG(
                    LogUAssetImporter,
                    Display,
                    TEXT("Preview mesh material slot %d: Slot='%s', Material='%s'."),
                    MaterialIndex,
                    *StaticMesh->GetStaticMaterials()[MaterialIndex].MaterialSlotName.ToString(),
                    Material ? *Material->GetPathName() : TEXT("<null>"));
            }
        }

        Item->PreviewTexture.Reset(
            UAssetImporterPrivate::CreateRenderedPreviewTexture(PreviewObject));

        if (Item->PreviewTexture.IsValid())
        {


            if (UTexture2D* SourceTexture = Cast<UTexture2D>(PreviewObject))
            {
                UE_LOG(
                    LogUAssetImporter,
                    Display,
                    TEXT("Texture preview ready: Asset='%s', SourceSize=%dx%d, PreviewSize=%dx%d, SourcePixels=%s."),
                    *SourceTexture->GetName(),
                    SourceTexture->GetSizeX(),
                    SourceTexture->GetSizeY(),
                    Item->PreviewTexture->GetSizeX(),
                    Item->PreviewTexture->GetSizeY(),
                    SourceTexture->Source.IsValid() ? TEXT("available") : TEXT("unavailable"));
            }
        }
        else
        {
            PreviewRenderFailures.AddUnique(
                Item->AssetName + TEXT(" (thumbnail renderer returned no image)"));
        }
    }

    if (!PreviewDependencyLoadFailures.IsEmpty() || !PreviewRenderFailures.IsEmpty())
    {
        UE_LOG(
            LogUAssetImporter,
            Warning,
            TEXT("Live preview rendering incomplete: %d dependency package(s) failed to load and %d asset preview(s) failed to render. First dependency failure: %s. First preview failure: %s."),
            PreviewDependencyLoadFailures.Num(),
            PreviewRenderFailures.Num(),
            PreviewDependencyLoadFailures.IsEmpty() ? TEXT("none") : *PreviewDependencyLoadFailures[0],
            PreviewRenderFailures.IsEmpty() ? TEXT("none") : *PreviewRenderFailures[0]);
    }
}

FUAssetImporterResult FUAssetImporterService::Import(
    const TArray<TSharedPtr<FUAssetImporterAssetItem>>& Items,
    const FUAssetImporterOptions& Options)
{
    FUAssetImporterResult Result;
    TArray<FString> Warnings;
    TArray<FString> Errors;
    TArray<FPlannedAsset> PlannedAssets;
    TMap<FString, int32> PlannedAssetIndices;
    TArray<FString> PendingPackages;
    TSet<FString> VisitedTemporaryPackages;
    TSet<FString> SelectedOriginalPackages;
    TSet<FString> ReferencedOriginalPackages;
    TArray<FString> PackagesToSanitize;
    TSet<FString> ExcludedDependencyPackageSet;
    for (const FString& ExcludedPackageName : Options.ExcludedDependencyPackageNames)
    {
        ExcludedDependencyPackageSet.Add(UAssetImporterPrivate::NormalizePackageName(ExcludedPackageName).ToLower());
    }
    const bool bRelocateDependencies = Options.DependencyPlacement != EUAssetImporterDependencyPlacement::KeepOriginalPaths;

    FScopedSlowTask ImportProgress(100.0f, FText::FromString(TEXT("Importing assets...")));
    ImportProgress.MakeDialog(Options.bRollbackOnFailure);
    float ReportedProgress = 0.0f;
    double LastProgressRefreshTime = FPlatformTime::Seconds();
    auto AdvanceProgress = [&ImportProgress, &ReportedProgress, &LastProgressRefreshTime](const float Amount, const FText& Message)
    {
        const float Remaining = FMath::Max(0.0f, 100.0f - ReportedProgress);
        const float Step = FMath::Min(Amount, Remaining);
        if (Step > SMALL_NUMBER)
        {
            ImportProgress.EnterProgressFrame(Step, Message);
            ReportedProgress += Step;

            const double CurrentTime = FPlatformTime::Seconds();
            if (CurrentTime - LastProgressRefreshTime >= 0.1 || ReportedProgress >= 100.0f)
            {
                ImportProgress.TickProgress();
                LastProgressRefreshTime = CurrentTime;
            }
        }
    };

    for (const TSharedPtr<FUAssetImporterAssetItem>& Item : Items)
    {
        if (!Item.IsValid() || !Item->bSelected)
        {
            continue;
        }
        if (!Item->bMetadataValid)
        {
            Errors.Add(FString::Printf(TEXT("Cannot import '%s' because its package metadata could not be read."), *Item->SourceFile));
            continue;
        }
        PendingPackages.Add(Item->TemporaryPackageName);
        SelectedOriginalPackages.Add(Item->OriginalPackageName);
    }

    for (const FString& SelectedDependencyPackageName : Options.SelectedDependencyPackageNames)
    {
        const FString TemporaryDependencyPackage = FindTemporaryPackageForPackage(
            UAssetImporterPrivate::NormalizePackageName(SelectedDependencyPackageName));
        if (!TemporaryDependencyPackage.IsEmpty())
        {
            PendingPackages.AddUnique(TemporaryDependencyPackage);
        }
    }

    AdvanceProgress(5.0f, FText::FromString(TEXT("Collecting selected assets and dependencies...")));

    if (PendingPackages.IsEmpty() || !Errors.IsEmpty())
    {
        Result.ProblemCount = Errors.Num();
        Result.Errors = Errors;
        Result.Warnings = Warnings;
        ReportIssues(Warnings, Errors, Result, Options.bShowProblemLog, Options.bLogSummary);
        return Result;
    }

    FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
    IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

    float DependencyProgress = 0.0f;
    auto GetOriginalDependencyPackage = [this](FString DependencyPackage)
    {
        DependencyPackage = UAssetImporterPrivate::NormalizePackageName(MoveTemp(DependencyPackage));
        for (const FSourceRoot& Root : SourceRoots)
        {
            const FString TemporaryPrefix = UAssetImporterPrivate::EnsureTrailingSlash(Root.TemporaryVirtualRoot);
            if (DependencyPackage.StartsWith(TemporaryPrefix, ESearchCase::IgnoreCase))
            {
                const FString RelativePackagePath = DependencyPackage.Mid(TemporaryPrefix.Len());
                return UAssetImporterPrivate::NormalizePackageName(Root.OriginalVirtualRoot + TEXT("/") + RelativePackagePath);
            }
        }
        return DependencyPackage;
    };

    while (!PendingPackages.IsEmpty())
    {
        const FString TemporaryPackage = PendingPackages.Pop(EAllowShrinking::No);
        if (VisitedTemporaryPackages.Contains(TemporaryPackage))
        {
            continue;
        }
        VisitedTemporaryPackages.Add(TemporaryPackage);
        if (DependencyProgress < 15.0f)
        {
            const float Step = FMath::Min(0.15f, 15.0f - DependencyProgress);
            AdvanceProgress(Step, FText::FromString(FString::Printf(TEXT("Resolving dependencies: %s"), *TemporaryPackage)));
            DependencyProgress += Step;
        }

        const FString* SourceFile = TemporaryPackageToFile.Find(TemporaryPackage);
        const FString* OriginalPackage = TemporaryPackageToOriginalPackage.Find(TemporaryPackage);
        if (!SourceFile || !OriginalPackage || !FPaths::FileExists(*SourceFile))
        {
            Errors.AddUnique(FString::Printf(TEXT("A required source package could not be found for '%s'."), *TemporaryPackage));
            continue;
        }
        if (!AssetDataByTemporaryPackage.Contains(TemporaryPackage))
        {
            Errors.AddUnique(FString::Printf(TEXT("The package metadata could not be read for '%s'."), **SourceFile));
            continue;
        }

        if (!PlannedAssetIndices.Contains(*OriginalPackage))
        {
            FPlannedAsset Planned;
            Planned.SourceFile = *SourceFile;
            Planned.TemporaryPackageName = TemporaryPackage;
            Planned.OriginalPackageName = *OriginalPackage;
            Planned.DestinationPackageName = *OriginalPackage;
            Planned.bIsSelectedRoot = SelectedOriginalPackages.Contains(*OriginalPackage);
            PlannedAssetIndices.Add(*OriginalPackage, PlannedAssets.Add(MoveTemp(Planned)));
        }

        auto ProcessDependency = [this, &PendingPackages, &Errors, &Warnings, &Options, &ReferencedOriginalPackages, &ExcludedDependencyPackageSet](const FString& DependencyName, const bool bSoft)
        {
            const FString Dependency = UAssetImporterPrivate::NormalizePackageName(DependencyName);
            if (Dependency.IsEmpty() || UAssetImporterPrivate::IsNativeOrEnginePackage(Dependency))
            {
                return;
            }

            if (ExcludedDependencyPackageSet.Contains(Dependency.ToLower()))
            {
                if (!IsTargetPackageAvailable(Dependency))
                {
                    Warnings.AddUnique(FString::Printf(
                        TEXT("Dependency was intentionally excluded from import: '%s'. Assets referencing it may have unresolved references."),
                        *Dependency));
                }
                else
                {
                    UE_LOG(LogUAssetImporter, Verbose,
                        TEXT("[Import] Excluded dependency '%s' is already available at its original package path and will be reused."),
                        *Dependency);
                }
                return;
            }

            const FString SourceDependencyFile = FindSourceFileForPackage(Dependency);
            const FString SourceTemporaryPackage = FindTemporaryPackageForPackage(Dependency);
            const bool bSourceMetadataAvailable = !SourceTemporaryPackage.IsEmpty()
                && AssetDataByTemporaryPackage.Contains(SourceTemporaryPackage);

            if (!SourceDependencyFile.IsEmpty() && !SourceTemporaryPackage.IsEmpty() && bSourceMetadataAvailable)
            {
                ReferencedOriginalPackages.Add(Dependency);
                PendingPackages.AddUnique(SourceTemporaryPackage);
                return;
            }

            if (IsTargetPackageAvailable(Dependency))
            {
                UE_LOG(LogUAssetImporter, Verbose,
                    TEXT("[Import] Reusing dependency already available at its original package path: '%s'."),
                    *Dependency);
                return;
            }

            const FString Issue = !SourceTemporaryPackage.IsEmpty() && !bSourceMetadataAvailable
                ? FString::Printf(
                    TEXT("The source %s dependency '%s' exists, but Unreal could not read its package metadata. It may use an unsupported custom version or plugin version."),
                    bSoft ? TEXT("soft") : TEXT("required"),
                    *Dependency)
                : FString::Printf(TEXT("Missing %s dependency: %s"), bSoft ? TEXT("soft") : TEXT("required"), *Dependency);
            if (bSoft && Options.bIgnoreSoftReferenceErrors)
            {
                Warnings.AddUnique(Issue);
            }
            else if (Options.bAbortIfDependencyMissing)
            {
                Errors.AddUnique(Issue);
            }
            else
            {
                Warnings.AddUnique(Issue);
            }
        };

        if (const TArray<FString>* HardDependencies = HardDependenciesByTemporaryPackage.Find(TemporaryPackage))
        {
            for (const FString& Dependency : *HardDependencies)
            {
                ProcessDependency(Dependency, false);
            }
        }
        if (const TArray<FString>* SoftDependencies = SoftDependenciesByTemporaryPackage.Find(TemporaryPackage))
        {
            for (const FString& Dependency : *SoftDependencies)
            {
                ProcessDependency(Dependency, true);
            }
        }

        if (!HardDependenciesByTemporaryPackage.Contains(TemporaryPackage)
            || !SoftDependenciesByTemporaryPackage.Contains(TemporaryPackage))
        {
            TArray<FName> HardDependencies;
            UE::AssetRegistry::FDependencyQuery HardQuery;
            HardQuery.Required = UE::AssetRegistry::EDependencyProperty::Hard;
            AssetRegistry.GetDependencies(
                FName(*TemporaryPackage),
                HardDependencies,
                UE::AssetRegistry::EDependencyCategory::Package,
                HardQuery);

            TArray<FName> SoftDependencies;
            UE::AssetRegistry::FDependencyQuery SoftQuery;
            SoftQuery.Excluded = UE::AssetRegistry::EDependencyProperty::Hard;
            AssetRegistry.GetDependencies(
                FName(*TemporaryPackage),
                SoftDependencies,
                UE::AssetRegistry::EDependencyCategory::Package,
                SoftQuery);

            for (const FName& Dependency : HardDependencies)
            {
                ProcessDependency(GetOriginalDependencyPackage(Dependency.ToString()), false);
            }
            for (const FName& Dependency : SoftDependencies)
            {
                ProcessDependency(GetOriginalDependencyPackage(Dependency.ToString()), true);
            }
        }

    }

    AdvanceProgress(15.0f - DependencyProgress, FText::FromString(TEXT("Dependency graph resolved.")));

    if (!Errors.IsEmpty())
    {
        Result.ProblemCount = Errors.Num() + Warnings.Num();
        Result.Errors = Errors;
        Result.Warnings = Warnings;
        ReportIssues(Warnings, Errors, Result, Options.bShowProblemLog, Options.bLogSummary);
        return Result;
    }

    AdvanceProgress(5.0f, FText::FromString(TEXT("Planning destination paths...")));

    FString RequestedDestinationFolder;
    if (Options.DestinationMode == EUAssetImporterDestinationMode::ProjectContentRoot)
    {
        RequestedDestinationFolder = TEXT("/Game");
    }
    else if (Options.DestinationMode == EUAssetImporterDestinationMode::PreserveOriginalPaths)
    {
        RequestedDestinationFolder.Reset();
        if (bRelocateDependencies)
        {
            Errors.Add(TEXT("Dependency placement must be set to 'Keep original paths' when preserving original package paths."));
        }
    }
    else
    {
        RequestedDestinationFolder = UAssetImporterPrivate::NormalizePackageName(Options.DestinationPackagePath);
    }

    if (Options.DestinationMode != EUAssetImporterDestinationMode::PreserveOriginalPaths
        && !FPackageName::IsValidLongPackageName(RequestedDestinationFolder, true))
    {
        Errors.Add(FString::Printf(TEXT("Invalid destination folder: %s"), *RequestedDestinationFolder));
    }

    for (FPlannedAsset& Planned : PlannedAssets)
    {
        Planned.DestinationPackageName = Planned.OriginalPackageName;
        if (Options.DestinationMode == EUAssetImporterDestinationMode::PreserveOriginalPaths)
        {
            continue;
        }

        if (Planned.bIsSelectedRoot)
        {
            Planned.DestinationPackageName = RequestedDestinationFolder / FPaths::GetBaseFilename(Planned.SourceFile);
        }
        else if (bRelocateDependencies)
        {
            switch (Options.DependencyPlacement)
            {
            case EUAssetImporterDependencyPlacement::DependenciesFolder:
                Planned.DestinationPackageName = RequestedDestinationFolder / TEXT("Dependencies")
                    / UAssetImporterPrivate::GetPackagePathRelativeToMount(Planned.OriginalPackageName);
                break;
            case EUAssetImporterDependencyPlacement::FlatDependenciesFolder:
                Planned.DestinationPackageName = RequestedDestinationFolder / TEXT("Dependencies")
                    / FPaths::GetBaseFilename(Planned.SourceFile);
                break;
            case EUAssetImporterDependencyPlacement::SameFolderAsAssets:
                Planned.DestinationPackageName = RequestedDestinationFolder
                    / FPaths::GetBaseFilename(Planned.SourceFile);
                break;
            default:
                Planned.DestinationPackageName = Planned.OriginalPackageName;
                break;
            }
        }

        Planned.DestinationPackageName = UAssetImporterPrivate::NormalizePackageName(Planned.DestinationPackageName);
        if (!FPackageName::IsValidLongPackageName(Planned.DestinationPackageName, false))
        {
            Errors.Add(FString::Printf(TEXT("Invalid destination package path: %s"), *Planned.DestinationPackageName));
        }
    }

    if (!Errors.IsEmpty())
    {
        Result.ProblemCount = Errors.Num() + Warnings.Num();
        Result.Errors = Errors;
        Result.Warnings = Warnings;
        ReportIssues(Warnings, Errors, Result, Options.bShowProblemLog, Options.bLogSummary);
        return Result;
    }

    if (!bRelocateDependencies)
    {
        TArray<FPlannedAsset> OriginalPathDependencyCopies;
        for (const FPlannedAsset& Planned : PlannedAssets)
        {
            if (Planned.bIsSelectedRoot
                && !Planned.DestinationPackageName.Equals(Planned.OriginalPackageName, ESearchCase::IgnoreCase)
                && ReferencedOriginalPackages.Contains(Planned.OriginalPackageName))
            {
                FPlannedAsset DependencyCopy = Planned;
                DependencyCopy.DestinationPackageName = Planned.OriginalPackageName;
                DependencyCopy.bIsSelectedRoot = false;
                OriginalPathDependencyCopies.Add(MoveTemp(DependencyCopy));
            }
        }
        PlannedAssets.Append(OriginalPathDependencyCopies);
    }

    PlannedAssets.Sort([](const FPlannedAsset& A, const FPlannedAsset& B)
    {
        if (A.DestinationPackageName.Equals(B.DestinationPackageName, ESearchCase::IgnoreCase))
        {
            return A.SourceFile < B.SourceFile;
        }
        return A.DestinationPackageName < B.DestinationPackageName;
    });

    TArray<FCopyOperation> CopyOperations;
    TSet<FString> ReusedDestinationPackages;
    TMap<FString, FString> DestinationPackagesByName;
    for (const FPlannedAsset& Planned : PlannedAssets)
    {
        if (const FString* ExistingSource = DestinationPackagesByName.Find(Planned.DestinationPackageName))
        {
            if (!ExistingSource->Equals(Planned.OriginalPackageName, ESearchCase::IgnoreCase))
            {
                Errors.AddUnique(FString::Printf(
                    TEXT("Multiple source packages map to destination '%s' (sources '%s' and '%s'). Choose another folder."),
                    *Planned.DestinationPackageName,
                    **ExistingSource,
                    *Planned.OriginalPackageName));
                continue;
            }
        }
        else
        {
            DestinationPackagesByName.Add(Planned.DestinationPackageName, Planned.OriginalPackageName);
        }

        FString DestinationBase;
        FString DestinationRoot;
        FString DestinationProblem;
        if (!GetDestinationBasePath(Planned.DestinationPackageName, DestinationBase, DestinationRoot, DestinationProblem))
        {
            Errors.AddUnique(DestinationProblem);
            continue;
        }

        FCopyOperation Operation;
        Operation.Asset = Planned;
        Operation.DestinationBase = DestinationBase;
        Operation.DestinationRoot = DestinationRoot;
        Operation.bDestinationExists = FPaths::FileExists(DestinationBase + TEXT(".uasset"));

        const bool bAlreadyProcessedInBatch = UAssetImporterPrivate::IsPackageNameAlreadyProcessed(
            Options.BatchAlreadyProcessedPackageNames,
            Planned.DestinationPackageName);

        if (bAlreadyProcessedInBatch)
        {
            ++Result.SkippedCount;
            continue;
        }

        if (Operation.bDestinationExists)
{
    UPackage* SourcePackage = FindPackage(
        nullptr,
        *Planned.TemporaryPackageName);

    if (!SourcePackage)
    {
        SourcePackage = LoadPackage(
            nullptr,
            *Planned.TemporaryPackageName,
            LOAD_None);
    }

    UPackage* ExistingDestinationPackage = FindPackage(
        nullptr,
        *Planned.DestinationPackageName);

    if (!ExistingDestinationPackage)
    {
        ExistingDestinationPackage = LoadPackage(
            nullptr,
            *Planned.DestinationPackageName,
            LOAD_None);
    }

    const bool bHasMatchingPersistentGuid =
        SourcePackage
        && ExistingDestinationPackage
        && SourcePackage->GetPersistentGuid().IsValid()
        && ExistingDestinationPackage->GetPersistentGuid().IsValid()
        && SourcePackage->GetPersistentGuid()
            == ExistingDestinationPackage->GetPersistentGuid();

    const bool bReuseExistingPackage =
        bHasMatchingPersistentGuid
        && (!Planned.bIsSelectedRoot || !Options.bOverwriteExistingAssets);

    if (bReuseExistingPackage)
    {
        ReusedDestinationPackages.Add(
            Planned.DestinationPackageName.ToLower());

        ++Result.SkippedCount;

        UE_LOG(
            LogUAssetImporter,
            Display,
            TEXT("[Import] Reusing existing %s verified by persistent package GUID: '%s'."),
            Planned.bIsSelectedRoot ? TEXT("root asset") : TEXT("dependency"),
            *Planned.DestinationPackageName);

        continue;
    }

    if (!Options.bOverwriteExistingAssets)
    {
        const FString SourceGuid = SourcePackage
            ? SourcePackage->GetPersistentGuid().ToString()
            : TEXT("<source package could not be loaded>");

        const FString DestinationGuid = ExistingDestinationPackage
            ? ExistingDestinationPackage->GetPersistentGuid().ToString()
            : TEXT("<destination package could not be loaded>");

        Errors.AddUnique(FString::Printf(
            TEXT("Destination package '%s' already exists and could not be verified as the same source asset (source GUID: %s, destination GUID: %s). Choose another destination or enable overwrite."),
            *Planned.DestinationPackageName,
            *SourceGuid,
            *DestinationGuid));

        continue;
    }
}
        
        if (!Operation.bDestinationExists && !GetPackageFiles(DestinationBase).IsEmpty() && !Options.bOverwriteExistingAssets)
        {
            Errors.AddUnique(FString::Printf(
                TEXT("Orphaned package sidecar files exist at '%s'. Enable overwrite to replace them safely."),
                *DestinationBase));
            continue;
        }

        CopyOperations.Add(MoveTemp(Operation));
    }

    if (!Errors.IsEmpty())
    {
        Result.ProblemCount = Errors.Num() + Warnings.Num();
        Result.Errors = Errors;
        Result.Warnings = Warnings;
        ReportIssues(Warnings, Errors, Result, Options.bShowProblemLog, Options.bLogSummary);
        return Result;
    }

    for (const FCopyOperation& Operation : CopyOperations)
    {
        if (Operation.bDestinationExists)
        {
            continue;
        }

        FString PlaceholderProblem;
        if (!UAssetImporterPrivate::ReleaseEmptyInMemoryPackage(Operation.Asset.DestinationPackageName, PlaceholderProblem))
        {
            Errors.AddUnique(PlaceholderProblem);
        }
    }

    if (!Errors.IsEmpty())
    {
        Result.ProblemCount = Errors.Num() + Warnings.Num();
        Result.Errors = Errors;
        Result.Warnings = Warnings;
        ReportIssues(Warnings, Errors, Result, Options.bShowProblemLog, Options.bLogSummary);
        return Result;
    }

    if (Options.bOverwriteExistingAssets)
    {
        TArray<UPackage*> PackagesToUnload;
        for (const FCopyOperation& Operation : CopyOperations)
        {
            if (!Operation.bDestinationExists)
            {
                continue;
            }

            if (UPackage* ExistingPackage = FindPackage(nullptr, *Operation.Asset.DestinationPackageName))
            {
                if (ExistingPackage->IsDirty())
                {
                    Errors.AddUnique(FString::Printf(
                        TEXT("Cannot overwrite '%s' because it is loaded and has unsaved changes. Save or discard those changes first."),
                        *Operation.Asset.DestinationPackageName));
                    continue;
                }
                PackagesToUnload.AddUnique(ExistingPackage);
            }
        }

        if (!Errors.IsEmpty())
        {
            Result.ProblemCount = Errors.Num() + Warnings.Num();
            Result.Errors = Errors;
            Result.Warnings = Warnings;
            ReportIssues(Warnings, Errors, Result, Options.bShowProblemLog, Options.bLogSummary);
            return Result;
        }

        if (!PackagesToUnload.IsEmpty())
        {
            bool bAnyPackagesUnloaded = false;
            FText UnloadError;
            UEditorLoadingAndSavingUtils::UnloadPackages(PackagesToUnload, bAnyPackagesUnloaded, UnloadError);

            for (const FCopyOperation& Operation : CopyOperations)
            {
                if (Operation.bDestinationExists && FindPackage(nullptr, *Operation.Asset.DestinationPackageName))
                {
                    Errors.AddUnique(FString::Printf(
                        TEXT("Cannot overwrite '%s' because its old package is still loaded."),
                        *Operation.Asset.DestinationPackageName));
                }
            }

            if (!Errors.IsEmpty() || !UnloadError.IsEmpty())
            {
                if (!UnloadError.IsEmpty())
                {
                    Errors.AddUnique(FString::Printf(TEXT("Could not unload target packages: %s"), *UnloadError.ToString()));
                }
                Result.ProblemCount = Errors.Num() + Warnings.Num();
                Result.Errors = Errors;
                Result.Warnings = Warnings;
                ReportIssues(Warnings, Errors, Result, Options.bShowProblemLog, Options.bLogSummary);
                return Result;
            }
        }
    }

    const FString BackupRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UAssetImporterRollback"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
    TMap<FString, FString> BackupsByDestination;
    TSet<FString> TouchedDestinationFiles;
    TArray<FString> CopiedDestinationFiles;
    bool bCopyFailed = false;

    for (const FCopyOperation& Operation : CopyOperations)
    {
        AdvanceProgress(
            CopyOperations.IsEmpty() ? 0.0f : 25.0f / static_cast<float>(CopyOperations.Num()),
            FText::FromString(FString::Printf(TEXT("Copying %s..."), *Operation.Asset.OriginalPackageName)));

        if (ImportProgress.ShouldCancel())
        {
            Errors.Add(TEXT("Import canceled."));
            bCopyFailed = true;
            break;
        }

        const TArray<FString> ExistingDestinationFiles = GetPackageFiles(Operation.DestinationBase);
        if (Options.bOverwriteExistingAssets && !ExistingDestinationFiles.IsEmpty() && Options.bRollbackOnFailure)
        {
            for (const FString& ExistingFile : ExistingDestinationFiles)
            {
                FString RelativeBackupPath = ExistingFile;
                FPaths::MakePathRelativeTo(RelativeBackupPath, *Operation.DestinationRoot);
                const FString BackupPath = FPaths::Combine(BackupRoot, RelativeBackupPath);
                IFileManager::Get().MakeDirectory(*FPaths::GetPath(BackupPath), true);
                FString BackupProblem;
                if (!CopyFileVerified(ExistingFile, BackupPath, BackupProblem))
                {
                    Errors.AddUnique(FString::Printf(TEXT("Could not back up '%s': %s"), *ExistingFile, *BackupProblem));
                    bCopyFailed = true;
                    break;
                }
                BackupsByDestination.Add(ExistingFile, BackupPath);
            }
        }
        if (bCopyFailed)
        {
            break;
        }

        const FString SourceBase = FPaths::Combine(FPaths::GetPath(Operation.Asset.SourceFile), FPaths::GetBaseFilename(Operation.Asset.SourceFile));
        const TArray<FString> SourcePackageFiles = GetPackageFiles(SourceBase);
        if (SourcePackageFiles.IsEmpty())
        {
            Errors.AddUnique(FString::Printf(TEXT("No package files were found for '%s'."), *Operation.Asset.SourceFile));
            bCopyFailed = true;
            break;
        }

        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Operation.DestinationBase), true);
        TSet<FString> SourceExtensions;
        for (const FString& SourcePackageFile : SourcePackageFiles)
        {
            const FString Extension = SourcePackageFile.Mid(SourceBase.Len());
            SourceExtensions.Add(Extension.ToLower());
            const FString DestinationFile = Operation.DestinationBase + Extension;
            TouchedDestinationFiles.Add(DestinationFile);
            FString CopyProblem;
            if (!CopyFileVerified(SourcePackageFile, DestinationFile, CopyProblem))
            {
                Errors.AddUnique(FString::Printf(TEXT("Failed to copy '%s' to '%s': %s"), *SourcePackageFile, *DestinationFile, *CopyProblem));
                bCopyFailed = true;
                break;
            }
            if (FPaths::GetExtension(DestinationFile).Equals(TEXT("uasset"), ESearchCase::IgnoreCase))
            {
                CopiedDestinationFiles.AddUnique(DestinationFile);
            }
        }
        if (bCopyFailed)
        {
            break;
        }

        if (Options.bOverwriteExistingAssets)
        {
            for (const FString& ExistingFile : ExistingDestinationFiles)
            {
                const FString Extension = ExistingFile.Mid(Operation.DestinationBase.Len()).ToLower();
                if (SourceExtensions.Contains(Extension))
                {
                    continue;
                }

                UAssetImporterPrivate::IgnoreAutoReimportDelete(ExistingFile);
                TouchedDestinationFiles.Add(ExistingFile);
                if (!IFileManager::Get().Delete(*ExistingFile, false, true, true))
                {
                    Errors.AddUnique(FString::Printf(TEXT("Could not remove obsolete package sidecar '%s'."), *ExistingFile));
                    bCopyFailed = true;
                    break;
                }
            }
        }
        if (bCopyFailed)
        {
            break;
        }

        Result.CopiedPackageNames.AddUnique(Operation.Asset.DestinationPackageName);
        ++Result.ImportedCount;
    }

    if (!bCopyFailed && bRelocateDependencies)
    {
        AdvanceProgress(5.0f, FText::FromString(TEXT("Preparing reference remapping...")));

        FLinkerInstancingContext RemapContext(true);
        TArray<FCoreRedirect> TemporaryCoreRedirects;
        const FString TemporaryRedirectSource = FString::Printf(
            TEXT("UAssetImporter_TemporaryRelocation_%s"),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits));
        bool bTemporaryRedirectRegistrationAttempted = false;
        ON_SCOPE_EXIT
        {
            if (bTemporaryRedirectRegistrationAttempted)
            {
                if (!FCoreRedirects::RemoveRedirectList(MakeArrayView(TemporaryCoreRedirects), TemporaryRedirectSource))
                {
                    UE_LOG(LogUAssetImporter, Warning,
                        TEXT("[Import][Remap] Could not remove every temporary core redirect registered for this import."));
                }
                else
                {
                    UE_LOG(LogUAssetImporter, Display,
                        TEXT("[Import][Remap] Removed %d temporary core redirect(s)."),
                        TemporaryCoreRedirects.Num());
                }
            }
        };

        int32 PackageMappingCount = 0;
        int32 ObjectPathMappingCount = 0;
        for (const FPlannedAsset& Planned : PlannedAssets)
        {
            const bool bPackageIsRelocated = !Planned.OriginalPackageName.Equals(
                Planned.DestinationPackageName,
                ESearchCase::IgnoreCase);
            const bool bOriginalPackageMissingInTarget = !IsTargetPackageAvailable(Planned.OriginalPackageName);

            if (bPackageIsRelocated)
            {
                RemapContext.AddPackageMapping(FName(*Planned.OriginalPackageName), FName(*Planned.DestinationPackageName));
                ++PackageMappingCount;

                if (bOriginalPackageMissingInTarget)
                {
                    TemporaryCoreRedirects.Emplace(
                        ECoreRedirectFlags::Type_Package,
                        Planned.OriginalPackageName,
                        Planned.DestinationPackageName);

                    UE_LOG(LogUAssetImporter, Display,
                        TEXT("[Import][Remap] Temporary package redirect: '%s' -> '%s'."),
                        *Planned.OriginalPackageName,
                        *Planned.DestinationPackageName);
                }
            }

            const FAssetData* AssetData = AssetDataByTemporaryPackage.Find(Planned.TemporaryPackageName);
            const FString AssetName = AssetData && AssetData->IsValid()
                ? AssetData->AssetName.ToString()
                : FPackageName::GetShortName(Planned.OriginalPackageName);
            const FString OriginalObjectPath = Planned.OriginalPackageName + TEXT(".") + AssetName;
            const FString DestinationObjectPath = Planned.DestinationPackageName + TEXT(".") + AssetName;
            if (!OriginalObjectPath.Equals(DestinationObjectPath, ESearchCase::IgnoreCase))
            {
                RemapContext.AddPathMapping(
                    FSoftObjectPath(OriginalObjectPath),
                    FSoftObjectPath(DestinationObjectPath));
                ++ObjectPathMappingCount;

                if (bOriginalPackageMissingInTarget)
                {
                    TemporaryCoreRedirects.Emplace(
                        ECoreRedirectFlags::Type_Asset,
                        OriginalObjectPath,
                        DestinationObjectPath);

                    UE_LOG(LogUAssetImporter, Display,
                        TEXT("[Import][Remap] Temporary asset redirect: '%s' -> '%s'."),
                        *OriginalObjectPath,
                        *DestinationObjectPath);
                }
            }
        }

        if (!TemporaryCoreRedirects.IsEmpty())
        {
            bTemporaryRedirectRegistrationAttempted = true;
            if (!FCoreRedirects::AddRedirectList(MakeArrayView(TemporaryCoreRedirects), TemporaryRedirectSource))
            {
                Errors.Add(TEXT("Could not register temporary reference redirects required to remap relocated dependencies."));
                bCopyFailed = true;
            }
            else
            {
                UE_LOG(LogUAssetImporter, Display,
                    TEXT("[Import][Remap] Registered %d temporary core redirect(s) for missing original package paths."),
                    TemporaryCoreRedirects.Num());
            }
        }

        TMap<FString, int32> PlannedIndicesByTemporaryPackage;
        for (int32 Index = 0; Index < PlannedAssets.Num(); ++Index)
        {
            PlannedIndicesByTemporaryPackage.Add(PlannedAssets[Index].TemporaryPackageName.ToLower(), Index);
        }

        TArray<int32> OrderedPlannedAssetIndices;
        TSet<int32> VisitedPlannedAssetIndices;
        TSet<int32> PlannedAssetIndicesBeingVisited;
        TFunction<void(int32)> VisitPlannedAsset;
        VisitPlannedAsset = [this, &PlannedAssets, &PlannedIndicesByTemporaryPackage, &OrderedPlannedAssetIndices,
            &VisitedPlannedAssetIndices, &PlannedAssetIndicesBeingVisited, &VisitPlannedAsset](const int32 Index)
        {
            if (!PlannedAssets.IsValidIndex(Index)
                || VisitedPlannedAssetIndices.Contains(Index)
                || PlannedAssetIndicesBeingVisited.Contains(Index))
            {
                return;
            }

            PlannedAssetIndicesBeingVisited.Add(Index);

            auto VisitDependencies = [this, &PlannedIndicesByTemporaryPackage, &VisitPlannedAsset](const TArray<FString>* Dependencies)
            {
                if (!Dependencies)
                {
                    return;
                }

                for (const FString& DependencyPackage : *Dependencies)
                {
                    const FString TemporaryDependency = FindTemporaryPackageForPackage(DependencyPackage);
                    if (TemporaryDependency.IsEmpty())
                    {
                        continue;
                    }

                    if (const int32* DependencyIndex = PlannedIndicesByTemporaryPackage.Find(TemporaryDependency.ToLower()))
                    {
                        VisitPlannedAsset(*DependencyIndex);
                    }
                }
            };

            const FPlannedAsset& Planned = PlannedAssets[Index];
            VisitDependencies(HardDependenciesByTemporaryPackage.Find(Planned.TemporaryPackageName));
            VisitDependencies(SoftDependenciesByTemporaryPackage.Find(Planned.TemporaryPackageName));

            PlannedAssetIndicesBeingVisited.Remove(Index);
            VisitedPlannedAssetIndices.Add(Index);
            OrderedPlannedAssetIndices.Add(Index);
        };

        for (int32 Index = 0; Index < PlannedAssets.Num(); ++Index)
        {
            VisitPlannedAsset(Index);
        }

        UE_LOG(LogUAssetImporter, Display,
            TEXT("[Import][Remap] Prepared %d package mappings and %d soft object path mappings. Loading %d packages in dependency-first order."),
            PackageMappingCount,
            ObjectPathMappingCount,
            OrderedPlannedAssetIndices.Num());

        for (const FPlannedAsset& Planned : PlannedAssets)
        {
            if (Planned.OriginalPackageName.Equals(Planned.DestinationPackageName, ESearchCase::IgnoreCase))
            {
                continue;
            }

            const FName RemappedPackageName = RemapContext.RemapPackage(FName(*Planned.OriginalPackageName));
            if (!RemappedPackageName.ToString().Equals(Planned.DestinationPackageName, ESearchCase::IgnoreCase))
            {
                UE_LOG(LogUAssetImporter, Error,
                    TEXT("[Import][Remap] Package mapping verification failed: '%s' -> '%s' (expected '%s')."),
                    *Planned.OriginalPackageName,
                    *RemappedPackageName.ToString(),
                    *Planned.DestinationPackageName);
                Errors.AddUnique(FString::Printf(
                    TEXT("Could not prepare a valid package reference mapping from '%s' to '%s'."),
                    *Planned.OriginalPackageName,
                    *Planned.DestinationPackageName));
                bCopyFailed = true;
                break;
            }

            const FAssetData* AssetData = AssetDataByTemporaryPackage.Find(Planned.TemporaryPackageName);
            const FString AssetName = AssetData && AssetData->IsValid()
                ? AssetData->AssetName.ToString()
                : FPackageName::GetShortName(Planned.OriginalPackageName);
            const FSoftObjectPath OriginalObjectPath(Planned.OriginalPackageName + TEXT(".") + AssetName);
            const FSoftObjectPath ExpectedObjectPath(Planned.DestinationPackageName + TEXT(".") + AssetName);
            const FSoftObjectPath RemappedObjectPath = RemapContext.RemapPath(OriginalObjectPath);
            if (!RemappedObjectPath.ToString().Equals(ExpectedObjectPath.ToString(), ESearchCase::IgnoreCase))
            {
                UE_LOG(LogUAssetImporter, Error,
                    TEXT("[Import][Remap] Object path mapping verification failed: '%s' -> '%s' (expected '%s')."),
                    *OriginalObjectPath.ToString(),
                    *RemappedObjectPath.ToString(),
                    *ExpectedObjectPath.ToString());
                Errors.AddUnique(FString::Printf(
                    TEXT("Could not prepare a valid object reference mapping from '%s' to '%s'."),
                    *OriginalObjectPath.ToString(),
                    *ExpectedObjectPath.ToString()));
                bCopyFailed = true;
                break;
            }
        }

        TArray<UPackage*> RemappedPackages;
        TSet<FString> RemappedPackageNames;
        for (const int32 PlannedIndex : OrderedPlannedAssetIndices)
        {
            if (bCopyFailed)
            {
                break;
            }
            const FPlannedAsset& Planned = PlannedAssets[PlannedIndex];
            if (RemappedPackageNames.Contains(Planned.DestinationPackageName))
            {
                continue;
            }
            RemappedPackageNames.Add(Planned.DestinationPackageName);

            if (ReusedDestinationPackages.Contains(Planned.DestinationPackageName.ToLower()))
            {
                UE_LOG(
                    LogUAssetImporter,
                    Display,
                    TEXT("[Import][Remap] Keeping verified existing package unchanged: '%s'."),
                    *Planned.DestinationPackageName);
                continue;
            }

            AdvanceProgress(
                OrderedPlannedAssetIndices.IsEmpty() ? 0.0f : 20.0f / static_cast<float>(OrderedPlannedAssetIndices.Num()),
                FText::FromString(FString::Printf(TEXT("Loading for reference remapping: %s"), *Planned.DestinationPackageName)));

            FString DestinationFilename;
            if (!FPackageName::DoesPackageExist(Planned.DestinationPackageName, &DestinationFilename, false)
                || DestinationFilename.IsEmpty()
                || !FPaths::FileExists(DestinationFilename))
            {
                Errors.AddUnique(FString::Printf(
                    TEXT("The copied package file is missing before reference remapping: '%s'."),
                    *Planned.DestinationPackageName));
                bCopyFailed = true;
                break;
            }

            UPackage* Package = FindPackage(nullptr, *Planned.DestinationPackageName);
            if (!Package)
            {
                UE_LOG(LogUAssetImporter, Display,
                    TEXT("[Import][Remap] Loading '%s' from '%s' (source package '%s')."),
                    *Planned.DestinationPackageName,
                    *DestinationFilename,
                    *Planned.OriginalPackageName);
                Package = LoadPackage(nullptr, *Planned.DestinationPackageName, LOAD_None, nullptr, &RemapContext);
            }
            else
            {
                UE_LOG(LogUAssetImporter, Display,
                    TEXT("[Import][Remap] Reusing package already loaded in memory: '%s'."),
                    *Planned.DestinationPackageName);
            }
            if (!Package)
            {
                Errors.AddUnique(FString::Printf(TEXT("Could not load copied package '%s' to remap its dependencies."), *Planned.DestinationPackageName));
                bCopyFailed = true;
                break;
            }
            RemappedPackages.AddUnique(Package);
        }

        if (!bCopyFailed)
        {
            for (UPackage* Package : RemappedPackages)
            {
                AdvanceProgress(
                    RemappedPackages.IsEmpty() ? 0.0f : 20.0f / static_cast<float>(RemappedPackages.Num()),
                    FText::FromString(FString::Printf(TEXT("Saving remapped package: %s"), *Package->GetName())));

                TArray<UObject*> PackageObjects;
                GetObjectsWithOuter(Package, PackageObjects, false);
                bool bChanged = false;
                for (UObject* Object : PackageObjects)
                {
                    if (Object && !Object->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject | RF_Transient))
                    {
                        UAssetImporterPrivate::SanitizeImportDataObject(Object, bChanged, true);
                    }
                }

                FString PackageFilename;
                if (!FPackageName::DoesPackageExist(Package->GetName(), &PackageFilename, false) || PackageFilename.IsEmpty())
                {
                    Errors.AddUnique(FString::Printf(TEXT("Could not resolve the destination filename for '%s'."), *Package->GetName()));
                    bCopyFailed = true;
                    break;
                }
                Package->MarkPackageDirty();
                FString SaveError;
                if (!UAssetImporterPrivate::SavePackageWithoutPrompt(Package, PackageFilename, SaveError))
                {
                    Errors.AddUnique(SaveError);
                    bCopyFailed = true;
                    break;
                }
            }
        }

        if (bCopyFailed && !RemappedPackages.IsEmpty())
        {
            bool bAnyUnloaded = false;
            FText UnloadError;
            UEditorLoadingAndSavingUtils::UnloadPackages(RemappedPackages, bAnyUnloaded, UnloadError);
            if (!UnloadError.IsEmpty())
            {
                Warnings.AddUnique(FString::Printf(TEXT("Some packages remained loaded after reference remapping failed: %s"), *UnloadError.ToString()));
            }
        }
    }
    else if (!bRelocateDependencies)
    {
        AdvanceProgress(45.0f, FText::FromString(TEXT("Finishing import...")));
    }

    if (!bCopyFailed && ImportProgress.ShouldCancel())
    {
        Errors.Add(TEXT("Import canceled."));
        bCopyFailed = true;
    }
    if (!bCopyFailed && Options.bClearImportSourceData)
    {
        TArray<FString> Problems;
        if (SanitizeImportedPackages(Result.CopiedPackageNames, Result.CopiedPackageNames, Problems))
        {
            Warnings.Append(Problems);
        }
        else
        {
            Errors.Append(Problems);
            bCopyFailed = true;
        }
    }

    if (bCopyFailed && Options.bRollbackOnFailure)
    {
        TArray<UPackage*> LoadedPackages;
        for (const FString& Name : Result.CopiedPackageNames)
        {
            if (UPackage* Package = FindPackage(nullptr, *Name))
            {
                LoadedPackages.AddUnique(Package);
            }
        }
        if (!LoadedPackages.IsEmpty())
        {
            bool bUnloaded = false;
            FText UnloadError;
            UEditorLoadingAndSavingUtils::UnloadPackages(LoadedPackages, bUnloaded, UnloadError);
            for (const FString& Name : Result.CopiedPackageNames)
            {
                if (FindPackage(nullptr, *Name))
                {
                    Errors.Add(FString::Printf(TEXT("Rollback paused because %s is still loaded. Recovery directory: %s"), *Name, *FPaths::ConvertRelativePathToFull(BackupRoot)));
                    Result.Errors = Errors;
                    Result.Warnings = Warnings;
                    Result.ProblemCount = Errors.Num() + Warnings.Num();
                    ReportIssues(Warnings, Errors, Result, Options.bShowProblemLog, Options.bLogSummary);
                    return Result;
                }
            }
        }
        bool bRollbackComplete = true;
        for (const FString& DestinationFile : TouchedDestinationFiles)
        {
            UAssetImporterPrivate::IgnoreAutoReimportDelete(DestinationFile);
            if (!BackupsByDestination.Contains(DestinationFile) && !IFileManager::Get().Delete(*DestinationFile, false, true, true))
            {
                bRollbackComplete = false;
                Errors.Add(FString::Printf(TEXT("Rollback could not remove %s"), *DestinationFile));
            }
        }
        for (const TPair<FString, FString>& Backup : BackupsByDestination)
        {
            FString RestoreProblem;
            if (!CopyFileVerified(Backup.Value, Backup.Key, RestoreProblem))
            {
                bRollbackComplete = false;
                Errors.AddUnique(FString::Printf(TEXT("Rollback could not restore '%s': %s"), *Backup.Key, *RestoreProblem));
            }
        }
        Result.ImportedCount = 0;
        Result.CopiedPackageNames.Reset();
        if (bRollbackComplete)
        {
            IFileManager::Get().DeleteDirectory(*BackupRoot, false, true);
        }
        else
        {
            Errors.Add(FString::Printf(TEXT("Rollback was incomplete. Recovery directory: %s"), *FPaths::ConvertRelativePathToFull(BackupRoot)));
        }
    }
    else if (!Options.bRollbackOnFailure)
    {
        IFileManager::Get().DeleteDirectory(*BackupRoot, false, true);
    }

    if (bCopyFailed)
    {
        Result.ProblemCount = Errors.Num() + Warnings.Num();
        Result.Errors = Errors;
        Result.Warnings = Warnings;
        ReportIssues(Warnings, Errors, Result, Options.bShowProblemLog, Options.bLogSummary);
        return Result;
    }

    if (Options.bRollbackOnFailure)
    {
        IFileManager::Get().DeleteDirectory(*BackupRoot, false, true);
    }

    PackagesToSanitize = Result.CopiedPackageNames;

    for (const FPlannedAsset& Planned : PlannedAssets)
    {
        if (Planned.bIsSelectedRoot)
        {
            Result.ImportedRootPackageNames.AddUnique(Planned.DestinationPackageName);
        }
    }

    Result.AffectedPackageNames = PackagesToSanitize;
    TArray<FString> FilesToScan = CopiedDestinationFiles;

    if (!FilesToScan.IsEmpty())
    {
        AssetRegistry.ScanFilesSynchronous(FilesToScan, true);
    }

    TArray<FAssetData> AssetsToSync;
    for (const FString& RootPackageName : Result.ImportedRootPackageNames)
    {
        const FString ObjectName = FPackageName::GetShortName(RootPackageName);
        const FString ObjectPath = UAssetImporterPrivate::MakeObjectPath(RootPackageName, ObjectName);

        if (Options.bLoadImportedRootAssets)
        {
            if (!LoadObject<UObject>(nullptr, *ObjectPath))
            {
                Warnings.AddUnique(FString::Printf(TEXT("The asset was copied but could not be loaded: %s"), *ObjectPath));
            }
        }

        if (Options.bSyncImportedAssets)
        {
            const FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
            if (AssetData.IsValid())
            {
                AssetsToSync.Add(AssetData);
            }
        }
    }

    if (Options.bSyncImportedAssets && GEditor && !AssetsToSync.IsEmpty())
    {
        GEditor->SyncBrowserToObjects(AssetsToSync, true);
    }

    AdvanceProgress(100.0f - ReportedProgress, FText::FromString(TEXT("Import finished.")));
    ImportProgress.ForceRefresh();

    Result.ProblemCount = Errors.Num() + Warnings.Num();
    Result.bSucceeded = Errors.IsEmpty();
    Result.Errors = Errors;
    Result.Warnings = Warnings;
    ReportIssues(Warnings, Errors, Result, Options.bShowProblemLog, Options.bLogSummary);
    return Result;
}

void FUAssetImporterService::GetDependencyItems(
    const TArray<TSharedPtr<FUAssetImporterAssetItem>>& Items,
    TArray<FUAssetImporterDependencyItem>& OutDependencies) const
{
    OutDependencies.Reset();

    TSet<FString> SelectedRootPackages;
    TArray<FString> PendingTemporaryPackages;
    for (const TSharedPtr<FUAssetImporterAssetItem>& Item : Items)
    {
        if (!Item.IsValid() || !Item->bSelected)
        {
            continue;
        }
        SelectedRootPackages.Add(Item->OriginalPackageName.ToLower());
        PendingTemporaryPackages.AddUnique(Item->TemporaryPackageName);
    }

    TMap<FString, int32> DependencyIndexByPackage;
    TSet<FString> VisitedTemporaryPackages;

    while (!PendingTemporaryPackages.IsEmpty())
    {
        const FString TemporaryPackage = PendingTemporaryPackages.Pop(EAllowShrinking::No);
        const FString TemporaryPackageKey = TemporaryPackage.ToLower();
        if (VisitedTemporaryPackages.Contains(TemporaryPackageKey))
        {
            continue;
        }
        VisitedTemporaryPackages.Add(TemporaryPackageKey);

        auto AddDependency = [this, &OutDependencies, &DependencyIndexByPackage, &PendingTemporaryPackages,
            &SelectedRootPackages](const FString& OriginalPackageName, const bool bSoftOnly)
        {
            const FString OriginalPackage = UAssetImporterPrivate::NormalizePackageName(OriginalPackageName);
            if (OriginalPackage.IsEmpty() || UAssetImporterPrivate::IsNativeOrEnginePackage(OriginalPackage))
            {
                return;
            }

            const FString PackageKey = OriginalPackage.ToLower();
            const FString TemporaryDependency = FindTemporaryPackageForPackage(OriginalPackage);
            const int32* ExistingIndex = DependencyIndexByPackage.Find(PackageKey);
            if (ExistingIndex)
            {
                if (!bSoftOnly)
                {
                    OutDependencies[*ExistingIndex].bSoftOnly = false;
                }
            }
            else if (!SelectedRootPackages.Contains(PackageKey))
            {
                FUAssetImporterDependencyItem DependencyItem;
                DependencyItem.OriginalPackageName = OriginalPackage;
                DependencyItem.TemporaryPackageName = TemporaryDependency;
                DependencyItem.SourceFile = FindSourceFileForPackage(OriginalPackage);
                DependencyItem.bHasSourceFile = !DependencyItem.SourceFile.IsEmpty() && FPaths::FileExists(DependencyItem.SourceFile);
                DependencyItem.bSoftOnly = bSoftOnly;

                if (!TemporaryDependency.IsEmpty())
                {
                    if (const FAssetData* AssetData = AssetDataByTemporaryPackage.Find(TemporaryDependency))
                    {
                        DependencyItem.AssetData = *AssetData;
                        DependencyItem.bHasMetadata = AssetData->IsValid();
                        if (DependencyItem.bHasMetadata)
                        {
                            DependencyItem.AssetName = AssetData->AssetName.ToString();
                            DependencyItem.AssetClass = AssetData->AssetClassPath.GetAssetName().ToString();
                        }
                    }
                }

                const bool bSourceCanBeImported = DependencyItem.bHasSourceFile && DependencyItem.bHasMetadata;
                DependencyItem.bAlreadyInProject = !bSourceCanBeImported && IsTargetPackageAvailable(OriginalPackage);
                DependencyItem.bMissing = !bSourceCanBeImported && !DependencyItem.bAlreadyInProject;

                if (DependencyItem.AssetName.IsEmpty())
                {
                    DependencyItem.AssetName = FPackageName::GetShortName(OriginalPackage);
                }
                if (DependencyItem.AssetClass.IsEmpty())
                {
                    DependencyItem.AssetClass = TEXT("Object");
                }

                const int32 NewIndex = OutDependencies.Add(MoveTemp(DependencyItem));
                DependencyIndexByPackage.Add(PackageKey, NewIndex);
            }

            if (!TemporaryDependency.IsEmpty())
            {
                PendingTemporaryPackages.AddUnique(TemporaryDependency);
            }
        };

        if (const TArray<FString>* HardDependencies = HardDependenciesByTemporaryPackage.Find(TemporaryPackage))
        {
            for (const FString& Dependency : *HardDependencies)
            {
                AddDependency(Dependency, false);
            }
        }

        if (const TArray<FString>* SoftDependencies = SoftDependenciesByTemporaryPackage.Find(TemporaryPackage))
        {
            for (const FString& Dependency : *SoftDependencies)
            {
                AddDependency(Dependency, true);
            }
        }
    }

    OutDependencies.Sort([](const FUAssetImporterDependencyItem& A, const FUAssetImporterDependencyItem& B)
    {
        return A.OriginalPackageName < B.OriginalPackageName;
    });
}

bool FUAssetImporterService::SanitizeImportedPackages(
    const TArray<FString>& PackageNames,
    const TArray<FString>& PackageNamesToClearAllSources,
    TArray<FString>& OutProblems)
{
    OutProblems.Reset();
    TSet<FString> ClearAllSourceSet;
    for (const FString& Name : PackageNamesToClearAllSources)
    {
        ClearAllSourceSet.Add(Name.ToLower());
    }

    TSet<FString> VisitedPackages;
    TMap<UPackage*, FString> DirtyPackages;
    bool bAllSucceeded = true;

    for (const FString& PackageName : PackageNames)
    {
        if (PackageName.IsEmpty() || VisitedPackages.Contains(PackageName.ToLower()))
        {
            continue;
        }
        VisitedPackages.Add(PackageName.ToLower());

        FString PackageFilename;
        if (!FPackageName::DoesPackageExist(PackageName, &PackageFilename, false) || PackageFilename.IsEmpty())
        {
            continue;
        }

        const bool bClearAllSources = ClearAllSourceSet.Contains(PackageName.ToLower());
        if (bClearAllSources)
        {
            UAssetImporterPrivate::IgnoreAutoReimportWrite(PackageFilename);
        }

        UPackage* Package = FindPackage(nullptr, *PackageName);
        if (!Package)
        {
            Package = LoadPackage(nullptr, *PackageName, LOAD_NoWarn);
        }
        if (!Package)
        {
            OutProblems.Add(FString::Printf(TEXT("Could not load package to clean import-source data: %s"), *PackageName));
            bAllSucceeded = false;
            continue;
        }

        TArray<UObject*> Objects;
        GetObjectsWithOuter(Package, Objects, false);
        bool bPackageChanged = false;
        for (UObject* Object : Objects)
        {
            if (!Object || Object->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject | RF_Transient))
            {
                continue;
            }
            UAssetImporterPrivate::SanitizeImportDataObject(Object, bPackageChanged, bClearAllSources);
        }

        if (bPackageChanged)
        {
            Package->MarkPackageDirty();
            DirtyPackages.Add(Package, PackageFilename);
        }

        if (bClearAllSources)
        {
            UAssetImporterPrivate::IgnoreAutoReimportWrite(PackageFilename);
        }
    }

    for (const TPair<UPackage*, FString>& Entry : DirtyPackages)
    {
        FString SaveError;
        if (!UAssetImporterPrivate::SavePackageWithoutPrompt(Entry.Key, Entry.Value, SaveError))
        {
            OutProblems.Add(SaveError);
            bAllSucceeded = false;
        }
    }

    return bAllSucceeded;
}

void FUAssetImporterService::Cleanup()
{
    auto IsTemporaryPackage = [this](const UPackage* Package)
    {
        if (!Package)
        {
            return false;
        }

        const FString PackageName = Package->GetName();
        for (const FSourceRoot& Root : SourceRoots)
        {
            if (PackageName.StartsWith(
                UAssetImporterPrivate::EnsureTrailingSlash(Root.TemporaryVirtualRoot),
                ESearchCase::IgnoreCase))
            {
                return true;
            }
        }
        return false;
    };

    TArray<UPackage*> PackagesToUnload;
    for (TObjectIterator<UPackage> It; It; ++It)
    {
        UPackage* Package = *It;
        if (IsTemporaryPackage(Package))
        {
            Package->SetDirtyFlag(false);
            PackagesToUnload.AddUnique(Package);
        }
    }

    IAssetRegistry* AssetRegistry = nullptr;
    if (FModuleManager::Get().IsModuleLoaded(TEXT("AssetRegistry")))
    {
        AssetRegistry = &FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
        for (UPackage* Package : PackagesToUnload)
        {
            AssetRegistry->PackageDeleted(Package);
        }
    }

    FText UnloadError;
    if (!PackagesToUnload.IsEmpty())
    {
        bool bAnyPackagesUnloaded = false;
        UEditorLoadingAndSavingUtils::UnloadPackages(PackagesToUnload, bAnyPackagesUnloaded, UnloadError);
        CollectGarbage(RF_NoFlags, true);
    }

    TArray<FString> RemainingTemporaryPackages;
    for (TObjectIterator<UPackage> It; It; ++It)
    {
        UPackage* Package = *It;
        if (IsTemporaryPackage(Package))
        {
            RemainingTemporaryPackages.AddUnique(Package->GetName());
        }
    }

    if (!RemainingTemporaryPackages.IsEmpty())
    {
        UE_LOG(
            LogUAssetImporter,
            Warning,
            TEXT("Cleanup kept temporary source mounts active because %d package(s) remain loaded. First: %s. %s"),
            RemainingTemporaryPackages.Num(),
            *RemainingTemporaryPackages[0],
            UnloadError.IsEmpty()
                ? TEXT("The mounts were not removed underneath live objects.")
                : *UnloadError.ToString());
        return;
    }

    for (const FSourceRoot& Root : SourceRoots)
    {
        if (AssetRegistry)
        {
            AssetRegistry->RemovePath(Root.TemporaryVirtualRoot);

            TArray<FAssetData> RemainingAssets;
            AssetRegistry->GetAssetsByPath(FName(*Root.TemporaryVirtualRoot), RemainingAssets, true);

            if (!RemainingAssets.IsEmpty())
            {
                UE_LOG(
                    LogUAssetImporter,
                    Warning,
                    TEXT("Asset Registry still contains %d temporary asset record(s) under '%s' after path removal."),
                    RemainingAssets.Num(),
                    *Root.TemporaryVirtualRoot);
            }
        }
    }

    for (const FSourceRoot& Root : SourceRoots)
    {
        if (!Root.bMountRegistered)
        {
            continue;
        }

        const FString VirtualRoot =
            UAssetImporterPrivate::EnsureTrailingSlash(Root.TemporaryVirtualRoot);

        const FString PhysicalRoot =
            UAssetImporterPrivate::EnsureTrailingSlash(
                NormalizeFilePath(Root.ContentRoot));

        FPackageName::UnRegisterMountPoint(VirtualRoot, PhysicalRoot);
    }

    SourceRoots.Reset();
    OriginalPackageToFile.Reset();
    OriginalPackageToTemporaryPackage.Reset();
    TemporaryPackageToFile.Reset();
    TemporaryPackageToOriginalPackage.Reset();
    AssetDataByTemporaryPackage.Reset();
    HardDependenciesByTemporaryPackage.Reset();
    SoftDependenciesByTemporaryPackage.Reset();
}

FString FUAssetImporterService::FindContentRoot(const FString& FilePath) const
{
    FString CurrentDirectory = FPaths::GetPath(FilePath);
    while (!CurrentDirectory.IsEmpty())
    {
        if (FPaths::GetCleanFilename(CurrentDirectory).Equals(TEXT("Content"), ESearchCase::IgnoreCase))
        {
            return NormalizeFilePath(CurrentDirectory);
        }

        const FString ParentDirectory = FPaths::GetPath(CurrentDirectory);
        if (ParentDirectory == CurrentDirectory)
        {
            break;
        }
        CurrentDirectory = ParentDirectory;
    }

    return NormalizeFilePath(FPaths::GetPath(FilePath));
}

FString FUAssetImporterService::DetermineOriginalVirtualRoot(const FString& ContentRoot, bool& bOutIsPluginRoot) const
{
    bOutIsPluginRoot = false;
    const FString ParentDirectory = FPaths::GetPath(ContentRoot);
    TArray<FString> PluginDescriptors;
    IFileManager::Get().FindFiles(PluginDescriptors, *FPaths::Combine(ParentDirectory, TEXT("*.uplugin")), true, false);
    if (!PluginDescriptors.IsEmpty())
    {
        bOutIsPluginRoot = true;
        return TEXT("/") + FPaths::GetBaseFilename(PluginDescriptors[0]);
    }

    if (FPaths::GetCleanFilename(ParentDirectory).Equals(TEXT("Engine"), ESearchCase::IgnoreCase))
    {
        return TEXT("/Engine");
    }

    return TEXT("/Game");
}

FString FUAssetImporterService::MakePackageName(const FString& VirtualRoot, const FString& ContentRoot, const FString& FilePath) const
{
    FString RelativePath = NormalizeFilePath(FilePath);
    const FString ContentRootWithSlash = UAssetImporterPrivate::EnsureTrailingSlash(ContentRoot);
    FPaths::MakePathRelativeTo(RelativePath, *ContentRootWithSlash);
    RelativePath = FPaths::Combine(FPaths::GetPath(RelativePath), FPaths::GetBaseFilename(RelativePath));
    RelativePath.ReplaceInline(TEXT("\\"), TEXT("/"));
    RelativePath.RemoveFromStart(TEXT("./"));
    return UAssetImporterPrivate::NormalizePackageName(VirtualRoot + TEXT("/") + RelativePath);
}

FString FUAssetImporterService::MakeTemporaryPackageName(const FSourceRoot& Root, const FString& FilePath) const
{
    return MakePackageName(Root.TemporaryVirtualRoot, Root.ContentRoot, FilePath);
}

FString FUAssetImporterService::NormalizeFilePath(const FString& Path) const
{
    FString Normalized = FPaths::ConvertRelativePathToFull(Path);
    FPaths::NormalizeFilename(Normalized);
    return Normalized;
}

FString FUAssetImporterService::FindSourceFileForPackage(const FString& PackageName) const
{
    if (const FString* File = OriginalPackageToFile.Find(PackageName))
    {
        return *File;
    }

    if (const FString* File = TemporaryPackageToFile.Find(PackageName))
    {
        return *File;
    }

    for (const FSourceRoot& Root : SourceRoots)
    {
        const FString OriginalPrefix = UAssetImporterPrivate::EnsureTrailingSlash(Root.OriginalVirtualRoot);
        const FString TemporaryPrefix = UAssetImporterPrivate::EnsureTrailingSlash(Root.TemporaryVirtualRoot);
        FString RelativePackagePath;

        if (PackageName.StartsWith(TemporaryPrefix, ESearchCase::IgnoreCase))
        {
            RelativePackagePath = PackageName.Mid(TemporaryPrefix.Len());
        }
        else if (PackageName.StartsWith(OriginalPrefix, ESearchCase::IgnoreCase))
        {
            RelativePackagePath = PackageName.Mid(OriginalPrefix.Len());
        }
        else
        {
            continue;
        }

        const FString CandidateFile = NormalizeFilePath(
            FPaths::Combine(Root.ContentRoot, RelativePackagePath + TEXT(".uasset")));
        if (FPaths::FileExists(CandidateFile))
        {
            return CandidateFile;
        }
    }

    return FString();
}

FString FUAssetImporterService::FindTemporaryPackageForPackage(const FString& PackageName) const
{
    if (const FString* TemporaryPackage = OriginalPackageToTemporaryPackage.Find(PackageName))
    {
        return *TemporaryPackage;
    }

    if (TemporaryPackageToFile.Contains(PackageName))
    {
        return PackageName;
    }

    for (const FSourceRoot& Root : SourceRoots)
    {
        const FString OriginalPrefix = UAssetImporterPrivate::EnsureTrailingSlash(Root.OriginalVirtualRoot);
        const FString TemporaryPrefix = UAssetImporterPrivate::EnsureTrailingSlash(Root.TemporaryVirtualRoot);

        if (PackageName.StartsWith(TemporaryPrefix, ESearchCase::IgnoreCase))
        {
            return PackageName;
        }

        if (!PackageName.StartsWith(OriginalPrefix, ESearchCase::IgnoreCase))
        {
            continue;
        }

        const FString RelativePackagePath = PackageName.Mid(OriginalPrefix.Len());
        const FString CandidateFile = NormalizeFilePath(
            FPaths::Combine(Root.ContentRoot, RelativePackagePath + TEXT(".uasset")));
        if (FPaths::FileExists(CandidateFile))
        {
            return MakeTemporaryPackageName(Root, CandidateFile);
        }
    }

    return FString();
}

bool FUAssetImporterService::IsTargetPackageAvailable(const FString& PackageName) const
{
    const FString NormalizedPackageName = UAssetImporterPrivate::NormalizePackageName(PackageName);
    if (UAssetImporterPrivate::IsNativeOrEnginePackage(NormalizedPackageName))
    {
        return true;
    }

    FString ExistingFilename;
    if (FPackageName::DoesPackageExist(NormalizedPackageName, &ExistingFilename, false)
        && !ExistingFilename.IsEmpty()
        && FPaths::FileExists(ExistingFilename))
    {
        return true;
    }

    // Resolve enabled content plugins by their actual mounted virtual root.
    // The mount name is not guaranteed to be identical to the plugin name.
    if (!NormalizedPackageName.StartsWith(TEXT("/"), ESearchCase::CaseSensitive))
    {
        return false;
    }

    const TArray<TSharedRef<IPlugin>> ContentPlugins = IPluginManager::Get().GetEnabledPluginsWithContent();
    for (const TSharedRef<IPlugin>& Plugin : ContentPlugins)
    {
        if (!Plugin->IsEnabled() || !Plugin->IsMounted() || !Plugin->CanContainContent())
        {
            continue;
        }

        FString MountedAssetPath = Plugin->GetMountedAssetPath();
        if (MountedAssetPath.IsEmpty())
        {
            continue;
        }

        MountedAssetPath = UAssetImporterPrivate::NormalizePackageName(MountedAssetPath);
        const FString MountedAssetPathPrefix = UAssetImporterPrivate::EnsureTrailingSlash(MountedAssetPath);
        if (!NormalizedPackageName.StartsWith(MountedAssetPathPrefix, ESearchCase::IgnoreCase))
        {
            continue;
        }

        const FString RelativePackagePath = NormalizedPackageName.Mid(MountedAssetPathPrefix.Len());
        const FString PluginContentDirectory = Plugin->GetContentDir();
        const FString AssetFilename = FPaths::Combine(PluginContentDirectory, RelativePackagePath + TEXT(".uasset"));
        const FString MapFilename = FPaths::Combine(PluginContentDirectory, RelativePackagePath + TEXT(".umap"));
        if (FPaths::FileExists(AssetFilename) || FPaths::FileExists(MapFilename))
        {
            return true;
        }
    }

    return false;
}

bool FUAssetImporterService::GetDestinationBasePath(
    const FString& PackageName,
    FString& OutBasePath,
    FString& OutDestinationRoot,
    FString& OutProblem) const
{
    const FString NormalizedPackageName = UAssetImporterPrivate::NormalizePackageName(PackageName);
    if (!FPackageName::IsValidLongPackageName(NormalizedPackageName, false))
    {
        OutProblem = FString::Printf(TEXT("Invalid package path: %s"), *PackageName);
        return false;
    }

    FString RootName;
    FString RelativePath;
    const int32 FirstSlash = NormalizedPackageName.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart);
    const int32 RootEnd = NormalizedPackageName.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, FirstSlash + 1);
    if (FirstSlash == INDEX_NONE || RootEnd == INDEX_NONE)
    {
        OutProblem = FString::Printf(TEXT("Could not resolve package root: %s"), *PackageName);
        return false;
    }

    RootName = NormalizedPackageName.Mid(FirstSlash + 1, RootEnd - FirstSlash - 1);
    RelativePath = NormalizedPackageName.Mid(RootEnd + 1);
    FString ContentRoot;

    if (RootName.Equals(TEXT("Game"), ESearchCase::CaseSensitive))
    {
        ContentRoot = FPaths::ProjectContentDir();
    }
    else if (RootName.Equals(TEXT("Engine"), ESearchCase::CaseSensitive))
    {
        OutProblem = FString::Printf(TEXT("Importing directly into engine content is not supported: %s"), *PackageName);
        return false;
    }
    else
    {
        const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(RootName);
        if (!Plugin.IsValid() || !Plugin->CanContainContent())
        {
            OutProblem = FString::Printf(TEXT("The destination plugin mount '/%s/' is not available in this project."), *RootName);
            return false;
        }
        ContentRoot = Plugin->GetContentDir();
    }

    OutDestinationRoot = NormalizeFilePath(ContentRoot);
    OutBasePath = NormalizeFilePath(FPaths::Combine(ContentRoot, RelativePath));
    return true;
}

TArray<FString> FUAssetImporterService::GetPackageFiles(const FString& BasePath) const
{
    TArray<FString> FullPaths;
    for (const TCHAR* Suffix : {TEXT(".uasset"), TEXT(".uexp"), TEXT(".ubulk"), TEXT(".uptnl"), TEXT(".m.ubulk"), TEXT(".upayload")})
    {
        const FString Component = BasePath + Suffix;
        if (FPaths::FileExists(Component))
        {
            FullPaths.Add(Component);
        }
    }
    FullPaths.Sort();
    return FullPaths;
}

bool FUAssetImporterService::CopyFileVerified(const FString& Source, const FString& Destination, FString& OutProblem) const
{
    if (!FPaths::FileExists(Source))
    {
        OutProblem = TEXT("Source file does not exist.");
        return false;
    }

    const bool bDestinationExists = FPaths::FileExists(Destination);
    UAssetImporterPrivate::IgnoreAutoReimportWrite(Destination);
    UAssetImporterPrivate::IgnoreAutoReimportChange(Destination, bDestinationExists);
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Destination), true);

    const uint32 CopyResult = IFileManager::Get().Copy(*Destination, *Source, true, true, false);
    UAssetImporterPrivate::IgnoreAutoReimportWrite(Destination);
    if (CopyResult != COPY_OK)
    {
        OutProblem = FString::Printf(TEXT("File manager returned copy code %u."), CopyResult);
        return false;
    }

    if (!FPaths::FileExists(Destination)
        || IFileManager::Get().FileSize(*Destination) != IFileManager::Get().FileSize(*Source))
    {
        OutProblem = TEXT("The destination file does not match the source file size after copying.");
        return false;
    }
    return true;
}

void FUAssetImporterService::ReportIssues(
    const TArray<FString>& Warnings,
    const TArray<FString>& Errors,
    const FUAssetImporterResult& Result,
    const bool bShowMessageLog,
    const bool bLogSummary) const
{
    if (bLogSummary)
    {
        UE_LOG(LogUAssetImporter, Display, TEXT("Import summary: %d imported, %d skipped, %d problems."),
            Result.ImportedCount, Result.SkippedCount, Errors.Num() + Warnings.Num());
    }

    if (Errors.IsEmpty() && Warnings.IsEmpty())
    {
        return;
    }

    FMessageLog MessageLog(UAssetImporterPrivate::MessageLogName);
    if (bShowMessageLog)
    {
        MessageLog.NewPage(FText::FromString(FDateTime::Now().ToString(TEXT("UAsset Import %Y-%m-%d %H-%M-%S"))));
    }

    for (const FString& Warning : Warnings)
    {
        UE_LOG(LogUAssetImporter, Warning, TEXT("%s"), *Warning);
        if (bShowMessageLog)
        {
            MessageLog.Warning(FText::FromString(Warning));
        }
    }
    for (const FString& Error : Errors)
    {
        UE_LOG(LogUAssetImporter, Error, TEXT("%s"), *Error);
        if (bShowMessageLog)
        {
            MessageLog.Error(FText::FromString(Error));
        }
    }

    if (bShowMessageLog)
    {
        MessageLog.Open(Errors.IsEmpty() ? EMessageSeverity::Warning : EMessageSeverity::Error, true);
    }
}
