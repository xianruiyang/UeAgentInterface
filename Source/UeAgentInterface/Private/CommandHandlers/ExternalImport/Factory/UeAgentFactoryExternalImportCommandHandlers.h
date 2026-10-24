#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "OperationOwners/ExternalImport/Factory/UeAgentFactoryExternalImportCommandService.h"

namespace UE::UAI::CommandHandlers::ExternalImport::Factory
{
	UEAGENTINTERFACE_API bool TryCreateFactoryExternalImportCommandHandlers(
		TSharedRef<OperationOwners::ExternalImport::Factory::
			IUeAgentFactoryExternalImportCommandService> Service,
		TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>&
			OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError);
}
