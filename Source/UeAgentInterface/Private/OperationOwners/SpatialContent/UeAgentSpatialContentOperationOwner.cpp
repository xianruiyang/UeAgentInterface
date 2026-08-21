#include "OperationOwners/SpatialContent/UeAgentSpatialContentOperationOwner.h"

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
			const auto Writer = TJsonWriterFactory<TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(Error, Writer);
			return Text;
		}
	}

	bool TryResolveMechanics(
		const FString& CommandId,
		TSharedPtr<IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		FString& OutError)
	{
		return DomainAdapters::SpatialContent::TryResolveMechanics(
			CommandId,
			OutHandler,
			OutAuthorityScopes,
			OutError);
	}

	bool IsDurableMutation(const FString& CommandId)
	{
		return CommandId == TEXT("mesh.transform.bake")
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
			FString ResolveError;
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
					ErrorJson(
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
				ErrorJson(
					TEXT("spatial_content_operation_factory_missing"),
					Request.GetCommandId(),
					TEXT("the exact spatial mutation has no factory")));
		}
		DomainCapability::FDomainOperationInvocation Invocation;
		DomainCapability::FDomainOperationInvocationError InvocationError;
		if (!Factory->TryCreate(
			Request, Caller, Cancellation, Invocation, InvocationError))
		{
			return FUeAgentCommandResult::Failure(
				Request,
				TEXT("{}"),
				ErrorJson(
					TEXT("spatial_content_runtime_unavailable"),
					Request.GetCommandId(),
					InvocationError.GetCode() + TEXT(":")
						+ InvocationError.GetDetail()));
		}
		BindingGateway::Operation::FOperationLifecycleReceipt Receipt;
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
					BindingGateway::Operation::LexToString(Receipt.GetStatus()),
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
				TEXT("spatial_content_operation_result_invalid"),
				PayloadError);
		}
		return FUeAgentCommandResult::Success(Request, MoveTemp(Payload));
	}
}
