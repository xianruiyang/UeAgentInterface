#pragma once

#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "CoreMinimal.h"

namespace UE::UAI::HjsonWorkspace { struct FPreparedWorkspaceExport; }

namespace UE::UAI::DomainAdapters::AudioGraph
{
	UEAGENTINTERFACE_API bool TryExecuteContentGraph(
		const FString& CommandId,
		const FString& TargetIdentity,
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		bool& bOutReadOnly,
		HjsonWorkspace::FWorkspaceError& OutError,
		HjsonWorkspace::FPreparedWorkspaceExport* PreparedExport = nullptr);
}
