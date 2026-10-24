#include "Misc/AutomationTest.h"

#include "Core/Contract/UeAgentContract.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/ExecutionMutation/UeAgentLongRunningOperation.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::ExecutionMutation;

	TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> MakeResources()
	{
		return MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>();
	}

	FUeAgentDescriptorIdentity MakeDescriptor(const FString& Suffix)
	{
		return FUeAgentDescriptorIdentity(
			TEXT("uai.test.long_running.") + Suffix + TEXT("/v1"),
			1,
			TEXT("sha256:7f8cf7b38d339d5ce25d939234a89875939a26865cac51c9c9542ecb93fc2af6"));
	}

	FUeAgentExecutionContextRefOwner MakeContext(
		const FUeAgentDescriptorIdentity& Descriptor)
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("fixture-project:ls-001a");
		Input.EditorInstanceId = TEXT("fixture-editor:ls-001a");
		Input.StatePlane = EUeAgentStatePlane::Persisted;
		Input.ContextRevision = TEXT("fixture-context:ls-001a:r1");
		FUeAgentExecutionContextRefPtr Context;
		FUeAgentExecutionContextError Error;
		check(FUeAgentExecutionContextRef::TryCreate(
			Input,
			Descriptor,
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

	FLongRunningOperationRequest MakeRequest(
		const FString& OperationId,
		const FString& OwnerLeaseId,
		double& Now,
		const double TimeoutSeconds)
	{
		const FUeAgentDescriptorIdentity Descriptor =
			MakeDescriptor(OperationId);
		return FLongRunningOperationRequest(
			OperationId,
			OwnerLeaseId,
			TEXT("server-session:ls-001a"),
			Descriptor,
			MakeContext(Descriptor),
			MakeResources(),
			FUeAgentOperationDeadline::FromTimeout(
				TimeoutSeconds,
				[&Now]() { return Now; }),
			{TEXT("caller:fixture"), TEXT("request:") + OperationId,
				TEXT("editor.session"), TEXT("world:fixture")});
	}

	class FRecordingLongRunningPort final : public ILongRunningOperationPort
	{
	public:
		virtual bool Start(
			const FLongRunningOperationRequest& Request,
			const FUeAgentCancellationToken& Cancellation,
			FLongRunningWorkIdentity& OutIdentity,
			FExecutionMutationError& OutError) override
		{
			++StartCalls;
			StartedOperationIds.Add(Request.GetOperationId());
			ObservedCancellation =
				MakeUnique<FUeAgentCancellationToken>(Cancellation);
			OutIdentity = FLongRunningWorkIdentity(
				TEXT("resource:fixture:") + Request.GetOperationId(),
				NextGeneration++);
			return true;
		}

		virtual bool Observe(
			const FString& OperationId,
			const FLongRunningWorkIdentity& Identity,
			FUeAgentResourceTracker& Resources,
			FLongRunningWorkObservation& OutObservation,
			FExecutionMutationError& OutError) override
		{
			++ObserveCalls;
			ObservedGenerations.Add(Identity.GetGeneration());
			OutObservation = NextObservation.IsSet()
				? NextObservation.GetValue()
				: FLongRunningWorkObservation(
					Identity.GetGeneration(),
					ELongRunningWorkState::Running,
					FString());
			NextObservation.Reset();
			return true;
		}

		virtual bool RequestStop(
			const FString& OperationId,
			const FLongRunningWorkIdentity& Identity,
			const FString& Reason,
			FExecutionMutationError& OutError) override
		{
			++StopCalls;
			StopReasons.Add(Reason);
			return true;
		}

		virtual bool Join(
			const FString& OperationId,
			const FLongRunningWorkIdentity& Identity,
			FExecutionMutationError& OutError) override
		{
			++JoinCalls;
			return true;
		}

		virtual bool Release(
			const FString& OperationId,
			const FLongRunningWorkIdentity& Identity,
			FExecutionMutationError& OutError) override
		{
			++ReleaseCalls;
			return true;
		}

		void Complete(
			const int64 Generation,
			const ELongRunningWorkState State,
			const FString& ErrorCode = FString())
		{
			NextObservation = FLongRunningWorkObservation(
				Generation,
				State,
				ErrorCode);
		}

		int64 NextGeneration = 31;
		int32 StartCalls = 0;
		int32 ObserveCalls = 0;
		int32 StopCalls = 0;
		int32 JoinCalls = 0;
		int32 ReleaseCalls = 0;
		TArray<FString> StartedOperationIds;
		TArray<int64> ObservedGenerations;
		TArray<FString> StopReasons;
		TUniquePtr<FUeAgentCancellationToken> ObservedCancellation;
		TOptional<FLongRunningWorkObservation> NextObservation;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentLongRunningOperationOwnerTest,
	"UeAgentInterface.Core.ExecutionMutation.LongRunningOperation.OwnerContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentLongRunningOperationOwnerTest::RunTest(const FString& Parameters)
{
	double Now = 100.0;
	const TSharedRef<FRecordingLongRunningPort, ESPMode::ThreadSafe> Port =
		MakeShared<FRecordingLongRunningPort, ESPMode::ThreadSafe>();
	FLongRunningOperationOwner Owner(
		Port,
		[&Now]() { return Now; });
	FExecutionMutationError Error;
	FLongRunningOperationSnapshot Snapshot;
	const FLongRunningOperationRequest Request = MakeRequest(
		TEXT("operation:ls-001a:cancel"),
		TEXT("lease:ls-001a:owner"),
		Now,
		30.0);

	TestTrue(
		TEXT("accept records the operation without running it"),
		Owner.Accept(Request, Snapshot, Error));
	TestEqual(
		TEXT("accepted is explicitly nonterminal"),
		Snapshot.GetPhase(),
		ELongRunningOperationPhase::Accepted);
	TestFalse(TEXT("accepted has no terminal status"), Snapshot.IsTerminal());
	TestEqual(TEXT("accept does not start native work"), Port->StartCalls, 0);
	TestEqual(TEXT("snapshot binds the owner lease"),
		Snapshot.GetOwnerLeaseId(), Request.GetOwnerLeaseId());
	TestEqual(TEXT("snapshot binds the server session"),
		Snapshot.GetServerSessionId(), Request.GetServerSessionId());
	TestEqual(TEXT("snapshot binds the descriptor identity"),
		Snapshot.GetDescriptorIdentity().GetDescriptorId(),
		Request.GetDescriptorIdentity().GetDescriptorId());
	FLongRunningOperationReceipt Receipt;
	TestFalse(
		TEXT("accepted operation has no terminal receipt"),
		Owner.GetTerminalReceipt(
			Request.GetOperationId(),
			Request.GetOwnerLeaseId(),
			Request.GetServerSessionId(),
			Receipt,
			Error));
	TestEqual(
		TEXT("nonterminal receipt failure is stable"),
		Error.GetCode(),
		FString(TEXT("operation_not_terminal")));

	TestTrue(
		TEXT("the owning lease can reattach to a nonterminal operation"),
		Owner.GetSnapshot(
			Request.GetOperationId(),
			Request.GetOwnerLeaseId(),
			Request.GetServerSessionId(),
			Snapshot,
			Error));
	TestFalse(
		TEXT("a different lease cannot reattach"),
		Owner.GetSnapshot(
			Request.GetOperationId(),
			TEXT("lease:other"),
			Request.GetServerSessionId(),
			Snapshot,
			Error));
	TestEqual(
		TEXT("wrong-lease failure is stable"),
		Error.GetCode(),
		FString(TEXT("operation_owner_lease_mismatch")));
	TestFalse(
		TEXT("a different server session cannot reattach"),
		Owner.GetSnapshot(
			Request.GetOperationId(),
			Request.GetOwnerLeaseId(),
			TEXT("server-session:other"),
			Snapshot,
			Error));
	TestEqual(
		TEXT("wrong-session failure is stable"),
		Error.GetCode(),
		FString(TEXT("operation_server_session_mismatch")));

	TestTrue(
		TEXT("first advance starts the owned work"),
		Owner.Advance(
			Request.GetOperationId(),
			Request.GetOwnerLeaseId(),
			Request.GetServerSessionId(),
			0,
			Snapshot,
			Error));
	TestEqual(TEXT("work starts once"), Port->StartCalls, 1);
	TestEqual(
		TEXT("running snapshot carries the native generation"),
		Snapshot.GetGeneration(),
		int64(31));
	TestEqual(
		TEXT("running snapshot carries the native resource identity"),
		Snapshot.GetNativeResourceId(),
		FString(TEXT("resource:fixture:operation:ls-001a:cancel")));
	TestEqual(
		TEXT("start produces running, not complete"),
		Snapshot.GetPhase(),
		ELongRunningOperationPhase::Running);
	TestTrue(
		TEXT("the port keeps the shared dynamic cancellation token"),
		Port->ObservedCancellation.IsValid());

	const int32 ObserveCallsBeforeStale = Port->ObserveCalls;
	TestFalse(
		TEXT("a stale continuation is rejected before observing work"),
		Owner.Advance(
			Request.GetOperationId(),
			Request.GetOwnerLeaseId(),
			Request.GetServerSessionId(),
			30,
			Snapshot,
			Error));
	TestEqual(
		TEXT("stale generation failure is stable"),
		Error.GetCode(),
		FString(TEXT("operation_generation_mismatch")));
	TestEqual(
		TEXT("stale continuation performs no native observation"),
		Port->ObserveCalls,
		ObserveCallsBeforeStale);

	TestTrue(
		TEXT("cancellation requests a safe native stop"),
		Owner.RequestCancellation(
			Request.GetOperationId(),
			Request.GetOwnerLeaseId(),
			Request.GetServerSessionId(),
			TEXT("caller_cancelled"),
			Snapshot,
			Error));
	TestEqual(TEXT("stop is requested once"), Port->StopCalls, 1);
	TestEqual(
		TEXT("stop request remains nonterminal"),
		Snapshot.GetPhase(),
		ELongRunningOperationPhase::StopRequested);
	TestFalse(TEXT("stop request is not completion"), Snapshot.IsTerminal());
	TestTrue(
		TEXT("running work observes cancellation dynamically"),
		Port->ObservedCancellation->IsCancelled());

	Port->Complete(31, ELongRunningWorkState::Running);
	TestTrue(
		TEXT("a still-running resource remains nonterminal after cancel"),
		Owner.Advance(
			Request.GetOperationId(),
			Request.GetOwnerLeaseId(),
			Request.GetServerSessionId(),
			31,
			Snapshot,
			Error));
	TestFalse(TEXT("running resource is not joined"), Snapshot.IsTerminal());
	TestEqual(TEXT("join waits for exit"), Port->JoinCalls, 0);

	Port->Complete(31, ELongRunningWorkState::ExitedSuccess);
	TestTrue(
		TEXT("exit is joined and released before terminal cancellation"),
		Owner.Advance(
			Request.GetOperationId(),
			Request.GetOwnerLeaseId(),
			Request.GetServerSessionId(),
			31,
			Snapshot,
			Error));
	TestTrue(TEXT("joined operation is terminal"), Snapshot.IsTerminal());
	TestEqual(TEXT("native resource is joined once"), Port->JoinCalls, 1);
	TestEqual(TEXT("native resource is released once"), Port->ReleaseCalls, 1);
	TestTrue(
		TEXT("terminal receipt becomes available after join"),
		Owner.GetTerminalReceipt(
			Request.GetOperationId(),
			Request.GetOwnerLeaseId(),
			Request.GetServerSessionId(),
			Receipt,
			Error));
	TestEqual(
		TEXT("caller cancellation remains the terminal status"),
		Receipt.GetStatus(),
		ELongRunningOperationStatus::Cancelled);
	TestFalse(
		TEXT("started cancellation is not declared retry-safe"),
		Receipt.IsRetrySafe());
	TestTrue(
		TEXT("terminal continuation returns its existing outcome"),
		Owner.Advance(
			Request.GetOperationId(),
			Request.GetOwnerLeaseId(),
			Request.GetServerSessionId(),
			31,
			Snapshot,
			Error));
	TestTrue(TEXT("late continuation confirms completed cleanup"), Snapshot.IsCleanupComplete());
	TestEqual(TEXT("late continuation does not restart work"), Port->StartCalls, 1);
	TestTrue(
		TEXT("terminal receipt retention is explicitly releasable"),
		Owner.ReleaseTerminal(
			Request.GetOperationId(),
			Request.GetOwnerLeaseId(),
			Request.GetServerSessionId(),
			Error));
	TestFalse(
		TEXT("released terminal receipt no longer reattaches"),
		Owner.GetSnapshot(
			Request.GetOperationId(),
			Request.GetOwnerLeaseId(),
			Request.GetServerSessionId(),
			Snapshot,
			Error));
	TestEqual(
		TEXT("released terminal operation is absent"),
		Error.GetCode(),
		FString(TEXT("operation_not_found")));

	// One central owner must be able to bind a distinct spec-bound port to
	// each operation.  Otherwise every C11 resource would require a separate
	// registry owner and reattach would split across second truths.
	double DedicatedNow = 150.0;
	const TSharedRef<FRecordingLongRunningPort, ESPMode::ThreadSafe>
		DedicatedPort = MakeShared<
			FRecordingLongRunningPort,
			ESPMode::ThreadSafe>();
	const FLongRunningOperationRequest DedicatedRequest = MakeRequest(
		TEXT("operation:ls-001a:dedicated-port"),
		TEXT("lease:ls-001a:dedicated-port"),
		DedicatedNow,
		30.0);
	TestTrue(TEXT("central owner accepts an operation-scoped port"),
		Owner.Accept(
			DedicatedRequest,
			DedicatedPort,
			Snapshot,
			Error));
	TestTrue(TEXT("operation advances through its bound port"),
		Owner.Advance(
			DedicatedRequest.GetOperationId(),
			DedicatedRequest.GetOwnerLeaseId(),
			DedicatedRequest.GetServerSessionId(),
			0,
			Snapshot,
			Error));
	TestEqual(TEXT("dedicated port starts once"), DedicatedPort->StartCalls, 1);
	TestEqual(TEXT("default port is not reused for the dedicated operation"),
		Port->StartCalls, 1);

	// Unknown state is terminal but explicitly not replayable.  Advancing it
	// again must never start a second copy of the native work.
	double UnknownNow = 300.0;
	const TSharedRef<FRecordingLongRunningPort, ESPMode::ThreadSafe>
		UnknownPort = MakeShared<
			FRecordingLongRunningPort,
			ESPMode::ThreadSafe>();
	FLongRunningOperationOwner UnknownOwner(
		UnknownPort,
		[&UnknownNow]() { return UnknownNow; });
	const FLongRunningOperationRequest UnknownRequest = MakeRequest(
		TEXT("operation:ls-001a:unknown"),
		TEXT("lease:ls-001a:unknown"),
		UnknownNow,
		30.0);
	TestTrue(TEXT("unknown fixture accepts"),
		UnknownOwner.Accept(UnknownRequest, Snapshot, Error));
	TestTrue(TEXT("unknown fixture starts"),
		UnknownOwner.Advance(
			UnknownRequest.GetOperationId(),
			UnknownRequest.GetOwnerLeaseId(),
			UnknownRequest.GetServerSessionId(),
			0,
			Snapshot,
			Error));
	UnknownPort->Complete(
		31,
		ELongRunningWorkState::StateUnknown,
		TEXT("native_state_unknown"));
	TestTrue(TEXT("unknown observation is recorded"),
		UnknownOwner.Advance(
			UnknownRequest.GetOperationId(),
			UnknownRequest.GetOwnerLeaseId(),
			UnknownRequest.GetServerSessionId(),
			31,
			Snapshot,
			Error));
	TestTrue(TEXT("unknown observation is terminal"), Snapshot.IsTerminal());
	TestTrue(TEXT("unknown receipt is readable"),
		UnknownOwner.GetTerminalReceipt(
			UnknownRequest.GetOperationId(),
			UnknownRequest.GetOwnerLeaseId(),
			UnknownRequest.GetServerSessionId(),
			Receipt,
			Error));
	TestEqual(TEXT("unknown status is preserved"), Receipt.GetStatus(),
		ELongRunningOperationStatus::StateUnknown);
	TestFalse(TEXT("unknown state is never retry-safe"), Receipt.IsRetrySafe());
	TestTrue(TEXT("unknown operation returns its retained result without replay"),
		UnknownOwner.Advance(
			UnknownRequest.GetOperationId(),
			UnknownRequest.GetOwnerLeaseId(),
			UnknownRequest.GetServerSessionId(),
			31,
			Snapshot,
			Error));
	TestEqual(TEXT("unknown work started exactly once"), UnknownPort->StartCalls, 1);
	TestTrue(TEXT("unknown outcome still requires successful cleanup"), Snapshot.IsCleanupComplete());
	TestEqual(TEXT("discovery is scoped to the trusted caller"),
		UnknownOwner.Discover(TEXT("caller:other")).Num(), 0);
	TestEqual(TEXT("caller can find its retained terminal operation"),
		UnknownOwner.Discover(TEXT("caller:fixture")).Num(), 1);
	TestTrue(TEXT("lost response recovers the original request"),
		UnknownOwner.Recover(TEXT("caller:fixture"), TEXT("request:") + UnknownRequest.GetOperationId(),
			TEXT("editor.session"), Snapshot, Error));
	TestTrue(TEXT("explicit takeover rotates custody"),
		UnknownOwner.TakeOver(UnknownRequest.GetOperationId(), TEXT("caller:fixture"),
			TEXT("lease:replacement"), TEXT("request:takeover"), Snapshot, Error));
	TestFalse(TEXT("old handle loses access after takeover"),
		UnknownOwner.GetSnapshot(UnknownRequest.GetOperationId(), UnknownRequest.GetOwnerLeaseId(),
			UnknownRequest.GetServerSessionId(), Snapshot, Error));
	TestTrue(TEXT("lost takeover reply recovers the new custody"),
		UnknownOwner.Recover(TEXT("caller:fixture"), TEXT("request:takeover"),
			TEXT("operation.takeover"), Snapshot, Error));
	TestEqual(TEXT("recovered takeover has the current lease"), Snapshot.GetOwnerLeaseId(),
		FString(TEXT("lease:replacement")));
	TestTrue(TEXT("late cancellation preserves the terminal outcome"),
		UnknownOwner.RequestCancellation(UnknownRequest.GetOperationId(), TEXT("lease:replacement"),
			UnknownRequest.GetServerSessionId(), TEXT("close"), Snapshot, Error));
	TestEqual(TEXT("late cancellation does not stop exited native work"), UnknownPort->StopCalls, 0);
	TestTrue(TEXT("new custodian releases the retained record"),
		UnknownOwner.ReleaseTerminal(UnknownRequest.GetOperationId(), TEXT("lease:replacement"),
			UnknownRequest.GetServerSessionId(), Error));
	TestEqual(TEXT("released records are absent from discovery"),
		UnknownOwner.Discover(TEXT("caller:fixture")).Num(), 0);
	return !HasAnyErrors();
}
