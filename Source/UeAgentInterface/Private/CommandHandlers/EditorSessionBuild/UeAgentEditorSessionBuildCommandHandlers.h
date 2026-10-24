#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "Core/Contract/UeAgentContract.h"

namespace UE::UAI::CommandHandlers::EditorSessionBuild
{
	UEAGENTINTERFACE_API bool TryCreateHandlers(
		TSharedRef<Contract::IUeAgentOperationApplicationPort> OperationPort,
		TSharedRef<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry,
		TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError);
}
