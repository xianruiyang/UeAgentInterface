#pragma once

#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Module::CommandRegistration::AnimationRigCommands
{
	UEAGENTINTERFACE_API bool TryCreateContribution(
		TSharedPtr<Active::IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError);
}
