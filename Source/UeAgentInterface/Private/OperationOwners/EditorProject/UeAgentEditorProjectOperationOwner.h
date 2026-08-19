#pragma once

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Core/Contract/UeAgentContract.h"

namespace UE::UAI::OperationOwners::EditorProject
{
	UEAGENTINTERFACE_API CommandEdge::FUeAgentCommandResult Submit(
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation);
}
