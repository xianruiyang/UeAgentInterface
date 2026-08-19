#pragma once

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/AnimationBlueprint/UeAgentAnimationBlueprintDomainService.h"

namespace UE::UAI::CommandHandlers::AnimationBlueprint
{
	UEAGENTINTERFACE_API bool TryHandleDomainRequest(
		const HjsonWorkspace::FImportInitializationDomainRequest& Request,
		HjsonWorkspace::FImportInitializationDomainResponse& OutResponse,
		HjsonWorkspace::FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryCreateAnimationBlueprintCommandHandlers(
		TSharedRef<OperationOwners::AnimationBlueprint::
			IUeAgentAnimationBlueprintCommandService> Service,
		TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError);
}
