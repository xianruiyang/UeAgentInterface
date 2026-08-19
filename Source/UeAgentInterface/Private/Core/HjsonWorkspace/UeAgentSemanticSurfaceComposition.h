#pragma once

#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "Core/StateSemantic/UeAgentSemanticSurface.h"

namespace UE::UAI::HjsonWorkspace
{
	UEAGENTINTERFACE_API bool TryBuildSemanticSurfaceAuthoringModel(
		StateSemantic::FUeAgentSemanticSurfaceRef Surface,
		const FString& BaseRevision,
		ContextResource::FUeAgentResourceTracker& Resources,
		FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError);
}
