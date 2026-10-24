#pragma once

#include "Containers/UnrealString.h"

namespace UE::UAI::CommandContract { class FUeAgentFrozenCommandContract; }

namespace UE::UAI::CommandRegistry
{
	/** Common scheduling policy consumed by ingress and SDK manifest projection. */
	bool UsesDeferredPreviewExecution(const FString& CommandId);
	bool SupportsOperationRequestRecovery(const CommandContract::FUeAgentFrozenCommandContract& Contract);
}
