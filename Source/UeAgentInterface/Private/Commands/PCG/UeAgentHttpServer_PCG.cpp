// Copyright Epic Games, Inc. All Rights Reserved.

#include "LegacyMechanics/UeAgentLegacyMechanics.h"
#include "Domains/PCG/Authoring/UeAgentPCGAuthoringApplyProvider.h"
#include "Domains/PCG/Authoring/UeAgentPCGAuthoringExport.h"
#include "Domains/PCG/UeAgentPCGDomainBackend.h"

#include "Module/UeAgentInterfaceLogger.h"
#include "Utilities/UeAgentAuthoringFileIO.h"
#include "Utilities/UeAgentAuthoringFolderIO.h"
#include "Utilities/UeAgentAuthoringFolderStats.h"
#include "Utilities/UeAgentAuthoringJson.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/UeAgentStructuredAuthoringIO.h"
#include "Utilities/UeAgentUObjectProperties.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Async/TaskGraphInterfaces.h"
#include "EdGraph/EdGraph.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "PCGAssetExporter.h"
#include "PCGComponent.h"
#include "PCGDataAsset.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGGraphExecutionInspection.h"
#include "PCGLevelToAsset.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "PCGSubsystem.h"
#include "PCGWorldActor.h"
#include "Data/PCGBasePointData.h"
#include "Elements/PCGExecuteBlueprint.h"
#include "Elements/PCGStaticMeshSpawner.h"
#include "Elements/PCGUserParameterGet.h"
#include "Grid/PCGPartitionActor.h"
#include "MeshSelectors/PCGMeshSelectorBase.h"
#include "RHI.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "StructUtils/PropertyBag.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

bool FUeAgentLegacyMechanics::ExecutePCGCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	struct FEntry
	{
		const TCHAR* Name;
		bool (FUeAgentLegacyMechanics::*Method)(const FUeAgentRequestContext&, TSharedPtr<FJsonObject>&, FString&) const;
	};

	static const FEntry Entries[] = {
		{ TEXT("pcg_capabilities_query"), &FUeAgentLegacyMechanics::CmdPCGCapabilitiesQuery },
		{ TEXT("pcg_node_catalog_export"), &FUeAgentLegacyMechanics::CmdPCGNodeCatalogExport },
		{ TEXT("pcg_asset_info"), &FUeAgentLegacyMechanics::CmdPCGAssetInfo },
		{ TEXT("pcg_graph_export_folder"), &FUeAgentLegacyMechanics::CmdPCGGraphExportFolder },
		{ TEXT("pcg_graph_validate_folder"), &FUeAgentLegacyMechanics::CmdPCGGraphValidateFolder },
		{ TEXT("pcg_graph_plan_folder"), &FUeAgentLegacyMechanics::CmdPCGGraphPlanFolder },
		{ TEXT("pcg_graph_diff_folder"), &FUeAgentLegacyMechanics::CmdPCGGraphDiffFolder },
		{ TEXT("pcg_graph_apply_folder"), &FUeAgentLegacyMechanics::CmdPCGGraphApplyFolder },
		{ TEXT("pcg_graph_compile"), &FUeAgentLegacyMechanics::CmdPCGGraphCompile },
		{ TEXT("pcg_graph_snapshot"), &FUeAgentLegacyMechanics::CmdPCGGraphSnapshot },
		{ TEXT("pcg_graph_layout"), &FUeAgentLegacyMechanics::CmdPCGGraphLayout },
		{ TEXT("pcg_graph_instance_json"), &FUeAgentLegacyMechanics::CmdPCGGraphInstanceJson },
		{ TEXT("pcg_graph_parameters_query"), &FUeAgentLegacyMechanics::CmdPCGGraphParametersQuery },
		{ TEXT("pcg_graph_parameters_apply_json"), &FUeAgentLegacyMechanics::CmdPCGGraphParametersApplyJson },
		{ TEXT("pcg_component_query"), &FUeAgentLegacyMechanics::CmdPCGComponentQuery },
		{ TEXT("pcg_component_apply_json"), &FUeAgentLegacyMechanics::CmdPCGComponentApplyJson },
		{ TEXT("pcg_component_snapshot_json"), &FUeAgentLegacyMechanics::CmdPCGComponentSnapshotJson },
		{ TEXT("pcg_world_actor_query"), &FUeAgentLegacyMechanics::CmdPCGWorldActorQuery },
		{ TEXT("pcg_world_actor_apply_json"), &FUeAgentLegacyMechanics::CmdPCGWorldActorApplyJson },
		{ TEXT("pcg_partition_probe"), &FUeAgentLegacyMechanics::CmdPCGPartitionProbe },
		{ TEXT("pcg_generate"), &FUeAgentLegacyMechanics::CmdPCGGenerate },
		{ TEXT("pcg_cleanup"), &FUeAgentLegacyMechanics::CmdPCGCleanup },
		{ TEXT("pcg_regenerate"), &FUeAgentLegacyMechanics::CmdPCGRegenerate },
		{ TEXT("pcg_cancel_generation"), &FUeAgentLegacyMechanics::CmdPCGCancelGeneration },
		{ TEXT("pcg_generation_status"), &FUeAgentLegacyMechanics::CmdPCGGenerationStatus },
		{ TEXT("pcg_wait"), &FUeAgentLegacyMechanics::CmdPCGWait },
		{ TEXT("pcg_runtime_generation_probe"), &FUeAgentLegacyMechanics::CmdPCGRuntimeGenerationProbe },
		{ TEXT("pcg_inspection_set"), &FUeAgentLegacyMechanics::CmdPCGInspectionSet },
		{ TEXT("pcg_node_inspect"), &FUeAgentLegacyMechanics::CmdPCGNodeInspect },
		{ TEXT("pcg_data_export_json"), &FUeAgentLegacyMechanics::CmdPCGDataExportJson },
		{ TEXT("pcg_data_assert_json"), &FUeAgentLegacyMechanics::CmdPCGDataAssertJson },
		{ TEXT("pcg_component_runtime_probe"), &FUeAgentLegacyMechanics::CmdPCGComponentRuntimeProbe },
		{ TEXT("pcg_profile_report"), &FUeAgentLegacyMechanics::CmdPCGProfileReport },
		{ TEXT("pcg_gpu_capabilities_query"), &FUeAgentLegacyMechanics::CmdPCGGPUCapabilitiesQuery },
		{ TEXT("pcg_compute_graph_validate"), &FUeAgentLegacyMechanics::CmdPCGComputeGraphValidate },
		{ TEXT("pcg_hlsl_validate"), &FUeAgentLegacyMechanics::CmdPCGHLSLValidate },
		{ TEXT("pcg_data_asset_export_json"), &FUeAgentLegacyMechanics::CmdPCGDataAssetExportJson },
		{ TEXT("pcg_data_asset_apply_json"), &FUeAgentLegacyMechanics::CmdPCGDataAssetApplyJson },
		{ TEXT("pcg_level_to_asset_export"), &FUeAgentLegacyMechanics::CmdPCGLevelToAssetExport },
	};

	for (const FEntry& Entry : Entries)
	{
		if (CommandLower == Entry.Name)
		{
			return (this->*Entry.Method)(Ctx, OutData, OutError);
		}
	}

	OutError = FString::Printf(TEXT("unknown_pcg_command:%s"), *CommandLower);
	return false;
}

