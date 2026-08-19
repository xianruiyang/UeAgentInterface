#include "QueryOwners/AudioGraph/UeAgentAudioGraphDomainQueryOwner.h"

namespace UE::UAI::QueryOwners::AudioGraph
{
	bool IsReadOnlyContentGraphCommand(const FString& CommandId)
	{
		return CommandId.StartsWith(TEXT("content_graph."))
			&& CommandId != TEXT("content_graph.import_workspace");
	}
}
