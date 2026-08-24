#pragma once

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"

namespace UE::UAI::CommandHandlers::AssetDurable
{
	class UEAGENTINTERFACE_API IUeAgentAssetDurableMaintenancePort
	{
	public:
		virtual ~IUeAgentAssetDurableMaintenancePort() = default;

		virtual bool TryInspect(
			const FString& JournalId,
			ContextResource::FUeAgentResourceTracker& Resources,
			UE::UAI::AssetDurable::FAssetDurableSessionPtr& OutSession,
			TArray<UE::UAI::AssetDurable::FDurableArtifactState>&
				OutObservedStates,
			UE::UAI::AssetDurable::FAssetDurableError& OutError) = 0;
		virtual bool TryReconcile(
			const FString& JournalId,
			const FString& ExpectedJournalOwnerId,
			int64 ExpectedFencingToken,
			const FString& VerificationId,
			ContextResource::FUeAgentResourceTracker& Resources,
			UE::UAI::AssetDurable::FDurableReceipt& OutReceipt,
			UE::UAI::AssetDurable::FAssetDurableError& OutError) = 0;
	};

	UEAGENTINTERFACE_API TSharedRef<IUeAgentAssetDurableMaintenancePort>
	CreateProductionAssetDurableMaintenancePort();

	UEAGENTINTERFACE_API bool TryCreateAssetDurableMaintenanceHandlers(
		TSharedRef<IUeAgentAssetDurableMaintenancePort> Port,
		TArray<TSharedRef<CommandEdge::IUeAgentExactCommandHandler>>&
			OutHandlers,
		FString& OutError);
}
