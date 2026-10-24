#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

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
		TArray<FString> Scopes;
		FString Kind;
		FString Category;
		bool bSupported = false;
		FString SupportReason;
		UE::UAI::Contract::FDiagnostic SupportDiagnostic;
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
		UE::UAI::Contract::FDiagnostic CatalogDiagnostic;
		FMaterialExpressionCandidateContext Context;
	};

	using FMaterialExpressionCandidateQueryError = UE::UAI::Contract::FDiagnostic;

	UEAGENTINTERFACE_API bool TryQueryMaterialExpressionCandidates(
		UObject& Target,
		const FString& Query,
		FMaterialExpressionCandidateQueryResult& OutResult,
		FMaterialExpressionCandidateQueryError& OutError,
		const FMaterialExpressionCandidateContext& Context = {});

}
