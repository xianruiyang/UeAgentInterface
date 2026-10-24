#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Composition/TimelineSpatial/UeAgentTimelineSpatialActivation.h"

namespace UE::UAI::QueryOwners::TimelineSequenceCommands
{
	UEAGENTINTERFACE_API const TArray<FString>& GetCommandIds();
	UEAGENTINTERFACE_API bool IsCommand(const FString& CommandId);
	UEAGENTINTERFACE_API bool IsQuery(const FString& CommandId);
	UEAGENTINTERFACE_API bool TryGetProfiles(
		TArray<Composition::TimelineSpatial::FUeAgentTimelineSpatialActivationProfile>& OutProfiles,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API const TArray<FString>& GetReadbackFlows();
}
