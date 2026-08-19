#pragma once

#include "CommandEdge/UeAgentExactCommandDispatch.h"

namespace UE::UAI::DomainAdapters::OperationPlan
{
	class UEAGENTINTERFACE_API FUeAgentOperationPlan final
	{
	public:
		explicit FUeAgentOperationPlan(
			TSharedRef<
				const CommandRegistry::FUeAgentImmutableCommandRegistry>
				InRegistry);

		bool BindNestedExactDispatch(
			TWeakPtr<const CommandEdge::FUeAgentExactCommandDispatch>
				InDispatch,
			TSharedRef<
				const CommandRegistry::FUeAgentImmutableCommandRegistry>
				InRegistry,
			FString& OutError);

		CommandEdge::FUeAgentCommandResult Execute(
			const CommandEdge::FUeAgentCommandRequest& Request,
			const Contract::FUeAgentCallerContext& Caller,
			const TSharedRef<Contract::IUeAgentCancellationSignal>&
				Cancellation) const;

	private:
		TSharedRef<
			const CommandRegistry::FUeAgentImmutableCommandRegistry>
			Registry;
		TWeakPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	};
}
