#pragma once

#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Module::CommandRegistration::DestructiveContent
{
	UEAGENTINTERFACE_API bool TryCreateDestructiveContentContribution(
		TSharedPtr<Active::IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError);
}
