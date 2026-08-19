#include "QueryOwners/WorldSpatial/UeAgentLevelWorldQueryOwner.h"

namespace UE::UAI::QueryOwners::WorldSpatial
{
	bool IsReadOnlyContentGraphCommand(const FString& CommandId)
	{
		return CommandId.StartsWith(TEXT("content_graph."))
			&& CommandId != TEXT("content_graph.import_workspace");
	}
}
