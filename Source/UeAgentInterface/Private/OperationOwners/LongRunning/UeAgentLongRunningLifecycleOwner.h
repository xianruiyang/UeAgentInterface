#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

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
		bool TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const;
	};

	UEAGENTINTERFACE_API bool TryGetLongRunningLifecycleRoutes(
		TArray<FUeAgentLongRunningLifecycleRoute>& OutRoutes,
		UE::UAI::Contract::FDiagnostic& OutError);

	/**
	 * Returns the process-owned lifecycle port. The host survives transport and
	 * CommandService reconstruction, so an operation reference is never
	 * silently rebound to a new generation or lease.
	 */
	UEAGENTINTERFACE_API bool TryGetProductionLongRunningLifecyclePort(
		TSharedPtr<Contract::IUeAgentOperationApplicationPort>& OutPort,
		TSharedPtr<Composition::LongRunningSession::
			FUeAgentLongRunningSessionRuntime>& OutRuntime,
		UE::UAI::Contract::FDiagnostic& OutError);
	/** Registers the single pre-exit owner, including when no operation has started. */
	UEAGENTINTERFACE_API void InitializeProductionLongRunningLifecycle();
	/** Drain native ownership while the GameThread and engine services still exist. */
	UEAGENTINTERFACE_API void ShutdownProductionLongRunningLifecycle();
}
