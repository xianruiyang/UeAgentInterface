#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "OperationOwners/BlueprintComponent/UeAgentBlueprintComponentCommandService.h"

namespace UE::UAI::CommandHandlers::BlueprintComponent
{
	UEAGENTINTERFACE_API bool TryCreateBlueprintComponentCommandHandlers(
		TSharedRef<OperationOwners::BlueprintComponent::
			IUeAgentBlueprintComponentCommandService> Service,
		TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>&
			OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError);
}
