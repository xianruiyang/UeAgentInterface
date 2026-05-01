#if WITH_DEV_AUTOMATION_TESTS

#include "UeAgentSmokeTestSharedIncludes.inl"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfacePingStatusSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.PingStatus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfacePingStatusSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	FHttpSmokeResult PingResult;
	TestTrue(TEXT("Ping request succeeded"), ExecuteHttpJsonRequest(TEXT("GET"), ServerScope.BaseUrl + TEXT("/api/ping"), FString(), FString(), PingResult));
	TestEqual(TEXT("Ping status code"), PingResult.StatusCode, 200);

	TSharedPtr<FJsonObject> PingJson;
	TestTrue(TEXT("Ping response parses as JSON"), ParseJson(PingResult.Body, PingJson));
	TestTrue(TEXT("Ping response ok=true"), PingJson.IsValid() && PingJson->GetBoolField(TEXT("ok")));

	FHttpSmokeResult StatusResult;
	TestTrue(TEXT("Status request succeeded"), ExecuteHttpJsonRequest(TEXT("GET"), ServerScope.BaseUrl + TEXT("/api/status"), ServerScope.Token, FString(), StatusResult));
	TestEqual(TEXT("Status status code"), StatusResult.StatusCode, 200);

	TSharedPtr<FJsonObject> StatusJson;
	TestTrue(TEXT("Status response parses as JSON"), ParseJson(StatusResult.Body, StatusJson));
	TestTrue(TEXT("Status root ok=true"), StatusJson.IsValid() && StatusJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* StatusData = nullptr;
	TestTrue(TEXT("Status contains data object"), StatusJson.IsValid() && StatusJson->TryGetObjectField(TEXT("data"), StatusData) && StatusData && StatusData->IsValid());
	if (StatusData && StatusData->IsValid())
	{
		TestTrue(TEXT("Status data running=true"), (*StatusData)->GetBoolField(TEXT("running")));
		TestEqual(TEXT("Status data port matches settings"), static_cast<int32>((*StatusData)->GetIntegerField(TEXT("port"))), UUeAgentInterfaceSettings::Get()->Port);
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceAuthorizationSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.AuthorizationFailures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceAuthorizationSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	FHttpSmokeResult StatusResult;
	TestTrue(TEXT("Unauthorized status request completed"), ExecuteHttpJsonRequest(TEXT("GET"), ServerScope.BaseUrl + TEXT("/api/status"), FString(), FString(), StatusResult));
	TestEqual(TEXT("Unauthorized status code"), StatusResult.StatusCode, 401);

	TSharedPtr<FJsonObject> StatusJson;
	TestTrue(TEXT("Unauthorized status response parses as JSON"), ParseJson(StatusResult.Body, StatusJson));
	TestTrue(TEXT("Unauthorized status root ok=false"), StatusJson.IsValid() && !StatusJson->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("Unauthorized status error code"), StatusJson.IsValid() ? StatusJson->GetStringField(TEXT("error")) : FString(), FString(TEXT("unauthorized")));

	const TSharedPtr<FJsonObject>* StatusData = nullptr;
	TestTrue(TEXT("Unauthorized status contains data object"), StatusJson.IsValid() && StatusJson->TryGetObjectField(TEXT("data"), StatusData) && StatusData && StatusData->IsValid());
	if (StatusData && StatusData->IsValid())
	{
		TestEqual(TEXT("Unauthorized status nested error code"), (*StatusData)->GetStringField(TEXT("error_code")), FString(TEXT("unauthorized")));
		TestTrue(TEXT("Unauthorized status nested error message is non-empty"), !(*StatusData)->GetStringField(TEXT("error_message")).IsEmpty());
	}

	const FString UnauthorizedExecBody = MakeExecRequestBody(TEXT("smoke-auth-exec-001"), TEXT("get_world_state"));
	FHttpSmokeResult ExecResult;
	TestTrue(TEXT("Unauthorized exec request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), TEXT("wrong-token"), UnauthorizedExecBody, ExecResult));
	TestEqual(TEXT("Unauthorized exec status code"), ExecResult.StatusCode, 401);

	TSharedPtr<FJsonObject> ExecJson;
	TestTrue(TEXT("Unauthorized exec response parses as JSON"), ParseJson(ExecResult.Body, ExecJson));
	TestTrue(TEXT("Unauthorized exec root ok=false"), ExecJson.IsValid() && !ExecJson->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("Unauthorized exec error code"), ExecJson.IsValid() ? ExecJson->GetStringField(TEXT("error")) : FString(), FString(TEXT("unauthorized")));

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceJsonParseVisibilitySmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.JsonParseVisibilityWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceJsonParseVisibilitySmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString BlueprintAssetPath = MakeAutomationAssetPath(TEXT("BPBadJson"));
	FString RelativeFolder = BlueprintAssetPath;
	while (RelativeFolder.StartsWith(TEXT("/")))
	{
		RelativeFolder.RightChopInline(1);
	}
	const FString FolderPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("ActorBlueprint"), RelativeFolder));
	IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	IFileManager::Get().MakeDirectory(*FPaths::Combine(FolderPath, TEXT("members")), true);

	auto SaveUtf8Json = [this](const FString& FilePath, const FString& JsonText) -> bool
	{
		const bool bOk = FFileHelper::SaveStringToFile(JsonText, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		TestTrue(FString::Printf(TEXT("Save JSON file: %s"), *FilePath), bOk);
		return bOk;
	};

	if (!SaveUtf8Json(
		FPaths::Combine(FolderPath, TEXT("asset.json")),
		FString::Printf(
			TEXT("{\n  \"format_version\": 1,\n  \"asset_kind\": \"actor_blueprint\",\n  \"asset_path\": \"%s\",\n  \"asset_name\": \"%s\",\n  \"parent_class\": \"/Script/Engine.Actor\"\n}\n"),
			*BlueprintAssetPath,
			*FPackageName::GetLongPackageAssetName(BlueprintAssetPath))))
	{
		return false;
	}

	if (!SaveUtf8Json(
		FPaths::Combine(FolderPath, TEXT("members"), TEXT("variables.json")),
		TEXT("{ \"variables\": [\n")))
	{
		return false;
	}

	const FString BadFolderApplyBody = MakeExecRequestBody(
		TEXT("smoke-json-parse-visibility-folder-bad-001"),
		TEXT("blueprint_apply_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"create_if_missing\":true,\"compile_after_apply\":false,\"save_after_apply\":false}"), *BlueprintAssetPath));
	FHttpSmokeResult BadFolderApplyResult;
	TestTrue(TEXT("Bad optional folder JSON request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, BadFolderApplyBody, BadFolderApplyResult));
	TestEqual(TEXT("Bad optional folder JSON status code"), BadFolderApplyResult.StatusCode, 400);
	TestTrue(TEXT("Bad optional folder JSON response names parse failure"), BadFolderApplyResult.Body.Contains(TEXT("json_parse_failed")));
	TestTrue(TEXT("Bad optional folder JSON response names file"), BadFolderApplyResult.Body.Contains(TEXT("variables.json")));

	TSharedPtr<FJsonObject> BadFolderApplyJson;
	TestTrue(TEXT("Bad optional folder JSON response parses"), ParseJson(BadFolderApplyResult.Body, BadFolderApplyJson));
	TestTrue(TEXT("Bad optional folder JSON root ok=false"), BadFolderApplyJson.IsValid() && !BadFolderApplyJson->GetBoolField(TEXT("ok")));

	if (!SaveUtf8Json(
		FPaths::Combine(FolderPath, TEXT("members"), TEXT("variables.json")),
		TEXT("{\n  \"variables\": [\n    \"not-an-object\",\n    { \"type\": { \"category\": \"bool\" } }\n  ]\n}\n")))
	{
		return false;
	}

	const FString WarningFolderApplyBody = MakeExecRequestBody(
		TEXT("smoke-json-parse-visibility-folder-warnings-001"),
		TEXT("blueprint_apply_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"create_if_missing\":true,\"compile_after_apply\":false,\"save_after_apply\":false}"), *BlueprintAssetPath));
	FHttpSmokeResult WarningFolderApplyResult;
	TestTrue(TEXT("Recoverable bad folder JSON items request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, WarningFolderApplyBody, WarningFolderApplyResult));
	TestEqual(TEXT("Recoverable bad folder JSON items status code"), WarningFolderApplyResult.StatusCode, 200);

	TSharedPtr<FJsonObject> WarningFolderApplyJson;
	TestTrue(TEXT("Recoverable bad folder JSON items response parses"), ParseJson(WarningFolderApplyResult.Body, WarningFolderApplyJson));
	TestTrue(TEXT("Recoverable bad folder JSON items root ok=true"), WarningFolderApplyJson.IsValid() && WarningFolderApplyJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* WarningData = nullptr;
	TestTrue(TEXT("Recoverable bad folder JSON items contains data"), WarningFolderApplyJson.IsValid() && WarningFolderApplyJson->TryGetObjectField(TEXT("data"), WarningData) && WarningData && WarningData->IsValid());
	if (WarningData && WarningData->IsValid())
	{
		TestTrue(TEXT("Recoverable bad folder JSON items warning_count"), (*WarningData)->GetIntegerField(TEXT("warning_count")) >= 2);
		const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
		TestTrue(TEXT("Recoverable bad folder JSON items warnings array"), (*WarningData)->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings);
		if (Warnings)
		{
			bool bFoundInvalidItem = false;
			bool bFoundMissingName = false;
			for (const TSharedPtr<FJsonValue>& WarningValue : *Warnings)
			{
				const TSharedPtr<FJsonObject>* WarningObj = nullptr;
				if (!WarningValue.IsValid() || !WarningValue->TryGetObject(WarningObj) || !WarningObj || !WarningObj->IsValid())
				{
					continue;
				}
				const FString Code = (*WarningObj)->GetStringField(TEXT("code"));
				bFoundInvalidItem |= Code == TEXT("variable_invalid_item");
				bFoundMissingName |= Code == TEXT("variable_missing_name");
			}
			TestTrue(TEXT("Recoverable bad folder JSON items includes variable_invalid_item"), bFoundInvalidItem);
			TestTrue(TEXT("Recoverable bad folder JSON items includes variable_missing_name"), bFoundMissingName);
		}
	}

	const FString InvalidPropertyBody = MakeExecRequestBody(
		TEXT("smoke-json-parse-visibility-property-invalid-001"),
		TEXT("asset_apply_property_json"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"properties\":[\"not-an-object\"],\"save_after_apply\":false}"), *BlueprintAssetPath));
	FHttpSmokeResult InvalidPropertyResult;
	TestTrue(TEXT("Invalid property JSON item request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, InvalidPropertyBody, InvalidPropertyResult));
	TestEqual(TEXT("Invalid property JSON item status code"), InvalidPropertyResult.StatusCode, 400);
	TestTrue(TEXT("Invalid property JSON item response names error"), InvalidPropertyResult.Body.Contains(TEXT("invalid_property_entry")));
	TestTrue(TEXT("Invalid property JSON item response includes property_results"), InvalidPropertyResult.Body.Contains(TEXT("property_results")));

	TSharedPtr<FJsonObject> InvalidPropertyJson;
	TestTrue(TEXT("Invalid property JSON item response parses"), ParseJson(InvalidPropertyResult.Body, InvalidPropertyJson));
	TestTrue(TEXT("Invalid property JSON item root ok=false"), InvalidPropertyJson.IsValid() && !InvalidPropertyJson->GetBoolField(TEXT("ok")));

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceJsonDiagnosticsHelperSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.JsonDiagnosticsHelpers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceJsonDiagnosticsHelperSmokeTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("has_ovverride"), TEXT("true"));
	Obj->SetObjectField(TEXT("has_override"), MakeShared<FJsonObject>());
	Obj->SetStringField(TEXT("display_name"), TEXT("Sample"));
	TSharedPtr<FJsonObject> UpperVectorObj = MakeShared<FJsonObject>();
	UpperVectorObj->SetNumberField(TEXT("X"), 1.0);
	UpperVectorObj->SetNumberField(TEXT("Y"), 2.0);
	UpperVectorObj->SetNumberField(TEXT("Z"), 3.0);
	Obj->SetObjectField(TEXT("upper_vector"), UpperVectorObj);
	TSharedPtr<FJsonObject> LowerRotatorObj = MakeShared<FJsonObject>();
	LowerRotatorObj->SetNumberField(TEXT("pitch"), 10.0);
	LowerRotatorObj->SetNumberField(TEXT("yaw"), 20.0);
	LowerRotatorObj->SetNumberField(TEXT("roll"), 30.0);
	Obj->SetObjectField(TEXT("lower_rotator"), LowerRotatorObj);
	TArray<TSharedPtr<FJsonValue>> VectorArray;
	VectorArray.Add(MakeShared<FJsonValueNumber>(4.0));
	VectorArray.Add(MakeShared<FJsonValueNumber>(5.0));
	VectorArray.Add(MakeShared<FJsonValueNumber>(6.0));
	Obj->SetArrayField(TEXT("array_vector"), VectorArray);

	TArray<TSharedPtr<FJsonValue>> Issues;
	UeAgentJsonDiagnostics::WarnUnknownFields(
		Obj,
		TEXT("root"),
		{ TEXT("has_override"), TEXT("display_name"), TEXT("items"), TEXT("upper_vector"), TEXT("lower_rotator"), TEXT("array_vector") },
		Issues);

	bool bHasOverride = false;
	TestFalse(
		TEXT("JSON diagnostics bool type mismatch returns false"),
		UeAgentJsonDiagnostics::ReadBoolField(Obj, TEXT("has_override"), TEXT("root.has_override"), bHasOverride, Issues, true));

	FString MissingValue;
	TestFalse(
		TEXT("JSON diagnostics missing required field returns false"),
		UeAgentJsonDiagnostics::ReadStringField(Obj, TEXT("missing_required"), TEXT("root.missing_required"), MissingValue, Issues, true));

	FVector ParsedUpperVector = FVector::ZeroVector;
	TestTrue(
		TEXT("JSON diagnostics reads UE-style uppercase vector"),
		UeAgentJsonDiagnostics::ReadVectorField(Obj, TEXT("upper_vector"), TEXT("root.upper_vector"), ParsedUpperVector, Issues, true));
	TestEqual(TEXT("Uppercase vector X"), ParsedUpperVector.X, 1.0);
	TestEqual(TEXT("Uppercase vector Y"), ParsedUpperVector.Y, 2.0);
	TestEqual(TEXT("Uppercase vector Z"), ParsedUpperVector.Z, 3.0);

	FVector ParsedArrayVector = FVector::ZeroVector;
	TestTrue(
		TEXT("JSON diagnostics reads array vector"),
		UeAgentJsonDiagnostics::ReadVectorField(Obj, TEXT("array_vector"), TEXT("root.array_vector"), ParsedArrayVector, Issues, true));
	TestEqual(TEXT("Array vector X"), ParsedArrayVector.X, 4.0);
	TestEqual(TEXT("Array vector Y"), ParsedArrayVector.Y, 5.0);
	TestEqual(TEXT("Array vector Z"), ParsedArrayVector.Z, 6.0);

	FRotator ParsedLowerRotator = FRotator::ZeroRotator;
	TestTrue(
		TEXT("JSON diagnostics reads lowercase rotator"),
		UeAgentJsonDiagnostics::ReadRotatorField(Obj, TEXT("lower_rotator"), TEXT("root.lower_rotator"), ParsedLowerRotator, Issues, true));
	TestEqual(TEXT("Lowercase rotator Pitch"), ParsedLowerRotator.Pitch, 10.0);
	TestEqual(TEXT("Lowercase rotator Yaw"), ParsedLowerRotator.Yaw, 20.0);
	TestEqual(TEXT("Lowercase rotator Roll"), ParsedLowerRotator.Roll, 30.0);

	TSharedPtr<FJsonValue> BadArrayItem = MakeShared<FJsonValueString>(TEXT("not-an-object"));
	TSharedPtr<FJsonObject> ParsedArrayObject;
	TestFalse(
		TEXT("JSON diagnostics array object type mismatch returns false"),
		UeAgentJsonDiagnostics::ReadObjectFromValue(BadArrayItem, TEXT("root.items[0]"), ParsedArrayObject, Issues));

	bool bFoundUnknownField = false;
	bool bFoundTypeMismatch = false;
	bool bFoundMissingRequired = false;
	bool bFoundArrayItemMismatch = false;
	for (const TSharedPtr<FJsonValue>& IssueValue : Issues)
	{
		const TSharedPtr<FJsonObject>* IssueObj = nullptr;
		if (!IssueValue.IsValid() || !IssueValue->TryGetObject(IssueObj) || !IssueObj || !IssueObj->IsValid())
		{
			continue;
		}

		const FString Code = (*IssueObj)->GetStringField(TEXT("code"));
		bFoundUnknownField |= Code == TEXT("json_unknown_field");
		bFoundTypeMismatch |= Code == TEXT("json_field_type_mismatch");
		bFoundMissingRequired |= Code == TEXT("json_missing_required_field");
		bFoundArrayItemMismatch |= Code == TEXT("json_array_item_type_mismatch");
		TestTrue(TEXT("JSON diagnostics issue has severity"), (*IssueObj)->HasField(TEXT("severity")));
		TestTrue(TEXT("JSON diagnostics issue has path"), (*IssueObj)->HasField(TEXT("path")));
		TestTrue(TEXT("JSON diagnostics issue has message"), (*IssueObj)->HasField(TEXT("message")));
	}

	TestTrue(TEXT("JSON diagnostics reports unknown fields"), bFoundUnknownField);
	TestTrue(TEXT("JSON diagnostics reports type mismatch"), bFoundTypeMismatch);
	TestTrue(TEXT("JSON diagnostics reports missing required fields"), bFoundMissingRequired);
	TestTrue(TEXT("JSON diagnostics reports array item mismatch"), bFoundArrayItemMismatch);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceExecSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.ExecAndBatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceExecSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString ExecBody = MakeExecRequestBody(TEXT("smoke-exec-001"), TEXT("list_actors"), TEXT("{\"limit\":1}"));
	FHttpSmokeResult ExecResult;
	TestTrue(TEXT("Exec request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ExecBody, ExecResult));
	TestEqual(TEXT("Exec status code"), ExecResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ExecJson;
	TestTrue(TEXT("Exec response parses as JSON"), ParseJson(ExecResult.Body, ExecJson));
	TestTrue(TEXT("Exec root ok=true"), ExecJson.IsValid() && ExecJson->GetBoolField(TEXT("ok")));

	const FString BatchBody =
		TEXT("{")
		TEXT("\"request_id\":\"smoke-batch-001\",")
		TEXT("\"command\":\"exec_batch\",")
		TEXT("\"params\":{")
		TEXT("\"stop_on_error\":true,")
		TEXT("\"commands\":[")
		TEXT("{\"request_id\":\"step-1\",\"command\":\"get_world_state\",\"params\":{}},")
		TEXT("{\"request_id\":\"step-2\",\"command\":\"list_actors\",\"params\":{\"limit\":1}}")
		TEXT("]}}");

	FHttpSmokeResult BatchResult;
	TestTrue(TEXT("Batch request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, BatchBody, BatchResult));
	TestEqual(TEXT("Batch status code"), BatchResult.StatusCode, 200);

	TSharedPtr<FJsonObject> BatchJson;
	TestTrue(TEXT("Batch response parses as JSON"), ParseJson(BatchResult.Body, BatchJson));
	TestTrue(TEXT("Batch root ok=true"), BatchJson.IsValid() && BatchJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* BatchData = nullptr;
	TestTrue(TEXT("Batch contains data object"), BatchJson.IsValid() && BatchJson->TryGetObjectField(TEXT("data"), BatchData) && BatchData && BatchData->IsValid());
	if (BatchData && BatchData->IsValid())
	{
		TestEqual(TEXT("Batch mode"), (*BatchData)->GetStringField(TEXT("mode")), FString(TEXT("exec_batch")));
		TestEqual(TEXT("Batch executed count"), static_cast<int32>((*BatchData)->GetIntegerField(TEXT("executed"))), 2);
		TestEqual(TEXT("Batch succeeded count"), static_cast<int32>((*BatchData)->GetIntegerField(TEXT("succeeded"))), 2);
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceEditorDirtyResolveAndCloseSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.EditorDirtyResolveAndClose",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceEditorDirtyResolveAndCloseSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	{
		const FString PreDirtyListBody = MakeExecRequestBody(TEXT("smoke-editor-preclean-list-001"), TEXT("editor_list_dirty_resources"));
		FHttpSmokeResult PreDirtyListResult;
		TestTrue(TEXT("Pre-clean dirty resource list request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, PreDirtyListBody, PreDirtyListResult));
		TestEqual(TEXT("Pre-clean dirty resource list status code"), PreDirtyListResult.StatusCode, 200);

		TSharedPtr<FJsonObject> PreDirtyListJson;
		TestTrue(TEXT("Pre-clean dirty resource list parses as JSON"), ParseJson(PreDirtyListResult.Body, PreDirtyListJson));
		TestTrue(TEXT("Pre-clean dirty resource list root ok=true"), PreDirtyListJson.IsValid() && PreDirtyListJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* PreDirtyListData = nullptr;
		TestTrue(TEXT("Pre-clean dirty resource list contains data"), PreDirtyListJson.IsValid() && PreDirtyListJson->TryGetObjectField(TEXT("data"), PreDirtyListData) && PreDirtyListData && PreDirtyListData->IsValid());
		const int32 PreDirtyCount = (PreDirtyListData && PreDirtyListData->IsValid()) ? static_cast<int32>((*PreDirtyListData)->GetIntegerField(TEXT("dirty_resource_count"))) : 0;
		if (PreDirtyCount > 0)
		{
			const FString PreResolveBody = MakeExecRequestBody(
				TEXT("smoke-editor-preclean-resolve-001"),
				TEXT("editor_resolve_dirty_resources"),
				TEXT("{\"discard_all\":true,\"close_all_asset_editors\":true}"));
			FHttpSmokeResult PreResolveResult;
			TestTrue(TEXT("Pre-clean resolve dirty resources request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, PreResolveBody, PreResolveResult));
			TestEqual(TEXT("Pre-clean resolve dirty resources status code"), PreResolveResult.StatusCode, 200);

			TSharedPtr<FJsonObject> PreResolveJson;
			TestTrue(TEXT("Pre-clean resolve dirty resources parses as JSON"), ParseJson(PreResolveResult.Body, PreResolveJson));
			TestTrue(TEXT("Pre-clean resolve dirty resources root ok=true"), PreResolveJson.IsValid() && PreResolveJson->GetBoolField(TEXT("ok")));
		}
	}

	const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ActorLabel = FString::Printf(TEXT("SmokeEditorDirty_%s"), *UniqueSuffix);

	const FString SpawnBody = MakeExecRequestBody(
		TEXT("smoke-editor-dirty-spawn-001"),
		TEXT("spawn_actor"),
		FString::Printf(
			TEXT("{\"class_path\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":-1200.0,\"y\":-300.0,\"z\":120.0}}"),
			*ActorLabel));
	FHttpSmokeResult SpawnResult;
	TestTrue(TEXT("Spawn dirty actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnBody, SpawnResult));
	TestEqual(TEXT("Spawn dirty actor status code"), SpawnResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SpawnJson;
	TestTrue(TEXT("Spawn dirty actor response parses as JSON"), ParseJson(SpawnResult.Body, SpawnJson));
	TestTrue(TEXT("Spawn dirty actor root ok=true"), SpawnJson.IsValid() && SpawnJson->GetBoolField(TEXT("ok")));

	const FString DirtyListBody = MakeExecRequestBody(TEXT("smoke-editor-dirty-list-001"), TEXT("editor_list_dirty_resources"));
	FHttpSmokeResult DirtyListResult;
	TestTrue(TEXT("Dirty resource list request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DirtyListBody, DirtyListResult));
	TestEqual(TEXT("Dirty resource list status code"), DirtyListResult.StatusCode, 200);

	TSharedPtr<FJsonObject> DirtyListJson;
	TestTrue(TEXT("Dirty resource list parses as JSON"), ParseJson(DirtyListResult.Body, DirtyListJson));
	TestTrue(TEXT("Dirty resource list root ok=true"), DirtyListJson.IsValid() && DirtyListJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* DirtyListData = nullptr;
	TestTrue(TEXT("Dirty resource list contains data"), DirtyListJson.IsValid() && DirtyListJson->TryGetObjectField(TEXT("data"), DirtyListData) && DirtyListData && DirtyListData->IsValid());
	if (DirtyListData && DirtyListData->IsValid())
	{
		TestTrue(TEXT("Dirty resource count is positive"), static_cast<int32>((*DirtyListData)->GetIntegerField(TEXT("dirty_resource_count"))) > 0);
	}

	const FString CloseBody = MakeExecRequestBody(
		TEXT("smoke-editor-close-001"),
		TEXT("editor_close"),
		TEXT("{\"request_exit\":false,\"close_all_asset_editors\":false}"));
	FHttpSmokeResult CloseResult;
	TestTrue(TEXT("Editor close request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CloseBody, CloseResult));
	TestEqual(TEXT("Editor close blocked status code"), CloseResult.StatusCode, 400);

	TSharedPtr<FJsonObject> CloseJson;
	TestTrue(TEXT("Editor close blocked parses as JSON"), ParseJson(CloseResult.Body, CloseJson));
	TestTrue(TEXT("Editor close blocked root ok=false"), CloseJson.IsValid() && !CloseJson->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("Editor close blocked error code"), CloseJson.IsValid() ? CloseJson->GetStringField(TEXT("error")) : FString(), FString(TEXT("editor_has_unresolved_dirty_resources")));

	const TSharedPtr<FJsonObject>* CloseData = nullptr;
	TestTrue(TEXT("Editor close blocked contains data"), CloseJson.IsValid() && CloseJson->TryGetObjectField(TEXT("data"), CloseData) && CloseData && CloseData->IsValid());
	if (CloseData && CloseData->IsValid())
	{
		TestTrue(TEXT("Editor close blocked returns dirty list"), static_cast<int32>((*CloseData)->GetIntegerField(TEXT("dirty_resource_count"))) > 0);
	}

	const FString ResolveBody = MakeExecRequestBody(
		TEXT("smoke-editor-resolve-001"),
		TEXT("editor_resolve_dirty_resources"),
		TEXT("{\"discard_current_level\":true,\"discard_all\":true}"));
	FHttpSmokeResult ResolveResult;
	TestTrue(TEXT("Editor resolve dirty resources request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ResolveBody, ResolveResult));
	TestEqual(TEXT("Editor resolve dirty resources status code"), ResolveResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ResolveJson;
	TestTrue(TEXT("Editor resolve dirty resources parses as JSON"), ParseJson(ResolveResult.Body, ResolveJson));
	TestTrue(TEXT("Editor resolve dirty resources root ok=true"), ResolveJson.IsValid() && ResolveJson->GetBoolField(TEXT("ok")));

	const FString CloseAfterResolveBody = MakeExecRequestBody(
		TEXT("smoke-editor-close-002"),
		TEXT("editor_close"),
		TEXT("{\"request_exit\":false,\"close_all_asset_editors\":false}"));
	FHttpSmokeResult CloseAfterResolveResult;
	TestTrue(TEXT("Editor close after resolve request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CloseAfterResolveBody, CloseAfterResolveResult));
	TestEqual(TEXT("Editor close after resolve status code"), CloseAfterResolveResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CloseAfterResolveJson;
	TestTrue(TEXT("Editor close after resolve parses as JSON"), ParseJson(CloseAfterResolveResult.Body, CloseAfterResolveJson));
	TestTrue(TEXT("Editor close after resolve root ok=true"), CloseAfterResolveJson.IsValid() && CloseAfterResolveJson->GetBoolField(TEXT("ok")));

	return !HasAnyErrors();
}

#endif
