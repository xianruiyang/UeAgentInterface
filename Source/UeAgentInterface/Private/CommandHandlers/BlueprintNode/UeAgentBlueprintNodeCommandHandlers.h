#pragma once

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "OperationOwners/BlueprintNode/UeAgentBlueprintNodeCommandService.h"

namespace UE::UAI::CommandHandlers::BlueprintNode
{
	UEAGENTINTERFACE_API bool TryCreateBlueprintNodeCommandHandlers(
		TSharedRef<OperationOwners::BlueprintNode::
			IUeAgentBlueprintNodeCommandService> Service,
		TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>&
			OutHandlers,
		FString& OutError);
}

