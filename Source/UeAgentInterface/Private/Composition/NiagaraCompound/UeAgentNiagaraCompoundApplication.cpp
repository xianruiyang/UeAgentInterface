#include "Composition/NiagaraCompound/UeAgentNiagaraCompoundApplication.h"

#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Core/Verification/UeAgentVerification.h"
#include "DomainAdapters/GenericEffect/UeAgentGenericEffectAdapter.h"
#include "DomainAdapters/NiagaraCompound/UeAgentNiagaraCompoundAdapter.h"
#include "DomainAdapters/NiagaraCompound/UeAgentNiagaraCompoundDiscovery.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace UE::UAI::Composition::NiagaraCompound
{
	namespace
	{
		using namespace AssetDurable;
		using namespace BindingGateway::Operation;
		using namespace ContextResource;
		using namespace Contract;
		using namespace Discovery;
		using namespace DomainAdapters::GenericEffect;
		using namespace DomainAdapters::NiagaraCompound;
		using namespace ExecutionMutation;
		using namespace PlanningPreflight;
		using namespace Projection;
		using namespace StateSemantic;
		using namespace Verification;

		void SetError(
			FUeAgentNiagaraCompoundApplicationError& OutError,
			const FString& Code,
			const FString& Detail)
		{
			OutError = {Code, Detail};
		}

		class FNiagaraSessionSource final
			: public IUeAgentOperationSessionSource
		{
		public:
			explicit FNiagaraSessionSource(
				FUeAgentOperationSessionIdentity InSession)
				: Session(MoveTemp(InSession))
			{
			}

			FUeAgentOperationSessionIdentity GetCurrent()
				const override
			{
				return Session;
			}

		private:
			FUeAgentOperationSessionIdentity Session;
		};

		class FNiagaraPlanner final
			: public IUeAgentOperationPlanner
		{
		public:
			bool TryPlan(
				const FPlanningRequest& Request,
				const FUeAgentOperationContext& Context,
				FPlanningResult& OutResult,
				FUeAgentOperationBindingError& OutError) override
			{
				FPlanningPreflightError Error;
				if (!TryPlanChanges(
						Request,
						Context.GetResourceTracker(),
						OutResult,
						Error))
				{
					OutError.Code = Error.GetCode();
					OutError.SubjectId =
						Error.GetIdentityKey();
					OutError.Detail = Error.GetDetail();
					return false;
				}
				return true;
			}
		};

		class FInlineGameThreadScheduler final
			: public IExecutionScheduler
		{
		public:
			bool Schedule(
				EExecutionThreadAffinity Affinity,
				const FString& OperationId,
				TFunction<void()> Callback,
				FExecutionMutationError& OutError) override
			{
				if (Affinity
						!= EExecutionThreadAffinity::GameThread
					|| !IsInGameThread()
					|| !Callback)
				{
					OutError = FExecutionMutationError(
						TEXT(
							"niagara_compound_scheduler_thread_invalid"),
						OperationId);
					return false;
				}
				Callback();
				return true;
			}

			bool IsCurrentThread(
				EExecutionThreadAffinity Affinity)
				const override
			{
				return Affinity
						== EExecutionThreadAffinity::GameThread
					&& IsInGameThread();
			}
		};

		class FNiagaraDurableSession final
			: public IExecutionDurableSession
		{
		public:
			explicit FNiagaraDurableSession(
				FAssetDurableSessionRef InSession)
				: Session(MoveTemp(InSession))
			{
			}

			FAssetDurableSessionRef Session;
		};

		bool TryPackageName(
			const FString& ObjectPath,
			FString& OutPackageName)
		{
			OutPackageName =
				FPackageName::ObjectPathToPackageName(ObjectPath);
			return FPackageName::IsValidLongPackageName(
				OutPackageName);
		}

		class FNiagaraDurablePort final
			: public IDurableExecutionPort
			, public IDurableVerificationAcknowledgerPort
		{
		public:
			explicit FNiagaraDurablePort(FString InJournalRoot)
				: JournalRoot(MoveTemp(InJournalRoot))
			{
			}

			bool Prepare(
				const FExecutionMutationRequest& Request,
				int64 FencingToken,
				FUeAgentResourceTracker& Resources,
				FExecutionDurableSessionPtr& OutSession,
				FExecutionMutationError& OutError) override
			{
				OutSession.Reset();
				TArray<FAssetDurableTarget> Targets;
				for (const FPlannedChange& Change :
					Request.GetPlan()
						->GetChangeSet()
						.GetChanges())
				{
					if (Change.GetIntent()
							!= EChangeIntent::Update
						&& Change.GetIntent()
							!= EChangeIntent::Replace)
					{
						OutError = FExecutionMutationError(
							TEXT(
								"niagara_compound_intent_unsupported"),
							Change.GetIdentity()
								.GetCanonicalKey());
						return false;
					}
					FString PackageName;
					if (!TryPackageName(
							Change.GetIdentity().GetOwnerId(),
							PackageName))
					{
						OutError = FExecutionMutationError(
							TEXT(
								"niagara_compound_package_invalid"),
							Change.GetIdentity().GetOwnerId());
						return false;
					}
					Targets.Emplace(
						Change.GetIdentity(),
						PackageName,
						EAssetDurableIntent::Save,
						ERollbackGrade::DurableBackup);
				}
				const FAssetDurableRequest DurableRequest(
					Request.GetPlan(),
					MoveTemp(Targets),
					FencingToken,
					Request.GetOwnerLeaseId(),
					EAtomicityMode::CompensatedSaga);
				FAssetDurableSessionPtr Session;
				FAssetDurableError Error;
				if (!TryPrepareAssetDurability(
						DurableRequest,
						Backend,
						JournalRoot,
						Resources,
						Session,
						Error)
					|| !Session.IsValid())
				{
					OutError = FExecutionMutationError(
						Error.GetCode(),
						Error.GetDetail());
					return false;
				}
				OutSession = MakeShared<
					FNiagaraDurableSession,
					ESPMode::ThreadSafe>(
					Session.ToSharedRef());
				return true;
			}

			bool Persist(
				const FExecutionDurableSessionRef& Session,
				int64 FencingToken,
				const FMutationFacts& Facts,
				FUeAgentResourceTracker& Resources,
				FDurableReceipt& OutReceipt,
				FExecutionMutationError& OutError) override
			{
				(void)Facts;
				const TSharedRef<FNiagaraDurableSession> Typed =
					StaticCastSharedRef<
						FNiagaraDurableSession>(Session);
				FAssetDurableError Error;
				if (!TryPersistAssetDurability(
						Typed->Session,
						Backend,
						FencingToken,
						Resources,
						OutReceipt,
						Error))
				{
					OutError = FExecutionMutationError(
						Error.GetCode(),
						Error.GetDetail());
					return false;
				}
				const FAssetDurableSessionRef Persisted =
					MakeShared<const FAssetDurableSession>(
						Typed->Session->GetRequest(),
						Typed->Session->GetInventory(),
						Typed->Session->GetJournalId(),
						Typed->Session->GetJournalPath(),
						Typed->Session->GetJournalRoot(),
						Typed->Session->GetTransactionRoot(),
						Typed->Session->GetStagingRoot(),
						OutReceipt.GetPhaseHistory());
				{
					FScopeLock Lock(&Mutex);
					PersistedByJournal.Add(
						OutReceipt.GetJournalId(),
						Persisted);
				}
				for (const FAssetDurableTarget& Target :
					Typed->Session->GetRequest().GetTargets())
				{
					if (UPackage* Package = FindPackage(
							nullptr,
							*Target.GetPackageName()))
					{
						Package->SetDirtyFlag(false);
					}
				}
				return true;
			}

			bool Rollback(
				const FExecutionDurableSessionRef& Session,
				int64 FencingToken,
				bool bMemoryRollbackConfirmed,
				FUeAgentResourceTracker& Resources,
				FDurableReceipt& OutReceipt,
				FExecutionMutationError& OutError) override
			{
				const TSharedRef<FNiagaraDurableSession> Typed =
					StaticCastSharedRef<
						FNiagaraDurableSession>(Session);
				FAssetDurableError Error;
				if (!TryRollbackAssetDurability(
						Typed->Session,
						Backend,
						FencingToken,
						bMemoryRollbackConfirmed,
						Resources,
						OutReceipt,
						Error))
				{
					OutError = FExecutionMutationError(
						Error.GetCode(),
						Error.GetDetail());
					return false;
				}
				return true;
			}

			bool TryAcknowledge(
				const FDurableVerificationAcknowledgement&
					Acknowledgement,
				FUeAgentResourceTracker& Resources,
				FAssetDurableError& OutError) override
			{
				FAssetDurableSessionPtr Session;
				{
					FScopeLock Lock(&Mutex);
					Session = PersistedByJournal.FindRef(
						Acknowledgement.GetJournalId());
				}
				if (!Session.IsValid())
				{
					OutError = FAssetDurableError(
						TEXT(
							"niagara_compound_durable_session_missing"),
						Acknowledgement.GetJournalId());
					return false;
				}
				if (!TryAcknowledgeAssetDurability(
						Session.ToSharedRef(),
						Acknowledgement,
						Acknowledgement.GetFencingToken(),
						Resources,
						OutError))
				{
					return false;
				}
				FScopeLock Lock(&Mutex);
				PersistedByJournal.Remove(
					Acknowledgement.GetJournalId());
				return true;
			}

			bool TryGetContext(
				const FString& JournalId,
				FDurableInventory& OutInventory,
				FString& OutOwnerId,
				int64& OutFencingToken) const
			{
				FScopeLock Lock(&Mutex);
				const FAssetDurableSessionPtr* Found =
					PersistedByJournal.Find(JournalId);
				if (!Found || !Found->IsValid())
				{
					return false;
				}
				OutInventory = (*Found)->GetInventory();
				OutOwnerId =
					(*Found)->GetRequest().GetJournalOwnerId();
				OutFencingToken =
					(*Found)->GetRequest().GetFencingToken();
				return true;
			}

		private:
			FString JournalRoot;
			FPackageAssetDurableBackend Backend;
			mutable FCriticalSection Mutex;
			TMap<FString, FAssetDurableSessionPtr>
				PersistedByJournal;
		};

		struct FNiagaraExecutionRecord
		{
			FPlanningPreflightPlanPtr Plan;
			TArray<FUeAgentOperationTargetBinding> Targets;
		};

		class FNiagaraExecutionStore final
		{
		public:
			void Put(
				const FString& OperationId,
				FNiagaraExecutionRecord Record)
			{
				FScopeLock Lock(&Mutex);
				Records.Add(OperationId, MoveTemp(Record));
			}

			bool Get(
				const FString& OperationId,
				FNiagaraExecutionRecord& OutRecord) const
			{
				FScopeLock Lock(&Mutex);
				const FNiagaraExecutionRecord* Found =
					Records.Find(OperationId);
				if (!Found)
				{
					return false;
				}
				OutRecord = *Found;
				return true;
			}

			void Remove(const FString& OperationId)
			{
				FScopeLock Lock(&Mutex);
				Records.Remove(OperationId);
			}

		private:
			mutable FCriticalSection Mutex;
			TMap<FString, FNiagaraExecutionRecord> Records;
		};

		class FNiagaraExecutor final
			: public IUeAgentOperationExecutor
		{
		public:
			FNiagaraExecutor(
				TSharedRef<
					FNiagaraExecutionStore,
					ESPMode::ThreadSafe> InStore,
				TSharedRef<
					FNiagaraDurablePort,
					ESPMode::ThreadSafe> InDurable,
				TSharedRef<
					FUeAgentNiagaraCompoundEffectHook,
					ESPMode::ThreadSafe> InEffect)
				: Store(MoveTemp(InStore))
				, Durable(MoveTemp(InDurable))
				, Effect(MoveTemp(InEffect))
				, Scheduler(
					MakeShared<
						FInlineGameThreadScheduler,
						ESPMode::ThreadSafe>())
				, LockFence(
					MakeShared<
						FMonotonicLockFenceProvider,
						ESPMode::ThreadSafe>())
				, Barrier(
					MakeShared<
						FUeAgentGenericEffectBarrierProvider,
						ESPMode::ThreadSafe>(Effect))
				, Coordinator(
					Scheduler,
					LockFence,
					Barrier,
					Durable,
					64)
			{
			}

			bool TryExecute(
				const FPlanningResult& Planning,
				const TArray<FUeAgentOperationTargetBinding>&
					Targets,
				const FUeAgentOperationContext& Context,
				FExecutionReceipt& OutReceipt,
				FUeAgentOperationBindingError& OutError)
				override
			{
				if (!IsInGameThread()
					|| !Planning.GetPlan().IsValid())
				{
					OutError.Code =
						TEXT(
							"niagara_compound_executor_thread_invalid");
					return false;
				}
				const FPlanningPreflightPlanRef Plan =
					Planning.GetPlan().ToSharedRef();
				const FString OperationId =
					TEXT("niagara-compound:")
					+ Plan->GetRequestId();
				TArray<FUeAgentGenericEffectBinding> Bindings;
				for (const FUeAgentOperationTargetBinding& Target :
					Targets)
				{
					Bindings.Emplace(
						Target.GetBinding(),
						TEXT("UE.NiagaraInput:compound"),
						FUeAgentCanonicalType(
							TEXT("uai.niagara.input.v1"),
							EUeAgentValueKind::Object,
							false,
							TEXT("object")),
						TEXT(
							"niagara-native-graph-input"));
				}
				const TSharedRef<
					FUeAgentGenericEffectMutationHook,
					ESPMode::ThreadSafe> Hook =
					MakeShared<
						FUeAgentGenericEffectMutationHook,
						ESPMode::ThreadSafe>(
						MoveTemp(Bindings),
						Effect);
				const FExecutionMutationRequest Request(
					OperationId,
					TEXT("niagara-compound-owner"),
					Plan,
					Context.GetExecutionContextOwner(),
					Context.GetResourceTrackerOwner(),
					Context.GetDeadline(),
					Context.GetCancellationToken(),
					EExecutionThreadAffinity::GameThread,
					EExecutionReentrancyPolicy::
						RejectSameTarget);
				Store->Put(
					OperationId,
					{Planning.GetPlan(), Targets});
				FString SubmittedId;
				FExecutionMutationError Error;
				if (!Coordinator.Submit(
						Request,
						Hook,
						SubmittedId,
						Error))
				{
					Store->Remove(OperationId);
					OutError.Code = Error.GetCode();
					OutError.Detail = Error.GetDetail();
					return false;
				}
				Coordinator.DispatchReady();
				if (!Coordinator.GetReceipt(
						SubmittedId,
						OutReceipt,
						Error))
				{
					Store->Remove(OperationId);
					OutError.Code = Error.GetCode();
					OutError.Detail = Error.GetDetail();
					return false;
				}
				return true;
			}

		private:
			TSharedRef<
				FNiagaraExecutionStore,
				ESPMode::ThreadSafe> Store;
			TSharedRef<
				FNiagaraDurablePort,
				ESPMode::ThreadSafe> Durable;
			TSharedRef<
				FUeAgentNiagaraCompoundEffectHook,
				ESPMode::ThreadSafe> Effect;
			TSharedRef<
				FInlineGameThreadScheduler,
				ESPMode::ThreadSafe> Scheduler;
			TSharedRef<
				FMonotonicLockFenceProvider,
				ESPMode::ThreadSafe> LockFence;
			TSharedRef<
				FUeAgentGenericEffectBarrierProvider,
				ESPMode::ThreadSafe> Barrier;
			FExecutionMutationCoordinator Coordinator;
		};

		class FNeverAbsenceOracle final
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
					false,
					TEXT("niagara-compound-absence-unused"));
				return true;
			}
		};

		class FNoReferenceOracle final
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
					false,
					TEXT("niagara-compound-references-unused"));
				return true;
			}
		};

		bool BuildExpectedSnapshot(
			const FPlannedChange& Change,
			const FUeAgentOperationTargetBinding& Target,
			const FPlanningPreflightPlan& Plan,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FString& OutError)
		{
			if (!Change.GetAfter().IsValid())
			{
				OutError =
					TEXT("niagara_compound_expected_value_missing");
				return false;
			}
			FUeAgentNiagaraCompoundSelector Selector;
			if (!TryParseNiagaraCompoundSelector(
					Target.GetBinding()
						.GetSummary()
						.GetIdentity()
						.GetSelector(),
					Selector,
					OutError))
			{
				return false;
			}
			TArray<FUeAgentStateEntry> Entries;
			Entries.Emplace(
				Change.GetIdentity(),
				FUeAgentCanonicalType(
					TEXT("uai.niagara.input.v1"),
					EUeAgentValueKind::Object,
					false,
					TEXT("object")),
				Change.GetAfter().ToSharedRef(),
				FUeAgentDefaultValue(
					EUeAgentDefaultSourceKind::DomainNative,
					TEXT("Niagara.Input"),
					TEXT("niagara-native-input-default"),
					Change.GetAfter().ToSharedRef()),
				FUeAgentStateDetails(
					Selector.InputName,
					TEXT("Fresh native Niagara graph capture"),
					{TEXT("Niagara"), TEXT("Inputs")},
					true,
					TEXT("available"),
					{
						{
							TEXT("module_node_guid"),
							Selector.ModuleNodeGuid,
						},
						{
							TEXT("stage_key"),
							Selector.StageKey,
						},
					}),
				Plan.GetObservedRevision());
			FUeAgentStateSemanticError Error;
			if (!FUeAgentCanonicalSnapshot::TryCreateExpected(
					TEXT("niagara-compound-expected:")
						+ Plan.GetRequestId(),
					EUeAgentStatePlane::Persisted,
					Plan.GetObservedRevision(),
					FUeAgentTemporalPoint(
						TEXT("uai-operation"),
						FString::Printf(
							TEXT("%lld"),
							Plan.GetObservedRevision()
								.GetSequence())),
					FUeAgentStateProvenance(
						TEXT("operation_expected_state"),
						Plan.GetRequestId(),
						TEXT("niagara-compound-expected:")
							+ Plan.GetRequestId()),
					Entries,
					Resources,
					OutSnapshot,
					Error))
				{
					OutError = Error.Code;
					return false;
				}
			return true;
		}

		class FNiagaraVerifier final
			: public IUeAgentOperationVerifier
		{
		public:
			FNiagaraVerifier(
				TSharedRef<
					FNiagaraExecutionStore,
					ESPMode::ThreadSafe> InStore,
				TSharedRef<
					FNiagaraDurablePort,
					ESPMode::ThreadSafe> InDurable,
				TSharedRef<IUeAgentStateReadPort> InReadPort,
				TSharedRef<
					FUeAgentNiagaraCompoundEffectHook,
					ESPMode::ThreadSafe> InEffect)
				: Store(MoveTemp(InStore))
				, Durable(MoveTemp(InDurable))
				, ReadPort(MoveTemp(InReadPort))
				, Effect(MoveTemp(InEffect))
			{
			}

			bool TryVerify(
				const FExecutionReceipt& Receipt,
				const TArray<FUeAgentOperationTargetBinding>&
					Targets,
				const FUeAgentOperationContext& Context,
				FVerificationOutcome& OutOutcome,
				FUeAgentOperationBindingError& OutError)
				override
			{
				FUeAgentResourceTracker& Resources =
					Context.GetResourceTracker();
				const FUeAgentOperationDeadline& Deadline =
					Context.GetDeadline();
				const FUeAgentCancellationToken& Cancellation =
					Context.GetCancellationToken();
				const FDurableReceipt* DurableReceipt =
					Receipt.GetDurableReceipt();
				FNiagaraExecutionRecord Record;
				if (!DurableReceipt)
				{
					OutError.Code =
						Receipt.GetErrorCode().IsEmpty()
							? TEXT(
								"niagara_compound_execution_not_durable")
							: Receipt.GetErrorCode();
					OutError.SubjectId =
						Receipt.GetOperationId();
					OutError.Detail =
						TEXT(
							"execution terminated before C8D produced a durable receipt");
					return false;
				}
				if (!Store->Get(
						Receipt.GetOperationId(),
						Record))
				{
					OutError.Code =
						TEXT(
							"niagara_compound_verification_context_missing");
					OutError.SubjectId =
						Receipt.GetOperationId();
					return false;
				}
				FDurableInventory Inventory;
				FString OwnerId;
				int64 FencingToken = -1;
				if (!Durable->TryGetContext(
						DurableReceipt->GetJournalId(),
						Inventory,
						OwnerId,
						FencingToken))
				{
					OutError.Code =
						TEXT(
							"niagara_compound_durable_context_missing");
					OutError.SubjectId =
						Receipt.GetOperationId();
					return false;
				}

				TArray<FVerificationTarget> VerificationTargets;
				for (const FPlannedChange& Change :
					Record.Plan->GetChangeSet().GetChanges())
				{
					const FUeAgentOperationTargetBinding*
						Target = Targets.FindByPredicate(
							[&Change](
								const FUeAgentOperationTargetBinding&
									Candidate)
							{
								return Candidate.GetBinding()
									.GetSummary()
									.GetIdentity()
									== Change.GetIdentity();
							});
					if (!Target)
					{
						OutError.Code =
							TEXT(
								"niagara_compound_verification_target_missing");
						return false;
					}
					TArray<FString> ArtifactIds;
					for (const FDurableArtifact& Artifact :
						Inventory.GetArtifacts())
					{
						if (Artifact.GetTargetIdentity()
							== Change.GetIdentity())
						{
							ArtifactIds.Add(
								Artifact.GetArtifactId());
						}
					}
					FUeAgentCanonicalSnapshotPtr Expected;
					if (!BuildExpectedSnapshot(
							Change,
							*Target,
							*Record.Plan,
							Resources,
							Expected,
							OutError.Detail))
					{
						OutError.Code =
							OutError.Detail.IsEmpty()
								? TEXT(
									"niagara_compound_expected_snapshot_failed")
								: OutError.Detail;
						return false;
					}
					VerificationTargets.Emplace(
						Change.GetIdentity(),
						Target->GetBinding(),
						EVerificationOracleKind::Persisted,
						Expected,
						MoveTemp(ArtifactIds),
						TArray<FString>());
				}
				FStateReadPortPersistedStateOracle PersistedOracle(
					*ReadPort);
				FVerificationService Service(
					PersistedOracle,
					Absence,
					Reference,
					*Durable);
				FVerificationError Error;
				const bool bVerified = Service.TryVerify(
					FVerificationRequest(
						Receipt.GetOperationId(),
						Record.Plan
							->GetDescriptorIdentity(),
						*DurableReceipt,
						Inventory,
						OwnerId,
						FencingToken,
						MoveTemp(VerificationTargets)),
					Resources,
					Deadline,
					Cancellation,
					OutOutcome,
					Error);
				Store->Remove(Receipt.GetOperationId());
				if (!bVerified)
				{
					OutError.Code = Error.GetCode();
					OutError.Detail = Error.GetDetail();
					return false;
				}
				Effect->ReleaseVerifiedOperation(
					Receipt.GetOperationId());
				return true;
			}

		private:
			TSharedRef<
				FNiagaraExecutionStore,
				ESPMode::ThreadSafe> Store;
			TSharedRef<
				FNiagaraDurablePort,
				ESPMode::ThreadSafe> Durable;
			TSharedRef<IUeAgentStateReadPort> ReadPort;
			TSharedRef<
				FUeAgentNiagaraCompoundEffectHook,
				ESPMode::ThreadSafe> Effect;
			FNeverAbsenceOracle Absence;
			FNoReferenceOracle Reference;
		};

		class FNiagaraTokenEstimator final
			: public IUeAgentTokenEstimator
		{
		public:
			bool TryEstimate(
				const FUeAgentProjectedPayload& Payload,
				const TArray<FUeAgentArtifactDescriptor>& Artifacts,
				const TArray<FUeAgentProjectionDiagnostic>&
					Diagnostics,
				FUeAgentResourceTracker& Resources,
				const FUeAgentOperationDeadline& Deadline,
				const FUeAgentCancellationToken& Cancellation,
				FUeAgentTokenEstimate& OutEstimate,
				FUeAgentProjectionError& OutError) override
			{
				OutEstimate = FUeAgentTokenEstimate(
					EUeAgentTokenEstimateKind::Conservative,
					4096,
					16384);
				return true;
			}
		};
	}

	class FUeAgentNiagaraCompoundApplication::FState final
	{
	public:
		TSharedRef<const FUeAgentOperationBinding> Binding;

		explicit FState(
			TSharedRef<const FUeAgentOperationBinding> InBinding)
			: Binding(MoveTemp(InBinding))
		{
		}
	};

	FUeAgentNiagaraCompoundApplication::
		FUeAgentNiagaraCompoundApplication(
			TSharedRef<FState, ESPMode::ThreadSafe> InState)
		: State(MoveTemp(InState))
	{
	}

	FUeAgentNiagaraCompoundApplication::
		~FUeAgentNiagaraCompoundApplication() = default;

	TSharedRef<const FUeAgentOperationBinding>
	FUeAgentNiagaraCompoundApplication::GetOperationBinding() const
	{
		return State->Binding;
	}

	bool FUeAgentNiagaraCompoundApplication::TryCreate(
		const FUeAgentNiagaraCompoundApplicationConfig& Config,
		TSharedRef<const
			BindingGateway::Ticket::
				FUeAgentOperationTicketService> TicketService,
		TSharedRef<IUeAgentStateReadPort> ReadPort,
		TSharedRef<
			FUeAgentNiagaraCompoundNativeHook,
			ESPMode::ThreadSafe> NativeHook,
		TSharedPtr<FUeAgentNiagaraCompoundApplication>&
			OutApplication,
		FUeAgentNiagaraCompoundApplicationError& OutError)
	{
		OutApplication.Reset();
		OutError = {};
		const TSharedRef<const FUeAgentDescriptorRegistry>
			OperationDescriptors =
				GetNiagaraCompoundOperationDescriptorRegistry();
		const FUeAgentDescriptorIdentity OperationDescriptor =
			GetNiagaraCompoundOperationDescriptorIdentity();
		const FUeAgentOperationDescriptor* Descriptor =
			OperationDescriptors->FindExact(
				OperationDescriptor);
		if (!Descriptor
			|| Config.ServerSessionId.IsEmpty()
			|| Config.RegistryEpoch < 1
			|| Config.TimeoutSeconds <= 0.0
			|| Config.MaxFrameBytes <= 0
			|| Config.JournalRoot.IsEmpty()
			|| ReadPort->GetPortId()
				!= GetNiagaraCompoundReadPortId()
			|| !NativeHook->IsAvailable())
		{
			SetError(
				OutError,
				TEXT(
					"niagara_compound_application_config_invalid"),
				TEXT(
					"Niagara compound operation composition is incomplete"));
			return false;
		}
		FUeAgentExecutionContextRefPtr ExpectedContext;
		FUeAgentExecutionContextError ContextError;
		if (!FUeAgentExecutionContextRef::TryCreate(
				Config.ExecutionContext,
				Descriptor->GetIdentity(),
				EUeAgentRequestKind::Operation,
				Descriptor->GetExecutionContextPolicy(),
				ExpectedContext,
				ContextError))
		{
			SetError(
				OutError,
				ContextError.Code,
				TEXT(
					"Niagara compound execution context is invalid"));
			return false;
		}
		const TSharedRef<
			FNiagaraExecutionStore,
			ESPMode::ThreadSafe> Store =
			MakeShared<
				FNiagaraExecutionStore,
				ESPMode::ThreadSafe>();
		const TSharedRef<
			FNiagaraDurablePort,
			ESPMode::ThreadSafe> Durable =
			MakeShared<
				FNiagaraDurablePort,
				ESPMode::ThreadSafe>(
				Config.JournalRoot);
		const TSharedRef<
			FUeAgentNiagaraCompoundEffectHook,
			ESPMode::ThreadSafe> Effect =
			MakeShared<
				FUeAgentNiagaraCompoundEffectHook,
				ESPMode::ThreadSafe>(NativeHook);
		const TSharedRef<FNiagaraPlanner> Planner =
			MakeShared<FNiagaraPlanner>();
		const TSharedRef<FNiagaraExecutor> Executor =
			MakeShared<FNiagaraExecutor>(
				Store,
				Durable,
				Effect);
		const TSharedRef<FNiagaraVerifier> Verifier =
			MakeShared<FNiagaraVerifier>(
				Store,
				Durable,
				ReadPort,
				Effect);
		const TSharedRef<FNiagaraTokenEstimator> Estimator =
			MakeShared<FNiagaraTokenEstimator>();
		TSharedPtr<const FUeAgentResponseProjector> Projector;
		FUeAgentProjectionError ProjectionError;
		if (!FUeAgentResponseProjector::TryCreate(
				Estimator,
				Projector,
				ProjectionError))
		{
			SetError(
				OutError,
				ProjectionError.Code,
				ProjectionError.Detail);
			return false;
		}
		const TSharedRef<FNiagaraSessionSource> SessionSource =
			MakeShared<FNiagaraSessionSource>(
				FUeAgentOperationSessionIdentity(
					Config.ServerSessionId,
					Config.RegistryEpoch,
					OperationDescriptors->GetRegistryHash()));
		const TSharedRef<
			FUeAgentExactExecutionContextValidator>
			ContextValidator =
				MakeShared<
					FUeAgentExactExecutionContextValidator>(
					ExpectedContext.ToSharedRef());
		TSharedPtr<const FUeAgentOperationBinding> Binding;
		FUeAgentOperationBindingError BindingError;
		if (!FUeAgentOperationBinding::TryCreate(
				OperationDescriptors,
				OperationDescriptor,
				ContextValidator,
				Planner,
				Executor,
				Verifier,
				Projector.ToSharedRef(),
				SessionSource,
				TicketService,
				Binding,
				BindingError))
		{
			SetError(
				OutError,
				BindingError.Code,
				BindingError.Detail);
			return false;
		}
		OutApplication = MakeShareable(
			new FUeAgentNiagaraCompoundApplication(
				MakeShared<FState, ESPMode::ThreadSafe>(
					Binding.ToSharedRef())));
		return true;
	}
}
