#pragma once

#include "CoreMinimal.h"

class UWidgetBlueprint;

namespace UE::UAI::QueryOwners::UmgWidget
{
	struct UEAGENTINTERFACE_API FUmgWidgetCandidate
	{
		FString Id;
		FString Name;
		FString Label;
		FString Kind;
		FString ActionKind;
		FString Category;
		FString Summary;
		FString ClassPath;
		FString SourceOrigin;
		FString WriteSupport;
		FString SupportReason;
		bool bSupported = false;
	};

	struct UEAGENTINTERFACE_API FUmgWidgetCandidateQueryResult
	{
		FString Target;
		FString TargetRevision;
		TArray<FUmgWidgetCandidate> Candidates;
		int32 TotalMatches = 0;
		int32 NextCursor = INDEX_NONE;
		bool bComplete = false;
		bool bHasMore = false;
		FString CoverageScope;
		bool bCoverageComplete = false;
		bool bCreationCoverageComplete = false;
	};

	struct UEAGENTINTERFACE_API FUmgWidgetCandidateQueryError
	{
		FString Code;
		FString SubjectId;
		FString Detail;
	};

	UEAGENTINTERFACE_API bool TryQueryUmgWidgetCandidates(
		UWidgetBlueprint& Target,
		const FString& Query,
		FUmgWidgetCandidateQueryResult& OutResult,
		FUmgWidgetCandidateQueryError& OutError,
		int32 Limit = 64,
		int32 Cursor = 0,
		const FString& CandidateId = FString());

	/** Re-enumerate the same target-bound partial catalog and resolve one selector. */
	UEAGENTINTERFACE_API bool TryResolveUmgWidgetCandidate(
		UWidgetBlueprint& Target,
		const FString& Name,
		const FString& Id,
		FUmgWidgetCandidate& OutCandidate,
		FUmgWidgetCandidateQueryError& OutError);

	UEAGENTINTERFACE_API bool IsReadOnlyContentGraphCommand(
		const FString& CommandId);
}
