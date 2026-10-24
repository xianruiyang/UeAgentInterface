#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class UBlueprint;

namespace UE::UAI::DomainAdapters::Blueprint::Node
{
	using FUeAgentBlueprintNodeMechanicsError = UE::UAI::Contract::FDiagnostic;

	struct UEAGENTINTERFACE_API FUeAgentBlueprintNodeCandidateQuery final
	{
		FString Target;
		FString GraphName;
		FString Query;
		FString NodeName;
		TOptional<int32> NodeOccurrence;
		FString PinName;
		TOptional<int32> PinOccurrence;
	};

	UEAGENTINTERFACE_API bool TryGetFirstEditableGraphIdentity(
		UBlueprint& Blueprint,
		FString& OutGraphId,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryQueryBlueprintNodeCandidates(
		const FUeAgentBlueprintNodeCandidateQuery& Query,
		FString& OutPayloadJson,
		FUeAgentBlueprintNodeMechanicsError& OutError);

}
