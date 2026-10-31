#pragma once

#include "CoreMinimal.h"
#include "Core/ContextResource/UeAgentContextResource.h"

namespace UE::UAI::HjsonWorkspace
{
	class FHjsonDocument;

	// Internal, bounded syntax proof for a previously parsed entry document.
	// Callers supply the SHA-256 of actual bytes; no source version or semantic
	// validation is implied. Only root strings are retained, never the full AST.
	void RememberParsedHjsonRootHeader(const FString& ContentHash, const FHjsonDocument& Document,
		const TMap<ContextResource::EUeAgentResourceDimension, int64>& ParseCharges);
	bool TryReuseParsedHjsonRootString(const FString& ContentHash, const FString& Field, FString& OutValue,
		TMap<ContextResource::EUeAgentResourceDimension, int64>& OutParseCharges);
}
