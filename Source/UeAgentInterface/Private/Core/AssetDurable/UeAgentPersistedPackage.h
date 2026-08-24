#pragma once

#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Templates/Function.h"

class UPackage;

namespace UE::UAI::AssetDurable
{
	struct UEAGENTINTERFACE_API FPersistedPackageLoadError final
	{
		FString Code;
		FString Detail;

		bool IsPresent() const { return !Code.IsEmpty(); }
	};

	using FPersistedPackageObserver = TFunctionRef<bool(
		UPackage& SnapshotPackage,
		FPersistedPackageLoadError& OutError)>;

	/**
	 * Loads one exact receipt-owned persisted asset package into an isolated
	 * transient namespace, observes it, and unloads the isolated instance.
	 * The source package and its live editor objects are never unloaded.
	 */
	UEAGENTINTERFACE_API bool TryObservePersistedPackage(
		const TArray<FDurableArtifact>& Artifacts,
		const FString& SourcePackageName,
		FPersistedPackageObserver Observer,
		FPersistedPackageLoadError& OutError);
}
