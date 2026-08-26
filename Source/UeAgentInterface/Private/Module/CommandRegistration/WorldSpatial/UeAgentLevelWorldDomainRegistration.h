#pragma once

#include "CoreMinimal.h"

namespace UE::UAI::Module::CommandRegistration::Active
{
	class IUeAgentActiveCommandContribution;
}

namespace UE::UAI::Module::CommandRegistration::WorldSpatial
{
	UEAGENTINTERFACE_API const FString& GetProviderRegistrationId();

	UEAGENTINTERFACE_API bool TryCreateLevelWorldCandidateContribution(
		TSharedPtr<Active::IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError);
}
