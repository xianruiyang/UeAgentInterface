#pragma once

#include "CoreMinimal.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

class FJsonObject;
class FJsonValue;

namespace UE::UAI::HjsonWorkspace
{
	/** Pure ordinary UObject property rows. Native capture supplies schema; no class selection or document parsing. */
	struct FReadablePropertySchema
	{
		FString Name;
		FString Category;
		FString Type;
		FString Definition;
	};

	struct FResolvedReadableProperty
	{
		int32 SchemaIndex = INDEX_NONE;
		StateSemantic::FUeAgentCanonicalValuePtr Value;
	};

	UEAGENTINTERFACE_API FString ReadablePropertyCategory(const TArray<FString>& CategoryPath);
	UEAGENTINTERFACE_API FString ShortReadableTypeName(FString Type);
	UEAGENTINTERFACE_API FString ReadablePropertyTypeName(const StateSemantic::FUeAgentCanonicalValue& Type);
	UEAGENTINTERFACE_API bool TryDescribeReadableProperty(
		const StateSemantic::FUeAgentCanonicalValue& Descriptor, const StateSemantic::FUeAgentCanonicalValue& Type,
		FReadablePropertySchema& OutSchema);
	UEAGENTINTERFACE_API bool TryReadReadablePropertyRows(
		const StateSemantic::FUeAgentCanonicalValue& Categories,
		const TArray<FReadablePropertySchema>& Schema,
		TArray<FResolvedReadableProperty>& OutRows,
		StateSemantic::FUeAgentStateSemanticError& OutError);
	UEAGENTINTERFACE_API bool TryAddReadablePropertyRow(
		const TSharedRef<FJsonObject>& Categories, const FReadablePropertySchema& Schema,
		const TSharedPtr<FJsonValue>& Value);
}
