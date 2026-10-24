#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/Contract/UeAgentContract.h"

#include "Templates/SharedPointer.h"

namespace UE::UAI::CommandEdge
{
	class UEAGENTINTERFACE_API IUeAgentCommandHandler
	{
	public:
		virtual ~IUeAgentCommandHandler() = default;

		virtual const FString& GetCommandId() const = 0;

		virtual FUeAgentCommandResult Submit(
			const FUeAgentCommandRequest& Request,
			const Contract::FUeAgentCallerContext& CallerContext,
			const TSharedRef<Contract::IUeAgentCancellationSignal>&
				Cancellation) = 0;
	};

	class UEAGENTINTERFACE_API FUeAgentCommandDispatch final
	{
	public:
		static bool TryCreate(
			TSharedRef<
				const CommandContract::FUeAgentCommandContractRegistry>
				Registry,
			TArray<TSharedRef<IUeAgentCommandHandler>> Handlers,
			TSharedPtr<const FUeAgentCommandDispatch>& OutDispatch,
			UE::UAI::Contract::FDiagnostic& OutError);

		FUeAgentCommandResult Submit(
			const FUeAgentCommandRequest& Request,
			const Contract::FUeAgentCallerContext& CallerContext,
			const TSharedRef<Contract::IUeAgentCancellationSignal>&
				Cancellation) const;

	private:
		explicit FUeAgentCommandDispatch(
			TMap<FString, TSharedRef<IUeAgentCommandHandler>>
				InHandlers);

		TMap<FString, TSharedRef<IUeAgentCommandHandler>> Handlers;
	};
}
