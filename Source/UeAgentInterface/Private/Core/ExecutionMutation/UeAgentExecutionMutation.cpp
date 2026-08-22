#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"

#include "Async/Async.h"
#include "Misc/ScopeLock.h"

namespace UE::UAI::ExecutionMutation
{
	namespace
	{
		using AssetDurable::EDurableStatus;
		using ContextResource::EUeAgentResourceDimension;
		using ContextResource::FUeAgentCancellationRequested;
		using ContextResource::FUeAgentOperationDeadlineExceeded;
		using ContextResource::FUeAgentResourceBudgetExceeded;

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

		static FString ErrorCodeOr(
			const FExecutionMutationError& Error,
			const TCHAR* Fallback)
		{
			return Error.IsPresent()
				? Error.GetCode()
				: FString(Fallback);
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

		static bool IsSubset(
			const TArray<FString>& Candidate,
			const TArray<FString>& Allowed)
		{
			for (const FString& Value : Candidate)
			{
				if (!Allowed.Contains(Value))
				{
					return false;
				}
			}
			return true;
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

	FExecutionMutationError::FExecutionMutationError(
		FString InCode,
		FString InDetail)
		: Code(NormalizeText(MoveTemp(InCode)))
		, Detail(NormalizeText(MoveTemp(InDetail)))
	{
	}

	const FString& FExecutionMutationError::GetCode() const
	{
		return Code;
	}

	const FString& FExecutionMutationError::GetDetail() const
	{
		return Detail;
	}

	bool FExecutionMutationError::IsPresent() const
	{
		return !Code.IsEmpty();
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

	FLockFenceLease::FLockFenceLease(
		FString InOwnerId,
		TArray<FString> InLockKeys,
		const int64 InFencingToken)
		: OwnerId(NormalizeText(MoveTemp(InOwnerId)))
		, LockKeys(NormalizeStrings(MoveTemp(InLockKeys)))
		, FencingToken(InFencingToken)
	{
		bValid = !OwnerId.IsEmpty()
			&& LockKeys.Num() > 0
			&& FencingToken >= 0;
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
		return bValid;
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

	FBarrierResult::FBarrierResult(
		const int64 InSubjectGeneration,
		TArray<FString> InObservedSignals,
		FString InPostBarrierRevision,
		const bool bInSettled)
		: SubjectGeneration(InSubjectGeneration)
		, ObservedSignals(NormalizeStrings(
			MoveTemp(InObservedSignals)))
		, PostBarrierRevision(NormalizeText(
			MoveTemp(InPostBarrierRevision)))
		, bSettled(bInSettled)
	{
		bValid = SubjectGeneration >= 0
			&& !PostBarrierRevision.IsEmpty();
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

	bool FBarrierResult::IsValid() const
	{
		return bValid;
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
		bValid = SubjectGeneration >= 0
			&& MutatedIdentityKeys.Num() > 0;
		for (const FAssetDurableTarget& Target : DurableTargets)
		{
			bValid = bValid && Target.IsValid();
		}
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
		return bValid;
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
		bValid = !OperationId.IsEmpty()
			&& !OwnerLeaseId.IsEmpty()
			&& !Plan->GetRequestId().IsEmpty()
			&& Plan->GetImpactSet().GetImpacts().Num() > 0
			&& !Plan->GetFingerprint().IsEmpty()
			&& ExecutionContext->GetRequestKind()
				== EUeAgentRequestKind::Operation
			&& ExecutionContext->GetDescriptorIdentity()
				== Plan->GetDescriptorIdentity();
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
		return bValid;
	}

	FExecutionReceipt::FExecutionReceipt(
		FString InOperationId,
		const EExecutionPhase InPhase,
		const EExecutionStatus InStatus,
		FString InErrorCode,
		const EExecutionCancellationMode InCancellationMode,
		const bool bInMutationStarted,
		TOptional<int64> InFencingToken,
		const bool bInRetrySafe,
		TOptional<FDurableReceipt> InDurableReceipt,
		TArray<FString> InPhaseHistory)
		: OperationId(NormalizeText(MoveTemp(InOperationId)))
		, Phase(InPhase)
		, Status(InStatus)
		, ErrorCode(NormalizeText(MoveTemp(InErrorCode)))
		, CancellationMode(InCancellationMode)
		, bMutationStarted(bInMutationStarted)
		, FencingToken(MoveTemp(InFencingToken))
		, bRetrySafe(bInRetrySafe)
		, DurableReceipt(MoveTemp(InDurableReceipt))
		, PhaseHistory(MoveTemp(InPhaseHistory))
	{
		bValid = !OperationId.IsEmpty()
			&& Phase == EExecutionPhase::Terminal
			&& PhaseHistory.Num() > 0
			&& PhaseHistory.Last() == TEXT("terminal")
			&& (!FencingToken.IsSet()
				|| FencingToken.GetValue() >= 0);
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
		return ErrorCode;
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
		return bValid;
	}

	bool FGameThreadExecutionScheduler::Schedule(
		const EExecutionThreadAffinity Affinity,
		const FString& OperationId,
		TFunction<void()> Callback,
		FExecutionMutationError& OutError)
	{
		if (Affinity != EExecutionThreadAffinity::GameThread)
		{
			OutError = FExecutionMutationError(
				TEXT("operation_thread_affinity_unsupported"));
			return false;
		}
		if (OperationId.TrimStartAndEnd().IsEmpty() || !Callback)
		{
			OutError = FExecutionMutationError(
				TEXT("operation_schedule_invalid"));
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
			OutError = FExecutionMutationError(
				TEXT("operation_lock_request_invalid"));
			return false;
		}
		FUeAgentResourceBudgetExceeded BudgetError;
		if (!Resources.TryCharge(
			EUeAgentResourceDimension::WorkUnits,
			1,
			TEXT("C7.ExecutionMutation.LockFence.Acquire"),
			BudgetError))
		{
			OutError = FExecutionMutationError(
				TEXT("resource_budget_exceeded"));
			return false;
		}
		FScopeLock Lock(&Mutex);
		for (const FString& Key : NormalizedKeys)
		{
			if (HeldLocks.Contains(Key))
			{
				OutError = FExecutionMutationError(
					TEXT("operation_target_locked"),
					Key);
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
				OutError = FExecutionMutationError(
					TEXT("operation_fence_stale"),
					Key);
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
				ESPMode::ThreadSafe> InDurable,
			const int32 InMaxQueueItems)
			: Scheduler(MoveTemp(InScheduler))
			, LockFence(MoveTemp(InLockFence))
			, Barrier(MoveTemp(InBarrier))
			, Durable(MoveTemp(InDurable))
			, MaxQueueItems(InMaxQueueItems)
		{
			checkf(
				MaxQueueItems > 0,
				TEXT("ExecutionMutation queue must be bounded and positive."));
		}

		bool Submit(
			const FExecutionMutationRequest& Request,
			TSharedRef<IMutationHook, ESPMode::ThreadSafe> Hook,
			FString& OutOperationId,
			FExecutionMutationError& OutError)
		{
			if (!Request.IsValid())
			{
				OutError = FExecutionMutationError(
					TEXT("operation_request_invalid"));
				return false;
			}
			const TArray<FString> LockKeys = BuildLockKeys(Request);
			if (LockKeys.Num() == 0)
			{
				OutError = FExecutionMutationError(
					TEXT("operation_impact_set_empty"));
				return false;
			}
			FScopeLock Lock(&Mutex);
			if (Operations.Contains(Request.GetOperationId()))
			{
				OutError = FExecutionMutationError(
					TEXT("operation_id_already_exists"));
				return false;
			}
			if (CallbackState == this
				&& Request.GetReentrancyPolicy()
					== EExecutionReentrancyPolicy::RejectSameTarget
				&& OverlapsActive(LockKeys))
			{
				OutError = FExecutionMutationError(
					TEXT("operation_reentrancy_rejected"));
				return false;
			}
			if (Queue.Num() >= MaxQueueItems)
			{
				OutError = FExecutionMutationError(
					TEXT("operation_queue_saturated"));
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

				FString FailureCode;
				EExecutionCancellationMode CancellationMode =
					EExecutionCancellationMode::NotRequested;
				if (!CheckBeforeMutation(
					Operation.ToSharedRef(),
					FailureCode,
					CancellationMode))
				{
					Finalize(
						Operation.ToSharedRef(),
						EExecutionStatus::CancelledNoChange,
						FailureCode,
						CancellationMode,
						true,
						nullptr);
					continue;
				}
				if (!Charge(
					Operation.ToSharedRef(),
					TEXT("acquire_lock"),
					FailureCode))
				{
					Finalize(
						Operation.ToSharedRef(),
						EExecutionStatus::Failed,
						FailureCode,
						EExecutionCancellationMode::NotRequested,
						true,
						nullptr);
					continue;
				}

				FLockFenceLease Lease;
				FExecutionMutationError Error;
				if (!LockFence->TryAcquire(
					Operation->LockKeys,
					Operation->Request.GetOwnerLeaseId(),
					Operation->Request.GetResources(),
					Lease,
					Error)
					|| !Lease.IsValid()
					|| Lease.GetOwnerId()
						!= Operation->Request.GetOwnerLeaseId()
					|| Lease.GetLockKeys() != Operation->LockKeys)
				{
					Finalize(
						Operation.ToSharedRef(),
						EExecutionStatus::Failed,
						ErrorCodeOr(
							Error,
							TEXT("operation_lease_invalid")),
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
					FailureCode,
					CancellationMode))
				{
					Finalize(
						Operation.ToSharedRef(),
						EExecutionStatus::CancelledNoChange,
						FailureCode,
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
						ErrorCodeOr(
							Error,
							TEXT("operation_schedule_failed")),
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
				OutError = FExecutionMutationError(
					TEXT("operation_cancellation_invalid"));
				return false;
			}
			TSharedPtr<FOperation, ESPMode::ThreadSafe> QueuedFinal;
			{
				FScopeLock Lock(&Mutex);
				const TSharedRef<FOperation, ESPMode::ThreadSafe>*
					Found = Operations.Find(NormalizedId);
				if (!Found)
				{
					OutError = FExecutionMutationError(
						TEXT("operation_not_found"));
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
				Finalize(
					QueuedFinal.ToSharedRef(),
					EExecutionStatus::CancelledNoChange,
					TEXT("cancellation_requested"),
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
				OutError = FExecutionMutationError(
					TEXT("operation_not_found"));
				return false;
			}
			if (!(*Found)->Receipt.IsSet())
			{
				OutError = FExecutionMutationError(
					TEXT("operation_not_terminal"));
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
				OutError = FExecutionMutationError(
					TEXT("operation_not_found"));
				return false;
			}
			if (!(*Found)->Lease.IsSet())
			{
				OutError = FExecutionMutationError(
					TEXT("operation_lease_unavailable"));
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
			FString& OutFailureCode) const
		{
			FUeAgentResourceBudgetExceeded BudgetError;
			if (Operation->Request.GetResources().TryCharge(
				EUeAgentResourceDimension::WorkUnits,
				1,
				TEXT("C7.ExecutionMutation.") + Stage,
				BudgetError))
			{
				return true;
			}
			OutFailureCode = TEXT("resource_budget_exceeded");
			return false;
		}

		bool CheckBeforeMutation(
			const TSharedRef<FOperation, ESPMode::ThreadSafe>& Operation,
			FString& OutFailureCode,
			EExecutionCancellationMode& OutCancellationMode) const
		{
			{
				FScopeLock Lock(&Mutex);
				if (Operation->bCancellationRequested)
				{
					OutFailureCode = TEXT("cancellation_requested");
					OutCancellationMode =
						EExecutionCancellationMode::SafeBoundary;
					return false;
				}
			}
			FUeAgentCancellationRequested CancellationError;
			if (!Operation->Request.GetCancellation().TryCheckpoint(
				CancellationError))
			{
				OutFailureCode = TEXT("cancellation_requested");
				OutCancellationMode =
					EExecutionCancellationMode::SafeBoundary;
				return false;
			}
			FUeAgentOperationDeadlineExceeded DeadlineError;
			if (!Operation->Request.GetDeadline().TryCheckpoint(
				TEXT("C7.ExecutionMutation.BeforeMutation"),
				DeadlineError))
			{
				OutFailureCode = TEXT("operation_deadline_exceeded");
				OutCancellationMode =
					EExecutionCancellationMode::NotRequested;
				return false;
			}
			return true;
		}

		bool CheckAfterMutation(
			const TSharedRef<FOperation, ESPMode::ThreadSafe>& Operation,
			FString& OutFailureCode,
			EExecutionCancellationMode& OutCancellationMode) const
		{
			{
				FScopeLock Lock(&Mutex);
				if (Operation->bCancellationRequested)
				{
					OutFailureCode = TEXT("cancellation_requested");
					OutCancellationMode =
						EExecutionCancellationMode::RollbackRequired;
					return false;
				}
			}
			FUeAgentCancellationRequested CancellationError;
			if (!Operation->Request.GetCancellation().TryCheckpoint(
				CancellationError))
			{
				OutFailureCode = TEXT("cancellation_requested");
				OutCancellationMode =
					EExecutionCancellationMode::RollbackRequired;
				return false;
			}
			FUeAgentOperationDeadlineExceeded DeadlineError;
			if (!Operation->Request.GetDeadline().TryCheckpoint(
				TEXT("C7.ExecutionMutation.AfterMutation"),
				DeadlineError))
			{
				OutFailureCode = TEXT("operation_deadline_exceeded");
				OutCancellationMode =
					EExecutionCancellationMode::RollbackRequired;
				return false;
			}
			if (!Operation->Lease.IsSet()
				|| !LockFence->IsCurrent(
					Operation->Lease.GetValue()))
			{
				OutFailureCode = TEXT("operation_fence_stale");
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
				Finalize(
					Operation.ToSharedRef(),
					EExecutionStatus::CancelledNoChange,
					TEXT("operation_thread_affinity_mismatch"),
					EExecutionCancellationMode::NotRequested,
					true,
					nullptr);
				return;
			}
			FString FailureCode;
			EExecutionCancellationMode CancellationMode =
				EExecutionCancellationMode::NotRequested;
			if (!CheckBeforeMutation(
				Operation.ToSharedRef(),
				FailureCode,
				CancellationMode))
			{
				Finalize(
					Operation.ToSharedRef(),
					EExecutionStatus::CancelledNoChange,
					FailureCode,
					CancellationMode,
					true,
					nullptr);
				return;
			}
			if (!Operation->Lease.IsSet()
				|| !LockFence->IsCurrent(
					Operation->Lease.GetValue()))
			{
				Finalize(
					Operation.ToSharedRef(),
					EExecutionStatus::CancelledNoChange,
					TEXT("operation_fence_stale"),
					EExecutionCancellationMode::NotRequested,
					true,
					nullptr);
				return;
			}
			if (!Charge(
				Operation.ToSharedRef(),
				TEXT("ScheduledCallback"),
				FailureCode))
			{
				Finalize(
					Operation.ToSharedRef(),
					EExecutionStatus::Failed,
					FailureCode,
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
			if (!Durable->Prepare(
				Operation->Request,
				FencingToken,
				Operation->Request.GetResources(),
				Session,
				Error)
				|| !Session.IsValid())
			{
				Finalize(
					Operation.ToSharedRef(),
					EExecutionStatus::Failed,
					ErrorCodeOr(
						Error,
						TEXT("operation_durable_prepare_failed")),
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
			if (!bExecuted || !Facts.IsValid())
			{
				if (Facts.IsValid())
				{
					Operation->MutationFacts = Facts;
					Rollback(
						Operation.ToSharedRef(),
						ErrorCodeOr(
							Error,
							TEXT("operation_execution_failed")),
						EExecutionCancellationMode::RollbackRequired);
				}
				else
				{
					Finalize(
						Operation.ToSharedRef(),
						EExecutionStatus::StateUnknown,
						ErrorCodeOr(
							Error,
							TEXT("operation_execution_failed")),
						EExecutionCancellationMode::RollbackRequired,
						false,
						nullptr);
				}
				return;
			}
			if (!IsSubset(
				Facts.GetMutatedIdentityKeys(),
				Operation->LockKeys))
			{
				Finalize(
					Operation.ToSharedRef(),
					EExecutionStatus::StateUnknown,
					TEXT("operation_mutation_targets_unlocked"),
					EExecutionCancellationMode::RollbackRequired,
					false,
					nullptr);
				return;
			}
			Operation->MutationFacts = Facts;
			if (!CheckAfterMutation(
				Operation.ToSharedRef(),
				FailureCode,
				CancellationMode))
			{
				Rollback(
					Operation.ToSharedRef(),
					FailureCode,
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
			if (!Barrier->Wait(
				BarrierRequest,
				Operation->Request.GetResources(),
				BarrierResult,
				Error)
				|| !BarrierResult.IsValid()
				|| !BarrierResult.IsSettled()
				|| BarrierResult.GetSubjectGeneration()
					!= BarrierRequest.GetSubjectGeneration()
				|| !ContainsAll(
					BarrierResult.GetObservedSignals(),
					BarrierRequest.GetRequiredSignals()))
			{
				Rollback(
					Operation.ToSharedRef(),
					TEXT("operation_barrier_unsettled"),
					EExecutionCancellationMode::RollbackRequired);
				return;
			}
			if (!CheckAfterMutation(
				Operation.ToSharedRef(),
				FailureCode,
				CancellationMode))
			{
				Rollback(
					Operation.ToSharedRef(),
					FailureCode,
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
					ErrorCodeOr(
						Error,
						TEXT("operation_durable_persist_failed")),
					EExecutionCancellationMode::RollbackRequired);
				return;
			}
			const TPair<EExecutionStatus, bool> Mapped =
				MapDurableStatus(DurableReceipt.GetStatus());
			Finalize(
				Operation.ToSharedRef(),
				Mapped.Key,
				FString(),
				EExecutionCancellationMode::NotRequested,
				Mapped.Value,
				&DurableReceipt);
		}

		void Rollback(
			const TSharedRef<FOperation, ESPMode::ThreadSafe>& Operation,
			const FString& ErrorCode,
			const EExecutionCancellationMode CancellationMode)
		{
			if (!Operation->Lease.IsSet()
				|| !Operation->DurableSession.IsValid()
				|| !Operation->MutationFacts.IsSet())
			{
				Finalize(
					Operation,
					EExecutionStatus::StateUnknown,
					ErrorCode,
					CancellationMode,
					false,
					nullptr);
				return;
			}
			bool bMemoryConfirmed = false;
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
				}
			}
			FDurableReceipt DurableReceipt;
			if (!Durable->Rollback(
				Operation->DurableSession.ToSharedRef(),
				Operation->Lease->GetFencingToken(),
				bMemoryConfirmed,
				Operation->Request.GetResources(),
				DurableReceipt,
				RollbackError))
			{
				Finalize(
					Operation,
					EExecutionStatus::StateUnknown,
					ErrorCode,
					CancellationMode,
					false,
					nullptr);
				return;
			}
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
			Finalize(
				Operation,
				Mapped.Key,
				ErrorCode,
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
			FString ErrorCode,
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
			if (Lease.IsSet())
			{
				FExecutionMutationError ReleaseError;
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
				ErrorCode = TEXT("operation_lock_release_failed");
				bRetrySafe = false;
			}

			TOptional<FDurableReceipt> DurableValue;
			if (DurableReceipt)
			{
				DurableValue = *DurableReceipt;
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
				MoveTemp(ErrorCode),
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
		int32 MaxQueueItems = 0;
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
			ESPMode::ThreadSafe> InDurable,
		const int32 InMaxQueueItems)
		: State(MakeShared<FState, ESPMode::ThreadSafe>(
			MoveTemp(InScheduler),
			MoveTemp(InLockFence),
			MoveTemp(InBarrier),
			MoveTemp(InDurable),
			InMaxQueueItems))
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
