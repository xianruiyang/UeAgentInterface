// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"

namespace UeAgentAuthoringTargetLocator
{
	struct FResolveResult;
}

namespace UeAgentAuthoringCatalogCandidates
{
	enum class ESortMode : uint8
	{
		UiOrder,
		Score,
		Name,
		Category
	};

	struct UEAGENTINTERFACE_API FIdentityInput
	{
		FString CatalogType;
		FString TargetKind;
		FString Name;
		FString DisplayName;
		FString FullName;
		FString StableId;
		FString SearchKey;
		FString ObjectPath;
		FString PackagePath;
		TMap<FString, FString> AuthoringSelectors;
	};

	struct UEAGENTINTERFACE_API FSortKey
	{
		FString CanonicalKey;
		FString Name;
		FString CategoryPath;
		int32 UiOrder = 0;
		double Score = 0.0;
	};

	UEAGENTINTERFACE_API bool BuildIdentity(
		const FIdentityInput& Input,
		UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
		UE::UAI::Contract::FDiagnostic& OutError);

	/**
	 * Builds the compact, model-facing candidate id from the canonical internal
	 * identity. The full canonical key remains the server-side authority.
	 */
	UEAGENTINTERFACE_API FString BuildPublicId(
		const UeAgentAuthoringIdentity::FCanonicalIdentity& Identity);

	/**
	 * Resolves one catalog candidate by the same compact id returned to callers,
	 * a canonical identity key, or an exact model-facing name. Name and id may be
	 * supplied together to make the selection stricter.
	 */
	UEAGENTINTERFACE_API bool ResolveExactCandidate(
		const FString& Name,
		const FString& Id,
		const TArray<UeAgentAuthoringIdentity::FCanonicalIdentity>& Identities,
		UeAgentAuthoringTargetLocator::FResolveResult& OutResult,
		int32 MaxDiagnosticCandidates = -1);

	UEAGENTINTERFACE_API bool Less(const FSortKey& Left, const FSortKey& Right, ESortMode Mode);
	UEAGENTINTERFACE_API void Sort(TArray<FSortKey>& Keys, ESortMode Mode);
}
