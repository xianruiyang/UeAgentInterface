#include "LifecycleHost/UeAgentOwnedOperationResource.h"

#include "Dom/JsonValue.h"
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
			const FString& Reason)
		{
			OutError = FExecutionMutationError::Failure(Code, Reason, TEXT("execution"));
			OutError.SourceKind = TEXT("native_result");
		}

		static void SetBackendError(
			FExecutionMutationError& OutError,
			const FUeAgentLifecycleDiagnostic& Diagnostic,
			const TCHAR* Fallback,
			const FString& Reason)
		{
			if (Diagnostic.IsValid()) OutError = Diagnostic;
			else SetError(OutError, Fallback, Reason);
		}
	}

	bool FUeAgentOwnedOperationResourceSpec::IsValid() const
	{
		FExecutionMutationError Error;
		return Validate(Error);
	}

	bool FUeAgentOwnedOperationResourceSpec::Validate(FExecutionMutationError& OutError) const
	{
		OutError.Reset();
		const TCHAR* Reason = ResourceId.TrimStartAndEnd().IsEmpty()
			? TEXT("The owned resource specification is missing its resource identity.")
			: OperationId.TrimStartAndEnd().IsEmpty()
			? TEXT("The owned resource specification is missing its operation identity.")
			: OwnerLeaseId.TrimStartAndEnd().IsEmpty()
			? TEXT("The owned resource specification is missing its owner lease.")
			: Generation <= 0 ? TEXT("The owned resource specification requires a positive generation.")
			: Executable.TrimStartAndEnd().IsEmpty()
			? TEXT("The owned resource specification is missing its executable.") : nullptr;
		if (Reason)
		{
			SetError(OutError, TEXT("owned_resource_spec_invalid"), Reason);
			return false;
		}
		for (int32 Index = 0; Index < Arguments.Num(); ++Index)
		{
			if (Arguments[Index].TrimStartAndEnd().IsEmpty())
			{
				SetError(OutError, TEXT("owned_resource_spec_invalid"),
					FString::Printf(TEXT("Executable argument at index %d is empty."), Index));
				OutError.Source.InternalJsonPath = FString::Printf(TEXT("/arguments/%d"), Index);
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
		FExecutionMutationError Error;
		return Validate(Error);
	}

	bool FUeAgentOwnedOperationResourceObservation::Validate(FExecutionMutationError& OutError) const
	{
		OutError.Reset();
		if (bRunning && bExited)
		{
			SetError(OutError, TEXT("owned_resource_observation_invalid"),
				TEXT("The native observation reports the resource as both running and exited."));
			return false;
		}
		if (bExited && !ExitCode.IsSet())
		{
			SetError(OutError, TEXT("owned_resource_observation_invalid"),
				TEXT("The native observation reports an exited resource without an exit code."));
			return false;
		}
		if (!bExited && ExitCode.IsSet())
		{
			SetError(OutError, TEXT("owned_resource_observation_invalid"),
				TEXT("The native observation supplies an exit code without reporting that the resource exited."));
			return false;
		}
		return true;
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
			if (!Spec.Validate(OutError))
			{
				return false;
			}
			if (!Request.IsValid())
			{
				OutError = Request.GetValidationDiagnostic();
				return false;
			}
			if (Request.GetOperationId() != Spec.OperationId || Request.GetOwnerLeaseId() != Spec.OwnerLeaseId)
			{
				SetError(
					OutError,
					TEXT("owned_resource_request_mismatch"),
					Request.GetOperationId() != Spec.OperationId
						? TEXT("The resource start request belongs to a different operation.")
						: TEXT("The resource start request belongs to a different owner lease."));
				return false;
			}
			if (Cancellation.IsCancelled())
			{
				SetError(
					OutError,
					TEXT("owned_resource_start_cancelled"), TEXT("The operation was cancelled before its owned resource started."));
				return false;
			}
			if (OwnedIdentity.IsSet() || bStartInFlight)
			{
				SetError(
					OutError,
					TEXT("owned_resource_already_started"),
					bStartInFlight ? TEXT("A start request for this owned resource is already in progress.")
						: TEXT("This operation already owns a started resource."));
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
					TEXT("owned_resource_start_failed"), TEXT("The resource backend failed to start the resource without reporting a diagnostic."));
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
					TEXT("owned_resource_process_identity_invalid"), TEXT("The resource backend returned an invalid process identity after starting."));
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
		const TCHAR* Reason = !OwnedIdentity.IsSet() ? TEXT("The operation has not acquired a resource identity.")
			: !OwnedIdentity->IsValid() ? TEXT("The stored owned resource identity is invalid.")
			: NormalizeText(OperationId) != OwnedIdentity->OperationId ? TEXT("The resource request belongs to a different operation.")
			: !Identity.IsValid() ? TEXT("The requested work identity is invalid.")
			: Identity.GetResourceId() != OwnedIdentity->ResourceId ? TEXT("The request identifies a different owned resource.")
			: Identity.GetGeneration() != OwnedIdentity->Generation ? TEXT("The request identifies a different resource generation.") : nullptr;
		if (Reason)
		{
			SetError(
				OutError,
				TEXT("owned_resource_identity_mismatch"), Reason);
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
				SetError(OutError, TEXT("owned_resource_released"), TEXT("The owned resource has already been released."));
				return false;
			}
			if (bCallInFlight)
			{
				SetError(OutError, TEXT("owned_resource_call_in_flight"), TEXT("Another lifecycle call for the owned resource is still in progress."));
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
					TEXT("owned_resource_observe_failed"), TEXT("The resource backend failed to observe the resource without reporting a diagnostic."));
				return false;
			}
			if (!NativeObservation.Validate(OutError))
			{
				return false;
			}
			if (NativeObservation.bRunning)
			{
				OutObservation =
					ExecutionMutation::FLongRunningWorkObservation(Spec.Generation,
			ELongRunningWorkState::Running,
			UE::UAI::Contract::FDiagnostic());
				return true;
			}
			if (!NativeObservation.bExited)
			{
				FExecutionMutationError UnknownState;
				SetError(UnknownState, TEXT("owned_resource_state_unknown"),
					TEXT("The native observation cannot determine whether the resource is running or has exited."));
				UnknownState.Stage = TEXT("native_observation");
				OutObservation =
					ExecutionMutation::FLongRunningWorkObservation(Spec.Generation,
			ELongRunningWorkState::StateUnknown,
			MoveTemp(UnknownState));
				return true;
			}

			bObservedExited = true;
			const int32 ExitCode =
				NativeObservation.ExitCode.GetValue();
			FExecutionMutationError ExitDiagnostic;
			if (ExitCode != 0)
			{
				const FString Code = FString::Printf(TEXT("native_resource_exit_code_%d"), ExitCode);
				SetError(ExitDiagnostic, *Code, FString::Printf(TEXT("The native resource exited with code %d."), ExitCode));
				ExitDiagnostic.Stage = TEXT("native_observation");
				ExitDiagnostic.ActualValue = MakeShared<FJsonValueNumber>(ExitCode);
				ExitDiagnostic.ExpectedValue = MakeShared<FJsonValueNumber>(0);
			}
			OutObservation =
				ExecutionMutation::FLongRunningWorkObservation(
					Spec.Generation,
					ExitCode == 0
						? ELongRunningWorkState::ExitedSuccess
						: ELongRunningWorkState::ExitedFailure,
					MoveTemp(ExitDiagnostic));
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
				SetError(OutError, TEXT("owned_resource_stop_reason_missing"), TEXT("Stopping the owned resource requires a non-empty reason."));
				return false;
			}
			if (bReleased)
			{
				SetError(OutError, TEXT("owned_resource_released"), TEXT("The owned resource has already been released."));
				return false;
			}
			if (bStopRequested)
			{
				return true;
			}
			if (bCallInFlight)
			{
				SetError(OutError, TEXT("owned_resource_call_in_flight"), TEXT("Another lifecycle call for the owned resource is still in progress."));
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
					TEXT("owned_resource_stop_failed"), TEXT("The resource backend failed to request a resource stop without reporting a diagnostic."));
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
				SetError(OutError, TEXT("owned_resource_call_in_flight"), TEXT("Another lifecycle call for the owned resource is still in progress."));
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
					TEXT("owned_resource_join_failed"), TEXT("The resource backend failed to join the resource without reporting a diagnostic."));
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
				SetError(OutError, TEXT("owned_resource_not_joined"), TEXT("The owned resource must finish joining before it can be released."));
				return false;
			}
			if (bCallInFlight)
			{
				SetError(OutError, TEXT("owned_resource_call_in_flight"), TEXT("Another lifecycle call for the owned resource is still in progress."));
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
					TEXT("owned_resource_release_failed"), TEXT("The resource backend failed to release the resource without reporting a diagnostic."));
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
			const TCHAR* IdentityReason = !OwnedIdentity.IsSet()
				? TEXT("The operation has no owned resource identity to clean up.")
				: OwnedIdentity->ResourceId != ExpectedIdentity.ResourceId ? TEXT("The cleanup request identifies a different resource.")
				: OwnedIdentity->OperationId != ExpectedIdentity.OperationId ? TEXT("The cleanup request belongs to a different operation.")
				: OwnedIdentity->OwnerLeaseId != ExpectedIdentity.OwnerLeaseId ? TEXT("The cleanup request belongs to a different owner lease.")
				: OwnedIdentity->Generation != ExpectedIdentity.Generation ? TEXT("The cleanup request identifies a different resource generation.")
				: !OwnedIdentity->Process.Matches(ExpectedIdentity.Process) ? TEXT("The cleanup request identifies a different native process.") : nullptr;
			if (IdentityReason)
			{
				SetError(
					OutError,
					TEXT("owned_resource_identity_mismatch"), IdentityReason);
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
				SetError(OutError, TEXT("owned_resource_call_in_flight"), TEXT("Another lifecycle call for the owned resource is still in progress."));
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
					TEXT("owned_resource_observe_failed"), TEXT("The resource backend failed to observe the resource without reporting a diagnostic."));
				return false;
			}
			if (!NativeObservation.Validate(OutError))
			{
				return false;
			}
			if (NativeObservation.bRunning)
			{
				SetError(OutError, TEXT("owned_resource_still_running"), TEXT("The owned resource is still running after the stop request."));
				return false;
			}
			if (!NativeObservation.bExited)
			{
				SetError(OutError, TEXT("owned_resource_state_unknown"), TEXT("The native observation cannot determine whether the resource is running or has exited."));
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
