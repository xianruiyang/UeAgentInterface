#pragma once

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringProduction.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Templates/SharedPointer.h"

#define UAI_SIMPLE_AUTHORING_DURABLE_SERVICE_V1 1

namespace UE::UAI::Composition::SimpleAuthoring
{
	struct UEAGENTINTERFACE_API
		FUeAgentSimpleAuthoringDurableServiceConfig
	{
		FString JournalRoot;
		TSharedPtr<
			AssetDurable::IAssetDurableBackend,
			ESPMode::ThreadSafe> PackageBackend;
		TSharedPtr<
			AssetDurable::IAssetDurableBackend,
			ESPMode::ThreadSafe> ProjectConfigBackend;
	};

	class UEAGENTINTERFACE_API
		FUeAgentSimpleAuthoringDurableServiceFactory final
	{
	public:
		static bool TryCreate(
			const FUeAgentSimpleAuthoringDurableServiceConfig& Config,
			TSharedPtr<
				IUeAgentSimpleAuthoringAssetDurableService,
				ESPMode::ThreadSafe>& OutService,
			FUeAgentSimpleAuthoringProductionError& OutError);
	};
}
