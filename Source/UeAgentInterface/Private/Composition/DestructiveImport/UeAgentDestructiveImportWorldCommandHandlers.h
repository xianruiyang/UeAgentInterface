#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "CommandEdge/UeAgentCommandDispatch.h"
#include "DomainAdapters/WorldDestructive/UeAgentWorldDestructiveOwner.h"

namespace UE::UAI::Composition::DestructiveImport
{
	UEAGENTINTERFACE_API bool TryCreateDestructiveImportWorldCommandHandlers(
		TSharedRef<DomainAdapters::WorldDestructive::
			IUeAgentWorldDestructiveOwner> Owner,
		TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError);
}
