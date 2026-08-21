#include "OperationOwners/ModelingGeometry/UeAgentModelingGeometryOperationOwner.h"

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
		FString ErrorJson(
			const FString& Code,
			const FString& Subject,
			const FString& Detail,
			const FString& TerminalStatus = TEXT("not_started"),
			const bool bRetrySafe = true)
		{
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(TEXT("subject_id"), Subject);
			Error->SetStringField(TEXT("detail"), Detail);
			Error->SetStringField(TEXT("terminal_status"), TerminalStatus);
			Error->SetBoolField(TEXT("retry_safe"), bRetrySafe);
			FString Text;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>>
				Writer = TJsonWriterFactory<TCHAR,
					TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(Error, Writer);
			return Text;
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
		FString& OutError)
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
				ErrorJson(
					TEXT("modeling_geometry_operation_factory_missing"),
					Request.GetCommandId(),
					TEXT("the exact modeling operation has no lifecycle factory")));
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
				ErrorJson(
					TEXT("modeling_geometry_runtime_unavailable"),
					Request.GetCommandId(),
					InvocationError.GetCode() + TEXT(":")
						+ InvocationError.GetDetail()));
		}

		FOperationLifecycleReceipt Receipt;
		if (!Invocation.GetService()->Run(
			*Invocation.GetPlan(), *Invocation.GetAuthorization(), Receipt))
		{
			FString Detail = Invocation.GetContext()->GetFailureDetail();
			if (Detail.IsEmpty())
			{
				Detail = TEXT("phase_history:")
					+ FString::Join(Receipt.GetPhaseHistory(), TEXT(","));
			}
			return FUeAgentCommandResult::Failure(
				Request,
				TEXT("{}"),
				ErrorJson(
					Receipt.GetErrorCode(),
					Request.GetCommandId(),
					Detail,
					LexToString(Receipt.GetStatus()),
					Receipt.IsRetrySafe()));
		}

		FString Payload;
		FString PayloadError;
		if (!Invocation.GetContext()->TryBuildSuccessPayload(
			Receipt, Payload, PayloadError))
		{
			return FUeAgentCommandResult::ProtocolError(
				Request.GetCorrelationId(),
				Request.GetRequestId(),
				Request.GetCommandId(),
				Request.GetCommandBindingHash(),
				TEXT("modeling_geometry_operation_result_invalid"),
				PayloadError);
		}
		return FUeAgentCommandResult::Success(Request, MoveTemp(Payload));
	}
}
