#pragma once

#include "Composition/GraphCompound/UeAgentGraphCompoundActivation.h"
#include "CoreMinimal.h"

namespace UE::UAI::DomainAdapters::TextureGraph
{
	UEAGENTINTERFACE_API const TArray<FString>& GetCommandIds();
	UEAGENTINTERFACE_API bool TryGetActivationProfiles(
		TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>&
			OutProfiles,
		FString& OutError);
}
