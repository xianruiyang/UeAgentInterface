#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

namespace UE::UAI::ContentDiscovery { class FReadOnlyCandidateBatch; }

class UObject;
class UNiagaraNodeFunctionCall;

namespace UE::UAI::QueryOwners::Niagara
{
	/** Exact Niagara stack context selected outside the candidate itself. */
	struct UEAGENTINTERFACE_API FNiagaraModuleCandidateContext
	{
		FString TargetUsage;
		FString EmitterName;
		FString StageName;
		// These are server-resolved context only; never read from public HJSON.
		FString TargetUsageId;
		FString EmitterHandleId;
		FString StageKey;

		// Input owners provide real locations; generated native contexts keep native provenance.
		UE::UAI::Contract::FSourceLocation Source;
		UE::UAI::Contract::FSourceLocation TargetUsageSource;
		FString SourceKind = TEXT("native_result");
	};

	struct UEAGENTINTERFACE_API FNiagaraModuleCandidate
	{
		FString NativeCandidateName;
		/** Internal native identity shared by query projection and action resolution. */
		FString NativeIdentity;
		FString Name;
		FString Label;
		FString Kind;
		FString ActionKind;
		FString Category;
		/** Internal readable semantic scopes used to disambiguate duplicate names. */
		TArray<FString> Scopes;
		TMap<FString, FString> SelectionFields;
		bool bSupported = false;
		FString SupportReason;
		UE::UAI::Contract::FDiagnostic SupportDiagnostic;
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
		UE::UAI::Contract::FDiagnostic CatalogDiagnostic;
		FNiagaraModuleCandidateContext Context;
	};

	using FNiagaraModuleCandidateQueryError = UE::UAI::Contract::FDiagnostic;

	UEAGENTINTERFACE_API bool TryQueryNiagaraModuleCandidates(
		UObject& Target,
		const FString& Query,
		const FNiagaraModuleCandidateContext& Context,
		FNiagaraModuleCandidateQueryResult& OutResult,
		FNiagaraModuleCandidateQueryError& OutError);

	/** Read-only selection reports both creatable and search-only native actions. */
	UEAGENTINTERFACE_API bool TrySelectNiagaraModuleCandidate(
		UObject& Target,
		const FString& CandidateName,
		const FNiagaraModuleCandidateContext& Context,
		FNiagaraModuleCandidate& OutCandidate,
		FNiagaraModuleCandidateContext& OutResolvedContext,
		FNiagaraModuleCandidateQueryError& OutError,
		ContentDiscovery::FReadOnlyCandidateBatch* Batch = nullptr,
		const TMap<FString, FString>& Select = {});

	/**
	 * Re-enumerates the native asset-module actions in the same resolved stack
	 * context and resolves the readable name and semantic select conjunctively.
	 */
	UEAGENTINTERFACE_API bool TryResolveNiagaraModuleCandidate(
		UObject& Target,
		const FString& CandidateName,
		const FNiagaraModuleCandidateContext& Context,
		FNiagaraModuleCandidate& OutCandidate,
		FNiagaraModuleCandidateContext& OutResolvedContext,
		FNiagaraModuleCandidateQueryError& OutError,
		ContentDiscovery::FReadOnlyCandidateBatch* Batch = nullptr,
		const TMap<FString, FString>& Select = {});

	/** Re-enumerates and executes the exact supported native Stack action. */
	UEAGENTINTERFACE_API bool TryPerformNiagaraModuleCandidate(
		UObject& Target,
		const FString& CandidateName,
		const FNiagaraModuleCandidateContext& Context,
		int32 TargetIndex,
		FNiagaraModuleCandidate& OutCandidate,
		FNiagaraModuleCandidateContext& OutResolvedContext,
		UNiagaraNodeFunctionCall*& OutCreatedNode,
		FNiagaraModuleCandidateQueryError& OutError,
		const TMap<FString, FString>& Select = {});
}
