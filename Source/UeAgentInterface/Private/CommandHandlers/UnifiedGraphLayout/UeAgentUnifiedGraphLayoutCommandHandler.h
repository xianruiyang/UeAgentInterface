#pragma once

#include "CommandEdge/UeAgentExactCommandDispatch.h"

namespace UE::UAI::CommandHandlers::UnifiedGraphLayout
{
	UEAGENTINTERFACE_API bool TryCreateHandler(
		TSharedPtr<CommandEdge::IUeAgentExactCommandHandler>& OutHandler,
		FString& OutError);
}
