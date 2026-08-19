#include "OperationOwners/WorldCommands/UeAgentWorldCommandOperationOwner.h"

#include "Core/DomainCapability/DelegatedCommandOperation/UeAgentDelegatedCommandOperation.h"
#include "DomainAdapters/WorldCommands/UeAgentWorldCommandMechanicsBoundary.h"
#include "Dom/JsonObject.h"
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
			const auto Writer = TJsonWriterFactory<TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(Object, Writer);
			return Text;
		}
	}

	bool TryResolveMechanics(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		FString& OutError)
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
				ErrorJson(
					TEXT("world_command_runtime_unavailable"),
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
					Receipt.GetErrorCode(), Request.GetCommandId(), Detail,
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
