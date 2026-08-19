#include "LifecycleHost/UeAgentOwnedOperationResource.h"

#include "Misc/ScopeLock.h"

namespace UE::UAI::LifecycleHost
{
	namespace
	{
		using ExecutionMutation::ELongRunningWorkState;
		using ExecutionMutation::FExecutionMutationError;

		static FString NormalizeText(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		static void SetError(
			FExecutionMutationError& OutError,
			const TCHAR* Code,
			FString Detail = FString())
		{
			OutError = FExecutionMutationError(
				Code,
				MoveTemp(Detail));
		}

		static void SetBackendError(
			FExecutionMutationError& OutError,
			const FUeAgentLifecycleDiagnostic& Diagnostic,
			const TCHAR* Fallback)
		{
			SetError(
				OutError,
				Diagnostic.Code.IsEmpty()
					? Fallback
					: *Diagnostic.Code,
				Diagnostic.Message);
		}
	}

	bool FUeAgentOwnedOperationResourceSpec::IsValid() const
	{
		if (ResourceId.TrimStartAndEnd().IsEmpty()
			|| OperationId.TrimStartAndEnd().IsEmpty()
			|| OwnerLeaseId.TrimStartAndEnd().IsEmpty()
			|| Generation <= 0
			|| Executable.TrimStartAndEnd().IsEmpty())
		{
			return false;
		}
		for (const FString& Argument : Arguments)
		{
			if (Argument.TrimStartAndEnd().IsEmpty())
			{
				return false;
			}
		}
		return true;
	}

	bool FUeAgentOwnedOperationResourceIdentity::IsValid() const
	{
		return !ResourceId.TrimStartAndEnd().IsEmpty()
			&& !OperationId.TrimStartAndEnd().IsEmpty()
			&& !OwnerLeaseId.TrimStartAndEnd().IsEmpty()
			&& Generation > 0
			&& Process.IsValid();
	}

	bool FUeAgentOwnedOperationResourceIdentity::Matches(
		const FUeAgentOwnedOperationResourceIdentity& Other) const
	{
		return ResourceId == Other.ResourceId
			&& OperationId == Other.OperationId
			&& OwnerLeaseId == Other.OwnerLeaseId
			&& Generation == Other.Generation
			&& Process.Matches(Other.Process);
	}

	bool FUeAgentOwnedOperationResourceObservation::IsValid() const
	{
		if (bRunning && bExited)
		{
			return false;
		}
		if (bRunning)
		{
			return !ExitCode.IsSet();
		}
		if (bExited)
		{
			return ExitCode.IsSet();
		}
		return !ExitCode.IsSet();
	}

	FUeAgentOwnedOperationResourcePort::
	FUeAgentOwnedOperationResourcePort(
		FUeAgentOwnedOperationResourceSpec InSpec,
		TSharedRef<IUeAgentOwnedOperationResourceBackend, ESPMode::ThreadSafe> InBackend)
		: Spec(MoveTemp(InSpec))
		, Backend(MoveTemp(InBackend))
	{
		Spec.ResourceId = NormalizeText(MoveTemp(Spec.ResourceId));
		Spec.OperationId = NormalizeText(MoveTemp(Spec.OperationId));
		Spec.OwnerLeaseId = NormalizeText(MoveTemp(Spec.OwnerLeaseId));
		Spec.Executable = NormalizeText(MoveTemp(Spec.Executable));
		for (FString& Argument : Spec.Arguments)
		{
			Argument = NormalizeText(MoveTemp(Argument));
		}
	}

