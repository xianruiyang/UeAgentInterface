#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringComposition.h"
#include "Core/Contract/UeAgentContract.h"

#include "Templates/SharedPointer.h"

namespace UE::UAI::Composition::SimpleAuthoring
{
	UEAGENTINTERFACE_API bool
	TryCreateSimpleAuthoringCommandHandlers(
		const TArray<FUeAgentSimpleAuthoringCommandRoute>& Routes,
		TSharedRef<Contract::IUeAgentOperationApplicationPort>
			OperationPort,
		TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>&
			OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError);
}
