#pragma once

#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"

namespace UE::UAI::DomainAdapters::WorldCommands
{
	UEAGENTINTERFACE_API bool TryExecuteHlodContentGraph(
		const FString& CommandId,
		const FString& TargetIdentity,
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		bool& bOutReadOnly,
		HjsonWorkspace::FWorkspaceError& OutError);
}
