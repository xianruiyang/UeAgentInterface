#include "CommandEdge/UeAgentCommandDispatch.h"

#include "Core/Contract/UeAgentDiagnostic.h"

namespace UE::UAI::CommandEdge
{
	using namespace CommandContract;
	using namespace Contract;

	namespace
	{
		FDiagnostic CommandDispatchFailure(const TCHAR* Code, const TCHAR* Reason, const FString& CommandId)
		{
			auto Diagnostic = FDiagnostic::Failure(Code, Reason, TEXT("command_composition"));
			Diagnostic.CandidateName = CommandId;
			Diagnostic.SourceKind = TEXT("native_result");
			return Diagnostic;
		}
	}

	bool FUeAgentCommandDispatch::TryCreate(
		TSharedRef<const FUeAgentCommandContractRegistry> Registry,
		TArray<TSharedRef<IUeAgentCommandHandler>> InHandlers,
		TSharedPtr<const FUeAgentCommandDispatch>& OutDispatch,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutDispatch.Reset();
		OutError.Reset();

		TMap<FString, TSharedRef<IUeAgentCommandHandler>> ById;
		for (const TSharedRef<IUeAgentCommandHandler>& Handler
			: InHandlers)
		{
			const FString& CommandId = Handler->GetCommandId();
			if (CommandId.IsEmpty()
				|| !Registry->FindExact(CommandId))
			{
				OutError = CommandDispatchFailure(TEXT("command_dispatch_handler_unknown"), CommandId.IsEmpty()
					? TEXT("The command handler must declare a non-empty command identifier.")
					: TEXT("The handler's command is absent from the command registry."), CommandId);
				return false;
			}
			if (ById.Contains(CommandId))
			{
				OutError = CommandDispatchFailure(TEXT("command_dispatch_handler_duplicate"),
					TEXT("More than one handler is registered for this command."), CommandId);
				return false;
			}
			ById.Add(CommandId, Handler);
		}

		for (const FUeAgentCommandContract& Command
			: Registry->GetContracts())
		{
			if (Command.GetStatus() == EUeAgentCommandStatus::Active
				&& !ById.Contains(Command.GetCommandId()))
			{
				OutError = CommandDispatchFailure(TEXT("command_dispatch_handler_missing"),
					TEXT("The active command has no registered handler."), Command.GetCommandId());
				return false;
			}
		}

		OutDispatch = MakeShareable(
			new FUeAgentCommandDispatch(MoveTemp(ById)));
		return true;
	}

	FUeAgentCommandResult FUeAgentCommandDispatch::Submit(
		const FUeAgentCommandRequest& Request,
		const FUeAgentCallerContext& CallerContext,
		const TSharedRef<IUeAgentCancellationSignal>& Cancellation) const
	{
		const TSharedRef<IUeAgentCommandHandler>* Handler =
			Handlers.Find(Request.GetCommandId());
		if (!Handler)
		{
			return FUeAgentCommandResult::ProtocolError(Request.GetCorrelationId(), Request.GetRequestId(), Request.GetCommandId(), Request.GetCommandBindingHash(), UE::UAI::Contract::FDiagnostic::Failure(TEXT("command_dispatch_handler_missing"), TEXT("no exact command handler is registered"), TEXT("command_protocol")));
		}

		FUeAgentCommandResult Result =
			(*Handler)->Submit(Request, CallerContext, Cancellation);
		UE::UAI::Contract::FDiagnostic ResultError;
		if (!Result.TryValidateForRequest(Request, ResultError))
		{
			return FUeAgentCommandResult::ProtocolError(Request.GetCorrelationId(), Request.GetRequestId(),
				Request.GetCommandId(), Request.GetCommandBindingHash(), MoveTemp(ResultError));
		}
		return Result;
	}

	FUeAgentCommandDispatch::FUeAgentCommandDispatch(
		TMap<FString, TSharedRef<IUeAgentCommandHandler>>
			InHandlers)
		: Handlers(MoveTemp(InHandlers))
	{
	}
}
