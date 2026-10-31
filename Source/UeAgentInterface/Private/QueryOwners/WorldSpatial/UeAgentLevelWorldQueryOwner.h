#pragma once

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"

namespace UE::UAI::QueryOwners::WorldSpatial
{
	UEAGENTINTERFACE_API bool IsReadOnlyContentGraphCommand(
		const FString& CommandId);

	UEAGENTINTERFACE_API bool TryExecuteActorCandidateQuery(
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		HjsonWorkspace::FWorkspaceError& OutError);

	bool TryExecuteComponentCandidateQuery(const FString& CanonicalParamsJson,
		FString& OutPayloadJson, HjsonWorkspace::FWorkspaceError& OutError);
}
