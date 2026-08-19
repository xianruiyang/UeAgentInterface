#pragma once

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringComposition.h"

namespace UE::UAI::CommandHandlers::Landscape
{
	UEAGENTINTERFACE_API bool TryCreateHandlers(
		const TArray<Composition::SimpleAuthoring::FUeAgentSimpleAuthoringCommandRoute>&
			Routes,
		TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError);
}
