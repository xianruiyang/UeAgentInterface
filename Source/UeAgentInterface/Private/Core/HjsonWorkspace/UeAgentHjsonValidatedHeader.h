#pragma once

#include "CoreMinimal.h"
#include "Core/ContextResource/UeAgentContextResource.h"

namespace UE::UAI::StateSemantic { class FUeAgentCanonicalValue; }

namespace UE::UAI::HjsonWorkspace
{
	class FHjsonDocument;
	struct FHjsonRootHeaderFacts;
	using FHjsonRootHeaderFactsPtr = TSharedPtr<const FHjsonRootHeaderFacts>;

	// Internal, bounded root-header facts from the parser or original formatter.
	// Callers supply the SHA-256 of actual bytes; no source version or semantic
	// validation is implied. Only root strings are retained, never the full AST.
	void RememberParsedHjsonRootHeader(const FString& ContentHash, const FHjsonDocument& Document,
		const TMap<ContextResource::EUeAgentResourceDimension, int64>& ParseCharges);
	/** Bind the original formatter's root fields to the digest it just produced;
	 * no parser/AST, source-currentness or semantic authority is inferred. */
	void RememberFormattedHjsonRootHeader(const FString& ContentHash, const StateSemantic::FUeAgentCanonicalValue& Root);
	/** Seal the existing formatter/parser's small facts with its actual bytes.
	 * This does not keep a document, model or native binding alive. */
	FHjsonRootHeaderFactsPtr FindHjsonRootHeaderFacts(const FString& ContentHash);
	bool TryReadHjsonRootString(const FHjsonRootHeaderFactsPtr& Facts, const FString& ContentHash,
		const FString& Field, FString& OutValue,
		TMap<ContextResource::EUeAgentResourceDimension, int64>* OutParseCharges = nullptr);
	bool TryReuseHjsonRootString(const FString& ContentHash, const FString& Field, FString& OutValue,
		TMap<ContextResource::EUeAgentResourceDimension, int64>* OutParseCharges = nullptr);
}
