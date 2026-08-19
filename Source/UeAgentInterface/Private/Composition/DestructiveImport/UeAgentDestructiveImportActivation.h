#pragma once

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
		FString& OutError);

	UEAGENTINTERFACE_API bool
	TryCreateDestructiveImportProductionCommandContractRegistry(
		TSharedPtr<const CommandContract::FUeAgentCommandContractRegistry>&
			OutRegistry,
		FString& OutError);
}
