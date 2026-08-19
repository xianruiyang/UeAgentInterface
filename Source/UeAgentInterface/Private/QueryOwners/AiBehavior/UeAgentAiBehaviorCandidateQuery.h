#pragma once

#include "CoreMinimal.h"

class UObject;

namespace UE::UAI::QueryOwners::AiBehavior
{
	struct UEAGENTINTERFACE_API FAiBehaviorCandidate
	{
		FString Name;
		FString Label;
		FString Kind;
		TArray<FString> SupportedOperations;
		FString InputContract;
		FString PlacementContract;
		FString RequiredContext;
		FString BindingRevision;
	};

	struct UEAGENTINTERFACE_API FAiBehaviorCandidateQueryResult
	{
		FString Target;
		FString TargetRevision;
		TArray<FAiBehaviorCandidate> Candidates;
		int32 TotalMatches = 0;
		int32 NextCursor = INDEX_NONE;
		bool bComplete = false;
		bool bHasMore = false;
	};

	struct UEAGENTINTERFACE_API FAiBehaviorCandidateQueryError
	{
		FString Code;
		FString SubjectId;
		FString Detail;
	};

	UEAGENTINTERFACE_API bool TryQueryAiBehaviorCandidates(
		UObject& Target,
		const FString& Query,
		FAiBehaviorCandidateQueryResult& OutResult,
		FAiBehaviorCandidateQueryError& OutError,
		int32 Limit = 64,
		int32 Cursor = 0);
}
