#pragma once

#include "CoreMinimal.h"
#include "OperationOwners/BlueprintComponent/UeAgentBlueprintComponentCommandService.h"

namespace UE::UAI::QueryOwners::BlueprintComponent
{
	UEAGENTINTERFACE_API bool TryExecuteCandidateQuery(
		const OperationOwners::BlueprintComponent::
			FUeAgentBlueprintComponentCommandRequest& Request,
		OperationOwners::BlueprintComponent::
			FUeAgentBlueprintComponentCommandResponse& OutResponse,
		OperationOwners::BlueprintComponent::
			FUeAgentBlueprintComponentCommandError& OutError);
}
