#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Composition/LongRunningSession/UeAgentLongRunningSessionRuntime.h"
#include "Templates/SharedPointer.h"

class UWorld;

namespace UE::UAI::Composition::LongRunningSession
{
	struct UEAGENTINTERFACE_API
	FUeAgentLongRunningSessionProductionConfig final
	{
		FString ProjectInstanceId;
		FString EditorInstanceId;
		FString ServerSessionId;
		FString OwnerLeaseSecret;
		TWeakObjectPtr<UWorld> EditorWorld;
		double TimeoutSeconds = 0.0;
		int64 MaxFrameBytes = 0;
		int32 MaxActiveOperations = 0;
		int32 MaxRetainedOperations = 0;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API
	FUeAgentLongRunningSessionProductionError final
	{
		FString Code;
		FString Detail;
	};

	/**
	 * Composes the immutable production policy catalogs, operation-scoped
	 * native builders, one LongRunningSession runtime and twelve exact public
	 * handlers.  No transport route or second lifecycle registry is created.
	 */
	UEAGENTINTERFACE_API bool TryCreateLongRunningSessionProduction(
		const FUeAgentLongRunningSessionProductionConfig& Config,
		TSharedPtr<FUeAgentLongRunningSessionRuntime>& OutRuntime,
		TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>& OutHandlers,
		TArray<FString>& OutAuthorityScopes,
		FUeAgentLongRunningSessionProductionError& OutError);
}
