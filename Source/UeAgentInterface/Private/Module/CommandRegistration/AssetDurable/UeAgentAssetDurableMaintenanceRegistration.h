#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandHandlers/AssetDurable/UeAgentAssetDurableMaintenanceHandlers.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Module::CommandRegistration::AssetDurable
{
	UEAGENTINTERFACE_API bool TryCreateAssetDurableMaintenanceContribution(
		TSharedRef<CommandHandlers::AssetDurable::
			IUeAgentAssetDurableMaintenancePort> Port,
		TSharedPtr<Active::IUeAgentActiveCommandContribution>&
			OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool
	TryCreateProductionAssetDurableMaintenanceContribution(
		TSharedPtr<Active::IUeAgentActiveCommandContribution>&
			OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryCreateAssetDurableMaintenanceRegistration(
		TSharedRef<CommandHandlers::AssetDurable::
			IUeAgentAssetDurableMaintenancePort> Port,
		TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>&
			OutRegistry,
		TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>&
			OutDispatch,
		UE::UAI::Contract::FDiagnostic& OutError);
}
