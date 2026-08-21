#pragma once

#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Module::CommandRegistration::AssetSimple
{
	UEAGENTINTERFACE_API bool TryCreateAssetSimpleQueryContribution(
		TSharedPtr<Active::IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError);
}
