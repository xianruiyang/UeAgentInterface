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

	struct UEAGENTINTERFACE_API FUeAgentBlueprintComponentCandidateQuery final
	{
		FString Target;
		FString Parent;
		FString Query;
		FString CandidateId;
		int32 Limit = 20;
		int32 Cursor = 0;
	};

	UEAGENTINTERFACE_API bool IsSupportedBlueprintComponentClass(
		const FString& ComponentClass);

	UEAGENTINTERFACE_API bool TryQueryBlueprintComponentCandidates(
		const FUeAgentBlueprintComponentCandidateQuery& Query,
		FString& OutPayloadJson,
		FUeAgentBlueprintComponentMechanicsError& OutError);

	/** Compatibility overload for the pre-v2 full-candidate contract. */
	UEAGENTINTERFACE_API bool TryQueryBlueprintComponentCandidates(
		const FString& Target,
		const FString& ComponentClass,
		const FString& RequestedName,
		FString& OutPayloadJson,
		FUeAgentBlueprintComponentMechanicsError& OutError);

}
