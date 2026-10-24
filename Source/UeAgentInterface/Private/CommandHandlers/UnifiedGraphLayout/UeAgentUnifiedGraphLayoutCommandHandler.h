#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentExactCommandDispatch.h"

namespace UE::UAI::CommandHandlers::UnifiedGraphLayout
{
	UEAGENTINTERFACE_API bool TryCreateHandler(
		TSharedPtr<CommandEdge::IUeAgentExactCommandHandler>& OutHandler,
		UE::UAI::Contract::FDiagnostic& OutError);
}
