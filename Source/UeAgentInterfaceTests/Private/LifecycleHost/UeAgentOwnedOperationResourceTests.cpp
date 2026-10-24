#include "Misc/AutomationTest.h"

#include "Core/Contract/UeAgentContract.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/ExecutionMutation/UeAgentLongRunningOperation.h"
#include "LifecycleHost/UeAgentOwnedOperationResource.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::LifecycleHost;

	TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> MakeResources()
	{
		return MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>();
	}

	FLongRunningOperationRequest MakeRequest(double& Now)
	{
		const FUeAgentDescriptorIdentity Descriptor(
			TEXT("uai.test.owned_resource/v1"),
			1,
			TEXT("sha256:7f8cf7b38d339d5ce25d939234a89875939a26865cac51c9c9542ecb93fc2af6"));
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("fixture-project:ls-001a");
		Input.EditorInstanceId = TEXT("fixture-editor:ls-001a");
		Input.StatePlane = EUeAgentStatePlane::Persisted;
		Input.ContextRevision = TEXT("fixture-context:ls-001a:r1");
		FUeAgentExecutionContextRefPtr Context;
		FUeAgentExecutionContextError ContextError;
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
			ContextError));
		return FLongRunningOperationRequest(
			TEXT("operation:ls-001a:resource"),
			TEXT("lease:ls-001a:resource"),
			TEXT("server-session:ls-001a"),
			Descriptor,
			Context.ToSharedRef(),
			MakeResources(),
			FUeAgentOperationDeadline::FromTimeout(
				30.0,
				[&Now]() { return Now; }));
	}

	FUeAgentProcessIdentity FixtureProcess()
	{
		return {
			51001,
			TEXT("2026-08-02T03:00:00.000000Z"),
			TEXT("UnrealEditor-Cmd.exe"),
		};
	}

	class FRecordingOwnedResourceBackend final
		: public IUeAgentOwnedOperationResourceBackend
	{
	public:
		virtual bool StartOwned(
			const FUeAgentOwnedOperationResourceSpec& Spec,
			FUeAgentProcessIdentity& OutProcess,
			FUeAgentLifecycleDiagnostic& OutDiagnostic) override
		{
			++StartCalls;
			Events.Add(TEXT("start:") + Spec.ResourceId);
			OutProcess = FixtureProcess();
			bRunning = true;
			bExited = false;
			return true;
		}

		virtual bool ObserveOwned(
			const FUeAgentOwnedOperationResourceIdentity& Identity,
			FUeAgentOwnedOperationResourceObservation& OutObservation,
			FUeAgentLifecycleDiagnostic& OutDiagnostic) override
		{
			++ObserveCalls;
			Events.Add(TEXT("observe:") + Identity.ResourceId);
			OutObservation.bRunning = bRunning;
			OutObservation.bExited = bExited;
			OutObservation.ExitCode = bExited
				? TOptional<int32>(ExitCode)
				: TOptional<int32>();
			return true;
		}

		virtual bool RequestStopOwned(
			const FUeAgentOwnedOperationResourceIdentity& Identity,
			FUeAgentLifecycleDiagnostic& OutDiagnostic) override
		{
			++StopCalls;
			Events.Add(TEXT("stop:") + Identity.ResourceId);
			// A stop request is asynchronous; the process remains running until
			// the independent observation below reports its real exit.
			return true;
		}

		virtual bool JoinOwned(
			const FUeAgentOwnedOperationResourceIdentity& Identity,
			FUeAgentLifecycleDiagnostic& OutDiagnostic) override
		{
			++JoinCalls;
			Events.Add(TEXT("join:") + Identity.ResourceId);
			if (!bExited)
			{
				OutDiagnostic = FUeAgentLifecycleDiagnostic::Failure(
					TEXT("fixture_resource_still_running"),
					TEXT("join must wait for an observed exit"));
				return false;
			}
			return true;
		}

		virtual bool ReleaseOwned(
			const FUeAgentOwnedOperationResourceIdentity& Identity,
			FUeAgentLifecycleDiagnostic& OutDiagnostic) override
		{
			++ReleaseCalls;
			Events.Add(TEXT("release:") + Identity.ResourceId);
			return true;
		}

		void Exit(const int32 InExitCode)
		{
			bRunning = false;
			bExited = true;
			ExitCode = InExitCode;
		}

		int32 StartCalls = 0;
		int32 ObserveCalls = 0;
		int32 StopCalls = 0;
		int32 JoinCalls = 0;
		int32 ReleaseCalls = 0;
		bool bRunning = false;
		bool bExited = false;
		int32 ExitCode = 0;
		TArray<FString> Events;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentOwnedOperationResourceTest,
	"UeAgentInterface.LifecycleHost.OwnedOperationResource.OwnerContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentOwnedOperationResourceTest::RunTest(const FString& Parameters)
{
	double Now = 100.0;
	const FLongRunningOperationRequest Request = MakeRequest(Now);
	FUeAgentCancellationSource Cancellation([&Now]() { return Now; });
	const TSharedRef<FRecordingOwnedResourceBackend, ESPMode::ThreadSafe>
		Backend = MakeShared<
			FRecordingOwnedResourceBackend,
			ESPMode::ThreadSafe>();
	const FUeAgentOwnedOperationResourceSpec Spec{
		TEXT("resource:compile-worker:ls-001a"),
		Request.GetOperationId(),
		Request.GetOwnerLeaseId(),
		41,
		TEXT("D:/UE/Engine/Binaries/Win64/UnrealEditor-Cmd.exe"),
		{TEXT("-unattended"), TEXT("-nop4")},
	};
	FUeAgentOwnedOperationResourcePort Port(Spec, Backend);
	FExecutionMutationError Error;
	FLongRunningWorkIdentity WorkIdentity;

	TestTrue(
		TEXT("the C11 port starts only its configured owned resource"),
		Port.Start(Request, Cancellation.GetToken(), WorkIdentity, Error));
	TestEqual(TEXT("resource starts once"), Backend->StartCalls, 1);
	TestEqual(TEXT("native generation is exact"),
		WorkIdentity.GetGeneration(), int64(41));
	TestEqual(TEXT("common work identity exposes only the stable resource id"),
		WorkIdentity.GetResourceId(), Spec.ResourceId);
	const FUeAgentOwnedOperationResourceIdentity* Identity = Port.Identity();
	TestNotNull(TEXT("start records an exact resource identity"), Identity);
	if (!Identity)
	{
		return false;
	}
	TestEqual(TEXT("identity binds the operation"), Identity->OperationId,
		Request.GetOperationId());
	TestEqual(TEXT("identity binds the owner lease"), Identity->OwnerLeaseId,
		Request.GetOwnerLeaseId());
	TestEqual(TEXT("identity binds the resource"), Identity->ResourceId,
		Spec.ResourceId);
	TestEqual(TEXT("identity binds generation"), Identity->Generation,
		Spec.Generation);
	TestTrue(TEXT("identity includes process start identity"),
		Identity->Process.Matches(FixtureProcess()));

	FLongRunningWorkObservation Observation;
	TestTrue(TEXT("running resource is observable"),
		Port.Observe(
			Request.GetOperationId(),
			WorkIdentity,
			Request.GetResources(),
			Observation,
			Error));
	TestEqual(TEXT("running maps to the common C7 state"),
		Observation.GetState(), ELongRunningWorkState::Running);

	FUeAgentOwnedOperationResourceIdentity ReusedPid = *Identity;
	ReusedPid.Process.StartTimeUtc = TEXT("2026-08-02T04:00:00.000000Z");
	const int32 EventsBeforeWrongCleanup = Backend->Events.Num();
	TestFalse(TEXT("same PID with a different start identity is not owned"),
		Port.Cleanup(ReusedPid, Error));
	TestEqual(TEXT("wrong identity is rejected with a stable error"),
		Error.GetCode(), FString(TEXT("owned_resource_identity_mismatch")));
	TestEqual(TEXT("wrong identity causes no native action"),
		Backend->Events.Num(), EventsBeforeWrongCleanup);

	const FLongRunningWorkIdentity StaleWorkIdentity(
		WorkIdentity.GetResourceId(),
		WorkIdentity.GetGeneration() - 1);
	TestFalse(TEXT("wrong generation cannot request stop"),
		Port.RequestStop(
			Request.GetOperationId(),
			StaleWorkIdentity,
			TEXT("stale_generation"),
			Error));
	TestEqual(TEXT("stale stop causes no backend call"), Backend->StopCalls, 0);
	TestTrue(TEXT("the exact owned generation can request stop"),
		Port.RequestStop(
			Request.GetOperationId(),
			WorkIdentity,
			TEXT("caller_cancelled"),
			Error));
	TestEqual(TEXT("backend receives one stop request"), Backend->StopCalls, 1);

	TestTrue(TEXT("stop request does not fabricate native exit"),
		Port.Observe(
			Request.GetOperationId(),
			WorkIdentity,
			Request.GetResources(),
			Observation,
			Error));
	TestEqual(TEXT("resource remains running until observed otherwise"),
		Observation.GetState(), ELongRunningWorkState::Running);
	TestFalse(TEXT("running resource cannot be joined"),
		Port.Join(Request.GetOperationId(), WorkIdentity, Error));
	TestEqual(TEXT("failed early join does not release"), Backend->ReleaseCalls, 0);

	Backend->Exit(0);
	TestTrue(TEXT("independent process exit is observed"),
		Port.Observe(
			Request.GetOperationId(),
			WorkIdentity,
			Request.GetResources(),
			Observation,
			Error));
	TestEqual(TEXT("zero exit maps to success"),
		Observation.GetState(), ELongRunningWorkState::ExitedSuccess);
	TestTrue(TEXT("observed exit can be joined"),
		Port.Join(Request.GetOperationId(), WorkIdentity, Error));
	TestTrue(TEXT("joined exact resource can be released"),
		Port.Release(Request.GetOperationId(), WorkIdentity, Error));
	TestEqual(TEXT("resource is joined once after its real exit"),
		Backend->JoinCalls, 2);
	TestEqual(TEXT("resource is released exactly once"),
		Backend->ReleaseCalls, 1);

	const int32 EventsBeforeIdempotentCleanup = Backend->Events.Num();
	TestTrue(TEXT("cleanup of an already released exact resource is idempotent"),
		Port.Cleanup(*Identity, Error));
	TestEqual(TEXT("idempotent cleanup performs no extra native action"),
		Backend->Events.Num(), EventsBeforeIdempotentCleanup);
	return !HasAnyErrors();
}
