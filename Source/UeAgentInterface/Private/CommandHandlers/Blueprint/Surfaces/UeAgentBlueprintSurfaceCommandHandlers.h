#pragma once

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/Blueprint/Surfaces/UeAgentBlueprintSurfaceDomainService.h"

namespace UE::UAI::CommandHandlers::Blueprint::Surfaces
{
	UEAGENTINTERFACE_API bool TryHandleDomainRequest(
		const HjsonWorkspace::FImportInitializationDomainRequest& Request,
		HjsonWorkspace::FImportInitializationDomainResponse& OutResponse,
		HjsonWorkspace::FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryCreateBlueprintSurfaceCommandHandlers(
		TSharedRef<OperationOwners::Blueprint::Surfaces::
			IUeAgentBlueprintSurfaceCommandService> Service,
		TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError);
}