	bool FUeAgentOwnedOperationResourcePort::Start(
		const ExecutionMutation::FLongRunningOperationRequest& Request,
		const ContextResource::FUeAgentCancellationToken& Cancellation,
		ExecutionMutation::FLongRunningWorkIdentity& OutIdentity,
		FExecutionMutationError& OutError)
	{
		OutIdentity = {};
		OutError = {};
		{
			FScopeLock Lock(&Mutex);
			if (!Spec.IsValid())
			{
				SetError(OutError, TEXT("owned_resource_spec_invalid"));
				return false;
			}
			if (!Request.IsValid()
				|| Request.GetOperationId() != Spec.OperationId
				|| Request.GetOwnerLeaseId() != Spec.OwnerLeaseId)
			{
				SetError(
					OutError,
					TEXT("owned_resource_request_mismatch"));
				return false;
			}
			if (Cancellation.IsCancelled())
			{
				SetError(
					OutError,
					TEXT("owned_resource_start_cancelled"));
				return false;
			}
			if (OwnedIdentity.IsSet() || bStartInFlight)
			{
				SetError(
					OutError,
					TEXT("owned_resource_already_started"));
				return false;
			}
			bStartInFlight = true;
		}

		FUeAgentProcessIdentity Process;
		FUeAgentLifecycleDiagnostic Diagnostic;
		const bool bStarted = Backend->StartOwned(
			Spec,
			Process,
			Diagnostic);
		{
			FScopeLock Lock(&Mutex);
			bStartInFlight = false;
			if (!bStarted)
			{
				SetBackendError(
					OutError,
					Diagnostic,
					TEXT("owned_resource_start_failed"));
				return false;
			}
			FUeAgentOwnedOperationResourceIdentity IdentityValue{
				Spec.ResourceId,
				Spec.OperationId,
				Spec.OwnerLeaseId,
				Spec.Generation,
				MoveTemp(Process),
			};
			if (!IdentityValue.IsValid())
			{
				SetError(
					OutError,
					TEXT("owned_resource_process_identity_invalid"));
				return false;
			}
			OwnedIdentity = MoveTemp(IdentityValue);
			OutIdentity = ExecutionMutation::FLongRunningWorkIdentity(
				Spec.ResourceId,
				Spec.Generation);
			return true;
		}
	}

	bool FUeAgentOwnedOperationResourcePort::ValidateWorkIdentity(
		const FString& OperationId,
		const ExecutionMutation::FLongRunningWorkIdentity& Identity,
		FExecutionMutationError& OutError) const
	{
		if (!OwnedIdentity.IsSet()
			|| !OwnedIdentity->IsValid()
			|| NormalizeText(OperationId) != OwnedIdentity->OperationId
			|| !Identity.IsValid()
			|| Identity.GetResourceId() != OwnedIdentity->ResourceId
			|| Identity.GetGeneration() != OwnedIdentity->Generation)
		{
			SetError(
				OutError,
				TEXT("owned_resource_identity_mismatch"));
			return false;
		}
		return true;
	}

	bool FUeAgentOwnedOperationResourcePort::Observe(
		const FString& OperationId,
		const ExecutionMutation::FLongRunningWorkIdentity& Identity,
		ContextResource::FUeAgentResourceTracker& Resources,
		ExecutionMutation::FLongRunningWorkObservation& OutObservation,
		FExecutionMutationError& OutError)
	{
		OutObservation = {};
		OutError = {};
		FUeAgentOwnedOperationResourceIdentity NativeIdentity;
		{
			FScopeLock Lock(&Mutex);
			if (!ValidateWorkIdentity(OperationId, Identity, OutError))
			{
				return false;
			}
			if (bReleased)
			{
				SetError(OutError, TEXT("owned_resource_released"));
				return false;
			}
			if (bCallInFlight)
			{
				SetError(OutError, TEXT("owned_resource_call_in_flight"));
				return false;
			}
			bCallInFlight = true;
			NativeIdentity = OwnedIdentity.GetValue();
		}

		FUeAgentOwnedOperationResourceObservation NativeObservation;
		FUeAgentLifecycleDiagnostic Diagnostic;
		const bool bObserved = Backend->ObserveOwned(
			NativeIdentity,
			NativeObservation,
			Diagnostic);
		{
			FScopeLock Lock(&Mutex);
			bCallInFlight = false;
			if (!bObserved)
			{
				SetBackendError(
					OutError,
					Diagnostic,
					TEXT("owned_resource_observe_failed"));
				return false;
			}
			if (!NativeObservation.IsValid())
			{
				SetError(
					OutError,
					TEXT("owned_resource_observation_invalid"));
				return false;
			}
			if (NativeObservation.bRunning)
			{
				OutObservation =
					ExecutionMutation::FLongRunningWorkObservation(
						Spec.Generation,
						ELongRunningWorkState::Running,
						FString());
				return true;
			}
			if (!NativeObservation.bExited)
			{
				OutObservation =
					ExecutionMutation::FLongRunningWorkObservation(
						Spec.Generation,
						ELongRunningWorkState::StateUnknown,
						TEXT("owned_resource_state_unknown"));
				return true;
			}

			bObservedExited = true;
			const int32 ExitCode =
				NativeObservation.ExitCode.GetValue();
			OutObservation =
				ExecutionMutation::FLongRunningWorkObservation(
					Spec.Generation,
					ExitCode == 0
						? ELongRunningWorkState::ExitedSuccess
						: ELongRunningWorkState::ExitedFailure,
					ExitCode == 0
						? FString()
						: FString::Printf(
							TEXT("native_resource_exit_code_%d"),
							ExitCode));
			return true;
		}
	}

