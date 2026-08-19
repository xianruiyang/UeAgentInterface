#pragma once

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Core/Contract/UeAgentContract.h"

namespace UE::UAI::OperationOwners::DestructiveContent
{
	UEAGENTINTERFACE_API const TArray<FString>&
	GetDestructiveContentCommandIds();

	UEAGENTINTERFACE_API CommandEdge::FUeAgentCommandResult
	SubmitProductionDestructiveContentOperation(
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& CallerContext,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation);
}
