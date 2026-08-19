#pragma once

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/AnimationRigGraph/UeAgentAnimationRigGraphDomainService.h"

namespace UE::UAI::CommandHandlers::AnimationRigGraph
{
	UEAGENTINTERFACE_API bool TryHandleDomainRequest(
		const HjsonWorkspace::FImportInitializationDomainRequest& Request,
		HjsonWorkspace::FImportInitializationDomainResponse& OutResponse,
		HjsonWorkspace::FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryCreateAnimationRigGraphCandidateCommandHandlers(
		TSharedRef<OperationOwners::AnimationRigGraph::IUeAgentAnimationRigGraphCandidateCommandService> Service,
		TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError);
}
