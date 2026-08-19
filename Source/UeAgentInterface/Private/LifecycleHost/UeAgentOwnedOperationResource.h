#pragma once

#include "CoreMinimal.h"
#include "Core/ExecutionMutation/UeAgentLongRunningOperation.h"
#include "LifecycleHost/UeAgentLifecycleHost.h"
#include "Misc/Optional.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::LifecycleHost
{
	struct UEAGENTINTERFACE_API FUeAgentOwnedOperationResourceSpec
	{
		FString ResourceId;
		FString OperationId;
		FString OwnerLeaseId;
		int64 Generation = 0;
		FString Executable;
		TArray<FString> Arguments;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FUeAgentOwnedOperationResourceIdentity
	{
		FString ResourceId;
		FString OperationId;
		FString OwnerLeaseId;
		int64 Generation = 0;
		FUeAgentProcessIdentity Process;

		bool IsValid() const;
		bool Matches(
			const FUeAgentOwnedOperationResourceIdentity& Other) const;
	};

	struct UEAGENTINTERFACE_API FUeAgentOwnedOperationResourceObservation
	{
		bool bRunning = false;
		bool bExited = false;
		TOptional<int32> ExitCode;

		bool IsValid() const;
	};

	class UEAGENTINTERFACE_API IUeAgentOwnedOperationResourceBackend
	{
	public:
		virtual ~IUeAgentOwnedOperationResourceBackend() = default;

		virtual bool StartOwned(
			const FUeAgentOwnedOperationResourceSpec& Spec,
			FUeAgentProcessIdentity& OutProcess,
			FUeAgentLifecycleDiagnostic& OutDiagnostic) = 0;
		virtual bool ObserveOwned(
			const FUeAgentOwnedOperationResourceIdentity& Identity,
			FUeAgentOwnedOperationResourceObservation& OutObservation,
			FUeAgentLifecycleDiagnostic& OutDiagnostic) = 0;
		virtual bool RequestStopOwned(
			const FUeAgentOwnedOperationResourceIdentity& Identity,
			FUeAgentLifecycleDiagnostic& OutDiagnostic) = 0;
		virtual bool JoinOwned(
			const FUeAgentOwnedOperationResourceIdentity& Identity,
			FUeAgentLifecycleDiagnostic& OutDiagnostic) = 0;
		virtual bool ReleaseOwned(
			const FUeAgentOwnedOperationResourceIdentity& Identity,
			FUeAgentLifecycleDiagnostic& OutDiagnostic) = 0;
	};

	class UEAGENTINTERFACE_API FUeAgentOwnedOperationResourcePort final
		: public ExecutionMutation::ILongRunningOperationPort
	{
	public:
		FUeAgentOwnedOperationResourcePort(
			FUeAgentOwnedOperationResourceSpec InSpec,
			TSharedRef<
				IUeAgentOwnedOperationResourceBackend,
				ESPMode::ThreadSafe> InBackend);

		virtual bool Start(
			const ExecutionMutation::FLongRunningOperationRequest& Request,
			const ContextResource::FUeAgentCancellationToken& Cancellation,
			ExecutionMutation::FLongRunningWorkIdentity& OutIdentity,
			ExecutionMutation::FExecutionMutationError& OutError) override;
		virtual bool Observe(
			const FString& OperationId,
			const ExecutionMutation::FLongRunningWorkIdentity& Identity,
			ContextResource::FUeAgentResourceTracker& Resources,
			ExecutionMutation::FLongRunningWorkObservation& OutObservation,
			ExecutionMutation::FExecutionMutationError& OutError) override;
		virtual bool RequestStop(
			const FString& OperationId,
			const ExecutionMutation::FLongRunningWorkIdentity& Identity,
			const FString& Reason,
			ExecutionMutation::FExecutionMutationError& OutError) override;
		virtual bool Join(
			const FString& OperationId,
			const ExecutionMutation::FLongRunningWorkIdentity& Identity,
			ExecutionMutation::FExecutionMutationError& OutError) override;
		virtual bool Release(
			const FString& OperationId,
			const ExecutionMutation::FLongRunningWorkIdentity& Identity,
			ExecutionMutation::FExecutionMutationError& OutError) override;

		const FUeAgentOwnedOperationResourceIdentity* Identity() const;
		bool Cleanup(
			const FUeAgentOwnedOperationResourceIdentity& ExpectedIdentity,
			ExecutionMutation::FExecutionMutationError& OutError);

	private:
		bool ValidateWorkIdentity(
			const FString& OperationId,
			const ExecutionMutation::FLongRunningWorkIdentity& Identity,
			ExecutionMutation::FExecutionMutationError& OutError) const;

		FUeAgentOwnedOperationResourceSpec Spec;
		TSharedRef<IUeAgentOwnedOperationResourceBackend, ESPMode::ThreadSafe> Backend;
		mutable FCriticalSection Mutex;
		TOptional<FUeAgentOwnedOperationResourceIdentity>
			OwnedIdentity;
		bool bStartInFlight = false;
		bool bCallInFlight = false;
		bool bStopRequested = false;
		bool bObservedExited = false;
		bool bJoined = false;
		bool bReleased = false;
	};
}
