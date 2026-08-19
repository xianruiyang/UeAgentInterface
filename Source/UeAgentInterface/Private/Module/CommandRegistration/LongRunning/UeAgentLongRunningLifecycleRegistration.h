#pragma once

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Module::CommandRegistration::LongRunning
{
	UEAGENTINTERFACE_API bool TryCreateLongRunningLifecycleContribution(
		TSharedRef<Contract::IUeAgentOperationApplicationPort> OperationPort,
		TSharedPtr<Active::IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError);

	UEAGENTINTERFACE_API bool TryCreateProductionLongRunningLifecycleContribution(
		TSharedPtr<Active::IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError);

	UEAGENTINTERFACE_API bool TryCreateLongRunningLifecycleRegistration(
		TSharedRef<Contract::IUeAgentOperationApplicationPort> OperationPort,
		TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>&
			OutRegistry,
		TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>& OutDispatch,
		FString& OutError);
}
