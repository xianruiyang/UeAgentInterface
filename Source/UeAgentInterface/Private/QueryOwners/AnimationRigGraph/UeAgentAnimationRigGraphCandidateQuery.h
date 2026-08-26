#pragma once

#include "CoreMinimal.h"

class UObject;
class UControlRigBlueprint;
class URigVMUnitNode;

namespace UE::UAI::QueryOwners::AnimationRigGraph
{
	struct UEAGENTINTERFACE_API FAnimationRigGraphCandidate
	{
		FString Id;
		FString Name;
		FString Label;
		FString Kind;
		FString ActionKind;
		FString Category;
		FString Summary;
		FString WriteSupport;
		FString SupportReason;
		bool bSupported = false;
		TArray<FString> SupportedOperations;
	};

	struct UEAGENTINTERFACE_API FAnimationRigGraphCandidateQueryResult
	{
		FString Target;
		FString TargetRevision;
		TArray<FAnimationRigGraphCandidate> Candidates;
		int32 TotalMatches = 0;
		int32 NextCursor = INDEX_NONE;
		bool bComplete = false;
		bool bHasMore = false;
		bool bCoverageComplete = false;
		bool bCreationCoverageComplete = false;
		int32 NativeActionCount = 0;
		int32 ExecutableActionCount = 0;
		FString CoverageScope;
		FString GraphName;
	};

	struct UEAGENTINTERFACE_API FAnimationRigGraphCandidateQueryError
	{
		FString Code;
		FString SubjectId;
		FString Detail;
	};

	UEAGENTINTERFACE_API bool TryQueryAnimationRigGraphCandidates(
		UObject& Target,
		const FString& Query,
		FAnimationRigGraphCandidateQueryResult& OutResult,
		FAnimationRigGraphCandidateQueryError& OutError,
		int32 Limit = 64,
		int32 Cursor = 0,
		const FString& CandidateId = FString());

	/** Re-enumerate the native default-graph blank context and resolve one selector. */
	UEAGENTINTERFACE_API bool TryResolveAnimationRigGraphCandidate(
		UControlRigBlueprint& Target,
		const FString& Name,
		const FString& Id,
		FAnimationRigGraphCandidate& OutCandidate,
		FAnimationRigGraphCandidateQueryError& OutError);

	/** Re-enumerate and invoke the same supported RigVM menu action. */
	UEAGENTINTERFACE_API bool TryPerformAnimationRigGraphCandidate(
		UControlRigBlueprint& Target,
		const FString& Name,
		const FString& Id,
		const FVector2f& Location,
		URigVMUnitNode*& OutNode,
		FAnimationRigGraphCandidate& OutCandidate,
		FAnimationRigGraphCandidateQueryError& OutError);
}
