#pragma once

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "DomainAdapters/Query/UeAgentQueryService.h"

namespace UE::UAI::CommandHandlers::Query
{
	UEAGENTINTERFACE_API bool TryCreateQueryHandlers(
		TSharedRef<DomainAdapters::Query::IUeAgentQueryService> Service,
		TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>&
			OutHandlers,
		FString& OutError);
}
