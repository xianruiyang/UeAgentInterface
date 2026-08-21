#pragma once

#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "CoreMinimal.h"

class FJsonObject;

namespace UE::UAI::DomainAdapters::AudioGraph
{
	/**
	 * Validates Audio candidate objects against the owning query service and
	 * expands them into the native node mutation fields. This is preflight-only.
	 */
	UEAGENTINTERFACE_API bool TryNormalizeAudioGraphCandidateBindings(
		const FString& TargetIdentity,
		const TSharedPtr<FJsonObject>& NativeParams,
		HjsonWorkspace::FWorkspaceError& OutError);
}
