// Copyright Epic Games, Inc. All Rights Reserved.

#include "LegacyMechanics/UeAgentLegacyMechanics.h"
#include "UeAgentHttpServer_NodeGraphOps.h"

#include "ControlRigBlueprint.h"
#include "Engine/Blueprint.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunctionInterface.h"

bool FUeAgentLegacyMechanics::CmdNodeGraphList(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentNodeGraphOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}

	FString RequestedAdapter = TEXT("auto");
	JsonTryGetString(Ctx.Params, TEXT("adapter"), RequestedAdapter);
	const FString Adapter = UeAgentNodeGraphOps::AdapterForAsset(Asset);

	bool bIncludeNestedGraphs = false;
	JsonTryGetBool(Ctx.Params, TEXT("include_nested_graphs"), bIncludeNestedGraphs);
	TSharedPtr<FJsonObject> Selector;
	if (UeAgentNodeGraphOps::TryGetObjectField(Ctx.Params, TEXT("graph_selector"), Selector))
	{
		Selector->TryGetBoolField(TEXT("include_nested_graphs"), bIncludeNestedGraphs);
	}

	TArray<TSharedPtr<FJsonValue>> GraphsJson;
	if (Asset->IsA<UControlRigBlueprint>())
	{
		const TArray<UeAgentNodeGraphOps::FGraphRef> Graphs = UeAgentNodeGraphOps::CollectGenericEdGraphs(Asset);
		for (const UeAgentNodeGraphOps::FGraphRef& Graph : Graphs)
		{
			GraphsJson.Add(MakeShared<FJsonValueObject>(UeAgentNodeGraphOps::GraphRefToJson(Graph)));
		}
	}
	else if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
	{
		const TArray<UeAgentNodeGraphOps::FGraphRef> Graphs = UeAgentNodeGraphOps::CollectBlueprintGraphs(Blueprint, bIncludeNestedGraphs);
		for (const UeAgentNodeGraphOps::FGraphRef& Graph : Graphs)
		{
			GraphsJson.Add(MakeShared<FJsonValueObject>(UeAgentNodeGraphOps::GraphRefToJson(Graph)));
		}
	}
	else if (Asset->IsA<UMaterial>() || Asset->IsA<UMaterialFunctionInterface>())
	{
		UeAgentNodeGraphOps::AddMaterialGraphSummary(Asset, GraphsJson);
	}
	else
	{
		const TArray<UeAgentNodeGraphOps::FGraphRef> Graphs = UeAgentNodeGraphOps::CollectGenericEdGraphs(Asset);
		for (const UeAgentNodeGraphOps::FGraphRef& Graph : Graphs)
		{
			GraphsJson.Add(MakeShared<FJsonValueObject>(UeAgentNodeGraphOps::GraphRefToJson(Graph)));
		}
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("asset_class"), Asset->GetClass()->GetPathName());
	OutData->SetStringField(TEXT("requested_adapter"), RequestedAdapter);
	OutData->SetStringField(TEXT("adapter"), Adapter);
	OutData->SetArrayField(TEXT("graphs"), GraphsJson);
	OutData->SetNumberField(TEXT("graph_count"), GraphsJson.Num());
	return true;
}

bool FUeAgentLegacyMechanics::CmdNodeGraphLayout(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (UeAgentNodeGraphOps::HasRemovedDryRunParameter(Ctx.Params))
	{
		OutError = TEXT("dry_run_removed_node_graph_layout_always_writes_use_undo_to_revert");
		return false;
	}

	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentNodeGraphOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}

	const FString Adapter = UeAgentNodeGraphOps::AdapterForAsset(Asset);
	if (Adapter == TEXT("generic_edgraph"))
	{
		return UeAgentNodeGraphOps::LayoutGenericEdGraph(Asset, Ctx.Params, OutData, OutError);
	}

	if (Asset->IsA<UBlueprint>())
	{
		TSharedPtr<FJsonObject> LegacyParams = UeAgentNodeGraphOps::BuildBlueprintLayoutParams(Ctx.Params);
		FUeAgentRequestContext LegacyCtx;
		LegacyCtx.RequestId = Ctx.RequestId + TEXT(":blueprint_layout_all_graphs");
		LegacyCtx.Command = TEXT("blueprint_layout_all_graphs");
		LegacyCtx.Params = LegacyParams;

		TSharedPtr<FJsonObject> LegacyData;
		FString AuthoringError;
		if (!ExecuteBlueprintCommand(TEXT("blueprint_layout_all_graphs"), LegacyCtx, LegacyData, AuthoringError))
		{
			OutData = LegacyData.IsValid() ? LegacyData : MakeShared<FJsonObject>();
			OutData->SetStringField(TEXT("adapter"), Adapter);
			OutData->SetStringField(TEXT("delegated_command"), TEXT("blueprint_layout_all_graphs"));
			OutError = AuthoringError;
			return false;
		}

		OutData = LegacyData.IsValid() ? LegacyData : MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("adapter"), Adapter);
		OutData->SetStringField(TEXT("delegated_command"), TEXT("blueprint_layout_all_graphs"));
		OutData->SetStringField(TEXT("node_graph_layout_strategy"), TEXT("delegated_blueprint_layout_kernel"));
		return true;
	}

	if (Asset->IsA<UMaterial>() || Asset->IsA<UMaterialFunctionInterface>())
	{
		return UeAgentNodeGraphOps::LayoutMaterialExpressionGraph(Asset, Ctx.Params, OutData, OutError);
	}

	return UeAgentNodeGraphOps::LayoutGenericEdGraph(Asset, Ctx.Params, OutData, OutError);
}
