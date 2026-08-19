#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Composition/HjsonImport/UeAgentHjsonImportOperationRuntime.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::Composition::TimelineSpatial
{
	struct UEAGENTINTERFACE_API
		FUeAgentTimelineSpatialHjsonCommandHandlerConfig
	{
		HjsonImport::FUeAgentHjsonImportOperationRuntimeConfig Operation;
		double TimeoutSeconds = 0.0;
		int64 MaxFrameBytes = 0;

		bool IsValid() const;
	};

	UEAGENTINTERFACE_API bool TryCreateTimelineSpatialHjsonCommandHandler(
		const FUeAgentTimelineSpatialHjsonCommandHandlerConfig& Config,
		TSharedRef<const BindingGateway::Ticket::
			FUeAgentOperationTicketService> TicketService,
		TSharedRef<const Projection::FUeAgentResponseProjector> Projector,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		FString& OutError);
}
