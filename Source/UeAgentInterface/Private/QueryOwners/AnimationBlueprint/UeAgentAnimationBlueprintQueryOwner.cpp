#include "QueryOwners/AnimationBlueprint/UeAgentAnimationBlueprintQueryOwner.h"

namespace UE::UAI::QueryOwners::AnimationBlueprint
{
	bool IsReadOnlyContentGraphCommand(const FString& CommandId)
	{
		return CommandId.StartsWith(TEXT("content_graph."))
			&& CommandId != TEXT("content_graph.import_workspace");
	}
}
