#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "DomainAdapters/Query/UeAgentQueryService.h"

namespace UE::UAI::CommandHandlers::Query
{
	UEAGENTINTERFACE_API bool TryCreateQueryHandlers(
		TSharedRef<DomainAdapters::Query::IUeAgentQueryService> Service,
		TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>&
			OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError);
}
