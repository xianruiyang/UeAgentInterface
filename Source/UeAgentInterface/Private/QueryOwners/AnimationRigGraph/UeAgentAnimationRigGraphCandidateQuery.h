#pragma once

#include "CoreMinimal.h"

class UObject;

namespace UE::UAI::QueryOwners::AnimationRigGraph
{
	struct UEAGENTINTERFACE_API FAnimationRigGraphCandidate
	{
		FString Name;
		FString Label;
		FString Kind;
		TArray<FString> SupportedOperations;
		FString InputContract;
		FString PlacementContract;
		FString RequiredContext;
		FString BindingRevision;
		int32 MajorVersion = 0;
		int32 MinorVersion = 0;
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
		int32 Cursor = 0);
}
