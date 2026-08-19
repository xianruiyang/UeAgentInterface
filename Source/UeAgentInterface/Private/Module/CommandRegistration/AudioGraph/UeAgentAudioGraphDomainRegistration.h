#pragma once

#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Module::CommandRegistration::AudioGraph
{
	UEAGENTINTERFACE_API const FString& GetProviderRegistrationId();

	UEAGENTINTERFACE_API bool TryCreateProductionAudioGraphCandidateContribution(
		TSharedPtr<Active::IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError);
}
