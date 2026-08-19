#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "DomainAdapters/GenericEffect/UeAgentGenericEffectAdapter.h"
#include "Templates/SharedPointer.h"

class UObject;
class UWorld;

namespace UE::UAI::DomainAdapters::ModelingGeometryEffect
{
	struct UEAGENTINTERFACE_API
		FUeAgentModelingGeometryEffectProductionProfile
	{
		FString DescriptorId;
		int32 DescriptorVersion = 1;
		FString DescriptorHash;
		FString CapabilityId;
		FString NativeTargetKind;
		FString OperationKey;
		bool bPrimitiveOutput = false;

		bool IsValid() const
		{
			return !DescriptorId.IsEmpty()
				&& DescriptorVersion > 0
				&& !DescriptorHash.IsEmpty()
				&& !CapabilityId.IsEmpty()
				&& !NativeTargetKind.IsEmpty()
				&& !OperationKey.IsEmpty();
		}
	};

	class UEAGENTINTERFACE_API
		IUeAgentModelingGeometryEffectReferenceResolver
	{
	public:
		virtual ~IUeAgentModelingGeometryEffectReferenceResolver() =
			default;

		virtual bool TryResolveReference(
			UWorld& World,
			const FString& CanonicalRef,
			UObject*& OutObject,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API
		IUeAgentModelingGeometryEffectNativeHook
	{
	public:
		virtual ~IUeAgentModelingGeometryEffectNativeHook() = default;

		virtual bool TryApply(
			UWorld& World,
			const ExecutionMutation::FExecutionMutationRequest& Request,
			const ExecutionMutation::FLockFenceLease& Lease,
			const TArray<
				GenericEffect::FUeAgentGenericEffectBinding>& Bindings,
			ContextResource::FUeAgentResourceTracker& Resources,
			GenericEffect::FUeAgentGenericEffectApplyResult& OutResult,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
		virtual bool TryRollback(
			UWorld& World,
			const ExecutionMutation::FExecutionMutationRequest& Request,
			const GenericEffect::FUeAgentGenericEffectApplyResult& Applied,
			ContextResource::FUeAgentResourceTracker& Resources,
			bool& bOutConfirmed,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
		virtual void ReleaseVerifiedOperation(
			const FString& OperationId) = 0;
		virtual bool TryWaitForSettle(
			const ExecutionMutation::FBarrierRequest& Request,
			ContextResource::FUeAgentResourceTracker& Resources,
			ExecutionMutation::FBarrierResult& OutResult,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool
	TryCreateModelingGeometryEffectDescriptorRegistry(
		TSharedPtr<const Contract::FUeAgentDescriptorRegistry>&
			OutRegistry,
		FString& OutError);

	UEAGENTINTERFACE_API TArray<FString>
	GetModelingGeometryEffectAuthorityScopes();

	UEAGENTINTERFACE_API bool
	TryGetModelingGeometryEffectProductionProfiles(
		TArray<FUeAgentModelingGeometryEffectProductionProfile>&
			OutProfiles,
		FString& OutError);

	UEAGENTINTERFACE_API bool
	TryBuildModelingGeometryEffectChangeSetDraft(
		const Contract::FUeAgentOperationRequest& Request,
		const ContextResource::FUeAgentExecutionContextRef& Context,
		const StateSemantic::FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
		ContextResource::FUeAgentResourceTracker& Resources,
		TOptional<PlanningPreflight::FChangeSetDraft>& OutDraft,
		ExecutionMutation::FExecutionMutationError& OutError);

	class UEAGENTINTERFACE_API
		FUeAgentModelingGeometryEffectNativeHookFactory final
	{
	public:
		static bool TryCreate(
			TSharedRef<
				IUeAgentModelingGeometryEffectReferenceResolver,
				ESPMode::ThreadSafe> Resolver,
			TSharedPtr<
				IUeAgentModelingGeometryEffectNativeHook>& OutHook,
			ExecutionMutation::FExecutionMutationError& OutError);
	};
}
