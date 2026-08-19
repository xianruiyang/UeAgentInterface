#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringProduction.h"

#include "Misc/ScopeLock.h"

namespace UE::UAI::Composition::SimpleAuthoring
{
	namespace
	{
		using namespace AssetDurable;
		using namespace BindingGateway::Operation;
		using namespace ContextResource;
		using namespace Contract;
		using namespace Discovery;
		using namespace DomainAdapters::GenericEffect;
		using namespace ExecutionMutation;
		using namespace PlanningPreflight;
		using namespace Projection;
		using namespace StateSemantic;
		using namespace Verification;

		void SetError(
			FUeAgentSimpleAuthoringProductionError& OutError,
			const FString& Code,
			const FString& SubjectId = FString(),
			const FString& Detail = FString())
		{
			OutError.Code = Code;
			OutError.SubjectId = SubjectId;
			OutError.Detail = Detail;
		}

		void CopyError(
			const FUeAgentSimpleAuthoringProductionError& Error,
			FUeAgentOperationBindingError& OutError)
		{
			OutError.Code = Error.Code;
			OutError.SubjectId = Error.SubjectId;
			OutError.Detail = Error.Detail;
		}

		struct FPreparationRecord
		{
			TSharedPtr<const FUeAgentCandidateBinding>
				ResolvedTarget;
			TArray<FUeAgentCandidateBinding> ChangeTargets;
			FUeAgentCanonicalSnapshotPtr PreState;
		};

		struct FExecutionRecord
		{
			FPlanningPreflightPlanPtr Plan;
			TSharedPtr<const FUeAgentCandidateBinding>
				ResolvedTarget;
			TArray<FUeAgentCandidateBinding> ChangeTargets;
			FUeAgentCanonicalSnapshotPtr PreState;
		};

		class FProductionStore final
		{
		public:
			void PutPreparation(
				const FString& RequestId,
				const FUeAgentCandidateBinding& ResolvedTarget,
				TArray<FUeAgentCandidateBinding> ChangeTargets,
				const FUeAgentCanonicalSnapshotRef& PreState)
			{
				FScopeLock Lock(&Mutex);
				Preparations.Add(
					RequestId,
					{
						MakeShared<const FUeAgentCandidateBinding>(
							ResolvedTarget),
						MoveTemp(ChangeTargets),
						PreState,
					});
			}

			bool GetPreparation(
				const FString& RequestId,
				FPreparationRecord& OutRecord) const
			{
				FScopeLock Lock(&Mutex);
				const FPreparationRecord* Found =
					Preparations.Find(RequestId);
				if (!Found)
				{
					return false;
				}
				OutRecord = *Found;
				return OutRecord.ResolvedTarget.IsValid()
					&& !OutRecord.ChangeTargets.IsEmpty()
					&& OutRecord.PreState.IsValid();
			}

			void RemovePreparation(const FString& RequestId)
			{
				FScopeLock Lock(&Mutex);
				Preparations.Remove(RequestId);
			}

			void PutExecution(
				const FString& OperationId,
				FExecutionRecord Record)
			{
				FScopeLock Lock(&Mutex);
				Executions.Add(OperationId, MoveTemp(Record));
			}

			bool GetExecution(
				const FString& OperationId,
				FExecutionRecord& OutRecord) const
			{
				FScopeLock Lock(&Mutex);
				const FExecutionRecord* Found =
					Executions.Find(OperationId);
				if (!Found)
				{
					return false;
				}
				OutRecord = *Found;
				return OutRecord.Plan.IsValid()
					&& OutRecord.ResolvedTarget.IsValid()
					&& !OutRecord.ChangeTargets.IsEmpty()
					&& OutRecord.PreState.IsValid();
			}

			void RemoveExecution(const FString& OperationId)
			{
				FScopeLock Lock(&Mutex);
				Executions.Remove(OperationId);
			}

		private:
			mutable FCriticalSection Mutex;
			TMap<FString, FPreparationRecord> Preparations;
			TMap<FString, FExecutionRecord> Executions;
		};

		EUeAgentCandidateOperation CandidateOperation(
			const EChangeIntent Intent)
		{
			switch (Intent)
			{
			case EChangeIntent::Create:
				return EUeAgentCandidateOperation::Create;
			case EChangeIntent::Replace:
				return EUeAgentCandidateOperation::Replace;
			case EChangeIntent::Delete:
				return EUeAgentCandidateOperation::Delete;
			case EChangeIntent::Update:
			default:
				return EUeAgentCandidateOperation::Update;
			}
		}

