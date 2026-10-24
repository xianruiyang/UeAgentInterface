#pragma once

#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "CoreMinimal.h"

namespace UE::UAI::DomainAdapters::WorldSpatial
{
	struct UEAGENTINTERFACE_API FUeAgentWorldActorCandidateQuery final
	{
		FString Target;
		FString Category;
		FString Query;
	};

	UEAGENTINTERFACE_API bool TryQueryWorldActorCandidates(
		const FUeAgentWorldActorCandidateQuery& Query,
		FString& OutPayloadJson,
		HjsonWorkspace::FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryExecuteContentGraph(
		const FString& CommandId,
		const FString& TargetIdentity,
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		bool& bOutReadOnly,
		HjsonWorkspace::FWorkspaceError& OutError,
		const FString& RequestId = FString());
}
