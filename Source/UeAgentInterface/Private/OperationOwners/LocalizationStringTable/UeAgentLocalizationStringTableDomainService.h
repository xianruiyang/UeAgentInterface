#pragma once

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"

namespace UE::UAI::OperationOwners::LocalizationStringTable
{
	UEAGENTINTERFACE_API bool TryCreateLocalizationStringTableDomainProvider(
		TSharedPtr<UE::UAI::HjsonWorkspace::IImportInitializationDomainProvider>& OutProvider,
		UE::UAI::HjsonWorkspace::FWorkspaceError& OutError);
}
