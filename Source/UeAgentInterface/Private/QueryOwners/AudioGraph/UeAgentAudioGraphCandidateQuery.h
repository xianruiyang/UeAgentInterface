#pragma once

#include "CoreMinimal.h"

class UObject;

namespace UE::UAI::QueryOwners::AudioGraph
{
	struct UEAGENTINTERFACE_API FAudioGraphCandidate
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

	struct UEAGENTINTERFACE_API FAudioGraphCandidateQueryResult
	{
		FString Target;
		FString TargetRevision;
		TArray<FAudioGraphCandidate> Candidates;
		int32 TotalMatches = 0;
		int32 NextCursor = INDEX_NONE;
		bool bComplete = false;
		bool bHasMore = false;
	};

	struct UEAGENTINTERFACE_API FAudioGraphCandidateQueryError
	{
		FString Code;
		FString SubjectId;
		FString Detail;
	};

	UEAGENTINTERFACE_API bool TryQueryAudioGraphCandidates(
		UObject& Target,
		const FString& Query,
		FAudioGraphCandidateQueryResult& OutResult,
		FAudioGraphCandidateQueryError& OutError,
		int32 Limit = 64,
		int32 Cursor = 0);
}
