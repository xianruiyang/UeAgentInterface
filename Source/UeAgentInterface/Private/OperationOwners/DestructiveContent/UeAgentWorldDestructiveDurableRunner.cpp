#include "OperationOwners/DestructiveContent/UeAgentWorldDestructiveDurableRunner.h"
#include "OperationOwners/DestructiveContent/UeAgentWorldDestructiveCheckpoint.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/AssetDurable/UeAgentPersistedWorld.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Dom/JsonValue.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeLock.h"
#include "UObject/Package.h"

namespace UE::UAI::OperationOwners::DestructiveContent
{
	using namespace AssetDurable;
	using namespace ContextResource;
	using namespace Contract;
	using namespace DomainAdapters::WorldDestructive;
	using namespace ExecutionMutation;
	using namespace PlanningPreflight;
	using namespace StateSemantic;

	namespace
	{
		FDiagnostic NativeValueFailure(const TCHAR* Code, const FString& Reason,
			const TCHAR* Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& Stage = TEXT("execution"), const FString& Target = FString())
		{
			auto Diagnostic = FDiagnostic::Failure(Code, Reason, Stage);
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.PropertyPath = Field;
			Diagnostic.ActualValue = MoveTemp(Actual);
			Diagnostic.ExpectedValue = MoveTemp(Expected);
			Diagnostic.TargetPath = Target;
			return Diagnostic;
		}

		FDiagnostic NativeConditionFailure(const TCHAR* Code, const FString& Reason,
			const TCHAR* Condition, const bool bActual, const bool bExpected,
			const FString& Stage = TEXT("execution"), const FString& Target = FString())
		{
			return NativeValueFailure(Code, Reason, Condition, MakeShared<FJsonValueBoolean>(bActual),
				MakeShared<FJsonValueBoolean>(bExpected), Stage, Target);
		}

		FDiagnostic PreserveFailure(FDiagnostic Diagnostic, const TCHAR* Code, const FString& Reason,
			const TCHAR* Condition, const FString& Stage = TEXT("execution"), const FString& Target = FString())
		{
			if (Diagnostic.IsEmpty()) Diagnostic = NativeConditionFailure(Code, Reason, Condition, false, true, Stage, Target);
			return Diagnostic;
		}

		FDiagnostic InvalidFenceFailure(const FString& Stage, const TCHAR* Code = TEXT("world_destructive_operation_fence_invalid"))
		{
			return NativeConditionFailure(Code,
				TEXT("The operation fencing token must be non-negative."), TEXT("operation_fence_valid"), false, true, Stage);
		}

		FUeAgentResourceTrackerOwner MakeResources()
		{
			return MakeShared<
				FUeAgentResourceTracker,
				ESPMode::ThreadSafe>();
		}

		FUeAgentCanonicalIdentity MakePackageIdentity(
			const FString& PackageName,
			const bool bMap)
		{
			return FUeAgentCanonicalIdentity(
				bMap
					? TEXT("uai.world-destructive.map.state")
					: TEXT(
						"uai.world-destructive.external-package.state"),
				bMap ? TEXT("world_map")
					: TEXT("world_external_actor_package"),
				PackageName,
				TEXT("package"),
				bMap ? TEXT("map") : TEXT("external_actor_package"),
				TEXT("world-destructive-v2"));
		}

		FPlanningPreflightPlanRef MakePlan(
			const FUeAgentWorldDestructiveRequest& Request,
			const FString& OwnerId,
			const TArray<FWorldDestructivePackage>& Packages)
		{
			TArray<FPlannedChange> Changes;
			TArray<FImpact> Impacts;
			FString PackageFingerprint;
			for (const FWorldDestructivePackage& Package : Packages)
			{
				PackageFingerprint += FString::Printf(TEXT("\n%s\t%d\t%d\t%s"), *Package.PackageName,
					static_cast<int32>(Package.HeaderKind), static_cast<int32>(Package.Intent), *Package.WorldPackageName);
				const auto Identity = MakePackageIdentity(Package.PackageName, Package.HeaderKind == EPackageHeaderKind::Map);
				Changes.Emplace(
					Identity,
					Package.Intent == EAssetDurableIntent::Delete ? EChangeIntent::Delete : EChangeIntent::Update,
					FUeAgentCanonicalValuePtr(),
					FUeAgentCanonicalValuePtr(),
					TArray<FString>{TEXT("uai:asset:write")},
					TEXT("world-destructive:durable-runner"));
				Impacts.Emplace(
					Identity,
					Identity,
					TEXT("direct"),
					TEXT("world-destructive:durable-runner"));
			}
			return MakeShared<FPlanningPreflightPlan>(
				FUeAgentDescriptorIdentity(
					TEXT("uai.world.destructive.durable/v1"),
					1,
					Json::Sha256Utf8(
						TEXT("uai.world.destructive.durable/v1"))),
				Request.RequestId,
				TEXT("world-destructive:durable:") + Request.RequestId,
				Request.CallerId,
				TArray<FString>{TEXT("uai:asset:write")},
				TEXT("world-destructive:preflight:") + Request.RequestId,
				FUeAgentStateRevision(
					OwnerId,
					TEXT("world-destructive-v2"),
					1),
				TEXT("world-destructive:preflight:native"),
				FChangeSet(MoveTemp(Changes)),
				FImpactSet(MoveTemp(Impacts)),
				TArray<FPrecondition>(),
				Json::Sha256Utf8(
					Request.CommandId + TEXT(":") + Request.RequestId
					+ TEXT(":") + Request.TargetRef + TEXT(":") + Request.ComponentName
					+ PackageFingerprint));
		}

