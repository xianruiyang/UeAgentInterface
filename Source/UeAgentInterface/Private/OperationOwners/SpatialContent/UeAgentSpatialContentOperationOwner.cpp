#include "OperationOwners/SpatialContent/UeAgentSpatialContentOperationOwner.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/DomainCapability/DelegatedCommandOperation/UeAgentDelegatedCommandOperation.h"
#include "Dom/JsonObject.h"
#include "DomainAdapters/SpatialContent/UeAgentSpatialContentMechanicsBoundary.h"
#include "QueryOwners/SpatialContent/UeAgentSpatialContentQueryOwner.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::OperationOwners::SpatialContent
{
	using namespace CommandEdge;

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

	bool TryResolveMechanics(
		const FString& CommandId,
		TSharedPtr<IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return DomainAdapters::SpatialContent::TryResolveMechanics(
			CommandId,
			OutHandler,
			OutAuthorityScopes,
			OutError);
	}

	bool IsDurableMutation(const FString& CommandId)
	{
		return CommandId == TEXT("world.object_transform.update")
			|| CommandId == TEXT("mesh.transform.bake")
			|| CommandId == TEXT("mesh.pivot.update");
	}

	TSharedPtr<const DomainCapability::IDomainOperationInvocationFactory>
	FindOperationInvocationFactory(const FString& CommandId)
	{
		static const TMap<FString, TSharedPtr<const DomainCapability::
			IDomainOperationInvocationFactory>> Factories = []()
		{
			TMap<FString, TSharedPtr<const DomainCapability::
				IDomainOperationInvocationFactory>> Result;
			for (const FString& Id : QueryOwners::SpatialContent::GetCommandIds())
			{
				if (!QueryOwners::SpatialContent::IsQuery(Id))
				{
					Result.Add(
						Id,
						DomainCapability::DelegatedCommandOperation::
							CreateInvocationFactory(
								Id,
								&TryResolveMechanics,
								IsDurableMutation(Id),
								true));
				}
			}
			return Result;
		}();
		return Factories.FindRef(CommandId);
	}

	FUeAgentCommandResult Submit(
		const FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
	{
		if (QueryOwners::SpatialContent::IsQuery(Request.GetCommandId()))
		{
			TSharedPtr<IUeAgentCommandHandler> Mechanics;
			TArray<FString> MechanicsScopes;
			UE::UAI::Contract::FDiagnostic ResolveError;
			if (!TryResolveMechanics(
					Request.GetCommandId(),
					Mechanics,
					MechanicsScopes,
					ResolveError)
				|| !Mechanics.IsValid())
			{
				return FUeAgentCommandResult::Failure(
					Request,
					TEXT("{}"),
					InvocationFailure(
						TEXT("spatial_content_runtime_unavailable"),
						Request.GetCommandId(),
						ResolveError));
			}
			TArray<FString> Scopes = Caller.GetAuthorityScopes();
			for (const FString& Scope : MechanicsScopes)
			{
				Scopes.AddUnique(Scope);
			}
			return Mechanics->Submit(
				Request,
				Contract::FUeAgentCallerContext(
					Caller.GetCallerId(),
					MoveTemp(Scopes),
					Caller.GetCorrelationId()),
				Cancellation);
		}

		const TSharedPtr<const DomainCapability::
			IDomainOperationInvocationFactory> Factory =
			FindOperationInvocationFactory(Request.GetCommandId());
		if (!Factory.IsValid())
		{
			return FUeAgentCommandResult::Failure(
				Request,
				TEXT("{}"),
				CommandFailure(TEXT("spatial_content_operation_factory_missing"), Request.GetCommandId(), TEXT("the exact spatial mutation has no factory")));
		}
		DomainCapability::FDomainOperationInvocation Invocation;
		DomainCapability::FDomainOperationInvocationError InvocationError;
		if (!Factory->TryCreate(
			Request, Caller, Cancellation, Invocation, InvocationError))
		{
			return FUeAgentCommandResult::Failure(
				Request,
				TEXT("{}"),
				InvocationFailure(TEXT("spatial_content_runtime_unavailable"), Request.GetCommandId(), InvocationError));
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
			return FUeAgentCommandResult::ProtocolError(Request.GetCorrelationId(), Request.GetRequestId(), Request.GetCommandId(), Request.GetCommandBindingHash(), ProjectionFailure(TEXT("spatial_content_operation_result_invalid"), PayloadError, Receipt));
		}
		return FUeAgentCommandResult::Success(Request, MoveTemp(Payload));
	}
}
