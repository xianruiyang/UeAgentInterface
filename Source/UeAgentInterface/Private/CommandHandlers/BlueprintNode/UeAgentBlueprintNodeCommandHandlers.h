#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "OperationOwners/BlueprintNode/UeAgentBlueprintNodeCommandService.h"

namespace UE::UAI::CommandHandlers::BlueprintNode
{
	UEAGENTINTERFACE_API bool TryCreateBlueprintNodeCommandHandlers(
		TSharedRef<OperationOwners::BlueprintNode::
			IUeAgentBlueprintNodeCommandService> Service,
		TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>&
			OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError);
}

