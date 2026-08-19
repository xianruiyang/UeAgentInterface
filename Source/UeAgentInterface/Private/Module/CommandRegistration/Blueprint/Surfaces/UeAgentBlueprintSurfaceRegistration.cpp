#include "Module/CommandRegistration/Blueprint/Surfaces/UeAgentBlueprintSurfaceRegistration.h"

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/Blueprint/Surfaces/UeAgentBlueprintSurfaceDomainService.h"

namespace UE::UAI::Module::CommandRegistration::Blueprint::Surfaces
{
	namespace
	{
		const FString ProviderRegistrationId =
			TEXT("blueprint-surface-domain-provider");
		const HjsonWorkspace::FImportInitializationDomainProviderFactoryRegistration
			ProviderRegistration(
				TEXT("blueprint-surface-domain-provider"),
				&OperationOwners::Blueprint::Surfaces::
					TryCreateBlueprintSurfaceDomainProvider);
	}

	const FString& GetProviderRegistrationId()
	{
		return ProviderRegistrationId;
	}
}
