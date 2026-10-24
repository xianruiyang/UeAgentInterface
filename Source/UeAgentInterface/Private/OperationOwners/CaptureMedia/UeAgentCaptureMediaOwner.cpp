#include "OperationOwners/CaptureMedia/UeAgentCaptureMediaOwner.h"
#include "CommandEdge/UeAgentApplicationResultProjection.h"
#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Core/DomainCapability/DelegatedCommandOperation/UeAgentDelegatedCommandOperation.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
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

		UE::UAI::Contract::FDiagnostic CommandFailure(FString Code, const FString& Subject, FString Reason, FString InternalContext = FString())
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(MoveTemp(Code), MoveTemp(Reason), TEXT("command"), MoveTemp(InternalContext), Subject);
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.TerminalStatus = TEXT("not_started");
			Diagnostic.bRetrySafe = true;
			Diagnostic.MutationStarted = false;
			Diagnostic.MutationMayHaveStarted = false;
			return Diagnostic;
		}

		UE::UAI::Contract::FDiagnostic InvocationFailure(const FString& Code, const FString& Subject, const UE::UAI::Contract::FDiagnostic& Cause)
		{
			auto Diagnostic = Cause.IsEmpty()
				? CommandFailure(Code, Subject, TEXT("The operation runtime could not be prepared."))
				: Contract::FDiagnostic::WrapFailure(Code, Cause, TEXT("command"), TEXT("The operation runtime could not be prepared."));
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = Subject;
			Diagnostic.TerminalStatus = TEXT("not_started");
			Diagnostic.bRetrySafe = true;
			Diagnostic.MutationStarted = false;
			Diagnostic.MutationMayHaveStarted = false;
			if (Cause.IsEmpty())
			{
				Diagnostic.PropertyPath = TEXT("runtime_ready");
				Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
				Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			}
			return Diagnostic;
		}

		UE::UAI::Contract::FDiagnostic ProjectionFailure(const FString& Code, const UE::UAI::Contract::FDiagnostic& Cause,
			const BindingGateway::Operation::FOperationLifecycleReceipt& Receipt)
		{
			auto Diagnostic = Cause.IsEmpty()
				? Contract::FDiagnostic::Failure(Code, TEXT("The operation completed, but its result payload could not be produced."), TEXT("result_projection"))
				: Contract::FDiagnostic::WrapFailure(Code, Cause, TEXT("result_projection"), TEXT("The operation completed, but its result payload could not be produced."));
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			if (Cause.IsEmpty())
			{
				Diagnostic.SourceKind = TEXT("native_result");
				Diagnostic.PropertyPath = TEXT("result_payload_ready");
				Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
				Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			}
			Diagnostic.TerminalStatus = BindingGateway::Operation::LexToString(Receipt.GetStatus());
			Diagnostic.MutationStarted = Receipt.DidMutationStart();
			Diagnostic.MutationMayHaveStarted = Receipt.DidMutationStart();
			Diagnostic.bRetrySafe = Receipt.IsRetrySafe();
			return Diagnostic;
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
				CommandEdge::FApplicationResultProjectionPolicy Policy;
				Policy.InvalidResultCode = TEXT("capture_media_result_invalid");
				Policy.BoundaryFailureKind = CommandEdge::EApplicationFailureKind::None;
				Policy.ApplicationFailureKind = CommandEdge::EApplicationFailureKind::None;
				Policy.bValidateObjectPayload = false; // The public codec owns this route's payload validation.
				Policy.bInvalidResultMayHaveMutated = true;
				return CommandEdge::ProjectOperationResult(Request, Route.DescriptorIdentity, Result, Policy);
			}

		private:
			FUeAgentCaptureMediaRoute Route;
			TSharedRef<Contract::IUeAgentOperationApplicationPort> Port;
		};

		bool TryResolveMechanics(
			const FString& CommandId,
			TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
			TArray<FString>& OutAuthorityScopes,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutHandler.Reset();
			OutAuthorityScopes.Reset();
			OutError.Reset();
			const TArray<FUeAgentCaptureMediaRoute>* Routes = nullptr;
			if (!TryGetRoutes(Routes, OutError)) return false;
			const FUeAgentCaptureMediaRoute* Route = Routes->FindByPredicate(
				[&CommandId](const FUeAgentCaptureMediaRoute& Item)
				{
					return Item.CommandId == CommandId;
				});
			if (!Route)
			{
				OutError = CommandFailure(TEXT("capture_media_command_not_supported"), CommandId,
					TEXT("This owner accepts capture.image.execute, capture.media.execute and capture.render.execute."));
				OutError.PropertyPath = TEXT("command_supported");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
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

	bool TryGetRoutes(const TArray<FUeAgentCaptureMediaRoute>*& OutRoutes, Contract::FDiagnostic& OutError)
	{
		OutRoutes = nullptr;
		const TArray<FUeAgentLongRunningSessionActivationProfile>* Profiles = nullptr;
		if (!TryGetLongRunningSessionActivationProfiles(Profiles, OutError)) return false;
		static const TArray<FUeAgentCaptureMediaRoute> Routes = [Profiles]()
		{
			TArray<FUeAgentCaptureMediaRoute> Result;
			for (const FUeAgentLongRunningSessionActivationProfile& Profile
				: *Profiles)
			{
				if (!IsOwnedCommand(Profile.CommandId)) continue;
				FUeAgentCaptureMediaRoute Route{
					Profile.CommandId, Profile.Method, Profile.DescriptorIdentity};
				Result.Add(MoveTemp(Route));
			}
			Result.Sort([](const auto& A, const auto& B)
			{
				return A.CommandId < B.CommandId;
			});
			return Result;
		}();
		if (Routes.Num() != 3)
		{
			OutError = CommandFailure(TEXT("capture_media_route_count_mismatch"), FString(),
				TEXT("The activation profiles did not produce the three capture command routes."));
			OutError.PropertyPath = TEXT("route_count");
			OutError.ActualValue = MakeShared<FJsonValueNumber>(Routes.Num());
			OutError.ExpectedValue = MakeShared<FJsonValueNumber>(3);
			return false;
		}
		OutRoutes = &Routes;
		return true;
	}

	bool TryGetProductionOperationPort(
		TSharedPtr<Contract::IUeAgentOperationApplicationPort>& OutPort,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		TSharedPtr<FUeAgentLongRunningSessionRuntime> Runtime;
		return OperationOwners::LongRunning::
			TryGetProductionLongRunningLifecyclePort(OutPort, Runtime, OutError)
			&& OutPort.IsValid();
	}

	bool TryFindOperationInvocationFactory(
		const FString& CommandId,
		TSharedPtr<const DomainCapability::IDomainOperationInvocationFactory>& OutFactory,
		Contract::FDiagnostic& OutError)
	{
		OutFactory.Reset();
		const TArray<FUeAgentCaptureMediaRoute>* Routes = nullptr;
		if (!TryGetRoutes(Routes, OutError)) return false;
		static const TArray<TSharedRef<const DomainCapability::
			IDomainOperationInvocationFactory>> Factories = [Routes]()
		{
			TArray<TSharedRef<const DomainCapability::
				IDomainOperationInvocationFactory>> Result;
			for (const FUeAgentCaptureMediaRoute& Route : *Routes)
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
		if (!Found)
		{
			OutError = CommandFailure(TEXT("capture_media_command_not_supported"), CommandId,
				TEXT("This owner accepts capture.image.execute, capture.media.execute and capture.render.execute."));
			OutError.PropertyPath = TEXT("command_supported");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return false;
		}
		OutFactory = *Found;
		return true;
	}

	CommandEdge::FUeAgentCommandResult Submit(
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
	{
		TSharedPtr<const DomainCapability::IDomainOperationInvocationFactory> Factory;
		Contract::FDiagnostic FactoryError;
		if (!TryFindOperationInvocationFactory(Request.GetCommandId(), Factory, FactoryError))
			return CommandEdge::FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(FactoryError));
		DomainCapability::FDomainOperationInvocation Invocation;
		DomainCapability::FDomainOperationInvocationError InvocationError;
		if (!Factory->TryCreate(
				Request, Caller, Cancellation, Invocation, InvocationError))
		{
			return CommandEdge::FUeAgentCommandResult::Failure(
				Request,
				TEXT("{}"),
				InvocationFailure(TEXT("capture_media_runtime_unavailable"), Request.GetCommandId(), InvocationError));
		}
		BindingGateway::Operation::FOperationLifecycleReceipt Receipt;
		if (!Invocation.GetService()->Run(
				*Invocation.GetPlan(), *Invocation.GetAuthorization(), Receipt))
		{
			auto Diagnostic = Receipt.GetDiagnostic();
			if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = Request.GetCommandId();
			return CommandEdge::FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(Diagnostic));
		}
		FString Payload;
		UE::UAI::Contract::FDiagnostic PayloadError;
		if (!Invocation.GetContext()->TryBuildSuccessPayload(
				Receipt, Payload, PayloadError))
		{
			return CommandEdge::FUeAgentCommandResult::ProtocolError(Request.GetCorrelationId(), Request.GetRequestId(), Request.GetCommandId(), Request.GetCommandBindingHash(), ProjectionFailure(TEXT("domain_operation_result_invalid"), PayloadError, Receipt));
		}
		return CommandEdge::FUeAgentCommandResult::Success(
			Request, MoveTemp(Payload));
	}
}
