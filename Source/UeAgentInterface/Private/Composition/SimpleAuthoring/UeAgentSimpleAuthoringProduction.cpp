#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringProduction.h"

#include "Dom/JsonValue.h"
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

		FString CandidateOperationName(const EUeAgentCandidateOperation Operation)
		{
			switch (Operation)
			{
			case EUeAgentCandidateOperation::Read: return TEXT("read");
			case EUeAgentCandidateOperation::Create: return TEXT("create");
			case EUeAgentCandidateOperation::Update: return TEXT("update");
			case EUeAgentCandidateOperation::Replace: return TEXT("replace");
			case EUeAgentCandidateOperation::Delete: return TEXT("delete");
			default: return FString::FromInt(static_cast<int32>(Operation));
			}
		}

		FDiagnostic ProductionFailure(const FString& Code, const FString& Reason, const FString& Field)
		{
			auto Diagnostic = FDiagnostic::Failure(Code, Reason, TEXT("simple_authoring"));
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.PropertyPath = Field;
			return Diagnostic;
		}

		FDiagnostic DescriptorMismatch(const FString& Code,
			const FUeAgentDescriptorIdentity& Actual, const FUeAgentDescriptorIdentity& Expected)
		{
			if (Actual.GetDescriptorId() != Expected.GetDescriptorId())
			{
				auto Error = ProductionFailure(Code, TEXT("The returned descriptor ID differs from the requested descriptor."), TEXT("descriptor/id"));
				Error.ActualValue = MakeShared<FJsonValueString>(Actual.GetDescriptorId());
				Error.ExpectedValue = MakeShared<FJsonValueString>(Expected.GetDescriptorId());
				return Error;
			}
			if (Actual.GetVersion() != Expected.GetVersion())
			{
				auto Error = ProductionFailure(Code, TEXT("The returned descriptor version differs from the requested version."), TEXT("descriptor/version"));
				Error.ActualValue = MakeShared<FJsonValueNumber>(Actual.GetVersion());
				Error.ExpectedValue = MakeShared<FJsonValueNumber>(Expected.GetVersion());
				return Error;
			}
			auto Error = ProductionFailure(Code, TEXT("The returned descriptor hash differs from the requested descriptor hash."), TEXT("descriptor/hash"));
			Error.ActualEvidence = MakeShared<FJsonValueString>(Actual.GetDescriptorHash());
			Error.ExpectedEvidence = MakeShared<FJsonValueString>(Expected.GetDescriptorHash());
			return Error;
		}

		void CopyError(const UE::UAI::Contract::FDiagnostic& Error, UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutError = Error;
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

		FUeAgentOperationBindingError CompareChangeTargets(
			const TArray<FUeAgentOperationTargetBinding>& Bound,
			const TArray<FUeAgentCandidateBinding>& Prepared,
			const FString& Code)
		{
			if (Bound.Num() != Prepared.Num())
			{
				auto Error = FUeAgentOperationBindingError::Failure(Code,
					TEXT("The bound target count differs from the prepared target count."), TEXT("simple_authoring"));
				Error.PropertyPath = TEXT("targets");
				Error.ActualValue = MakeShared<FJsonValueNumber>(Bound.Num());
				Error.ExpectedValue = MakeShared<FJsonValueNumber>(Prepared.Num());
				return Error;
			}
			for (int32 Index = 0; Index < Prepared.Num(); ++Index)
			{
				const FUeAgentCandidateBinding& Candidate = Prepared[Index];
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
					const auto* SameBinding = Bound.FindByPredicate([&Candidate](const FUeAgentOperationTargetBinding& Target)
						{ return Target.GetBinding().GetBindingId() == Candidate.GetBindingId(); });
					auto Error = FUeAgentOperationBindingError::Failure(Code, SameBinding
						? TEXT("A bound target identifies a different object from the prepared target.")
						: TEXT("A prepared target binding is missing from the bound targets."), TEXT("simple_authoring"));
					Error.PropertyPath = FString::Printf(TEXT("targets[%d]"), Index);
					Error.TargetPath = Candidate.GetPublicRef();
					Error.ExpectedEvidence = MakeShared<FJsonValueString>(Candidate.GetSummary().GetIdentity().GetCanonicalKey());
					if (SameBinding) Error.ActualEvidence = MakeShared<FJsonValueString>(SameBinding->GetBinding().GetSummary().GetIdentity().GetCanonicalKey());
					else Error.ActualValuePresent = false;
					return Error;
				}
			}
			return {};
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
			if (Changes.IsEmpty() || Targets.Num() != Changes.Num())
			{
				OutError = ProductionFailure(TEXT("simple_authoring_change_targets_missing"),
					Changes.IsEmpty() ? TEXT("There are no planned changes to associate with target bindings.")
						: TEXT("The number of target bindings differs from the number of planned changes."),
					Changes.IsEmpty() ? TEXT("changes") : TEXT("targets"));
				OutError.SubjectId = Draft.GetRequestId();
				OutError.ActualValue = MakeShared<FJsonValueNumber>(Changes.IsEmpty() ? Changes.Num() : Targets.Num());
				OutError.ExpectedValue = Changes.IsEmpty()
					? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("at least one planned change")))
					: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Changes.Num()));
				return false;
			}
			TSet<FString> SeenIdentities;
			for (int32 ChangeIndex = 0; ChangeIndex < Changes.Num(); ++ChangeIndex)
			{
				const FChangeDraft& Change = Changes[ChangeIndex];
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
				if (!Target)
				{
					OutError = ProductionFailure(TEXT("simple_authoring_change_target_invalid"),
						TEXT("The planned change has no matching target binding."),
						FString::Printf(TEXT("changes/%d/target"), ChangeIndex));
					OutError.SubjectId = IdentityKey;
					OutError.ActualValuePresent = false;
					OutError.ExpectedValuePresent = true;
					return false;
				}
				if (!Target->IsValid())
				{
					OutError = *Target->GetValidationDiagnostic();
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target->GetPublicRef();
					return false;
				}
				if (SeenIdentities.Contains(IdentityKey))
				{
					OutError = ProductionFailure(TEXT("simple_authoring_change_target_invalid"),
						TEXT("More than one planned change uses the same target identity."),
						FString::Printf(TEXT("changes/%d/target"), ChangeIndex));
					OutError.TargetPath = Target->GetPublicRef();
					OutError.SubjectId = IdentityKey;
					OutError.ActualValue = MakeShared<FJsonValueNumber>(2);
					OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1);
					return false;
				}
				if (!(Target->GetSummary().GetDescriptorIdentity() == DescriptorIdentity))
				{
					OutError = DescriptorMismatch(TEXT("simple_authoring_change_target_invalid"),
						Target->GetSummary().GetDescriptorIdentity(), DescriptorIdentity);
					OutError.TargetPath = Target->GetPublicRef();
					return false;
				}
				if (Target->GetReadPortId() != PersistedPortId)
				{
					OutError = ProductionFailure(TEXT("simple_authoring_change_target_invalid"),
						TEXT("The change target is bound to a different persisted-state read port."), TEXT("read_port_id"));
					OutError.TargetPath = Target->GetPublicRef();
					OutError.ActualValue = MakeShared<FJsonValueString>(Target->GetReadPortId());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(PersistedPortId);
					return false;
				}
				const EUeAgentCandidateOperation RequiredOperation = CandidateOperation(Change.GetIntent());
				if (!Target->Supports(RequiredOperation))
				{
					OutError = ProductionFailure(TEXT("simple_authoring_change_target_invalid"),
						TEXT("The change target does not support the required candidate operation."), TEXT("supported_operations"));
					OutError.TargetPath = Target->GetPublicRef();
					TArray<TSharedPtr<FJsonValue>> Supported;
					for (const EUeAgentCandidateOperation Operation : Target->GetSupportedOperations())
						Supported.Add(MakeShared<FJsonValueString>(CandidateOperationName(Operation)));
					OutError.ActualValue = MakeShared<FJsonValueArray>(Supported);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(CandidateOperationName(RequiredOperation));
					return false;
				}
				SeenIdentities.Add(IdentityKey);
			}
			return true;
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
					OutError = Error;
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
					OutError = FExecutionMutationError::Failure(TEXT(
							"simple_authoring_execution_thread_invalid"), FString(), TEXT("execution"), OperationId);
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
					OutError = FExecutionMutationError::Failure(TEXT("simple_authoring_execution_record_missing"),
						TEXT("No prepared execution record is available for durable preparation."), TEXT("execution"));
					OutError.SourceKind = TEXT("native_result");
					OutError.SubjectId = Request.GetOperationId();
					OutError.PropertyPath = TEXT("execution_record");
					OutError.ActualValuePresent = false;
					OutError.ExpectedValuePresent = true;
					return false;
				}

				TArray<FAssetDurableTarget> Targets;
				FUeAgentSimpleAuthoringProductionError ProjectionError;
				const bool bProjected = Projector->TryProjectDurableTargets(
					Request.GetPlan(),
					*Record.ResolvedTarget,
					Record.PreState.ToSharedRef(),
					Resources,
					Targets,
					ProjectionError);
				if (!bProjected || Targets.IsEmpty())
				{
					OutError = ProjectionError;
					if (OutError.IsEmpty())
					{
						OutError = FExecutionMutationError::Failure(TEXT("simple_authoring_durable_targets_missing"),
							bProjected ? TEXT("The durable target projector returned no targets.")
								: TEXT("Durable target projection failed without reporting a diagnostic."), TEXT("simple_authoring"));
						OutError.SourceKind = TEXT("native_result");
						OutError.PropertyPath = TEXT("durable_targets");
						OutError.ActualValue = MakeShared<FJsonValueNumber>(Targets.Num());
						OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("successful projection with at least one target"));
						OutError.SubjectId = Request.GetOperationId();
					}
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
				if (!DurableRequest.IsValid())
				{
					OutError = DurableRequest.GetValidationDiagnostic();
					return false;
				}
				if (!Service->TryPrepare(DurableRequest, Resources, Session, Error))
				{
					OutError = Error;
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
				const FMutationFacts& Facts,
				FUeAgentResourceTracker& Resources,
				FDurableReceipt& OutReceipt,
				FExecutionMutationError& OutError) override
			{
				const TSharedRef<FProductionDurableSession> Typed =
					StaticCastSharedRef<
						FProductionDurableSession>(Session);
				FAssetDurableError Error;
				FAssetDurableSessionPtr Enlisted;
				if (!Service->TryEnlist(
						Typed->Session,
						Facts.GetDurableTargets(),
						FencingToken,
						Resources,
						Enlisted,
						Error))
				{
					OutError = Error;
					return false;
				}
				Typed->Session = Enlisted.ToSharedRef();
				Error = FAssetDurableError();
				if (!Service->TryPersist(
					Typed->Session,
					FencingToken,
					Resources,
					OutReceipt,
					Error))
				{
					OutError = Error;
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
					OutError = Error;
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
					OutError = FAssetDurableError::Failure(TEXT(
							"simple_authoring_durable_session_missing"), FString(), TEXT("asset_durable"), Acknowledgement.GetJournalId());
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
					Durable)
			{
			}

			bool TryExecute(
				const FPlanningResult& Planning,
				const TArray<FUeAgentOperationTargetBinding>& Targets,
				const FUeAgentOperationContext& Context,
				FExecutionReceipt& OutReceipt,
				FUeAgentOperationBindingError& OutError) override
			{
				if (!IsInGameThread())
				{
					OutError = FDiagnostic::Failure(TEXT("simple_authoring_execution_not_ready"),
						TEXT("Authoring execution must run on the game thread."), TEXT("simple_authoring"));
					OutError.SourceKind = TEXT("native_result");
					OutError.CandidateName = FamilyId;
					OutError.PropertyPath = TEXT("game_thread");
					OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
					OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					return false;
				}
				if (!Planning.IsReady())
				{
					OutError = FDiagnostic::Failure(TEXT("simple_authoring_execution_not_ready"),
						Planning.GetPlan().IsValid() ? TEXT("The planning result is not ready for authoring execution.")
							: TEXT("The planning result contains no execution plan."), TEXT("simple_authoring"));
					OutError.SourceKind = TEXT("native_result");
					OutError.CandidateName = FamilyId;
					if (Planning.GetPlan().IsValid())
					{
						OutError.PropertyPath = TEXT("planning/status");
						OutError.ActualValue = MakeShared<FJsonValueString>(Planning.GetStatus());
						OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("ready or no_change"));
					}
					else
					{
						OutError.PropertyPath = TEXT("planning/plan");
						OutError.ActualValuePresent = false;
						OutError.ExpectedValuePresent = true;
					}
					return false;
				}
				const FPlanningPreflightPlanRef Plan =
					Planning.GetPlan().ToSharedRef();
				FPreparationRecord Preparation;
				if (!Store->GetPreparation(
					Plan->GetRequestId(),
					Preparation))
				{
					OutError = FDiagnostic::Failure(TEXT("simple_authoring_preparation_mismatch"),
						TEXT("The prepared operation record is unavailable."), TEXT("simple_authoring"));
					OutError.SourceKind = TEXT("native_result");
					OutError.SubjectId = Plan->GetRequestId();
					OutError.CandidateName = FamilyId;
					OutError.PropertyPath = TEXT("preparation_record");
					OutError.ActualValuePresent = false;
					OutError.ExpectedValuePresent = true;
					return false;
				}
				OutError = CompareChangeTargets(Targets, Preparation.ChangeTargets, TEXT("simple_authoring_preparation_mismatch"));
				if (!OutError.IsEmpty()) return false;

				TArray<FUeAgentGenericEffectBinding> Bindings;
				FUeAgentSimpleAuthoringProductionError ProjectionError;
				const bool bProjected = Projector->TryProjectMutationBindings(
					Plan,
					Preparation.ChangeTargets,
					Preparation.PreState.ToSharedRef(),
					Context.GetExecutionContext(),
					Context.GetResourceTracker(),
					Bindings,
					ProjectionError);
				if (!bProjected || Bindings.IsEmpty())
				{
					OutError = ProjectionError;
					if (OutError.IsEmpty())
					{
						OutError = FDiagnostic::Failure(TEXT("simple_authoring_mutation_bindings_missing"),
							bProjected ? TEXT("The mutation projector returned no bindings.")
								: TEXT("Mutation binding projection failed without reporting a diagnostic."), TEXT("simple_authoring"));
						OutError.SourceKind = TEXT("native_result");
						OutError.CandidateName = FamilyId;
						OutError.PropertyPath = TEXT("mutation_bindings");
						OutError.ActualValue = MakeShared<FJsonValueNumber>(Bindings.Num());
						OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("successful projection with at least one binding"));
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
					OutError = Error;
					return false;
				}
				Coordinator.DispatchReady();
				if (!Coordinator.GetReceipt(
					SubmittedId,
					OutReceipt,
					Error))
				{
					Store->RemoveExecution(OperationId);
					OutError = Error;
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
					IPersistedStateOracle,
					ESPMode::ThreadSafe> InPersistedStateOracle,
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
				, PersistedStateOracle(
					MoveTemp(InPersistedStateOracle))
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
				if (!DurableReceipt)
				{
					OutError = Receipt.GetDiagnostic().IsEmpty() ? UE::UAI::Contract::FDiagnostic::Failure(
						TEXT("simple_authoring_verification_context_missing"), TEXT("The execution produced no durable receipt to verify."), TEXT("simple_authoring")) : Receipt.GetDiagnostic();
					if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Receipt.GetOperationId();
					return false;
				}
				if (!Store->GetExecution(Receipt.GetOperationId(), Record))
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("simple_authoring_verification_context_missing"),
						TEXT("The execution record needed for independent verification is unavailable."), TEXT("simple_authoring"));
					OutError.SubjectId = Receipt.GetOperationId();
					return false;
				}
				OutError = CompareChangeTargets(Targets, Record.ChangeTargets, TEXT("simple_authoring_verification_context_missing"));
				if (!OutError.IsEmpty()) return false;

				FDurableInventory Inventory;
				FString OwnerId;
				int64 FencingToken = -1;
				if (!Durable->TryGetVerificationContext(
					DurableReceipt->GetJournalId(),
					Inventory,
					OwnerId,
					FencingToken))
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT(
							"simple_authoring_durable_context_missing"), TEXT("The durable journal has no active artifact inventory and ownership context for verification."), TEXT("simple_authoring"));
					OutError.SubjectId =
						DurableReceipt->GetJournalId();
					return false;
				}

				TArray<FVerificationTarget> VerificationTargets;
				FUeAgentSimpleAuthoringProductionError ProjectionError;
				const bool bProjected = ExpectedStateProjector
					->TryProjectExpectedState(
						Record.Plan.ToSharedRef(),
						*Record.ResolvedTarget,
						Record.ChangeTargets,
						Record.PreState.ToSharedRef(),
						Inventory,
						Context.GetResourceTracker(),
						VerificationTargets,
						ProjectionError);
				if (!bProjected || VerificationTargets.IsEmpty())
				{
					CopyError(ProjectionError, OutError);
					if (OutError.Code.IsEmpty())
					{
						OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT(
								"simple_authoring_expected_state_missing"), bProjected
								? TEXT("The expected-state projector returned no verification targets.")
								: TEXT("The expected-state projector failed without reporting a diagnostic."), TEXT("simple_authoring"));
					}
					return false;
				}

				FVerificationService Service(
					*PersistedStateOracle,
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
					Error, true);
				Store->RemoveExecution(Receipt.GetOperationId());
				UeHook->ReleaseVerifiedOperation(
					Receipt.GetOperationId());
				if (!bVerified)
				{
					OutError = Error;
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
				IPersistedStateOracle,
				ESPMode::ThreadSafe> PersistedStateOracle;
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
				const bool bResolved = Config.TargetResolver->TryResolveTarget(
					CanonicalRequest, Context, Resources, Deadline, Cancellation, Target, Error);
				if (!bResolved || !Target.IsSet() || !Target->IsValid())
				{
					OutError = Error;
					if (OutError.IsEmpty() && Target.IsSet() && !Target->IsValid())
						OutError = *Target->GetValidationDiagnostic();
					if (OutError.IsEmpty())
					{
						OutError = ProductionFailure(TEXT("simple_authoring_target_resolution_failed"),
							bResolved ? TEXT("The target resolver reported success without returning a binding.")
								: TEXT("The target resolver returned failure without reporting a diagnostic."), TEXT("target_binding"));
						OutError.ActualValuePresent = Target.IsSet();
						OutError.ExpectedValuePresent = true;
						OutError.CandidateName = CanonicalRequest.GetDescriptorIdentity().GetDescriptorId();
					}
					return false;
				}
				if (!(Target->GetSummary().GetDescriptorIdentity() == CanonicalRequest.GetDescriptorIdentity()))
				{
					OutError = DescriptorMismatch(TEXT("simple_authoring_target_binding_mismatch"),
						Target->GetSummary().GetDescriptorIdentity(), CanonicalRequest.GetDescriptorIdentity());
					OutError.TargetPath = Target->GetPublicRef();
					OutError.SubjectId = Target->GetBindingId();
					return false;
				}
				if (Target->GetReadPortId() != Config.PersistedStateOracle->GetReadPortId())
				{
					OutError = ProductionFailure(TEXT("simple_authoring_target_binding_mismatch"),
						TEXT("The resolved target uses a different persisted-state read port."), TEXT("read_port_id"));
					OutError.TargetPath = Target->GetPublicRef();
					OutError.SubjectId = Target->GetBindingId();
					OutError.ActualValue = MakeShared<FJsonValueString>(Target->GetReadPortId());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(Config.PersistedStateOracle->GetReadPortId());
					return false;
				}

				FUeAgentCanonicalSnapshotPtr PreState;
				FUeAgentDiscoveryError ReadError;
				const bool bRead = Config.PreStateReadPort->TryRead(
					FUeAgentStateReadRequest(*Target, Config.PreStatePlane),
					Resources, Deadline, Cancellation, PreState, ReadError);
				if (!bRead || !PreState.IsValid())
				{
					OutError = ReadError;
					if (OutError.IsEmpty())
					{
						OutError = ProductionFailure(TEXT("simple_authoring_pre_state_unavailable"),
							bRead ? TEXT("The pre-state read port reported success without returning a snapshot.")
								: TEXT("The pre-state read port failed without reporting a diagnostic."), TEXT("pre_state"));
						OutError.ActualValuePresent = PreState.IsValid();
						OutError.ExpectedValuePresent = true;
					}
					if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Target->GetBindingId();
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target->GetPublicRef();
					return false;
				}

				TOptional<FChangeSetDraft> Draft;
				const bool bBuilt = Config.DraftBuilder->TryBuildDraft(
					CanonicalRequest, Context, PreState.ToSharedRef(), Resources, Draft, Error);
				if (!bBuilt || !Draft.IsSet() || !Draft->IsValid())
				{
					OutError = Error;
					if (OutError.IsEmpty() && Draft.IsSet() && !Draft->IsValid())
						OutError = Draft->GetValidationDiagnostic();
					if (OutError.IsEmpty())
					{
						OutError = ProductionFailure(TEXT("simple_authoring_draft_invalid"),
							bBuilt ? TEXT("The draft builder reported success without returning a change-set draft.")
								: TEXT("The draft builder returned failure without reporting a diagnostic."), TEXT("draft"));
						OutError.ActualValuePresent = Draft.IsSet();
						OutError.ExpectedValuePresent = true;
					}
					if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = CanonicalRequest.GetRequestId();
					return false;
				}
				if (!(Draft->GetDescriptorIdentity() == CanonicalRequest.GetDescriptorIdentity()))
				{
					OutError = DescriptorMismatch(TEXT("simple_authoring_draft_invalid"),
						Draft->GetDescriptorIdentity(), CanonicalRequest.GetDescriptorIdentity());
					OutError.SubjectId = CanonicalRequest.GetRequestId();
					return false;
				}
				const auto MatchesOpaque = [&](const FString& Field, const FString& Actual,
					const FString& Expected, const FString& Reason)
				{
					if (Actual == Expected) return true;
					OutError = ProductionFailure(TEXT("simple_authoring_draft_invalid"), Reason, Field);
					OutError.SubjectId = CanonicalRequest.GetRequestId();
					OutError.ActualEvidence = MakeShared<FJsonValueString>(Actual);
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Expected);
					return false;
				};
				if (!MatchesOpaque(TEXT("request_id"), Draft->GetRequestId(), CanonicalRequest.GetRequestId(),
					TEXT("The change-set draft belongs to a different request."))
					|| !MatchesOpaque(TEXT("idempotency_key"), Draft->GetIdempotencyKey(), CanonicalRequest.GetIdempotencyKey(),
						TEXT("The change-set draft has a different idempotency key from the request."))
					|| !MatchesOpaque(TEXT("snapshot_id"), Draft->GetSnapshot()->GetSnapshotId(), PreState->GetSnapshotId(),
						TEXT("The change-set draft references a different pre-state snapshot.")))
					return false;

				TArray<FUeAgentCandidateBinding> ChangeTargets;
				Error =
					FUeAgentSimpleAuthoringProductionError();
				if (!Config.ChangeTargetProjector->TryProjectChangeTargets(
					CanonicalRequest, Context, Draft.GetValue(), *Target, PreState.ToSharedRef(),
					Resources, ChangeTargets, Error))
				{
					OutError = Error;
					if (OutError.IsEmpty())
					{
						OutError = ProductionFailure(TEXT("simple_authoring_change_targets_missing"),
							TEXT("The change-target projector returned failure without reporting a diagnostic."), TEXT("change_targets"));
						OutError.TargetPath = Target->GetPublicRef();
						OutError.ActualValue = MakeShared<FJsonValueNumber>(ChangeTargets.Num());
					}
					return false;
				}
				if (!ValidateChangeTargets(Draft.GetValue(), CanonicalRequest.GetDescriptorIdentity(),
					Config.PersistedStateOracle->GetReadPortId(), ChangeTargets, Error))
				{
					OutError = Error;
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
					OutError = OutPrepared->GetValidationDiagnostic();
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
			const FUeAgentSimpleAuthoringProductionProviderConfig& Config,
			FUeAgentSimpleAuthoringProductionError& OutError)
		{
			const auto Missing = [&](const FString& Code, const FString& Field,
				TSharedPtr<FJsonValue> Actual = nullptr)
			{
				const FString Reason = Actual.IsValid()
					? FString::Printf(TEXT("The provider configuration field '%s' must not be empty."), *Field)
					: FString::Printf(TEXT("The authoring provider has no '%s'."), *Field);
				OutError = ProductionFailure(Code, Reason, Field);
				OutError.CandidateName = Config.FamilyId;
				OutError.ActualValuePresent = Actual.IsValid();
				OutError.ActualValue = MoveTemp(Actual);
				OutError.ExpectedValuePresent = true;
				return false;
			};
			if (Config.FamilyId.IsEmpty())
				return Missing(TEXT("simple_authoring_production_family_missing"), TEXT("family_id"),
					MakeShared<FJsonValueString>(Config.FamilyId));
			if (Config.OwnerLeaseId.IsEmpty())
				return Missing(TEXT("simple_authoring_production_owner_missing"), TEXT("owner_lease_id"),
					MakeShared<FJsonValueString>(Config.OwnerLeaseId));
			if (!Config.Projection.IsSet())
				return Missing(TEXT("simple_authoring_production_projection_missing"), TEXT("projection"));
			if (!Config.Projection->IsValid())
			{
				OutError = Config.Projection->GetValidationDiagnostic();
				return false;
			}
			if (!Config.TargetResolver.IsValid())
				return Missing(TEXT("simple_authoring_target_resolver_missing"), TEXT("target_resolver"));
			if (!Config.PreStateReadPort.IsValid())
				return Missing(TEXT("simple_authoring_pre_state_port_missing"), TEXT("pre_state_read_port"));
			if (!Config.ChangeTargetProjector.IsValid())
				return Missing(TEXT("simple_authoring_change_target_projector_missing"), TEXT("change_target_projector"));
			if (!Config.DraftBuilder.IsValid())
				return Missing(TEXT("simple_authoring_draft_builder_missing"), TEXT("draft_builder"));
			if (!Config.MutationBindingProjector.IsValid() || !Config.UeHook.IsValid())
				return Missing(TEXT("simple_authoring_mutation_port_missing"),
					!Config.MutationBindingProjector.IsValid() ? TEXT("mutation_binding_projector") : TEXT("ue_hook"));
			if (!Config.DurableTargetProjector.IsValid() || !Config.DurableService.IsValid())
				return Missing(TEXT("simple_authoring_durable_port_missing"),
					!Config.DurableTargetProjector.IsValid() ? TEXT("durable_target_projector") : TEXT("durable_service"));
			if (!Config.ExpectedStateProjector.IsValid() || !Config.PersistedStateOracle.IsValid()
				|| !Config.AbsenceOracle.IsValid() || !Config.ReferenceOracle.IsValid())
				return Missing(TEXT("simple_authoring_verification_port_missing"),
					!Config.ExpectedStateProjector.IsValid() ? TEXT("expected_state_projector")
						: !Config.PersistedStateOracle.IsValid() ? TEXT("persisted_state_oracle")
							: !Config.AbsenceOracle.IsValid() ? TEXT("absence_oracle") : TEXT("reference_oracle"));
			const FString& PreStatePortId = Config.PreStateReadPort->GetPortId();
			if (PreStatePortId.IsEmpty())
				return Missing(TEXT("simple_authoring_c9_read_port_not_independent"), TEXT("pre_state_read_port/id"),
					MakeShared<FJsonValueString>(PreStatePortId));
			const FString& PersistedPortId = Config.PersistedStateOracle->GetReadPortId();
			if (PersistedPortId.IsEmpty())
				return Missing(TEXT("simple_authoring_c9_read_port_not_independent"), TEXT("persisted_state_oracle/read_port_id"),
					MakeShared<FJsonValueString>(PersistedPortId));
			if (PreStatePortId == PersistedPortId)
			{
				OutError = ProductionFailure(TEXT("simple_authoring_c9_read_port_not_independent"),
					TEXT("The persisted-state oracle uses the same read port as the pre-state reader."), TEXT("persisted_state_oracle/read_port_id"));
				OutError.CandidateName = Config.FamilyId;
				OutError.ActualValue = MakeShared<FJsonValueString>(PersistedPortId);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("a read port different from the pre-state reader"));
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
					Config.PersistedStateOracle
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
