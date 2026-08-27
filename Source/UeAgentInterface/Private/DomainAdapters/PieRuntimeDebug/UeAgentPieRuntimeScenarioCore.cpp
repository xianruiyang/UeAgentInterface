// Copyright Epic Games, Inc. All Rights Reserved.

#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeScenarioCore.h"

namespace UE::UAI::DomainAdapters::PieRuntimeDebug
{
	namespace
	{
		void SetStepOutcome(
			EUeAgentPieScenarioStepResult StepResult,
			FUeAgentPieScenarioReceipt& Receipt)
		{
			switch (StepResult)
			{
			case EUeAgentPieScenarioStepResult::Succeeded:
				Receipt.TerminalStatus = TEXT("succeeded");
				break;
			case EUeAgentPieScenarioStepResult::Failed:
				Receipt.TerminalStatus = TEXT("failed");
				Receipt.ErrorCode = TEXT("scenario_step_failed");
				break;
			case EUeAgentPieScenarioStepResult::TimedOut:
				Receipt.TerminalStatus = TEXT("timed_out");
				Receipt.ErrorCode = TEXT("scenario_timed_out");
				break;
			case EUeAgentPieScenarioStepResult::Cancelled:
				Receipt.TerminalStatus = TEXT("cancelled");
				Receipt.ErrorCode = TEXT("scenario_cancelled");
				break;
			default:
				Receipt.TerminalStatus = TEXT("state_unknown");
				Receipt.ErrorCode = TEXT("scenario_invalid_step_result");
				Receipt.bStateKnown = false;
				break;
			}
		}
	}

	bool FUeAgentPieScenarioRequest::IsValid() const
	{
		FString Normalized = SessionIdentity;
		Normalized.TrimStartAndEndInline();
		return !Normalized.IsEmpty()
			&& Normalized == SessionIdentity
			&& ExpectedPauseEpoch > 0
			&& ExpectedWorkspaceRevision > 0
			&& FMath::IsFinite(WallTimeLimitSeconds)
			&& WallTimeLimitSeconds > 0.0
			&& FMath::IsFinite(GameTimeLimitSeconds)
			&& GameTimeLimitSeconds > 0.0
			&& FrameLimit > 0;
	}

	FUeAgentPieScenarioReceipt FinalizePieRuntimeScenario(
		const FUeAgentPieScenarioRequest& Request,
		const EUeAgentPieScenarioStepResult StepResult,
		IUeAgentPieScenarioPorts& Ports)
	{
		FUeAgentPieScenarioReceipt Receipt;
		Receipt.SessionIdentity = Request.SessionIdentity;
		Receipt.PauseEpoch = Request.ExpectedPauseEpoch;
		Receipt.WorkspaceRevision = Request.ExpectedWorkspaceRevision;
		SetStepOutcome(StepResult, Receipt);

		const bool bStopSucceeded = Ports.StopDispatch();
		const bool bReleaseSucceeded = Ports.ReleaseAllForOperation();
		int64 VerifiedPauseEpoch = Request.ExpectedPauseEpoch;
		if (!Ports.RepauseAndVerify(Request.SessionIdentity, VerifiedPauseEpoch))
		{
			Receipt.TerminalStatus = TEXT("state_unknown");
			Receipt.ErrorCode = TEXT("scenario_repause_verification_failed");
			Receipt.bStateKnown = false;
			return Receipt;
		}
		Receipt.PauseEpoch = VerifiedPauseEpoch;

		int64 RefreshedRevision = Request.ExpectedWorkspaceRevision;
		if (!Ports.RefreshCurrentWorkspace(
				Request.SessionIdentity,
				Request.ExpectedWorkspaceRevision,
				RefreshedRevision)
			|| RefreshedRevision <= Request.ExpectedWorkspaceRevision)
		{
			Receipt.TerminalStatus = TEXT("state_unknown");
			Receipt.ErrorCode = TEXT("scenario_workspace_refresh_failed");
			Receipt.bStateKnown = false;
			return Receipt;
		}
		Receipt.WorkspaceRevision = RefreshedRevision;

		if (!bStopSucceeded || !bReleaseSucceeded)
		{
			Receipt.TerminalStatus = TEXT("state_unknown");
			Receipt.ErrorCode = !bStopSucceeded
				? TEXT("scenario_stop_dispatch_failed")
				: TEXT("scenario_release_all_failed");
			Receipt.bStateKnown = false;
		}
		return Receipt;
	}

	FUeAgentPieScenarioReceipt RunPieRuntimeScenario(
		const FUeAgentPieScenarioRequest& Request,
		IUeAgentPieScenarioPorts& Ports)
	{
		FUeAgentPieScenarioReceipt Receipt;
		Receipt.SessionIdentity = Request.SessionIdentity;
		Receipt.PauseEpoch = Request.ExpectedPauseEpoch;
		Receipt.WorkspaceRevision = Request.ExpectedWorkspaceRevision;
		if (!Request.IsValid())
		{
			Receipt.TerminalStatus = TEXT("failed");
			Receipt.ErrorCode = TEXT("scenario_request_invalid");
			return Receipt;
		}

		if (!Ports.IsCurrentPausedSnapshot(
				Request.SessionIdentity,
				Request.ExpectedPauseEpoch,
				Request.ExpectedWorkspaceRevision))
		{
			Receipt.TerminalStatus = TEXT("stale");
			Receipt.ErrorCode = TEXT("scenario_request_stale");
			return Receipt;
		}

		if (!Ports.Resume())
		{
			Receipt = FinalizePieRuntimeScenario(
				Request,
				EUeAgentPieScenarioStepResult::Failed,
				Ports);
			if (Receipt.bStateKnown)
			{
				Receipt.ErrorCode = TEXT("scenario_resume_failed");
			}
			return Receipt;
		}
		return FinalizePieRuntimeScenario(
			Request,
			Ports.ExecuteSteps(
				Request.WallTimeLimitSeconds,
				Request.GameTimeLimitSeconds,
				Request.FrameLimit),
			Ports);
	}
}
