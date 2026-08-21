#pragma once

#include "CoreMinimal.h"

namespace UE::UAI::DomainAdapters::AssetSimple
{
	/** Exact T37 inventory slice accepted by the AssetSimple mechanics seam. */
	UEAGENTINTERFACE_API const TArray<FString>&
	GetAssetSimpleMechanicsCommandIds();

	UEAGENTINTERFACE_API bool IsAssetSimpleMechanicsCommand(
		const FString& CommandId);

	/** Domain capability granted only after the public uai:operate check. */
	UEAGENTINTERFACE_API const TArray<FString>&
	GetAssetSimpleRequiredAuthorityScopes();

	/** Returns the official plugin required by this exact command, if any. */
	UEAGENTINTERFACE_API FString GetAssetSimpleRequiredOfficialPlugin(
		const FString& CommandId);
}