		bool SameChangeTargets(
			const TArray<FUeAgentOperationTargetBinding>& Bound,
			const TArray<FUeAgentCandidateBinding>& Prepared)
		{
			if (Bound.Num() != Prepared.Num())
			{
				return false;
			}
			for (const FUeAgentCandidateBinding& Candidate : Prepared)
			{
				const FUeAgentOperationTargetBinding* Match =
					Bound.FindByPredicate(
						[&Candidate](
							const FUeAgentOperationTargetBinding&
								Target)
						{
							return Target.GetBinding()
									.GetBindingId()
								== Candidate.GetBindingId()
								&& Target.GetBinding()
										.GetSummary()
										.GetIdentity()
										.GetCanonicalKey()
									== Candidate.GetSummary()
										.GetIdentity()
										.GetCanonicalKey();
						});
				if (!Match)
				{
					return false;
				}
			}
			return true;
		}

		bool ValidateChangeTargets(
			const FChangeSetDraft& Draft,
			const FUeAgentDescriptorIdentity& DescriptorIdentity,
			const FString& PersistedPortId,
			const TArray<FUeAgentCandidateBinding>& Targets,
			FUeAgentSimpleAuthoringProductionError& OutError)
		{
			const TArray<FChangeDraft>& Changes =
				Draft.GetChanges();
			if (Changes.IsEmpty()
				|| Targets.Num() != Changes.Num())
			{
				SetError(
					OutError,
					TEXT(
						"simple_authoring_change_targets_missing"),
					Draft.GetRequestId());
				return false;
			}
			TSet<FString> SeenIdentities;
			for (const FChangeDraft& Change : Changes)
			{
				const FString& IdentityKey =
					Change.GetIdentity().GetCanonicalKey();
				const FUeAgentCandidateBinding* Target =
					Targets.FindByPredicate(
						[&IdentityKey](
							const FUeAgentCandidateBinding&
								Candidate)
						{
							return Candidate.GetSummary()
									.GetIdentity()
									.GetCanonicalKey()
								== IdentityKey;
						});
				if (!Target
					|| !Target->IsValid()
					|| SeenIdentities.Contains(IdentityKey)
					|| !(Target->GetSummary()
							.GetDescriptorIdentity()
						== DescriptorIdentity)
					|| Target->GetReadPortId()
						!= PersistedPortId
					|| !Target->Supports(
						CandidateOperation(
							Change.GetIntent())))
				{
					SetError(
						OutError,
						TEXT(
							"simple_authoring_change_target_invalid"),
						IdentityKey);
					return false;
				}
				SeenIdentities.Add(IdentityKey);
			}
			return SeenIdentities.Num() == Targets.Num();
		}

		class FProductionPlanner final
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
					OutError.SubjectId = Error.GetIdentityKey();
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
				if (Affinity != EExecutionThreadAffinity::GameThread
					|| !IsInGameThread()
					|| !Callback)
				{
					OutError = FExecutionMutationError(
						TEXT(
							"simple_authoring_execution_thread_invalid"),
						OperationId);
					return false;
				}
				Callback();
				return true;
			}

