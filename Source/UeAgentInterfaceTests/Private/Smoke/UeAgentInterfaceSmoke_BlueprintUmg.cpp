#if WITH_DEV_AUTOMATION_TESTS

#include "UeAgentSmokeTestSharedIncludes.inl"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceBlueprintSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.BlueprintWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceBlueprintSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString BlueprintAssetPath = MakeAutomationAssetPath(TEXT("BP"));
	const FString VariableName = TEXT("SmokeFlag");

	const FString CreateBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-create-001"),
		TEXT("blueprint_create"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"parent_class\":\"/Script/Engine.Actor\",\"compile_after_create\":false,\"open_editor\":false,\"save_after_create\":false}"),
			*BlueprintAssetPath));
	FHttpSmokeResult CreateResult;
	TestTrue(TEXT("Blueprint create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateBody, CreateResult));
	TestEqual(TEXT("Blueprint create status code"), CreateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CreateJson;
	TestTrue(TEXT("Blueprint create response parses as JSON"), ParseJson(CreateResult.Body, CreateJson));
	TestTrue(TEXT("Blueprint create root ok=true"), CreateJson.IsValid() && CreateJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* CreateData = nullptr;
	TestTrue(TEXT("Blueprint create contains data object"), CreateJson.IsValid() && CreateJson->TryGetObjectField(TEXT("data"), CreateData) && CreateData && CreateData->IsValid());
	if (CreateData && CreateData->IsValid())
	{
		TestEqual(TEXT("Blueprint create asset path"), (*CreateData)->GetStringField(TEXT("asset_path")), BlueprintAssetPath);
		TestFalse(TEXT("Blueprint create not saved"), (*CreateData)->GetBoolField(TEXT("saved")));
	}

	const FString GetInfoBeforeBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-info-before-001"),
		TEXT("blueprint_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *BlueprintAssetPath));
	FHttpSmokeResult GetInfoBeforeResult;
	TestTrue(TEXT("Blueprint get_info before request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoBeforeBody, GetInfoBeforeResult));
	TestEqual(TEXT("Blueprint get_info before status code"), GetInfoBeforeResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoBeforeJson;
	TestTrue(TEXT("Blueprint get_info before response parses as JSON"), ParseJson(GetInfoBeforeResult.Body, GetInfoBeforeJson));
	TestTrue(TEXT("Blueprint get_info before root ok=true"), GetInfoBeforeJson.IsValid() && GetInfoBeforeJson->GetBoolField(TEXT("ok")));

	int32 VariableCountBefore = 0;
	const TSharedPtr<FJsonObject>* GetInfoBeforeData = nullptr;
	TestTrue(TEXT("Blueprint get_info before contains data object"), GetInfoBeforeJson.IsValid() && GetInfoBeforeJson->TryGetObjectField(TEXT("data"), GetInfoBeforeData) && GetInfoBeforeData && GetInfoBeforeData->IsValid());
	if (GetInfoBeforeData && GetInfoBeforeData->IsValid())
	{
		VariableCountBefore = static_cast<int32>((*GetInfoBeforeData)->GetIntegerField(TEXT("new_variable_count")));
		TestEqual(TEXT("Blueprint get_info parent class"), (*GetInfoBeforeData)->GetStringField(TEXT("parent_class")), FString(TEXT("/Script/Engine.Actor")));
	}

	const FString AddVariableBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-add-variable-001"),
		TEXT("blueprint_add_variable"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"variable_name\":\"%s\",\"pin_category\":\"bool\",\"compile_after_add\":false,\"save_after_add\":false}"),
			*BlueprintAssetPath,
			*VariableName));
	FHttpSmokeResult AddVariableResult;
	TestTrue(TEXT("Blueprint add_variable request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddVariableBody, AddVariableResult));
	TestEqual(TEXT("Blueprint add_variable status code"), AddVariableResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddVariableJson;
	TestTrue(TEXT("Blueprint add_variable response parses as JSON"), ParseJson(AddVariableResult.Body, AddVariableJson));
	TestTrue(TEXT("Blueprint add_variable root ok=true"), AddVariableJson.IsValid() && AddVariableJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* AddVariableData = nullptr;
	TestTrue(TEXT("Blueprint add_variable contains data object"), AddVariableJson.IsValid() && AddVariableJson->TryGetObjectField(TEXT("data"), AddVariableData) && AddVariableData && AddVariableData->IsValid());
	if (AddVariableData && AddVariableData->IsValid())
	{
		TestEqual(TEXT("Blueprint add_variable variable name"), (*AddVariableData)->GetStringField(TEXT("variable_name")), VariableName);
		TestEqual(TEXT("Blueprint add_variable pin category"), (*AddVariableData)->GetStringField(TEXT("pin_category")), FString(TEXT("bool")));
		TestFalse(TEXT("Blueprint add_variable not saved"), (*AddVariableData)->GetBoolField(TEXT("saved")));
	}

	auto AddVariableByJsonTail = [this, &ServerScope, &BlueprintAssetPath](
		const FString& RequestId,
		const FString& InVariableName,
		const FString& JsonTail) -> bool
	{
		const FString Body = MakeExecRequestBody(
			RequestId,
			TEXT("blueprint_add_variable"),
			FString::Printf(
				TEXT("{\"asset_path\":\"%s\",\"variable_name\":\"%s\",%s,\"compile_after_add\":false,\"save_after_add\":false}"),
				*BlueprintAssetPath,
				*InVariableName,
				*JsonTail));
		FHttpSmokeResult Result;
		const bool bRequestOk = ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, Body, Result);
		TestTrue(FString::Printf(TEXT("Blueprint add_variable %s request succeeded"), *InVariableName), bRequestOk);
		TestEqual(FString::Printf(TEXT("Blueprint add_variable %s status code"), *InVariableName), Result.StatusCode, 200);

		TSharedPtr<FJsonObject> Json;
		TestTrue(FString::Printf(TEXT("Blueprint add_variable %s response parses"), *InVariableName), ParseJson(Result.Body, Json));
		TestTrue(FString::Printf(TEXT("Blueprint add_variable %s root ok=true"), *InVariableName), Json.IsValid() && Json->GetBoolField(TEXT("ok")));
		return bRequestOk && Result.StatusCode == 200 && Json.IsValid() && Json->GetBoolField(TEXT("ok"));
	};

	TestTrue(TEXT("Blueprint add vector alias variable"), AddVariableByJsonTail(
		TEXT("smoke-blueprint-add-vector-variable-001"),
		TEXT("SmokeVector"),
		TEXT("\"pin_category\":\"vector\",\"default_value\":\"(X=1.000000,Y=2.000000,Z=3.000000)\"")));
	TestTrue(TEXT("Blueprint add rotator alias variable"), AddVariableByJsonTail(
		TEXT("smoke-blueprint-add-rotator-variable-001"),
		TEXT("SmokeRotator"),
		TEXT("\"pin_category\":\"rotator\",\"default_value\":\"(Pitch=10.000000,Yaw=20.000000,Roll=30.000000)\"")));
	TestTrue(TEXT("Blueprint add linear color alias variable"), AddVariableByJsonTail(
		TEXT("smoke-blueprint-add-linear-color-variable-001"),
		TEXT("SmokeTint"),
		TEXT("\"pin_category\":\"linearcolor\",\"default_value\":\"(R=0.100000,G=0.200000,B=0.300000,A=1.000000)\"")));
	TestTrue(TEXT("Blueprint add object typed variable"), AddVariableByJsonTail(
		TEXT("smoke-blueprint-add-object-variable-001"),
		TEXT("SmokeActorRef"),
		TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"/Script/Engine.Actor\"")));
	TestTrue(TEXT("Blueprint add int array variable"), AddVariableByJsonTail(
		TEXT("smoke-blueprint-add-int-array-variable-001"),
		TEXT("SmokeInts"),
		TEXT("\"pin_category\":\"int\",\"container_type\":\"array\"")));
	TestTrue(TEXT("Blueprint add string-int map variable"), AddVariableByJsonTail(
		TEXT("smoke-blueprint-add-map-variable-001"),
		TEXT("SmokeScores"),
		TEXT("\"pin_category\":\"string\",\"container_type\":\"map\",\"value_type\":{\"pin_category\":\"int\"}")));

	const FString GetInfoAfterBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-info-after-001"),
		TEXT("blueprint_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *BlueprintAssetPath));
	FHttpSmokeResult GetInfoAfterResult;
	TestTrue(TEXT("Blueprint get_info after request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoAfterBody, GetInfoAfterResult));
	TestEqual(TEXT("Blueprint get_info after status code"), GetInfoAfterResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoAfterJson;
	TestTrue(TEXT("Blueprint get_info after response parses as JSON"), ParseJson(GetInfoAfterResult.Body, GetInfoAfterJson));
	TestTrue(TEXT("Blueprint get_info after root ok=true"), GetInfoAfterJson.IsValid() && GetInfoAfterJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* GetInfoAfterData = nullptr;
	TestTrue(TEXT("Blueprint get_info after contains data object"), GetInfoAfterJson.IsValid() && GetInfoAfterJson->TryGetObjectField(TEXT("data"), GetInfoAfterData) && GetInfoAfterData && GetInfoAfterData->IsValid());
	if (GetInfoAfterData && GetInfoAfterData->IsValid())
	{
		const int32 VariableCountAfter = static_cast<int32>((*GetInfoAfterData)->GetIntegerField(TEXT("new_variable_count")));
		TestTrue(TEXT("Blueprint new variable count increased"), VariableCountAfter >= VariableCountBefore + 1);
		TestTrue(TEXT("Blueprint generated class is non-empty"), !(*GetInfoAfterData)->GetStringField(TEXT("generated_class")).IsEmpty());
	}

	const FString CompileBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-compile-001"),
		TEXT("blueprint_compile"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_messages\":true,\"max_messages\":16,\"save_after_compile\":false}"), *BlueprintAssetPath));
	FHttpSmokeResult CompileResult;
	TestTrue(TEXT("Blueprint compile request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CompileBody, CompileResult));
	TestEqual(TEXT("Blueprint compile status code"), CompileResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CompileJson;
	TestTrue(TEXT("Blueprint compile response parses as JSON"), ParseJson(CompileResult.Body, CompileJson));
	TestTrue(TEXT("Blueprint compile root ok=true"), CompileJson.IsValid() && CompileJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* CompileData = nullptr;
	TestTrue(TEXT("Blueprint compile contains data object"), CompileJson.IsValid() && CompileJson->TryGetObjectField(TEXT("data"), CompileData) && CompileData && CompileData->IsValid());
	if (CompileData && CompileData->IsValid())
	{
		TestFalse(TEXT("Blueprint compile reports no error"), (*CompileData)->GetBoolField(TEXT("has_error")));
		TestTrue(TEXT("Blueprint compile error count is zero"), static_cast<int32>((*CompileData)->GetIntegerField(TEXT("error_count"))) == 0);
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceBlueprintVariableTypeMatrixSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.BlueprintVariableTypeMatrixWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceBlueprintVariableTypeMatrixSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString BlueprintAssetPath = MakeAutomationAssetPath(TEXT("BPVarTypes"));

	const FString CreateBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-var-matrix-create-001"),
		TEXT("blueprint_create"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"parent_class\":\"/Script/Engine.Actor\",\"compile_after_create\":false,\"open_editor\":false,\"save_after_create\":false}"),
			*BlueprintAssetPath));
	FHttpSmokeResult CreateResult;
	TestTrue(TEXT("Blueprint variable matrix create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateBody, CreateResult));
	TestEqual(TEXT("Blueprint variable matrix create status code"), CreateResult.StatusCode, 200);

	struct FVariableTypeCase
	{
		const TCHAR* Name;
		const TCHAR* JsonTail;
	};

	const FVariableTypeCase Cases[] = {
		{ TEXT("VarBool"), TEXT("\"pin_category\":\"bool\",\"default_value\":\"false\"") },
		{ TEXT("VarByte"), TEXT("\"pin_category\":\"byte\",\"default_value\":\"1\"") },
		{ TEXT("VarInt"), TEXT("\"pin_category\":\"int\",\"default_value\":\"42\"") },
		{ TEXT("VarInt64"), TEXT("\"pin_category\":\"int64\",\"default_value\":\"42000000000\"") },
		{ TEXT("VarFloat"), TEXT("\"pin_category\":\"float\",\"default_value\":\"3.5\"") },
		{ TEXT("VarDouble"), TEXT("\"pin_category\":\"double\",\"default_value\":\"6.25\"") },
		{ TEXT("VarName"), TEXT("\"pin_category\":\"name\",\"default_value\":\"SmokeName\"") },
		{ TEXT("VarString"), TEXT("\"pin_category\":\"string\",\"default_value\":\"SmokeString\"") },
		{ TEXT("VarText"), TEXT("\"pin_category\":\"text\"") },
		{ TEXT("VarVector"), TEXT("\"pin_category\":\"vector\"") },
		{ TEXT("VarVector2D"), TEXT("\"pin_category\":\"vector2d\"") },
		{ TEXT("VarVector4"), TEXT("\"pin_category\":\"vector4\"") },
		{ TEXT("VarRotator"), TEXT("\"pin_category\":\"rotator\"") },
		{ TEXT("VarTransform"), TEXT("\"pin_category\":\"transform\"") },
		{ TEXT("VarLinearColor"), TEXT("\"pin_category\":\"linearcolor\"") },
		{ TEXT("VarColor"), TEXT("\"pin_category\":\"color\"") },
		{ TEXT("VarQuat"), TEXT("\"pin_category\":\"quat\"") },
		{ TEXT("VarIntPoint"), TEXT("\"pin_category\":\"intpoint\"") },
		{ TEXT("VarIntVector"), TEXT("\"pin_category\":\"intvector\"") },
		{ TEXT("VarInt64Vector2"), TEXT("\"pin_category\":\"int64vector2\"") },
		{ TEXT("VarIntVector4"), TEXT("\"pin_category\":\"intvector4\"") },
		{ TEXT("VarRandomStream"), TEXT("\"pin_category\":\"randomstream\"") },
		{ TEXT("VarGuid"), TEXT("\"pin_category\":\"guid\"") },
		{ TEXT("VarDateTime"), TEXT("\"pin_category\":\"datetime\"") },
		{ TEXT("VarTimespan"), TEXT("\"pin_category\":\"timespan\"") },
		{ TEXT("VarFrameNumber"), TEXT("\"pin_category\":\"framenumber\"") },
		{ TEXT("VarFrameTime"), TEXT("\"pin_category\":\"frametime\"") },
		{ TEXT("VarFrameRate"), TEXT("\"pin_category\":\"framerate\"") },
		{ TEXT("VarSoftObjectPath"), TEXT("\"pin_category\":\"softobjectpath\"") },
		{ TEXT("VarSoftClassPath"), TEXT("\"pin_category\":\"softclasspath\"") },
		{ TEXT("VarPrimaryAssetType"), TEXT("\"pin_category\":\"primaryassettype\"") },
		{ TEXT("VarPrimaryAssetId"), TEXT("\"pin_category\":\"primaryassetid\"") },
		{ TEXT("VarTopLevelAssetPath"), TEXT("\"pin_category\":\"toplevelassetpath\"") },
		{ TEXT("VarBox"), TEXT("\"pin_category\":\"box\"") },
		{ TEXT("VarBox2D"), TEXT("\"pin_category\":\"box2d\"") },
		{ TEXT("VarBoxSphereBounds"), TEXT("\"pin_category\":\"boxspherebounds\"") },
		{ TEXT("VarTwoVectors"), TEXT("\"pin_category\":\"twovectors\"") },
		{ TEXT("VarHitResult"), TEXT("\"pin_category\":\"hitresult\"") },
		{ TEXT("VarTimerHandle"), TEXT("\"pin_category\":\"timerhandle\"") },
		{ TEXT("VarDataTableRowHandle"), TEXT("\"pin_category\":\"datatablerowhandle\"") },
		{ TEXT("VarCurveTableRowHandle"), TEXT("\"pin_category\":\"curvetablerowhandle\"") },
		{ TEXT("VarVectorNetQuantize"), TEXT("\"pin_category\":\"vectornetquantize\"") },
		{ TEXT("VarVectorNetQuantize10"), TEXT("\"pin_category\":\"vectornetquantize10\"") },
		{ TEXT("VarVectorNetQuantize100"), TEXT("\"pin_category\":\"vectornetquantize100\"") },
		{ TEXT("VarVectorNetQuantizeNormal"), TEXT("\"pin_category\":\"vectornetquantizenormal\"") },
		{ TEXT("VarKey"), TEXT("\"pin_category\":\"key\"") },
		{ TEXT("VarInputChord"), TEXT("\"pin_category\":\"inputchord\"") },
		{ TEXT("VarGameplayTag"), TEXT("\"pin_category\":\"gameplaytag\"") },
		{ TEXT("VarGameplayTagContainer"), TEXT("\"pin_category\":\"gameplaytagcontainer\"") },
		{ TEXT("VarGameplayTagQuery"), TEXT("\"pin_category\":\"gameplaytagquery\"") },
		{ TEXT("VarMargin"), TEXT("\"pin_category\":\"margin\"") },
		{ TEXT("VarSlateColor"), TEXT("\"pin_category\":\"slatecolor\"") },
		{ TEXT("VarSlateBrush"), TEXT("\"pin_category\":\"slatebrush\"") },
		{ TEXT("VarSlateFontInfo"), TEXT("\"pin_category\":\"slatefontinfo\"") },
		{ TEXT("VarWidgetTransform"), TEXT("\"pin_category\":\"widgettransform\"") },
		{ TEXT("VarAnchors"), TEXT("\"pin_category\":\"anchors\"") },
		{ TEXT("VarAnchorData"), TEXT("\"pin_category\":\"anchordata\"") },
		{ TEXT("VarCollisionChannel"), TEXT("\"pin_category\":\"collisionchannel\"") },
		{ TEXT("VarObjectTypeQuery"), TEXT("\"pin_category\":\"objecttypequery\"") },
		{ TEXT("VarTraceTypeQuery"), TEXT("\"pin_category\":\"tracetypequery\"") },
		{ TEXT("VarInputEvent"), TEXT("\"pin_category\":\"enum\",\"pin_subcategory_object\":\"inputevent\"") },
		{ TEXT("VarControllerHand"), TEXT("\"pin_category\":\"enum\",\"pin_subcategory_object\":\"controllerhand\"") },
		{ TEXT("VarTouchIndex"), TEXT("\"pin_category\":\"enum\",\"pin_subcategory_object\":\"touchindex\"") },
		{ TEXT("VarPhysicalSurface"), TEXT("\"pin_category\":\"enum\",\"pin_subcategory_object\":\"physicalsurface\"") },
		{ TEXT("VarGenericObject"), TEXT("\"pin_category\":\"object\"") },
		{ TEXT("VarActorObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"actor\"") },
		{ TEXT("VarPawnObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"pawn\"") },
		{ TEXT("VarCharacterObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"character\"") },
		{ TEXT("VarControllerObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"controller\"") },
		{ TEXT("VarPlayerControllerObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"playercontroller\"") },
		{ TEXT("VarGameModeBaseObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"gamemodebase\"") },
		{ TEXT("VarGameStateBaseObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"gamestatebase\"") },
		{ TEXT("VarPlayerStateObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"playerstate\"") },
		{ TEXT("VarHudObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"hud\"") },
		{ TEXT("VarSaveGameObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"savegame\"") },
		{ TEXT("VarActorComponentObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"actorcomponent\"") },
		{ TEXT("VarSceneComponentObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"scenecomponent\"") },
		{ TEXT("VarPrimitiveComponentObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"primitivecomponent\"") },
		{ TEXT("VarStaticMeshComponentObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"staticmeshcomponent\"") },
		{ TEXT("VarSkeletalMeshComponentObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"skeletalmeshcomponent\"") },
		{ TEXT("VarCameraComponentObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"cameracomponent\"") },
		{ TEXT("VarSpringArmComponentObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"springarmcomponent\"") },
		{ TEXT("VarNiagaraComponentObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"niagaracomponent\"") },
		{ TEXT("VarStaticMeshObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"staticmesh\"") },
		{ TEXT("VarSkeletalMeshObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"skeletalmesh\"") },
		{ TEXT("VarMaterialInterfaceObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"materialinterface\"") },
		{ TEXT("VarMaterialObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"material\"") },
		{ TEXT("VarTexture2DObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"texture2d\"") },
		{ TEXT("VarTextureRenderTarget2DObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"texturerendertarget2d\"") },
		{ TEXT("VarSoundBaseObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"soundbase\"") },
		{ TEXT("VarAnimSequenceObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"animsequence\"") },
		{ TEXT("VarAnimMontageObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"animmontage\"") },
		{ TEXT("VarSkeletonObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"skeleton\"") },
		{ TEXT("VarDataTableObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"datatable\"") },
		{ TEXT("VarDataAssetObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"dataasset\"") },
		{ TEXT("VarCurveFloatObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"curvefloat\"") },
		{ TEXT("VarLevelSequenceObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"levelsequence\"") },
		{ TEXT("VarNiagaraSystemObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"niagarasystem\"") },
		{ TEXT("VarNiagaraEmitterObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"niagaraemitter\"") },
		{ TEXT("VarUserWidgetObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"userwidget\"") },
		{ TEXT("VarInputActionObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"inputaction\"") },
		{ TEXT("VarInputMappingContextObject"), TEXT("\"pin_category\":\"object\",\"pin_subcategory_object\":\"inputmappingcontext\"") },
		{ TEXT("VarActorClass"), TEXT("\"pin_category\":\"class\",\"pin_subcategory_object\":\"actor\"") },
		{ TEXT("VarPawnClass"), TEXT("\"pin_category\":\"class\",\"pin_subcategory_object\":\"pawn\"") },
		{ TEXT("VarUserWidgetClass"), TEXT("\"pin_category\":\"class\",\"pin_subcategory_object\":\"userwidget\"") },
		{ TEXT("VarStaticMeshSoftObject"), TEXT("\"pin_category\":\"softobject\",\"pin_subcategory_object\":\"staticmesh\"") },
		{ TEXT("VarNiagaraSystemSoftObject"), TEXT("\"pin_category\":\"softobject\",\"pin_subcategory_object\":\"niagarasystem\"") },
		{ TEXT("VarActorSoftClass"), TEXT("\"pin_category\":\"softclass\",\"pin_subcategory_object\":\"actor\"") },
		{ TEXT("VarUserWidgetSoftClass"), TEXT("\"pin_category\":\"softclass\",\"pin_subcategory_object\":\"userwidget\"") },
		{ TEXT("VarIntArray"), TEXT("\"pin_category\":\"int\",\"container_type\":\"array\"") },
		{ TEXT("VarNameSet"), TEXT("\"pin_category\":\"name\",\"container_type\":\"set\"") },
		{ TEXT("VarStringIntMap"), TEXT("\"pin_category\":\"string\",\"container_type\":\"map\",\"value_type\":{\"pin_category\":\"int\"}") },
		{ TEXT("VarNameVectorMap"), TEXT("\"pin_category\":\"name\",\"container_type\":\"map\",\"value_type\":{\"pin_category\":\"vector\"}") },
		{ TEXT("VarStringActorMap"), TEXT("\"pin_category\":\"string\",\"container_type\":\"map\",\"value_type\":{\"pin_category\":\"object\",\"pin_subcategory_object\":\"actor\"}") }
	};

	for (const FVariableTypeCase& Case : Cases)
	{
		const FString RequestId = FString::Printf(TEXT("smoke-blueprint-var-matrix-add-%s"), Case.Name);
		const FString Body = MakeExecRequestBody(
			RequestId,
			TEXT("blueprint_add_variable"),
			FString::Printf(
				TEXT("{\"asset_path\":\"%s\",\"variable_name\":\"%s\",%s,\"compile_after_add\":false,\"save_after_add\":false}"),
				*BlueprintAssetPath,
				Case.Name,
				Case.JsonTail));

		FHttpSmokeResult Result;
		const bool bRequestOk = ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, Body, Result);
		TestTrue(FString::Printf(TEXT("Blueprint variable matrix %s request succeeded"), Case.Name), bRequestOk);
		if (Result.StatusCode != 200)
		{
			AddError(FString::Printf(TEXT("Blueprint variable matrix %s status %d body: %s"), Case.Name, Result.StatusCode, *Result.Body));
		}
		TestEqual(FString::Printf(TEXT("Blueprint variable matrix %s status code"), Case.Name), Result.StatusCode, 200);

		TSharedPtr<FJsonObject> Json;
		const bool bJsonOk = ParseJson(Result.Body, Json);
		TestTrue(FString::Printf(TEXT("Blueprint variable matrix %s response parses"), Case.Name), bJsonOk);
		const bool bOk = Json.IsValid() && Json->GetBoolField(TEXT("ok"));
		if (!bOk)
		{
			AddError(FString::Printf(TEXT("Blueprint variable matrix %s response body: %s"), Case.Name, *Result.Body));
		}
		TestTrue(FString::Printf(TEXT("Blueprint variable matrix %s root ok=true"), Case.Name), bOk);
	}

	const FString CompileBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-var-matrix-compile-001"),
		TEXT("blueprint_compile"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_messages\":true,\"max_messages\":32,\"save_after_compile\":false}"), *BlueprintAssetPath));
	FHttpSmokeResult CompileResult;
	TestTrue(TEXT("Blueprint variable matrix compile request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CompileBody, CompileResult));
	if (CompileResult.StatusCode != 200)
	{
		AddError(FString::Printf(TEXT("Blueprint variable matrix compile status %d body: %s"), CompileResult.StatusCode, *CompileResult.Body));
	}
	TestEqual(TEXT("Blueprint variable matrix compile status code"), CompileResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CompileJson;
	TestTrue(TEXT("Blueprint variable matrix compile response parses"), ParseJson(CompileResult.Body, CompileJson));
	const TSharedPtr<FJsonObject>* CompileData = nullptr;
	TestTrue(TEXT("Blueprint variable matrix compile contains data"), CompileJson.IsValid() && CompileJson->TryGetObjectField(TEXT("data"), CompileData) && CompileData && CompileData->IsValid());
	if (CompileData && CompileData->IsValid())
	{
		TestFalse(TEXT("Blueprint variable matrix compile reports no error"), (*CompileData)->GetBoolField(TEXT("has_error")));
		TestTrue(TEXT("Blueprint variable matrix compile error count is zero"), static_cast<int32>((*CompileData)->GetIntegerField(TEXT("error_count"))) == 0);
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceBlueprintStructureSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.BlueprintStructureWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceBlueprintStructureSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString BlueprintAssetPath = MakeAutomationAssetPath(TEXT("BPStruct"));
	const FString VariableName = TEXT("SmokeStructFlag");
	const FString FunctionName = TEXT("SmokeUtilityFunction");
	const FString MacroName = TEXT("SmokeUtilityMacro");
	const FString DispatcherName = TEXT("SmokeChanged");

	auto CountGraphsByType = [](const TSharedPtr<FJsonObject>* ListData, const FString& GraphType) -> int32
	{
		if (!ListData || !ListData->IsValid())
		{
			return 0;
		}

		const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
		if (!(*ListData)->TryGetArrayField(TEXT("graphs"), Graphs) || !Graphs)
		{
			return 0;
		}

		int32 Count = 0;
		for (const TSharedPtr<FJsonValue>& GraphValue : *Graphs)
		{
			const TSharedPtr<FJsonObject>* GraphObject = nullptr;
			if (!GraphValue.IsValid() || !GraphValue->TryGetObject(GraphObject) || !GraphObject || !GraphObject->IsValid())
			{
				continue;
			}

			if ((*GraphObject)->GetStringField(TEXT("graph_type")).Equals(GraphType, ESearchCase::IgnoreCase))
			{
				++Count;
			}
		}

		return Count;
	};

	auto FindGraphByNameAndType = [](const TSharedPtr<FJsonObject>* ListData, const FString& GraphName, const FString& GraphType) -> bool
	{
		if (!ListData || !ListData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
		if (!(*ListData)->TryGetArrayField(TEXT("graphs"), Graphs) || !Graphs)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& GraphValue : *Graphs)
		{
			const TSharedPtr<FJsonObject>* GraphObject = nullptr;
			if (!GraphValue.IsValid() || !GraphValue->TryGetObject(GraphObject) || !GraphObject || !GraphObject->IsValid())
			{
				continue;
			}

			if ((*GraphObject)->GetStringField(TEXT("graph_name")) == GraphName &&
				(*GraphObject)->GetStringField(TEXT("graph_type")).Equals(GraphType, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}

		return false;
	};

	const FString CreateBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-structure-create-001"),
		TEXT("blueprint_create"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"parent_class\":\"/Script/Engine.Actor\",\"compile_after_create\":false,\"open_editor\":false,\"save_after_create\":false}"), *BlueprintAssetPath));
	FHttpSmokeResult CreateResult;
	TestTrue(TEXT("Blueprint structure create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateBody, CreateResult));
	TestEqual(TEXT("Blueprint structure create status code"), CreateResult.StatusCode, 200);

	const FString ListBeforeBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-structure-list-before-001"),
		TEXT("blueprint_list_graphs"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *BlueprintAssetPath));
	FHttpSmokeResult ListBeforeResult;
	TestTrue(TEXT("Blueprint list_graphs before request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListBeforeBody, ListBeforeResult));
	TestEqual(TEXT("Blueprint list_graphs before status code"), ListBeforeResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ListBeforeJson;
	TestTrue(TEXT("Blueprint list_graphs before response parses as JSON"), ParseJson(ListBeforeResult.Body, ListBeforeJson));
	TestTrue(TEXT("Blueprint list_graphs before root ok=true"), ListBeforeJson.IsValid() && ListBeforeJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* ListBeforeData = nullptr;
	TestTrue(TEXT("Blueprint list_graphs before contains data object"), ListBeforeJson.IsValid() && ListBeforeJson->TryGetObjectField(TEXT("data"), ListBeforeData) && ListBeforeData && ListBeforeData->IsValid());

	const int32 FunctionCountBefore = CountGraphsByType(ListBeforeData, TEXT("function"));
	const int32 MacroCountBefore = CountGraphsByType(ListBeforeData, TEXT("macro"));

	const FString AddVariableBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-structure-add-variable-001"),
		TEXT("blueprint_add_variable"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"variable_name\":\"%s\",\"pin_category\":\"bool\",\"compile_after_add\":false,\"save_after_add\":false}"), *BlueprintAssetPath, *VariableName));
	FHttpSmokeResult AddVariableResult;
	TestTrue(TEXT("Blueprint structure add_variable request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddVariableBody, AddVariableResult));
	TestEqual(TEXT("Blueprint structure add_variable status code"), AddVariableResult.StatusCode, 200);

	const FString AddFunctionGraphBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-structure-add-function-001"),
		TEXT("blueprint_add_function_graph"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"function_name\":\"%s\",\"compile_after_add\":false,\"save_after_add\":false}"), *BlueprintAssetPath, *FunctionName));
	FHttpSmokeResult AddFunctionGraphResult;
	TestTrue(TEXT("Blueprint structure add_function_graph request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddFunctionGraphBody, AddFunctionGraphResult));
	TestEqual(TEXT("Blueprint structure add_function_graph status code"), AddFunctionGraphResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddFunctionGraphJson;
	TestTrue(TEXT("Blueprint structure add_function_graph response parses as JSON"), ParseJson(AddFunctionGraphResult.Body, AddFunctionGraphJson));
	TestTrue(TEXT("Blueprint structure add_function_graph root ok=true"), AddFunctionGraphJson.IsValid() && AddFunctionGraphJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* AddFunctionGraphData = nullptr;
	TestTrue(TEXT("Blueprint structure add_function_graph contains data object"), AddFunctionGraphJson.IsValid() && AddFunctionGraphJson->TryGetObjectField(TEXT("data"), AddFunctionGraphData) && AddFunctionGraphData && AddFunctionGraphData->IsValid());
	if (AddFunctionGraphData && AddFunctionGraphData->IsValid())
	{
		TestEqual(TEXT("Blueprint structure add_function_graph name"), (*AddFunctionGraphData)->GetStringField(TEXT("graph_name")), FunctionName);
		TestTrue(TEXT("Blueprint structure add_function_graph changed"), (*AddFunctionGraphData)->GetBoolField(TEXT("changed")));
	}

	const FString AddMacroGraphBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-structure-add-macro-001"),
		TEXT("blueprint_add_macro_graph"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"macro_name\":\"%s\",\"compile_after_add\":false,\"save_after_add\":false}"), *BlueprintAssetPath, *MacroName));
	FHttpSmokeResult AddMacroGraphResult;
	TestTrue(TEXT("Blueprint structure add_macro_graph request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddMacroGraphBody, AddMacroGraphResult));
	TestEqual(TEXT("Blueprint structure add_macro_graph status code"), AddMacroGraphResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddMacroGraphJson;
	TestTrue(TEXT("Blueprint structure add_macro_graph response parses as JSON"), ParseJson(AddMacroGraphResult.Body, AddMacroGraphJson));
	TestTrue(TEXT("Blueprint structure add_macro_graph root ok=true"), AddMacroGraphJson.IsValid() && AddMacroGraphJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* AddMacroGraphData = nullptr;
	TestTrue(TEXT("Blueprint structure add_macro_graph contains data object"), AddMacroGraphJson.IsValid() && AddMacroGraphJson->TryGetObjectField(TEXT("data"), AddMacroGraphData) && AddMacroGraphData && AddMacroGraphData->IsValid());
	if (AddMacroGraphData && AddMacroGraphData->IsValid())
	{
		TestEqual(TEXT("Blueprint structure add_macro_graph name"), (*AddMacroGraphData)->GetStringField(TEXT("graph_name")), MacroName);
		TestTrue(TEXT("Blueprint structure add_macro_graph changed"), (*AddMacroGraphData)->GetBoolField(TEXT("changed")));
	}

	const FString AddDispatcherBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-structure-add-dispatcher-001"),
		TEXT("blueprint_add_event_dispatcher"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"dispatcher_name\":\"%s\",\"compile_after_add\":false,\"save_after_add\":false}"), *BlueprintAssetPath, *DispatcherName));
	FHttpSmokeResult AddDispatcherResult;
	TestTrue(TEXT("Blueprint structure add_event_dispatcher request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddDispatcherBody, AddDispatcherResult));
	TestEqual(TEXT("Blueprint structure add_event_dispatcher status code"), AddDispatcherResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddDispatcherJson;
	TestTrue(TEXT("Blueprint structure add_event_dispatcher response parses as JSON"), ParseJson(AddDispatcherResult.Body, AddDispatcherJson));
	TestTrue(TEXT("Blueprint structure add_event_dispatcher root ok=true"), AddDispatcherJson.IsValid() && AddDispatcherJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* AddDispatcherData = nullptr;
	TestTrue(TEXT("Blueprint structure add_event_dispatcher contains data object"), AddDispatcherJson.IsValid() && AddDispatcherJson->TryGetObjectField(TEXT("data"), AddDispatcherData) && AddDispatcherData && AddDispatcherData->IsValid());
	if (AddDispatcherData && AddDispatcherData->IsValid())
	{
		TestEqual(TEXT("Blueprint structure add_event_dispatcher name"), (*AddDispatcherData)->GetStringField(TEXT("dispatcher_name")), DispatcherName);
		TestTrue(TEXT("Blueprint structure add_event_dispatcher changed"), (*AddDispatcherData)->GetBoolField(TEXT("changed")));
	}

	const FString ListAfterBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-structure-list-after-001"),
		TEXT("blueprint_list_graphs"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *BlueprintAssetPath));
	FHttpSmokeResult ListAfterResult;
	TestTrue(TEXT("Blueprint list_graphs after request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListAfterBody, ListAfterResult));
	TestEqual(TEXT("Blueprint list_graphs after status code"), ListAfterResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ListAfterJson;
	TestTrue(TEXT("Blueprint list_graphs after response parses as JSON"), ParseJson(ListAfterResult.Body, ListAfterJson));
	TestTrue(TEXT("Blueprint list_graphs after root ok=true"), ListAfterJson.IsValid() && ListAfterJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* ListAfterData = nullptr;
	TestTrue(TEXT("Blueprint list_graphs after contains data object"), ListAfterJson.IsValid() && ListAfterJson->TryGetObjectField(TEXT("data"), ListAfterData) && ListAfterData && ListAfterData->IsValid());
	if (ListAfterData && ListAfterData->IsValid())
	{
		TestTrue(TEXT("Blueprint function graph count increased"), CountGraphsByType(ListAfterData, TEXT("function")) >= FunctionCountBefore + 1);
		TestTrue(TEXT("Blueprint macro graph count increased"), CountGraphsByType(ListAfterData, TEXT("macro")) >= MacroCountBefore + 1);
		TestTrue(TEXT("Blueprint list_graphs contains function graph"), FindGraphByNameAndType(ListAfterData, FunctionName, TEXT("function")));
		TestTrue(TEXT("Blueprint list_graphs contains macro graph"), FindGraphByNameAndType(ListAfterData, MacroName, TEXT("macro")));
	}

	const FString RemoveVariableBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-structure-remove-variable-001"),
		TEXT("blueprint_remove_variable"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"variable_name\":\"%s\",\"compile_after_remove\":false,\"save_after_remove\":false}"), *BlueprintAssetPath, *VariableName));
	FHttpSmokeResult RemoveVariableResult;
	TestTrue(TEXT("Blueprint structure remove_variable request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveVariableBody, RemoveVariableResult));
	TestEqual(TEXT("Blueprint structure remove_variable status code"), RemoveVariableResult.StatusCode, 200);

	TSharedPtr<FJsonObject> RemoveVariableJson;
	TestTrue(TEXT("Blueprint structure remove_variable response parses as JSON"), ParseJson(RemoveVariableResult.Body, RemoveVariableJson));
	TestTrue(TEXT("Blueprint structure remove_variable root ok=true"), RemoveVariableJson.IsValid() && RemoveVariableJson->GetBoolField(TEXT("ok")));

	const FString RemoveVariableAgainBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-structure-remove-variable-002"),
		TEXT("blueprint_remove_variable"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"variable_name\":\"%s\",\"compile_after_remove\":false,\"save_after_remove\":false}"), *BlueprintAssetPath, *VariableName));
	FHttpSmokeResult RemoveVariableAgainResult;
	TestTrue(TEXT("Blueprint structure remove_variable second request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveVariableAgainBody, RemoveVariableAgainResult));
	TestEqual(TEXT("Blueprint structure remove_variable second status code"), RemoveVariableAgainResult.StatusCode, 400);

	TSharedPtr<FJsonObject> RemoveVariableAgainJson;
	TestTrue(TEXT("Blueprint structure remove_variable second response parses as JSON"), ParseJson(RemoveVariableAgainResult.Body, RemoveVariableAgainJson));
	TestTrue(TEXT("Blueprint structure remove_variable second root ok=false"), RemoveVariableAgainJson.IsValid() && !RemoveVariableAgainJson->GetBoolField(TEXT("ok")));
	if (RemoveVariableAgainJson.IsValid())
	{
		TestEqual(TEXT("Blueprint structure remove_variable second error"), RemoveVariableAgainJson->GetStringField(TEXT("error")), FString(TEXT("variable_not_found")));
	}

	const FString CompileBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-structure-compile-001"),
		TEXT("blueprint_compile"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_messages\":true,\"max_messages\":16,\"save_after_compile\":false}"), *BlueprintAssetPath));
	FHttpSmokeResult CompileResult;
	TestTrue(TEXT("Blueprint structure compile request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CompileBody, CompileResult));
	TestEqual(TEXT("Blueprint structure compile status code"), CompileResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CompileJson;
	TestTrue(TEXT("Blueprint structure compile response parses as JSON"), ParseJson(CompileResult.Body, CompileJson));
	TestTrue(TEXT("Blueprint structure compile root ok=true"), CompileJson.IsValid() && CompileJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* CompileData = nullptr;
	TestTrue(TEXT("Blueprint structure compile contains data object"), CompileJson.IsValid() && CompileJson->TryGetObjectField(TEXT("data"), CompileData) && CompileData && CompileData->IsValid());
	if (CompileData && CompileData->IsValid())
	{
		TestFalse(TEXT("Blueprint structure compile reports no error"), (*CompileData)->GetBoolField(TEXT("has_error")));
		TestTrue(TEXT("Blueprint structure compile error count is zero"), static_cast<int32>((*CompileData)->GetIntegerField(TEXT("error_count"))) == 0);
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceUmgSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.UmgWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceUmgSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString WidgetBlueprintAssetPath = MakeAutomationAssetPath(TEXT("WBP"));
	const FString WidgetName = TEXT("BtnSmoke");
	const FString RenamedWidgetName = TEXT("BtnSmokeRenamed");

	const FString CreateBody = MakeExecRequestBody(
		TEXT("smoke-umg-create-001"),
		TEXT("umg_create_widget_blueprint"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"parent_class\":\"/Script/UMG.UserWidget\",\"create_default_root\":true,\"compile_after_create\":false,\"open_editor\":false,\"save_after_create\":false}"),
			*WidgetBlueprintAssetPath));
	FHttpSmokeResult CreateResult;
	TestTrue(TEXT("UMG create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateBody, CreateResult));
	TestEqual(TEXT("UMG create status code"), CreateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CreateJson;
	TestTrue(TEXT("UMG create response parses as JSON"), ParseJson(CreateResult.Body, CreateJson));
	TestTrue(TEXT("UMG create root ok=true"), CreateJson.IsValid() && CreateJson->GetBoolField(TEXT("ok")));

	FString RootWidgetName;
	const TSharedPtr<FJsonObject>* CreateData = nullptr;
	TestTrue(TEXT("UMG create contains data object"), CreateJson.IsValid() && CreateJson->TryGetObjectField(TEXT("data"), CreateData) && CreateData && CreateData->IsValid());
	if (CreateData && CreateData->IsValid())
	{
		RootWidgetName = (*CreateData)->GetStringField(TEXT("root_widget"));
		TestEqual(TEXT("UMG create asset path"), (*CreateData)->GetStringField(TEXT("asset_path")), WidgetBlueprintAssetPath);
		TestTrue(TEXT("UMG create root widget is non-empty"), !RootWidgetName.IsEmpty());
		TestFalse(TEXT("UMG create not saved"), (*CreateData)->GetBoolField(TEXT("saved")));
	}

	const FString GetInfoBeforeBody = MakeExecRequestBody(
		TEXT("smoke-umg-info-before-001"),
		TEXT("umg_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *WidgetBlueprintAssetPath));
	FHttpSmokeResult GetInfoBeforeResult;
	TestTrue(TEXT("UMG get_info before request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoBeforeBody, GetInfoBeforeResult));
	TestEqual(TEXT("UMG get_info before status code"), GetInfoBeforeResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoBeforeJson;
	TestTrue(TEXT("UMG get_info before response parses as JSON"), ParseJson(GetInfoBeforeResult.Body, GetInfoBeforeJson));
	TestTrue(TEXT("UMG get_info before root ok=true"), GetInfoBeforeJson.IsValid() && GetInfoBeforeJson->GetBoolField(TEXT("ok")));

	int32 WidgetCountBefore = 0;
	const TSharedPtr<FJsonObject>* GetInfoBeforeData = nullptr;
	TestTrue(TEXT("UMG get_info before contains data object"), GetInfoBeforeJson.IsValid() && GetInfoBeforeJson->TryGetObjectField(TEXT("data"), GetInfoBeforeData) && GetInfoBeforeData && GetInfoBeforeData->IsValid());
	if (GetInfoBeforeData && GetInfoBeforeData->IsValid())
	{
		WidgetCountBefore = static_cast<int32>((*GetInfoBeforeData)->GetIntegerField(TEXT("widget_count")));
		TestEqual(TEXT("UMG get_info root widget"), (*GetInfoBeforeData)->GetStringField(TEXT("root_widget")), RootWidgetName);
	}

	const FString AddWidgetBody = MakeExecRequestBody(
		TEXT("smoke-umg-add-widget-001"),
		TEXT("umg_add_widget"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"widget_class\":\"/Script/UMG.Button\",\"widget_name\":\"%s\",\"parent_widget\":\"%s\",\"make_variable\":true,\"compile_after_add\":false,\"save_after_add\":false}"),
			*WidgetBlueprintAssetPath,
			*WidgetName,
			*RootWidgetName));
	FHttpSmokeResult AddWidgetResult;
	TestTrue(TEXT("UMG add_widget request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddWidgetBody, AddWidgetResult));
	TestEqual(TEXT("UMG add_widget status code"), AddWidgetResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddWidgetJson;
	TestTrue(TEXT("UMG add_widget response parses as JSON"), ParseJson(AddWidgetResult.Body, AddWidgetJson));
	TestTrue(TEXT("UMG add_widget root ok=true"), AddWidgetJson.IsValid() && AddWidgetJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* AddWidgetData = nullptr;
	TestTrue(TEXT("UMG add_widget contains data object"), AddWidgetJson.IsValid() && AddWidgetJson->TryGetObjectField(TEXT("data"), AddWidgetData) && AddWidgetData && AddWidgetData->IsValid());
	if (AddWidgetData && AddWidgetData->IsValid())
	{
		TestEqual(TEXT("UMG add_widget name"), (*AddWidgetData)->GetStringField(TEXT("widget_name")), WidgetName);
		TestEqual(TEXT("UMG add_widget parent"), (*AddWidgetData)->GetStringField(TEXT("parent_widget")), RootWidgetName);
		TestEqual(TEXT("UMG add_widget class"), (*AddWidgetData)->GetStringField(TEXT("widget_class")), FString(TEXT("/Script/UMG.Button")));
		TestFalse(TEXT("UMG add_widget not saved"), (*AddWidgetData)->GetBoolField(TEXT("saved")));
	}

	const FString GetInfoAfterBody = MakeExecRequestBody(
		TEXT("smoke-umg-info-after-001"),
		TEXT("umg_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *WidgetBlueprintAssetPath));
	FHttpSmokeResult GetInfoAfterResult;
	TestTrue(TEXT("UMG get_info after request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoAfterBody, GetInfoAfterResult));
	TestEqual(TEXT("UMG get_info after status code"), GetInfoAfterResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoAfterJson;
	TestTrue(TEXT("UMG get_info after response parses as JSON"), ParseJson(GetInfoAfterResult.Body, GetInfoAfterJson));
	TestTrue(TEXT("UMG get_info after root ok=true"), GetInfoAfterJson.IsValid() && GetInfoAfterJson->GetBoolField(TEXT("ok")));

	bool bFoundButtonWidget = false;
	const TSharedPtr<FJsonObject>* GetInfoAfterData = nullptr;
	TestTrue(TEXT("UMG get_info after contains data object"), GetInfoAfterJson.IsValid() && GetInfoAfterJson->TryGetObjectField(TEXT("data"), GetInfoAfterData) && GetInfoAfterData && GetInfoAfterData->IsValid());
	if (GetInfoAfterData && GetInfoAfterData->IsValid())
	{
		const int32 WidgetCountAfter = static_cast<int32>((*GetInfoAfterData)->GetIntegerField(TEXT("widget_count")));
		TestTrue(TEXT("UMG widget count increased"), WidgetCountAfter >= WidgetCountBefore + 1);

		const TArray<TSharedPtr<FJsonValue>>* Widgets = nullptr;
		TestTrue(TEXT("UMG get_info after contains widgets array"), (*GetInfoAfterData)->TryGetArrayField(TEXT("widgets"), Widgets) && Widgets != nullptr);
		if (Widgets)
		{
			for (const TSharedPtr<FJsonValue>& WidgetValue : *Widgets)
			{
				const TSharedPtr<FJsonObject>* WidgetObject = nullptr;
				if (!WidgetValue.IsValid() || !WidgetValue->TryGetObject(WidgetObject) || !WidgetObject || !WidgetObject->IsValid())
				{
					continue;
				}

				if ((*WidgetObject)->GetStringField(TEXT("name")) == WidgetName)
				{
					bFoundButtonWidget = true;
					TestEqual(TEXT("UMG added widget parent in tree"), (*WidgetObject)->GetStringField(TEXT("parent")), RootWidgetName);
					break;
				}
			}
		}
	}
	TestTrue(TEXT("UMG get_info after contains added widget"), bFoundButtonWidget);

	const FString RenameWidgetBody = MakeExecRequestBody(
		TEXT("smoke-umg-rename-widget-001"),
		TEXT("umg_rename_widget"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"widget_name\":\"%s\",\"new_widget_name\":\"%s\",\"compile_after_rename\":false,\"save_after_rename\":false}"),
			*WidgetBlueprintAssetPath,
			*WidgetName,
			*RenamedWidgetName));
	FHttpSmokeResult RenameWidgetResult;
	TestTrue(TEXT("UMG rename_widget request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RenameWidgetBody, RenameWidgetResult));
	TestEqual(TEXT("UMG rename_widget status code"), RenameWidgetResult.StatusCode, 200);

	TSharedPtr<FJsonObject> RenameWidgetJson;
	TestTrue(TEXT("UMG rename_widget response parses as JSON"), ParseJson(RenameWidgetResult.Body, RenameWidgetJson));
	TestTrue(TEXT("UMG rename_widget root ok=true"), RenameWidgetJson.IsValid() && RenameWidgetJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* RenameWidgetData = nullptr;
	TestTrue(TEXT("UMG rename_widget contains data object"), RenameWidgetJson.IsValid() && RenameWidgetJson->TryGetObjectField(TEXT("data"), RenameWidgetData) && RenameWidgetData && RenameWidgetData->IsValid());
	if (RenameWidgetData && RenameWidgetData->IsValid())
	{
		TestEqual(TEXT("UMG rename_widget old name"), (*RenameWidgetData)->GetStringField(TEXT("widget_name")), WidgetName);
		TestEqual(TEXT("UMG rename_widget new name"), (*RenameWidgetData)->GetStringField(TEXT("new_widget_name")), RenamedWidgetName);
		TestTrue(TEXT("UMG rename_widget reports changed"), (*RenameWidgetData)->GetBoolField(TEXT("changed")));
	}

	const FString GetInfoAfterRenameBody = MakeExecRequestBody(
		TEXT("smoke-umg-info-after-rename-001"),
		TEXT("umg_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *WidgetBlueprintAssetPath));
	FHttpSmokeResult GetInfoAfterRenameResult;
	TestTrue(TEXT("UMG get_info after rename request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoAfterRenameBody, GetInfoAfterRenameResult));
	TestEqual(TEXT("UMG get_info after rename status code"), GetInfoAfterRenameResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoAfterRenameJson;
	TestTrue(TEXT("UMG get_info after rename response parses as JSON"), ParseJson(GetInfoAfterRenameResult.Body, GetInfoAfterRenameJson));
	TestTrue(TEXT("UMG get_info after rename root ok=true"), GetInfoAfterRenameJson.IsValid() && GetInfoAfterRenameJson->GetBoolField(TEXT("ok")));

	bool bFoundOldWidgetAfterRename = false;
	bool bFoundRenamedWidget = false;
	const TSharedPtr<FJsonObject>* GetInfoAfterRenameData = nullptr;
	TestTrue(TEXT("UMG get_info after rename contains data object"), GetInfoAfterRenameJson.IsValid() && GetInfoAfterRenameJson->TryGetObjectField(TEXT("data"), GetInfoAfterRenameData) && GetInfoAfterRenameData && GetInfoAfterRenameData->IsValid());
	if (GetInfoAfterRenameData && GetInfoAfterRenameData->IsValid())
	{
		const int32 WidgetCountAfterRename = static_cast<int32>((*GetInfoAfterRenameData)->GetIntegerField(TEXT("widget_count")));
		TestEqual(TEXT("UMG widget count unchanged after rename"), WidgetCountAfterRename, WidgetCountBefore + 1);

		const TArray<TSharedPtr<FJsonValue>>* Widgets = nullptr;
		TestTrue(TEXT("UMG get_info after rename contains widgets array"), (*GetInfoAfterRenameData)->TryGetArrayField(TEXT("widgets"), Widgets) && Widgets != nullptr);
		if (Widgets)
		{
			for (const TSharedPtr<FJsonValue>& WidgetValue : *Widgets)
			{
				const TSharedPtr<FJsonObject>* WidgetObject = nullptr;
				if (!WidgetValue.IsValid() || !WidgetValue->TryGetObject(WidgetObject) || !WidgetObject || !WidgetObject->IsValid())
				{
					continue;
				}

				const FString CurrentWidgetName = (*WidgetObject)->GetStringField(TEXT("name"));
				if (CurrentWidgetName == WidgetName)
				{
					bFoundOldWidgetAfterRename = true;
				}
				if (CurrentWidgetName == RenamedWidgetName)
				{
					bFoundRenamedWidget = true;
					TestEqual(TEXT("UMG renamed widget parent in tree"), (*WidgetObject)->GetStringField(TEXT("parent")), RootWidgetName);
				}
			}
		}
	}
	TestFalse(TEXT("UMG old widget name no longer present after rename"), bFoundOldWidgetAfterRename);
	TestTrue(TEXT("UMG renamed widget present after rename"), bFoundRenamedWidget);

	const FString CompileAfterRenameBody = MakeExecRequestBody(
		TEXT("smoke-umg-compile-after-rename-001"),
		TEXT("umg_compile"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_messages\":true,\"max_messages\":16,\"save_after_compile\":false}"), *WidgetBlueprintAssetPath));
	FHttpSmokeResult CompileAfterRenameResult;
	TestTrue(TEXT("UMG compile after rename request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CompileAfterRenameBody, CompileAfterRenameResult));
	TestEqual(TEXT("UMG compile after rename status code"), CompileAfterRenameResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CompileAfterRenameJson;
	TestTrue(TEXT("UMG compile after rename response parses as JSON"), ParseJson(CompileAfterRenameResult.Body, CompileAfterRenameJson));
	TestTrue(TEXT("UMG compile after rename root ok=true"), CompileAfterRenameJson.IsValid() && CompileAfterRenameJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* CompileAfterRenameData = nullptr;
	TestTrue(TEXT("UMG compile after rename contains data object"), CompileAfterRenameJson.IsValid() && CompileAfterRenameJson->TryGetObjectField(TEXT("data"), CompileAfterRenameData) && CompileAfterRenameData && CompileAfterRenameData->IsValid());
	if (CompileAfterRenameData && CompileAfterRenameData->IsValid())
	{
		TestFalse(TEXT("UMG compile after rename reports no error"), (*CompileAfterRenameData)->GetBoolField(TEXT("has_error")));
		TestTrue(TEXT("UMG compile after rename error count is zero"), static_cast<int32>((*CompileAfterRenameData)->GetIntegerField(TEXT("error_count"))) == 0);
	}

	const FString RemoveWidgetBody = MakeExecRequestBody(
		TEXT("smoke-umg-remove-widget-001"),
		TEXT("umg_remove_widget"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"widget_name\":\"%s\",\"compile_after_remove\":false,\"save_after_remove\":false}"),
			*WidgetBlueprintAssetPath,
			*RenamedWidgetName));
	FHttpSmokeResult RemoveWidgetResult;
	TestTrue(TEXT("UMG remove_widget request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveWidgetBody, RemoveWidgetResult));
	TestEqual(TEXT("UMG remove_widget status code"), RemoveWidgetResult.StatusCode, 200);

	TSharedPtr<FJsonObject> RemoveWidgetJson;
	TestTrue(TEXT("UMG remove_widget response parses as JSON"), ParseJson(RemoveWidgetResult.Body, RemoveWidgetJson));
	TestTrue(TEXT("UMG remove_widget root ok=true"), RemoveWidgetJson.IsValid() && RemoveWidgetJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* RemoveWidgetData = nullptr;
	TestTrue(TEXT("UMG remove_widget contains data object"), RemoveWidgetJson.IsValid() && RemoveWidgetJson->TryGetObjectField(TEXT("data"), RemoveWidgetData) && RemoveWidgetData && RemoveWidgetData->IsValid());
	if (RemoveWidgetData && RemoveWidgetData->IsValid())
	{
		TestEqual(TEXT("UMG remove_widget removed name"), (*RemoveWidgetData)->GetStringField(TEXT("removed_widget")), RenamedWidgetName);
		TestFalse(TEXT("UMG remove_widget not saved"), (*RemoveWidgetData)->GetBoolField(TEXT("saved")));
	}

	const FString GetInfoAfterRemoveBody = MakeExecRequestBody(
		TEXT("smoke-umg-info-after-remove-001"),
		TEXT("umg_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *WidgetBlueprintAssetPath));
	FHttpSmokeResult GetInfoAfterRemoveResult;
	TestTrue(TEXT("UMG get_info after remove request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoAfterRemoveBody, GetInfoAfterRemoveResult));
	TestEqual(TEXT("UMG get_info after remove status code"), GetInfoAfterRemoveResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoAfterRemoveJson;
	TestTrue(TEXT("UMG get_info after remove response parses as JSON"), ParseJson(GetInfoAfterRemoveResult.Body, GetInfoAfterRemoveJson));
	TestTrue(TEXT("UMG get_info after remove root ok=true"), GetInfoAfterRemoveJson.IsValid() && GetInfoAfterRemoveJson->GetBoolField(TEXT("ok")));

	bool bFoundRemovedWidget = false;
	const TSharedPtr<FJsonObject>* GetInfoAfterRemoveData = nullptr;
	TestTrue(TEXT("UMG get_info after remove contains data object"), GetInfoAfterRemoveJson.IsValid() && GetInfoAfterRemoveJson->TryGetObjectField(TEXT("data"), GetInfoAfterRemoveData) && GetInfoAfterRemoveData && GetInfoAfterRemoveData->IsValid());
	if (GetInfoAfterRemoveData && GetInfoAfterRemoveData->IsValid())
	{
		const int32 WidgetCountAfterRemove = static_cast<int32>((*GetInfoAfterRemoveData)->GetIntegerField(TEXT("widget_count")));
		TestEqual(TEXT("UMG widget count restored after remove"), WidgetCountAfterRemove, WidgetCountBefore);

		const TArray<TSharedPtr<FJsonValue>>* Widgets = nullptr;
		TestTrue(TEXT("UMG get_info after remove contains widgets array"), (*GetInfoAfterRemoveData)->TryGetArrayField(TEXT("widgets"), Widgets) && Widgets != nullptr);
		if (Widgets)
		{
			for (const TSharedPtr<FJsonValue>& WidgetValue : *Widgets)
			{
				const TSharedPtr<FJsonObject>* WidgetObject = nullptr;
				if (!WidgetValue.IsValid() || !WidgetValue->TryGetObject(WidgetObject) || !WidgetObject || !WidgetObject->IsValid())
				{
					continue;
				}

				if ((*WidgetObject)->GetStringField(TEXT("name")) == RenamedWidgetName)
				{
					bFoundRemovedWidget = true;
					break;
				}
			}
		}
	}
	TestFalse(TEXT("UMG removed widget no longer present"), bFoundRemovedWidget);

	const FString CompileBody = MakeExecRequestBody(
		TEXT("smoke-umg-compile-001"),
		TEXT("umg_compile"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_messages\":true,\"max_messages\":16,\"save_after_compile\":false}"), *WidgetBlueprintAssetPath));
	FHttpSmokeResult CompileResult;
	TestTrue(TEXT("UMG compile request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CompileBody, CompileResult));
	TestEqual(TEXT("UMG compile status code"), CompileResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CompileJson;
	TestTrue(TEXT("UMG compile response parses as JSON"), ParseJson(CompileResult.Body, CompileJson));
	TestTrue(TEXT("UMG compile root ok=true"), CompileJson.IsValid() && CompileJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* CompileData = nullptr;
	TestTrue(TEXT("UMG compile contains data object"), CompileJson.IsValid() && CompileJson->TryGetObjectField(TEXT("data"), CompileData) && CompileData && CompileData->IsValid());
	if (CompileData && CompileData->IsValid())
	{
		TestFalse(TEXT("UMG compile reports no error"), (*CompileData)->GetBoolField(TEXT("has_error")));
		TestTrue(TEXT("UMG compile error count is zero"), static_cast<int32>((*CompileData)->GetIntegerField(TEXT("error_count"))) == 0);
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceBlueprintComponentSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.BlueprintComponentWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceBlueprintComponentSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString BlueprintAssetPath = MakeAutomationAssetPath(TEXT("BPComp"));
	const FString ComponentName = TEXT("SmokeMesh");
	const FString ComponentClass = TEXT("/Script/Engine.StaticMeshComponent");

	const FString CreateBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-component-create-001"),
		TEXT("blueprint_create"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"parent_class\":\"/Script/Engine.Actor\",\"compile_after_create\":false,\"open_editor\":false,\"save_after_create\":false}"),
			*BlueprintAssetPath));
	FHttpSmokeResult CreateResult;
	TestTrue(TEXT("Blueprint component create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateBody, CreateResult));
	TestEqual(TEXT("Blueprint component create status code"), CreateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CreateJson;
	TestTrue(TEXT("Blueprint component create response parses as JSON"), ParseJson(CreateResult.Body, CreateJson));
	TestTrue(TEXT("Blueprint component create root ok=true"), CreateJson.IsValid() && CreateJson->GetBoolField(TEXT("ok")));

	const FString InspectBeforeBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-component-inspect-before-001"),
		TEXT("blueprint_inspect_components"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *BlueprintAssetPath));
	FHttpSmokeResult InspectBeforeResult;
	TestTrue(TEXT("Blueprint inspect_components before request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, InspectBeforeBody, InspectBeforeResult));
	TestEqual(TEXT("Blueprint inspect_components before status code"), InspectBeforeResult.StatusCode, 200);

	TSharedPtr<FJsonObject> InspectBeforeJson;
	TestTrue(TEXT("Blueprint inspect_components before response parses as JSON"), ParseJson(InspectBeforeResult.Body, InspectBeforeJson));
	TestTrue(TEXT("Blueprint inspect_components before root ok=true"), InspectBeforeJson.IsValid() && InspectBeforeJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* InspectBeforeData = nullptr;
	TestTrue(TEXT("Blueprint inspect_components before contains data object"), InspectBeforeJson.IsValid() && InspectBeforeJson->TryGetObjectField(TEXT("data"), InspectBeforeData) && InspectBeforeData && InspectBeforeData->IsValid());

	const FString AddComponentBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-add-component-001"),
		TEXT("blueprint_add_component"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"component_class\":\"%s\",\"component_name\":\"%s\",\"compile_after_add\":false,\"save_after_add\":false}"),
			*BlueprintAssetPath,
			*ComponentClass,
			*ComponentName));
	FHttpSmokeResult AddComponentResult;
	TestTrue(TEXT("Blueprint add_component request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddComponentBody, AddComponentResult));
	TestEqual(TEXT("Blueprint add_component status code"), AddComponentResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddComponentJson;
	TestTrue(TEXT("Blueprint add_component response parses as JSON"), ParseJson(AddComponentResult.Body, AddComponentJson));
	TestTrue(TEXT("Blueprint add_component root ok=true"), AddComponentJson.IsValid() && AddComponentJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* AddComponentData = nullptr;
	TestTrue(TEXT("Blueprint add_component contains data object"), AddComponentJson.IsValid() && AddComponentJson->TryGetObjectField(TEXT("data"), AddComponentData) && AddComponentData && AddComponentData->IsValid());
	if (AddComponentData && AddComponentData->IsValid())
	{
		TestEqual(TEXT("Blueprint add_component component name"), (*AddComponentData)->GetStringField(TEXT("component_name")), ComponentName);
		TestEqual(TEXT("Blueprint add_component component class"), (*AddComponentData)->GetStringField(TEXT("component_class")), ComponentClass);
		TestFalse(TEXT("Blueprint add_component not saved"), (*AddComponentData)->GetBoolField(TEXT("saved")));
	}

	const FString InspectAfterAddBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-component-inspect-after-add-001"),
		TEXT("blueprint_inspect_components"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *BlueprintAssetPath));
	FHttpSmokeResult InspectAfterAddResult;
	TestTrue(TEXT("Blueprint inspect_components after add request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, InspectAfterAddBody, InspectAfterAddResult));
	TestEqual(TEXT("Blueprint inspect_components after add status code"), InspectAfterAddResult.StatusCode, 200);

	TSharedPtr<FJsonObject> InspectAfterAddJson;
	TestTrue(TEXT("Blueprint inspect_components after add response parses as JSON"), ParseJson(InspectAfterAddResult.Body, InspectAfterAddJson));
	TestTrue(TEXT("Blueprint inspect_components after add root ok=true"), InspectAfterAddJson.IsValid() && InspectAfterAddJson->GetBoolField(TEXT("ok")));

	bool bFoundAddedComponent = false;
	const TSharedPtr<FJsonObject>* InspectAfterAddData = nullptr;
	TestTrue(TEXT("Blueprint inspect_components after add contains data object"), InspectAfterAddJson.IsValid() && InspectAfterAddJson->TryGetObjectField(TEXT("data"), InspectAfterAddData) && InspectAfterAddData && InspectAfterAddData->IsValid());
	if (InspectAfterAddData && InspectAfterAddData->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
		TestTrue(TEXT("Blueprint inspect_components after add contains components array"), (*InspectAfterAddData)->TryGetArrayField(TEXT("components"), Components) && Components != nullptr);
		if (Components)
		{
			for (const TSharedPtr<FJsonValue>& ComponentValue : *Components)
			{
				const TSharedPtr<FJsonObject>* ComponentObject = nullptr;
				if (!ComponentValue.IsValid() || !ComponentValue->TryGetObject(ComponentObject) || !ComponentObject || !ComponentObject->IsValid())
				{
					continue;
				}

				if ((*ComponentObject)->GetStringField(TEXT("variable_name")) == ComponentName)
				{
					bFoundAddedComponent = true;
					TestEqual(TEXT("Blueprint added component class in inspect"), (*ComponentObject)->GetStringField(TEXT("component_class")), ComponentClass);
					break;
				}
			}
		}
	}
	TestTrue(TEXT("Blueprint inspect_components after add contains added component"), bFoundAddedComponent);

	const FString SetComponentPropertyBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-set-component-property-001"),
		TEXT("blueprint_set_component_property"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"component_name\":\"%s\",\"property_name\":\"bVisible\",\"value_text\":\"false\",\"compile_after_set\":false,\"save_after_set\":false}"),
			*BlueprintAssetPath,
			*ComponentName));
	FHttpSmokeResult SetComponentPropertyResult;
	TestTrue(TEXT("Blueprint set_component_property request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetComponentPropertyBody, SetComponentPropertyResult));
	TestEqual(TEXT("Blueprint set_component_property status code"), SetComponentPropertyResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SetComponentPropertyJson;
	TestTrue(TEXT("Blueprint set_component_property response parses as JSON"), ParseJson(SetComponentPropertyResult.Body, SetComponentPropertyJson));
	TestTrue(TEXT("Blueprint set_component_property root ok=true"), SetComponentPropertyJson.IsValid() && SetComponentPropertyJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* SetComponentPropertyData = nullptr;
	TestTrue(TEXT("Blueprint set_component_property contains data object"), SetComponentPropertyJson.IsValid() && SetComponentPropertyJson->TryGetObjectField(TEXT("data"), SetComponentPropertyData) && SetComponentPropertyData && SetComponentPropertyData->IsValid());
	if (SetComponentPropertyData && SetComponentPropertyData->IsValid())
	{
		TestEqual(TEXT("Blueprint set_component_property component name"), (*SetComponentPropertyData)->GetStringField(TEXT("component_name")), ComponentName);
		TestEqual(TEXT("Blueprint set_component_property property name"), (*SetComponentPropertyData)->GetStringField(TEXT("property_name")), FString(TEXT("bVisible")));
		TestTrue(TEXT("Blueprint set_component_property applied value contains false"), (*SetComponentPropertyData)->GetStringField(TEXT("applied_value_text")).Contains(TEXT("False"), ESearchCase::IgnoreCase));
	}

	const FString CompileAfterAddBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-component-compile-after-add-001"),
		TEXT("blueprint_compile"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_messages\":true,\"max_messages\":16,\"save_after_compile\":false}"), *BlueprintAssetPath));
	FHttpSmokeResult CompileAfterAddResult;
	TestTrue(TEXT("Blueprint compile after add request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CompileAfterAddBody, CompileAfterAddResult));
	TestEqual(TEXT("Blueprint compile after add status code"), CompileAfterAddResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CompileAfterAddJson;
	TestTrue(TEXT("Blueprint compile after add response parses as JSON"), ParseJson(CompileAfterAddResult.Body, CompileAfterAddJson));
	TestTrue(TEXT("Blueprint compile after add root ok=true"), CompileAfterAddJson.IsValid() && CompileAfterAddJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* CompileAfterAddData = nullptr;
	TestTrue(TEXT("Blueprint compile after add contains data object"), CompileAfterAddJson.IsValid() && CompileAfterAddJson->TryGetObjectField(TEXT("data"), CompileAfterAddData) && CompileAfterAddData && CompileAfterAddData->IsValid());
	if (CompileAfterAddData && CompileAfterAddData->IsValid())
	{
		TestFalse(TEXT("Blueprint compile after add reports no error"), (*CompileAfterAddData)->GetBoolField(TEXT("has_error")));
		TestTrue(TEXT("Blueprint compile after add error count is zero"), static_cast<int32>((*CompileAfterAddData)->GetIntegerField(TEXT("error_count"))) == 0);
	}

	const FString RemoveComponentBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-remove-component-001"),
		TEXT("blueprint_remove_component"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"component_name\":\"%s\",\"compile_after_remove\":false,\"save_after_remove\":false}"),
			*BlueprintAssetPath,
			*ComponentName));
	FHttpSmokeResult RemoveComponentResult;
	TestTrue(TEXT("Blueprint remove_component request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveComponentBody, RemoveComponentResult));
	TestEqual(TEXT("Blueprint remove_component status code"), RemoveComponentResult.StatusCode, 200);

	TSharedPtr<FJsonObject> RemoveComponentJson;
	TestTrue(TEXT("Blueprint remove_component response parses as JSON"), ParseJson(RemoveComponentResult.Body, RemoveComponentJson));
	TestTrue(TEXT("Blueprint remove_component root ok=true"), RemoveComponentJson.IsValid() && RemoveComponentJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* RemoveComponentData = nullptr;
	TestTrue(TEXT("Blueprint remove_component contains data object"), RemoveComponentJson.IsValid() && RemoveComponentJson->TryGetObjectField(TEXT("data"), RemoveComponentData) && RemoveComponentData && RemoveComponentData->IsValid());
	if (RemoveComponentData && RemoveComponentData->IsValid())
	{
		TestEqual(TEXT("Blueprint remove_component component name"), (*RemoveComponentData)->GetStringField(TEXT("component_name")), ComponentName);
		TestFalse(TEXT("Blueprint remove_component not saved"), (*RemoveComponentData)->GetBoolField(TEXT("saved")));
	}

	const FString InspectAfterRemoveBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-component-inspect-after-remove-001"),
		TEXT("blueprint_inspect_components"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *BlueprintAssetPath));
	FHttpSmokeResult InspectAfterRemoveResult;
	TestTrue(TEXT("Blueprint inspect_components after remove request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, InspectAfterRemoveBody, InspectAfterRemoveResult));
	TestEqual(TEXT("Blueprint inspect_components after remove status code"), InspectAfterRemoveResult.StatusCode, 200);

	TSharedPtr<FJsonObject> InspectAfterRemoveJson;
	TestTrue(TEXT("Blueprint inspect_components after remove response parses as JSON"), ParseJson(InspectAfterRemoveResult.Body, InspectAfterRemoveJson));
	TestTrue(TEXT("Blueprint inspect_components after remove root ok=true"), InspectAfterRemoveJson.IsValid() && InspectAfterRemoveJson->GetBoolField(TEXT("ok")));

	bool bFoundRemovedComponent = false;
	const TSharedPtr<FJsonObject>* InspectAfterRemoveData = nullptr;
	TestTrue(TEXT("Blueprint inspect_components after remove contains data object"), InspectAfterRemoveJson.IsValid() && InspectAfterRemoveJson->TryGetObjectField(TEXT("data"), InspectAfterRemoveData) && InspectAfterRemoveData && InspectAfterRemoveData->IsValid());
	if (InspectAfterRemoveData && InspectAfterRemoveData->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
		TestTrue(TEXT("Blueprint inspect_components after remove contains components array"), (*InspectAfterRemoveData)->TryGetArrayField(TEXT("components"), Components) && Components != nullptr);
		if (Components)
		{
			for (const TSharedPtr<FJsonValue>& ComponentValue : *Components)
			{
				const TSharedPtr<FJsonObject>* ComponentObject = nullptr;
				if (!ComponentValue.IsValid() || !ComponentValue->TryGetObject(ComponentObject) || !ComponentObject || !ComponentObject->IsValid())
				{
					continue;
				}

				if ((*ComponentObject)->GetStringField(TEXT("variable_name")) == ComponentName)
				{
					bFoundRemovedComponent = true;
					break;
				}
			}
		}
	}
	TestFalse(TEXT("Blueprint removed component no longer present"), bFoundRemovedComponent);

	const FString CompileAfterRemoveBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-component-compile-after-remove-001"),
		TEXT("blueprint_compile"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_messages\":true,\"max_messages\":16,\"save_after_compile\":false}"), *BlueprintAssetPath));
	FHttpSmokeResult CompileAfterRemoveResult;
	TestTrue(TEXT("Blueprint compile after remove request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CompileAfterRemoveBody, CompileAfterRemoveResult));
	TestEqual(TEXT("Blueprint compile after remove status code"), CompileAfterRemoveResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CompileAfterRemoveJson;
	TestTrue(TEXT("Blueprint compile after remove response parses as JSON"), ParseJson(CompileAfterRemoveResult.Body, CompileAfterRemoveJson));
	TestTrue(TEXT("Blueprint compile after remove root ok=true"), CompileAfterRemoveJson.IsValid() && CompileAfterRemoveJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* CompileAfterRemoveData = nullptr;
	TestTrue(TEXT("Blueprint compile after remove contains data object"), CompileAfterRemoveJson.IsValid() && CompileAfterRemoveJson->TryGetObjectField(TEXT("data"), CompileAfterRemoveData) && CompileAfterRemoveData && CompileAfterRemoveData->IsValid());
	if (CompileAfterRemoveData && CompileAfterRemoveData->IsValid())
	{
		TestFalse(TEXT("Blueprint compile after remove reports no error"), (*CompileAfterRemoveData)->GetBoolField(TEXT("has_error")));
		TestTrue(TEXT("Blueprint compile after remove error count is zero"), static_cast<int32>((*CompileAfterRemoveData)->GetIntegerField(TEXT("error_count"))) == 0);
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceBlueprintFolderSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.BlueprintFolderWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceBlueprintFolderSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString BlueprintAssetPath = MakeAutomationAssetPath(TEXT("BPFolder"));
	FString RelativeFolder = BlueprintAssetPath;
	while (RelativeFolder.StartsWith(TEXT("/")))
	{
		RelativeFolder.RightChopInline(1);
	}
	const FString FolderPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("ActorBlueprint"), RelativeFolder));
	IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	IFileManager::Get().MakeDirectory(*FPaths::Combine(FolderPath, TEXT("members")), true);
	IFileManager::Get().MakeDirectory(*FPaths::Combine(FolderPath, TEXT("components")), true);
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
			TEXT("{\n  \"format_version\": 1,\n  \"asset_kind\": \"actor_blueprint\",\n  \"asset_path\": \"%s\",\n  \"asset_name\": \"%s\",\n  \"parent_class\": \"/Script/Engine.Actor\"\n}\n"),
			*BlueprintAssetPath,
			*FPackageName::GetLongPackageAssetName(BlueprintAssetPath))))
	{
		return false;
	}

	if (!SaveUtf8Json(
		FPaths::Combine(FolderPath, TEXT("members"), TEXT("variables.json")),
		TEXT("{\n  \"variables\": [\n    {\n      \"name\": \"SmokeFlag\",\n      \"type\": { \"pin_category\": \"bool\" },\n      \"default_value\": \"false\",\n      \"instance_editable\": true\n    },\n    {\n      \"name\": \"SmokeVector\",\n      \"type\": { \"pin_category\": \"vector\" },\n      \"default_value\": \"(X=1.000000,Y=2.000000,Z=3.000000)\",\n      \"instance_editable\": true\n    },\n    {\n      \"name\": \"SmokeRotator\",\n      \"type\": { \"pin_category\": \"rotator\" },\n      \"default_value\": \"(Pitch=10.000000,Yaw=20.000000,Roll=30.000000)\",\n      \"instance_editable\": false\n    },\n    {\n      \"name\": \"SmokeInts\",\n      \"type\": { \"pin_category\": \"int\", \"container_type\": \"array\" },\n      \"default_value\": \"\",\n      \"instance_editable\": false\n    },\n    {\n      \"name\": \"SmokeScores\",\n      \"type\": { \"pin_category\": \"string\", \"container_type\": \"map\", \"value_type\": { \"pin_category\": \"int\" } },\n      \"default_value\": \"\",\n      \"instance_editable\": false\n    }\n  ]\n}\n")))
	{
		return false;
	}

	if (!SaveUtf8Json(
		FPaths::Combine(FolderPath, TEXT("members"), TEXT("delegates.json")),
		TEXT("{\n  \"delegates\": []\n}\n")))
	{
		return false;
	}

	if (!SaveUtf8Json(
		FPaths::Combine(FolderPath, TEXT("members"), TEXT("defaults.json")),
		TEXT("{\n  \"defaults\": []\n}\n")))
	{
		return false;
	}

	if (!SaveUtf8Json(
		FPaths::Combine(FolderPath, TEXT("components"), TEXT("tree.json")),
		TEXT("{\n  \"components\": [\n    {\n      \"name\": \"DefaultSceneRoot\",\n      \"class\": \"/Script/Engine.SceneComponent\",\n      \"parent\": \"\",\n      \"properties\": []\n    },\n    {\n      \"name\": \"SmokeMesh\",\n      \"class\": \"/Script/Engine.StaticMeshComponent\",\n      \"parent\": \"DefaultSceneRoot\",\n      \"properties\": [\n        { \"property_name\": \"RelativeLocation\", \"value_text\": \"(X=0.000000,Y=0.000000,Z=0.000000)\" },\n        { \"property_name\": \"RelativeRotation\", \"value_text\": \"(Pitch=0.000000,Yaw=0.000000,Roll=0.000000)\" },\n        { \"property_name\": \"RelativeScale3D\", \"value_text\": \"(X=1.000000,Y=0.250000,Z=2.000000)\" },\n        { \"property_name\": \"StaticMesh\", \"value_text\": \"StaticMesh'/Engine/BasicShapes/Cube.Cube'\" }\n      ]\n    }\n  ]\n}\n")))
	{
		return false;
	}

	if (!SaveUtf8Json(
		FPaths::Combine(FolderPath, TEXT("graphs"), TEXT("EventGraph.json")),
		TEXT("{\n  \"graph\": {\n    \"name\": \"EventGraph\",\n    \"graph_kind\": \"event_graph\"\n  },\n  \"nodes\": [\n    {\n      \"id\": \"event_begin_play\",\n      \"node_type\": \"event\",\n      \"event_name\": \"ReceiveBeginPlay\",\n      \"event_class\": \"/Script/Engine.Actor\",\n      \"pos\": { \"x\": 0, \"y\": 0 }\n    },\n    {\n      \"id\": \"call_print_string\",\n      \"node_type\": \"call_function\",\n      \"function_owner_class\": \"/Script/Engine.KismetSystemLibrary\",\n      \"function_name\": \"PrintString\",\n      \"pos\": { \"x\": 260, \"y\": 0 },\n      \"pin_defaults\": [\n        { \"pin_name\": \"InString\", \"default_value\": \"Folder workflow smoke\" }\n      ]\n    },\n    {\n      \"id\": \"custom_smoke_event\",\n      \"node_type\": \"custom_event\",\n      \"event_name\": \"SmokeCustomEvent\",\n      \"pos\": { \"x\": 0, \"y\": 180 }\n    }\n  ],\n  \"edges\": [\n    {\n      \"from\": { \"node\": \"event_begin_play\", \"pin\": \"then\" },\n      \"to\": { \"node\": \"call_print_string\", \"pin\": \"execute\" }\n    }\n  ]\n}\n")))
	{
		return false;
	}

	if (!SaveUtf8Json(
		FPaths::Combine(FolderPath, TEXT("validation"), TEXT("checks.json")),
		TEXT("{\n  \"checks\": [\n    { \"kind\": \"compile_success\" }\n  ]\n}\n")))
	{
		return false;
	}

	const FString ApplyBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-folder-apply-001"),
		TEXT("blueprint_apply_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"compile_after_apply\":true,\"save_after_apply\":false}"), *BlueprintAssetPath));
	FHttpSmokeResult ApplyResult;
	TestTrue(TEXT("Blueprint folder apply request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ApplyBody, ApplyResult, 15.0));
	TestEqual(TEXT("Blueprint folder apply status code"), ApplyResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ApplyJson;
	TestTrue(TEXT("Blueprint folder apply parses JSON"), ParseJson(ApplyResult.Body, ApplyJson));
	TestTrue(TEXT("Blueprint folder apply root ok=true"), ApplyJson.IsValid() && ApplyJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* ApplyData = nullptr;
	TestTrue(TEXT("Blueprint folder apply contains data object"), ApplyJson.IsValid() && ApplyJson->TryGetObjectField(TEXT("data"), ApplyData) && ApplyData && ApplyData->IsValid());
	if (ApplyData && ApplyData->IsValid())
	{
		TestTrue(TEXT("Blueprint folder apply added variable"), (*ApplyData)->GetIntegerField(TEXT("variables_added")) >= 1);
		TestTrue(TEXT("Blueprint folder apply added component"), (*ApplyData)->GetIntegerField(TEXT("components_added")) >= 1);
		TestTrue(TEXT("Blueprint folder apply rebuilt graph"), (*ApplyData)->GetIntegerField(TEXT("graphs_rebuilt")) >= 1);
		TestTrue(TEXT("Blueprint folder apply created nodes"), (*ApplyData)->GetIntegerField(TEXT("nodes_created")) >= 3);
		TestTrue(TEXT("Blueprint folder apply created edge"), (*ApplyData)->GetIntegerField(TEXT("edges_created")) >= 1);
	}

	const FString CompileBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-folder-compile-001"),
		TEXT("blueprint_compile"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_messages\":true,\"max_messages\":16,\"save_after_compile\":false}"), *BlueprintAssetPath));
	FHttpSmokeResult CompileResult;
	TestTrue(TEXT("Blueprint folder compile request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CompileBody, CompileResult, 10.0));
	TestEqual(TEXT("Blueprint folder compile status code"), CompileResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CompileJson;
	TestTrue(TEXT("Blueprint folder compile parses JSON"), ParseJson(CompileResult.Body, CompileJson));
	TestTrue(TEXT("Blueprint folder compile root ok=true"), CompileJson.IsValid() && CompileJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* CompileData = nullptr;
	TestTrue(TEXT("Blueprint folder compile contains data object"), CompileJson.IsValid() && CompileJson->TryGetObjectField(TEXT("data"), CompileData) && CompileData && CompileData->IsValid());
	if (CompileData && CompileData->IsValid())
	{
		TestFalse(TEXT("Blueprint folder compile has no error"), (*CompileData)->GetBoolField(TEXT("has_error")));
	}

	const FString ExportBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-folder-export-001"),
		TEXT("blueprint_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clean_output_dir\":true,\"include_validation\":true}"), *BlueprintAssetPath));
	FHttpSmokeResult ExportResult;
	TestTrue(TEXT("Blueprint folder export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ExportBody, ExportResult, 15.0));
	TestEqual(TEXT("Blueprint folder export status code"), ExportResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ExportJson;
	TestTrue(TEXT("Blueprint folder export parses JSON"), ParseJson(ExportResult.Body, ExportJson));
	TestTrue(TEXT("Blueprint folder export root ok=true"), ExportJson.IsValid() && ExportJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* ExportData = nullptr;
	TestTrue(TEXT("Blueprint folder export contains data object"), ExportJson.IsValid() && ExportJson->TryGetObjectField(TEXT("data"), ExportData) && ExportData && ExportData->IsValid());
	if (ExportData && ExportData->IsValid())
	{
		TestEqual(TEXT("Blueprint folder export folder path"), (*ExportData)->GetStringField(TEXT("folder_path")), FolderPath);
		TestTrue(TEXT("Blueprint folder export file count reasonable"), (*ExportData)->GetIntegerField(TEXT("file_count")) >= 6);
	}

	TSharedPtr<FJsonObject> ExportedAssetJson;
	{
		const FString AssetJsonPath = FPaths::Combine(FolderPath, TEXT("asset.json"));
		FString ExportedAssetText;
		TestTrue(TEXT("Blueprint folder exported asset.json exists"), FFileHelper::LoadFileToString(ExportedAssetText, *AssetJsonPath));
		TestTrue(TEXT("Blueprint folder exported asset.json parses"), ParseJson(ExportedAssetText, ExportedAssetJson));
		if (ExportedAssetJson.IsValid())
		{
			TestEqual(TEXT("Blueprint folder exported asset path"), ExportedAssetJson->GetStringField(TEXT("asset_path")), BlueprintAssetPath);
			TestEqual(TEXT("Blueprint folder exported asset kind"), ExportedAssetJson->GetStringField(TEXT("asset_kind")), FString(TEXT("actor_blueprint")));
		}
	}

	TSharedPtr<FJsonObject> ExportedVariablesJson;
	{
		const FString VariablesJsonPath = FPaths::Combine(FolderPath, TEXT("members"), TEXT("variables.json"));
		FString ExportedVariablesText;
		TestTrue(TEXT("Blueprint folder exported variables.json exists"), FFileHelper::LoadFileToString(ExportedVariablesText, *VariablesJsonPath));
		TestTrue(TEXT("Blueprint folder exported variables.json parses"), ParseJson(ExportedVariablesText, ExportedVariablesJson));
		if (ExportedVariablesJson.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Variables = nullptr;
			TestTrue(TEXT("Blueprint folder exported variables array exists"), ExportedVariablesJson->TryGetArrayField(TEXT("variables"), Variables) && Variables);
			if (Variables)
			{
				bool bFoundSmokeFlag = false;
				bool bFoundSmokeVector = false;
				bool bFoundSmokeRotator = false;
				bool bFoundSmokeScoresMapValue = false;
				for (const TSharedPtr<FJsonValue>& VariableValue : *Variables)
				{
					const TSharedPtr<FJsonObject>* VariableObj = nullptr;
					if (!VariableValue.IsValid() || !VariableValue->TryGetObject(VariableObj) || !VariableObj || !VariableObj->IsValid())
					{
						continue;
					}
					const FString ExportedVariableName = (*VariableObj)->GetStringField(TEXT("name"));
					if (ExportedVariableName == TEXT("SmokeFlag"))
					{
						bFoundSmokeFlag = true;
					}
					else if (ExportedVariableName == TEXT("SmokeVector"))
					{
						bFoundSmokeVector = true;
					}
					else if (ExportedVariableName == TEXT("SmokeRotator"))
					{
						bFoundSmokeRotator = true;
					}
					else if (ExportedVariableName == TEXT("SmokeScores"))
					{
						const TSharedPtr<FJsonObject>* TypeObj = nullptr;
						if ((*VariableObj)->TryGetObjectField(TEXT("type"), TypeObj) && TypeObj && TypeObj->IsValid())
						{
							const TSharedPtr<FJsonObject>* ValueTypeObj = nullptr;
							bFoundSmokeScoresMapValue = (*TypeObj)->GetStringField(TEXT("container_type")) == TEXT("map")
								&& (*TypeObj)->TryGetObjectField(TEXT("value_type"), ValueTypeObj)
								&& ValueTypeObj
								&& ValueTypeObj->IsValid()
								&& (*ValueTypeObj)->GetStringField(TEXT("pin_category")) == TEXT("int");
						}
					}
				}
				TestTrue(TEXT("Blueprint folder exported SmokeFlag variable"), bFoundSmokeFlag);
				TestTrue(TEXT("Blueprint folder exported SmokeVector variable"), bFoundSmokeVector);
				TestTrue(TEXT("Blueprint folder exported SmokeRotator variable"), bFoundSmokeRotator);
				TestTrue(TEXT("Blueprint folder exported map value type"), bFoundSmokeScoresMapValue);
			}
		}
	}

	TSharedPtr<FJsonObject> ExportedEventGraphJson;
	{
		const FString EventGraphJsonPath = FPaths::Combine(FolderPath, TEXT("graphs"), TEXT("EventGraph.json"));
		FString ExportedEventGraphText;
		TestTrue(TEXT("Blueprint folder exported EventGraph.json exists"), FFileHelper::LoadFileToString(ExportedEventGraphText, *EventGraphJsonPath));
		TestTrue(TEXT("Blueprint folder exported EventGraph.json parses"), ParseJson(ExportedEventGraphText, ExportedEventGraphJson));
		if (ExportedEventGraphJson.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
			TestTrue(TEXT("Blueprint folder exported EventGraph nodes array exists"), ExportedEventGraphJson->TryGetArrayField(TEXT("nodes"), Nodes) && Nodes);
			if (Nodes)
			{
				bool bFoundCustomEvent = false;
				for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
				{
					const TSharedPtr<FJsonObject>* NodeObj = nullptr;
					if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObj) || !NodeObj || !NodeObj->IsValid())
					{
						continue;
					}

					FString NodeType;
					(*NodeObj)->TryGetStringField(TEXT("node_type"), NodeType);
					if (NodeType.Equals(TEXT("custom_event"), ESearchCase::IgnoreCase)
						&& (*NodeObj)->GetStringField(TEXT("event_name")) == TEXT("SmokeCustomEvent"))
					{
						bFoundCustomEvent = true;
						break;
					}
				}
				TestTrue(TEXT("Blueprint folder export preserves custom_event node"), bFoundCustomEvent);
			}
		}
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceBlueprintGraphSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.BlueprintGraphWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceBlueprintGraphSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString BlueprintAssetPath = MakeAutomationAssetPath(TEXT("BPGraph"));
	const FString CustomEventName = TEXT("SmokeCustomEvent");
	const FString StringValue = TEXT("SmokeGraphMessage");

	auto FindNodeInInspect = [](const TSharedPtr<FJsonObject>* InspectData, const FString& NodeGuid, TSharedPtr<FJsonObject>& OutNodeObject) -> bool
	{
		if (!InspectData || !InspectData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
		if (!(*InspectData)->TryGetArrayField(TEXT("graphs"), Graphs) || !Graphs)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& GraphValue : *Graphs)
		{
			const TSharedPtr<FJsonObject>* GraphObject = nullptr;
			if (!GraphValue.IsValid() || !GraphValue->TryGetObject(GraphObject) || !GraphObject || !GraphObject->IsValid())
			{
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
			if (!(*GraphObject)->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
			{
				continue;
			}

			for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
			{
				const TSharedPtr<FJsonObject>* NodeObject = nullptr;
				if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObject) || !NodeObject || !NodeObject->IsValid())
				{
					continue;
				}

				if ((*NodeObject)->GetStringField(TEXT("node_guid")) == NodeGuid)
				{
					OutNodeObject = *NodeObject;
					return true;
				}
			}
		}

		return false;
	};

	auto FindPinName = [](const TSharedPtr<FJsonObject>& NodeObject, const FString& Direction, const FString& Category, FString& OutPinName) -> bool
	{
		if (!NodeObject.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
		if (!NodeObject->TryGetArrayField(TEXT("pins"), Pins) || !Pins)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& PinValue : *Pins)
		{
			const TSharedPtr<FJsonObject>* PinObject = nullptr;
			if (!PinValue.IsValid() || !PinValue->TryGetObject(PinObject) || !PinObject || !PinObject->IsValid())
			{
				continue;
			}

			if ((*PinObject)->GetStringField(TEXT("direction")) == Direction &&
				(*PinObject)->GetStringField(TEXT("category")).Equals(Category, ESearchCase::IgnoreCase))
			{
				OutPinName = (*PinObject)->GetStringField(TEXT("name"));
				return true;
			}
		}

		return false;
	};

	auto FindNodeByClassContains = [](const TSharedPtr<FJsonObject>* InspectData, const FString& ClassNeedle, TSharedPtr<FJsonObject>& OutNodeObject) -> bool
	{
		if (!InspectData || !InspectData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
		if (!(*InspectData)->TryGetArrayField(TEXT("graphs"), Graphs) || !Graphs)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& GraphValue : *Graphs)
		{
			const TSharedPtr<FJsonObject>* GraphObject = nullptr;
			if (!GraphValue.IsValid() || !GraphValue->TryGetObject(GraphObject) || !GraphObject || !GraphObject->IsValid())
			{
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
			if (!(*GraphObject)->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
			{
				continue;
			}

			for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
			{
				const TSharedPtr<FJsonObject>* NodeObject = nullptr;
				if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObject) || !NodeObject || !NodeObject->IsValid())
				{
					continue;
				}

				if ((*NodeObject)->GetStringField(TEXT("class")).Contains(ClassNeedle))
				{
					OutNodeObject = *NodeObject;
					return true;
				}
			}
		}

		return false;
	};

	auto FindPinByName = [](const TSharedPtr<FJsonObject>& NodeObject, const FString& PinName, TSharedPtr<FJsonObject>& OutPinObject) -> bool
	{
		if (!NodeObject.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
		if (!NodeObject->TryGetArrayField(TEXT("pins"), Pins) || !Pins)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& PinValue : *Pins)
		{
			const TSharedPtr<FJsonObject>* PinObject = nullptr;
			if (!PinValue.IsValid() || !PinValue->TryGetObject(PinObject) || !PinObject || !PinObject->IsValid())
			{
				continue;
			}

			if ((*PinObject)->GetStringField(TEXT("name")) == PinName)
			{
				OutPinObject = *PinObject;
				return true;
			}
		}

		return false;
	};

	const FString CreateBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-graph-create-001"),
		TEXT("blueprint_create"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"parent_class\":\"/Script/Engine.Actor\",\"compile_after_create\":false,\"open_editor\":false,\"save_after_create\":false}"), *BlueprintAssetPath));
	FHttpSmokeResult CreateResult;
	TestTrue(TEXT("Blueprint graph create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateBody, CreateResult));
	TestEqual(TEXT("Blueprint graph create status code"), CreateResult.StatusCode, 200);

	const FString AddCustomEventBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-graph-custom-event-001"),
		TEXT("blueprint_add_custom_event_node"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"event_name\":\"%s\",\"graph_name\":\"EventGraph\",\"pos_x\":-320,\"pos_y\":0,\"compile_after_add\":false,\"save_after_add\":false}"), *BlueprintAssetPath, *CustomEventName));
	FHttpSmokeResult AddCustomEventResult;
	TestTrue(TEXT("Blueprint graph add_custom_event request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddCustomEventBody, AddCustomEventResult));
	TestEqual(TEXT("Blueprint graph add_custom_event status code"), AddCustomEventResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddCustomEventJson;
	TestTrue(TEXT("Blueprint graph add_custom_event response parses as JSON"), ParseJson(AddCustomEventResult.Body, AddCustomEventJson));
	TestTrue(TEXT("Blueprint graph add_custom_event root ok=true"), AddCustomEventJson.IsValid() && AddCustomEventJson->GetBoolField(TEXT("ok")));

	FString CustomEventNodeGuid;
	const TSharedPtr<FJsonObject>* AddCustomEventData = nullptr;
	TestTrue(TEXT("Blueprint graph add_custom_event contains data object"), AddCustomEventJson.IsValid() && AddCustomEventJson->TryGetObjectField(TEXT("data"), AddCustomEventData) && AddCustomEventData && AddCustomEventData->IsValid());
	if (AddCustomEventData && AddCustomEventData->IsValid())
	{
		CustomEventNodeGuid = (*AddCustomEventData)->GetStringField(TEXT("node_guid"));
		TestTrue(TEXT("Blueprint graph custom event guid non-empty"), !CustomEventNodeGuid.IsEmpty());
	}

	const FString AddCallFunctionBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-graph-call-function-001"),
		TEXT("blueprint_add_call_function_node"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"EventGraph\",\"function_owner_class\":\"/Script/Engine.KismetSystemLibrary\",\"function_name\":\"PrintString\",\"pos_x\":32,\"pos_y\":0,\"compile_after_add\":false,\"save_after_add\":false}"), *BlueprintAssetPath));
	FHttpSmokeResult AddCallFunctionResult;
	TestTrue(TEXT("Blueprint graph add_call_function request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddCallFunctionBody, AddCallFunctionResult));
	TestEqual(TEXT("Blueprint graph add_call_function status code"), AddCallFunctionResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddCallFunctionJson;
	TestTrue(TEXT("Blueprint graph add_call_function response parses as JSON"), ParseJson(AddCallFunctionResult.Body, AddCallFunctionJson));
	TestTrue(TEXT("Blueprint graph add_call_function root ok=true"), AddCallFunctionJson.IsValid() && AddCallFunctionJson->GetBoolField(TEXT("ok")));

	FString CallFunctionNodeGuid;
	const TSharedPtr<FJsonObject>* AddCallFunctionData = nullptr;
	TestTrue(TEXT("Blueprint graph add_call_function contains data object"), AddCallFunctionJson.IsValid() && AddCallFunctionJson->TryGetObjectField(TEXT("data"), AddCallFunctionData) && AddCallFunctionData && AddCallFunctionData->IsValid());
	if (AddCallFunctionData && AddCallFunctionData->IsValid())
	{
		CallFunctionNodeGuid = (*AddCallFunctionData)->GetStringField(TEXT("node_guid"));
		TestTrue(TEXT("Blueprint graph call function guid non-empty"), !CallFunctionNodeGuid.IsEmpty());
	}

	const FString AddGenericNodeBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-graph-generic-node-001"),
		TEXT("blueprint_add_node_by_class"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"EventGraph\",\"node_class\":\"/Script/BlueprintGraph.K2Node_ExecutionSequence\",\"pos_x\":220,\"pos_y\":96,\"compile_after_add\":false,\"save_after_add\":false}"), *BlueprintAssetPath));
	FHttpSmokeResult AddGenericNodeResult;
	TestTrue(TEXT("Blueprint graph add_node_by_class request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddGenericNodeBody, AddGenericNodeResult));
	TestEqual(TEXT("Blueprint graph add_node_by_class status code"), AddGenericNodeResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddGenericNodeJson;
	TestTrue(TEXT("Blueprint graph add_node_by_class response parses as JSON"), ParseJson(AddGenericNodeResult.Body, AddGenericNodeJson));
	FString GenericNodeGuid;
	const TSharedPtr<FJsonObject>* AddGenericNodeData = nullptr;
	TestTrue(TEXT("Blueprint graph add_node_by_class contains data object"), AddGenericNodeJson.IsValid() && AddGenericNodeJson->TryGetObjectField(TEXT("data"), AddGenericNodeData) && AddGenericNodeData && AddGenericNodeData->IsValid());
	if (AddGenericNodeData && AddGenericNodeData->IsValid())
	{
		GenericNodeGuid = (*AddGenericNodeData)->GetStringField(TEXT("node_guid"));
		TestTrue(TEXT("Blueprint graph generic node guid non-empty"), !GenericNodeGuid.IsEmpty());
	}

	const FString InspectNodesBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-graph-inspect-001"),
		TEXT("blueprint_inspect_nodes"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"EventGraph\",\"include_pins\":true,\"limit_per_graph\":64}"), *BlueprintAssetPath));
	FHttpSmokeResult InspectNodesResult;
	TestTrue(TEXT("Blueprint graph inspect_nodes request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, InspectNodesBody, InspectNodesResult));
	TestEqual(TEXT("Blueprint graph inspect_nodes status code"), InspectNodesResult.StatusCode, 200);

	TSharedPtr<FJsonObject> InspectNodesJson;
	TestTrue(TEXT("Blueprint graph inspect_nodes response parses as JSON"), ParseJson(InspectNodesResult.Body, InspectNodesJson));
	TestTrue(TEXT("Blueprint graph inspect_nodes root ok=true"), InspectNodesJson.IsValid() && InspectNodesJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* InspectNodesData = nullptr;
	TestTrue(TEXT("Blueprint graph inspect_nodes contains data object"), InspectNodesJson.IsValid() && InspectNodesJson->TryGetObjectField(TEXT("data"), InspectNodesData) && InspectNodesData && InspectNodesData->IsValid());

	TSharedPtr<FJsonObject> CustomEventNodeObject;
	TSharedPtr<FJsonObject> CallFunctionNodeObject;
	TSharedPtr<FJsonObject> GenericNodeObject;
	TestTrue(TEXT("Blueprint graph inspect_nodes finds custom event node"), FindNodeInInspect(InspectNodesData, CustomEventNodeGuid, CustomEventNodeObject));
	TestTrue(TEXT("Blueprint graph inspect_nodes finds call function node"), FindNodeInInspect(InspectNodesData, CallFunctionNodeGuid, CallFunctionNodeObject));
	TestTrue(TEXT("Blueprint graph inspect_nodes finds generic node"), FindNodeInInspect(InspectNodesData, GenericNodeGuid, GenericNodeObject));
	if (GenericNodeObject.IsValid())
	{
		TestTrue(TEXT("Blueprint graph generic node class contains execution sequence"), GenericNodeObject->GetStringField(TEXT("class")).Contains(TEXT("K2Node_ExecutionSequence")));
	}

	FString EventExecPinName;
	FString CallExecPinName;
	FString CallStringPinName;
	TestTrue(TEXT("Blueprint graph custom event has exec output pin"), FindPinName(CustomEventNodeObject, TEXT("output"), TEXT("exec"), EventExecPinName));
	TestTrue(TEXT("Blueprint graph call function has exec input pin"), FindPinName(CallFunctionNodeObject, TEXT("input"), TEXT("exec"), CallExecPinName));
	TestTrue(TEXT("Blueprint graph call function has string input pin"), FindPinName(CallFunctionNodeObject, TEXT("input"), TEXT("string"), CallStringPinName));

	const FString ConnectPinsBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-graph-connect-001"),
		TEXT("blueprint_connect_pins"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"EventGraph\",\"from_node_guid\":\"%s\",\"from_pin\":\"%s\",\"to_node_guid\":\"%s\",\"to_pin\":\"%s\",\"compile_after_connect\":false,\"save_after_connect\":false}"), *BlueprintAssetPath, *CustomEventNodeGuid, *EventExecPinName, *CallFunctionNodeGuid, *CallExecPinName));
	FHttpSmokeResult ConnectPinsResult;
	TestTrue(TEXT("Blueprint graph connect_pins request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ConnectPinsBody, ConnectPinsResult));
	TestEqual(TEXT("Blueprint graph connect_pins status code"), ConnectPinsResult.StatusCode, 200);

	const FString SetPinDefaultBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-graph-set-pin-001"),
		TEXT("blueprint_set_pin_default_value"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"EventGraph\",\"node_guid\":\"%s\",\"pin_name\":\"%s\",\"default_value\":\"%s\",\"compile_after_set\":false,\"save_after_set\":false}"), *BlueprintAssetPath, *CallFunctionNodeGuid, *CallStringPinName, *StringValue));
	FHttpSmokeResult SetPinDefaultResult;
	TestTrue(TEXT("Blueprint graph set_pin_default_value request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetPinDefaultBody, SetPinDefaultResult));
	TestEqual(TEXT("Blueprint graph set_pin_default_value status code"), SetPinDefaultResult.StatusCode, 200);

	const FString LayoutSelfTestBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-graph-layout-self-test-001"),
		TEXT("blueprint_layout_graph_self_test"),
		TEXT("{}"));
	FHttpSmokeResult LayoutSelfTestResult;
	TestTrue(TEXT("Blueprint graph layout self-test request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, LayoutSelfTestBody, LayoutSelfTestResult));
	TestEqual(TEXT("Blueprint graph layout self-test status code"), LayoutSelfTestResult.StatusCode, 200);

	TSharedPtr<FJsonObject> LayoutSelfTestJson;
	TestTrue(TEXT("Blueprint graph layout self-test response parses as JSON"), ParseJson(LayoutSelfTestResult.Body, LayoutSelfTestJson));
	TestTrue(TEXT("Blueprint graph layout self-test root ok=true"), LayoutSelfTestJson.IsValid() && LayoutSelfTestJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* LayoutSelfTestData = nullptr;
	TestTrue(TEXT("Blueprint graph layout self-test contains data object"), LayoutSelfTestJson.IsValid() && LayoutSelfTestJson->TryGetObjectField(TEXT("data"), LayoutSelfTestData) && LayoutSelfTestData && LayoutSelfTestData->IsValid());
	if (LayoutSelfTestData && LayoutSelfTestData->IsValid())
	{
		TestTrue(TEXT("Blueprint graph layout self-test all passed"), (*LayoutSelfTestData)->GetBoolField(TEXT("all_passed")));
		TestEqual(TEXT("Blueprint graph layout self-test failed count is zero"), static_cast<int32>((*LayoutSelfTestData)->GetIntegerField(TEXT("failed_count"))), 0);
		TestTrue(TEXT("Blueprint graph layout self-test covers subalgorithms"), static_cast<int32>((*LayoutSelfTestData)->GetIntegerField(TEXT("case_count"))) >= 6);
	}

	const FString LayoutAllGraphsBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-layout-all-graphs-001"),
		TEXT("blueprint_layout_all_graphs"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_names\":[\"EventGraph\"],\"dry_run\":true,\"compile_after_layout\":false,\"save_after_layout\":false,\"stop_on_error\":true}"), *BlueprintAssetPath));
	FHttpSmokeResult LayoutAllGraphsResult;
	TestTrue(TEXT("Blueprint layout_all_graphs request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, LayoutAllGraphsBody, LayoutAllGraphsResult));
	TestEqual(TEXT("Blueprint layout_all_graphs status code"), LayoutAllGraphsResult.StatusCode, 200);

	TSharedPtr<FJsonObject> LayoutAllGraphsJson;
	TestTrue(TEXT("Blueprint layout_all_graphs response parses as JSON"), ParseJson(LayoutAllGraphsResult.Body, LayoutAllGraphsJson));
	TestTrue(TEXT("Blueprint layout_all_graphs root ok=true"), LayoutAllGraphsJson.IsValid() && LayoutAllGraphsJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* LayoutAllGraphsData = nullptr;
	TestTrue(TEXT("Blueprint layout_all_graphs contains data object"), LayoutAllGraphsJson.IsValid() && LayoutAllGraphsJson->TryGetObjectField(TEXT("data"), LayoutAllGraphsData) && LayoutAllGraphsData && LayoutAllGraphsData->IsValid());
	if (LayoutAllGraphsData && LayoutAllGraphsData->IsValid())
	{
		TestEqual(TEXT("Blueprint layout_all_graphs has no failed graphs"), static_cast<int32>((*LayoutAllGraphsData)->GetIntegerField(TEXT("failed_graph_count"))), 0);
		TestTrue(TEXT("Blueprint layout_all_graphs processed requested graph"), static_cast<int32>((*LayoutAllGraphsData)->GetIntegerField(TEXT("processed_graph_count"))) >= 1);
		TestTrue(TEXT("Blueprint layout_all_graphs reports graph results"), (*LayoutAllGraphsData)->HasField(TEXT("graphs")));
	}

	const FString NodeGraphListBody = MakeExecRequestBody(
		TEXT("smoke-node-graph-list-blueprint-001"),
		TEXT("node_graph_list"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_nested_graphs\":false}"), *BlueprintAssetPath));
	FHttpSmokeResult NodeGraphListResult;
	TestTrue(TEXT("Node graph list request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, NodeGraphListBody, NodeGraphListResult));
	TestEqual(TEXT("Node graph list status code"), NodeGraphListResult.StatusCode, 200);

	TSharedPtr<FJsonObject> NodeGraphListJson;
	TestTrue(TEXT("Node graph list response parses as JSON"), ParseJson(NodeGraphListResult.Body, NodeGraphListJson));
	TestTrue(TEXT("Node graph list root ok=true"), NodeGraphListJson.IsValid() && NodeGraphListJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* NodeGraphListData = nullptr;
	TestTrue(TEXT("Node graph list contains data object"), NodeGraphListJson.IsValid() && NodeGraphListJson->TryGetObjectField(TEXT("data"), NodeGraphListData) && NodeGraphListData && NodeGraphListData->IsValid());
	if (NodeGraphListData && NodeGraphListData->IsValid())
	{
		TestEqual(TEXT("Node graph list delegates blueprint adapter"), (*NodeGraphListData)->GetStringField(TEXT("adapter")), FString(TEXT("k2_blueprint")));
		TestTrue(TEXT("Node graph list returns graphs"), static_cast<int32>((*NodeGraphListData)->GetIntegerField(TEXT("graph_count"))) >= 1);
	}

	const FString NodeGraphLayoutBody = MakeExecRequestBody(
		TEXT("smoke-node-graph-layout-blueprint-001"),
		TEXT("node_graph_layout"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_selector\":{\"mode\":\"single\",\"graph_name\":\"EventGraph\"},\"dry_run\":true,\"compile_after_layout\":false,\"save_after_layout\":false,\"stop_on_error\":true}"), *BlueprintAssetPath));
	FHttpSmokeResult NodeGraphLayoutResult;
	TestTrue(TEXT("Node graph layout request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, NodeGraphLayoutBody, NodeGraphLayoutResult));
	TestEqual(TEXT("Node graph layout status code"), NodeGraphLayoutResult.StatusCode, 200);

	TSharedPtr<FJsonObject> NodeGraphLayoutJson;
	TestTrue(TEXT("Node graph layout response parses as JSON"), ParseJson(NodeGraphLayoutResult.Body, NodeGraphLayoutJson));
	TestTrue(TEXT("Node graph layout root ok=true"), NodeGraphLayoutJson.IsValid() && NodeGraphLayoutJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* NodeGraphLayoutData = nullptr;
	TestTrue(TEXT("Node graph layout contains data object"), NodeGraphLayoutJson.IsValid() && NodeGraphLayoutJson->TryGetObjectField(TEXT("data"), NodeGraphLayoutData) && NodeGraphLayoutData && NodeGraphLayoutData->IsValid());
	if (NodeGraphLayoutData && NodeGraphLayoutData->IsValid())
	{
		TestEqual(TEXT("Node graph layout uses k2 adapter"), (*NodeGraphLayoutData)->GetStringField(TEXT("adapter")), FString(TEXT("k2_blueprint")));
		TestEqual(TEXT("Node graph layout delegates to blueprint all graphs"), (*NodeGraphLayoutData)->GetStringField(TEXT("delegated_command")), FString(TEXT("blueprint_layout_all_graphs")));
		TestEqual(TEXT("Node graph layout has no failed graphs"), static_cast<int32>((*NodeGraphLayoutData)->GetIntegerField(TEXT("failed_graph_count"))), 0);
	}

	const FString LayoutGraphBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-graph-layout-001"),
		TEXT("blueprint_layout_graph"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"EventGraph\",\"horizontal_spacing\":360,\"vertical_spacing\":96,\"node_padding\":16,\"compile_after_layout\":false,\"save_after_layout\":false}"), *BlueprintAssetPath));
	FHttpSmokeResult LayoutGraphResult;
	TestTrue(TEXT("Blueprint graph layout request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, LayoutGraphBody, LayoutGraphResult));
	TestEqual(TEXT("Blueprint graph layout status code"), LayoutGraphResult.StatusCode, 200);

	TSharedPtr<FJsonObject> LayoutGraphJson;
	TestTrue(TEXT("Blueprint graph layout response parses as JSON"), ParseJson(LayoutGraphResult.Body, LayoutGraphJson));
	TestTrue(TEXT("Blueprint graph layout root ok=true"), LayoutGraphJson.IsValid() && LayoutGraphJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* LayoutGraphData = nullptr;
	TestTrue(TEXT("Blueprint graph layout contains data object"), LayoutGraphJson.IsValid() && LayoutGraphJson->TryGetObjectField(TEXT("data"), LayoutGraphData) && LayoutGraphData && LayoutGraphData->IsValid());
	if (LayoutGraphData && LayoutGraphData->IsValid())
	{
		TestTrue(TEXT("Blueprint graph layout covers created nodes"), static_cast<int32>((*LayoutGraphData)->GetIntegerField(TEXT("node_count"))) >= 3);
		TestTrue(TEXT("Blueprint graph layout sees connected edge"), static_cast<int32>((*LayoutGraphData)->GetIntegerField(TEXT("edge_count"))) >= 1);
		TestTrue(TEXT("Blueprint graph layout reports connected components"), static_cast<int32>((*LayoutGraphData)->GetIntegerField(TEXT("component_count"))) >= 1);
		TestEqual(TEXT("Blueprint graph layout reports semantic island strategy"), (*LayoutGraphData)->GetStringField(TEXT("layout_strategy")), FString(TEXT("semantic_island_multiport_bus_layout_v2")));
		TestEqual(TEXT("Blueprint graph layout selects semantic island engine"), (*LayoutGraphData)->GetStringField(TEXT("selected_candidate")), FString(TEXT("semantic_island_multiport_bus_layout")));
		TestTrue(TEXT("Blueprint graph layout design coverage is complete"), (*LayoutGraphData)->GetBoolField(TEXT("layout_design_complete")));
		TestTrue(TEXT("Blueprint graph layout moved nodes"), static_cast<int32>((*LayoutGraphData)->GetIntegerField(TEXT("moved_node_count"))) >= 2);
		TestEqual(TEXT("Blueprint graph layout removes estimated node overlaps"), static_cast<int32>((*LayoutGraphData)->GetIntegerField(TEXT("overlap_count_after"))), 0);
		TestEqual(TEXT("Blueprint graph layout avoids estimated wire-node intersections"), static_cast<int32>((*LayoutGraphData)->GetIntegerField(TEXT("edge_node_intersection_count_after"))), 0);
		TestTrue(TEXT("Blueprint graph layout reports changed"), (*LayoutGraphData)->GetBoolField(TEXT("changed")));
		TestTrue(TEXT("Blueprint graph layout reports exec-dominant metrics"), (*LayoutGraphData)->HasField(TEXT("metrics_exec_dominant_candidate")));
		TestTrue(TEXT("Blueprint graph layout reports exec-dominant centered metrics"), (*LayoutGraphData)->HasField(TEXT("metrics_exec_dominant_centered_candidate")));
		TestTrue(TEXT("Blueprint graph layout reports dependency-dominant metrics"), (*LayoutGraphData)->HasField(TEXT("metrics_dependency_dominant_candidate")));
		TestTrue(TEXT("Blueprint graph layout reports dependency-dominant refined metrics"), (*LayoutGraphData)->HasField(TEXT("metrics_dependency_dominant_refined_candidate")));
		TestTrue(TEXT("Blueprint graph layout reports dependency-dominant centered metrics"), (*LayoutGraphData)->HasField(TEXT("metrics_dependency_dominant_centered_candidate")));
		TestTrue(TEXT("Blueprint graph layout reports existing-refined metrics"), (*LayoutGraphData)->HasField(TEXT("metrics_existing_refined_candidate")));
		TestTrue(TEXT("Blueprint graph layout reports existing semantic port constraint metrics"), (*LayoutGraphData)->HasField(TEXT("metrics_existing_semantic_port_constraint_candidate")));
		TestTrue(TEXT("Blueprint graph layout reports semantic port constraint metrics"), (*LayoutGraphData)->HasField(TEXT("metrics_semantic_port_constraint_candidate")));
		TestTrue(TEXT("Blueprint graph layout reports exec-dominant score"), (*LayoutGraphData)->HasField(TEXT("score_exec_dominant_candidate")));
		TestTrue(TEXT("Blueprint graph layout reports exec-dominant centered score"), (*LayoutGraphData)->HasField(TEXT("score_exec_dominant_centered_candidate")));
		TestTrue(TEXT("Blueprint graph layout reports dependency-dominant score"), (*LayoutGraphData)->HasField(TEXT("score_dependency_dominant_candidate")));
		TestTrue(TEXT("Blueprint graph layout reports dependency-dominant refined score"), (*LayoutGraphData)->HasField(TEXT("score_dependency_dominant_refined_candidate")));
		TestTrue(TEXT("Blueprint graph layout reports dependency-dominant centered score"), (*LayoutGraphData)->HasField(TEXT("score_dependency_dominant_centered_candidate")));
		TestTrue(TEXT("Blueprint graph layout reports existing-refined score"), (*LayoutGraphData)->HasField(TEXT("score_existing_refined_candidate")));
		TestTrue(TEXT("Blueprint graph layout reports existing semantic port constraint score"), (*LayoutGraphData)->HasField(TEXT("score_existing_semantic_port_constraint_candidate")));
		TestTrue(TEXT("Blueprint graph layout reports semantic port constraint score"), (*LayoutGraphData)->HasField(TEXT("score_semantic_port_constraint_candidate")));
		TestTrue(TEXT("Blueprint graph layout reports backward edge diagnostics"), (*LayoutGraphData)->HasField(TEXT("backward_edges_after")));
		TestTrue(TEXT("Blueprint graph layout reports pin order diagnostics"), (*LayoutGraphData)->HasField(TEXT("pin_order_violations_after")));
		TestTrue(TEXT("Blueprint graph layout reports raw edge count"), (*LayoutGraphData)->HasField(TEXT("raw_edge_count")));
		TestTrue(TEXT("Blueprint graph layout reports reroute plan"), (*LayoutGraphData)->HasField(TEXT("reroute_plan")));
		TestTrue(TEXT("Blueprint graph layout reports reroute plan count"), (*LayoutGraphData)->HasField(TEXT("reroute_plan_count")));
		TestTrue(TEXT("Blueprint graph layout reports inserted reroute count"), (*LayoutGraphData)->HasField(TEXT("reroute_inserted_edge_count")));

		const TArray<TSharedPtr<FJsonValue>>* LayoutNodes = nullptr;
		TestTrue(TEXT("Blueprint graph layout returns node entries"), (*LayoutGraphData)->TryGetArrayField(TEXT("nodes"), LayoutNodes) && LayoutNodes && LayoutNodes->Num() >= 3);
		if (LayoutNodes && LayoutNodes->Num() > 0)
		{
			const TSharedPtr<FJsonObject> FirstLayoutNode = (*LayoutNodes)[0]->AsObject();
			TestTrue(TEXT("Blueprint graph layout node reports component"), FirstLayoutNode.IsValid() && FirstLayoutNode->HasField(TEXT("component")));
			TestTrue(TEXT("Blueprint graph layout node reports input pin count"), FirstLayoutNode.IsValid() && FirstLayoutNode->HasField(TEXT("input_pin_count")));
			TestTrue(TEXT("Blueprint graph layout node reports output pin count"), FirstLayoutNode.IsValid() && FirstLayoutNode->HasField(TEXT("output_pin_count")));
			TestTrue(TEXT("Blueprint graph layout node reports exec pin classification"), FirstLayoutNode.IsValid() && FirstLayoutNode->HasField(TEXT("has_exec_pin")));
			TestTrue(TEXT("Blueprint graph layout node reports reroute classification"), FirstLayoutNode.IsValid() && FirstLayoutNode->HasField(TEXT("is_reroute_node")));
		}

		const TSharedPtr<FJsonObject>* MetricsAfter = nullptr;
		TestTrue(TEXT("Blueprint graph layout reports final metrics object"), (*LayoutGraphData)->TryGetObjectField(TEXT("metrics_after"), MetricsAfter) && MetricsAfter && MetricsAfter->IsValid());
		if (MetricsAfter && MetricsAfter->IsValid())
		{
			TestTrue(TEXT("Blueprint graph layout final metrics include backward edge count"), (*MetricsAfter)->HasField(TEXT("backward_edge_count")));
			TestTrue(TEXT("Blueprint graph layout final metrics include backward wire length"), (*MetricsAfter)->HasField(TEXT("backward_wire_manhattan_length")));
			TestTrue(TEXT("Blueprint graph layout final metrics include pin order violation count"), (*MetricsAfter)->HasField(TEXT("pin_order_violation_count")));
			TestTrue(TEXT("Blueprint graph layout final metrics include layout area"), (*MetricsAfter)->HasField(TEXT("layout_area")));
			TestTrue(TEXT("Blueprint graph layout final metrics include backward data edge count"), (*MetricsAfter)->HasField(TEXT("backward_data_edge_count")));
			TestTrue(TEXT("Blueprint graph layout final metrics include aspect ratio"), (*MetricsAfter)->HasField(TEXT("layout_aspect_ratio")));
		}
	}

	const FString CompileBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-graph-compile-001"),
		TEXT("blueprint_compile"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_messages\":true,\"max_messages\":16,\"save_after_compile\":false}"), *BlueprintAssetPath));
	FHttpSmokeResult CompileResult;
	TestTrue(TEXT("Blueprint graph compile request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CompileBody, CompileResult));
	TestEqual(TEXT("Blueprint graph compile status code"), CompileResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CompileJson;
	TestTrue(TEXT("Blueprint graph compile response parses as JSON"), ParseJson(CompileResult.Body, CompileJson));
	TestTrue(TEXT("Blueprint graph compile root ok=true"), CompileJson.IsValid() && CompileJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* CompileData = nullptr;
	TestTrue(TEXT("Blueprint graph compile contains data object"), CompileJson.IsValid() && CompileJson->TryGetObjectField(TEXT("data"), CompileData) && CompileData && CompileData->IsValid());
	if (CompileData && CompileData->IsValid())
	{
		TestFalse(TEXT("Blueprint graph compile reports no error"), (*CompileData)->GetBoolField(TEXT("has_error")));
		TestTrue(TEXT("Blueprint graph compile error count is zero"), static_cast<int32>((*CompileData)->GetIntegerField(TEXT("error_count"))) == 0);
	}

	const FString InspectAfterConnectBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-graph-inspect-002"),
		TEXT("blueprint_inspect_nodes"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"EventGraph\",\"include_pins\":true,\"limit_per_graph\":64}"), *BlueprintAssetPath));
	FHttpSmokeResult InspectAfterConnectResult;
	TestTrue(TEXT("Blueprint graph inspect after connect request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, InspectAfterConnectBody, InspectAfterConnectResult));
	TestEqual(TEXT("Blueprint graph inspect after connect status code"), InspectAfterConnectResult.StatusCode, 200);

	TSharedPtr<FJsonObject> InspectAfterConnectJson;
	TestTrue(TEXT("Blueprint graph inspect after connect response parses as JSON"), ParseJson(InspectAfterConnectResult.Body, InspectAfterConnectJson));
	TestTrue(TEXT("Blueprint graph inspect after connect root ok=true"), InspectAfterConnectJson.IsValid() && InspectAfterConnectJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* InspectAfterConnectData = nullptr;
	TestTrue(TEXT("Blueprint graph inspect after connect contains data object"), InspectAfterConnectJson.IsValid() && InspectAfterConnectJson->TryGetObjectField(TEXT("data"), InspectAfterConnectData) && InspectAfterConnectData && InspectAfterConnectData->IsValid());

	TSharedPtr<FJsonObject> CallFunctionAfterConnectObject;
	TestTrue(TEXT("Blueprint graph inspect after connect finds call function node"), FindNodeInInspect(InspectAfterConnectData, CallFunctionNodeGuid, CallFunctionAfterConnectObject));

	TSharedPtr<FJsonObject> ExecPinAfterConnectObject;
	TSharedPtr<FJsonObject> StringPinAfterConnectObject;
	TestTrue(TEXT("Blueprint graph inspect after connect finds exec input pin"), FindPinByName(CallFunctionAfterConnectObject, CallExecPinName, ExecPinAfterConnectObject));
	TestTrue(TEXT("Blueprint graph inspect after connect finds string input pin"), FindPinByName(CallFunctionAfterConnectObject, CallStringPinName, StringPinAfterConnectObject));
	if (ExecPinAfterConnectObject.IsValid())
	{
		TestTrue(TEXT("Blueprint graph exec input pin linked"), static_cast<int32>(ExecPinAfterConnectObject->GetIntegerField(TEXT("link_count"))) >= 1);
	}
	if (StringPinAfterConnectObject.IsValid())
	{
		TestTrue(TEXT("Blueprint graph string pin default contains message"), StringPinAfterConnectObject->GetStringField(TEXT("default_value")).Contains(StringValue));
	}

	const FString RemoveNodeBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-graph-remove-001"),
		TEXT("blueprint_remove_node"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"EventGraph\",\"node_guid\":\"%s\",\"compile_after_remove\":false,\"save_after_remove\":false}"), *BlueprintAssetPath, *CallFunctionNodeGuid));
	FHttpSmokeResult RemoveNodeResult;
	TestTrue(TEXT("Blueprint graph remove_node request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveNodeBody, RemoveNodeResult));
	TestEqual(TEXT("Blueprint graph remove_node status code"), RemoveNodeResult.StatusCode, 200);

	const FString CompileAfterRemoveBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-graph-compile-002"),
		TEXT("blueprint_compile"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_messages\":true,\"max_messages\":16,\"save_after_compile\":false}"), *BlueprintAssetPath));
	FHttpSmokeResult CompileAfterRemoveResult;
	TestTrue(TEXT("Blueprint graph compile after remove request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CompileAfterRemoveBody, CompileAfterRemoveResult));
	TestEqual(TEXT("Blueprint graph compile after remove status code"), CompileAfterRemoveResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CompileAfterRemoveJson;
	TestTrue(TEXT("Blueprint graph compile after remove response parses as JSON"), ParseJson(CompileAfterRemoveResult.Body, CompileAfterRemoveJson));
	TestTrue(TEXT("Blueprint graph compile after remove root ok=true"), CompileAfterRemoveJson.IsValid() && CompileAfterRemoveJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* CompileAfterRemoveData = nullptr;
	TestTrue(TEXT("Blueprint graph compile after remove contains data object"), CompileAfterRemoveJson.IsValid() && CompileAfterRemoveJson->TryGetObjectField(TEXT("data"), CompileAfterRemoveData) && CompileAfterRemoveData && CompileAfterRemoveData->IsValid());
	if (CompileAfterRemoveData && CompileAfterRemoveData->IsValid())
	{
		TestFalse(TEXT("Blueprint graph compile after remove reports no error"), (*CompileAfterRemoveData)->GetBoolField(TEXT("has_error")));
		TestTrue(TEXT("Blueprint graph compile after remove error count is zero"), static_cast<int32>((*CompileAfterRemoveData)->GetIntegerField(TEXT("error_count"))) == 0);
	}

	const FString InspectAfterRemoveBody = MakeExecRequestBody(
		TEXT("smoke-blueprint-graph-inspect-003"),
		TEXT("blueprint_inspect_nodes"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"EventGraph\",\"include_pins\":true,\"limit_per_graph\":64}"), *BlueprintAssetPath));
	FHttpSmokeResult InspectAfterRemoveResult;
	TestTrue(TEXT("Blueprint graph inspect after remove request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, InspectAfterRemoveBody, InspectAfterRemoveResult));
	TestEqual(TEXT("Blueprint graph inspect after remove status code"), InspectAfterRemoveResult.StatusCode, 200);

	TSharedPtr<FJsonObject> InspectAfterRemoveJson;
	TestTrue(TEXT("Blueprint graph inspect after remove response parses as JSON"), ParseJson(InspectAfterRemoveResult.Body, InspectAfterRemoveJson));
	TestTrue(TEXT("Blueprint graph inspect after remove root ok=true"), InspectAfterRemoveJson.IsValid() && InspectAfterRemoveJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* InspectAfterRemoveData = nullptr;
	TestTrue(TEXT("Blueprint graph inspect after remove contains data object"), InspectAfterRemoveJson.IsValid() && InspectAfterRemoveJson->TryGetObjectField(TEXT("data"), InspectAfterRemoveData) && InspectAfterRemoveData && InspectAfterRemoveData->IsValid());

	TSharedPtr<FJsonObject> RemovedCallFunctionNodeObject;
	TestFalse(TEXT("Blueprint graph removed call function no longer present"), FindNodeInInspect(InspectAfterRemoveData, CallFunctionNodeGuid, RemovedCallFunctionNodeObject));

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceUmgPropertySmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.UmgPropertyWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceUmgPropertySmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString WidgetBlueprintAssetPath = MakeAutomationAssetPath(TEXT("WBPProp"));
	const FString WidgetName = TEXT("BtnProp");

	const FString CreateBody = MakeExecRequestBody(
		TEXT("smoke-umg-prop-create-001"),
		TEXT("umg_create_widget_blueprint"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"parent_class\":\"/Script/UMG.UserWidget\",\"create_default_root\":true,\"compile_after_create\":false,\"open_editor\":false,\"save_after_create\":false}"), *WidgetBlueprintAssetPath));
	FHttpSmokeResult CreateResult;
	TestTrue(TEXT("UMG property create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateBody, CreateResult));
	TestEqual(TEXT("UMG property create status code"), CreateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CreateJson;
	TestTrue(TEXT("UMG property create response parses as JSON"), ParseJson(CreateResult.Body, CreateJson));
	TestTrue(TEXT("UMG property create root ok=true"), CreateJson.IsValid() && CreateJson->GetBoolField(TEXT("ok")));

	FString RootWidgetName;
	const TSharedPtr<FJsonObject>* CreateData = nullptr;
	TestTrue(TEXT("UMG property create contains data object"), CreateJson.IsValid() && CreateJson->TryGetObjectField(TEXT("data"), CreateData) && CreateData && CreateData->IsValid());
	if (CreateData && CreateData->IsValid())
	{
		RootWidgetName = (*CreateData)->GetStringField(TEXT("root_widget"));
		TestTrue(TEXT("UMG property create root widget non-empty"), !RootWidgetName.IsEmpty());
	}

	const FString AddWidgetBody = MakeExecRequestBody(
		TEXT("smoke-umg-prop-add-001"),
		TEXT("umg_add_widget"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"widget_class\":\"/Script/UMG.Button\",\"widget_name\":\"%s\",\"parent_widget\":\"%s\",\"make_variable\":true,\"compile_after_add\":false,\"save_after_add\":false}"), *WidgetBlueprintAssetPath, *WidgetName, *RootWidgetName));
	FHttpSmokeResult AddWidgetResult;
	TestTrue(TEXT("UMG property add_widget request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddWidgetBody, AddWidgetResult));
	TestEqual(TEXT("UMG property add_widget status code"), AddWidgetResult.StatusCode, 200);

	const FString SetWidgetPropertyBody = MakeExecRequestBody(
		TEXT("smoke-umg-prop-set-widget-001"),
		TEXT("umg_set_widget_property"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"widget_name\":\"%s\",\"property_name\":\"RenderOpacity\",\"value_text\":\"0.25\",\"compile_after_set\":false,\"save_after_set\":false}"), *WidgetBlueprintAssetPath, *WidgetName));
	FHttpSmokeResult SetWidgetPropertyResult;
	TestTrue(TEXT("UMG property set_widget_property request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetWidgetPropertyBody, SetWidgetPropertyResult));
	TestEqual(TEXT("UMG property set_widget_property status code"), SetWidgetPropertyResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SetWidgetPropertyJson;
	TestTrue(TEXT("UMG property set_widget_property response parses as JSON"), ParseJson(SetWidgetPropertyResult.Body, SetWidgetPropertyJson));
	TestTrue(TEXT("UMG property set_widget_property root ok=true"), SetWidgetPropertyJson.IsValid() && SetWidgetPropertyJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* SetWidgetPropertyData = nullptr;
	TestTrue(TEXT("UMG property set_widget_property contains data object"), SetWidgetPropertyJson.IsValid() && SetWidgetPropertyJson->TryGetObjectField(TEXT("data"), SetWidgetPropertyData) && SetWidgetPropertyData && SetWidgetPropertyData->IsValid());
	if (SetWidgetPropertyData && SetWidgetPropertyData->IsValid())
	{
		TestEqual(TEXT("UMG property set_widget_property widget name"), (*SetWidgetPropertyData)->GetStringField(TEXT("widget_name")), WidgetName);
		TestEqual(TEXT("UMG property set_widget_property property name"), (*SetWidgetPropertyData)->GetStringField(TEXT("property_name")), FString(TEXT("RenderOpacity")));
		TestTrue(TEXT("UMG property set_widget_property applied value contains 0.25"), (*SetWidgetPropertyData)->GetStringField(TEXT("applied_value_text")).Contains(TEXT("0.25")));
	}

	const FString SetSlotPropertyBody = MakeExecRequestBody(
		TEXT("smoke-umg-prop-set-slot-001"),
		TEXT("umg_set_slot_property"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"widget_name\":\"%s\",\"property_name\":\"ZOrder\",\"value_text\":\"7\",\"compile_after_set\":false,\"save_after_set\":false}"), *WidgetBlueprintAssetPath, *WidgetName));
	FHttpSmokeResult SetSlotPropertyResult;
	TestTrue(TEXT("UMG property set_slot_property request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetSlotPropertyBody, SetSlotPropertyResult));
	TestEqual(TEXT("UMG property set_slot_property status code"), SetSlotPropertyResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SetSlotPropertyJson;
	TestTrue(TEXT("UMG property set_slot_property response parses as JSON"), ParseJson(SetSlotPropertyResult.Body, SetSlotPropertyJson));
	TestTrue(TEXT("UMG property set_slot_property root ok=true"), SetSlotPropertyJson.IsValid() && SetSlotPropertyJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* SetSlotPropertyData = nullptr;
	TestTrue(TEXT("UMG property set_slot_property contains data object"), SetSlotPropertyJson.IsValid() && SetSlotPropertyJson->TryGetObjectField(TEXT("data"), SetSlotPropertyData) && SetSlotPropertyData && SetSlotPropertyData->IsValid());
	if (SetSlotPropertyData && SetSlotPropertyData->IsValid())
	{
		TestEqual(TEXT("UMG property set_slot_property widget name"), (*SetSlotPropertyData)->GetStringField(TEXT("widget_name")), WidgetName);
		TestEqual(TEXT("UMG property set_slot_property property name"), (*SetSlotPropertyData)->GetStringField(TEXT("property_name")), FString(TEXT("ZOrder")));
		TestTrue(TEXT("UMG property set_slot_property applied value contains 7"), (*SetSlotPropertyData)->GetStringField(TEXT("applied_value_text")).Contains(TEXT("7")));
	}

	const FString CompileBody = MakeExecRequestBody(
		TEXT("smoke-umg-prop-compile-001"),
		TEXT("umg_compile"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_messages\":true,\"max_messages\":16,\"save_after_compile\":false}"), *WidgetBlueprintAssetPath));
	FHttpSmokeResult CompileResult;
	TestTrue(TEXT("UMG property compile request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CompileBody, CompileResult));
	TestEqual(TEXT("UMG property compile status code"), CompileResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CompileJson;
	TestTrue(TEXT("UMG property compile response parses as JSON"), ParseJson(CompileResult.Body, CompileJson));
	TestTrue(TEXT("UMG property compile root ok=true"), CompileJson.IsValid() && CompileJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* CompileData = nullptr;
	TestTrue(TEXT("UMG property compile contains data object"), CompileJson.IsValid() && CompileJson->TryGetObjectField(TEXT("data"), CompileData) && CompileData && CompileData->IsValid());
	if (CompileData && CompileData->IsValid())
	{
		TestFalse(TEXT("UMG property compile reports no error"), (*CompileData)->GetBoolField(TEXT("has_error")));
		TestTrue(TEXT("UMG property compile error count is zero"), static_cast<int32>((*CompileData)->GetIntegerField(TEXT("error_count"))) == 0);
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceUmgBindingSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.UmgBindingWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceUmgBindingSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString WidgetBlueprintAssetPath = MakeAutomationAssetPath(TEXT("WBPBind"));
	const FString WidgetName = TEXT("TxtBound");
	const FString VariableName = TEXT("BoundText");

	const FString CreateBody = MakeExecRequestBody(
		TEXT("smoke-umg-bind-create-001"),
		TEXT("umg_create_widget_blueprint"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"parent_class\":\"/Script/UMG.UserWidget\",\"create_default_root\":true,\"compile_after_create\":false,\"open_editor\":false,\"save_after_create\":false}"), *WidgetBlueprintAssetPath));
	FHttpSmokeResult CreateResult;
	TestTrue(TEXT("UMG binding create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateBody, CreateResult));
	TestEqual(TEXT("UMG binding create status code"), CreateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CreateJson;
	TestTrue(TEXT("UMG binding create response parses as JSON"), ParseJson(CreateResult.Body, CreateJson));
	TestTrue(TEXT("UMG binding create root ok=true"), CreateJson.IsValid() && CreateJson->GetBoolField(TEXT("ok")));

	FString RootWidgetName;
	const TSharedPtr<FJsonObject>* CreateData = nullptr;
	TestTrue(TEXT("UMG binding create contains data object"), CreateJson.IsValid() && CreateJson->TryGetObjectField(TEXT("data"), CreateData) && CreateData && CreateData->IsValid());
	if (CreateData && CreateData->IsValid())
	{
		RootWidgetName = (*CreateData)->GetStringField(TEXT("root_widget"));
		TestTrue(TEXT("UMG binding create root widget non-empty"), !RootWidgetName.IsEmpty());
	}

	const FString AddWidgetBody = MakeExecRequestBody(
		TEXT("smoke-umg-bind-add-widget-001"),
		TEXT("umg_add_widget"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"widget_class\":\"/Script/UMG.TextBlock\",\"widget_name\":\"%s\",\"parent_widget\":\"%s\",\"make_variable\":true,\"compile_after_add\":false,\"save_after_add\":false}"), *WidgetBlueprintAssetPath, *WidgetName, *RootWidgetName));
	FHttpSmokeResult AddWidgetResult;
	TestTrue(TEXT("UMG binding add_widget request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddWidgetBody, AddWidgetResult));
	TestEqual(TEXT("UMG binding add_widget status code"), AddWidgetResult.StatusCode, 200);

	const FString AddVariableBody = MakeExecRequestBody(
		TEXT("smoke-umg-bind-add-variable-001"),
		TEXT("blueprint_add_variable"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"variable_name\":\"%s\",\"pin_category\":\"text\",\"compile_after_add\":false,\"save_after_add\":false}"), *WidgetBlueprintAssetPath, *VariableName));
	FHttpSmokeResult AddVariableResult;
	TestTrue(TEXT("UMG binding add variable request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddVariableBody, AddVariableResult));
	TestEqual(TEXT("UMG binding add variable status code"), AddVariableResult.StatusCode, 200);

	const FString BindBody = MakeExecRequestBody(
		TEXT("smoke-umg-bind-001"),
		TEXT("umg_bind_widget_property_to_variable"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"widget_name\":\"%s\",\"property_name\":\"Text\",\"source_variable_name\":\"%s\",\"compile_after_bind\":false,\"save_after_bind\":false}"), *WidgetBlueprintAssetPath, *WidgetName, *VariableName));
	FHttpSmokeResult BindResult;
	TestTrue(TEXT("UMG binding bind request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, BindBody, BindResult));
	TestEqual(TEXT("UMG binding bind status code"), BindResult.StatusCode, 200);

	TSharedPtr<FJsonObject> BindJson;
	TestTrue(TEXT("UMG binding bind response parses as JSON"), ParseJson(BindResult.Body, BindJson));
	TestTrue(TEXT("UMG binding bind root ok=true"), BindJson.IsValid() && BindJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* BindData = nullptr;
	TestTrue(TEXT("UMG binding bind contains data object"), BindJson.IsValid() && BindJson->TryGetObjectField(TEXT("data"), BindData) && BindData && BindData->IsValid());
	if (BindData && BindData->IsValid())
	{
		TestEqual(TEXT("UMG binding bind widget name"), (*BindData)->GetStringField(TEXT("widget_name")), WidgetName);
		TestEqual(TEXT("UMG binding bind property name"), (*BindData)->GetStringField(TEXT("property_name")), FString(TEXT("Text")));
		TestEqual(TEXT("UMG binding bind source variable"), (*BindData)->GetStringField(TEXT("source_variable_name")), VariableName);
		TestFalse(TEXT("UMG binding first bind does not replace existing"), (*BindData)->GetBoolField(TEXT("replaced_existing")));
	}

	FHttpSmokeResult BindAgainResult;
	TestTrue(TEXT("UMG binding second bind request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, BindBody, BindAgainResult));
	TestEqual(TEXT("UMG binding second bind status code"), BindAgainResult.StatusCode, 200);

	TSharedPtr<FJsonObject> BindAgainJson;
	TestTrue(TEXT("UMG binding second bind response parses as JSON"), ParseJson(BindAgainResult.Body, BindAgainJson));
	TestTrue(TEXT("UMG binding second bind root ok=true"), BindAgainJson.IsValid() && BindAgainJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* BindAgainData = nullptr;
	TestTrue(TEXT("UMG binding second bind contains data object"), BindAgainJson.IsValid() && BindAgainJson->TryGetObjectField(TEXT("data"), BindAgainData) && BindAgainData && BindAgainData->IsValid());
	if (BindAgainData && BindAgainData->IsValid())
	{
		TestTrue(TEXT("UMG binding second bind replaces existing"), (*BindAgainData)->GetBoolField(TEXT("replaced_existing")));
	}

	const FString CompileLogBody = MakeExecRequestBody(
		TEXT("smoke-umg-bind-compile-log-001"),
		TEXT("umg_get_compile_log"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"severity_filter\":\"all\",\"max_messages\":16,\"save_after_compile\":false}"), *WidgetBlueprintAssetPath));
	FHttpSmokeResult CompileLogResult;
	TestTrue(TEXT("UMG binding get_compile_log request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CompileLogBody, CompileLogResult));
	TestEqual(TEXT("UMG binding get_compile_log status code"), CompileLogResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CompileLogJson;
	TestTrue(TEXT("UMG binding get_compile_log response parses as JSON"), ParseJson(CompileLogResult.Body, CompileLogJson));
	TestTrue(TEXT("UMG binding get_compile_log root ok=true"), CompileLogJson.IsValid() && CompileLogJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* CompileLogData = nullptr;
	TestTrue(TEXT("UMG binding get_compile_log contains data object"), CompileLogJson.IsValid() && CompileLogJson->TryGetObjectField(TEXT("data"), CompileLogData) && CompileLogData && CompileLogData->IsValid());
	if (CompileLogData && CompileLogData->IsValid())
	{
		TestFalse(TEXT("UMG binding get_compile_log reports no error"), (*CompileLogData)->GetBoolField(TEXT("has_error")));
		TestTrue(TEXT("UMG binding get_compile_log error count is zero"), static_cast<int32>((*CompileLogData)->GetIntegerField(TEXT("error_count"))) == 0);
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceUmgFolderSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.UmgFolderWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceUmgFolderSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString WidgetBlueprintAssetPath = MakeAutomationAssetPath(TEXT("WBPFolder"));
	const FString WidgetName = TEXT("TxtTitle");
	const FString ButtonWidgetName = TEXT("BtnStart");
	const FString VariableName = TEXT("LabelText");
	const FString RequestedAnimationName = TEXT("Intro");
	const FString RenamedAnimationName = TEXT("IntroFolder");

	auto LoadJsonFile = [this](const FString& FilePath, TSharedPtr<FJsonObject>& OutObj) -> bool
	{
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
		{
			AddError(FString::Printf(TEXT("Load file failed: %s"), *FilePath));
			return false;
		}
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutObj) || !OutObj.IsValid())
		{
			AddError(FString::Printf(TEXT("Parse json failed: %s"), *FilePath));
			return false;
		}
		return true;
	};

	auto SaveJsonFile = [this](const FString& FilePath, const TSharedPtr<FJsonObject>& Obj) -> bool
	{
		FString JsonText;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
		if (!FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer))
		{
			AddError(FString::Printf(TEXT("Serialize json failed: %s"), *FilePath));
			return false;
		}
		if (!FFileHelper::SaveStringToFile(JsonText, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			AddError(FString::Printf(TEXT("Save file failed: %s"), *FilePath));
			return false;
		}
		return true;
	};

	const FString CreateBody = MakeExecRequestBody(
		TEXT("smoke-umg-folder-create-001"),
		TEXT("umg_create_widget_blueprint"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"parent_class\":\"/Script/UMG.UserWidget\",\"create_default_root\":true,\"compile_after_create\":false,\"open_editor\":false,\"save_after_create\":false}"), *WidgetBlueprintAssetPath));
	FHttpSmokeResult CreateResult;
	TestTrue(TEXT("UMG folder create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateBody, CreateResult));
	TestEqual(TEXT("UMG folder create status code"), CreateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CreateJson;
	TestTrue(TEXT("UMG folder create parses JSON"), ParseJson(CreateResult.Body, CreateJson));
	const TSharedPtr<FJsonObject>* CreateData = nullptr;
	TestTrue(TEXT("UMG folder create contains data"), CreateJson.IsValid() && CreateJson->TryGetObjectField(TEXT("data"), CreateData) && CreateData && CreateData->IsValid());
	const FString RootWidgetName = (CreateData && CreateData->IsValid()) ? (*CreateData)->GetStringField(TEXT("root_widget")) : FString();
	TestTrue(TEXT("UMG folder root widget non-empty"), !RootWidgetName.IsEmpty());

	const FString AddVariableBody = MakeExecRequestBody(
		TEXT("smoke-umg-folder-add-variable-001"),
		TEXT("blueprint_add_variable"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"variable_name\":\"%s\",\"pin_category\":\"text\",\"default_value\":\"Hello\",\"compile_after_add\":false,\"save_after_add\":false}"), *WidgetBlueprintAssetPath, *VariableName));
	FHttpSmokeResult AddVariableResult;
	TestTrue(TEXT("UMG folder add variable request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddVariableBody, AddVariableResult));
	TestEqual(TEXT("UMG folder add variable status code"), AddVariableResult.StatusCode, 200);

	const FString AddWidgetBody = MakeExecRequestBody(
		TEXT("smoke-umg-folder-add-widget-001"),
		TEXT("umg_add_widget"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"widget_class\":\"/Script/UMG.TextBlock\",\"widget_name\":\"%s\",\"parent_widget\":\"%s\",\"make_variable\":true,\"compile_after_add\":false,\"save_after_add\":false}"), *WidgetBlueprintAssetPath, *WidgetName, *RootWidgetName));
	FHttpSmokeResult AddWidgetResult;
	TestTrue(TEXT("UMG folder add widget request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddWidgetBody, AddWidgetResult));
	TestEqual(TEXT("UMG folder add widget status code"), AddWidgetResult.StatusCode, 200);

	const FString AddButtonBody = MakeExecRequestBody(
		TEXT("smoke-umg-folder-add-button-001"),
		TEXT("umg_add_widget"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"widget_class\":\"/Script/UMG.Button\",\"widget_name\":\"%s\",\"parent_widget\":\"%s\",\"make_variable\":true,\"compile_after_add\":false,\"save_after_add\":false}"), *WidgetBlueprintAssetPath, *ButtonWidgetName, *RootWidgetName));
	FHttpSmokeResult AddButtonResult;
	TestTrue(TEXT("UMG folder add button request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddButtonBody, AddButtonResult));
	TestEqual(TEXT("UMG folder add button status code"), AddButtonResult.StatusCode, 200);

	const FString SetWidgetPropertyBody = MakeExecRequestBody(
		TEXT("smoke-umg-folder-set-widget-001"),
		TEXT("umg_set_widget_property"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"widget_name\":\"%s\",\"property_name\":\"RenderOpacity\",\"value_text\":\"0.5\",\"compile_after_set\":false,\"save_after_set\":false}"), *WidgetBlueprintAssetPath, *WidgetName));
	FHttpSmokeResult SetWidgetPropertyResult;
	TestTrue(TEXT("UMG folder set widget property request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetWidgetPropertyBody, SetWidgetPropertyResult));
	TestEqual(TEXT("UMG folder set widget property status code"), SetWidgetPropertyResult.StatusCode, 200);

	const FString SetFontSizeBody = MakeExecRequestBody(
		TEXT("smoke-umg-folder-set-font-size-001"),
		TEXT("umg_set_widget_property"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"widget_name\":\"%s\",\"property_name\":\"Font.Size\",\"value_text\":\"32\",\"compile_after_set\":false,\"save_after_set\":false}"), *WidgetBlueprintAssetPath, *WidgetName));
	FHttpSmokeResult SetFontSizeResult;
	TestTrue(TEXT("UMG folder set font size request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetFontSizeBody, SetFontSizeResult));
	TestEqual(TEXT("UMG folder set font size status code"), SetFontSizeResult.StatusCode, 200);

	const FString SetSlotPropertyBody = MakeExecRequestBody(
		TEXT("smoke-umg-folder-set-slot-001"),
		TEXT("umg_set_slot_property"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"widget_name\":\"%s\",\"property_name\":\"ZOrder\",\"value_text\":\"3\",\"compile_after_set\":false,\"save_after_set\":false}"), *WidgetBlueprintAssetPath, *WidgetName));
	FHttpSmokeResult SetSlotPropertyResult;
	TestTrue(TEXT("UMG folder set slot property request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetSlotPropertyBody, SetSlotPropertyResult));
	TestEqual(TEXT("UMG folder set slot property status code"), SetSlotPropertyResult.StatusCode, 200);

	const FString BindBody = MakeExecRequestBody(
		TEXT("smoke-umg-folder-bind-001"),
		TEXT("umg_bind_widget_property_to_variable"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"widget_name\":\"%s\",\"property_name\":\"Text\",\"source_variable_name\":\"%s\",\"compile_after_bind\":false,\"save_after_bind\":false}"), *WidgetBlueprintAssetPath, *WidgetName, *VariableName));
	FHttpSmokeResult BindResult;
	TestTrue(TEXT("UMG folder bind request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, BindBody, BindResult));
	TestEqual(TEXT("UMG folder bind status code"), BindResult.StatusCode, 200);

	const FString CreateAnimationBody = MakeExecRequestBody(
		TEXT("smoke-umg-folder-create-animation-001"),
		TEXT("sequence_create_umg_animation"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"animation_name\":\"%s\",\"start_seconds\":0.0,\"duration_seconds\":1.0,\"compile_after_create\":false,\"save_after_create\":false}"), *WidgetBlueprintAssetPath, *RequestedAnimationName));
	FHttpSmokeResult CreateAnimationResult;
	TestTrue(TEXT("UMG folder create animation request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateAnimationBody, CreateAnimationResult));
	TestEqual(TEXT("UMG folder create animation status code"), CreateAnimationResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CreateAnimationJson;
	TestTrue(TEXT("UMG folder create animation parses JSON"), ParseJson(CreateAnimationResult.Body, CreateAnimationJson));
	const TSharedPtr<FJsonObject>* CreateAnimationData = nullptr;
	TestTrue(TEXT("UMG folder create animation contains data"), CreateAnimationJson.IsValid() && CreateAnimationJson->TryGetObjectField(TEXT("data"), CreateAnimationData) && CreateAnimationData && CreateAnimationData->IsValid());
	const FString ActualAnimationName = (CreateAnimationData && CreateAnimationData->IsValid()) ? (*CreateAnimationData)->GetStringField(TEXT("animation_name")) : FString();
	TestTrue(TEXT("UMG folder create animation actual name non-empty"), !ActualAnimationName.IsEmpty());

	const FString AddOpacityKeyStartBody = MakeExecRequestBody(
		TEXT("smoke-umg-folder-opacity-key-start-001"),
		TEXT("sequence_add_umg_widget_opacity_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"animation_name\":\"%s\",\"widget_name\":\"%s\",\"time_seconds\":0.0,\"opacity\":0.0,\"compile_after_set\":false,\"save_after_set\":false}"), *WidgetBlueprintAssetPath, *ActualAnimationName, *WidgetName));
	FHttpSmokeResult AddOpacityKeyStartResult;
	TestTrue(TEXT("UMG folder add opacity start key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddOpacityKeyStartBody, AddOpacityKeyStartResult));
	TestEqual(TEXT("UMG folder add opacity start key status code"), AddOpacityKeyStartResult.StatusCode, 200);

	const FString AddOpacityKeyEndBody = MakeExecRequestBody(
		TEXT("smoke-umg-folder-opacity-key-end-001"),
		TEXT("sequence_add_umg_widget_opacity_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"animation_name\":\"%s\",\"widget_name\":\"%s\",\"time_seconds\":1.0,\"opacity\":1.0,\"compile_after_set\":false,\"save_after_set\":false}"), *WidgetBlueprintAssetPath, *ActualAnimationName, *WidgetName));
	FHttpSmokeResult AddOpacityKeyEndResult;
	TestTrue(TEXT("UMG folder add opacity end key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddOpacityKeyEndBody, AddOpacityKeyEndResult));
	TestEqual(TEXT("UMG folder add opacity end key status code"), AddOpacityKeyEndResult.StatusCode, 200);

	const FString AddBackgroundColorKeyStartBody = MakeExecRequestBody(
		TEXT("smoke-umg-folder-bg-color-start-001"),
		TEXT("sequence_add_umg_widget_color_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"animation_name\":\"%s\",\"widget_name\":\"%s\",\"property_name\":\"BackgroundColor\",\"property_path\":\"BackgroundColor\",\"time_seconds\":0.0,\"red\":1.0,\"green\":1.0,\"blue\":1.0,\"alpha\":1.0,\"compile_after_set\":false,\"save_after_set\":false}"), *WidgetBlueprintAssetPath, *ActualAnimationName, *ButtonWidgetName));
	FHttpSmokeResult AddBackgroundColorKeyStartResult;
	TestTrue(TEXT("UMG folder add background color start key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddBackgroundColorKeyStartBody, AddBackgroundColorKeyStartResult));
	TestEqual(TEXT("UMG folder add background color start key status code"), AddBackgroundColorKeyStartResult.StatusCode, 200);

	const FString AddBackgroundColorKeyEndBody = MakeExecRequestBody(
		TEXT("smoke-umg-folder-bg-color-end-001"),
		TEXT("sequence_add_umg_widget_color_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"animation_name\":\"%s\",\"widget_name\":\"%s\",\"property_name\":\"BackgroundColor\",\"property_path\":\"BackgroundColor\",\"time_seconds\":1.0,\"red\":1.0,\"green\":1.0,\"blue\":0.0,\"alpha\":1.0,\"compile_after_set\":false,\"save_after_set\":false}"), *WidgetBlueprintAssetPath, *ActualAnimationName, *ButtonWidgetName));
	FHttpSmokeResult AddBackgroundColorKeyEndResult;
	TestTrue(TEXT("UMG folder add background color end key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddBackgroundColorKeyEndBody, AddBackgroundColorKeyEndResult));
	TestEqual(TEXT("UMG folder add background color end key status code"), AddBackgroundColorKeyEndResult.StatusCode, 200);

	const FString ExportBody = MakeExecRequestBody(
		TEXT("smoke-umg-folder-export-001"),
		TEXT("umg_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clean_output_dir\":true,\"include_validation\":true}"), *WidgetBlueprintAssetPath));
	FHttpSmokeResult ExportResult;
	TestTrue(TEXT("UMG folder export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ExportBody, ExportResult));
	TestEqual(TEXT("UMG folder export status code"), ExportResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ExportJson;
	TestTrue(TEXT("UMG folder export parses JSON"), ParseJson(ExportResult.Body, ExportJson));
	const TSharedPtr<FJsonObject>* ExportData = nullptr;
	TestTrue(TEXT("UMG folder export contains data"), ExportJson.IsValid() && ExportJson->TryGetObjectField(TEXT("data"), ExportData) && ExportData && ExportData->IsValid());
	const FString FolderPath = (ExportData && ExportData->IsValid()) ? (*ExportData)->GetStringField(TEXT("folder_path")) : FString();
	TestTrue(TEXT("UMG folder export folder path non-empty"), !FolderPath.IsEmpty());

	const FString VariablesPath = FPaths::Combine(FolderPath, TEXT("members"), TEXT("variables.json"));
	const FString TreePath = FPaths::Combine(FolderPath, TEXT("widget_tree"), TEXT("tree.json"));
	const FString AnimationsPath = FPaths::Combine(FolderPath, TEXT("animations"), TEXT("animations.json"));

	TSharedPtr<FJsonObject> VariablesJson;
	TestTrue(TEXT("UMG folder variables load"), LoadJsonFile(VariablesPath, VariablesJson));
	const TArray<TSharedPtr<FJsonValue>>* VariablesArray = nullptr;
	TestTrue(TEXT("UMG folder variables array exists"), VariablesJson->TryGetArrayField(TEXT("variables"), VariablesArray) && VariablesArray && VariablesArray->Num() == 1);
	if (VariablesArray && VariablesArray->Num() == 1)
	{
		const TSharedPtr<FJsonObject>* VariableObj = nullptr;
		TestTrue(TEXT("UMG folder variable parses"), (*VariablesArray)[0]->TryGetObject(VariableObj) && VariableObj && VariableObj->IsValid());
		if (VariableObj && VariableObj->IsValid())
		{
			(*VariableObj)->SetStringField(TEXT("default_value"), TEXT("FolderFlow"));
		}
	}
	TestTrue(TEXT("UMG folder variables save"), SaveJsonFile(VariablesPath, VariablesJson));

	TSharedPtr<FJsonObject> TreeJson;
	TestTrue(TEXT("UMG folder tree load"), LoadJsonFile(TreePath, TreeJson));
	const TArray<TSharedPtr<FJsonValue>>* WidgetsArray = nullptr;
	TestTrue(TEXT("UMG folder widgets array exists"), TreeJson->TryGetArrayField(TEXT("widgets"), WidgetsArray) && WidgetsArray != nullptr);
	bool bFoundExportedFontSize = false;
	bool bUpdatedFontSize = false;
	if (WidgetsArray)
	{
		for (const TSharedPtr<FJsonValue>& WidgetValue : *WidgetsArray)
		{
			const TSharedPtr<FJsonObject>* WidgetObj = nullptr;
			if (!WidgetValue.IsValid() || !WidgetValue->TryGetObject(WidgetObj) || !WidgetObj || !WidgetObj->IsValid())
			{
				continue;
			}
			if ((*WidgetObj)->GetStringField(TEXT("id")) != WidgetName)
			{
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
			if ((*WidgetObj)->TryGetArrayField(TEXT("properties"), Properties) && Properties)
			{
				for (const TSharedPtr<FJsonValue>& PropertyValue : *Properties)
				{
					const TSharedPtr<FJsonObject>* PropertyObj = nullptr;
					if (!PropertyValue.IsValid() || !PropertyValue->TryGetObject(PropertyObj) || !PropertyObj || !PropertyObj->IsValid())
					{
						continue;
					}
					if ((*PropertyObj)->GetStringField(TEXT("property_name")) == TEXT("RenderOpacity"))
					{
						(*PropertyObj)->SetStringField(TEXT("value_text"), TEXT("0.750000"));
					}
					if ((*PropertyObj)->GetStringField(TEXT("property_name")) == TEXT("Font.Size"))
					{
						bFoundExportedFontSize = (*PropertyObj)->GetStringField(TEXT("value_text")).Contains(TEXT("32"));
						(*PropertyObj)->SetStringField(TEXT("value_text"), TEXT("36"));
						bUpdatedFontSize = true;
					}
				}
			}

			const TSharedPtr<FJsonObject>* SlotObj = nullptr;
			if ((*WidgetObj)->TryGetObjectField(TEXT("slot"), SlotObj) && SlotObj && SlotObj->IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* SlotProperties = nullptr;
				if ((*SlotObj)->TryGetArrayField(TEXT("properties"), SlotProperties) && SlotProperties)
				{
					for (const TSharedPtr<FJsonValue>& PropertyValue : *SlotProperties)
					{
						const TSharedPtr<FJsonObject>* PropertyObj = nullptr;
						if (!PropertyValue.IsValid() || !PropertyValue->TryGetObject(PropertyObj) || !PropertyObj || !PropertyObj->IsValid())
						{
							continue;
						}
						if ((*PropertyObj)->GetStringField(TEXT("property_name")) == TEXT("ZOrder"))
						{
							(*PropertyObj)->SetStringField(TEXT("value_text"), TEXT("9"));
						}
					}
				}
			}
		}
	}
	TestTrue(TEXT("UMG folder exported text font size"), bFoundExportedFontSize);
	TestTrue(TEXT("UMG folder updated text font size in folder JSON"), bUpdatedFontSize);
	TestTrue(TEXT("UMG folder tree save"), SaveJsonFile(TreePath, TreeJson));

	TSharedPtr<FJsonObject> AnimationsJson;
	TestTrue(TEXT("UMG folder animations load"), LoadJsonFile(AnimationsPath, AnimationsJson));
	const TArray<TSharedPtr<FJsonValue>>* AnimationsArray = nullptr;
	TestTrue(TEXT("UMG folder animations array exists"), AnimationsJson->TryGetArrayField(TEXT("animations"), AnimationsArray) && AnimationsArray && AnimationsArray->Num() == 1);
	if (AnimationsArray && AnimationsArray->Num() == 1)
	{
		const TSharedPtr<FJsonObject>* AnimationObj = nullptr;
		TestTrue(TEXT("UMG folder animation parses"), (*AnimationsArray)[0]->TryGetObject(AnimationObj) && AnimationObj && AnimationObj->IsValid());
		if (AnimationObj && AnimationObj->IsValid())
		{
			(*AnimationObj)->SetStringField(TEXT("name"), RenamedAnimationName);
			(*AnimationObj)->SetStringField(TEXT("display_label"), RenamedAnimationName);
			const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
			if ((*AnimationObj)->TryGetArrayField(TEXT("tracks"), Tracks) && Tracks && Tracks->Num() >= 1)
			{
				for (const TSharedPtr<FJsonValue>& TrackValue : *Tracks)
				{
					const TSharedPtr<FJsonObject>* TrackObj = nullptr;
					if (!TrackValue.IsValid() || !TrackValue->TryGetObject(TrackObj) || !TrackObj || !TrackObj->IsValid())
					{
						continue;
					}
					if ((*TrackObj)->GetStringField(TEXT("track_kind")) == TEXT("opacity"))
					{
						const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
						if ((*TrackObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys)
						{
							TSharedPtr<FJsonObject> MidKey = MakeShared<FJsonObject>();
							MidKey->SetNumberField(TEXT("frame"), 30000);
							MidKey->SetNumberField(TEXT("time_seconds"), 0.5);
							MidKey->SetNumberField(TEXT("opacity"), 0.35);
							const_cast<TArray<TSharedPtr<FJsonValue>>*>(Keys)->Add(MakeShared<FJsonValueObject>(MidKey));
						}
					}
				}
			}
		}
	}
	TestTrue(TEXT("UMG folder animations save"), SaveJsonFile(AnimationsPath, AnimationsJson));

	const FString ApplyBody = MakeExecRequestBody(
		TEXT("smoke-umg-folder-apply-001"),
		TEXT("umg_apply_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"create_if_missing\":false,\"compile_after_apply\":true,\"save_after_apply\":true}"), *WidgetBlueprintAssetPath));
	FHttpSmokeResult ApplyResult;
	TestTrue(TEXT("UMG folder apply request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ApplyBody, ApplyResult));
	TestEqual(TEXT("UMG folder apply status code"), ApplyResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ApplyJson;
	TestTrue(TEXT("UMG folder apply parses JSON"), ParseJson(ApplyResult.Body, ApplyJson));
	const TSharedPtr<FJsonObject>* ApplyData = nullptr;
	TestTrue(TEXT("UMG folder apply contains data"), ApplyJson.IsValid() && ApplyJson->TryGetObjectField(TEXT("data"), ApplyData) && ApplyData && ApplyData->IsValid());
	if (ApplyData && ApplyData->IsValid())
	{
		const TSharedPtr<FJsonObject>* CompileObj = nullptr;
		TestTrue(TEXT("UMG folder apply compile data exists"), (*ApplyData)->TryGetObjectField(TEXT("compile"), CompileObj) && CompileObj && CompileObj->IsValid());
		if (CompileObj && CompileObj->IsValid())
		{
			TestFalse(TEXT("UMG folder apply compile has no errors"), (*CompileObj)->GetBoolField(TEXT("has_error")));
		}
		TestTrue(TEXT("UMG folder apply changed widgets"), static_cast<int32>((*ApplyData)->GetIntegerField(TEXT("widgets_added"))) >= 1);
		TestTrue(TEXT("UMG folder apply changed animation keys"), static_cast<int32>((*ApplyData)->GetIntegerField(TEXT("animation_keys_applied"))) >= 3);
	}

	const FString ReExportBody = MakeExecRequestBody(
		TEXT("smoke-umg-folder-export-002"),
		TEXT("umg_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clean_output_dir\":true,\"include_validation\":true}"), *WidgetBlueprintAssetPath));
	FHttpSmokeResult ReExportResult;
	TestTrue(TEXT("UMG folder re-export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ReExportBody, ReExportResult));
	TestEqual(TEXT("UMG folder re-export status code"), ReExportResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ReExportJson;
	TestTrue(TEXT("UMG folder re-export parses JSON"), ParseJson(ReExportResult.Body, ReExportJson));
	const TSharedPtr<FJsonObject>* ReExportData = nullptr;
	TestTrue(TEXT("UMG folder re-export contains data"), ReExportJson.IsValid() && ReExportJson->TryGetObjectField(TEXT("data"), ReExportData) && ReExportData && ReExportData->IsValid());
	const FString ReExportFolderPath = (ReExportData && ReExportData->IsValid()) ? (*ReExportData)->GetStringField(TEXT("folder_path")) : FString();
	TestTrue(TEXT("UMG folder re-export folder path non-empty"), !ReExportFolderPath.IsEmpty());

	TSharedPtr<FJsonObject> VerifyVariablesJson;
	TestTrue(TEXT("UMG folder verify variables load"), LoadJsonFile(FPaths::Combine(ReExportFolderPath, TEXT("members"), TEXT("variables.json")), VerifyVariablesJson));
	const TArray<TSharedPtr<FJsonValue>>* VerifyVariablesArray = nullptr;
	TestTrue(TEXT("UMG folder verify variables array exists"), VerifyVariablesJson->TryGetArrayField(TEXT("variables"), VerifyVariablesArray) && VerifyVariablesArray && VerifyVariablesArray->Num() == 1);
	if (VerifyVariablesArray && VerifyVariablesArray->Num() == 1)
	{
		const TSharedPtr<FJsonObject>* VariableObj = nullptr;
		TestTrue(TEXT("UMG folder verify variable parses"), (*VerifyVariablesArray)[0]->TryGetObject(VariableObj) && VariableObj && VariableObj->IsValid());
		if (VariableObj && VariableObj->IsValid())
		{
			TestTrue(TEXT("UMG folder variable default contains FolderFlow"), (*VariableObj)->GetStringField(TEXT("default_value")).Contains(TEXT("FolderFlow")));
		}
	}

	TSharedPtr<FJsonObject> VerifyTreeJson;
	TestTrue(TEXT("UMG folder verify tree load"), LoadJsonFile(FPaths::Combine(ReExportFolderPath, TEXT("widget_tree"), TEXT("tree.json")), VerifyTreeJson));
	const TArray<TSharedPtr<FJsonValue>>* VerifyWidgetsArray = nullptr;
	TestTrue(TEXT("UMG folder verify widgets array exists"), VerifyTreeJson->TryGetArrayField(TEXT("widgets"), VerifyWidgetsArray) && VerifyWidgetsArray != nullptr);
	bool bVerifiedWidgetProperty = false;
	bool bVerifiedFontSize = false;
	bool bVerifiedSlotProperty = false;
	if (VerifyWidgetsArray)
	{
		for (const TSharedPtr<FJsonValue>& WidgetValue : *VerifyWidgetsArray)
		{
			const TSharedPtr<FJsonObject>* WidgetObj = nullptr;
			if (!WidgetValue.IsValid() || !WidgetValue->TryGetObject(WidgetObj) || !WidgetObj || !WidgetObj->IsValid())
			{
				continue;
			}
			if ((*WidgetObj)->GetStringField(TEXT("id")) != WidgetName)
			{
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
			if ((*WidgetObj)->TryGetArrayField(TEXT("properties"), Properties) && Properties)
			{
				for (const TSharedPtr<FJsonValue>& PropertyValue : *Properties)
				{
					const TSharedPtr<FJsonObject>* PropertyObj = nullptr;
					if (!PropertyValue.IsValid() || !PropertyValue->TryGetObject(PropertyObj) || !PropertyObj || !PropertyObj->IsValid())
					{
						continue;
					}
					if ((*PropertyObj)->GetStringField(TEXT("property_name")) == TEXT("RenderOpacity") &&
						(*PropertyObj)->GetStringField(TEXT("value_text")).Contains(TEXT("0.750000")))
					{
						bVerifiedWidgetProperty = true;
					}
					if ((*PropertyObj)->GetStringField(TEXT("property_name")) == TEXT("Font.Size") &&
						(*PropertyObj)->GetStringField(TEXT("value_text")).Contains(TEXT("36")))
					{
						bVerifiedFontSize = true;
					}
				}
			}

			const TSharedPtr<FJsonObject>* SlotObj = nullptr;
			if ((*WidgetObj)->TryGetObjectField(TEXT("slot"), SlotObj) && SlotObj && SlotObj->IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* SlotProperties = nullptr;
				if ((*SlotObj)->TryGetArrayField(TEXT("properties"), SlotProperties) && SlotProperties)
				{
					for (const TSharedPtr<FJsonValue>& PropertyValue : *SlotProperties)
					{
						const TSharedPtr<FJsonObject>* PropertyObj = nullptr;
						if (!PropertyValue.IsValid() || !PropertyValue->TryGetObject(PropertyObj) || !PropertyObj || !PropertyObj->IsValid())
						{
							continue;
						}
						if ((*PropertyObj)->GetStringField(TEXT("property_name")) == TEXT("ZOrder") &&
							(*PropertyObj)->GetStringField(TEXT("value_text")).Contains(TEXT("9")))
						{
							bVerifiedSlotProperty = true;
						}
					}
				}
			}
		}
	}
	TestTrue(TEXT("UMG folder verify widget property applied"), bVerifiedWidgetProperty);
	TestTrue(TEXT("UMG folder verify font size applied"), bVerifiedFontSize);
	TestTrue(TEXT("UMG folder verify slot property applied"), bVerifiedSlotProperty);

	const FString ListAnimationsBody = MakeExecRequestBody(
		TEXT("smoke-umg-folder-list-anim-001"),
		TEXT("sequence_list_umg_animations"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *WidgetBlueprintAssetPath));
	FHttpSmokeResult ListAnimationsResult;
	TestTrue(TEXT("UMG folder list animations request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListAnimationsBody, ListAnimationsResult));
	TestEqual(TEXT("UMG folder list animations status code"), ListAnimationsResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ListAnimationsJson;
	TestTrue(TEXT("UMG folder list animations parses JSON"), ParseJson(ListAnimationsResult.Body, ListAnimationsJson));
	const TSharedPtr<FJsonObject>* ListAnimationsData = nullptr;
	TestTrue(TEXT("UMG folder list animations contains data"), ListAnimationsJson.IsValid() && ListAnimationsJson->TryGetObjectField(TEXT("data"), ListAnimationsData) && ListAnimationsData && ListAnimationsData->IsValid());
	bool bFoundRenamedAnimation = false;
	if (ListAnimationsData && ListAnimationsData->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Animations = nullptr;
		TestTrue(TEXT("UMG folder list animations array exists"), (*ListAnimationsData)->TryGetArrayField(TEXT("animations"), Animations) && Animations != nullptr);
		if (Animations)
		{
			for (const TSharedPtr<FJsonValue>& AnimationValue : *Animations)
			{
				const TSharedPtr<FJsonObject>* AnimationObj = nullptr;
				if (!AnimationValue.IsValid() || !AnimationValue->TryGetObject(AnimationObj) || !AnimationObj || !AnimationObj->IsValid())
				{
					continue;
				}
				if ((*AnimationObj)->GetStringField(TEXT("name")) == RenamedAnimationName)
				{
					bFoundRenamedAnimation = true;
				}
			}
		}
	}
	TestTrue(TEXT("UMG folder renamed animation exists"), bFoundRenamedAnimation);

	const FString GetAnimationInfoBody = MakeExecRequestBody(
		TEXT("smoke-umg-folder-info-001"),
		TEXT("sequence_get_umg_animation_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"animation_name\":\"%s\"}"), *WidgetBlueprintAssetPath, *RenamedAnimationName));
	FHttpSmokeResult GetAnimationInfoResult;
	TestTrue(TEXT("UMG folder animation info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetAnimationInfoBody, GetAnimationInfoResult));
	TestEqual(TEXT("UMG folder animation info status code"), GetAnimationInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetAnimationInfoJson;
	TestTrue(TEXT("UMG folder animation info parses JSON"), ParseJson(GetAnimationInfoResult.Body, GetAnimationInfoJson));
	const TSharedPtr<FJsonObject>* GetAnimationInfoData = nullptr;
	TestTrue(TEXT("UMG folder animation info contains data"), GetAnimationInfoJson.IsValid() && GetAnimationInfoJson->TryGetObjectField(TEXT("data"), GetAnimationInfoData) && GetAnimationInfoData && GetAnimationInfoData->IsValid());
	if (GetAnimationInfoData && GetAnimationInfoData->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		TestTrue(TEXT("UMG folder animation bindings array exists"), (*GetAnimationInfoData)->TryGetArrayField(TEXT("bindings"), Bindings) && Bindings != nullptr);
		bool bVerifiedThreeKeys = false;
		bool bVerifiedBackgroundColorTrack = false;
		if (Bindings)
		{
			for (const TSharedPtr<FJsonValue>& BindingValue : *Bindings)
			{
				const TSharedPtr<FJsonObject>* BindingObj = nullptr;
				if (!BindingValue.IsValid() || !BindingValue->TryGetObject(BindingObj) || !BindingObj || !BindingObj->IsValid())
				{
					continue;
				}
				const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
				if (!(*BindingObj)->TryGetArrayField(TEXT("tracks"), Tracks) || !Tracks)
				{
					continue;
				}
				for (const TSharedPtr<FJsonValue>& TrackValue : *Tracks)
				{
					const TSharedPtr<FJsonObject>* TrackObj = nullptr;
					if (!TrackValue.IsValid() || !TrackValue->TryGetObject(TrackObj) || !TrackObj || !TrackObj->IsValid())
					{
						continue;
					}
					if ((*TrackObj)->GetStringField(TEXT("property_name")) == TEXT("RenderOpacity"))
					{
						const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
						if ((*TrackObj)->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() > 0)
						{
							const TSharedPtr<FJsonObject>* SectionObj = nullptr;
							if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && SectionObj->IsValid())
							{
								bVerifiedThreeKeys = static_cast<int32>((*SectionObj)->GetIntegerField(TEXT("key_count"))) == 3;
							}
						}
					}
					if ((*TrackObj)->GetStringField(TEXT("property_path")) == TEXT("BackgroundColor"))
					{
						bVerifiedBackgroundColorTrack = true;
					}
				}
			}
		}
		TestTrue(TEXT("UMG folder animation contains three opacity keys"), bVerifiedThreeKeys);
		TestTrue(TEXT("UMG folder animation contains background color track"), bVerifiedBackgroundColorTrack);
	}

	return !HasAnyErrors();
}

#endif
