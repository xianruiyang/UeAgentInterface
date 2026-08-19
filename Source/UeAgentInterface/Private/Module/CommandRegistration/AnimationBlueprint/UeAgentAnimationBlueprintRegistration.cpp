#include "Module/CommandRegistration/AnimationBlueprint/UeAgentAnimationBlueprintRegistration.h"

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/AnimationBlueprint/UeAgentAnimationBlueprintDomainService.h"

namespace UE::UAI::Module::CommandRegistration::AnimationBlueprint
{
	namespace
	{
		const FString ProviderRegistrationId =
			TEXT("animation-blueprint-domain-provider");
		const HjsonWorkspace::FImportInitializationDomainProviderFactoryRegistration
			ProviderRegistration(
				TEXT("animation-blueprint-domain-provider"),
				&OperationOwners::AnimationBlueprint::
					TryCreateAnimationBlueprintDomainProvider);
	}

	const FString& GetProviderRegistrationId()
	{
		return ProviderRegistrationId;
	}
}
