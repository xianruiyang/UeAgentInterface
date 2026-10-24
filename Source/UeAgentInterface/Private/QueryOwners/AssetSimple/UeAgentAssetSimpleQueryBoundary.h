#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

namespace UE::UAI::QueryOwners::AssetSimple
{
	struct UEAGENTINTERFACE_API FAssetFactoryCandidate
	{
		FString DisplayName;
		FString ClassPath;
		FString ProfileId;
		FString CandidateRef;
		FString Name;
		FString Id;
		FString SupportedClassPath;
		FString WriteStatus;
		bool bGenericReady = false;
		FString CatalogSourceId;
		FString CatalogGeneration;
		int64 CatalogSequence = -1;
	};

	struct UEAGENTINTERFACE_API FAssetFactoryCandidateQueryResult
	{
		FString CreateCommandId;
		FString ProfileId;
		FString CatalogSourceId;
		FString CatalogGeneration;
		int64 CatalogSequence = -1;
		TArray<FAssetFactoryCandidate> Candidates;
		int32 TotalMatches = 0;
	};

	using FAssetFactoryCandidateQueryError = UE::UAI::Contract::FDiagnostic;

	/**
	 * Projects factory_ref values from the process-owned factory catalog used by
	 * AssetSimple planning. Returned candidate_ref and catalog revision fields
	 * are opaque and directly consumable by the selected create command.
	 */
	UEAGENTINTERFACE_API bool TryQueryAssetFactoryCandidates(
		const FString& CreateCommandId,
		const FString& Query,
		FAssetFactoryCandidateQueryResult& OutResult,
		FAssetFactoryCandidateQueryError& OutError);
}
