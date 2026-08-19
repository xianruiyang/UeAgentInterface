#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"

namespace UE::UAI::OperationOwners::TextureGraph
{
	UEAGENTINTERFACE_API TSharedRef<const DomainCapability::
		IDomainOperationInvocationFactory>
	GetTextureGraphBakeInvocationFactory();

	UEAGENTINTERFACE_API bool TryCreateArtifactHandlers(
		TSharedRef<const DomainCapability::IDomainOperationInvocationFactory>
			BakeFactory,
		TMap<FString, TSharedRef<CommandEdge::IUeAgentCommandHandler>>&
			OutHandlers,
		FString& OutError);

	UEAGENTINTERFACE_API bool TryCreateArtifactHandlers(
		TMap<FString, TSharedRef<CommandEdge::IUeAgentCommandHandler>>&
			OutHandlers,
		FString& OutError);
}