		bool TryMakeExecutionContext(
			UWorld& World,
			const FPlanningPreflightPlanRef& Plan,
			FUeAgentExecutionContextRefPtr& OutContext,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			FUeAgentExecutionContextInput Input;
			Input.ProjectInstanceId = FString(FApp::GetProjectName());
			Input.EditorInstanceId = TEXT("uai-editor:")
				+ Input.ProjectInstanceId;
			Input.StatePlane = EUeAgentStatePlane::Persisted;
			Input.WorldType = EUeAgentExecutionWorldType::Editor;
			Input.WorldIdentity = World.GetPathName();
			Input.LevelIdentity = World.PersistentLevel
				? World.PersistentLevel->GetPathName()
				: FString();
			Input.ContextRevision = Json::Sha256Utf8(
				Input.WorldIdentity + TEXT(":")
				+ Plan->GetRequestId());

			FUeAgentExecutionContextRefPtr Context;
			FUeAgentExecutionContextError Error;
			if (!FUeAgentExecutionContextRef::TryCreate(
					Input,
					Plan->GetDescriptorIdentity(),
					EUeAgentRequestKind::Operation,
					FUeAgentExecutionContextPolicy(
						{EUeAgentStatePlane::Persisted},
						{EUeAgentExecutionContextFacet::World},
						EUeAgentContextResolutionPolicy::ExplicitOnly,
						EUeAgentTransientContextPolicy::
							TemporaryMutationAllowed),
					Context,
					Error)
				|| !Context.IsValid())
			{
				OutError = PreserveFailure(MoveTemp(Error), TEXT("world_destructive_execution_context_invalid"),
					TEXT("The world execution context was not created and no diagnostic was provided."), TEXT("execution_context_ready"));
				return false;
			}
			OutContext = MoveTemp(Context);
			return true;
		}

		class FInlineGameThreadScheduler final
			: public IExecutionScheduler
		{
		public:
			bool Schedule(
				const EExecutionThreadAffinity Affinity,
				const FString& OperationId,
				TFunction<void()> Callback,
				FExecutionMutationError& OutError) override
			{
				if (Affinity != EExecutionThreadAffinity::GameThread)
				{
					OutError = NativeValueFailure(TEXT("world_destructive_execution_thread_invalid"),
						TEXT("World destructive execution requires game-thread affinity."), TEXT("thread_affinity"),
						MakeShared<FJsonValueString>(LexToString(Affinity)), MakeShared<FJsonValueString>(TEXT("game_thread")));
					OutError.SubjectId = OperationId;
					return false;
				}
				if (!IsInGameThread())
				{
					OutError = NativeConditionFailure(TEXT("world_destructive_execution_thread_invalid"),
						TEXT("World destructive execution must run on the game thread."), TEXT("game_thread"), false, true);
					OutError.SubjectId = OperationId;
					return false;
				}
				if (!Callback)
				{
					OutError = NativeConditionFailure(TEXT("world_destructive_execution_thread_invalid"),
						TEXT("World destructive execution requires a callback."), TEXT("callback_available"), false, true);
					OutError.SubjectId = OperationId;
					return false;
				}
				Callback();
				return true;
			}

			bool IsCurrentThread(
				const EExecutionThreadAffinity Affinity) const override
			{
				return Affinity == EExecutionThreadAffinity::GameThread
					&& IsInGameThread();
			}
		};

		class FImmediateBarrier final : public IAsyncBarrierProvider
		{
		public:
			bool Wait(
				const FBarrierRequest& Request,
				FUeAgentResourceTracker& Resources,
				FBarrierResult& OutResult,
				FExecutionMutationError& OutError) override
			{
				(void)Resources;
				if (!Request.IsValid())
				{
					OutError = NativeValueFailure(TEXT("world_destructive_barrier_invalid"),
						TEXT("The settle barrier subject generation must be non-negative."), TEXT("subject_generation"),
						MakeShared<FJsonValueNumber>(Request.GetSubjectGeneration()), MakeShared<FJsonValueString>(TEXT("non-negative integer")));
					return false;
				}
				OutResult = FBarrierResult(
					Request.GetSubjectGeneration(),
					Request.GetRequiredSignals(),
					FString::Printf(
						TEXT("world-destructive:settled:%lld"),
						Request.GetSubjectGeneration()),
					true);
				return true;
			}
		};

