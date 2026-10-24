#pragma once

#include "CoreMinimal.h"

namespace UE::UAI::HjsonWorkspace
{
	class FAuthoringModel;
	class FWorkspaceError;

	struct FSemanticReferenceProjection
	{
		TMap<FString, FString> ReadableAliases;
		TMap<FString, FString> Handles;
		TMap<FString, FString> PublicRefs;
	};

	// Shared by workspace layout and read-only consumers of model-visible references.
	UEAGENTINTERFACE_API bool TryBuildSemanticReferenceProjection(
		const FAuthoringModel& Model, FSemanticReferenceProjection& OutProjection,
		FWorkspaceError& OutError);
}
