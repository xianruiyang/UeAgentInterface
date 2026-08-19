#include "OperationOwners/AnimationRigCommands/UeAgentAnimationRigCommandOperationOwner.h"

#include "DomainAdapters/AnimationRigCommands/UeAgentAnimationRigCommandMechanicsBoundary.h"

namespace UE::UAI::OperationOwners::AnimationRigCommands
{
	bool TryResolveMechanics(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		FString& OutError)
	{
		return DomainAdapters::AnimationRigCommands::TryResolveMechanics(
			CommandId, OutHandler, OutAuthorityScopes, OutError);
	}

	CommandEdge::FUeAgentCommandResult Submit(
		const TSharedRef<CommandEdge::IUeAgentCommandHandler>& Mechanics,
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
	{
		return DomainAdapters::AnimationRigCommands::Submit(
			Mechanics, Request, Caller, Cancellation);
	}
}
