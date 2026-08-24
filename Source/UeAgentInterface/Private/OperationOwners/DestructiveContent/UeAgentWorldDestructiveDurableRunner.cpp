#include "OperationOwners/DestructiveContent/UeAgentWorldDestructiveDurableRunner.h"

#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/AssetDurable/UeAgentPersistedWorld.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
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
		FUeAgentResourceTrackerOwner MakeResources()
		{
			return MakeShared<
				FUeAgentResourceTracker,
				ESPMode::ThreadSafe>(FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 1024 * 1024},
				{EUeAgentResourceDimension::StringBytes, 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 8192},
				{EUeAgentResourceDimension::Items, 8192},
				{EUeAgentResourceDimension::Files, 512},
				{EUeAgentResourceDimension::ReferenceEdges, 8192},
				{EUeAgentResourceDimension::WorkUnits, 131072},
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					32 * 1024 * 1024},
			}));
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
			const FString& MapPackageName,
			const FString& MutationPackageName,
			const FString& OwnerId,
			const FUeAgentCanonicalIdentity& MapIdentity,
			const FUeAgentCanonicalIdentity& MutationIdentity)
		{
			TArray<FPlannedChange> Changes;
			TArray<FImpact> Impacts;
			auto AddPackage = [&Changes, &Impacts](
				const FUeAgentCanonicalIdentity& Identity)
			{
				Changes.Emplace(
					Identity,
					EChangeIntent::Update,
					FUeAgentCanonicalValuePtr(),
					FUeAgentCanonicalValuePtr(),
					TArray<FString>{TEXT("uai:asset:write")},
					TEXT("world-destructive:durable-runner"));
				Impacts.Emplace(
					Identity,
					Identity,
					TEXT("direct"),
					TEXT("world-destructive:durable-runner"));
			};
			AddPackage(MapIdentity);
			if (!MutationPackageName.Equals(
					MapPackageName,
					ESearchCase::CaseSensitive))
			{
				AddPackage(MutationIdentity);
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
					+ TEXT(":") + MapPackageName + TEXT(":")
					+ MutationPackageName + TEXT(":")
					+ Request.TargetRef));
		}

		bool TryMakeExecutionContext(
			UWorld& World,
			const FPlanningPreflightPlanRef& Plan,
			FUeAgentExecutionContextRefPtr& OutContext,
			FString& OutError)
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
				OutError = Error.Code.IsEmpty()
					? TEXT("world_destructive_execution_context_invalid")
					: Error.Code;
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
				if (Affinity != EExecutionThreadAffinity::GameThread
					|| !IsInGameThread()
					|| !Callback)
				{
					OutError = FExecutionMutationError(
						TEXT("world_destructive_execution_thread_invalid"),
						OperationId);
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
					OutError = FExecutionMutationError(
						TEXT("world_destructive_barrier_invalid"));
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
				if (FencingToken < 0 || Impacts.IsEmpty())
				{
					OutError = FExecutionMutationError(
						TEXT("world_destructive_durable_target_invalid"));
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
						OutError = FExecutionMutationError(
							TEXT(
								"world_destructive_durable_package_invalid"),
							PackageName);
						return false;
					}
					Targets.Emplace(
						Identity,
						PackageName,
						bMap
							? EPackageHeaderKind::Map
							: EPackageHeaderKind::EvidenceRequired,
						EAssetDurableIntent::Save,
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
						OutError = FExecutionMutationError(
							Error.GetCode(),
							Error.GetDetail());
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
					OutError = FExecutionMutationError(
						Error.GetCode(),
						Error.GetDetail());
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
					OutError = FExecutionMutationError(
						TEXT("world_destructive_operation_fence_invalid"));
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
					OutError = FExecutionMutationError(
						Error.GetCode(),
						Error.GetDetail());
					return false;
				}
				Typed->Session = Enlisted.ToSharedRef();
				if (!TryPersistAssetDurability(
						Typed->Session,
						Backend,
						Typed->Session->GetRequest().GetFencingToken(),
						Resources,
						OutReceipt,
						Error)
					|| OutReceipt.GetStatus()
						!= EDurableStatus::PersistedUnverified
					|| !OutReceipt.RequiresIndependentVerification())
				{
					OutError = FExecutionMutationError(
						Error.IsPresent()
							? Error.GetCode()
							: TEXT(
								"world_destructive_durable_persist_unconfirmed"),
						Error.IsPresent()
							? Error.GetDetail()
							: AssetDurable::LexToString(
								OutReceipt.GetStatus()));
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
					OutError = FExecutionMutationError(
						TEXT("world_destructive_operation_fence_invalid"));
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
					OutError = FExecutionMutationError(
						Error.GetCode(),
						Error.GetDetail());
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
				FString& OutError)
			{
				FAssetDurableSessionPtr Session;
				{
					FScopeLock Lock(&Mutex);
					Session = PersistedByJournal.FindRef(
						Acknowledgement.GetJournalId());
				}
				if (!Session.IsValid())
				{
					OutError = TEXT(
						"world_destructive_durable_session_missing");
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
					OutError = Error.GetCode() + TEXT(":")
						+ Error.GetDetail();
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
				FString& OutError)
			{
				FAssetDurableSessionPtr Session;
				{
					FScopeLock Lock(&Mutex);
					Session = PersistedByJournal.FindRef(JournalId);
				}
				if (!Session.IsValid())
				{
					OutError = TEXT(
						"world_destructive_durable_session_missing");
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
					OutError = Error.IsPresent()
						? Error.GetCode() + TEXT(":") + Error.GetDetail()
						: TEXT(
							"world_destructive_durable_rollback_unconfirmed");
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
				FString InIdentityKey)
				: Mutation(MoveTemp(InMutation))
				, IdentityKey(MoveTemp(InIdentityKey))
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
					{IdentityKey},
					{},
					true);
				FString Error;
				if (!Mutation->Apply(Error))
				{
					OutError = FExecutionMutationError(
						TEXT("world_destructive_mutation_failed"),
						Error);
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
				FString Error;
				bOutConfirmed = Mutation->Rollback(Error);
				if (!bOutConfirmed)
				{
					OutError = FExecutionMutationError(
						TEXT("world_destructive_memory_rollback_failed"),
						Error);
				}
				return bOutConfirmed;
			}

		private:
			TSharedRef<IUeAgentWorldDestructiveDurableMutation> Mutation;
			FString IdentityKey;
		};

		class FWorldDestructiveDurableRunner final
			: public IUeAgentWorldDestructiveDurableRunner
		{
		public:
			FWorldDestructiveDurableRunner(
				FString InJournalRoot,
				FString InStableJournalOwnerId)
				: StableJournalOwnerId(MoveTemp(InStableJournalOwnerId))
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
					Durable,
					32)
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
				auto Fail = [&Result](
					FString Code,
					FString Detail = FString())
				{
					Result.ErrorCode = MoveTemp(Code);
					Result.ErrorDetail = MoveTemp(Detail);
					return Result;
				};
				if (!IsInGameThread())
				{
					return Fail(
						TEXT("world_destructive_game_thread_required"));
				}
				if (!IsValid(&ExplicitWorld)
					|| Mutation->GetPackageName().IsEmpty()
					|| Mutation->GetMutationPackageName().IsEmpty())
				{
					return Fail(
						TEXT("world_destructive_durable_mutation_invalid"));
				}
				if (Cancellation->IsCancelled())
				{
					return Fail(
						TEXT("world_destructive_cancelled"),
						Request.RequestId);
				}

				const FUeAgentCanonicalIdentity MapIdentity =
					MakePackageIdentity(
						Mutation->GetPackageName(),
						true);
				const bool bMutationInMap =
					Mutation->GetMutationPackageName().Equals(
						Mutation->GetPackageName(),
						ESearchCase::CaseSensitive);
				const FUeAgentCanonicalIdentity MutationIdentity =
					bMutationInMap
					? MapIdentity
					: MakePackageIdentity(
						Mutation->GetMutationPackageName(),
						false);
				const FPlanningPreflightPlanRef Plan = MakePlan(
					Request,
					Mutation->GetPackageName(),
					Mutation->GetMutationPackageName(),
					StableJournalOwnerId,
					MapIdentity,
					MutationIdentity);
				FUeAgentExecutionContextRefPtr ExecutionContext;
				FString Error;
				if (!TryMakeExecutionContext(
						ExplicitWorld,
						Plan,
						ExecutionContext,
						Error))
				{
					return Fail(
						TEXT("world_destructive_execution_context_failed"),
						Error);
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
							MutationIdentity.GetCanonicalKey());
				const FExecutionMutationRequest ExecutionRequest(
					OperationId,
					StableJournalOwnerId,
					Plan,
					ExecutionContext.ToSharedRef(),
					Resources,
					FUeAgentOperationDeadline::FromTimeout(
						30.0,
						[]() { return FPlatformTime::Seconds(); }),
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
					return Fail(
						ExecutionError.GetCode(),
						ExecutionError.GetDetail());
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
					return Fail(
						ExecutionError.GetCode(),
						ExecutionError.GetDetail());
				}
				const FDurableReceipt* DurableReceipt =
					Receipt.GetDurableReceipt();
				if (DurableReceipt)
				{
					Result.JournalId = DurableReceipt->GetJournalId();
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
					return Fail(
						Receipt.GetErrorCode().IsEmpty()
							? TEXT(
								"world_destructive_durable_execution_failed")
							: Receipt.GetErrorCode(),
						ExecutionMutation::LexToString(
							Receipt.GetStatus()));
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
					return Fail(
						TEXT(
							"world_destructive_durable_context_missing"),
						DurableReceipt->GetJournalId());
				}
				FPersistedWorldLoadError ReadbackError;
				FString SemanticError;
				Result.bReadbackVerified = TryObservePersistedWorld(
					Inventory.GetArtifacts(),
					[&Mutation, &SemanticError](
						UWorld& PersistedWorld,
						const FPersistedWorldReferenceMap&,
						FPersistedWorldLoadError& OutError)
					{
						if (Mutation->VerifyPersisted(
							PersistedWorld,
							SemanticError))
						{
							return true;
						}
						OutError.Code = TEXT(
							"world_destructive_persisted_semantic_mismatch");
						OutError.Detail = SemanticError;
						return false;
					},
					ReadbackError);
				Result.VerificationId = TEXT(
					"world-destructive-persisted-readback:")
					+ Request.RequestId;
				if (!Result.bReadbackVerified)
				{
					FString MemoryRollbackError;
					const bool bMemoryRolledBack =
						Mutation->Rollback(MemoryRollbackError);
					FString DurableRollbackError;
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
					return Fail(
						TEXT(
							"world_destructive_persisted_readback_failed"),
						ReadbackError.Code + TEXT(":")
						+ ReadbackError.Detail + TEXT(":")
						+ MemoryRollbackError + TEXT(":")
						+ DurableRollbackError);
				}

				FString AcknowledgeError;
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
					return Fail(
						TEXT(
							"world_destructive_durable_acknowledge_failed"),
						AcknowledgeError);
				}

				Result.bChanged = true;
				Result.bPersisted = true;
				Result.PersistenceScope = Mutation->GetPersistenceScope();
				Result.TerminalState = TEXT("committed");
				return Result;
			}

		private:
			FString StableJournalOwnerId;
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
		FString& OutError)
	{
		OutRunner.Reset();
		OutError.Reset();
		JournalRoot.TrimStartAndEndInline();
		StableJournalOwnerId.TrimStartAndEndInline();
		if (JournalRoot.IsEmpty() || StableJournalOwnerId.IsEmpty())
		{
			OutError = TEXT(
				"world_destructive_durable_runner_config_invalid");
			return false;
		}
		OutRunner = MakeShared<FWorldDestructiveDurableRunner>(
			MoveTemp(JournalRoot),
			MoveTemp(StableJournalOwnerId));
		return OutRunner.IsValid();
	}
}
