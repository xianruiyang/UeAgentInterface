#include "OperationOwners/Landscape/UeAgentLandscapeOperationOwner.h"

#include "Composition/SimpleAuthoring/OperationHost/UeAgentSimpleAuthoringOperationHost.h"
#include "DomainAdapters/Landscape/UeAgentLandscapeMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::OperationOwners::Landscape
{
	using namespace CommandEdge;
	using namespace Composition::SimpleAuthoring;

	namespace
	{
		UE::UAI::Contract::FDiagnostic CommandFailure(FString Code, const FString& Subject, FString Reason = FString(), FString InternalContext = FString())
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(MoveTemp(Code), MoveTemp(Reason), TEXT("command"), MoveTemp(InternalContext), Subject);
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			Diagnostic.TerminalStatus = TEXT("not_started");
			Diagnostic.bRetrySafe = true;
			Diagnostic.MutationStarted = false;
			Diagnostic.MutationMayHaveStarted = false;
			return Diagnostic;
		}

		UE::UAI::Contract::FDiagnostic InvocationFailure(const FString& Code, const FString& Subject, const UE::UAI::Contract::FDiagnostic& Cause)
		{
			auto Diagnostic = CommandFailure(Code, Subject, TEXT("The operation runtime could not be prepared."));
			if (!Cause.IsEmpty()) Diagnostic.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(Cause);
			return Diagnostic;
		}

		UE::UAI::Contract::FDiagnostic ProjectionFailure(const FString& Code, const UE::UAI::Contract::FDiagnostic& Cause,
			const BindingGateway::Operation::FOperationLifecycleReceipt& Receipt)
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code, TEXT("The operation completed, but its result payload could not be produced."), TEXT("result_projection"));
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			if (!Cause.IsEmpty()) Diagnostic.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(Cause);
			Diagnostic.TerminalStatus = BindingGateway::Operation::LexToString(Receipt.GetStatus());
			Diagnostic.MutationStarted = Receipt.DidMutationStart();
			Diagnostic.MutationMayHaveStarted = Receipt.DidMutationStart();
			Diagnostic.bRetrySafe = Receipt.IsRetrySafe();
			return Diagnostic;
		}
	}

	const TArray<FUeAgentSimpleAuthoringCommandRoute>& GetRoutes()
	{
		static const TArray<FUeAgentSimpleAuthoringCommandRoute> Routes = []()
		{
			TArray<FUeAgentSimpleAuthoringCommandRoute> Result;
			for (const FUeAgentSimpleAuthoringCommandRoute& Route
				: GetSimpleAuthoringCommandRoutes())
			{
				if (DomainAdapters::Landscape::IsCommand(Route.CommandId))
				{
					Result.Add(Route);
				}
			}
			if (Result.Num() != DomainAdapters::Landscape::GetCommandIds().Num())
			{
				Result.Reset();
			}
			return Result;
		}();
		return Routes;
	}

	TSharedRef<const DomainCapability::IDomainOperationInvocationFactory>
	FindOperationInvocationFactory(const FString& OperationId)
	{
		return OperationHost::GetOperationInvocationFactory(OperationId);
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
				Request,
				TEXT("{}"),
				InvocationFailure(InvocationError.Code == TEXT("simple_authoring_handler_missing")
						? FString(TEXT("landscape_command_not_bound"))
						: FString(TEXT("landscape_runtime_unavailable")), Request.GetCommandId(), InvocationError));
		}
		BindingGateway::Operation::FOperationLifecycleReceipt Receipt;
		if (!Invocation.GetService()->Run(
				*Invocation.GetPlan(),
				*Invocation.GetAuthorization(),
				Receipt))
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
}
