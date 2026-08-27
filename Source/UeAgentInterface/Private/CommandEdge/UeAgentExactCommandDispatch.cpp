#include "CommandEdge/UeAgentExactCommandDispatch.h"

#include "Core/Contract/Command/UeAgentCommandLifecyclePolicy.h"

namespace UE::UAI::CommandEdge
{
	using namespace CommandContract;
	using namespace CommandRegistry;
	using namespace Contract;

	bool FUeAgentExactCommandDispatch::TryCreate(
		TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
		TArray<TSharedRef<IUeAgentExactCommandHandler>> InHandlers,
		TSharedPtr<const FUeAgentExactCommandDispatch>& OutDispatch,
		FString& OutError)
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
				OutError = TEXT("command_handler_unknown");
				return false;
			}
			if (!CommandLifecycle::IsExecutable(
					Contract->GetAvailability()))
			{
				OutError = TEXT("command_handler_disabled");
				return false;
			}
			if (Handler->GetPublicOwner() != Contract->GetPublicOwner())
			{
				OutError = TEXT("command_handler_owner_mismatch");
				return false;
			}
			if (ById.Contains(CommandId))
			{
				OutError = TEXT("command_handler_duplicate");
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
				OutError = TEXT("command_handler_missing");
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
			FString BindError;
			if (!Handler.Value->BindNestedExactDispatch(
				NestedDispatch, Registry, BindError))
			{
				OutError = TEXT("command_handler_nested_dispatch_bind_failed:")
					+ Handler.Key + TEXT(":") + BindError;
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
		const FUeAgentFrozenCommandContract* Contract =
			Registry->FindExact(Request.GetCommandId());
		if (!Contract)
		{
			return FUeAgentCommandResult::ProtocolError(
				Request.GetCorrelationId(),
				Request.GetRequestId(),
				Request.GetCommandId(),
				Request.GetCommandBindingHash(),
				TEXT("command_not_registered_exact"),
				TEXT("the complete command identity is not registered"));
		}
		if (!CommandLifecycle::IsExecutable(
				Contract->GetAvailability()))
		{
			return FUeAgentCommandResult::ProtocolError(
				Request.GetCorrelationId(),
				Request.GetRequestId(),
				Request.GetCommandId(),
				Request.GetCommandBindingHash(),
				TEXT("command_disabled"),
				TEXT("the exact command contract is disabled"));
		}
		if (Request.GetCommandBindingHash() != Contract->GetContractHash())
		{
			return FUeAgentCommandResult::ProtocolError(
				Request.GetCorrelationId(),
				Request.GetRequestId(),
				Request.GetCommandId(),
				Request.GetCommandBindingHash(),
				TEXT("command_contract_version_mismatch"),
				TEXT("the request was generated from another contract"));
		}

		const TSharedRef<IUeAgentExactCommandHandler>* Handler =
			Handlers.Find(Request.GetCommandId());
		if (!Handler)
		{
			return FUeAgentCommandResult::ProtocolError(
				Request.GetCorrelationId(),
				Request.GetRequestId(),
				Request.GetCommandId(),
				Request.GetCommandBindingHash(),
				TEXT("command_handler_missing"),
				TEXT("the exact command has no composed handler"));
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
				TEXT("the handler returned another request identity"));
		}
		return Result;
	}

	FUeAgentExactCommandDispatch::FUeAgentExactCommandDispatch(
		TSharedRef<const FUeAgentImmutableCommandRegistry> InRegistry,
		TMap<FString, TSharedRef<IUeAgentExactCommandHandler>> InHandlers)
		: Registry(MoveTemp(InRegistry))
		, Handlers(MoveTemp(InHandlers))
	{
	}
}
