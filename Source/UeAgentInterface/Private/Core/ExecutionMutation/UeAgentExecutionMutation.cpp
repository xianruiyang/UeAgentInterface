#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"

#include "Async/Async.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeLock.h"

namespace UE::UAI::ExecutionMutation
{
	namespace
	{
		using AssetDurable::EDurableStatus;
		using ContextResource::EUeAgentResourceDimension;
		using ContextResource::FUeAgentCancellationRequested;
		using ContextResource::FUeAgentOperationDeadlineExceeded;
		using ContextResource::FUeAgentResourceChargeError;

		static FString NormalizeText(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		static TArray<FString> NormalizeStrings(
			TArray<FString> Values)
		{
			for (FString& Value : Values)
			{
				Value.TrimStartAndEndInline();
			}
			Values.RemoveAll(
				[](const FString& Value)
				{
					return Value.IsEmpty();
				});
			Values.Sort();
			for (int32 Index = Values.Num() - 1; Index > 0; --Index)
			{
				if (Values[Index] == Values[Index - 1])
				{
					Values.RemoveAt(Index);
				}
			}
			return Values;
		}

		static FExecutionMutationError ExecutionFailure(
			const TCHAR* Code, const FString& Reason, const FString& Path = FString())
		{
			FExecutionMutationError Error = FExecutionMutationError::Failure(Code, Reason, TEXT("execution"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Path;
			return Error;
		}

		static FExecutionMutationError DiagnosticOr(
			const FExecutionMutationError& Error, const TCHAR* Fallback, const FString& Reason)
		{
			return Error.IsEmpty() ? ExecutionFailure(Fallback, Reason) : Error;
		}

		static const FExecutionMutationError& ValidationOrEmpty(
			const TSharedPtr<const FExecutionMutationError>& Diagnostic)
		{
			static const FExecutionMutationError Empty;
			return Diagnostic.IsValid() ? *Diagnostic : Empty;
		}

		static bool ContainsAll(
			const TArray<FString>& Observed,
			const TArray<FString>& Required)
		{
			for (const FString& Signal : Required)
			{
				if (!Observed.Contains(Signal))
				{
					return false;
				}
			}
			return true;
		}

		static int32 FindFirstUnlockedTarget(
			const TArray<FString>& Candidate,
			const TArray<FString>& Allowed)
		{
			for (int32 Index = 0; Index < Candidate.Num(); ++Index)
			{
				if (!Allowed.Contains(Candidate[Index]))
				{
					return Index;
				}
			}
			return INDEX_NONE;
		}

		static TPair<EExecutionStatus, bool> MapDurableStatus(
			const EDurableStatus Status)
		{
			switch (Status)
			{
			case EDurableStatus::Prepared:
				return {
					EExecutionStatus::RecoveryRequired,
					false
				};
			case EDurableStatus::PersistedUnverified:
				return {
					EExecutionStatus::PersistedUnverified,
					false
				};
			case EDurableStatus::RolledBack:
				return {EExecutionStatus::RolledBack, true};
			case EDurableStatus::RecoveryRequired:
				return {
					EExecutionStatus::RecoveryRequired,
					false
				};
			case EDurableStatus::StateUnknown:
				return {EExecutionStatus::StateUnknown, false};
			default:
				return {EExecutionStatus::StateUnknown, false};
			}
		}
	}









	FString LexToString(const EExecutionPhase Value)
	{
		switch (Value)
		{
		case EExecutionPhase::Queued:
			return TEXT("queued");
		case EExecutionPhase::WaitingForLock:
			return TEXT("waiting_for_lock");
		case EExecutionPhase::Executing:
			return TEXT("executing");
		case EExecutionPhase::Settling:
			return TEXT("settling");
		case EExecutionPhase::Persisting:
			return TEXT("persisting");
		case EExecutionPhase::Terminal:
			return TEXT("terminal");
		default:
			return TEXT("unknown");
		}
	}

	FString LexToString(const EExecutionStatus Value)
	{
		switch (Value)
		{
		case EExecutionStatus::CancelledNoChange:
			return TEXT("cancelled_no_change");
		case EExecutionStatus::AppliedInMemory:
			return TEXT("applied_in_memory");
		case EExecutionStatus::PersistedUnverified:
			return TEXT("persisted_unverified");
		case EExecutionStatus::RolledBack:
			return TEXT("rolled_back");
		case EExecutionStatus::RecoveryRequired:
			return TEXT("recovery_required");
		case EExecutionStatus::StateUnknown:
			return TEXT("state_unknown");
		case EExecutionStatus::Failed:
			return TEXT("failed");
		default:
			return TEXT("unknown");
		}
	}

	FString LexToString(const EExecutionCancellationMode Value)
	{
		switch (Value)
		{
		case EExecutionCancellationMode::NotRequested:
			return TEXT("not_requested");
		case EExecutionCancellationMode::SafeBoundary:
			return TEXT("safe_boundary");
		case EExecutionCancellationMode::RollbackRequired:
			return TEXT("rollback_required");
		default:
			return TEXT("unknown");
		}
	}

	FString LexToString(const EExecutionThreadAffinity Value)
	{
		switch (Value)
		{
		case EExecutionThreadAffinity::GameThread:
			return TEXT("game_thread");
		default:
			return TEXT("unknown");
		}
	}

	FString LexToString(const EExecutionReentrancyPolicy Value)
	{
		switch (Value)
		{
		case EExecutionReentrancyPolicy::RejectSameTarget:
			return TEXT("reject_same_target");
		default:
			return TEXT("unknown");
		}
	}

	FLockFenceLease::FLockFenceLease()
		: FLockFenceLease(FString(), {}, -1)
	{
	}

	FLockFenceLease::FLockFenceLease(
		FString InOwnerId,
		TArray<FString> InLockKeys,
		const int64 InFencingToken)
		: OwnerId(NormalizeText(MoveTemp(InOwnerId)))
		, LockKeys(NormalizeStrings(MoveTemp(InLockKeys)))
		, FencingToken(InFencingToken)
	{
		FExecutionMutationError Error;
		if (OwnerId.IsEmpty())
			Error = ExecutionFailure(TEXT("operation_lease_invalid"), TEXT("The lock lease has no owner."), TEXT("owner_id"));
		else if (LockKeys.IsEmpty())
			Error = ExecutionFailure(TEXT("operation_lease_invalid"), TEXT("The lock lease has no target keys."), TEXT("lock_keys"));
		else if (FencingToken < 0)
		{
			Error = ExecutionFailure(TEXT("operation_lease_invalid"), TEXT("The lock lease fencing token must be non-negative."), TEXT("fencing_token"));
			Error.ActualEvidence = MakeShared<FJsonValueNumber>(FencingToken);
		}
		if (!Error.IsEmpty()) ValidationDiagnostic = MakeShared<FExecutionMutationError>(MoveTemp(Error));
	}

	const FString& FLockFenceLease::GetOwnerId() const
	{
		return OwnerId;
	}

	const TArray<FString>& FLockFenceLease::GetLockKeys() const
	{
		return LockKeys;
	}

	int64 FLockFenceLease::GetFencingToken() const
	{
		return FencingToken;
	}

	bool FLockFenceLease::IsValid() const
	{
		return !ValidationDiagnostic.IsValid();
	}

	const FExecutionMutationError& FLockFenceLease::GetValidationDiagnostic() const
	{
		return ValidationOrEmpty(ValidationDiagnostic);
	}

	FBarrierRequest::FBarrierRequest(
		const int64 InSubjectGeneration,
		TArray<FString> InRequiredSignals)
		: SubjectGeneration(InSubjectGeneration)
		, RequiredSignals(NormalizeStrings(
			MoveTemp(InRequiredSignals)))
	{
		bValid = SubjectGeneration >= 0;
	}

	int64 FBarrierRequest::GetSubjectGeneration() const
	{
		return SubjectGeneration;
	}

	const TArray<FString>& FBarrierRequest::GetRequiredSignals() const
	{
		return RequiredSignals;
	}

	bool FBarrierRequest::IsValid() const
	{
		return bValid;
	}

	FBarrierResult::FBarrierResult()
		: FBarrierResult(-1, {}, FString(), false)
	{
	}

	FBarrierResult::FBarrierResult(
		const int64 InSubjectGeneration,
		TArray<FString> InObservedSignals,
		FString InPostBarrierRevision,
		const bool bInSettled)
		: FBarrierResult(InSubjectGeneration, MoveTemp(InObservedSignals), MoveTemp(InPostBarrierRevision), bInSettled, {})
	{
	}

	FBarrierResult::FBarrierResult(
		const int64 InSubjectGeneration,
		TArray<FString> InObservedSignals,
		FString InPostBarrierRevision,
		const bool bInSettled,
		FExecutionMutationError InSettleDiagnostic)
		: SubjectGeneration(InSubjectGeneration)
		, ObservedSignals(NormalizeStrings(
			MoveTemp(InObservedSignals)))
		, PostBarrierRevision(NormalizeText(
			MoveTemp(InPostBarrierRevision)))
		, bSettled(bInSettled)
	{
		if (!InSettleDiagnostic.IsEmpty()) SettleDiagnostic = MakeShared<FExecutionMutationError>(MoveTemp(InSettleDiagnostic));
		FExecutionMutationError Error;
		if (SubjectGeneration < 0)
		{
			Error = ExecutionFailure(TEXT("operation_barrier_result_invalid"), TEXT("The barrier result subject generation must be non-negative."), TEXT("subject_generation"));
			Error.ActualEvidence = MakeShared<FJsonValueNumber>(SubjectGeneration);
		}
		else if (PostBarrierRevision.IsEmpty())
			Error = ExecutionFailure(TEXT("operation_barrier_result_invalid"), TEXT("The barrier result has no post-barrier revision."), TEXT("post_barrier_revision"));
		if (!Error.IsEmpty()) ValidationDiagnostic = MakeShared<FExecutionMutationError>(MoveTemp(Error));
	}

	int64 FBarrierResult::GetSubjectGeneration() const
	{
		return SubjectGeneration;
	}

	const TArray<FString>& FBarrierResult::GetObservedSignals() const
	{
		return ObservedSignals;
	}

	const FString& FBarrierResult::GetPostBarrierRevision() const
	{
		return PostBarrierRevision;
	}

	bool FBarrierResult::IsSettled() const
	{
		return bSettled;
	}

	const FExecutionMutationError& FBarrierResult::GetSettleDiagnostic() const
	{
		return ValidationOrEmpty(SettleDiagnostic);
	}

	bool FBarrierResult::IsValid() const
	{
		return !ValidationDiagnostic.IsValid();
	}

	const FExecutionMutationError& FBarrierResult::GetValidationDiagnostic() const
	{
		return ValidationOrEmpty(ValidationDiagnostic);
	}

	FMutationFacts FMutationFacts::NoMutation()
	{
		FMutationFacts Facts;
		Facts.bNoMutationConfirmed = true;
		return Facts;
	}

	bool FMutationFacts::IsNoMutationConfirmed() const
	{
		return bNoMutationConfirmed;
	}

	FMutationFacts::FMutationFacts()
		: FMutationFacts(-1, {}, {}, false)
	{
	}

	FMutationFacts::FMutationFacts(
		const int64 InSubjectGeneration,
		TArray<FString> InMutatedIdentityKeys,
		TArray<FString> InRequiredBarrierSignals,
		const bool bInRollbackSupported,
		TArray<FAssetDurableTarget> InDurableTargets)
		: SubjectGeneration(InSubjectGeneration)
		, MutatedIdentityKeys(NormalizeStrings(
			MoveTemp(InMutatedIdentityKeys)))
		, RequiredBarrierSignals(NormalizeStrings(
			MoveTemp(InRequiredBarrierSignals)))
		, DurableTargets(MoveTemp(InDurableTargets))
		, bRollbackSupported(bInRollbackSupported)
	{
		FExecutionMutationError Error;
		if (SubjectGeneration < 0)
		{
			Error = ExecutionFailure(TEXT("operation_mutation_facts_invalid"), TEXT("The mutation result subject generation must be non-negative."), TEXT("subject_generation"));
			Error.ActualEvidence = MakeShared<FJsonValueNumber>(SubjectGeneration);
		}
		else if (MutatedIdentityKeys.IsEmpty())
			Error = ExecutionFailure(TEXT("operation_mutation_facts_invalid"), TEXT("The mutation result identifies no changed targets."), TEXT("mutated_identity_keys"));
		else
		{
			for (int32 Index = 0; Index < DurableTargets.Num(); ++Index)
			{
				if (!DurableTargets[Index].IsValid())
				{
					Error = ExecutionFailure(TEXT("operation_mutation_facts_invalid"), TEXT("The mutation result contains an invalid durable target."), FString::Printf(TEXT("durable_targets[%d]"), Index));
					Error.PublicCause = MakeShared<FExecutionMutationError>(DurableTargets[Index].GetValidationDiagnostic());
					break;
				}
			}
		}
		if (!Error.IsEmpty()) ValidationDiagnostic = MakeShared<FExecutionMutationError>(MoveTemp(Error));
	}

	int64 FMutationFacts::GetSubjectGeneration() const
	{
		return SubjectGeneration;
	}

	const TArray<FString>& FMutationFacts::GetMutatedIdentityKeys() const
	{
		return MutatedIdentityKeys;
	}

	const TArray<FString>&
	FMutationFacts::GetRequiredBarrierSignals() const
	{
		return RequiredBarrierSignals;
	}

	const TArray<FAssetDurableTarget>&
	FMutationFacts::GetDurableTargets() const
	{
		return DurableTargets;
	}

	bool FMutationFacts::IsRollbackSupported() const
	{
		return bRollbackSupported;
	}

	bool FMutationFacts::IsValid() const
	{
		return !ValidationDiagnostic.IsValid();
	}

	const FExecutionMutationError& FMutationFacts::GetValidationDiagnostic() const
	{
		return ValidationOrEmpty(ValidationDiagnostic);
	}

	FExecutionMutationRequest::FExecutionMutationRequest(
		FString InOperationId,
		FString InOwnerLeaseId,
		FPlanningPreflightPlanRef InPlan,
		FUeAgentExecutionContextRefOwner InExecutionContext,
		TSharedRef<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe> InResources,
		FUeAgentOperationDeadline InDeadline,
		FUeAgentCancellationToken InCancellation,
		const EExecutionThreadAffinity InThreadAffinity,
		const EExecutionReentrancyPolicy InReentrancyPolicy)
		: OperationId(NormalizeText(MoveTemp(InOperationId)))
		, OwnerLeaseId(NormalizeText(MoveTemp(InOwnerLeaseId)))
		, Plan(MoveTemp(InPlan))
		, ExecutionContext(MoveTemp(InExecutionContext))
		, Resources(MoveTemp(InResources))
		, Deadline(MoveTemp(InDeadline))
		, Cancellation(MoveTemp(InCancellation))
		, ThreadAffinity(InThreadAffinity)
		, ReentrancyPolicy(InReentrancyPolicy)
	{
		FExecutionMutationError Error;
		if (OperationId.IsEmpty())
			Error = ExecutionFailure(TEXT("operation_request_invalid"), TEXT("The execution request has no operation identifier."), TEXT("operation_id"));
		else if (OwnerLeaseId.IsEmpty())
			Error = ExecutionFailure(TEXT("operation_request_invalid"), TEXT("The execution request has no owner lease identifier."), TEXT("owner_lease_id"));
		else if (Plan->GetRequestId().IsEmpty())
			Error = ExecutionFailure(TEXT("operation_request_invalid"), TEXT("The execution plan has no request identifier."), TEXT("plan.request_id"));
		else if (Plan->GetImpactSet().GetImpacts().IsEmpty())
			Error = ExecutionFailure(TEXT("operation_request_invalid"), TEXT("The execution plan identifies no affected targets."), TEXT("plan.impact_set"));
		else if (Plan->GetFingerprint().IsEmpty())
			Error = ExecutionFailure(TEXT("operation_request_invalid"), TEXT("The execution plan has no fingerprint."), TEXT("plan.fingerprint"));
		else if (ExecutionContext->GetRequestKind() != EUeAgentRequestKind::Operation)
		{
			Error = ExecutionFailure(TEXT("operation_request_invalid"), TEXT("Execution requires an operation context."), TEXT("execution_context.request_kind"));
			Error.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(ExecutionContext->GetRequestKind()));
			Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("operation"));
		}
		else if (!(ExecutionContext->GetDescriptorIdentity() == Plan->GetDescriptorIdentity()))
			Error = ExecutionFailure(TEXT("operation_request_invalid"), TEXT("The execution context and plan belong to different operation descriptors."), TEXT("execution_context.descriptor"));
		if (!Error.IsEmpty()) ValidationDiagnostic = MakeShared<FExecutionMutationError>(MoveTemp(Error));
	}

