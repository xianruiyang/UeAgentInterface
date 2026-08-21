#pragma once

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::CommandHandlers::UnifiedContentSearch
{
	UEAGENTINTERFACE_API bool TryCreateUnifiedContentSearchHandlers(
		TArray<TSharedRef<UE::UAI::CommandEdge::
			IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError);
}
