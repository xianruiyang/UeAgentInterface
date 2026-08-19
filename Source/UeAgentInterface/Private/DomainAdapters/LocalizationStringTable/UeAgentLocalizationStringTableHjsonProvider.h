#pragma once

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"

namespace UE::UAI::DomainAdapters::LocalizationStringTable
{
	UEAGENTINTERFACE_API bool TryExecuteContentGraph(
		const FString& CommandId,
		const FString& TargetIdentity,
		const FString& RootTypeIdentity,
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		bool& bOutReadOnly,
		UE::UAI::HjsonWorkspace::FWorkspaceError& OutError);

#if WITH_DEV_AUTOMATION_TESTS
	UEAGENTINTERFACE_API void SetStringTableSaveFailureForTests(bool bFail);
#endif
}