		class FWorldDestructiveDurableSession final
			: public IExecutionDurableSession
		{
		public:
			explicit FWorldDestructiveDurableSession(
				FAssetDurableSessionRef InSession)
				: Session(MoveTemp(InSession))
			{
			}

			FAssetDurableSessionRef Session;
		};

		class FWorldDestructiveDurablePort final
			: public IDurableExecutionPort
		{
		public:
			FWorldDestructiveDurablePort(
				FString InJournalRoot,
				FString InStableJournalOwnerId)
				: JournalRoot(MoveTemp(InJournalRoot))
				, StableJournalOwnerId(MoveTemp(InStableJournalOwnerId))
			{
			}

			bool Prepare(
				const FExecutionMutationRequest& Request,
				const int64 FencingToken,
				FUeAgentResourceTracker& Resources,
				FExecutionDurableSessionPtr& OutSession,
				FExecutionMutationError& OutError) override
			{
				OutSession.Reset();
				const TArray<FImpact>& Impacts =
					Request.GetPlan()->GetImpactSet().GetImpacts();
				if (FencingToken < 0)
				{
					OutError = InvalidFenceFailure(TEXT("prepare"), TEXT("world_destructive_durable_target_invalid"));
					OutError.ActualEvidence = MakeShared<FJsonValueNumber>(FencingToken);
					return false;
				}
				if (Impacts.IsEmpty())
				{
					OutError = NativeValueFailure(TEXT("world_destructive_durable_target_invalid"),
						TEXT("Durable execution requires at least one impacted package."), TEXT("impact_count"),
						MakeShared<FJsonValueNumber>(Impacts.Num()), MakeShared<FJsonValueString>(TEXT("at least one")), TEXT("prepare"));
					return false;
				}
				TArray<FAssetDurableTarget> Targets;
				for (const FImpact& Impact : Impacts)
				{
					const FUeAgentCanonicalIdentity& Identity =
						Impact.GetIdentity();
					const FString PackageName = Identity.GetOwnerId();
					const bool bMap = Identity.GetTargetKind()
						== TEXT("world_map");
					if ((!bMap
							&& Identity.GetTargetKind()
								!= TEXT(
									"world_external_actor_package"))
						|| !PackageName.StartsWith(TEXT("/Game/"))
						|| !FPackageName::IsValidLongPackageName(
							PackageName,
							true))
					{
						const bool bKindValid = bMap || Identity.GetTargetKind() == TEXT("world_external_actor_package");
						OutError = NativeValueFailure(TEXT("world_destructive_durable_package_invalid"),
							bKindValid ? TEXT("Durable execution requires a valid long package name under /Game/.")
								: TEXT("Durable execution accepts only world map or external actor package targets."),
							bKindValid ? TEXT("package_name") : TEXT("target_kind"),
							MakeShared<FJsonValueString>(bKindValid ? PackageName : Identity.GetTargetKind()),
							MakeShared<FJsonValueString>(bKindValid ? TEXT("valid /Game/ long package name") : TEXT("world_map or world_external_actor_package")),
							TEXT("prepare"), PackageName);
						return false;
					}
					const FPlannedChange* Change = Request.GetPlan()->GetChangeSet().GetChanges().FindByPredicate(
						[&Identity](const FPlannedChange& Item) { return Item.GetIdentity().GetCanonicalKey() == Identity.GetCanonicalKey(); });
					Targets.Emplace(
						Identity,
						PackageName,
						bMap
							? EPackageHeaderKind::Map
							: EPackageHeaderKind::EvidenceRequired,
						Change && Change->GetIntent() == EChangeIntent::Delete ? EAssetDurableIntent::Delete : EAssetDurableIntent::Save,
						ERollbackGrade::DurableBackup);
				}

				FAssetDurableError Error;
				int64 DurableFencingToken = -1;
				{
					FScopeLock Lock(&Mutex);
					if (!TryAcquireAssetDurableFence(
							JournalRoot,
							StableJournalOwnerId,
							Resources,
							DurableFencingToken,
							Error))
					{
						OutError = PreserveFailure(MoveTemp(Error), TEXT("world_destructive_durable_fence_failed"),
							TEXT("The durable package fence could not be acquired and no diagnostic was provided."), TEXT("durable_fence_acquired"), TEXT("prepare"));
						return false;
					}
				}
				const FAssetDurableRequest DurableRequest(
					Request.GetPlan(),
					MoveTemp(Targets),
					DurableFencingToken,
					StableJournalOwnerId,
					EAtomicityMode::CompensatedSaga);
				FAssetDurableSessionPtr Session;
				if (!TryPrepareAssetDurability(
						DurableRequest,
						Backend,
						JournalRoot,
						Resources,
						Session,
						Error)
					|| !Session.IsValid())
				{
					OutError = PreserveFailure(MoveTemp(Error), TEXT("world_destructive_durable_session_invalid"),
						TEXT("Durable preparation did not provide a session or a diagnostic."), TEXT("durable_session_ready"), TEXT("prepare"));
					return false;
				}
				OutSession = MakeShared<
					FWorldDestructiveDurableSession,
					ESPMode::ThreadSafe>(Session.ToSharedRef());
				return true;
			}

			bool Persist(
				const FExecutionDurableSessionRef& Session,
				const int64 FencingToken,
				const FMutationFacts& Facts,
				FUeAgentResourceTracker& Resources,
				FDurableReceipt& OutReceipt,
				FExecutionMutationError& OutError) override
			{
				const TSharedRef<FWorldDestructiveDurableSession> Typed =
					StaticCastSharedRef<
						FWorldDestructiveDurableSession>(Session);
				if (FencingToken < 0)
				{
					OutError = InvalidFenceFailure(TEXT("persist"));
					OutError.ActualEvidence = MakeShared<FJsonValueNumber>(FencingToken);
					return false;
				}
				FAssetDurableError Error;
				FAssetDurableSessionPtr Enlisted;
				if (!TryEnlistAssetDurableTargets(
						Typed->Session,
						Facts.GetDurableTargets(),
						Backend,
						Typed->Session->GetRequest().GetFencingToken(),
						Resources,
						Enlisted,
						Error)
					|| !Enlisted.IsValid())
				{
					OutError = PreserveFailure(MoveTemp(Error), TEXT("world_destructive_durable_enlist_failed"),
						TEXT("The changed packages could not be enlisted for persistence and no diagnostic was provided."), TEXT("durable_targets_enlisted"), TEXT("persist"));
					return false;
				}
				Typed->Session = Enlisted.ToSharedRef();
				if (!TryPersistAssetDurabilityForVerification(
						Typed->Session,
						Backend,
						Typed->Session->GetRequest().GetFencingToken(),
						Resources,
						OutReceipt,
						Error))
				{
					OutError = PreserveFailure(MoveTemp(Error), TEXT("world_destructive_durable_persist_failed"),
						TEXT("The packages could not be persisted for verification and no diagnostic was provided."), TEXT("packages_persisted"), TEXT("persist"));
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
						OutReceipt.GetPhaseHistory(),
						OutReceipt.GetPreMutationStates(),
						OutReceipt.GetExpectedPersistedStates());
				{
					FScopeLock Lock(&Mutex);
					PersistedByJournal.Add(
						Persisted->GetJournalId(),
						Persisted);
				}
				for (const FAssetDurableTarget& Target :
					Persisted->GetRequest().GetTargets())
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
				const int64 FencingToken,
				const bool bMemoryRollbackConfirmed,
				FUeAgentResourceTracker& Resources,
				FDurableReceipt& OutReceipt,
				FExecutionMutationError& OutError) override
			{
				const TSharedRef<FWorldDestructiveDurableSession> Typed =
					StaticCastSharedRef<
						FWorldDestructiveDurableSession>(Session);
				if (FencingToken < 0)
				{
					OutError = InvalidFenceFailure(TEXT("rollback"));
					OutError.ActualEvidence = MakeShared<FJsonValueNumber>(FencingToken);
					return false;
				}
				FAssetDurableError Error;
				if (!TryRollbackAssetDurability(
						Typed->Session,
						Backend,
						Typed->Session->GetRequest().GetFencingToken(),
						bMemoryRollbackConfirmed,
						Resources,
						OutReceipt,
						Error))
				{
					OutError = PreserveFailure(MoveTemp(Error), TEXT("world_destructive_durable_rollback_failed"),
						TEXT("Durable rollback failed without providing a diagnostic."), TEXT("durable_rollback_succeeded"), TEXT("rollback"));
					return false;
				}
				FScopeLock Lock(&Mutex);
				PersistedByJournal.Remove(Typed->Session->GetJournalId());
				return OutReceipt.GetStatus() == EDurableStatus::RolledBack;
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
				OutOwnerId = (*Found)->GetRequest().GetJournalOwnerId();
				OutFencingToken =
					(*Found)->GetRequest().GetFencingToken();
				return true;
			}

			bool TryAcknowledge(
				const FDurableVerificationAcknowledgement& Acknowledgement,
				FUeAgentResourceTracker& Resources,
				UE::UAI::Contract::FDiagnostic& OutError)
			{
				FAssetDurableSessionPtr Session;
				{
					FScopeLock Lock(&Mutex);
					Session = PersistedByJournal.FindRef(
						Acknowledgement.GetJournalId());
				}
				if (!Session.IsValid())
				{
					OutError = NativeConditionFailure(TEXT("world_destructive_durable_session_missing"),
						TEXT("The durable session for acknowledgement is no longer available."), TEXT("durable_session_available"), false, true, TEXT("durable_recovery"));
					OutError.SubjectId = Acknowledgement.GetJournalId();
					return false;
				}
				FAssetDurableError Error;
				if (!TryAcknowledgeAssetDurability(
						Session.ToSharedRef(),
						Acknowledgement,
						Session->GetRequest().GetFencingToken(),
						Resources,
						Error))
				{
					OutError = PreserveFailure(MoveTemp(Error), TEXT("world_destructive_durable_acknowledge_failed"),
						TEXT("Durability acknowledgement failed without providing a diagnostic."), TEXT("durability_acknowledged"), TEXT("independent_verify"));
					return false;
				}
				FScopeLock Lock(&Mutex);
				PersistedByJournal.Remove(
					Acknowledgement.GetJournalId());
				return true;
			}

			bool TryRollbackPersisted(
				const FString& JournalId,
				const bool bMemoryRollbackConfirmed,
				FUeAgentResourceTracker& Resources,
				UE::UAI::Contract::FDiagnostic& OutError)
			{
				FAssetDurableSessionPtr Session;
				{
					FScopeLock Lock(&Mutex);
					Session = PersistedByJournal.FindRef(JournalId);
				}
				if (!Session.IsValid())
				{
					OutError = NativeConditionFailure(TEXT("world_destructive_durable_session_missing"),
						TEXT("The durable session for rollback is no longer available."), TEXT("durable_session_available"), false, true, TEXT("durable_recovery"));
					OutError.SubjectId = JournalId;
					return false;
				}
				FDurableReceipt Receipt;
				FAssetDurableError Error;
				if (!TryRollbackAssetDurability(
						Session.ToSharedRef(),
						Backend,
						Session->GetRequest().GetFencingToken(),
						bMemoryRollbackConfirmed,
						Resources,
						Receipt,
						Error)
					|| Receipt.GetStatus() != EDurableStatus::RolledBack)
				{
					OutError = PreserveFailure(MoveTemp(Error), TEXT("world_destructive_durable_rollback_unconfirmed"),
						TEXT("The durable rollback could not be confirmed."), TEXT("durable_rollback_confirmed"), TEXT("rollback"));
					return false;
				}
				FScopeLock Lock(&Mutex);
				PersistedByJournal.Remove(JournalId);
				return true;
			}

		private:
			FString JournalRoot;
			FString StableJournalOwnerId;
			FPackageAssetDurableBackend Backend;
			mutable FCriticalSection Mutex;
			TMap<FString, FAssetDurableSessionPtr> PersistedByJournal;
		};

