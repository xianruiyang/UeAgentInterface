#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "CoreMinimal.h"

namespace UE::UAI::OperationOwners::BlueprintCommands
{
	UEAGENTINTERFACE_API bool TryCreateMechanicsHandlers(
		TMap<FString, TSharedRef<CommandEdge::IUeAgentCommandHandler>>& OutHandlers,
		FString& OutError);

	UEAGENTINTERFACE_API CommandEdge::FUeAgentCommandResult Submit(
		const TSharedRef<CommandEdge::IUeAgentCommandHandler>& Mechanics,
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation);
}
