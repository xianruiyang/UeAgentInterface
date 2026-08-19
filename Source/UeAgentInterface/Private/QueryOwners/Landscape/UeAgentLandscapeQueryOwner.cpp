#include "QueryOwners/Landscape/UeAgentLandscapeQueryOwner.h"

namespace UE::UAI::QueryOwners::Landscape
{
	const FString& GetPersistedStateLayer()
	{
		static const FString Layer(TEXT("world_persisted_state"));
		return Layer;
	}

	const TArray<FString>& GetIndependentReadbackFlows()
	{
		static const TArray<FString> Flows = {
			TEXT("query.command.detail"),
			TEXT("world actor and landscape persisted-state query"),
		};
		return Flows;
	}
}