		class FWorldDestructiveMutationHook final : public IMutationHook
		{
		public:
			FWorldDestructiveMutationHook(
				TSharedRef<IUeAgentWorldDestructiveDurableMutation> InMutation,
				TArray<FString> InIdentityKeys)
				: Mutation(MoveTemp(InMutation))
				, IdentityKeys(MoveTemp(InIdentityKeys))
			{
			}

			bool Execute(
				const FExecutionMutationRequest& Request,
				const FLockFenceLease& Lease,
				FUeAgentResourceTracker& Resources,
				FMutationFacts& OutFacts,
				FExecutionMutationError& OutError) override
			{
				(void)Request;
				(void)Lease;
				(void)Resources;
				OutFacts = FMutationFacts(
					1,
					IdentityKeys,
					{},
					true);
				UE::UAI::Contract::FDiagnostic Error;
				if (!Mutation->Apply(Error))
				{
					OutError = PreserveFailure(MoveTemp(Error), TEXT("world_destructive_mutation_failed"),
						TEXT("The world mutation failed without providing a diagnostic."), TEXT("mutation_succeeded"), TEXT("domain_execute"), Mutation->GetPackageName());
					return false;
				}
				return true;
			}

			bool Rollback(
				const FExecutionMutationRequest& Request,
				const FMutationFacts& Facts,
				FUeAgentResourceTracker& Resources,
				bool& bOutConfirmed,
				FExecutionMutationError& OutError) override
			{
				(void)Request;
				(void)Facts;
				(void)Resources;
				UE::UAI::Contract::FDiagnostic Error;
				bOutConfirmed = Mutation->Rollback(Error);
				if (!bOutConfirmed)
				{
					OutError = PreserveFailure(MoveTemp(Error), TEXT("world_destructive_rollback_unconfirmed"),
						TEXT("The world mutation rollback could not be confirmed and no diagnostic was provided."), TEXT("rollback_confirmed"), TEXT("rollback"), Mutation->GetPackageName());
				}
				return bOutConfirmed;
			}

