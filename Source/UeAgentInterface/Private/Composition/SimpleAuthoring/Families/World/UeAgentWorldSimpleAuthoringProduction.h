#pragma once

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringProduction.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectCapabilities.h"
#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectProduction.h"
#include "Templates/SharedPointer.h"

#define UAI_WORLD_SIMPLE_AUTHORING_PRODUCTION_V1 1
#define UAI_WORLD_SIMPLE_AUTHORING_RELOAD_READBACK_V1 1

namespace UE::UAI::Composition::SimpleAuthoring::World
{
	struct UEAGENTINTERFACE_API
		FUeAgentWorldSimpleAuthoringError
	{
		FString Code;
		FString SubjectId;
		FString Detail;

		bool IsValid() const
		{
			return !Code.IsEmpty();
		}
	};

	struct UEAGENTINTERFACE_API
		FUeAgentWorldSimpleAuthoringProviderConfig
	{
		TSharedPtr<const Contract::FUeAgentDescriptorRegistry>
			DescriptorRegistry;
		TSharedPtr<
			DomainAdapters::WorldSimpleEffect::
				IUeAgentWorldSimpleEffectWorldCatalog,
			ESPMode::ThreadSafe> WorldCatalog;
		TSharedPtr<
			DomainAdapters::WorldSimpleEffect::
				IUeAgentWorldSimpleEffectWorldCatalog,
			ESPMode::ThreadSafe> PersistedWorldCatalog;
		TSharedPtr<
			DomainAdapters::WorldSimpleEffect::
				IUeAgentWorldSimpleEffectReferenceResolver,
			ESPMode::ThreadSafe> ReferenceResolver;
		TSharedPtr<
			IUeAgentSimpleAuthoringAssetDurableService,
			ESPMode::ThreadSafe> DurableService;
		TSharedPtr<
			Verification::IAbsenceOracle,
			ESPMode::ThreadSafe> AbsenceOracle;
		TSharedPtr<
			Verification::IReferenceOracle,
			ESPMode::ThreadSafe> ReferenceOracle;
		TOptional<Projection::FUeAgentProjectionRequest> Projection;
		FString OwnerLeaseId;
		int32 MaxQueueItems = 64;
	};

	class UEAGENTINTERFACE_API
		FUeAgentWorldSimpleAuthoringProviderFactory final
	{
	public:
		static bool TryCreate(
			const FUeAgentWorldSimpleAuthoringProviderConfig& Config,
			TSharedPtr<
				IUeAgentSimpleAuthoringCapabilityProvider>&
					OutProvider,
			FUeAgentWorldSimpleAuthoringError& OutError);
	};
}
