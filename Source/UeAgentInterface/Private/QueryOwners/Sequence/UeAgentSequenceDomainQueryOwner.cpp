#include "QueryOwners/Sequence/UeAgentSequenceDomainQueryOwner.h"

namespace UE::UAI::QueryOwners::Sequence
{
	bool IsReadOnlyContentGraphCommand(const FString& CommandId)
	{
		return CommandId.StartsWith(TEXT("content_graph."))
			&& CommandId != TEXT("content_graph.import_workspace");
	}
}
