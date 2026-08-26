#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UAnimBlueprint;
class UEdGraph;
class UEdGraphNode;
class UAnimationStateMachineGraph;

namespace UE::UAI::QueryOwners::AnimationBlueprint
{
	struct UEAGENTINTERFACE_API FAnimationBlueprintCandidate
	{
		FString Id;
		FString Name;
		FString Label;
		FString Surface;
		FString Kind;
		FString ActionKind;
		FString Category;
		FString Summary;
		FString NodeClassPath;
		FString SourceOrigin;
		FString WriteSupport;
		FString SupportReason;
		bool bSupported = false;
	};

	struct UEAGENTINTERFACE_API FAnimationBlueprintCandidateQueryResult
	{
		FString Target;
		FString TargetRevision;
		FString Surface;
		TArray<FAnimationBlueprintCandidate> Candidates;
		int32 TotalMatches = 0;
		int32 NextCursor = INDEX_NONE;
		bool bComplete = false;
		bool bHasMore = false;
		FString CoverageScope;
		bool bCoverageComplete = false;
		bool bCreationCoverageComplete = false;
	};

	struct UEAGENTINTERFACE_API FAnimationBlueprintCandidateQueryError
	{
		FString Code;
		FString SubjectId;
		FString Detail;
	};

	/**
	 * Enumerates one native creation surface on a transient AnimBlueprint made
	 * by the same factory seed used by T16 create preflight.
	 */
	UEAGENTINTERFACE_API bool TryQueryAnimationBlueprintCandidates(
		const FString& Target,
		const FString& Surface,
		const TSharedPtr<FJsonObject>& ClassSettings,
		const FString& Query,
		FAnimationBlueprintCandidateQueryResult& OutResult,
		FAnimationBlueprintCandidateQueryError& OutError,
		int32 Limit = 64,
		int32 Cursor = 0,
		const FString& CandidateId = FString());

	/** Re-enumerates and invokes the native AnimGraph StateMachine spawner. */
	UEAGENTINTERFACE_API bool TryPerformAnimationGraphStateMachineCandidate(
		UAnimBlueprint& Blueprint,
		UEdGraph& Graph,
		const FString& Name,
		const FString& Id,
		const FVector2f& Location,
		UEdGraphNode*& OutNode,
		FAnimationBlueprintCandidate& OutCandidate,
		FAnimationBlueprintCandidateQueryError& OutError);

	/** Re-enumerates and invokes one native StateMachine graph schema action. */
	UEAGENTINTERFACE_API bool TryPerformStateMachineGraphCandidate(
		UAnimationStateMachineGraph& Graph,
		const FString& Name,
		const FString& Id,
		const FVector2f& Location,
		UEdGraphNode*& OutNode,
		FAnimationBlueprintCandidate& OutCandidate,
		FAnimationBlueprintCandidateQueryError& OutError);

	UEAGENTINTERFACE_API bool IsReadOnlyContentGraphCommand(
		const FString& CommandId);
}
