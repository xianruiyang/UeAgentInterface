#include "CommandEdge/UeAgentCommandDispatch.h"

namespace UE::UAI::CommandEdge
{
	using namespace CommandContract;
	using namespace Contract;

	bool FUeAgentCommandDispatch::TryCreate(
		TSharedRef<const FUeAgentCommandContractRegistry> Registry,
		TArray<TSharedRef<IUeAgentCommandHandler>> InHandlers,
		TSharedPtr<const FUeAgentCommandDispatch>& OutDispatch,
		FString& OutError)
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
				OutError = TEXT("command_dispatch_handler_unknown");
				return false;
			}
			if (ById.Contains(CommandId))
			{
				OutError =
					TEXT("command_dispatch_handler_duplicate");
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
				OutError = TEXT("command_dispatch_handler_missing");
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
			return FUeAgentCommandResult::ProtocolError(
				Request.GetCorrelationId(),
				Request.GetRequestId(),
				Request.GetCommandId(),
				Request.GetCommandBindingHash(),
				TEXT("command_dispatch_handler_missing"),
				TEXT("no exact command handler is registered"));
		}

		FUeAgentCommandResult Result =
			(*Handler)->Submit(Request, CallerContext, Cancellation);
		if (!Result.IsValid() || !Result.MatchesRequest(Request))
		{
			return FUeAgentCommandResult::ProtocolError(
				Request.GetCorrelationId(),
				Request.GetRequestId(),
				Request.GetCommandId(),
				Request.GetCommandBindingHash(),
				TEXT("command_handler_result_identity_mismatch"),
				TEXT("command handler returned a mismatched result"));
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
