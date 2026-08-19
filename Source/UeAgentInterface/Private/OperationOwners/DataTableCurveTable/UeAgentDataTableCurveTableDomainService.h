#pragma once

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"

namespace UE::UAI::OperationOwners::DataTableCurveTable
{
	UEAGENTINTERFACE_API bool TryCreateDataTableCurveTableDomainProvider(
		TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>&
			OutProvider,
		HjsonWorkspace::FWorkspaceError& OutError);
}
