#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "OperationOwners/ExternalImport/Factory/UeAgentFactoryExternalImportCommandService.h"

namespace UE::UAI::Module::CommandRegistration::ExternalImport::Factory
{
	UEAGENTINTERFACE_API bool TryCreateFactoryExternalImportContribution(
		TSharedRef<OperationOwners::ExternalImport::Factory::
			IUeAgentFactoryExternalImportCommandService> Service,
		TSharedPtr<Active::IUeAgentActiveCommandContribution>&
			OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool
	TryCreateProductionFactoryExternalImportContribution(
		TSharedPtr<Active::IUeAgentActiveCommandContribution>&
			OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryCreateFactoryExternalImportRegistration(
		TSharedRef<OperationOwners::ExternalImport::Factory::
			IUeAgentFactoryExternalImportCommandService> Service,
		TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>&
			OutRegistry,
		TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>&
			OutDispatch,
		UE::UAI::Contract::FDiagnostic& OutError);
}
