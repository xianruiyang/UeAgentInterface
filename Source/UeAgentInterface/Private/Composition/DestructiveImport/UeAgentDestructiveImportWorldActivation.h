#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentCommandContract.h"

namespace UE::UAI::Composition::DestructiveImport
{
	UEAGENTINTERFACE_API void AppendWorldDestructiveProductionCommandContracts(
		TArray<CommandContract::FUeAgentCommandContract>& Contracts);
}
