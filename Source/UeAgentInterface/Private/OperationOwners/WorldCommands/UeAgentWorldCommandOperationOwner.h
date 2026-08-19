#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"

namespace UE::UAI::OperationOwners::WorldCommands
{
	UEAGENTINTERFACE_API bool TryResolveMechanics(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		FString& OutError);

	UEAGENTINTERFACE_API TSharedRef<const DomainCapability::
		IDomainOperationInvocationFactory> FindOperationInvocationFactory(
		const FString& OperationId);

	UEAGENTINTERFACE_API CommandEdge::FUeAgentCommandResult Submit(
		const TSharedRef<CommandEdge::IUeAgentCommandHandler>& Mechanics,
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation);

	UEAGENTINTERFACE_API CommandEdge::FUeAgentCommandResult Submit(
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation);
}
