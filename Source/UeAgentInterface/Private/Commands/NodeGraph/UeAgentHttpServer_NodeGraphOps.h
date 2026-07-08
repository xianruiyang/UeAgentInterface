// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

class UBlueprint;
class UEdGraph;
class UObject;

namespace UeAgentNodeGraphOps
{
	struct FGraphRef
	{
		FGraphRef();

		UEdGraph* Graph;
		FString GraphKind;
		bool bSupported;
		FString UnsupportedReason;
	};

	UObject* LoadAssetObject(const FString& InPath);
	bool TryGetObjectField(const TSharedPtr<FJsonObject>& Obj, const FString& Field, TSharedPtr<FJsonObject>& OutObj);
	bool HasRemovedDryRunParameter(const TSharedPtr<FJsonObject>& Params);
	FString AdapterForAsset(const UObject* Asset);
	TSharedPtr<FJsonObject> GraphRefToJson(const FGraphRef& GraphRef);
	TArray<FGraphRef> CollectBlueprintGraphs(UBlueprint* Blueprint, bool bIncludeNestedGraphs);
	TArray<FGraphRef> CollectGenericEdGraphs(UObject* Asset);
	void AddMaterialGraphSummary(UObject* Asset, TArray<TSharedPtr<FJsonValue>>& Graphs);
	TSharedPtr<FJsonObject> BuildBlueprintLayoutParams(const TSharedPtr<FJsonObject>& Params);
	bool LayoutMaterialExpressionGraph(UObject* Asset, const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, FString& OutError);
	bool LayoutGenericEdGraph(UObject* Asset, const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, FString& OutError);
}
