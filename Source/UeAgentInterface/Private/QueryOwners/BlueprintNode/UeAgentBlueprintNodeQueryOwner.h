#pragma once

#include "CoreMinimal.h"
#include "OperationOwners/BlueprintNode/UeAgentBlueprintNodeCommandService.h"

namespace UE::UAI::QueryOwners::BlueprintNode
{
	UEAGENTINTERFACE_API bool TryExecuteCandidateQuery(
		const OperationOwners::BlueprintNode::
			FUeAgentBlueprintNodeCommandRequest& Request,
		OperationOwners::BlueprintNode::
			FUeAgentBlueprintNodeCommandResponse& OutResponse,
		OperationOwners::BlueprintNode::
			FUeAgentBlueprintNodeCommandError& OutError);
}

