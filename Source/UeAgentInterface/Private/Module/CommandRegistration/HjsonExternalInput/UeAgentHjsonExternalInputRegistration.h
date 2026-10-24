#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

namespace UE::UAI::Module::CommandRegistration::HjsonExternalInput
{
	UEAGENTINTERFACE_API const FString&
	GetHjsonExternalInputProviderContributionId();

	UEAGENTINTERFACE_API bool ValidateHjsonExternalInputContribution(
		UE::UAI::Contract::FDiagnostic& OutError);
}
