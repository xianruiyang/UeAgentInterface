#include "Core/AssetDurable/UeAgentPackageAssetRevision.h"

#include "Core/AssetDurable/UeAgentPackageHeader.h"
#include "HAL/FileManager.h"
#include "Misc/SecureHash.h"
#include "Serialization/ArchiveObjectCrc32.h"
#include "UObject/Package.h"

namespace UE::UAI::AssetDurable
{
	FString ReadPackageAssetRevision(const FString& TargetRef)
	{
		FString Filename;
		FString ErrorCode;
		if (!TryResolvePackageHeaderFilename(
				TargetRef,
				EPackageHeaderKind::EvidenceRequired,
				Filename,
				ErrorCode)
			|| !IFileManager::Get().FileExists(*Filename))
		{
			return FString();
		}
		return TEXT("package-md5:")
			+ LexToString(FMD5Hash::HashFile(*Filename)).ToLower();
	}

	FString ReadStableAssetRevision(UObject* Object)
	{
		if (!IsValid(Object))
		{
			return FString();
		}
		UPackage* const Package = Object->GetPackage();
		if (Package && !Package->HasAnyFlags(RF_Transient))
		{
			FString Filename;
			FString ErrorCode;
			if (TryResolvePackageHeaderFilename(
					Package->GetName(),
					EPackageHeaderKind::EvidenceRequired,
					Filename,
					ErrorCode)
				&& IFileManager::Get().FileExists(*Filename))
			{
				return ReadPackageAssetRevision(Package->GetName());
			}
			if (ErrorCode == TEXT("durable_package_extension_ambiguous")
				|| ErrorCode
					== TEXT("durable_package_extension_kind_mismatch"))
			{
				return FString();
			}
		}
		return FString::Printf(
			TEXT("asset-crc32:%08x"),
			FArchiveObjectCrc32().Crc32(Object));
	}
}
