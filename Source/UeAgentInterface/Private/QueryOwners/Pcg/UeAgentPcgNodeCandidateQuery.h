#pragma once

#include "CoreMinimal.h"

class UObject;
class UPCGGraph;
class UPCGNode;

namespace UE::UAI::QueryOwners::Pcg
{
	struct UEAGENTINTERFACE_API FPcgNodeCandidate
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
		bool bCoverageComplete = false;
		bool bCreationCoverageComplete = false;
		int32 NativeActionCount = 0;
		int32 ExecutableActionCount = 0;
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
		int32 Cursor = 0,
		const FString& CandidateId = FString());

	/** Re-enumerate the exact native PCG graph context and resolve one selector. */
	UEAGENTINTERFACE_API bool TryResolvePcgNodeCandidate(
		UPCGGraph& Target,
		const FString& Name,
		const FString& Id,
		FPcgNodeCandidate& OutCandidate,
		FPcgNodeCandidateQueryError& OutError);

	/** Re-enumerate and invoke the selected native PCG schema action. */
	UEAGENTINTERFACE_API bool TryPerformPcgNodeCandidate(
		UPCGGraph& Target,
		const FString& Name,
		const FString& Id,
		const FVector2f& Location,
		UPCGNode*& OutNode,
		FPcgNodeCandidate& OutCandidate,
		FPcgNodeCandidateQueryError& OutError);
}
