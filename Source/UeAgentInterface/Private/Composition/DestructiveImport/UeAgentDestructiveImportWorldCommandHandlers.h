#pragma once

#include "CoreMinimal.h"
#include "CommandEdge/UeAgentCommandDispatch.h"
#include "DomainAdapters/WorldDestructive/UeAgentWorldDestructiveOwner.h"

namespace UE::UAI::Composition::DestructiveImport
{
	UEAGENTINTERFACE_API bool TryCreateDestructiveImportWorldCommandHandlers(
		TSharedRef<DomainAdapters::WorldDestructive::
			IUeAgentWorldDestructiveOwner> Owner,
		TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>& OutHandlers,
		FString& OutError);
}
