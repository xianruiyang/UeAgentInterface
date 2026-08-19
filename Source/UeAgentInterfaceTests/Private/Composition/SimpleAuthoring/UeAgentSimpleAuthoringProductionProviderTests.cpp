#include "Misc/AutomationTest.h"

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringProduction.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"


namespace
{
	static_assert(UAI_SIMPLE_AUTHORING_PRODUCTION_PROVIDER_V1 == 1);
	static_assert(UAI_SIMPLE_AUTHORING_CHANGE_TARGETS_V1 == 1);
	static_assert(UAI_SIMPLE_AUTHORING_VERIFICATION_TARGETS_V1 == 1);

	using namespace UE::UAI::AssetDurable;
	using namespace UE::UAI::BindingGateway::Ticket;
	using namespace UE::UAI::Composition::SimpleAuthoring;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::DomainAdapters::GenericEffect;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::Projection;
	using namespace UE::UAI::StateSemantic;
	using namespace UE::UAI::Verification;

	class FTrace final
	{
	public:
		void Add(const FString& Event)
		{
			Events.Add(Event);
		}

		int32 Count(const FString& Event) const
		{
			int32 Result = 0;
			for (const FString& Candidate : Events)
			{
				Result += Candidate == Event ? 1 : 0;
			}
			return Result;
		}

		int32 Index(const FString& Event) const
		{
			return Events.IndexOfByKey(Event);
		}

	private:
		TArray<FString> Events;
	};

	FUeAgentResourceBudget TestBudget()
	{
		return FUeAgentResourceBudget({
			{EUeAgentResourceDimension::InputBytes, 1048576},
			{EUeAgentResourceDimension::OutputBytes, 1048576},
			{EUeAgentResourceDimension::StringBytes, 1048576},
			{EUeAgentResourceDimension::Nodes, 4096},
			{EUeAgentResourceDimension::Items, 4096},
			{EUeAgentResourceDimension::Files, 64},
			{EUeAgentResourceDimension::ReferenceEdges, 4096},
			{EUeAgentResourceDimension::WorkUnits, 65536},
			{
				EUeAgentResourceDimension::EstimatedMemoryBytes,
				16777216,
			},
		});
	}

	FUeAgentDescriptorIdentity TestDescriptorIdentity()
	{
		return FUeAgentDescriptorIdentity(
			TEXT("uai.reference.asset_apply"),
			1,
			TEXT(
				"sha256:d5d77b007fce21ae3743d655c3bc324e"
				"f69d3f08f899faa0be224b7bea6695fe"));
	}

