#include "Core/ExecutionMutation/UeAgentLongRunningOperation.h"

#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"

namespace UE::UAI::ExecutionMutation
{
	namespace
	{
		using ContextResource::FUeAgentCancellationRequest;
		using ContextResource::FUeAgentCancellationSource;

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

		static FString ErrorCodeOr(
			const FExecutionMutationError& Error,
			const TCHAR* Fallback)
		{
			return Error.IsPresent()
				? Error.GetCode()
				: FString(Fallback);
		}

		static bool IsLifecycleObservationValid(
			const FLongRunningWorkObservation& Observation)
		{
			if (Observation.GetGeneration() <= 0
				|| Observation.GetState()
					== ELongRunningWorkState::Invalid)
			{
				return false;
			}
			if (Observation.GetState()
					== ELongRunningWorkState::ExitedFailure
				|| Observation.GetState()
					== ELongRunningWorkState::StateUnknown)
			{
				return !Observation.GetErrorCode().IsEmpty();
			}
			return Observation.GetErrorCode().IsEmpty();
		}
	}

	FString LexToString(const ELongRunningOperationPhase Value)
	{
		switch (Value)
		{
		case ELongRunningOperationPhase::Accepted:
			return TEXT("accepted");
		case ELongRunningOperationPhase::Running:
			return TEXT("running");
		case ELongRunningOperationPhase::StopRequested:
			return TEXT("stop_requested");
		case ELongRunningOperationPhase::Joining:
			return TEXT("joining");
		case ELongRunningOperationPhase::Terminal:
			return TEXT("terminal");
		default:
			return TEXT("unknown");
		}
	}

	FString LexToString(const ELongRunningOperationStatus Value)
	{
		switch (Value)
		{
		case ELongRunningOperationStatus::None:
			return TEXT("none");
		case ELongRunningOperationStatus::Succeeded:
			return TEXT("succeeded");
		case ELongRunningOperationStatus::Cancelled:
			return TEXT("cancelled");
		case ELongRunningOperationStatus::Failed:
			return TEXT("failed");
		case ELongRunningOperationStatus::StateUnknown:
			return TEXT("state_unknown");
		default:
			return TEXT("unknown");
		}
	}

	FString LexToString(const ELongRunningWorkState Value)
	{
		switch (Value)
		{
		case ELongRunningWorkState::Invalid:
			return TEXT("invalid");
		case ELongRunningWorkState::Running:
			return TEXT("running");
		case ELongRunningWorkState::ExitedSuccess:
			return TEXT("exited_success");
		case ELongRunningWorkState::ExitedFailure:
			return TEXT("exited_failure");
		case ELongRunningWorkState::StateUnknown:
			return TEXT("state_unknown");
		default:
			return TEXT("unknown");
		}
	}

	FLongRunningWorkIdentity::FLongRunningWorkIdentity(
		FString InResourceId,
		const int64 InGeneration)
		: ResourceId(NormalizeText(MoveTemp(InResourceId)))
		, Generation(InGeneration)
	{
	}

	const FString& FLongRunningWorkIdentity::GetResourceId() const
	{
		return ResourceId;
	}

	int64 FLongRunningWorkIdentity::GetGeneration() const
	{
		return Generation;
	}

	bool FLongRunningWorkIdentity::IsValid() const
	{
		return !ResourceId.IsEmpty() && Generation > 0;
	}

	bool FLongRunningWorkIdentity::operator==(
		const FLongRunningWorkIdentity& Other) const
	{
		return ResourceId == Other.ResourceId
			&& Generation == Other.Generation;
	}

	FLongRunningOperationSignal::FLongRunningOperationSignal(
		FString InTypeId,
		const int64 InRevision,
		FUeAgentCanonicalValueRef InPayload)
		: TypeId(NormalizeText(MoveTemp(InTypeId)))
		, Revision(InRevision)
		, Payload(MoveTemp(InPayload))
	{
	}

	const FString& FLongRunningOperationSignal::GetTypeId() const
	{
		return TypeId;
	}

	int64 FLongRunningOperationSignal::GetRevision() const
	{
		return Revision;
	}

	const FUeAgentCanonicalValueRef&
	FLongRunningOperationSignal::GetPayload() const
	{
		return Payload;
	}

	bool FLongRunningOperationSignal::IsValid() const
	{
		return !TypeId.IsEmpty() && Revision > 0;
	}

	FLongRunningWorkObservation::FLongRunningWorkObservation(
		const int64 InGeneration,
		const ELongRunningWorkState InState,
		FString InErrorCode,
		TOptional<FLongRunningOperationSignal> InSignal)
		: Generation(InGeneration)
		, State(InState)
		, ErrorCode(NormalizeText(MoveTemp(InErrorCode)))
		, Signal(MoveTemp(InSignal))
	{
	}

	int64 FLongRunningWorkObservation::GetGeneration() const
	{
		return Generation;
	}

	ELongRunningWorkState
	FLongRunningWorkObservation::GetState() const
	{
		return State;
	}

