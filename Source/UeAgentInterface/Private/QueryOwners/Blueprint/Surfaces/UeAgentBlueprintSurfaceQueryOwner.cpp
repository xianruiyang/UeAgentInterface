#include "QueryOwners/Blueprint/Surfaces/UeAgentBlueprintSurfaceQueryOwner.h"

namespace UE::UAI::QueryOwners::Blueprint::Surfaces
{
	bool IsReadOnlyContentGraphCommand(const FString& CommandId)
	{
		return CommandId.StartsWith(TEXT("content_graph."))
			&& CommandId != TEXT("content_graph.import_workspace");
	}
}
