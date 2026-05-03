#if WITH_DEV_AUTOMATION_TESTS

#include "../UeAgentSmokeTestSharedIncludes.inl"

namespace
{
	static bool ExecProductionPipelineSmokeCommand(
		FAutomationTestBase* Test,
		const FScopedUeAgentHttpServer& ServerScope,
		const FString& RequestId,
		const FString& Command,
		const FString& ParamsJson,
		const int32 ExpectedStatusCode,
		TSharedPtr<FJsonObject>& OutRoot,
		TSharedPtr<FJsonObject>& OutData,
		const double TimeoutSeconds = 30.0)
	{
		const FString Body = MakeExecRequestBody(RequestId, Command, ParamsJson);
		FHttpSmokeResult Result;
		const bool bRequestOk = ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, Body, Result, TimeoutSeconds);
		Test->TestTrue(FString::Printf(TEXT("%s request completed"), *RequestId), bRequestOk);
		Test->TestEqual(FString::Printf(TEXT("%s status code"), *RequestId), Result.StatusCode, ExpectedStatusCode);
		if (Result.StatusCode != ExpectedStatusCode)
		{
			Test->AddError(FString::Printf(TEXT("%s response body: %s"), *RequestId, *Result.Body));
		}
		Test->TestTrue(FString::Printf(TEXT("%s response parses as JSON"), *RequestId), ParseJson(Result.Body, OutRoot));
		const bool bExpectOk = ExpectedStatusCode >= 200 && ExpectedStatusCode < 300;
		Test->TestTrue(FString::Printf(TEXT("%s ok flag matches status"), *RequestId), OutRoot.IsValid() && OutRoot->GetBoolField(TEXT("ok")) == bExpectOk);
		OutData.Reset();
		const TSharedPtr<FJsonObject>* DataObject = nullptr;
		if (OutRoot.IsValid() && OutRoot->TryGetObjectField(TEXT("data"), DataObject) && DataObject && DataObject->IsValid())
		{
			OutData = *DataObject;
		}
		return bRequestOk && Result.StatusCode == ExpectedStatusCode && OutRoot.IsValid();
	}

	static bool WriteProductionSmokeTextFile(FAutomationTestBase* Test, const FString& FilePath, const FString& Text)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
		const bool bSaved = FFileHelper::SaveStringToFile(Text, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		Test->TestTrue(FString::Printf(TEXT("write text file %s"), *FilePath), bSaved);
		return bSaved;
	}

	static FString ProductionSmokeJsonPath(const FString& FileName)
	{
		return ToSmokeJsonPath(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("UAIProductionPipeline"), FileName));
	}

	static bool JsonIssuesContainCode(const TSharedPtr<FJsonObject>& Data, const FString& Code)
	{
		if (!Data.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Issues = nullptr;
		if (!Data->TryGetArrayField(TEXT("issues"), Issues) || !Issues)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& IssueValue : *Issues)
		{
			const TSharedPtr<FJsonObject> Issue = IssueValue.IsValid() ? IssueValue->AsObject() : nullptr;
			FString IssueCode;
			if (Issue.IsValid() && Issue->TryGetStringField(TEXT("code"), IssueCode) && IssueCode.Equals(Code, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceProductionPipelineLocalizationSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.ProductionPipeline.LocalizationAndStringTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceProductionPipelineLocalizationSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString StringTablePath = MakeAutomationAssetPath(TEXT("ST_ProductionPipeline"));
	const FString StringTableJson = FString::Printf(
		TEXT("{\"schema\":\"uai.string_table.v1\",\"asset_path\":\"%s\",\"namespace\":\"UAIProductionPipeline\",\"entries\":[{\"key\":\"Start\",\"source\":\"Start Game\",\"metadata\":{\"context\":\"menu\"}},{\"key\":\"Quit\",\"source\":\"Quit\"}]}"),
		*StringTablePath);
	const FString BadStringTableJson = FString::Printf(
		TEXT("{\"schema\":\"uai.string_table.v1\",\"asset_path\":\"%s\",\"entries\":[{\"key\":\"Dup\",\"source\":\"A\"},{\"key\":\"Dup\"}]}"),
		*StringTablePath);
	const FString ExportFile = ProductionSmokeJsonPath(TEXT("string_table_export.json"));
	const FString PoFile = ProductionSmokeJsonPath(TEXT("smoke.po"));
	const FString BadPoFile = ProductionSmokeJsonPath(TEXT("bad.po"));
	WriteProductionSmokeTextFile(this, PoFile, TEXT("msgid \"Start Game\"\nmsgstr \"Start Game\"\n"));
	WriteProductionSmokeTextFile(this, BadPoFile, TEXT("this is not a po file\n"));
	const FString PoExportTarget = TEXT("UAIProductionPipelinePoExportSmoke");
	const FString PoExportTargetDir = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Localization"), PoExportTarget);
	const FString PoExportOutputDir = ProductionSmokeJsonPath(TEXT("po_export_preserve_cultures"));
	IFileManager::Get().DeleteDirectory(*PoExportTargetDir, false, true);
	IFileManager::Get().DeleteDirectory(*PoExportOutputDir, false, true);
	WriteProductionSmokeTextFile(this, FPaths::Combine(PoExportTargetDir, TEXT("en"), TEXT("Game.po")), TEXT("msgid \"Start\"\nmsgstr \"Start\"\n"));
	WriteProductionSmokeTextFile(this, FPaths::Combine(PoExportTargetDir, TEXT("zh-Hans"), TEXT("Game.po")), TEXT("msgid \"Start\"\nmsgstr \"开始\"\n"));

	TSharedPtr<FJsonObject> Root;
	TSharedPtr<FJsonObject> Data;
	TestTrue(TEXT("string_table_apply_json creates asset"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-st-apply-001"), TEXT("string_table_apply_json"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"json\":%s,\"backup\":false,\"save_after_apply\":false}"), *StringTablePath, *StringTableJson),
		200, Root, Data));
	TestEqual(TEXT("string table changed count"), static_cast<int32>(Data->GetIntegerField(TEXT("changed_count"))), 2);

	TestTrue(TEXT("string_table_export_json reads back entries"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-st-export-001"), TEXT("string_table_export_json"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"output_file\":\"%s\"}"), *StringTablePath, *ExportFile),
		200, Root, Data));
	TestTrue(TEXT("string table export file exists"), IFileManager::Get().FileExists(*ExportFile));
	TestEqual(TEXT("string table exported two entries"), static_cast<int32>(Data->GetIntegerField(TEXT("entry_count"))), 2);

	TestTrue(TEXT("string_table_validate_json accepts valid schema"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-st-validate-001"), TEXT("string_table_validate_json"),
		FString::Printf(TEXT("{\"json\":%s}"), *StringTableJson),
		200, Root, Data));

	TestTrue(TEXT("string_table_validate_json rejects duplicate key"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-st-validate-bad-001"), TEXT("string_table_validate_json"),
		FString::Printf(TEXT("{\"json\":%s}"), *BadStringTableJson),
		400, Root, Data));

	TestTrue(TEXT("string_table_diff_json succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-st-diff-001"), TEXT("string_table_diff_json"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"json\":%s}"), *StringTablePath, *StringTableJson),
		200, Root, Data));

	TestTrue(TEXT("localization_query_targets succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-loc-query-001"), TEXT("localization_query_targets"),
		TEXT("{\"include_stats\":true,\"include_files\":true}"),
		200, Root, Data));

	const FString LocalizationJson = FString::Printf(
		TEXT("{\"schema\":\"uai.localization.v1\",\"targets\":[{\"name\":\"UAIProductionPipeline\",\"native_culture\":\"en\",\"cultures\":[\"en\"]}],\"string_tables\":[%s]}"),
		*StringTableJson);
	const FString SettingsOnlyLocalizationJson = TEXT("{\"schema\":\"uai.localization.v1\",\"settings\":{\"Localization\":{\"NativeCulture\":\"en\"}}}");
	const FString LocalizationExportFile = ProductionSmokeJsonPath(TEXT("localization_export.json"));
	TestTrue(TEXT("localization_export_json succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-loc-export-001"), TEXT("localization_export_json"),
		FString::Printf(TEXT("{\"output_file\":\"%s\",\"include_entries\":true,\"include_word_count\":true}"), *LocalizationExportFile),
		200, Root, Data));

	TestTrue(TEXT("localization_validate_json succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-loc-validate-001"), TEXT("localization_validate_json"),
		FString::Printf(TEXT("{\"json\":%s,\"strict\":true}"), *LocalizationJson),
		200, Root, Data));

	TestTrue(TEXT("localization_diff_json succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-loc-diff-001"), TEXT("localization_diff_json"),
		FString::Printf(TEXT("{\"json\":%s}"), *LocalizationJson),
		200, Root, Data));

	TestTrue(TEXT("localization_apply_json dry run succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-loc-apply-001"), TEXT("localization_apply_json"),
		FString::Printf(TEXT("{\"json\":%s,\"dry_run\":true,\"backup\":false,\"save_policy\":\"config_only\"}"), *LocalizationJson),
		200, Root, Data));

	TestTrue(TEXT("localization_apply_json rejects project settings-only input"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-loc-apply-settings-only-001"), TEXT("localization_apply_json"),
		FString::Printf(TEXT("{\"json\":%s,\"dry_run\":true,\"backup\":false}"), *SettingsOnlyLocalizationJson),
		400, Root, Data));
	TestTrue(TEXT("localization_apply_json reports project settings handoff"), JsonIssuesContainCode(Data, TEXT("use_project_settings_required")));

	TestTrue(TEXT("localization_gather_text plan succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-loc-gather-001"), TEXT("localization_gather_text"),
		TEXT("{\"target\":\"UAIProductionPipeline\",\"execute\":false,\"allow_commandlet\":false,\"timeout_seconds\":5}"),
		200, Root, Data));

	TestTrue(TEXT("localization_export_po plan succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-loc-export-po-001"), TEXT("localization_export_po"),
		TEXT("{\"target\":\"UAIProductionPipeline\",\"cultures\":[\"en\"],\"execute\":false}"),
		200, Root, Data));

	TestTrue(TEXT("localization_import_po dry run succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-loc-import-po-001"), TEXT("localization_import_po"),
		FString::Printf(TEXT("{\"target\":\"UAIProductionPipeline\",\"culture\":\"en\",\"po_file\":\"%s\",\"dry_run\":true,\"backup\":false}"), *PoFile),
		200, Root, Data));

	TestTrue(TEXT("localization_import_po rejects unparseable PO"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-loc-import-po-bad-001"), TEXT("localization_import_po"),
		FString::Printf(TEXT("{\"target\":\"UAIProductionPipeline\",\"culture\":\"en\",\"po_file\":\"%s\",\"dry_run\":true,\"backup\":false}"), *BadPoFile),
		400, Root, Data));

	TestTrue(TEXT("localization_export_po execute preserves culture folders"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-loc-export-po-preserve-culture-001"), TEXT("localization_export_po"),
		FString::Printf(TEXT("{\"target\":\"%s\",\"cultures\":[\"en\",\"zh-Hans\"],\"output_dir\":\"%s\",\"execute\":true}"), *PoExportTarget, *PoExportOutputDir),
		200, Root, Data));
	TestTrue(TEXT("localization_export_po wrote en/Game.po"), IFileManager::Get().FileExists(*FPaths::Combine(PoExportOutputDir, TEXT("en"), TEXT("Game.po"))));
	TestTrue(TEXT("localization_export_po wrote zh-Hans/Game.po"), IFileManager::Get().FileExists(*FPaths::Combine(PoExportOutputDir, TEXT("zh-Hans"), TEXT("Game.po"))));

	TestTrue(TEXT("localization_compile_text plan succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-loc-compile-001"), TEXT("localization_compile_text"),
		TEXT("{\"target\":\"UAIProductionPipeline\",\"cultures\":[\"en\"],\"execute\":false,\"timeout_seconds\":5}"),
		200, Root, Data));

	TestTrue(TEXT("localization_report succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-loc-report-001"), TEXT("localization_report"),
		TEXT("{\"target\":\"UAIProductionPipeline\",\"cultures\":[\"en\"],\"include_assets\":false}"),
		200, Root, Data));

	TestTrue(TEXT("localization_preview_culture succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-loc-preview-001"), TEXT("localization_preview_culture"),
		TEXT("{\"culture\":\"en\",\"scope\":\"session\",\"restore_after\":true}"),
		200, Root, Data));

	IFileManager::Get().DeleteDirectory(*PoExportTargetDir, false, true);
	IFileManager::Get().DeleteDirectory(*PoExportOutputDir, false, true);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceProductionPipelinePackagingSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.ProductionPipeline.Packaging",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceProductionPipelinePackagingSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString ProfileJson = FString::Printf(
		TEXT("{\"schema\":\"uai.packaging.v1\",\"target\":{\"platform\":\"Win64\",\"configuration\":\"Development\"},\"pipeline\":{\"build\":false,\"cook\":true,\"stage\":true,\"package\":true,\"archive\":true,\"pak\":true,\"iostore\":false},\"content\":{\"maps\":[],\"cultures\":[\"en\"]},\"output\":{\"archive_directory\":\"%s\"},\"options\":{\"clean\":false,\"compressed\":true,\"prereqs\":false}}"),
		*ToSmokeJsonPath(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAgentInterface"), TEXT("Packages"), TEXT("SmokeWin64"))));
	const FString InvalidProfileJson = TEXT("{\"schema\":\"uai.packaging.v1\",\"target\":{\"platform\":\"Win64\",\"configuration\":\"Development\"},\"content\":{\"maps\":[\"/Game/__MissingMap\"]},\"output\":{\"archive_directory\":\"Saved/UeAgentInterface/Packages/SmokeWin64\"}}");
	const FString ProfileExportFile = ProductionSmokeJsonPath(TEXT("packaging_profile.json"));
	const FString LogFile = ProductionSmokeJsonPath(TEXT("uat_fixture.log"));
	WriteProductionSmokeTextFile(this, LogFile, TEXT("LogCook: Warning: Fixture warning\nAutomationTool exiting with ExitCode=1 (Error_Unknown)\n"));

	TSharedPtr<FJsonObject> Root;
	TSharedPtr<FJsonObject> Data;
	TestTrue(TEXT("packaging_query_environment succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-pack-env-001"), TEXT("packaging_query_environment"),
		TEXT("{\"include_sdks\":true,\"include_tools\":true}"),
		200, Root, Data));

	TestTrue(TEXT("packaging_export_profile_json succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-pack-export-001"), TEXT("packaging_export_profile_json"),
		FString::Printf(TEXT("{\"platform\":\"Win64\",\"configuration\":\"Development\",\"include_project_settings\":true,\"output_file\":\"%s\"}"), *ProfileExportFile),
		200, Root, Data));

	TestTrue(TEXT("packaging_validate_profile_json rejects missing map"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-pack-validate-bad-001"), TEXT("packaging_validate_profile_json"),
		FString::Printf(TEXT("{\"json\":%s}"), *InvalidProfileJson),
		400, Root, Data));

	TestTrue(TEXT("packaging_validate_profile_json accepts minimal profile"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-pack-validate-001"), TEXT("packaging_validate_profile_json"),
		FString::Printf(TEXT("{\"json\":%s}"), *ProfileJson),
		200, Root, Data));

	TestTrue(TEXT("packaging_diff_profile_json succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-pack-diff-001"), TEXT("packaging_diff_profile_json"),
		FString::Printf(TEXT("{\"json\":%s}"), *ProfileJson),
		200, Root, Data));

	TestTrue(TEXT("packaging_plan_build succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-pack-plan-001"), TEXT("packaging_plan_build"),
		FString::Printf(TEXT("{\"json\":%s,\"cook\":true,\"stage\":true,\"package\":true,\"archive\":true}"), *ProfileJson),
		200, Root, Data));

	TestTrue(TEXT("packaging_run_build plan succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-pack-run-plan-001"), TEXT("packaging_run_build"),
		FString::Printf(TEXT("{\"json\":%s,\"execute\":false,\"allow_uat\":false,\"timeout_seconds\":5}"), *ProfileJson),
		200, Root, Data));
	const FString TaskId = Data.IsValid() ? Data->GetStringField(TEXT("task_id")) : FString();
	TestTrue(TEXT("packaging_run_build returns task id"), !TaskId.IsEmpty());

	TestTrue(TEXT("packaging_run_build rejects invalid profile before task registration"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-pack-run-invalid-001"), TEXT("packaging_run_build"),
		FString::Printf(TEXT("{\"json\":%s,\"execute\":false,\"allow_uat\":false,\"timeout_seconds\":5}"), *InvalidProfileJson),
		400, Root, Data));
	TestTrue(TEXT("packaging_run_build reports map_not_found"), JsonIssuesContainCode(Data, TEXT("map_not_found")));
	TestFalse(TEXT("packaging_run_build invalid profile does not register task"), Data.IsValid() && Data->HasField(TEXT("task_id")));

	TestTrue(TEXT("packaging_task_status succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-pack-status-001"), TEXT("packaging_task_status"),
		FString::Printf(TEXT("{\"task_id\":\"%s\"}"), *TaskId),
		200, Root, Data));

	TestTrue(TEXT("packaging_cancel_task succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-pack-cancel-001"), TEXT("packaging_cancel_task"),
		FString::Printf(TEXT("{\"task_id\":\"%s\",\"kill_process_tree\":true}"), *TaskId),
		200, Root, Data));

	TestTrue(TEXT("packaging_parse_log succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-pack-parse-log-001"), TEXT("packaging_parse_log"),
		FString::Printf(TEXT("{\"log_file\":\"%s\",\"classify_errors\":true}"), *LogFile),
		200, Root, Data));

	TestTrue(TEXT("packaging_report_artifacts succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-pack-artifacts-001"), TEXT("packaging_report_artifacts"),
		TEXT("{\"archive_dir\":\"Saved/UeAgentInterface/Packages/SmokeWin64\",\"platform\":\"Win64\",\"include_hashes\":false}"),
		200, Root, Data));

	TestTrue(TEXT("packaging_clean_artifacts dry run succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-pack-clean-001"), TEXT("packaging_clean_artifacts"),
		TEXT("{\"archive_dir\":\"Saved/UeAgentInterface/Packages/SmokeWin64\",\"dry_run\":true,\"max_delete_bytes\":1048576}"),
		200, Root, Data));

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceProductionPipelinePlatformProfilesSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.ProductionPipeline.PlatformProfiles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceProductionPipelinePlatformProfilesSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString DeviceProfileJson = TEXT("{\"schema\":\"uai.device_profile.v1\",\"profiles\":[{\"name\":\"UAIProductionPipeline\",\"base_profile\":\"Windows\",\"device_type\":\"Windows\",\"cvars\":{\"sg.ViewDistanceQuality\":\"3\",\"r.Streaming.PoolSize\":\"1500\"}}]}");
	const FString ScalabilityJson = TEXT("{\"schema\":\"uai.scalability.v1\",\"groups\":[{\"section\":\"ViewDistanceQuality@0\",\"values\":{\"r.ViewDistanceScale\":\"0.4\"}}]}");
	const FString PlatformConfigJson = TEXT("{\"schema\":\"uai.platform_config.v1\",\"platform\":\"Windows\",\"files\":[{\"file\":\"DefaultEngine.ini\",\"sections\":[{\"name\":\"/Script/Engine.Engine\",\"values\":{\"NearClipPlane\":\"10.0\"}}]}]}");
	const FString BadPlatformConfigJson = TEXT("{\"schema\":\"uai.platform_config.v1\",\"platform\":\"Windows\",\"files\":[{\"file\":\"../UnsafeEngine.ini\",\"sections\":[{\"name\":\"/Script/Engine.Engine\",\"values\":{\"NearClipPlane\":\"10.0\"}}]}]}");
	const FString PackagingJson = TEXT("{\"schema\":\"uai.packaging.v1\",\"target\":{\"platform\":\"Win64\",\"configuration\":\"Development\"},\"content\":{\"maps\":[]},\"output\":{\"archive_directory\":\"Saved/UeAgentInterface/Packages/SmokeWin64\"}}");

	TSharedPtr<FJsonObject> Root;
	TSharedPtr<FJsonObject> Data;
	TestTrue(TEXT("target_platform_query succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-platform-query-001"), TEXT("target_platform_query"),
		TEXT("{\"include_sdk\":true,\"include_texture_formats\":true}"),
		200, Root, Data));

	TestTrue(TEXT("platform_config_export_json succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-platform-config-export-001"), TEXT("platform_config_export_json"),
		TEXT("{\"platform\":\"Windows\",\"sections\":[\"/Script/Engine.Engine\"],\"include_sources\":true}"),
		200, Root, Data));

	TestTrue(TEXT("platform_config_validate_json succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-platform-config-validate-001"), TEXT("platform_config_validate_json"),
		FString::Printf(TEXT("{\"json\":%s}"), *PlatformConfigJson),
		200, Root, Data));

	TestTrue(TEXT("platform_config_validate_json rejects traversal path"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-platform-config-validate-bad-path-001"), TEXT("platform_config_validate_json"),
		FString::Printf(TEXT("{\"json\":%s}"), *BadPlatformConfigJson),
		400, Root, Data));
	TestTrue(TEXT("platform_config_validate_json reports file allow-list violation"), JsonIssuesContainCode(Data, TEXT("platform_config_file_not_allowed")));

	TestTrue(TEXT("platform_config_apply_json dry run succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-platform-config-apply-001"), TEXT("platform_config_apply_json"),
		FString::Printf(TEXT("{\"json\":%s,\"dry_run\":true,\"backup\":false}"), *PlatformConfigJson),
		200, Root, Data));

	TestTrue(TEXT("device_profile_query succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-device-query-001"), TEXT("device_profile_query"),
		TEXT("{\"include_inherited\":true}"),
		200, Root, Data));

	TestTrue(TEXT("device_profile_export_json succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-device-export-001"), TEXT("device_profile_export_json"),
		TEXT("{\"include_inherited\":true}"),
		200, Root, Data));

	TestTrue(TEXT("device_profile_validate_json succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-device-validate-001"), TEXT("device_profile_validate_json"),
		FString::Printf(TEXT("{\"json\":%s}"), *DeviceProfileJson),
		200, Root, Data));

	TestTrue(TEXT("device_profile_diff_json succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-device-diff-001"), TEXT("device_profile_diff_json"),
		FString::Printf(TEXT("{\"json\":%s}"), *DeviceProfileJson),
		200, Root, Data));

	TestTrue(TEXT("device_profile_apply_json dry run succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-device-apply-001"), TEXT("device_profile_apply_json"),
		FString::Printf(TEXT("{\"json\":%s,\"dry_run\":true,\"backup\":false,\"read_back\":true}"), *DeviceProfileJson),
		200, Root, Data));

	TestTrue(TEXT("device_profile_resolve_cvars succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-device-resolve-001"), TEXT("device_profile_resolve_cvars"),
		TEXT("{\"profile\":\"Windows\",\"keys\":[\"sg.ViewDistanceQuality\"],\"include_sources\":true}"),
		200, Root, Data));

	TestTrue(TEXT("scalability_export_json succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-scalability-export-001"), TEXT("scalability_export_json"),
		TEXT("{\"platform\":\"Windows\"}"),
		200, Root, Data));
	const TArray<TSharedPtr<FJsonValue>>* QualityLevels = nullptr;
	TestTrue(TEXT("scalability_export_json returns quality_levels"), Data.IsValid() && Data->TryGetArrayField(TEXT("quality_levels"), QualityLevels) && QualityLevels && QualityLevels->Num() > 0);

	TestTrue(TEXT("scalability_validate_json succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-scalability-validate-001"), TEXT("scalability_validate_json"),
		FString::Printf(TEXT("{\"json\":%s}"), *ScalabilityJson),
		200, Root, Data));

	TestTrue(TEXT("scalability_apply_json dry run succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-scalability-apply-001"), TEXT("scalability_apply_json"),
		FString::Printf(TEXT("{\"json\":%s,\"dry_run\":true,\"backup\":false}"), *ScalabilityJson),
		200, Root, Data));

	TestTrue(TEXT("platform_profile_validate_packaging succeeds"), ExecProductionPipelineSmokeCommand(
		this, ServerScope, TEXT("smoke-production-platform-profile-validate-001"), TEXT("platform_profile_validate_packaging"),
		FString::Printf(TEXT("{\"packaging_json\":%s,\"platform\":\"Win64\",\"device_profile\":\"Windows\"}"), *PackagingJson),
		200, Root, Data));

	return !HasAnyErrors();
}

#endif