	const FString& FLongRunningWorkObservation::GetErrorCode() const
	{
		return ErrorCode;
	}

	const TOptional<FLongRunningOperationSignal>&
	FLongRunningWorkObservation::GetSignal() const
	{
		return Signal;
	}

	bool FLongRunningWorkObservation::IsValid() const
	{
		return IsLifecycleObservationValid(*this)
			&& (!Signal.IsSet()
				|| Signal.GetValue().IsValid());
	}

	FLongRunningOperationRequest::FLongRunningOperationRequest(
		FString InOperationId,
		FString InOwnerLeaseId,
		FString InServerSessionId,
		FUeAgentDescriptorIdentity InDescriptorIdentity,
		FUeAgentExecutionContextRefOwner InExecutionContext,
		TSharedRef<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe> InResources,
		FUeAgentOperationDeadline InDeadline)
		: OperationId(NormalizeText(MoveTemp(InOperationId)))
		, OwnerLeaseId(NormalizeText(MoveTemp(InOwnerLeaseId)))
		, ServerSessionId(NormalizeText(MoveTemp(InServerSessionId)))
		, DescriptorIdentity(MoveTemp(InDescriptorIdentity))
		, ExecutionContext(MoveTemp(InExecutionContext))
		, Resources(MoveTemp(InResources))
		, Deadline(MoveTemp(InDeadline))
	{
		bValid = !OperationId.IsEmpty()
			&& !OwnerLeaseId.IsEmpty()
			&& !ServerSessionId.IsEmpty()
			&& DescriptorIdentity.IsValid()
			&& ExecutionContext->GetRequestKind()
				== ContextResource::EUeAgentRequestKind::Operation
			&& ExecutionContext->GetDescriptorIdentity()
				== DescriptorIdentity;
	}

	const FString& FLongRunningOperationRequest::GetOperationId() const
	{
		return OperationId;
	}

	const FString& FLongRunningOperationRequest::GetOwnerLeaseId() const
	{
		return OwnerLeaseId;
	}

	const FString& FLongRunningOperationRequest::GetServerSessionId() const
	{
		return ServerSessionId;
	}

	const FUeAgentDescriptorIdentity&
	FLongRunningOperationRequest::GetDescriptorIdentity() const
	{
		return DescriptorIdentity;
	}

	const FUeAgentExecutionContextRef&
	FLongRunningOperationRequest::GetExecutionContext() const
	{
		return *ExecutionContext;
	}

	FUeAgentResourceTracker&
	FLongRunningOperationRequest::GetResources() const
	{
		return *Resources;
	}

	const FUeAgentOperationDeadline&
	FLongRunningOperationRequest::GetDeadline() const
	{
		return Deadline;
	}

	bool FLongRunningOperationRequest::IsValid() const
	{
		return bValid;
	}

	FLongRunningOperationSnapshot::FLongRunningOperationSnapshot(
		FString InOperationId,
		FString InOwnerLeaseId,
		FString InServerSessionId,
		FUeAgentDescriptorIdentity InDescriptorIdentity,
		const ELongRunningOperationPhase InPhase,
		FLongRunningWorkIdentity InWorkIdentity,
		const ELongRunningOperationStatus InStatus,
		FString InErrorCode,
		const bool bInStopRequested,
		TOptional<FLongRunningOperationSignal> InLatestSignal)
		: OperationId(NormalizeText(MoveTemp(InOperationId)))
		, OwnerLeaseId(NormalizeText(MoveTemp(InOwnerLeaseId)))
		, ServerSessionId(NormalizeText(MoveTemp(InServerSessionId)))
		, DescriptorIdentity(MoveTemp(InDescriptorIdentity))
		, Phase(InPhase)
		, WorkIdentity(MoveTemp(InWorkIdentity))
		, Status(InStatus)
		, ErrorCode(NormalizeText(MoveTemp(InErrorCode)))
		, bStopRequested(bInStopRequested)
		, LatestSignal(MoveTemp(InLatestSignal))
	{
		bValid = !OperationId.IsEmpty()
			&& !OwnerLeaseId.IsEmpty()
			&& !ServerSessionId.IsEmpty()
			&& DescriptorIdentity.IsSet()
			&& DescriptorIdentity->IsValid()
			&& (WorkIdentity.IsValid()
				|| (WorkIdentity.GetResourceId().IsEmpty()
					&& WorkIdentity.GetGeneration() == 0))
			&& (!LatestSignal.IsSet()
				|| (WorkIdentity.IsValid()
					&& LatestSignal.GetValue().IsValid()))
			&& ((Phase == ELongRunningOperationPhase::Terminal)
				== (Status != ELongRunningOperationStatus::None));
	}

	const FString& FLongRunningOperationSnapshot::GetOperationId() const
	{
		return OperationId;
	}

	const FString& FLongRunningOperationSnapshot::GetOwnerLeaseId() const
	{
		return OwnerLeaseId;
	}

	const FString& FLongRunningOperationSnapshot::GetServerSessionId() const
	{
		return ServerSessionId;
	}

