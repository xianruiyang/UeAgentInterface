#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "OperationOwners/ExternalImport/Localization/UeAgentLocalizationImportCommandService.h"

namespace UE::UAI::Module::CommandRegistration::ExternalImport::Localization
{
	UEAGENTINTERFACE_API bool TryCreateLocalizationImportContribution(
		TSharedRef<OperationOwners::ExternalImport::Localization::
			IUeAgentLocalizationImportCommandService> Service,
		TSharedPtr<Active::IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryCreateProductionLocalizationImportContribution(
		TSharedPtr<Active::IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryCreateLocalizationImportRegistration(
		TSharedRef<OperationOwners::ExternalImport::Localization::
			IUeAgentLocalizationImportCommandService> Service,
		TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>&
			OutRegistry,
		TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>& OutDispatch,
		UE::UAI::Contract::FDiagnostic& OutError);
}
