#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"

namespace UE::UAI::DomainAdapters::SpatialContent
{
	UEAGENTINTERFACE_API bool TryResolveMechanics(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		FString& OutError);
}