	const FUeAgentDescriptorIdentity&
	FLongRunningOperationSnapshot::GetDescriptorIdentity() const
	{
		check(DescriptorIdentity.IsSet());
		return DescriptorIdentity.GetValue();
	}

	ELongRunningOperationPhase
	FLongRunningOperationSnapshot::GetPhase() const
	{
		return Phase;
	}

	const FString&
	FLongRunningOperationSnapshot::GetNativeResourceId() const
	{
		return WorkIdentity.GetResourceId();
	}

	int64 FLongRunningOperationSnapshot::GetGeneration() const
	{
		return WorkIdentity.GetGeneration();
	}

	ELongRunningOperationStatus
	FLongRunningOperationSnapshot::GetStatus() const
	{
		return Status;
	}

	const FString& FLongRunningOperationSnapshot::GetErrorCode() const
	{
		return ErrorCode;
	}

	bool FLongRunningOperationSnapshot::IsStopRequested() const
	{
		return bStopRequested;
	}

	const TOptional<FLongRunningOperationSignal>&
	FLongRunningOperationSnapshot::GetLatestSignal() const
	{
		return LatestSignal;
	}

	bool FLongRunningOperationSnapshot::IsTerminal() const
	{
		return Phase == ELongRunningOperationPhase::Terminal;
	}

	bool FLongRunningOperationSnapshot::IsValid() const
	{
		return bValid;
	}

	FLongRunningOperationReceipt::FLongRunningOperationReceipt(
		FString InOperationId,
		FLongRunningWorkIdentity InWorkIdentity,
		const ELongRunningOperationStatus InStatus,
		FString InErrorCode,
		const bool bInRetrySafe)
		: OperationId(NormalizeText(MoveTemp(InOperationId)))
		, WorkIdentity(MoveTemp(InWorkIdentity))
		, Status(InStatus)
		, ErrorCode(NormalizeText(MoveTemp(InErrorCode)))
		, bRetrySafe(bInRetrySafe)
	{
		bValid = !OperationId.IsEmpty()
			&& Status != ELongRunningOperationStatus::None
			&& (WorkIdentity.IsValid()
				|| (WorkIdentity.GetResourceId().IsEmpty()
					&& WorkIdentity.GetGeneration() == 0));
	}

	const FString& FLongRunningOperationReceipt::GetOperationId() const
	{
		return OperationId;
	}

	const FString&
	FLongRunningOperationReceipt::GetNativeResourceId() const
	{
		return WorkIdentity.GetResourceId();
	}

	int64 FLongRunningOperationReceipt::GetGeneration() const
	{
		return WorkIdentity.GetGeneration();
	}

	ELongRunningOperationStatus
	FLongRunningOperationReceipt::GetStatus() const
	{
		return Status;
	}

	const FString& FLongRunningOperationReceipt::GetErrorCode() const
	{
		return ErrorCode;
	}

	bool FLongRunningOperationReceipt::IsRetrySafe() const
	{
		return bRetrySafe;
	}

	bool FLongRunningOperationReceipt::IsValid() const
	{
		return bValid;
	}

	class FLongRunningOperationOwner::FState final
	{
	public:
		class FOperation final
		{
		public:
			FOperation(
				FLongRunningOperationRequest InRequest,
				TSharedRef<ILongRunningOperationPort, ESPMode::ThreadSafe> InPort,
				const FUeAgentMonotonicClock& Clock)
				: Request(MoveTemp(InRequest))
				, Port(MoveTemp(InPort))
				, Cancellation(MakeShared<
					FUeAgentCancellationSource,
					ESPMode::ThreadSafe>(Clock))
				, ActiveDeadline(Request.GetDeadline())
			{
			}

			mutable FCriticalSection Mutex;
			FLongRunningOperationRequest Request;
			TSharedRef<ILongRunningOperationPort, ESPMode::ThreadSafe> Port;
			TSharedRef<
				FUeAgentCancellationSource,
				ESPMode::ThreadSafe> Cancellation;
			FUeAgentOperationDeadline ActiveDeadline;
			FLongRunningWorkIdentity WorkIdentity;
			ELongRunningOperationPhase Phase =
				ELongRunningOperationPhase::Accepted;
			ELongRunningOperationStatus Status =
				ELongRunningOperationStatus::None;
			FString ErrorCode;
			FString StopReason;
			TOptional<FLongRunningOperationSignal> LatestSignal;
			ELongRunningOperationStatus PendingTerminalStatus =
				ELongRunningOperationStatus::None;
			FString PendingTerminalErrorCode;
			bool bStopRequested = false;
			bool bStopDelivered = false;
			bool bJoined = false;
			bool bReleased = false;
			bool bAdvanceInProgress = false;
			bool bPortCallInFlight = false;
			bool bRetrySafe = false;
		};

		FState(
			TSharedRef<ILongRunningOperationPort, ESPMode::ThreadSafe> InPort,
			FUeAgentMonotonicClock InClock,
			const int32 InMaxActiveOperations,
			const int32 InMaxRetainedOperations)
			: DefaultPort(MoveTemp(InPort))
			, Clock(MoveTemp(InClock))
			, MaxActiveOperations(InMaxActiveOperations)
			, MaxRetainedOperations(InMaxRetainedOperations)
		{
		}

