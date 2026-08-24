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
		const EPackageHeaderKind HeaderKind,
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

		TOptional<EPackageHeaderKind> ExistingKind;
		if (bAssetExists)
		{
			ExistingKind = EPackageHeaderKind::Asset;
		}
		else if (bMapExists)
		{
			ExistingKind = EPackageHeaderKind::Map;
		}

		TOptional<EPackageHeaderKind> LoadedKind;
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
			if (LoadedPackage->ContainsMap()
				|| (TopLevelAsset && TopLevelAsset->IsA<UWorld>()))
			{
				LoadedKind = EPackageHeaderKind::Map;
			}
			else if (TopLevelAsset)
			{
				LoadedKind = EPackageHeaderKind::Asset;
			}
		}
		if (ExistingKind.IsSet() && LoadedKind.IsSet()
			&& ExistingKind.GetValue() != LoadedKind.GetValue())
		{
			OutErrorCode = TEXT("durable_package_extension_kind_mismatch");
			return false;
		}
		EPackageHeaderKind ResolvedKind = HeaderKind;
		if (HeaderKind == EPackageHeaderKind::EvidenceRequired)
		{
			if (ExistingKind.IsSet())
			{
				ResolvedKind = ExistingKind.GetValue();
			}
			else if (LoadedKind.IsSet())
			{
				ResolvedKind = LoadedKind.GetValue();
			}
			else
			{
				OutErrorCode = TEXT("durable_package_header_kind_evidence_required");
				return false;
			}
		}
		if ((ExistingKind.IsSet() && ExistingKind.GetValue() != ResolvedKind)
			|| (LoadedKind.IsSet() && LoadedKind.GetValue() != ResolvedKind))
		{
			OutErrorCode = TEXT("durable_package_extension_kind_mismatch");
			return false;
		}
		OutFilename = ResolvedKind == EPackageHeaderKind::Map
			? MoveTemp(MapFilename) : MoveTemp(AssetFilename);
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
				bMapPackage ? EPackageHeaderKind::Map : EPackageHeaderKind::Asset,
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
