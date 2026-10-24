#include "OperationOwners/ModelingGeometry/UeAgentModelingGeometryOperationOwner.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Composition/SimpleAuthoring/OperationHost/UeAgentSimpleAuthoringOperationHost.h"
#include "Core/DomainCapability/DelegatedCommandOperation/UeAgentDelegatedCommandOperation.h"
#include "DomainAdapters/ModelingGeometry/UeAgentModelingGeometryMechanicsBoundary.h"
#include "DomainAdapters/ModelingGeometry/UeAgentModelingVoxelGrid.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::OperationOwners::ModelingGeometry
{
	using namespace BindingGateway::Operation;
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

	TSharedPtr<const DomainCapability::IDomainOperationInvocationFactory>
	FindOperationInvocationFactory(const FString& OperationId)
	{
		using namespace DomainAdapters::ModelingGeometry;
		if (VoxelGrid::IsCommand(OperationId))
		{
			static const TMap<FString, TSharedPtr<const DomainCapability::
				IDomainOperationInvocationFactory>> Factories = {
				{
					VoxelGrid::GetBeginCommandId(),
					DomainCapability::DelegatedCommandOperation::
						CreateInvocationFactory(
							VoxelGrid::GetBeginCommandId(),
							&TryResolveVoxelGridMechanics,
							false,
							true),
				},
				{
					VoxelGrid::GetSessionCommandId(),
					DomainCapability::DelegatedCommandOperation::
						CreateInvocationFactory(
							VoxelGrid::GetSessionCommandId(),
							&TryResolveVoxelGridMechanics,
							false,
							true),
				},
				{
					VoxelGrid::GetApplyCommandId(),
					DomainCapability::DelegatedCommandOperation::
						CreateInvocationFactory(
							VoxelGrid::GetApplyCommandId(),
							&TryResolveVoxelGridMechanics,
							true,
							true),
				},
			};
			return Factories.FindRef(OperationId);
		}
		if (OperationId == TEXT("mesh.modeling_session")
			|| !GetCommandIds().Contains(OperationId))
		{
			return nullptr;
		}
		return Composition::SimpleAuthoring::OperationHost::
			GetOperationInvocationFactory(OperationId);
	}

	bool TryResolveVoxelGridMechanics(
		const FString& CommandId,
		TSharedPtr<IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandler.Reset();
		OutAuthorityScopes = DomainAdapters::ModelingGeometry::
			GetRequiredAuthorityScopes();
		return DomainAdapters::ModelingGeometry::VoxelGrid::
			TryCreateMechanicsHandler(CommandId, OutHandler, OutError);
	}

	FUeAgentCommandResult Submit(
		const FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
	{
		const TSharedPtr<const DomainCapability::
			IDomainOperationInvocationFactory> Factory =
				FindOperationInvocationFactory(Request.GetCommandId());
		if (!Factory.IsValid())
		{
			return FUeAgentCommandResult::Failure(
				Request,
				TEXT("{}"),
				CommandFailure(TEXT("modeling_geometry_operation_factory_missing"), Request.GetCommandId(), TEXT("the exact modeling operation has no lifecycle factory")));
		}

		TArray<FString> Scopes = Caller.GetAuthorityScopes();
		for (const FString& Scope : DomainAdapters::ModelingGeometry::
			GetRequiredAuthorityScopes())
		{
			Scopes.AddUnique(Scope);
		}
		const Contract::FUeAgentCallerContext EffectiveCaller(
			Caller.GetCallerId(), MoveTemp(Scopes), Caller.GetCorrelationId());
		DomainCapability::FDomainOperationInvocation Invocation;
		DomainCapability::FDomainOperationInvocationError InvocationError;
		if (!Factory->TryCreate(
			Request,
			EffectiveCaller,
			Cancellation,
			Invocation,
			InvocationError))
		{
			return FUeAgentCommandResult::Failure(
				Request,
				TEXT("{}"),
				InvocationFailure(TEXT("modeling_geometry_runtime_unavailable"), Request.GetCommandId(), InvocationError));
		}

		FOperationLifecycleReceipt Receipt;
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
			return FUeAgentCommandResult::ProtocolError(Request.GetCorrelationId(), Request.GetRequestId(), Request.GetCommandId(), Request.GetCommandBindingHash(), ProjectionFailure(TEXT("modeling_geometry_operation_result_invalid"), PayloadError, Receipt));
		}
		return FUeAgentCommandResult::Success(Request, MoveTemp(Payload));
	}
}
