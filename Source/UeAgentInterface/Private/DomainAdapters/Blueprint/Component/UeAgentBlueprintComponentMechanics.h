#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

namespace UE::UAI::DomainAdapters::Blueprint::Component
{
	using FUeAgentBlueprintComponentMechanicsError = UE::UAI::Contract::FDiagnostic;

	struct UEAGENTINTERFACE_API FUeAgentBlueprintComponentCandidateQuery final
	{
		FString Target;
		FString Parent;
		FString Query;
	};

	UEAGENTINTERFACE_API bool TryQueryBlueprintComponentCandidates(
		const FUeAgentBlueprintComponentCandidateQuery& Query,
		FString& OutPayloadJson,
		FUeAgentBlueprintComponentMechanicsError& OutError);
}
