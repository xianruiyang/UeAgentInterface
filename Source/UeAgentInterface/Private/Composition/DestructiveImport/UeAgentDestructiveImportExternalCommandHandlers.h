#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "CommandEdge/UeAgentCommandDispatch.h"
#include "DomainAdapters/ExternalDataImport/UeAgentExternalDataImportOwner.h"

namespace UE::UAI::Composition::DestructiveImport
{
	UEAGENTINTERFACE_API bool TryCreateDestructiveImportExternalCommandHandlers(
		TSharedRef<DomainAdapters::ExternalDataImport::
			IUeAgentExternalDataImportOwner> Owner,
		TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError);
}
