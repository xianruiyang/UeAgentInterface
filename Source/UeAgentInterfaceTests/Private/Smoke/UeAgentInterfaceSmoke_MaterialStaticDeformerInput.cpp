#if WITH_DEV_AUTOMATION_TESTS

#include "UeAgentSmokeTestSharedIncludes.inl"

#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"

static bool SmokeMoveInlinePropertiesToGroupedDetails(
	FAutomationTestBase* Test,
	const FString& EntryFilePath,
	const TSharedPtr<FJsonObject>& EntryObject,
	const FString& OwnerKind,
	const FString& OwnerId,
	const FString& DetailsCategory,
	const FString& SourceCommand)
{
	if (!EntryObject.IsValid())
	{
		if (Test)
		{
			Test->AddError(FString::Printf(TEXT("details migration entry invalid: %s"), *EntryFilePath));
		}
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
	if (!EntryObject->TryGetArrayField(TEXT("properties"), Properties) || !Properties)
	{
		if (Test)
		{
			Test->AddError(FString::Printf(TEXT("inline properties missing before details migration: %s"), *EntryFilePath));
		}
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> PropertiesCopy = *Properties;
	EntryObject->RemoveField(TEXT("properties"));

	UeAgentDetailsAuthoring::FOwnerInfo OwnerInfo;
	OwnerInfo.OwnerKind = OwnerKind;
	OwnerInfo.OwnerId = OwnerId;
	OwnerInfo.OwnerName = OwnerId;
	OwnerInfo.SourceCommand = SourceCommand;

	FString SafeOwnerId = FPaths::MakeValidFileName(OwnerId, TEXT('_'));
	if (SafeOwnerId.IsEmpty())
	{
		SafeOwnerId = TEXT("owner");
	}
	const FString DetailsIndexFile = FPaths::Combine(TEXT("details"), DetailsCategory, SafeOwnerId, TEXT("index.json"));
	return SmokeWriteGroupedDetailsForRef(
		Test,
		EntryFilePath,
		EntryObject,
		TEXT("details"),
		DetailsIndexFile,
		OwnerInfo,
		PropertiesCopy);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceMaterialSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.MaterialWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceMaterialSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString MaterialAssetPath = MakeAutomationAssetPath(TEXT("MAT"));
	const FString SwitchMaterialAssetPath = MakeAutomationAssetPath(TEXT("MATSW"));
	const FString MaskMaterialAssetPath = MakeAutomationAssetPath(TEXT("MATMASK"));
	const FString MaterialInstanceAssetPath = MakeAutomationAssetPath(TEXT("MIC"));
	const FString MaskMaterialInstanceAssetPath = MakeAutomationAssetPath(TEXT("MICMASK"));
	const FString SwitchParameterName = TEXT("UseAltColor");
	const FString MaskParameterName = TEXT("UseRGB");

	const FString CreateBody = MakeExecRequestBody(
		TEXT("smoke-material-create-001"),
		TEXT("material_create"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"compile_after_create\":false,\"save_after_create\":false}"), *MaterialAssetPath));
	FHttpSmokeResult CreateResult;
	TestTrue(TEXT("Material create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateBody, CreateResult));
	TestEqual(TEXT("Material create status code"), CreateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CreateJson;
	TestTrue(TEXT("Material create response parses as JSON"), ParseJson(CreateResult.Body, CreateJson));
	TestTrue(TEXT("Material create root ok=true"), CreateJson.IsValid() && CreateJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* CreateData = nullptr;
	TestTrue(TEXT("Material create contains data object"), CreateJson.IsValid() && CreateJson->TryGetObjectField(TEXT("data"), CreateData) && CreateData && CreateData->IsValid());
	if (CreateData && CreateData->IsValid())
	{
		TestEqual(TEXT("Material create asset path"), (*CreateData)->GetStringField(TEXT("asset_path")), MaterialAssetPath);
		TestTrue(TEXT("Material create has object_path"), !(*CreateData)->GetStringField(TEXT("object_path")).IsEmpty());
	}

	const FString GetInfoBody = MakeExecRequestBody(
		TEXT("smoke-material-info-001"),
		TEXT("material_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *MaterialAssetPath));
	FHttpSmokeResult GetInfoResult;
	TestTrue(TEXT("Material get_info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoBody, GetInfoResult));
	TestEqual(TEXT("Material get_info status code"), GetInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoJson;
	TestTrue(TEXT("Material get_info response parses as JSON"), ParseJson(GetInfoResult.Body, GetInfoJson));
	TestTrue(TEXT("Material get_info root ok=true"), GetInfoJson.IsValid() && GetInfoJson->GetBoolField(TEXT("ok")));

	int32 ExpressionCountBefore = 0;
	const TSharedPtr<FJsonObject>* GetInfoData = nullptr;
	TestTrue(TEXT("Material get_info contains data object"), GetInfoJson.IsValid() && GetInfoJson->TryGetObjectField(TEXT("data"), GetInfoData) && GetInfoData && GetInfoData->IsValid());
	if (GetInfoData && GetInfoData->IsValid())
	{
		ExpressionCountBefore = static_cast<int32>((*GetInfoData)->GetIntegerField(TEXT("expression_count")));
		TestFalse(TEXT("Material get_info reports not instance"), (*GetInfoData)->GetBoolField(TEXT("is_material_instance")));
	}

	const FString AddExpressionBody = MakeExecRequestBody(
		TEXT("smoke-material-add-expression-001"),
		TEXT("material_add_expression"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"expression_class\":\"/Script/Engine.MaterialExpressionConstant3Vector\",\"node_pos_x\":120,\"node_pos_y\":80,\"compile_after_add\":false,\"save_after_add\":false}"),
			*MaterialAssetPath));
	FHttpSmokeResult AddExpressionResult;
	TestTrue(TEXT("Material add_expression request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddExpressionBody, AddExpressionResult));
	TestEqual(TEXT("Material add_expression status code"), AddExpressionResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddExpressionJson;
	TestTrue(TEXT("Material add_expression response parses as JSON"), ParseJson(AddExpressionResult.Body, AddExpressionJson));
	TestTrue(TEXT("Material add_expression root ok=true"), AddExpressionJson.IsValid() && AddExpressionJson->GetBoolField(TEXT("ok")));

	FString ExpressionGuid;
	const TSharedPtr<FJsonObject>* AddExpressionData = nullptr;
	TestTrue(TEXT("Material add_expression contains data object"), AddExpressionJson.IsValid() && AddExpressionJson->TryGetObjectField(TEXT("data"), AddExpressionData) && AddExpressionData && AddExpressionData->IsValid());
	if (AddExpressionData && AddExpressionData->IsValid())
	{
		const TSharedPtr<FJsonObject>* ExpressionObject = nullptr;
		TestTrue(TEXT("Material add_expression contains expression object"), (*AddExpressionData)->TryGetObjectField(TEXT("expression"), ExpressionObject) && ExpressionObject && ExpressionObject->IsValid());
		if (ExpressionObject && ExpressionObject->IsValid())
		{
			ExpressionGuid = (*ExpressionObject)->GetStringField(TEXT("guid"));
			TestTrue(TEXT("Material add_expression guid is non-empty"), !ExpressionGuid.IsEmpty());
		}
	}

	const FString ConnectBody = MakeExecRequestBody(
		TEXT("smoke-material-connect-001"),
		TEXT("material_connect_expression_to_property"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"expression_guid\":\"%s\",\"material_property\":\"BaseColor\",\"compile_after_connect\":false,\"save_after_connect\":false}"),
			*MaterialAssetPath,
			*ExpressionGuid));
	FHttpSmokeResult ConnectResult;
	TestTrue(TEXT("Material connect_expression_to_property request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ConnectBody, ConnectResult));
	TestEqual(TEXT("Material connect_expression_to_property status code"), ConnectResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ConnectJson;
	TestTrue(TEXT("Material connect_expression_to_property response parses as JSON"), ParseJson(ConnectResult.Body, ConnectJson));
	TestTrue(TEXT("Material connect_expression_to_property root ok=true"), ConnectJson.IsValid() && ConnectJson->GetBoolField(TEXT("ok")));

	const FString ListExpressionsBody = MakeExecRequestBody(
		TEXT("smoke-material-list-expr-001"),
		TEXT("material_list_expressions"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *MaterialAssetPath));
	FHttpSmokeResult ListExpressionsResult;
	TestTrue(TEXT("Material list_expressions request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListExpressionsBody, ListExpressionsResult));
	TestEqual(TEXT("Material list_expressions status code"), ListExpressionsResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ListExpressionsJson;
	TestTrue(TEXT("Material list_expressions response parses as JSON"), ParseJson(ListExpressionsResult.Body, ListExpressionsJson));
	TestTrue(TEXT("Material list_expressions root ok=true"), ListExpressionsJson.IsValid() && ListExpressionsJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* ListExpressionsData = nullptr;
	TestTrue(TEXT("Material list_expressions contains data object"), ListExpressionsJson.IsValid() && ListExpressionsJson->TryGetObjectField(TEXT("data"), ListExpressionsData) && ListExpressionsData && ListExpressionsData->IsValid());
	if (ListExpressionsData && ListExpressionsData->IsValid())
	{
		const int32 ExpressionCountAfter = static_cast<int32>((*ListExpressionsData)->GetIntegerField(TEXT("count")));
		TestTrue(TEXT("Material expression count increased"), ExpressionCountAfter >= ExpressionCountBefore + 1);
	}

	const FString CompileBody = MakeExecRequestBody(
		TEXT("smoke-material-compile-001"),
		TEXT("material_compile"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_messages\":true,\"max_messages\":16,\"save_after_compile\":false}"), *MaterialAssetPath));
	FHttpSmokeResult CompileResult;
	TestTrue(TEXT("Material compile request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CompileBody, CompileResult));
	TestEqual(TEXT("Material compile status code"), CompileResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CompileJson;
	TestTrue(TEXT("Material compile response parses as JSON"), ParseJson(CompileResult.Body, CompileJson));
	TestTrue(TEXT("Material compile root ok=true"), CompileJson.IsValid() && CompileJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* CompileData = nullptr;
	TestTrue(TEXT("Material compile contains data object"), CompileJson.IsValid() && CompileJson->TryGetObjectField(TEXT("data"), CompileData) && CompileData && CompileData->IsValid());
	if (CompileData && CompileData->IsValid())
	{
		TestTrue(TEXT("Material compile reports compiled=true"), (*CompileData)->GetBoolField(TEXT("compiled")));
		TestFalse(TEXT("Material compile reports no error"), (*CompileData)->GetBoolField(TEXT("has_error")));
	}

	const FString CreateSwitchMaterialBody = MakeExecRequestBody(
		TEXT("smoke-material-static-switch-create-001"),
		TEXT("material_create"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"compile_after_create\":false,\"save_after_create\":false}"), *SwitchMaterialAssetPath));
	FHttpSmokeResult CreateSwitchMaterialResult;
	TestTrue(TEXT("Material static switch create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateSwitchMaterialBody, CreateSwitchMaterialResult));
	TestEqual(TEXT("Material static switch create status code"), CreateSwitchMaterialResult.StatusCode, 200);

	auto AddMaterialExpressionAndGetGuid = [&](const FString& RequestId, const FString& AssetPath, const FString& ExpressionClassPath, int32 PosX, int32 PosY, FString& OutGuid) -> bool
	{
		const FString Body = MakeExecRequestBody(
			RequestId,
			TEXT("material_add_expression"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"expression_class\":\"%s\",\"node_pos_x\":%d,\"node_pos_y\":%d,\"compile_after_add\":false,\"save_after_add\":false}"),
				*AssetPath, *ExpressionClassPath, PosX, PosY));
		FHttpSmokeResult Result;
		if (!ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, Body, Result) || Result.StatusCode != 200)
		{
			return false;
		}

		TSharedPtr<FJsonObject> Json;
		if (!ParseJson(Result.Body, Json) || !Json.IsValid() || !Json->GetBoolField(TEXT("ok")))
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* Data = nullptr;
		if (!Json->TryGetObjectField(TEXT("data"), Data) || !Data || !Data->IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* ExpressionObject = nullptr;
		if (!(*Data)->TryGetObjectField(TEXT("expression"), ExpressionObject) || !ExpressionObject || !ExpressionObject->IsValid())
		{
			return false;
		}

		OutGuid = (*ExpressionObject)->GetStringField(TEXT("guid"));
		return !OutGuid.IsEmpty();
	};

	FString StaticSwitchGuid;
	TestTrue(TEXT("Material static switch add_expression succeeded"), AddMaterialExpressionAndGetGuid(
		TEXT("smoke-material-static-switch-add-switch-001"),
		SwitchMaterialAssetPath,
		TEXT("/Script/Engine.MaterialExpressionStaticSwitchParameter"),
		-160,
		0,
		StaticSwitchGuid));

	FString RedConstGuid;
	TestTrue(TEXT("Material static switch add red const succeeded"), AddMaterialExpressionAndGetGuid(
		TEXT("smoke-material-static-switch-add-red-001"),
		SwitchMaterialAssetPath,
		TEXT("/Script/Engine.MaterialExpressionConstant3Vector"),
		-420,
		-120,
		RedConstGuid));

	FString BlueConstGuid;
	TestTrue(TEXT("Material static switch add blue const succeeded"), AddMaterialExpressionAndGetGuid(
		TEXT("smoke-material-static-switch-add-blue-001"),
		SwitchMaterialAssetPath,
		TEXT("/Script/Engine.MaterialExpressionConstant3Vector"),
		-420,
		120,
		BlueConstGuid));

	const FString SetSwitchNameBody = MakeExecRequestBody(
		TEXT("smoke-material-static-switch-set-name-001"),
		TEXT("material_set_expression_property"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"expression_guid\":\"%s\",\"property_name\":\"ParameterName\",\"value_text\":\"%s\",\"compile_after_set\":false,\"save_after_set\":false}"),
			*SwitchMaterialAssetPath, *StaticSwitchGuid, *SwitchParameterName));
	FHttpSmokeResult SetSwitchNameResult;
	TestTrue(TEXT("Material static switch set expression property request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetSwitchNameBody, SetSwitchNameResult));
	TestEqual(TEXT("Material static switch set expression property status code"), SetSwitchNameResult.StatusCode, 200);

	const FString CompileSwitchMaterialBody = MakeExecRequestBody(
		TEXT("smoke-material-static-switch-compile-001"),
		TEXT("material_compile"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_messages\":true,\"max_messages\":16,\"save_after_compile\":false}"), *SwitchMaterialAssetPath));
	FHttpSmokeResult CompileSwitchMaterialResult;
	TestTrue(TEXT("Material static switch compile request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CompileSwitchMaterialBody, CompileSwitchMaterialResult));
	TestEqual(TEXT("Material static switch compile status code"), CompileSwitchMaterialResult.StatusCode, 200);

	const FString CreateMaterialInstanceBody = MakeExecRequestBody(
		TEXT("smoke-material-static-switch-create-instance-001"),
		TEXT("material_instance_create"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"parent_material_path\":\"%s\",\"save_after_create\":false}"), *MaterialInstanceAssetPath, *SwitchMaterialAssetPath));
	FHttpSmokeResult CreateMaterialInstanceResult;
	TestTrue(TEXT("Material instance create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateMaterialInstanceBody, CreateMaterialInstanceResult));
	TestEqual(TEXT("Material instance create status code"), CreateMaterialInstanceResult.StatusCode, 200);

	auto StaticSwitchValueMatches = [&](const TSharedPtr<FJsonObject>* Data, const FString& Name, const bool bExpected) -> bool
	{
		if (!Data || !Data->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* StaticSwitches = nullptr;
		if (!(*Data)->TryGetArrayField(TEXT("static_switch_parameters"), StaticSwitches) || !StaticSwitches)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& SwitchValue : *StaticSwitches)
		{
			const TSharedPtr<FJsonObject>* SwitchObject = nullptr;
			if (!SwitchValue.IsValid() || !SwitchValue->TryGetObject(SwitchObject) || !SwitchObject || !SwitchObject->IsValid())
			{
				continue;
			}

			if ((*SwitchObject)->GetStringField(TEXT("name")) == Name)
			{
				return (*SwitchObject)->GetBoolField(TEXT("value")) == bExpected;
			}
		}

		return false;
	};

	auto StaticComponentMaskMatches = [&](const TSharedPtr<FJsonObject>* Data, const FString& Name, const bool bExpectedR, const bool bExpectedG, const bool bExpectedB, const bool bExpectedA) -> bool
	{
		if (!Data || !Data->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Masks = nullptr;
		if (!(*Data)->TryGetArrayField(TEXT("static_component_mask_parameters"), Masks) || !Masks)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& MaskValue : *Masks)
		{
			const TSharedPtr<FJsonObject>* MaskObject = nullptr;
			if (!MaskValue.IsValid() || !MaskValue->TryGetObject(MaskObject) || !MaskObject || !MaskObject->IsValid())
			{
				continue;
			}

			if ((*MaskObject)->GetStringField(TEXT("name")) == Name)
			{
				return (*MaskObject)->GetBoolField(TEXT("r")) == bExpectedR &&
					(*MaskObject)->GetBoolField(TEXT("g")) == bExpectedG &&
					(*MaskObject)->GetBoolField(TEXT("b")) == bExpectedB &&
					(*MaskObject)->GetBoolField(TEXT("a")) == bExpectedA;
			}
		}

		return false;
	};

	const FString GetMaterialInstanceInfoBeforeBody = MakeExecRequestBody(
		TEXT("smoke-material-static-switch-info-before-001"),
		TEXT("material_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *MaterialInstanceAssetPath));
	FHttpSmokeResult GetMaterialInstanceInfoBeforeResult;
	TestTrue(TEXT("Material instance get_info before request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetMaterialInstanceInfoBeforeBody, GetMaterialInstanceInfoBeforeResult));
	TestEqual(TEXT("Material instance get_info before status code"), GetMaterialInstanceInfoBeforeResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetMaterialInstanceInfoBeforeJson;
	TestTrue(TEXT("Material instance get_info before parses JSON"), ParseJson(GetMaterialInstanceInfoBeforeResult.Body, GetMaterialInstanceInfoBeforeJson));
	const TSharedPtr<FJsonObject>* GetMaterialInstanceInfoBeforeData = nullptr;
	TestTrue(TEXT("Material instance get_info before contains data"), GetMaterialInstanceInfoBeforeJson.IsValid() && GetMaterialInstanceInfoBeforeJson->TryGetObjectField(TEXT("data"), GetMaterialInstanceInfoBeforeData) && GetMaterialInstanceInfoBeforeData && GetMaterialInstanceInfoBeforeData->IsValid());
	if (GetMaterialInstanceInfoBeforeData && GetMaterialInstanceInfoBeforeData->IsValid())
	{
		TestTrue(TEXT("Material instance get_info reports instance"), (*GetMaterialInstanceInfoBeforeData)->GetBoolField(TEXT("is_material_instance")));
		TestEqual(TEXT("Material instance parent path matches"), (*GetMaterialInstanceInfoBeforeData)->GetStringField(TEXT("parent_path")), SwitchMaterialAssetPath + TEXT(".") + FPackageName::GetLongPackageAssetName(SwitchMaterialAssetPath));
		TestTrue(TEXT("Material instance static switch default false"), StaticSwitchValueMatches(GetMaterialInstanceInfoBeforeData, SwitchParameterName, false));
	}

	const FString SetStaticSwitchBody = MakeExecRequestBody(
		TEXT("smoke-material-static-switch-set-001"),
		TEXT("material_set_static_switch_parameter"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"parameter_name\":\"%s\",\"value\":true,\"save_after_set\":false}"), *MaterialInstanceAssetPath, *SwitchParameterName));
	FHttpSmokeResult SetStaticSwitchResult;
	TestTrue(TEXT("Material set_static_switch_parameter request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetStaticSwitchBody, SetStaticSwitchResult));
	TestEqual(TEXT("Material set_static_switch_parameter status code"), SetStaticSwitchResult.StatusCode, 200);

	const FString GetMaterialInstanceInfoAfterBody = MakeExecRequestBody(
		TEXT("smoke-material-static-switch-info-after-001"),
		TEXT("material_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *MaterialInstanceAssetPath));
	FHttpSmokeResult GetMaterialInstanceInfoAfterResult;
	TestTrue(TEXT("Material instance get_info after request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetMaterialInstanceInfoAfterBody, GetMaterialInstanceInfoAfterResult));
	TestEqual(TEXT("Material instance get_info after status code"), GetMaterialInstanceInfoAfterResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetMaterialInstanceInfoAfterJson;
	TestTrue(TEXT("Material instance get_info after parses JSON"), ParseJson(GetMaterialInstanceInfoAfterResult.Body, GetMaterialInstanceInfoAfterJson));
	const TSharedPtr<FJsonObject>* GetMaterialInstanceInfoAfterData = nullptr;
	TestTrue(TEXT("Material instance get_info after contains data"), GetMaterialInstanceInfoAfterJson.IsValid() && GetMaterialInstanceInfoAfterJson->TryGetObjectField(TEXT("data"), GetMaterialInstanceInfoAfterData) && GetMaterialInstanceInfoAfterData && GetMaterialInstanceInfoAfterData->IsValid());
	if (GetMaterialInstanceInfoAfterData && GetMaterialInstanceInfoAfterData->IsValid())
	{
		TestTrue(TEXT("Material instance static switch updated true"), StaticSwitchValueMatches(GetMaterialInstanceInfoAfterData, SwitchParameterName, true));
	}

	const FString CreateMaskMaterialBody = MakeExecRequestBody(
		TEXT("smoke-material-mask-create-001"),
		TEXT("material_create"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"compile_after_create\":false,\"save_after_create\":false}"), *MaskMaterialAssetPath));
	FHttpSmokeResult CreateMaskMaterialResult;
	TestTrue(TEXT("Material mask create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateMaskMaterialBody, CreateMaskMaterialResult));
	TestEqual(TEXT("Material mask create status code"), CreateMaskMaterialResult.StatusCode, 200);

	FString StaticMaskGuid;
	TestTrue(TEXT("Material static component mask add_expression succeeded"), AddMaterialExpressionAndGetGuid(
		TEXT("smoke-material-mask-add-mask-001"),
		MaskMaterialAssetPath,
		TEXT("/Script/Engine.MaterialExpressionStaticComponentMaskParameter"),
		-160,
		0,
		StaticMaskGuid));

	FString MaskConstGuid;
	TestTrue(TEXT("Material static component mask add const succeeded"), AddMaterialExpressionAndGetGuid(
		TEXT("smoke-material-mask-add-const-001"),
		MaskMaterialAssetPath,
		TEXT("/Script/Engine.MaterialExpressionConstant4Vector"),
		-420,
		0,
		MaskConstGuid));

	const FString SetMaskNameBody = MakeExecRequestBody(
		TEXT("smoke-material-mask-set-name-001"),
		TEXT("material_set_expression_property"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"expression_guid\":\"%s\",\"property_name\":\"ParameterName\",\"value_text\":\"%s\",\"compile_after_set\":false,\"save_after_set\":false}"),
			*MaskMaterialAssetPath, *StaticMaskGuid, *MaskParameterName));
	FHttpSmokeResult SetMaskNameResult;
	TestTrue(TEXT("Material static component mask set expression property request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetMaskNameBody, SetMaskNameResult));
	TestEqual(TEXT("Material static component mask set expression property status code"), SetMaskNameResult.StatusCode, 200);

	const FString CompileMaskMaterialBody = MakeExecRequestBody(
		TEXT("smoke-material-mask-compile-001"),
		TEXT("material_compile"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_messages\":true,\"max_messages\":16,\"save_after_compile\":false}"), *MaskMaterialAssetPath));
	FHttpSmokeResult CompileMaskMaterialResult;
	TestTrue(TEXT("Material static component mask compile request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CompileMaskMaterialBody, CompileMaskMaterialResult));
	TestEqual(TEXT("Material static component mask compile status code"), CompileMaskMaterialResult.StatusCode, 200);

	const FString CreateMaskMaterialInstanceBody = MakeExecRequestBody(
		TEXT("smoke-material-mask-create-instance-001"),
		TEXT("material_instance_create"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"parent_material_path\":\"%s\",\"save_after_create\":false}"), *MaskMaterialInstanceAssetPath, *MaskMaterialAssetPath));
	FHttpSmokeResult CreateMaskMaterialInstanceResult;
	TestTrue(TEXT("Material mask instance create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateMaskMaterialInstanceBody, CreateMaskMaterialInstanceResult));
	TestEqual(TEXT("Material mask instance create status code"), CreateMaskMaterialInstanceResult.StatusCode, 200);

	const FString GetMaskMaterialInstanceInfoBeforeBody = MakeExecRequestBody(
		TEXT("smoke-material-mask-info-before-001"),
		TEXT("material_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *MaskMaterialInstanceAssetPath));
	FHttpSmokeResult GetMaskMaterialInstanceInfoBeforeResult;
	TestTrue(TEXT("Material mask instance get_info before request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetMaskMaterialInstanceInfoBeforeBody, GetMaskMaterialInstanceInfoBeforeResult));
	TestEqual(TEXT("Material mask instance get_info before status code"), GetMaskMaterialInstanceInfoBeforeResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetMaskMaterialInstanceInfoBeforeJson;
	TestTrue(TEXT("Material mask instance get_info before parses JSON"), ParseJson(GetMaskMaterialInstanceInfoBeforeResult.Body, GetMaskMaterialInstanceInfoBeforeJson));
	const TSharedPtr<FJsonObject>* GetMaskMaterialInstanceInfoBeforeData = nullptr;
	TestTrue(TEXT("Material mask instance get_info before contains data"), GetMaskMaterialInstanceInfoBeforeJson.IsValid() && GetMaskMaterialInstanceInfoBeforeJson->TryGetObjectField(TEXT("data"), GetMaskMaterialInstanceInfoBeforeData) && GetMaskMaterialInstanceInfoBeforeData && GetMaskMaterialInstanceInfoBeforeData->IsValid());
	if (GetMaskMaterialInstanceInfoBeforeData && GetMaskMaterialInstanceInfoBeforeData->IsValid())
	{
		TestTrue(TEXT("Material mask instance default state matches"), StaticComponentMaskMatches(GetMaskMaterialInstanceInfoBeforeData, MaskParameterName, true, false, false, false));
	}

	const FString SetMaskParameterBody = MakeExecRequestBody(
		TEXT("smoke-material-mask-set-001"),
		TEXT("material_set_parameter"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"parameter_type\":\"static_component_mask\",\"parameter_name\":\"%s\",\"value\":{\"r\":true,\"g\":false,\"b\":true,\"a\":false},\"save_after_set\":false}"), *MaskMaterialInstanceAssetPath, *MaskParameterName));
	FHttpSmokeResult SetMaskParameterResult;
	TestTrue(TEXT("Material set_parameter static_component_mask request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetMaskParameterBody, SetMaskParameterResult));
	TestEqual(TEXT("Material set_parameter static_component_mask status code"), SetMaskParameterResult.StatusCode, 200);

	const FString GetMaskMaterialInstanceInfoAfterBody = MakeExecRequestBody(
		TEXT("smoke-material-mask-info-after-001"),
		TEXT("material_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *MaskMaterialInstanceAssetPath));
	FHttpSmokeResult GetMaskMaterialInstanceInfoAfterResult;
	TestTrue(TEXT("Material mask instance get_info after request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetMaskMaterialInstanceInfoAfterBody, GetMaskMaterialInstanceInfoAfterResult));
	TestEqual(TEXT("Material mask instance get_info after status code"), GetMaskMaterialInstanceInfoAfterResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetMaskMaterialInstanceInfoAfterJson;
	TestTrue(TEXT("Material mask instance get_info after parses JSON"), ParseJson(GetMaskMaterialInstanceInfoAfterResult.Body, GetMaskMaterialInstanceInfoAfterJson));
	const TSharedPtr<FJsonObject>* GetMaskMaterialInstanceInfoAfterData = nullptr;
	TestTrue(TEXT("Material mask instance get_info after contains data"), GetMaskMaterialInstanceInfoAfterJson.IsValid() && GetMaskMaterialInstanceInfoAfterJson->TryGetObjectField(TEXT("data"), GetMaskMaterialInstanceInfoAfterData) && GetMaskMaterialInstanceInfoAfterData && GetMaskMaterialInstanceInfoAfterData->IsValid());
	if (GetMaskMaterialInstanceInfoAfterData && GetMaskMaterialInstanceInfoAfterData->IsValid())
	{
		TestTrue(TEXT("Material mask instance updated state matches"), StaticComponentMaskMatches(GetMaskMaterialInstanceInfoAfterData, MaskParameterName, true, false, true, false));
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceStaticMeshSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.StaticMeshWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceStaticMeshSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString StaticMeshAssetPath = TEXT("/Engine/BasicShapes/Cube.Cube");
	const FString SocketName = FString::Printf(TEXT("SmokeSocket_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString RenamedSocketName = SocketName + TEXT("_Renamed");
	const FString ReplacementMaterialPath = TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial");

	const FString GetInfoBeforeBody = MakeExecRequestBody(
		TEXT("smoke-staticmesh-info-before-001"),
		TEXT("static_mesh_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *StaticMeshAssetPath));
	FHttpSmokeResult GetInfoBeforeResult;
	TestTrue(TEXT("StaticMesh get_info before request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoBeforeBody, GetInfoBeforeResult));
	TestEqual(TEXT("StaticMesh get_info before status code"), GetInfoBeforeResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoBeforeJson;
	TestTrue(TEXT("StaticMesh get_info before response parses as JSON"), ParseJson(GetInfoBeforeResult.Body, GetInfoBeforeJson));
	TestTrue(TEXT("StaticMesh get_info before root ok=true"), GetInfoBeforeJson.IsValid() && GetInfoBeforeJson->GetBoolField(TEXT("ok")));

	int32 SocketCountBefore = 0;
	int32 SphereCountBefore = 0;
	int32 CapsuleCountBefore = 0;
	int32 MaterialSlotCountBefore = 0;
	FString OriginalMaterialPath;
	const TSharedPtr<FJsonObject>* GetInfoBeforeData = nullptr;
	TestTrue(TEXT("StaticMesh get_info before contains data object"), GetInfoBeforeJson.IsValid() && GetInfoBeforeJson->TryGetObjectField(TEXT("data"), GetInfoBeforeData) && GetInfoBeforeData && GetInfoBeforeData->IsValid());
	if (GetInfoBeforeData && GetInfoBeforeData->IsValid())
	{
		SocketCountBefore = static_cast<int32>((*GetInfoBeforeData)->GetIntegerField(TEXT("socket_count")));
		TestTrue(TEXT("StaticMesh get_info before has positive sphere radius"), (*GetInfoBeforeData)->GetNumberField(TEXT("bounds_sphere_radius")) > 0.0);

		MaterialSlotCountBefore = static_cast<int32>((*GetInfoBeforeData)->GetIntegerField(TEXT("material_slot_count")));
		TestTrue(TEXT("StaticMesh material slot count is positive"), MaterialSlotCountBefore > 0);

		const TArray<TSharedPtr<FJsonValue>>* MaterialSlots = nullptr;
		if ((*GetInfoBeforeData)->TryGetArrayField(TEXT("material_slots"), MaterialSlots) && MaterialSlots && MaterialSlots->Num() > 0)
		{
			const TSharedPtr<FJsonObject>* MaterialSlotObject = nullptr;
			if ((*MaterialSlots)[0].IsValid() && (*MaterialSlots)[0]->TryGetObject(MaterialSlotObject) && MaterialSlotObject && MaterialSlotObject->IsValid())
			{
				OriginalMaterialPath = (*MaterialSlotObject)->GetStringField(TEXT("material_path"));
			}
		}

		const TSharedPtr<FJsonObject>* CollisionObj = nullptr;
		if ((*GetInfoBeforeData)->TryGetObjectField(TEXT("collision"), CollisionObj) && CollisionObj && CollisionObj->IsValid())
		{
			SphereCountBefore = static_cast<int32>((*CollisionObj)->GetIntegerField(TEXT("sphere_count")));
			CapsuleCountBefore = static_cast<int32>((*CollisionObj)->GetIntegerField(TEXT("capsule_count")));
		}
	}

	const FString SetMaterialSlotBody = MakeExecRequestBody(
		TEXT("smoke-staticmesh-set-material-slot-001"),
		TEXT("static_mesh_set_material_slot"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"slot_index\":0,\"material_path\":\"%s\",\"save_after_set\":false}"), *StaticMeshAssetPath, *ReplacementMaterialPath));
	FHttpSmokeResult SetMaterialSlotResult;
	TestTrue(TEXT("StaticMesh set_material_slot request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetMaterialSlotBody, SetMaterialSlotResult));
	TestEqual(TEXT("StaticMesh set_material_slot status code"), SetMaterialSlotResult.StatusCode, 200);

	const FString SetCollisionSpheresBody = MakeExecRequestBody(
		TEXT("smoke-staticmesh-set-collision-spheres-001"),
		TEXT("static_mesh_set_collision_spheres"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"spheres\":[{\"center\":{\"x\":0.0,\"y\":0.0,\"z\":0.0},\"radius\":25.0}],\"clear_other_shapes\":false,\"save_after_set\":false}"), *StaticMeshAssetPath));
	FHttpSmokeResult SetCollisionSpheresResult;
	TestTrue(TEXT("StaticMesh set_collision_spheres request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetCollisionSpheresBody, SetCollisionSpheresResult));
	TestEqual(TEXT("StaticMesh set_collision_spheres status code"), SetCollisionSpheresResult.StatusCode, 200);

	const FString SetCollisionCapsulesBody = MakeExecRequestBody(
		TEXT("smoke-staticmesh-set-collision-capsules-001"),
		TEXT("static_mesh_set_collision_capsules"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"capsules\":[{\"center\":{\"x\":0.0,\"y\":0.0,\"z\":0.0},\"rotation\":{\"pitch\":0.0,\"yaw\":0.0,\"roll\":90.0},\"radius\":12.0,\"length\":40.0}],\"clear_other_shapes\":false,\"save_after_set\":false}"), *StaticMeshAssetPath));
	FHttpSmokeResult SetCollisionCapsulesResult;
	TestTrue(TEXT("StaticMesh set_collision_capsules request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetCollisionCapsulesBody, SetCollisionCapsulesResult));
	TestEqual(TEXT("StaticMesh set_collision_capsules status code"), SetCollisionCapsulesResult.StatusCode, 200);

	const FString GetInfoAfterMaterialCollisionBody = MakeExecRequestBody(
		TEXT("smoke-staticmesh-info-after-material-collision-001"),
		TEXT("static_mesh_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *StaticMeshAssetPath));
	FHttpSmokeResult GetInfoAfterMaterialCollisionResult;
	TestTrue(TEXT("StaticMesh get_info after material/collision request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoAfterMaterialCollisionBody, GetInfoAfterMaterialCollisionResult));
	TestEqual(TEXT("StaticMesh get_info after material/collision status code"), GetInfoAfterMaterialCollisionResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoAfterMaterialCollisionJson;
	TestTrue(TEXT("StaticMesh get_info after material/collision parses as JSON"), ParseJson(GetInfoAfterMaterialCollisionResult.Body, GetInfoAfterMaterialCollisionJson));
	const TSharedPtr<FJsonObject>* GetInfoAfterMaterialCollisionData = nullptr;
	TestTrue(TEXT("StaticMesh get_info after material/collision contains data object"), GetInfoAfterMaterialCollisionJson.IsValid() && GetInfoAfterMaterialCollisionJson->TryGetObjectField(TEXT("data"), GetInfoAfterMaterialCollisionData) && GetInfoAfterMaterialCollisionData && GetInfoAfterMaterialCollisionData->IsValid());
	if (GetInfoAfterMaterialCollisionData && GetInfoAfterMaterialCollisionData->IsValid())
	{
		TestEqual(TEXT("StaticMesh material slot count unchanged"), static_cast<int32>((*GetInfoAfterMaterialCollisionData)->GetIntegerField(TEXT("material_slot_count"))), MaterialSlotCountBefore);

		const TArray<TSharedPtr<FJsonValue>>* MaterialSlots = nullptr;
		TestTrue(TEXT("StaticMesh get_info after material/collision has material_slots"), (*GetInfoAfterMaterialCollisionData)->TryGetArrayField(TEXT("material_slots"), MaterialSlots) && MaterialSlots && MaterialSlots->Num() > 0);
		if (MaterialSlots && MaterialSlots->Num() > 0)
		{
			const TSharedPtr<FJsonObject>* MaterialSlotObject = nullptr;
			if ((*MaterialSlots)[0].IsValid() && (*MaterialSlots)[0]->TryGetObject(MaterialSlotObject) && MaterialSlotObject && MaterialSlotObject->IsValid())
			{
				TestEqual(TEXT("StaticMesh slot 0 material updated"), (*MaterialSlotObject)->GetStringField(TEXT("material_path")), ReplacementMaterialPath);
			}
		}

		const TSharedPtr<FJsonObject>* CollisionObj = nullptr;
		TestTrue(TEXT("StaticMesh get_info after material/collision has collision object"), (*GetInfoAfterMaterialCollisionData)->TryGetObjectField(TEXT("collision"), CollisionObj) && CollisionObj && CollisionObj->IsValid());
		if (CollisionObj && CollisionObj->IsValid())
		{
			TestTrue(TEXT("StaticMesh sphere count increased"), static_cast<int32>((*CollisionObj)->GetIntegerField(TEXT("sphere_count"))) >= SphereCountBefore + 1);
			TestTrue(TEXT("StaticMesh capsule count increased"), static_cast<int32>((*CollisionObj)->GetIntegerField(TEXT("capsule_count"))) >= CapsuleCountBefore + 1);
		}
	}

	const FString AddSocketBody = MakeExecRequestBody(
		TEXT("smoke-staticmesh-add-socket-001"),
		TEXT("static_mesh_add_socket"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"socket_name\":\"%s\",\"location\":{\"x\":10.0,\"y\":20.0,\"z\":30.0},\"rotation\":{\"pitch\":0.0,\"yaw\":45.0,\"roll\":0.0},\"scale\":{\"x\":1.0,\"y\":1.0,\"z\":1.0},\"save_after_set\":false}"),
			*StaticMeshAssetPath,
			*SocketName));
	FHttpSmokeResult AddSocketResult;
	TestTrue(TEXT("StaticMesh add_socket request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddSocketBody, AddSocketResult));
	TestEqual(TEXT("StaticMesh add_socket status code"), AddSocketResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddSocketJson;
	TestTrue(TEXT("StaticMesh add_socket response parses as JSON"), ParseJson(AddSocketResult.Body, AddSocketJson));
	TestTrue(TEXT("StaticMesh add_socket root ok=true"), AddSocketJson.IsValid() && AddSocketJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* AddSocketData = nullptr;
	TestTrue(TEXT("StaticMesh add_socket contains data object"), AddSocketJson.IsValid() && AddSocketJson->TryGetObjectField(TEXT("data"), AddSocketData) && AddSocketData && AddSocketData->IsValid());
	if (AddSocketData && AddSocketData->IsValid())
	{
		const TSharedPtr<FJsonObject>* AddedSocket = nullptr;
		TestTrue(TEXT("StaticMesh add_socket contains socket object"), (*AddSocketData)->TryGetObjectField(TEXT("socket"), AddedSocket) && AddedSocket && AddedSocket->IsValid());
		if (AddedSocket && AddedSocket->IsValid())
		{
			TestEqual(TEXT("StaticMesh add_socket name"), (*AddedSocket)->GetStringField(TEXT("socket_name")), SocketName);
		}
		TestFalse(TEXT("StaticMesh add_socket not saved"), (*AddSocketData)->GetBoolField(TEXT("saved")));
	}

	const FString GetInfoAfterAddBody = MakeExecRequestBody(
		TEXT("smoke-staticmesh-info-after-add-001"),
		TEXT("static_mesh_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *StaticMeshAssetPath));
	FHttpSmokeResult GetInfoAfterAddResult;
	TestTrue(TEXT("StaticMesh get_info after add request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoAfterAddBody, GetInfoAfterAddResult));
	TestEqual(TEXT("StaticMesh get_info after add status code"), GetInfoAfterAddResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoAfterAddJson;
	TestTrue(TEXT("StaticMesh get_info after add response parses as JSON"), ParseJson(GetInfoAfterAddResult.Body, GetInfoAfterAddJson));
	TestTrue(TEXT("StaticMesh get_info after add root ok=true"), GetInfoAfterAddJson.IsValid() && GetInfoAfterAddJson->GetBoolField(TEXT("ok")));

	bool bFoundAddedSocket = false;
	const TSharedPtr<FJsonObject>* GetInfoAfterAddData = nullptr;
	TestTrue(TEXT("StaticMesh get_info after add contains data object"), GetInfoAfterAddJson.IsValid() && GetInfoAfterAddJson->TryGetObjectField(TEXT("data"), GetInfoAfterAddData) && GetInfoAfterAddData && GetInfoAfterAddData->IsValid());
	if (GetInfoAfterAddData && GetInfoAfterAddData->IsValid())
	{
		const int32 SocketCountAfterAdd = static_cast<int32>((*GetInfoAfterAddData)->GetIntegerField(TEXT("socket_count")));
		TestTrue(TEXT("StaticMesh socket count increased"), SocketCountAfterAdd >= SocketCountBefore + 1);

		const TArray<TSharedPtr<FJsonValue>>* Sockets = nullptr;
		TestTrue(TEXT("StaticMesh get_info after add contains sockets array"), (*GetInfoAfterAddData)->TryGetArrayField(TEXT("sockets"), Sockets) && Sockets != nullptr);
		if (Sockets)
		{
			for (const TSharedPtr<FJsonValue>& SocketValue : *Sockets)
			{
				const TSharedPtr<FJsonObject>* SocketObject = nullptr;
				if (!SocketValue.IsValid() || !SocketValue->TryGetObject(SocketObject) || !SocketObject || !SocketObject->IsValid())
				{
					continue;
				}

				if ((*SocketObject)->GetStringField(TEXT("socket_name")) == SocketName)
				{
					bFoundAddedSocket = true;
					TestTrue(TEXT("StaticMesh added socket location_x applied"), FMath::IsNearlyEqual((*SocketObject)->GetNumberField(TEXT("location_x")), 10.0));
					break;
				}
			}
		}
	}
	TestTrue(TEXT("StaticMesh get_info after add contains added socket"), bFoundAddedSocket);

	const FString UpdateSocketBody = MakeExecRequestBody(
		TEXT("smoke-staticmesh-update-socket-001"),
		TEXT("static_mesh_update_socket"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"socket_name\":\"%s\",\"new_socket_name\":\"%s\",\"location\":{\"x\":15.0,\"y\":25.0,\"z\":35.0},\"rotation\":{\"pitch\":0.0,\"yaw\":90.0,\"roll\":0.0},\"save_after_set\":false}"),
			*StaticMeshAssetPath,
			*SocketName,
			*RenamedSocketName));
	FHttpSmokeResult UpdateSocketResult;
	TestTrue(TEXT("StaticMesh update_socket request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, UpdateSocketBody, UpdateSocketResult));
	TestEqual(TEXT("StaticMesh update_socket status code"), UpdateSocketResult.StatusCode, 200);

	TSharedPtr<FJsonObject> UpdateSocketJson;
	TestTrue(TEXT("StaticMesh update_socket response parses as JSON"), ParseJson(UpdateSocketResult.Body, UpdateSocketJson));
	TestTrue(TEXT("StaticMesh update_socket root ok=true"), UpdateSocketJson.IsValid() && UpdateSocketJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* UpdateSocketData = nullptr;
	TestTrue(TEXT("StaticMesh update_socket contains data object"), UpdateSocketJson.IsValid() && UpdateSocketJson->TryGetObjectField(TEXT("data"), UpdateSocketData) && UpdateSocketData && UpdateSocketData->IsValid());
	if (UpdateSocketData && UpdateSocketData->IsValid())
	{
		const TSharedPtr<FJsonObject>* UpdatedSocket = nullptr;
		TestTrue(TEXT("StaticMesh update_socket contains socket object"), (*UpdateSocketData)->TryGetObjectField(TEXT("socket"), UpdatedSocket) && UpdatedSocket && UpdatedSocket->IsValid());
		if (UpdatedSocket && UpdatedSocket->IsValid())
		{
			TestEqual(TEXT("StaticMesh update_socket renamed socket"), (*UpdatedSocket)->GetStringField(TEXT("socket_name")), RenamedSocketName);
			TestTrue(TEXT("StaticMesh update_socket location_x applied"), FMath::IsNearlyEqual((*UpdatedSocket)->GetNumberField(TEXT("location_x")), 15.0));
		}
	}

	const FString RemoveSocketBody = MakeExecRequestBody(
		TEXT("smoke-staticmesh-remove-socket-001"),
		TEXT("static_mesh_remove_socket"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"socket_name\":\"%s\",\"save_after_set\":false}"), *StaticMeshAssetPath, *RenamedSocketName));
	FHttpSmokeResult RemoveSocketResult;
	TestTrue(TEXT("StaticMesh remove_socket request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveSocketBody, RemoveSocketResult));
	TestEqual(TEXT("StaticMesh remove_socket status code"), RemoveSocketResult.StatusCode, 200);

	TSharedPtr<FJsonObject> RemoveSocketJson;
	TestTrue(TEXT("StaticMesh remove_socket response parses as JSON"), ParseJson(RemoveSocketResult.Body, RemoveSocketJson));
	TestTrue(TEXT("StaticMesh remove_socket root ok=true"), RemoveSocketJson.IsValid() && RemoveSocketJson->GetBoolField(TEXT("ok")));

	if (!OriginalMaterialPath.IsEmpty())
	{
		const FString RestoreMaterialSlotBody = MakeExecRequestBody(
			TEXT("smoke-staticmesh-restore-material-slot-001"),
			TEXT("static_mesh_set_material_slot"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"slot_index\":0,\"material_path\":\"%s\",\"save_after_set\":false}"), *StaticMeshAssetPath, *OriginalMaterialPath));
		FHttpSmokeResult RestoreMaterialSlotResult;
		TestTrue(TEXT("StaticMesh restore_material_slot request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RestoreMaterialSlotBody, RestoreMaterialSlotResult));
		TestEqual(TEXT("StaticMesh restore_material_slot status code"), RestoreMaterialSlotResult.StatusCode, 200);
	}

	const FString ClearCollisionSpheresBody = MakeExecRequestBody(
		TEXT("smoke-staticmesh-clear-collision-spheres-001"),
		TEXT("static_mesh_set_collision_spheres"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"spheres\":[],\"clear_other_shapes\":false,\"save_after_set\":false}"), *StaticMeshAssetPath));
	FHttpSmokeResult ClearCollisionSpheresResult;
	TestTrue(TEXT("StaticMesh clear_collision_spheres request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ClearCollisionSpheresBody, ClearCollisionSpheresResult));
	TestEqual(TEXT("StaticMesh clear_collision_spheres status code"), ClearCollisionSpheresResult.StatusCode, 200);

	const FString ClearCollisionCapsulesBody = MakeExecRequestBody(
		TEXT("smoke-staticmesh-clear-collision-capsules-001"),
		TEXT("static_mesh_set_collision_capsules"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"capsules\":[],\"clear_other_shapes\":false,\"save_after_set\":false}"), *StaticMeshAssetPath));
	FHttpSmokeResult ClearCollisionCapsulesResult;
	TestTrue(TEXT("StaticMesh clear_collision_capsules request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ClearCollisionCapsulesBody, ClearCollisionCapsulesResult));
	TestEqual(TEXT("StaticMesh clear_collision_capsules status code"), ClearCollisionCapsulesResult.StatusCode, 200);

	const FString GetInfoAfterRemoveBody = MakeExecRequestBody(
		TEXT("smoke-staticmesh-info-after-remove-001"),
		TEXT("static_mesh_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *StaticMeshAssetPath));
	FHttpSmokeResult GetInfoAfterRemoveResult;
	TestTrue(TEXT("StaticMesh get_info after remove request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoAfterRemoveBody, GetInfoAfterRemoveResult));
	TestEqual(TEXT("StaticMesh get_info after remove status code"), GetInfoAfterRemoveResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoAfterRemoveJson;
	TestTrue(TEXT("StaticMesh get_info after remove response parses as JSON"), ParseJson(GetInfoAfterRemoveResult.Body, GetInfoAfterRemoveJson));
	TestTrue(TEXT("StaticMesh get_info after remove root ok=true"), GetInfoAfterRemoveJson.IsValid() && GetInfoAfterRemoveJson->GetBoolField(TEXT("ok")));

	bool bFoundRemovedSocket = false;
	const TSharedPtr<FJsonObject>* GetInfoAfterRemoveData = nullptr;
	TestTrue(TEXT("StaticMesh get_info after remove contains data object"), GetInfoAfterRemoveJson.IsValid() && GetInfoAfterRemoveJson->TryGetObjectField(TEXT("data"), GetInfoAfterRemoveData) && GetInfoAfterRemoveData && GetInfoAfterRemoveData->IsValid());
	if (GetInfoAfterRemoveData && GetInfoAfterRemoveData->IsValid())
	{
		const int32 SocketCountAfterRemove = static_cast<int32>((*GetInfoAfterRemoveData)->GetIntegerField(TEXT("socket_count")));
		TestEqual(TEXT("StaticMesh socket count restored"), SocketCountAfterRemove, SocketCountBefore);
		TestEqual(TEXT("StaticMesh material slot count restored"), static_cast<int32>((*GetInfoAfterRemoveData)->GetIntegerField(TEXT("material_slot_count"))), MaterialSlotCountBefore);

		const TArray<TSharedPtr<FJsonValue>>* Sockets = nullptr;
		if ((*GetInfoAfterRemoveData)->TryGetArrayField(TEXT("sockets"), Sockets) && Sockets)
		{
			for (const TSharedPtr<FJsonValue>& SocketValue : *Sockets)
			{
				const TSharedPtr<FJsonObject>* SocketObject = nullptr;
				if (!SocketValue.IsValid() || !SocketValue->TryGetObject(SocketObject) || !SocketObject || !SocketObject->IsValid())
				{
					continue;
				}

				if ((*SocketObject)->GetStringField(TEXT("socket_name")) == RenamedSocketName)
				{
					bFoundRemovedSocket = true;
					break;
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* MaterialSlots = nullptr;
		if ((*GetInfoAfterRemoveData)->TryGetArrayField(TEXT("material_slots"), MaterialSlots) && MaterialSlots && MaterialSlots->Num() > 0 && !OriginalMaterialPath.IsEmpty())
		{
			const TSharedPtr<FJsonObject>* MaterialSlotObject = nullptr;
			if ((*MaterialSlots)[0].IsValid() && (*MaterialSlots)[0]->TryGetObject(MaterialSlotObject) && MaterialSlotObject && MaterialSlotObject->IsValid())
			{
				TestEqual(TEXT("StaticMesh slot 0 material restored"), (*MaterialSlotObject)->GetStringField(TEXT("material_path")), OriginalMaterialPath);
			}
		}

		const TSharedPtr<FJsonObject>* CollisionObj = nullptr;
		if ((*GetInfoAfterRemoveData)->TryGetObjectField(TEXT("collision"), CollisionObj) && CollisionObj && CollisionObj->IsValid())
		{
			TestEqual(TEXT("StaticMesh sphere count restored"), static_cast<int32>((*CollisionObj)->GetIntegerField(TEXT("sphere_count"))), SphereCountBefore);
			TestEqual(TEXT("StaticMesh capsule count restored"), static_cast<int32>((*CollisionObj)->GetIntegerField(TEXT("capsule_count"))), CapsuleCountBefore);
		}
	}
	TestFalse(TEXT("StaticMesh removed socket no longer present"), bFoundRemovedSocket);

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceMeshDeformerJsonSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.MeshDeformerJsonWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceMeshDeformerJsonSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	auto ExecCommand = [this, &ServerScope](const FString& RequestId, const FString& Command, const FString& ParamsJson, FHttpSmokeResult& OutResult, TSharedPtr<FJsonObject>& OutJson) -> bool
	{
		const FString Body = MakeExecRequestBody(RequestId, Command, ParamsJson);
		const bool bCompleted = ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, Body, OutResult);
		TestTrue(FString::Printf(TEXT("%s request completed"), *RequestId), bCompleted);
		TestTrue(FString::Printf(TEXT("%s response parses"), *RequestId), ParseJson(OutResult.Body, OutJson));
		return bCompleted && OutJson.IsValid();
	};

	const FString StaticMeshAssetPath = TEXT("/Engine/BasicShapes/Cube.Cube");
	const FString StaticFolderPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("StaticMesh"), TEXT("SmokeFolderProfile")));
	IFileManager::Get().DeleteDirectory(*StaticFolderPath, false, true);

	FHttpSmokeResult StaticExportResult;
	TSharedPtr<FJsonObject> StaticExportJson;
	TestTrue(
		TEXT("StaticMesh folder export succeeds"),
		ExecCommand(
			TEXT("smoke-staticmesh-folder-export-001"),
			TEXT("static_mesh_export_folder"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"folder_path\":\"%s\"}"), *StaticMeshAssetPath, *ToSmokeJsonPath(StaticFolderPath)),
			StaticExportResult,
			StaticExportJson));
	TestEqual(TEXT("StaticMesh folder export status code"), StaticExportResult.StatusCode, 200);
	TestTrue(TEXT("StaticMesh folder export ok=true"), StaticExportJson.IsValid() && StaticExportJson->GetBoolField(TEXT("ok")));
	TestTrue(TEXT("StaticMesh mesh.json exists"), FPaths::FileExists(FPaths::Combine(StaticFolderPath, TEXT("mesh.json"))));
	TestTrue(TEXT("StaticMesh import_data.json exists"), FPaths::FileExists(FPaths::Combine(StaticFolderPath, TEXT("import_data.json"))));
	TestTrue(TEXT("StaticMesh raw_mesh_summary.json exists"), FPaths::FileExists(FPaths::Combine(StaticFolderPath, TEXT("raw_mesh_summary.json"))));
	TSharedPtr<FJsonObject> StaticMeshAssetFolderJson;
	TestTrue(TEXT("StaticMesh folder asset details load"), LoadSmokeJsonFile(this, FPaths::Combine(StaticFolderPath, TEXT("asset.json")), StaticMeshAssetFolderJson, TEXT("StaticMesh folder asset.json")));
	TSharedPtr<FJsonObject> StaticMeshDetailsJson;
	TestTrue(TEXT("StaticMesh folder asset details grouped"), SmokeLoadGroupedDetailsForRef(this, FPaths::Combine(StaticFolderPath, TEXT("asset.json")), StaticMeshAssetFolderJson, TEXT("details"), TEXT("static_mesh_asset"), StaticMeshDetailsJson));
	TestFalse(TEXT("StaticMesh folder omits root raw_properties.json"), FPaths::FileExists(FPaths::Combine(StaticFolderPath, TEXT("raw_properties.json"))));
	TestFalse(TEXT("StaticMesh folder omits root readonly_properties.json"), FPaths::FileExists(FPaths::Combine(StaticFolderPath, TEXT("readonly_properties.json"))));

	FHttpSmokeResult StaticValidateResult;
	TSharedPtr<FJsonObject> StaticValidateJson;
	TestTrue(
		TEXT("StaticMesh folder validate succeeds"),
		ExecCommand(
			TEXT("smoke-staticmesh-folder-validate-001"),
			TEXT("static_mesh_validate_folder"),
			FString::Printf(TEXT("{\"folder_path\":\"%s\",\"dry_run\":true}"), *ToSmokeJsonPath(StaticFolderPath)),
			StaticValidateResult,
			StaticValidateJson));
	TestEqual(TEXT("StaticMesh folder validate status code"), StaticValidateResult.StatusCode, 200);
	TestTrue(TEXT("StaticMesh folder validate ok=true"), StaticValidateJson.IsValid() && StaticValidateJson->GetBoolField(TEXT("ok")));

	const FString StaticCollisionJsonPath = FPaths::Combine(StaticFolderPath, TEXT("collision.json"));
	const FString StaticSocketsJsonPath = FPaths::Combine(StaticFolderPath, TEXT("sockets.json"));
	FString OriginalStaticCollisionJson;
	FString OriginalStaticSocketsJson;
	if (!TestTrue(TEXT("StaticMesh original collision.json loads"), FFileHelper::LoadFileToString(OriginalStaticCollisionJson, *StaticCollisionJsonPath)))
	{
		return false;
	}
	if (!TestTrue(TEXT("StaticMesh original sockets.json loads"), FFileHelper::LoadFileToString(OriginalStaticSocketsJson, *StaticSocketsJsonPath)))
	{
		return false;
	}

	const FString FolderSocketName = FString::Printf(TEXT("FolderSmokeSocket_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	TestTrue(
		TEXT("StaticMesh folder replacement collision json saves"),
		FFileHelper::SaveStringToFile(
			TEXT("{\"collision_complexity\":0,\"boxes\":[{\"center\":{\"x\":0,\"y\":0,\"z\":0},\"rotation\":{\"pitch\":0,\"yaw\":0,\"roll\":0},\"x\":32,\"y\":34,\"z\":36}],\"spheres\":[{\"center\":{\"x\":1,\"y\":2,\"z\":3},\"radius\":18}],\"capsules\":[{\"center\":{\"x\":4,\"y\":5,\"z\":6},\"rotation\":{\"pitch\":0,\"yaw\":90,\"roll\":0},\"radius\":9,\"length\":28}],\"convex_hulls\":[],\"edit_policy\":\"simple_collision_safe\"}\n"),
			*StaticCollisionJsonPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	TestTrue(
		TEXT("StaticMesh folder replacement sockets json saves"),
		FFileHelper::SaveStringToFile(
			FString::Printf(
				TEXT("{\"sockets\":[{\"socket_name\":\"%s\",\"relative_location\":{\"x\":7,\"y\":8,\"z\":9},\"relative_rotation\":{\"pitch\":0,\"yaw\":25,\"roll\":0},\"relative_scale\":{\"x\":1,\"y\":1,\"z\":1},\"tag\":\"folder_smoke\"}]}\n"),
				*FolderSocketName),
			*StaticSocketsJsonPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));

	FHttpSmokeResult StaticApplyResult;
	TSharedPtr<FJsonObject> StaticApplyJson;
	TestTrue(
		TEXT("StaticMesh folder apply succeeds"),
		ExecCommand(
			TEXT("smoke-staticmesh-folder-apply-001"),
			TEXT("static_mesh_apply_folder"),
			FString::Printf(TEXT("{\"folder_path\":\"%s\",\"dry_run\":false,\"save_after_apply\":false,\"build_after_apply\":false}"), *ToSmokeJsonPath(StaticFolderPath)),
			StaticApplyResult,
			StaticApplyJson));
	TestEqual(TEXT("StaticMesh folder apply status code"), StaticApplyResult.StatusCode, 200);
	TestTrue(TEXT("StaticMesh folder apply ok=true"), StaticApplyJson.IsValid() && StaticApplyJson->GetBoolField(TEXT("ok")));
	TestTrue(TEXT("StaticMesh folder apply reports BodySetup Modify"), StaticApplyResult.Body.Contains(TEXT("collision.json.body_setup_modify")));

	const TSharedPtr<FJsonObject>* StaticApplyData = nullptr;
	const TSharedPtr<FJsonObject>* StaticApplyReadback = nullptr;
	TestTrue(
		TEXT("StaticMesh folder apply contains readback"),
		StaticApplyJson.IsValid()
		&& StaticApplyJson->TryGetObjectField(TEXT("data"), StaticApplyData)
		&& StaticApplyData
		&& StaticApplyData->IsValid()
		&& (*StaticApplyData)->TryGetObjectField(TEXT("readback"), StaticApplyReadback)
		&& StaticApplyReadback
		&& StaticApplyReadback->IsValid());
	if (StaticApplyReadback && StaticApplyReadback->IsValid())
	{
		const TSharedPtr<FJsonObject>* CollisionObj = nullptr;
		TestTrue(TEXT("StaticMesh folder apply readback has collision"), (*StaticApplyReadback)->TryGetObjectField(TEXT("collision"), CollisionObj) && CollisionObj && CollisionObj->IsValid());
		if (CollisionObj && CollisionObj->IsValid())
		{
			TestEqual(TEXT("StaticMesh folder apply replaces box count"), static_cast<int32>((*CollisionObj)->GetIntegerField(TEXT("box_count"))), 1);
			TestEqual(TEXT("StaticMesh folder apply replaces sphere count"), static_cast<int32>((*CollisionObj)->GetIntegerField(TEXT("sphere_count"))), 1);
			TestEqual(TEXT("StaticMesh folder apply replaces capsule count"), static_cast<int32>((*CollisionObj)->GetIntegerField(TEXT("capsule_count"))), 1);
			TestEqual(TEXT("StaticMesh folder apply clears convex hulls"), static_cast<int32>((*CollisionObj)->GetIntegerField(TEXT("convex_count"))), 0);
		}

		bool bFoundFolderSocket = false;
		const TArray<TSharedPtr<FJsonValue>>* Sockets = nullptr;
		TestTrue(TEXT("StaticMesh folder apply readback contains sockets"), (*StaticApplyReadback)->TryGetArrayField(TEXT("sockets"), Sockets) && Sockets);
		if (Sockets)
		{
			for (const TSharedPtr<FJsonValue>& SocketValue : *Sockets)
			{
				const TSharedPtr<FJsonObject>* SocketObject = nullptr;
				if (SocketValue.IsValid() && SocketValue->TryGetObject(SocketObject) && SocketObject && SocketObject->IsValid())
				{
					bFoundFolderSocket |= ((*SocketObject)->GetStringField(TEXT("socket_name")) == FolderSocketName);
				}
			}
		}
		TestTrue(TEXT("StaticMesh folder apply creates replacement socket"), bFoundFolderSocket);
	}

	TestTrue(
		TEXT("StaticMesh folder unsupported convex hull json saves"),
		FFileHelper::SaveStringToFile(
			TEXT("{\"convex_hulls\":[{\"vertices\":[{\"x\":0,\"y\":0,\"z\":0}]}]}\n"),
			*StaticCollisionJsonPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	FHttpSmokeResult StaticUnsupportedCollisionValidateResult;
	TSharedPtr<FJsonObject> StaticUnsupportedCollisionValidateJson;
	TestTrue(
		TEXT("StaticMesh unsupported convex hull validate completes"),
		ExecCommand(
			TEXT("smoke-staticmesh-unsupported-convex-validate-001"),
			TEXT("static_mesh_validate_folder"),
			FString::Printf(TEXT("{\"folder_path\":\"%s\",\"dry_run\":true}"), *ToSmokeJsonPath(StaticFolderPath)),
			StaticUnsupportedCollisionValidateResult,
			StaticUnsupportedCollisionValidateJson));
	TestEqual(TEXT("StaticMesh unsupported convex validate status code"), StaticUnsupportedCollisionValidateResult.StatusCode, 400);
	TestTrue(TEXT("StaticMesh unsupported convex validate ok=false"), StaticUnsupportedCollisionValidateJson.IsValid() && !StaticUnsupportedCollisionValidateJson->GetBoolField(TEXT("ok")));
	TestTrue(TEXT("StaticMesh unsupported convex validate reports unsupported_apply_profile"), StaticUnsupportedCollisionValidateResult.Body.Contains(TEXT("unsupported_apply_profile")));

	TestTrue(
		TEXT("StaticMesh folder collision json restores"),
		FFileHelper::SaveStringToFile(OriginalStaticCollisionJson, *StaticCollisionJsonPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	TestTrue(
		TEXT("StaticMesh folder sockets json restores"),
		FFileHelper::SaveStringToFile(OriginalStaticSocketsJson, *StaticSocketsJsonPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	FHttpSmokeResult StaticRestoreResult;
	TSharedPtr<FJsonObject> StaticRestoreJson;
	TestTrue(
		TEXT("StaticMesh folder restore apply succeeds"),
		ExecCommand(
			TEXT("smoke-staticmesh-folder-restore-001"),
			TEXT("static_mesh_apply_folder"),
			FString::Printf(TEXT("{\"folder_path\":\"%s\",\"dry_run\":false,\"save_after_apply\":false,\"build_after_apply\":false}"), *ToSmokeJsonPath(StaticFolderPath)),
			StaticRestoreResult,
			StaticRestoreJson));
	TestEqual(TEXT("StaticMesh folder restore status code"), StaticRestoreResult.StatusCode, 200);
	TestTrue(TEXT("StaticMesh folder restore ok=true"), StaticRestoreJson.IsValid() && StaticRestoreJson->GetBoolField(TEXT("ok")));
	TestFalse(TEXT("StaticMesh folder restore removes smoke socket"), StaticRestoreResult.Body.Contains(*FolderSocketName));

	const FString StaticUnsupportedJsonPath = FPaths::Combine(StaticFolderPath, TEXT("raw_mesh_summary.json"));
	TestTrue(
		TEXT("StaticMesh unsupported write-intent json saves"),
		FFileHelper::SaveStringToFile(
			TEXT("{\"schema\":\"ue_agent_interface.static_mesh.raw_mesh_summary.v1\",\"operations\":[{\"apply\":true}]}\n"),
			*StaticUnsupportedJsonPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	FHttpSmokeResult StaticUnsupportedValidateResult;
	TSharedPtr<FJsonObject> StaticUnsupportedValidateJson;
	TestTrue(
		TEXT("StaticMesh unsupported summary write-intent validate completes"),
		ExecCommand(
			TEXT("smoke-staticmesh-unsupported-summary-validate-001"),
			TEXT("static_mesh_validate_folder"),
			FString::Printf(TEXT("{\"folder_path\":\"%s\",\"dry_run\":true}"), *ToSmokeJsonPath(StaticFolderPath)),
			StaticUnsupportedValidateResult,
			StaticUnsupportedValidateJson));
	TestEqual(TEXT("StaticMesh unsupported summary validate status code"), StaticUnsupportedValidateResult.StatusCode, 400);
	TestTrue(TEXT("StaticMesh unsupported summary validate ok=false"), StaticUnsupportedValidateJson.IsValid() && !StaticUnsupportedValidateJson->GetBoolField(TEXT("ok")));
	TestTrue(TEXT("StaticMesh unsupported summary validate reports unsupported_apply_profile"), StaticUnsupportedValidateResult.Body.Contains(TEXT("unsupported_apply_profile")));

	FHttpSmokeResult StaticCollisionPreviewResult;
	TSharedPtr<FJsonObject> StaticCollisionPreviewJson;
	TestTrue(
		TEXT("StaticMesh collision preview succeeds"),
		ExecCommand(
			TEXT("smoke-staticmesh-collision-preview-001"),
			TEXT("static_mesh_preview_collision"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *StaticMeshAssetPath),
			StaticCollisionPreviewResult,
			StaticCollisionPreviewJson));
	TestEqual(TEXT("StaticMesh collision preview status code"), StaticCollisionPreviewResult.StatusCode, 200);
	TestTrue(TEXT("StaticMesh collision preview ok=true"), StaticCollisionPreviewJson.IsValid() && StaticCollisionPreviewJson->GetBoolField(TEXT("ok")));
	TestTrue(TEXT("StaticMesh collision preview includes collision"), StaticCollisionPreviewResult.Body.Contains(TEXT("collision")));

	const FString SkeletalMeshAssetPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP");
	const FString SkeletalFolderPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("SkeletalMesh"), TEXT("SmokeFolderProfile")));
	IFileManager::Get().DeleteDirectory(*SkeletalFolderPath, false, true);

	FHttpSmokeResult SkeletalExportResult;
	TSharedPtr<FJsonObject> SkeletalExportJson;
	TestTrue(
		TEXT("SkeletalMesh folder export succeeds"),
		ExecCommand(
			TEXT("smoke-skeletalmesh-folder-export-001"),
			TEXT("skeletal_mesh_export_folder"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"folder_path\":\"%s\"}"), *SkeletalMeshAssetPath, *ToSmokeJsonPath(SkeletalFolderPath)),
			SkeletalExportResult,
			SkeletalExportJson));
	TestEqual(TEXT("SkeletalMesh folder export status code"), SkeletalExportResult.StatusCode, 200);
	TestTrue(TEXT("SkeletalMesh folder export ok=true"), SkeletalExportJson.IsValid() && SkeletalExportJson->GetBoolField(TEXT("ok")));
	TestTrue(TEXT("SkeletalMesh morph_targets.json exists"), FPaths::FileExists(FPaths::Combine(SkeletalFolderPath, TEXT("morph_targets.json"))));
	TestTrue(TEXT("SkeletalMesh skin_weights.json exists"), FPaths::FileExists(FPaths::Combine(SkeletalFolderPath, TEXT("skin_weights.json"))));
	TestTrue(TEXT("SkeletalMesh cloth.json exists"), FPaths::FileExists(FPaths::Combine(SkeletalFolderPath, TEXT("cloth.json"))));
	TestTrue(TEXT("SkeletalMesh clothing.json compatibility alias exists"), FPaths::FileExists(FPaths::Combine(SkeletalFolderPath, TEXT("clothing.json"))));
	TestTrue(TEXT("SkeletalMesh import_data.json exists"), FPaths::FileExists(FPaths::Combine(SkeletalFolderPath, TEXT("import_data.json"))));
	TSharedPtr<FJsonObject> SkeletalMeshAssetFolderJson;
	TestTrue(TEXT("SkeletalMesh folder asset details load"), LoadSmokeJsonFile(this, FPaths::Combine(SkeletalFolderPath, TEXT("asset.json")), SkeletalMeshAssetFolderJson, TEXT("SkeletalMesh folder asset.json")));
	TSharedPtr<FJsonObject> SkeletalMeshDetailsJson;
	TestTrue(TEXT("SkeletalMesh folder asset details grouped"), SmokeLoadGroupedDetailsForRef(this, FPaths::Combine(SkeletalFolderPath, TEXT("asset.json")), SkeletalMeshAssetFolderJson, TEXT("details"), TEXT("skeletal_mesh_asset"), SkeletalMeshDetailsJson));
	TestFalse(TEXT("SkeletalMesh folder omits root raw_properties.json"), FPaths::FileExists(FPaths::Combine(SkeletalFolderPath, TEXT("raw_properties.json"))));
	TestFalse(TEXT("SkeletalMesh folder omits root readonly_properties.json"), FPaths::FileExists(FPaths::Combine(SkeletalFolderPath, TEXT("readonly_properties.json"))));

	FHttpSmokeResult SkeletalValidateResult;
	TSharedPtr<FJsonObject> SkeletalValidateJson;
	TestTrue(
		TEXT("SkeletalMesh folder validate succeeds"),
		ExecCommand(
			TEXT("smoke-skeletalmesh-folder-validate-001"),
			TEXT("skeletal_mesh_validate_folder"),
			FString::Printf(TEXT("{\"folder_path\":\"%s\",\"dry_run\":true}"), *ToSmokeJsonPath(SkeletalFolderPath)),
			SkeletalValidateResult,
			SkeletalValidateJson));
	TestEqual(TEXT("SkeletalMesh folder validate status code"), SkeletalValidateResult.StatusCode, 200);
	TestTrue(TEXT("SkeletalMesh folder validate ok=true"), SkeletalValidateJson.IsValid() && SkeletalValidateJson->GetBoolField(TEXT("ok")));

	const FString SkeletalUnsupportedJsonPath = FPaths::Combine(SkeletalFolderPath, TEXT("skin_weights.json"));
	TestTrue(
		TEXT("SkeletalMesh unsupported write-intent json saves"),
		FFileHelper::SaveStringToFile(
			TEXT("{\"schema\":\"ue_agent_interface.skeletal_mesh.skin_weights.v1\",\"operations\":[{\"apply\":true}]}\n"),
			*SkeletalUnsupportedJsonPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	FHttpSmokeResult SkeletalUnsupportedValidateResult;
	TSharedPtr<FJsonObject> SkeletalUnsupportedValidateJson;
	TestTrue(
		TEXT("SkeletalMesh unsupported summary write-intent validate completes"),
		ExecCommand(
			TEXT("smoke-skeletalmesh-unsupported-summary-validate-001"),
			TEXT("skeletal_mesh_validate_folder"),
			FString::Printf(TEXT("{\"folder_path\":\"%s\",\"dry_run\":true}"), *ToSmokeJsonPath(SkeletalFolderPath)),
			SkeletalUnsupportedValidateResult,
			SkeletalUnsupportedValidateJson));
	TestEqual(TEXT("SkeletalMesh unsupported summary validate status code"), SkeletalUnsupportedValidateResult.StatusCode, 400);
	TestTrue(TEXT("SkeletalMesh unsupported summary validate ok=false"), SkeletalUnsupportedValidateJson.IsValid() && !SkeletalUnsupportedValidateJson->GetBoolField(TEXT("ok")));
	TestTrue(TEXT("SkeletalMesh unsupported summary validate reports unsupported_apply_profile"), SkeletalUnsupportedValidateResult.Body.Contains(TEXT("unsupported_apply_profile")));

	FHttpSmokeResult MorphValidateResult;
	TSharedPtr<FJsonObject> MorphValidateJson;
	TestTrue(
		TEXT("SkeletalMesh morph validate succeeds"),
		ExecCommand(
			TEXT("smoke-skeletalmesh-morph-validate-001"),
			TEXT("skeletal_mesh_validate_morph_targets"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SkeletalMeshAssetPath),
			MorphValidateResult,
			MorphValidateJson));
	TestEqual(TEXT("SkeletalMesh morph validate status code"), MorphValidateResult.StatusCode, 200);
	TestTrue(TEXT("SkeletalMesh morph validate ok=true"), MorphValidateJson.IsValid() && MorphValidateJson->GetBoolField(TEXT("ok")));

	FHttpSmokeResult MorphPreviewMissingResult;
	TSharedPtr<FJsonObject> MorphPreviewMissingJson;
	TestTrue(
		TEXT("SkeletalMesh missing morph preview reports structured failure"),
		ExecCommand(
			TEXT("smoke-skeletalmesh-morph-preview-missing-001"),
			TEXT("skeletal_mesh_preview_morph_target"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"morph_target\":\"__MissingMorphForSmoke__\",\"weight\":1.0}"), *SkeletalMeshAssetPath),
			MorphPreviewMissingResult,
			MorphPreviewMissingJson));
	TestEqual(TEXT("SkeletalMesh missing morph preview status code"), MorphPreviewMissingResult.StatusCode, 400);
	TestTrue(TEXT("SkeletalMesh missing morph preview ok=false"), MorphPreviewMissingJson.IsValid() && !MorphPreviewMissingJson->GetBoolField(TEXT("ok")));
	TestTrue(TEXT("SkeletalMesh missing morph preview error"), MorphPreviewMissingResult.Body.Contains(TEXT("morph_target_not_found")));

	FHttpSmokeResult SkinProfilePreviewMissingResult;
	TSharedPtr<FJsonObject> SkinProfilePreviewMissingJson;
	TestTrue(
		TEXT("SkeletalMesh missing skin profile preview reports structured failure"),
		ExecCommand(
			TEXT("smoke-skeletalmesh-skin-profile-preview-missing-001"),
			TEXT("skeletal_mesh_preview_skin_weight_profile"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"profile_name\":\"__MissingProfileForSmoke__\"}"), *SkeletalMeshAssetPath),
			SkinProfilePreviewMissingResult,
			SkinProfilePreviewMissingJson));
	TestEqual(TEXT("SkeletalMesh missing skin profile preview status code"), SkinProfilePreviewMissingResult.StatusCode, 400);
	TestTrue(TEXT("SkeletalMesh missing skin profile preview ok=false"), SkinProfilePreviewMissingJson.IsValid() && !SkinProfilePreviewMissingJson->GetBoolField(TEXT("ok")));
	TestTrue(TEXT("SkeletalMesh missing skin profile preview error"), SkinProfilePreviewMissingResult.Body.Contains(TEXT("skin_weight_profile_not_found")));

	FHttpSmokeResult SkinProfileImportMissingResult;
	TSharedPtr<FJsonObject> SkinProfileImportMissingJson;
	TestTrue(
		TEXT("SkeletalMesh missing skin profile import source reports structured failure"),
		ExecCommand(
			TEXT("smoke-skeletalmesh-skin-profile-import-missing-source-001"),
			TEXT("skeletal_mesh_import_skin_weight_profile"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"source_filename\":\"D:/__MissingSkinWeightProfileSmoke__.fbx\",\"profile_name\":\"SmokeProfile\",\"lod_index\":0}"), *SkeletalMeshAssetPath),
			SkinProfileImportMissingResult,
			SkinProfileImportMissingJson));
	TestEqual(TEXT("SkeletalMesh missing skin profile import source status code"), SkinProfileImportMissingResult.StatusCode, 400);
	TestTrue(TEXT("SkeletalMesh missing skin profile import source ok=false"), SkinProfileImportMissingJson.IsValid() && !SkinProfileImportMissingJson->GetBoolField(TEXT("ok")));
	TestTrue(TEXT("SkeletalMesh missing skin profile import source error"), SkinProfileImportMissingResult.Body.Contains(TEXT("source_file_not_found")));

	const FString SkeletonObjectPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton");
	const FString BlendSpaceAssetPath = MakeAutomationAssetPath(TEXT("BSJson"));
	const FString BlendSpaceJsonPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("BlendSpace"), TEXT("SmokeBlendSpace.json")));

	FHttpSmokeResult BlendSpaceCreateResult;
	TSharedPtr<FJsonObject> BlendSpaceCreateJson;
	TestTrue(
		TEXT("BlendSpace create succeeds"),
		ExecCommand(
			TEXT("smoke-blendspace-create-001"),
			TEXT("blendspace_create"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"skeleton\":\"%s\",\"blendspace_kind\":\"blendspace_1d\",\"axes\":[{\"axis_index\":0,\"name\":\"Speed\",\"min\":0.0,\"max\":600.0,\"grid_num\":4}],\"save_after_create\":false}"), *BlendSpaceAssetPath, *SkeletonObjectPath),
			BlendSpaceCreateResult,
			BlendSpaceCreateJson));
	TestEqual(TEXT("BlendSpace create status code"), BlendSpaceCreateResult.StatusCode, 200);
	TestTrue(TEXT("BlendSpace create ok=true"), BlendSpaceCreateJson.IsValid() && BlendSpaceCreateJson->GetBoolField(TEXT("ok")));

	FHttpSmokeResult BlendSpaceExportResult;
	TSharedPtr<FJsonObject> BlendSpaceExportJson;
	TestTrue(
		TEXT("BlendSpace export succeeds"),
		ExecCommand(
			TEXT("smoke-blendspace-export-001"),
			TEXT("blendspace_export_json"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"output_file\":\"%s\"}"), *BlendSpaceAssetPath, *ToSmokeJsonPath(BlendSpaceJsonPath)),
			BlendSpaceExportResult,
			BlendSpaceExportJson));
	TestEqual(TEXT("BlendSpace export status code"), BlendSpaceExportResult.StatusCode, 200);
	TestTrue(TEXT("BlendSpace export file exists"), FPaths::FileExists(BlendSpaceJsonPath));

	FHttpSmokeResult BlendSpaceValidateResult;
	TSharedPtr<FJsonObject> BlendSpaceValidateJson;
	TestTrue(
		TEXT("BlendSpace validate succeeds"),
		ExecCommand(
			TEXT("smoke-blendspace-validate-001"),
			TEXT("blendspace_validate_json"),
			FString::Printf(TEXT("{\"json_file\":\"%s\"}"), *ToSmokeJsonPath(BlendSpaceJsonPath)),
			BlendSpaceValidateResult,
			BlendSpaceValidateJson));
	TestEqual(TEXT("BlendSpace validate status code"), BlendSpaceValidateResult.StatusCode, 200);
	TestTrue(TEXT("BlendSpace validate ok=true"), BlendSpaceValidateJson.IsValid() && BlendSpaceValidateJson->GetBoolField(TEXT("ok")));

	const FString DeformerGraphAssetPath = MakeAutomationAssetPath(TEXT("DGJson"));
	const FString DeformerGraphFolderPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("DeformerGraph"), TEXT("SmokeFolderProfile")));
	IFileManager::Get().DeleteDirectory(*DeformerGraphFolderPath, false, true);

	FHttpSmokeResult DeformerGraphCreateResult;
	TSharedPtr<FJsonObject> DeformerGraphCreateJson;
	TestTrue(
		TEXT("DeformerGraph create succeeds"),
		ExecCommand(
			TEXT("smoke-deformergraph-create-001"),
			TEXT("deformer_graph_create"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"save_after_create\":false}"), *DeformerGraphAssetPath),
			DeformerGraphCreateResult,
			DeformerGraphCreateJson));
	TestEqual(TEXT("DeformerGraph create status code"), DeformerGraphCreateResult.StatusCode, 200);
	TestTrue(TEXT("DeformerGraph create ok=true"), DeformerGraphCreateJson.IsValid() && DeformerGraphCreateJson->GetBoolField(TEXT("ok")));

	FHttpSmokeResult DeformerGraphExportResult;
	TSharedPtr<FJsonObject> DeformerGraphExportJson;
	TestTrue(
		TEXT("DeformerGraph folder export succeeds"),
		ExecCommand(
			TEXT("smoke-deformergraph-folder-export-001"),
			TEXT("deformer_graph_export_folder"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"folder_path\":\"%s\"}"), *DeformerGraphAssetPath, *ToSmokeJsonPath(DeformerGraphFolderPath)),
			DeformerGraphExportResult,
			DeformerGraphExportJson));
	TestEqual(TEXT("DeformerGraph folder export status code"), DeformerGraphExportResult.StatusCode, 200);
	TestTrue(TEXT("DeformerGraph folder export ok=true"), DeformerGraphExportJson.IsValid() && DeformerGraphExportJson->GetBoolField(TEXT("ok")));
	TestTrue(TEXT("DeformerGraph settings.json exists"), FPaths::FileExists(FPaths::Combine(DeformerGraphFolderPath, TEXT("settings.json"))));
	TestTrue(TEXT("DeformerGraph graphs.json exists"), FPaths::FileExists(FPaths::Combine(DeformerGraphFolderPath, TEXT("graphs.json"))));
	TestTrue(TEXT("DeformerGraph kernels.json exists"), FPaths::FileExists(FPaths::Combine(DeformerGraphFolderPath, TEXT("kernels.json"))));
	TSharedPtr<FJsonObject> DeformerGraphAssetJson;
	TSharedPtr<FJsonObject> DeformerGraphAssetDetailsJson;
	TestTrue(TEXT("DeformerGraph asset parses"), LoadSmokeJsonFile(this, FPaths::Combine(DeformerGraphFolderPath, TEXT("asset.json")), DeformerGraphAssetJson, TEXT("DeformerGraph asset.json")));
	TestTrue(TEXT("DeformerGraph asset details grouped"), SmokeLoadGroupedDetailsForRef(this, FPaths::Combine(DeformerGraphFolderPath, TEXT("asset.json")), DeformerGraphAssetJson, TEXT("details"), TEXT("deformer_graph_asset"), DeformerGraphAssetDetailsJson));
	TestFalse(TEXT("DeformerGraph folder omits root raw_properties.json"), FPaths::FileExists(FPaths::Combine(DeformerGraphFolderPath, TEXT("raw_properties.json"))));
	TestFalse(TEXT("DeformerGraph folder omits root readonly_properties.json"), FPaths::FileExists(FPaths::Combine(DeformerGraphFolderPath, TEXT("readonly_properties.json"))));

	TSharedPtr<FJsonObject> DeformerGraphSettingsJson;
	TestTrue(TEXT("DeformerGraph settings parses"), LoadSmokeJsonFile(this, FPaths::Combine(DeformerGraphFolderPath, TEXT("settings.json")), DeformerGraphSettingsJson, TEXT("DeformerGraph settings.json")));
	if (DeformerGraphSettingsJson.IsValid())
	{
		TestEqual(TEXT("DeformerGraph settings schema"), DeformerGraphSettingsJson->GetStringField(TEXT("schema")), FString(TEXT("ue_agent_interface.deformer_settings.v1")));
		TestTrue(TEXT("DeformerGraph settings identifies Optimus"), DeformerGraphSettingsJson->GetBoolField(TEXT("is_optimus_deformer")));
	}

	TSharedPtr<FJsonObject> DeformerGraphGraphsJson;
	TestTrue(TEXT("DeformerGraph graphs parses"), LoadSmokeJsonFile(this, FPaths::Combine(DeformerGraphFolderPath, TEXT("graphs.json")), DeformerGraphGraphsJson, TEXT("DeformerGraph graphs.json")));
	if (DeformerGraphGraphsJson.IsValid())
	{
		TestEqual(TEXT("DeformerGraph graphs schema"), DeformerGraphGraphsJson->GetStringField(TEXT("schema")), FString(TEXT("ue_agent_interface.deformer_graphs.v1")));
		TestTrue(TEXT("DeformerGraph graphs has items field"), DeformerGraphGraphsJson->HasTypedField<EJson::Array>(TEXT("items")));
		const TArray<TSharedPtr<FJsonValue>>* GraphItems = nullptr;
		if (DeformerGraphGraphsJson->TryGetArrayField(TEXT("items"), GraphItems) && GraphItems)
		{
			for (const TSharedPtr<FJsonValue>& GraphItemValue : *GraphItems)
			{
				const TSharedPtr<FJsonObject>* GraphItemObj = nullptr;
				if (!GraphItemValue.IsValid() || !GraphItemValue->TryGetObject(GraphItemObj) || !GraphItemObj || !GraphItemObj->IsValid())
				{
					continue;
				}
				const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
				if ((*GraphItemObj)->TryGetArrayField(TEXT("nodes"), Nodes) && Nodes && Nodes->Num() > 0)
				{
					const TSharedPtr<FJsonObject>* NodeObj = nullptr;
					TestTrue(TEXT("DeformerGraph node parses"), (*Nodes)[0].IsValid() && (*Nodes)[0]->TryGetObject(NodeObj) && NodeObj && NodeObj->IsValid());
					if (NodeObj && NodeObj->IsValid())
					{
						TestTrue(TEXT("DeformerGraph node exposes authoring metadata"), (*NodeObj)->HasTypedField<EJson::Object>(TEXT("selector")) && (*NodeObj)->HasTypedField<EJson::Array>(TEXT("operations")));
					}
					break;
				}
			}
		}
	}

	TSharedPtr<FJsonObject> DeformerGraphCoverageJson;
	TestTrue(TEXT("DeformerGraph coverage parses"), LoadSmokeJsonFile(this, FPaths::Combine(DeformerGraphFolderPath, TEXT("validation"), TEXT("coverage_report.json")), DeformerGraphCoverageJson, TEXT("DeformerGraph coverage_report.json")));
	if (DeformerGraphCoverageJson.IsValid())
	{
		TestTrue(TEXT("DeformerGraph coverage marked complete schema"), DeformerGraphCoverageJson->GetBoolField(TEXT("is_complete_target_schema")));
		TestTrue(TEXT("DeformerGraph coverage exposes adapter boundaries"), DeformerGraphCoverageJson->HasTypedField<EJson::Array>(TEXT("adapter_boundaries")));
		TestTrue(TEXT("DeformerGraph coverage has empty blocking gaps"), DeformerGraphCoverageJson->HasTypedField<EJson::Array>(TEXT("blocking_gaps")));
	}

	FHttpSmokeResult DeformerGraphValidateResult;
	TSharedPtr<FJsonObject> DeformerGraphValidateJson;
	TestTrue(
		TEXT("DeformerGraph folder validate succeeds"),
		ExecCommand(
			TEXT("smoke-deformergraph-folder-validate-001"),
			TEXT("deformer_graph_validate_folder"),
			FString::Printf(TEXT("{\"folder_path\":\"%s\"}"), *ToSmokeJsonPath(DeformerGraphFolderPath)),
			DeformerGraphValidateResult,
			DeformerGraphValidateJson));
	TestEqual(TEXT("DeformerGraph folder validate status code"), DeformerGraphValidateResult.StatusCode, 200);
	TestTrue(TEXT("DeformerGraph folder validate ok=true"), DeformerGraphValidateJson.IsValid() && DeformerGraphValidateJson->GetBoolField(TEXT("ok")));

	const FString DeformerGraphUnsupportedJsonPath = FPaths::Combine(DeformerGraphFolderPath, TEXT("graphs.json"));
	TestTrue(
		TEXT("DeformerGraph unsupported write-intent json saves"),
		FFileHelper::SaveStringToFile(
			TEXT("{\"schema\":\"ue_agent_interface.deformer_graphs.v1\",\"operations\":[{\"apply\":true}]}\n"),
			*DeformerGraphUnsupportedJsonPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	FHttpSmokeResult DeformerGraphUnsupportedValidateResult;
	TSharedPtr<FJsonObject> DeformerGraphUnsupportedValidateJson;
	TestTrue(
		TEXT("DeformerGraph unsupported summary write-intent validate completes"),
		ExecCommand(
			TEXT("smoke-deformergraph-unsupported-summary-validate-001"),
			TEXT("deformer_graph_validate_folder"),
			FString::Printf(TEXT("{\"folder_path\":\"%s\"}"), *ToSmokeJsonPath(DeformerGraphFolderPath)),
			DeformerGraphUnsupportedValidateResult,
			DeformerGraphUnsupportedValidateJson));
	TestEqual(TEXT("DeformerGraph unsupported summary validate status code"), DeformerGraphUnsupportedValidateResult.StatusCode, 400);
	TestTrue(TEXT("DeformerGraph unsupported summary validate ok=false"), DeformerGraphUnsupportedValidateJson.IsValid() && !DeformerGraphUnsupportedValidateJson->GetBoolField(TEXT("ok")));
	TestTrue(TEXT("DeformerGraph unsupported summary validate reports unsupported_apply_profile"), DeformerGraphUnsupportedValidateResult.Body.Contains(TEXT("unsupported_apply_profile")));

	FHttpSmokeResult DeformerBadResult;
	TSharedPtr<FJsonObject> DeformerBadJson;
	TestTrue(
		TEXT("Deformer bad property request completes"),
		ExecCommand(
			TEXT("smoke-deformer-property-bad-001"),
			TEXT("deformer_source_library_apply_json"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"properties\":[{\"value_text\":\"False\",\"apply\":true}],\"dry_run\":true,\"save_after_apply\":false}"), *StaticMeshAssetPath),
			DeformerBadResult,
			DeformerBadJson));
	TestEqual(TEXT("Deformer bad property status code"), DeformerBadResult.StatusCode, 400);
	TestTrue(TEXT("Deformer bad property ok=false"), DeformerBadJson.IsValid() && !DeformerBadJson->GetBoolField(TEXT("ok")));
	TestTrue(TEXT("Deformer bad property reports required profile"), DeformerBadResult.Body.Contains(TEXT("json_missing_required_field")) && DeformerBadResult.Body.Contains(TEXT("profile")));

	FHttpSmokeResult MeshCollectionBadProfileResult;
	TSharedPtr<FJsonObject> MeshCollectionBadProfileJson;
	TestTrue(
		TEXT("MeshDeformerCollection wrong profile validation completes"),
		ExecCommand(
			TEXT("smoke-meshdeformercollection-wrong-profile-001"),
			TEXT("mesh_deformer_collection_validate_json"),
			FString::Printf(TEXT("{\"profile\":\"deformer_source_library\",\"asset_class\":\"/Script/Engine.StaticMesh\",\"asset_path\":\"%s\",\"properties\":[]}"), *StaticMeshAssetPath),
			MeshCollectionBadProfileResult,
			MeshCollectionBadProfileJson));
	TestEqual(TEXT("MeshDeformerCollection wrong profile status code"), MeshCollectionBadProfileResult.StatusCode, 400);
	TestTrue(TEXT("MeshDeformerCollection wrong profile ok=false"), MeshCollectionBadProfileJson.IsValid() && !MeshCollectionBadProfileJson->GetBoolField(TEXT("ok")));
	TestTrue(TEXT("MeshDeformerCollection wrong profile reports mismatch"), MeshCollectionBadProfileResult.Body.Contains(TEXT("json_profile_mismatch")));

	FHttpSmokeResult GeometryCacheWrongTypeResult;
	TSharedPtr<FJsonObject> GeometryCacheWrongTypeJson;
	TestTrue(
		TEXT("GeometryCache get_info rejects wrong asset type"),
		ExecCommand(
			TEXT("smoke-geometrycache-wrong-type-001"),
			TEXT("geometry_cache_get_info"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *StaticMeshAssetPath),
			GeometryCacheWrongTypeResult,
			GeometryCacheWrongTypeJson));
	TestEqual(TEXT("GeometryCache wrong type status code"), GeometryCacheWrongTypeResult.StatusCode, 400);
	TestTrue(TEXT("GeometryCache wrong type ok=false"), GeometryCacheWrongTypeJson.IsValid() && !GeometryCacheWrongTypeJson->GetBoolField(TEXT("ok")));
	TestTrue(TEXT("GeometryCache wrong type error"), GeometryCacheWrongTypeResult.Body.Contains(TEXT("asset_is_not_geometry_cache")));

	FHttpSmokeResult GeometryCacheFrameWrongTypeResult;
	TSharedPtr<FJsonObject> GeometryCacheFrameWrongTypeJson;
	TestTrue(
		TEXT("GeometryCache frame sample rejects wrong asset type"),
		ExecCommand(
			TEXT("smoke-geometrycache-frame-wrong-type-001"),
			TEXT("geometry_cache_screenshot_frame"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"frame_index\":0}"), *StaticMeshAssetPath),
			GeometryCacheFrameWrongTypeResult,
			GeometryCacheFrameWrongTypeJson));
	TestEqual(TEXT("GeometryCache frame wrong type status code"), GeometryCacheFrameWrongTypeResult.StatusCode, 400);
	TestTrue(TEXT("GeometryCache frame wrong type ok=false"), GeometryCacheFrameWrongTypeJson.IsValid() && !GeometryCacheFrameWrongTypeJson->GetBoolField(TEXT("ok")));
	TestTrue(TEXT("GeometryCache frame wrong type error"), GeometryCacheFrameWrongTypeResult.Body.Contains(TEXT("asset_is_not_geometry_cache")));

	FHttpSmokeResult MLInputsResult;
	TSharedPtr<FJsonObject> MLInputsJson;
	TestTrue(
		TEXT("ML Deformer training inputs validation succeeds"),
		ExecCommand(
			TEXT("smoke-mldeformer-inputs-001"),
			TEXT("ml_deformer_validate_training_inputs"),
			TEXT("{}"),
			MLInputsResult,
			MLInputsJson));
	TestEqual(TEXT("ML Deformer training inputs status code"), MLInputsResult.StatusCode, 200);
	TestTrue(TEXT("ML Deformer training inputs ok=true"), MLInputsJson.IsValid() && MLInputsJson->GetBoolField(TEXT("ok")));
	TestTrue(TEXT("ML Deformer training inputs include plugin status"), MLInputsResult.Body.Contains(TEXT("plugin_status")));
	const TSharedPtr<FJsonObject>* MLInputsData = nullptr;
	TestTrue(TEXT("ML Deformer training inputs contains data"), MLInputsJson.IsValid() && MLInputsJson->TryGetObjectField(TEXT("data"), MLInputsData) && MLInputsData && MLInputsData->IsValid());
	if (MLInputsJson.IsValid() && MLInputsJson->TryGetObjectField(TEXT("data"), MLInputsData) && MLInputsData && MLInputsData->IsValid())
	{
		TestFalse(TEXT("ML Deformer empty inputs validation_passed=false"), (*MLInputsData)->GetBoolField(TEXT("validation_passed")));
		TestTrue(TEXT("ML Deformer empty inputs has validation issues"), static_cast<int32>((*MLInputsData)->GetIntegerField(TEXT("validation_issue_count"))) >= 3);
		TestTrue(TEXT("ML Deformer empty inputs has topology_check"), (*MLInputsData)->HasTypedField<EJson::Object>(TEXT("topology_check")));
		TestTrue(TEXT("ML Deformer empty inputs has timing_check"), (*MLInputsData)->HasTypedField<EJson::Object>(TEXT("timing_check")));
	}

	FHttpSmokeResult MLTrainResult;
	TSharedPtr<FJsonObject> MLTrainJson;
	TestTrue(
		TEXT("ML Deformer train boundary reports adapter status"),
		ExecCommand(
			TEXT("smoke-mldeformer-train-boundary-001"),
			TEXT("ml_deformer_train"),
			TEXT("{\"confirm_long_running_training\":false}"),
			MLTrainResult,
			MLTrainJson));
	TestEqual(TEXT("ML Deformer train boundary status code"), MLTrainResult.StatusCode, 200);
	TestTrue(TEXT("ML Deformer train boundary ok=true"), MLTrainJson.IsValid() && MLTrainJson->GetBoolField(TEXT("ok")));
	TestTrue(TEXT("ML Deformer train boundary status included"), MLTrainResult.Body.Contains(TEXT("training_adapter_not_available")));

	FHttpSmokeResult MLPreviewResult;
	TSharedPtr<FJsonObject> MLPreviewJson;
	TestTrue(
		TEXT("ML Deformer preview boundary reports adapter status"),
		ExecCommand(
			TEXT("smoke-mldeformer-preview-boundary-001"),
			TEXT("ml_deformer_preview"),
			TEXT("{}"),
			MLPreviewResult,
			MLPreviewJson));
	TestEqual(TEXT("ML Deformer preview boundary status code"), MLPreviewResult.StatusCode, 200);
	TestTrue(TEXT("ML Deformer preview boundary ok=true"), MLPreviewJson.IsValid() && MLPreviewJson->GetBoolField(TEXT("ok")));
	TestTrue(TEXT("ML Deformer preview boundary status included"), MLPreviewResult.Body.Contains(TEXT("preview_adapter_not_available")));

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceEnhancedInputSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.EnhancedInputWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceEnhancedInputSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString InputActionAssetPath = MakeAutomationAssetPath(TEXT("IA"));
	const FString MappingContextAssetPath = MakeAutomationAssetPath(TEXT("IMC"));
	const FString InputActionValueType = TEXT("Axis2D");
	const FString InputKey = TEXT("SpaceBar");

	const FString CreateActionBody = MakeExecRequestBody(
		TEXT("smoke-enhanced-input-create-action-001"),
		TEXT("enhanced_input_create_action"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"value_type\":\"%s\",\"save_after_create\":false}"),
			*InputActionAssetPath,
			*InputActionValueType));
	FHttpSmokeResult CreateActionResult;
	TestTrue(TEXT("EnhancedInput create_action request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateActionBody, CreateActionResult));
	TestEqual(TEXT("EnhancedInput create_action status code"), CreateActionResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CreateActionJson;
	TestTrue(TEXT("EnhancedInput create_action response parses as JSON"), ParseJson(CreateActionResult.Body, CreateActionJson));
	TestTrue(TEXT("EnhancedInput create_action root ok=true"), CreateActionJson.IsValid() && CreateActionJson->GetBoolField(TEXT("ok")));

	FString InputActionObjectPath;
	const TSharedPtr<FJsonObject>* CreateActionData = nullptr;
	TestTrue(TEXT("EnhancedInput create_action contains data object"), CreateActionJson.IsValid() && CreateActionJson->TryGetObjectField(TEXT("data"), CreateActionData) && CreateActionData && CreateActionData->IsValid());
	if (CreateActionData && CreateActionData->IsValid())
	{
		InputActionObjectPath = (*CreateActionData)->GetStringField(TEXT("object_path"));
		TestEqual(TEXT("EnhancedInput create_action asset path"), (*CreateActionData)->GetStringField(TEXT("asset_path")), InputActionAssetPath);
		TestEqual(TEXT("EnhancedInput create_action value type"), (*CreateActionData)->GetStringField(TEXT("value_type")), InputActionValueType);
		TestFalse(TEXT("EnhancedInput create_action not saved"), (*CreateActionData)->GetBoolField(TEXT("saved")));
	}

	const FString GetActionInfoBeforeBody = MakeExecRequestBody(
		TEXT("smoke-enhanced-input-action-info-before-001"),
		TEXT("enhanced_input_get_action_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *InputActionAssetPath));
	FHttpSmokeResult GetActionInfoBeforeResult;
	TestTrue(TEXT("EnhancedInput get_action_info before request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetActionInfoBeforeBody, GetActionInfoBeforeResult));
	TestEqual(TEXT("EnhancedInput get_action_info before status code"), GetActionInfoBeforeResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetActionInfoBeforeJson;
	TestTrue(TEXT("EnhancedInput get_action_info before response parses as JSON"), ParseJson(GetActionInfoBeforeResult.Body, GetActionInfoBeforeJson));
	TestTrue(TEXT("EnhancedInput get_action_info before root ok=true"), GetActionInfoBeforeJson.IsValid() && GetActionInfoBeforeJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* GetActionInfoBeforeData = nullptr;
	TestTrue(TEXT("EnhancedInput get_action_info before contains data object"), GetActionInfoBeforeJson.IsValid() && GetActionInfoBeforeJson->TryGetObjectField(TEXT("data"), GetActionInfoBeforeData) && GetActionInfoBeforeData && GetActionInfoBeforeData->IsValid());
	if (GetActionInfoBeforeData && GetActionInfoBeforeData->IsValid())
	{
		TestEqual(TEXT("EnhancedInput get_action_info before value type"), (*GetActionInfoBeforeData)->GetStringField(TEXT("value_type")), InputActionValueType);
		TestTrue(TEXT("EnhancedInput get_action_info before object path is non-empty"), !(*GetActionInfoBeforeData)->GetStringField(TEXT("object_path")).IsEmpty());
	}

	const FString CreateMappingContextBody = MakeExecRequestBody(
		TEXT("smoke-enhanced-input-create-context-001"),
		TEXT("enhanced_input_create_mapping_context"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"save_after_create\":false}"), *MappingContextAssetPath));
	FHttpSmokeResult CreateMappingContextResult;
	TestTrue(TEXT("EnhancedInput create_mapping_context request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateMappingContextBody, CreateMappingContextResult));
	TestEqual(TEXT("EnhancedInput create_mapping_context status code"), CreateMappingContextResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CreateMappingContextJson;
	TestTrue(TEXT("EnhancedInput create_mapping_context response parses as JSON"), ParseJson(CreateMappingContextResult.Body, CreateMappingContextJson));
	TestTrue(TEXT("EnhancedInput create_mapping_context root ok=true"), CreateMappingContextJson.IsValid() && CreateMappingContextJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* CreateMappingContextData = nullptr;
	TestTrue(TEXT("EnhancedInput create_mapping_context contains data object"), CreateMappingContextJson.IsValid() && CreateMappingContextJson->TryGetObjectField(TEXT("data"), CreateMappingContextData) && CreateMappingContextData && CreateMappingContextData->IsValid());
	if (CreateMappingContextData && CreateMappingContextData->IsValid())
	{
		TestEqual(TEXT("EnhancedInput create_mapping_context asset path"), (*CreateMappingContextData)->GetStringField(TEXT("asset_path")), MappingContextAssetPath);
		TestEqual(TEXT("EnhancedInput create_mapping_context starts empty"), static_cast<int32>((*CreateMappingContextData)->GetIntegerField(TEXT("mapping_count"))), 0);
	}

	const FString GetContextInfoBeforeBody = MakeExecRequestBody(
		TEXT("smoke-enhanced-input-context-info-before-001"),
		TEXT("enhanced_input_get_mapping_context_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *MappingContextAssetPath));
	FHttpSmokeResult GetContextInfoBeforeResult;
	TestTrue(TEXT("EnhancedInput get_mapping_context_info before request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetContextInfoBeforeBody, GetContextInfoBeforeResult));
	TestEqual(TEXT("EnhancedInput get_mapping_context_info before status code"), GetContextInfoBeforeResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetContextInfoBeforeJson;
	TestTrue(TEXT("EnhancedInput get_mapping_context_info before response parses as JSON"), ParseJson(GetContextInfoBeforeResult.Body, GetContextInfoBeforeJson));
	TestTrue(TEXT("EnhancedInput get_mapping_context_info before root ok=true"), GetContextInfoBeforeJson.IsValid() && GetContextInfoBeforeJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* GetContextInfoBeforeData = nullptr;
	TestTrue(TEXT("EnhancedInput get_mapping_context_info before contains data object"), GetContextInfoBeforeJson.IsValid() && GetContextInfoBeforeJson->TryGetObjectField(TEXT("data"), GetContextInfoBeforeData) && GetContextInfoBeforeData && GetContextInfoBeforeData->IsValid());
	if (GetContextInfoBeforeData && GetContextInfoBeforeData->IsValid())
	{
		TestEqual(TEXT("EnhancedInput get_mapping_context_info before mapping count"), static_cast<int32>((*GetContextInfoBeforeData)->GetIntegerField(TEXT("mapping_count"))), 0);
	}

	const FString AddMappingBody = MakeExecRequestBody(
		TEXT("smoke-enhanced-input-add-mapping-001"),
		TEXT("enhanced_input_add_mapping"),
		FString::Printf(
			TEXT("{\"mapping_context_path\":\"%s\",\"action_path\":\"%s\",\"key\":\"%s\",\"save_after_set\":false}"),
			*MappingContextAssetPath,
			*InputActionAssetPath,
			*InputKey));
	FHttpSmokeResult AddMappingResult;
	TestTrue(TEXT("EnhancedInput add_mapping request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddMappingBody, AddMappingResult));
	TestEqual(TEXT("EnhancedInput add_mapping status code"), AddMappingResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddMappingJson;
	TestTrue(TEXT("EnhancedInput add_mapping response parses as JSON"), ParseJson(AddMappingResult.Body, AddMappingJson));
	TestTrue(TEXT("EnhancedInput add_mapping root ok=true"), AddMappingJson.IsValid() && AddMappingJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* AddMappingData = nullptr;
	TestTrue(TEXT("EnhancedInput add_mapping contains data object"), AddMappingJson.IsValid() && AddMappingJson->TryGetObjectField(TEXT("data"), AddMappingData) && AddMappingData && AddMappingData->IsValid());
	if (AddMappingData && AddMappingData->IsValid())
	{
		const TSharedPtr<FJsonObject>* MappingObject = nullptr;
		TestTrue(TEXT("EnhancedInput add_mapping contains mapping object"), (*AddMappingData)->TryGetObjectField(TEXT("mapping"), MappingObject) && MappingObject && MappingObject->IsValid());
		if (MappingObject && MappingObject->IsValid())
		{
			TestEqual(TEXT("EnhancedInput add_mapping key"), (*MappingObject)->GetStringField(TEXT("key")), InputKey);
			TestEqual(TEXT("EnhancedInput add_mapping action path"), (*MappingObject)->GetStringField(TEXT("action_path")), InputActionObjectPath);
		}
		TestEqual(TEXT("EnhancedInput add_mapping mapping count"), static_cast<int32>((*AddMappingData)->GetIntegerField(TEXT("mapping_count"))), 1);
	}

	const FString GetContextInfoAfterAddBody = MakeExecRequestBody(
		TEXT("smoke-enhanced-input-context-info-after-add-001"),
		TEXT("enhanced_input_get_mapping_context_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *MappingContextAssetPath));
	FHttpSmokeResult GetContextInfoAfterAddResult;
	TestTrue(TEXT("EnhancedInput get_mapping_context_info after add request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetContextInfoAfterAddBody, GetContextInfoAfterAddResult));
	TestEqual(TEXT("EnhancedInput get_mapping_context_info after add status code"), GetContextInfoAfterAddResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetContextInfoAfterAddJson;
	TestTrue(TEXT("EnhancedInput get_mapping_context_info after add response parses as JSON"), ParseJson(GetContextInfoAfterAddResult.Body, GetContextInfoAfterAddJson));
	TestTrue(TEXT("EnhancedInput get_mapping_context_info after add root ok=true"), GetContextInfoAfterAddJson.IsValid() && GetContextInfoAfterAddJson->GetBoolField(TEXT("ok")));

	bool bFoundAddedMapping = false;
	const TSharedPtr<FJsonObject>* GetContextInfoAfterAddData = nullptr;
	TestTrue(TEXT("EnhancedInput get_mapping_context_info after add contains data object"), GetContextInfoAfterAddJson.IsValid() && GetContextInfoAfterAddJson->TryGetObjectField(TEXT("data"), GetContextInfoAfterAddData) && GetContextInfoAfterAddData && GetContextInfoAfterAddData->IsValid());
	if (GetContextInfoAfterAddData && GetContextInfoAfterAddData->IsValid())
	{
		TestEqual(TEXT("EnhancedInput get_mapping_context_info after add mapping count"), static_cast<int32>((*GetContextInfoAfterAddData)->GetIntegerField(TEXT("mapping_count"))), 1);

		const TArray<TSharedPtr<FJsonValue>>* Mappings = nullptr;
		TestTrue(TEXT("EnhancedInput get_mapping_context_info after add contains mappings array"), (*GetContextInfoAfterAddData)->TryGetArrayField(TEXT("mappings"), Mappings) && Mappings != nullptr);
		if (Mappings)
		{
			for (const TSharedPtr<FJsonValue>& MappingValue : *Mappings)
			{
				const TSharedPtr<FJsonObject>* MappingObject = nullptr;
				if (!MappingValue.IsValid() || !MappingValue->TryGetObject(MappingObject) || !MappingObject || !MappingObject->IsValid())
				{
					continue;
				}

				if ((*MappingObject)->GetStringField(TEXT("key")) == InputKey)
				{
					bFoundAddedMapping = true;
					TestEqual(TEXT("EnhancedInput mapping action path in list"), (*MappingObject)->GetStringField(TEXT("action_path")), InputActionObjectPath);
					break;
				}
			}
		}
	}
	TestTrue(TEXT("EnhancedInput get_mapping_context_info after add contains added mapping"), bFoundAddedMapping);

	const FString RemoveMappingBody = MakeExecRequestBody(
		TEXT("smoke-enhanced-input-remove-mapping-001"),
		TEXT("enhanced_input_remove_mapping"),
		FString::Printf(
			TEXT("{\"mapping_context_path\":\"%s\",\"action_path\":\"%s\",\"key\":\"%s\",\"save_after_set\":false}"),
			*MappingContextAssetPath,
			*InputActionAssetPath,
			*InputKey));
	FHttpSmokeResult RemoveMappingResult;
	TestTrue(TEXT("EnhancedInput remove_mapping request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveMappingBody, RemoveMappingResult));
	TestEqual(TEXT("EnhancedInput remove_mapping status code"), RemoveMappingResult.StatusCode, 200);

	TSharedPtr<FJsonObject> RemoveMappingJson;
	TestTrue(TEXT("EnhancedInput remove_mapping response parses as JSON"), ParseJson(RemoveMappingResult.Body, RemoveMappingJson));
	TestTrue(TEXT("EnhancedInput remove_mapping root ok=true"), RemoveMappingJson.IsValid() && RemoveMappingJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* RemoveMappingData = nullptr;
	TestTrue(TEXT("EnhancedInput remove_mapping contains data object"), RemoveMappingJson.IsValid() && RemoveMappingJson->TryGetObjectField(TEXT("data"), RemoveMappingData) && RemoveMappingData && RemoveMappingData->IsValid());
	if (RemoveMappingData && RemoveMappingData->IsValid())
	{
		TestEqual(TEXT("EnhancedInput remove_mapping mapping count"), static_cast<int32>((*RemoveMappingData)->GetIntegerField(TEXT("mapping_count"))), 0);
	}

	const FString GetContextInfoAfterRemoveBody = MakeExecRequestBody(
		TEXT("smoke-enhanced-input-context-info-after-remove-001"),
		TEXT("enhanced_input_get_mapping_context_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *MappingContextAssetPath));
	FHttpSmokeResult GetContextInfoAfterRemoveResult;
	TestTrue(TEXT("EnhancedInput get_mapping_context_info after remove request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetContextInfoAfterRemoveBody, GetContextInfoAfterRemoveResult));
	TestEqual(TEXT("EnhancedInput get_mapping_context_info after remove status code"), GetContextInfoAfterRemoveResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetContextInfoAfterRemoveJson;
	TestTrue(TEXT("EnhancedInput get_mapping_context_info after remove response parses as JSON"), ParseJson(GetContextInfoAfterRemoveResult.Body, GetContextInfoAfterRemoveJson));
	TestTrue(TEXT("EnhancedInput get_mapping_context_info after remove root ok=true"), GetContextInfoAfterRemoveJson.IsValid() && GetContextInfoAfterRemoveJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* GetContextInfoAfterRemoveData = nullptr;
	TestTrue(TEXT("EnhancedInput get_mapping_context_info after remove contains data object"), GetContextInfoAfterRemoveJson.IsValid() && GetContextInfoAfterRemoveJson->TryGetObjectField(TEXT("data"), GetContextInfoAfterRemoveData) && GetContextInfoAfterRemoveData && GetContextInfoAfterRemoveData->IsValid());
	if (GetContextInfoAfterRemoveData && GetContextInfoAfterRemoveData->IsValid())
	{
		TestEqual(TEXT("EnhancedInput get_mapping_context_info after remove mapping count"), static_cast<int32>((*GetContextInfoAfterRemoveData)->GetIntegerField(TEXT("mapping_count"))), 0);
	}

	const FString JsonDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("EnhancedInputSingleJson"));
	IFileManager::Get().MakeDirectory(*JsonDir, true);
	const FString ActionJsonPath = FPaths::Combine(JsonDir, TEXT("InputAction.json"));
	const FString ContextJsonPath = FPaths::Combine(JsonDir, TEXT("InputMappingContext.json"));
	const FString ActionJsonPathForRequest = ActionJsonPath.Replace(TEXT("\\"), TEXT("/"));
	const FString ContextJsonPathForRequest = ContextJsonPath.Replace(TEXT("\\"), TEXT("/"));
	const FString JsonApplyKey = TEXT("LeftShift");
	const FString JsonApplyValueType = TEXT("Axis1D");

	const FString ExportActionJsonBody = MakeExecRequestBody(
		TEXT("smoke-enhanced-input-export-action-json-001"),
		TEXT("enhanced_input_export_action_json"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"output_file\":\"%s\"}"), *InputActionAssetPath, *ActionJsonPathForRequest));
	FHttpSmokeResult ExportActionJsonResult;
	TestTrue(TEXT("EnhancedInput export_action_json request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ExportActionJsonBody, ExportActionJsonResult));
	TestEqual(TEXT("EnhancedInput export_action_json status code"), ExportActionJsonResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ExportActionJsonResponse;
	TestTrue(TEXT("EnhancedInput export_action_json response parses as JSON"), ParseJson(ExportActionJsonResult.Body, ExportActionJsonResponse));
	TestTrue(TEXT("EnhancedInput export_action_json root ok=true"), ExportActionJsonResponse.IsValid() && ExportActionJsonResponse->GetBoolField(TEXT("ok")));

	const FString ExportContextJsonBody = MakeExecRequestBody(
		TEXT("smoke-enhanced-input-export-context-json-001"),
		TEXT("enhanced_input_export_mapping_context_json"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"output_file\":\"%s\"}"), *MappingContextAssetPath, *ContextJsonPathForRequest));
	FHttpSmokeResult ExportContextJsonResult;
	TestTrue(TEXT("EnhancedInput export_mapping_context_json request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ExportContextJsonBody, ExportContextJsonResult));
	TestEqual(TEXT("EnhancedInput export_mapping_context_json status code"), ExportContextJsonResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ExportContextJsonResponse;
	TestTrue(TEXT("EnhancedInput export_mapping_context_json response parses as JSON"), ParseJson(ExportContextJsonResult.Body, ExportContextJsonResponse));
	TestTrue(TEXT("EnhancedInput export_mapping_context_json root ok=true"), ExportContextJsonResponse.IsValid() && ExportContextJsonResponse->GetBoolField(TEXT("ok")));

	FString ActionJsonText;
	TestTrue(TEXT("EnhancedInput action json file exists"), FFileHelper::LoadFileToString(ActionJsonText, *ActionJsonPath));
	TSharedPtr<FJsonObject> ActionJsonRoot;
	TestTrue(TEXT("EnhancedInput action json file parses"), ParseJson(ActionJsonText, ActionJsonRoot));
	if (ActionJsonRoot.IsValid())
	{
		TArray<TSharedPtr<FJsonValue>> EmptyJsonArray;
		ActionJsonRoot->SetStringField(TEXT("value_type"), JsonApplyValueType);
		ActionJsonRoot->SetBoolField(TEXT("consume_input"), false);
		ActionJsonRoot->SetBoolField(TEXT("trigger_when_paused"), true);
		ActionJsonRoot->SetBoolField(TEXT("reserve_all_mappings"), true);
		ActionJsonRoot->SetArrayField(TEXT("modifier_classes"), EmptyJsonArray);
		ActionJsonRoot->SetArrayField(TEXT("trigger_classes"), EmptyJsonArray);
		ActionJsonRoot->SetNumberField(TEXT("modifier_count"), 0);
		ActionJsonRoot->SetNumberField(TEXT("trigger_count"), 0);

		FString UpdatedActionJsonText;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&UpdatedActionJsonText);
		TestTrue(TEXT("EnhancedInput action json file serializes"), FJsonSerializer::Serialize(ActionJsonRoot.ToSharedRef(), Writer));
		TestTrue(TEXT("EnhancedInput action json file saves"), FFileHelper::SaveStringToFile(UpdatedActionJsonText, *ActionJsonPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	}

	FString ContextJsonText;
	TestTrue(TEXT("EnhancedInput context json file exists"), FFileHelper::LoadFileToString(ContextJsonText, *ContextJsonPath));
	TSharedPtr<FJsonObject> ContextJsonRoot;
	TestTrue(TEXT("EnhancedInput context json file parses"), ParseJson(ContextJsonText, ContextJsonRoot));
	if (ContextJsonRoot.IsValid())
	{
		TArray<TSharedPtr<FJsonValue>> EmptyJsonArray;
		TSharedPtr<FJsonObject> MappingJson = MakeShared<FJsonObject>();
		MappingJson->SetStringField(TEXT("key"), JsonApplyKey);
		MappingJson->SetStringField(TEXT("action_path"), InputActionAssetPath);
		MappingJson->SetArrayField(TEXT("modifier_classes"), EmptyJsonArray);
		MappingJson->SetArrayField(TEXT("trigger_classes"), EmptyJsonArray);

		TArray<TSharedPtr<FJsonValue>> MappingArray;
		MappingArray.Add(MakeShared<FJsonValueObject>(MappingJson));
		ContextJsonRoot->SetArrayField(TEXT("mappings"), MappingArray);
		ContextJsonRoot->SetNumberField(TEXT("mapping_count"), 1);

		FString UpdatedContextJsonText;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&UpdatedContextJsonText);
		TestTrue(TEXT("EnhancedInput context json file serializes"), FJsonSerializer::Serialize(ContextJsonRoot.ToSharedRef(), Writer));
		TestTrue(TEXT("EnhancedInput context json file saves"), FFileHelper::SaveStringToFile(UpdatedContextJsonText, *ContextJsonPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	}

	const FString ApplyActionJsonBody = MakeExecRequestBody(
		TEXT("smoke-enhanced-input-apply-action-json-001"),
		TEXT("enhanced_input_apply_action_json"),
		FString::Printf(TEXT("{\"json_file\":\"%s\",\"save_after_apply\":false}"), *ActionJsonPathForRequest));
	FHttpSmokeResult ApplyActionJsonResult;
	TestTrue(TEXT("EnhancedInput apply_action_json request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ApplyActionJsonBody, ApplyActionJsonResult));
	TestEqual(TEXT("EnhancedInput apply_action_json status code"), ApplyActionJsonResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ApplyActionJsonResponse;
	TestTrue(TEXT("EnhancedInput apply_action_json response parses as JSON"), ParseJson(ApplyActionJsonResult.Body, ApplyActionJsonResponse));
	TestTrue(TEXT("EnhancedInput apply_action_json root ok=true"), ApplyActionJsonResponse.IsValid() && ApplyActionJsonResponse->GetBoolField(TEXT("ok")));

	const FString ApplyContextJsonBody = MakeExecRequestBody(
		TEXT("smoke-enhanced-input-apply-context-json-001"),
		TEXT("enhanced_input_apply_mapping_context_json"),
		FString::Printf(TEXT("{\"json_file\":\"%s\",\"save_after_apply\":false}"), *ContextJsonPathForRequest));
	FHttpSmokeResult ApplyContextJsonResult;
	TestTrue(TEXT("EnhancedInput apply_mapping_context_json request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ApplyContextJsonBody, ApplyContextJsonResult));
	TestEqual(TEXT("EnhancedInput apply_mapping_context_json status code"), ApplyContextJsonResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ApplyContextJsonResponse;
	TestTrue(TEXT("EnhancedInput apply_mapping_context_json response parses as JSON"), ParseJson(ApplyContextJsonResult.Body, ApplyContextJsonResponse));
	TestTrue(TEXT("EnhancedInput apply_mapping_context_json root ok=true"), ApplyContextJsonResponse.IsValid() && ApplyContextJsonResponse->GetBoolField(TEXT("ok")));

	const FString GetActionInfoAfterJsonApplyBody = MakeExecRequestBody(
		TEXT("smoke-enhanced-input-action-info-after-json-001"),
		TEXT("enhanced_input_get_action_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *InputActionAssetPath));
	FHttpSmokeResult GetActionInfoAfterJsonApplyResult;
	TestTrue(TEXT("EnhancedInput get_action_info after json apply request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetActionInfoAfterJsonApplyBody, GetActionInfoAfterJsonApplyResult));
	TestEqual(TEXT("EnhancedInput get_action_info after json apply status code"), GetActionInfoAfterJsonApplyResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetActionInfoAfterJsonApplyJson;
	TestTrue(TEXT("EnhancedInput get_action_info after json apply response parses as JSON"), ParseJson(GetActionInfoAfterJsonApplyResult.Body, GetActionInfoAfterJsonApplyJson));
	TestTrue(TEXT("EnhancedInput get_action_info after json apply root ok=true"), GetActionInfoAfterJsonApplyJson.IsValid() && GetActionInfoAfterJsonApplyJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* GetActionInfoAfterJsonApplyData = nullptr;
	TestTrue(TEXT("EnhancedInput get_action_info after json apply contains data object"), GetActionInfoAfterJsonApplyJson.IsValid() && GetActionInfoAfterJsonApplyJson->TryGetObjectField(TEXT("data"), GetActionInfoAfterJsonApplyData) && GetActionInfoAfterJsonApplyData && GetActionInfoAfterJsonApplyData->IsValid());
	if (GetActionInfoAfterJsonApplyData && GetActionInfoAfterJsonApplyData->IsValid())
	{
		TestEqual(TEXT("EnhancedInput get_action_info after json apply value type"), (*GetActionInfoAfterJsonApplyData)->GetStringField(TEXT("value_type")), JsonApplyValueType);
		TestFalse(TEXT("EnhancedInput get_action_info after json apply consume_input"), (*GetActionInfoAfterJsonApplyData)->GetBoolField(TEXT("consume_input")));
		TestTrue(TEXT("EnhancedInput get_action_info after json apply trigger_when_paused"), (*GetActionInfoAfterJsonApplyData)->GetBoolField(TEXT("trigger_when_paused")));
		TestTrue(TEXT("EnhancedInput get_action_info after json apply reserve_all_mappings"), (*GetActionInfoAfterJsonApplyData)->GetBoolField(TEXT("reserve_all_mappings")));
	}

	const FString GetContextInfoAfterJsonApplyBody = MakeExecRequestBody(
		TEXT("smoke-enhanced-input-context-info-after-json-001"),
		TEXT("enhanced_input_get_mapping_context_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *MappingContextAssetPath));
	FHttpSmokeResult GetContextInfoAfterJsonApplyResult;
	TestTrue(TEXT("EnhancedInput get_mapping_context_info after json apply request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetContextInfoAfterJsonApplyBody, GetContextInfoAfterJsonApplyResult));
	TestEqual(TEXT("EnhancedInput get_mapping_context_info after json apply status code"), GetContextInfoAfterJsonApplyResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetContextInfoAfterJsonApplyJson;
	TestTrue(TEXT("EnhancedInput get_mapping_context_info after json apply response parses as JSON"), ParseJson(GetContextInfoAfterJsonApplyResult.Body, GetContextInfoAfterJsonApplyJson));
	TestTrue(TEXT("EnhancedInput get_mapping_context_info after json apply root ok=true"), GetContextInfoAfterJsonApplyJson.IsValid() && GetContextInfoAfterJsonApplyJson->GetBoolField(TEXT("ok")));

	bool bFoundJsonAppliedMapping = false;
	const TSharedPtr<FJsonObject>* GetContextInfoAfterJsonApplyData = nullptr;
	TestTrue(TEXT("EnhancedInput get_mapping_context_info after json apply contains data object"), GetContextInfoAfterJsonApplyJson.IsValid() && GetContextInfoAfterJsonApplyJson->TryGetObjectField(TEXT("data"), GetContextInfoAfterJsonApplyData) && GetContextInfoAfterJsonApplyData && GetContextInfoAfterJsonApplyData->IsValid());
	if (GetContextInfoAfterJsonApplyData && GetContextInfoAfterJsonApplyData->IsValid())
	{
		TestEqual(TEXT("EnhancedInput get_mapping_context_info after json apply mapping count"), static_cast<int32>((*GetContextInfoAfterJsonApplyData)->GetIntegerField(TEXT("mapping_count"))), 1);

		const TArray<TSharedPtr<FJsonValue>>* Mappings = nullptr;
		TestTrue(TEXT("EnhancedInput get_mapping_context_info after json apply contains mappings array"), (*GetContextInfoAfterJsonApplyData)->TryGetArrayField(TEXT("mappings"), Mappings) && Mappings != nullptr);
		if (Mappings)
		{
			for (const TSharedPtr<FJsonValue>& MappingValue : *Mappings)
			{
				const TSharedPtr<FJsonObject>* MappingObject = nullptr;
				if (!MappingValue.IsValid() || !MappingValue->TryGetObject(MappingObject) || !MappingObject || !MappingObject->IsValid())
				{
					continue;
				}

				if ((*MappingObject)->GetStringField(TEXT("key")) == JsonApplyKey)
				{
					bFoundJsonAppliedMapping = true;
					break;
				}
			}
		}
	}
	TestTrue(TEXT("EnhancedInput get_mapping_context_info after json apply contains json-applied mapping"), bFoundJsonAppliedMapping);

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceMaterialFolderSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.MaterialFolderWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceMaterialFolderSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString MaterialAssetPath = MakeAutomationAssetPath(TEXT("MATFOLDER"));
	FString RelativeFolder = MaterialAssetPath;
	while (RelativeFolder.StartsWith(TEXT("/")))
	{
		RelativeFolder.RightChopInline(1);
	}
	const FString FolderPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("MaterialGraph"), RelativeFolder));
	IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	IFileManager::Get().MakeDirectory(*FPaths::Combine(FolderPath, TEXT("settings")), true);
	IFileManager::Get().MakeDirectory(*FPaths::Combine(FolderPath, TEXT("parameters")), true);
	IFileManager::Get().MakeDirectory(*FPaths::Combine(FolderPath, TEXT("graphs")), true);
	IFileManager::Get().MakeDirectory(*FPaths::Combine(FolderPath, TEXT("validation")), true);

	auto SaveUtf8Json = [this](const FString& FilePath, const FString& JsonText) -> bool
	{
		const bool bOk = FFileHelper::SaveStringToFile(JsonText, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		TestTrue(FString::Printf(TEXT("Save JSON file: %s"), *FilePath), bOk);
		return bOk;
	};

	if (!SaveUtf8Json(
		FPaths::Combine(FolderPath, TEXT("asset.json")),
		FString::Printf(
			TEXT("{\n  \"format_version\": 1,\n  \"asset_kind\": \"material_graph\",\n  \"asset_path\": \"%s\",\n  \"asset_name\": \"%s\",\n  \"profile_version\": 1,\n  \"material_subkind\": \"material\"\n}\n"),
			*MaterialAssetPath,
			*FPackageName::GetLongPackageAssetName(MaterialAssetPath))))
	{
		return false;
	}

	if (!SaveUtf8Json(
		FPaths::Combine(FolderPath, TEXT("settings"), TEXT("material.json")),
		TEXT("{\n  \"properties\": [\n    { \"key\": \"material_domain\", \"property_name\": \"MaterialDomain\", \"value_text\": \"MD_Surface\" },\n    { \"key\": \"blend_mode\", \"property_name\": \"BlendMode\", \"value_text\": \"BLEND_Opaque\" },\n    { \"key\": \"two_sided\", \"property_name\": \"TwoSided\", \"value_text\": \"true\" }\n  ]\n}\n")))
	{
		return false;
	}

	if (!SaveUtf8Json(
		FPaths::Combine(FolderPath, TEXT("parameters"), TEXT("interface.json")),
		TEXT("{\n  \"parameters\": [\n    {\n      \"name\": \"Color_Main\",\n      \"parameter_type\": \"vector\",\n      \"backing_expression_id\": \"param_color_main\",\n      \"group\": \"Surface\",\n      \"sort_priority\": \"0\",\n      \"default_property_name\": \"DefaultValue\",\n      \"default_value_text\": \"(R=0.200000,G=0.400000,B=1.000000,A=1.000000)\"\n    }\n  ]\n}\n")))
	{
		return false;
	}

	if (!SaveUtf8Json(
		FPaths::Combine(FolderPath, TEXT("graphs"), TEXT("MaterialGraph.json")),
		TEXT("{\n  \"graph\": {\n    \"name\": \"MaterialGraph\",\n    \"graph_kind\": \"material_graph\"\n  },\n  \"expressions\": [\n    {\n      \"id\": \"param_color_main\",\n      \"expression_class\": \"/Script/Engine.MaterialExpressionVectorParameter\",\n      \"expression_name\": \"SmokeColorParam\",\n      \"pos\": { \"x\": -420, \"y\": 0 },\n      \"properties\": [\n        { \"property_name\": \"ParameterName\", \"value_text\": \"Color_Main\" },\n        { \"property_name\": \"DefaultValue\", \"value_text\": \"(R=0.200000,G=0.400000,B=1.000000,A=1.000000)\" },\n        { \"property_name\": \"Group\", \"value_text\": \"Surface\" },\n        { \"property_name\": \"SortPriority\", \"value_text\": \"0\" }\n      ]\n    },\n    {\n      \"id\": \"custom_passthrough\",\n      \"expression_class\": \"/Script/Engine.MaterialExpressionCustom\",\n      \"expression_name\": \"SmokeCustomPassthrough\",\n      \"pos\": { \"x\": -120, \"y\": 0 },\n      \"properties\": [\n        { \"property_name\": \"Code\", \"value_text\": \"return InputA;\" },\n        { \"property_name\": \"Description\", \"value_text\": \"SmokeCustomPassthrough\" },\n        { \"property_name\": \"OutputType\", \"value_text\": \"CMOT_Float3\" }\n      ]\n    }\n  ],\n  \"edges\": [\n    {\n      \"from\": { \"node\": \"param_color_main\", \"output\": \"None\" },\n      \"to\": { \"node\": \"custom_passthrough\", \"input\": \"InputA\" }\n    }\n  ],\n  \"root_inputs\": [\n    {\n      \"material_property\": \"BaseColor\",\n      \"from\": { \"node\": \"custom_passthrough\", \"output\": \"None\" }\n    }\n  ]\n}\n")))
	{
		return false;
	}

	if (!SaveUtf8Json(
		FPaths::Combine(FolderPath, TEXT("validation"), TEXT("checks.json")),
		TEXT("{\n  \"checks\": [\n    { \"kind\": \"compile_success\" }\n  ]\n}\n")))
	{
		return false;
	}

	const FString AuthoringSettingsJsonPath = FPaths::Combine(FolderPath, TEXT("settings"), TEXT("material.json"));
	TSharedPtr<FJsonObject> SettingsAuthoringJson;
	if (!LoadSmokeJsonFile(this, AuthoringSettingsJsonPath, SettingsAuthoringJson, TEXT("Material folder settings authoring json"))
		|| !SmokeMoveInlinePropertiesToGroupedDetails(
			this,
			AuthoringSettingsJsonPath,
			SettingsAuthoringJson,
			TEXT("material_settings"),
			TEXT("settings"),
			TEXT("settings"),
			TEXT("material_apply_folder"))
		|| !SaveSmokeJsonFile(this, AuthoringSettingsJsonPath, SettingsAuthoringJson, TEXT("Material folder settings grouped authoring json")))
	{
		return false;
	}

	const FString AuthoringGraphJsonPath = FPaths::Combine(FolderPath, TEXT("graphs"), TEXT("MaterialGraph.json"));
	TSharedPtr<FJsonObject> GraphAuthoringJson;
	if (!LoadSmokeJsonFile(this, AuthoringGraphJsonPath, GraphAuthoringJson, TEXT("Material folder graph authoring json")))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* AuthoringExpressions = nullptr;
	if (!GraphAuthoringJson->TryGetArrayField(TEXT("expressions"), AuthoringExpressions) || !AuthoringExpressions)
	{
		AddError(TEXT("Material folder graph authoring expressions missing"));
		return false;
	}
	for (const TSharedPtr<FJsonValue>& ExpressionValue : *AuthoringExpressions)
	{
		const TSharedPtr<FJsonObject>* ExpressionObj = nullptr;
		if (!ExpressionValue.IsValid() || !ExpressionValue->TryGetObject(ExpressionObj) || !ExpressionObj || !ExpressionObj->IsValid())
		{
			continue;
		}
		FString ExpressionId;
		if (!(*ExpressionObj)->TryGetStringField(TEXT("id"), ExpressionId) || ExpressionId.IsEmpty())
		{
			AddError(TEXT("Material folder expression id missing before details migration"));
			return false;
		}
		if (!SmokeMoveInlinePropertiesToGroupedDetails(
			this,
			AuthoringGraphJsonPath,
			*ExpressionObj,
			TEXT("material_expression"),
			ExpressionId,
			TEXT("expressions"),
			TEXT("material_apply_folder")))
		{
			return false;
		}
	}
	if (!SaveSmokeJsonFile(this, AuthoringGraphJsonPath, GraphAuthoringJson, TEXT("Material folder graph grouped authoring json")))
	{
		return false;
	}

	const FString ApplyBody = MakeExecRequestBody(
		TEXT("smoke-material-folder-apply-001"),
		TEXT("material_apply_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"create_if_missing\":true,\"compile_after_apply\":true,\"save_after_apply\":false}"), *MaterialAssetPath));
	FHttpSmokeResult ApplyResult;
	TestTrue(TEXT("Material folder apply request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ApplyBody, ApplyResult, 15.0));
	TestEqual(TEXT("Material folder apply status code"), ApplyResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ApplyJson;
	TestTrue(TEXT("Material folder apply parses JSON"), ParseJson(ApplyResult.Body, ApplyJson));
	TestTrue(TEXT("Material folder apply root ok=true"), ApplyJson.IsValid() && ApplyJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* ApplyData = nullptr;
	TestTrue(TEXT("Material folder apply contains data object"), ApplyJson.IsValid() && ApplyJson->TryGetObjectField(TEXT("data"), ApplyData) && ApplyData && ApplyData->IsValid());
	if (ApplyData && ApplyData->IsValid())
	{
		TestTrue(TEXT("Material folder apply applied settings"), (*ApplyData)->GetIntegerField(TEXT("settings_applied")) >= 3);
		TestTrue(TEXT("Material folder apply created expressions"), (*ApplyData)->GetIntegerField(TEXT("expressions_created")) >= 1);
		TestTrue(TEXT("Material folder apply connected expression edge with None output"), (*ApplyData)->GetIntegerField(TEXT("edges_created")) >= 1);
		TestTrue(TEXT("Material folder apply connected root input"), (*ApplyData)->GetIntegerField(TEXT("root_inputs_connected")) >= 1);
	}

	const FString GetInfoBody = MakeExecRequestBody(
		TEXT("smoke-material-folder-info-001"),
		TEXT("material_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *MaterialAssetPath));
	FHttpSmokeResult GetInfoResult;
	TestTrue(TEXT("Material folder get_info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoBody, GetInfoResult, 10.0));
	TestEqual(TEXT("Material folder get_info status code"), GetInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoJson;
	TestTrue(TEXT("Material folder get_info parses JSON"), ParseJson(GetInfoResult.Body, GetInfoJson));
	TestTrue(TEXT("Material folder get_info root ok=true"), GetInfoJson.IsValid() && GetInfoJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* GetInfoData = nullptr;
	TestTrue(TEXT("Material folder get_info contains data object"), GetInfoJson.IsValid() && GetInfoJson->TryGetObjectField(TEXT("data"), GetInfoData) && GetInfoData && GetInfoData->IsValid());
	if (GetInfoData && GetInfoData->IsValid())
	{
		TestFalse(TEXT("Material folder get_info reports not instance"), (*GetInfoData)->GetBoolField(TEXT("is_material_instance")));
		TestTrue(TEXT("Material folder get_info expression count >= 1"), (*GetInfoData)->GetIntegerField(TEXT("expression_count")) >= 1);
	}

	const FString ExportBody = MakeExecRequestBody(
		TEXT("smoke-material-folder-export-001"),
		TEXT("material_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clean_output_dir\":true,\"include_validation\":true}"), *MaterialAssetPath));
	FHttpSmokeResult ExportResult;
	TestTrue(TEXT("Material folder export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ExportBody, ExportResult, 15.0));
	TestEqual(TEXT("Material folder export status code"), ExportResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ExportJson;
	TestTrue(TEXT("Material folder export parses JSON"), ParseJson(ExportResult.Body, ExportJson));
	TestTrue(TEXT("Material folder export root ok=true"), ExportJson.IsValid() && ExportJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* ExportData = nullptr;
	TestTrue(TEXT("Material folder export contains data object"), ExportJson.IsValid() && ExportJson->TryGetObjectField(TEXT("data"), ExportData) && ExportData && ExportData->IsValid());
	if (ExportData && ExportData->IsValid())
	{
		TestEqual(TEXT("Material folder export folder path"), (*ExportData)->GetStringField(TEXT("folder_path")), FolderPath);
		TestTrue(TEXT("Material folder export file count reasonable"), (*ExportData)->GetIntegerField(TEXT("file_count")) >= 4);
	}

	TSharedPtr<FJsonObject> ExportedAssetJson;
	{
		const FString AssetJsonPath = FPaths::Combine(FolderPath, TEXT("asset.json"));
		FString ExportedAssetText;
		TestTrue(TEXT("Material folder exported asset.json exists"), FFileHelper::LoadFileToString(ExportedAssetText, *AssetJsonPath));
		TestTrue(TEXT("Material folder exported asset.json parses"), ParseJson(ExportedAssetText, ExportedAssetJson));
		if (ExportedAssetJson.IsValid())
		{
			TestEqual(TEXT("Material folder exported asset kind"), ExportedAssetJson->GetStringField(TEXT("asset_kind")), FString(TEXT("material_graph")));
			TestEqual(TEXT("Material folder exported asset path"), ExportedAssetJson->GetStringField(TEXT("asset_path")), MaterialAssetPath);
		}
	}

	TSharedPtr<FJsonObject> ExportedInterfaceJson;
	{
		const FString InterfaceJsonPath = FPaths::Combine(FolderPath, TEXT("parameters"), TEXT("interface.json"));
		FString ExportedInterfaceText;
		TestTrue(TEXT("Material folder exported interface.json exists"), FFileHelper::LoadFileToString(ExportedInterfaceText, *InterfaceJsonPath));
		TestTrue(TEXT("Material folder exported interface.json parses"), ParseJson(ExportedInterfaceText, ExportedInterfaceJson));
		if (ExportedInterfaceJson.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* ParametersArray = nullptr;
			TestTrue(TEXT("Material folder exported parameters array exists"), ExportedInterfaceJson->TryGetArrayField(TEXT("parameters"), ParametersArray) && ParametersArray);
			if (ParametersArray)
			{
				bool bFoundColorMain = false;
				bool bFoundParameterAuthoringMetadata = false;
				for (const TSharedPtr<FJsonValue>& ParameterValue : *ParametersArray)
				{
					const TSharedPtr<FJsonObject>* ParameterObj = nullptr;
					if (!ParameterValue.IsValid() || !ParameterValue->TryGetObject(ParameterObj) || !ParameterObj || !ParameterObj->IsValid())
					{
						continue;
					}
					if ((*ParameterObj)->GetStringField(TEXT("name")) == TEXT("Color_Main"))
					{
						bFoundColorMain = true;
						bFoundParameterAuthoringMetadata = (*ParameterObj)->HasTypedField<EJson::Object>(TEXT("selector"))
							&& (*ParameterObj)->HasTypedField<EJson::Array>(TEXT("operations"));
						break;
					}
				}
				TestTrue(TEXT("Material folder exported Color_Main parameter"), bFoundColorMain);
				TestTrue(TEXT("Material folder parameter exposes authoring metadata"), bFoundParameterAuthoringMetadata);
			}
		}
	}

	TSharedPtr<FJsonObject> ExportedSettingsJson;
	{
		const FString SettingsJsonPath = FPaths::Combine(FolderPath, TEXT("settings"), TEXT("material.json"));
		FString ExportedSettingsText;
		TestTrue(TEXT("Material folder exported material.json exists"), FFileHelper::LoadFileToString(ExportedSettingsText, *SettingsJsonPath));
		TestFalse(TEXT("Material folder exported material.json omits inline properties"), ExportedSettingsText.Contains(TEXT("\"properties\"")));
		TestTrue(TEXT("Material folder exported material.json parses"), ParseJson(ExportedSettingsText, ExportedSettingsJson));
		if (ExportedSettingsJson.IsValid())
		{
			TSharedPtr<FJsonObject> SettingsDetailsJson;
			TestTrue(
				TEXT("Material folder settings details grouped"),
				SmokeLoadGroupedDetailsForRef(this, SettingsJsonPath, ExportedSettingsJson, TEXT("details"), TEXT("material_settings"), SettingsDetailsJson));
			TestTrue(
				TEXT("Material folder settings details contains TwoSided"),
				SmokeDetailsPropertiesContain(SettingsDetailsJson, TEXT("TwoSided")));
		}
	}

	TSharedPtr<FJsonObject> ExportedGraphJson;
	{
		const FString GraphJsonPath = FPaths::Combine(FolderPath, TEXT("graphs"), TEXT("MaterialGraph.json"));
		FString ExportedGraphText;
		TestTrue(TEXT("Material folder exported MaterialGraph.json exists"), FFileHelper::LoadFileToString(ExportedGraphText, *GraphJsonPath));
		TestFalse(TEXT("Material folder exported MaterialGraph.json omits inline properties"), ExportedGraphText.Contains(TEXT("\"properties\"")));
		TestTrue(TEXT("Material folder exported MaterialGraph.json parses"), ParseJson(ExportedGraphText, ExportedGraphJson));
		if (ExportedGraphJson.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* ExpressionsArray = nullptr;
			TestTrue(TEXT("Material folder exported expressions array exists"), ExportedGraphJson->TryGetArrayField(TEXT("expressions"), ExpressionsArray) && ExpressionsArray);
			bool bFoundExpressionAuthoringMetadata = false;
			bool bFoundExpressionDetails = false;
			if (ExpressionsArray)
			{
				for (const TSharedPtr<FJsonValue>& ExpressionValue : *ExpressionsArray)
				{
					const TSharedPtr<FJsonObject>* ExpressionObj = nullptr;
					if (!ExpressionValue.IsValid() || !ExpressionValue->TryGetObject(ExpressionObj) || !ExpressionObj || !ExpressionObj->IsValid())
					{
						continue;
					}
					bFoundExpressionAuthoringMetadata = bFoundExpressionAuthoringMetadata
						|| ((*ExpressionObj)->HasTypedField<EJson::Object>(TEXT("selector")) && (*ExpressionObj)->HasTypedField<EJson::Array>(TEXT("operations")));

					TSharedPtr<FJsonObject> ExpressionDetailsJson;
					if (SmokeLoadGroupedDetailsForRef(this, GraphJsonPath, *ExpressionObj, TEXT("details"), TEXT("material_expression"), ExpressionDetailsJson))
					{
						bFoundExpressionDetails = bFoundExpressionDetails
							|| SmokeDetailsPropertiesContain(ExpressionDetailsJson, TEXT("DefaultValue"))
							|| SmokeDetailsPropertiesContain(ExpressionDetailsJson, TEXT("Code"));
					}
				}
			}
			TestTrue(TEXT("Material folder expression exposes authoring metadata"), bFoundExpressionAuthoringMetadata);
			TestTrue(TEXT("Material folder expression details grouped"), bFoundExpressionDetails);
		}
	}

	if (ExportedSettingsJson.IsValid())
	{
		TArray<TSharedPtr<FJsonValue>> LegacyProperties;
		LegacyProperties.Add(MakeShared<FJsonValueObject>(SmokeMakeDetailsProperty(TEXT("TwoSided"), TEXT("false"))));
		ExportedSettingsJson->SetArrayField(TEXT("properties"), LegacyProperties);
		const FString LegacySettingsJsonPath = FPaths::Combine(FolderPath, TEXT("settings"), TEXT("material.json"));
		TestTrue(TEXT("Material folder save legacy inline properties"), SaveSmokeJsonFile(this, LegacySettingsJsonPath, ExportedSettingsJson, TEXT("Material folder legacy settings json")));

		const FString LegacyApplyBody = MakeExecRequestBody(
			TEXT("smoke-material-folder-legacy-properties-001"),
			TEXT("material_apply_folder"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"compile_after_apply\":false,\"save_after_apply\":false}"), *MaterialAssetPath));
		FHttpSmokeResult LegacyApplyResult;
		TestTrue(TEXT("Material folder legacy properties request reached server"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, LegacyApplyBody, LegacyApplyResult, 15.0));
		TestEqual(TEXT("Material folder legacy properties are rejected"), LegacyApplyResult.StatusCode, 400);

		TSharedPtr<FJsonObject> LegacyApplyJson;
		TestTrue(TEXT("Material folder legacy properties response parses JSON"), ParseJson(LegacyApplyResult.Body, LegacyApplyJson));
		const TSharedPtr<FJsonObject>* LegacyApplyData = nullptr;
		TestTrue(TEXT("Material folder legacy rejection data exists"), LegacyApplyJson.IsValid() && LegacyApplyJson->TryGetObjectField(TEXT("data"), LegacyApplyData) && LegacyApplyData && LegacyApplyData->IsValid());
		if (LegacyApplyData && LegacyApplyData->IsValid())
		{
			TestEqual(TEXT("Material folder legacy rejection schema"), (*LegacyApplyData)->GetStringField(TEXT("schema")), FString(TEXT("uai.legacy_rejection.v1")));
			TestEqual(TEXT("Material folder legacy rejection error code"), (*LegacyApplyData)->GetStringField(TEXT("error_code")), FString(TEXT("legacy_inline_properties_not_supported")));
			TestEqual(TEXT("Material folder legacy rejection surface"), (*LegacyApplyData)->GetStringField(TEXT("legacy_surface")), FString(TEXT("properties[]")));
		}
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceMaterialInstanceFolderSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.MaterialInstanceFolderWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceMaterialInstanceFolderSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString ParentMaterialPath = MakeAutomationAssetPath(TEXT("MIPARENT"));
	const FString MaterialInstanceAssetPath = MakeAutomationAssetPath(TEXT("MIFOLDER"));
	FString RelativeParentFolder = ParentMaterialPath;
	while (RelativeParentFolder.StartsWith(TEXT("/")))
	{
		RelativeParentFolder.RightChopInline(1);
	}
	const FString ParentFolderPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("MaterialGraph"), RelativeParentFolder));
	IFileManager::Get().DeleteDirectory(*ParentFolderPath, false, true);
	IFileManager::Get().MakeDirectory(*FPaths::Combine(ParentFolderPath, TEXT("settings")), true);
	IFileManager::Get().MakeDirectory(*FPaths::Combine(ParentFolderPath, TEXT("parameters")), true);
	IFileManager::Get().MakeDirectory(*FPaths::Combine(ParentFolderPath, TEXT("graphs")), true);
	IFileManager::Get().MakeDirectory(*FPaths::Combine(ParentFolderPath, TEXT("validation")), true);

	auto SaveUtf8Json = [this](const FString& FilePath, const FString& JsonText) -> bool
	{
		const bool bOk = FFileHelper::SaveStringToFile(JsonText, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		TestTrue(FString::Printf(TEXT("Save JSON file: %s"), *FilePath), bOk);
		return bOk;
	};

	if (!SaveUtf8Json(
		FPaths::Combine(ParentFolderPath, TEXT("asset.json")),
		FString::Printf(
			TEXT("{\n  \"format_version\": 1,\n  \"asset_kind\": \"material_graph\",\n  \"asset_path\": \"%s\",\n  \"asset_name\": \"%s\",\n  \"profile_version\": 1,\n  \"material_subkind\": \"material\"\n}\n"),
			*ParentMaterialPath,
			*FPackageName::GetLongPackageAssetName(ParentMaterialPath))))
	{
		return false;
	}

	if (!SaveUtf8Json(
		FPaths::Combine(ParentFolderPath, TEXT("settings"), TEXT("material.json")),
		TEXT("{\n  \"properties\": [\n    { \"key\": \"material_domain\", \"property_name\": \"MaterialDomain\", \"value_text\": \"MD_Surface\" },\n    { \"key\": \"blend_mode\", \"property_name\": \"BlendMode\", \"value_text\": \"BLEND_Opaque\" }\n  ]\n}\n")))
	{
		return false;
	}

	if (!SaveUtf8Json(
		FPaths::Combine(ParentFolderPath, TEXT("parameters"), TEXT("interface.json")),
		TEXT("{\n  \"parameters\": [\n    {\n      \"name\": \"BillboardColor\",\n      \"parameter_type\": \"vector\",\n      \"backing_expression_id\": \"param_billboard_color\",\n      \"group\": \"Smoke\",\n      \"sort_priority\": \"0\",\n      \"default_property_name\": \"DefaultValue\",\n      \"default_value_text\": \"(R=1.000000,G=1.000000,B=1.000000,A=1.000000)\"\n    }\n  ]\n}\n")))
	{
		return false;
	}

	if (!SaveUtf8Json(
		FPaths::Combine(ParentFolderPath, TEXT("graphs"), TEXT("MaterialGraph.json")),
		TEXT("{\n  \"graph\": {\n    \"name\": \"MaterialGraph\",\n    \"graph_kind\": \"material_graph\"\n  },\n  \"expressions\": [\n    {\n      \"id\": \"param_billboard_color\",\n      \"expression_class\": \"/Script/Engine.MaterialExpressionVectorParameter\",\n      \"expression_name\": \"SmokeBillboardColorParam\",\n      \"pos\": { \"x\": -280, \"y\": 0 },\n      \"properties\": [\n        { \"property_name\": \"ParameterName\", \"value_text\": \"BillboardColor\" },\n        { \"property_name\": \"DefaultValue\", \"value_text\": \"(R=1.000000,G=1.000000,B=1.000000,A=1.000000)\" },\n        { \"property_name\": \"Group\", \"value_text\": \"Smoke\" },\n        { \"property_name\": \"SortPriority\", \"value_text\": \"0\" }\n      ]\n    }\n  ],\n  \"edges\": [],\n  \"root_inputs\": [\n    {\n      \"material_property\": \"BaseColor\",\n      \"from\": { \"node\": \"param_billboard_color\", \"output\": \"\" }\n    }\n  ]\n}\n")))
	{
		return false;
	}

	if (!SaveUtf8Json(
		FPaths::Combine(ParentFolderPath, TEXT("validation"), TEXT("checks.json")),
		TEXT("{\n  \"checks\": [\n    { \"kind\": \"compile_success\" }\n  ]\n}\n")))
	{
		return false;
	}

	const FString ParentSettingsJsonPath = FPaths::Combine(ParentFolderPath, TEXT("settings"), TEXT("material.json"));
	TSharedPtr<FJsonObject> ParentSettingsJson;
	if (!LoadSmokeJsonFile(this, ParentSettingsJsonPath, ParentSettingsJson, TEXT("Material instance parent settings authoring json"))
		|| !SmokeMoveInlinePropertiesToGroupedDetails(
			this,
			ParentSettingsJsonPath,
			ParentSettingsJson,
			TEXT("material_settings"),
			TEXT("settings"),
			TEXT("settings"),
			TEXT("material_apply_folder"))
		|| !SaveSmokeJsonFile(this, ParentSettingsJsonPath, ParentSettingsJson, TEXT("Material instance parent settings grouped authoring json")))
	{
		return false;
	}

	const FString ParentGraphJsonPath = FPaths::Combine(ParentFolderPath, TEXT("graphs"), TEXT("MaterialGraph.json"));
	TSharedPtr<FJsonObject> ParentGraphJson;
	if (!LoadSmokeJsonFile(this, ParentGraphJsonPath, ParentGraphJson, TEXT("Material instance parent graph authoring json")))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* ParentExpressions = nullptr;
	if (!ParentGraphJson->TryGetArrayField(TEXT("expressions"), ParentExpressions) || !ParentExpressions)
	{
		AddError(TEXT("Material instance parent graph authoring expressions missing"));
		return false;
	}
	for (const TSharedPtr<FJsonValue>& ExpressionValue : *ParentExpressions)
	{
		const TSharedPtr<FJsonObject>* ExpressionObj = nullptr;
		if (!ExpressionValue.IsValid() || !ExpressionValue->TryGetObject(ExpressionObj) || !ExpressionObj || !ExpressionObj->IsValid())
		{
			continue;
		}
		FString ExpressionId;
		if (!(*ExpressionObj)->TryGetStringField(TEXT("id"), ExpressionId) || ExpressionId.IsEmpty())
		{
			AddError(TEXT("Material instance parent expression id missing before details migration"));
			return false;
		}
		if (!SmokeMoveInlinePropertiesToGroupedDetails(
			this,
			ParentGraphJsonPath,
			*ExpressionObj,
			TEXT("material_expression"),
			ExpressionId,
			TEXT("expressions"),
			TEXT("material_apply_folder")))
		{
			return false;
		}
	}
	if (!SaveSmokeJsonFile(this, ParentGraphJsonPath, ParentGraphJson, TEXT("Material instance parent graph grouped authoring json")))
	{
		return false;
	}

	const FString ParentApplyBody = MakeExecRequestBody(
		TEXT("smoke-material-instance-folder-parent-apply-001"),
		TEXT("material_apply_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"create_if_missing\":true,\"compile_after_apply\":true,\"save_after_apply\":false}"), *ParentMaterialPath));
	FHttpSmokeResult ParentApplyResult;
	TestTrue(TEXT("Material instance folder parent apply request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ParentApplyBody, ParentApplyResult, 15.0));
	TestEqual(TEXT("Material instance folder parent apply status code"), ParentApplyResult.StatusCode, 200);
	if (ParentApplyResult.StatusCode != 200)
	{
		AddError(FString::Printf(TEXT("Parent material apply response: %s"), *ParentApplyResult.Body));
		return false;
	}

	FString RelativeFolder = MaterialInstanceAssetPath;
	while (RelativeFolder.StartsWith(TEXT("/")))
	{
		RelativeFolder.RightChopInline(1);
	}
	const FString FolderPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("MaterialInstance"), RelativeFolder));
	IFileManager::Get().DeleteDirectory(*FolderPath, false, true);

	const FString CreateBody = MakeExecRequestBody(
		TEXT("smoke-material-instance-folder-create-001"),
		TEXT("material_instance_create"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"parent_material_path\":\"%s\",\"save_after_create\":false}"),
			*MaterialInstanceAssetPath,
			*ParentMaterialPath));
	FHttpSmokeResult CreateResult;
	TestTrue(TEXT("Material instance folder create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateBody, CreateResult));
	TestEqual(TEXT("Material instance folder create status code"), CreateResult.StatusCode, 200);

	const FString SetVectorBody = MakeExecRequestBody(
		TEXT("smoke-material-instance-folder-set-vector-001"),
		TEXT("material_set_parameter"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"parameter_type\":\"vector\",\"parameter_name\":\"BillboardColor\",\"value\":{\"r\":1.0,\"g\":0.0,\"b\":1.0,\"a\":1.0}}"),
			*MaterialInstanceAssetPath));
	FHttpSmokeResult SetVectorResult;
	TestTrue(TEXT("Material instance folder set vector request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetVectorBody, SetVectorResult));
	TestEqual(TEXT("Material instance folder set vector status code"), SetVectorResult.StatusCode, 200);

	const FString ExportBody = MakeExecRequestBody(
		TEXT("smoke-material-instance-folder-export-001"),
		TEXT("material_instance_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clean_output_dir\":true,\"include_validation\":true}"), *MaterialInstanceAssetPath));
	FHttpSmokeResult ExportResult;
	TestTrue(TEXT("Material instance folder export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ExportBody, ExportResult));
	TestEqual(TEXT("Material instance folder export status code"), ExportResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ExportJson;
	TestTrue(TEXT("Material instance folder export parses JSON"), ParseJson(ExportResult.Body, ExportJson));
	TestTrue(TEXT("Material instance folder export root ok=true"), ExportJson.IsValid() && ExportJson->GetBoolField(TEXT("ok")));

	const FString LegacyPropertiesPath = FPaths::Combine(FolderPath, TEXT("properties.json"));
	TestTrue(TEXT("Material instance folder legacy properties.json writes"), SaveUtf8Json(
		LegacyPropertiesPath,
		TEXT("{\"properties\":[{\"property_name\":\"BlendMode\",\"value_text\":\"BLEND_Opaque\"}]}\n")));

	const FString LegacyApplyBody = MakeExecRequestBody(
		TEXT("smoke-material-instance-folder-legacy-apply-001"),
		TEXT("material_instance_apply_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"create_if_missing\":false,\"clear_existing_overrides\":false,\"compile_after_apply\":false,\"save_after_apply\":false}"), *MaterialInstanceAssetPath));
	FHttpSmokeResult LegacyApplyResult;
	TestTrue(TEXT("Material instance folder legacy apply request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, LegacyApplyBody, LegacyApplyResult));
	TestEqual(TEXT("Material instance folder legacy apply status code"), LegacyApplyResult.StatusCode, 400);
	TSharedPtr<FJsonObject> LegacyApplyJson;
	TestTrue(TEXT("Material instance folder legacy apply parses JSON"), ParseJson(LegacyApplyResult.Body, LegacyApplyJson));
	const TSharedPtr<FJsonObject>* LegacyApplyData = nullptr;
	TestTrue(TEXT("Material instance folder legacy apply contains data object"), LegacyApplyJson.IsValid() && LegacyApplyJson->TryGetObjectField(TEXT("data"), LegacyApplyData) && LegacyApplyData && LegacyApplyData->IsValid());
	if (LegacyApplyData && LegacyApplyData->IsValid())
	{
		TestEqual(TEXT("Material instance folder legacy rejection schema"), (*LegacyApplyData)->GetStringField(TEXT("schema")), FString(TEXT("uai.legacy_rejection.v1")));
		TestEqual(TEXT("Material instance folder legacy rejection error code"), (*LegacyApplyData)->GetStringField(TEXT("error_code")), FString(TEXT("legacy_properties_file_not_supported")));
		TestEqual(TEXT("Material instance folder legacy rejection surface"), (*LegacyApplyData)->GetStringField(TEXT("legacy_surface")), FString(TEXT("properties.json")));
	}
	TestTrue(TEXT("Material instance folder legacy properties.json removed"), IFileManager::Get().Delete(*LegacyPropertiesPath, false, true));

	const FString OverridesPath = FPaths::Combine(FolderPath, TEXT("parameters"), TEXT("overrides.json"));
	FString OverridesText;
	TestTrue(TEXT("Material instance folder overrides.json exists"), FFileHelper::LoadFileToString(OverridesText, *OverridesPath));

	TSharedPtr<FJsonObject> OverridesJson;
	TestTrue(TEXT("Material instance folder overrides.json parses"), ParseJson(OverridesText, OverridesJson));
	const TArray<TSharedPtr<FJsonValue>>* ParametersArray = nullptr;
	TestTrue(TEXT("Material instance folder overrides.json has parameters"), OverridesJson.IsValid() && OverridesJson->TryGetArrayField(TEXT("parameters"), ParametersArray) && ParametersArray);
	if (ParametersArray)
	{
		bool bUpdated = false;
		for (const TSharedPtr<FJsonValue>& ParameterValue : *ParametersArray)
		{
			const TSharedPtr<FJsonObject>* ParameterObj = nullptr;
			if (!ParameterValue.IsValid() || !ParameterValue->TryGetObject(ParameterObj) || !ParameterObj || !ParameterObj->IsValid())
			{
				continue;
			}

			if ((*ParameterObj)->GetStringField(TEXT("name")) == TEXT("BillboardColor"))
			{
				TSharedPtr<FJsonObject> ValueObj = MakeShared<FJsonObject>();
				ValueObj->SetNumberField(TEXT("r"), 0.05);
				ValueObj->SetNumberField(TEXT("g"), 0.95);
				ValueObj->SetNumberField(TEXT("b"), 0.35);
				ValueObj->SetNumberField(TEXT("a"), 1.0);
				(*ParameterObj)->SetObjectField(TEXT("value"), ValueObj);
				bUpdated = true;
				break;
			}
		}
		TestTrue(TEXT("Material instance folder overrides.json updated BillboardColor"), bUpdated);
	}

	{
		FString UpdatedOverridesText;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&UpdatedOverridesText);
		TestTrue(TEXT("Material instance folder overrides.json serializes"), OverridesJson.IsValid() && FJsonSerializer::Serialize(OverridesJson.ToSharedRef(), Writer));
		TestTrue(TEXT("Material instance folder overrides.json saves"), FFileHelper::SaveStringToFile(UpdatedOverridesText, *OverridesPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	}

	const FString ApplyBody = MakeExecRequestBody(
		TEXT("smoke-material-instance-folder-apply-001"),
		TEXT("material_instance_apply_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"create_if_missing\":false,\"clear_existing_overrides\":true,\"compile_after_apply\":true,\"save_after_apply\":false}"), *MaterialInstanceAssetPath));
	FHttpSmokeResult ApplyResult;
	TestTrue(TEXT("Material instance folder apply request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ApplyBody, ApplyResult));
	TestEqual(TEXT("Material instance folder apply status code"), ApplyResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ApplyJson;
	TestTrue(TEXT("Material instance folder apply parses JSON"), ParseJson(ApplyResult.Body, ApplyJson));
	TestTrue(TEXT("Material instance folder apply root ok=true"), ApplyJson.IsValid() && ApplyJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* ApplyData = nullptr;
	TestTrue(TEXT("Material instance folder apply contains data object"), ApplyJson.IsValid() && ApplyJson->TryGetObjectField(TEXT("data"), ApplyData) && ApplyData && ApplyData->IsValid());
	if (ApplyData && ApplyData->IsValid())
	{
		TestTrue(TEXT("Material instance folder apply applied parent"), (*ApplyData)->GetIntegerField(TEXT("parent_applied")) >= 1);
		TestTrue(TEXT("Material instance folder apply cleared overrides"), (*ApplyData)->GetIntegerField(TEXT("overrides_cleared")) >= 1);
		TestTrue(TEXT("Material instance folder apply applied parameters"), (*ApplyData)->GetIntegerField(TEXT("parameters_applied")) >= 1);
	}

	const FString GetInfoBody = MakeExecRequestBody(
		TEXT("smoke-material-instance-folder-info-001"),
		TEXT("material_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *MaterialInstanceAssetPath));
	FHttpSmokeResult GetInfoResult;
	TestTrue(TEXT("Material instance folder get_info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoBody, GetInfoResult));
	TestEqual(TEXT("Material instance folder get_info status code"), GetInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoJson;
	TestTrue(TEXT("Material instance folder get_info parses JSON"), ParseJson(GetInfoResult.Body, GetInfoJson));
	TestTrue(TEXT("Material instance folder get_info root ok=true"), GetInfoJson.IsValid() && GetInfoJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* GetInfoData = nullptr;
	TestTrue(TEXT("Material instance folder get_info contains data object"), GetInfoJson.IsValid() && GetInfoJson->TryGetObjectField(TEXT("data"), GetInfoData) && GetInfoData && GetInfoData->IsValid());
	if (GetInfoData && GetInfoData->IsValid())
	{
		TestTrue(TEXT("Material instance folder get_info reports instance"), (*GetInfoData)->GetBoolField(TEXT("is_material_instance")));
		const FString ExpectedParentObjectPath = FString::Printf(TEXT("%s.%s"), *ParentMaterialPath, *FPackageName::GetLongPackageAssetName(ParentMaterialPath));
		TestEqual(TEXT("Material instance folder get_info parent path"), (*GetInfoData)->GetStringField(TEXT("parent_path")), ExpectedParentObjectPath);

		const TArray<TSharedPtr<FJsonValue>>* VectorParameters = nullptr;
		TestTrue(TEXT("Material instance folder get_info has vector parameters"), (*GetInfoData)->TryGetArrayField(TEXT("vector_parameters"), VectorParameters) && VectorParameters);
		if (VectorParameters)
		{
			bool bFoundBillboardColor = false;
			for (const TSharedPtr<FJsonValue>& ParameterValue : *VectorParameters)
			{
				const TSharedPtr<FJsonObject>* ParameterObj = nullptr;
				if (!ParameterValue.IsValid() || !ParameterValue->TryGetObject(ParameterObj) || !ParameterObj || !ParameterObj->IsValid())
				{
					continue;
				}

				if ((*ParameterObj)->GetStringField(TEXT("name")) == TEXT("BillboardColor"))
				{
					const TSharedPtr<FJsonObject>* ValueObj = nullptr;
					TestTrue(TEXT("Material instance folder BillboardColor has value object"), (*ParameterObj)->TryGetObjectField(TEXT("value"), ValueObj) && ValueObj && ValueObj->IsValid());
					if (ValueObj && ValueObj->IsValid())
					{
						TestTrue(TEXT("Material instance folder BillboardColor.r matches"), FMath::IsNearlyEqual((*ValueObj)->GetNumberField(TEXT("r")), 0.05, 0.001));
						TestTrue(TEXT("Material instance folder BillboardColor.g matches"), FMath::IsNearlyEqual((*ValueObj)->GetNumberField(TEXT("g")), 0.95, 0.001));
						TestTrue(TEXT("Material instance folder BillboardColor.b matches"), FMath::IsNearlyEqual((*ValueObj)->GetNumberField(TEXT("b")), 0.35, 0.001));
					}
					bFoundBillboardColor = true;
					break;
				}
			}
			TestTrue(TEXT("Material instance folder get_info contains BillboardColor"), bFoundBillboardColor);
		}
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceMaterialFunctionFolderSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.MaterialFunctionFolderWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceMaterialFunctionFolderSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString FunctionAssetPath = MakeAutomationAssetPath(TEXT("MFOLDER"));
	FString RelativeFolder = FunctionAssetPath;
	while (RelativeFolder.StartsWith(TEXT("/")))
	{
		RelativeFolder.RightChopInline(1);
	}
	const FString FolderPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("MaterialFunction"), RelativeFolder));
	IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	IFileManager::Get().MakeDirectory(*FPaths::Combine(FolderPath, TEXT("graphs")), true);
	IFileManager::Get().MakeDirectory(*FPaths::Combine(FolderPath, TEXT("validation")), true);

	auto SaveUtf8Json = [this](const FString& FilePath, const FString& JsonText) -> bool
	{
		const bool bOk = FFileHelper::SaveStringToFile(JsonText, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		TestTrue(FString::Printf(TEXT("Save JSON file: %s"), *FilePath), bOk);
		return bOk;
	};

	if (!SaveUtf8Json(
		FPaths::Combine(FolderPath, TEXT("asset.json")),
		FString::Printf(
			TEXT("{\n  \"format_version\": 1,\n  \"asset_kind\": \"material_function\",\n  \"asset_path\": \"%s\",\n  \"asset_name\": \"%s\",\n  \"profile_version\": 1,\n  \"function_subkind\": \"material_function\"\n}\n"),
			*FunctionAssetPath,
			*FPackageName::GetLongPackageAssetName(FunctionAssetPath))))
	{
		return false;
	}

	if (!SaveUtf8Json(
		FPaths::Combine(FolderPath, TEXT("function.json")),
		TEXT("{\n  \"description\": \"Smoke material function\",\n  \"expose_to_library\": true\n}\n")))
	{
		return false;
	}

	if (!SaveUtf8Json(
		FPaths::Combine(FolderPath, TEXT("graphs"), TEXT("MaterialFunctionGraph.json")),
		TEXT("{\n  \"graph\": {\n    \"name\": \"MaterialFunctionGraph\",\n    \"graph_kind\": \"material_function_graph\"\n  },\n  \"expressions\": [\n    {\n      \"id\": \"const_color\",\n      \"expression_class\": \"/Script/Engine.MaterialExpressionConstant3Vector\",\n      \"pos\": { \"x\": -240, \"y\": 0 },\n      \"properties\": [\n        { \"property_name\": \"Constant\", \"value_text\": \"(R=0.300000,G=0.600000,B=0.900000,A=1.000000)\" },\n        { \"property_name\": \"Desc\", \"value_text\": \"SmokeColor\" }\n      ]\n    },\n    {\n      \"id\": \"func_output\",\n      \"expression_class\": \"/Script/Engine.MaterialExpressionFunctionOutput\",\n      \"pos\": { \"x\": 40, \"y\": 0 },\n      \"properties\": [\n        { \"property_name\": \"OutputName\", \"value_text\": \"ColorOut\" },\n        { \"property_name\": \"SortPriority\", \"value_text\": \"0\" },\n        { \"property_name\": \"Description\", \"value_text\": \"Smoke output\" }\n      ]\n    }\n  ],\n  \"edges\": [\n    {\n      \"from\": { \"node\": \"const_color\", \"output\": \"\" },\n      \"to\": { \"node\": \"func_output\", \"input\": \"\" }\n    }\n  ]\n}\n")))
	{
		return false;
	}

	if (!SaveUtf8Json(
		FPaths::Combine(FolderPath, TEXT("validation"), TEXT("checks.json")),
		TEXT("{\n  \"checks\": [\n    { \"kind\": \"graph_roundtrip\" }\n  ]\n}\n")))
	{
		return false;
	}

	const FString FunctionGraphJsonPath = FPaths::Combine(FolderPath, TEXT("graphs"), TEXT("MaterialFunctionGraph.json"));
	TSharedPtr<FJsonObject> FunctionGraphAuthoringJson;
	if (!LoadSmokeJsonFile(this, FunctionGraphJsonPath, FunctionGraphAuthoringJson, TEXT("Material function folder graph authoring json")))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* FunctionAuthoringExpressions = nullptr;
	if (!FunctionGraphAuthoringJson->TryGetArrayField(TEXT("expressions"), FunctionAuthoringExpressions) || !FunctionAuthoringExpressions)
	{
		AddError(TEXT("Material function folder graph authoring expressions missing"));
		return false;
	}
	for (const TSharedPtr<FJsonValue>& ExpressionValue : *FunctionAuthoringExpressions)
	{
		const TSharedPtr<FJsonObject>* ExpressionObj = nullptr;
		if (!ExpressionValue.IsValid() || !ExpressionValue->TryGetObject(ExpressionObj) || !ExpressionObj || !ExpressionObj->IsValid())
		{
			continue;
		}
		FString ExpressionId;
		if (!(*ExpressionObj)->TryGetStringField(TEXT("id"), ExpressionId) || ExpressionId.IsEmpty())
		{
			AddError(TEXT("Material function expression id missing before details migration"));
			return false;
		}
		if (!SmokeMoveInlinePropertiesToGroupedDetails(
			this,
			FunctionGraphJsonPath,
			*ExpressionObj,
			TEXT("material_function_expression"),
			ExpressionId,
			TEXT("expressions"),
			TEXT("material_function_apply_folder")))
		{
			return false;
		}
	}
	if (!SaveSmokeJsonFile(this, FunctionGraphJsonPath, FunctionGraphAuthoringJson, TEXT("Material function folder graph grouped authoring json")))
	{
		return false;
	}

	const FString ApplyBody = MakeExecRequestBody(
		TEXT("smoke-material-function-folder-apply-001"),
		TEXT("material_function_apply_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"create_if_missing\":true,\"update_after_apply\":true,\"save_after_apply\":false}"), *FunctionAssetPath));
	FHttpSmokeResult ApplyResult;
	TestTrue(TEXT("Material function folder apply request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ApplyBody, ApplyResult));
	TestEqual(TEXT("Material function folder apply status code"), ApplyResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ApplyJson;
	TestTrue(TEXT("Material function folder apply parses JSON"), ParseJson(ApplyResult.Body, ApplyJson));
	TestTrue(TEXT("Material function folder apply root ok=true"), ApplyJson.IsValid() && ApplyJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* ApplyData = nullptr;
	TestTrue(TEXT("Material function folder apply contains data object"), ApplyJson.IsValid() && ApplyJson->TryGetObjectField(TEXT("data"), ApplyData) && ApplyData && ApplyData->IsValid());
	if (ApplyData && ApplyData->IsValid())
	{
		TestTrue(TEXT("Material function folder apply created expressions"), (*ApplyData)->GetIntegerField(TEXT("expressions_created")) >= 2);
		TestTrue(TEXT("Material function folder apply created edges"), (*ApplyData)->GetIntegerField(TEXT("edges_created")) >= 1);
	}

	const FString ExportBody = MakeExecRequestBody(
		TEXT("smoke-material-function-folder-export-001"),
		TEXT("material_function_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clean_output_dir\":true,\"include_validation\":true}"), *FunctionAssetPath));
	FHttpSmokeResult ExportResult;
	TestTrue(TEXT("Material function folder export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ExportBody, ExportResult));
	TestEqual(TEXT("Material function folder export status code"), ExportResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ExportJson;
	TestTrue(TEXT("Material function folder export parses JSON"), ParseJson(ExportResult.Body, ExportJson));
	TestTrue(TEXT("Material function folder export root ok=true"), ExportJson.IsValid() && ExportJson->GetBoolField(TEXT("ok")));

	const FString GraphJsonPath = FPaths::Combine(FolderPath, TEXT("graphs"), TEXT("MaterialFunctionGraph.json"));
	FString ExportedGraphText;
	TestTrue(TEXT("Material function folder exported graph exists"), FFileHelper::LoadFileToString(ExportedGraphText, *GraphJsonPath));
	TestFalse(TEXT("Material function folder exported graph omits inline properties"), ExportedGraphText.Contains(TEXT("\"properties\"")));

	TSharedPtr<FJsonObject> ExportedGraphJson;
	TestTrue(TEXT("Material function folder exported graph parses"), ParseJson(ExportedGraphText, ExportedGraphJson));
	if (ExportedGraphJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Expressions = nullptr;
		TestTrue(TEXT("Material function folder exported graph has expressions"), ExportedGraphJson->TryGetArrayField(TEXT("expressions"), Expressions) && Expressions);
		if (Expressions)
		{
			TestTrue(TEXT("Material function folder exported expression count >= 2"), Expressions->Num() >= 2);
			bool bFoundFunctionExpressionDetails = false;
			for (const TSharedPtr<FJsonValue>& ExpressionValue : *Expressions)
			{
				const TSharedPtr<FJsonObject>* ExpressionObj = nullptr;
				if (!ExpressionValue.IsValid() || !ExpressionValue->TryGetObject(ExpressionObj) || !ExpressionObj || !ExpressionObj->IsValid())
				{
					continue;
				}

				TSharedPtr<FJsonObject> ExpressionDetailsJson;
				if (SmokeLoadGroupedDetailsForRef(this, GraphJsonPath, *ExpressionObj, TEXT("details"), TEXT("material_function_expression"), ExpressionDetailsJson))
				{
					bFoundFunctionExpressionDetails = bFoundFunctionExpressionDetails
						|| SmokeDetailsPropertiesContain(ExpressionDetailsJson, TEXT("Constant"))
						|| SmokeDetailsPropertiesContain(ExpressionDetailsJson, TEXT("OutputName"));
				}
			}
			TestTrue(TEXT("Material function folder expression details grouped"), bFoundFunctionExpressionDetails);
		}
	}

	if (ExportedGraphJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Expressions = nullptr;
		if (ExportedGraphJson->TryGetArrayField(TEXT("expressions"), Expressions) && Expressions && Expressions->Num() > 0)
		{
			const TSharedPtr<FJsonObject>* FirstExpressionObj = nullptr;
			if ((*Expressions)[0].IsValid() && (*Expressions)[0]->TryGetObject(FirstExpressionObj) && FirstExpressionObj && FirstExpressionObj->IsValid())
			{
				TArray<TSharedPtr<FJsonValue>> LegacyProperties;
				LegacyProperties.Add(MakeShared<FJsonValueObject>(SmokeMakeDetailsProperty(TEXT("Desc"), TEXT("LegacySmoke"))));
				(*FirstExpressionObj)->SetArrayField(TEXT("properties"), LegacyProperties);
				TestTrue(TEXT("Material function folder save legacy inline properties"), SaveSmokeJsonFile(this, GraphJsonPath, ExportedGraphJson, TEXT("Material function legacy graph json")));

				const FString LegacyApplyBody = MakeExecRequestBody(
					TEXT("smoke-material-function-folder-legacy-properties-001"),
					TEXT("material_function_apply_folder"),
					FString::Printf(TEXT("{\"asset_path\":\"%s\",\"update_after_apply\":false,\"save_after_apply\":false}"), *FunctionAssetPath));
				FHttpSmokeResult LegacyApplyResult;
				TestTrue(TEXT("Material function folder legacy properties request reached server"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, LegacyApplyBody, LegacyApplyResult));
				TestEqual(TEXT("Material function folder legacy properties are rejected"), LegacyApplyResult.StatusCode, 400);

				TSharedPtr<FJsonObject> LegacyApplyJson;
				TestTrue(TEXT("Material function folder legacy properties response parses JSON"), ParseJson(LegacyApplyResult.Body, LegacyApplyJson));
				const TSharedPtr<FJsonObject>* LegacyApplyData = nullptr;
				TestTrue(TEXT("Material function folder legacy rejection data exists"), LegacyApplyJson.IsValid() && LegacyApplyJson->TryGetObjectField(TEXT("data"), LegacyApplyData) && LegacyApplyData && LegacyApplyData->IsValid());
				if (LegacyApplyData && LegacyApplyData->IsValid())
				{
					TestEqual(TEXT("Material function folder legacy rejection schema"), (*LegacyApplyData)->GetStringField(TEXT("schema")), FString(TEXT("uai.legacy_rejection.v1")));
					TestEqual(TEXT("Material function folder legacy rejection error code"), (*LegacyApplyData)->GetStringField(TEXT("error_code")), FString(TEXT("legacy_inline_properties_not_supported")));
					TestEqual(TEXT("Material function folder legacy rejection surface"), (*LegacyApplyData)->GetStringField(TEXT("legacy_surface")), FString(TEXT("properties[]")));
				}
			}
		}
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceMaterialExpressionLifecycleSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.MaterialExpressionLifecycleWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceMaterialExpressionLifecycleSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString MaterialAssetPath = MakeAutomationAssetPath(TEXT("MATLife"));

	const FString CreateBody = MakeExecRequestBody(
		TEXT("smoke-material-life-create-001"),
		TEXT("material_create"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"compile_after_create\":false,\"save_after_create\":false}"), *MaterialAssetPath));
	FHttpSmokeResult CreateResult;
	TestTrue(TEXT("Material lifecycle create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateBody, CreateResult));
	TestEqual(TEXT("Material lifecycle create status code"), CreateResult.StatusCode, 200);

	const FString AddExpressionBody = MakeExecRequestBody(
		TEXT("smoke-material-life-add-expression-001"),
		TEXT("material_add_expression"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"expression_class\":\"/Script/Engine.MaterialExpressionConstant\",\"node_pos_x\":32,\"node_pos_y\":64,\"compile_after_add\":false,\"save_after_add\":false}"), *MaterialAssetPath));
	FHttpSmokeResult AddExpressionResult;
	TestTrue(TEXT("Material lifecycle add_expression request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddExpressionBody, AddExpressionResult));
	TestEqual(TEXT("Material lifecycle add_expression status code"), AddExpressionResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddExpressionJson;
	TestTrue(TEXT("Material lifecycle add_expression response parses as JSON"), ParseJson(AddExpressionResult.Body, AddExpressionJson));
	TestTrue(TEXT("Material lifecycle add_expression root ok=true"), AddExpressionJson.IsValid() && AddExpressionJson->GetBoolField(TEXT("ok")));

	FString ExpressionGuid;
	const TSharedPtr<FJsonObject>* AddExpressionData = nullptr;
	TestTrue(TEXT("Material lifecycle add_expression contains data object"), AddExpressionJson.IsValid() && AddExpressionJson->TryGetObjectField(TEXT("data"), AddExpressionData) && AddExpressionData && AddExpressionData->IsValid());
	if (AddExpressionData && AddExpressionData->IsValid())
	{
		const TSharedPtr<FJsonObject>* ExpressionObject = nullptr;
		TestTrue(TEXT("Material lifecycle add_expression contains expression object"), (*AddExpressionData)->TryGetObjectField(TEXT("expression"), ExpressionObject) && ExpressionObject && ExpressionObject->IsValid());
		if (ExpressionObject && ExpressionObject->IsValid())
		{
			ExpressionGuid = (*ExpressionObject)->GetStringField(TEXT("guid"));
			TestTrue(TEXT("Material lifecycle expression guid non-empty"), !ExpressionGuid.IsEmpty());
		}
	}

	const FString SetExpressionPropertyBody = MakeExecRequestBody(
		TEXT("smoke-material-life-set-expression-001"),
		TEXT("material_set_expression_property"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"expression_guid\":\"%s\",\"property_name\":\"R\",\"value_text\":\"0.25\",\"compile_after_set\":false,\"save_after_set\":false}"), *MaterialAssetPath, *ExpressionGuid));
	FHttpSmokeResult SetExpressionPropertyResult;
	TestTrue(TEXT("Material lifecycle set_expression_property request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetExpressionPropertyBody, SetExpressionPropertyResult));
	TestEqual(TEXT("Material lifecycle set_expression_property status code"), SetExpressionPropertyResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SetExpressionPropertyJson;
	TestTrue(TEXT("Material lifecycle set_expression_property response parses as JSON"), ParseJson(SetExpressionPropertyResult.Body, SetExpressionPropertyJson));
	TestTrue(TEXT("Material lifecycle set_expression_property root ok=true"), SetExpressionPropertyJson.IsValid() && SetExpressionPropertyJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* SetExpressionPropertyData = nullptr;
	TestTrue(TEXT("Material lifecycle set_expression_property contains data object"), SetExpressionPropertyJson.IsValid() && SetExpressionPropertyJson->TryGetObjectField(TEXT("data"), SetExpressionPropertyData) && SetExpressionPropertyData && SetExpressionPropertyData->IsValid());
	if (SetExpressionPropertyData && SetExpressionPropertyData->IsValid())
	{
		TestEqual(TEXT("Material lifecycle set_expression_property guid"), (*SetExpressionPropertyData)->GetStringField(TEXT("expression_guid")), ExpressionGuid);
		TestEqual(TEXT("Material lifecycle set_expression_property property name"), (*SetExpressionPropertyData)->GetStringField(TEXT("property_name")), FString(TEXT("R")));
	}

	const FString ConnectBody = MakeExecRequestBody(
		TEXT("smoke-material-life-connect-001"),
		TEXT("material_connect_expression_to_property"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"expression_guid\":\"%s\",\"material_property\":\"Metallic\",\"compile_after_connect\":false,\"save_after_connect\":false}"), *MaterialAssetPath, *ExpressionGuid));
	FHttpSmokeResult ConnectResult;
	TestTrue(TEXT("Material lifecycle connect_expression_to_property request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ConnectBody, ConnectResult));
	TestEqual(TEXT("Material lifecycle connect_expression_to_property status code"), ConnectResult.StatusCode, 200);

	const FString CompileBody = MakeExecRequestBody(
		TEXT("smoke-material-life-compile-001"),
		TEXT("material_compile"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_messages\":true,\"max_messages\":16,\"save_after_compile\":false}"), *MaterialAssetPath));
	FHttpSmokeResult CompileResult;
	TestTrue(TEXT("Material lifecycle compile request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CompileBody, CompileResult));
	TestEqual(TEXT("Material lifecycle compile status code"), CompileResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CompileJson;
	TestTrue(TEXT("Material lifecycle compile response parses as JSON"), ParseJson(CompileResult.Body, CompileJson));
	TestTrue(TEXT("Material lifecycle compile root ok=true"), CompileJson.IsValid() && CompileJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* CompileData = nullptr;
	TestTrue(TEXT("Material lifecycle compile contains data object"), CompileJson.IsValid() && CompileJson->TryGetObjectField(TEXT("data"), CompileData) && CompileData && CompileData->IsValid());
	if (CompileData && CompileData->IsValid())
	{
		TestFalse(TEXT("Material lifecycle compile reports no error"), (*CompileData)->GetBoolField(TEXT("has_error")));
	}

	const FString DeleteExpressionBody = MakeExecRequestBody(
		TEXT("smoke-material-life-delete-001"),
		TEXT("material_delete_expression"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"expression_guid\":\"%s\",\"compile_after_delete\":false,\"save_after_delete\":false}"), *MaterialAssetPath, *ExpressionGuid));
	const FString MaterialObjectPath = FString::Printf(TEXT("%s.%s"), *MaterialAssetPath, *FPackageName::GetShortName(MaterialAssetPath));
	UMaterial* LifecycleMaterial = LoadObject<UMaterial>(nullptr, *MaterialObjectPath);
	TestNotNull(TEXT("Material lifecycle material loaded for rooted delete regression"), LifecycleMaterial);
	UMaterialExpression* ExpressionToRootBeforeDelete = nullptr;
	if (LifecycleMaterial)
	{
		FGuid ParsedExpressionGuid;
		FGuid::Parse(ExpressionGuid, ParsedExpressionGuid);
		for (UMaterialExpression* Expression : LifecycleMaterial->GetExpressions())
		{
			if (Expression && Expression->MaterialExpressionGuid == ParsedExpressionGuid)
			{
				ExpressionToRootBeforeDelete = Expression;
				break;
			}
		}
	}
	TestNotNull(TEXT("Material lifecycle expression found for rooted delete regression"), ExpressionToRootBeforeDelete);
	if (ExpressionToRootBeforeDelete)
	{
		ExpressionToRootBeforeDelete->AddToRoot();
		TestTrue(TEXT("Material lifecycle expression rooted before delete"), ExpressionToRootBeforeDelete->IsRooted());
	}

	FHttpSmokeResult DeleteExpressionResult;
	TestTrue(TEXT("Material lifecycle delete_expression request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DeleteExpressionBody, DeleteExpressionResult));
	TestEqual(TEXT("Material lifecycle delete_expression status code"), DeleteExpressionResult.StatusCode, 200);
	TSharedPtr<FJsonObject> DeleteExpressionJson;
	TestTrue(TEXT("Material lifecycle delete_expression response parses as JSON"), ParseJson(DeleteExpressionResult.Body, DeleteExpressionJson));
	TestTrue(TEXT("Material lifecycle delete_expression root ok=true"), DeleteExpressionJson.IsValid() && DeleteExpressionJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* DeleteExpressionData = nullptr;
	TestTrue(TEXT("Material lifecycle delete_expression contains data object"), DeleteExpressionJson.IsValid() && DeleteExpressionJson->TryGetObjectField(TEXT("data"), DeleteExpressionData) && DeleteExpressionData && DeleteExpressionData->IsValid());
	if (DeleteExpressionData && DeleteExpressionData->IsValid())
	{
		TestTrue(TEXT("Material lifecycle delete_expression saw rooted expression"), (*DeleteExpressionData)->GetBoolField(TEXT("was_rooted_before_delete")));
		TestTrue(TEXT("Material lifecycle delete_expression removed root before delete"), (*DeleteExpressionData)->GetBoolField(TEXT("removed_root_before_delete")));
	}

	const FString ListExpressionsBody = MakeExecRequestBody(
		TEXT("smoke-material-life-list-001"),
		TEXT("material_list_expressions"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *MaterialAssetPath));
	FHttpSmokeResult ListExpressionsResult;
	TestTrue(TEXT("Material lifecycle list_expressions request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListExpressionsBody, ListExpressionsResult));
	TestEqual(TEXT("Material lifecycle list_expressions status code"), ListExpressionsResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ListExpressionsJson;
	TestTrue(TEXT("Material lifecycle list_expressions response parses as JSON"), ParseJson(ListExpressionsResult.Body, ListExpressionsJson));
	TestTrue(TEXT("Material lifecycle list_expressions root ok=true"), ListExpressionsJson.IsValid() && ListExpressionsJson->GetBoolField(TEXT("ok")));

	bool bFoundDeletedExpression = false;
	const TSharedPtr<FJsonObject>* ListExpressionsData = nullptr;
	TestTrue(TEXT("Material lifecycle list_expressions contains data object"), ListExpressionsJson.IsValid() && ListExpressionsJson->TryGetObjectField(TEXT("data"), ListExpressionsData) && ListExpressionsData && ListExpressionsData->IsValid());
	if (ListExpressionsData && ListExpressionsData->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Expressions = nullptr;
		TestTrue(TEXT("Material lifecycle list_expressions contains expressions array"), (*ListExpressionsData)->TryGetArrayField(TEXT("expressions"), Expressions) && Expressions != nullptr);
		if (Expressions)
		{
			for (const TSharedPtr<FJsonValue>& ExpressionValue : *Expressions)
			{
				const TSharedPtr<FJsonObject>* ExpressionObject = nullptr;
				if (!ExpressionValue.IsValid() || !ExpressionValue->TryGetObject(ExpressionObject) || !ExpressionObject || !ExpressionObject->IsValid())
				{
					continue;
				}

				if ((*ExpressionObject)->GetStringField(TEXT("guid")) == ExpressionGuid)
				{
					bFoundDeletedExpression = true;
					break;
				}
			}
		}
	}
	TestFalse(TEXT("Material lifecycle deleted expression no longer present"), bFoundDeletedExpression);

	const FString CompileAfterDeleteBody = MakeExecRequestBody(
		TEXT("smoke-material-life-compile-002"),
		TEXT("material_compile"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_messages\":true,\"max_messages\":16,\"save_after_compile\":false}"), *MaterialAssetPath));
	FHttpSmokeResult CompileAfterDeleteResult;
	TestTrue(TEXT("Material lifecycle compile after delete request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CompileAfterDeleteBody, CompileAfterDeleteResult));
	TestEqual(TEXT("Material lifecycle compile after delete status code"), CompileAfterDeleteResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CompileAfterDeleteJson;
	TestTrue(TEXT("Material lifecycle compile after delete response parses as JSON"), ParseJson(CompileAfterDeleteResult.Body, CompileAfterDeleteJson));
	TestTrue(TEXT("Material lifecycle compile after delete root ok=true"), CompileAfterDeleteJson.IsValid() && CompileAfterDeleteJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* CompileAfterDeleteData = nullptr;
	TestTrue(TEXT("Material lifecycle compile after delete contains data object"), CompileAfterDeleteJson.IsValid() && CompileAfterDeleteJson->TryGetObjectField(TEXT("data"), CompileAfterDeleteData) && CompileAfterDeleteData && CompileAfterDeleteData->IsValid());
	if (CompileAfterDeleteData && CompileAfterDeleteData->IsValid())
	{
		TestFalse(TEXT("Material lifecycle compile after delete reports no error"), (*CompileAfterDeleteData)->GetBoolField(TEXT("has_error")));
	}

	return !HasAnyErrors();
}

#endif
