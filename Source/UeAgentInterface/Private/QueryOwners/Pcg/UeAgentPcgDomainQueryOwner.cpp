#include "QueryOwners/Pcg/UeAgentPcgDomainQueryOwner.h"

namespace UE::UAI::QueryOwners::Pcg
{
	bool IsReadOnlyContentGraphCommand(const FString& CommandId)
	{
		return CommandId.StartsWith(TEXT("content_graph."))
			&& CommandId != TEXT("content_graph.import_workspace");
	}
}
