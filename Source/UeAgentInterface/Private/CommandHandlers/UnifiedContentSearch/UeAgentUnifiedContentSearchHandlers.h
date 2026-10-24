#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::CommandHandlers::UnifiedContentSearch
{
	UEAGENTINTERFACE_API bool TryCreateUnifiedContentSearchHandlers(
		TArray<TSharedRef<UE::UAI::CommandEdge::
			IUeAgentExactCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError);
}
