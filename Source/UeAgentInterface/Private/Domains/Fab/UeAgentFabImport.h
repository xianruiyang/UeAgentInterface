#pragma once

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Core/Contract/UeAgentContract.h"

namespace UE::UAI::Fab
{
CommandEdge::FUeAgentCommandResult ListFiles(const CommandEdge::FUeAgentCommandRequest& Request);
CommandEdge::FUeAgentCommandResult ImportAsset(const CommandEdge::FUeAgentCommandRequest& Request,
    const Contract::FUeAgentCallerContext& Caller, TFunctionRef<bool()> IsCancelled);
}
