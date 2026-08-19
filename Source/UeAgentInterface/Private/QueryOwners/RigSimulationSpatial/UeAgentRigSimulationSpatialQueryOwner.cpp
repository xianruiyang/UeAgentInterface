#include "QueryOwners/RigSimulationSpatial/UeAgentRigSimulationSpatialQueryOwner.h"

namespace UE::UAI::QueryOwners::RigSimulationSpatial
{
	bool IsReadOnlyContentGraphCommand(const FString& CommandId)
	{
		return CommandId.StartsWith(TEXT("content_graph."))
			&& CommandId != TEXT("content_graph.import_workspace");
	}
}
