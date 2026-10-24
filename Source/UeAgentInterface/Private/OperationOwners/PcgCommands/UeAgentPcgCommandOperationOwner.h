#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentCommandDispatch.h"

class FJsonObject;

namespace UE::UAI::OperationOwners::PcgCommands
{
	UEAGENTINTERFACE_API bool TryCreateMechanicsHandlers(
		TMap<FString, TSharedRef<CommandEdge::IUeAgentCommandHandler>>& OutHandlers,
		TArray<FString>& OutAuthorityScopes,
		UE::UAI::Contract::FDiagnostic& OutError);

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

	/** Reuses the public PCG graph-create mechanic without re-entering command composition. */
	UEAGENTINTERFACE_API bool TryCreatePcgGraphAsset(
		const TSharedRef<FJsonObject>& Params,
		const FString& CorrelationId,
		FString& OutPayload,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryApplyPcgGraph(
		const TSharedRef<FJsonObject>& Params,
		const FString& CorrelationId,
		FString& OutPayload,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryApplyPcgGraphParameters(
		const TSharedRef<FJsonObject>& Params,
		const FString& CorrelationId,
		FString& OutPayload,
		UE::UAI::Contract::FDiagnostic& OutError);
}
