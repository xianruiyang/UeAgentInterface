#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "DomainAdapters/GenericEffect/UeAgentGenericEffectAdapter.h"
#include "Templates/SharedPointer.h"

class UObject;
class UWorld;

namespace UE::UAI::DomainAdapters::LandscapeGeometryEffect
{
	class UEAGENTINTERFACE_API
		IUeAgentLandscapeGeometryEffectReferenceResolver
	{
	public:
		virtual ~IUeAgentLandscapeGeometryEffectReferenceResolver() =
			default;

		virtual bool TryResolveReference(
			UWorld& World,
			const FString& CanonicalRef,
			UObject*& OutObject,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API
		IUeAgentLandscapeGeometryEffectNativeHook
	{
	public:
		virtual ~IUeAgentLandscapeGeometryEffectNativeHook() = default;

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
	TryCreateLandscapeGeometryEffectDescriptorRegistry(
		TSharedPtr<const Contract::FUeAgentDescriptorRegistry>&
			OutRegistry,
		FString& OutError);

	UEAGENTINTERFACE_API TArray<FString>
	GetLandscapeGeometryEffectAuthorityScopes();

	UEAGENTINTERFACE_API bool
	TryBuildLandscapeGeometryEffectChangeSetDraft(
		const Contract::FUeAgentOperationRequest& Request,
		const ContextResource::FUeAgentExecutionContextRef& Context,
		const StateSemantic::FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
		ContextResource::FUeAgentResourceTracker& Resources,
		TOptional<PlanningPreflight::FChangeSetDraft>& OutDraft,
		ExecutionMutation::FExecutionMutationError& OutError);

	class UEAGENTINTERFACE_API
		FUeAgentLandscapeGeometryEffectNativeHookFactory final
	{
	public:
		static bool TryCreate(
			TSharedRef<
				IUeAgentLandscapeGeometryEffectReferenceResolver,
				ESPMode::ThreadSafe> Resolver,
			TSharedPtr<
				IUeAgentLandscapeGeometryEffectNativeHook>& OutHook,
			ExecutionMutation::FExecutionMutationError& OutError);
	};
}
