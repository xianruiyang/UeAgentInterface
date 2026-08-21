#pragma once

#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "CoreMinimal.h"

class FJsonObject;

namespace UE::UAI::DomainAdapters::AnimationRigGraph
{
	/** Validates Rig unit candidates and expands native RigVM unit fields. */
	UEAGENTINTERFACE_API bool TryNormalizeAnimationRigGraphCandidateBindings(
		const FString& TargetIdentity,
		const TSharedPtr<FJsonObject>& NativeParams,
		HjsonWorkspace::FWorkspaceError& OutError);
}
