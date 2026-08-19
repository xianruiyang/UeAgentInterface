#pragma once

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "OperationOwners/BlueprintComponent/UeAgentBlueprintComponentCommandService.h"

namespace UE::UAI::CommandHandlers::BlueprintComponent
{
	UEAGENTINTERFACE_API bool TryCreateBlueprintComponentCommandHandlers(
		TSharedRef<OperationOwners::BlueprintComponent::
			IUeAgentBlueprintComponentCommandService> Service,
		TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>&
			OutHandlers,
		FString& OutError);
}
