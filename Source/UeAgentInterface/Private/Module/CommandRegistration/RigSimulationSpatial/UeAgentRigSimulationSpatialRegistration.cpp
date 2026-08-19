#include "Module/CommandRegistration/RigSimulationSpatial/UeAgentRigSimulationSpatialRegistration.h"

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/RigSimulationSpatial/UeAgentRigSimulationSpatialDomainService.h"

namespace UE::UAI::Module::CommandRegistration::RigSimulationSpatial
{
	namespace
	{
		const FString ProviderRegistrationId =
			TEXT("rig-simulation-spatial-domain-provider");
		const HjsonWorkspace::FImportInitializationDomainProviderFactoryRegistration
			Registration(
				TEXT("rig-simulation-spatial-domain-provider"),
				&OperationOwners::RigSimulationSpatial::
					TryCreateRigSimulationSpatialDomainProvider);
	}

	const FString& GetProviderRegistrationId()
	{
		return ProviderRegistrationId;
	}
}
