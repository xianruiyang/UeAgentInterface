#pragma once

#include "Composition/GraphCompound/UeAgentGraphCompoundActivation.h"

namespace UE::UAI::DomainAdapters::AiCommands
{
	UEAGENTINTERFACE_API const TArray<FString>& GetCommandIds();
	UEAGENTINTERFACE_API bool IsQuery(const FString& CommandId);
	UEAGENTINTERFACE_API bool TryGetActivationProfiles(
		TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>& OutProfiles,
		FString& OutError);
}
