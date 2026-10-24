// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonValue;
class UMaterialInterface;
class UObject;

namespace UeAgentMaterialAuthoring
{
	enum class ECompileMessageSeverityFilter : uint8
	{
		All,
		Error,
		Warning,
		Info,
		WarningOrError
	};

	bool ParseCompileSeverityFilter(const FString& InFilterText, ECompileMessageSeverityFilter& OutFilter);
	void CollectMaterialCompileMessages(
		UMaterialInterface* MaterialInterface,
		ECompileMessageSeverityFilter SeverityFilter,
		TArray<TSharedPtr<FJsonValue>>& OutMessages,
		int32& OutErrorCount);
	bool SaveAssetPackage(UObject* Asset, UE::UAI::Contract::FDiagnostic& OutError);
}
