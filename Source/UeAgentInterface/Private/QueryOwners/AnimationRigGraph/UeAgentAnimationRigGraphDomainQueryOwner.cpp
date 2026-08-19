#include "QueryOwners/AnimationRigGraph/UeAgentAnimationRigGraphDomainQueryOwner.h"

namespace UE::UAI::QueryOwners::AnimationRigGraph
{
	bool IsReadOnlyContentGraphCommand(const FString& CommandId)
	{
		return CommandId.StartsWith(TEXT("content_graph."))
			&& CommandId != TEXT("content_graph.import_workspace");
	}
}
