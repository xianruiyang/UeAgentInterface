#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringProduction.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectCapabilities.h"
#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectProduction.h"
#include "Templates/SharedPointer.h"

#define UAI_WORLD_SIMPLE_AUTHORING_PRODUCTION_V1 1
#define UAI_WORLD_SIMPLE_AUTHORING_RELOAD_READBACK_V1 1

namespace UE::UAI::Composition::SimpleAuthoring::World
{
	using FUeAgentWorldSimpleAuthoringError = UE::UAI::Contract::FDiagnostic;

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
