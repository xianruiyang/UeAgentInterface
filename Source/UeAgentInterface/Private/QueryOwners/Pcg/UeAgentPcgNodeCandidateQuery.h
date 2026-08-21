#pragma once

#include "CoreMinimal.h"

class UObject;

namespace UE::UAI::QueryOwners::Pcg
{
	struct UEAGENTINTERFACE_API FPcgNodeCandidate
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

	struct UEAGENTINTERFACE_API FPcgNodeCandidateQueryResult
	{
		FString Target;
		FString TargetRevision;
		TArray<FPcgNodeCandidate> Candidates;
		int32 TotalMatches = 0;
		int32 NextCursor = INDEX_NONE;
		bool bComplete = false;
		bool bHasMore = false;
	};

	struct UEAGENTINTERFACE_API FPcgNodeCandidateQueryError
	{
		FString Code;
		FString SubjectId;
		FString Detail;
	};

	UEAGENTINTERFACE_API bool TryQueryPcgNodeCandidates(
		UObject& Target,
		const FString& Query,
		FPcgNodeCandidateQueryResult& OutResult,
		FPcgNodeCandidateQueryError& OutError,
		int32 Limit = 64,
		int32 Cursor = 0);

	UEAGENTINTERFACE_API bool IsCurrentPcgNodeCandidateBinding(
		UObject& Target,
		const FString& CandidateName,
		const FString& CandidateKind,
		const TArray<FString>& SupportedOperations,
		const FString& InputContract,
		const FString& PlacementContract,
		const FString& RequiredContext,
		const FString& BindingRevision,
		FPcgNodeCandidateQueryError& OutError);
}
