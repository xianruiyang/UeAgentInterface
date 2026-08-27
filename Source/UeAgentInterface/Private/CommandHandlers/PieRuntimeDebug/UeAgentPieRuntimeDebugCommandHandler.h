#pragma once

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "Core/Contract/UeAgentContract.h"

namespace UE::UAI::CommandHandlers::PieRuntimeDebug
{
	UEAGENTINTERFACE_API bool TryCreateHandler(
		TSharedRef<Contract::IUeAgentOperationApplicationPort> OperationPort,
		TSharedPtr<CommandEdge::IUeAgentExactCommandHandler>& OutHandler,
		FString& OutError);
}
