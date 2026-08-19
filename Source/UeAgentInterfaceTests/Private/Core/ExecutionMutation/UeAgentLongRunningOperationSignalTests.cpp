#include "Misc/AutomationTest.h"

#include "Core/Contract/UeAgentContract.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/ExecutionMutation/UeAgentLongRunningOperation.h"

#include "Core/StateSemantic/UeAgentStateSemantic.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::StateSemantic;

	TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> MakeResources()
	{
		return MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>(
			FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 1024 * 1024},
				{EUeAgentResourceDimension::StringBytes, 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 4096},
				{EUeAgentResourceDimension::Items, 4096},
				{EUeAgentResourceDimension::Files, 256},
				{EUeAgentResourceDimension::ReferenceEdges, 4096},
				{EUeAgentResourceDimension::WorkUnits, 65536},
				{EUeAgentResourceDimension::EstimatedMemoryBytes, 16 * 1024 * 1024},
			}));
	}

	FUeAgentDescriptorIdentity MakeDescriptor(const FString& Suffix)
	{
		return FUeAgentDescriptorIdentity(
			TEXT("uai.test.long_running_signal.") + Suffix + TEXT("/v1"),
			1,
			TEXT("sha256:c20f5ff1e690ef98b27bc2298b386be06b387783b45c118d026e7824b9d2e953"));
	}

	FUeAgentExecutionContextRefOwner MakeContext(
		const FUeAgentDescriptorIdentity& Descriptor)
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("fixture-project:ls-001c");
		Input.EditorInstanceId = TEXT("fixture-editor:ls-001c");
		Input.StatePlane = EUeAgentStatePlane::Persisted;
		Input.ContextRevision = TEXT("fixture-context:ls-001c:r1");
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
		double& Now)
	{
		const FUeAgentDescriptorIdentity Descriptor =
			MakeDescriptor(OperationId);
		return FLongRunningOperationRequest(
			OperationId,
			TEXT("lease:ls-001c:owner"),
			TEXT("server-session:ls-001c"),
			Descriptor,
			MakeContext(Descriptor),
			MakeResources(),
			FUeAgentOperationDeadline::FromTimeout(
				30.0,
				[&Now]() { return Now; }));
	}

	class FSignalPort final : public ILongRunningOperationPort
	{
	public:
		virtual bool Start(
			const FLongRunningOperationRequest& Request,
			const FUeAgentCancellationToken& Cancellation,
			FLongRunningWorkIdentity& OutIdentity,
			FExecutionMutationError& OutError) override
		{
			++StartCalls;
			OutIdentity = FLongRunningWorkIdentity(
				TEXT("resource:ls-001c:") + Request.GetOperationId(),
				Generation);
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
			if (!QueuedSignal.IsSet())
			{
				OutObservation = FLongRunningWorkObservation(
					QueuedGeneration,
					QueuedState,
					QueuedErrorCode);
				ResetQueue();
				return true;
			}

			FUeAgentCanonicalValuePtr Payload;
			FUeAgentStateSemanticError CanonicalError;
			const FQueuedSignal Signal = QueuedSignal.GetValue();
			if (!FUeAgentCanonicalValue::TryString(
				Signal.Payload,
				Resources,
				Payload,
				CanonicalError))
			{
				OutError = FExecutionMutationError(
					CanonicalError.Code,
					CanonicalError.Detail);
				return false;
			}
			OutObservation = FLongRunningWorkObservation(
				QueuedGeneration,
				QueuedState,
				QueuedErrorCode,
				TOptional<FLongRunningOperationSignal>(
					FLongRunningOperationSignal(
						Signal.TypeId,
						Signal.Revision,
						Payload.ToSharedRef())));
			ResetQueue();
			return true;
		}

		virtual bool RequestStop(
			const FString& OperationId,
			const FLongRunningWorkIdentity& Identity,
			const FString& Reason,
			FExecutionMutationError& OutError) override
		{
			++StopCalls;
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

		void EmitSignal(
			const int64 Revision,
			FString TypeId,
			FString Payload,
			const int64 ObservationGeneration = 41)
		{
			QueuedGeneration = ObservationGeneration;
			QueuedState = ELongRunningWorkState::Running;
			QueuedErrorCode.Reset();
			QueuedSignal.Emplace(FQueuedSignal{
				Revision,
				MoveTemp(TypeId),
				MoveTemp(Payload),
			});
		}

		void EmitTerminalSuccess()
		{
			QueuedGeneration = Generation;
			QueuedState = ELongRunningWorkState::ExitedSuccess;
			QueuedErrorCode.Reset();
			QueuedSignal.Reset();
		}

		int64 Generation = 41;
		int32 StartCalls = 0;
		int32 ObserveCalls = 0;
		int32 StopCalls = 0;
		int32 JoinCalls = 0;
		int32 ReleaseCalls = 0;

	private:
		struct FQueuedSignal
		{
			int64 Revision = 0;
			FString TypeId;
			FString Payload;
		};

		void ResetQueue()
		{
			QueuedGeneration = Generation;
			QueuedState = ELongRunningWorkState::Running;
			QueuedErrorCode.Reset();
			QueuedSignal.Reset();
		}

		int64 QueuedGeneration = 41;
		ELongRunningWorkState QueuedState =
			ELongRunningWorkState::Running;
		FString QueuedErrorCode;
		TOptional<FQueuedSignal> QueuedSignal;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentLongRunningOperationTypedSignalTest,
	"UeAgentInterface.Core.ExecutionMutation.LongRunningOperation.TypedSignalContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentLongRunningOperationTypedSignalTest::RunTest(
	const FString& Parameters)
{
	double Now = 100.0;
	const TSharedRef<FSignalPort, ESPMode::ThreadSafe> Port =
		MakeShared<FSignalPort, ESPMode::ThreadSafe>();
	FLongRunningOperationOwner Owner(
		Port,
		[&Now]() { return Now; });
	const FLongRunningOperationRequest Request = MakeRequest(
		TEXT("operation:ls-001c:latest-signal"),
		Now);
	FLongRunningOperationSnapshot Snapshot;
	FExecutionMutationError Error;

	TestTrue(TEXT("accept succeeds"),
		Owner.Accept(Request, Snapshot, Error));
	TestFalse(TEXT("accepted operation has no signal"),
		Snapshot.GetLatestSignal().IsSet());
	TestTrue(TEXT("first advance starts native work"),
		Owner.Advance(
			Request.GetOperationId(),
			Request.GetOwnerLeaseId(),
			Request.GetServerSessionId(),
			0,
			Snapshot,
			Error));
	TestFalse(TEXT("start alone invents no signal"),
		Snapshot.GetLatestSignal().IsSet());

	Port->EmitSignal(
		1,
		TEXT("uai.compile.progress/v1"),
		TEXT("40"));
	TestTrue(TEXT("current-generation signal is observed"),
		Owner.Advance(
			Request.GetOperationId(),
			Request.GetOwnerLeaseId(),
			Request.GetServerSessionId(),
			41,
			Snapshot,
			Error));
	TestTrue(TEXT("snapshot exposes the latest typed signal"),
		Snapshot.GetLatestSignal().IsSet());
	if (Snapshot.GetLatestSignal().IsSet())
	{
		const FLongRunningOperationSignal& Signal =
			Snapshot.GetLatestSignal().GetValue();
		TestEqual(TEXT("signal type is preserved without interpretation"),
			Signal.GetTypeId(),
			FString(TEXT("uai.compile.progress/v1")));
		TestEqual(TEXT("signal revision is preserved"),
			Signal.GetRevision(), int64(1));
		TestEqual(TEXT("signal payload remains a C4 canonical value"),
			Signal.GetPayload()->GetKind(),
			EUeAgentValueKind::String);
		TestEqual(TEXT("signal payload is lossless"),
			Signal.GetPayload()->GetText(), FString(TEXT("40")));
	}

	FLongRunningOperationSnapshot Reattached;
	TestFalse(TEXT("wrong session cannot read a signal"),
		Owner.GetSnapshot(
			Request.GetOperationId(),
			Request.GetOwnerLeaseId(),
			TEXT("server-session:other"),
			Reattached,
			Error));
	TestFalse(TEXT("failed read returns no leaked signal"),
		Reattached.GetLatestSignal().IsSet());
	TestTrue(TEXT("owning lease and session can reattach"),
		Owner.GetSnapshot(
			Request.GetOperationId(),
			Request.GetOwnerLeaseId(),
			Request.GetServerSessionId(),
			Reattached,
			Error));
	if (Reattached.GetLatestSignal().IsSet())
	{
		TestEqual(TEXT("reattach preserves signal revision"),
			Reattached.GetLatestSignal().GetValue().GetRevision(), int64(1));
	}

	Port->EmitSignal(
		2,
		TEXT("uai.external_process.report_ref/v1"),
		TEXT("artifact:compile-report:42"));
	TestTrue(TEXT("a newer opaque signal replaces the previous signal"),
		Owner.Advance(
			Request.GetOperationId(),
			Request.GetOwnerLeaseId(),
			Request.GetServerSessionId(),
			41,
			Snapshot,
			Error));
	TestTrue(TEXT("newer observation still exposes a signal"),
		Snapshot.GetLatestSignal().IsSet());
	if (Snapshot.GetLatestSignal().IsSet())
	{
		TestEqual(TEXT("only the latest signal is retained"),
			Snapshot.GetLatestSignal().GetValue().GetRevision(), int64(2));
		TestEqual(TEXT("C7 does not restrict domain signal types"),
			Snapshot.GetLatestSignal().GetValue().GetTypeId(),
			FString(TEXT("uai.external_process.report_ref/v1")));
	}
	TestFalse(TEXT("a signal cannot complete an operation"),
		Snapshot.IsTerminal());

	Port->EmitTerminalSuccess();
	TestTrue(TEXT("native exit completes through lifecycle state"),
		Owner.Advance(
			Request.GetOperationId(),
			Request.GetOwnerLeaseId(),
			Request.GetServerSessionId(),
			41,
			Snapshot,
			Error));
	TestTrue(TEXT("operation becomes terminal only after join/release"),
		Snapshot.IsTerminal());
	TestEqual(TEXT("join executes exactly once"), Port->JoinCalls, 1);
	TestEqual(TEXT("release executes exactly once"), Port->ReleaseCalls, 1);
	TestTrue(TEXT("terminal snapshot still exposes the latest signal"),
		Snapshot.GetLatestSignal().IsSet());
	if (Snapshot.GetLatestSignal().IsSet())
	{
		TestEqual(TEXT("terminal snapshot retains latest signal until release"),
			Snapshot.GetLatestSignal().GetValue().GetRevision(), int64(2));
	}
	FLongRunningOperationReceipt Receipt;
	TestTrue(TEXT("terminal receipt remains a lifecycle receipt"),
		Owner.GetTerminalReceipt(
			Request.GetOperationId(),
			Request.GetOwnerLeaseId(),
			Request.GetServerSessionId(),
			Receipt,
			Error));
	TestEqual(TEXT("typed signal does not rewrite terminal outcome"),
		Receipt.GetStatus(), ELongRunningOperationStatus::Succeeded);

	double StaleNow = 200.0;
	const TSharedRef<FSignalPort, ESPMode::ThreadSafe> StalePort =
		MakeShared<FSignalPort, ESPMode::ThreadSafe>();
	FLongRunningOperationOwner StaleOwner(
		StalePort,
		[&StaleNow]() { return StaleNow; });
	const FLongRunningOperationRequest StaleRequest = MakeRequest(
		TEXT("operation:ls-001c:stale-revision"),
		StaleNow);
	TestTrue(TEXT("stale-revision fixture accepts"),
		StaleOwner.Accept(StaleRequest, Snapshot, Error));
	TestTrue(TEXT("stale-revision fixture starts"),
		StaleOwner.Advance(
			StaleRequest.GetOperationId(),
			StaleRequest.GetOwnerLeaseId(),
			StaleRequest.GetServerSessionId(),
			0,
			Snapshot,
			Error));
	StalePort->EmitSignal(
		2,
		TEXT("uai.compile.progress/v1"),
		TEXT("80"));
	TestTrue(TEXT("initial positive revision is accepted"),
		StaleOwner.Advance(
			StaleRequest.GetOperationId(),
			StaleRequest.GetOwnerLeaseId(),
			StaleRequest.GetServerSessionId(),
			41,
			Snapshot,
			Error));
	StalePort->EmitSignal(
		1,
		TEXT("uai.compile.progress/v1"),
		TEXT("20"));
	TestTrue(TEXT("stale signal requests a safe native stop"),
		StaleOwner.Advance(
			StaleRequest.GetOperationId(),
			StaleRequest.GetOwnerLeaseId(),
			StaleRequest.GetServerSessionId(),
			41,
			Snapshot,
			Error));
	TestFalse(TEXT("stale signal is not terminal before native exit"),
		Snapshot.IsTerminal());
	TestEqual(TEXT("stale signal enters stop-requested phase"),
		Snapshot.GetPhase(),
		ELongRunningOperationPhase::StopRequested);
	TestEqual(TEXT("stale signal requests stop exactly once"),
		StalePort->StopCalls, 1);
	TestTrue(TEXT("state_unknown snapshot keeps the prior valid signal"),
		Snapshot.GetLatestSignal().IsSet());
	if (Snapshot.GetLatestSignal().IsSet())
	{
		TestEqual(TEXT("stale signal cannot overwrite the latest value"),
			Snapshot.GetLatestSignal().GetValue().GetRevision(), int64(2));
	}
	StalePort->EmitTerminalSuccess();
	TestTrue(TEXT("native exit is joined and released after signal failure"),
		StaleOwner.Advance(
			StaleRequest.GetOperationId(),
			StaleRequest.GetOwnerLeaseId(),
			StaleRequest.GetServerSessionId(),
			41,
			Snapshot,
			Error));
	TestTrue(TEXT("signal failure becomes terminal only at safe boundary"),
		Snapshot.IsTerminal());
	TestEqual(TEXT("stale signal terminal status is explicit"),
		Snapshot.GetStatus(),
		ELongRunningOperationStatus::StateUnknown);
	TestEqual(TEXT("stale signal has a stable failure code"),
		Snapshot.GetErrorCode(),
		FString(TEXT("native_operation_signal_revision_invalid")));
	TestEqual(TEXT("signal-failure path joins exactly once"),
		StalePort->JoinCalls, 1);
	TestEqual(TEXT("signal-failure path releases exactly once"),
		StalePort->ReleaseCalls, 1);
	const int32 ObservationsAtTerminal = StalePort->ObserveCalls;
	TestFalse(TEXT("state_unknown cannot replay or continue"),
		StaleOwner.Advance(
			StaleRequest.GetOperationId(),
			StaleRequest.GetOwnerLeaseId(),
			StaleRequest.GetServerSessionId(),
			41,
			Snapshot,
			Error));
	TestEqual(TEXT("late continuation performs no native observation"),
		StalePort->ObserveCalls,
		ObservationsAtTerminal);

	double WrongGenerationNow = 300.0;
	const TSharedRef<FSignalPort, ESPMode::ThreadSafe> WrongGenerationPort =
		MakeShared<FSignalPort, ESPMode::ThreadSafe>();
	FLongRunningOperationOwner WrongGenerationOwner(
		WrongGenerationPort,
		[&WrongGenerationNow]() { return WrongGenerationNow; });
	const FLongRunningOperationRequest WrongGenerationRequest = MakeRequest(
		TEXT("operation:ls-001c:wrong-signal-generation"),
		WrongGenerationNow);
	TestTrue(TEXT("wrong-generation fixture accepts"),
		WrongGenerationOwner.Accept(
			WrongGenerationRequest,
			Snapshot,
			Error));
	TestTrue(TEXT("wrong-generation fixture starts"),
		WrongGenerationOwner.Advance(
			WrongGenerationRequest.GetOperationId(),
			WrongGenerationRequest.GetOwnerLeaseId(),
			WrongGenerationRequest.GetServerSessionId(),
			0,
			Snapshot,
			Error));
	WrongGenerationPort->EmitSignal(
		1,
		TEXT("uai.compile.progress/v1"),
		TEXT("10"),
		40);
	TestTrue(TEXT("wrong observation generation requests safe stop"),
		WrongGenerationOwner.Advance(
			WrongGenerationRequest.GetOperationId(),
			WrongGenerationRequest.GetOwnerLeaseId(),
			WrongGenerationRequest.GetServerSessionId(),
			41,
			Snapshot,
			Error));
	TestFalse(TEXT("wrong generation is not terminal before native exit"),
		Snapshot.IsTerminal());
	TestEqual(TEXT("wrong generation requests native stop once"),
		WrongGenerationPort->StopCalls, 1);
	TestFalse(TEXT("wrong-generation signal is never retained"),
		Snapshot.GetLatestSignal().IsSet());
	WrongGenerationPort->EmitTerminalSuccess();
	TestTrue(TEXT("wrong-generation work reaches a safe boundary"),
		WrongGenerationOwner.Advance(
			WrongGenerationRequest.GetOperationId(),
			WrongGenerationRequest.GetOwnerLeaseId(),
			WrongGenerationRequest.GetServerSessionId(),
			41,
			Snapshot,
			Error));
	TestTrue(TEXT("wrong generation terminates after join/release"),
		Snapshot.IsTerminal());
	TestEqual(TEXT("wrong generation is state_unknown"),
		Snapshot.GetStatus(),
		ELongRunningOperationStatus::StateUnknown);
	TestEqual(TEXT("wrong generation has a stable error"),
		Snapshot.GetErrorCode(),
		FString(TEXT("native_observation_invalid")));
	TestEqual(TEXT("wrong-generation path joins once"),
		WrongGenerationPort->JoinCalls, 1);
	TestEqual(TEXT("wrong-generation path releases once"),
		WrongGenerationPort->ReleaseCalls, 1);

	double InvalidSignalNow = 400.0;
	const TSharedRef<FSignalPort, ESPMode::ThreadSafe> InvalidSignalPort =
		MakeShared<FSignalPort, ESPMode::ThreadSafe>();
	FLongRunningOperationOwner InvalidSignalOwner(
		InvalidSignalPort,
		[&InvalidSignalNow]() { return InvalidSignalNow; });
	const FLongRunningOperationRequest InvalidSignalRequest = MakeRequest(
		TEXT("operation:ls-001c:invalid-signal"),
		InvalidSignalNow);
	TestTrue(TEXT("invalid-signal fixture accepts"),
		InvalidSignalOwner.Accept(InvalidSignalRequest, Snapshot, Error));
	TestTrue(TEXT("invalid-signal fixture starts"),
		InvalidSignalOwner.Advance(
			InvalidSignalRequest.GetOperationId(),
			InvalidSignalRequest.GetOwnerLeaseId(),
			InvalidSignalRequest.GetServerSessionId(),
			0,
			Snapshot,
			Error));
	InvalidSignalPort->EmitSignal(1, TEXT("   "), TEXT("invalid"));
	TestTrue(TEXT("invalid typed signal requests safe stop"),
		InvalidSignalOwner.Advance(
			InvalidSignalRequest.GetOperationId(),
			InvalidSignalRequest.GetOwnerLeaseId(),
			InvalidSignalRequest.GetServerSessionId(),
			41,
			Snapshot,
			Error));
	TestFalse(TEXT("invalid signal is not terminal before native exit"),
		Snapshot.IsTerminal());
	TestEqual(TEXT("invalid signal requests native stop once"),
		InvalidSignalPort->StopCalls, 1);
	TestFalse(TEXT("invalid signal is never retained"),
		Snapshot.GetLatestSignal().IsSet());
	InvalidSignalPort->EmitTerminalSuccess();
	TestTrue(TEXT("invalid-signal work reaches a safe boundary"),
		InvalidSignalOwner.Advance(
			InvalidSignalRequest.GetOperationId(),
			InvalidSignalRequest.GetOwnerLeaseId(),
			InvalidSignalRequest.GetServerSessionId(),
			41,
			Snapshot,
			Error));
	TestEqual(TEXT("invalid signal is state_unknown"),
		Snapshot.GetStatus(),
		ELongRunningOperationStatus::StateUnknown);
	TestEqual(TEXT("invalid signal has a stable error"),
		Snapshot.GetErrorCode(),
		FString(TEXT("native_operation_signal_invalid")));
	TestEqual(TEXT("invalid-signal path joins once"),
		InvalidSignalPort->JoinCalls, 1);
	TestEqual(TEXT("invalid-signal path releases once"),
		InvalidSignalPort->ReleaseCalls, 1);

	return !HasAnyErrors();
}
