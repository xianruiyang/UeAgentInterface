#pragma once

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringProduction.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectCapabilities.h"
#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectProduction.h"
#include "Templates/SharedPointer.h"

#define UAI_MODELING_SIMPLE_AUTHORING_PRODUCTION_V1 1
#define UAI_MODELING_SIMPLE_AUTHORING_ARTIFACT_READBACK_V1 1

namespace UE::UAI::Composition::SimpleAuthoring::Modeling
{
	struct UEAGENTINTERFACE_API
		FUeAgentModelingSimpleAuthoringError
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
		FUeAgentModelingSimpleAuthoringProviderConfig
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
			DomainAdapters::ModelingGeometryEffect::
				IUeAgentModelingGeometryEffectReferenceResolver,
			ESPMode::ThreadSafe> ReferenceResolver;
		// Optional explicit readback owner for embedded/custom compositions.
		// The production runtime leaves this unset so StaticMesh C9 reads the
		// receipt-owned package through an isolated transient package instance.
		TSharedPtr<
			DomainAdapters::ModelingGeometryEffect::
				IUeAgentModelingGeometryEffectReferenceResolver,
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
		FUeAgentModelingSimpleAuthoringProviderFactory final
	{
	public:
		static bool TryCreate(
			const FUeAgentModelingSimpleAuthoringProviderConfig&
				Config,
			TSharedPtr<
				IUeAgentSimpleAuthoringCapabilityProvider>&
					OutProvider,
			FUeAgentModelingSimpleAuthoringError& OutError);
	};
}
