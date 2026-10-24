#pragma once

#define UAI_LONG_RUNNING_OPERATION_TYPED_SIGNAL_V1 1

#include "CoreMinimal.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::ExecutionMutation
{
	using ContextResource::FUeAgentCancellationToken;
	using ContextResource::FUeAgentExecutionContextRef;
	using ContextResource::FUeAgentExecutionContextRefOwner;
	using ContextResource::FUeAgentMonotonicClock;
	using ContextResource::FUeAgentOperationDeadline;
	using ContextResource::FUeAgentResourceTracker;
	using Contract::FUeAgentDescriptorIdentity;
	using StateSemantic::FUeAgentCanonicalValueRef;

	enum class ELongRunningOperationPhase : uint8
	{
		Accepted,
		Running,
		StopRequested,
		Joining,
		Terminal,
	};

	enum class ELongRunningOperationStatus : uint8
	{
		None,
		Succeeded,
		Cancelled,
		Failed,
		StateUnknown,
	};

	enum class ELongRunningWorkState : uint8
	{
		Invalid,
		Running,
		ExitedSuccess,
		ExitedFailure,
		StateUnknown,
	};

	UEAGENTINTERFACE_API FString LexToString(
		ELongRunningOperationPhase Value);
	UEAGENTINTERFACE_API FString LexToString(
		ELongRunningOperationStatus Value);
	UEAGENTINTERFACE_API FString LexToString(
		ELongRunningWorkState Value);

	class UEAGENTINTERFACE_API FLongRunningWorkIdentity final
	{
	public:
		FLongRunningWorkIdentity() = default;
		FLongRunningWorkIdentity(
			FString InResourceId,
			int64 InGeneration);

		const FString& GetResourceId() const;
		int64 GetGeneration() const;
		bool IsValid() const;

		bool operator==(
			const FLongRunningWorkIdentity& Other) const;

	private:
		FString ResourceId;
		int64 Generation = 0;
	};

	class UEAGENTINTERFACE_API FLongRunningOperationSignal final
	{
	public:
		FLongRunningOperationSignal(
			FString InTypeId,
			int64 InRevision,
			FUeAgentCanonicalValueRef InPayload);

		const FString& GetTypeId() const;
		int64 GetRevision() const;
		const FUeAgentCanonicalValueRef& GetPayload() const;
		bool IsValid() const;

	private:
		FString TypeId;
		int64 Revision = 0;
		FUeAgentCanonicalValueRef Payload;
	};

	class UEAGENTINTERFACE_API FLongRunningWorkObservation final
	{
	public:
		FLongRunningWorkObservation() = default;
		FLongRunningWorkObservation(
			int64 InGeneration,
			ELongRunningWorkState InState,
			FExecutionMutationError InDiagnostic,
			TOptional<FLongRunningOperationSignal> InSignal = {});

		int64 GetGeneration() const;
		ELongRunningWorkState GetState() const;
		const FString& GetErrorCode() const;
		const FExecutionMutationError& GetDiagnostic() const;
		const TOptional<FLongRunningOperationSignal>& GetSignal() const;
		bool IsValid() const;

	private:
		int64 Generation = 0;
		ELongRunningWorkState State =
			ELongRunningWorkState::Invalid;
		FExecutionMutationError Diagnostic;
		TOptional<FLongRunningOperationSignal> Signal;
	};

	struct UEAGENTINTERFACE_API FLongRunningOperationOrigin final
	{
		FString CallerId;
		FString RequestId;
		FString CommandId;
		FString SubjectRef;
		bool bCompletesOnHostExit = false;
	};

	class UEAGENTINTERFACE_API FLongRunningOperationRequest final
	{
	public:
		FLongRunningOperationRequest(
			FString InOperationId,
			FString InOwnerLeaseId,
			FString InServerSessionId,
			FUeAgentDescriptorIdentity InDescriptorIdentity,
			FUeAgentExecutionContextRefOwner InExecutionContext,
			TSharedRef<
				FUeAgentResourceTracker,
				ESPMode::ThreadSafe> InResources,
			FUeAgentOperationDeadline InDeadline,
			FLongRunningOperationOrigin InOrigin = {});

		const FString& GetOperationId() const;
		const FString& GetOwnerLeaseId() const;
		const FString& GetServerSessionId() const;
		const FUeAgentDescriptorIdentity&
			GetDescriptorIdentity() const;
		const FUeAgentExecutionContextRef&
			GetExecutionContext() const;
		FUeAgentResourceTracker& GetResources() const;
		const FUeAgentOperationDeadline& GetDeadline() const;
		const FLongRunningOperationOrigin& GetOrigin() const { return Origin; }
		bool IsValid() const;
		const FExecutionMutationError& GetValidationDiagnostic() const;

	private:
		FString OperationId;
		FString OwnerLeaseId;
		FString ServerSessionId;
		FUeAgentDescriptorIdentity DescriptorIdentity;
		FUeAgentExecutionContextRefOwner ExecutionContext;
		TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
			Resources;
		FUeAgentOperationDeadline Deadline;
		FLongRunningOperationOrigin Origin;
		TSharedPtr<const FExecutionMutationError> ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API FLongRunningOperationSnapshot final
	{
	public:
		FLongRunningOperationSnapshot() = default;
		FLongRunningOperationSnapshot(
			FString InOperationId,
			FString InOwnerLeaseId,
			FString InServerSessionId,
			FUeAgentDescriptorIdentity InDescriptorIdentity,
			ELongRunningOperationPhase InPhase,
			FLongRunningWorkIdentity InWorkIdentity,
			ELongRunningOperationStatus InStatus,
			FExecutionMutationError InDiagnostic,
			bool bInStopRequested,
			TOptional<FLongRunningOperationSignal> InLatestSignal = {},
			bool bInCleanupComplete = false,
			FExecutionMutationError InProgressDiagnostic = {});

		const FString& GetOperationId() const;
		const FString& GetOwnerLeaseId() const;
		const FString& GetServerSessionId() const;
		const FUeAgentDescriptorIdentity&
			GetDescriptorIdentity() const;
		ELongRunningOperationPhase GetPhase() const;
		const FString& GetNativeResourceId() const;
		int64 GetGeneration() const;
		ELongRunningOperationStatus GetStatus() const;
		const FString& GetErrorCode() const;
		const FExecutionMutationError& GetDiagnostic() const;
		bool IsStopRequested() const;
		const TOptional<FLongRunningOperationSignal>&
			GetLatestSignal() const;
		bool IsTerminal() const;
		bool IsCleanupComplete() const { return bCleanupComplete; }
		const FExecutionMutationError& GetProgressDiagnostic() const { return ProgressDiagnostic; }
		bool IsValid() const;

	private:
		FString OperationId;
		FString OwnerLeaseId;
		FString ServerSessionId;
		TOptional<FUeAgentDescriptorIdentity> DescriptorIdentity;
		ELongRunningOperationPhase Phase =
			ELongRunningOperationPhase::Accepted;
		FLongRunningWorkIdentity WorkIdentity;
		ELongRunningOperationStatus Status =
			ELongRunningOperationStatus::None;
		FExecutionMutationError Diagnostic;
		bool bStopRequested = false;
		TOptional<FLongRunningOperationSignal> LatestSignal;
		bool bCleanupComplete = false;
		FExecutionMutationError ProgressDiagnostic;
		bool bValid = false;
	};

	struct UEAGENTINTERFACE_API FLongRunningOperationEntry final
	{
		FLongRunningOperationOrigin Origin;
		FLongRunningOperationSnapshot Snapshot;
	};

	class UEAGENTINTERFACE_API FLongRunningOperationReceipt final
	{
	public:
		FLongRunningOperationReceipt() = default;
		FLongRunningOperationReceipt(
			FString InOperationId,
			FLongRunningWorkIdentity InWorkIdentity,
			ELongRunningOperationStatus InStatus,
			FExecutionMutationError InDiagnostic,
			bool bInRetrySafe);

		const FString& GetOperationId() const;
		const FString& GetNativeResourceId() const;
		int64 GetGeneration() const;
		ELongRunningOperationStatus GetStatus() const;
		const FString& GetErrorCode() const;
		const FExecutionMutationError& GetDiagnostic() const;
		bool IsRetrySafe() const;
		bool IsValid() const;

	private:
		FString OperationId;
		FLongRunningWorkIdentity WorkIdentity;
		ELongRunningOperationStatus Status =
			ELongRunningOperationStatus::None;
		FExecutionMutationError Diagnostic;
		bool bRetrySafe = false;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API ILongRunningOperationPort
	{
	public:
		virtual ~ILongRunningOperationPort() = default;

		virtual bool Start(
			const FLongRunningOperationRequest& Request,
			const FUeAgentCancellationToken& Cancellation,
			FLongRunningWorkIdentity& OutIdentity,
			FExecutionMutationError& OutError) = 0;
		// Queried only after Start returned false. True is explicit proof that no
		// operation-owned native resource remains; false preserves unknown cleanup.
		virtual bool IsFailedStartCleanupComplete() const { return false; }
		virtual bool Observe(
			const FString& OperationId,
			const FLongRunningWorkIdentity& Identity,
			FUeAgentResourceTracker& Resources,
			FLongRunningWorkObservation& OutObservation,
			FExecutionMutationError& OutError) = 0;
		virtual bool RequestStop(
			const FString& OperationId,
			const FLongRunningWorkIdentity& Identity,
			const FString& Reason,
			FExecutionMutationError& OutError) = 0;
		virtual bool Join(
			const FString& OperationId,
			const FLongRunningWorkIdentity& Identity,
			FExecutionMutationError& OutError) = 0;
		virtual bool Release(
			const FString& OperationId,
			const FLongRunningWorkIdentity& Identity,
			FExecutionMutationError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API FLongRunningOperationOwner final
	{
	public:
		FLongRunningOperationOwner(
			TSharedRef<ILongRunningOperationPort, ESPMode::ThreadSafe> InPort,
			FUeAgentMonotonicClock InClock);
		~FLongRunningOperationOwner();

		bool Accept(
			const FLongRunningOperationRequest& Request,
			FLongRunningOperationSnapshot& OutSnapshot,
			FExecutionMutationError& OutError);
		bool Accept(
			const FLongRunningOperationRequest& Request,
			TSharedRef<ILongRunningOperationPort, ESPMode::ThreadSafe> OperationPort,
			FLongRunningOperationSnapshot& OutSnapshot,
			FExecutionMutationError& OutError);
		bool GetSnapshot(
			const FString& OperationId,
			const FString& OwnerLeaseId,
			const FString& ServerSessionId,
			FLongRunningOperationSnapshot& OutSnapshot,
			FExecutionMutationError& OutError) const;
		bool Advance(
			const FString& OperationId,
			const FString& OwnerLeaseId,
			const FString& ServerSessionId,
			int64 ExpectedGeneration,
			FLongRunningOperationSnapshot& OutSnapshot,
			FExecutionMutationError& OutError);
		bool Advance(
			const FString& OperationId,
			const FString& OwnerLeaseId,
			const FString& ServerSessionId,
			int64 ExpectedGeneration,
			const FUeAgentOperationDeadline& CallDeadline,
			FLongRunningOperationSnapshot& OutSnapshot,
			FExecutionMutationError& OutError);
		bool RequestCancellation(
			const FString& OperationId,
			const FString& OwnerLeaseId,
			const FString& ServerSessionId,
			const FString& Reason,
			FLongRunningOperationSnapshot& OutSnapshot,
			FExecutionMutationError& OutError);
		bool GetTerminalReceipt(
			const FString& OperationId,
			const FString& OwnerLeaseId,
			const FString& ServerSessionId,
			FLongRunningOperationReceipt& OutReceipt,
			FExecutionMutationError& OutError) const;
		bool ReleaseTerminal(
			const FString& OperationId,
			const FString& OwnerLeaseId,
			const FString& ServerSessionId,
			FExecutionMutationError& OutError);

		/** Discovery is scoped to the authenticated caller, never to an asset path. */
		TArray<FLongRunningOperationEntry> Discover(const FString& CallerId) const;
		bool Recover(
			const FString& CallerId, const FString& RequestId, const FString& CommandId,
			FLongRunningOperationSnapshot& OutSnapshot, FExecutionMutationError& OutError) const;
		bool TakeOver(
			const FString& OperationId,
			const FString& CallerId,
			const FString& NewOwnerLeaseId,
			const FString& ClaimRequestId,
			FLongRunningOperationSnapshot& OutSnapshot,
			FExecutionMutationError& OutError);
		/** Observe and finish work already started; never starts an accepted operation. */
		void PollStarted();
		/** Request native shutdown through each owned port; later polls finish draining. */
		void RequestShutdown(const FString& Reason);
		int32 GetPendingCount() const;

	private:
		class FState;
		bool AdvanceInternal(
			const FString& OperationId,
			const FString& OwnerLeaseId,
			const FString& ServerSessionId,
			int64 ExpectedGeneration,
			const FUeAgentOperationDeadline* CallDeadline,
			FLongRunningOperationSnapshot& OutSnapshot,
			FExecutionMutationError& OutError);
		TSharedRef<FState, ESPMode::ThreadSafe> State;
	};
}
