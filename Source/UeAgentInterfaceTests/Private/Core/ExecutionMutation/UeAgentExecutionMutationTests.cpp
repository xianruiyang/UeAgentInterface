#include "Misc/AutomationTest.h"

#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"


namespace
{
	using namespace UE::UAI::AssetDurable;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;

	static TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
	MakeTracker(const int64 WorkUnits = 65536)
	{
		return MakeShared<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe>(
			FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 1048576},
				{EUeAgentResourceDimension::OutputBytes, 1048576},
				{EUeAgentResourceDimension::StringBytes, 1048576},
				{EUeAgentResourceDimension::Nodes, 4096},
				{EUeAgentResourceDimension::Items, 4096},
				{EUeAgentResourceDimension::Files, 256},
				{EUeAgentResourceDimension::ReferenceEdges, 4096},
				{EUeAgentResourceDimension::WorkUnits, WorkUnits},
				{
					EUeAgentResourceDimension::EstimatedMemoryBytes,
					16777216
				},
			}));
	}

	static FUeAgentCanonicalIdentity MakeIdentity(
		const FString& Suffix)
	{
		return FUeAgentCanonicalIdentity(
			TEXT("uai.test.asset.") + Suffix.ToLower()
				+ TEXT(".value"),
			TEXT("asset"),
			TEXT("/Game/UAI/P4_006"),
			Suffix + TEXT(".value"),
			TEXT("property"),
			TEXT("fixture-generation-7"));
	}

	static FPlanningPreflightPlanRef MakePlan()
	{
		const FUeAgentCanonicalIdentity TargetB =
			MakeIdentity(TEXT("TargetB"));
		const FUeAgentCanonicalIdentity TargetA =
			MakeIdentity(TEXT("TargetA"));
		TArray<FImpact> Impacts;
		Impacts.Emplace(
			TargetB,
			TargetB,
			TEXT("direct"),
			TEXT("fixture:p4-006:impact-b"));
		Impacts.Emplace(
			TargetA,
			TargetA,
			TEXT("direct"),
			TEXT("fixture:p4-006:impact-a"));
		return MakeShared<FPlanningPreflightPlan>(
			FUeAgentDescriptorIdentity(
				TEXT("uai.test.execution_mutation.update/v1"),
				1,
				TEXT(
					"sha256:7f8cf7b38d339d5ce25d939234a89875939a26865cac51c9c9542ecb93fc2af6")),
			TEXT("p4-006:execution-mutation"),
			TEXT("p4-006:execution-mutation:key"),
			TEXT("agent:p4-006"),
			TArray<FString>{TEXT("uai.test.update")},
			TEXT("snapshot:p4-006:before"),
			FUeAgentStateRevision(
				TEXT("fixture:p4-006"),
				TEXT("fixture-generation-7"),
				11),
			TEXT("fixture:p4-006:independent-before"),
			FChangeSet(),
			FImpactSet(MoveTemp(Impacts)),
			TArray<FPrecondition>(),
			TEXT(
				"sha256:1111111111111111111111111111111111111111111111111111111111111111"));
	}

	static FUeAgentExecutionContextRefOwner MakeExecutionContext(
		const FUeAgentDescriptorIdentity& DescriptorIdentity)
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("fixture-project:p4-006");
		Input.EditorInstanceId = TEXT("fixture-editor:p4-006");
		Input.StatePlane = EUeAgentStatePlane::Persisted;
		Input.ContextRevision = TEXT("fixture-context:p4-006:r1");

		FUeAgentExecutionContextRefPtr Context;
		FUeAgentExecutionContextError Error;
		check(FUeAgentExecutionContextRef::TryCreate(
			Input,
			DescriptorIdentity,
			EUeAgentRequestKind::Operation,
			FUeAgentExecutionContextPolicy(
				{EUeAgentStatePlane::Persisted},
				{},
				EUeAgentContextResolutionPolicy::ExplicitOnly,
				EUeAgentTransientContextPolicy::ReadOnly),
			Context,
			Error));
		return Context.ToSharedRef();
	}

	class FFakeScheduler final : public IExecutionScheduler
	{
	public:
		virtual bool Schedule(
			const EExecutionThreadAffinity Affinity,
			const FString& OperationId,
			TFunction<void()> Callback,
			FExecutionMutationError& OutError) override
		{
			if (!Callback)
			{
				OutError = FExecutionMutationError(
					TEXT("fixture_callback_missing"));
				return false;
			}
			Affinities.Add(Affinity);
			OperationIds.Add(OperationId);
			Pending.Add(MoveTemp(Callback));
			return true;
		}

		virtual bool IsCurrentThread(
			const EExecutionThreadAffinity Affinity) const override
		{
			return bRunning
				&& Affinity == EExecutionThreadAffinity::GameThread;
		}

		void RunNext()
		{
			check(Pending.Num() > 0);
			TFunction<void()> Callback = MoveTemp(Pending[0]);
			Pending.RemoveAt(0);
			bRunning = true;
			Callback();
			bRunning = false;
		}

		TArray<EExecutionThreadAffinity> Affinities;
		TArray<FString> OperationIds;
		TArray<TFunction<void()>> Pending;
		bool bRunning = false;
	};

	class FFakeLockFence final : public ILockFenceProvider
	{
	public:
		virtual bool TryAcquire(
			const TArray<FString>& LockKeys,
			const FString& OwnerId,
			FUeAgentResourceTracker& Resources,
			FLockFenceLease& OutLease,
			FExecutionMutationError& OutError) override
		{
			AcquiredKeys.Add(LockKeys);
			CurrentToken = NextToken++;
			OutLease = FLockFenceLease(
				OwnerId,
				LockKeys,
				CurrentToken);
			return true;
		}

		virtual bool IsCurrent(
			const FLockFenceLease& Lease) const override
		{
			return Lease.GetFencingToken() == CurrentToken;
		}

		virtual bool Release(
			const FLockFenceLease& Lease,
			FExecutionMutationError& OutError) override
		{
			ReleasedTokens.Add(Lease.GetFencingToken());
			return true;
		}

		void Invalidate()
		{
			++CurrentToken;
		}

		int64 NextToken = 41;
		int64 CurrentToken = -1;
		TArray<TArray<FString>> AcquiredKeys;
		TArray<int64> ReleasedTokens;
	};

	class FFakeMutationHook final : public IMutationHook
	{
	public:
		virtual bool Execute(
			const FExecutionMutationRequest& Request,
			const FLockFenceLease& Lease,
			FUeAgentResourceTracker& Resources,
			FMutationFacts& OutFacts,
			FExecutionMutationError& OutError) override
		{
			++ExecuteCalls;
			ObservedTokens.Add(Lease.GetFencingToken());
			if (OnExecute)
			{
				OnExecute();
			}
			OutFacts = FMutationFacts(
				12,
				Lease.GetLockKeys(),
				{
					TEXT("domain_hook_complete"),
					TEXT("generated_state_settled")
				},
				true);
			return true;
		}

		virtual bool Rollback(
			const FExecutionMutationRequest& Request,
			const FMutationFacts& Facts,
			FUeAgentResourceTracker& Resources,
			bool& bOutConfirmed,
			FExecutionMutationError& OutError) override
		{
			++RollbackCalls;
			bOutConfirmed = true;
			return true;
		}

		int32 ExecuteCalls = 0;
		int32 RollbackCalls = 0;
		TArray<int64> ObservedTokens;
		TFunction<void()> OnExecute;
	};

	class FFakeBarrier final : public IAsyncBarrierProvider
	{
	public:
		virtual bool Wait(
			const FBarrierRequest& Request,
			FUeAgentResourceTracker& Resources,
			FBarrierResult& OutResult,
			FExecutionMutationError& OutError) override
		{
			++WaitCalls;
			TArray<FString> Signals = Request.GetRequiredSignals();
			if (bMissingSignal && Signals.Num() > 0)
			{
				Signals.RemoveAt(0);
			}
			OutResult = FBarrierResult(
				Request.GetSubjectGeneration()
					+ (bGenerationMismatch ? 1 : 0),
				MoveTemp(Signals),
				TEXT("revision:p4-006:settled"),
				!bMissingSignal);
			return true;
		}

		int32 WaitCalls = 0;
		bool bGenerationMismatch = false;
		bool bMissingSignal = false;
	};

	class FFakeDurableSession final : public IExecutionDurableSession
	{
	};

	class FFakeDurablePort final : public IDurableExecutionPort
	{
	public:
		virtual bool Prepare(
			const FExecutionMutationRequest& Request,
			const int64 FencingToken,
			FUeAgentResourceTracker& Resources,
			FExecutionDurableSessionPtr& OutSession,
			FExecutionMutationError& OutError) override
		{
			++PrepareCalls;
			ObservedTokens.Add(FencingToken);
			OutSession = MakeShared<
				FFakeDurableSession,
				ESPMode::ThreadSafe>();
			return true;
		}

		virtual bool Persist(
			const FExecutionDurableSessionRef& Session,
			const int64 FencingToken,
			FUeAgentResourceTracker& Resources,
			FDurableReceipt& OutReceipt,
			FExecutionMutationError& OutError) override
		{
			++PersistCalls;
			ObservedTokens.Add(FencingToken);
			OutReceipt = MakeReceipt(Status);
			return true;
		}

		virtual bool Rollback(
			const FExecutionDurableSessionRef& Session,
			const int64 FencingToken,
			const bool bMemoryRollbackConfirmed,
			FUeAgentResourceTracker& Resources,
			FDurableReceipt& OutReceipt,
			FExecutionMutationError& OutError) override
		{
			++RollbackCalls;
			OutReceipt = MakeReceipt(
				bMemoryRollbackConfirmed
					? EDurableStatus::RolledBack
					: EDurableStatus::StateUnknown);
			return true;
		}

		static FDurableReceipt MakeReceipt(
			const EDurableStatus InStatus)
		{
			return FDurableReceipt(
				TEXT("journal:p4-006"),
				InStatus,
				TEXT(
					"sha256:1111111111111111111111111111111111111111111111111111111111111111"),
				TEXT(
					"sha256:2222222222222222222222222222222222222222222222222222222222222222"),
				InStatus == EDurableStatus::RolledBack
					? TArray<FString>{
						TEXT("prepared"),
						TEXT("rolled_back")
					}
					: TArray<FString>{
						TEXT("prepared"),
						TEXT("published")
					},
				InStatus == EDurableStatus::PersistedUnverified
					? ERecoveryAction::VerifyPersisted
					: ERecoveryAction::RestoredOriginal,
				InStatus == EDurableStatus::PersistedUnverified,
				false);
		}

		int32 PrepareCalls = 0;
		int32 PersistCalls = 0;
		int32 RollbackCalls = 0;
		EDurableStatus Status =
			EDurableStatus::PersistedUnverified;
		TArray<int64> ObservedTokens;
	};

	struct FFixture
	{
		FFixture()
			: Resources(MakeTracker())
			, Cancellation([this]() { return Now; })
			, Plan(MakePlan())
			, ExecutionContext(
				MakeExecutionContext(
					Plan->GetDescriptorIdentity()))
		{
		}

		FExecutionMutationRequest MakeRequest(
			const FString& OperationId,
			const double TimeoutSeconds = 30.0)
		{
			return FExecutionMutationRequest(
				OperationId,
				TEXT("owned-worker:p4-006"),
				Plan,
				ExecutionContext,
				Resources,
				FUeAgentOperationDeadline::FromTimeout(
					TimeoutSeconds,
					[this]() { return Now; }),
				Cancellation.GetToken(),
				EExecutionThreadAffinity::GameThread,
				EExecutionReentrancyPolicy::RejectSameTarget);
		}

		double Now = 100.0;
		TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
			Resources;
		FUeAgentCancellationSource Cancellation;
		FPlanningPreflightPlanRef Plan;
		FUeAgentExecutionContextRefOwner ExecutionContext;
	};

	struct FCoordinatorFixture
	{
		explicit FCoordinatorFixture(const int32 MaxQueueItems = 1)
			: Scheduler(MakeShared<
				FFakeScheduler,
				ESPMode::ThreadSafe>())
			, Locks(MakeShared<
				FFakeLockFence,
				ESPMode::ThreadSafe>())
			, Barrier(MakeShared<
				FFakeBarrier,
				ESPMode::ThreadSafe>())
			, Durable(MakeShared<
				FFakeDurablePort,
				ESPMode::ThreadSafe>())
			, Coordinator(MakeShared<
				FExecutionMutationCoordinator,
				ESPMode::ThreadSafe>(
				Scheduler,
				Locks,
				Barrier,
				Durable,
				MaxQueueItems))
		{
		}

		TSharedRef<FFakeScheduler, ESPMode::ThreadSafe> Scheduler;
		TSharedRef<FFakeLockFence, ESPMode::ThreadSafe> Locks;
		TSharedRef<FFakeBarrier, ESPMode::ThreadSafe> Barrier;
		TSharedRef<FFakeDurablePort, ESPMode::ThreadSafe> Durable;
		TSharedRef<
			FExecutionMutationCoordinator,
			ESPMode::ThreadSafe> Coordinator;
	};
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentExecutionMutationModuleAvailableTest,
	"UeAgentInterface.Core.ExecutionMutation.ModuleAvailable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentExecutionMutationModuleAvailableTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::ExecutionMutation;

	TestEqual(
		TEXT("queued is an execution phase"),
		LexToString(EExecutionPhase::Queued),
		TEXT("queued"));
	TestEqual(
		TEXT("persisted_unverified is not complete"),
		LexToString(EExecutionStatus::PersistedUnverified),
		TEXT("persisted_unverified"));
	TestEqual(
		TEXT("pre-mutation cancellation proves no change"),
		LexToString(EExecutionStatus::CancelledNoChange),
		TEXT("cancelled_no_change"));
	TestEqual(
		TEXT("thread affinity is explicit"),
		LexToString(EExecutionThreadAffinity::GameThread),
		TEXT("game_thread"));
	TestEqual(
		TEXT("same-target reentrancy is rejected"),
		LexToString(EExecutionReentrancyPolicy::RejectSameTarget),
		TEXT("reject_same_target"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentExecutionMutationQueueFenceTest,
	"UeAgentInterface.Core.ExecutionMutation.QueueFence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentExecutionMutationQueueFenceTest::RunTest(
	const FString& Parameters)
{
	FFixture Fixture;
	FCoordinatorFixture Ports;
	const TSharedRef<FFakeMutationHook, ESPMode::ThreadSafe> FirstHook =
		MakeShared<FFakeMutationHook, ESPMode::ThreadSafe>();
	FString FirstId;
	FExecutionMutationError Error;
	TestTrue(
		TEXT("first operation enters bounded queue"),
		Ports.Coordinator->Submit(
			Fixture.MakeRequest(TEXT("operation:p4-006:001")),
			FirstHook,
			FirstId,
			Error));
	FString SecondId;
	TestFalse(
		TEXT("second operation is rejected before mutation"),
		Ports.Coordinator->Submit(
			Fixture.MakeRequest(TEXT("operation:p4-006:002")),
			MakeShared<FFakeMutationHook, ESPMode::ThreadSafe>(),
			SecondId,
			Error));
	TestEqual(
		TEXT("bounded queue failure code"),
		Error.GetCode(),
		TEXT("operation_queue_saturated"));
	TestEqual(
		TEXT("one operation scheduled"),
		Ports.Coordinator->DispatchReady(),
		1);
	TestEqual(TEXT("one callback pending"), Ports.Scheduler->Pending.Num(), 1);
	Ports.Scheduler->RunNext();
	FExecutionReceipt Receipt;
	TestTrue(
		TEXT("terminal receipt is available"),
		Ports.Coordinator->GetReceipt(FirstId, Receipt, Error));
	TestEqual(
		TEXT("durable result is not complete"),
		Receipt.GetStatus(),
		EExecutionStatus::PersistedUnverified);
	TestEqual(TEXT("mutation executes once"), FirstHook->ExecuteCalls, 1);
	TestEqual(TEXT("durable prepare once"), Ports.Durable->PrepareCalls, 1);
	TestEqual(TEXT("durable persist once"), Ports.Durable->PersistCalls, 1);
	TestEqual(TEXT("two fence observations"), Ports.Durable->ObservedTokens.Num(), 2);
	TestEqual(TEXT("prepare fence"), Ports.Durable->ObservedTokens[0], static_cast<int64>(41));
	TestEqual(TEXT("persist fence"), Ports.Durable->ObservedTokens[1], static_cast<int64>(41));
	TestEqual(TEXT("one lock acquisition"), Ports.Locks->AcquiredKeys.Num(), 1);
	TestTrue(
		TEXT("lock keys use canonical sorted order"),
		Ports.Locks->AcquiredKeys[0][0]
			< Ports.Locks->AcquiredKeys[0][1]);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentExecutionMutationLateMutationTest,
	"UeAgentInterface.Core.ExecutionMutation.LateMutation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentExecutionMutationLateMutationTest::RunTest(
	const FString& Parameters)
{
	FFixture Fixture;
	FCoordinatorFixture Ports;
	const TSharedRef<FFakeMutationHook, ESPMode::ThreadSafe> Hook =
		MakeShared<FFakeMutationHook, ESPMode::ThreadSafe>();
	FString OperationId;
	FExecutionMutationError Error;
	TestTrue(
		TEXT("operation submitted"),
		Ports.Coordinator->Submit(
			Fixture.MakeRequest(TEXT("operation:p4-006:late")),
			Hook,
			OperationId,
			Error));
	TestEqual(
		TEXT("callback scheduled"),
		Ports.Coordinator->DispatchReady(),
		1);
	Ports.Locks->Invalidate();
	Ports.Scheduler->RunNext();
	FExecutionReceipt Receipt;
	TestTrue(
		TEXT("late callback has a terminal receipt"),
		Ports.Coordinator->GetReceipt(OperationId, Receipt, Error));
	TestEqual(
		TEXT("stale fence blocks mutation"),
		Receipt.GetStatus(),
		EExecutionStatus::CancelledNoChange);
	TestEqual(
		TEXT("stale fence is explicit"),
		Receipt.GetErrorCode(),
		TEXT("operation_fence_stale"));
	TestFalse(TEXT("mutation never started"), Receipt.DidMutationStart());
	TestEqual(TEXT("hook not called"), Hook->ExecuteCalls, 0);
	TestEqual(TEXT("durable prepare not called"), Ports.Durable->PrepareCalls, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentExecutionMutationCancelBarrierTest,
	"UeAgentInterface.Core.ExecutionMutation.CancelBarrier",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentExecutionMutationCancelBarrierTest::RunTest(
	const FString& Parameters)
{
	{
		FFixture Fixture;
		FCoordinatorFixture Ports;
		const TSharedRef<FFakeMutationHook, ESPMode::ThreadSafe> Hook =
			MakeShared<FFakeMutationHook, ESPMode::ThreadSafe>();
		FString OperationId;
		FExecutionMutationError Error;
		TestTrue(
			TEXT("cancel fixture submitted"),
			Ports.Coordinator->Submit(
				Fixture.MakeRequest(TEXT("operation:p4-006:cancel")),
				Hook,
				OperationId,
				Error));
		Ports.Coordinator->DispatchReady();
		bool bAccepted = false;
		TestTrue(
			TEXT("coordinator cancellation accepted"),
			Ports.Coordinator->RequestCancellation(
				OperationId,
				TEXT("fixture_cancel"),
				bAccepted,
				Error));
		TestTrue(TEXT("cancel signal accepted"), bAccepted);
		Ports.Scheduler->RunNext();
		FExecutionReceipt Receipt;
		TestTrue(
			TEXT("cancel receipt available"),
			Ports.Coordinator->GetReceipt(OperationId, Receipt, Error));
		TestEqual(
			TEXT("cancel before callback proves no change"),
			Receipt.GetStatus(),
			EExecutionStatus::CancelledNoChange);
		TestEqual(TEXT("cancelled hook not called"), Hook->ExecuteCalls, 0);
	}
	{
		FFixture Fixture;
		FCoordinatorFixture Ports;
		Ports.Barrier->bGenerationMismatch = true;
		const TSharedRef<FFakeMutationHook, ESPMode::ThreadSafe> Hook =
			MakeShared<FFakeMutationHook, ESPMode::ThreadSafe>();
		FString OperationId;
		FExecutionMutationError Error;
		TestTrue(
			TEXT("barrier fixture submitted"),
			Ports.Coordinator->Submit(
				Fixture.MakeRequest(TEXT("operation:p4-006:barrier")),
				Hook,
				OperationId,
				Error));
		Ports.Coordinator->DispatchReady();
		Ports.Scheduler->RunNext();
		FExecutionReceipt Receipt;
		TestTrue(
			TEXT("barrier receipt available"),
			Ports.Coordinator->GetReceipt(OperationId, Receipt, Error));
		TestEqual(
			TEXT("generation mismatch rolls back"),
			Receipt.GetStatus(),
			EExecutionStatus::RolledBack);
		TestEqual(
			TEXT("barrier error is explicit"),
			Receipt.GetErrorCode(),
			TEXT("operation_barrier_unsettled"));
		TestEqual(TEXT("hook rollback once"), Hook->RollbackCalls, 1);
		TestEqual(TEXT("durable rollback once"), Ports.Durable->RollbackCalls, 1);
		TestEqual(TEXT("durable persist not called"), Ports.Durable->PersistCalls, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentExecutionMutationReentrancyDurableTest,
	"UeAgentInterface.Core.ExecutionMutation.ReentrancyDurable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentExecutionMutationReentrancyDurableTest::RunTest(
	const FString& Parameters)
{
	{
		FFixture Fixture;
		FCoordinatorFixture Ports;
		const TSharedRef<FFakeMutationHook, ESPMode::ThreadSafe> Hook =
			MakeShared<FFakeMutationHook, ESPMode::ThreadSafe>();
		FString NestedCode;
		Hook->OnExecute = [&]()
		{
			FString NestedId;
			FExecutionMutationError NestedError;
			if (!Ports.Coordinator->Submit(
				Fixture.MakeRequest(TEXT("operation:p4-006:nested")),
				MakeShared<
					FFakeMutationHook,
					ESPMode::ThreadSafe>(),
				NestedId,
				NestedError))
			{
				NestedCode = NestedError.GetCode();
			}
		};
		FString OperationId;
		FExecutionMutationError Error;
		TestTrue(
			TEXT("outer operation submitted"),
			Ports.Coordinator->Submit(
				Fixture.MakeRequest(TEXT("operation:p4-006:outer")),
				Hook,
				OperationId,
				Error));
		Ports.Coordinator->DispatchReady();
		Ports.Scheduler->RunNext();
		TestEqual(
			TEXT("same-target nested operation rejected"),
			NestedCode,
			TEXT("operation_reentrancy_rejected"));
	}
	{
		FFixture Fixture;
		FCoordinatorFixture Ports;
		Ports.Durable->Status = EDurableStatus::StateUnknown;
		FString OperationId;
		FExecutionMutationError Error;
		TestTrue(
			TEXT("state-unknown fixture submitted"),
			Ports.Coordinator->Submit(
				Fixture.MakeRequest(
					TEXT("operation:p4-006:state-unknown")),
				MakeShared<
					FFakeMutationHook,
					ESPMode::ThreadSafe>(),
				OperationId,
				Error));
		Ports.Coordinator->DispatchReady();
		Ports.Scheduler->RunNext();
		FExecutionReceipt Receipt;
		TestTrue(
			TEXT("state-unknown receipt available"),
			Ports.Coordinator->GetReceipt(OperationId, Receipt, Error));
		TestEqual(
			TEXT("state unknown is propagated"),
			Receipt.GetStatus(),
			EExecutionStatus::StateUnknown);
		TestFalse(TEXT("state unknown is not retry safe"), Receipt.IsRetrySafe());
		TestEqual(
			TEXT("terminal operation is not replayed"),
			Ports.Coordinator->DispatchReady(),
			0);
		TestEqual(TEXT("persist called once"), Ports.Durable->PersistCalls, 1);
	}
	return true;
}
