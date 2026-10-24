#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::Composition::DestructiveImport
{
	UEAGENTINTERFACE_API bool
	TryExtendDestructiveImportProductionCommandContractRegistry(
		TSharedRef<const CommandContract::FUeAgentCommandContractRegistry>
			BaseRegistry,
		TSharedPtr<const CommandContract::FUeAgentCommandContractRegistry>&
			OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool
	TryCreateDestructiveImportProductionCommandContractRegistry(
		TSharedPtr<const CommandContract::FUeAgentCommandContractRegistry>&
			OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError);
}
