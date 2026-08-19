#include "OperationOwners/TimelineSequenceCommands/UeAgentTimelineSequenceCommandOperationOwner.h"

#include "DomainAdapters/TimelineSequenceCommands/UeAgentTimelineSequenceCommandMechanicsBoundary.h"

namespace UE::UAI::OperationOwners::TimelineSequenceCommands
{
	bool TryResolveMechanics(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		FString& OutError)
	{
		return DomainAdapters::TimelineSequenceCommands::TryResolveMechanics(
			CommandId, OutHandler, OutAuthorityScopes, OutError);
	}

	CommandEdge::FUeAgentCommandResult Submit(
		const TSharedRef<CommandEdge::IUeAgentCommandHandler>& Mechanics,
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
	{
		return DomainAdapters::TimelineSequenceCommands::Submit(
			Mechanics, Request, Caller, Cancellation);
	}
}
