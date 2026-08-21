#pragma once

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"

class FJsonObject;

namespace UE::UAI::DomainAdapters::Blueprint::Surfaces
{
	/**
	 * Validates Blueprint node/component candidate identities against their
	 * owning query services and expands them into the native authoring fields.
	 * No UObject mutation is performed by this preflight.
	 */
	UEAGENTINTERFACE_API bool TryNormalizeBlueprintCandidateBindings(
		const FString& TargetIdentity,
		const TSharedPtr<FJsonObject>& ProviderExtension,
		HjsonWorkspace::FWorkspaceError& OutError);
}