		TSharedPtr<FOperation, ESPMode::ThreadSafe> Find(
			const FString& OperationId) const
		{
			FScopeLock Lock(&Mutex);
			const TSharedRef<FOperation, ESPMode::ThreadSafe>* Found =
				Operations.Find(OperationId);
			return Found
				? TSharedPtr<FOperation, ESPMode::ThreadSafe>(*Found)
				: nullptr;
		}

		static FLongRunningOperationSnapshot MakeSnapshot(
			const FOperation& Operation)
		{
			return FLongRunningOperationSnapshot(
				Operation.Request.GetOperationId(),
				Operation.Request.GetOwnerLeaseId(),
				Operation.Request.GetServerSessionId(),
				Operation.Request.GetDescriptorIdentity(),
				Operation.Phase,
				Operation.WorkIdentity,
				Operation.Status,
				Operation.ErrorCode,
				Operation.bStopRequested,
				Operation.LatestSignal);
		}

		static bool ValidateAccess(
			const FOperation& Operation,
			const FString& OwnerLeaseId,
			const FString& ServerSessionId,
			FExecutionMutationError& OutError)
		{
			if (Operation.Request.GetOwnerLeaseId()
				!= NormalizeText(OwnerLeaseId))
			{
				SetError(
					OutError,
					TEXT("operation_owner_lease_mismatch"));
				return false;
			}
			if (Operation.Request.GetServerSessionId()
				!= NormalizeText(ServerSessionId))
			{
				SetError(
					OutError,
					TEXT("operation_server_session_mismatch"));
				return false;
			}
			return true;
		}

		static void SetTerminal(
			FOperation& Operation,
			const ELongRunningOperationStatus Status,
			FString ErrorCode,
			const bool bRetrySafe)
		{
			Operation.Phase = ELongRunningOperationPhase::Terminal;
			Operation.Status = Status;
			Operation.ErrorCode = NormalizeText(MoveTemp(ErrorCode));
			Operation.bRetrySafe = bRetrySafe;
		}

		static void SetPendingStateUnknown(
			FOperation& Operation,
			FString ErrorCode,
			const bool bRequestStop)
		{
			const FString NormalizedError =
				NormalizeText(MoveTemp(ErrorCode));
			if (Operation.PendingTerminalStatus
				== ELongRunningOperationStatus::None)
			{
				Operation.PendingTerminalStatus =
					ELongRunningOperationStatus::StateUnknown;
				Operation.PendingTerminalErrorCode = NormalizedError;
			}
			if (!bRequestStop)
			{
				return;
			}
			if (!Operation.bStopRequested)
			{
				Operation.Cancellation->Request(NormalizedError);
				Operation.bStopRequested = true;
				Operation.StopReason = NormalizedError;
			}
			Operation.Phase = ELongRunningOperationPhase::StopRequested;
		}

		TSharedRef<ILongRunningOperationPort, ESPMode::ThreadSafe> DefaultPort;
		FUeAgentMonotonicClock Clock;
		int32 MaxActiveOperations = 0;
		int32 MaxRetainedOperations = 0;
		mutable FCriticalSection Mutex;
		TMap<
			FString,
			TSharedRef<FOperation, ESPMode::ThreadSafe>> Operations;
	};

	FLongRunningOperationOwner::FLongRunningOperationOwner(
		TSharedRef<ILongRunningOperationPort, ESPMode::ThreadSafe> InPort,
		FUeAgentMonotonicClock InClock,
		const int32 InMaxActiveOperations,
		const int32 InMaxRetainedOperations)
		: State(MakeShared<FState, ESPMode::ThreadSafe>(
			MoveTemp(InPort),
			MoveTemp(InClock),
			InMaxActiveOperations,
			InMaxRetainedOperations))
	{
		checkf(State->Clock, TEXT("Long-running clock must be provided."));
		checkf(
			State->MaxActiveOperations > 0,
			TEXT("Long-running active-operation limit must be positive."));
		checkf(
			State->MaxRetainedOperations
				>= State->MaxActiveOperations,
			TEXT("Long-running retained-operation limit must cover active operations."));
	}

	FLongRunningOperationOwner::~FLongRunningOperationOwner() = default;

	bool FLongRunningOperationOwner::Accept(
		const FLongRunningOperationRequest& Request,
		FLongRunningOperationSnapshot& OutSnapshot,
		FExecutionMutationError& OutError)
	{
		return Accept(
			Request,
			State->DefaultPort,
			OutSnapshot,
			OutError);
	}