	bool FUeAgentOwnedOperationResourcePort::RequestStop(
		const FString& OperationId,
		const ExecutionMutation::FLongRunningWorkIdentity& Identity,
		const FString& Reason,
		FExecutionMutationError& OutError)
	{
		OutError = {};
		FUeAgentOwnedOperationResourceIdentity NativeIdentity;
		{
			FScopeLock Lock(&Mutex);
			if (!ValidateWorkIdentity(OperationId, Identity, OutError))
			{
				return false;
			}
			if (NormalizeText(Reason).IsEmpty())
			{
				SetError(OutError, TEXT("owned_resource_stop_reason_missing"));
				return false;
			}
			if (bReleased)
			{
				SetError(OutError, TEXT("owned_resource_released"));
				return false;
			}
			if (bStopRequested)
			{
				return true;
			}
			if (bCallInFlight)
			{
				SetError(OutError, TEXT("owned_resource_call_in_flight"));
				return false;
			}
			bCallInFlight = true;
			NativeIdentity = OwnedIdentity.GetValue();
		}

		FUeAgentLifecycleDiagnostic Diagnostic;
		const bool bStopped = Backend->RequestStopOwned(
			NativeIdentity,
			Diagnostic);
		{
			FScopeLock Lock(&Mutex);
			bCallInFlight = false;
			if (!bStopped)
			{
				SetBackendError(
					OutError,
					Diagnostic,
					TEXT("owned_resource_stop_failed"));
				return false;
			}
			bStopRequested = true;
			return true;
		}
	}

	bool FUeAgentOwnedOperationResourcePort::Join(
		const FString& OperationId,
		const ExecutionMutation::FLongRunningWorkIdentity& Identity,
		FExecutionMutationError& OutError)
	{
		OutError = {};
		FUeAgentOwnedOperationResourceIdentity NativeIdentity;
		{
			FScopeLock Lock(&Mutex);
			if (!ValidateWorkIdentity(OperationId, Identity, OutError))
			{
				return false;
			}
			if (bReleased || bJoined)
			{
				return true;
			}
			if (bCallInFlight)
			{
				SetError(OutError, TEXT("owned_resource_call_in_flight"));
				return false;
			}
			bCallInFlight = true;
			NativeIdentity = OwnedIdentity.GetValue();
		}

		FUeAgentLifecycleDiagnostic Diagnostic;
		const bool bJoinedNow = Backend->JoinOwned(
			NativeIdentity,
			Diagnostic);
		{
			FScopeLock Lock(&Mutex);
			bCallInFlight = false;
			if (!bJoinedNow)
			{
				SetBackendError(
					OutError,
					Diagnostic,
					TEXT("owned_resource_join_failed"));
				return false;
			}
			bJoined = true;
			return true;
		}
	}

