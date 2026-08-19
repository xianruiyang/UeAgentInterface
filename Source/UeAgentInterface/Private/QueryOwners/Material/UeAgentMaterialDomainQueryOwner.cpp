#include "QueryOwners/Material/UeAgentMaterialDomainQueryOwner.h"

namespace UE::UAI::QueryOwners::Material
{
	bool IsReadOnlyContentGraphCommand(const FString& CommandId)
	{
		return CommandId.StartsWith(TEXT("content_graph."))
			&& CommandId != TEXT("content_graph.import_workspace");
	}
}

