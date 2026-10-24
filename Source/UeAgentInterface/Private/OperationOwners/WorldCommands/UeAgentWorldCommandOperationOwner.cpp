#include "OperationOwners/WorldCommands/UeAgentWorldCommandOperationOwner.h"

#include "Core/DomainCapability/DelegatedCommandOperation/UeAgentDelegatedCommandOperation.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringComposition.h"
#include "Core/Contract/UeAgentInternalCommandExecution.h"
#include "DomainAdapters/WorldCommands/UeAgentWorldCommandMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "QueryOwners/WorldCommands/UeAgentWorldCommandQueryOwner.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::OperationOwners::WorldCommands
{
	using namespace CommandEdge;

	namespace
	{
		struct FProcessOperationFactories final
		{
			FCriticalSection Mutex;
			TMap<FString, TSharedRef<const DomainCapability::
				IDomainOperationInvocationFactory>> Factories;
		};

		FProcessOperationFactories& ProcessOperationFactories()
		{
			static FProcessOperationFactories Value;
			return Value;
		}

		UE::UAI::Contract::FDiagnostic InvocationFailure(const FString& Code, const FString& Subject, const UE::UAI::Contract::FDiagnostic& Cause)
		{
			auto Diagnostic = Cause.IsEmpty()
				? UE::UAI::Contract::FDiagnostic::Failure(Code, TEXT("The World operation runtime could not be prepared."), TEXT("command"))
				: UE::UAI::Contract::FDiagnostic::WrapFailure(Code, Cause, TEXT("command"), TEXT("The World operation runtime could not be prepared."));
			Diagnostic.SubjectId = Subject;
			if (QueryOwners::WorldCommands::IsCommand(Subject))
				Diagnostic.PublicMessage += TEXT(" Command: ") + Subject + TEXT(".");
			if (Cause.IsEmpty()) {
				Diagnostic.SourceKind = TEXT("native_result");
				Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
				Diagnostic.PropertyPath = TEXT("operation_runtime_prepared");
			}
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			Diagnostic.TerminalStatus = TEXT("not_started");
			Diagnostic.bRetrySafe = true;
			Diagnostic.MutationStarted = false;
			Diagnostic.MutationMayHaveStarted = false;
			return Diagnostic;
		}

		UE::UAI::Contract::FDiagnostic ProjectionFailure(const FString& Code, const UE::UAI::Contract::FDiagnostic& Cause,
			const BindingGateway::Operation::FOperationLifecycleReceipt& Receipt)
		{
			auto Diagnostic = Cause.IsEmpty()
				? UE::UAI::Contract::FDiagnostic::Failure(Code, TEXT("The operation completed, but its result payload could not be produced."), TEXT("result_projection"))
				: UE::UAI::Contract::FDiagnostic::WrapFailure(Code, Cause, TEXT("result_projection"), TEXT("The operation completed, but its result payload could not be produced."));
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			if (Cause.IsEmpty()) {
				Diagnostic.SourceKind = TEXT("native_result");
				Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
				Diagnostic.PropertyPath = TEXT("result_payload_produced");
			}
			Diagnostic.TerminalStatus = BindingGateway::Operation::LexToString(Receipt.GetStatus());
			Diagnostic.MutationStarted = Receipt.DidMutationStart();
			Diagnostic.MutationMayHaveStarted = Receipt.DidMutationStart();
			Diagnostic.bRetrySafe = Receipt.IsRetrySafe();
			return Diagnostic;
		}

	}

	bool TryResolveMechanics(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return DomainAdapters::WorldCommands::TryResolveMechanics(
			CommandId, OutHandler, OutAuthorityScopes, OutError);
	}

	TSharedRef<const DomainCapability::IDomainOperationInvocationFactory>
	FindOperationInvocationFactory(const FString& OperationId)
	{
		FProcessOperationFactories& Host = ProcessOperationFactories();
		FScopeLock Lock(&Host.Mutex);
		if (const TSharedRef<const DomainCapability::
			IDomainOperationInvocationFactory>* Existing =
				Host.Factories.Find(OperationId))
		{
			return *Existing;
		}
		const TSharedRef<const DomainCapability::
			IDomainOperationInvocationFactory> Factory =
				DomainCapability::DelegatedCommandOperation::
					CreateInvocationFactory(
						OperationId, &TryResolveMechanics, true, true);
		Host.Factories.Add(OperationId, Factory);
		return Factory;
	}

	FUeAgentCommandResult Submit(
		const TSharedRef<IUeAgentCommandHandler>& Mechanics,
		const FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
	{
		return DomainAdapters::WorldCommands::Submit(
			Mechanics, Request, Caller, Cancellation);
	}

	FUeAgentCommandResult Submit(
		const FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
	{
		const TSharedRef<const DomainCapability::
			IDomainOperationInvocationFactory> Factory =
				FindOperationInvocationFactory(Request.GetCommandId());
		DomainCapability::FDomainOperationInvocation Invocation;
		DomainCapability::FDomainOperationInvocationError InvocationError;
		if (!Factory->TryCreate(
			Request, Caller, Cancellation, Invocation, InvocationError))
		{
			return FUeAgentCommandResult::Failure(
				Request, TEXT("{}"),
				InvocationFailure(TEXT("world_command_runtime_unavailable"), Request.GetCommandId(), InvocationError));
		}
		BindingGateway::Operation::FOperationLifecycleReceipt Receipt;
		if (!Invocation.GetService()->Run(
			*Invocation.GetPlan(), *Invocation.GetAuthorization(), Receipt))
		{
			auto Diagnostic = Receipt.GetDiagnostic();
			if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = Request.GetCommandId();
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(Diagnostic));
		}
		FString Payload;
		UE::UAI::Contract::FDiagnostic PayloadError;
		if (!Invocation.GetContext()->TryBuildSuccessPayload(
			Receipt, Payload, PayloadError))
		{
			return FUeAgentCommandResult::ProtocolError(Request.GetCorrelationId(), Request.GetRequestId(), Request.GetCommandId(), Request.GetCommandBindingHash(), ProjectionFailure(TEXT("domain_operation_result_invalid"), PayloadError, Receipt));
		}
		return FUeAgentCommandResult::Success(Request, MoveTemp(Payload));
	}

	bool TryExecuteInternal(
		const FString& CommandId,
		const FString& ParamsJson,
		const Contract::FUeAgentCallerContext& Caller,
		FString& OutPayloadJson,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutPayloadJson.Reset();
		OutError.Reset();

		// This is intentionally the canonical production contract source.  It
		// is not the active registry, which is model-facing and may omit private
		// mechanics used by WorldSpatial HJSON import.
		TSharedPtr<const CommandContract::FUeAgentCommandContractRegistry>
			Contracts;
		if (!Composition::SimpleAuthoring::
			TryCreateProductionCommandContractRegistry(Contracts, OutError)
			|| !Contracts.IsValid())
		{
			if (OutError.IsEmpty()) {
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("world_command_internal_registry_unavailable"),
					TEXT("The World command contract factory failed or returned no registry without reporting a diagnostic."), TEXT("internal_command"), CommandId);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.PropertyPath = TEXT("command_registry_ready");
				OutError.SourceKind = TEXT("native_result");
			}
			return false;
		}
		const CommandContract::FUeAgentCommandContract* Contract =
			Contracts->FindExact(CommandId);
		if (!Contract)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("world_command_internal_contract_missing"),
				TEXT("No execution contract is registered for the required World operation."), TEXT("internal_command"), CommandId);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.PropertyPath = TEXT("command_contract_registered");
			OutError.SourceKind = TEXT("native_result");
			if (QueryOwners::WorldCommands::IsCommand(CommandId))
				OutError.PublicMessage += TEXT(" Command: ") + CommandId + TEXT(".");
			return false;
		}

		TSharedPtr<IUeAgentCommandHandler> Mechanics;
		TArray<FString> AuthorityScopes;
		if (!TryResolveMechanics(CommandId, Mechanics, AuthorityScopes, OutError))
		{
			return false;
		}
		return CommandContract::TryExecuteInternalCommand(
			*Contract, Mechanics.ToSharedRef(), ParamsJson, Caller,
			TEXT("world_command_"), OutPayloadJson,
			OutError);
	}
}
