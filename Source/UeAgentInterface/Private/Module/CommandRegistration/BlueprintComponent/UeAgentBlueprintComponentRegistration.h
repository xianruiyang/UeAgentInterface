#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "OperationOwners/BlueprintComponent/UeAgentBlueprintComponentCommandService.h"

namespace UE::UAI::Module::CommandRegistration::BlueprintComponent
{
	UEAGENTINTERFACE_API bool TryCreateBlueprintComponentContribution(
		TSharedRef<OperationOwners::BlueprintComponent::
			IUeAgentBlueprintComponentCommandService> Service,
		TSharedPtr<Active::IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryCreateProductionBlueprintComponentContribution(
		TSharedPtr<Active::IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryCreateBlueprintComponentRegistration(
		TSharedRef<OperationOwners::BlueprintComponent::
			IUeAgentBlueprintComponentCommandService> Service,
		TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>&
			OutRegistry,
		TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>& OutDispatch,
		UE::UAI::Contract::FDiagnostic& OutError);
}
