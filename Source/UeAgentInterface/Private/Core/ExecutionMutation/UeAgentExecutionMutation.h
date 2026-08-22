#pragma once

#include "CoreMinimal.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::ExecutionMutation
{
	using AssetDurable::FDurableReceipt;
	using AssetDurable::FAssetDurableTarget;
	using ContextResource::EUeAgentRequestKind;
	using ContextResource::FUeAgentCancellationToken;
	using ContextResource::FUeAgentExecutionContextRef;
	using ContextResource::FUeAgentExecutionContextRefOwner;
	using ContextResource::FUeAgentOperationDeadline;
	using ContextResource::FUeAgentResourceTracker;
	using PlanningPreflight::FPlanningPreflightPlanRef;

	class UEAGENTINTERFACE_API FExecutionMutationError final
	{
	public:
		FExecutionMutationError() = default;
		explicit FExecutionMutationError(
			FString InCode,
			FString InDetail = FString());

		const FString& GetCode() const;
		const FString& GetDetail() const;
		bool IsPresent() const;

	private:
		FString Code;
		FString Detail;
	};

	enum class EExecutionPhase : uint8
	{
		Queued,
		WaitingForLock,
		Executing,
		Settling,
		Persisting,
		Terminal,
	};

	enum class EExecutionStatus : uint8
	{
		CancelledNoChange,
		AppliedInMemory,
		PersistedUnverified,
		RolledBack,
		RecoveryRequired,
		StateUnknown,
		Failed,
	};

	enum class EExecutionCancellationMode : uint8
	{
		NotRequested,
		SafeBoundary,
		RollbackRequired,
	};

	enum class EExecutionThreadAffinity : uint8
	{
		GameThread,
	};

	enum class EExecutionReentrancyPolicy : uint8
	{
		RejectSameTarget,
	};

	UEAGENTINTERFACE_API FString LexToString(EExecutionPhase Value);
	UEAGENTINTERFACE_API FString LexToString(EExecutionStatus Value);
	UEAGENTINTERFACE_API FString LexToString(
		EExecutionCancellationMode Value);
	UEAGENTINTERFACE_API FString LexToString(
		EExecutionThreadAffinity Value);
	UEAGENTINTERFACE_API FString LexToString(
		EExecutionReentrancyPolicy Value);

	class UEAGENTINTERFACE_API FLockFenceLease final
	{
	public:
		FLockFenceLease() = default;
		FLockFenceLease(
			FString InOwnerId,
			TArray<FString> InLockKeys,
			int64 InFencingToken);

		const FString& GetOwnerId() const;
		const TArray<FString>& GetLockKeys() const;
		int64 GetFencingToken() const;
		bool IsValid() const;

	private:
		FString OwnerId;
		TArray<FString> LockKeys;
		int64 FencingToken = -1;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FBarrierRequest final
	{
	public:
		FBarrierRequest() = default;
		FBarrierRequest(
			int64 InSubjectGeneration,
			TArray<FString> InRequiredSignals);

		int64 GetSubjectGeneration() const;
		const TArray<FString>& GetRequiredSignals() const;
		bool IsValid() const;

	private:
		int64 SubjectGeneration = -1;
		TArray<FString> RequiredSignals;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FBarrierResult final
	{
	public:
		FBarrierResult() = default;
		FBarrierResult(
			int64 InSubjectGeneration,
			TArray<FString> InObservedSignals,
			FString InPostBarrierRevision,
			bool bInSettled);

		int64 GetSubjectGeneration() const;
		const TArray<FString>& GetObservedSignals() const;
		const FString& GetPostBarrierRevision() const;
		bool IsSettled() const;
		bool IsValid() const;

	private:
		int64 SubjectGeneration = -1;
		TArray<FString> ObservedSignals;
		FString PostBarrierRevision;
		bool bSettled = false;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FMutationFacts final
	{
	public:
		FMutationFacts() = default;
		FMutationFacts(
			int64 InSubjectGeneration,
			TArray<FString> InMutatedIdentityKeys,
			TArray<FString> InRequiredBarrierSignals,
			bool bInRollbackSupported,
			TArray<FAssetDurableTarget> InDurableTargets = {});

		int64 GetSubjectGeneration() const;
		const TArray<FString>& GetMutatedIdentityKeys() const;
		const TArray<FString>& GetRequiredBarrierSignals() const;
		const TArray<FAssetDurableTarget>& GetDurableTargets() const;
		bool IsRollbackSupported() const;
		bool IsValid() const;

	private:
		int64 SubjectGeneration = -1;
		TArray<FString> MutatedIdentityKeys;
		TArray<FString> RequiredBarrierSignals;
		TArray<FAssetDurableTarget> DurableTargets;
		bool bRollbackSupported = false;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FExecutionMutationRequest final
	{
	public:
		FExecutionMutationRequest(
			FString InOperationId,
			FString InOwnerLeaseId,
			FPlanningPreflightPlanRef InPlan,
			FUeAgentExecutionContextRefOwner InExecutionContext,
			TSharedRef<
				FUeAgentResourceTracker,
				ESPMode::ThreadSafe> InResources,
			FUeAgentOperationDeadline InDeadline,
			FUeAgentCancellationToken InCancellation,
			EExecutionThreadAffinity InThreadAffinity,
			EExecutionReentrancyPolicy InReentrancyPolicy);

		const FString& GetOperationId() const;
		const FString& GetOwnerLeaseId() const;
		const FPlanningPreflightPlanRef& GetPlan() const;
		const FUeAgentExecutionContextRef&
			GetExecutionContext() const;
		FUeAgentResourceTracker& GetResources() const;
		const FUeAgentOperationDeadline& GetDeadline() const;
		const FUeAgentCancellationToken& GetCancellation() const;
		EExecutionThreadAffinity GetThreadAffinity() const;
		EExecutionReentrancyPolicy GetReentrancyPolicy() const;
		bool IsValid() const;

	private:
		FString OperationId;
		FString OwnerLeaseId;
		FPlanningPreflightPlanRef Plan;
		FUeAgentExecutionContextRefOwner ExecutionContext;
		TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
			Resources;
		FUeAgentOperationDeadline Deadline;
		FUeAgentCancellationToken Cancellation;
		EExecutionThreadAffinity ThreadAffinity =
			EExecutionThreadAffinity::GameThread;
		EExecutionReentrancyPolicy ReentrancyPolicy =
			EExecutionReentrancyPolicy::RejectSameTarget;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FExecutionReceipt final
	{
	public:
		FExecutionReceipt() = default;
		FExecutionReceipt(
			FString InOperationId,
			EExecutionPhase InPhase,
			EExecutionStatus InStatus,
			FString InErrorCode,
			EExecutionCancellationMode InCancellationMode,
			bool bInMutationStarted,
			TOptional<int64> InFencingToken,
			bool bInRetrySafe,
			TOptional<FDurableReceipt> InDurableReceipt,
			TArray<FString> InPhaseHistory);

		const FString& GetOperationId() const;
		EExecutionPhase GetPhase() const;
		EExecutionStatus GetStatus() const;
		const FString& GetErrorCode() const;
		EExecutionCancellationMode GetCancellationMode() const;
		bool DidMutationStart() const;
		const TOptional<int64>& GetFencingToken() const;
		bool IsRetrySafe() const;
		const FDurableReceipt* GetDurableReceipt() const;
		const TArray<FString>& GetPhaseHistory() const;
		bool IsValid() const;

	private:
		FString OperationId;
		EExecutionPhase Phase = EExecutionPhase::Queued;
		EExecutionStatus Status = EExecutionStatus::Failed;
		FString ErrorCode;
		EExecutionCancellationMode CancellationMode =
			EExecutionCancellationMode::NotRequested;
		bool bMutationStarted = false;
		TOptional<int64> FencingToken;
		bool bRetrySafe = false;
		TOptional<FDurableReceipt> DurableReceipt;
		TArray<FString> PhaseHistory;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API IExecutionScheduler
	{
	public:
		virtual ~IExecutionScheduler() = default;

		virtual bool Schedule(
			EExecutionThreadAffinity Affinity,
			const FString& OperationId,
			TFunction<void()> Callback,
			FExecutionMutationError& OutError) = 0;
		virtual bool IsCurrentThread(
			EExecutionThreadAffinity Affinity) const = 0;
	};

	class UEAGENTINTERFACE_API FGameThreadExecutionScheduler final
		: public IExecutionScheduler
	{
	public:
		virtual bool Schedule(
			EExecutionThreadAffinity Affinity,
			const FString& OperationId,
			TFunction<void()> Callback,
			FExecutionMutationError& OutError) override;
		virtual bool IsCurrentThread(
			EExecutionThreadAffinity Affinity) const override;
	};

	class UEAGENTINTERFACE_API ILockFenceProvider
	{
	public:
		virtual ~ILockFenceProvider() = default;

		virtual bool TryAcquire(
			const TArray<FString>& LockKeys,
			const FString& OwnerId,
			FUeAgentResourceTracker& Resources,
			FLockFenceLease& OutLease,
			FExecutionMutationError& OutError) = 0;
		virtual bool IsCurrent(
			const FLockFenceLease& Lease) const = 0;
		virtual bool Release(
			const FLockFenceLease& Lease,
			FExecutionMutationError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API FMonotonicLockFenceProvider final
		: public ILockFenceProvider
	{
	public:
		virtual bool TryAcquire(
			const TArray<FString>& LockKeys,
			const FString& OwnerId,
			FUeAgentResourceTracker& Resources,
			FLockFenceLease& OutLease,
			FExecutionMutationError& OutError) override;
		virtual bool IsCurrent(
			const FLockFenceLease& Lease) const override;
		virtual bool Release(
			const FLockFenceLease& Lease,
			FExecutionMutationError& OutError) override;

	private:
		struct FHeldLock
		{
			FString OwnerId;
			int64 FencingToken = -1;
		};

		mutable FCriticalSection Mutex;
		TMap<FString, FHeldLock> HeldLocks;
		int64 NextFencingToken = 0;
	};

	class UEAGENTINTERFACE_API IMutationHook
	{
	public:
		virtual ~IMutationHook() = default;

		virtual bool Execute(
			const FExecutionMutationRequest& Request,
			const FLockFenceLease& Lease,
			FUeAgentResourceTracker& Resources,
			FMutationFacts& OutFacts,
			FExecutionMutationError& OutError) = 0;
		virtual bool Rollback(
			const FExecutionMutationRequest& Request,
			const FMutationFacts& Facts,
			FUeAgentResourceTracker& Resources,
			bool& bOutConfirmed,
			FExecutionMutationError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API IAsyncBarrierProvider
	{
	public:
		virtual ~IAsyncBarrierProvider() = default;

		virtual bool Wait(
			const FBarrierRequest& Request,
			FUeAgentResourceTracker& Resources,
			FBarrierResult& OutResult,
			FExecutionMutationError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API IExecutionDurableSession
	{
	public:
		virtual ~IExecutionDurableSession() = default;
	};

	using FExecutionDurableSessionPtr = TSharedPtr<
		IExecutionDurableSession,
		ESPMode::ThreadSafe>;
	using FExecutionDurableSessionRef = TSharedRef<
		IExecutionDurableSession,
		ESPMode::ThreadSafe>;

	class UEAGENTINTERFACE_API IDurableExecutionPort
	{
	public:
		virtual ~IDurableExecutionPort() = default;

		virtual bool Prepare(
			const FExecutionMutationRequest& Request,
			int64 FencingToken,
			FUeAgentResourceTracker& Resources,
			FExecutionDurableSessionPtr& OutSession,
			FExecutionMutationError& OutError) = 0;
		virtual bool Persist(
			const FExecutionDurableSessionRef& Session,
			int64 FencingToken,
			const FMutationFacts& Facts,
			FUeAgentResourceTracker& Resources,
			FDurableReceipt& OutReceipt,
			FExecutionMutationError& OutError) = 0;
		virtual bool Rollback(
			const FExecutionDurableSessionRef& Session,
			int64 FencingToken,
			bool bMemoryRollbackConfirmed,
			FUeAgentResourceTracker& Resources,
			FDurableReceipt& OutReceipt,
			FExecutionMutationError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API FExecutionMutationCoordinator final
	{
	public:
		FExecutionMutationCoordinator(
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
			int32 InMaxQueueItems);
		~FExecutionMutationCoordinator();

		bool Submit(
			const FExecutionMutationRequest& Request,
			TSharedRef<IMutationHook, ESPMode::ThreadSafe> Hook,
			FString& OutOperationId,
			FExecutionMutationError& OutError);
		int32 DispatchReady();
		bool RequestCancellation(
			const FString& OperationId,
			const FString& Reason,
			bool& bOutAccepted,
			FExecutionMutationError& OutError);
		bool GetReceipt(
			const FString& OperationId,
			FExecutionReceipt& OutReceipt,
			FExecutionMutationError& OutError) const;
		bool GetLease(
			const FString& OperationId,
			FLockFenceLease& OutLease,
			FExecutionMutationError& OutError) const;

	private:
		class FState;
		TSharedRef<FState, ESPMode::ThreadSafe> State;
	};
}