		private:
			TSharedRef<IUeAgentWorldDestructiveDurableMutation> Mutation;
			TArray<FString> IdentityKeys;
		};

		class FWorldDestructiveDurableRunner final
			: public IUeAgentWorldDestructiveDurableRunner
		{
		public:
			FWorldDestructiveDurableRunner(
				FString InJournalRoot,
				FString InStableJournalOwnerId)
				: StableJournalOwnerId(MoveTemp(InStableJournalOwnerId))
				, CheckpointJournalRoot(InJournalRoot / TEXT("world-checkpoints"))
				, Scheduler(MakeShared<
					FInlineGameThreadScheduler,
					ESPMode::ThreadSafe>())
				, LockFence(MakeShared<
					FMonotonicLockFenceProvider,
					ESPMode::ThreadSafe>())
				, Barrier(MakeShared<
					FImmediateBarrier,
					ESPMode::ThreadSafe>())
				, Durable(MakeShared<
					FWorldDestructiveDurablePort,
					ESPMode::ThreadSafe>(
						MoveTemp(InJournalRoot),
						StableJournalOwnerId))
				, Coordinator(
					Scheduler,
					LockFence,
					Barrier,
					Durable)
			{
			}

			FUeAgentWorldDestructiveResult Execute(
				UWorld& ExplicitWorld,
				const FUeAgentWorldDestructiveRequest& Request,
				TSharedRef<IUeAgentWorldDestructiveDurableMutation> Mutation,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation)
				override
			{
				FUeAgentWorldDestructiveResult Result;
				Result.CommandId = Request.CommandId;
				Result.TargetRef = Request.TargetRef;
				Result.ResultRef = Request.OperandRef;
				auto Fail = [&Result](UE::UAI::Contract::FDiagnostic Diagnostic)
                {
                    Result.Diagnostic = MoveTemp(Diagnostic);
                    if (Result.Diagnostic.TargetPath.IsEmpty()) Result.Diagnostic.TargetPath = Result.TargetRef;
                    Result.Diagnostic.TerminalStatus = Result.TerminalState;
                    return Result;
                };
				if (!IsInGameThread())
				{
					return Fail(NativeConditionFailure(TEXT("world_destructive_game_thread_required"),
						TEXT("World destructive execution must start on the game thread."), TEXT("game_thread"), false, true, TEXT("world_destructive")));
				}
				if (!IsValid(&ExplicitWorld))
				{
					return Fail(NativeConditionFailure(TEXT("world_destructive_durable_mutation_invalid"),
						TEXT("World destructive execution requires a valid explicit world."), TEXT("world_valid"), false, true, TEXT("world_destructive")));
				}
				if (Mutation->GetPackageName().IsEmpty() || Mutation->GetMutationPackages().IsEmpty())
				{
					return Fail(NativeValueFailure(TEXT("world_destructive_durable_mutation_invalid"),
						TEXT("World destructive execution requires a primary map and a complete mutation package inventory."),
						Mutation->GetPackageName().IsEmpty() ? TEXT("map_package_name") : TEXT("mutation_packages"),
						MakeShared<FJsonValueString>(TEXT("")), MakeShared<FJsonValueString>(TEXT("non-empty package name")), TEXT("world_destructive")));
				}
				if (Cancellation->IsCancelled())
				{
					auto Diagnostic = NativeConditionFailure(TEXT("world_destructive_cancelled"),
						TEXT("The world destructive operation was cancelled before submission."), TEXT("cancellation_requested"), true, false, TEXT("world_destructive"));
					Diagnostic.SubjectId = Request.RequestId;
					return Fail(MoveTemp(Diagnostic));
				}

				TArray<FString> IdentityKeys;
				for (const auto& Package : Mutation->GetMutationPackages())
					IdentityKeys.Add(MakePackageIdentity(Package.PackageName, Package.HeaderKind == EPackageHeaderKind::Map).GetCanonicalKey());
				const FPlanningPreflightPlanRef Plan = MakePlan(
					Request,
					StableJournalOwnerId,
					Mutation->GetMutationPackages());
				if (Mutation->RequiresPackageCheckpoint())
					return ExecuteCheckpointedWorldMutation(ExplicitWorld, Request, Mutation,
						Plan, CheckpointJournalRoot, Cancellation);
				FUeAgentExecutionContextRefPtr ExecutionContext;
				UE::UAI::Contract::FDiagnostic Error;
				if (!TryMakeExecutionContext(
						ExplicitWorld,
						Plan,
						ExecutionContext,
						Error))
				{
					return Fail(MoveTemp(Error));
				}

				const FUeAgentResourceTrackerOwner Resources = MakeResources();
				FUeAgentCancellationSource CancellationSource(
					[]() { return FPlatformTime::Seconds(); });
				const FString OperationId = TEXT("world-destructive:")
					+ Request.CommandId + TEXT(":") + Request.RequestId;
				const TSharedRef<
					FWorldDestructiveMutationHook,
					ESPMode::ThreadSafe> Hook = MakeShared<
						FWorldDestructiveMutationHook,
						ESPMode::ThreadSafe>(
							Mutation,
							MoveTemp(IdentityKeys));
				const FExecutionMutationRequest ExecutionRequest(
					OperationId,
					StableJournalOwnerId,
					Plan,
					ExecutionContext.ToSharedRef(),
					Resources,
					FUeAgentOperationDeadline(),
					CancellationSource.GetToken(),
					EExecutionThreadAffinity::GameThread,
					EExecutionReentrancyPolicy::RejectSameTarget);
				FString SubmittedId;
				FExecutionMutationError ExecutionError;
				if (!Coordinator.Submit(
						ExecutionRequest,
						Hook,
						SubmittedId,
						ExecutionError))
				{
					return Fail(PreserveFailure(MoveTemp(ExecutionError), TEXT("world_destructive_submit_failed"),
						TEXT("The world mutation could not be submitted and no diagnostic was provided."), TEXT("operation_submitted")));
				}
				Coordinator.DispatchReady();
				FExecutionReceipt Receipt;
				ExecutionError = FExecutionMutationError();
				if (!Coordinator.GetReceipt(
						SubmittedId,
						Receipt,
						ExecutionError))
				{
					Result.TerminalState = TEXT("state_unknown");
					auto Diagnostic = PreserveFailure(MoveTemp(ExecutionError), TEXT("world_destructive_receipt_unavailable"),
						TEXT("The submitted world mutation receipt could not be read and no diagnostic was provided."), TEXT("execution_receipt_available"));
					Diagnostic.MutationMayHaveStarted = true;
					Diagnostic.bRetrySafe = false;
					return Fail(MoveTemp(Diagnostic));
				}
				const FDurableReceipt* DurableReceipt =
					Receipt.GetDurableReceipt();
				if (DurableReceipt)
				{
					Result.JournalId = DurableReceipt->GetJournalId();
				}
				if (Receipt.GetStatus() == EExecutionStatus::PersistedUnverified)
				{
					Result.bChanged = true;
					Result.bPersisted = true;
				}
				if (Receipt.GetStatus()
					!= EExecutionStatus::PersistedUnverified
					|| !DurableReceipt)
				{
					Result.TerminalState = Receipt.GetStatus()
						== EExecutionStatus::RolledBack
						|| Receipt.GetStatus()
							== EExecutionStatus::CancelledNoChange
						? TEXT("rolled_back")
						: TEXT("state_unknown");
					auto Diagnostic = Receipt.GetDiagnostic();
					if (Diagnostic.IsEmpty())
					{
						Diagnostic = Receipt.GetStatus() != EExecutionStatus::PersistedUnverified
							? NativeValueFailure(TEXT("world_destructive_durable_execution_failed"),
								TEXT("The world mutation did not reach the persisted state required for verification."), TEXT("execution_status"),
								MakeShared<FJsonValueString>(LexToString(Receipt.GetStatus())), MakeShared<FJsonValueString>(TEXT("persisted_unverified")))
							: NativeConditionFailure(TEXT("world_destructive_durable_execution_failed"),
								TEXT("The persisted world mutation has no durable receipt for verification."), TEXT("durable_receipt_available"), false, true);
					}
					return Fail(MoveTemp(Diagnostic));
				}

				FDurableInventory Inventory;
				FString JournalOwnerId;
				int64 DurableFencingToken = -1;
				if (!Durable->TryGetVerificationContext(
						DurableReceipt->GetJournalId(),
						Inventory,
						JournalOwnerId,
						DurableFencingToken))
				{
					Result.TerminalState = TEXT("state_unknown");
					auto Diagnostic = NativeConditionFailure(TEXT("world_destructive_durable_context_missing"),
						TEXT("The world mutation was persisted, but its durable verification context is no longer available."),
						TEXT("verification_context_available"), false, true, TEXT("independent_verify"));
					Diagnostic.SubjectId = DurableReceipt->GetJournalId();
					Diagnostic.MutationStarted = true;
					return Fail(MoveTemp(Diagnostic));
				}
				FPersistedWorldLoadError ReadbackError;
				UE::UAI::Contract::FDiagnostic SemanticError;
				Result.bReadbackVerified = TryObservePersistedWorld(
					Inventory.GetArtifacts(),
					[&Mutation, &SemanticError](
						UWorld& PersistedWorld,
						const FPersistedWorldReferenceMap& References,
						FPersistedWorldLoadError& OutError)
					{
						if (Mutation->VerifyPersisted(
							PersistedWorld,
							References,
							SemanticError))
						{
							return true;
						}
						OutError = PreserveFailure(MoveTemp(SemanticError), TEXT("world_destructive_persisted_semantic_mismatch"),
							TEXT("The persisted world does not match the requested change."), TEXT("persisted_state_equivalent"), TEXT("independent_verify"), Mutation->GetPackageName());
						return false;
					},
					ReadbackError);
				Result.VerificationId = TEXT(
					"world-destructive-persisted-readback:")
					+ Request.RequestId;
				if (!Result.bReadbackVerified)
				{
					UE::UAI::Contract::FDiagnostic MemoryRollbackError;
					const bool bMemoryRolledBack =
						Mutation->Rollback(MemoryRollbackError);
					if (!bMemoryRolledBack)
						MemoryRollbackError = PreserveFailure(MoveTemp(MemoryRollbackError), TEXT("world_destructive_rollback_unconfirmed"),
							TEXT("The in-memory world rollback could not be confirmed after readback failed."), TEXT("rollback_confirmed"), TEXT("rollback"), Mutation->GetPackageName());
					UE::UAI::Contract::FDiagnostic DurableRollbackError;
					const bool bDurableRolledBack =
						Durable->TryRollbackPersisted(
							DurableReceipt->GetJournalId(),
							bMemoryRolledBack,
							*Resources,
							DurableRollbackError);
					Result.TerminalState = bMemoryRolledBack
						&& bDurableRolledBack
						? TEXT("rolled_back")
						: TEXT("state_unknown");
					Result.bChanged = !bMemoryRolledBack;
					Result.bPersisted = !bDurableRolledBack;
					ReadbackError = PreserveFailure(MoveTemp(ReadbackError), TEXT("world_destructive_readback_failed"),
						TEXT("Persisted world readback failed without providing a diagnostic."), TEXT("readback_verified"), TEXT("independent_verify"), Mutation->GetPackageName());
					ReadbackError.Append(MemoryRollbackError);
                    ReadbackError.Append(DurableRollbackError);
                    ReadbackError.RollbackAttempted = true;
                    ReadbackError.RollbackSucceeded = bMemoryRolledBack && bDurableRolledBack;
                    return Fail(ReadbackError);
				}

				UE::UAI::Contract::FDiagnostic AcknowledgeError;
				if (!Durable->TryAcknowledge(
						FDurableVerificationAcknowledgement(
							DurableReceipt->GetJournalId(),
							DurableReceipt->GetPlanFingerprint(),
							DurableReceipt->GetInventoryHash(),
							Result.VerificationId,
							JournalOwnerId,
							DurableFencingToken),
						*Resources,
						AcknowledgeError))
				{
					Result.TerminalState = TEXT("state_unknown");
					Result.bChanged = true;
					Result.bPersisted = true;
					return Fail(PreserveFailure(MoveTemp(AcknowledgeError), TEXT("world_destructive_durable_acknowledge_failed"),
						TEXT("The persisted world could not be acknowledged after verification and no diagnostic was provided."), TEXT("durability_acknowledged"), TEXT("independent_verify")));
				}

				Result.bChanged = true;
				Result.bPersisted = true;
				Result.PersistenceScope = Mutation->GetPersistenceScope();
				Result.TerminalState = TEXT("committed");
				return Result;
			}

		private:
			FString StableJournalOwnerId;
			FString CheckpointJournalRoot;
			TSharedRef<FInlineGameThreadScheduler, ESPMode::ThreadSafe>
				Scheduler;
			TSharedRef<FMonotonicLockFenceProvider, ESPMode::ThreadSafe>
				LockFence;
			TSharedRef<FImmediateBarrier, ESPMode::ThreadSafe> Barrier;
			TSharedRef<FWorldDestructiveDurablePort, ESPMode::ThreadSafe>
				Durable;
			FExecutionMutationCoordinator Coordinator;
		};
	}

