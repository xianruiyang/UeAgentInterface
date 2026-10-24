#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentCommandDispatch.h"

class FJsonObject;

namespace UE::UAI::OperationOwners::AnimationRigCommands
{
	UEAGENTINTERFACE_API bool TryResolveMechanics(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API CommandEdge::FUeAgentCommandResult Submit(
		const TSharedRef<CommandEdge::IUeAgentCommandHandler>& Mechanics,
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation);

	/** Calls one command already owned by Animation/Rig mechanics without re-entering public composition. */
	UEAGENTINTERFACE_API bool TrySubmitMechanic(
		const FString& CommandId,
		const TSharedRef<FJsonObject>& Params,
		const FString& CorrelationId,
		FString& OutPayload,
		UE::UAI::Contract::FDiagnostic& OutError);
}
