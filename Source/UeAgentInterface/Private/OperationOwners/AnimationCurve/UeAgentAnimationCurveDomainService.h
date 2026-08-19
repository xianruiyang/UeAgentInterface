#pragma once

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"

namespace UE::UAI::OperationOwners::AnimationCurve
{
	UEAGENTINTERFACE_API bool TryCreateAnimationCurveDomainProvider(
		TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>&
			OutProvider,
		HjsonWorkspace::FWorkspaceError& OutError);
}
