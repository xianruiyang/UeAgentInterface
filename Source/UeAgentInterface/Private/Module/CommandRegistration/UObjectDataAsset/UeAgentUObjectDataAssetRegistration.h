#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "OperationOwners/UObjectDataAsset/UeAgentUObjectDataAssetCommandService.h"

namespace UE::UAI::Module::CommandRegistration::UObjectDataAsset
{
	UEAGENTINTERFACE_API bool TryCreateUObjectDataAssetContribution(
		TSharedRef<OperationOwners::UObjectDataAsset::
			IUeAgentUObjectDataAssetCommandService> Service,
		TSharedPtr<Active::IUeAgentActiveCommandContribution>&
			OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool
	TryCreateProductionUObjectDataAssetContribution(
		TSharedPtr<Active::IUeAgentActiveCommandContribution>&
			OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryCreateUObjectDataAssetRegistration(
		TSharedRef<OperationOwners::UObjectDataAsset::
			IUeAgentUObjectDataAssetCommandService> Service,
		TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>&
			OutRegistry,
		TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>&
			OutDispatch,
		UE::UAI::Contract::FDiagnostic& OutError);
}
