#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentCommandContract.h"

namespace UE::UAI::Composition::DestructiveImport
{
	UEAGENTINTERFACE_API void AppendExternalDataImportProductionCommandContracts(
		TArray<CommandContract::FUeAgentCommandContract>& Contracts);
}
