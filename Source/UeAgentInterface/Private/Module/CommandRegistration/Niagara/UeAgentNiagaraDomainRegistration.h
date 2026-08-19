#pragma once

#include "CoreMinimal.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Module::CommandRegistration::Niagara
{
	UEAGENTINTERFACE_API const FString& GetProviderRegistrationId();

	UEAGENTINTERFACE_API bool TryCreateProductionNiagaraCandidateContribution(
		TSharedPtr<Active::IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError);
}
