#include "OperationOwners/CaptureMedia/UeAgentCaptureMediaOwner.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Core/DomainCapability/DelegatedCommandOperation/UeAgentDelegatedCommandOperation.h"
#include "Dom/JsonObject.h"
#include "OperationOwners/LongRunning/UeAgentLongRunningLifecycleOwner.h"

namespace UE::UAI::OperationOwners::CaptureMedia
{
	using namespace Composition::LongRunningSession;
	namespace
	{
		bool IsOwnedCommand(const FString& Id)
		{
			return Id == TEXT("capture.image.execute")
				|| Id == TEXT("capture.media.execute")
				|| Id == TEXT("capture.render.execute");
		}

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
			FString CanonicalError;
			return Contract::Json::TryCanonicalize(
				Error, Text, CanonicalError) ? Text : FString(TEXT("{}"));
		}

		bool TryReadOperationError(
			const FString& Payload,
			FString& OutError)
		{
			OutError.Reset();
			TSharedPtr<FJsonObject> Root;
			FString ParseError;
			if (!Contract::Json::TryParseObject(
					Payload, Root, ParseError) || !Root.IsValid())
			{
				return false;
			}
			const TSharedPtr<FJsonObject>* Error = nullptr;
			if (Root->TryGetObjectField(TEXT("operation_error"), Error)
				&& Error && Error->IsValid())
			{
				return Contract::Json::TryCanonicalize(
					Error->ToSharedRef(), OutError, ParseError);
			}
			return true;
		}

		class FPortMechanicsHandler final
			: public CommandEdge::IUeAgentCommandHandler
		{
		public:
			FPortMechanicsHandler(
				FUeAgentCaptureMediaRoute InRoute,
				TSharedRef<Contract::IUeAgentOperationApplicationPort> InPort)
				: Route(MoveTemp(InRoute))
				, Port(MoveTemp(InPort))
			{
			}

			const FString& GetCommandId() const override
			{
				return Route.CommandId;
			}

			CommandEdge::FUeAgentCommandResult Submit(
				const CommandEdge::FUeAgentCommandRequest& Request,
				const Contract::FUeAgentCallerContext& Caller,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
				override
			{
				const Contract::FUeAgentOperationResult Result = Port->Submit(
					Contract::FUeAgentOperationRequest(
						Route.DescriptorIdentity,
						Request.GetRequestId(),
						TEXT("uai-command:") + Request.GetRequestId(),
						Contract::FUeAgentContractValue(Request.GetParamsJson())),
					Caller,
					Cancellation);
				if (!Result.IsValid()
					|| !(Result.GetDescriptorIdentity() == Route.DescriptorIdentity)
					|| Result.GetRequestId() != Request.GetRequestId())
				{
					return CommandEdge::FUeAgentCommandResult::Failure(
						Request,
						TEXT("{}"),
						ErrorJson(
							TEXT("capture_media_result_invalid"),
							Request.GetRequestId(),
							TEXT("OperationService returned an invalid result.")));
				}
				const FString& Payload = Result.GetValue().GetJsonValue();
				FString OperationError;
				if (!TryReadOperationError(Payload, OperationError))
				{
					return CommandEdge::FUeAgentCommandResult::Failure(
						Request,
						TEXT("{}"),
						ErrorJson(
							TEXT("capture_media_payload_invalid"),
							Request.GetRequestId(),
							TEXT("OperationService returned a non-object payload.")));
				}
				return OperationError.IsEmpty()
					? CommandEdge::FUeAgentCommandResult::Success(Request, Payload)
					: CommandEdge::FUeAgentCommandResult::Failure(
						Request, Payload, OperationError);
			}

		private:
			FUeAgentCaptureMediaRoute Route;
			TSharedRef<Contract::IUeAgentOperationApplicationPort> Port;
		};

		bool TryResolveMechanics(
			const FString& CommandId,
			TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
			TArray<FString>& OutAuthorityScopes,
			FString& OutError)
		{
			OutHandler.Reset();
			OutAuthorityScopes.Reset();
			OutError.Reset();
			const FUeAgentCaptureMediaRoute* Route = GetRoutes().FindByPredicate(
				[&CommandId](const FUeAgentCaptureMediaRoute& Item)
				{
					return Item.CommandId == CommandId;
				});
			if (!Route)
			{
				OutError = TEXT("capture_media_command_not_supported:") + CommandId;
				return false;
			}
			TSharedPtr<Contract::IUeAgentOperationApplicationPort> Port;
			if (!TryGetProductionOperationPort(Port, OutError) || !Port.IsValid())
			{
				return false;
			}
			OutHandler = MakeShared<FPortMechanicsHandler>(
				*Route, Port.ToSharedRef());
			OutAuthorityScopes = {TEXT("uai:operation"), TEXT("uai:capture")};
			return true;
		}
	}

