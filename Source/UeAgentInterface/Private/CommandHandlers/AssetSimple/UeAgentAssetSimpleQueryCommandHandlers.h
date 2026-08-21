#pragma once

#include "CommandEdge/UeAgentExactCommandDispatch.h"

namespace UE::UAI::CommandHandlers::AssetSimple
{
	UEAGENTINTERFACE_API bool TryCreateAssetSimpleQueryCommandHandlers(
		TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>&
			OutHandlers,
		FString& OutError);
}
