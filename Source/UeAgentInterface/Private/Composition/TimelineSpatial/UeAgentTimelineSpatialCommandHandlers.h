#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialActivation.h"
#include "Core/Contract/UeAgentContract.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::Composition::TimelineSpatial
{
	UEAGENTINTERFACE_API bool TryCreateTimelineSpatialCommandHandlers(
		const TArray<FUeAgentTimelineSpatialActivationProfile>& Profiles,
		TSharedRef<Contract::IUeAgentQueryApplicationPort> QueryPort,
		TSharedRef<Contract::IUeAgentOperationApplicationPort> OperationPort,
		TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>& OutHandlers,
		FString& OutError);
}
