#include "OperationOwners/SpatialContent/UeAgentSpatialContentOperationOwner.h"

#include "DomainAdapters/SpatialContent/UeAgentSpatialContentMechanicsBoundary.h"

namespace UE::UAI::OperationOwners::SpatialContent
{
	bool TryResolveMechanics(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		FString& OutError)
	{
		return DomainAdapters::SpatialContent::TryResolveMechanics(
			CommandId,
			OutHandler,
			OutAuthorityScopes,
			OutError);
	}

	CommandEdge::FUeAgentCommandResult Submit(
		const TSharedRef<CommandEdge::IUeAgentCommandHandler>& Mechanics,
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
	{
		return DomainAdapters::SpatialContent::Submit(
			Mechanics,
			Request,
			Caller,
			Cancellation);
	}
}
