#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "DomainAdapters/AssetDestructiveImport/UeAgentAssetDeleteOwner.h"
#include "DomainAdapters/AssetDestructiveImport/UeAgentAssetOperationsOwner.h"

namespace UE::UAI::Composition::DestructiveImport
{
	UEAGENTINTERFACE_API bool TryCreateDestructiveImportCommandHandlers(
		TSharedRef<DomainAdapters::AssetDestructiveImport::
			IUeAgentAssetDeleteOwner> AssetDeleteOwner,
		TSharedRef<DomainAdapters::AssetDestructiveImport::
			IUeAgentAssetOperationsOwner> AssetOperationsOwner,
		TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>& OutHandlers,
		FString& OutError);
}
