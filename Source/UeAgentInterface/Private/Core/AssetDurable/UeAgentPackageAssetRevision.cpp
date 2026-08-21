#include "Core/AssetDurable/UeAgentPackageAssetRevision.h"

#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"

namespace UE::UAI::AssetDurable
{
	bool TryResolvePackageAssetFilename(
		const FString& TargetRef,
		FString& OutFilename)
	{
		OutFilename.Reset();
		const FString PackageName =
			FPackageName::ObjectPathToPackageName(TargetRef);
		return FPackageName::IsValidLongPackageName(PackageName)
			&& !FPackageName::GetPackageMountPoint(PackageName).IsNone()
			&& FPackageName::TryConvertLongPackageNameToFilename(
				PackageName,
				OutFilename,
				FPackageName::GetAssetPackageExtension());
	}

	FString ReadPackageAssetRevision(const FString& TargetRef)
	{
		FString Filename;
		if (!TryResolvePackageAssetFilename(TargetRef, Filename)
			|| !IFileManager::Get().FileExists(*Filename))
		{
			return FString();
		}
		return TEXT("package-md5:")
			+ LexToString(FMD5Hash::HashFile(*Filename)).ToLower();
	}
}
