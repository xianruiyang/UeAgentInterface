#pragma once

#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Templates/Function.h"

class UWorld;

namespace UE::UAI::AssetDurable
{
	struct UEAGENTINTERFACE_API FPersistedWorldLoadError final
	{
		FString Code;
		FString Detail;

		bool IsPresent() const { return !Code.IsEmpty(); }
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
}
