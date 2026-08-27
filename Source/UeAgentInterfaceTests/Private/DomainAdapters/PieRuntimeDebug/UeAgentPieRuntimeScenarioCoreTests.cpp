// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeScenarioCore.h"

namespace
{
	using namespace UE::UAI::DomainAdapters::PieRuntimeDebug;

	class FFakeScenarioPorts final : public IUeAgentPieScenarioPorts
	{
	public:
		bool IsCurrentPausedSnapshot(
			const FString& SessionIdentity,
			int64 ExpectedPauseEpoch,
			int64 ExpectedWorkspaceRevision) const override
		{
			Calls.Add(TEXT("validate"));
			ObservedSessionIdentity = SessionIdentity;
			ObservedPauseEpoch = ExpectedPauseEpoch;
			ObservedWorkspaceRevision = ExpectedWorkspaceRevision;
			return bSnapshotCurrent;
		}

		bool Resume() override
		{
			Calls.Add(TEXT("resume"));
			return bResumeSucceeds;
		}

		EUeAgentPieScenarioStepResult ExecuteSteps(
			double WallTimeLimitSeconds,
			double GameTimeLimitSeconds,
			int64 FrameLimit) override
		{
			Calls.Add(TEXT("step"));
			ObservedWallLimit = WallTimeLimitSeconds;
			ObservedGameLimit = GameTimeLimitSeconds;
			ObservedFrameLimit = FrameLimit;
			return StepResult;
		}

		bool StopDispatch() override
		{
			Calls.Add(TEXT("stop"));
			return bStopSucceeds;
		}

		bool ReleaseAllForOperation() override
		{
			Calls.Add(TEXT("release"));
			++ReleaseCount;
			return bReleaseSucceeds;
		}

		bool RepauseAndVerify(
			const FString& SessionIdentity,
			int64& OutPauseEpoch) override
		{
			Calls.Add(TEXT("pause"));
			ObservedSessionIdentity = SessionIdentity;
			OutPauseEpoch = 12;
			return bPauseSucceeds;
		}

		bool RefreshCurrentWorkspace(
			const FString& SessionIdentity,
			int64 PreviousWorkspaceRevision,
			int64& OutWorkspaceRevision) override
		{
			Calls.Add(TEXT("refresh"));
			ObservedSessionIdentity = SessionIdentity;
			ObservedWorkspaceRevision = PreviousWorkspaceRevision;
			OutWorkspaceRevision = RefreshedRevision;
			return bRefreshSucceeds;
		}

		mutable TArray<FString> Calls;
		mutable FString ObservedSessionIdentity;
		mutable int64 ObservedPauseEpoch = 0;
		mutable int64 ObservedWorkspaceRevision = 0;
		double ObservedWallLimit = 0.0;
		double ObservedGameLimit = 0.0;
		int64 ObservedFrameLimit = 0;
		int32 ReleaseCount = 0;
		bool bSnapshotCurrent = true;
		bool bResumeSucceeds = true;
		bool bPauseSucceeds = true;
		bool bRefreshSucceeds = true;
		bool bStopSucceeds = true;
		bool bReleaseSucceeds = true;
		int64 RefreshedRevision = 21;
		EUeAgentPieScenarioStepResult StepResult =
			EUeAgentPieScenarioStepResult::Succeeded;
	};

	FUeAgentPieScenarioRequest MakeRequest()
	{
		FUeAgentPieScenarioRequest Request;
		Request.SessionIdentity = TEXT("pie-session-1/local-player-0");
		Request.ExpectedPauseEpoch = 11;
		Request.ExpectedWorkspaceRevision = 20;
		Request.WallTimeLimitSeconds = 3.0;
		Request.GameTimeLimitSeconds = 1.5;
		Request.FrameLimit = 90;
		return Request;
	}

