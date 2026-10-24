#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "CoreMinimal.h"

namespace UE::UAI::Module::CommandRegistration::Material
{
	UEAGENTINTERFACE_API const FString& GetProviderRegistrationId();

	UEAGENTINTERFACE_API bool TryCreateProductionMaterialCandidateContribution(
		TSharedPtr<Active::IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError);
}
