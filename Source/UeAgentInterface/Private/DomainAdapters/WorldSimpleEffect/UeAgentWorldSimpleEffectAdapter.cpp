#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectAdapter.h"

#include "Engine/World.h"

namespace UE::UAI::DomainAdapters::WorldSimpleEffect
{
	using namespace ContextResource;
	using namespace ExecutionMutation;
	using namespace GenericEffect;

	namespace
	{
		bool Fail(
			FExecutionMutationError& OutError,
			FString Code,
			FString Detail = FString())
		{
			OutError = FExecutionMutationError(
				MoveTemp(Code),
				MoveTemp(Detail));
			return false;
		}
	}

	FUeAgentWorldSimpleEffectUeHook::
	FUeAgentWorldSimpleEffectUeHook(
		TSharedRef<
			IUeAgentWorldSimpleEffectWorldResolver,
			ESPMode::ThreadSafe> InWorldResolver,
		TSharedRef<
			IUeAgentWorldSimpleEffectNativeHook,
			ESPMode::ThreadSafe> InNativeHook)
		: WorldResolver(MoveTemp(InWorldResolver))
		, NativeHook(MoveTemp(InNativeHook))
	{
	}

	bool FUeAgentWorldSimpleEffectUeHook::TryApply(
		const FExecutionMutationRequest& Request,
		const FLockFenceLease& Lease,
		const TArray<FUeAgentGenericEffectBinding>& Bindings,
		FUeAgentResourceTracker& Resources,
		FUeAgentGenericEffectApplyResult& OutResult,
		FExecutionMutationError& OutError)
	{
		OutResult = FUeAgentGenericEffectApplyResult();
		OutError = FExecutionMutationError();
		if (!Request.IsValid() || !Lease.IsValid())
		{
			return Fail(
				OutError,
				TEXT("world_simple_request_invalid"));
		}

		UWorld* World = nullptr;
		if (!WorldResolver->TryResolveWorld(
				Request.GetExecutionContext(),
				World,
				OutError))
		{
			return false;
		}
		if (!IsValid(World))
		{
			return Fail(
				OutError,
				TEXT("world_simple_context_invalid"),
				Request.GetExecutionContext().GetWorldIdentity());
		}
		return NativeHook->TryApply(
			*World,
			Request,
			Lease,
			Bindings,
			Resources,
			OutResult,
			OutError);
	}

	bool FUeAgentWorldSimpleEffectUeHook::TryRollback(
		const FExecutionMutationRequest& Request,
		const FUeAgentGenericEffectApplyResult& Applied,
		FUeAgentResourceTracker& Resources,
		bool& bOutConfirmed,
		FExecutionMutationError& OutError)
	{
		bOutConfirmed = false;
		OutError = FExecutionMutationError();
		if (!Request.IsValid() || !Applied.IsValid())
		{
			return Fail(
				OutError,
				TEXT("world_simple_rollback_request_invalid"));
		}

		UWorld* World = nullptr;
		if (!WorldResolver->TryResolveWorld(
				Request.GetExecutionContext(),
				World,
				OutError))
		{
			return false;
		}
		if (!IsValid(World))
		{
			return Fail(
				OutError,
				TEXT("world_simple_context_invalid"),
				Request.GetExecutionContext().GetWorldIdentity());
		}
		return NativeHook->TryRollback(
			*World,
			Request,
			Applied,
			Resources,
			bOutConfirmed,
			OutError);
	}

	void FUeAgentWorldSimpleEffectUeHook::
	ReleaseVerifiedOperation(const FString& OperationId)
	{
		NativeHook->ReleaseVerifiedOperation(OperationId);
	}

	bool FUeAgentWorldSimpleEffectUeHook::TryWaitForSettle(
		const FBarrierRequest& Request,
		FUeAgentResourceTracker& Resources,
		FBarrierResult& OutResult,
		FExecutionMutationError& OutError)
	{
		OutResult = FBarrierResult();
		OutError = FExecutionMutationError();
		if (!Request.IsValid())
		{
			return Fail(
				OutError,
				TEXT("world_simple_settle_request_invalid"));
		}
		return NativeHook->TryWaitForSettle(
			Request,
			Resources,
			OutResult,
			OutError);
	}
}

