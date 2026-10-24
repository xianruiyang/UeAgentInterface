#include "Core/ExecutionMutation/UeAgentLongRunningOperation.h"

#include "Dom/JsonValue.h"
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
			const TCHAR* Reason,
			const TCHAR* Property)
		{
			OutError = FExecutionMutationError::Failure(Code, Reason, TEXT("execution"));
			OutError.PropertyPath = Property;
		}

		static FExecutionMutationError DiagnosticOr(const FExecutionMutationError& Error, const TCHAR* Fallback, const TCHAR* Reason)
		{
			return Error.IsEmpty() ? FExecutionMutationError::Failure(Fallback, Reason, TEXT("long_running")) : Error;
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
		FExecutionMutationError InDiagnostic,
		TOptional<FLongRunningOperationSignal> InSignal)
		: Generation(InGeneration)
		, State(InState)
		, Diagnostic(MoveTemp(InDiagnostic))
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
		return Diagnostic.Code;
	}

	const FExecutionMutationError& FLongRunningWorkObservation::GetDiagnostic() const
	{
		return Diagnostic;
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
		FUeAgentOperationDeadline InDeadline,
		FLongRunningOperationOrigin InOrigin)
		: OperationId(NormalizeText(MoveTemp(InOperationId)))
		, OwnerLeaseId(NormalizeText(MoveTemp(InOwnerLeaseId)))
		, ServerSessionId(NormalizeText(MoveTemp(InServerSessionId)))
		, DescriptorIdentity(MoveTemp(InDescriptorIdentity))
		, ExecutionContext(MoveTemp(InExecutionContext))
		, Resources(MoveTemp(InResources))
		, Deadline(MoveTemp(InDeadline))
		, Origin(MoveTemp(InOrigin))
	{
		FExecutionMutationError Error;
		if (OperationId.IsEmpty())
			SetError(Error, TEXT("long_running_request_invalid"),
				TEXT("The operation request is missing its operation identity."), TEXT("operation_id"));
		else if (OwnerLeaseId.IsEmpty())
			SetError(Error, TEXT("long_running_request_invalid"),
				TEXT("The operation request is missing its owner lease."), TEXT("owner_lease_id"));
		else if (ServerSessionId.IsEmpty())
			SetError(Error, TEXT("long_running_request_invalid"),
				TEXT("The operation request is missing its server session identity."), TEXT("server_session_id"));
		else if (!DescriptorIdentity.IsValid())
		{
			SetError(Error, TEXT("long_running_request_invalid"),
				TEXT("The operation request contains an invalid descriptor."), TEXT("descriptor"));
			Error.PublicCause = MakeShared<FExecutionMutationError>(DescriptorIdentity.GetValidationDiagnostic());
		}
		else if (ExecutionContext->GetRequestKind() != ContextResource::EUeAgentRequestKind::Operation)
			SetError(Error, TEXT("long_running_request_invalid"),
				TEXT("The execution context is not an operation context."), TEXT("execution_context"));
		else if (!(ExecutionContext->GetDescriptorIdentity() == DescriptorIdentity))
			SetError(Error, TEXT("long_running_request_invalid"),
				TEXT("The execution context belongs to a different operation descriptor."), TEXT("execution_context"));
		if (!Error.IsEmpty())
		{
			Error.SourceKind = TEXT("native_result");
			ValidationDiagnostic = MakeShared<FExecutionMutationError>(MoveTemp(Error));
		}
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
		return !ValidationDiagnostic.IsValid();
	}

	const FExecutionMutationError& FLongRunningOperationRequest::GetValidationDiagnostic() const
	{
		static const FExecutionMutationError Empty;
		return ValidationDiagnostic.IsValid() ? *ValidationDiagnostic : Empty;
	}

	FLongRunningOperationSnapshot::FLongRunningOperationSnapshot(
		FString InOperationId,
		FString InOwnerLeaseId,
		FString InServerSessionId,
		FUeAgentDescriptorIdentity InDescriptorIdentity,
		const ELongRunningOperationPhase InPhase,
		FLongRunningWorkIdentity InWorkIdentity,
		const ELongRunningOperationStatus InStatus,
		FExecutionMutationError InDiagnostic,
		const bool bInStopRequested,
		TOptional<FLongRunningOperationSignal> InLatestSignal,
		const bool bInCleanupComplete,
		FExecutionMutationError InProgressDiagnostic)
		: OperationId(NormalizeText(MoveTemp(InOperationId)))
		, OwnerLeaseId(NormalizeText(MoveTemp(InOwnerLeaseId)))
		, ServerSessionId(NormalizeText(MoveTemp(InServerSessionId)))
		, DescriptorIdentity(MoveTemp(InDescriptorIdentity))
		, Phase(InPhase)
		, WorkIdentity(MoveTemp(InWorkIdentity))
		, Status(InStatus)
		, Diagnostic(MoveTemp(InDiagnostic))
		, bStopRequested(bInStopRequested)
		, LatestSignal(MoveTemp(InLatestSignal))
		, bCleanupComplete(bInCleanupComplete)
		, ProgressDiagnostic(MoveTemp(InProgressDiagnostic))
	{
		if (!Diagnostic.IsEmpty())
		{
			Diagnostic.Schema = TEXT("uai.operation.failure/1");
			Diagnostic.TerminalStatus = LexToString(Status);
			Diagnostic.bRetrySafe = false;
		}
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
		return Diagnostic.Code;
	}

	const FExecutionMutationError& FLongRunningOperationSnapshot::GetDiagnostic() const
	{
		return Diagnostic;
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
		FExecutionMutationError InDiagnostic,
		const bool bInRetrySafe)
		: OperationId(NormalizeText(MoveTemp(InOperationId)))
		, WorkIdentity(MoveTemp(InWorkIdentity))
		, Status(InStatus)
		, Diagnostic(MoveTemp(InDiagnostic))
		, bRetrySafe(bInRetrySafe)
	{
		if (!Diagnostic.IsEmpty())
		{
			Diagnostic.Schema = TEXT("uai.operation.failure/1");
			Diagnostic.TerminalStatus = LexToString(Status);
			Diagnostic.bRetrySafe = bRetrySafe;
		}
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
		return Diagnostic.Code;
	}

	const FExecutionMutationError& FLongRunningOperationReceipt::GetDiagnostic() const
	{
		return Diagnostic;
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
				, CustodyLeaseId(Request.GetOwnerLeaseId())
				, Cancellation(MakeShared<
					FUeAgentCancellationSource,
					ESPMode::ThreadSafe>(Clock))
				, ActiveDeadline(Request.GetDeadline())
			{
			}

			mutable FCriticalSection Mutex;
			FLongRunningOperationRequest Request;
			TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe> Port;
			FString CustodyLeaseId;
			FString CustodyRequestId;
			TSharedRef<
				FUeAgentCancellationSource,
				ESPMode::ThreadSafe> Cancellation;
			FUeAgentOperationDeadline ActiveDeadline;
			FLongRunningWorkIdentity WorkIdentity;
			ELongRunningOperationPhase Phase =
				ELongRunningOperationPhase::Accepted;
			ELongRunningOperationStatus Status =
				ELongRunningOperationStatus::None;
			FExecutionMutationError Diagnostic;
			FExecutionMutationError ProgressDiagnostic;
			FString StopReason;
			TOptional<FLongRunningOperationSignal> LatestSignal;
			ELongRunningOperationStatus PendingTerminalStatus =
				ELongRunningOperationStatus::None;
			FExecutionMutationError PendingTerminalDiagnostic;
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
			FUeAgentMonotonicClock InClock)
			: DefaultPort(MoveTemp(InPort))
			, Clock(MoveTemp(InClock))
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

		TArray<TSharedRef<FOperation, ESPMode::ThreadSafe>> CopyOperations() const
		{
			FScopeLock Lock(&Mutex);
			TArray<TSharedRef<FOperation, ESPMode::ThreadSafe>> Result;
			Operations.GenerateValueArray(Result);
			return Result;
		}

		static FLongRunningOperationSnapshot MakeSnapshot(
			const FOperation& Operation)
		{
			return FLongRunningOperationSnapshot(
				Operation.Request.GetOperationId(),
				Operation.CustodyLeaseId,
				Operation.Request.GetServerSessionId(),
				Operation.Request.GetDescriptorIdentity(),
				Operation.Phase,
				Operation.WorkIdentity,
				Operation.Status,
				Operation.Diagnostic,
				Operation.bStopRequested,
				Operation.LatestSignal,
				Operation.bReleased,
				Operation.ProgressDiagnostic);
		}

		static bool ValidateAccess(
			const FOperation& Operation,
			const FString& OwnerLeaseId,
			const FString& ServerSessionId,
			FExecutionMutationError& OutError)
		{
			if (Operation.CustodyLeaseId
				!= NormalizeText(OwnerLeaseId))
			{
				SetError(
					OutError,
					TEXT("operation_owner_lease_mismatch"), TEXT("The operation reference belongs to a different owner lease."), TEXT("operation_ref.owner_lease_id"));
				return false;
			}
			if (Operation.Request.GetServerSessionId()
				!= NormalizeText(ServerSessionId))
			{
				SetError(
					OutError,
					TEXT("operation_server_session_mismatch"), TEXT("The operation reference belongs to a different server session."), TEXT("operation_ref.server_session_id"));
				return false;
			}
			return true;
		}

		static void SetTerminal(FOperation& Operation, const ELongRunningOperationStatus Status,
			FExecutionMutationError Diagnostic, const bool bRetrySafe)
		{
			Operation.Phase = ELongRunningOperationPhase::Terminal;
			Operation.Status = Status;
			Operation.Diagnostic = MoveTemp(Diagnostic);
			Operation.bRetrySafe = bRetrySafe;
		}

		static void SetPendingStateUnknown(FOperation& Operation, FExecutionMutationError Diagnostic, const bool bRequestStop)
		{
			if (Operation.PendingTerminalStatus == ELongRunningOperationStatus::None)
			{
				Operation.PendingTerminalStatus = ELongRunningOperationStatus::StateUnknown;
				Operation.PendingTerminalDiagnostic = Diagnostic;
			}
			if (!bRequestStop) return;
			if (!Operation.bStopRequested)
			{
				Operation.Cancellation->Request(Diagnostic.Code);
				Operation.bStopRequested = true;
				Operation.StopReason = Diagnostic.Code;
			}
			Operation.Phase = ELongRunningOperationPhase::StopRequested;
		}

		TSharedRef<ILongRunningOperationPort, ESPMode::ThreadSafe> DefaultPort;
		FUeAgentMonotonicClock Clock;
		mutable FCriticalSection Mutex;
		TMap<
			FString,
			TSharedRef<FOperation, ESPMode::ThreadSafe>> Operations;
	};

	FLongRunningOperationOwner::FLongRunningOperationOwner(
		TSharedRef<ILongRunningOperationPort, ESPMode::ThreadSafe> InPort,
		FUeAgentMonotonicClock InClock)
		: State(MakeShared<FState, ESPMode::ThreadSafe>(
			MoveTemp(InPort),
			MoveTemp(InClock)))
	{
		checkf(State->Clock, TEXT("Long-running clock must be provided."));
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
			OutError = Request.GetValidationDiagnostic();
			return false;
		}

		FScopeLock Lock(&State->Mutex);
		if (State->Operations.Contains(Request.GetOperationId()))
		{
			SetError(OutError, TEXT("operation_already_exists"), TEXT("An operation with this identity is already registered."), TEXT("operation_ref"));
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
			SetError(OutError, TEXT("operation_not_found"), TEXT("The referenced operation is no longer registered in this session."), TEXT("operation_ref"));
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
			SetError(OutError, TEXT("operation_not_found"), TEXT("The referenced operation is no longer registered in this session."), TEXT("operation_ref"));
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
				OutSnapshot = FState::MakeSnapshot(*Operation);
				return true;
			}
			const int64 CurrentGeneration =
				Operation->WorkIdentity.IsValid()
					? Operation->WorkIdentity.GetGeneration()
					: 0;
			if (ExpectedGeneration != CurrentGeneration)
			{
				SetError(
					OutError,
					TEXT("operation_generation_mismatch"), TEXT("The expected operation generation is stale; refresh the operation before advancing it."), TEXT("expected_generation"));
				return false;
			}
			if (Operation->bAdvanceInProgress
				|| Operation->bPortCallInFlight)
			{
				// The shared driver may already be observing this work. Do not
				// execute a second native call or turn ordinary progress into failure.
				OutSnapshot = FState::MakeSnapshot(*Operation);
				return true;
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
			bool bFailedStartCleanupComplete = false;
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
				if (!bPortSucceeded)
					bFailedStartCleanupComplete = Operation->Port->IsFailedStartCleanupComplete();
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
						if (bFailedStartCleanupComplete)
						{
							Operation->bReleased = true;
							Operation->Port.Reset();
							FState::SetTerminal(*Operation, ELongRunningOperationStatus::Failed,
								DiagnosticOr(PortError, TEXT("native_start_rejected"),
									TEXT("The native operation rejected Start and confirmed that no resource requires cleanup.")), false);
							OutSnapshot = FState::MakeSnapshot(*Operation);
							return true;
						}
						FState::SetTerminal(
							*Operation,
							ELongRunningOperationStatus::StateUnknown,
							DiagnosticOr(
								PortError,
								TEXT("native_start_state_unknown"), TEXT("The native operation failed to start without reporting a cause; whether it started is unknown.")),
							false);
						OutSnapshot = FState::MakeSnapshot(*Operation);
						return true;
					}
					OutError = (!PortError.IsEmpty())
						? PortError
						: FExecutionMutationError::Failure(TEXT("native_operation_failed"),
							FString::Printf(TEXT("The native operation %s step failed without reporting a cause."),
								Action == EPortAction::Stop ? TEXT("stop") : Action == EPortAction::Join ? TEXT("join")
								: Action == EPortAction::Release ? TEXT("release") : TEXT("observe")), TEXT("execution"));
					Operation->ProgressDiagnostic = OutError;
					if (Action == EPortAction::Join
						&& Operation->PendingTerminalStatus == ELongRunningOperationStatus::StateUnknown)
						FState::SetPendingStateUnknown(*Operation, Operation->PendingTerminalDiagnostic, true);
					OutSnapshot = FState::MakeSnapshot(*Operation);
					return false;
				}
				Operation->ProgressDiagnostic.Reset();

				switch (Action)
				{
				case EPortAction::Start:
					if (!StartedIdentity.IsValid())
					{
						FState::SetTerminal(*Operation,
				ELongRunningOperationStatus::StateUnknown,
				FExecutionMutationError::Failure(TEXT("native_identity_invalid"),
					TEXT("The native start returned an invalid work identity, so its execution state cannot be confirmed."), TEXT("long_running")),
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
						const bool bGenerationMismatch = Observation.GetGeneration() != Operation->WorkIdentity.GetGeneration();
						FExecutionMutationError Invalid = FExecutionMutationError::Failure(TEXT("native_observation_invalid"),
							bGenerationMismatch ? TEXT("The native observation belongs to a different work generation.")
							: Observation.GetState() == ELongRunningWorkState::Invalid ? TEXT("The native observation has no valid lifecycle state.")
							: Observation.GetGeneration() <= 0 ? TEXT("The native observation has no valid work generation.")
							: TEXT("The native observation's failure diagnostic does not agree with its lifecycle state."), TEXT("long_running"));
						Invalid.PropertyPath = bGenerationMismatch || Observation.GetGeneration() <= 0 ? TEXT("generation") : TEXT("state");
						if (!Observation.GetDiagnostic().IsEmpty())
						{
							Invalid.PublicCause = MakeShared<FExecutionMutationError>(Observation.GetDiagnostic());
							Invalid.CauseCode = Observation.GetDiagnostic().Code;
						}
						FState::SetPendingStateUnknown(*Operation, MoveTemp(Invalid), true);
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
						FState::SetPendingStateUnknown(*Operation, Observation.GetDiagnostic(), false);
						// The native port must still prove Join/Release, even
						// when the business outcome (e.g. exit code) is unknown.
					}

					UE::UAI::Contract::FDiagnostic SignalDiagnostic;
					if (Operation->PendingTerminalStatus
							== ELongRunningOperationStatus::None
						&& Observation.GetSignal().IsSet())
					{
						const FLongRunningOperationSignal& Signal =
							Observation.GetSignal().GetValue();
						if (!Signal.IsValid())
						{
							SignalDiagnostic = FExecutionMutationError::Failure(TEXT("native_operation_signal_invalid"),
								Signal.GetTypeId().IsEmpty() ? TEXT("The native operation signal has no type.")
								: TEXT("The native operation signal revision must be positive."), TEXT("long_running"));
							SignalDiagnostic.PropertyPath = Signal.GetTypeId().IsEmpty() ? TEXT("latest_signal.type_id") : TEXT("latest_signal.revision");
						}
						else if (Operation->LatestSignal.IsSet()
							&& Signal.GetRevision()
								<= Operation->LatestSignal.GetValue()
									.GetRevision())
						{
							SignalDiagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT(
								"native_operation_signal_revision_invalid"), TEXT("The received signal revision must be newer than the last accepted revision."), TEXT("long_running"));
							SignalDiagnostic.PropertyPath = TEXT("latest_signal.revision");
						}
						else
						{
							Operation->LatestSignal = Signal;
						}
					}

					if (!SignalDiagnostic.IsEmpty())
					{
						if (Observation.GetState()
							== ELongRunningWorkState::Running)
						{
							FState::SetPendingStateUnknown(
								*Operation,
								SignalDiagnostic,
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
							SignalDiagnostic,
							false);
					}
					if (Observation.GetState()
						== ELongRunningWorkState::Running)
					{
						if (!Operation->ActiveDeadline.CanContinue()) {
							Operation->Diagnostic = FExecutionMutationError::Failure(Operation->bStopRequested
								? TEXT("native_stop_pending_deadline_exceeded") : TEXT("deadline_exceeded"),
								Operation->bStopRequested ? TEXT("The native operation has not stopped before the deadline.") : TEXT("The operation deadline was exceeded."), TEXT("long_running"));
							if (!Operation->bStopRequested) {
								Operation->Cancellation->Request(TEXT("deadline_exceeded"));
								Operation->StopReason = TEXT("deadline_exceeded");
								Operation->bStopRequested = true;
								Operation->Phase = ELongRunningOperationPhase::StopRequested;
								continue;
							}
						}
						OutSnapshot = FState::MakeSnapshot(*Operation);
						return true;
					}
					if (Operation->PendingTerminalStatus
						!= ELongRunningOperationStatus::None)
					{
						Operation->Status =
							Operation->PendingTerminalStatus;
						Operation->Diagnostic =
							Operation->PendingTerminalDiagnostic;
					}
					else
					{
						Operation->Status = Operation->bStopRequested
							? ELongRunningOperationStatus::Cancelled
							: (Observation.GetState()
								== ELongRunningWorkState::ExitedFailure
									? ELongRunningOperationStatus::Failed
									: ELongRunningOperationStatus::Succeeded);
						Operation->Diagnostic = Operation->bStopRequested
							? FExecutionMutationError::Failure(Operation->StopReason, Operation->StopReason, TEXT("cancellation"))
							: (Observation.GetState()
								== ELongRunningWorkState::ExitedFailure
									? Observation.GetDiagnostic()
									: FExecutionMutationError());
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
					Operation->Port.Reset();
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
			SetError(OutError, TEXT("cancellation_reason_missing"), TEXT("Cancellation requires a non-empty reason."), TEXT("reason"));
			OutError.Source.JsonPath = TEXT("/reason");
			OutError.ActualValue = MakeShared<FJsonValueString>(Reason);
			return false;
		}
		const TSharedPtr<FState::FOperation, ESPMode::ThreadSafe>
			Operation = State->Find(NormalizeText(OperationId));
		if (!Operation)
		{
			SetError(OutError, TEXT("operation_not_found"), TEXT("The referenced operation is no longer registered in this session."), TEXT("operation_ref"));
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
				OutSnapshot = FState::MakeSnapshot(*Operation);
				return true;
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
				FState::SetTerminal(*Operation,
				ELongRunningOperationStatus::Cancelled,
				FExecutionMutationError::Failure(Operation->StopReason, Operation->StopReason, TEXT("cancellation")),
				true);
				Operation->bReleased = true; // Cancelled before Start acquired native resources.
				Operation->Port.Reset();
				OutSnapshot = FState::MakeSnapshot(*Operation);
				return true;
			}
			if (Operation->Phase
				!= ELongRunningOperationPhase::Joining)
			{
				// An explicit repeat cancel is a retry of unfinished native
				// cleanup, not a replay of Start. Autonomous polling alone does
				// not redeliver stop requests or reopen native dialogs.
				Operation->bStopDelivered = false;
				Operation->Phase =
					ELongRunningOperationPhase::StopRequested;
			}
			bDeliverStop = Operation->WorkIdentity.IsValid()
				&& Operation->Phase != ELongRunningOperationPhase::Joining
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
			OutError = (!PortError.IsEmpty())
				? PortError
				: FExecutionMutationError::Failure(TEXT("native_stop_request_failed"),
					TEXT("The native operation rejected the stop request without reporting a cause."), TEXT("execution"));
			Operation->ProgressDiagnostic = OutError;
			OutSnapshot = FState::MakeSnapshot(*Operation);
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
			SetError(OutError, TEXT("operation_not_found"), TEXT("The referenced operation is no longer registered in this session."), TEXT("operation_ref"));
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
			SetError(OutError, TEXT("operation_not_terminal"), TEXT("The operation must reach a terminal state before its receipt can be read or its resources released."), TEXT("phase"));
			return false;
		}
		OutReceipt = FLongRunningOperationReceipt(
			Operation->Request.GetOperationId(),
			Operation->WorkIdentity,
			Operation->Status,
			Operation->Diagnostic,
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
			SetError(OutError, TEXT("operation_not_found"), TEXT("The referenced operation is no longer registered in this session."), TEXT("operation_ref"));
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
			SetError(OutError, TEXT("operation_not_terminal"), TEXT("The operation must reach a terminal state before its receipt can be read or its resources released."), TEXT("phase"));
			return false;
		}
		if (Operation->bAdvanceInProgress
			|| Operation->bPortCallInFlight)
		{
			SetError(OutError, TEXT("operation_advance_in_flight"), TEXT("Another advance or native port call is still running for this operation."), TEXT("operation_ref"));
			return false;
		}
		if (!Operation->bReleased)
		{
			SetError(OutError, TEXT("operation_cleanup_unconfirmed"),
				TEXT("Native resource cleanup has not been confirmed; the recovery record is retained."),
				TEXT("cleanup_complete"));
			OutError.PublicCause = MakeShared<FExecutionMutationError>(Operation->Diagnostic);
			return false;
		}
		State->Operations.Remove(NormalizedOperationId);
		return true;
	}

	TArray<FLongRunningOperationEntry> FLongRunningOperationOwner::Discover(
		const FString& CallerId) const
	{
		TArray<FLongRunningOperationEntry> Result;
		if (CallerId.IsEmpty()) return Result;
		for (const auto& Operation : State->CopyOperations())
		{
			FScopeLock Lock(&Operation->Mutex);
			if (Operation->Request.GetOrigin().CallerId == CallerId)
			{
				Result.Add({Operation->Request.GetOrigin(), FState::MakeSnapshot(*Operation)});
			}
		}
		return Result;
	}

	bool FLongRunningOperationOwner::Recover(
		const FString& CallerId, const FString& RequestId, const FString& CommandId,
		FLongRunningOperationSnapshot& OutSnapshot, FExecutionMutationError& OutError) const
	{
		OutSnapshot = {};
		OutError.Reset();
		for (const auto& Operation : State->CopyOperations())
		{
			FScopeLock Lock(&Operation->Mutex);
			const auto& Origin = Operation->Request.GetOrigin();
			if (CallerId.IsEmpty() || Origin.CallerId != CallerId) continue;
			if (CommandId == TEXT("operation.takeover") && Operation->CustodyRequestId == RequestId)
			{
				OutSnapshot = FState::MakeSnapshot(*Operation);
				return true;
			}
			if (Origin.RequestId != RequestId || Origin.CommandId != CommandId) continue;
			if (Operation->CustodyLeaseId != Operation->Request.GetOwnerLeaseId())
			{
				SetError(OutError, TEXT("operation_taken_over"),
					TEXT("The operation has been taken over; discover it and explicitly take custody to continue."),
					TEXT("request_id"));
				return false;
			}
			OutSnapshot = FState::MakeSnapshot(*Operation);
			return true;
		}
		SetError(OutError, TEXT("operation_recovery_evidence_missing"),
			TEXT("No retained operation matches this original request in the current Editor session. Do not replay the action based on this result."),
			TEXT("request_id"));
		return false;
	}

	bool FLongRunningOperationOwner::TakeOver(
		const FString& OperationId,
		const FString& CallerId,
		const FString& NewOwnerLeaseId,
		const FString& ClaimRequestId,
		FLongRunningOperationSnapshot& OutSnapshot,
		FExecutionMutationError& OutError)
	{
		OutSnapshot = {};
		OutError = {};
		const auto Operation = State->Find(OperationId);
		if (!Operation)
		{
			SetError(OutError, TEXT("operation_not_found"),
				TEXT("The operation is not registered in this Editor session."), TEXT("operation_id"));
			return false;
		}
		FScopeLock Lock(&Operation->Mutex);
		if (CallerId.IsEmpty() || Operation->Request.GetOrigin().CallerId != CallerId)
		{
			SetError(OutError, TEXT("operation_caller_mismatch"),
				TEXT("Only the authenticated owner can take over this operation."), TEXT("caller"));
			return false;
		}
		if (NewOwnerLeaseId.IsEmpty())
		{
			SetError(OutError, TEXT("operation_owner_lease_missing"),
				TEXT("Takeover requires a new custody lease."), TEXT("owner_lease_id"));
			return false;
		}
		if (Operation->bAdvanceInProgress || Operation->bPortCallInFlight)
		{
			SetError(OutError, TEXT("operation_takeover_pending"),
				TEXT("A native operation call is in progress; retry takeover after it returns."), TEXT("phase"));
			return false;
		}
		Operation->CustodyLeaseId = NewOwnerLeaseId;
		Operation->CustodyRequestId = ClaimRequestId;
		OutSnapshot = FState::MakeSnapshot(*Operation);
		return true;
	}

	void FLongRunningOperationOwner::PollStarted()
	{
		for (const auto& Operation : State->CopyOperations())
		{
			FLongRunningOperationSnapshot Snapshot;
			{
				FScopeLock Lock(&Operation->Mutex);
				if (Operation->Phase == ELongRunningOperationPhase::Accepted
					|| Operation->Phase == ELongRunningOperationPhase::Terminal
					|| Operation->bAdvanceInProgress || Operation->bPortCallInFlight)
					continue;
				Snapshot = FState::MakeSnapshot(*Operation);
			}
			FLongRunningOperationSnapshot Updated;
			FExecutionMutationError Error;
			Advance(Snapshot.GetOperationId(), Snapshot.GetOwnerLeaseId(),
				Snapshot.GetServerSessionId(), Snapshot.GetGeneration(), Updated, Error);
			// An observation/cleanup failure remains on its operation for recovery.
			// A poll never removes a record or replays accepted business work.
		}
	}

	void FLongRunningOperationOwner::RequestShutdown(const FString& Reason)
	{
		for (const auto& Operation : State->CopyOperations())
		{
			FLongRunningOperationSnapshot Snapshot;
			{
				FScopeLock Lock(&Operation->Mutex);
				if (Operation->Phase == ELongRunningOperationPhase::Terminal
					|| Operation->Request.GetOrigin().bCompletesOnHostExit) continue;
				Snapshot = FState::MakeSnapshot(*Operation);
			}
			FLongRunningOperationSnapshot Updated;
			FExecutionMutationError Error;
			RequestCancellation(Snapshot.GetOperationId(), Snapshot.GetOwnerLeaseId(),
				Snapshot.GetServerSessionId(), Reason, Updated, Error);
		}
	}

	int32 FLongRunningOperationOwner::GetPendingCount() const
	{
		int32 Count = 0;
		for (const auto& Operation : State->CopyOperations())
		{
			FScopeLock Lock(&Operation->Mutex);
			if (!Operation->bReleased && !Operation->Request.GetOrigin().bCompletesOnHostExit) ++Count;
		}
		return Count;
	}
}
