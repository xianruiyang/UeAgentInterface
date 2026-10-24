#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonValue;

namespace UeAgentCurveJson
{
	// The public curve codec owns these float fields and chronological key order.
	// Other numeric fields, collections and identities retain their own semantics.
	void CollectNativeCurveComparisonTypes(const TSharedPtr<FJsonValue>& Value,
		const FString& Path, TMap<FString, FString>& Types);
	bool ProjectOriginalCurveKeyOrder(TSharedPtr<FJsonValue>& Desired,
		TMap<FString, FString>& SourcePaths, UE::UAI::Contract::FDiagnostic& Error);
}
