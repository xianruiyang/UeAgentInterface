#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "CoreMinimal.h"

namespace UE::UAI::OperationOwners::MaterialCommands
{
	UEAGENTINTERFACE_API bool TryCreateMechanicsHandlers(
		TMap<FString, TSharedRef<CommandEdge::IUeAgentCommandHandler>>& OutHandlers,
		FString& OutError);

	UEAGENTINTERFACE_API bool TryResolveMechanicsHandler(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		FString& OutError);

	UEAGENTINTERFACE_API bool TryResolveOperationMechanics(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		FString& OutError);

	UEAGENTINTERFACE_API TSharedPtr<const DomainCapability::
		IDomainOperationInvocationFactory> FindOperationInvocationFactory(
			const FString& OperationId);

	UEAGENTINTERFACE_API CommandEdge::FUeAgentCommandResult Submit(
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation);
}