	const FString& FExecutionMutationRequest::GetOperationId() const
	{
		return OperationId;
	}

	const FString& FExecutionMutationRequest::GetOwnerLeaseId() const
	{
		return OwnerLeaseId;
	}

	const FPlanningPreflightPlanRef&
	FExecutionMutationRequest::GetPlan() const
	{
		return Plan;
	}

	const FUeAgentExecutionContextRef&
	FExecutionMutationRequest::GetExecutionContext() const
	{
		return ExecutionContext.Get();
	}

	FUeAgentResourceTracker&
	FExecutionMutationRequest::GetResources() const
	{
		return Resources.Get();
	}

	const FUeAgentOperationDeadline&
	FExecutionMutationRequest::GetDeadline() const
	{
		return Deadline;
	}

	const FUeAgentCancellationToken&
	FExecutionMutationRequest::GetCancellation() const
	{
		return Cancellation;
	}

	EExecutionThreadAffinity
	FExecutionMutationRequest::GetThreadAffinity() const
	{
		return ThreadAffinity;
	}

	EExecutionReentrancyPolicy
	FExecutionMutationRequest::GetReentrancyPolicy() const
	{
		return ReentrancyPolicy;
	}

	bool FExecutionMutationRequest::IsValid() const
	{
		return !ValidationDiagnostic.IsValid();
	}

