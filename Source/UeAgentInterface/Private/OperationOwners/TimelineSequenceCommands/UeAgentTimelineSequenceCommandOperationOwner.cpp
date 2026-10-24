#include "OperationOwners/TimelineSequenceCommands/UeAgentTimelineSequenceCommandOperationOwner.h"

#include "Composition/TimelineSpatial/UeAgentTimelineSpatialActivation.h"
#include "Core/Contract/UeAgentInternalCommandExecution.h"
#include "Dom/JsonValue.h"
#include "DomainAdapters/TimelineSequenceCommands/UeAgentTimelineSequenceCommandMechanicsBoundary.h"

namespace UE::UAI::OperationOwners::TimelineSequenceCommands
{
	bool TryExecuteInternal(
		const FString& CommandId,
		const FString& ParamsJson,
		const Contract::FUeAgentCallerContext& Caller,
		FString& OutPayloadJson,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutPayloadJson.Reset();
		OutError.Reset();
		TSharedPtr<const CommandContract::FUeAgentCommandContractRegistry> Contracts;
		if (!Composition::TimelineSpatial::TryCreateTimelineSpatialProductionCommandContractRegistry(
				Contracts, OutError)
			|| !Contracts.IsValid())
		{
			return false;
		}
		const CommandContract::FUeAgentCommandContract* Contract =
			Contracts->FindExact(CommandId);
		if (!Contract)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("timeline_sequence_internal_contract_missing"),
				TEXT("No internal command contract is registered for this Timeline Sequence operation."),
				TEXT("internal_command"), CommandId);
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("command_contract_registered");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return false;
		}
		TSharedPtr<CommandEdge::IUeAgentCommandHandler> Mechanics;
		TArray<FString> Scopes;
		if (!TryResolveMechanics(CommandId, Mechanics, Scopes, OutError)
			|| !Mechanics.IsValid())
		{
			return false;
		}
		return CommandContract::TryExecuteInternalCommand(
			*Contract, Mechanics.ToSharedRef(), ParamsJson, Caller,
			TEXT("timeline_sequence_"), OutPayloadJson,
			OutError);
	}

	bool TryResolveMechanics(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return DomainAdapters::TimelineSequenceCommands::TryResolveMechanics(
			CommandId, OutHandler, OutAuthorityScopes, OutError);
	}

	CommandEdge::FUeAgentCommandResult Submit(
		const TSharedRef<CommandEdge::IUeAgentCommandHandler>& Mechanics,
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
	{
		return DomainAdapters::TimelineSequenceCommands::Submit(
			Mechanics, Request, Caller, Cancellation);
	}
}