	bool FLongRunningOperationOwner::Accept(
		const FLongRunningOperationRequest& Request,
		TSharedRef<ILongRunningOperationPort, ESPMode::ThreadSafe> OperationPort,
		FLongRunningOperationSnapshot& OutSnapshot,
		FExecutionMutationError& OutError)
	{
		OutSnapshot = {};
		OutError = {};
		if (!Request.IsValid())
		{
			SetError(OutError, TEXT("long_running_request_invalid"));
			return false;
		}

		FScopeLock Lock(&State->Mutex);
		if (State->Operations.Contains(Request.GetOperationId()))
		{
			SetError(OutError, TEXT("operation_already_exists"));
			return false;
		}
		if (State->Operations.Num() >= State->MaxRetainedOperations)
		{
			SetError(OutError, TEXT("operation_registry_full"));
			return false;
		}

		int32 ActiveCount = 0;
		for (const TPair<
			FString,
			TSharedRef<FState::FOperation, ESPMode::ThreadSafe>>& Pair
			: State->Operations)
		{
			FScopeLock OperationLock(&Pair.Value->Mutex);
			if (Pair.Value->Phase
				!= ELongRunningOperationPhase::Terminal)
			{
				++ActiveCount;
			}
		}
		if (ActiveCount >= State->MaxActiveOperations)
		{
			SetError(OutError, TEXT("operation_queue_full"));
			return false;
		}

		const TSharedRef<FState::FOperation, ESPMode::ThreadSafe>
			Operation = MakeShared<
				FState::FOperation,
				ESPMode::ThreadSafe>(
					Request,
					MoveTemp(OperationPort),
					State->Clock);
		State->Operations.Add(Request.GetOperationId(), Operation);
		OutSnapshot = FState::MakeSnapshot(*Operation);
		return true;
	}

	bool FLongRunningOperationOwner::GetSnapshot(
		const FString& OperationId,
		const FString& OwnerLeaseId,
		const FString& ServerSessionId,
		FLongRunningOperationSnapshot& OutSnapshot,
		FExecutionMutationError& OutError) const
	{
		OutSnapshot = {};
		OutError = {};
		const TSharedPtr<FState::FOperation, ESPMode::ThreadSafe>
			Operation = State->Find(NormalizeText(OperationId));
		if (!Operation)
		{
			SetError(OutError, TEXT("operation_not_found"));
			return false;
		}
		FScopeLock Lock(&Operation->Mutex);
		if (!FState::ValidateAccess(
			*Operation,
			OwnerLeaseId,
			ServerSessionId,
			OutError))
		{
			return false;
		}
		OutSnapshot = FState::MakeSnapshot(*Operation);
		return true;
	}

	bool FLongRunningOperationOwner::Advance(
		const FString& OperationId,
		const FString& OwnerLeaseId,
		const FString& ServerSessionId,
		const int64 ExpectedGeneration,
		FLongRunningOperationSnapshot& OutSnapshot,
		FExecutionMutationError& OutError)
	{
		return AdvanceInternal(
			OperationId,
			OwnerLeaseId,
			ServerSessionId,
			ExpectedGeneration,
			nullptr,
			OutSnapshot,
			OutError);
	}

	bool FLongRunningOperationOwner::Advance(
		const FString& OperationId,
		const FString& OwnerLeaseId,
		const FString& ServerSessionId,
		const int64 ExpectedGeneration,
		const FUeAgentOperationDeadline& CallDeadline,
		FLongRunningOperationSnapshot& OutSnapshot,
		FExecutionMutationError& OutError)
	{
		return AdvanceInternal(
			OperationId,
			OwnerLeaseId,
			ServerSessionId,
			ExpectedGeneration,
			&CallDeadline,
			OutSnapshot,
			OutError);
	}

