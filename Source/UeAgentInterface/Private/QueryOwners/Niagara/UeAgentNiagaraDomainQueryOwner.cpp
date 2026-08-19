#include "QueryOwners/Niagara/UeAgentNiagaraDomainQueryOwner.h"

namespace UE::UAI::QueryOwners::Niagara
{
	bool IsReadOnlyContentGraphCommand(const FString& CommandId)
	{
		return CommandId.StartsWith(TEXT("content_graph."))
			&& CommandId != TEXT("content_graph.import_workspace");
	}
}
