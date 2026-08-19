#pragma once

#include "CoreMinimal.h"

namespace UE::UAI::Module::CommandRegistration::HjsonExternalInput
{
	UEAGENTINTERFACE_API const FString&
	GetHjsonExternalInputProviderContributionId();

	UEAGENTINTERFACE_API bool ValidateHjsonExternalInputContribution(
		FString& OutError);
}