	bool FLongRunningOperationOwner::AdvanceInternal(
		const FString& OperationId,
		const FString& OwnerLeaseId,
		const FString& ServerSessionId,
		const int64 ExpectedGeneration,
		const FUeAgentOperationDeadline* CallDeadline,
		FLongRunningOperationSnapshot& OutSnapshot,
		FExecutionMutationError& OutError)
	{
		OutSnapshot = {};
		OutError = {};
		const TSharedPtr<FState::FOperation, ESPMode::ThreadSafe>
			Operation = State->Find(NormalizeText(OperationId));
		if (!Operation)
		{
			SetError(OutError, TEXT("operation_not_found"));
			return false;
		}

		{
			FScopeLock Lock(&Operation->Mutex);
			if (!FState::ValidateAccess(
				*Operation,
				OwnerLeaseId,
				ServerSessionId,
				OutError))
			{
				return false;
			}
			if (Operation->Phase == ELongRunningOperationPhase::Terminal)
			{
				SetError(OutError, TEXT("operation_terminal"));
				return false;
			}
			const int64 CurrentGeneration =
				Operation->WorkIdentity.IsValid()
					? Operation->WorkIdentity.GetGeneration()
					: 0;
			if (ExpectedGeneration != CurrentGeneration)
			{
				SetError(
					OutError,
					TEXT("operation_generation_mismatch"));
				return false;
			}
			if (Operation->bAdvanceInProgress
				|| Operation->bPortCallInFlight)
			{
				SetError(OutError, TEXT("operation_advance_in_flight"));
				return false;
			}
			Operation->ActiveDeadline = CallDeadline
				? *CallDeadline
				: Operation->Request.GetDeadline();
			Operation->bAdvanceInProgress = true;
		}

		ON_SCOPE_EXIT
		{
			FScopeLock Lock(&Operation->Mutex);
			Operation->bAdvanceInProgress = false;
			Operation->bPortCallInFlight = false;
		};

		enum class EPortAction : uint8
		{
			Start,
			Stop,
			Observe,
			Join,
			Release,
		};

		for (;;)
		{
			EPortAction Action = EPortAction::Observe;
			FLongRunningWorkIdentity WorkIdentity;
			FString StopReason;
			FUeAgentCancellationToken Cancellation =
				Operation->Cancellation->GetToken();

			{
				FScopeLock Lock(&Operation->Mutex);
				if (Operation->Phase
					== ELongRunningOperationPhase::Accepted)
				{
					if (!Operation->ActiveDeadline.CanContinue())
					{
						Operation->Cancellation->Request(
							TEXT("deadline_exceeded"));
						Operation->bStopRequested = true;
						Operation->StopReason =
							TEXT("deadline_exceeded");
						FState::SetTerminal(
							*Operation,
							ELongRunningOperationStatus::Cancelled,
							TEXT("deadline_exceeded"),
							true);
						OutSnapshot = FState::MakeSnapshot(*Operation);
						return true;
					}
					Action = EPortAction::Start;
				}
				else if (Operation->Phase
					== ELongRunningOperationPhase::Joining)
				{
					Action = Operation->bJoined
						? EPortAction::Release
						: EPortAction::Join;
				}
				else
				{
					if (!Operation->bStopRequested
						&& !Operation->ActiveDeadline
							.CanContinue())
					{
						Operation->Cancellation->Request(
							TEXT("deadline_exceeded"));
						Operation->bStopRequested = true;
						Operation->StopReason =
							TEXT("deadline_exceeded");
						Operation->Phase =
							ELongRunningOperationPhase::StopRequested;
					}
					Action = Operation->bStopRequested
						&& !Operation->bStopDelivered
						? EPortAction::Stop
						: EPortAction::Observe;
				}

				Operation->bPortCallInFlight = true;
				WorkIdentity = Operation->WorkIdentity;
				StopReason = Operation->StopReason;
				Cancellation = Operation->Cancellation->GetToken();
			}

			FExecutionMutationError PortError;
			bool bPortSucceeded = false;
			FLongRunningWorkIdentity StartedIdentity;
			FLongRunningWorkObservation Observation;
			switch (Action)
			{
			case EPortAction::Start:
				bPortSucceeded = Operation->Port->Start(
					Operation->Request,
					Cancellation,
					StartedIdentity,
					PortError);
				break;
			case EPortAction::Stop:
				bPortSucceeded = Operation->Port->RequestStop(
					Operation->Request.GetOperationId(),
					WorkIdentity,
					StopReason,
					PortError);
				break;
			case EPortAction::Observe:
				bPortSucceeded = Operation->Port->Observe(
					Operation->Request.GetOperationId(),
					WorkIdentity,
					Operation->Request.GetResources(),
					Observation,
					PortError);
				break;
			case EPortAction::Join:
				bPortSucceeded = Operation->Port->Join(
					Operation->Request.GetOperationId(),
					WorkIdentity,
					PortError);
				break;
			case EPortAction::Release:
				bPortSucceeded = Operation->Port->Release(
					Operation->Request.GetOperationId(),
					WorkIdentity,
					PortError);
				break;
			default:
				checkNoEntry();
				break;
			}

			{
				FScopeLock Lock(&Operation->Mutex);
				Operation->bPortCallInFlight = false;
				if (!bPortSucceeded)
				{
					if (Action == EPortAction::Start)
					{
						FState::SetTerminal(
							*Operation,
							ELongRunningOperationStatus::StateUnknown,
							ErrorCodeOr(
								PortError,
								TEXT("native_start_state_unknown")),
							false);
						OutSnapshot = FState::MakeSnapshot(*Operation);
						return true;
					}
					OutSnapshot = FState::MakeSnapshot(*Operation);
					OutError = PortError.IsPresent()
						? PortError
						: FExecutionMutationError(
							TEXT("native_operation_failed"));
					return false;
				}

				switch (Action)
				{
				case EPortAction::Start:
					if (!StartedIdentity.IsValid())
					{
						FState::SetTerminal(
							*Operation,
							ELongRunningOperationStatus::StateUnknown,
							TEXT("native_identity_invalid"),
							false);
						OutSnapshot = FState::MakeSnapshot(*Operation);
						return true;
					}
					Operation->WorkIdentity = StartedIdentity;
					Operation->Phase =
						ELongRunningOperationPhase::Running;
					if (Operation->Cancellation->GetToken()
						.IsCancelled()
						|| !Operation->ActiveDeadline
							.CanContinue())
					{
						if (!Operation->Cancellation->GetToken()
							.IsCancelled())
						{
							Operation->Cancellation->Request(
								TEXT("deadline_exceeded"));
						}
						const FUeAgentCancellationRequest* Request =
							Operation->Cancellation->GetToken()
								.GetRequest();
						Operation->StopReason = Request
							? Request->Reason
							: FString(TEXT("cancellation_requested"));
						Operation->bStopRequested = true;
						Operation->Phase =
							ELongRunningOperationPhase::StopRequested;
						continue;
					}
					OutSnapshot = FState::MakeSnapshot(*Operation);
					return true;
				case EPortAction::Stop:
					Operation->bStopDelivered = true;
					Operation->Phase =
						ELongRunningOperationPhase::StopRequested;
					OutSnapshot = FState::MakeSnapshot(*Operation);
					return true;
				case EPortAction::Observe:
				{
					if (!IsLifecycleObservationValid(Observation)
						|| Observation.GetGeneration()
							!= Operation->WorkIdentity.GetGeneration())
					{
						FState::SetPendingStateUnknown(
							*Operation,
							TEXT("native_observation_invalid"),
							true);
						if (!Operation->bStopDelivered)
						{
							continue;
						}
						OutSnapshot = FState::MakeSnapshot(*Operation);
						return true;
					}
					if (Observation.GetState()
						== ELongRunningWorkState::StateUnknown)
					{
						FState::SetTerminal(
							*Operation,
							ELongRunningOperationStatus::StateUnknown,
							Observation.GetErrorCode(),
							false);
						OutSnapshot = FState::MakeSnapshot(*Operation);
						return true;
					}

					FString SignalFailureCode;
					if (Operation->PendingTerminalStatus
							== ELongRunningOperationStatus::None
						&& Observation.GetSignal().IsSet())
					{
						const FLongRunningOperationSignal& Signal =
							Observation.GetSignal().GetValue();
						if (!Signal.IsValid())
						{
							SignalFailureCode =
								TEXT("native_operation_signal_invalid");
						}
						else if (Operation->LatestSignal.IsSet()
							&& Signal.GetRevision()
								<= Operation->LatestSignal.GetValue()
									.GetRevision())
						{
							SignalFailureCode = TEXT(
								"native_operation_signal_revision_invalid");
						}
						else
						{
							Operation->LatestSignal = Signal;
						}
					}

					if (!SignalFailureCode.IsEmpty())
					{
						if (Observation.GetState()
							== ELongRunningWorkState::Running)
						{
							FState::SetPendingStateUnknown(
								*Operation,
								SignalFailureCode,
								true);
							if (!Operation->bStopDelivered)
							{
								continue;
							}
							OutSnapshot = FState::MakeSnapshot(*Operation);
							return true;
						}
						FState::SetPendingStateUnknown(
							*Operation,
							SignalFailureCode,
							false);
					}
					if (Observation.GetState()
						== ELongRunningWorkState::Running)
					{
						OutSnapshot = FState::MakeSnapshot(*Operation);
						return true;
					}
					if (Operation->PendingTerminalStatus
						!= ELongRunningOperationStatus::None)
					{
						Operation->Status =
							Operation->PendingTerminalStatus;
						Operation->ErrorCode =
							Operation->PendingTerminalErrorCode;
					}
					else
					{
						Operation->Status = Operation->bStopRequested
							? ELongRunningOperationStatus::Cancelled
							: (Observation.GetState()
								== ELongRunningWorkState::ExitedFailure
									? ELongRunningOperationStatus::Failed
									: ELongRunningOperationStatus::Succeeded);
						Operation->ErrorCode = Operation->bStopRequested
							? Operation->StopReason
							: (Observation.GetState()
								== ELongRunningWorkState::ExitedFailure
									? Observation.GetErrorCode()
									: FString());
					}
					Operation->Phase =
						ELongRunningOperationPhase::Joining;
					continue;
				}
				case EPortAction::Join:
					Operation->bJoined = true;
					continue;
				case EPortAction::Release:
					Operation->bReleased = true;
					Operation->Phase =
						ELongRunningOperationPhase::Terminal;
					Operation->bRetrySafe = false;
					OutSnapshot = FState::MakeSnapshot(*Operation);
					return true;
				default:
					checkNoEntry();
					return false;
				}
			}
		}
	}