	bool FUeAgentOwnedOperationResourcePort::Release(
		const FString& OperationId,
		const ExecutionMutation::FLongRunningWorkIdentity& Identity,
		FExecutionMutationError& OutError)
	{
		OutError = {};
		FUeAgentOwnedOperationResourceIdentity NativeIdentity;
		{
			FScopeLock Lock(&Mutex);
			if (!ValidateWorkIdentity(OperationId, Identity, OutError))
			{
				return false;
			}
			if (bReleased)
			{
				return true;
			}
			if (!bJoined)
			{
				SetError(OutError, TEXT("owned_resource_not_joined"));
				return false;
			}
			if (bCallInFlight)
			{
				SetError(OutError, TEXT("owned_resource_call_in_flight"));
				return false;
			}
			bCallInFlight = true;
			NativeIdentity = OwnedIdentity.GetValue();
		}

		FUeAgentLifecycleDiagnostic Diagnostic;
		const bool bReleasedNow = Backend->ReleaseOwned(
			NativeIdentity,
			Diagnostic);
		{
			FScopeLock Lock(&Mutex);
			bCallInFlight = false;
			if (!bReleasedNow)
			{
				SetBackendError(
					OutError,
					Diagnostic,
					TEXT("owned_resource_release_failed"));
				return false;
			}
			bReleased = true;
			return true;
		}
	}

	const FUeAgentOwnedOperationResourceIdentity*
	FUeAgentOwnedOperationResourcePort::Identity() const
	{
		FScopeLock Lock(&Mutex);
		return OwnedIdentity.IsSet()
			? &OwnedIdentity.GetValue()
			: nullptr;
	}

	bool FUeAgentOwnedOperationResourcePort::Cleanup(
		const FUeAgentOwnedOperationResourceIdentity& ExpectedIdentity,
		FExecutionMutationError& OutError)
	{
		OutError = {};
		ExecutionMutation::FLongRunningWorkIdentity WorkIdentity;
		bool bNeedsStop = false;
		{
			FScopeLock Lock(&Mutex);
			if (!OwnedIdentity.IsSet()
				|| !OwnedIdentity->Matches(ExpectedIdentity))
			{
				SetError(
					OutError,
					TEXT("owned_resource_identity_mismatch"));
				return false;
			}
			if (bReleased)
			{
				return true;
			}
			WorkIdentity =
				ExecutionMutation::FLongRunningWorkIdentity(
					OwnedIdentity->ResourceId,
					OwnedIdentity->Generation);
			bNeedsStop = !bStopRequested;
		}

		if (bNeedsStop
			&& !RequestStop(
				ExpectedIdentity.OperationId,
				WorkIdentity,
				TEXT("cleanup_requested"),
				OutError))
		{
			return false;
		}

		FUeAgentOwnedOperationResourceIdentity NativeIdentity;
		{
			FScopeLock Lock(&Mutex);
			if (bReleased)
			{
				return true;
			}
			if (bCallInFlight)
			{
				SetError(OutError, TEXT("owned_resource_call_in_flight"));
				return false;
			}
			bCallInFlight = true;
			NativeIdentity = OwnedIdentity.GetValue();
		}

		FUeAgentOwnedOperationResourceObservation NativeObservation;
		FUeAgentLifecycleDiagnostic Diagnostic;
		const bool bObserved = Backend->ObserveOwned(
			NativeIdentity,
			NativeObservation,
			Diagnostic);
		{
			FScopeLock Lock(&Mutex);
			bCallInFlight = false;
			if (!bObserved)
			{
				SetBackendError(
					OutError,
					Diagnostic,
					TEXT("owned_resource_observe_failed"));
				return false;
			}
			if (!NativeObservation.IsValid())
			{
				SetError(
					OutError,
					TEXT("owned_resource_observation_invalid"));
				return false;
			}
			if (NativeObservation.bRunning)
			{
				SetError(OutError, TEXT("owned_resource_still_running"));
				return false;
			}
			if (!NativeObservation.bExited)
			{
				SetError(OutError, TEXT("owned_resource_state_unknown"));
				return false;
			}
			bObservedExited = true;
		}

		if (!Join(
			ExpectedIdentity.OperationId,
			WorkIdentity,
			OutError))
		{
			return false;
		}
		return Release(
			ExpectedIdentity.OperationId,
			WorkIdentity,
			OutError);
	}
}
