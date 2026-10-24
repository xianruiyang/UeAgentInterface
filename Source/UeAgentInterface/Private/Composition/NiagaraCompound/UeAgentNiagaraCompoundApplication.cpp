#include "Composition/NiagaraCompound/UeAgentNiagaraCompoundApplication.h"

#include "Core/Contract/UeAgentDiagnostic.h"

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
#include "Dom/JsonValue.h"
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

		FDiagnostic FailureWithFacts(
			const FString& Code, const FString& Reason,
			const FString& Stage, const FString& Field,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			FDiagnostic Error = FDiagnostic::Failure(Code, Reason, Stage);
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Field;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			Error.ActualValuePresent = true;
			Error.ExpectedValuePresent = true;
			return Error;
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
				if (Affinity != EExecutionThreadAffinity::GameThread)
				{
					OutError = FailureWithFacts(
						TEXT("niagara_compound_scheduler_thread_invalid"),
						TEXT("Niagara execution requires game-thread affinity"),
						TEXT("execution"), TEXT("thread_affinity"),
						MakeShared<FJsonValueNumber>(static_cast<int32>(Affinity)),
						MakeShared<FJsonValueNumber>(static_cast<int32>(EExecutionThreadAffinity::GameThread)));
					OutError.InternalContext = OperationId;
					return false;
				}
				if (!IsInGameThread() || !Callback)
				{
					const bool bOnGameThread = IsInGameThread();
					OutError = FailureWithFacts(
						TEXT("niagara_compound_scheduler_thread_invalid"),
						bOnGameThread ? TEXT("Niagara execution callback is missing")
							: TEXT("Niagara execution must run on the game thread"),
						TEXT("execution"), bOnGameThread ? TEXT("callback_present") : TEXT("on_game_thread"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					OutError.InternalContext = OperationId;
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
			FString& OutPackageName,
			FText& OutReason)
		{
			OutPackageName =
				FPackageName::ObjectPathToPackageName(ObjectPath);
			return FPackageName::IsValidLongPackageName(
				OutPackageName, false, &OutReason);
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
						OutError = FailureWithFacts(
							TEXT("niagara_compound_intent_unsupported"),
							TEXT("Niagara durable execution only supports update and replace"),
							TEXT("execution"), TEXT("intent"),
							MakeShared<FJsonValueString>(PlanningPreflight::LexToString(Change.GetIntent())),
							MakeShared<FJsonValueString>(TEXT("update or replace")));
						OutError.TargetPath = Change.GetIdentity().GetOwnerId();
						return false;
					}
					FString PackageName;
					FText PackageReason;
					if (!TryPackageName(
							Change.GetIdentity().GetOwnerId(),
							PackageName, PackageReason))
					{
						OutError = FailureWithFacts(
							TEXT("niagara_compound_package_invalid"),
							FString::Printf(TEXT("Niagara target package path is invalid: %s"), *PackageReason.ToString()),
							TEXT("execution"), TEXT("package_name"),
							MakeShared<FJsonValueString>(PackageName),
							MakeShared<FJsonValueString>(TEXT("valid long package name")));
						OutError.TargetPath = Change.GetIdentity().GetOwnerId();
						return false;
					}
					Targets.Emplace(
						Change.GetIdentity(),
						PackageName,
						EPackageHeaderKind::Asset,
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
				const bool bPrepared = TryPrepareAssetDurability(
					DurableRequest, Backend, JournalRoot, Resources, Session, Error);
				if (!bPrepared || !Session.IsValid())
				{
					OutError = MoveTemp(Error);
					if (OutError.IsEmpty())
					{
						OutError = FailureWithFacts(
							TEXT("niagara_compound_durable_prepare_failed"),
							bPrepared ? TEXT("Niagara durability preparation returned no session")
								: TEXT("Niagara durability preparation failed without a diagnostic"),
							TEXT("execution"), bPrepared ? TEXT("durable_session_present") : TEXT("durable_prepare_succeeded"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					}
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
				if (!TryPersistAssetDurabilityForVerification(
						Typed->Session,
						Backend,
						FencingToken,
						Resources,
						OutReceipt,
						Error))
				{
					OutError = Error;
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
					OutError = FailureWithFacts(
						TEXT("niagara_compound_durable_session_missing"),
						TEXT("No persisted Niagara durability session exists for this acknowledgement"),
						TEXT("asset_durable"), TEXT("persisted_session_present"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					OutError.InternalContext = Acknowledgement.GetJournalId();
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
					Durable)
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
				if (!IsInGameThread())
				{
					OutError = FailureWithFacts(
						TEXT("niagara_compound_executor_thread_invalid"),
						TEXT("Niagara mutation must execute on the game thread"),
						TEXT("execution"), TEXT("on_game_thread"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				if (!Planning.GetPlan().IsValid())
				{
					OutError = FailureWithFacts(
						TEXT("niagara_compound_executor_thread_invalid"),
						TEXT("Niagara mutation requires a prepared plan"),
						TEXT("execution"), TEXT("plan_present"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
					OutError = Error;
					return false;
				}
				Coordinator.DispatchReady();
				if (!Coordinator.GetReceipt(
						SubmittedId,
						OutReceipt,
						Error))
				{
					Store->Remove(OperationId);
					OutError = Error;
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
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (!Change.GetAfter().IsValid())
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("niagara_compound_expected_value_missing"),
					TEXT("The planned Niagara change has no resulting value for independent verification."), TEXT("verification"));
				OutError.TargetPath = Target.GetBinding().GetPublicRef();
				OutError.PropertyPath = TEXT("after");
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
					OutError = Error;
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
					OutError = Receipt.GetDiagnostic().IsEmpty() ? UE::UAI::Contract::FDiagnostic::Failure(
						TEXT("niagara_compound_execution_not_durable"), TEXT("The execution produced no durable receipt to verify."), TEXT("verification")) : Receipt.GetDiagnostic();
					if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Receipt.GetOperationId();
					return false;
				}
				if (!Store->Get(
						Receipt.GetOperationId(),
						Record))
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("niagara_compound_verification_context_missing"),
						TEXT("The Niagara execution record needed for independent verification is unavailable."), TEXT("verification"));
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
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("niagara_compound_durable_context_missing"),
						TEXT("The durable journal has no active artifact inventory and ownership context for verification."), TEXT("verification"));
					OutError.SubjectId =
						Receipt.GetOperationId();
					return false;
				}

				TArray<FVerificationTarget> VerificationTargets;
				const auto& Changes = Record.Plan->GetChangeSet().GetChanges();
				for (int32 ChangeIndex = 0; ChangeIndex < Changes.Num(); ++ChangeIndex)
				{
					const FPlannedChange& Change = Changes[ChangeIndex];
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
						OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("niagara_compound_verification_target_missing"),
							TEXT("A planned Niagara change has no bound target for independent verification."), TEXT("verification"));
						OutError.PropertyPath = FString::Printf(TEXT("changes[%d].target"), ChangeIndex);
						OutError.SubjectId = Change.GetIdentity().GetCanonicalKey();
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
							OutError))
					{
						if (!OutError.IsValid()) OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("niagara_compound_expected_snapshot_failed"), TEXT("The expected Niagara snapshot could not be produced."), TEXT("prepare"));
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
					OutError = Error;
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
		if (!Descriptor)
		{
			OutError = FailureWithFacts(
				TEXT("niagara_compound_application_config_invalid"),
				TEXT("Niagara operation descriptor is not registered"), TEXT("composition"), TEXT("operation_descriptor_present"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		if (Config.ServerSessionId.IsEmpty())
		{
			OutError = FailureWithFacts(
				TEXT("niagara_compound_application_config_invalid"),
				TEXT("Niagara composition requires a server session identity"), TEXT("composition"), TEXT("server_session_present"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		if (Config.RegistryEpoch < 1)
		{
			OutError = FailureWithFacts(
				TEXT("niagara_compound_application_config_invalid"),
				TEXT("Niagara composition requires a positive registry epoch"), TEXT("composition"), TEXT("registry_epoch"),
				MakeShared<FJsonValueString>(::LexToString(Config.RegistryEpoch)), MakeShared<FJsonValueString>(TEXT(">= 1")));
			return false;
		}
		if (Config.JournalRoot.IsEmpty())
		{
			OutError = FailureWithFacts(
				TEXT("niagara_compound_application_config_invalid"),
				TEXT("Niagara composition requires a durability journal root"), TEXT("composition"), TEXT("journal_root_present"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		if (ReadPort->GetPortId() != GetNiagaraCompoundReadPortId())
		{
			OutError = FailureWithFacts(
				TEXT("niagara_compound_application_config_invalid"),
				TEXT("Niagara composition requires the Niagara compound read port"), TEXT("composition"), TEXT("read_port_matches"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		if (!NativeHook->IsAvailable())
		{
			OutError = FailureWithFacts(
				TEXT("niagara_compound_application_config_invalid"),
				TEXT("Niagara native hook has no available generation"), TEXT("composition"), TEXT("native_hook_generation_present"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
			OutError = MoveTemp(ContextError);
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
			OutError = MoveTemp(ProjectionError);
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
			OutError = MoveTemp(BindingError);
			return false;
		}
		OutApplication = MakeShareable(
			new FUeAgentNiagaraCompoundApplication(
				MakeShared<FState, ESPMode::ThreadSafe>(
					Binding.ToSharedRef())));
		return true;
	}
}
