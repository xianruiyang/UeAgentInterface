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
		FString ErrorJson(
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail,
			const FString& TerminalStatus = TEXT("not_started"),
			const bool bRetrySafe = true)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("code"), Code);
			Object->SetStringField(TEXT("subject_id"), SubjectId);
			Object->SetStringField(TEXT("detail"), Detail);
			Object->SetStringField(TEXT("terminal_status"), TerminalStatus);
			Object->SetBoolField(TEXT("retry_safe"), bRetrySafe);
			FString Text;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>>
				Writer = TJsonWriterFactory<TCHAR,
					TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(Object, Writer);
			return Text;
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
				ErrorJson(
					InvocationError.GetDetail().StartsWith(
						TEXT("simple_authoring_handler_missing:"))
						? FString(TEXT("landscape_command_not_bound"))
						: FString(TEXT("landscape_runtime_unavailable")),
					Request.GetCommandId(),
					InvocationError.GetCode() + TEXT(":")
						+ InvocationError.GetDetail()));
		}
		BindingGateway::Operation::FOperationLifecycleReceipt Receipt;
		if (!Invocation.GetService()->Run(
				*Invocation.GetPlan(),
				*Invocation.GetAuthorization(),
				Receipt))
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
					BindingGateway::Operation::LexToString(
						Receipt.GetStatus()),
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
				TEXT("domain_operation_result_invalid"),
				PayloadError);
		}
		return FUeAgentCommandResult::Success(Request, MoveTemp(Payload));
	}
}
