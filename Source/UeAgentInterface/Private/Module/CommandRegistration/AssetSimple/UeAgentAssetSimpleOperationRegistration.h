#pragma once

#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Module::CommandRegistration::AssetSimple
{
	UEAGENTINTERFACE_API bool TryCreateAssetSimpleOperationContribution(
		TSharedPtr<Active::IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError);

	UEAGENTINTERFACE_API bool TryCreateAssetSimpleOperationRegistration(
		TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry>&
			OutRegistry,
		TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch>& OutDispatch,
		FString& OutError);
}
