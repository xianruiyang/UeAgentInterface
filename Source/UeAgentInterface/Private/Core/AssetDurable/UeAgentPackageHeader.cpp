#include "Core/AssetDurable/UeAgentPackageHeader.h"

#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::AssetDurable
{
	bool TryResolvePackageHeaderFilename(
		const FString& TargetRef,
		FString& OutFilename,
		FString& OutErrorCode)
	{
		OutFilename.Reset();
		OutErrorCode.Reset();
		const FString PackageName =
			FPackageName::ObjectPathToPackageName(TargetRef);
		if (!FPackageName::IsValidLongPackageName(PackageName)
			|| FPackageName::GetPackageMountPoint(PackageName).IsNone())
		{
			OutErrorCode = TEXT("durable_package_name_invalid");
			return false;
		}

		FString AssetFilename;
		FString MapFilename;
		if (!FPackageName::TryConvertLongPackageNameToFilename(
				PackageName,
				AssetFilename,
				FPackageName::GetAssetPackageExtension())
			|| !FPackageName::TryConvertLongPackageNameToFilename(
				PackageName,
				MapFilename,
				FPackageName::GetMapPackageExtension()))
		{
			OutErrorCode = TEXT("durable_package_path_unavailable");
			return false;
		}

		const bool bAssetExists =
			IFileManager::Get().FileExists(*AssetFilename);
		const bool bMapExists =
			IFileManager::Get().FileExists(*MapFilename);
		if (bAssetExists && bMapExists)
		{
			OutErrorCode = TEXT("durable_package_extension_ambiguous");
			return false;
		}

		UPackage* const LoadedPackage =
			FindPackage(nullptr, *PackageName);
		if (LoadedPackage)
		{
			UObject* TopLevelAsset =
				LoadedPackage->FindAssetInPackage();
			if (!TopLevelAsset)
			{
				TopLevelAsset =
					UWorld::FindWorldInPackage(LoadedPackage);
			}
			const bool bLoadedMap = LoadedPackage->ContainsMap()
				|| (TopLevelAsset && TopLevelAsset->IsA<UWorld>());
			if (bLoadedMap && bAssetExists)
			{
				OutErrorCode =
					TEXT("durable_package_extension_kind_mismatch");
				return false;
			}
			if (!bLoadedMap && bMapExists)
			{
				OutErrorCode =
					TEXT("durable_package_extension_kind_mismatch");
				return false;
			}
			OutFilename = bLoadedMap
				? MoveTemp(MapFilename)
				: MoveTemp(AssetFilename);
			return true;
		}

		OutFilename = bMapExists
			? MoveTemp(MapFilename)
			: MoveTemp(AssetFilename);
		return true;
	}

	bool TryPrepareLoadedPackageHeaderForSave(
		UPackage& Package,
		UObject*& InOutTopLevelAsset,
		FString& OutFilename,
		FString& OutErrorCode)
	{
		OutFilename.Reset();
		OutErrorCode.Reset();
		if (InOutTopLevelAsset
			&& InOutTopLevelAsset->GetOutermost() != &Package)
		{
			OutErrorCode = TEXT("durable_package_top_level_mismatch");
			return false;
		}

		if (!InOutTopLevelAsset)
		{
			InOutTopLevelAsset = Package.FindAssetInPackage();
		}
		if (!InOutTopLevelAsset)
		{
			InOutTopLevelAsset = UWorld::FindWorldInPackage(&Package);
		}
		if (!InOutTopLevelAsset)
		{
			OutErrorCode =
				TEXT("durable_package_top_level_asset_missing");
			return false;
		}
		const bool bMapPackage = Package.ContainsMap()
			|| InOutTopLevelAsset->IsA<UWorld>();
		if (bMapPackage)
		{
			Package.SetPackageFlags(PKG_ContainsMap);
		}

		if (!TryResolvePackageHeaderFilename(
				Package.GetName(),
				OutFilename,
				OutErrorCode))
		{
			return false;
		}

		const FString ExpectedExtension = bMapPackage
			? FPackageName::GetMapPackageExtension()
			: FPackageName::GetAssetPackageExtension();
		if (!OutFilename.EndsWith(
				ExpectedExtension,
				ESearchCase::IgnoreCase))
		{
			OutFilename.Reset();
			OutErrorCode = TEXT("durable_package_extension_kind_mismatch");
			return false;
		}
		return true;
	}
}