			bool IsCurrentThread(
				EExecutionThreadAffinity Affinity) const override
			{
				return Affinity == EExecutionThreadAffinity::GameThread
					&& IsInGameThread();
			}
		};

		class FProductionDurableSession final
			: public IExecutionDurableSession
		{
		public:
			explicit FProductionDurableSession(
				FAssetDurableSessionRef InSession)
				: Session(MoveTemp(InSession))
			{
			}

			FAssetDurableSessionRef Session;
		};

		class FProductionDurablePort final
			: public IDurableExecutionPort
			, public IDurableVerificationAcknowledgerPort
		{
		public:
			FProductionDurablePort(
				TSharedRef<
					FProductionStore,
					ESPMode::ThreadSafe> InStore,
				TSharedRef<
					IUeAgentSimpleAuthoringDurableTargetProjector,
					ESPMode::ThreadSafe> InProjector,
				TSharedRef<
					IUeAgentSimpleAuthoringAssetDurableService,
					ESPMode::ThreadSafe> InService,
				EAtomicityMode InAtomicityMode)
				: Store(MoveTemp(InStore))
				, Projector(MoveTemp(InProjector))
				, Service(MoveTemp(InService))
				, AtomicityMode(InAtomicityMode)
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
				FExecutionRecord Record;
				if (!Store->GetExecution(
					Request.GetOperationId(),
					Record))
				{
					OutError = FExecutionMutationError(
						TEXT(
							"simple_authoring_execution_record_missing"),
						Request.GetOperationId());
					return false;
				}

				TArray<FAssetDurableTarget> Targets;
				FUeAgentSimpleAuthoringProductionError ProjectionError;
				if (!Projector->TryProjectDurableTargets(
					Request.GetPlan(),
					*Record.ResolvedTarget,
					Record.PreState.ToSharedRef(),
					Resources,
					Targets,
					ProjectionError)
					|| Targets.IsEmpty())
				{
					OutError = FExecutionMutationError(
						ProjectionError.IsValid()
							? ProjectionError.Code
							: TEXT(
								"simple_authoring_durable_targets_missing"),
						ProjectionError.IsValid()
							? ProjectionError.Detail
							: Request.GetOperationId());
					return false;
				}

				const FAssetDurableRequest DurableRequest(
					Request.GetPlan(),
					MoveTemp(Targets),
					FencingToken,
					Request.GetOwnerLeaseId(),
					AtomicityMode);
				FAssetDurableSessionPtr Session;
				FAssetDurableError Error;
				if (!DurableRequest.IsValid()
					|| !Service->TryPrepare(
						DurableRequest,
						Resources,
						Session,
						Error)
					|| !Session.IsValid())
				{
					OutError = FExecutionMutationError(
						Error.IsPresent()
							? Error.GetCode()
							: TEXT(
								"simple_authoring_durable_prepare_failed"),
						Error.IsPresent()
							? Error.GetDetail()
							: Request.GetOperationId());
					return false;
				}
				OutSession = MakeShared<
					FProductionDurableSession,
					ESPMode::ThreadSafe>(Session.ToSharedRef());
				return true;
			}

			bool Persist(
				const FExecutionDurableSessionRef& Session,
				int64 FencingToken,
				FUeAgentResourceTracker& Resources,
				FDurableReceipt& OutReceipt,
				FExecutionMutationError& OutError) override
			{
				const TSharedRef<FProductionDurableSession> Typed =
					StaticCastSharedRef<
						FProductionDurableSession>(Session);
				FAssetDurableError Error;
				if (!Service->TryPersist(
					Typed->Session,
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
				{
					FScopeLock Lock(&Mutex);
					PersistedByJournal.Add(
						OutReceipt.GetJournalId(),
						Typed->Session);
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
				const TSharedRef<FProductionDurableSession> Typed =
					StaticCastSharedRef<
						FProductionDurableSession>(Session);
				FAssetDurableError Error;
				if (!Service->TryRollback(
					Typed->Session,
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
							"simple_authoring_durable_session_missing"),
						Acknowledgement.GetJournalId());
					return false;
				}
				if (!Service->TryAcknowledge(
					Session.ToSharedRef(),
					Acknowledgement,
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

			bool TryGetVerificationContext(
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
			TSharedRef<FProductionStore, ESPMode::ThreadSafe> Store;
			TSharedRef<
				IUeAgentSimpleAuthoringDurableTargetProjector,
				ESPMode::ThreadSafe> Projector;
			TSharedRef<
				IUeAgentSimpleAuthoringAssetDurableService,
				ESPMode::ThreadSafe> Service;
			EAtomicityMode AtomicityMode =
				EAtomicityMode::CompensatedSaga;
			mutable FCriticalSection Mutex;
			TMap<FString, FAssetDurableSessionPtr>
				PersistedByJournal;
		};

		class FProductionExecutor final
			: public IUeAgentOperationExecutor
		{
		public:
			FProductionExecutor(
				FString InFamilyId,
				FString InOwnerLeaseId,
				int32 InMaxQueueItems,
				TSharedRef<
					FProductionStore,
					ESPMode::ThreadSafe> InStore,
				TSharedRef<
					IUeAgentSimpleAuthoringMutationBindingProjector,
					ESPMode::ThreadSafe> InProjector,
				TSharedRef<
					IUeAgentGenericEffectUeHook,
					ESPMode::ThreadSafe> InUeHook,
				TSharedRef<
					FProductionDurablePort,
					ESPMode::ThreadSafe> InDurable)
				: FamilyId(MoveTemp(InFamilyId))
				, OwnerLeaseId(MoveTemp(InOwnerLeaseId))
				, Store(MoveTemp(InStore))
				, Projector(MoveTemp(InProjector))
				, UeHook(MoveTemp(InUeHook))
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
						ESPMode::ThreadSafe>(UeHook))
				, Durable(MoveTemp(InDurable))
				, Coordinator(
					Scheduler,
					LockFence,
					Barrier,
					Durable,
					InMaxQueueItems)
			{
			}

			bool TryExecute(
				const FPlanningResult& Planning,
				const TArray<FUeAgentOperationTargetBinding>& Targets,
				const FUeAgentOperationContext& Context,
				FExecutionReceipt& OutReceipt,
				FUeAgentOperationBindingError& OutError) override
			{
				if (!IsInGameThread()
					|| !Planning.IsReady()
					|| !Planning.GetPlan().IsValid())
				{
					OutError.Code =
						TEXT(
							"simple_authoring_execution_not_ready");
					return false;
				}
				const FPlanningPreflightPlanRef Plan =
					Planning.GetPlan().ToSharedRef();
				FPreparationRecord Preparation;
				if (!Store->GetPreparation(
					Plan->GetRequestId(),
					Preparation)
					|| !SameChangeTargets(
						Targets,
						Preparation.ChangeTargets))
				{
					OutError.Code =
						TEXT(
							"simple_authoring_preparation_mismatch");
					OutError.SubjectId = Plan->GetRequestId();
					return false;
				}

				TArray<FUeAgentGenericEffectBinding> Bindings;
				FUeAgentSimpleAuthoringProductionError ProjectionError;
				if (!Projector->TryProjectMutationBindings(
					Plan,
					Preparation.ChangeTargets,
					Preparation.PreState.ToSharedRef(),
					Context.GetExecutionContext(),
					Context.GetResourceTracker(),
					Bindings,
					ProjectionError)
					|| Bindings.IsEmpty())
				{
					CopyError(ProjectionError, OutError);
					if (OutError.Code.IsEmpty())
					{
						OutError.Code =
							TEXT(
								"simple_authoring_mutation_bindings_missing");
					}
					return false;
				}

				const FString OperationId =
					TEXT("simple-authoring:")
					+ FamilyId
					+ TEXT(":")
					+ Plan->GetRequestId();
				Store->PutExecution(
					OperationId,
					{
						Planning.GetPlan(),
						Preparation.ResolvedTarget,
						Preparation.ChangeTargets,
						Preparation.PreState,
					});
				Store->RemovePreparation(Plan->GetRequestId());

				const TSharedRef<
					FUeAgentGenericEffectMutationHook,
					ESPMode::ThreadSafe> Hook =
						MakeShared<
							FUeAgentGenericEffectMutationHook,
							ESPMode::ThreadSafe>(
							MoveTemp(Bindings),
							UeHook);
				const FExecutionMutationRequest Request(
					OperationId,
					OwnerLeaseId,
					Plan,
					Context.GetExecutionContextOwner(),
					Context.GetResourceTrackerOwner(),
					Context.GetDeadline(),
					Context.GetCancellationToken(),
					EExecutionThreadAffinity::GameThread,
					EExecutionReentrancyPolicy::RejectSameTarget);
				FString SubmittedId;
				FExecutionMutationError Error;
				if (!Coordinator.Submit(
					Request,
					Hook,
					SubmittedId,
					Error))
				{
					Store->RemoveExecution(OperationId);
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
					Store->RemoveExecution(OperationId);
					OutError.Code = Error.GetCode();
					OutError.Detail = Error.GetDetail();
					return false;
				}
				return true;
			}

		private:
			FString FamilyId;
			FString OwnerLeaseId;
			TSharedRef<FProductionStore, ESPMode::ThreadSafe> Store;
			TSharedRef<
				IUeAgentSimpleAuthoringMutationBindingProjector,
				ESPMode::ThreadSafe> Projector;
			TSharedRef<
				IUeAgentGenericEffectUeHook,
				ESPMode::ThreadSafe> UeHook;
			TSharedRef<
				FInlineGameThreadScheduler,
				ESPMode::ThreadSafe> Scheduler;
			TSharedRef<
				FMonotonicLockFenceProvider,
				ESPMode::ThreadSafe> LockFence;
			TSharedRef<
				FUeAgentGenericEffectBarrierProvider,
				ESPMode::ThreadSafe> Barrier;
			TSharedRef<
				FProductionDurablePort,
				ESPMode::ThreadSafe> Durable;
			FExecutionMutationCoordinator Coordinator;
		};

		class FProductionVerifier final
			: public IUeAgentOperationVerifier
		{
		public:
			FProductionVerifier(
				TSharedRef<
					FProductionStore,
					ESPMode::ThreadSafe> InStore,
				TSharedRef<
					FProductionDurablePort,
					ESPMode::ThreadSafe> InDurable,
				TSharedRef<
					IUeAgentSimpleAuthoringExpectedStateProjector,
					ESPMode::ThreadSafe> InExpectedStateProjector,
				TSharedRef<
					IUeAgentStateReadPort,
					ESPMode::ThreadSafe> InPersistedStateReadPort,
				TSharedRef<
					IAbsenceOracle,
					ESPMode::ThreadSafe> InAbsenceOracle,
				TSharedRef<
					IReferenceOracle,
					ESPMode::ThreadSafe> InReferenceOracle,
				TSharedRef<
					IUeAgentGenericEffectUeHook,
					ESPMode::ThreadSafe> InUeHook)
				: Store(MoveTemp(InStore))
				, Durable(MoveTemp(InDurable))
				, ExpectedStateProjector(
					MoveTemp(InExpectedStateProjector))
				, PersistedStateReadPort(
					MoveTemp(InPersistedStateReadPort))
				, AbsenceOracle(MoveTemp(InAbsenceOracle))
				, ReferenceOracle(MoveTemp(InReferenceOracle))
				, UeHook(MoveTemp(InUeHook))
			{
			}

			bool TryVerify(
				const FExecutionReceipt& Receipt,
				const TArray<FUeAgentOperationTargetBinding>& Targets,
				const FUeAgentOperationContext& Context,
				FVerificationOutcome& OutOutcome,
				FUeAgentOperationBindingError& OutError) override
			{
				const FDurableReceipt* DurableReceipt =
					Receipt.GetDurableReceipt();
				FExecutionRecord Record;
				if (!DurableReceipt
					|| !Store->GetExecution(
						Receipt.GetOperationId(), Record)
					|| !SameChangeTargets(
						Targets, Record.ChangeTargets))
				{
					OutError.Code =
						TEXT(
							"simple_authoring_verification_context_missing");
					OutError.SubjectId = Receipt.GetOperationId();
					return false;
				}

				FDurableInventory Inventory;
				FString OwnerId;
				int64 FencingToken = -1;
				if (!Durable->TryGetVerificationContext(
					DurableReceipt->GetJournalId(),
					Inventory,
					OwnerId,
					FencingToken))
				{
					OutError.Code =
						TEXT(
							"simple_authoring_durable_context_missing");
					OutError.SubjectId =
						DurableReceipt->GetJournalId();
					return false;
				}

				TArray<FVerificationTarget> VerificationTargets;
				FUeAgentSimpleAuthoringProductionError ProjectionError;
				if (!ExpectedStateProjector
					->TryProjectExpectedState(
						Record.Plan.ToSharedRef(),
						*Record.ResolvedTarget,
						Record.ChangeTargets,
						Record.PreState.ToSharedRef(),
						Inventory,
						Context.GetResourceTracker(),
						VerificationTargets,
						ProjectionError)
					|| VerificationTargets.IsEmpty())
				{
					CopyError(ProjectionError, OutError);
					if (OutError.Code.IsEmpty())
					{
						OutError.Code =
							TEXT(
								"simple_authoring_expected_state_missing");
					}
					return false;
				}

				FVerificationService Service(
					*PersistedStateReadPort,
					*AbsenceOracle,
					*ReferenceOracle,
					*Durable);
				FVerificationError Error;
				const bool bVerified = Service.TryVerify(
					FVerificationRequest(
						Receipt.GetOperationId(),
						Record.Plan->GetDescriptorIdentity(),
						*DurableReceipt,
						Inventory,
						OwnerId,
						FencingToken,
						MoveTemp(VerificationTargets)),
					Context.GetResourceTracker(),
					Context.GetDeadline(),
					Context.GetCancellationToken(),
					OutOutcome,
					Error);
				Store->RemoveExecution(Receipt.GetOperationId());
				UeHook->ReleaseVerifiedOperation(
					Receipt.GetOperationId());
				if (!bVerified)
				{
					OutError.Code = Error.GetCode();
					OutError.Detail = Error.GetDetail();
					return false;
				}
				return true;
			}

		private:
			TSharedRef<FProductionStore, ESPMode::ThreadSafe> Store;
			TSharedRef<
				FProductionDurablePort,
				ESPMode::ThreadSafe> Durable;
			TSharedRef<
				IUeAgentSimpleAuthoringExpectedStateProjector,
				ESPMode::ThreadSafe> ExpectedStateProjector;
			TSharedRef<
				IUeAgentStateReadPort,
				ESPMode::ThreadSafe> PersistedStateReadPort;
			TSharedRef<
				IAbsenceOracle,
				ESPMode::ThreadSafe> AbsenceOracle;
			TSharedRef<
				IReferenceOracle,
				ESPMode::ThreadSafe> ReferenceOracle;
			TSharedRef<
				IUeAgentGenericEffectUeHook,
				ESPMode::ThreadSafe> UeHook;
		};

		class FProductionProvider final
			: public IUeAgentSimpleAuthoringCapabilityProvider
		{
		public:
			FProductionProvider(
				FUeAgentSimpleAuthoringProductionProviderConfig
					InConfig,
				TSharedRef<
					FProductionStore,
					ESPMode::ThreadSafe> InStore,
				TSharedRef<
					FProductionPlanner,
					ESPMode::ThreadSafe> InPlanner,
				TSharedRef<
					FProductionExecutor,
					ESPMode::ThreadSafe> InExecutor,
				TSharedRef<
					FProductionVerifier,
					ESPMode::ThreadSafe> InVerifier)
				: Config(MoveTemp(InConfig))
				, Store(MoveTemp(InStore))
				, Planner(MoveTemp(InPlanner))
				, Executor(MoveTemp(InExecutor))
				, Verifier(MoveTemp(InVerifier))
			{
			}

			const FString& GetFamilyId() const override
			{
				return Config.FamilyId;
			}

			TSharedRef<IUeAgentOperationPlanner>
			GetPlanner() override
			{
				return Planner;
			}

			TSharedRef<IUeAgentOperationExecutor>
			GetExecutor() override
			{
				return Executor;
			}

			TSharedRef<IUeAgentOperationVerifier>
			GetVerifier() override
			{
				return Verifier;
			}

			bool TryPrepare(
				const FUeAgentOperationRequest& CanonicalRequest,
				const FUeAgentExecutionContextRef& Context,
				const FUeAgentCallerContext& CallerContext,
				FUeAgentResourceTracker& Resources,
				const FUeAgentOperationDeadline& Deadline,
				const FUeAgentCancellationToken& Cancellation,
				TOptional<
					FUeAgentSimpleAuthoringPreparedOperation>&
						OutPrepared,
				FUeAgentSimpleAuthoringApplicationError& OutError)
				override
			{
				OutPrepared.Reset();
				TOptional<FUeAgentCandidateBinding> Target;
				FUeAgentSimpleAuthoringProductionError Error;
				if (!Config.TargetResolver->TryResolveTarget(
					CanonicalRequest,
					Context,
					Resources,
					Deadline,
					Cancellation,
					Target,
					Error)
					|| !Target.IsSet()
					|| !Target->IsValid())
				{
					OutError.Code = Error.IsValid()
						? Error.Code
						: TEXT(
							"simple_authoring_target_resolution_failed");
					OutError.SubjectId = Error.SubjectId;
					OutError.Detail = Error.Detail;
					return false;
				}
				if (!(Target->GetSummary().GetDescriptorIdentity()
						== CanonicalRequest.GetDescriptorIdentity())
					|| Target->GetReadPortId()
						!= Config.PersistedStateReadPort
							->GetPortId())
				{
					OutError.Code =
						TEXT(
							"simple_authoring_target_binding_mismatch");
					OutError.SubjectId = Target->GetBindingId();
					return false;
				}

				FUeAgentCanonicalSnapshotPtr PreState;
				FUeAgentDiscoveryError ReadError;
				if (!Config.PreStateReadPort->TryRead(
					FUeAgentStateReadRequest(
						*Target,
						Config.PreStatePlane),
					Resources,
					Deadline,
					Cancellation,
					PreState,
					ReadError)
					|| !PreState.IsValid())
				{
					OutError.Code = !ReadError.Code.IsEmpty()
						? ReadError.Code
						: TEXT(
							"simple_authoring_pre_state_unavailable");
					OutError.SubjectId = Target->GetBindingId();
					OutError.Detail = ReadError.Detail;
					return false;
				}

				TOptional<FChangeSetDraft> Draft;
				if (!Config.DraftBuilder->TryBuildDraft(
					CanonicalRequest,
					Context,
					PreState.ToSharedRef(),
					Resources,
					Draft,
					Error)
					|| !Draft.IsSet()
					|| !Draft->IsValid()
					|| !(Draft->GetDescriptorIdentity()
						== CanonicalRequest.GetDescriptorIdentity())
					|| Draft->GetRequestId()
						!= CanonicalRequest.GetRequestId()
					|| Draft->GetIdempotencyKey()
						!= CanonicalRequest.GetIdempotencyKey()
					|| Draft->GetSnapshot()->GetSnapshotId()
						!= PreState->GetSnapshotId())
				{
					OutError.Code = Error.IsValid()
						? Error.Code
						: TEXT(
							"simple_authoring_draft_invalid");
					OutError.SubjectId =
						CanonicalRequest.GetRequestId();
					OutError.Detail = Error.Detail;
					return false;
				}

				TArray<FUeAgentCandidateBinding> ChangeTargets;
				Error =
					FUeAgentSimpleAuthoringProductionError();
				if (!Config.ChangeTargetProjector
						->TryProjectChangeTargets(
							CanonicalRequest,
							Context,
							Draft.GetValue(),
							*Target,
							PreState.ToSharedRef(),
							Resources,
							ChangeTargets,
							Error)
					|| !ValidateChangeTargets(
						Draft.GetValue(),
						CanonicalRequest.GetDescriptorIdentity(),
						Config.PersistedStateReadPort
							->GetPortId(),
						ChangeTargets,
						Error))
				{
					OutError.Code = Error.IsValid()
						? Error.Code
						: TEXT(
							"simple_authoring_change_targets_missing");
					OutError.SubjectId = Error.SubjectId;
					OutError.Detail = Error.Detail;
					return false;
				}

				Store->PutPreparation(
					CanonicalRequest.GetRequestId(),
					*Target,
					ChangeTargets,
					PreState.ToSharedRef());
				OutPrepared.Emplace(
					MoveTemp(ChangeTargets),
					FPlanningRequest(
						MoveTemp(Draft.GetValue()),
						CallerContext,
						false),
					Config.Projection.GetValue());
				if (!OutPrepared->IsValid())
				{
					Store->RemovePreparation(
						CanonicalRequest.GetRequestId());
					OutError.Code =
						TEXT(
							"simple_authoring_prepared_operation_invalid");
					return false;
				}
				return true;
			}

		private:
			FUeAgentSimpleAuthoringProductionProviderConfig Config;
			TSharedRef<FProductionStore, ESPMode::ThreadSafe> Store;
			TSharedRef<
				FProductionPlanner,
				ESPMode::ThreadSafe> Planner;
			TSharedRef<
				FProductionExecutor,
				ESPMode::ThreadSafe> Executor;
			TSharedRef<
				FProductionVerifier,
				ESPMode::ThreadSafe> Verifier;
		};

		bool ValidateConfig(
			const FUeAgentSimpleAuthoringProductionProviderConfig&
				Config,
			FUeAgentSimpleAuthoringProductionError& OutError)
		{
			if (Config.FamilyId.IsEmpty())
			{
				SetError(
					OutError,
					TEXT(
						"simple_authoring_production_family_missing"));
				return false;
			}
			if (Config.OwnerLeaseId.IsEmpty())
			{
				SetError(
					OutError,
					TEXT(
						"simple_authoring_production_owner_missing"),
					Config.FamilyId);
				return false;
			}
			if (!Config.Projection.IsSet()
				|| !Config.Projection->IsValid())
			{
				SetError(
					OutError,
					TEXT(
						"simple_authoring_production_projection_missing"),
					Config.FamilyId);
				return false;
			}
			if (!Config.TargetResolver.IsValid())
			{
				SetError(
					OutError,
					TEXT(
						"simple_authoring_target_resolver_missing"),
					Config.FamilyId);
				return false;
			}
			if (!Config.PreStateReadPort.IsValid())
			{
				SetError(
					OutError,
					TEXT(
						"simple_authoring_pre_state_port_missing"),
					Config.FamilyId);
				return false;
			}
			if (!Config.ChangeTargetProjector.IsValid())
			{
				SetError(
					OutError,
					TEXT(
						"simple_authoring_change_target_"
						"projector_missing"),
					Config.FamilyId);
				return false;
			}
			if (!Config.DraftBuilder.IsValid())
			{
				SetError(
					OutError,
					TEXT(
						"simple_authoring_draft_builder_missing"),
					Config.FamilyId);
				return false;
			}
			if (!Config.MutationBindingProjector.IsValid()
				|| !Config.UeHook.IsValid())
			{
				SetError(
					OutError,
					TEXT(
						"simple_authoring_mutation_port_missing"),
					Config.FamilyId);
				return false;
			}
			if (!Config.DurableTargetProjector.IsValid()
				|| !Config.DurableService.IsValid())
			{
				SetError(
					OutError,
					TEXT(
						"simple_authoring_durable_port_missing"),
					Config.FamilyId);
				return false;
			}
			if (!Config.ExpectedStateProjector.IsValid()
				|| !Config.PersistedStateReadPort.IsValid()
				|| !Config.AbsenceOracle.IsValid()
				|| !Config.ReferenceOracle.IsValid())
			{
				SetError(
					OutError,
					TEXT(
						"simple_authoring_verification_port_missing"),
					Config.FamilyId);
				return false;
			}
			if (Config.PreStateReadPort
					== Config.PersistedStateReadPort
				|| Config.PreStateReadPort->GetPortId().IsEmpty()
				|| Config.PersistedStateReadPort
					->GetPortId()
					.IsEmpty()
				|| Config.PreStateReadPort->GetPortId()
					== Config.PersistedStateReadPort
						->GetPortId())
			{
				SetError(
					OutError,
					TEXT(
						"simple_authoring_c9_read_port_not_independent"),
					Config.FamilyId);
				return false;
			}
			if (Config.MaxQueueItems < 1)
			{
				SetError(
					OutError,
					TEXT(
						"simple_authoring_queue_limit_invalid"),
					Config.FamilyId);
				return false;
			}
			return true;
		}
	}

	bool FUeAgentSimpleAuthoringProductionProvider::TryCreate(
		const FUeAgentSimpleAuthoringProductionProviderConfig& Config,
		TSharedPtr<IUeAgentSimpleAuthoringCapabilityProvider>&
			OutProvider,
		FUeAgentSimpleAuthoringProductionError& OutError)
	{
		OutProvider.Reset();
		OutError = FUeAgentSimpleAuthoringProductionError();
		if (!ValidateConfig(Config, OutError))
		{
			return false;
		}

		const TSharedRef<FProductionStore, ESPMode::ThreadSafe> Store =
			MakeShared<FProductionStore, ESPMode::ThreadSafe>();
		const TSharedRef<
			FProductionPlanner,
			ESPMode::ThreadSafe> Planner =
				MakeShared<
					FProductionPlanner,
					ESPMode::ThreadSafe>();
		const TSharedRef<
			FProductionDurablePort,
			ESPMode::ThreadSafe> Durable =
				MakeShared<
					FProductionDurablePort,
					ESPMode::ThreadSafe>(
					Store,
					Config.DurableTargetProjector.ToSharedRef(),
					Config.DurableService.ToSharedRef(),
					Config.AtomicityMode);
		const TSharedRef<
			FProductionExecutor,
			ESPMode::ThreadSafe> Executor =
				MakeShared<
					FProductionExecutor,
					ESPMode::ThreadSafe>(
					Config.FamilyId,
					Config.OwnerLeaseId,
					Config.MaxQueueItems,
					Store,
					Config.MutationBindingProjector
						.ToSharedRef(),
					Config.UeHook.ToSharedRef(),
					Durable);
		const TSharedRef<
			FProductionVerifier,
			ESPMode::ThreadSafe> Verifier =
				MakeShared<
					FProductionVerifier,
					ESPMode::ThreadSafe>(
					Store,
					Durable,
					Config.ExpectedStateProjector
						.ToSharedRef(),
					Config.PersistedStateReadPort
						.ToSharedRef(),
					Config.AbsenceOracle.ToSharedRef(),
					Config.ReferenceOracle.ToSharedRef(),
					Config.UeHook.ToSharedRef());
		OutProvider = MakeShared<
			FProductionProvider,
			ESPMode::ThreadSafe>(
			Config,
			Store,
			Planner,
			Executor,
			Verifier);
		return OutProvider.IsValid();
	}
}
