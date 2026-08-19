#pragma once

#include "CoreMinimal.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Module::CommandRegistration::Pcg
{
	UEAGENTINTERFACE_API const FString& GetProviderRegistrationId();

	UEAGENTINTERFACE_API bool TryCreateProductionPcgCandidateContribution(
		TSharedPtr<Active::IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError);
}
