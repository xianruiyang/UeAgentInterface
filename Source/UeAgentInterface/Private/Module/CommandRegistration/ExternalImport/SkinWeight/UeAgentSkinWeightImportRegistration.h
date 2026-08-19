#pragma once

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "OperationOwners/ExternalImport/SkinWeight/UeAgentSkinWeightImportCommandService.h"

namespace UE::UAI::Module::CommandRegistration::ExternalImport::SkinWeight
{
	UEAGENTINTERFACE_API bool TryCreateSkinWeightImportContribution(
		TSharedRef<OperationOwners::ExternalImport::SkinWeight::
			IUeAgentSkinWeightImportCommandService> Service,
		TSharedPtr<Active::IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError);

	UEAGENTINTERFACE_API bool TryCreateProductionSkinWeightImportContribution(
		TSharedPtr<Active::IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError);

	UEAGENTINTERFACE_API bool TryCreateSkinWeightImportRegistration(
		TSharedRef<OperationOwners::ExternalImport::SkinWeight::
			IUeAgentSkinWeightImportCommandService> Service,
		TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>&
			OutRegistry,
		TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>& OutDispatch,
		FString& OutError);
}
