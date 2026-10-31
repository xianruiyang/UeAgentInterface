#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Templates/Function.h"

class UWorld;

namespace UE::UAI::AssetDurable
{
	using FPersistedWorldLoadError = UE::UAI::Contract::FDiagnostic;

	// A published package header from a durable owner's verified inventory.
	// This neutral view lets both durable transaction owners use the same loader.
	struct FPersistedWorldPackage
	{
		FString PackageName;
		FString FinalPath;
		bool bDeleted = false;
	};

	class UEAGENTINTERFACE_API FPersistedWorldReferenceMap final
	{
	public:
		FPersistedWorldReferenceMap(
			FString InSourceWorldRef,
			FString InSnapshotWorldRef);

		bool IsValid() const;
		bool TryMapReference(
			const FString& SourceRef,
			FString& OutSnapshotRef) const;

	private:
		FString SourceWorldRef;
		FString SnapshotWorldRef;
	};

	using FPersistedWorldObserver =
		TFunctionRef<bool(
			UWorld& World,
			const FPersistedWorldReferenceMap& ReferenceMap,
			FPersistedWorldLoadError& OutError)>;

	/**
	 * Loads the exact persisted Map and any receipt-owned External Actor package
	 * into an isolated transient namespace, invokes one semantic observer, and
	 * unloads every isolated package before returning.
	 */
	UEAGENTINTERFACE_API bool TryObservePersistedWorld(
		const TArray<FDurableArtifact>& Artifacts,
		FPersistedWorldObserver Observer,
		FPersistedWorldLoadError& OutError);

	UEAGENTINTERFACE_API bool TryObservePersistedWorld(
		const TArray<FPersistedWorldPackage>& Packages,
		FPersistedWorldObserver Observer,
		FPersistedWorldLoadError& OutError);
}