	bool FLongRunningOperationOwner::RequestCancellation(
		const FString& OperationId,
		const FString& OwnerLeaseId,
		const FString& ServerSessionId,
		const FString& Reason,
		FLongRunningOperationSnapshot& OutSnapshot,
		FExecutionMutationError& OutError)
	{
		OutSnapshot = {};
		OutError = {};
		const FString NormalizedReason = NormalizeText(Reason);
		if (NormalizedReason.IsEmpty())
		{
			SetError(OutError, TEXT("cancellation_reason_missing"));
			return false;
		}
		const TSharedPtr<FState::FOperation, ESPMode::ThreadSafe>
			Operation = State->Find(NormalizeText(OperationId));
		if (!Operation)
		{
			SetError(OutError, TEXT("operation_not_found"));
			return false;
		}

		FLongRunningWorkIdentity WorkIdentity;
		FString StopReason;
		bool bDeliverStop = false;
		{
			FScopeLock Lock(&Operation->Mutex);
			if (!FState::ValidateAccess(
				*Operation,
				OwnerLeaseId,
				ServerSessionId,
				OutError))
			{
				return false;
			}
			if (Operation->Phase == ELongRunningOperationPhase::Terminal)
			{
				SetError(OutError, TEXT("operation_terminal"));
				return false;
			}
			Operation->Cancellation->Request(NormalizedReason);
			const FUeAgentCancellationRequest* Request =
				Operation->Cancellation->GetToken().GetRequest();
			Operation->StopReason = Request
				? Request->Reason
				: NormalizedReason;
			Operation->bStopRequested = true;
			if (Operation->Phase
				== ELongRunningOperationPhase::Accepted
				&& !Operation->bAdvanceInProgress
				&& !Operation->bPortCallInFlight)
			{
				FState::SetTerminal(
					*Operation,
					ELongRunningOperationStatus::Cancelled,
					Operation->StopReason,
					true);
				OutSnapshot = FState::MakeSnapshot(*Operation);
				return true;
			}
			if (Operation->Phase
				!= ELongRunningOperationPhase::Joining)
			{
				Operation->Phase =
					ELongRunningOperationPhase::StopRequested;
			}
			bDeliverStop = Operation->WorkIdentity.IsValid()
				&& !Operation->bStopDelivered
				&& !Operation->bAdvanceInProgress
				&& !Operation->bPortCallInFlight;
			if (bDeliverStop)
			{
				Operation->bPortCallInFlight = true;
				WorkIdentity = Operation->WorkIdentity;
				StopReason = Operation->StopReason;
			}
			else
			{
				OutSnapshot = FState::MakeSnapshot(*Operation);
				return true;
			}
		}

		FExecutionMutationError PortError;
		const bool bStopped = Operation->Port->RequestStop(
			Operation->Request.GetOperationId(),
			WorkIdentity,
			StopReason,
			PortError);
		{
			FScopeLock Lock(&Operation->Mutex);
			Operation->bPortCallInFlight = false;
			if (bStopped)
			{
				Operation->bStopDelivered = true;
				OutSnapshot = FState::MakeSnapshot(*Operation);
				return true;
			}
			OutSnapshot = FState::MakeSnapshot(*Operation);
			OutError = PortError.IsPresent()
				? PortError
				: FExecutionMutationError(
					TEXT("native_stop_request_failed"));
			return false;
		}
	}

