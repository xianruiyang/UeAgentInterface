#pragma once

#include "CoreMinimal.h"

class UObject;

namespace UE::UAI::QueryOwners::AudioGraph
{
	struct UEAGENTINTERFACE_API FAudioGraphCandidate
	{
		FString TargetRef;
		FString TargetRevision;
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
		FString CoverageScope;
		bool bCoverageComplete = false;
		bool bCreationCoverageComplete = false;
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
		int32 Cursor = 0,
		const FString& CandidateId = FString());

	/** Re-enumerate the current MetaSound frontend registry and resolve one selector. */
	UEAGENTINTERFACE_API bool TryResolveAudioGraphCandidate(
		UObject& Target,
		const FString& Name,
		const FString& Id,
		FAudioGraphCandidate& OutCandidate,
		FAudioGraphCandidateQueryError& OutError);

	UEAGENTINTERFACE_API bool IsCurrentAudioGraphCandidateBinding(
		UObject& Target,
		const FString& CandidateTargetRef,
		const FString& CandidateTargetRevision,
		const FString& CandidateName,
		const FString& CandidateKind,
		const TArray<FString>& SupportedOperations,
		const FString& InputContract,
		const FString& PlacementContract,
		const FString& RequiredContext,
		int32 MajorVersion,
		int32 MinorVersion,
		const FString& BindingRevision,
		FAudioGraphCandidateQueryError& OutError);
}
