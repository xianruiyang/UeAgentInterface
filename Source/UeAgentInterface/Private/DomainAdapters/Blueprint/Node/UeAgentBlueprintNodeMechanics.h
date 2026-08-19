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

	UEAGENTINTERFACE_API bool TryGetFirstEditableGraphIdentity(
		UBlueprint& Blueprint,
		FString& OutGraphId,
		FString& OutError);

	UEAGENTINTERFACE_API bool TryQueryBlueprintNodeCandidates(
		const FString& Target,
		const FString& GraphId,
		const FString& Query,
		FString& OutPayloadJson,
		FUeAgentBlueprintNodeMechanicsError& OutError);

	UEAGENTINTERFACE_API bool TryExecuteBlueprintNodeContentGraph(
		const FString& CommandId,
		const FString& RequestId,
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		bool& bOutReadOnly,
		FUeAgentBlueprintNodeMechanicsError& OutError);
}

