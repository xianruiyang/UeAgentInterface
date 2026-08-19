#pragma once

#include "Composition/GraphCompound/UeAgentGraphCompoundActivation.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialActivation.h"

namespace UE::UAI::QueryOwners::AnimationRigCommands
{
	UEAGENTINTERFACE_API const TArray<FString>& GetCommandIds();
	UEAGENTINTERFACE_API bool IsCommand(const FString& CommandId);
	UEAGENTINTERFACE_API bool IsQuery(const FString& CommandId);
	UEAGENTINTERFACE_API bool TryGetGraphProfiles(
		TArray<Composition::GraphCompound::FUeAgentGraphCompoundActivationProfile>& OutProfiles,
		FString& OutError);
	UEAGENTINTERFACE_API bool TryGetTimelineProfiles(
		TArray<Composition::TimelineSpatial::FUeAgentTimelineSpatialActivationProfile>& OutProfiles,
		FString& OutError);
	UEAGENTINTERFACE_API const TArray<FString>& GetReadbackFlows();
}
