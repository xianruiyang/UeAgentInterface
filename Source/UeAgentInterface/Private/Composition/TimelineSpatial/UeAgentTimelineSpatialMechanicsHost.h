#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"

namespace UE::UAI::Composition::TimelineSpatial
{
	UEAGENTINTERFACE_API bool TryResolveTimelineSpatialMechanics(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		FString& OutError);
}
