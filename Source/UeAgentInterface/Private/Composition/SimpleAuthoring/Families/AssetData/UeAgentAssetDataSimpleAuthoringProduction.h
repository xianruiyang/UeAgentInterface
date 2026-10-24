#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringProduction.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"
#include "Templates/SharedPointer.h"

#define UAI_ASSET_DATA_SIMPLE_AUTHORING_PRODUCTION_V1 1

namespace UE::UAI::Composition::SimpleAuthoring::AssetData
{
	struct UEAGENTINTERFACE_API
		FUeAgentPersistedCreatedAssetObservation final
	{
		FString SourceObjectPath;
		FString ClassPath;
		bool bWorld = false;
		bool bHasPersistentLevel = false;
		int64 SnapshotSequence = -1;

		bool IsValid() const
		{
			return !SourceObjectPath.IsEmpty()
				&& !ClassPath.IsEmpty()
				&& SnapshotSequence >= 0
				&& (!bWorld || bHasPersistentLevel);
		}
	};

	using FUeAgentAssetDataSimpleAuthoringError = UE::UAI::Contract::FDiagnostic;

	bool UEAGENTINTERFACE_API TryObservePersistedCreatedAsset(
		const TArray<AssetDurable::FDurableArtifact>& Artifacts,
		const FString& SourcePackageName,
		const FString& SourceObjectName,
		const FString& SourceObjectPath,
		const FString& ExpectedClassPath,
		FUeAgentPersistedCreatedAssetObservation& OutObservation,
		FUeAgentAssetDataSimpleAuthoringError& OutError);

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
