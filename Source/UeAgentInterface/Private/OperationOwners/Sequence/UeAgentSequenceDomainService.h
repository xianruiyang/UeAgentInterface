#pragma once

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"

namespace UE::UAI::OperationOwners::Sequence
{
	UEAGENTINTERFACE_API bool TryCreateSequenceDomainProvider(
		TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>&
			OutProvider,
		HjsonWorkspace::FWorkspaceError& OutError);
}
