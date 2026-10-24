#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "OperationOwners/BlueprintNode/UeAgentBlueprintNodeCommandService.h"

namespace UE::UAI::Module::CommandRegistration::BlueprintNode
{
	UEAGENTINTERFACE_API bool TryCreateBlueprintNodeContribution(
		TSharedRef<OperationOwners::BlueprintNode::
			IUeAgentBlueprintNodeCommandService> Service,
		TSharedPtr<Active::IUeAgentActiveCommandContribution>&
			OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryCreateProductionBlueprintNodeContribution(
		TSharedPtr<Active::IUeAgentActiveCommandContribution>&
			OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryCreateBlueprintNodeRegistration(
		TSharedRef<OperationOwners::BlueprintNode::
			IUeAgentBlueprintNodeCommandService> Service,
		TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>&
			OutRegistry,
		TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>&
			OutDispatch,
		UE::UAI::Contract::FDiagnostic& OutError);
}

