#include "Core/AssetDurable/UeAgentPackageAssetRevision.h"

#include "Core/AssetDurable/UeAgentPackageHeader.h"
#include "HAL/FileManager.h"
#include "Misc/SecureHash.h"

namespace UE::UAI::AssetDurable
{
	FString ReadPackageAssetRevision(const FString& TargetRef)
	{
		FString Filename;
		FString ErrorCode;
		if (!TryResolvePackageHeaderFilename(
				TargetRef,
				Filename,
				ErrorCode)
			|| !IFileManager::Get().FileExists(*Filename))
		{
			return FString();
		}
		return TEXT("package-md5:")
			+ LexToString(FMD5Hash::HashFile(*Filename)).ToLower();
	}
}
