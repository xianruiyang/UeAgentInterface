#pragma once

#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "DomainAdapters/EditorProjectSimpleEffect/UeAgentEditorProjectSimpleEffectCapabilities.h"
#include "Templates/SharedPointer.h"

class FJsonObject;

namespace UE::UAI::Composition::SimpleAuthoring::EditorProject::Private
{
	struct FEditorProjectDependencyState
	{
		FString TargetRef;
		FString TargetRevision;
		FString ProfileId;
		FString TypeId;

		bool IsValid() const
		{
			return !TargetRef.IsEmpty()
				&& !TargetRevision.IsEmpty()
				&& !ProfileId.IsEmpty()
				&& !TypeId.IsEmpty();
		}
	};

	bool TryBuildEditorProjectTargetState(
		DomainAdapters::EditorProjectSimpleEffect::
			IUeAgentEditorProjectSimpleEffectTargetResolver& Resolver,
		const DomainAdapters::EditorProjectSimpleEffect::
			FUeAgentEditorProjectSimpleEffectResolvedTarget& Target,
		const FString& ProfileId,
		const TSharedPtr<FJsonObject>& RequestPayload,
		bool bIncludePlanningDependencies,
		ContextResource::FUeAgentResourceTracker& Resources,
		StateSemantic::FUeAgentCanonicalValuePtr& OutState,
		TArray<FEditorProjectDependencyState>& OutDependencies,
		ExecutionMutation::FExecutionMutationError& OutError);

	bool TryProjectEditorProjectExpectedState(
		const StateSemantic::FUeAgentCanonicalValue& Before,
		const StateSemantic::FUeAgentCanonicalValue& ProposedAfter,
		ContextResource::FUeAgentResourceTracker& Resources,
		StateSemantic::FUeAgentCanonicalValuePtr& OutState,
		ExecutionMutation::FExecutionMutationError& OutError);

	bool TryWriteEditorProjectStagedConfig(
		const DomainAdapters::EditorProjectSimpleEffect::
			FUeAgentEditorProjectSimpleEffectResolvedTarget& Target,
		const FString& StagedPath,
		ExecutionMutation::FExecutionMutationError& OutError);
}
