#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Composition/LongRunningSession/UeAgentLongRunningSessionActivation.h"
#include "Core/Contract/UeAgentContract.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::Composition::LongRunningSession
{
	UEAGENTINTERFACE_API bool TryCreateLongRunningSessionCommandHandlers(
		const TArray<FUeAgentLongRunningSessionActivationProfile>& Profiles,
		TSharedRef<Contract::IUeAgentOperationApplicationPort> OperationPort,
		TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>& OutHandlers,
		FString& OutError);
}
