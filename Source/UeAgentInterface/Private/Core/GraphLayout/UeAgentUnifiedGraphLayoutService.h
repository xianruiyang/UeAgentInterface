#pragma once

#include "CommandEdge/UeAgentCommandProtocol.h"

namespace UE::UAI::Core::GraphLayout
{
	UEAGENTINTERFACE_API CommandEdge::FUeAgentCommandResult Execute(
		const CommandEdge::FUeAgentCommandRequest& Request);
}
