#include "QueryOwners/AiBehavior/UeAgentAiBehaviorDomainQueryOwner.h"

namespace UE::UAI::QueryOwners::AiBehavior
{
	bool IsReadOnlyContentGraphCommand(const FString& CommandId)
	{
		return CommandId.StartsWith(TEXT("content_graph."))
			&& CommandId != TEXT("content_graph.import_workspace");
	}
}
