#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "CoreMinimal.h"

namespace UE::UAI::OperationOwners::MaterialCommands
{
	UEAGENTINTERFACE_API bool TryCreateMechanicsHandlers(
		TMap<FString, TSharedRef<CommandEdge::IUeAgentCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryResolveMechanicsHandler(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryResolveOperationMechanics(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		UE::UAI::Contract::FDiagnostic& OutError);

	/** Execute a material command through its private production contract.
	 *  This is an owner-to-owner seam; it is intentionally not a public command
	 *  registration or a lookup through the active command registry.
	 */
	UEAGENTINTERFACE_API bool TryExecuteInternal(
		const FString& CommandId,
		const FString& ParamsJson,
		const Contract::FUeAgentCallerContext& Caller,
		FString& OutPayloadJson,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API TSharedPtr<const DomainCapability::
		IDomainOperationInvocationFactory> FindOperationInvocationFactory(
			const FString& OperationId);

	UEAGENTINTERFACE_API CommandEdge::FUeAgentCommandResult Submit(
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation);
}


