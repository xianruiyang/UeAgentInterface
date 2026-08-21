#include "DomainAdapters/SpatialContent/UeAgentSpatialContentMechanicsBoundary.h"

#include "Composition/TimelineSpatial/UeAgentTimelineSpatialMechanicsHost.h"

namespace UE::UAI::DomainAdapters::SpatialContent
{
	bool TryResolveMechanics(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		FString& OutError)
	{
		return Composition::TimelineSpatial::TryResolveTimelineSpatialMechanics(
			CommandId,
			OutHandler,
			OutAuthorityScopes,
			OutError);
	}
}