	bool TryCreateWorldDestructiveDurableRunner(
		FString JournalRoot,
		FString StableJournalOwnerId,
		TSharedPtr<IUeAgentWorldDestructiveDurableRunner>& OutRunner,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRunner.Reset();
		OutError.Reset();
		JournalRoot.TrimStartAndEndInline();
		StableJournalOwnerId.TrimStartAndEndInline();
		if (JournalRoot.IsEmpty())
		{
			OutError = NativeConditionFailure(TEXT("world_destructive_durable_runner_config_invalid"),
				TEXT("The world destructive durable runner requires a non-empty journal root."), TEXT("journal_root_configured"), false, true, TEXT("authoring"));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(JournalRoot);
			return false;
		}
		if (StableJournalOwnerId.IsEmpty())
		{
			OutError = NativeConditionFailure(TEXT("world_destructive_durable_runner_config_invalid"),
				TEXT("The world destructive durable runner requires a non-empty stable journal owner identifier."), TEXT("journal_owner_configured"), false, true, TEXT("authoring"));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(StableJournalOwnerId);
			return false;
		}
		OutRunner = MakeShared<FWorldDestructiveDurableRunner>(
			MoveTemp(JournalRoot),
			MoveTemp(StableJournalOwnerId));
		return OutRunner.IsValid();
	}
}
