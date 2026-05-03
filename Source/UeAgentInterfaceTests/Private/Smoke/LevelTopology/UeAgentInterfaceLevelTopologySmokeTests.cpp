#if WITH_DEV_AUTOMATION_TESTS

#include "../UeAgentSmokeTestSharedIncludes.inl"

#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/World.h"

namespace
{
	static bool ExecLevelTopologySmokeCommand(
		FAutomationTestBase* Test,
		const FScopedUeAgentHttpServer& ServerScope,
		const FString& RequestId,
		const FString& Command,
		const FString& ParamsJson,
		TSharedPtr<FJsonObject>& OutData,
		bool bExpectOk = true,
		double TimeoutSeconds = 30.0)
	{
		const FString Body = MakeExecRequestBody(RequestId, Command, ParamsJson);
		FHttpSmokeResult Result;
		if (!Test->TestTrue(Command + TEXT(" request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, Body, Result, TimeoutSeconds)))
		{
			return false;
		}

		if (!Test->TestEqual(Command + TEXT(" status code"), Result.StatusCode, bExpectOk ? 200 : 400))
		{
			Test->AddError(Command + TEXT(" response body: ") + Result.Body);
		}
		TSharedPtr<FJsonObject> Root;
		if (!Test->TestTrue(Command + TEXT(" response parses"), ParseJson(Result.Body, Root)) || !Root.IsValid())
		{
			return false;
		}

		bool bOk = false;
		Root->TryGetBoolField(TEXT("ok"), bOk);
		if (!Test->TestEqual(Command + TEXT(" ok flag"), bOk, bExpectOk))
		{
			Test->AddError(Command + TEXT(" response body: ") + Result.Body);
		}

		const TSharedPtr<FJsonObject>* Data = nullptr;
		if (!Test->TestTrue(Command + TEXT(" has data"), Root->TryGetObjectField(TEXT("data"), Data) && Data && Data->IsValid()))
		{
			return false;
		}
		OutData = *Data;
		return true;
	}

	static bool JsonArrayContainsStringField(const TArray<TSharedPtr<FJsonValue>>* Items, const TCHAR* FieldName, const FString& ExpectedValue)
	{
		if (!Items)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Items)
		{
			const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
			FString ActualValue;
			if (Item.IsValid() && Item->TryGetStringField(FieldName, ActualValue) && ActualValue.Equals(ExpectedValue, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	static FString SmokeJsonString(const FString& Value)
	{
		FString Escaped = Value;
		Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
		return FString::Printf(TEXT("\"%s\""), *Escaped);
	}

	static bool EnsureSmokeWorldUsesExternalObjects(FAutomationTestBase* Test)
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		ULevel* PersistentLevel = World ? World->PersistentLevel.Get() : nullptr;
		if (!Test->TestNotNull(TEXT("editor world exists"), World) || !Test->TestNotNull(TEXT("persistent level exists"), PersistentLevel))
		{
			return false;
		}
		if (!PersistentLevel->IsUsingExternalObjects())
		{
			PersistentLevel->SetUseExternalActors(true);
		}
		return Test->TestTrue(TEXT("smoke world supports DataLayer instance authoring"), PersistentLevel->IsUsingExternalObjects());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceLevelTopologyStreamingSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.LevelTopology.StreamingWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceLevelTopologyStreamingSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString FolderRoot = FString::Printf(TEXT("UAI/LevelTopology/Streaming_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString ActorId = TEXT("LT_Streaming_Cube");
	const FString ActorLabel = TEXT("LT_Streaming_Cube_Label");
	const FString StreamingLevelPath = MakeAutomationAssetPath(TEXT("LT_SubLevel"));

	TSharedPtr<FJsonObject> Data;
	const FString ContentJson = FString::Printf(
		TEXT("{\"schema\":\"uai.level_content.v1\",\"scope\":{\"folder_root\":\"%s\",\"managed_tag\":\"UAI.Managed\"},\"resources\":[{\"id\":\"%s\",\"kind\":\"static_mesh\",\"class\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"folder_path\":\"%s\",\"location\":[20,0,90],\"scale\":[0.5,0.5,0.5],\"tags\":[\"UAI.Managed\",\"LevelTopologySmoke\"],\"components\":[{\"name\":\"StaticMeshComponent\",\"properties\":[{\"path\":\"StaticMesh\",\"value_text\":\"/Engine/BasicShapes/Cube.Cube\"}]}]}]}"),
		*FolderRoot,
		*ActorId,
		*ActorLabel,
		*FolderRoot);

	TestTrue(TEXT("Create actor through LevelContent"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-stream-content-create"),
		TEXT("level_content_apply_json"),
		FString::Printf(TEXT("{\"content\":%s,\"mode\":\"upsert\",\"transaction\":true,\"preflight\":true}"), *ContentJson),
		Data,
		true,
		60.0));

	TestTrue(TEXT("Create streaming level"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-stream-create-level"),
		TEXT("level_streaming_create_level"),
		FString::Printf(TEXT("{\"level_package\":\"%s\",\"streaming_class\":\"/Script/Engine.LevelStreamingDynamic\",\"use_external_actors\":false,\"create_world_partition\":false,\"save_after_create\":false,\"transaction\":true}"), *StreamingLevelPath),
		Data,
		true,
		60.0));
	TestEqual(TEXT("created streaming level path"), Data->GetStringField(TEXT("level_package")), StreamingLevelPath);

	TestTrue(TEXT("Move actor to streaming level"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-stream-move-actors"),
		TEXT("level_streaming_move_actors"),
		FString::Printf(TEXT("{\"target_level\":\"%s\",\"actor_ids\":[\"%s\"],\"warn_about_references\":false,\"transaction\":true}"), *StreamingLevelPath, *ActorLabel),
		Data,
		true,
		60.0));
	TestTrue(TEXT("one actor moved"), Data->GetIntegerField(TEXT("moved_count")) >= 1);

	TestTrue(TEXT("Set streaming level state"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-stream-set-state"),
		TEXT("level_streaming_set_state"),
		FString::Printf(TEXT("{\"level_package\":\"%s\",\"loaded\":true,\"visible\":true,\"current\":true,\"transaction\":true}"), *StreamingLevelPath),
		Data,
		true,
		60.0));
	TestTrue(TEXT("state readback loaded"), Data->GetBoolField(TEXT("loaded")));
	TestTrue(TEXT("state readback visible"), Data->GetBoolField(TEXT("visible")));

	TestTrue(TEXT("Validate streaming level"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-stream-validate"),
		TEXT("level_streaming_validate"),
		FString::Printf(TEXT("{\"level_package\":\"%s\",\"expected_visible\":true,\"expected_loaded\":true,\"actor_ids\":[\"%s\"]}"), *StreamingLevelPath, *ActorLabel),
		Data));
	TestEqual(TEXT("streaming validate status"), Data->GetStringField(TEXT("status")), FString(TEXT("valid")));

	TestTrue(TEXT("Save streaming level"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-stream-save"),
		TEXT("level_streaming_save"),
		FString::Printf(TEXT("{\"level_package\":\"%s\"}"), *StreamingLevelPath),
		Data,
		true,
		60.0));
	TestTrue(TEXT("streaming save returned packages"), Data->GetIntegerField(TEXT("saved_count")) >= 1);

	TestTrue(TEXT("Query streaming levels"), ExecLevelTopologySmokeCommand(this, ServerScope, TEXT("lt-stream-query"), TEXT("level_streaming_query"), TEXT("{}"), Data));
	const TArray<TSharedPtr<FJsonValue>>* Levels = nullptr;
	TestTrue(TEXT("streaming levels returned"), Data->TryGetArrayField(TEXT("streaming_levels"), Levels) && Levels);
	TestTrue(TEXT("created streaming level present"), JsonArrayContainsStringField(Levels, TEXT("level_package"), StreamingLevelPath));

	TestTrue(TEXT("Remove streaming level"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-stream-remove"),
		TEXT("level_streaming_remove"),
		FString::Printf(TEXT("{\"level_package\":\"%s\",\"clear_selection\":true,\"reset_transaction_buffer\":false,\"delete_asset\":false,\"transaction\":true}"), *StreamingLevelPath),
		Data,
		true,
		60.0));
	TestTrue(TEXT("streaming level removed"), Data->GetBoolField(TEXT("removed")));

	TestTrue(TEXT("Add existing streaming level"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-stream-add-existing"),
		TEXT("level_streaming_add_existing"),
		FString::Printf(TEXT("{\"level_package\":\"%s\",\"streaming_class\":\"/Script/Engine.LevelStreamingDynamic\",\"transaction\":true}"), *StreamingLevelPath),
		Data,
		true,
		60.0));
	TestTrue(TEXT("existing streaming level added"), Data->GetBoolField(TEXT("added")));

	TestTrue(TEXT("Remove re-added streaming level"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-stream-remove-readded"),
		TEXT("level_streaming_remove"),
		FString::Printf(TEXT("{\"level_package\":\"%s\",\"clear_selection\":true,\"reset_transaction_buffer\":false,\"delete_asset\":false,\"transaction\":true}"), *StreamingLevelPath),
		Data,
		true,
		60.0));
	TestTrue(TEXT("re-added streaming level removed"), Data->GetBoolField(TEXT("removed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceLevelTopologyDataLayerSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.LevelTopology.DataLayerWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceLevelTopologyDataLayerSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}
	if (!EnsureSmokeWorldUsesExternalObjects(this))
	{
		return false;
	}

	const FString FolderRoot = FString::Printf(TEXT("UAI/LevelTopology/DataLayer_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString ActorLabel = TEXT("LT_DataLayer_Cube_Label");
	const FString AssetPath = MakeAutomationAssetPath(TEXT("DL_LT_Runtime"));

	TSharedPtr<FJsonObject> Data;
	TestTrue(TEXT("Create DataLayer asset"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-dl-create-asset"),
		TEXT("data_layer_create_asset"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"type\":\"Runtime\",\"debug_color\":\"(R=10,G=120,B=240,A=255)\",\"load_filter\":\"None\",\"overwrite_policy\":\"replace\",\"save_after_create\":false,\"transaction\":true}"), *AssetPath),
		Data,
		true,
		60.0));
	TestEqual(TEXT("DataLayer asset path"), Data->GetStringField(TEXT("asset_path")), AssetPath);

	TestTrue(TEXT("Apply DataLayer asset JSON"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-dl-apply-asset-json"),
		TEXT("data_layer_apply_asset_json"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"json\":{\"schema\":\"uai.data_layer_asset.v1\",\"asset_path\":\"%s\",\"type\":\"Runtime\",\"debug_color\":{\"r\":12,\"g\":128,\"b\":250,\"a\":255}},\"overwrite_policy\":\"replace\",\"save_after_apply\":false,\"transaction\":true}"), *AssetPath, *AssetPath),
		Data,
		true,
		60.0));
	TestEqual(TEXT("DataLayer asset apply status"), Data->GetStringField(TEXT("status")), FString(TEXT("applied")));

	TestTrue(TEXT("Create DataLayer instance"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-dl-create-instance"),
		TEXT("data_layer_create_instance"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"transaction\":true}"), *AssetPath),
		Data,
		true,
		60.0));
	const FString InstanceName = Data->GetStringField(TEXT("instance_name"));
	TestFalse(TEXT("DataLayer instance name present"), InstanceName.IsEmpty());

	const FString ContentJson = FString::Printf(
		TEXT("{\"schema\":\"uai.level_content.v1\",\"scope\":{\"folder_root\":\"%s\",\"managed_tag\":\"UAI.Managed\"},\"resources\":[{\"id\":\"LT_DL_Cube\",\"kind\":\"static_mesh\",\"class\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"folder_path\":\"%s\",\"location\":[80,0,90],\"tags\":[\"UAI.Managed\",\"LevelTopologySmoke\"],\"components\":[{\"name\":\"StaticMeshComponent\",\"properties\":[{\"path\":\"StaticMesh\",\"value_text\":\"/Engine/BasicShapes/Cube.Cube\"}]}]}]}"),
		*FolderRoot,
		*ActorLabel,
		*FolderRoot);
	TestTrue(TEXT("Create actor for DataLayer"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-dl-content-create"),
		TEXT("level_content_apply_json"),
		FString::Printf(TEXT("{\"content\":%s,\"mode\":\"upsert\",\"transaction\":true,\"preflight\":true}"), *ContentJson),
		Data,
		true,
		60.0));

	TestTrue(TEXT("Add actor to DataLayer"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-dl-membership-add"),
		TEXT("data_layer_set_actor_membership"),
		FString::Printf(TEXT("{\"actor_ids\":[\"%s\"],\"layers\":[\"%s\"],\"mode\":\"replace\",\"transaction\":true}"), *ActorLabel, *AssetPath),
		Data,
		true,
		60.0));
	TestTrue(TEXT("DataLayer add readback ok"), Data->GetIntegerField(TEXT("matched_count")) == 1 && Data->GetIntegerField(TEXT("changed_count")) >= 1);

	TestTrue(TEXT("Set DataLayer state"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-dl-set-state"),
		TEXT("data_layer_set_state"),
		FString::Printf(TEXT("{\"layer\":\"%s\",\"visible\":true,\"loaded_in_editor\":true,\"initial_runtime_state\":\"Activated\",\"initially_visible\":true,\"transaction\":true}"), *AssetPath),
		Data,
		true,
		60.0));
	TestEqual(TEXT("DataLayer state runtime readback"), Data->GetStringField(TEXT("initial_runtime_state")), FString(TEXT("Activated")));

	TestTrue(TEXT("Query DataLayers"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-dl-query"),
		TEXT("data_layer_query"),
		TEXT("{\"include_actors\":true}"),
		Data,
		true,
		60.0));
	const TArray<TSharedPtr<FJsonValue>>* DataLayers = nullptr;
	TestTrue(TEXT("DataLayer query returned instances"), Data->TryGetArrayField(TEXT("data_layers"), DataLayers) && DataLayers);
	TestTrue(TEXT("Created DataLayer query present"), JsonArrayContainsStringField(DataLayers, TEXT("instance_name"), InstanceName));

	TestTrue(TEXT("Validate DataLayer"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-dl-validate"),
		TEXT("data_layer_validate"),
		FString::Printf(TEXT("{\"layers\":[\"%s\"],\"actor_ids\":[\"%s\"],\"expect_actor_membership\":[\"%s\"]}"), *AssetPath, *ActorLabel, *AssetPath),
		Data));
	TestEqual(TEXT("DataLayer validate status"), Data->GetStringField(TEXT("status")), FString(TEXT("valid")));

	TestTrue(TEXT("Remove actor from DataLayer"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-dl-membership-clear"),
		TEXT("data_layer_set_actor_membership"),
		FString::Printf(TEXT("{\"actor_ids\":[\"%s\"],\"layers\":[\"%s\"],\"mode\":\"clear\",\"transaction\":true}"), *ActorLabel, *AssetPath),
		Data,
		true,
		60.0));

	TestTrue(TEXT("Delete DataLayer instance"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-dl-delete-instance"),
		TEXT("data_layer_delete_instance"),
		FString::Printf(TEXT("{\"layer\":\"%s\",\"delete_asset\":false,\"transaction\":true}"), *AssetPath),
		Data,
		true,
		60.0));
	TestTrue(TEXT("DataLayer deleted"), Data->GetBoolField(TEXT("deleted")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceLevelTopologyWorldPartitionSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.LevelTopology.WorldPartitionWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceLevelTopologyWorldPartitionSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	TSharedPtr<FJsonObject> Data;
	TestTrue(TEXT("World Partition query succeeds"), ExecLevelTopologySmokeCommand(this, ServerScope, TEXT("lt-wp-query"), TEXT("world_partition_query"), TEXT("{\"include_actor_desc\":true,\"actor_desc_limit\":5}"), Data));
	const bool bEnabled = Data->GetBoolField(TEXT("enabled"));

	if (!bEnabled)
	{
		TestTrue(TEXT("World Partition write fails cleanly on non-WP world"), ExecLevelTopologySmokeCommand(
			this,
			ServerScope,
			TEXT("lt-wp-load-region-nonwp"),
			TEXT("world_partition_load_region"),
			TEXT("{\"bounds\":{\"center\":[0,0,0],\"extent\":[100,100,100]},\"dry_run\":false}"),
			Data,
			false));
		TestEqual(TEXT("non-WP error code"), Data->GetStringField(TEXT("error_code")), FString(TEXT("world_partition_not_enabled")));

		TestTrue(TEXT("World Partition load actors fails cleanly on non-WP world"), ExecLevelTopologySmokeCommand(
			this,
			ServerScope,
			TEXT("lt-wp-load-actors-nonwp"),
			TEXT("world_partition_load_actors"),
			TEXT("{\"actor_guids\":[\"00000000000000000000000000000000\"],\"dry_run\":false}"),
			Data,
			false));
		TestEqual(TEXT("non-WP load actors error code"), Data->GetStringField(TEXT("error_code")), FString(TEXT("world_partition_not_enabled")));

		TestTrue(TEXT("World Partition generate streaming fails cleanly on non-WP world"), ExecLevelTopologySmokeCommand(
			this,
			ServerScope,
			TEXT("lt-wp-generate-nonwp"),
			TEXT("world_partition_generate_streaming"),
			TEXT("{\"dry_run\":true,\"allow_write\":false,\"allow_commandlet\":false}"),
			Data,
			false));
		TestEqual(TEXT("non-WP generate error code"), Data->GetStringField(TEXT("error_code")), FString(TEXT("world_partition_not_enabled")));

		TestTrue(TEXT("World Partition resave actors plan is available"), ExecLevelTopologySmokeCommand(
			this,
			ServerScope,
			TEXT("lt-wp-resave-plan-nonwp"),
			TEXT("world_partition_resave_actors"),
			TEXT("{\"execute\":false,\"allow_commandlet\":false}"),
			Data,
			true,
			60.0));
		TestEqual(TEXT("WP resave plan status"), Data->GetStringField(TEXT("status")), FString(TEXT("planned")));
		return true;
	}

	const FString FolderRoot = FString::Printf(TEXT("UAI/LevelTopology/WP_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString ActorLabel = TEXT("LT_WP_Cube_Label");
	const FString ContentJson = FString::Printf(
		TEXT("{\"schema\":\"uai.level_content.v1\",\"scope\":{\"folder_root\":\"%s\",\"managed_tag\":\"UAI.Managed\"},\"resources\":[{\"id\":\"LT_WP_Cube\",\"kind\":\"static_mesh\",\"class\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"folder_path\":\"%s\",\"location\":[160,0,90],\"tags\":[\"UAI.Managed\",\"LevelTopologySmoke\"],\"components\":[{\"name\":\"StaticMeshComponent\",\"properties\":[{\"path\":\"StaticMesh\",\"value_text\":\"/Engine/BasicShapes/Cube.Cube\"}]}]}]}"),
		*FolderRoot,
		*ActorLabel,
		*FolderRoot);
	TestTrue(TEXT("Create actor for World Partition loader"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-wp-content-create"),
		TEXT("level_content_apply_json"),
		FString::Printf(TEXT("{\"content\":%s,\"mode\":\"upsert\",\"transaction\":true,\"preflight\":true}"), *ContentJson),
		Data,
		true,
		60.0));

	TestTrue(TEXT("Set World Partition streaming enabled"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-wp-streaming-enabled"),
		TEXT("world_partition_set_streaming_enabled"),
		TEXT("{\"enabled\":true,\"transaction\":true}"),
		Data,
		true,
		60.0));
	TestTrue(TEXT("WP streaming enabled readback"), Data->GetBoolField(TEXT("streaming_enabled")));

	TestTrue(TEXT("Load World Partition region"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-wp-load-region"),
		TEXT("world_partition_load_region"),
		TEXT("{\"bounds\":{\"center\":[0,0,0],\"extent\":[512,512,512]},\"label\":\"LT_WP_Region\",\"dry_run\":false}"),
		Data,
		true,
		60.0));
	const FString LoaderId = Data->GetStringField(TEXT("loader_id"));
	TestFalse(TEXT("WP loader id present"), LoaderId.IsEmpty());
	TestTrue(TEXT("WP loader loaded"), Data->GetBoolField(TEXT("loaded")));

	TestTrue(TEXT("List UAI WP loaders"), ExecLevelTopologySmokeCommand(this, ServerScope, TEXT("lt-wp-list-loaders"), TEXT("world_partition_list_loaded_regions"), TEXT("{}"), Data));
	const TArray<TSharedPtr<FJsonValue>>* Loaders = nullptr;
	TestTrue(TEXT("WP loaders returned"), Data->TryGetArrayField(TEXT("uai_loaders"), Loaders) && Loaders);
	TestTrue(TEXT("WP loader present"), JsonArrayContainsStringField(Loaders, TEXT("loader_id"), LoaderId));

	TestTrue(TEXT("Unload World Partition region"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-wp-unload-region"),
		TEXT("world_partition_unload_region"),
		FString::Printf(TEXT("{\"loader_id\":%s}"), *SmokeJsonString(LoaderId)),
		Data,
		true,
		60.0));
	TestTrue(TEXT("WP loader unloaded"), Data->GetBoolField(TEXT("unloaded")));

	TestTrue(TEXT("Load World Partition actors"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-wp-load-actors"),
		TEXT("world_partition_load_actors"),
		FString::Printf(TEXT("{\"actor_ids\":[\"%s\"],\"dry_run\":false}"), *ActorLabel),
		Data,
		true,
		60.0));
	const FString ActorLoaderId = Data->GetStringField(TEXT("loader_id"));
	TestFalse(TEXT("WP actor loader id present"), ActorLoaderId.IsEmpty());

	TestTrue(TEXT("Unload World Partition actors"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-wp-unload-actors"),
		TEXT("world_partition_unload_actors"),
		FString::Printf(TEXT("{\"loader_id\":%s}"), *SmokeJsonString(ActorLoaderId)),
		Data,
		true,
		60.0));
	TestTrue(TEXT("WP actor loader unloaded"), Data->GetBoolField(TEXT("unloaded")));

	TestTrue(TEXT("World Partition check errors returns structure"), ExecLevelTopologySmokeCommand(this, ServerScope, TEXT("lt-wp-check-errors"), TEXT("world_partition_check_errors"), TEXT("{\"report_only\":true}"), Data, true, 60.0));
	TestTrue(TEXT("WP check has issues array"), Data->HasField(TEXT("issues")));

	TestTrue(TEXT("World Partition generate streaming dry run"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-wp-generate-plan"),
		TEXT("world_partition_generate_streaming"),
		TEXT("{\"dry_run\":true,\"allow_write\":false,\"allow_commandlet\":false}"),
		Data,
		true,
		60.0));
	TestEqual(TEXT("WP generate plan status"), Data->GetStringField(TEXT("status")), FString(TEXT("planned")));

	TestTrue(TEXT("World Partition resave actors dry run"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-wp-resave-plan"),
		TEXT("world_partition_resave_actors"),
		TEXT("{\"execute\":false,\"allow_commandlet\":false}"),
		Data,
		true,
		60.0));
	TestEqual(TEXT("WP resave plan status"), Data->GetStringField(TEXT("status")), FString(TEXT("planned")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceLevelTopologyHLODSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.LevelTopology.HLODWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceLevelTopologyHLODSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString FolderRoot = FString::Printf(TEXT("UAI/LevelTopology/HLOD_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString ActorLabel = TEXT("LT_HLOD_Cube_Label");
	const FString HLODLayerPath = MakeAutomationAssetPath(TEXT("HLOD_LT_Layer"));

	TSharedPtr<FJsonObject> Data;
	TestTrue(TEXT("Create HLOD layer"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-hlod-create-layer"),
		TEXT("hlod_layer_create_json"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"json\":{\"schema\":\"uai.hlod_layer.v1\",\"asset_path\":\"%s\",\"layer_type\":\"Instancing\",\"is_spatially_loaded\":true,\"cell_size\":256,\"loading_range\":1024},\"overwrite_policy\":\"replace\",\"save_after_apply\":false,\"transaction\":true}"), *HLODLayerPath, *HLODLayerPath),
		Data,
		true,
		60.0));
	TestEqual(TEXT("HLOD layer asset path"), Data->GetStringField(TEXT("asset_path")), HLODLayerPath);

	TestTrue(TEXT("Export HLOD layer"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-hlod-export-layer"),
		TEXT("hlod_layer_export_json"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *HLODLayerPath),
		Data));
	TestEqual(TEXT("HLOD layer type readback"), Data->GetStringField(TEXT("layer_type")), FString(TEXT("Instancing")));

	TestTrue(TEXT("Validate HLOD layer JSON"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-hlod-validate-layer"),
		TEXT("hlod_layer_validate_json"),
		FString::Printf(TEXT("{\"json\":{\"schema\":\"uai.hlod_layer.v1\",\"asset_path\":\"%s\",\"layer_type\":\"Instancing\",\"is_spatially_loaded\":true}}"), *HLODLayerPath),
		Data,
		true,
		60.0));
	TestEqual(TEXT("HLOD layer validate status"), Data->GetStringField(TEXT("status")), FString(TEXT("valid")));

	TestTrue(TEXT("Validate HLOD layer rejects invalid asset path"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-hlod-validate-layer-invalid-path"),
		TEXT("hlod_layer_validate_json"),
		TEXT("{\"json\":{\"schema\":\"uai.hlod_layer.v1\",\"asset_path\":\"Not/A/Package\",\"layer_type\":\"Instancing\"}}"),
		Data,
		false,
		60.0));
	const TArray<TSharedPtr<FJsonValue>>* InvalidHLODValidateIssues = nullptr;
	Data->TryGetArrayField(TEXT("issues"), InvalidHLODValidateIssues);
	TestTrue(TEXT("HLOD invalid asset path issue"), JsonArrayContainsStringField(InvalidHLODValidateIssues, TEXT("code"), TEXT("hlod_layer_path_invalid")));

	TestTrue(TEXT("Apply HLOD layer JSON"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-hlod-apply-layer"),
		TEXT("hlod_layer_apply_json"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"json\":{\"schema\":\"uai.hlod_layer.v1\",\"asset_path\":\"%s\",\"layer_type\":\"Instancing\",\"is_spatially_loaded\":true,\"cell_size\":384,\"loading_range\":1536},\"overwrite_policy\":\"replace\",\"save_after_apply\":false,\"transaction\":true}"), *HLODLayerPath, *HLODLayerPath),
		Data,
		true,
		60.0));
	TestEqual(TEXT("HLOD layer apply status"), Data->GetStringField(TEXT("status")), FString(TEXT("applied")));

	const FString ContentJson = FString::Printf(
		TEXT("{\"schema\":\"uai.level_content.v1\",\"scope\":{\"folder_root\":\"%s\",\"managed_tag\":\"UAI.Managed\"},\"resources\":[{\"id\":\"LT_HLOD_Cube\",\"kind\":\"static_mesh\",\"class\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"folder_path\":\"%s\",\"location\":[120,0,90],\"tags\":[\"UAI.Managed\",\"LevelTopologySmoke\"],\"components\":[{\"name\":\"StaticMeshComponent\",\"properties\":[{\"path\":\"StaticMesh\",\"value_text\":\"/Engine/BasicShapes/Cube.Cube\"}]}]}]}"),
		*FolderRoot,
		*ActorLabel,
		*FolderRoot);
	TestTrue(TEXT("Create actor for HLOD"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-hlod-content-create"),
		TEXT("level_content_apply_json"),
		FString::Printf(TEXT("{\"content\":%s,\"mode\":\"upsert\",\"transaction\":true,\"preflight\":true}"), *ContentJson),
		Data,
		true,
		60.0));

	TestTrue(TEXT("Assign HLOD layer"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-hlod-assign"),
		TEXT("hlod_assign_actors"),
		FString::Printf(TEXT("{\"actor_ids\":[\"%s\"],\"hlod_layer\":\"%s\",\"runtime_grid\":\"LTGrid\",\"is_spatially_loaded\":true,\"transaction\":true}"), *ActorLabel, *HLODLayerPath),
		Data,
		true,
		60.0));
	TestEqual(TEXT("HLOD assigned actor count"), Data->GetIntegerField(TEXT("matched_count")), 1);

	TestTrue(TEXT("Query HLOD assignment"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-hlod-query-actors"),
		TEXT("hlod_query_actors"),
		FString::Printf(TEXT("{\"actor_ids\":[\"%s\"]}"), *ActorLabel),
		Data));
	const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
	TestTrue(TEXT("HLOD actor query returned"), Data->TryGetArrayField(TEXT("actors"), Actors) && Actors && Actors->Num() == 1);

	TestTrue(TEXT("Set topology actor assignments directly"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-topology-set-assignments"),
		TEXT("level_topology_set_actor_assignments"),
		FString::Printf(TEXT("{\"actor_ids\":[\"%s\"],\"assignments\":{\"hlod_layer\":\"%s\",\"world_partition\":{\"runtime_grid\":\"LTGrid2\",\"is_spatially_loaded\":true}}}"), *ActorLabel, *HLODLayerPath),
		Data,
		true,
		60.0));
	TestTrue(TEXT("topology direct assignment changed"), Data->GetBoolField(TEXT("changed")));

	TestTrue(TEXT("Validate topology JSON"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-topology-validate"),
		TEXT("level_topology_validate_json"),
		TEXT("{\"json\":{\"schema\":\"uai.level_topology.v1\",\"streaming_levels\":[{\"level_package\":\"/Game/UAI/Automation/LevelTopology/LT_Diff_Level\"}],\"data_layers\":[{\"asset_path\":\"/Game/UAI/Automation/LevelTopology/DL_Validate\"}]}}"),
		Data,
		true,
		60.0));
	TestEqual(TEXT("topology validate status"), Data->GetStringField(TEXT("status")), FString(TEXT("valid")));

	TestTrue(TEXT("Diff topology JSON"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-topology-diff"),
		TEXT("level_topology_diff_json"),
		TEXT("{\"json\":{\"schema\":\"uai.level_topology.v1\",\"streaming_levels\":[{\"level_package\":\"/Game/UAI/Automation/LevelTopology/LT_Diff_Level\"}]}}"),
		Data,
		true,
		60.0));
	TestTrue(TEXT("topology diff detected missing streaming level"), Data->GetBoolField(TEXT("changed")) && Data->GetIntegerField(TEXT("diff_count")) >= 1);

	TestTrue(TEXT("Apply topology JSON actor assignment"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-topology-apply"),
		TEXT("level_topology_apply_json"),
		FString::Printf(TEXT("{\"json\":{\"schema\":\"uai.level_topology.v1\",\"actor_assignments\":[{\"actor_ids\":[\"%s\"],\"assignments\":{\"hlod_layer\":\"%s\",\"world_partition\":{\"runtime_grid\":\"LTApplyGrid\",\"is_spatially_loaded\":true}}}]}}"), *ActorLabel, *HLODLayerPath),
		Data,
		true,
		60.0));
	TestEqual(TEXT("topology apply status"), Data->GetStringField(TEXT("status")), FString(TEXT("applied")));

	TestTrue(TEXT("HLOD build report path is guarded"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-hlod-build-report"),
		TEXT("hlod_build"),
		TEXT("{\"step\":\"stats\",\"execute\":false,\"report_only\":true,\"allow_commandlet\":false,\"allow_rhi\":false}"),
		Data,
		true,
		60.0));
	TestEqual(TEXT("HLOD build report status"), Data->GetStringField(TEXT("status")), FString(TEXT("planned")));

	TestTrue(TEXT("HLOD validate build returns structure"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-hlod-validate-build"),
		TEXT("hlod_validate_build"),
		TEXT("{}"),
		Data,
		true,
		60.0));
	TestEqual(TEXT("HLOD validate build status"), Data->GetStringField(TEXT("status")), FString(TEXT("checked")));

	TestTrue(TEXT("Topology export includes systems"), ExecLevelTopologySmokeCommand(
		this,
		ServerScope,
		TEXT("lt-topology-export"),
		TEXT("level_topology_export_json"),
		TEXT("{\"include_actor_assignments\":true,\"actor_assignment_limit\":10}"),
		Data,
		true,
		60.0));
	TestEqual(TEXT("topology schema"), Data->GetStringField(TEXT("schema")), FString(TEXT("uai.level_topology.v1")));
	TestTrue(TEXT("topology has feature gates"), Data->HasField(TEXT("feature_gates")));
	return true;
}

#endif
