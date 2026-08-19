#include "OperationOwners/Reference/UeAgentReferenceOperationOwner.h"

#include "Core/DomainCapability/DelegatedCommandOperation/UeAgentDelegatedCommandOperation.h"
#include "DomainAdapters/Reference/UeAgentReferenceMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::OperationOwners::Reference
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

	TSharedRef<const DomainCapability::IDomainOperationInvocationFactory>
	FindOperationInvocationFactory()
	{
		static const TSharedRef<const DomainCapability::
			IDomainOperationInvocationFactory> Factory =
			DomainCapability::DelegatedCommandOperation::CreateInvocationFactory(
				DomainAdapters::Reference::GetCommandId(),
				&DomainAdapters::Reference::TryResolveMechanics,
				true,
				true);
		return Factory;
	}

	FUeAgentCommandResult Submit(
		const FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
	{
		const TSharedRef<const DomainCapability::
			IDomainOperationInvocationFactory> Factory =
			FindOperationInvocationFactory();
		DomainCapability::FDomainOperationInvocation Invocation;
		DomainCapability::FDomainOperationInvocationError InvocationError;
		if (!Factory->TryCreate(
				Request, Caller, Cancellation, Invocation, InvocationError))
		{
			return FUeAgentCommandResult::Failure(
				Request, TEXT("{}"),
				ErrorJson(
					TEXT("reference_runtime_unavailable"),
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
				Request, TEXT("{}"),
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
				Request.GetCorrelationId(), Request.GetRequestId(),
				Request.GetCommandId(), Request.GetCommandBindingHash(),
				TEXT("domain_operation_result_invalid"), PayloadError);
		}
		return FUeAgentCommandResult::Success(Request, MoveTemp(Payload));
	}
}
