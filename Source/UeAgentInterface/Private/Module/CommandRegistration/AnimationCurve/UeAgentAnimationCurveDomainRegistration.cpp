#include "Module/CommandRegistration/AnimationCurve/UeAgentAnimationCurveDomainRegistration.h"

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/AnimationCurve/UeAgentAnimationCurveDomainService.h"

namespace UE::UAI::Module::CommandRegistration::AnimationCurve
{
	namespace
	{
		const FString ProviderRegistrationId =
			TEXT("animation-curve-domain-provider");
		const HjsonWorkspace::FImportInitializationDomainProviderFactoryRegistration
			Registration(
				TEXT("animation-curve-command-migration"),
				TEXT("animation-curve-domain-provider"),
				&OperationOwners::AnimationCurve::
					TryCreateAnimationCurveDomainProvider);
	}

	const FString& GetProviderRegistrationId()
	{
		return ProviderRegistrationId;
	}
}
