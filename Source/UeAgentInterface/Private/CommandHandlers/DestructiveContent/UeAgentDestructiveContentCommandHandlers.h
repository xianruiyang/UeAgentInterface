#pragma once

#include "CommandEdge/UeAgentExactCommandDispatch.h"

namespace UE::UAI::CommandHandlers::DestructiveContent
{
	UEAGENTINTERFACE_API bool TryCreateDestructiveContentCommandHandlers(
		TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError);
}
