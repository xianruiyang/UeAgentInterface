#pragma once

#include "CoreMinimal.h"

class UObject;

namespace UE::UAI::QueryOwners::Niagara
{
	struct UEAGENTINTERFACE_API FNiagaraModuleCandidate
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

	struct UEAGENTINTERFACE_API FNiagaraModuleCandidateQueryResult
	{
		FString Target;
		FString TargetRevision;
		TArray<FNiagaraModuleCandidate> Candidates;
		int32 TotalMatches = 0;
		int32 NextCursor = INDEX_NONE;
		bool bComplete = false;
		bool bHasMore = false;
	};

	struct UEAGENTINTERFACE_API FNiagaraModuleCandidateQueryError
	{
		FString Code;
		FString SubjectId;
		FString Detail;
	};

	UEAGENTINTERFACE_API bool TryQueryNiagaraModuleCandidates(
		UObject& Target,
		const FString& Query,
		const FString& TargetUsage,
		FNiagaraModuleCandidateQueryResult& OutResult,
		FNiagaraModuleCandidateQueryError& OutError,
		int32 Limit = 20,
		int32 Cursor = 0);

	UEAGENTINTERFACE_API bool IsCurrentNiagaraModuleCandidateBinding(
		UObject& Target,
		const FString& CandidateName,
		const FString& TargetUsage,
		const FString& BindingRevision,
		FNiagaraModuleCandidateQueryError& OutError);

	UEAGENTINTERFACE_API bool IsCurrentNiagaraModuleCandidateBinding(
		UObject& Target,
		const FString& CandidateName,
		const FString& CandidateKind,
		const TArray<FString>& SupportedOperations,
		const FString& InputContract,
		const FString& PlacementContract,
		const FString& RequiredContext,
		const FString& TargetUsage,
		const FString& BindingRevision,
		FNiagaraModuleCandidateQueryError& OutError);
}
