#pragma once

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"

namespace UE::UAI::CommandHandlers::TextureGraph
{
	UEAGENTINTERFACE_API bool TryCreateHandlers(
		TSharedRef<const DomainCapability::IDomainOperationInvocationFactory>
			BakeFactory,
		TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError);
}
