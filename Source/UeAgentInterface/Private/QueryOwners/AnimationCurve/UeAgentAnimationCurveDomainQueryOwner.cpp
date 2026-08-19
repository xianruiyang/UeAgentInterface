#include "QueryOwners/AnimationCurve/UeAgentAnimationCurveDomainQueryOwner.h"

namespace UE::UAI::QueryOwners::AnimationCurve
{
	bool IsReadOnlyContentGraphCommand(const FString& CommandId)
	{
		return CommandId.StartsWith(TEXT("content_graph."))
			&& CommandId != TEXT("content_graph.import_workspace");
	}
}
