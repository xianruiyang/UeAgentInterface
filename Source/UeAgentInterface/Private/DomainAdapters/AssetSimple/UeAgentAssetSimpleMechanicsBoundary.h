#pragma once

#include "CoreMinimal.h"

namespace UE::UAI::DomainAdapters::AssetSimple
{
	/** Exact T37 inventory slice accepted by the AssetSimple mechanics seam. */
	UEAGENTINTERFACE_API const TArray<FString>&
	GetAssetSimpleMechanicsCommandIds();

	/** Non-public routes delegated into the shared SimpleAuthoring lifecycle. */
	UEAGENTINTERFACE_API const TArray<FString>&
	GetAssetSimpleDelegatedLifecycleCommandIds();

	/** Public mechanics plus delegated lifecycle-only identities. */
	UEAGENTINTERFACE_API const TArray<FString>&
	GetAssetSimpleLifecycleCommandIds();

	UEAGENTINTERFACE_API bool IsAssetSimpleMechanicsCommand(
		const FString& CommandId);

	UEAGENTINTERFACE_API bool IsAssetSimpleLifecycleCommand(
		const FString& CommandId);

	/** Domain capability granted only after the public uai:operate check. */
	UEAGENTINTERFACE_API const TArray<FString>&
	GetAssetSimpleRequiredAuthorityScopes();

	/** Returns the official plugin required by this exact command, if any. */
	UEAGENTINTERFACE_API FString GetAssetSimpleRequiredOfficialPlugin(
		const FString& CommandId);
}
