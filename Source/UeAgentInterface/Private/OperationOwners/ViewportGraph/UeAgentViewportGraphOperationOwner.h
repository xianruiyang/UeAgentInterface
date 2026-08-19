#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"

namespace UE::UAI::OperationOwners::ViewportGraph
{
	UEAGENTINTERFACE_API bool TryResolveMechanics(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		FString& OutError);

	UEAGENTINTERFACE_API TSharedPtr<const DomainCapability::
		IDomainOperationInvocationFactory> FindOperationInvocationFactory(
			const FString& CommandId);

	UEAGENTINTERFACE_API CommandEdge::FUeAgentCommandResult Submit(
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation);
}
