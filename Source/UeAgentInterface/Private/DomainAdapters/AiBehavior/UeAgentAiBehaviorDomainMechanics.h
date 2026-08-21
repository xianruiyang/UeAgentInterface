#pragma once

#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "CoreMinimal.h"

namespace UE::UAI::DomainAdapters::AiBehavior
{
	UEAGENTINTERFACE_API bool TryExecuteContentGraph(
		const FString& CommandId,
		const FString& TargetIdentity,
		const FString& RootTypeIdentity,
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		bool& bOutReadOnly,
		HjsonWorkspace::FWorkspaceError& OutError);
}
