#pragma once

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/UmgWidget/UeAgentUmgWidgetDomainService.h"

namespace UE::UAI::CommandHandlers::UmgWidget
{
	UEAGENTINTERFACE_API bool TryHandleDomainRequest(
		const HjsonWorkspace::FImportInitializationDomainRequest& Request,
		HjsonWorkspace::FImportInitializationDomainResponse& OutResponse,
		HjsonWorkspace::FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryCreateUmgWidgetCommandHandlers(
		TSharedRef<OperationOwners::UmgWidget::IUeAgentUmgWidgetCommandService> Service,
		TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError);
}
