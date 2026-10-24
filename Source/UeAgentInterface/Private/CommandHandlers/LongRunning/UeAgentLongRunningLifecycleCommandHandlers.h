#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "OperationOwners/LongRunning/UeAgentLongRunningLifecycleOwner.h"

namespace UE::UAI::CommandHandlers::LongRunning
{
	UEAGENTINTERFACE_API bool TryCreateLongRunningLifecycleCommandHandlers(
		TSharedRef<Contract::IUeAgentOperationApplicationPort> OperationPort,
		TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError);
}