	const FExecutionMutationError& FExecutionMutationRequest::GetValidationDiagnostic() const
	{
		return ValidationOrEmpty(ValidationDiagnostic);
	}

	FExecutionReceipt::FExecutionReceipt()
		: ValidationDiagnostic(ExecutionFailure(TEXT("execution_receipt_invalid"),
			TEXT("No execution receipt was returned."), TEXT("receipt")))
	{
	}

	FExecutionReceipt::FExecutionReceipt(
		FString InOperationId,
		const EExecutionPhase InPhase,
		const EExecutionStatus InStatus,
		FExecutionMutationError InDiagnostic,
		const EExecutionCancellationMode InCancellationMode,
		const bool bInMutationStarted,
		TOptional<int64> InFencingToken,
		const bool bInRetrySafe,
		TOptional<FDurableReceipt> InDurableReceipt,
		TArray<FString> InPhaseHistory)
		: OperationId(NormalizeText(MoveTemp(InOperationId)))
		, Phase(InPhase)
		, Status(InStatus)
		, Diagnostic(MoveTemp(InDiagnostic))
		, CancellationMode(InCancellationMode)
		, bMutationStarted(bInMutationStarted)
		, FencingToken(MoveTemp(InFencingToken))
		, bRetrySafe(bInRetrySafe)
		, DurableReceipt(MoveTemp(InDurableReceipt))
		, PhaseHistory(MoveTemp(InPhaseHistory))
	{
		if (!Diagnostic.IsEmpty())
		{
			Diagnostic.Schema = TEXT("uai.operation.failure/1");
			Diagnostic.ExecutionStatus = LexToString(Status);
			Diagnostic.TerminalStatus = LexToString(Status);
			Diagnostic.MutationStarted = bMutationStarted;
			Diagnostic.MutationMayHaveStarted = bMutationStarted || Status == EExecutionStatus::StateUnknown;
			Diagnostic.bRetrySafe = bRetrySafe;
		}
		if (OperationId.IsEmpty())
			ValidationDiagnostic = ExecutionFailure(TEXT("execution_receipt_invalid"), TEXT("The execution receipt has an empty operation identifier."), TEXT("operation_id"));
		else if (Phase != EExecutionPhase::Terminal)
		{
			ValidationDiagnostic = ExecutionFailure(TEXT("execution_receipt_invalid"), TEXT("The execution receipt has not reached the terminal phase."), TEXT("phase"));
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueString>(LexToString(Phase));
			ValidationDiagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("terminal"));
		}
		else if (PhaseHistory.IsEmpty())
			ValidationDiagnostic = ExecutionFailure(TEXT("execution_receipt_invalid"), TEXT("The execution receipt has no phase history."), TEXT("phase_history"));
		else if (PhaseHistory.Last() != TEXT("terminal"))
		{
			ValidationDiagnostic = ExecutionFailure(TEXT("execution_receipt_invalid"), TEXT("The last execution history entry is not terminal."), FString::Printf(TEXT("phase_history[%d]"), PhaseHistory.Num() - 1));
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueString>(PhaseHistory.Last());
			ValidationDiagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("terminal"));
		}
		else if (FencingToken.IsSet() && FencingToken.GetValue() < 0)
		{
			ValidationDiagnostic = ExecutionFailure(TEXT("execution_receipt_invalid"), TEXT("The execution receipt has a negative fencing token."), TEXT("fencing_token"));
			ValidationDiagnostic.ActualEvidence = MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), FencingToken.GetValue()));
			ValidationDiagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-negative integer"));
		}
	}

	const FString& FExecutionReceipt::GetOperationId() const
	{
		return OperationId;
	}

	EExecutionPhase FExecutionReceipt::GetPhase() const
	{
		return Phase;
	}

	EExecutionStatus FExecutionReceipt::GetStatus() const
	{
		return Status;
	}

	const FString& FExecutionReceipt::GetErrorCode() const
	{
		return Diagnostic.Code;
	}

	const FExecutionMutationError& FExecutionReceipt::GetDiagnostic() const
	{
		return Diagnostic;
	}

	EExecutionCancellationMode
	FExecutionReceipt::GetCancellationMode() const
	{
		return CancellationMode;
	}

	bool FExecutionReceipt::DidMutationStart() const
	{
		return bMutationStarted;
	}

	const TOptional<int64>&
	FExecutionReceipt::GetFencingToken() const
	{
		return FencingToken;
	}

	bool FExecutionReceipt::IsRetrySafe() const
	{
		return bRetrySafe;
	}

	const FDurableReceipt*
	FExecutionReceipt::GetDurableReceipt() const
	{
		return DurableReceipt.IsSet()
			? &DurableReceipt.GetValue()
			: nullptr;
	}

	const TArray<FString>&
	FExecutionReceipt::GetPhaseHistory() const
	{
		return PhaseHistory;
	}

	bool FExecutionReceipt::IsValid() const
	{
		return ValidationDiagnostic.IsEmpty();
	}

	const FExecutionMutationError& FExecutionReceipt::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic;
	}

	bool FGameThreadExecutionScheduler::Schedule(
		const EExecutionThreadAffinity Affinity,
		const FString& OperationId,
		TFunction<void()> Callback,
		FExecutionMutationError& OutError)
	{
		if (Affinity != EExecutionThreadAffinity::GameThread)
		{
			OutError = ExecutionFailure(TEXT("operation_thread_affinity_unsupported"), TEXT("The scheduler supports only the game thread."), TEXT("thread_affinity"));
			OutError.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(Affinity));
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("game_thread"));
			return false;
		}
		if (OperationId.TrimStartAndEnd().IsEmpty() || !Callback)
		{
			OutError = OperationId.TrimStartAndEnd().IsEmpty()
				? ExecutionFailure(TEXT("operation_schedule_invalid"), TEXT("The scheduled operation identifier is empty."), TEXT("operation_id"))
				: ExecutionFailure(TEXT("operation_schedule_invalid"), TEXT("The scheduled operation has no callback."), TEXT("callback"));
			return false;
		}
		AsyncTask(
			ENamedThreads::GameThread,
			[Callback = MoveTemp(Callback)]() mutable
			{
				Callback();
			});
		return true;
	}

	bool FGameThreadExecutionScheduler::IsCurrentThread(
		const EExecutionThreadAffinity Affinity) const
	{
		return Affinity == EExecutionThreadAffinity::GameThread
			&& IsInGameThread();
	}

	bool FMonotonicLockFenceProvider::TryAcquire(
		const TArray<FString>& LockKeys,
		const FString& OwnerId,
		FUeAgentResourceTracker& Resources,
		FLockFenceLease& OutLease,
		FExecutionMutationError& OutError)
	{
		const TArray<FString> NormalizedKeys =
			NormalizeStrings(LockKeys);
		const FString NormalizedOwner = NormalizeText(OwnerId);
		if (NormalizedKeys.Num() == 0 || NormalizedOwner.IsEmpty())
		{
			OutError = NormalizedKeys.IsEmpty()
				? ExecutionFailure(TEXT("operation_lock_request_invalid"), TEXT("The lock request has no non-empty target keys."), TEXT("lock_keys"))
				: ExecutionFailure(TEXT("operation_lock_request_invalid"), TEXT("The lock request has no owner identifier."), TEXT("owner_id"));
			return false;
		}
		FUeAgentResourceChargeError ChargeError;
		if (!Resources.TryCharge(
			EUeAgentResourceDimension::WorkUnits,
			1,
			TEXT("C7.ExecutionMutation.LockFence.Acquire"),
			ChargeError))
		{
			OutError = DiagnosticOr(ChargeError, TEXT("resource_accounting_failed"), TEXT("Lock acquisition resource accounting failed without reporting a diagnostic."));
			return false;
		}
		FScopeLock Lock(&Mutex);
		for (const FString& Key : NormalizedKeys)
		{
			if (HeldLocks.Contains(Key))
			{
				OutError = ExecutionFailure(TEXT("operation_target_locked"), TEXT("A requested target is already locked by another operation."));
				OutError.InternalContext = Key;
				return false;
			}
		}
		const int64 Token = NextFencingToken++;
		for (const FString& Key : NormalizedKeys)
		{
			HeldLocks.Add(
				Key,
				FHeldLock{NormalizedOwner, Token});
		}
		OutLease = FLockFenceLease(
			NormalizedOwner,
			NormalizedKeys,
			Token);
		return true;
	}

	bool FMonotonicLockFenceProvider::IsCurrent(
		const FLockFenceLease& Lease) const
	{
		if (!Lease.IsValid())
		{
			return false;
		}
		FScopeLock Lock(&Mutex);
		for (const FString& Key : Lease.GetLockKeys())
		{
			const FHeldLock* Held = HeldLocks.Find(Key);
			if (!Held
				|| Held->OwnerId != Lease.GetOwnerId()
				|| Held->FencingToken != Lease.GetFencingToken())
			{
				return false;
			}
		}
		return true;
	}

	bool FMonotonicLockFenceProvider::Release(
		const FLockFenceLease& Lease,
		FExecutionMutationError& OutError)
	{
		FScopeLock Lock(&Mutex);
		for (const FString& Key : Lease.GetLockKeys())
		{
			const FHeldLock* Held = HeldLocks.Find(Key);
			if (!Held
				|| Held->OwnerId != Lease.GetOwnerId()
				|| Held->FencingToken != Lease.GetFencingToken())
			{
				OutError = ExecutionFailure(TEXT("operation_fence_stale"),
					!Held ? TEXT("The target lock no longer exists.")
					: Held->OwnerId != Lease.GetOwnerId() ? TEXT("The target lock belongs to a different owner.")
					: TEXT("The target lock fencing token no longer matches the lease."));
				OutError.InternalContext = Key;
				return false;
			}
		}
		for (const FString& Key : Lease.GetLockKeys())
		{
			HeldLocks.Remove(Key);
		}
		return true;
	}

	class FExecutionMutationCoordinator::FState final
		: public TSharedFromThis<FState, ESPMode::ThreadSafe>
	{
	public:
		FState(
			TSharedRef<
				IExecutionScheduler,
				ESPMode::ThreadSafe> InScheduler,
			TSharedRef<
				ILockFenceProvider,
				ESPMode::ThreadSafe> InLockFence,
			TSharedRef<
				IAsyncBarrierProvider,
				ESPMode::ThreadSafe> InBarrier,
			TSharedRef<
				IDurableExecutionPort,
				ESPMode::ThreadSafe> InDurable)
			: Scheduler(MoveTemp(InScheduler))
			, LockFence(MoveTemp(InLockFence))
			, Barrier(MoveTemp(InBarrier))
			, Durable(MoveTemp(InDurable))
		{
		}

		bool Submit(
			const FExecutionMutationRequest& Request,
			TSharedRef<IMutationHook, ESPMode::ThreadSafe> Hook,
			FString& OutOperationId,
			FExecutionMutationError& OutError)
		{
			if (!Request.IsValid())
			{
				OutError = Request.GetValidationDiagnostic();
				return false;
			}
			const TArray<FString> LockKeys = BuildLockKeys(Request);
			if (LockKeys.Num() == 0)
			{
				OutError = ExecutionFailure(TEXT("operation_impact_set_empty"), TEXT("The execution plan contains no usable target lock keys."), TEXT("plan.impact_set"));
				return false;
			}
			FScopeLock Lock(&Mutex);
			if (Operations.Contains(Request.GetOperationId()))
			{
				OutError = ExecutionFailure(TEXT("operation_id_already_exists"), TEXT("An operation with this identifier has already been submitted."), TEXT("operation_id"));
				OutError.SubjectId = Request.GetOperationId();
				return false;
			}
			if (CallbackState == this
				&& Request.GetReentrancyPolicy()
					== EExecutionReentrancyPolicy::RejectSameTarget
				&& OverlapsActive(LockKeys))
			{
				OutError = ExecutionFailure(TEXT("operation_reentrancy_rejected"), TEXT("A mutation callback cannot submit another operation that overlaps its active targets."));
				OutError.SubjectId = Request.GetOperationId();
				return false;
			}
			const TSharedRef<FOperation, ESPMode::ThreadSafe> Operation =
				MakeShared<FOperation, ESPMode::ThreadSafe>(
					Request,
					MoveTemp(Hook),
					LockKeys);
			Operations.Add(Request.GetOperationId(), Operation);
			Queue.Add(Request.GetOperationId());
			OutOperationId = Request.GetOperationId();
			return true;
		}

		int32 DispatchReady()
		{
			int32 Scheduled = 0;
			for (;;)
			{
				TSharedPtr<FOperation, ESPMode::ThreadSafe> Operation;
				{
					FScopeLock Lock(&Mutex);
					const FString OperationId =
						FindNextDispatchable();
					if (OperationId.IsEmpty())
					{
						return Scheduled;
					}
					Operation = Operations.FindChecked(OperationId);
					Queue.RemoveSingle(OperationId);
					TransitionUnlocked(
						Operation.ToSharedRef(),
						EExecutionPhase::WaitingForLock);
				}

				FExecutionMutationError Failure;
				EExecutionCancellationMode CancellationMode =
					EExecutionCancellationMode::NotRequested;
				if (!CheckBeforeMutation(
					Operation.ToSharedRef(),
					Failure,
					CancellationMode))
				{
					Finalize(
						Operation.ToSharedRef(),
						EExecutionStatus::CancelledNoChange,
						Failure,
						CancellationMode,
						true,
						nullptr);
					continue;
				}
				if (!Charge(
					Operation.ToSharedRef(),
					TEXT("acquire_lock"),
					Failure))
				{
					Finalize(
						Operation.ToSharedRef(),
						EExecutionStatus::Failed,
						Failure,
						EExecutionCancellationMode::NotRequested,
						true,
						nullptr);
					continue;
				}

				FLockFenceLease Lease;
				FExecutionMutationError Error;
				const bool bAcquired = LockFence->TryAcquire(
					Operation->LockKeys,
					Operation->Request.GetOwnerLeaseId(),
					Operation->Request.GetResources(),
					Lease,
					Error);
				if (!bAcquired || !Lease.IsValid()
					|| Lease.GetOwnerId()
						!= Operation->Request.GetOwnerLeaseId()
					|| Lease.GetLockKeys() != Operation->LockKeys)
				{
					if (Error.IsEmpty())
					{
						if (!bAcquired)
							Error = ExecutionFailure(TEXT("operation_lease_invalid"), TEXT("The lock provider failed to acquire a lease without reporting a diagnostic."));
						else if (!Lease.IsValid())
							Error = Lease.GetValidationDiagnostic();
						else if (Lease.GetOwnerId() != Operation->Request.GetOwnerLeaseId())
						{
							Error = ExecutionFailure(TEXT("operation_lease_invalid"), TEXT("The acquired lock lease belongs to a different owner."), TEXT("owner_id"));
							Error.ActualEvidence = MakeShared<FJsonValueString>(Lease.GetOwnerId());
							Error.ExpectedEvidence = MakeShared<FJsonValueString>(Operation->Request.GetOwnerLeaseId());
						}
						else
							Error = ExecutionFailure(TEXT("operation_lease_invalid"), TEXT("The acquired lock lease covers a different target set than requested."), TEXT("lock_keys"));
					}
					Finalize(
						Operation.ToSharedRef(),
						EExecutionStatus::Failed,
						Error,
						EExecutionCancellationMode::NotRequested,
						true,
						nullptr);
					continue;
				}
				{
					FScopeLock Lock(&Mutex);
					Operation->Lease = Lease;
					ActiveLockKeys.Add(
						Operation->Request.GetOperationId(),
						TSet<FString>(Operation->LockKeys));
				}
				if (!CheckBeforeMutation(
					Operation.ToSharedRef(),
					Failure,
					CancellationMode))
				{
					Finalize(
						Operation.ToSharedRef(),
						EExecutionStatus::CancelledNoChange,
						Failure,
						CancellationMode,
						true,
						nullptr);
					continue;
				}

				const TSharedRef<FState, ESPMode::ThreadSafe> KeepAlive =
					AsShared();
				Error = FExecutionMutationError();
				if (!Scheduler->Schedule(
					Operation->Request.GetThreadAffinity(),
					Operation->Request.GetOperationId(),
					[
						KeepAlive,
						OperationId =
							Operation->Request.GetOperationId()
					]()
					{
						KeepAlive->RunScheduled(OperationId);
					},
					Error))
				{
					Finalize(
						Operation.ToSharedRef(),
						EExecutionStatus::Failed,
						DiagnosticOr(
							Error,
							TEXT("operation_schedule_failed"), TEXT("The scheduler failed without reporting a diagnostic.")),
						EExecutionCancellationMode::NotRequested,
						true,
						nullptr);
					continue;
				}
				++Scheduled;
			}
		}

		bool RequestCancellation(
			const FString& OperationId,
			const FString& Reason,
			bool& bOutAccepted,
			FExecutionMutationError& OutError)
		{
			bOutAccepted = false;
			const FString NormalizedId = NormalizeText(OperationId);
			const FString NormalizedReason = NormalizeText(Reason);
			if (NormalizedId.IsEmpty() || NormalizedReason.IsEmpty())
			{
				OutError = NormalizedId.IsEmpty()
					? ExecutionFailure(TEXT("operation_cancellation_invalid"), TEXT("Cancellation requires an operation identifier."), TEXT("operation_id"))
					: ExecutionFailure(TEXT("operation_cancellation_invalid"), TEXT("Cancellation requires a non-empty reason."), TEXT("reason"));
				return false;
			}
			TSharedPtr<FOperation, ESPMode::ThreadSafe> QueuedFinal;
			{
				FScopeLock Lock(&Mutex);
				const TSharedRef<FOperation, ESPMode::ThreadSafe>*
					Found = Operations.Find(NormalizedId);
				if (!Found)
				{
					OutError = ExecutionFailure(TEXT("operation_not_found"), TEXT("The operation requested for cancellation is not registered."), TEXT("operation_id"));
					OutError.SubjectId = OperationId;
					return false;
				}
				if ((*Found)->Receipt.IsSet()
					|| (*Found)->bFinalizing)
				{
					return true;
				}
				(*Found)->bCancellationRequested = true;
				(*Found)->CancellationReason = NormalizedReason;
				bOutAccepted = true;
				if ((*Found)->Phase == EExecutionPhase::Queued
					&& !(*Found)->Lease.IsSet())
				{
					QueuedFinal = *Found;
				}
			}
			if (QueuedFinal.IsValid())
			{
				Finalize(QueuedFinal.ToSharedRef(),
				EExecutionStatus::CancelledNoChange,
				FExecutionMutationError::Failure(TEXT("cancellation_requested"), NormalizedReason, TEXT("cancellation")),
				EExecutionCancellationMode::SafeBoundary,
				true,
				nullptr);
			}
			return true;
		}

		bool GetReceipt(
			const FString& OperationId,
			FExecutionReceipt& OutReceipt,
			FExecutionMutationError& OutError) const
		{
			FScopeLock Lock(&Mutex);
			const TSharedRef<FOperation, ESPMode::ThreadSafe>*
				Found = Operations.Find(NormalizeText(OperationId));
			if (!Found)
			{
				OutError = ExecutionFailure(TEXT("operation_not_found"), TEXT("No registered operation matches the requested receipt."), TEXT("operation_id"));
				OutError.SubjectId = OperationId;
				return false;
			}
			if (!(*Found)->Receipt.IsSet())
			{
				OutError = ExecutionFailure(TEXT("operation_not_terminal"), TEXT("The operation has not completed, so its terminal receipt is not available."));
				OutError.ActualValue = MakeShared<FJsonValueString>(LexToString((*Found)->Phase));
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("terminal"));
				return false;
			}
			OutReceipt = (*Found)->Receipt.GetValue();
			return true;
		}

		bool GetLease(
			const FString& OperationId,
			FLockFenceLease& OutLease,
			FExecutionMutationError& OutError) const
		{
			FScopeLock Lock(&Mutex);
			const TSharedRef<FOperation, ESPMode::ThreadSafe>*
				Found = Operations.Find(NormalizeText(OperationId));
			if (!Found)
			{
				OutError = ExecutionFailure(TEXT("operation_not_found"), TEXT("No registered operation matches the requested lock lease."), TEXT("operation_id"));
				OutError.SubjectId = OperationId;
				return false;
			}
			if (!(*Found)->Lease.IsSet())
			{
				OutError = ExecutionFailure(TEXT("operation_lease_unavailable"), TEXT("The operation has no acquired lock lease."));
				OutError.ActualValue = MakeShared<FJsonValueString>(LexToString((*Found)->Phase));
				return false;
			}
			OutLease = (*Found)->Lease.GetValue();
			return true;
		}

	private:
		struct FOperation
		{
			FOperation(
				const FExecutionMutationRequest& InRequest,
				TSharedRef<
					IMutationHook,
					ESPMode::ThreadSafe> InHook,
				TArray<FString> InLockKeys)
				: Request(InRequest)
				, Hook(MoveTemp(InHook))
				, LockKeys(MoveTemp(InLockKeys))
			{
				PhaseHistory.Add(TEXT("queued"));
			}

			FExecutionMutationRequest Request;
			TSharedRef<IMutationHook, ESPMode::ThreadSafe> Hook;
			TArray<FString> LockKeys;
			EExecutionPhase Phase = EExecutionPhase::Queued;
			TArray<FString> PhaseHistory;
			TOptional<FLockFenceLease> Lease;
			FExecutionDurableSessionPtr DurableSession;
			TOptional<FMutationFacts> MutationFacts;
			bool bMutationStarted = false;
			bool bCancellationRequested = false;
			FString CancellationReason;
			bool bFinalizing = false;
			TOptional<FExecutionReceipt> Receipt;
		};

		static thread_local FState* CallbackState;

		static TArray<FString> BuildLockKeys(
			const FExecutionMutationRequest& Request)
		{
			TArray<FString> Keys;
			for (const PlanningPreflight::FImpact& Impact :
				Request.GetPlan()->GetImpactSet().GetImpacts())
			{
				Keys.Add(Impact.GetIdentity().GetCanonicalKey());
			}
			return NormalizeStrings(MoveTemp(Keys));
		}

		bool OverlapsActive(
			const TArray<FString>& LockKeys) const
		{
			for (const TPair<FString, TSet<FString>>& Entry :
				ActiveLockKeys)
			{
				for (const FString& Key : LockKeys)
				{
					if (Entry.Value.Contains(Key))
					{
						return true;
					}
				}
			}
			return false;
		}

		FString FindNextDispatchable() const
		{
			for (const FString& OperationId : Queue)
			{
				const TSharedRef<FOperation, ESPMode::ThreadSafe>&
					Operation = Operations.FindChecked(OperationId);
				if (!OverlapsActive(Operation->LockKeys))
				{
					return OperationId;
				}
			}
			return FString();
		}

		bool Charge(
			const TSharedRef<FOperation, ESPMode::ThreadSafe>& Operation,
			const FString& Stage,
			FExecutionMutationError& OutFailure) const
		{
			FUeAgentResourceChargeError ChargeError;
			if (Operation->Request.GetResources().TryCharge(
				EUeAgentResourceDimension::WorkUnits,
				1,
				TEXT("C7.ExecutionMutation.") + Stage,
				ChargeError))
			{
				return true;
			}
			OutFailure = DiagnosticOr(ChargeError, TEXT("resource_accounting_failed"), TEXT("Execution resource accounting failed without reporting a diagnostic."));
			return false;
		}

		bool CheckBeforeMutation(
			const TSharedRef<FOperation, ESPMode::ThreadSafe>& Operation,
			FExecutionMutationError& OutFailure,
			EExecutionCancellationMode& OutCancellationMode) const
		{
			{
				FScopeLock Lock(&Mutex);
				if (Operation->bCancellationRequested)
				{
					OutFailure = FExecutionMutationError::Failure(TEXT("cancellation_requested"), Operation->CancellationReason, TEXT("cancellation"));
					OutCancellationMode =
						EExecutionCancellationMode::SafeBoundary;
					return false;
				}
			}
			FUeAgentCancellationRequested CancellationError;
			if (!Operation->Request.GetCancellation().TryCheckpoint(
				CancellationError))
			{
				OutFailure = CancellationError;
				OutCancellationMode =
					EExecutionCancellationMode::SafeBoundary;
				return false;
			}
			return true;
		}

		bool CheckAfterMutation(
			const TSharedRef<FOperation, ESPMode::ThreadSafe>& Operation,
			FExecutionMutationError& OutFailure,
			EExecutionCancellationMode& OutCancellationMode) const
		{
			{
				FScopeLock Lock(&Mutex);
				if (Operation->bCancellationRequested)
				{
					OutFailure = FExecutionMutationError::Failure(TEXT("cancellation_requested"), Operation->CancellationReason, TEXT("cancellation"));
					OutCancellationMode =
						EExecutionCancellationMode::RollbackRequired;
					return false;
				}
			}
			FUeAgentCancellationRequested CancellationError;
			if (!Operation->Request.GetCancellation().TryCheckpoint(
				CancellationError))
			{
				OutFailure = CancellationError;
				OutCancellationMode =
					EExecutionCancellationMode::RollbackRequired;
				return false;
			}
			if (!Operation->Lease.IsSet()
				|| !LockFence->IsCurrent(
					Operation->Lease.GetValue()))
			{
				OutFailure = FExecutionMutationError::Failure(TEXT("operation_fence_stale"), TEXT("The operation lock fence is no longer current."), TEXT("execution"));
				OutCancellationMode =
					EExecutionCancellationMode::RollbackRequired;
				return false;
			}
			return true;
		}

		void RunScheduled(const FString& OperationId)
		{
			TSharedPtr<FOperation, ESPMode::ThreadSafe> Operation;
			{
				FScopeLock Lock(&Mutex);
				const TSharedRef<FOperation, ESPMode::ThreadSafe>*
					Found = Operations.Find(OperationId);
				if (!Found
					|| (*Found)->Receipt.IsSet()
					|| (*Found)->bFinalizing)
				{
					return;
				}
				Operation = *Found;
			}
			if (!Scheduler->IsCurrentThread(
				Operation->Request.GetThreadAffinity()))
			{
				Finalize(Operation.ToSharedRef(),
				EExecutionStatus::CancelledNoChange,
				ExecutionFailure(TEXT("operation_thread_affinity_mismatch"), TEXT("The scheduled callback is running on a different thread than the operation requires."), TEXT("thread_affinity")),
				EExecutionCancellationMode::NotRequested,
				true,
				nullptr);
				return;
			}
			FExecutionMutationError Failure;
			EExecutionCancellationMode CancellationMode =
				EExecutionCancellationMode::NotRequested;
			if (!CheckBeforeMutation(
				Operation.ToSharedRef(),
				Failure,
				CancellationMode))
			{
				Finalize(
					Operation.ToSharedRef(),
					EExecutionStatus::CancelledNoChange,
					Failure,
					CancellationMode,
					true,
					nullptr);
				return;
			}
			if (!Operation->Lease.IsSet()
				|| !LockFence->IsCurrent(
					Operation->Lease.GetValue()))
			{
				Finalize(Operation.ToSharedRef(),
				EExecutionStatus::CancelledNoChange,
				ExecutionFailure(TEXT("operation_fence_stale"), !Operation->Lease.IsSet()
					? TEXT("The operation has no lock lease at execution time.")
					: TEXT("The operation lock lease is no longer current.")),
				EExecutionCancellationMode::NotRequested,
				true,
				nullptr);
				return;
			}
			if (!Charge(
				Operation.ToSharedRef(),
				TEXT("ScheduledCallback"),
				Failure))
			{
				Finalize(
					Operation.ToSharedRef(),
					EExecutionStatus::Failed,
					Failure,
					EExecutionCancellationMode::NotRequested,
					true,
					nullptr);
				return;
			}
			Transition(
				Operation.ToSharedRef(),
				EExecutionPhase::Executing);

			FExecutionMutationError Error;
			FExecutionDurableSessionPtr Session;
			const int64 FencingToken =
				Operation->Lease->GetFencingToken();
			const bool bPrepared = Durable->Prepare(
				Operation->Request,
				FencingToken,
				Operation->Request.GetResources(),
				Session,
				Error);
			if (!bPrepared || !Session.IsValid())
			{
				Finalize(
					Operation.ToSharedRef(),
					EExecutionStatus::Failed,
					DiagnosticOr(
						Error,
						TEXT("operation_durable_prepare_failed"), bPrepared
							? TEXT("Durable preparation succeeded without returning a session.")
							: TEXT("Durable preparation failed without reporting a diagnostic.")),
					EExecutionCancellationMode::NotRequested,
					true,
					nullptr);
				return;
			}
			Operation->DurableSession = Session;
			Operation->bMutationStarted = true;

			FMutationFacts Facts;
			CallbackState = this;
			const bool bExecuted = Operation->Hook->Execute(
				Operation->Request,
				Operation->Lease.GetValue(),
				Operation->Request.GetResources(),
				Facts,
				Error);
			CallbackState = nullptr;
			if (!bExecuted && Facts.IsNoMutationConfirmed())
			{
				Operation->bMutationStarted = false;
				Operation->MutationFacts = Facts;
				// Prepare owns a durable journal even when the native hook made no
				// change. Close it through the same recovery owner before retrying.
				Rollback(
					Operation.ToSharedRef(),
					DiagnosticOr(Error, TEXT("operation_execution_failed"), TEXT("The mutation hook failed without reporting a diagnostic; no target mutation was confirmed.")),
					EExecutionCancellationMode::NotRequested);
				return;
			}
			if (!bExecuted || !Facts.IsValid())
			{
				if (Facts.IsValid())
				{
					Operation->MutationFacts = Facts;
					Rollback(
						Operation.ToSharedRef(),
						DiagnosticOr(
							Error,
							TEXT("operation_execution_failed"), TEXT("The mutation hook failed without reporting a diagnostic.")),
						EExecutionCancellationMode::RollbackRequired);
				}
				else
				{
					if (Error.IsEmpty()) Error = Facts.GetValidationDiagnostic();
					Finalize(
						Operation.ToSharedRef(),
						EExecutionStatus::StateUnknown,
						Error,
						EExecutionCancellationMode::RollbackRequired,
						false,
						nullptr);
				}
				return;
			}
			const int32 UnlockedTargetIndex = FindFirstUnlockedTarget(
				Facts.GetMutatedIdentityKeys(),
				Operation->LockKeys);
			if (UnlockedTargetIndex != INDEX_NONE)
			{
				FExecutionMutationError Unlocked = ExecutionFailure(TEXT("operation_mutation_targets_unlocked"),
					TEXT("The mutation changed a target outside the operation's locked target set."),
					FString::Printf(TEXT("mutated_identity_keys[%d]"), UnlockedTargetIndex));
				Unlocked.ActualEvidence = MakeShared<FJsonValueString>(Facts.GetMutatedIdentityKeys()[UnlockedTargetIndex]);
				Finalize(Operation.ToSharedRef(),
				EExecutionStatus::StateUnknown,
				MoveTemp(Unlocked),
				EExecutionCancellationMode::RollbackRequired,
				false,
				nullptr);
				return;
			}
			Operation->MutationFacts = Facts;
			if (!CheckAfterMutation(
				Operation.ToSharedRef(),
				Failure,
				CancellationMode))
			{
				Rollback(
					Operation.ToSharedRef(),
					Failure,
					CancellationMode);
				return;
			}

			Transition(
				Operation.ToSharedRef(),
				EExecutionPhase::Settling);
			const FBarrierRequest BarrierRequest(
				Facts.GetSubjectGeneration(),
				Facts.GetRequiredBarrierSignals());
			FBarrierResult BarrierResult;
			Error = FExecutionMutationError();
			const bool bBarrierWaited = Barrier->Wait(
				BarrierRequest,
				Operation->Request.GetResources(),
				BarrierResult,
				Error);
			if (!bBarrierWaited || !BarrierResult.IsValid()
				|| !BarrierResult.IsSettled()
				|| BarrierResult.GetSubjectGeneration()
					!= BarrierRequest.GetSubjectGeneration()
				|| !ContainsAll(
					BarrierResult.GetObservedSignals(),
					BarrierRequest.GetRequiredSignals()))
			{
				FExecutionMutationError BarrierFailure;
				if (!bBarrierWaited)
					BarrierFailure = !Error.IsEmpty() ? FExecutionMutationError::WrapFailure(TEXT("operation_barrier_unsettled"), Error)
						: ExecutionFailure(TEXT("operation_barrier_unsettled"), TEXT("The barrier provider failed without providing a diagnostic."), TEXT("barrier_wait_succeeded"));
				else if (!BarrierResult.IsValid())
					BarrierFailure = BarrierResult.GetValidationDiagnostic();
				else if (!BarrierResult.IsSettled())
					BarrierFailure = !BarrierResult.GetSettleDiagnostic().IsEmpty()
						? FExecutionMutationError::WrapFailure(TEXT("operation_barrier_unsettled"), BarrierResult.GetSettleDiagnostic())
						: ExecutionFailure(TEXT("operation_barrier_unsettled"), TEXT("The barrier provider reports that the operation is not settled."), TEXT("settled"));
				else if (BarrierResult.GetSubjectGeneration() != BarrierRequest.GetSubjectGeneration())
				{
					BarrierFailure = ExecutionFailure(TEXT("operation_barrier_unsettled"), TEXT("The barrier result belongs to a different subject generation."), TEXT("subject_generation"));
					BarrierFailure.ActualEvidence = MakeShared<FJsonValueNumber>(BarrierResult.GetSubjectGeneration());
					BarrierFailure.ExpectedEvidence = MakeShared<FJsonValueNumber>(BarrierRequest.GetSubjectGeneration());
				}
				else
				{
					BarrierFailure = ExecutionFailure(TEXT("operation_barrier_unsettled"), TEXT("The barrier result is missing required completion signals."), TEXT("observed_signals"));
					TArray<TSharedPtr<FJsonValue>> Missing;
					for (const FString& Signal : BarrierRequest.GetRequiredSignals())
						if (!BarrierResult.GetObservedSignals().Contains(Signal)) Missing.Add(MakeShared<FJsonValueString>(Signal));
					BarrierFailure.ExpectedValue = MakeShared<FJsonValueArray>(MoveTemp(Missing));
					TArray<TSharedPtr<FJsonValue>> Observed;
					for (const FString& Signal : BarrierResult.GetObservedSignals()) Observed.Add(MakeShared<FJsonValueString>(Signal));
					BarrierFailure.ActualValue = MakeShared<FJsonValueArray>(MoveTemp(Observed));
					BarrierFailure.ExpectedValuePresent = true;
					BarrierFailure.ActualValuePresent = true;
				}
				if (BarrierFailure.Stage.IsEmpty()) BarrierFailure.Stage = TEXT("settling");
				if (bBarrierWaited && !Error.IsEmpty()) BarrierFailure.Append(Error);
				Rollback(Operation.ToSharedRef(),
					MoveTemp(BarrierFailure),
				EExecutionCancellationMode::RollbackRequired);
				return;
			}
			if (!CheckAfterMutation(
				Operation.ToSharedRef(),
				Failure,
				CancellationMode))
			{
				Rollback(
					Operation.ToSharedRef(),
					Failure,
					CancellationMode);
				return;
			}

			Transition(
				Operation.ToSharedRef(),
				EExecutionPhase::Persisting);
			FDurableReceipt DurableReceipt;
			Error = FExecutionMutationError();
			if (!Durable->Persist(
				Operation->DurableSession.ToSharedRef(),
				FencingToken,
				Facts,
				Operation->Request.GetResources(),
				DurableReceipt,
				Error))
			{
				Rollback(
					Operation.ToSharedRef(),
					DiagnosticOr(
						Error,
						TEXT("operation_durable_persist_failed"), TEXT("Durable persistence failed without reporting a diagnostic.")),
					EExecutionCancellationMode::RollbackRequired);
				return;
			}
			const TPair<EExecutionStatus, bool> Mapped =
				MapDurableStatus(DurableReceipt.GetStatus());
			Finalize(Operation.ToSharedRef(),
				Mapped.Key,
				FExecutionMutationError(),
				EExecutionCancellationMode::NotRequested,
				Mapped.Value,
				&DurableReceipt);
		}

		void Rollback(
			const TSharedRef<FOperation, ESPMode::ThreadSafe>& Operation,
			FExecutionMutationError Diagnostic,
			const EExecutionCancellationMode CancellationMode)
		{
			Diagnostic.RollbackAttempted = false;
			if (!Operation->Lease.IsSet()
				|| !Operation->DurableSession.IsValid()
				|| !Operation->MutationFacts.IsSet())
			{
				Finalize(
					Operation,
					EExecutionStatus::StateUnknown,
					Diagnostic,
					CancellationMode,
					false,
					nullptr);
				return;
			}
			Diagnostic.RollbackAttempted = true;
			bool bMemoryConfirmed = Operation->MutationFacts->IsNoMutationConfirmed();
			FExecutionMutationError RollbackError;
			if (Operation->MutationFacts->IsRollbackSupported())
			{
				if (!Operation->Hook->Rollback(
					Operation->Request,
					Operation->MutationFacts.GetValue(),
					Operation->Request.GetResources(),
					bMemoryConfirmed,
					RollbackError))
				{
					bMemoryConfirmed = false;
					Diagnostic.Diagnostics.Add(DiagnosticOr(RollbackError, TEXT("operation_memory_rollback_failed"), TEXT("The mutation hook failed to roll back without reporting a diagnostic.")));
				}
			}
			FDurableReceipt DurableReceipt;
			RollbackError.Reset();
			if (!Durable->Rollback(
				Operation->DurableSession.ToSharedRef(),
				Operation->Lease->GetFencingToken(),
				bMemoryConfirmed,
				Operation->Request.GetResources(),
				DurableReceipt,
				RollbackError))
			{
				Diagnostic.Diagnostics.Add(DiagnosticOr(RollbackError, TEXT("operation_durable_rollback_failed"), TEXT("Durable rollback failed without reporting a diagnostic.")));
				Diagnostic.RollbackSucceeded = false;
				Finalize(
					Operation,
					EExecutionStatus::StateUnknown,
					Diagnostic,
					CancellationMode,
					false,
					nullptr);
				return;
			}
			Diagnostic.RollbackSucceeded = bMemoryConfirmed && DurableReceipt.GetStatus() == EDurableStatus::RolledBack;
			TPair<EExecutionStatus, bool> Mapped =
				MapDurableStatus(DurableReceipt.GetStatus());
			if (Mapped.Key == EExecutionStatus::RolledBack
				&& !bMemoryConfirmed)
			{
				Mapped = {
					EExecutionStatus::StateUnknown,
					false
				};
			}
			else if (Mapped.Key == EExecutionStatus::RolledBack
				&& Operation->MutationFacts->IsNoMutationConfirmed())
			{
				Mapped = { EExecutionStatus::Failed, true };
			}
			Finalize(
				Operation,
				Mapped.Key,
				Diagnostic,
				CancellationMode,
				Mapped.Value,
				&DurableReceipt);
		}

		void Transition(
			const TSharedRef<FOperation, ESPMode::ThreadSafe>& Operation,
			const EExecutionPhase Phase)
		{
			FScopeLock Lock(&Mutex);
			TransitionUnlocked(Operation, Phase);
		}

		static void TransitionUnlocked(
			const TSharedRef<FOperation, ESPMode::ThreadSafe>& Operation,
			const EExecutionPhase Phase)
		{
			if (Operation->Receipt.IsSet()
				|| Operation->bFinalizing)
			{
				return;
			}
			Operation->Phase = Phase;
			Operation->PhaseHistory.Add(LexToString(Phase));
		}

		void Finalize(
			const TSharedRef<FOperation, ESPMode::ThreadSafe>& Operation,
			EExecutionStatus Status,
			FExecutionMutationError Diagnostic,
			const EExecutionCancellationMode CancellationMode,
			bool bRetrySafe,
			const FDurableReceipt* DurableReceipt)
		{
			TOptional<FLockFenceLease> Lease;
			{
				FScopeLock Lock(&Mutex);
				if (Operation->Receipt.IsSet()
					|| Operation->bFinalizing)
				{
					return;
				}
				Operation->bFinalizing = true;
				Operation->Phase = EExecutionPhase::Terminal;
				Operation->PhaseHistory.Add(TEXT("terminal"));
				Queue.RemoveSingle(
					Operation->Request.GetOperationId());
				Lease = Operation->Lease;
			}

			bool bReleaseFailed = false;
			FExecutionMutationError ReleaseError;
			if (Lease.IsSet())
			{
				bReleaseFailed = !LockFence->Release(
					Lease.GetValue(),
					ReleaseError);
			}
			if (bReleaseFailed)
			{
				if (Status != EExecutionStatus::StateUnknown)
				{
					Status = EExecutionStatus::RecoveryRequired;
				}
				FExecutionMutationError ReleaseFailure = FExecutionMutationError::Failure(
					TEXT("operation_lock_release_failed"), TEXT("The operation lock could not be released; recovery is required."), TEXT("execution"));
				if (!Diagnostic.IsEmpty()) ReleaseFailure.PublicCause = MakeShared<FExecutionMutationError>(MoveTemp(Diagnostic));
				if (!ReleaseError.IsEmpty()) ReleaseFailure.Diagnostics.Add(MoveTemp(ReleaseError));
				Diagnostic = MoveTemp(ReleaseFailure);
				bRetrySafe = false;
			}

			TOptional<FDurableReceipt> DurableValue;
			if (DurableReceipt)
			{
				DurableValue = *DurableReceipt;
				const auto& DurableFailure = DurableReceipt->GetFailureDiagnostic();
				if (!DurableFailure.IsEmpty())
				{
					if (Diagnostic.IsEmpty()) Diagnostic = DurableFailure;
					else Diagnostic.Diagnostics.Add(DurableFailure);
				}
				Diagnostic.TransactionStatus = AssetDurable::LexToString(DurableReceipt->GetStatus());
				Diagnostic.RecoveryAction = AssetDurable::LexToString(DurableReceipt->GetRecoveryAction());
			}
			TOptional<int64> FencingToken;
			if (Lease.IsSet())
			{
				FencingToken = Lease->GetFencingToken();
			}
			const FExecutionReceipt Receipt(
				Operation->Request.GetOperationId(),
				EExecutionPhase::Terminal,
				Status,
				MoveTemp(Diagnostic),
				CancellationMode,
				Operation->bMutationStarted,
				MoveTemp(FencingToken),
				bRetrySafe,
				MoveTemp(DurableValue),
				Operation->PhaseHistory);
			{
				FScopeLock Lock(&Mutex);
				Operation->Receipt = Receipt;
				Operation->bFinalizing = false;
				if (!bReleaseFailed)
				{
					ActiveLockKeys.Remove(
						Operation->Request.GetOperationId());
				}
			}
		}

		TSharedRef<IExecutionScheduler, ESPMode::ThreadSafe>
			Scheduler;
		TSharedRef<ILockFenceProvider, ESPMode::ThreadSafe>
			LockFence;
		TSharedRef<IAsyncBarrierProvider, ESPMode::ThreadSafe>
			Barrier;
		TSharedRef<IDurableExecutionPort, ESPMode::ThreadSafe>
			Durable;
		mutable FCriticalSection Mutex;
		TMap<
			FString,
			TSharedRef<FOperation, ESPMode::ThreadSafe>> Operations;
		TArray<FString> Queue;
		TMap<FString, TSet<FString>> ActiveLockKeys;
	};

	thread_local FExecutionMutationCoordinator::FState*
		FExecutionMutationCoordinator::FState::CallbackState = nullptr;

	FExecutionMutationCoordinator::FExecutionMutationCoordinator(
		TSharedRef<
			IExecutionScheduler,
			ESPMode::ThreadSafe> InScheduler,
		TSharedRef<
			ILockFenceProvider,
			ESPMode::ThreadSafe> InLockFence,
		TSharedRef<
			IAsyncBarrierProvider,
			ESPMode::ThreadSafe> InBarrier,
		TSharedRef<
			IDurableExecutionPort,
			ESPMode::ThreadSafe> InDurable)
		: State(MakeShared<FState, ESPMode::ThreadSafe>(
			MoveTemp(InScheduler),
			MoveTemp(InLockFence),
			MoveTemp(InBarrier),
			MoveTemp(InDurable)))
	{
	}

	FExecutionMutationCoordinator::~FExecutionMutationCoordinator() =
		default;

	bool FExecutionMutationCoordinator::Submit(
		const FExecutionMutationRequest& Request,
		TSharedRef<IMutationHook, ESPMode::ThreadSafe> Hook,
		FString& OutOperationId,
		FExecutionMutationError& OutError)
	{
		return State->Submit(
			Request,
			MoveTemp(Hook),
			OutOperationId,
			OutError);
	}

	int32 FExecutionMutationCoordinator::DispatchReady()
	{
		return State->DispatchReady();
	}

	bool FExecutionMutationCoordinator::RequestCancellation(
		const FString& OperationId,
		const FString& Reason,
		bool& bOutAccepted,
		FExecutionMutationError& OutError)
	{
		return State->RequestCancellation(
			OperationId,
			Reason,
			bOutAccepted,
			OutError);
	}

	bool FExecutionMutationCoordinator::GetReceipt(
		const FString& OperationId,
		FExecutionReceipt& OutReceipt,
		FExecutionMutationError& OutError) const
	{
		return State->GetReceipt(
			OperationId,
			OutReceipt,
			OutError);
	}

	bool FExecutionMutationCoordinator::GetLease(
		const FString& OperationId,
		FLockFenceLease& OutLease,
		FExecutionMutationError& OutError) const
	{
		return State->GetLease(
			OperationId,
			OutLease,
			OutError);
	}
}
