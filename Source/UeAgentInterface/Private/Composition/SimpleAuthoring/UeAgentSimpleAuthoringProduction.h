#pragma once

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringApplication.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Core/Verification/UeAgentVerification.h"
#include "DomainAdapters/GenericEffect/UeAgentGenericEffectAdapter.h"
#include "Templates/SharedPointer.h"

#define UAI_SIMPLE_AUTHORING_PRODUCTION_PROVIDER_V1 1
#define UAI_SIMPLE_AUTHORING_CHANGE_TARGETS_V1 1
#define UAI_SIMPLE_AUTHORING_VERIFICATION_TARGETS_V1 1

namespace UE::UAI::Composition::SimpleAuthoring
{
	struct UEAGENTINTERFACE_API
		FUeAgentSimpleAuthoringProductionError
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
		IUeAgentSimpleAuthoringTargetResolver
	{
	public:
		virtual ~IUeAgentSimpleAuthoringTargetResolver() = default;

		virtual bool TryResolveTarget(
			const Contract::FUeAgentOperationRequest& CanonicalRequest,
			const ContextResource::FUeAgentExecutionContextRef& Context,
			ContextResource::FUeAgentResourceTracker& Resources,
			const ContextResource::FUeAgentOperationDeadline& Deadline,
			const ContextResource::FUeAgentCancellationToken& Cancellation,
			TOptional<Discovery::FUeAgentCandidateBinding>& OutTarget,
			FUeAgentSimpleAuthoringProductionError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API
		IUeAgentSimpleAuthoringChangeTargetProjector
	{
	public:
		virtual ~IUeAgentSimpleAuthoringChangeTargetProjector() =
			default;

		virtual bool TryProjectChangeTargets(
			const Contract::FUeAgentOperationRequest&
				CanonicalRequest,
			const ContextResource::FUeAgentExecutionContextRef&
				Context,
			const PlanningPreflight::FChangeSetDraft& Draft,
			const Discovery::FUeAgentCandidateBinding&
				ResolvedTarget,
			const StateSemantic::FUeAgentCanonicalSnapshotRef&
				PreState,
			ContextResource::FUeAgentResourceTracker& Resources,
			TArray<Discovery::FUeAgentCandidateBinding>&
				OutTargets,
			FUeAgentSimpleAuthoringProductionError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API
		IUeAgentSimpleAuthoringDraftBuilder
	{
	public:
		virtual ~IUeAgentSimpleAuthoringDraftBuilder() = default;

		virtual bool TryBuildDraft(
			const Contract::FUeAgentOperationRequest& CanonicalRequest,
			const ContextResource::FUeAgentExecutionContextRef& Context,
			const StateSemantic::FUeAgentCanonicalSnapshotRef& PreState,
			ContextResource::FUeAgentResourceTracker& Resources,
			TOptional<PlanningPreflight::FChangeSetDraft>& OutDraft,
			FUeAgentSimpleAuthoringProductionError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API
		IUeAgentSimpleAuthoringMutationBindingProjector
	{
	public:
		virtual ~IUeAgentSimpleAuthoringMutationBindingProjector() =
			default;

		virtual bool TryProjectMutationBindings(
			const PlanningPreflight::FPlanningPreflightPlanRef& Plan,
			const TArray<
				Discovery::FUeAgentCandidateBinding>& Targets,
			const StateSemantic::FUeAgentCanonicalSnapshotRef& PreState,
			const ContextResource::FUeAgentExecutionContextRef& Context,
			ContextResource::FUeAgentResourceTracker& Resources,
			TArray<
				DomainAdapters::GenericEffect::
					FUeAgentGenericEffectBinding>& OutBindings,
			FUeAgentSimpleAuthoringProductionError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API
		IUeAgentSimpleAuthoringDurableTargetProjector
	{
	public:
		virtual ~IUeAgentSimpleAuthoringDurableTargetProjector() =
			default;

		virtual bool TryProjectDurableTargets(
			const PlanningPreflight::FPlanningPreflightPlanRef& Plan,
			const Discovery::FUeAgentCandidateBinding& Target,
			const StateSemantic::FUeAgentCanonicalSnapshotRef& PreState,
			ContextResource::FUeAgentResourceTracker& Resources,
			TArray<AssetDurable::FAssetDurableTarget>& OutTargets,
			FUeAgentSimpleAuthoringProductionError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API
		IUeAgentSimpleAuthoringExpectedStateProjector
	{
	public:
		virtual ~IUeAgentSimpleAuthoringExpectedStateProjector() =
			default;

		virtual bool TryProjectExpectedState(
			const PlanningPreflight::FPlanningPreflightPlanRef& Plan,
			const Discovery::FUeAgentCandidateBinding& ResolvedTarget,
			const TArray<Discovery::FUeAgentCandidateBinding>&
				ChangeTargets,
			const StateSemantic::FUeAgentCanonicalSnapshotRef& PreState,
			const AssetDurable::FDurableInventory& Inventory,
			ContextResource::FUeAgentResourceTracker& Resources,
			TArray<Verification::FVerificationTarget>& OutTargets,
			FUeAgentSimpleAuthoringProductionError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API
		IUeAgentSimpleAuthoringAssetDurableService
	{
	public:
		virtual ~IUeAgentSimpleAuthoringAssetDurableService() = default;

		virtual bool TryPrepare(
			const AssetDurable::FAssetDurableRequest& Request,
			ContextResource::FUeAgentResourceTracker& Resources,
			AssetDurable::FAssetDurableSessionPtr& OutSession,
			AssetDurable::FAssetDurableError& OutError) = 0;
		virtual bool TryPersist(
			const AssetDurable::FAssetDurableSessionRef& Session,
			int64 CurrentFencingToken,
			ContextResource::FUeAgentResourceTracker& Resources,
			AssetDurable::FDurableReceipt& OutReceipt,
			AssetDurable::FAssetDurableError& OutError) = 0;
		virtual bool TryEnlist(
			const AssetDurable::FAssetDurableSessionRef& Session,
			const TArray<AssetDurable::FAssetDurableTarget>&
				MutationTargets,
			int64 CurrentFencingToken,
			ContextResource::FUeAgentResourceTracker& Resources,
			AssetDurable::FAssetDurableSessionPtr& OutSession,
			AssetDurable::FAssetDurableError& OutError) = 0;
		virtual bool TryRollback(
			const AssetDurable::FAssetDurableSessionRef& Session,
			int64 CurrentFencingToken,
			bool bMemoryRollbackConfirmed,
			ContextResource::FUeAgentResourceTracker& Resources,
			AssetDurable::FDurableReceipt& OutReceipt,
			AssetDurable::FAssetDurableError& OutError) = 0;
		virtual bool TryAcknowledge(
			const AssetDurable::FAssetDurableSessionRef& Session,
			const AssetDurable::FDurableVerificationAcknowledgement&
				Acknowledgement,
			ContextResource::FUeAgentResourceTracker& Resources,
			AssetDurable::FAssetDurableError& OutError) = 0;
		virtual bool TryInspect(
			const FString& JournalId,
			ContextResource::FUeAgentResourceTracker& Resources,
			AssetDurable::FAssetDurableSessionPtr& OutSession,
			TArray<AssetDurable::FDurableArtifactState>& OutObservedStates,
			AssetDurable::FAssetDurableError& OutError)
		{
			OutSession.Reset();
			OutObservedStates.Reset();
			OutError = AssetDurable::FAssetDurableError(
				TEXT("simple_authoring_durable_inspect_unavailable"));
			return false;
		}
		virtual bool TryReconcile(
			const FString& JournalId,
			const FString& ExpectedJournalOwnerId,
			int64 ExpectedFencingToken,
			const FString& VerificationId,
			ContextResource::FUeAgentResourceTracker& Resources,
			AssetDurable::FDurableReceipt& OutReceipt,
			AssetDurable::FAssetDurableError& OutError)
		{
			OutReceipt = AssetDurable::FDurableReceipt();
			OutError = AssetDurable::FAssetDurableError(
				TEXT("simple_authoring_durable_reconcile_unavailable"));
			return false;
		}
	};

	struct UEAGENTINTERFACE_API
		FUeAgentSimpleAuthoringProductionProviderConfig
	{
		FString FamilyId;
		FString OwnerLeaseId;
		Discovery::EUeAgentStatePlane PreStatePlane =
			Discovery::EUeAgentStatePlane::AuthoringMemory;
		AssetDurable::EAtomicityMode AtomicityMode =
			AssetDurable::EAtomicityMode::CompensatedSaga;
		int32 MaxQueueItems = 64;
		TOptional<Projection::FUeAgentProjectionRequest> Projection;
		TSharedPtr<
			IUeAgentSimpleAuthoringTargetResolver,
			ESPMode::ThreadSafe> TargetResolver;
		TSharedPtr<
			Discovery::IUeAgentStateReadPort,
			ESPMode::ThreadSafe> PreStateReadPort;
		TSharedPtr<
			IUeAgentSimpleAuthoringChangeTargetProjector,
			ESPMode::ThreadSafe> ChangeTargetProjector;
		TSharedPtr<
			IUeAgentSimpleAuthoringDraftBuilder,
			ESPMode::ThreadSafe> DraftBuilder;
		TSharedPtr<
			IUeAgentSimpleAuthoringMutationBindingProjector,
			ESPMode::ThreadSafe> MutationBindingProjector;
		TSharedPtr<
			IUeAgentSimpleAuthoringDurableTargetProjector,
			ESPMode::ThreadSafe> DurableTargetProjector;
		TSharedPtr<
			IUeAgentSimpleAuthoringAssetDurableService,
			ESPMode::ThreadSafe> DurableService;
		TSharedPtr<
			DomainAdapters::GenericEffect::
				IUeAgentGenericEffectUeHook,
			ESPMode::ThreadSafe> UeHook;
		TSharedPtr<
			IUeAgentSimpleAuthoringExpectedStateProjector,
			ESPMode::ThreadSafe> ExpectedStateProjector;
		TSharedPtr<
			Verification::IPersistedStateOracle,
			ESPMode::ThreadSafe> PersistedStateOracle;
		TSharedPtr<
			Verification::IAbsenceOracle,
			ESPMode::ThreadSafe> AbsenceOracle;
		TSharedPtr<
			Verification::IReferenceOracle,
			ESPMode::ThreadSafe> ReferenceOracle;
	};

	class UEAGENTINTERFACE_API
		FUeAgentSimpleAuthoringProductionProvider final
	{
	public:
		static bool TryCreate(
			const FUeAgentSimpleAuthoringProductionProviderConfig&
				Config,
			TSharedPtr<IUeAgentSimpleAuthoringCapabilityProvider>&
				OutProvider,
			FUeAgentSimpleAuthoringProductionError& OutError);
	};
}
