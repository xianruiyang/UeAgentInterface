// Copyright Epic Games, Inc. All Rights Reserved.
#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeScenarioCore.h"
#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeDiagnostics.h"

namespace UE::UAI::DomainAdapters::PieRuntimeDebug
{
    using UE::UAI::Contract::FDiagnostic;
    namespace
    {
        FDiagnostic FailureOr(const FDiagnostic& Diagnostic, const TCHAR* Code, const TCHAR* Message)
        {
            return Diagnostic.IsEmpty()
                ? PieDiagnostics::Native(Code, FString(Message) + TEXT(" The operation did not provide a diagnostic."),
                    TEXT("pie_scenario"), TEXT("diagnostic_available"),
                    MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true))
                : Diagnostic;
        }
        void SetStepOutcome(EUeAgentPieScenarioStepResult StepResult,
            const FDiagnostic& Diagnostic, FUeAgentPieScenarioReceipt& Receipt)
        {
            Receipt.Diagnostic = Diagnostic;
            switch (StepResult)
            {
            case EUeAgentPieScenarioStepResult::Succeeded:
                Receipt.TerminalStatus = TEXT("succeeded"); break;
            case EUeAgentPieScenarioStepResult::Failed:
                Receipt.TerminalStatus = TEXT("failed");
                Receipt.Diagnostic = FailureOr(Diagnostic, TEXT("scenario_step_failed"), TEXT("A scenario step failed.")); break;
            case EUeAgentPieScenarioStepResult::TimedOut:
                Receipt.TerminalStatus = TEXT("timed_out");
                Receipt.Diagnostic = FailureOr(Diagnostic, TEXT("scenario_timed_out"), TEXT("The scenario reached its time or frame limit.")); break;
            case EUeAgentPieScenarioStepResult::Cancelled:
                Receipt.TerminalStatus = TEXT("cancelled");
                Receipt.Diagnostic = FailureOr(Diagnostic, TEXT("scenario_cancelled"), TEXT("The scenario was cancelled.")); break;
            default:
                Receipt.TerminalStatus = TEXT("state_unknown");
                Receipt.Diagnostic = Diagnostic.IsEmpty() ? PieDiagnostics::Native(TEXT("scenario_invalid_step_result"),
                    TEXT("The scenario returned an invalid step outcome."), TEXT("pie_scenario"), TEXT("step_result"),
                    PieDiagnostics::Number(static_cast<int32>(StepResult)),
                    MakeShared<FJsonValueString>(TEXT("Succeeded, Failed, TimedOut, or Cancelled"))) : Diagnostic;
                Receipt.bStateKnown = false; break;
            }
        }
    }
	bool FUeAgentPieScenarioRequest::TryValidate(Contract::FDiagnostic& OutError,
		const TMap<FString, Contract::FSourceLocation>& InputSources) const
	{
		const auto Reject = [&](const FString& Field, const FString& Reason,
			const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
		{
			OutError = PieDiagnostics::Native(TEXT("scenario_request_invalid"), Reason, TEXT("pie_scenario"), Field, Actual, Expected);
			if (const auto* Source = InputSources.Find(Field))
			{
				OutError.Source = *Source;
				OutError.SourceKind = TEXT("command_input");
				OutError.SemanticPointer = Source->JsonPath;
			}
			return false;
		};
		FString Normalized = SessionIdentity;
		Normalized.TrimStartAndEndInline();
		if (Normalized.IsEmpty() || Normalized != SessionIdentity)
		{
			Reject(TEXT("session_ref"), TEXT("A scenario requires a nonempty session identity without surrounding whitespace."),
				PieDiagnostics::Facts({ { TEXT("empty"), MakeShared<FJsonValueBoolean>(Normalized.IsEmpty()) },
					{ TEXT("normalized"), MakeShared<FJsonValueBoolean>(Normalized == SessionIdentity) } }),
				PieDiagnostics::Facts({ { TEXT("empty"), MakeShared<FJsonValueBoolean>(false) }, { TEXT("normalized"), MakeShared<FJsonValueBoolean>(true) } }));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(SessionIdentity);
			return false;
		}
		if (ExpectedPauseEpoch <= 0)
			return Reject(TEXT("expected_pause_epoch"), TEXT("The expected pause epoch must be positive."),
				PieDiagnostics::Number(ExpectedPauseEpoch), MakeShared<FJsonValueString>(TEXT("integer > 0")));
		if (ExpectedWorkspaceRevision <= 0)
			return Reject(TEXT("expected_workspace_revision"), TEXT("The expected workspace revision must be positive."),
				PieDiagnostics::Number(ExpectedWorkspaceRevision), MakeShared<FJsonValueString>(TEXT("integer > 0")));
		if (!FMath::IsFinite(WallTimeLimitSeconds) || WallTimeLimitSeconds < 0.0)
			return Reject(TEXT("limits/wall_seconds"), TEXT("The wall-time limit must be finite and nonnegative."),
				PieDiagnostics::Number(WallTimeLimitSeconds), MakeShared<FJsonValueString>(TEXT("finite number >= 0")));
		if (!FMath::IsFinite(GameTimeLimitSeconds) || GameTimeLimitSeconds < 0.0)
			return Reject(TEXT("limits/game_seconds"), TEXT("The game-time limit must be finite and nonnegative."),
				PieDiagnostics::Number(GameTimeLimitSeconds), MakeShared<FJsonValueString>(TEXT("finite number >= 0")));
		if (FrameLimit < 0)
			return Reject(TEXT("limits/frame_count"), TEXT("The frame limit must be nonnegative."),
				PieDiagnostics::Number(FrameLimit), MakeShared<FJsonValueString>(TEXT("integer >= 0")));
		return true;
	}

	bool FUeAgentPieScenarioRequest::IsValid() const
	{
		Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

    FUeAgentPieScenarioReceipt FinalizePieRuntimeScenario(
        const FUeAgentPieScenarioRequest& Request,
        const EUeAgentPieScenarioStepResult StepResult,
        IUeAgentPieScenarioPorts& Ports, const FDiagnostic& StepDiagnostic)
    {
        FUeAgentPieScenarioReceipt Receipt;
        Receipt.SessionIdentity = Request.SessionIdentity;
        Receipt.PauseEpoch = Request.ExpectedPauseEpoch;
        Receipt.WorkspaceRevision = Request.ExpectedWorkspaceRevision;
        SetStepOutcome(StepResult, StepDiagnostic, Receipt);
        FDiagnostic StopError;
        const bool bStopSucceeded = Ports.StopDispatch(StopError);
        if (!bStopSucceeded) Receipt.CleanupDiagnostics.Add(FailureOr(StopError,
            TEXT("scenario_stop_dispatch_failed"), TEXT("Scenario dispatch could not be stopped.")));
        Receipt.bStateKnown &= bStopSucceeded;
        bool bReleaseAttempted = false;
        const auto Release = [&]()
        {
            if (bReleaseAttempted) return;
            bReleaseAttempted = true;
            FDiagnostic ReleaseError;
            if (!Ports.ReleaseAllForOperation(Receipt, ReleaseError))
            {
                Receipt.CleanupDiagnostics.Add(FailureOr(ReleaseError,
                    TEXT("scenario_release_all_failed"), TEXT("Scenario input cleanup could not be completed.")));
                Receipt.bStateKnown = false;
            }
        };
        if (StepResult != EUeAgentPieScenarioStepResult::Succeeded || !bStopSucceeded) Release();
        int64 VerifiedPauseEpoch = Request.ExpectedPauseEpoch;
        FDiagnostic PauseError;
        if (!Ports.RepauseAndVerify(Request.SessionIdentity, VerifiedPauseEpoch, PauseError))
        {
            Receipt.CleanupDiagnostics.Add(FailureOr(PauseError,
                TEXT("scenario_repause_verification_failed"), TEXT("The paused state could not be verified.")));
            Receipt.bStateKnown = false;
            Release();
            return Receipt;
        }
        Receipt.bPaused = true;
        Receipt.PauseEpoch = VerifiedPauseEpoch;
        FDiagnostic ObservationError;
        if (!Ports.CapturePausedObservations(Receipt, ObservationError))
        {
            const auto Failure = FailureOr(ObservationError,
                TEXT("scenario_observation_failed"), TEXT("The paused scenario observations could not be captured."));
            if (Receipt.TerminalStatus == TEXT("succeeded"))
            {
                Receipt.TerminalStatus = TEXT("failed");
                Receipt.Diagnostic = Failure;
            }
            else Receipt.CleanupDiagnostics.Add(Failure);
            Release();
        }
        int64 RefreshedRevision = Request.ExpectedWorkspaceRevision;
        FDiagnostic RefreshError;
        const bool bRefreshed = Ports.RefreshCurrentWorkspace(Request.SessionIdentity,
            Request.ExpectedWorkspaceRevision, RefreshedRevision, RefreshError);
        if (!bRefreshed || RefreshedRevision <= Request.ExpectedWorkspaceRevision)
        {
            if (bRefreshed && RefreshError.IsEmpty())
                RefreshError = PieDiagnostics::Native(TEXT("scenario_workspace_refresh_failed"),
                    TEXT("The refreshed workspace revision did not advance."), TEXT("pie_scenario"), TEXT("workspace_revision"),
                    PieDiagnostics::Number(RefreshedRevision),
                    MakeShared<FJsonValueString>(TEXT("greater than ") + ::LexToString(Request.ExpectedWorkspaceRevision)));
            Receipt.CleanupDiagnostics.Add(FailureOr(RefreshError,
                TEXT("scenario_workspace_refresh_failed"), TEXT("The scenario workspace revision could not be refreshed.")));
            Receipt.bStateKnown = false;
            Release();
            return Receipt;
        }
        Receipt.WorkspaceRevision = RefreshedRevision;
        Receipt.bWorkspaceRefreshed = true;
        return Receipt;
    }

    FUeAgentPieScenarioReceipt RunPieRuntimeScenario(
        const FUeAgentPieScenarioRequest& Request, IUeAgentPieScenarioPorts& Ports)
    {
        FUeAgentPieScenarioReceipt Receipt;
        Receipt.SessionIdentity = Request.SessionIdentity;
        Receipt.PauseEpoch = Request.ExpectedPauseEpoch;
        Receipt.WorkspaceRevision = Request.ExpectedWorkspaceRevision;
        FDiagnostic ValidationError;
        if (!Request.TryValidate(ValidationError))
        {
            Receipt.TerminalStatus = TEXT("failed");
            Receipt.Diagnostic = MoveTemp(ValidationError);
            return Receipt;
        }
        FDiagnostic Error;
        if (!Ports.IsCurrentPausedSnapshot(Request.SessionIdentity,
                Request.ExpectedPauseEpoch, Request.ExpectedWorkspaceRevision, Error))
        {
            Receipt.TerminalStatus = TEXT("stale");
            Receipt.Diagnostic = FailureOr(Error, TEXT("scenario_request_stale"), TEXT("The scenario request refers to a stale paused snapshot."));
            return Receipt;
        }
        if (!Ports.Resume(Error))
            return FinalizePieRuntimeScenario(Request, EUeAgentPieScenarioStepResult::Failed, Ports,
                FailureOr(Error, TEXT("scenario_resume_failed"), TEXT("The scenario could not resume.")));
        const auto Result = Ports.ExecuteSteps(Request.WallTimeLimitSeconds,
            Request.GameTimeLimitSeconds, Request.FrameLimit, Error);
        return FinalizePieRuntimeScenario(Request, Result, Ports, Error);
    }
}