bool FUeAgentLegacyMechanics::CmdPCGCapabilitiesQuery(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), SchemaCapabilities);
	OutData->SetBoolField(TEXT("pcg_module_loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("PCG")));
	OutData->SetBoolField(TEXT("pcg_editor_module_loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("PCGEditor")));
	OutData->SetBoolField(TEXT("compute_framework_loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("ComputeFramework")));
	OutData->SetBoolField(TEXT("supports_graph_folder_workflow"), true);
	OutData->SetBoolField(TEXT("supports_component_generation"), true);
	OutData->SetBoolField(TEXT("supports_execution_inspection"), true);
	OutData->SetBoolField(TEXT("supports_level_to_asset"), true);
	OutData->SetBoolField(TEXT("supports_viewport_screenshot_bridge"), true);
	OutData->SetStringField(TEXT("rhi"), GDynamicRHI ? GDynamicRHI->GetName() : FString());

	TArray<TSharedPtr<FJsonValue>> Commands;
	static const TCHAR* CommandNames[] = {
		TEXT("pcg_capabilities_query"), TEXT("pcg_node_catalog_export"), TEXT("pcg_asset_info"),
		TEXT("pcg_graph_export_folder"), TEXT("pcg_graph_validate_folder"), TEXT("pcg_graph_plan_folder"),
		TEXT("pcg_graph_diff_folder"), TEXT("pcg_graph_apply_folder"), TEXT("pcg_graph_compile"),
		TEXT("pcg_graph_snapshot"), TEXT("pcg_graph_layout"), TEXT("pcg_graph_instance_json"), TEXT("pcg_graph_parameters_query"),
		TEXT("pcg_graph_parameters_apply_json"), TEXT("pcg_component_query"), TEXT("pcg_component_apply_json"),
		TEXT("pcg_component_snapshot_json"), TEXT("pcg_world_actor_query"), TEXT("pcg_world_actor_apply_json"),
		TEXT("pcg_partition_probe"), TEXT("pcg_generate"), TEXT("pcg_cleanup"), TEXT("pcg_regenerate"),
		TEXT("pcg_cancel_generation"), TEXT("pcg_generation_status"), TEXT("pcg_wait"),
		TEXT("pcg_runtime_generation_probe"), TEXT("pcg_inspection_set"), TEXT("pcg_node_inspect"),
		TEXT("pcg_data_export_json"), TEXT("pcg_data_assert_json"), TEXT("pcg_component_runtime_probe"),
		TEXT("pcg_profile_report"), TEXT("pcg_gpu_capabilities_query"),
		TEXT("pcg_compute_graph_validate"), TEXT("pcg_hlsl_validate"), TEXT("pcg_data_asset_export_json"),
		TEXT("pcg_data_asset_apply_json"), TEXT("pcg_level_to_asset_export")
	};
	for (const TCHAR* Name : CommandNames)
	{
		Commands.Add(MakeShared<FJsonValueString>(Name));
	}
	OutData->SetArrayField(TEXT("commands"), Commands);
	OutData->SetNumberField(TEXT("command_count"), Commands.Num());
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGNodeCatalogExport(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), SchemaCatalog);

	bool bIncludeDeprecated = false;
	if (Ctx.Params.IsValid())
	{
		Ctx.Params->TryGetBoolField(TEXT("include_deprecated"), bIncludeDeprecated);
	}

	TArray<TSharedPtr<FJsonValue>> Nodes;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (!Class || !Class->IsChildOf(UPCGSettings::StaticClass()))
		{
			continue;
		}
		if (Class->HasAnyClassFlags(CLASS_Abstract))
		{
			continue;
		}
		if (!bIncludeDeprecated && Class->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			continue;
		}
		Nodes.Add(MakeShared<FJsonValueObject>(SettingsClassToCatalogJson(Class)));
	}

	Nodes.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		const TSharedPtr<FJsonObject>* ObjA = nullptr;
		const TSharedPtr<FJsonObject>* ObjB = nullptr;
		FString NameA;
		FString NameB;
		if (A.IsValid() && A->TryGetObject(ObjA) && ObjA && ObjA->IsValid())
		{
			(*ObjA)->TryGetStringField(TEXT("class_name"), NameA);
		}
		if (B.IsValid() && B->TryGetObject(ObjB) && ObjB && ObjB->IsValid())
		{
			(*ObjB)->TryGetStringField(TEXT("class_name"), NameB);
		}
		return NameA < NameB;
	});

	OutData->SetArrayField(TEXT("nodes"), Nodes);
	OutData->SetNumberField(TEXT("node_count"), Nodes.Num());

	FString OutputFile;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetStringField(TEXT("output_file"), OutputFile) && !OutputFile.TrimStartAndEnd().IsEmpty())
	{
		FString SavedPath;
		if (!SaveAuthoringFile(OutputFile, OutData, SavedPath, OutError))
		{
			return false;
		}
		OutData->SetStringField(TEXT("output_file"), SavedPath);
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGAssetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString AssetPath;
	if (!GetStringAny(Ctx.Params, { TEXT("asset_path"), TEXT("graph"), TEXT("graph_asset_path") }, AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = LoadAssetObject(AssetPath);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.asset_info.v1"));
	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost() ? Asset->GetOutermost()->GetName() : FString());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("class_path"), Asset->GetClass()->GetPathName());
	OutData->SetBoolField(TEXT("is_graph"), Asset->IsA<UPCGGraph>());
	OutData->SetBoolField(TEXT("is_graph_instance"), Asset->IsA<UPCGGraphInstance>());
	OutData->SetBoolField(TEXT("is_data_asset"), Asset->IsA<UPCGDataAsset>());
	if (UPCGGraph* Graph = Cast<UPCGGraph>(Asset))
	{
		OutData->SetObjectField(TEXT("graph"), ExportGraphToJson(Graph));
	}
	if (UPCGGraphInstance* Instance = Cast<UPCGGraphInstance>(Asset))
	{
		OutData->SetStringField(TEXT("source_graph"), Instance->Graph ? Instance->Graph->GetPathName() : FString());
		OutData->SetObjectField(TEXT("parameters"), ExportParameterBagToJson(Instance->GetUserParametersStruct()));
	}
	if (UPCGDataAsset* DataAsset = Cast<UPCGDataAsset>(Asset))
	{
		OutData->SetObjectField(TEXT("data"), PCGDataCollectionToJson(DataAsset->Data, 8));
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGGraphExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString AssetPath;
	FString FolderPathInput;
	if (!GetStringAny(Ctx.Params, { TEXT("asset_path"), TEXT("graph"), TEXT("graph_asset_path") }, AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	if (!GetStringAny(Ctx.Params, { TEXT("folder_path"), TEXT("output_folder"), TEXT("folder") }, FolderPathInput))
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}
	UPCGGraph* Graph = LoadAsset<UPCGGraph>(AssetPath);
	if (!Graph)
	{
		OutError = TEXT("pcg_graph_asset_not_found");
		return false;
	}
	FString ProfileId = TEXT("llm_authoring");
	if (Ctx.Params.IsValid()) Ctx.Params->TryGetStringField(TEXT("profile"), ProfileId);
	const FString FolderPath = ResolveFolderPath(FolderPathInput);
	UeAgentExportWorkspaceWriter::FWorkspaceResult Result;
	if (!UeAgentPCGAuthoring::ExportAssetWorkspace(*Graph, FolderPath, ProfileId, Result, OutError))
	{
		OutData = Result.ToJsonObject();
		return false;
	}
	OutData = Result.ToJsonObject();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.graph_folder.v2"));
	OutData->SetStringField(TEXT("asset_path"), Graph->GetOutermost()->GetName());
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGGraphValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString FolderPathInput;
	if (!GetStringAny(Ctx.Params, { TEXT("folder_path"), TEXT("input_folder"), TEXT("folder") }, FolderPathInput))
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}
	FString ExpectedAssetPath;
	if (Ctx.Params.IsValid()) Ctx.Params->TryGetStringField(TEXT("asset_path"), ExpectedAssetPath);
	const FString FolderPath = ResolveFolderPath(FolderPathInput);
	UeAgentPCGAuthoring::FWorkspaceInspectResult Result;
	const bool bReady = UeAgentPCGAuthoring::InspectAssetWorkspace(FolderPath, ExpectedAssetPath, Result);
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.folder_validation.v2"));
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("asset_path"), Result.AssetPath);
	OutData->SetBoolField(TEXT("valid"), bReady);
	OutData->SetNumberField(TEXT("units_checked"), Result.UnitsChecked);
	OutData->SetStringField(TEXT("error_code"), Result.ErrorCode);
	OutData->SetStringField(TEXT("error_detail"), Result.ErrorDetail);
	if (!bReady)
	{
		OutError = UeAgentAuthoringDiagnostics::GetActionableFailureMessage(
			Result.ErrorCode, Result.ErrorDetail, TEXT("pcg_graph_folder_validation_failed"));
		return false;
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGGraphPlanFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString FolderPathInput;
	if (!GetStringAny(Ctx.Params, { TEXT("folder_path"), TEXT("input_folder"), TEXT("folder") }, FolderPathInput))
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}
	FString ExpectedAssetPath;
	if (Ctx.Params.IsValid()) Ctx.Params->TryGetStringField(TEXT("asset_path"), ExpectedAssetPath);
	const FString FolderPath = ResolveFolderPath(FolderPathInput);
	UeAgentPCGAuthoring::FWorkspaceInspectResult Result;
	const bool bReady = UeAgentPCGAuthoring::InspectAssetWorkspace(FolderPath, ExpectedAssetPath, Result);
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.graph_plan.v2"));
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("asset_path"), Result.AssetPath);
	OutData->SetStringField(TEXT("operation"), Result.RequestedOperation);
	OutData->SetBoolField(TEXT("asset_exists"), Result.bAssetExists);
	OutData->SetBoolField(TEXT("would_create_asset"), Result.bWouldCreate);
	OutData->SetBoolField(TEXT("no_op"), Result.bNoOp);
	OutData->SetNumberField(TEXT("planned_node_count"), Result.NodesPlanned);
	OutData->SetNumberField(TEXT("planned_edge_count"), Result.EdgesPlanned);
	OutData->SetNumberField(TEXT("planned_node_property_count"), Result.NodePropertiesPlanned);
	OutData->SetNumberField(TEXT("units_checked"), Result.UnitsChecked);
	if (!bReady)
	{
		OutData->SetStringField(TEXT("error_code"), Result.ErrorCode);
		OutData->SetStringField(TEXT("error_detail"), Result.ErrorDetail);
		OutError = UeAgentAuthoringDiagnostics::GetActionableFailureMessage(
			Result.ErrorCode, Result.ErrorDetail, TEXT("pcg_graph_plan_failed"));
		return false;
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGGraphDiffFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString FolderPathInput;
	if (!GetStringAny(Ctx.Params, { TEXT("folder_path"), TEXT("input_folder"), TEXT("folder") }, FolderPathInput))
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}
	FString ExpectedAssetPath;
	if (Ctx.Params.IsValid()) Ctx.Params->TryGetStringField(TEXT("asset_path"), ExpectedAssetPath);
	const FString FolderPath = ResolveFolderPath(FolderPathInput);
	UeAgentPCGAuthoring::FWorkspaceInspectResult Result;
	const bool bReady = UeAgentPCGAuthoring::InspectAssetWorkspace(FolderPath, ExpectedAssetPath, Result);
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.graph_diff.v2"));
	OutData->SetStringField(TEXT("asset_path"), Result.AssetPath);
	OutData->SetBoolField(TEXT("different"), bReady && !Result.bNoOp);
	OutData->SetBoolField(TEXT("no_op"), Result.bNoOp);
	OutData->SetBoolField(TEXT("would_create_asset"), Result.bWouldCreate);
	OutData->SetNumberField(TEXT("change_count"), bReady && !Result.bNoOp ? 1 : 0);
	if (!bReady)
	{
		OutData->SetStringField(TEXT("error_code"), Result.ErrorCode);
		OutData->SetStringField(TEXT("error_detail"), Result.ErrorDetail);
		OutError = UeAgentAuthoringDiagnostics::GetActionableFailureMessage(
			Result.ErrorCode, Result.ErrorDetail, TEXT("pcg_graph_diff_failed"));
		return false;
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGGraphApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString FolderPathInput;
	if (!GetStringAny(Ctx.Params, { TEXT("folder_path"), TEXT("input_folder"), TEXT("folder") }, FolderPathInput))
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}
	FString ExpectedAssetPath;
	bool bDryRun = true;
	if (Ctx.Params.IsValid())
	{
		Ctx.Params->TryGetStringField(TEXT("asset_path"), ExpectedAssetPath);
		Ctx.Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	}
	const FString FolderPath = ResolveFolderPath(FolderPathInput);
	if (bDryRun)
	{
		UeAgentPCGAuthoring::FWorkspaceInspectResult Result;
		const bool bReady = UeAgentPCGAuthoring::InspectAssetWorkspace(FolderPath, ExpectedAssetPath, Result);
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.graph_apply.v2"));
		OutData->SetStringField(TEXT("folder_path"), FolderPath);
		OutData->SetStringField(TEXT("asset_path"), Result.AssetPath);
		OutData->SetBoolField(TEXT("dry_run"), true);
		OutData->SetBoolField(TEXT("ready"), bReady);
		OutData->SetBoolField(TEXT("no_op"), Result.bNoOp);
		OutData->SetNumberField(TEXT("planned_node_count"), Result.NodesPlanned);
		OutData->SetNumberField(TEXT("planned_edge_count"), Result.EdgesPlanned);
		OutData->SetNumberField(TEXT("planned_node_property_count"), Result.NodePropertiesPlanned);
		if (!bReady)
		{
			OutData->SetStringField(TEXT("error_code"), Result.ErrorCode);
			OutData->SetStringField(TEXT("error_detail"), Result.ErrorDetail);
			OutError = UeAgentAuthoringDiagnostics::GetActionableFailureMessage(
				Result.ErrorCode, Result.ErrorDetail, TEXT("pcg_graph_apply_preflight_failed"));
			return false;
		}
		return true;
	}

	UeAgentPCGAuthoring::FAssetApplyResult Result;
	const bool bApplied = UeAgentPCGAuthoring::ApplyAssetWorkspace(FolderPath, ExpectedAssetPath, Result, Ctx.RequestId);
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.graph_apply.v2"));
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("asset_path"), Result.AssetPath);
	OutData->SetStringField(TEXT("object_path"), Result.ObjectPath);
	OutData->SetStringField(TEXT("status"), Result.Status);
	OutData->SetStringField(TEXT("phase"), Result.Phase);
	OutData->SetStringField(TEXT("resolved_operation"), Result.ResolvedOperation);
	OutData->SetBoolField(TEXT("dry_run"), false);
	OutData->SetNumberField(TEXT("applied_node_count"), Result.NodesApplied);
	OutData->SetNumberField(TEXT("applied_edge_count"), Result.EdgesApplied);
	OutData->SetNumberField(TEXT("applied_node_property_count"), Result.NodePropertiesApplied);
	OutData->SetNumberField(TEXT("mutations_applied"), Result.MutationsApplied);
	OutData->SetBoolField(TEXT("rollback_attempted"), Result.bRollbackAttempted);
	OutData->SetBoolField(TEXT("rollback_succeeded"), Result.bRollbackSucceeded);
	if (!bApplied)
	{
		OutData->SetStringField(TEXT("error_code"), Result.ErrorCode);
		OutData->SetStringField(TEXT("error_detail"), Result.ErrorDetail);
		OutError = UeAgentAuthoringDiagnostics::GetActionableFailureMessage(
			Result.ErrorCode, Result.ErrorDetail, TEXT("pcg_graph_apply_failed"));
		return false;
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGGraphCompile(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString AssetPath;
	if (!GetStringAny(Ctx.Params, { TEXT("asset_path"), TEXT("graph"), TEXT("graph_asset_path") }, AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UPCGGraph* Graph = LoadAsset<UPCGGraph>(AssetPath);
	if (!Graph)
	{
		OutError = TEXT("pcg_graph_asset_not_found");
		return false;
	}

	Graph->Recompile();
	const bool bPrimed = Graph->PrimeGraphCompilationCache();
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.graph_compile.v1"));
	OutData->SetStringField(TEXT("asset_path"), Graph->GetOutermost() ? Graph->GetOutermost()->GetName() : AssetPath);
	OutData->SetBoolField(TEXT("recompiled"), true);
	OutData->SetBoolField(TEXT("primed_compilation_cache"), bPrimed);
	OutData->SetObjectField(TEXT("snapshot"), ExportGraphToJson(Graph));
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGGraphSnapshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString AssetPath;
	if (!GetStringAny(Ctx.Params, { TEXT("asset_path"), TEXT("graph"), TEXT("graph_asset_path") }, AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UPCGGraph* Graph = LoadAsset<UPCGGraph>(AssetPath);
	if (!Graph)
	{
		OutError = TEXT("pcg_graph_asset_not_found");
		return false;
	}
	OutData = ExportGraphToJson(Graph);
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGGraphLayout(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString AssetPath;
	if (!GetStringAny(Ctx.Params, { TEXT("asset_path"), TEXT("graph"), TEXT("graph_asset_path") }, AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UPCGGraph* Graph = LoadAsset<UPCGGraph>(AssetPath);
	if (!Graph)
	{
		OutError = TEXT("pcg_graph_asset_not_found");
		return false;
	}
	const bool bHadMaterializedEditorGraph = PCGGraphHasMaterializedEditorGraph(Graph);
	bool bOpenedEditorForGraphInit = false;
	const bool bEditorGraphReady = EnsurePCGEditorGraphMaterialized(Graph, bOpenedEditorForGraphInit);
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	auto CleanupGraphInitEditor = [&]()
	{
		ClosePCGEditorOpenedForGraphInit(AssetEditorSubsystem, Graph, bOpenedEditorForGraphInit);
	};
	if (!bEditorGraphReady)
	{
		const FString CanonicalAssetPath = Graph->GetOutermost() ? Graph->GetOutermost()->GetName() : NormalizeAssetPath(AssetPath);
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.graph_layout.v1"));
		OutData->SetStringField(TEXT("asset_path"), CanonicalAssetPath);
		OutData->SetStringField(TEXT("asset_class"), Graph->GetClass()->GetPathName());
		OutData->SetStringField(TEXT("delegated_command"), TEXT("node_graph_layout"));
		OutData->SetStringField(TEXT("pcg_graph_layout_strategy"), TEXT("shared_node_graph_blueprint_layout_core"));
		OutData->SetBoolField(TEXT("pcg_editor_graph_had_materialized_edgraph_before_init"), bHadMaterializedEditorGraph);
		OutData->SetBoolField(TEXT("pcg_editor_graph_opened_editor_for_init"), bOpenedEditorForGraphInit);
		OutError = TEXT("pcg_editor_graph_not_materialized");
		CleanupGraphInitEditor();
		return false;
	}

	TSharedPtr<FJsonObject> LayoutParams = MakeShared<FJsonObject>();
	if (Ctx.Params.IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Ctx.Params->Values)
		{
			LayoutParams->SetField(Pair.Key, Pair.Value);
		}
	}

	const FString CanonicalAssetPath = Graph->GetOutermost() ? Graph->GetOutermost()->GetName() : NormalizeAssetPath(AssetPath);
	LayoutParams->SetStringField(TEXT("asset_path"), CanonicalAssetPath);
	LayoutParams->SetStringField(TEXT("adapter"), TEXT("generic_edgraph"));

	bool bSaveAfterLayout = false;
	if (Ctx.Params.IsValid())
	{
		bool bSaveSpecified = Ctx.Params->TryGetBoolField(TEXT("save_after_layout"), bSaveAfterLayout);
		if (!bSaveSpecified)
		{
			bSaveSpecified = Ctx.Params->TryGetBoolField(TEXT("save"), bSaveAfterLayout);
		}
		const TSharedPtr<FJsonObject>* LayoutOptions = nullptr;
		if (!bSaveSpecified
			&& Ctx.Params->TryGetObjectField(TEXT("layout_options"), LayoutOptions)
			&& LayoutOptions
			&& LayoutOptions->IsValid())
		{
			bSaveSpecified = (*LayoutOptions)->TryGetBoolField(TEXT("save_after_layout"), bSaveAfterLayout);
			if (!bSaveSpecified)
			{
				bSaveSpecified = (*LayoutOptions)->TryGetBoolField(TEXT("save"), bSaveAfterLayout);
			}
		}
		if (bSaveSpecified && !LayoutParams->HasField(TEXT("save_after_layout")))
		{
			LayoutParams->SetBoolField(TEXT("save_after_layout"), bSaveAfterLayout);
		}
	}

	bool bCompileAfterLayout = false;
	if (Ctx.Params.IsValid())
	{
		Ctx.Params->TryGetBoolField(TEXT("compile_after_layout"), bCompileAfterLayout);
		if (!bCompileAfterLayout)
		{
			Ctx.Params->TryGetBoolField(TEXT("compile"), bCompileAfterLayout);
		}
		const TSharedPtr<FJsonObject>* LayoutOptions = nullptr;
		if (!bCompileAfterLayout
			&& Ctx.Params->TryGetObjectField(TEXT("layout_options"), LayoutOptions)
			&& LayoutOptions
			&& LayoutOptions->IsValid())
		{
			(*LayoutOptions)->TryGetBoolField(TEXT("compile_after_layout"), bCompileAfterLayout);
			if (!bCompileAfterLayout)
			{
				(*LayoutOptions)->TryGetBoolField(TEXT("compile"), bCompileAfterLayout);
			}
		}
	}

	FUeAgentRequestContext LayoutCtx = Ctx;
	LayoutCtx.Command = TEXT("node_graph_layout");
	LayoutCtx.Params = LayoutParams;
	TSharedPtr<FJsonObject> LayoutData;
	FString LayoutError;
	if (!CmdNodeGraphLayout(LayoutCtx, LayoutData, LayoutError))
	{
		OutData = LayoutData.IsValid() ? LayoutData : MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.graph_layout.v1"));
		OutData->SetStringField(TEXT("asset_path"), CanonicalAssetPath);
		OutData->SetStringField(TEXT("delegated_command"), TEXT("node_graph_layout"));
		OutData->SetStringField(TEXT("pcg_graph_layout_strategy"), TEXT("shared_node_graph_blueprint_layout_core"));
		OutData->SetBoolField(TEXT("pcg_editor_graph_had_materialized_edgraph_before_init"), bHadMaterializedEditorGraph);
		OutData->SetBoolField(TEXT("pcg_editor_graph_opened_editor_for_init"), bOpenedEditorForGraphInit);
		OutError = LayoutError.IsEmpty() ? TEXT("pcg_graph_layout_failed") : LayoutError;
		CleanupGraphInitEditor();
		return false;
	}

	OutData = LayoutData.IsValid() ? LayoutData : MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.graph_layout.v1"));
	OutData->SetStringField(TEXT("asset_path"), CanonicalAssetPath);
	OutData->SetStringField(TEXT("asset_class"), Graph->GetClass()->GetPathName());
	OutData->SetStringField(TEXT("delegated_command"), TEXT("node_graph_layout"));
	OutData->SetStringField(TEXT("pcg_graph_layout_strategy"), TEXT("shared_node_graph_blueprint_layout_core"));
	OutData->SetBoolField(TEXT("pcg_editor_graph_had_materialized_edgraph_before_init"), bHadMaterializedEditorGraph);
	OutData->SetBoolField(TEXT("pcg_editor_graph_opened_editor_for_init"), bOpenedEditorForGraphInit);
	bool bLayoutDesignComplete = false;
	OutData->TryGetBoolField(TEXT("layout_design_complete"), bLayoutDesignComplete);
	OutData->SetBoolField(TEXT("pcg_graph_layout_complete"), bLayoutDesignComplete);

	if (bCompileAfterLayout)
	{
		FUeAgentRequestContext CompileCtx = Ctx;
		CompileCtx.Command = TEXT("pcg_graph_compile");
		CompileCtx.Params = MakeShared<FJsonObject>();
		CompileCtx.Params->SetStringField(TEXT("asset_path"), CanonicalAssetPath);
		TSharedPtr<FJsonObject> CompileData;
		FString CompileError;
		if (!CmdPCGGraphCompile(CompileCtx, CompileData, CompileError))
		{
			OutData->SetBoolField(TEXT("compiled"), false);
			OutData->SetStringField(TEXT("compile_error"), CompileError);
			OutError = TEXT("pcg_graph_layout_compile_failed");
			CleanupGraphInitEditor();
			return false;
		}
		OutData->SetBoolField(TEXT("compiled"), true);
		if (CompileData.IsValid())
		{
			OutData->SetObjectField(TEXT("compile_result"), CompileData);
		}
		if (bSaveAfterLayout)
		{
			FString SaveError;
			if (!SavePackagesForObjects({ Graph }, SaveError))
			{
				OutData->SetBoolField(TEXT("saved_after_compile"), false);
				OutData->SetStringField(TEXT("save_after_compile_error"), SaveError);
				OutError = TEXT("pcg_graph_layout_save_after_compile_failed");
				CleanupGraphInitEditor();
				return false;
			}
			OutData->SetBoolField(TEXT("saved"), true);
			OutData->SetBoolField(TEXT("saved_after_compile"), true);
		}
	}
	else
	{
		OutData->SetBoolField(TEXT("compiled"), false);
	}
	CleanupGraphInitEditor();
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGGraphInstanceJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString AssetPath;
	if (!GetStringAny(Ctx.Params, { TEXT("asset_path"), TEXT("instance_asset_path") }, AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	bool bCreate = false;
	bool bSave = true;
	FString GraphPath;
	if (Ctx.Params.IsValid())
	{
		Ctx.Params->TryGetBoolField(TEXT("create"), bCreate);
		Ctx.Params->TryGetBoolField(TEXT("save"), bSave);
		Ctx.Params->TryGetStringField(TEXT("graph"), GraphPath);
		Ctx.Params->TryGetStringField(TEXT("graph_asset_path"), GraphPath);
	}

	UPCGGraphInstance* Instance = LoadAsset<UPCGGraphInstance>(AssetPath);
	UPCGGraphInterface* SourceGraphInterface = nullptr;
	if (!GraphPath.IsEmpty())
	{
		SourceGraphInterface = LoadAsset<UPCGGraphInterface>(GraphPath);
		if (!SourceGraphInterface)
		{
			OutError = TEXT("source_graph_not_found");
			return false;
		}
	}

	const TSharedPtr<FJsonObject>* Parameters = nullptr;
	const bool bHasParameters = Ctx.Params.IsValid() && Ctx.Params->TryGetObjectField(TEXT("parameters"), Parameters) && Parameters && Parameters->IsValid();
	if (bHasParameters)
	{
		UPCGGraphInstance* PreviewInstance = Instance ? DuplicateObject<UPCGGraphInstance>(Instance, GetTransientPackage()) : NewObject<UPCGGraphInstance>(GetTransientPackage(), UPCGGraphInstance::StaticClass(), NAME_None, RF_Transient);
		if (!PreviewInstance)
		{
			OutError = TEXT("create_graph_instance_preflight_failed");
			return false;
		}
		PreviewInstance->SetFlags(RF_Transient);
		if (SourceGraphInterface)
		{
			PreviewInstance->SetGraph(SourceGraphInterface);
		}
		RefreshGraphInstanceParameters(PreviewInstance);
		TArray<TSharedPtr<FJsonValue>> PreflightIssues;
		PreflightCanonicalParameterBag(&PreviewInstance->ParametersOverrides.Parameters, *Parameters, PreflightIssues, false);
		if (CountErrors(PreflightIssues) > 0)
		{
			OutError = TEXT("graph_instance_parameter_preflight_failed");
			OutData = MakeShared<FJsonObject>();
			OutData->SetArrayField(TEXT("issues"), PreflightIssues);
			OutData->SetNumberField(TEXT("error_count"), CountErrors(PreflightIssues));
			return false;
		}
	}

	bool bCreated = false;
	if (!Instance && bCreate)
	{
		const FString NormalizedAssetPath = NormalizeAssetPath(AssetPath);
		if (!FPackageName::IsValidLongPackageName(NormalizedAssetPath))
		{
			OutError = TEXT("invalid_asset_path");
			return false;
		}
		UPackage* Package = CreatePackage(*NormalizedAssetPath);
		if (!Package)
		{
			OutError = TEXT("create_package_failed");
			return false;
		}
		Instance = NewObject<UPCGGraphInstance>(Package, UPCGGraphInstance::StaticClass(), *FPackageName::GetLongPackageAssetName(NormalizedAssetPath), RF_Public | RF_Standalone | RF_Transactional);
		if (!Instance)
		{
			OutError = TEXT("create_graph_instance_failed");
			return false;
		}
		FAssetRegistryModule::AssetCreated(Instance);
		Instance->MarkPackageDirty();
		Package->MarkPackageDirty();
		bCreated = true;
	}
	if (!Instance)
	{
		OutError = TEXT("pcg_graph_instance_not_found");
		return false;
	}

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterfacePCG", "ApplyPCGGraphInstance", "Apply PCG Graph Instance"), bCreated || SourceGraphInterface || bHasParameters);
	Instance->Modify();

	if (SourceGraphInterface)
	{
		Instance->SetGraph(SourceGraphInterface);
		Instance->MarkPackageDirty();
	}
	if (bHasParameters)
	{
		TArray<TSharedPtr<FJsonValue>> Issues;
		TArray<FName> AppliedNames;
		RefreshGraphInstanceParameters(Instance);
		ApplyCanonicalParameterBag(Instance->ParametersOverrides.Parameters, *Parameters, Issues, &AppliedNames, false);
		NotifyGraphInstanceParameterValuesChanged(Instance, AppliedNames);
		Instance->MarkPackageDirty();
		if (CountErrors(Issues) > 0)
		{
			Transaction.Cancel();
			OutError = TEXT("graph_instance_parameter_apply_failed");
			OutData = MakeShared<FJsonObject>();
			OutData->SetArrayField(TEXT("issues"), Issues);
			OutData->SetNumberField(TEXT("error_count"), CountErrors(Issues));
			return false;
		}
	}
	if (bSave && (bCreated || SourceGraphInterface || bHasParameters))
	{
		if (!SavePackagesForObjects({ Instance }, OutError))
		{
			return false;
		}
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.graph_instance.v1"));
	OutData->SetStringField(TEXT("asset_path"), Instance->GetOutermost() ? Instance->GetOutermost()->GetName() : AssetPath);
	OutData->SetStringField(TEXT("object_path"), Instance->GetPathName());
	OutData->SetStringField(TEXT("graph"), Instance->Graph ? Instance->Graph->GetPathName() : FString());
	OutData->SetBoolField(TEXT("created"), bCreated);
	const FInstancedPropertyBag* InstanceParameters = Instance->GetUserParametersStruct();
	OutData->SetObjectField(TEXT("parameters"), ExportParameterBagToJson(InstanceParameters));
	OutData->SetNumberField(TEXT("parameter_count"), GetParameterBagPropertyCount(InstanceParameters));
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGGraphParametersQuery(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString AssetPath;
	if (!GetStringAny(Ctx.Params, { TEXT("asset_path"), TEXT("graph"), TEXT("instance_asset_path") }, AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UPCGGraphInterface* GraphInterface = LoadAsset<UPCGGraphInterface>(AssetPath);
	if (!GraphInterface)
	{
		OutError = TEXT("pcg_graph_interface_not_found");
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.graph_parameters.v1"));
	OutData->SetStringField(TEXT("asset_path"), GraphInterface->GetOutermost() ? GraphInterface->GetOutermost()->GetName() : AssetPath);
	const FInstancedPropertyBag* Parameters = GraphInterface->GetUserParametersStruct();
	OutData->SetObjectField(TEXT("parameters"), ExportParameterBagToJson(Parameters));
	OutData->SetNumberField(TEXT("parameter_count"), GetParameterBagPropertyCount(Parameters));
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGGraphParametersApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString AssetPath;
	if (!GetStringAny(Ctx.Params, { TEXT("asset_path"), TEXT("graph"), TEXT("instance_asset_path") }, AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UPCGGraphInterface* GraphInterface = LoadAsset<UPCGGraphInterface>(AssetPath);
	if (!GraphInterface)
	{
		OutError = TEXT("pcg_graph_interface_not_found");
		return false;
	}

	const TSharedPtr<FJsonObject>* Parameters = nullptr;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetObjectField(TEXT("parameters"), Parameters) || !Parameters || !Parameters->IsValid())
	{
		OutError = TEXT("missing_parameters");
		return false;
	}
	bool bSave = true;
	Ctx.Params->TryGetBoolField(TEXT("save"), bSave);

	TArray<TSharedPtr<FJsonValue>> Issues;
	int32 PreflightAppliedCount = 0;
	if (UPCGGraph* Graph = Cast<UPCGGraph>(GraphInterface))
	{
		PreflightAppliedCount = PreflightCanonicalParameterBag(Graph->GetUserParametersStruct(), *Parameters, Issues, true);
	}
	else if (UPCGGraphInstance* Instance = Cast<UPCGGraphInstance>(GraphInterface))
	{
		UPCGGraphInstance* PreviewInstance = DuplicateObject<UPCGGraphInstance>(Instance, GetTransientPackage());
		if (!PreviewInstance)
		{
			OutError = TEXT("graph_parameters_preflight_failed");
			return false;
		}
		PreviewInstance->SetFlags(RF_Transient);
		RefreshGraphInstanceParameters(PreviewInstance);
		PreflightAppliedCount = PreflightCanonicalParameterBag(&PreviewInstance->ParametersOverrides.Parameters, *Parameters, Issues, false);
	}
	if (CountErrors(Issues) > 0)
	{
		OutError = TEXT("graph_parameters_preflight_failed");
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.graph_parameters_apply.v1"));
		OutData->SetObjectField(TEXT("parameters"), ExportParameterBagToJson(GraphInterface->GetUserParametersStruct()));
		OutData->SetNumberField(TEXT("parameter_count"), GetParameterBagPropertyCount(GraphInterface->GetUserParametersStruct()));
		OutData->SetNumberField(TEXT("applied_count"), 0);
		OutData->SetNumberField(TEXT("preflight_applied_count"), PreflightAppliedCount);
		OutData->SetArrayField(TEXT("issues"), Issues);
		OutData->SetNumberField(TEXT("error_count"), CountErrors(Issues));
		return false;
	}

	int32 AppliedCount = 0;
	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterfacePCG", "ApplyPCGGraphParameters", "Apply PCG Graph Parameters"), true);
	GraphInterface->Modify();
	if (UPCGGraph* Graph = Cast<UPCGGraph>(GraphInterface))
	{
		Graph->UpdateUserParametersStruct([&](FInstancedPropertyBag& Bag)
		{
			AppliedCount = ApplyCanonicalParameterBag(Bag, *Parameters, Issues);
		});
		Graph->MarkPackageDirty();
	}
	else if (UPCGGraphInstance* Instance = Cast<UPCGGraphInstance>(GraphInterface))
	{
		TArray<FName> AppliedNames;
		RefreshGraphInstanceParameters(Instance);
		AppliedCount = ApplyCanonicalParameterBag(Instance->ParametersOverrides.Parameters, *Parameters, Issues, &AppliedNames, false);
		NotifyGraphInstanceParameterValuesChanged(Instance, AppliedNames);
		Instance->MarkPackageDirty();
	}
	if (CountErrors(Issues) > 0)
	{
		Transaction.Cancel();
		OutError = TEXT("graph_parameters_apply_failed");
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.graph_parameters_apply.v1"));
		OutData->SetObjectField(TEXT("parameters"), ExportParameterBagToJson(GraphInterface->GetUserParametersStruct()));
		OutData->SetNumberField(TEXT("parameter_count"), GetParameterBagPropertyCount(GraphInterface->GetUserParametersStruct()));
		OutData->SetNumberField(TEXT("applied_count"), AppliedCount);
		OutData->SetArrayField(TEXT("issues"), Issues);
		OutData->SetNumberField(TEXT("error_count"), CountErrors(Issues));
		return false;
	}

	if (CountErrors(Issues) == 0 && bSave && !SavePackagesForObjects({ GraphInterface }, OutError))
	{
		return false;
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.graph_parameters_apply.v1"));
	OutData->SetObjectField(TEXT("parameters"), ExportParameterBagToJson(GraphInterface->GetUserParametersStruct()));
	OutData->SetNumberField(TEXT("parameter_count"), GetParameterBagPropertyCount(GraphInterface->GetUserParametersStruct()));
	OutData->SetNumberField(TEXT("applied_count"), AppliedCount);
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetNumberField(TEXT("error_count"), CountErrors(Issues));
	return CountErrors(Issues) == 0;
}

bool FUeAgentLegacyMechanics::CmdPCGComponentQuery(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError;
		return false;
	}
	UPCGComponent* Component = FindPCGComponent(World, Ctx.Params, OutError);
	if (!Component)
	{
		return false;
	}
	OutData = ComponentToJson(Component);
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGComponentApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError;
		return false;
	}

	FString ActorName;
	if (!GetStringAny(Ctx.Params, { TEXT("actor"), TEXT("actor_name"), TEXT("actor_label") }, ActorName))
	{
		OutError = TEXT("missing_actor");
		return false;
	}
	AActor* Actor = FindActorByNameOrLabelDirect(World, ActorName);
	if (!Actor)
	{
		OutError = TEXT("actor_not_found");
		return false;
	}

	FString ComponentName;
	GetStringAny(Ctx.Params, { TEXT("component"), TEXT("component_name"), TEXT("pcg_component") }, ComponentName);
	if (ComponentName.IsEmpty())
	{
		ComponentName = TEXT("PCGComponent");
	}

	FString GraphPath;
	UPCGGraphInterface* GraphInterface = nullptr;
	if (GetStringAny(Ctx.Params, { TEXT("graph"), TEXT("graph_asset_path"), TEXT("asset_path") }, GraphPath))
	{
		GraphInterface = LoadAsset<UPCGGraphInterface>(GraphPath);
		if (!GraphInterface)
		{
			OutError = TEXT("pcg_graph_interface_not_found");
			return false;
		}
	}

	FString TriggerText;
	EPCGComponentGenerationTrigger ParsedTrigger = EPCGComponentGenerationTrigger::GenerateOnDemand;
	const bool bHasGenerationTrigger = Ctx.Params->TryGetStringField(TEXT("generation_trigger"), TriggerText);
	if (bHasGenerationTrigger && !TryParseGenerationTrigger(TriggerText, ParsedTrigger))
	{
		OutError = TEXT("invalid_generation_trigger");
		return false;
	}

	UPCGComponent* Component = nullptr;
	TArray<UPCGComponent*> Components;
	Actor->GetComponents<UPCGComponent>(Components);
	for (UPCGComponent* Existing : Components)
	{
		if (Existing && Existing->GetName().Equals(ComponentName, ESearchCase::IgnoreCase))
		{
			Component = Existing;
			break;
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
	TArray<TSharedPtr<FJsonValue>> Issues;
	if (Ctx.Params->TryGetArrayField(TEXT("properties"), Properties))
	{
		UPCGComponent* PreviewComponent = Component ? DuplicateObject<UPCGComponent>(Component, GetTransientPackage()) : NewObject<UPCGComponent>(GetTransientPackage(), UPCGComponent::StaticClass(), NAME_None, RF_Transient);
		if (!PreviewComponent)
		{
			OutError = TEXT("component_property_preflight_failed");
			return false;
		}
		PreviewComponent->SetFlags(RF_Transient);
		if (GraphInterface)
		{
			PreviewComponent->SetGraph(GraphInterface);
		}
		ApplyPropertyArray(PreviewComponent, Properties, Issues, TEXT("component.properties"));
		if (CountErrors(Issues) > 0)
		{
			OutError = TEXT("component_property_preflight_failed");
			OutData = MakeShared<FJsonObject>();
			OutData->SetArrayField(TEXT("issues"), Issues);
			OutData->SetNumberField(TEXT("error_count"), CountErrors(Issues));
			return false;
		}
	}

	bool bCreated = false;
	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterfacePCG", "ApplyPCGComponent", "Apply PCG Component"), true);
	Actor->Modify();
	if (!Component)
	{
		Component = NewObject<UPCGComponent>(Actor, *ComponentName, RF_Transactional);
		if (!Component)
		{
			Transaction.Cancel();
			OutError = TEXT("create_pcg_component_failed");
			return false;
		}
		Actor->AddInstanceComponent(Component);
		Component->RegisterComponent();
		bCreated = true;
	}
	Component->Modify();

	if (GraphInterface)
	{
		Component->SetGraph(GraphInterface);
	}

	double NumberValue = 0.0;
	if (Ctx.Params->TryGetNumberField(TEXT("seed"), NumberValue))
	{
		Component->Seed = static_cast<int32>(NumberValue);
	}
	bool BoolValue = false;
	if (Ctx.Params->TryGetBoolField(TEXT("activated"), BoolValue))
	{
		Component->bActivated = BoolValue;
	}
	if (Ctx.Params->TryGetBoolField(TEXT("partitioned"), BoolValue))
	{
		Component->bIsComponentPartitioned = BoolValue;
	}
	if (bHasGenerationTrigger)
	{
		Component->GenerationTrigger = ParsedTrigger;
	}
	if (Properties)
	{
		ApplyPropertyArray(Component, Properties, Issues, TEXT("component.properties"));
		if (CountErrors(Issues) > 0)
		{
			Transaction.Cancel();
			OutError = TEXT("component_property_apply_failed");
			OutData = ComponentToJson(Component);
			OutData->SetBoolField(TEXT("created"), bCreated);
			OutData->SetArrayField(TEXT("issues"), Issues);
			OutData->SetNumberField(TEXT("error_count"), CountErrors(Issues));
			return false;
		}
	}

	bool bSaveLevel = false;
	Ctx.Params->TryGetBoolField(TEXT("save_level"), bSaveLevel);
	Actor->MarkPackageDirty();
	if (bSaveLevel)
	{
		if (!UEditorLoadingAndSavingUtils::SaveDirtyPackages(true, true))
		{
			OutError = TEXT("save_level_failed");
			return false;
		}
	}

	OutData = ComponentToJson(Component);
	OutData->SetBoolField(TEXT("created"), bCreated);
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetNumberField(TEXT("error_count"), CountErrors(Issues));
	return CountErrors(Issues) == 0;
}

bool FUeAgentLegacyMechanics::CmdPCGComponentSnapshotJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return CmdPCGComponentQuery(Ctx, OutData, OutError);
}

bool FUeAgentLegacyMechanics::CmdPCGWorldActorQuery(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError;
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Actors;
	for (TActorIterator<APCGWorldActor> It(World); It; ++It)
	{
		APCGWorldActor* Actor = *It;
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Actor->GetName());
#if WITH_EDITOR
		Obj->SetStringField(TEXT("label"), Actor->GetActorLabel());
		Obj->SetBoolField(TEXT("treat_editor_viewport_as_generation_source"), Actor->bTreatEditorViewportAsGenerationSource);
#endif
		Obj->SetNumberField(TEXT("partition_grid_size"), Actor->PartitionGridSize);
		Obj->SetBoolField(TEXT("enable_world_partition_generation_sources"), Actor->bEnableWorldPartitionGenerationSources);
		Actors.Add(MakeShared<FJsonValueObject>(Obj));
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.world_actor.v1"));
	OutData->SetArrayField(TEXT("world_actors"), Actors);
	OutData->SetNumberField(TEXT("world_actor_count"), Actors.Num());
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGWorldActorApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError;
		return false;
	}

	double RequestedPartitionGridSize = 0.0;
	const bool bHasPartitionGridSize = Ctx.Params.IsValid() && Ctx.Params->TryGetNumberField(TEXT("partition_grid_size"), RequestedPartitionGridSize);
	if (bHasPartitionGridSize && RequestedPartitionGridSize <= 0.0)
	{
		OutError = TEXT("invalid_partition_grid_size");
		return false;
	}

	APCGWorldActor* WorldActor = nullptr;
	for (TActorIterator<APCGWorldActor> It(World); It; ++It)
	{
		WorldActor = *It;
		break;
	}
	bool bCreated = false;
#if WITH_EDITOR
	if (!WorldActor)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.Name = MakeUniqueObjectName(World, APCGWorldActor::StaticClass(), TEXT("PCGWorldActor"));
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParams.ObjectFlags |= RF_Transactional;
		WorldActor = World->SpawnActor<APCGWorldActor>(APCGWorldActor::StaticClass(), FTransform::Identity, SpawnParams);
		bCreated = WorldActor != nullptr;
	}
#endif
	if (!WorldActor)
	{
		OutError = TEXT("pcg_world_actor_create_failed");
		return false;
	}

	double NumberValue = 0.0;
	if (bHasPartitionGridSize)
	{
		WorldActor->PartitionGridSize = static_cast<uint32>(RequestedPartitionGridSize);
	}
	bool BoolValue = false;
	if (Ctx.Params->TryGetBoolField(TEXT("enable_world_partition_generation_sources"), BoolValue))
	{
		WorldActor->bEnableWorldPartitionGenerationSources = BoolValue;
	}
#if WITH_EDITOR
	if (Ctx.Params->TryGetBoolField(TEXT("treat_editor_viewport_as_generation_source"), BoolValue))
	{
		WorldActor->bTreatEditorViewportAsGenerationSource = BoolValue;
	}
#endif
	WorldActor->MarkPackageDirty();

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.world_actor_apply.v1"));
	OutData->SetBoolField(TEXT("created"), bCreated);
	OutData->SetStringField(TEXT("name"), WorldActor->GetName());
	OutData->SetNumberField(TEXT("partition_grid_size"), WorldActor->PartitionGridSize);
	OutData->SetBoolField(TEXT("enable_world_partition_generation_sources"), WorldActor->bEnableWorldPartitionGenerationSources);
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGPartitionProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError;
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Partitions;
	for (TActorIterator<APCGPartitionActor> It(World); It; ++It)
	{
		APCGPartitionActor* Actor = *It;
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Actor->GetName());
#if WITH_EDITOR
		Obj->SetStringField(TEXT("label"), Actor->GetActorLabel());
#endif
		Obj->SetNumberField(TEXT("pcg_grid_size"), Actor->GetPCGGridSize());
		Obj->SetBoolField(TEXT("using_2d_grid"), Actor->IsUsing2DGrid());
		Obj->SetBoolField(TEXT("runtime_generated"), Actor->IsRuntimeGenerated());
		Obj->SetBoolField(TEXT("has_local_pcg_components"), Actor->HasLocalPCGComponents());
		Obj->SetNumberField(TEXT("local_component_count"), Actor->GetAllLocalPCGComponents().Num());
		Obj->SetNumberField(TEXT("original_component_count"), Actor->GetAllOriginalPCGComponents().Num());
		Partitions.Add(MakeShared<FJsonValueObject>(Obj));
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.partition_probe.v1"));
	OutData->SetArrayField(TEXT("partition_actors"), Partitions);
	OutData->SetNumberField(TEXT("partition_actor_count"), Partitions.Num());
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGGenerate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError;
		return false;
	}
	UPCGComponent* Component = FindPCGComponent(World, Ctx.Params, OutError);
	if (!Component)
	{
		return false;
	}
	bool bForce = true;
	bool bWait = false;
	Ctx.Params->TryGetBoolField(TEXT("force"), bForce);
	Ctx.Params->TryGetBoolField(TEXT("wait"), bWait);
	FString TriggerText;
	const bool bHasGenerationTrigger = Ctx.Params->TryGetStringField(TEXT("generation_trigger"), TriggerText);
	EPCGComponentGenerationTrigger Trigger = EPCGComponentGenerationTrigger::GenerateOnDemand;
	if (bHasGenerationTrigger && !TryParseGenerationTrigger(TriggerText, Trigger))
	{
		OutError = TEXT("invalid_generation_trigger");
		return false;
	}
	const FPCGTaskId TaskId = Component->GenerateLocalGetTaskId(Trigger, bForce);
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.generate.v1"));
	OutData->SetNumberField(TEXT("task_id"), static_cast<double>(TaskId));
	SetActionStatus(OutData, Component);
	if (bWait)
	{
		TSharedPtr<FJsonObject> WaitData;
		FString WaitError;
		CmdPCGWait(Ctx, WaitData, WaitError);
		OutData->SetObjectField(TEXT("wait"), WaitData);
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGCleanup(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError;
		return false;
	}
	UPCGComponent* Component = FindPCGComponent(World, Ctx.Params, OutError);
	if (!Component)
	{
		return false;
	}
	bool bRemoveComponents = true;
	Ctx.Params->TryGetBoolField(TEXT("remove_components"), bRemoveComponents);
	const TArray<FPCGTaskId> Dependencies;
	const FPCGTaskId TaskId = Component->CleanupLocal(bRemoveComponents, Dependencies);
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.cleanup.v1"));
	OutData->SetNumberField(TEXT("task_id"), static_cast<double>(TaskId));
	SetActionStatus(OutData, Component);
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGRegenerate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> CleanupData;
	if (!CmdPCGCleanup(Ctx, CleanupData, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> GenerateData;
	if (!CmdPCGGenerate(Ctx, GenerateData, OutError))
	{
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.regenerate.v1"));
	OutData->SetObjectField(TEXT("cleanup"), CleanupData);
	OutData->SetObjectField(TEXT("generate"), GenerateData);
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGCancelGeneration(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError;
		return false;
	}
	UPCGComponent* Component = FindPCGComponent(World, Ctx.Params, OutError);
	if (!Component)
	{
		return false;
	}
	Component->CancelGeneration();
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.cancel_generation.v1"));
	SetActionStatus(OutData, Component);
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGGenerationStatus(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return CmdPCGComponentQuery(Ctx, OutData, OutError);
}

bool FUeAgentLegacyMechanics::CmdPCGWait(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError;
		return false;
	}
	UPCGComponent* Component = FindPCGComponent(World, Ctx.Params, OutError);
	if (!Component)
	{
		return false;
	}
	double TimeoutSeconds = 10.0;
	Ctx.Params->TryGetNumberField(TEXT("timeout_seconds"), TimeoutSeconds);
	const double StartTime = FPlatformTime::Seconds();
	double LastTickTime = StartTime;
	int32 Iterations = 0;
	UPCGSubsystem* PCGSubsystem = UPCGSubsystem::GetInstance(World);
	while ((Component->IsGenerating() || Component->IsCleaningUp()) && FPlatformTime::Seconds() - StartTime < TimeoutSeconds)
	{
		const double Now = FPlatformTime::Seconds();
		const float DeltaSeconds = FMath::Clamp(static_cast<float>(Now - LastTickTime), 0.001f, 0.05f);
		LastTickTime = Now;
		if (PCGSubsystem)
		{
			PCGSubsystem->Tick(DeltaSeconds);
		}
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		FPlatformProcess::Sleep(0.005f);
		Iterations++;
	}
	const bool bTimedOut = Component->IsGenerating() || Component->IsCleaningUp();
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.wait.v1"));
	OutData->SetBoolField(TEXT("completed"), !bTimedOut);
	OutData->SetBoolField(TEXT("timed_out"), bTimedOut);
	OutData->SetNumberField(TEXT("iterations"), Iterations);
	OutData->SetNumberField(TEXT("elapsed_seconds"), FPlatformTime::Seconds() - StartTime);
	SetActionStatus(OutData, Component);
	return !bTimedOut;
}

bool FUeAgentLegacyMechanics::CmdPCGRuntimeGenerationProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	TSharedPtr<FJsonObject> ComponentData;
	if (!CmdPCGComponentQuery(Ctx, ComponentData, OutError))
	{
		return false;
	}
	OutData = ComponentData;
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.runtime_generation_probe.v1"));
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGInspectionSet(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError;
		return false;
	}
	UPCGComponent* Component = FindPCGComponent(World, Ctx.Params, OutError);
	if (!Component)
	{
		return false;
	}
	bool bEnabled = true;
	Ctx.Params->TryGetBoolField(TEXT("enabled"), bEnabled);
	if (bEnabled)
	{
		Component->GetExecutionState().GetInspection().EnableInspection();
	}
	else
	{
		Component->GetExecutionState().GetInspection().DisableInspection();
	}
	bool bClear = false;
	Ctx.Params->TryGetBoolField(TEXT("clear"), bClear);
	if (bClear)
	{
		Component->GetExecutionState().GetInspection().ClearInspectionData(true);
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.inspection_set.v1"));
	OutData->SetBoolField(TEXT("enabled"), Component->GetExecutionState().GetInspection().IsInspecting());
	SetActionStatus(OutData, Component);
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGNodeInspect(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError;
		return false;
	}
	UPCGComponent* Component = FindPCGComponent(World, Ctx.Params, OutError);
	if (!Component)
	{
		return false;
	}
	FString NodeIdOrTitle;
	GetStringAny(Ctx.Params, { TEXT("node"), TEXT("node_id"), TEXT("node_title") }, NodeIdOrTitle);

	UPCGGraph* Graph = Component->GetGraph();
	TArray<TSharedPtr<FJsonValue>> Nodes;
	if (Graph)
	{
		TSet<FString> UsedIds;
		for (UPCGNode* Node : Graph->GetNodes())
		{
			const FString NodeId = MakeStableNodeId(Graph, Node, UsedIds);
			if (NodeIdOrTitle.IsEmpty() ||
				NodeId.Equals(NodeIdOrTitle, ESearchCase::IgnoreCase) ||
				Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString().Equals(NodeIdOrTitle, ESearchCase::IgnoreCase) ||
				Node->GetAuthoredTitleName().ToString().Equals(NodeIdOrTitle, ESearchCase::IgnoreCase))
			{
				TSharedPtr<FJsonObject> NodeObj = NodeToJson(Graph, Node, NodeId);
				const FPCGStack Stack;
				const FPCGGraphExecutionInspection& Inspection = Component->GetExecutionState().GetInspection();
				NodeObj->SetBoolField(TEXT("was_executed"), Inspection.WasNodeExecuted(Node, Stack));
				NodeObj->SetBoolField(TEXT("produced_data"), Inspection.HasNodeProducedData(Node, Stack));
				NodeObj->SetBoolField(TEXT("gpu_to_cpu_readback"), Inspection.DidNodeTriggerGPUToCPUReadback(Node, Stack));
				NodeObj->SetBoolField(TEXT("cpu_to_gpu_upload"), Inspection.DidNodeTriggerCPUToGPUUpload(Node, Stack));
				Nodes.Add(MakeShared<FJsonValueObject>(NodeObj));
			}
		}
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.node_inspect.v1"));
	OutData->SetArrayField(TEXT("nodes"), Nodes);
	OutData->SetNumberField(TEXT("node_count"), Nodes.Num());
	SetActionStatus(OutData, Component);
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGDataExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError;
		return false;
	}
	UPCGComponent* Component = FindPCGComponent(World, Ctx.Params, OutError);
	if (!Component)
	{
		return false;
	}
	double MaxPointsNumber = 16.0;
	Ctx.Params->TryGetNumberField(TEXT("max_points_per_data"), MaxPointsNumber);
	OutData = PCGDataCollectionToJson(Component->GetGeneratedGraphOutput(), static_cast<int32>(MaxPointsNumber));
	OutData->SetObjectField(TEXT("component"), ComponentToJson(Component));

	FString OutputFile;
	if (Ctx.Params->TryGetStringField(TEXT("output_file"), OutputFile) && !OutputFile.TrimStartAndEnd().IsEmpty())
	{
		FString SavedPath;
		if (!SaveAuthoringFile(OutputFile, OutData, SavedPath, OutError))
		{
			return false;
		}
		OutData->SetStringField(TEXT("output_file"), SavedPath);
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGDataAssertJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	TSharedPtr<FJsonObject> Data;
	if (!CmdPCGDataExportJson(Ctx, Data, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	double MinDataCount = -1.0;
	double MinTotalPointCount = -1.0;
	if (Ctx.Params->TryGetNumberField(TEXT("min_data_count"), MinDataCount) && Data->GetNumberField(TEXT("data_count")) < MinDataCount)
	{
		AddIssue(Issues, TEXT("error"), TEXT("min_data_count_failed"), TEXT("data_count"), FString::Printf(TEXT("Expected at least %.0f data entries."), MinDataCount));
	}
	if (Ctx.Params->TryGetNumberField(TEXT("min_total_point_count"), MinTotalPointCount) && Data->GetNumberField(TEXT("total_point_count")) < MinTotalPointCount)
	{
		AddIssue(Issues, TEXT("error"), TEXT("min_total_point_count_failed"), TEXT("total_point_count"), FString::Printf(TEXT("Expected at least %.0f points."), MinTotalPointCount));
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.data_assert.v1"));
	OutData->SetBoolField(TEXT("passed"), CountErrors(Issues) == 0);
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetObjectField(TEXT("data"), Data);
	return CountErrors(Issues) == 0;
}

bool FUeAgentLegacyMechanics::CmdPCGComponentRuntimeProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return CmdPCGRuntimeGenerationProbe(Ctx, OutData, OutError);
}

bool FUeAgentLegacyMechanics::CmdPCGScreenshot(
	const FUeAgentRequestContext& Ctx,
	TSharedPtr<FJsonObject>& OutData,
	FString& OutError) const
{
	using namespace UeAgentPCG;
	TSharedPtr<FJsonObject> FrameData;
	if (Ctx.Params.IsValid()
		&& (Ctx.Params->HasField(TEXT("actor"))
			|| Ctx.Params->HasField(TEXT("actor_name"))
			|| Ctx.Params->HasField(TEXT("actor_label"))))
	{
		FUeAgentRequestContext FrameContext = Ctx;
		TSharedPtr<FJsonObject> FrameParameters = MakeShared<FJsonObject>();
		FrameParameters->Values = Ctx.Params->Values;
		if (!FrameParameters->HasField(TEXT("id")))
		{
			FString ActorId;
			if (GetStringAny(
					FrameParameters,
					{TEXT("actor"), TEXT("actor_name"), TEXT("actor_label")},
					ActorId)
				&& !ActorId.IsEmpty())
			{
				FrameParameters->SetStringField(TEXT("id"), ActorId);
			}
		}
		FrameContext.Params = FrameParameters;
		if (!CmdViewportFrameActor(FrameContext, FrameData, OutError))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("pcg_frame_actor_failed");
			}
			return false;
		}
	}

	TSharedPtr<FJsonObject> ScreenshotData;
	if (!CmdScreenshotViewport(Ctx, ScreenshotData, OutError)
		|| !ScreenshotData.IsValid())
	{
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(
		TEXT("schema"), TEXT("ue_agent_interface.pcg.screenshot.v2"));
	OutData->SetObjectField(TEXT("frame_actor"),
		FrameData.IsValid() ? FrameData : MakeShared<FJsonObject>());
	OutData->SetObjectField(TEXT("screenshot"), ScreenshotData);
	FString FilePath;
	if (ScreenshotData->TryGetStringField(TEXT("file_path"), FilePath))
	{
		OutData->SetStringField(TEXT("file_path"), FilePath);
	}
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGProfileReport(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString AssetPath;
	if (GetStringAny(Ctx.Params, { TEXT("asset_path"), TEXT("graph"), TEXT("graph_asset_path") }, AssetPath))
	{
		UPCGGraph* Graph = LoadAsset<UPCGGraph>(AssetPath);
		if (!Graph)
		{
			OutError = TEXT("pcg_graph_asset_not_found");
			return false;
		}
		TSharedPtr<FJsonObject> Snapshot = ExportGraphToJson(Graph);
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.profile_report.v1"));
		OutData->SetStringField(TEXT("mode"), TEXT("graph_static"));
		OutData->SetNumberField(TEXT("node_count"), Snapshot->GetIntegerField(TEXT("node_count")));
		OutData->SetNumberField(TEXT("edge_count"), Snapshot->GetIntegerField(TEXT("edge_count")));
		OutData->SetObjectField(TEXT("graph"), Snapshot);
		return true;
	}

	TSharedPtr<FJsonObject> ComponentData;
	if (!CmdPCGComponentQuery(Ctx, ComponentData, OutError))
	{
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.profile_report.v1"));
	OutData->SetStringField(TEXT("mode"), TEXT("component_runtime"));
	OutData->SetObjectField(TEXT("component"), ComponentData);
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGGPUCapabilitiesQuery(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	TSharedPtr<FJsonObject> Catalog;
	FString CatalogError;
	CmdPCGNodeCatalogExport(Ctx, Catalog, CatalogError);
	int32 GPUCapableNodes = 0;
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	if (Catalog.IsValid() && Catalog->TryGetArrayField(TEXT("nodes"), Nodes) && Nodes)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Nodes)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			bool bSupportsGPU = false;
			if (Value.IsValid() && Value->TryGetObject(Obj) && Obj && Obj->IsValid() && (*Obj)->TryGetBoolField(TEXT("supports_gpu_toggle"), bSupportsGPU) && bSupportsGPU)
			{
				GPUCapableNodes++;
			}
		}
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.gpu_capabilities.v1"));
	OutData->SetStringField(TEXT("rhi"), GDynamicRHI ? GDynamicRHI->GetName() : FString());
	OutData->SetBoolField(TEXT("compute_framework_loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("ComputeFramework")));
	OutData->SetNumberField(TEXT("gpu_capable_node_class_count"), GPUCapableNodes);
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGComputeGraphValidate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString AssetPath;
	if (!GetStringAny(Ctx.Params, { TEXT("asset_path"), TEXT("graph"), TEXT("graph_asset_path") }, AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UPCGGraph* Graph = LoadAsset<UPCGGraph>(AssetPath);
	if (!Graph)
	{
		OutError = TEXT("pcg_graph_asset_not_found");
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> GPUNodeEntries;
	for (UPCGNode* Node : Graph->GetNodes())
	{
		if (UPCGSettings* Settings = Node ? Node->GetSettings() : nullptr)
		{
			if (Settings->ShouldExecuteOnGPU())
			{
				GPUNodeEntries.Add(MakeShared<FJsonValueObject>(NodeToJson(Graph, Node, Node->GetName())));
			}
		}
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.compute_graph_validate.v1"));
	OutData->SetStringField(TEXT("asset_path"), Graph->GetOutermost() ? Graph->GetOutermost()->GetName() : AssetPath);
	OutData->SetArrayField(TEXT("gpu_nodes"), GPUNodeEntries);
	OutData->SetNumberField(TEXT("gpu_node_count"), GPUNodeEntries.Num());
	OutData->SetBoolField(TEXT("compute_framework_loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("ComputeFramework")));
	OutData->SetBoolField(TEXT("valid"), true);
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGHLSLValidate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString AssetPath;
	if (!GetStringAny(Ctx.Params, { TEXT("asset_path"), TEXT("graph"), TEXT("graph_asset_path") }, AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UPCGGraph* Graph = LoadAsset<UPCGGraph>(AssetPath);
	if (!Graph)
	{
		OutError = TEXT("pcg_graph_asset_not_found");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> HLSLNodes;
	for (UPCGNode* Node : Graph->GetNodes())
	{
		UPCGSettings* Settings = Node ? Node->GetSettings() : nullptr;
		if (!Settings)
		{
			continue;
		}
		const FString ClassName = Settings->GetClass()->GetName();
		const bool bLooksLikeHLSL = ClassName.Contains(TEXT("HLSL"), ESearchCase::IgnoreCase) || ClassName.Contains(TEXT("Compute"), ESearchCase::IgnoreCase);
		if (!bLooksLikeHLSL)
		{
			continue;
		}
		TSharedPtr<FJsonObject> NodeObj = NodeToJson(Graph, Node, Node->GetName());
		HLSLNodes.Add(MakeShared<FJsonValueObject>(NodeObj));
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.hlsl_validate.v1"));
	OutData->SetStringField(TEXT("asset_path"), Graph->GetOutermost() ? Graph->GetOutermost()->GetName() : AssetPath);
	OutData->SetArrayField(TEXT("hlsl_nodes"), HLSLNodes);
	OutData->SetNumberField(TEXT("hlsl_node_count"), HLSLNodes.Num());
	OutData->SetBoolField(TEXT("valid"), true);
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGDataAssetExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString AssetPath;
	if (!GetStringAny(Ctx.Params, { TEXT("asset_path"), TEXT("data_asset") }, AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UPCGDataAsset* DataAsset = LoadAsset<UPCGDataAsset>(AssetPath);
	if (!DataAsset)
	{
		OutError = TEXT("pcg_data_asset_not_found");
		return false;
	}
	double MaxPointsNumber = 16.0;
	Ctx.Params->TryGetNumberField(TEXT("max_points_per_data"), MaxPointsNumber);
	OutData = PCGDataCollectionToJson(DataAsset->Data, static_cast<int32>(MaxPointsNumber));
	OutData->SetStringField(TEXT("asset_path"), DataAsset->GetOutermost() ? DataAsset->GetOutermost()->GetName() : AssetPath);
	OutData->SetStringField(TEXT("object_path"), DataAsset->GetPathName());
	OutData->SetStringField(TEXT("name"), DataAsset->Name);
#if WITH_EDITORONLY_DATA
	OutData->SetStringField(TEXT("object_path_source"), DataAsset->ObjectPath.ToString());
	OutData->SetBoolField(TEXT("expose_to_library"), DataAsset->bExposeToLibrary);
	OutData->SetStringField(TEXT("category"), DataAsset->Category.ToString());
	OutData->SetStringField(TEXT("description"), DataAsset->Description.ToString());
#endif
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGDataAssetApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString AssetPath;
	if (!GetStringAny(Ctx.Params, { TEXT("asset_path"), TEXT("data_asset") }, AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	bool bCreate = true;
	bool bSave = true;
	Ctx.Params->TryGetBoolField(TEXT("create"), bCreate);
	Ctx.Params->TryGetBoolField(TEXT("save"), bSave);

	UPCGDataAsset* DataAsset = LoadAsset<UPCGDataAsset>(AssetPath);
	bool bCreated = false;
	if (!DataAsset && bCreate)
	{
		const FString NormalizedAssetPath = NormalizeAssetPath(AssetPath);
		if (!FPackageName::IsValidLongPackageName(NormalizedAssetPath))
		{
			OutError = TEXT("invalid_asset_path");
			return false;
		}
		UPackage* Package = CreatePackage(*NormalizedAssetPath);
		if (!Package)
		{
			OutError = TEXT("create_package_failed");
			return false;
		}
		DataAsset = NewObject<UPCGDataAsset>(Package, UPCGDataAsset::StaticClass(), *FPackageName::GetLongPackageAssetName(NormalizedAssetPath), RF_Public | RF_Standalone | RF_Transactional);
		if (!DataAsset)
		{
			OutError = TEXT("create_pcg_data_asset_failed");
			return false;
		}
		FAssetRegistryModule::AssetCreated(DataAsset);
		Package->MarkPackageDirty();
		bCreated = true;
	}
	if (!DataAsset)
	{
		OutError = TEXT("pcg_data_asset_not_found");
		return false;
	}
	FString TextValue;
	if (Ctx.Params->TryGetStringField(TEXT("name"), TextValue))
	{
		DataAsset->Name = TextValue;
	}
#if WITH_EDITORONLY_DATA
	if (Ctx.Params->TryGetStringField(TEXT("object_path_source"), TextValue) || Ctx.Params->TryGetStringField(TEXT("object_path"), TextValue))
	{
		DataAsset->ObjectPath = FSoftObjectPath(TextValue);
	}
	bool BoolValue = false;
	if (Ctx.Params->TryGetBoolField(TEXT("expose_to_library"), BoolValue))
	{
		DataAsset->bExposeToLibrary = BoolValue;
	}
	if (Ctx.Params->TryGetStringField(TEXT("category"), TextValue))
	{
		DataAsset->Category = FText::FromString(TextValue);
	}
	if (Ctx.Params->TryGetStringField(TEXT("description"), TextValue))
	{
		DataAsset->Description = FText::FromString(TextValue);
	}
#endif
	DataAsset->MarkPackageDirty();
	if (bSave && !SavePackagesForObjects({ DataAsset }, OutError))
	{
		return false;
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.data_asset_apply.v1"));
	OutData->SetBoolField(TEXT("created"), bCreated);
	OutData->SetStringField(TEXT("asset_path"), DataAsset->GetOutermost() ? DataAsset->GetOutermost()->GetName() : AssetPath);
	OutData->SetStringField(TEXT("object_path"), DataAsset->GetPathName());
	return true;
}

bool FUeAgentLegacyMechanics::CmdPCGLevelToAssetExport(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError;
		return false;
	}

	FString AssetPath;
	if (!GetStringAny(Ctx.Params, { TEXT("asset_path"), TEXT("output_asset_path") }, AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	const FString NormalizedAssetPath = NormalizeAssetPath(AssetPath);
	const FString AssetName = FPackageName::GetLongPackageAssetName(NormalizedAssetPath);
	const FString AssetFolder = FPackageName::GetLongPackagePath(NormalizedAssetPath);
	if (AssetName.IsEmpty() || AssetFolder.IsEmpty())
	{
		OutError = TEXT("invalid_asset_path");
		return false;
	}
	bool bSave = true;
	Ctx.Params->TryGetBoolField(TEXT("save"), bSave);
	bool bAllowOverwrite = false;
	Ctx.Params->TryGetBoolField(TEXT("allow_overwrite"), bAllowOverwrite);
	if (LoadAssetObject(NormalizedAssetPath) && !bAllowOverwrite)
	{
		OutError = TEXT("allow_overwrite_required");
		return false;
	}

	FPCGAssetExporterParameters Parameters;
	Parameters.bOpenSaveDialog = false;
	Parameters.AssetName = AssetName;
	Parameters.AssetPath = AssetFolder;
	Parameters.bSaveOnExportEnded = bSave;
	UPackage* ModifiedPackage = UPCGLevelToAsset::CreateOrUpdatePCGAsset(World, Parameters);
	if (!ModifiedPackage)
	{
		OutError = TEXT("level_to_asset_export_failed");
		return false;
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.level_to_asset_export.v1"));
	OutData->SetStringField(TEXT("asset_path"), NormalizedAssetPath);
	OutData->SetStringField(TEXT("package"), ModifiedPackage->GetName());
	OutData->SetBoolField(TEXT("saved"), bSave);
	OutData->SetBoolField(TEXT("allow_overwrite"), bAllowOverwrite);
	return true;
}
