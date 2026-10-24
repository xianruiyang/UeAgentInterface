#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectAdapter.h"

#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"

namespace UE::UAI::DomainAdapters::WorldSimpleEffect
{
	using namespace ContextResource;
	using namespace ExecutionMutation;
	using namespace GenericEffect;

	namespace
	{
		bool Fail(FExecutionMutationError& OutError, FExecutionMutationError Diagnostic)
		{
			OutError = MoveTemp(Diagnostic);
			return false;
		}

		FExecutionMutationError MissingWorld()
		{
			auto Error = FExecutionMutationError::Failure(TEXT("world_simple_context_invalid"),
				TEXT("World resolver succeeded without a valid world object."), TEXT("execution"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = TEXT("/world");
			Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
			Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return Error;
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
		OutResult = FUeAgentGenericEffectApplyResult::NoMutation();
		OutError = FExecutionMutationError();
		if (!Request.IsValid()) return Fail(OutError, Request.GetValidationDiagnostic());
		if (!Lease.IsValid()) return Fail(OutError, Lease.GetValidationDiagnostic());

		UWorld* World = nullptr;
		if (!WorldResolver->TryResolveWorld(
				Request.GetExecutionContext(),
				World,
				OutError))
		{
			return false;
		}
		if (!IsValid(World)) return Fail(OutError, MissingWorld());
		ON_SCOPE_EXIT { if (!OutError.IsEmpty() && OutError.TargetPath.IsEmpty()) OutError.TargetPath = World->GetPathName(); };
		OutResult = FUeAgentGenericEffectApplyResult();
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
		if (!Request.IsValid()) return Fail(OutError, Request.GetValidationDiagnostic());
		if (!Applied.IsValid()) return Fail(OutError, Applied.GetValidationDiagnostic());

		UWorld* World = nullptr;
		if (!WorldResolver->TryResolveWorld(
				Request.GetExecutionContext(),
				World,
				OutError))
		{
			return false;
		}
		if (!IsValid(World)) return Fail(OutError, MissingWorld());
		ON_SCOPE_EXIT { if (!OutError.IsEmpty() && OutError.TargetPath.IsEmpty()) OutError.TargetPath = World->GetPathName(); };
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
			auto Error = FExecutionMutationError::Failure(TEXT("world_simple_settle_request_invalid"),
				TEXT("Settle request subject generation must be non-negative."), TEXT("execution"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = TEXT("/subject_generation");
			Error.ActualValue = MakeShared<FJsonValueNumber>(Request.GetSubjectGeneration());
			Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("integer >= 0"));
			return Fail(OutError, MoveTemp(Error));
		}
		return NativeHook->TryWaitForSettle(
			Request,
			Resources,
			OutResult,
			OutError);
	}
}
