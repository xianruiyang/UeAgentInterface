#include "DomainAdapters/RigSpatialCommands/UeAgentRigSpatialCommandMechanicsBoundary.h"

#include "Composition/TimelineSpatial/UeAgentTimelineSpatialMechanicsHost.h"

namespace UE::UAI::DomainAdapters::RigSpatialCommands
{
	bool TryResolveMechanics(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		FString& OutError)
	{
		return Composition::TimelineSpatial::TryResolveTimelineSpatialMechanics(
			CommandId, OutHandler, OutAuthorityScopes, OutError);
	}

	CommandEdge::FUeAgentCommandResult Submit(
		const TSharedRef<CommandEdge::IUeAgentCommandHandler>& Mechanics,
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
	{
		return Mechanics->Submit(Request, Caller, Cancellation);
	}
}
