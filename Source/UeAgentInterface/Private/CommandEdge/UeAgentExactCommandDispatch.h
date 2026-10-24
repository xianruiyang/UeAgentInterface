#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/UeAgentContract.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::CommandEdge
{
	class FUeAgentExactCommandDispatch;

	class UEAGENTINTERFACE_API IUeAgentExactCommandHandler
	{
	public:
		virtual ~IUeAgentExactCommandHandler() = default;

		virtual const FString& GetCommandId() const = 0;
		virtual const FString& GetPublicOwner() const = 0;
		virtual FUeAgentCommandResult Submit(
			const FUeAgentCommandRequest& Request,
			const Contract::FUeAgentCallerContext& CallerContext,
			const TSharedRef<Contract::IUeAgentCancellationSignal>&
				Cancellation) = 0;

		virtual bool BindNestedExactDispatch(
			TWeakPtr<const FUeAgentExactCommandDispatch> Dispatch,
			TSharedRef<
				const CommandRegistry::FUeAgentImmutableCommandRegistry>
				Registry,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutError.Reset();
			return true;
		}
	};

	class UEAGENTINTERFACE_API FUeAgentExactCommandDispatch final
	{
	public:
		static bool TryCreate(
			TSharedRef<
				const CommandRegistry::FUeAgentImmutableCommandRegistry>
				Registry,
			TArray<TSharedRef<IUeAgentExactCommandHandler>> Handlers,
			TSharedPtr<const FUeAgentExactCommandDispatch>& OutDispatch,
			UE::UAI::Contract::FDiagnostic& OutError);

		FUeAgentCommandResult Submit(
			const FUeAgentCommandRequest& Request,
			const Contract::FUeAgentCallerContext& CallerContext,
			const TSharedRef<Contract::IUeAgentCancellationSignal>&
				Cancellation) const;

	private:
		FUeAgentExactCommandDispatch(
			TSharedRef<
				const CommandRegistry::FUeAgentImmutableCommandRegistry>
				InRegistry,
			TMap<FString, TSharedRef<IUeAgentExactCommandHandler>>
				InHandlers);

		TSharedRef<
			const CommandRegistry::FUeAgentImmutableCommandRegistry>
			Registry;
		TMap<FString, TSharedRef<IUeAgentExactCommandHandler>> Handlers;
	};
}