	bool FUeAgentCaptureMediaRoute::IsValid() const
	{
		return IsOwnedCommand(CommandId)
			&& !PythonFunction.IsEmpty()
			&& DescriptorIdentity.IsValid();
	}

	const TArray<FUeAgentCaptureMediaRoute>& GetRoutes()
	{
		static const TArray<FUeAgentCaptureMediaRoute> Routes = []()
		{
			TArray<FUeAgentCaptureMediaRoute> Result;
			for (const FUeAgentLongRunningSessionActivationProfile& Profile
				: GetLongRunningSessionActivationProfiles())
			{
				if (!IsOwnedCommand(Profile.CommandId)) continue;
				FUeAgentCaptureMediaRoute Route{
					Profile.CommandId, Profile.Method, Profile.DescriptorIdentity};
				if (!Route.IsValid()) return TArray<FUeAgentCaptureMediaRoute>();
				Result.Add(MoveTemp(Route));
			}
			Result.Sort([](const auto& A, const auto& B)
			{
				return A.CommandId < B.CommandId;
			});
			return Result.Num() == 3 ? Result : TArray<FUeAgentCaptureMediaRoute>();
		}();
		return Routes;
	}

	bool TryGetProductionOperationPort(
		TSharedPtr<Contract::IUeAgentOperationApplicationPort>& OutPort,
		FString& OutError)
	{
		TSharedPtr<FUeAgentLongRunningSessionRuntime> Runtime;
		return OperationOwners::LongRunning::
			TryGetProductionLongRunningLifecyclePort(OutPort, Runtime, OutError)
			&& OutPort.IsValid();
	}

	TSharedRef<const DomainCapability::IDomainOperationInvocationFactory>
	FindOperationInvocationFactory(const FString& CommandId)
	{
		static const TArray<TSharedRef<const DomainCapability::
			IDomainOperationInvocationFactory>> Factories = []
		{
			TArray<TSharedRef<const DomainCapability::
				IDomainOperationInvocationFactory>> Result;
			for (const FUeAgentCaptureMediaRoute& Route : GetRoutes())
			{
				Result.Add(DomainCapability::DelegatedCommandOperation::
					CreateInvocationFactory(
						Route.CommandId,
						&TryResolveMechanics,
						false,
						true));
			}
			return Result;
		}();
		const TSharedRef<const DomainCapability::
			IDomainOperationInvocationFactory>* Found =
			Factories.FindByPredicate(
				[&CommandId](const TSharedRef<const DomainCapability::
					IDomainOperationInvocationFactory>& Factory)
				{
					return Factory->GetOperationId() == CommandId;
				});
		checkf(Found, TEXT("unknown capture media operation %s"), *CommandId);
		return Found ? *Found : Factories[0];
	}

	CommandEdge::FUeAgentCommandResult Submit(
		const CommandEdge::FUeAgentCommandRequest& Request,
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
			return CommandEdge::FUeAgentCommandResult::Failure(
				Request,
				TEXT("{}"),
				ErrorJson(
					TEXT("capture_media_runtime_unavailable"),
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
			return CommandEdge::FUeAgentCommandResult::Failure(
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
			return CommandEdge::FUeAgentCommandResult::ProtocolError(
				Request.GetCorrelationId(),
				Request.GetRequestId(),
				Request.GetCommandId(),
				Request.GetCommandBindingHash(),
				TEXT("domain_operation_result_invalid"),
				PayloadError);
		}
		return CommandEdge::FUeAgentCommandResult::Success(
			Request, MoveTemp(Payload));
	}
}
