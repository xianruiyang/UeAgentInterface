#pragma once

#include "CoreMinimal.h"

class UObject;
class UNiagaraNodeFunctionCall;

namespace UE::UAI::QueryOwners::Niagara
{
	/** Exact Niagara stack context selected outside the candidate itself. */
	struct UEAGENTINTERFACE_API FNiagaraModuleCandidateContext
	{
		FString TargetUsage;
		FString TargetUsageId;
		FString EmitterHandleId;
		FString StageKey;
	};

	struct UEAGENTINTERFACE_API FNiagaraModuleCandidate
	{
		FString Id;
		FString Name;
		FString Label;
		FString Kind;
		FString ActionKind;
		FString Category;
		bool bSupported = false;
		FString SupportReason;
		/** Server-side action payload. This field is not serialized publicly. */
		FString ScriptAssetPath;
		TArray<FString> SupportedOperations;
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
		bool bCoverageComplete = false;
		bool bCreationCoverageComplete = false;
		FString CoverageScope;
		TArray<FString> UnsupportedActionKinds;
		FNiagaraModuleCandidateContext Context;
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
		const FNiagaraModuleCandidateContext& Context,
		FNiagaraModuleCandidateQueryResult& OutResult,
		FNiagaraModuleCandidateQueryError& OutError,
		int32 Limit = 20,
		int32 Cursor = 0);

	/**
	 * Re-enumerates the native asset-module actions in the same resolved stack
	 * context and resolves the minimal name/id selector conjunctively.
	 */
	UEAGENTINTERFACE_API bool TryResolveNiagaraModuleCandidate(
		UObject& Target,
		const FString& CandidateName,
		const FString& CandidateId,
		const FNiagaraModuleCandidateContext& Context,
		FNiagaraModuleCandidate& OutCandidate,
		FNiagaraModuleCandidateContext& OutResolvedContext,
		FNiagaraModuleCandidateQueryError& OutError);

	/** Re-enumerates and executes the exact supported native Stack action. */
	UEAGENTINTERFACE_API bool TryPerformNiagaraModuleCandidate(
		UObject& Target,
		const FString& CandidateName,
		const FString& CandidateId,
		const FNiagaraModuleCandidateContext& Context,
		int32 TargetIndex,
		FNiagaraModuleCandidate& OutCandidate,
		FNiagaraModuleCandidateContext& OutResolvedContext,
		UNiagaraNodeFunctionCall*& OutCreatedNode,
		FNiagaraModuleCandidateQueryError& OutError);
}
