#pragma once

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "Core/Contract/UeAgentContract.h"

namespace UE::UAI::CommandHandlers::EditorSessionBuild
{
	UEAGENTINTERFACE_API bool TryCreateHandlers(
		TSharedRef<Contract::IUeAgentOperationApplicationPort> OperationPort,
		TSharedRef<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry,
		TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError);
}
