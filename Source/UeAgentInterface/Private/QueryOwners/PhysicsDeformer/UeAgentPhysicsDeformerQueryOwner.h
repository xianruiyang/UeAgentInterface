#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"

namespace UE::UAI::QueryOwners::PhysicsDeformer
{
	UEAGENTINTERFACE_API const TArray<FString>& GetStateLayers();
	UEAGENTINTERFACE_API const TArray<FString>& GetIndependentReadbackFlows();
	UEAGENTINTERFACE_API CommandEdge::FUeAgentCommandResult Submit(
		const TSharedRef<CommandEdge::IUeAgentCommandHandler>& Mechanics,
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation);
}
