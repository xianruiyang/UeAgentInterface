#pragma once

#include "CoreMinimal.h"

class UObject;

namespace UE::UAI::QueryOwners::Material
{
	struct UEAGENTINTERFACE_API FMaterialExpressionCandidate
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

	struct UEAGENTINTERFACE_API FMaterialExpressionCandidateQueryResult
	{
		FString Target;
		FString TargetRevision;
		TArray<FMaterialExpressionCandidate> Candidates;
		int32 TotalMatches = 0;
		int32 NextCursor = INDEX_NONE;
		bool bComplete = false;
		bool bHasMore = false;
	};

	struct UEAGENTINTERFACE_API FMaterialExpressionCandidateQueryError
	{
		FString Code;
		FString SubjectId;
		FString Detail;
	};

	UEAGENTINTERFACE_API bool TryQueryMaterialExpressionCandidates(
		UObject& Target,
		const FString& Query,
		FMaterialExpressionCandidateQueryResult& OutResult,
		FMaterialExpressionCandidateQueryError& OutError,
		int32 Limit = 20,
		int32 Cursor = 0);

	UEAGENTINTERFACE_API bool IsCurrentMaterialExpressionCandidateBinding(
		UObject& Target,
		const FString& CandidateName,
		const FString& BindingRevision,
		FMaterialExpressionCandidateQueryError& OutError);
}