	bool FLongRunningOperationOwner::GetTerminalReceipt(
		const FString& OperationId,
		const FString& OwnerLeaseId,
		const FString& ServerSessionId,
		FLongRunningOperationReceipt& OutReceipt,
		FExecutionMutationError& OutError) const
	{
		OutReceipt = {};
		OutError = {};
		const TSharedPtr<FState::FOperation, ESPMode::ThreadSafe>
			Operation = State->Find(NormalizeText(OperationId));
		if (!Operation)
		{
			SetError(OutError, TEXT("operation_not_found"));
			return false;
		}
		FScopeLock Lock(&Operation->Mutex);
		if (!FState::ValidateAccess(
			*Operation,
			OwnerLeaseId,
			ServerSessionId,
			OutError))
		{
			return false;
		}
		if (Operation->Phase != ELongRunningOperationPhase::Terminal)
		{
			SetError(OutError, TEXT("operation_not_terminal"));
			return false;
		}
		OutReceipt = FLongRunningOperationReceipt(
			Operation->Request.GetOperationId(),
			Operation->WorkIdentity,
			Operation->Status,
			Operation->ErrorCode,
			Operation->bRetrySafe);
		return true;
	}

	bool FLongRunningOperationOwner::ReleaseTerminal(
		const FString& OperationId,
		const FString& OwnerLeaseId,
		const FString& ServerSessionId,
		FExecutionMutationError& OutError)
	{
		OutError = {};
		const FString NormalizedOperationId = NormalizeText(OperationId);
		FScopeLock StateLock(&State->Mutex);
		const TSharedRef<FState::FOperation, ESPMode::ThreadSafe>* Found =
			State->Operations.Find(NormalizedOperationId);
		if (!Found)
		{
			SetError(OutError, TEXT("operation_not_found"));
			return false;
		}

		const TSharedRef<FState::FOperation, ESPMode::ThreadSafe>
			Operation = *Found;
		FScopeLock OperationLock(&Operation->Mutex);
		if (!FState::ValidateAccess(
			*Operation,
			OwnerLeaseId,
			ServerSessionId,
			OutError))
		{
			return false;
		}
		if (Operation->Phase != ELongRunningOperationPhase::Terminal)
		{
			SetError(OutError, TEXT("operation_not_terminal"));
			return false;
		}
		if (Operation->bAdvanceInProgress
			|| Operation->bPortCallInFlight)
		{
			SetError(OutError, TEXT("operation_advance_in_flight"));
			return false;
		}
		State->Operations.Remove(NormalizedOperationId);
		return true;
	}
}