	bool HasOrderedFinally(const TArray<FString>& Calls)
	{
		const int32 Stop = Calls.Find(TEXT("stop"));
		const int32 Release = Calls.Find(TEXT("release"));
		const int32 Pause = Calls.Find(TEXT("pause"));
		const int32 Refresh = Calls.Find(TEXT("refresh"));
		return Stop != INDEX_NONE
			&& Stop < Release
			&& Release < Pause
			&& Pause < Refresh;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentPieRuntimeScenarioCoreTest,
	"UeAgentInterface.DomainAdapters.PieRuntimeDebug.ScenarioCore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentPieRuntimeScenarioCoreTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI::DomainAdapters::PieRuntimeDebug;

	{
		FFakeScenarioPorts Ports;
		FUeAgentPieScenarioRequest Invalid = MakeRequest();
		Invalid.FrameLimit = 0;
		const FUeAgentPieScenarioReceipt Receipt =
			RunPieRuntimeScenario(Invalid, Ports);
		TestEqual(TEXT("invalid request terminal"), Receipt.TerminalStatus,
			FString(TEXT("failed")));
		TestTrue(TEXT("invalid request has no side effects"), Ports.Calls.IsEmpty());
	}

	{
		FFakeScenarioPorts Ports;
		Ports.bSnapshotCurrent = false;
		const FUeAgentPieScenarioReceipt Receipt =
			RunPieRuntimeScenario(MakeRequest(), Ports);
		TestEqual(TEXT("stale terminal"), Receipt.TerminalStatus, FString(TEXT("stale")));
		TestTrue(
			TEXT("stale validates only"),
			Ports.Calls == TArray<FString>{TEXT("validate")});
		TestEqual(TEXT("stale releases zero times"), Ports.ReleaseCount, 0);
	}

	{
		FFakeScenarioPorts Ports;
		const FUeAgentPieScenarioReceipt Receipt =
			RunPieRuntimeScenario(MakeRequest(), Ports);
		TestEqual(TEXT("success terminal"), Receipt.TerminalStatus, FString(TEXT("succeeded")));
		TestTrue(TEXT("success finally order"), HasOrderedFinally(Ports.Calls));
		TestEqual(TEXT("success release once"), Ports.ReleaseCount, 1);
		TestEqual(TEXT("refresh raises revision"), Receipt.WorkspaceRevision, int64(21));
		TestEqual(TEXT("session identity frozen"), Ports.ObservedSessionIdentity, MakeRequest().SessionIdentity);
		TestEqual(TEXT("wall limit forwarded"), Ports.ObservedWallLimit, 3.0);
		TestEqual(TEXT("game limit forwarded"), Ports.ObservedGameLimit, 1.5);
		TestEqual(TEXT("frame limit forwarded"), Ports.ObservedFrameLimit, int64(90));
	}

	struct FOutcomeCase
	{
		EUeAgentPieScenarioStepResult StepResult;
		const TCHAR* ExpectedTerminal;
	};
	const FOutcomeCase OutcomeCases[] = {
		{EUeAgentPieScenarioStepResult::Failed, TEXT("failed")},
		{EUeAgentPieScenarioStepResult::TimedOut, TEXT("timed_out")},
		{EUeAgentPieScenarioStepResult::Cancelled, TEXT("cancelled")},
	};
	for (const FOutcomeCase& OutcomeCase : OutcomeCases)
	{
		FFakeScenarioPorts Ports;
		Ports.StepResult = OutcomeCase.StepResult;
		const FUeAgentPieScenarioReceipt Receipt =
			RunPieRuntimeScenario(MakeRequest(), Ports);
		TestEqual(TEXT("non-success terminal"), Receipt.TerminalStatus, FString(OutcomeCase.ExpectedTerminal));
		TestTrue(TEXT("non-success finally order"), HasOrderedFinally(Ports.Calls));
		TestEqual(TEXT("non-success release once"), Ports.ReleaseCount, 1);
	}

	{
		FFakeScenarioPorts Ports;
		Ports.bPauseSucceeds = false;
		const FUeAgentPieScenarioReceipt Receipt =
			RunPieRuntimeScenario(MakeRequest(), Ports);
		TestEqual(TEXT("pause failure is unknown"), Receipt.TerminalStatus, FString(TEXT("state_unknown")));
		TestFalse(TEXT("pause failure skips refresh"), Ports.Calls.Contains(TEXT("refresh")));
		TestEqual(TEXT("pause failure release once"), Ports.ReleaseCount, 1);
	}

	{
		FFakeScenarioPorts Ports;
		Ports.bRefreshSucceeds = false;
		const FUeAgentPieScenarioReceipt Receipt =
			RunPieRuntimeScenario(MakeRequest(), Ports);
		TestEqual(TEXT("refresh failure is unknown"), Receipt.TerminalStatus, FString(TEXT("state_unknown")));
		TestTrue(TEXT("refresh follows pause"), HasOrderedFinally(Ports.Calls));
		TestEqual(TEXT("failed refresh keeps old revision"), Receipt.WorkspaceRevision, int64(20));
		TestEqual(TEXT("refresh failure release once"), Ports.ReleaseCount, 1);
	}

	{
		FFakeScenarioPorts Ports;
		Ports.RefreshedRevision = 20;
		const FUeAgentPieScenarioReceipt Receipt =
			RunPieRuntimeScenario(MakeRequest(), Ports);
		TestEqual(TEXT("non-increasing refresh is unknown"), Receipt.TerminalStatus, FString(TEXT("state_unknown")));
	}

	{

		FFakeScenarioPorts Ports;
		Ports.bStopSucceeds = false;
		const FUeAgentPieScenarioReceipt Receipt =
			RunPieRuntimeScenario(MakeRequest(), Ports);
		TestEqual(TEXT("stop failure is unknown"), Receipt.TerminalStatus,
			FString(TEXT("state_unknown")));
		TestTrue(TEXT("stop failure still refreshes"), HasOrderedFinally(Ports.Calls));
	}

	{
		FFakeScenarioPorts Ports;
		Ports.bReleaseSucceeds = false;
		const FUeAgentPieScenarioReceipt Receipt =
			RunPieRuntimeScenario(MakeRequest(), Ports);
		TestEqual(TEXT("release failure is unknown"), Receipt.TerminalStatus,
			FString(TEXT("state_unknown")));
		TestEqual(TEXT("release failure calls release once"), Ports.ReleaseCount, 1);
	}

	return true;
}