	bool TryCreateTestRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		FString& OutError)
	{
		return FUeAgentDescriptorRegistry::TryCreate(
			{
				FUeAgentOperationDescriptor(
					TestDescriptorIdentity(),
					TEXT("reference.asset_apply"),
					TEXT("reference"),
					EUeAgentDescriptorOperationKind::Operation,
					FUeAgentExecutionContextPolicy(
						{
							EUeAgentStatePlane::AuthoringMemory,
							EUeAgentStatePlane::Persisted,
						},
						{},
						EUeAgentContextResolutionPolicy::
							ExplicitOnly,
						EUeAgentTransientContextPolicy::
							ReadOnly),
					FUeAgentContractValue(
						TEXT(
							"{\"required\":[\"object_path\"],"
							"\"type\":\"object\"}")),
					FUeAgentContractValue(
						TEXT("{\"type\":\"object\"}"))),
			},
			OutRegistry,
			OutError);
	}

	FUeAgentSimpleAuthoringCommandRoute TestRoute()
	{
		return FUeAgentSimpleAuthoringCommandRoute(
			TEXT("reference.asset_apply"),
			TEXT("reference.asset_apply"),
			TEXT("reference"),
			TEXT("asset_apply"),
			TEXT("reference"),
			TestDescriptorIdentity());
	}

	FUeAgentExecutionContextInput TestExecutionContext()
	{
		FUeAgentExecutionContextInput Result;
		Result.ProjectInstanceId =
			TEXT("project:sa-008-production");
		Result.EditorInstanceId =
			TEXT("editor:sa-008-production");
		Result.StatePlane = EUeAgentStatePlane::Persisted;
		Result.ContextRevision =
			TEXT("context:sa-008-production:1");
		Result.bResolvedFromCurrent = false;
		return Result;
	}

	FUeAgentCanonicalIdentity TestTargetIdentity()
	{
		return FUeAgentCanonicalIdentity(
			TEXT("uai.test.simple-authoring.production"),
			TEXT("production_fixture"),
			TEXT("/Game/UAI/SA_008/ProductionFixture"),
			TEXT("value"),
			TEXT("fixture_property"),
			TEXT("sa-008-production-generation"));
	}

	FUeAgentCanonicalIdentity TestResolvedTargetIdentity()
	{
		return FUeAgentCanonicalIdentity(
			TEXT("uai.test.simple-authoring.resolved-target"),
			TEXT("production_fixture_scope"),
			TEXT("/Game/UAI/SA_008/ProductionFixture"),
			TEXT("operation_scope"),
			TEXT("resolved_target"),
			TEXT("sa-008-production-generation"));
	}

	FUeAgentCanonicalValueRef MakeString(
		const FString& Value,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Result;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryString(
			Value,
			Resources,
			Result,
			Error));
		return Result.ToSharedRef();
	}

	bool TryMakeSnapshot(
		const FString& SnapshotId,
		const FString& Value,
		const FString& SourceKind,
		const FString& SourceId,
		const FString& EvidenceId,
		bool bExpected,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalSnapshotPtr& OutSnapshot)
	{
		const FUeAgentStateRevision Revision(
			TEXT("sa-008-production-source"),
			TEXT("sa-008-production-generation"),
			1);
		TArray<FUeAgentStateEntry> Entries;
		Entries.Emplace(
			TestTargetIdentity(),
			FUeAgentCanonicalType(
				TEXT("core.string"),
				EUeAgentValueKind::String,
				false,
				TEXT("scalar")),
			MakeString(Value, Resources),
			FUeAgentDefaultValue(
				EUeAgentDefaultSourceKind::Schema,
				TEXT("sa-008-production-default"),
				TEXT("production fixture"),
				MakeString(TEXT(""), Resources)),
			FUeAgentStateDetails(
				TEXT("value"),
				TEXT(""),
				{TEXT("SA-008")},
				true,
				TEXT("available"),
				{}),
			Revision);
		FUeAgentStateSemanticError Error;
		if (bExpected)
		{
			return FUeAgentCanonicalSnapshot::TryCreateExpected(
				SnapshotId,
				EUeAgentStatePlane::Persisted,
				Revision,
				FUeAgentTemporalPoint(
					TEXT("sa-008-production-clock"),
					TEXT("2")),
				FUeAgentStateProvenance(
					SourceKind,
					SourceId,
					EvidenceId),
				Entries,
				Resources,
				OutSnapshot,
				Error);
		}
		return FUeAgentCanonicalSnapshot::TryCreate(
			SnapshotId,
			EUeAgentStatePlane::Persisted,
			Revision,
			FUeAgentTemporalPoint(
				TEXT("sa-008-production-clock"),
				TEXT("1")),
			FUeAgentStateProvenance(
				SourceKind,
				SourceId,
				EvidenceId),
			Entries,
			Resources,
			OutSnapshot,
			Error);
	}

	FUeAgentCandidateBinding MakeCandidateBinding()
	{
		return FUeAgentCandidateBinding(
			TEXT("binding:sa-008-production"),
			FUeAgentCandidateSummary(
				TestTargetIdentity(),
				TestDescriptorIdentity(),
				TEXT("simple_authoring_fixture"),
				TEXT("SA-008 production fixture"),
				TEXT("production lifecycle target"),
				{TEXT("SA-008")}),
			FUeAgentCatalogRevision(
				TEXT("catalog:sa-008-production"),
				TEXT("sa-008-production-generation"),
				1),
			TEXT("/Game/UAI/SA_008/ProductionFixture.value"),
			TEXT("sa-008-production-persisted-read"),
			{
				EUeAgentCandidateOperation::Read,
				EUeAgentCandidateOperation::Update,
			},
			TEXT("evidence:sa-008-production-binding"));
	}

	FUeAgentCandidateBinding MakeResolvedTargetBinding()
	{
		return FUeAgentCandidateBinding(
			TEXT("binding:sa-008-production:resolved"),
			FUeAgentCandidateSummary(
				TestResolvedTargetIdentity(),
				TestDescriptorIdentity(),
				TEXT("simple_authoring_fixture"),
				TEXT("SA-008 resolved operation scope"),
				TEXT(
					"family target resolved before draft expansion"),
				{TEXT("SA-008")}),
			FUeAgentCatalogRevision(
				TEXT("catalog:sa-008-production"),
				TEXT("sa-008-production-generation"),
				1),
			TEXT("/Game/UAI/SA_008/ProductionFixture"),
			TEXT("sa-008-production-persisted-read"),
			{
				EUeAgentCandidateOperation::Read,
				EUeAgentCandidateOperation::Update,
			},
			TEXT(
				"evidence:sa-008-production-resolved-binding"));
	}

	class FRecordingTargetResolver final
		: public IUeAgentSimpleAuthoringTargetResolver
	{
	public:
		explicit FRecordingTargetResolver(TSharedRef<FTrace> InTrace)
			: Trace(MoveTemp(InTrace))
		{
		}

		bool TryResolveTarget(
			const FUeAgentOperationRequest& CanonicalRequest,
			const FUeAgentExecutionContextRef& Context,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			TOptional<FUeAgentCandidateBinding>& OutTarget,
			FUeAgentSimpleAuthoringProductionError& OutError) override
		{
			Trace->Add(TEXT("resolve"));
			SeenProjectId = Context.GetProjectInstanceId();
			OutTarget.Emplace(MakeResolvedTargetBinding());
			return CanonicalRequest.GetDescriptorIdentity()
					== TestDescriptorIdentity()
				&& OutTarget->IsValid();
		}

		FString SeenProjectId;

	private:
		TSharedRef<FTrace> Trace;
	};

	class FRecordingStateReadPort final
		: public IUeAgentStateReadPort
	{
	public:
		FRecordingStateReadPort(
			FString InPortId,
			bool bInPostMutation,
			bool bInAvailable,
			TSharedRef<FTrace> InTrace)
			: PortId(MoveTemp(InPortId))
			, bPostMutation(bInPostMutation)
			, bAvailable(bInAvailable)
			, Trace(MoveTemp(InTrace))
		{
		}

		const FString& GetPortId() const override
		{
			return PortId;
		}

		bool TryRead(
			const FUeAgentStateReadRequest& Request,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
			FUeAgentDiscoveryError& OutError) override
		{
			Trace->Add(
				bPostMutation
					? TEXT("persisted-read")
					: TEXT("pre-read"));
			++CallCount;
			if (!bAvailable)
			{
				OutSnapshot.Reset();
				return false;
			}
			FUeAgentCanonicalSnapshotPtr Snapshot;
			const bool bCreated = TryMakeSnapshot(
				bPostMutation
					? TEXT("snapshot:sa-008-production:actual")
					: TEXT("snapshot:sa-008-production:before"),
				bPostMutation ? TEXT("after") : TEXT("before"),
				TEXT("independent_readback"),
				PortId,
				bPostMutation
					? TEXT("evidence:sa-008-production:actual")
					: TEXT("evidence:sa-008-production:before"),
				false,
				Resources,
				Snapshot);
			OutSnapshot = Snapshot;
			return bCreated
				&& Request.GetBinding().GetBindingId()
					== (bPostMutation
						? MakeCandidateBinding().GetBindingId()
						: MakeResolvedTargetBinding().GetBindingId());
		}

		int32 CallCount = 0;

	private:
		FString PortId;
		bool bPostMutation = false;
		bool bAvailable = true;
		TSharedRef<FTrace> Trace;
	};

	class FRecordingDraftBuilder final
		: public IUeAgentSimpleAuthoringDraftBuilder
	{
	public:
		explicit FRecordingDraftBuilder(TSharedRef<FTrace> InTrace)
			: Trace(MoveTemp(InTrace))
		{
		}

		bool TryBuildDraft(
			const FUeAgentOperationRequest& CanonicalRequest,
			const FUeAgentExecutionContextRef& Context,
			const FUeAgentCanonicalSnapshotRef& PreState,
			FUeAgentResourceTracker& Resources,
			TOptional<FChangeSetDraft>& OutDraft,
			FUeAgentSimpleAuthoringProductionError& OutError) override
		{
			Trace->Add(TEXT("draft"));
			SeenProjectId = Context.GetProjectInstanceId();
			const FChangeDraft Change(
				TestTargetIdentity(),
				EChangeIntent::Update,
				PreState->GetEntries()[0].GetValue(),
				MakeString(TEXT("after"), Resources),
				{TEXT("uai:test:write")},
				TEXT("impact:sa-008-production"),
				{TestTargetIdentity()});
			OutDraft.Emplace(
				CanonicalRequest.GetDescriptorIdentity(),
				CanonicalRequest.GetRequestId(),
				CanonicalRequest.GetIdempotencyKey(),
				PreState,
				TArray<FChangeDraft>{Change});
			return OutDraft->IsValid();
		}

		FString SeenProjectId;

	private:
		TSharedRef<FTrace> Trace;
	};

	class FRecordingChangeTargetProjector final
		: public IUeAgentSimpleAuthoringChangeTargetProjector
	{
	public:
		explicit FRecordingChangeTargetProjector(
			TSharedRef<FTrace> InTrace)
			: Trace(MoveTemp(InTrace))
		{
		}

		bool TryProjectChangeTargets(
			const FUeAgentOperationRequest& CanonicalRequest,
			const FUeAgentExecutionContextRef& Context,
			const FChangeSetDraft& Draft,
			const FUeAgentCandidateBinding& ResolvedTarget,
			const FUeAgentCanonicalSnapshotRef& PreState,
			FUeAgentResourceTracker& Resources,
			TArray<FUeAgentCandidateBinding>& OutTargets,
			FUeAgentSimpleAuthoringProductionError& OutError)
			override
		{
			Trace->Add(TEXT("change-target-project"));
			OutTargets = {MakeCandidateBinding()};
			return Draft.GetChanges().Num() == 1
				&& OutTargets[0].IsValid();
		}

	private:
		TSharedRef<FTrace> Trace;
	};

	class FRecordingMutationProjector final
		: public IUeAgentSimpleAuthoringMutationBindingProjector
	{
	public:
		explicit FRecordingMutationProjector(
			TSharedRef<FTrace> InTrace)
			: Trace(MoveTemp(InTrace))
		{
		}

		bool TryProjectMutationBindings(
			const FPlanningPreflightPlanRef& Plan,
			const TArray<FUeAgentCandidateBinding>& Targets,
			const FUeAgentCanonicalSnapshotRef& PreState,
			const FUeAgentExecutionContextRef& Context,
			FUeAgentResourceTracker& Resources,
			TArray<FUeAgentGenericEffectBinding>& OutBindings,
			FUeAgentSimpleAuthoringProductionError& OutError) override
		{
			Trace->Add(TEXT("mutation-project"));
			SeenProjectId = Context.GetProjectInstanceId();
			if (Targets.Num() != 1)
			{
				return false;
			}
			OutBindings.Emplace(
				Targets[0],
				GetUeStringPropertySourceTypeId(),
				FUeAgentCanonicalType(
					TEXT("core.string"),
					EUeAgentValueKind::String,
					false,
					TEXT("scalar")),
				TEXT("sa-008-production-type-binding"));
			return Plan->GetChangeSet().GetChanges().Num() == 1
				&& OutBindings[0].IsValid();
		}

		FString SeenProjectId;

	private:
		TSharedRef<FTrace> Trace;
	};

	class FRecordingDurableTargetProjector final
		: public IUeAgentSimpleAuthoringDurableTargetProjector
	{
	public:
		explicit FRecordingDurableTargetProjector(
			TSharedRef<FTrace> InTrace)
			: Trace(MoveTemp(InTrace))
		{
		}

		bool TryProjectDurableTargets(
			const FPlanningPreflightPlanRef& Plan,
			const FUeAgentCandidateBinding& Target,
			const FUeAgentCanonicalSnapshotRef& PreState,
			FUeAgentResourceTracker& Resources,
			TArray<FAssetDurableTarget>& OutTargets,
			FUeAgentSimpleAuthoringProductionError& OutError) override
		{
			Trace->Add(TEXT("durable-project"));
			OutTargets.Emplace(
				TestTargetIdentity(),
				EDurableArtifactKind::ProjectConfig,
				TEXT(
					"D:/program/UE/GptProjectTest/runtimeLogs/"
					"SA008ProductionProvider/fixture.ini"),
				EAssetDurableIntent::Save,
				ERollbackGrade::DurableBackup);
			return Plan->GetChangeSet().GetChanges().Num() == 1
				&& OutTargets[0].IsValid();
		}

	private:
		TSharedRef<FTrace> Trace;
	};

	class FRecordingDurableService final
		: public IUeAgentSimpleAuthoringAssetDurableService
	{
	public:
		explicit FRecordingDurableService(TSharedRef<FTrace> InTrace)
			: Trace(MoveTemp(InTrace))
		{
		}

		bool TryPrepare(
			const FAssetDurableRequest& Request,
			FUeAgentResourceTracker& Resources,
			FAssetDurableSessionPtr& OutSession,
			FAssetDurableError& OutError) override
		{
			Trace->Add(TEXT("durable-prepare"));
			++PrepareCount;
			if (!Request.IsValid()
				|| Request.GetTargets().Num() != 1)
			{
				OutError = FAssetDurableError(
					TEXT("sa008_durable_request_invalid"));
				return false;
			}
			SeenArtifactKind =
				Request.GetTargets()[0].GetArtifactKind();
			TArray<FDurableArtifact> Artifacts;
			Artifacts.Emplace(
				TEXT("artifact:sa-008-production"),
				Request.GetTargets()[0].GetIdentity(),
				Request.GetTargets()[0].GetArtifactKind(),
				Request.GetTargets()[0].GetDurableLocation(),
				Request.GetTargets()[0].GetDurableLocation(),
				Request.GetTargets()[0].GetIntent(),
				TEXT("project_config_document"));
			FString InventoryHash;
			if (!TryComputeDurableInventoryHash(
				Artifacts,
				Resources,
				InventoryHash,
				OutError))
			{
				return false;
			}
			const FDurableInventory Inventory(
				MoveTemp(Artifacts),
				MoveTemp(InventoryHash));
			OutSession = MakeShared<const FAssetDurableSession>(
				Request,
				Inventory,
				TEXT("journal:sa-008-production"),
				TEXT(
					"D:/program/UE/GptProjectTest/runtimeLogs/"
					"SA008ProductionProvider/journal.hjson"),
				TEXT(
					"D:/program/UE/GptProjectTest/runtimeLogs/"
					"SA008ProductionProvider"),
				TEXT(
					"D:/program/UE/GptProjectTest/runtimeLogs/"
					"SA008ProductionProvider/transaction"),
				TEXT(
					"D:/program/UE/GptProjectTest/runtimeLogs/"
					"SA008ProductionProvider/staging"),
				TArray<FString>{TEXT("prepared")});
			return OutSession.IsValid()
				&& OutSession->GetInventory().IsValid();
		}

		bool TryPersist(
			const FAssetDurableSessionRef& Session,
			int64 CurrentFencingToken,
			FUeAgentResourceTracker& Resources,
			FDurableReceipt& OutReceipt,
			FAssetDurableError& OutError) override
		{
			Trace->Add(TEXT("durable-persist"));
			++PersistCount;
			OutReceipt = FDurableReceipt(
				Session->GetJournalId(),
				EDurableStatus::PersistedUnverified,
				Session->GetRequest()
					.GetPlan()
					->GetFingerprint(),
				Session->GetInventory().GetInventoryHash(),
				{
					TEXT("prepared"),
					TEXT("persisted_unverified"),
				},
				ERecoveryAction::None,
				true,
				false);
			return CurrentFencingToken
					== Session->GetRequest().GetFencingToken()
				&& OutReceipt.RequiresIndependentVerification();
		}

		bool TryRollback(
			const FAssetDurableSessionRef& Session,
			int64 CurrentFencingToken,
			bool bMemoryRollbackConfirmed,
			FUeAgentResourceTracker& Resources,
			FDurableReceipt& OutReceipt,
			FAssetDurableError& OutError) override
		{
			Trace->Add(TEXT("durable-rollback"));
			return false;
		}

		bool TryAcknowledge(
			const FAssetDurableSessionRef& Session,
			const FDurableVerificationAcknowledgement&
				Acknowledgement,
			FUeAgentResourceTracker& Resources,
			FAssetDurableError& OutError) override
		{
			Trace->Add(TEXT("acknowledge"));
			++AcknowledgeCount;
			return Acknowledgement.IsValid()
				&& Acknowledgement.GetJournalId()
					== Session->GetJournalId();
		}

		int32 PrepareCount = 0;
		int32 PersistCount = 0;
		int32 AcknowledgeCount = 0;
		EDurableArtifactKind SeenArtifactKind =
			EDurableArtifactKind::Package;

	private:
		TSharedRef<FTrace> Trace;
	};

	class FRecordingUeHook final
		: public IUeAgentGenericEffectUeHook
	{
	public:
		explicit FRecordingUeHook(TSharedRef<FTrace> InTrace)
			: Trace(MoveTemp(InTrace))
		{
		}

		bool TryApply(
			const FExecutionMutationRequest& Request,
			const FLockFenceLease& Lease,
			const TArray<FUeAgentGenericEffectBinding>& Bindings,
			FUeAgentResourceTracker& Resources,
			FUeAgentGenericEffectApplyResult& OutResult,
			FExecutionMutationError& OutError) override
		{
			Trace->Add(TEXT("effect-apply"));
			++ApplyCount;
			OutResult = FUeAgentGenericEffectApplyResult(
				1,
				{TestTargetIdentity().GetCanonicalKey()},
				{TEXT("sa-008-production-settled")},
				true,
				TEXT("rollback:sa-008-production"),
				true);
			return Lease.IsValid()
				&& Bindings.Num() == 1
				&& OutResult.IsValid();
		}

		bool TryRollback(
			const FExecutionMutationRequest& Request,
			const FUeAgentGenericEffectApplyResult& Applied,
			FUeAgentResourceTracker& Resources,
			bool& bOutConfirmed,
			FExecutionMutationError& OutError) override
		{
			Trace->Add(TEXT("effect-rollback"));
			bOutConfirmed = true;
			return true;
		}

		void ReleaseVerifiedOperation(
			const FString& OperationId) override
		{
			Trace->Add(TEXT("release"));
			++ReleaseCount;
		}

		bool TryWaitForSettle(
			const FBarrierRequest& Request,
			FUeAgentResourceTracker& Resources,
			FBarrierResult& OutResult,
			FExecutionMutationError& OutError) override
		{
			Trace->Add(TEXT("effect-settle"));
			OutResult = FBarrierResult(
				Request.GetSubjectGeneration(),
				Request.GetRequiredSignals(),
				TEXT("revision:sa-008-production:after"),
				true);
			return OutResult.IsValid();
		}

		int32 ApplyCount = 0;
		int32 ReleaseCount = 0;

	private:
		TSharedRef<FTrace> Trace;
	};

	class FRecordingExpectedProjector final
		: public IUeAgentSimpleAuthoringExpectedStateProjector
	{
	public:
		explicit FRecordingExpectedProjector(
			TSharedRef<FTrace> InTrace)
			: Trace(MoveTemp(InTrace))
		{
		}

		bool TryProjectExpectedState(
			const FPlanningPreflightPlanRef& Plan,
			const FUeAgentCandidateBinding& ResolvedTarget,
			const TArray<FUeAgentCandidateBinding>& ChangeTargets,
			const FUeAgentCanonicalSnapshotRef& PreState,
			const FDurableInventory& Inventory,
			FUeAgentResourceTracker& Resources,
			TArray<FVerificationTarget>& OutTargets,
			FUeAgentSimpleAuthoringProductionError& OutError) override
		{
			Trace->Add(TEXT("expected-project"));
			if (ResolvedTarget.GetBindingId()
					!= MakeResolvedTargetBinding().GetBindingId()
				|| ChangeTargets.Num() != 1
				|| ChangeTargets[0].GetBindingId()
					!= MakeCandidateBinding().GetBindingId())
			{
				OutError.Code =
					TEXT("sa008_verification_targets_invalid");
				return false;
			}
			FUeAgentCanonicalSnapshotPtr Expected;
			if (!TryMakeSnapshot(
				TEXT("snapshot:sa-008-production:expected"),
				TEXT("after"),
				TEXT("operation_expected_state"),
				Plan->GetRequestId(),
				TEXT("evidence:sa-008-production:expected"),
				true,
				Resources,
				Expected))
			{
				OutError.Code =
					TEXT("sa008_expected_snapshot_invalid");
				return false;
			}
			TArray<FString> ArtifactIds;
			for (const FDurableArtifact& Artifact :
				Inventory.GetArtifacts())
			{
				ArtifactIds.Add(Artifact.GetArtifactId());
			}
			OutTargets.Emplace(
				TestTargetIdentity(),
				ChangeTargets[0],
				EVerificationOracleKind::Persisted,
				Expected,
				MoveTemp(ArtifactIds),
				TArray<FString>());
			return OutTargets[0].IsValid();
		}

	private:
		TSharedRef<FTrace> Trace;
	};

	class FUnusedAbsenceOracle final
		: public IAbsenceOracle
	{
	public:
		bool TryObserve(
			const FUeAgentCanonicalIdentity& Identity,
			const TArray<FDurableArtifact>& Artifacts,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FAbsenceObservation& OutObservation,
			FVerificationError& OutError) override
		{
			OutObservation = FAbsenceObservation(
				Identity,
				false,
				true,
				TEXT("unused-absence"));
			return true;
		}
	};

	class FUnusedReferenceOracle final
		: public IReferenceOracle
	{
	public:
		bool TryObserve(
			const FUeAgentCanonicalIdentity& Identity,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FReferenceObservation& OutObservation,
			FVerificationError& OutError) override
		{
			OutObservation = FReferenceObservation(
				Identity,
				{},
				true,
				TEXT("unused-reference"));
			return true;
		}
	};

	class FRecordingTokenEstimator final
		: public IUeAgentTokenEstimator
	{
	public:
		bool TryEstimate(
			const FUeAgentProjectedPayload& Payload,
			const TArray<FUeAgentArtifactDescriptor>& Artifacts,
			const TArray<FUeAgentProjectionDiagnostic>& Diagnostics,
			FUeAgentResourceTracker& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FUeAgentTokenEstimate& OutEstimate,
			FUeAgentProjectionError& OutError) override
		{
			OutEstimate = FUeAgentTokenEstimate(
				EUeAgentTokenEstimateKind::Exact,
				24,
				96);
			return true;
		}
	};

	class FNeverCancelled final
		: public IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override
		{
			return false;
		}
	};

	struct FScenario
	{
		TSharedPtr<FTrace> Trace;
		TSharedPtr<FRecordingTargetResolver> Resolver;
		TSharedPtr<FRecordingStateReadPort> PreRead;
		TSharedPtr<FRecordingStateReadPort> PersistedRead;
		TSharedPtr<FRecordingDraftBuilder> DraftBuilder;
		TSharedPtr<FRecordingChangeTargetProjector>
			ChangeTargetProjector;
		TSharedPtr<FRecordingMutationProjector>
			MutationProjector;
		TSharedPtr<FRecordingDurableTargetProjector>
			DurableProjector;
		TSharedPtr<FRecordingDurableService> DurableService;
		TSharedPtr<FRecordingUeHook> UeHook;
		TSharedPtr<FRecordingExpectedProjector>
			ExpectedProjector;
		FUeAgentSimpleAuthoringProductionProviderConfig
			ProviderConfig;
		TSharedPtr<FUeAgentSimpleAuthoringApplication> Application;
	};

	bool TryCreateScenario(
		bool bPersistedReadAvailable,
		FScenario& OutScenario,
		FString& OutError)
	{
		OutScenario.Trace = MakeShared<FTrace>();
		OutScenario.Resolver =
			MakeShared<FRecordingTargetResolver>(
				OutScenario.Trace.ToSharedRef());
		OutScenario.PreRead =
			MakeShared<FRecordingStateReadPort>(
				TEXT("sa-008-production-pre-read"),
				false,
				true,
				OutScenario.Trace.ToSharedRef());
		OutScenario.PersistedRead =
			MakeShared<FRecordingStateReadPort>(
				TEXT("sa-008-production-persisted-read"),
				true,
				bPersistedReadAvailable,
				OutScenario.Trace.ToSharedRef());
		OutScenario.DraftBuilder =
			MakeShared<FRecordingDraftBuilder>(
				OutScenario.Trace.ToSharedRef());
		OutScenario.ChangeTargetProjector =
			MakeShared<FRecordingChangeTargetProjector>(
				OutScenario.Trace.ToSharedRef());
		OutScenario.MutationProjector =
			MakeShared<FRecordingMutationProjector>(
				OutScenario.Trace.ToSharedRef());
		OutScenario.DurableProjector =
			MakeShared<FRecordingDurableTargetProjector>(
				OutScenario.Trace.ToSharedRef());
		OutScenario.DurableService =
			MakeShared<FRecordingDurableService>(
				OutScenario.Trace.ToSharedRef());
		OutScenario.UeHook =
			MakeShared<FRecordingUeHook>(
				OutScenario.Trace.ToSharedRef());
		OutScenario.ExpectedProjector =
			MakeShared<FRecordingExpectedProjector>(
				OutScenario.Trace.ToSharedRef());

		FUeAgentSimpleAuthoringProductionProviderConfig&
			ProviderConfig = OutScenario.ProviderConfig;
		ProviderConfig.FamilyId = TEXT("reference");
		ProviderConfig.OwnerLeaseId =
			TEXT("owner:sa-008-production");
		ProviderConfig.PreStatePlane =
			EUeAgentStatePlane::Persisted;
		ProviderConfig.Projection.Emplace(
			FUeAgentProjectionProfile(
				TEXT("compact"),
				false,
				false,
				false),
			FUeAgentPageRequest(),
			TOptional<int64>(),
			TArray<FUeAgentArtifactDescriptor>(),
			TArray<FUeAgentProjectionDiagnostic>());
		ProviderConfig.TargetResolver = OutScenario.Resolver;
		ProviderConfig.PreStateReadPort = OutScenario.PreRead;
		ProviderConfig.ChangeTargetProjector =
			OutScenario.ChangeTargetProjector;
		ProviderConfig.DraftBuilder = OutScenario.DraftBuilder;
		ProviderConfig.MutationBindingProjector =
			OutScenario.MutationProjector;
		ProviderConfig.DurableTargetProjector =
			OutScenario.DurableProjector;
		ProviderConfig.DurableService =
			OutScenario.DurableService;
		ProviderConfig.UeHook = OutScenario.UeHook;
		ProviderConfig.ExpectedStateProjector =
			OutScenario.ExpectedProjector;
		ProviderConfig.PersistedStateReadPort =
			OutScenario.PersistedRead;
		ProviderConfig.AbsenceOracle =
			MakeShared<FUnusedAbsenceOracle>();
		ProviderConfig.ReferenceOracle =
			MakeShared<FUnusedReferenceOracle>();

		TSharedPtr<IUeAgentSimpleAuthoringCapabilityProvider> Provider;
		FUeAgentSimpleAuthoringProductionError ProviderError;
		if (!FUeAgentSimpleAuthoringProductionProvider::TryCreate(
			ProviderConfig,
			Provider,
			ProviderError)
			|| !Provider.IsValid())
		{
			OutError = ProviderError.Code
				+ TEXT(":")
				+ ProviderError.Detail;
			return false;
		}

		TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
		if (!TryCreateTestRegistry(Registry, OutError))
		{
			return false;
		}
		TSharedPtr<const FUeAgentOperationTicketService> TicketService;
		FUeAgentOperationTicketError TicketError;
		if (!FUeAgentOperationTicketService::TryCreate(
			TEXT("sa-008-production-integrity-key"),
			30.0,
			[]()
			{
				return 100.0;
			},
			[]()
			{
				return FString(
					TEXT("ticket:sa-008-production"));
			},
			TicketService,
			TicketError))
		{
			OutError = TicketError.Code;
			return false;
		}
		TSharedPtr<const FUeAgentResponseProjector> Projector;
		FUeAgentProjectionError ProjectionError;
		if (!FUeAgentResponseProjector::TryCreate(
			MakeShared<FRecordingTokenEstimator>(),
			Projector,
			ProjectionError))
		{
			OutError = ProjectionError.Code;
			return false;
		}

		FUeAgentSimpleAuthoringApplicationConfig Config;
		Config.DescriptorRegistry = Registry;
		Config.ExecutionContext = TestExecutionContext();
		Config.ServerSessionId =
			TEXT("session:sa-008-production");
		Config.RegistryEpoch = 1;
		Config.TimeoutSeconds = 30.0;
		Config.MaxFrameBytes = 1024 * 1024;
		FUeAgentSimpleAuthoringApplicationError ApplicationError;
		if (!FUeAgentSimpleAuthoringApplication::TryCreate(
			Config,
			{TestRoute()},
			{Provider.ToSharedRef()},
			TicketService.ToSharedRef(),
			Projector.ToSharedRef(),
			OutScenario.Application,
			ApplicationError))
		{
			OutError = ApplicationError.Code
				+ TEXT(":")
				+ ApplicationError.Detail;
			return false;
		}
		return OutScenario.Application.IsValid();
	}

	FUeAgentOperationResult SubmitScenario(FScenario& Scenario)
	{
		return Scenario.Application->Submit(
			FUeAgentOperationRequest(
				TestDescriptorIdentity(),
				TEXT("request:sa-008-production"),
				TEXT("idempotency:sa-008-production"),
				FUeAgentContractValue(
					TEXT(
						"{\"object_path\":\"/Game/UAI/SA_008/"
						"ProductionFixture\"}"))),
			FUeAgentCallerContext(
				TEXT("sa-008-production-caller"),
				{TEXT("uai:test:write")},
				TEXT("sa-008-production-correlation")),
			MakeShared<FNeverCancelled>());
	}
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSimpleAuthoringProductionProviderContractTest,
	"UeAgentInterface.Composition.SimpleAuthoring.E3.ProductionProviderContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentSimpleAuthoringProductionProviderContractTest::RunTest(
	const FString& Parameters)
{
	FUeAgentSimpleAuthoringProductionProviderConfig EmptyConfig;
	TSharedPtr<IUeAgentSimpleAuthoringCapabilityProvider> EmptyProvider;
	FUeAgentSimpleAuthoringProductionError EmptyError;
	TestFalse(
		TEXT("production provider rejects an ownerless family"),
		FUeAgentSimpleAuthoringProductionProvider::TryCreate(
			EmptyConfig,
			EmptyProvider,
			EmptyError));
	TestEqual(
		TEXT("missing family has a stable failure semantic"),
		EmptyError.Code,
		FString(TEXT("simple_authoring_production_family_missing")));
	TestFalse(
		TEXT("failed construction never returns a provider"),
		EmptyProvider.IsValid());

	FScenario Success;
	FString ScenarioError;
	TestTrue(
		TEXT("complete production scenario composes"),
		TryCreateScenario(true, Success, ScenarioError));
	if (!Success.Application.IsValid())
	{
		AddError(
			TEXT("sa008_success_scenario_invalid:")
			+ ScenarioError);
		return false;
	}

	FUeAgentSimpleAuthoringProductionProviderConfig
		AliasedReadConfig = Success.ProviderConfig;
	AliasedReadConfig.PreStateReadPort =
		AliasedReadConfig.PersistedStateReadPort;
	TSharedPtr<IUeAgentSimpleAuthoringCapabilityProvider>
		AliasedProvider;
	FUeAgentSimpleAuthoringProductionError AliasedError;
	TestFalse(
		TEXT("C9 read port cannot alias the pre-state port"),
		FUeAgentSimpleAuthoringProductionProvider::TryCreate(
			AliasedReadConfig,
			AliasedProvider,
			AliasedError));
	TestEqual(
		TEXT("aliased read ports have a stable failure semantic"),
		AliasedError.Code,
		FString(
			TEXT(
				"simple_authoring_c9_read_port_not_independent")));

	const FUeAgentOperationResult SuccessResult =
		SubmitScenario(Success);
	TestFalse(
		TEXT("complete lifecycle returns no operation error"),
		SuccessResult.GetValue()
			.GetJsonValue()
			.Contains(TEXT("operation_error")));
	TestTrue(
		TEXT("complete lifecycle reaches the C3O operation"),
		SuccessResult.GetValue()
			.GetJsonValue()
			.Contains(TEXT("simple-authoring:reference")));
	TestEqual(
		TEXT("resolver executes once"),
		Success.Trace->Count(TEXT("resolve")),
		1);
	TestEqual(
		TEXT("pre-state reader executes once"),
		Success.PreRead->CallCount,
		1);
	TestEqual(
		TEXT("one GenericEffect hook executes"),
		Success.UeHook->ApplyCount,
		1);
	TestEqual(
		TEXT("C8D persists once"),
		Success.DurableService->PersistCount,
		1);
	TestEqual(
		TEXT("C9 performs one independent readback"),
		Success.PersistedRead->CallCount,
		1);
	TestEqual(
		TEXT("C9 acknowledges only after verification"),
		Success.DurableService->AcknowledgeCount,
		1);
	TestEqual(
		TEXT("typed ProjectConfig target reaches C8D"),
		Success.DurableService->SeenArtifactKind,
		EDurableArtifactKind::ProjectConfig);
	TestTrue(
		TEXT("resolver precedes pre-state read"),
		Success.Trace->Index(TEXT("resolve"))
			< Success.Trace->Index(TEXT("pre-read")));
	TestTrue(
		TEXT("pre-state read precedes draft"),
		Success.Trace->Index(TEXT("pre-read"))
			< Success.Trace->Index(TEXT("draft")));
	TestTrue(
		TEXT("mutation binding projection precedes C8D prepare"),
		Success.Trace->Index(TEXT("mutation-project"))
			< Success.Trace->Index(TEXT("durable-prepare")));
	TestTrue(
		TEXT("C8D prepare precedes mutation"),
		Success.Trace->Index(TEXT("durable-prepare"))
			< Success.Trace->Index(TEXT("effect-apply")));
	TestTrue(
		TEXT("mutation settles before C8D publish"),
		Success.Trace->Index(TEXT("effect-settle"))
			< Success.Trace->Index(TEXT("durable-persist")));
	TestTrue(
		TEXT("expected state is projected after persistence"),
		Success.Trace->Index(TEXT("durable-persist"))
			< Success.Trace->Index(TEXT("expected-project")));
	TestTrue(
		TEXT("actual state comes from the independent port"),
		Success.Trace->Index(TEXT("expected-project"))
			< Success.Trace->Index(TEXT("persisted-read")));
	TestTrue(
		TEXT("acknowledgement follows independent readback"),
		Success.Trace->Index(TEXT("persisted-read"))
			< Success.Trace->Index(TEXT("acknowledge")));
	TestEqual(
		TEXT("all stages receive the same explicit context"),
		Success.Resolver->SeenProjectId,
		TestExecutionContext().ProjectInstanceId);
	TestEqual(
		TEXT("draft receives the same explicit context"),
		Success.DraftBuilder->SeenProjectId,
		TestExecutionContext().ProjectInstanceId);
	TestEqual(
		TEXT("mutation receives the same explicit context"),
		Success.MutationProjector->SeenProjectId,
		TestExecutionContext().ProjectInstanceId);

	FScenario Unavailable;
	ScenarioError.Reset();
	TestTrue(
		TEXT("unavailable-readback scenario composes"),
		TryCreateScenario(false, Unavailable, ScenarioError));
	if (!Unavailable.Application.IsValid())
	{
		AddError(
			TEXT("sa008_unavailable_scenario_invalid:")
			+ ScenarioError);
		return false;
	}
	const FUeAgentOperationResult UnavailableResult =
		SubmitScenario(Unavailable);
	TestTrue(
		TEXT("unavailable C9 returns state_unknown"),
		UnavailableResult.GetValue()
			.GetJsonValue()
			.Contains(TEXT("state_unknown")));
	TestEqual(
		TEXT("state_unknown never replays mutation"),
		Unavailable.UeHook->ApplyCount,
		1);
	TestEqual(
		TEXT("state_unknown never republishes durability"),
		Unavailable.DurableService->PersistCount,
		1);
	TestEqual(
		TEXT("unavailable C9 does not acknowledge"),
		Unavailable.DurableService->AcknowledgeCount,
		0);
	TestEqual(
		TEXT("state_unknown still releases operation-local hook state"),
		Unavailable.UeHook->ReleaseCount,
		1);
	return true;
}
