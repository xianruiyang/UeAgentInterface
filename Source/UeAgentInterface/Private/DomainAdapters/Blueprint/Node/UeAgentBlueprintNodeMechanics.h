#pragma once

#include "CoreMinimal.h"

class UBlueprint;

namespace UE::UAI::DomainAdapters::Blueprint::Node
{
	struct UEAGENTINTERFACE_API FUeAgentBlueprintNodeMechanicsError final
	{
		FString Code;
		FString SubjectId;
		FString Detail;
	};

	struct UEAGENTINTERFACE_API FUeAgentBlueprintNodeCandidateQuery final
	{
		FString Target;
		FString GraphId;
		FString Query;
		FString CandidateId;
		FString NodeGuid;
		FString PinGuid;
		FString PinName;
		int32 Limit = 20;
		int32 Cursor = 0;
	};

	UEAGENTINTERFACE_API bool TryGetFirstEditableGraphIdentity(
		UBlueprint& Blueprint,
		FString& OutGraphId,
		FString& OutError);

	UEAGENTINTERFACE_API bool TryQueryBlueprintNodeCandidates(
		const FUeAgentBlueprintNodeCandidateQuery& Query,
		FString& OutPayloadJson,
		FUeAgentBlueprintNodeMechanicsError& OutError);

	/** Compatibility overload for existing internal callers. */
	UEAGENTINTERFACE_API bool TryQueryBlueprintNodeCandidates(
		const FString& Target,
		const FString& GraphId,
		const FString& Query,
		FString& OutPayloadJson,
		FUeAgentBlueprintNodeMechanicsError& OutError);

	/** Validate a pre-v2 full candidate object without making it canonical again. */
	UEAGENTINTERFACE_API bool TryValidateLegacyBlueprintNodeCandidateBinding(
		const FString& Target,
		const FString& GraphId,
		const FString& Name,
		const FString& CandidateId,
		const FString& BindingRevision,
		FUeAgentBlueprintNodeMechanicsError& OutError);

}
