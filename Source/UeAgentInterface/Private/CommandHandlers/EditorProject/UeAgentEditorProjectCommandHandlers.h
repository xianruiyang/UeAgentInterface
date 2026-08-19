#pragma once

#include "CommandEdge/UeAgentExactCommandDispatch.h"

namespace UE::UAI::CommandHandlers::EditorProject
{
	UEAGENTINTERFACE_API bool TryCreateHandlers(
		TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError);
}
