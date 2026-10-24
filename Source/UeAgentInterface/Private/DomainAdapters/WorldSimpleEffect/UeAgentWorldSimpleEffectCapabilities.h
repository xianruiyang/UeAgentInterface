#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectAdapter.h"
#include "Templates/SharedPointer.h"

class UObject;
class UWorld;

namespace UE::UAI::DomainAdapters::WorldSimpleEffect
{
	class UEAGENTINTERFACE_API
		IUeAgentWorldSimpleEffectReferenceResolver
	{
	public:
		virtual ~IUeAgentWorldSimpleEffectReferenceResolver() = default;

		virtual bool TryResolveReference(
			UWorld& World,
			const FString& CanonicalRef,
			UObject*& OutObject,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool
	TryCreateWorldSimpleEffectDescriptorRegistry(
		TSharedPtr<const Contract::FUeAgentDescriptorRegistry>&
			OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API TArray<FString>
	GetWorldSimpleEffectAuthorityScopes();

	UEAGENTINTERFACE_API bool
	TryBuildWorldSimpleEffectChangeSetDraft(
		const Contract::FUeAgentOperationRequest& Request,
		const ContextResource::FUeAgentExecutionContextRef& Context,
		const StateSemantic::FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
		ContextResource::FUeAgentResourceTracker& Resources,
		TOptional<PlanningPreflight::FChangeSetDraft>& OutDraft,
		ExecutionMutation::FExecutionMutationError& OutError);

	class UEAGENTINTERFACE_API
		FUeAgentWorldSimpleEffectNativeHookFactory final
	{
	public:
		static bool TryCreate(
			TSharedRef<
				IUeAgentWorldSimpleEffectReferenceResolver,
				ESPMode::ThreadSafe> Resolver,
			TSharedPtr<IUeAgentWorldSimpleEffectNativeHook>& OutHook,
			ExecutionMutation::FExecutionMutationError& OutError);
	};
}
