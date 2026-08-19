#pragma once

#include "Composition/LongRunningSession/UeAgentLongRunningSessionActivation.h"
#include "Composition/LongRunningSession/UeAgentLongRunningSessionRuntime.h"
#include "Core/Contract/UeAgentContract.h"

namespace UE::UAI::OperationOwners::LongRunning
{
	struct UEAGENTINTERFACE_API FUeAgentLongRunningLifecycleRoute final
	{
		FString CommandId;
		FString PythonFunction;
		Contract::FUeAgentDescriptorIdentity DescriptorIdentity;

		bool IsValid() const;
	};

	UEAGENTINTERFACE_API const TArray<FUeAgentLongRunningLifecycleRoute>&
	GetLongRunningLifecycleRoutes();

	/**
	 * Returns the process-owned lifecycle port. The host survives transport and
	 * CommandService reconstruction, so an operation reference is never
	 * silently rebound to a new generation or lease.
	 */
	UEAGENTINTERFACE_API bool TryGetProductionLongRunningLifecyclePort(
		TSharedPtr<Contract::IUeAgentOperationApplicationPort>& OutPort,
		TSharedPtr<Composition::LongRunningSession::
			FUeAgentLongRunningSessionRuntime>& OutRuntime,
		FString& OutError);
}
