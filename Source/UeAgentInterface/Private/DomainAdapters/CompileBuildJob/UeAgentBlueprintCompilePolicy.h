#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UBlueprint;

namespace UE::UAI::DomainAdapters::CompileBuildJob
{
	struct FBlueprintCompilePolicyRequest final
	{
		FString AssetPath;
		FString SeverityFilter = TEXT("all");
		int32 MaxMessages = 200;
		bool bIncludeMessages = false;
		bool bSaveAfterCompile = false;
	};

	bool TryDecodeBlueprintCompilePolicyRequest(
		const TSharedPtr<FJsonObject>& Parameters,
		FBlueprintCompilePolicyRequest& OutRequest,
		FString& OutErrorCode,
		FString& OutErrorDetail);

	bool TryExecuteBlueprintCompilePolicy(
		UBlueprint& Blueprint,
		const FBlueprintCompilePolicyRequest& Request,
		TSharedPtr<FJsonObject>& OutReport,
		FString& OutErrorCode,
		FString& OutErrorDetail);
}
