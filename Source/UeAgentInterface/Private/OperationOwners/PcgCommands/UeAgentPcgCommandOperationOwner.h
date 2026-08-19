#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"

namespace UE::UAI::OperationOwners::PcgCommands
{
	UEAGENTINTERFACE_API bool TryCreateMechanicsHandlers(
		TMap<FString, TSharedRef<CommandEdge::IUeAgentCommandHandler>>& OutHandlers,
		TArray<FString>& OutAuthorityScopes,
		FString& OutError);

	UEAGENTINTERFACE_API bool TryResolveMechanics(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		FString& OutError);

	UEAGENTINTERFACE_API CommandEdge::FUeAgentCommandResult Submit(
		const TSharedRef<CommandEdge::IUeAgentCommandHandler>& Mechanics,
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation);
}
