#pragma once

#include "CoreMinimal.h"

class UObject;

namespace UE::UAI::QueryOwners::Material
{
	struct UEAGENTINTERFACE_API FMaterialExpressionCandidateContext
	{
		FString GraphName;
		FString NodeGuid;
		FString PinGuid;
		FString PinName;
	};

	struct UEAGENTINTERFACE_API FMaterialExpressionCandidate
	{
		FString Id;
		FString Name;
		FString Label;
		FString Kind;
		FString Category;
		bool bSupported = false;
		FString SupportReason;
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
		bool bCoverageComplete = false;
		FString CoverageScope;
		FMaterialExpressionCandidateContext Context;
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
		int32 Cursor = 0,
		const FMaterialExpressionCandidateContext& Context = {});

	UEAGENTINTERFACE_API bool IsCurrentMaterialExpressionCandidateBinding(
		UObject& Target,
		const FString& CandidateName,
		const FString& BindingRevision,
		FMaterialExpressionCandidateQueryError& OutError);
}
