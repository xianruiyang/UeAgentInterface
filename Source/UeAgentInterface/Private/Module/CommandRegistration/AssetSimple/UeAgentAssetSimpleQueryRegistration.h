#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Module::CommandRegistration::AssetSimple
{
	UEAGENTINTERFACE_API bool TryCreateAssetSimpleQueryContribution(
		TSharedPtr<Active::IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError);
}
