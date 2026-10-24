#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "OperationOwners/ExternalImport/SkinWeight/UeAgentSkinWeightImportCommandService.h"

namespace UE::UAI::CommandHandlers::ExternalImport::SkinWeight
{
	UEAGENTINTERFACE_API bool TryCreateSkinWeightImportCommandHandlers(
		TSharedRef<OperationOwners::ExternalImport::SkinWeight::
			IUeAgentSkinWeightImportCommandService> Service,
		TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>&
			OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError);
}
