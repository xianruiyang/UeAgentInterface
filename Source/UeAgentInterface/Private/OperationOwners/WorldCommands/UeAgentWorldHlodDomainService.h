#pragma once

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"

namespace UE::UAI::OperationOwners::WorldCommands
{
	UEAGENTINTERFACE_API bool TryCreateWorldHlodDomainProvider(
		TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>& OutProvider,
		HjsonWorkspace::FWorkspaceError& OutError);
}
