#pragma once

#include "CoreMinimal.h"

class UObject;
class UBehaviorTree;
class UBehaviorTreeGraphNode;

namespace UE::UAI::QueryOwners::AiBehavior
{
	struct UEAGENTINTERFACE_API FAiBehaviorCandidate
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
		FString PayloadType;
		bool bSupported = false;
		TArray<FString> SupportedOperations;

		// Legacy compatibility fields for AI contexts not yet migrated to a
		// native creation surface. New BehaviorTree candidates do not require
		// clients to copy these fields.
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
		bool bCoverageComplete = false;
		bool bCreationCoverageComplete = false;
		int32 NativeActionCount = 0;
		int32 ExecutableActionCount = 0;
		FString CoverageScope;
		FString GraphName;
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
		int32 Cursor = 0,
		const FString& CandidateId = FString());

	/** Re-enumerate the native BehaviorTree blank-background menu. */
	UEAGENTINTERFACE_API bool TryResolveAiBehaviorCandidate(
		UObject& Target,
		const FString& Name,
		const FString& Id,
		FAiBehaviorCandidate& OutCandidate,
		FAiBehaviorCandidateQueryError& OutError);

	/** Re-enumerate and invoke the same native BehaviorTree schema action. */
	UEAGENTINTERFACE_API bool TryPerformBehaviorTreeCandidate(
		UBehaviorTree& Target,
		const FString& Name,
		const FString& Id,
		const FVector2f& Location,
		UBehaviorTreeGraphNode*& OutNode,
		FAiBehaviorCandidate& OutCandidate,
		FAiBehaviorCandidateQueryError& OutError);

	UEAGENTINTERFACE_API bool IsCurrentAiBehaviorCandidateBinding(
		UObject& Target,
		const FString& CandidateName,
		const FString& CandidateKind,
		const TArray<FString>& SupportedOperations,
		const FString& InputContract,
		const FString& PlacementContract,
		const FString& RequiredContext,
		const FString& BindingRevision,
		FAiBehaviorCandidateQueryError& OutError);
}
