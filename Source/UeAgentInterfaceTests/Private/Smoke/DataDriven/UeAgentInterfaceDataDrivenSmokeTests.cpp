#if WITH_DEV_AUTOMATION_TESTS

#include "../UeAgentSmokeTestSharedIncludes.inl"

namespace
{
	static bool ExecDataDrivenSmokeCommand(
		FAutomationTestBase* Test,
		const FScopedUeAgentHttpServer& ServerScope,
		const FString& RequestId,
		const FString& Command,
		const FString& ParamsJson,
		const int32 ExpectedStatusCode,
		TSharedPtr<FJsonObject>& OutRoot,
		TSharedPtr<FJsonObject>& OutData)
	{
		const FString Body = MakeExecRequestBody(RequestId, Command, ParamsJson);
		FHttpSmokeResult Result;
		const bool bRequestOk = ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, Body, Result);
		Test->TestTrue(FString::Printf(TEXT("%s request completed"), *RequestId), bRequestOk);
		Test->TestEqual(FString::Printf(TEXT("%s status code"), *RequestId), Result.StatusCode, ExpectedStatusCode);
		Test->TestTrue(FString::Printf(TEXT("%s response parses as JSON"), *RequestId), ParseJson(Result.Body, OutRoot));

		const bool bExpectOk = ExpectedStatusCode >= 200 && ExpectedStatusCode < 300;
		Test->TestTrue(
			FString::Printf(TEXT("%s ok flag matches status"), *RequestId),
			OutRoot.IsValid() && OutRoot->GetBoolField(TEXT("ok")) == bExpectOk);

