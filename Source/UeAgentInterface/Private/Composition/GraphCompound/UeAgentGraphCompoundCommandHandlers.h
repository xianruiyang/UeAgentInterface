#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundActivation.h"
#include "Core/Contract/UeAgentContract.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::Composition::GraphCompound
{
	UEAGENTINTERFACE_API bool TryCreateGraphCompoundCommandHandlers(
		const TArray<FUeAgentGraphCompoundActivationProfile>& Profiles,
		TSharedRef<Contract::IUeAgentQueryApplicationPort> QueryPort,
		TSharedRef<Contract::IUeAgentOperationApplicationPort> OperationPort,
		TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>& OutHandlers,
		FString& OutError);
}
