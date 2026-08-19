#include "OperationOwners/RigSpatialCommands/UeAgentRigSpatialCommandOperationOwner.h"

#include "DomainAdapters/RigSpatialCommands/UeAgentRigSpatialCommandMechanicsBoundary.h"

namespace UE::UAI::OperationOwners::RigSpatialCommands
{
	bool TryResolveMechanics(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		FString& OutError)
	{
		return DomainAdapters::RigSpatialCommands::TryResolveMechanics(
			CommandId, OutHandler, OutAuthorityScopes, OutError);
	}

	CommandEdge::FUeAgentCommandResult Submit(
		const TSharedRef<CommandEdge::IUeAgentCommandHandler>& Mechanics,
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
	{
		return DomainAdapters::RigSpatialCommands::Submit(
			Mechanics, Request, Caller, Cancellation);
	}
}
