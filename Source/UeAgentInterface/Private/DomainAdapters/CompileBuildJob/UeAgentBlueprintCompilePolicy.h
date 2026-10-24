#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"

class FJsonObject;
class UBlueprint;

namespace UE::UAI::DomainAdapters::CompileBuildJob
{
	struct FBlueprintCompilePolicyRequest final
	{
		FString AssetPath;
		FString SeverityFilter = TEXT("all");
		bool bIncludeMessages = false;
		bool bSaveAfterCompile = false;
	};

	bool TryDecodeBlueprintCompilePolicyRequest(
		const TSharedPtr<FJsonObject>& Parameters,
		FBlueprintCompilePolicyRequest& OutRequest,
		UE::UAI::Contract::FDiagnostic& OutError);

	bool TryExecuteBlueprintCompilePolicy(
		UBlueprint& Blueprint,
		const FBlueprintCompilePolicyRequest& Request,
		TSharedPtr<FJsonObject>& OutReport,
		UE::UAI::Contract::FDiagnostic& OutError);
}
