#pragma once

#include "CoreMinimal.h"

namespace UE::UAI::QueryOwners::AssetSimple
{
	struct UEAGENTINTERFACE_API FAssetFactoryCandidate
	{
		FString DisplayName;
		FString ClassPath;
		FString ProfileId;
		FString CandidateRef;
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
		int32 NextCursor = INDEX_NONE;
		bool bComplete = false;
		bool bHasMore = false;
	};

	struct UEAGENTINTERFACE_API FAssetFactoryCandidateQueryError
	{
		FString Code;
		FString SubjectId;
		FString Detail;
	};

	/**
	 * Projects factory_ref values from the process-owned factory catalog used by
	 * AssetSimple planning. Returned candidate_ref and catalog revision fields
	 * are opaque and directly consumable by the selected create command.
	 */
	UEAGENTINTERFACE_API bool TryQueryAssetFactoryCandidates(
		const FString& CreateCommandId,
		const FString& Query,
		int32 Limit,
		int32 Cursor,
		FAssetFactoryCandidateQueryResult& OutResult,
		FAssetFactoryCandidateQueryError& OutError);

	inline int32 GetAssetSimpleOwnedQueryCommandCount()
	{
		return 1;
	}
}
