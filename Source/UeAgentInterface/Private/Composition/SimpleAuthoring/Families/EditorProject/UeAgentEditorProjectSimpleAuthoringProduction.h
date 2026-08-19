#pragma once

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringProduction.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "DomainAdapters/EditorProjectSimpleEffect/UeAgentEditorProjectSimpleEffectCapabilities.h"
#include "Templates/SharedPointer.h"

#define UAI_EDITOR_PROJECT_SIMPLE_AUTHORING_PRODUCTION_V1 1

namespace UE::UAI::Composition::SimpleAuthoring::EditorProject
{
	struct UEAGENTINTERFACE_API
		FUeAgentEditorProjectSimpleAuthoringError
	{
		FString Code;
		FString SubjectId;
		FString Detail;

		bool IsValid() const
		{
			return !Code.IsEmpty();
		}
	};

	class UEAGENTINTERFACE_API
		FUeAgentEditorProjectSimpleAuthoringDurableBinding final
	{
	public:
		FUeAgentEditorProjectSimpleAuthoringDurableBinding() = default;
		FUeAgentEditorProjectSimpleAuthoringDurableBinding(
			AssetDurable::EDurableArtifactKind InArtifactKind,
			FString InDurableLocation);

		AssetDurable::EDurableArtifactKind GetArtifactKind() const;
		const FString& GetDurableLocation() const;
		bool IsValid() const;

	private:
		AssetDurable::EDurableArtifactKind ArtifactKind =
			AssetDurable::EDurableArtifactKind::Package;
		FString DurableLocation;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API
		IUeAgentEditorProjectSimpleAuthoringDurableBindingResolver
	{
	public:
		virtual ~IUeAgentEditorProjectSimpleAuthoringDurableBindingResolver()
			= default;

		virtual bool TryResolveDurableBinding(
			const DomainAdapters::EditorProjectSimpleEffect::
				FUeAgentEditorProjectSimpleEffectResolvedTarget& Target,
			FUeAgentEditorProjectSimpleAuthoringDurableBinding& OutBinding,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
	};

	struct UEAGENTINTERFACE_API
		FUeAgentEditorProjectSimpleAuthoringProviderConfig
	{
		TSharedPtr<const Contract::FUeAgentDescriptorRegistry>
			DescriptorRegistry;
		TSharedPtr<
			DomainAdapters::EditorProjectSimpleEffect::
				IUeAgentEditorProjectSimpleEffectTargetResolver,
			ESPMode::ThreadSafe> MutationTargetResolver;
		TSharedPtr<
			DomainAdapters::EditorProjectSimpleEffect::
				IUeAgentEditorProjectSimpleEffectTargetResolver,
			ESPMode::ThreadSafe> PreStateTargetResolver;
		TSharedPtr<
			DomainAdapters::EditorProjectSimpleEffect::
				IUeAgentEditorProjectSimpleEffectTargetResolver,
			ESPMode::ThreadSafe> PersistedTargetResolver;
		TSharedPtr<
			IUeAgentEditorProjectSimpleAuthoringDurableBindingResolver,
			ESPMode::ThreadSafe> DurableBindingResolver;
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
		FUeAgentEditorProjectConfigStageSourceFactory final
	{
	public:
		static bool TryCreate(
			TSharedPtr<
				DomainAdapters::EditorProjectSimpleEffect::
					IUeAgentEditorProjectSimpleEffectTargetResolver,
				ESPMode::ThreadSafe> TargetResolver,
			TSharedPtr<
				IUeAgentEditorProjectSimpleAuthoringDurableBindingResolver,
				ESPMode::ThreadSafe> DurableBindingResolver,
			TSharedPtr<
				AssetDurable::IProjectConfigDurableStageSource>&
					OutStageSource,
			FUeAgentEditorProjectSimpleAuthoringError& OutError);
	};

	class UEAGENTINTERFACE_API
		FUeAgentEditorProjectSimpleAuthoringProviderFactory final
	{
	public:
		static bool TryCreate(
			const FUeAgentEditorProjectSimpleAuthoringProviderConfig&
				Config,
			TSharedPtr<
				IUeAgentSimpleAuthoringCapabilityProvider>& OutProvider,
			FUeAgentEditorProjectSimpleAuthoringError& OutError);
	};
}
