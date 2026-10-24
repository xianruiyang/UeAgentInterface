#include "LifecycleHost/UeAgentLifecycleHost.h"

#include "Misc/AutomationTest.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::LifecycleHost;
	using namespace UE::UAI::NetworkTransport;

	static FUeAgentProcessIdentity WorkerProcess()
	{
		return {
			41001,
			TEXT("2026-07-27T03:00:00.000000Z"),
			TEXT("UnrealEditor-Cmd.exe"),
		};
	}

	static FUeAgentOwnedWorkerSpec WorkerSpec(EUeAgentRenderMode RenderMode)
	{
		return {
			TEXT("owned-worker-001"),
			TEXT("automation-session-001"),
			TEXT("D:/UE/Engine/Binaries/Win64/UnrealEditor-Cmd.exe"),
			TEXT("D:/program/UE/GptProjectTest/GptProjectTest.uproject"),
			{TEXT("-unattended"), TEXT("-nop4")},
			RenderMode,
		};
	}

	class FRecordingProcessBackend final : public IUeAgentProcessLifecycleBackend
	{
	public:
		explicit FRecordingProcessBackend(TArray<FString>& InEvents)
			: Events(InEvents)
		{
		}

		virtual bool StartOwned(
			const FUeAgentOwnedWorkerSpec& Spec,
			FUeAgentProcessIdentity& OutIdentity,
			FUeAgentLifecycleDiagnostic& OutDiagnostic) override
		{
			OutIdentity = WorkerProcess();
			bAlive = true;
			Events.Add(FString::Printf(
				TEXT("process:start:%u"),
				OutIdentity.ProcessId));
			return true;
		}

		virtual bool StopOwned(
			const FUeAgentProcessIdentity& Identity,
			FUeAgentLifecycleDiagnostic& OutDiagnostic) override
		{
			if (!Identity.Matches(WorkerProcess()))
			{
				OutDiagnostic = FUeAgentLifecycleDiagnostic::Failure(
					TEXT("process_identity_mismatch"),
					TEXT("fixture refuses a different process identity"));
				return false;
			}
			Events.Add(FString::Printf(
				TEXT("process:stop:%u"),
				Identity.ProcessId));
			bAlive = false;
			return true;
		}

		virtual bool IsAlive(const FUeAgentProcessIdentity& Identity) const override
		{
			return bAlive && Identity.Matches(WorkerProcess());
		}

	private:
		TArray<FString>& Events;
		bool bAlive = false;
	};

	class FRecordingTickerBackend final : public IUeAgentTickerLifecycleBackend
	{
	public:
		explicit FRecordingTickerBackend(TArray<FString>& InEvents)
			: Events(InEvents)
		{
		}

		virtual bool Register(
			const FUeAgentTickerIdentity& Identity,
			FUeAgentLifecycleDiagnostic& OutDiagnostic) override
		{
			Registered.Add(Identity.TickerId, Identity);
			Events.Add(TEXT("ticker:register:") + Identity.TickerId);
			return true;
		}

		virtual bool Unregister(
			const FUeAgentTickerIdentity& Identity,
			FUeAgentLifecycleDiagnostic& OutDiagnostic) override
		{
			if (bFailNextUnregister)
			{
				bFailNextUnregister = false;
				Events.Add(
					TEXT("ticker:unregister_failed:")
					+ Identity.TickerId);
				OutDiagnostic = FUeAgentLifecycleDiagnostic::Failure(
					TEXT("fixture_ticker_unregister_failed"),
					TEXT("fixture keeps the ticker registered"));
				return false;
			}
			const FUeAgentTickerIdentity* Existing =
				Registered.Find(Identity.TickerId);
			if (!Existing
				|| Existing->OwnerId != Identity.OwnerId
				|| Existing->HostId != Identity.HostId)
			{
				OutDiagnostic = FUeAgentLifecycleDiagnostic::Failure(
					TEXT("ticker_identity_mismatch"),
					TEXT("fixture refuses a non-owned ticker"));
				return false;
			}
			Events.Add(TEXT("ticker:unregister:") + Identity.TickerId);
			Registered.Remove(Identity.TickerId);
			return true;
		}

		void FailNextUnregister()
		{
			bFailNextUnregister = true;
		}

	private:
		TArray<FString>& Events;
		TMap<FString, FUeAgentTickerIdentity> Registered;
		bool bFailNextUnregister = false;
	};

	class FRecordingServerControl final : public IUeAgentNetworkServerControl
	{
	public:
		FRecordingServerControl(
			FString InOwnerId,
			FString InInstanceId,
			TArray<FString>& InEvents,
			bool bInRunning = false,
			FString InStartedInstanceId = FString())
			: ServerIdentity{MoveTemp(InOwnerId), MoveTemp(InInstanceId)}
			, Events(InEvents)
			, bRunning(bInRunning)
			, StartedInstanceId(MoveTemp(InStartedInstanceId))
		{
		}

		virtual bool Start(
			FUeAgentTransportServerIdentity& OutIdentity,
			FUeAgentTransportDiagnostic& OutDiagnostic) override
		{
			Events.Add(TEXT("server:start:") + ServerIdentity.OwnerId);
			bRunning = true;
			OutIdentity = ServerIdentity;
			if (!StartedInstanceId.IsEmpty())
			{
				OutIdentity.InstanceId = StartedInstanceId;
			}
			return true;
		}

		virtual bool Stop(
			const FString& OwnerId,
			FUeAgentTransportDiagnostic& OutDiagnostic) override
		{
			if (OwnerId != ServerIdentity.OwnerId)
			{
				OutDiagnostic = {
					TEXT("transport_server_owner_mismatch"),
					TEXT("fixture refuses a different owner"),
				};
				return false;
			}
			Events.Add(TEXT("server:stop:") + OwnerId);
			bRunning = false;
			return true;
		}

		virtual FUeAgentTransportServerHealth Health() const override
		{
			return {bRunning, {}};
		}

		virtual FUeAgentTransportServerIdentity Identity() const override
		{
			return ServerIdentity;
		}

	private:
		FUeAgentTransportServerIdentity ServerIdentity;
		TArray<FString>& Events;
		bool bRunning = false;
		FString StartedInstanceId;
	};

	struct FLifecycleFixture
	{
		FLifecycleFixture()
			: Deadline(FUeAgentOperationDeadline::FromTimeout(
				30.0,
				[this]() { return Now; }))
			, Cancellation([this]() { return Now; })
			, Process(MakeShared<FRecordingProcessBackend>(Events))
			, Ticker(MakeShared<FRecordingTickerBackend>(Events))
			, Host(
				TEXT("c11-run-001"),
				Deadline,
				Cancellation.GetToken(),
				Process,
				Ticker)
		{
		}

		double Now = 100.0;
		TArray<FString> Events;
		FUeAgentOperationDeadline Deadline;
		FUeAgentCancellationSource Cancellation;
		TSharedRef<FRecordingProcessBackend> Process;
		TSharedRef<FRecordingTickerBackend> Ticker;
		FUeAgentLifecycleHost Host;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentLifecycleHostProcessIdentityTest,
	"UeAgentInterface.LifecycleHost.ProcessIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentLifecycleHostProcessIdentityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FUeAgentProcessIdentity Recorded = WorkerProcess();
	const FUeAgentProcessIdentity Reused{
		Recorded.ProcessId,
		TEXT("2026-07-27T04:00:00.000000Z"),
		Recorded.Executable,
	};
	TestFalse(TEXT("PID reuse is not the same process"), Recorded.Matches(Reused));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentLifecycleHostOwnedServerTest,
	"UeAgentInterface.LifecycleHost.OwnedServer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentLifecycleHostOwnedServerTest::RunTest(const FString& Parameters)
{
	FLifecycleFixture Fixture;
	FRecordingServerControl OwnedServer(
		TEXT("c11-run-001"),
		TEXT("transport-server-001"),
		Fixture.Events);
	FUeAgentHostIdentity HostIdentity;
	FUeAgentLifecycleDiagnostic Diagnostic;
	TestTrue(
		TEXT("owned worker and server start"),
		Fixture.Host.StartOwnedWorker(
			WorkerSpec(EUeAgentRenderMode::Rhi),
			OwnedServer,
			HostIdentity,
			Diagnostic));
	TestEqual(
		TEXT("worker host kind"),
		HostIdentity.Kind,
		EUeAgentHostKind::OwnedWorker);
	TestEqual(
		TEXT("server owner"),
		Fixture.Host.ServerIdentity()->OwnerId,
		FString(TEXT("c11-run-001")));
	TestEqual(
		TEXT("process starts before server"),
		Fixture.Events,
		TArray<FString>({
			TEXT("process:start:41001"),
			TEXT("server:start:c11-run-001"),
		}));

	FLifecycleFixture UnknownFixture;
	FRecordingServerControl UnknownServer(
		TEXT("unknown-main-editor-owner"),
		TEXT("external-server-001"),
		UnknownFixture.Events,
		true);
	TestFalse(
		TEXT("running unknown server is rejected"),
		UnknownFixture.Host.StartOwnedWorker(
			WorkerSpec(EUeAgentRenderMode::Rhi),
			UnknownServer,
			HostIdentity,
			Diagnostic));
	TestEqual(
		TEXT("unknown owner code"),
		Diagnostic.Code,
		FString(TEXT("transport_server_owner_mismatch")));
	TestEqual(
		TEXT("unknown server receives no lifecycle calls"),
		UnknownFixture.Events.Num(),
		0);

	FLifecycleFixture ChangedFixture;
	FRecordingServerControl ChangedServer(
		TEXT("c11-run-001"),
		TEXT("transport-server-advertised"),
		ChangedFixture.Events,
		false,
		TEXT("transport-server-changed"));
	TestFalse(
		TEXT("changed started identity rolls back"),
		ChangedFixture.Host.StartOwnedWorker(
			WorkerSpec(EUeAgentRenderMode::Rhi),
			ChangedServer,
			HostIdentity,
			Diagnostic));
	TestEqual(
		TEXT("changed identity code"),
		Diagnostic.Code,
		FString(TEXT("transport_server_identity_changed")));
	TestTrue(
		TEXT("changed server stops before process"),
		ChangedFixture.Events == TArray<FString>({
			TEXT("process:start:41001"),
			TEXT("server:start:c11-run-001"),
			TEXT("server:stop:c11-run-001"),
			TEXT("process:stop:41001"),
		}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentLifecycleHostCleanupTest,
	"UeAgentInterface.LifecycleHost.Cleanup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentLifecycleHostCleanupTest::RunTest(const FString& Parameters)
{
	FLifecycleFixture Fixture;
	FRecordingServerControl Server(
		TEXT("c11-run-001"),
		TEXT("transport-server-001"),
		Fixture.Events);
	FUeAgentHostIdentity HostIdentity;
	FUeAgentLifecycleDiagnostic Diagnostic;
	if (!Fixture.Host.StartOwnedWorker(
		WorkerSpec(EUeAgentRenderMode::Rhi),
		Server,
		HostIdentity,
		Diagnostic))
	{
		AddError(Diagnostic.Code);
		return false;
	}

	FUeAgentTickerIdentity TickerIdentity;
	TestTrue(
		TEXT("owned ticker registers"),
		Fixture.Host.RegisterTicker(
			TEXT("health-ticker-001"),
			TickerIdentity,
			Diagnostic));
	TestEqual(
		TEXT("ticker owner"),
		TickerIdentity.OwnerId,
		FString(TEXT("c11-run-001")));
	TestEqual(
		TEXT("ticker host"),
		TickerIdentity.HostId,
		HostIdentity.HostId);

	FUeAgentCleanupReport First;
	TestTrue(TEXT("first cleanup completes"), Fixture.Host.Cleanup(First));
	TestEqual(
		TEXT("cleanup reverse dependency order"),
		Fixture.Events,
		TArray<FString>({
			TEXT("process:start:41001"),
			TEXT("server:start:c11-run-001"),
			TEXT("ticker:register:health-ticker-001"),
			TEXT("ticker:unregister:health-ticker-001"),
			TEXT("server:stop:c11-run-001"),
			TEXT("process:stop:41001"),
		}));
	TestTrue(TEXT("cleanup report complete"), First.bComplete);
	TestEqual(TEXT("cleanup has no failures"), First.Failures.Num(), 0);

	const int32 EventCount = Fixture.Events.Num();
	FUeAgentCleanupReport Second;
	TestTrue(TEXT("repeat cleanup is idempotent"), Fixture.Host.Cleanup(Second));
	TestEqual(TEXT("repeat cleanup has no effects"), Fixture.Events.Num(), EventCount);

	FLifecycleFixture RetryFixture;
	FRecordingServerControl RetryServer(
		TEXT("c11-run-001"),
		TEXT("transport-server-retry"),
		RetryFixture.Events);
	if (!RetryFixture.Host.StartOwnedWorker(
		WorkerSpec(EUeAgentRenderMode::Rhi),
		RetryServer,
		HostIdentity,
		Diagnostic))
	{
		AddError(Diagnostic.Code);
		return false;
	}
	TestTrue(
		TEXT("retry ticker registers"),
		RetryFixture.Host.RegisterTicker(
			TEXT("retry-ticker-001"),
			TickerIdentity,
			Diagnostic));
	RetryFixture.Ticker->FailNextUnregister();
	FUeAgentCleanupReport FailedCleanup;
	TestFalse(
		TEXT("failed ticker cleanup is incomplete"),
		RetryFixture.Host.Cleanup(FailedCleanup));
	TestTrue(
		TEXT("failed cleanup retains dependent process and server"),
		!RetryFixture.Events.Contains(TEXT("server:stop:c11-run-001"))
			&& !RetryFixture.Events.Contains(TEXT("process:stop:41001")));
	const FUeAgentLifecycleHealth RetainedHealth =
		RetryFixture.Host.Health();
	TestEqual(
		TEXT("failed cleanup retains ticker"),
		RetainedHealth.TickerCount,
		1);
	TestTrue(
		TEXT("failed cleanup retains process"),
		RetainedHealth.bProcessAlive);
	FUeAgentCleanupReport RetryCleanup;
	TestTrue(
		TEXT("cleanup retry completes"),
		RetryFixture.Host.Cleanup(RetryCleanup));
	TestTrue(
		TEXT("cleanup retry releases dependent resources"),
		RetryFixture.Events.Contains(TEXT("server:stop:c11-run-001"))
			&& RetryFixture.Events.Contains(TEXT("process:stop:41001")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentLifecycleHostContextGateTest,
	"UeAgentInterface.LifecycleHost.ContextGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentLifecycleHostContextGateTest::RunTest(const FString& Parameters)
{
	FLifecycleFixture Cancelled;
	Cancelled.Cancellation.Request(TEXT("stop_before_worker_start"));
	FRecordingServerControl CancelledServer(
		TEXT("c11-run-001"),
		TEXT("transport-server-cancelled"),
		Cancelled.Events);
	FUeAgentHostIdentity HostIdentity;
	FUeAgentLifecycleDiagnostic Diagnostic;
	TestFalse(
		TEXT("cancellation rejects start before effects"),
		Cancelled.Host.StartOwnedWorker(
			WorkerSpec(EUeAgentRenderMode::Rhi),
			CancelledServer,
			HostIdentity,
			Diagnostic));
	TestEqual(
		TEXT("cancellation code"),
		Diagnostic.Code,
		FString(TEXT("cancellation_requested")));
	TestEqual(TEXT("cancellation has no effects"), Cancelled.Events.Num(), 0);

	return true;
}
