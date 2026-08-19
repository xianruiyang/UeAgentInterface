#pragma once

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringComposition.h"

namespace UE::UAI::CommandHandlers::AssetSimple
{
	UEAGENTINTERFACE_API bool TryCreateAssetSimpleOperationCommandHandlers(
		const TArray<Composition::SimpleAuthoring::
			FUeAgentSimpleAuthoringCommandRoute>& Routes,
		TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>&
			OutHandlers,
		FString& OutError);
}
