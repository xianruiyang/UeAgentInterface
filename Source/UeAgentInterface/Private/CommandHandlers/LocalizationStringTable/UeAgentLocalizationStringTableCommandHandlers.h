#pragma once

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"

namespace UE::UAI::CommandHandlers::LocalizationStringTable
{
	UEAGENTINTERFACE_API bool TryHandleDomainRequest(
		const UE::UAI::HjsonWorkspace::FImportInitializationDomainRequest& Request,
		UE::UAI::HjsonWorkspace::FImportInitializationDomainResponse& OutResponse,
		UE::UAI::HjsonWorkspace::FWorkspaceError& OutError);
}
