// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UClass;
class UMaterialFunctionInterface;

namespace UeAgentMaterialAuthoring
{
	struct UEAGENTINTERFACE_API FExpressionResolveRequest
	{
		FString ExpressionClassPath;
		FString Path;
		FString CatalogName;
		FString MaterialFunctionPath;
		FString ContextAssetPath;
		FString ContextGraphName;
	};

	struct UEAGENTINTERFACE_API FExpressionResolveResult
	{
		UClass* ExpressionClass = nullptr;
		UMaterialFunctionInterface* MaterialFunction = nullptr;
		FString ExpressionClassPath;
		FString MaterialFunctionPath;
	};

	UEAGENTINTERFACE_API UClass* ResolveExpressionClassPath(const FString& ClassPath);
	UEAGENTINTERFACE_API UMaterialFunctionInterface* ResolveMaterialFunctionPath(const FString& FunctionPath);
	UEAGENTINTERFACE_API bool ResolveExpression(
		const FExpressionResolveRequest& Request,
		FExpressionResolveResult& OutResult,
		FString& OutError);
}
