#include "QueryOwners/AnimationBlueprintCommands/UeAgentAnimationBlueprintCommandQueryOwner.h"

namespace UE::UAI::QueryOwners::AnimationBlueprintCommands
{
	CommandEdge::FUeAgentCommandResult Submit(
		const TSharedRef<CommandEdge::IUeAgentCommandHandler>& Mechanics,
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
	{
		return Mechanics->Submit(Request, Caller, Cancellation);
	}
}

