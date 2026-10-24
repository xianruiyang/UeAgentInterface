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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutFilename.Reset();
		OutError.Reset();
		const FString PackageName =
			FPackageName::ObjectPathToPackageName(TargetRef);
		if (!FPackageName::IsValidLongPackageName(PackageName)
			|| FPackageName::GetPackageMountPoint(PackageName).IsNone())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_package_name_invalid"), TEXT("The package name is invalid or its mount point is unavailable."), TEXT("package_header"));
            OutError.TargetPath = TargetRef;
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
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_package_path_unavailable"), TEXT("The package name could not be resolved to a file path."), TEXT("package_header"));
            OutError.TargetPath = TargetRef;
			return false;
		}

		const bool bAssetExists =
			IFileManager::Get().FileExists(*AssetFilename);
		const bool bMapExists =
			IFileManager::Get().FileExists(*MapFilename);
		if (bAssetExists && bMapExists)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_package_extension_ambiguous"), TEXT("Both asset and map files exist for this package name."), TEXT("package_header"));
            OutError.TargetPath = TargetRef;
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
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_package_extension_kind_mismatch"), TEXT("The package file kind does not match the loaded or requested asset kind."), TEXT("package_header"));
            OutError.TargetPath = TargetRef;
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
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_package_header_kind_evidence_required"), TEXT("The package kind cannot be determined from a loaded asset or existing file."), TEXT("package_header"));
            OutError.TargetPath = TargetRef;
				return false;
			}
		}
		if ((ExistingKind.IsSet() && ExistingKind.GetValue() != ResolvedKind)
			|| (LoadedKind.IsSet() && LoadedKind.GetValue() != ResolvedKind))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_package_extension_kind_mismatch"), TEXT("The package file kind does not match the loaded or requested asset kind."), TEXT("package_header"));
            OutError.TargetPath = TargetRef;
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutFilename.Reset();
		OutError.Reset();
		if (InOutTopLevelAsset
			&& InOutTopLevelAsset->GetOutermost() != &Package)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_package_top_level_mismatch"), TEXT("The top-level asset belongs to a different package."), TEXT("package_header"));
            OutError.TargetPath = Package.GetName();
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
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_package_top_level_asset_missing"), TEXT("The loaded package has no top-level asset to save."), TEXT("package_header"));
            OutError.TargetPath = Package.GetName();
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
				OutError))
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
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("durable_package_extension_kind_mismatch"), TEXT("The package file kind does not match the loaded or requested asset kind."), TEXT("package_header"));
            OutError.TargetPath = Package.GetName();
			return false;
		}
		return true;
	}
}
