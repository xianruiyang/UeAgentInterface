#include "Module/CommandRegistration/WorldSpatial/UeAgentLevelWorldDomainRegistration.h"

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/WorldSpatial/UeAgentLevelWorldDomainService.h"

namespace UE::UAI::Module::CommandRegistration::WorldSpatial
{
	namespace
	{
		const FString ProviderRegistrationId = TEXT("level-world-domain-provider");
		const HjsonWorkspace::FImportInitializationDomainProviderFactoryRegistration
			Registration(
				TEXT("spatial-content-operation"),
				TEXT("level-world-domain-provider"),
				&OperationOwners::WorldSpatial::TryCreateLevelWorldDomainProvider);
	}

	const FString& GetProviderRegistrationId()
	{
		return ProviderRegistrationId;
	}
}
