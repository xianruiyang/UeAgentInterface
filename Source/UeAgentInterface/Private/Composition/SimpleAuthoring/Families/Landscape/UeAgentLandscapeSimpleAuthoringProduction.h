#pragma once

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringProduction.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "DomainAdapters/LandscapeGeometryEffect/UeAgentLandscapeGeometryEffectCapabilities.h"
#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectProduction.h"
#include "Templates/SharedPointer.h"

#define UAI_LANDSCAPE_SIMPLE_AUTHORING_PRODUCTION_V1 1

namespace UE::UAI::Composition::SimpleAuthoring::Landscape
{
	struct UEAGENTINTERFACE_API
		FUeAgentLandscapeSimpleAuthoringError
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
		FUeAgentLandscapeSimpleAuthoringProviderConfig
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
			DomainAdapters::LandscapeGeometryEffect::
				IUeAgentLandscapeGeometryEffectReferenceResolver,
			ESPMode::ThreadSafe> ReferenceResolver;
		TSharedPtr<
			DomainAdapters::LandscapeGeometryEffect::
				IUeAgentLandscapeGeometryEffectReferenceResolver,
			ESPMode::ThreadSafe> PersistedReferenceResolver;
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
		FUeAgentLandscapeSimpleAuthoringProviderFactory final
	{
	public:
		static bool TryCreate(
			const FUeAgentLandscapeSimpleAuthoringProviderConfig&
				Config,
			TSharedPtr<
				IUeAgentSimpleAuthoringCapabilityProvider>&
					OutProvider,
			FUeAgentLandscapeSimpleAuthoringError& OutError);
	};
}
