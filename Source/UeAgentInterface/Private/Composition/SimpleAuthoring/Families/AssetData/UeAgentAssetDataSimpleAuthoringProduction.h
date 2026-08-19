#pragma once

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringProduction.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"
#include "Templates/SharedPointer.h"

#define UAI_ASSET_DATA_SIMPLE_AUTHORING_PRODUCTION_V1 1

namespace UE::UAI::Composition::SimpleAuthoring::AssetData
{
	struct UEAGENTINTERFACE_API
		FUeAgentAssetDataSimpleAuthoringError
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
		FUeAgentAssetDataSimpleAuthoringProviderConfig
	{
		TSharedPtr<const Contract::FUeAgentDescriptorRegistry>
			DescriptorRegistry;
		TSharedPtr<
			DomainAdapters::AssetDataSimpleEffect::
				IUeAgentAssetDataSimpleEffectAssetResolver,
			ESPMode::ThreadSafe> MutationAssetResolver;
		TSharedPtr<
			DomainAdapters::AssetDataSimpleEffect::
				IUeAgentAssetDataSimpleEffectAssetResolver,
			ESPMode::ThreadSafe> PreStateAssetResolver;
		TSharedPtr<
			DomainAdapters::AssetDataSimpleEffect::
				IUeAgentAssetDataSimpleEffectAssetResolver,
			ESPMode::ThreadSafe> PersistedAssetResolver;
		TSharedPtr<
			DomainAdapters::AssetDataSimpleEffect::
				IUeAgentAssetDataSimpleEffectFactoryCandidateResolver,
			ESPMode::ThreadSafe> FactoryResolver;
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
		FUeAgentAssetDataSimpleAuthoringProviderFactory final
	{
	public:
		static bool TryCreate(
			const FUeAgentAssetDataSimpleAuthoringProviderConfig&
				Config,
			TSharedPtr<
				IUeAgentSimpleAuthoringCapabilityProvider>&
					OutProvider,
			FUeAgentAssetDataSimpleAuthoringError& OutError);
	};
}
