#pragma once

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "DomainAdapters/ModelingGeometry/UeAgentModelingGeometryMechanicsBoundary.h"

namespace UE::UAI::CommandHandlers::ModelingGeometry
{
	UEAGENTINTERFACE_API bool TryCreateHandlers(
		TSharedRef<const DomainAdapters::ModelingGeometry::IUeAgentModelingSessionNativeMechanicsPort>
			NativeMechanicsPort,
		TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError);
}
