#pragma once

#include "CoreMinimal.h"

namespace UE::UAI::DomainAdapters::Blueprint::Component
{
	struct UEAGENTINTERFACE_API FUeAgentBlueprintComponentMechanicsError final
	{
		FString Code;
		FString SubjectId;
		FString Detail;
	};

	UEAGENTINTERFACE_API bool TryQueryBlueprintComponentCandidates(
		const FString& Target,
		const FString& ComponentClass,
		const FString& RequestedName,
		FString& OutPayloadJson,
		FUeAgentBlueprintComponentMechanicsError& OutError);

	UEAGENTINTERFACE_API bool TryExecuteBlueprintComponentImport(
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		FUeAgentBlueprintComponentMechanicsError& OutError);
}
