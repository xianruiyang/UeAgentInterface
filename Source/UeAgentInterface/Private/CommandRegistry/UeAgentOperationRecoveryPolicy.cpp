#include "CommandRegistry/UeAgentOperationRecoveryPolicy.h"

#include "Core/Contract/Command/UeAgentCommandContractModel.h"

namespace UE::UAI::CommandRegistry
{
	bool UsesDeferredPreviewExecution(const FString& CommandId)
	{
		return CommandId == TEXT("content_graph.import_workspace");
	}

	bool SupportsOperationRequestRecovery(const CommandContract::FUeAgentFrozenCommandContract& Contract)
	{
		if (UsesDeferredPreviewExecution(Contract.GetCommandId())) return true;
		if (Contract.GetCommandId() == TEXT("world.scene_context") || Contract.GetCommandId() == TEXT("world.geometry_overlap")) return true;
		if (!Contract.GetStateLayers().Contains(TEXT("server_operation_registry"))
			|| Contract.GetSideEffect() == CommandContract::EUeAgentCommandSideEffect::None) return false;
		// Continuations already carry custody. Starts and explicit takeover need
		// a request journal before the response can supply their first lease.
		for (const auto& Field : Contract.GetInputFields())
			if (Field.Name == TEXT("operation_ref")) return false;
		return true;
	}
}
