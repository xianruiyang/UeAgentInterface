#pragma once

#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Module::CommandRegistration::AnimationRigGraph
{
	UEAGENTINTERFACE_API const FString& GetProviderRegistrationId();

	UEAGENTINTERFACE_API bool TryCreateProductionAnimationRigGraphCandidateContribution(
		TSharedPtr<Active::IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError);
}
