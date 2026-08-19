#pragma once

#include "CoreMinimal.h"
#include "CommandEdge/UeAgentCommandDispatch.h"
#include "DomainAdapters/ExternalDataImport/UeAgentExternalDataImportOwner.h"

namespace UE::UAI::Composition::DestructiveImport
{
	UEAGENTINTERFACE_API bool TryCreateDestructiveImportExternalCommandHandlers(
		TSharedRef<DomainAdapters::ExternalDataImport::
			IUeAgentExternalDataImportOwner> Owner,
		TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>& OutHandlers,
		FString& OutError);
}
