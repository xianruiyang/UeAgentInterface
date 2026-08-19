#include "QueryOwners/DataTableCurveTable/UeAgentDataTableCurveTableDomainQueryOwner.h"

namespace UE::UAI::QueryOwners::DataTableCurveTable
{
	bool IsReadOnlyContentGraphCommand(const FString& CommandId)
	{
		return CommandId.StartsWith(TEXT("content_graph."))
			&& CommandId != TEXT("content_graph.import_workspace");
	}
}
