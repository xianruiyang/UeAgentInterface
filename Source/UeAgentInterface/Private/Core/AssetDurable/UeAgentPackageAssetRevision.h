#pragma once

#include "Containers/UnrealString.h"

namespace UE::UAI::AssetDurable
{
	// One durable package revision authority is shared by operation producers and
	// consumers that hand revisions across command boundaries.
	UEAGENTINTERFACE_API bool TryResolvePackageAssetFilename(
		const FString& TargetRef,
		FString& OutFilename);
	UEAGENTINTERFACE_API FString ReadPackageAssetRevision(
		const FString& TargetRef);
}
