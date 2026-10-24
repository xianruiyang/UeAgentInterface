#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Containers/UnrealString.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Module::CommandRegistration::AnimationBlueprint
{
	UEAGENTINTERFACE_API const FString& GetProviderRegistrationId();

	UEAGENTINTERFACE_API bool
	TryCreateProductionAnimationBlueprintCandidateContribution(
		TSharedPtr<Active::IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError);
}
