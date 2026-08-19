#pragma once

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "OperationOwners/UObjectDataAsset/UeAgentUObjectDataAssetCommandService.h"

namespace UE::UAI::CommandHandlers::UObjectDataAsset
{
	UEAGENTINTERFACE_API bool TryCreateUObjectDataAssetCommandHandlers(
		TSharedRef<OperationOwners::UObjectDataAsset::
			IUeAgentUObjectDataAssetCommandService> Service,
		TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>&
			OutHandlers,
		FString& OutError);
}