		OutData.Reset();
		const TSharedPtr<FJsonObject>* DataObject = nullptr;
		if (OutRoot.IsValid() && OutRoot->TryGetObjectField(TEXT("data"), DataObject) && DataObject && DataObject->IsValid())
		{
			OutData = *DataObject;
		}
		return bRequestOk && Result.StatusCode == ExpectedStatusCode && OutRoot.IsValid();
	}

	static bool FindSmokeFieldValue(const TArray<TSharedPtr<FJsonValue>>* Fields, const FString& FieldName, FString& OutValueText)
	{
		OutValueText.Reset();
		if (!Fields)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& FieldValue : *Fields)
		{
			const TSharedPtr<FJsonObject>* FieldObj = nullptr;
			if (!FieldValue.IsValid() || !FieldValue->TryGetObject(FieldObj) || !FieldObj || !FieldObj->IsValid())
			{
				continue;
			}

			FString Name;
			if (!(*FieldObj)->TryGetStringField(TEXT("property_path"), Name))
			{
				(*FieldObj)->TryGetStringField(TEXT("name"), Name);
			}
			const FString AuthoredName = (*FieldObj)->HasField(TEXT("authored_name")) ? (*FieldObj)->GetStringField(TEXT("authored_name")) : FString();
			if (Name.Equals(FieldName, ESearchCase::IgnoreCase) || AuthoredName.Equals(FieldName, ESearchCase::IgnoreCase))
			{
				return (*FieldObj)->TryGetStringField(TEXT("value_text"), OutValueText);
			}
		}
		return false;
	}

	static const TSharedPtr<FJsonObject>* FindSmokeFieldObject(const TArray<TSharedPtr<FJsonValue>>* Fields, const FString& FieldName)
	{
		if (!Fields)
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& FieldValue : *Fields)
		{
			const TSharedPtr<FJsonObject>* FieldObj = nullptr;
			if (!FieldValue.IsValid() || !FieldValue->TryGetObject(FieldObj) || !FieldObj || !FieldObj->IsValid())
			{
				continue;
			}
			FString Name;
			if (!(*FieldObj)->TryGetStringField(TEXT("property_path"), Name))
			{
				(*FieldObj)->TryGetStringField(TEXT("name"), Name);
			}
			const FString AuthoredName = (*FieldObj)->HasField(TEXT("authored_name")) ? (*FieldObj)->GetStringField(TEXT("authored_name")) : FString();
			if (Name.Equals(FieldName, ESearchCase::IgnoreCase) || AuthoredName.Equals(FieldName, ESearchCase::IgnoreCase))
			{
				return FieldObj;
			}
		}
		return nullptr;
	}

	static bool HasAuthoringMetadata(const TSharedPtr<FJsonObject>& Obj)
	{
		return Obj.IsValid()
			&& Obj->HasTypedField<EJson::Object>(TEXT("selector"))
			&& Obj->HasTypedField<EJson::Array>(TEXT("operations"))
			&& Obj->HasTypedField<EJson::Object>(TEXT("availability"));
	}

	static bool FindSmokeRowFields(const TArray<TSharedPtr<FJsonValue>>* Rows, const FString& RowName, const TArray<TSharedPtr<FJsonValue>>*& OutFields)
	{
		OutFields = nullptr;
		if (!Rows)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& RowValue : *Rows)
		{
			const TSharedPtr<FJsonObject>* RowObj = nullptr;
			if (!RowValue.IsValid() || !RowValue->TryGetObject(RowObj) || !RowObj || !RowObj->IsValid())
			{
				continue;
			}
			FString CurrentRowName;
			if ((*RowObj)->TryGetStringField(TEXT("row_name"), CurrentRowName) && CurrentRowName == RowName)
			{
				return (*RowObj)->TryGetArrayField(TEXT("fields"), OutFields) && OutFields;
			}
		}
		return false;
	}

	static bool JsonArrayContainsCode(const TArray<TSharedPtr<FJsonValue>>* Issues, const FString& ExpectedCode)
	{
		if (!Issues)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& IssueValue : *Issues)
		{
			const TSharedPtr<FJsonObject> IssueObj = IssueValue.IsValid() ? IssueValue->AsObject() : nullptr;
			FString Code;
			if (IssueObj.IsValid() && IssueObj->TryGetStringField(TEXT("code"), Code) && Code.Equals(ExpectedCode, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceDataDrivenDataAssetSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.DataDriven.DataAssetWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceDataDrivenDataAssetSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString BlueprintAssetPath = MakeAutomationAssetPath(TEXT("BP_PrimaryDataAsset"));
	const FString DataAssetPath = MakeAutomationAssetPath(TEXT("DA_PrimaryDataAsset"));
	const FString ExportFile = ToSmokeJsonPath(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("UAIDataDriven"), TEXT("data_asset_export.json")));

	TSharedPtr<FJsonObject> Root;
	TSharedPtr<FJsonObject> Data;

	TestTrue(TEXT("Create PrimaryDataAsset blueprint"), ExecDataDrivenSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-data-asset-bp-create-001"),
		TEXT("blueprint_create"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"parent_class\":\"/Script/Engine.PrimaryDataAsset\",\"compile_after_create\":false,\"open_editor\":false,\"save_after_create\":false}"),
			*BlueprintAssetPath),
		200,
		Root,
		Data));

	TestTrue(TEXT("Add editable int field"), ExecDataDrivenSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-data-asset-bp-add-var-001"),
		TEXT("blueprint_add_variable"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"variable_name\":\"Power\",\"pin_category\":\"int\",\"default_value\":\"0\",\"instance_editable\":true,\"compile_after_add\":true,\"save_after_add\":false}"),
			*BlueprintAssetPath),
		200,
		Root,
		Data));

	TestTrue(TEXT("Read generated class"), ExecDataDrivenSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-data-asset-bp-info-001"),
		TEXT("blueprint_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *BlueprintAssetPath),
		200,
		Root,
		Data));

	const FString GeneratedClass = Data.IsValid() ? Data->GetStringField(TEXT("generated_class")) : FString();
	TestTrue(TEXT("Generated PrimaryDataAsset class is present"), !GeneratedClass.IsEmpty());

	TestTrue(TEXT("Create DataAsset instance"), ExecDataDrivenSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-data-asset-create-001"),
		TEXT("data_asset_create"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"class_path\":\"%s\",\"overwrite_policy\":\"replace\",\"save_after_create\":false}"),
			*DataAssetPath,
			*GeneratedClass),
		200,
		Root,
		Data));

	TestTrue(TEXT("Apply DataAsset JSON"), ExecDataDrivenSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-data-asset-apply-001"),
		TEXT("data_asset_apply_json"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"json\":{\"schema\":\"ue_agent_interface.data_asset.v1\",\"asset_path\":\"%s\",\"asset_class\":\"%s\",\"fields\":[{\"property_path\":\"Power\",\"value_text\":\"42\"}]},\"backup\":true,\"save_after_apply\":false}"),
			*DataAssetPath,
			*DataAssetPath,
			*GeneratedClass),
		200,
		Root,
		Data));
	TestTrue(TEXT("DataAsset apply reported one field"), Data.IsValid() && static_cast<int32>(Data->GetIntegerField(TEXT("applied_field_count"))) == 1);
	TestTrue(TEXT("DataAsset apply created backup"), Data.IsValid() && Data->GetBoolField(TEXT("backup_created")) && !Data->GetStringField(TEXT("backup_asset_path")).IsEmpty());

	TestTrue(TEXT("Export DataAsset JSON"), ExecDataDrivenSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-data-asset-export-001"),
		TEXT("data_asset_export_json"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"include_primary_asset\":true,\"output_file\":\"%s\"}"),
			*DataAssetPath,
			*ExportFile),
		200,
		Root,
		Data));
	TestTrue(TEXT("DataAsset export file exists"), IFileManager::Get().FileExists(*ExportFile));

	const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
	TestTrue(TEXT("DataAsset export has fields"), Data.IsValid() && Data->TryGetArrayField(TEXT("fields"), Fields) && Fields);
	FString PowerValue;
	TestTrue(TEXT("DataAsset exported Power field"), FindSmokeFieldValue(Fields, TEXT("Power"), PowerValue));
	TestEqual(TEXT("DataAsset exported Power value"), PowerValue, FString(TEXT("42")));
	const TSharedPtr<FJsonObject>* PowerField = FindSmokeFieldObject(Fields, TEXT("Power"));
	TestTrue(TEXT("DataAsset Power exposes authoring metadata"), PowerField && PowerField->IsValid() && HasAuthoringMetadata(*PowerField));
	if (PowerField && PowerField->IsValid())
	{
		TestEqual(TEXT("DataAsset Power property_path"), (*PowerField)->GetStringField(TEXT("property_path")), FString(TEXT("Power")));
		TestFalse(TEXT("DataAsset Power omits duplicate name"), (*PowerField)->HasField(TEXT("name")));
		TestFalse(TEXT("DataAsset Power omits duplicate authored_name"), (*PowerField)->HasField(TEXT("authored_name")));
		TestFalse(TEXT("DataAsset Power omits duplicate cpp_type"), (*PowerField)->HasField(TEXT("cpp_type")));
	}

	TestTrue(TEXT("Diff DataAsset JSON"), ExecDataDrivenSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-data-asset-diff-001"),
		TEXT("data_asset_diff_json"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"json\":{\"asset_path\":\"%s\",\"asset_class\":\"%s\",\"fields\":[{\"name\":\"Power\",\"value_text\":\"42\"}]}}"),
			*DataAssetPath,
			*DataAssetPath,
			*GeneratedClass),
		200,
		Root,
		Data));
	TestFalse(TEXT("DataAsset diff unchanged"), Data.IsValid() && Data->GetBoolField(TEXT("changed")));

	TestTrue(TEXT("PrimaryAsset validate reports mismatch diagnostics"), ExecDataDrivenSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-primary-asset-validate-001"),
		TEXT("primary_asset_validate_asset"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"expected_type\":\"__UAIWrongType\",\"load_probe\":true,\"validate_bundles\":true}"),
			*DataAssetPath),
		400,
		Root,
		Data));
	TestTrue(TEXT("PrimaryAsset validate returns type_matches=false"), Data.IsValid() && !Data->GetBoolField(TEXT("type_matches")));
	TestTrue(TEXT("PrimaryAsset validate returns bundles array"), Data.IsValid() && Data->HasField(TEXT("bundles")));
	const TArray<TSharedPtr<FJsonValue>>* PrimaryIssues = nullptr;
	TestTrue(TEXT("PrimaryAsset validate reports not registered"), Data.IsValid() && Data->TryGetArrayField(TEXT("issues"), PrimaryIssues) && JsonArrayContainsCode(PrimaryIssues, TEXT("primary_asset_not_registered")));
	TestTrue(TEXT("PrimaryAsset validate reports type mismatch"), JsonArrayContainsCode(PrimaryIssues, TEXT("primary_asset_type_mismatch")));

	TestTrue(TEXT("PrimaryAsset scan command succeeds"), ExecDataDrivenSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-primary-asset-scan-001"),
		TEXT("primary_asset_scan_assets"),
		TEXT("{\"primary_asset_type\":\"__UAINotConfigured\",\"paths\":[\"/Game/UAI/Automation\"],\"load_assets\":true}"),
		200,
		Root,
		Data));
	TestTrue(TEXT("PrimaryAsset scan echoes load_assets"), Data.IsValid() && Data->GetBoolField(TEXT("load_assets")));

	TestTrue(TEXT("DataAsset validate can warn unknown fields"), ExecDataDrivenSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-data-asset-validate-warn-001"),
		TEXT("data_asset_validate_json"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"allow_unknown_fields\":true,\"json\":{\"asset_path\":\"%s\",\"asset_class\":\"%s\",\"fields\":[{\"name\":\"MissingField\",\"value_text\":\"1\"}]}}"),
			*DataAssetPath,
			*DataAssetPath,
			*GeneratedClass),
		200,
		Root,
		Data));
	TestTrue(TEXT("DataAsset unknown-field warning counted"), Data.IsValid() && Data->GetIntegerField(TEXT("warning_count")) > 0);

	TestTrue(TEXT("DataAsset validate rejects unknown field"), ExecDataDrivenSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-data-asset-validate-bad-001"),
		TEXT("data_asset_validate_json"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"json\":{\"asset_path\":\"%s\",\"asset_class\":\"%s\",\"fields\":[{\"name\":\"MissingField\",\"value_text\":\"1\"}]}}"),
			*DataAssetPath,
			*DataAssetPath,
			*GeneratedClass),
		400,
		Root,
		Data));

	TestTrue(TEXT("DataAsset apply rejects bad int import"), ExecDataDrivenSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-data-asset-apply-bad-import-001"),
		TEXT("data_asset_apply_json"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"json\":{\"schema\":\"ue_agent_interface.data_asset.v1\",\"asset_path\":\"%s\",\"asset_class\":\"%s\",\"fields\":[{\"name\":\"Power\",\"value_text\":\"NotAnInt\"}]},\"backup\":false,\"save_after_apply\":false}"),
			*DataAssetPath,
			*DataAssetPath,
			*GeneratedClass),
		400,
		Root,
		Data));
	const TArray<TSharedPtr<FJsonValue>>* BadApplyIssues = nullptr;
	TestTrue(TEXT("DataAsset bad import issue emitted"), Data.IsValid() && Data->TryGetArrayField(TEXT("issues"), BadApplyIssues) && JsonArrayContainsCode(BadApplyIssues, TEXT("data_asset_property_import_failed")));

	TestTrue(TEXT("Export DataAsset after failed apply"), ExecDataDrivenSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-data-asset-export-after-bad-001"),
		TEXT("data_asset_export_json"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *DataAssetPath),
		200,
		Root,
		Data));
	Fields = nullptr;
	TestTrue(TEXT("DataAsset after failed apply has fields"), Data.IsValid() && Data->TryGetArrayField(TEXT("fields"), Fields) && Fields);
	PowerValue.Reset();
	TestTrue(TEXT("DataAsset after failed apply exported Power field"), FindSmokeFieldValue(Fields, TEXT("Power"), PowerValue));
	TestEqual(TEXT("DataAsset failed apply did not change Power"), PowerValue, FString(TEXT("42")));

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceDataDrivenDataTableSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.DataDriven.DataTableWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceDataDrivenDataTableSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString DataTablePath = MakeAutomationAssetPath(TEXT("DT_GameplayTags"));
	const FString RowStructPath = TEXT("/Script/GameplayTags.GameplayTagTableRow");
	const FString ExportFile = ToSmokeJsonPath(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("UAIDataDriven"), TEXT("data_table_export.json")));

	const FString InitialRowsJson = FString::Printf(
		TEXT("{\"schema\":\"ue_agent_interface.data_table.v1\",\"asset_path\":\"%s\",\"row_struct\":\"%s\",\"row_policy\":\"replace_all\",\"rows\":[{\"row_name\":\"RowA\",\"fields\":[{\"name\":\"Tag\",\"value_text\":\"UAI.Smoke.RowA\"},{\"name\":\"DevComment\",\"value_text\":\"FirstRow\"}]},{\"row_name\":\"RowB\",\"fields\":[{\"name\":\"Tag\",\"value_text\":\"UAI.Smoke.RowB\"},{\"name\":\"DevComment\",\"value_text\":\"SecondRow\"}]}]}"),
		*DataTablePath,
		*RowStructPath);

	TSharedPtr<FJsonObject> Root;
	TSharedPtr<FJsonObject> Data;

	TestTrue(TEXT("Apply DataTable JSON creates table"), ExecDataDrivenSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-data-table-apply-create-001"),
		TEXT("data_table_apply_json"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"json\":%s,\"create_if_missing\":true,\"overwrite_policy\":\"replace\",\"save_after_apply\":false}"),
			*DataTablePath,
			*InitialRowsJson),
		200,
		Root,
		Data));
	TestEqual(TEXT("DataTable apply created two rows"), static_cast<int32>(Data->GetIntegerField(TEXT("row_count"))), 2);

	TestTrue(TEXT("Validate DataTable JSON succeeds"), ExecDataDrivenSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-data-table-validate-001"),
		TEXT("data_table_validate_json"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"json\":%s}"), *DataTablePath, *InitialRowsJson),
		200,
		Root,
		Data));
	TestTrue(TEXT("DataTable validate valid=true"), Data.IsValid() && Data->GetBoolField(TEXT("valid")));

	TestTrue(TEXT("Export DataTable JSON"), ExecDataDrivenSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-data-table-export-001"),
		TEXT("data_table_export_json"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"output_file\":\"%s\"}"),
			*DataTablePath,
			*ExportFile),
		200,
		Root,
		Data));
	TestTrue(TEXT("DataTable export file exists"), IFileManager::Get().FileExists(*ExportFile));
	TestEqual(TEXT("DataTable export has two rows"), static_cast<int32>(Data->GetIntegerField(TEXT("row_count"))), 2);
	TestEqual(TEXT("DataTable export row_policy is replace_all"), Data->GetStringField(TEXT("row_policy")), FString(TEXT("replace_all")));
	TestTrue(TEXT("DataTable export has row_struct_schema_hash"), Data.IsValid() && !Data->GetStringField(TEXT("row_struct_schema_hash")).IsEmpty());
	const TArray<TSharedPtr<FJsonValue>>* ExportedRows = nullptr;
	TestTrue(TEXT("DataTable export has rows"), Data.IsValid() && Data->TryGetArrayField(TEXT("rows"), ExportedRows) && ExportedRows);
	bool bFoundRowAuthoringMetadata = false;
	if (ExportedRows)
	{
		for (const TSharedPtr<FJsonValue>& RowValue : *ExportedRows)
		{
			const TSharedPtr<FJsonObject>* RowObj = nullptr;
			if (RowValue.IsValid() && RowValue->TryGetObject(RowObj) && RowObj && HasAuthoringMetadata(*RowObj))
			{
				bFoundRowAuthoringMetadata = true;
				break;
			}
		}
	}
	TestTrue(TEXT("DataTable row exposes authoring metadata"), bFoundRowAuthoringMetadata);
	const TArray<TSharedPtr<FJsonValue>>* RowAFields = nullptr;
	TestTrue(TEXT("DataTable RowA has fields"), FindSmokeRowFields(ExportedRows, TEXT("RowA"), RowAFields));
	const TSharedPtr<FJsonObject>* RowATagField = FindSmokeFieldObject(RowAFields, TEXT("Tag"));
	TestTrue(TEXT("DataTable RowA field exposes authoring metadata"), RowATagField && RowATagField->IsValid() && HasAuthoringMetadata(*RowATagField));
	if (RowATagField && RowATagField->IsValid())
	{
		TestEqual(TEXT("DataTable RowA Tag property_path"), (*RowATagField)->GetStringField(TEXT("property_path")), FString(TEXT("Tag")));
		TestFalse(TEXT("DataTable RowA Tag omits duplicate name"), (*RowATagField)->HasField(TEXT("name")));
		TestFalse(TEXT("DataTable RowA Tag omits duplicate authored_name"), (*RowATagField)->HasField(TEXT("authored_name")));
		TestFalse(TEXT("DataTable RowA Tag omits duplicate cpp_type"), (*RowATagField)->HasField(TEXT("cpp_type")));
	}

	TestTrue(TEXT("Diff DataTable unchanged JSON"), ExecDataDrivenSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-data-table-diff-same-001"),
		TEXT("data_table_diff_json"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"json\":%s}"), *DataTablePath, *InitialRowsJson),
		200,
		Root,
		Data));
	TestFalse(TEXT("DataTable unchanged diff reports changed=false"), Data.IsValid() && Data->GetBoolField(TEXT("changed")));
	TestEqual(TEXT("DataTable unchanged diff count"), static_cast<int32>(Data->GetIntegerField(TEXT("diff_count"))), 0);

	TestTrue(TEXT("Query DataTable RowA"), ExecDataDrivenSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-data-table-query-001"),
		TEXT("data_table_query_rows"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"row_names\":[\"RowA\"],\"field_names\":[\"Tag\"],\"field_filters\":[{\"field\":\"Tag\",\"op\":\"==\",\"value_text\":\"UAI.Smoke.RowA\"}],\"limit\":4}"),
			*DataTablePath),
		200,
		Root,
		Data));
	TestEqual(TEXT("DataTable query one row"), static_cast<int32>(Data->GetIntegerField(TEXT("row_count"))), 1);
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* RowFields = nullptr;
	TestTrue(TEXT("DataTable query has rows"), Data->TryGetArrayField(TEXT("rows"), Rows) && Rows);
	TestTrue(TEXT("DataTable query finds RowA fields"), FindSmokeRowFields(Rows, TEXT("RowA"), RowFields));
	FString TagValue;
	TestTrue(TEXT("DataTable query returned Tag value"), FindSmokeFieldValue(RowFields, TEXT("Tag"), TagValue));
	TestEqual(TEXT("DataTable query Tag matches"), TagValue, FString(TEXT("UAI.Smoke.RowA")));

	TestTrue(TEXT("Sample DataTable rows"), ExecDataDrivenSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-data-table-sample-001"),
		TEXT("data_table_sample_rows"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"checks\":[{\"row_name\":\"RowA\",\"field\":\"Tag\",\"op\":\"==\",\"value_text\":\"UAI.Smoke.RowA\"},{\"row_name\":\"RowB\",\"field\":\"DevComment\",\"op\":\"contains\",\"value_text\":\"Second\"}]}"),
			*DataTablePath),
		200,
		Root,
		Data));
	TestTrue(TEXT("DataTable sample passed"), Data.IsValid() && Data->GetBoolField(TEXT("passed")));

	const FString PatchRowsJson = FString::Printf(
		TEXT("{\"schema\":\"ue_agent_interface.data_table.v1\",\"asset_path\":\"%s\",\"row_struct\":\"%s\",\"row_policy\":\"patch\",\"update_rows\":[{\"row_name\":\"RowA\",\"fields\":[{\"property_path\":\"Tag\",\"value_text\":\"UAI.Smoke.RowA\"},{\"property_path\":\"DevComment\",\"value_text\":\"UpdatedRow\"}]}],\"remove_rows\":[\"RowB\"]}"),
		*DataTablePath,
		*RowStructPath);

	TestTrue(TEXT("Diff DataTable patch JSON"), ExecDataDrivenSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-data-table-diff-001"),
		TEXT("data_table_diff_json"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"json\":%s}"), *DataTablePath, *PatchRowsJson),
		200,
		Root,
		Data));
	TestTrue(TEXT("DataTable diff changed"), Data.IsValid() && Data->GetBoolField(TEXT("changed")));
	TestTrue(TEXT("DataTable patch diff reports destructive removal"), Data.IsValid() && Data->GetIntegerField(TEXT("destructive_change_count")) > 0);
	TestTrue(TEXT("DataTable patch diff reports field change"), Data.IsValid() && Data->HasField(TEXT("field_changed")));

	TestTrue(TEXT("Apply DataTable patch JSON"), ExecDataDrivenSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-data-table-apply-patch-001"),
		TEXT("data_table_apply_json"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"json\":%s,\"backup\":true,\"save_after_apply\":false}"),
			*DataTablePath,
			*PatchRowsJson),
		200,
		Root,
		Data));
	TestEqual(TEXT("DataTable patch leaves one row"), static_cast<int32>(Data->GetIntegerField(TEXT("row_count"))), 1);
	TestEqual(TEXT("DataTable patch removed one row"), static_cast<int32>(Data->GetIntegerField(TEXT("removed_row_count"))), 1);
	TestTrue(TEXT("DataTable patch is destructive"), Data.IsValid() && Data->GetBoolField(TEXT("destructive")));
	TestTrue(TEXT("DataTable patch created backup"), Data.IsValid() && Data->GetBoolField(TEXT("backup_created")) && !Data->GetStringField(TEXT("backup_asset_path")).IsEmpty());
	TestTrue(TEXT("DataTable apply returns read_back_hash"), Data.IsValid() && !Data->GetStringField(TEXT("read_back_hash")).IsEmpty());

	TestTrue(TEXT("DataTable validate rejects unknown field"), ExecDataDrivenSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-data-table-validate-bad-001"),
		TEXT("data_table_validate_json"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"json\":{\"schema\":\"ue_agent_interface.data_table.v1\",\"asset_path\":\"%s\",\"row_struct\":\"%s\",\"rows\":[{\"row_name\":\"BadRow\",\"fields\":[{\"name\":\"MissingField\",\"value_text\":\"1\"}]}]}}"),
			*DataTablePath,
			*DataTablePath,
			*RowStructPath),
		400,
		Root,
		Data));
	const TArray<TSharedPtr<FJsonValue>>* DataTableIssues = nullptr;
	TestTrue(TEXT("DataTable unknown field issue emitted"), Data.IsValid() && Data->TryGetArrayField(TEXT("issues"), DataTableIssues) && JsonArrayContainsCode(DataTableIssues, TEXT("data_table_field_not_found")));

	TestTrue(TEXT("DataTable validate rejects row struct mismatch"), ExecDataDrivenSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-data-table-validate-row-struct-mismatch-001"),
		TEXT("data_table_validate_json"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"json\":{\"schema\":\"ue_agent_interface.data_table.v1\",\"asset_path\":\"%s\",\"row_struct\":\"/Script/Engine.MirrorTableRow\",\"rows\":[{\"row_name\":\"MirrorRow\",\"fields\":[]}]}}"),
			*DataTablePath,
			*DataTablePath),
		400,
		Root,
		Data));
	DataTableIssues = nullptr;
	TestTrue(TEXT("DataTable row struct mismatch issue emitted"), Data.IsValid() && Data->TryGetArrayField(TEXT("issues"), DataTableIssues) && JsonArrayContainsCode(DataTableIssues, TEXT("data_table_row_struct_mismatch")));

	TestTrue(TEXT("DataTable validate rejects duplicate rows"), ExecDataDrivenSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-data-table-validate-duplicate-rows-001"),
		TEXT("data_table_validate_json"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"json\":{\"schema\":\"ue_agent_interface.data_table.v1\",\"asset_path\":\"%s\",\"row_struct\":\"%s\",\"row_policy\":\"merge\",\"rows\":[{\"row_name\":\"DupRow\",\"fields\":[{\"name\":\"Tag\",\"value_text\":\"UAI.Smoke.Dup\"}]},{\"row_name\":\"DupRow\",\"fields\":[{\"name\":\"Tag\",\"value_text\":\"UAI.Smoke.Dup2\"}]}]}}"),
			*DataTablePath,
			*DataTablePath,
			*RowStructPath),
		400,
		Root,
		Data));
	DataTableIssues = nullptr;
	TestTrue(TEXT("DataTable duplicate row issue emitted"), Data.IsValid() && Data->TryGetArrayField(TEXT("issues"), DataTableIssues) && JsonArrayContainsCode(DataTableIssues, TEXT("data_table_duplicate_row_name")));

	const FString MirrorDataTablePath = MakeAutomationAssetPath(TEXT("DT_MirrorRows"));
	const FString MirrorRowStructPath = TEXT("/Script/Engine.MirrorTableRow");
	const FString MirrorInitialRowsJson = FString::Printf(
		TEXT("{\"schema\":\"ue_agent_interface.data_table.v1\",\"asset_path\":\"%s\",\"row_struct\":\"%s\",\"row_policy\":\"replace_all\",\"rows\":[{\"row_name\":\"MirrorRowA\",\"fields\":[{\"name\":\"Name\",\"value_text\":\"OriginalName\"},{\"name\":\"MirroredName\",\"value_text\":\"OriginalMirror\"},{\"name\":\"MirrorEntryType\",\"value_text\":\"Bone\"}]}]}"),
		*MirrorDataTablePath,
		*MirrorRowStructPath);

	TestTrue(TEXT("Apply Mirror DataTable JSON creates table"), ExecDataDrivenSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-data-table-mirror-create-001"),
		TEXT("data_table_apply_json"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"json\":%s,\"create_if_missing\":true,\"overwrite_policy\":\"replace\",\"save_after_apply\":false}"),
			*MirrorDataTablePath,
			*MirrorInitialRowsJson),
		200,
		Root,
		Data));

	TestTrue(TEXT("Mirror DataTable apply rejects bad enum import"), ExecDataDrivenSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-data-table-mirror-bad-import-001"),
		TEXT("data_table_apply_json"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"json\":{\"schema\":\"ue_agent_interface.data_table.v1\",\"asset_path\":\"%s\",\"row_struct\":\"%s\",\"row_policy\":\"patch\",\"update_rows\":[{\"row_name\":\"MirrorRowA\",\"fields\":[{\"name\":\"MirrorEntryType\",\"value_text\":\"DefinitelyNotAMirrorRowType\"},{\"name\":\"Name\",\"value_text\":\"ShouldNotApply\"}]}]},\"save_after_apply\":false}"),
			*MirrorDataTablePath,
			*MirrorDataTablePath,
			*MirrorRowStructPath),
		400,
		Root,
		Data));
	DataTableIssues = nullptr;
	TestTrue(TEXT("Mirror DataTable field import issue emitted"), Data.IsValid() && Data->TryGetArrayField(TEXT("issues"), DataTableIssues) && JsonArrayContainsCode(DataTableIssues, TEXT("data_table_field_import_failed")));

	TestTrue(TEXT("Query Mirror DataTable after failed apply"), ExecDataDrivenSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-data-table-mirror-query-after-bad-apply-001"),
		TEXT("data_table_query_rows"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"row_names\":[\"MirrorRowA\"],\"field_names\":[\"Name\",\"MirrorEntryType\"],\"limit\":4}"),
			*MirrorDataTablePath),
		200,
		Root,
		Data));
	Rows = nullptr;
	RowFields = nullptr;
	TestTrue(TEXT("Mirror DataTable after failed apply has rows"), Data->TryGetArrayField(TEXT("rows"), Rows) && Rows);
	TestTrue(TEXT("Mirror DataTable after failed apply finds row fields"), FindSmokeRowFields(Rows, TEXT("MirrorRowA"), RowFields));
	FString MirrorNameValue;
	TestTrue(TEXT("Mirror DataTable after failed apply returned Name"), FindSmokeFieldValue(RowFields, TEXT("Name"), MirrorNameValue));
	TestEqual(TEXT("Mirror DataTable failed apply did not change Name"), MirrorNameValue, FString(TEXT("OriginalName")));

	return !HasAnyErrors();
}

#endif
