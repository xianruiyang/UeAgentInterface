#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandEdge/UeAgentCommandParameterValidation.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonValue.h"

#include "Core/Contract/Command/UeAgentCommandLifecyclePolicy.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

namespace UE::UAI::CommandEdge
{
	using namespace CommandContract;
	using namespace CommandRegistry;
	using namespace Contract;

	namespace
	{
		FDiagnostic ExactDispatchFailure(const TCHAR* Code, const TCHAR* Reason, const FString& CommandId)
		{
			auto Diagnostic = FDiagnostic::Failure(Code, Reason, TEXT("command_composition"));
			Diagnostic.CandidateName = CommandId;
			Diagnostic.SourceKind = TEXT("native_result");
			return Diagnostic;
		}
	}

	bool FUeAgentExactCommandDispatch::TryCreate(
		TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
		TArray<TSharedRef<IUeAgentExactCommandHandler>> InHandlers,
		TSharedPtr<const FUeAgentExactCommandDispatch>& OutDispatch,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutDispatch.Reset();
		OutError.Reset();
		TMap<FString, TSharedRef<IUeAgentExactCommandHandler>> ById;
		for (const TSharedRef<IUeAgentExactCommandHandler>& Handler :
			InHandlers)
		{
			const FString& CommandId = Handler->GetCommandId();
			const FUeAgentFrozenCommandContract* Contract =
				Registry->FindExact(CommandId);
			if (!Contract)
			{
				OutError = ExactDispatchFailure(TEXT("command_handler_unknown"), CommandId.IsEmpty()
					? TEXT("The exact command handler must declare a non-empty command identifier.")
					: TEXT("The handler's command is absent from the exact command registry."), CommandId);
				return false;
			}
			if (!CommandLifecycle::IsExecutable(
					Contract->GetAvailability()))
			{
				OutError = ExactDispatchFailure(TEXT("command_handler_disabled"),
					TEXT("The handler's command is not executable in its current lifecycle state."), CommandId);
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				OutError.PropertyPath = TEXT("/executable");
				return false;
			}
			if (Handler->GetPublicOwner() != Contract->GetPublicOwner())
			{
				OutError = ExactDispatchFailure(TEXT("command_handler_owner_mismatch"),
					TEXT("The handler owner differs from the command's registered owner."), CommandId);
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Handler->GetPublicOwner());
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Contract->GetPublicOwner());
				return false;
			}
			if (ById.Contains(CommandId))
			{
				OutError = ExactDispatchFailure(TEXT("command_handler_duplicate"),
					TEXT("More than one exact handler is registered for this command."), CommandId);
				return false;
			}
			ById.Add(CommandId, Handler);
		}

		for (const FUeAgentFrozenCommandContract& Contract :
			Registry->GetContracts())
		{
			if (CommandLifecycle::IsExecutable(
					Contract.GetAvailability())
				&& !ById.Contains(Contract.GetCommandId()))
			{
				OutError = ExactDispatchFailure(TEXT("command_handler_missing"),
					TEXT("The executable command has no registered exact handler."), Contract.GetCommandId());
				return false;
			}
		}

		TSharedPtr<FUeAgentExactCommandDispatch> MutableDispatch =
			MakeShareable(new FUeAgentExactCommandDispatch(
			Registry,
			MoveTemp(ById)));
		const TWeakPtr<const FUeAgentExactCommandDispatch> NestedDispatch(
			MutableDispatch);
		for (const TPair<FString, TSharedRef<IUeAgentExactCommandHandler>>&
			Handler : MutableDispatch->Handlers)
		{
			UE::UAI::Contract::FDiagnostic BindError;
			if (!Handler.Value->BindNestedExactDispatch(
				NestedDispatch, Registry, BindError))
			{
				OutError = BindError;
				return false;
			}
		}
		OutDispatch = MoveTemp(MutableDispatch);
		return true;
	}

	FUeAgentCommandResult FUeAgentExactCommandDispatch::Submit(
		const FUeAgentCommandRequest& Request,
		const FUeAgentCallerContext& CallerContext,
		const TSharedRef<IUeAgentCancellationSignal>& Cancellation) const
	{
		auto Reject = [&](const TCHAR* Code, const TCHAR* Reason)
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("command_protocol"));
			Diagnostic.CandidateName = Request.GetCommandId();
			Diagnostic.Source.JsonPath = TEXT("/command_id");
			return FUeAgentCommandResult::ProtocolError(Request.GetCorrelationId(), Request.GetRequestId(),
				Request.GetCommandId(), Request.GetCommandBindingHash(), MoveTemp(Diagnostic));
		};
		const FUeAgentFrozenCommandContract* Contract =
			Registry->FindExact(Request.GetCommandId());
		if (!Contract)
		{
			return Reject(TEXT("command_not_registered_exact"), TEXT("The requested command is not registered."));
		}
		if (!CommandLifecycle::IsExecutable(
				Contract->GetAvailability()))
		{
			return Reject(TEXT("command_disabled"), TEXT("The requested command is disabled."));
		}
		UE::UAI::Contract::FDiagnostic ParameterError;
		if (!TryValidateCommandParameters(*Contract, Request.GetParams(), ParameterError))
			return FUeAgentCommandResult::ProtocolError(Request.GetCorrelationId(), Request.GetRequestId(),
				Request.GetCommandId(), Request.GetCommandBindingHash(), MoveTemp(ParameterError));
		const TSharedRef<IUeAgentExactCommandHandler>* Handler =
			Handlers.Find(Request.GetCommandId());
		if (!Handler)
		{
			return Reject(TEXT("command_handler_missing"), TEXT("The requested command has no composed handler."));
		}
		UeAgentAuthoringDiagnostics::FScopedWarnings Warnings;
		FUeAgentCommandResult Result =
			(*Handler)->Submit(Request, CallerContext, Cancellation);
		UE::UAI::Contract::FDiagnostic ResultError;
		if (!Result.TryValidateForRequest(Request, ResultError))
		{
			return FUeAgentCommandResult::ProtocolError(Request.GetCorrelationId(), Request.GetRequestId(),
				Request.GetCommandId(), Request.GetCommandBindingHash(), MoveTemp(ResultError));
		}
		if (Result.IsProtocolError()) return Result;
		const FString Payload = Warnings.AppendToPayload(Result.GetPayloadJson());
		return Result.WithPayload(Payload);
	}

	FUeAgentExactCommandDispatch::FUeAgentExactCommandDispatch(
		TSharedRef<const FUeAgentImmutableCommandRegistry> InRegistry,
		TMap<FString, TSharedRef<IUeAgentExactCommandHandler>> InHandlers)
		: Registry(MoveTemp(InRegistry))
		, Handlers(MoveTemp(InHandlers))
	{
	}
}
