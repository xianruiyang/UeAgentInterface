#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "OperationOwners/ExternalImport/Localization/UeAgentLocalizationImportCommandService.h"

namespace UE::UAI::CommandHandlers::ExternalImport::Localization
{
	UEAGENTINTERFACE_API bool TryCreateLocalizationImportCommandHandlers(
		TSharedRef<OperationOwners::ExternalImport::Localization::
			IUeAgentLocalizationImportCommandService> Service,
		TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError);
}
