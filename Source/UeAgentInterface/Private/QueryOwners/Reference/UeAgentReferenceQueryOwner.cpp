#include "QueryOwners/Reference/UeAgentReferenceQueryOwner.h"

namespace UE::UAI::QueryOwners::Reference
{
	const TArray<FString>& GetStateLayers()
	{
		static const TArray<FString> Values = {
			TEXT("persisted_asset_state"),
		};
		return Values;
	}

	const TArray<FString>& GetIndependentReadbackFlows()
	{
		static const TArray<FString> Values = {
			TEXT("reference.asset_read"),
			TEXT("reference.origin.resolve"),
			TEXT("asset.registry_record"),
		};
		return Values;
	}
}
