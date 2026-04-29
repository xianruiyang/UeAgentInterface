#if WITH_DEV_AUTOMATION_TESTS

#include "UeAgentHttpServer.h"
#include "UeAgentJsonDiagnostics.h"
#include "UeAgentInterfaceSettings.h"

#include "ActorFactories/ActorFactory.h"
#include "Camera/CameraActor.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/BlendSpace.h"
#include "AnimationEditorUtils.h"
#include "AnimationGraph.h"
#include "AnimationGraphSchema.h"
#include "AnimGraphNode_BlendSpacePlayer.h"
#include "AnimGraphNode_Root.h"
#include "AnimationStateMachineGraph.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimStateEntryNode.h"
#include "AnimStateNode.h"
#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "Builders/CubeBuilder.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "EngineUtils.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavigationSystem.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "LevelSequence.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "MovieScene.h"
#include "MovieSceneCommonHelpers.h"
#include "MovieSceneFolder.h"
#include "MovieScenePossessable.h"
#include "Sections/MovieSceneCinematicShotSection.h"
#include "Sections/MovieSceneSpawnSection.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tracks/MovieSceneCinematicShotTrack.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "Tracks/MovieSceneSpawnTrack.h"

DEFINE_LOG_CATEGORY_STATIC(LogUeAgentInterfaceSmoke, Log, All);

#include "UeAgentInterfaceSmokeTestCommon.inl"

namespace
{
	template<typename NodeType>
	NodeType* AddAutomationNodeToGraph(UEdGraph* Graph)
	{
		if (!Graph)
		{
			return nullptr;
		}

		NodeType* NewNode = NewObject<NodeType>(Graph);
		Graph->AddNode(NewNode, /*bFromUI=*/true, /*bSelectNewNode=*/false);
		NewNode->CreateNewGuid();
		NewNode->PostPlacedNewNode();
		NewNode->AllocateDefaultPins();
		return NewNode;
	}

	static bool CheckSmokeLinearColorField(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonObject>& OwnerObject,
		const TCHAR* FieldName,
		const FLinearColor& ExpectedColor,
		const TCHAR* ContextLabel)
	{
		if (!Test || !OwnerObject.IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* ColorObject = nullptr;
		const bool bHasField = OwnerObject->TryGetObjectField(FieldName, ColorObject) && ColorObject && ColorObject->IsValid();
		Test->TestTrue(FString::Printf(TEXT("%s color field exists"), ContextLabel), bHasField);
		if (!bHasField)
		{
			return false;
		}

		const double R = (*ColorObject)->GetNumberField(TEXT("r"));
		const double G = (*ColorObject)->GetNumberField(TEXT("g"));
		const double B = (*ColorObject)->GetNumberField(TEXT("b"));
		const double A = (*ColorObject)->GetNumberField(TEXT("a"));
		Test->TestTrue(FString::Printf(TEXT("%s color r matches"), ContextLabel), FMath::IsNearlyEqual(static_cast<float>(R), ExpectedColor.R, 0.01f));
		Test->TestTrue(FString::Printf(TEXT("%s color g matches"), ContextLabel), FMath::IsNearlyEqual(static_cast<float>(G), ExpectedColor.G, 0.01f));
		Test->TestTrue(FString::Printf(TEXT("%s color b matches"), ContextLabel), FMath::IsNearlyEqual(static_cast<float>(B), ExpectedColor.B, 0.01f));
		Test->TestTrue(FString::Printf(TEXT("%s color a matches"), ContextLabel), FMath::IsNearlyEqual(static_cast<float>(A), ExpectedColor.A, 0.01f));
		return true;
	}

	static FString MakeSmokeAssetFolderPath(const FString& Profile, const FString& AssetPath)
	{
		FString RelativeFolder = AssetPath;
		while (RelativeFolder.StartsWith(TEXT("/")))
		{
			RelativeFolder.RightChopInline(1, EAllowShrinking::No);
		}

		return FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), Profile, RelativeFolder));
	}

	static FString ToSmokeJsonPath(const FString& FilePath)
	{
		return FilePath.Replace(TEXT("\\"), TEXT("/"));
	}

	static bool LoadSmokeJsonFile(FAutomationTestBase* Test, const FString& FilePath, TSharedPtr<FJsonObject>& OutObj, const TCHAR* ContextLabel)
	{
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
		{
			if (Test)
			{
				Test->AddError(FString::Printf(TEXT("%s load failed: %s"), ContextLabel, *FilePath));
			}
			return false;
		}

		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutObj) || !OutObj.IsValid())
		{
			if (Test)
			{
				Test->AddError(FString::Printf(TEXT("%s parse failed: %s"), ContextLabel, *FilePath));
			}
			return false;
		}
		return true;
	}
}

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
	FUeAgentInterfaceLevelReadonlySmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.LevelReadonlyQueries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceLevelReadonlySmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString WorldStateBody = MakeExecRequestBody(TEXT("smoke-world-001"), TEXT("get_world_state"));
	FHttpSmokeResult WorldStateResult;
	TestTrue(TEXT("World state request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, WorldStateBody, WorldStateResult));
	TestEqual(TEXT("World state status code"), WorldStateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> WorldStateJson;
	TestTrue(TEXT("World state response parses as JSON"), ParseJson(WorldStateResult.Body, WorldStateJson));
	TestTrue(TEXT("World state root ok=true"), WorldStateJson.IsValid() && WorldStateJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* WorldStateData = nullptr;
	TestTrue(TEXT("World state contains data object"), WorldStateJson.IsValid() && WorldStateJson->TryGetObjectField(TEXT("data"), WorldStateData) && WorldStateData && WorldStateData->IsValid());
	if (WorldStateData && WorldStateData->IsValid())
	{
		TestTrue(TEXT("World state contains map_name"), (*WorldStateData)->HasTypedField<EJson::String>(TEXT("map_name")));
		TestTrue(TEXT("World state contains package_name"), (*WorldStateData)->HasTypedField<EJson::String>(TEXT("package_name")));
		TestTrue(TEXT("World state contains dirty flag"), (*WorldStateData)->HasTypedField<EJson::Boolean>(TEXT("dirty")));
	}

	const FString ViewportBody = MakeExecRequestBody(TEXT("smoke-viewport-001"), TEXT("viewport_get_camera"));
	FHttpSmokeResult ViewportResult;
	TestTrue(TEXT("Viewport camera request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ViewportBody, ViewportResult));
	TestEqual(TEXT("Viewport camera status code"), ViewportResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ViewportJson;
	TestTrue(TEXT("Viewport camera response parses as JSON"), ParseJson(ViewportResult.Body, ViewportJson));
	TestTrue(TEXT("Viewport camera root ok=true"), ViewportJson.IsValid() && ViewportJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* ViewportData = nullptr;
	TestTrue(TEXT("Viewport camera contains data object"), ViewportJson.IsValid() && ViewportJson->TryGetObjectField(TEXT("data"), ViewportData) && ViewportData && ViewportData->IsValid());
	if (ViewportData && ViewportData->IsValid())
	{
		TestTrue(TEXT("Viewport camera contains location"), (*ViewportData)->HasTypedField<EJson::Object>(TEXT("location")));
		TestTrue(TEXT("Viewport camera contains rotation"), (*ViewportData)->HasTypedField<EJson::Object>(TEXT("rotation")));
		TestTrue(TEXT("Viewport camera contains fov"), (*ViewportData)->HasTypedField<EJson::Number>(TEXT("fov")));
		TestTrue(TEXT("Viewport camera contains realtime"), (*ViewportData)->HasTypedField<EJson::Boolean>(TEXT("realtime")));
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceLevelCollisionQueriesSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.LevelCollisionQueries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceLevelCollisionQueriesSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ActorLabel = FString::Printf(TEXT("SmokeCollisionCube_%s"), *UniqueSuffix);
	const FVector ActorLocation(50000.0f, 50000.0f, 100.0f);
	const FVector ActorScale(5.0f, 5.0f, 5.0f);

	const FString SpawnBody = MakeExecRequestBody(
		TEXT("smoke-collision-spawn-001"),
		TEXT("spawn_actor"),
		FString::Printf(
			TEXT("{\"class_path\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"scale\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}}"),
			*ActorLabel,
			ActorLocation.X,
			ActorLocation.Y,
			ActorLocation.Z,
			ActorScale.X,
			ActorScale.Y,
			ActorScale.Z));
	FHttpSmokeResult SpawnResult;
	TestTrue(TEXT("Collision spawn_actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnBody, SpawnResult));
	TestEqual(TEXT("Collision spawn_actor status code"), SpawnResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SpawnJson;
	TestTrue(TEXT("Collision spawn_actor response parses as JSON"), ParseJson(SpawnResult.Body, SpawnJson));
	TestTrue(TEXT("Collision spawn_actor root ok=true"), SpawnJson.IsValid() && SpawnJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* SpawnData = nullptr;
	TestTrue(TEXT("Collision spawn_actor contains data object"), SpawnJson.IsValid() && SpawnJson->TryGetObjectField(TEXT("data"), SpawnData) && SpawnData && SpawnData->IsValid());
	const FString ActorName = (SpawnData && SpawnData->IsValid()) ? (*SpawnData)->GetStringField(TEXT("name")) : FString();
	TestFalse(TEXT("Collision spawn_actor returned name"), ActorName.IsEmpty());
	if (ActorName.IsEmpty())
	{
		return false;
	}

	auto DestroyActorByName = [this, &ServerScope](const FString& RequestId, const FString& InActorName) -> void
	{
		if (InActorName.IsEmpty())
		{
			return;
		}

		const FString DestroyBody = MakeExecRequestBody(
			RequestId,
			TEXT("destroy_actor"),
			FString::Printf(TEXT("{\"id\":\"%s\"}"), *InActorName));
		FHttpSmokeResult DestroyResult;
		ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DestroyBody, DestroyResult, 5.0, false);
	};

	{
		const FVector TraceStart = ActorLocation + FVector(0.0f, 0.0f, 2000.0f);
		const FString TraceBody = MakeExecRequestBody(
			TEXT("smoke-collision-trace-001"),
			TEXT("level_trace_world_ray"),
			FString::Printf(
				TEXT("{\"start\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"direction\":{\"x\":0.0,\"y\":0.0,\"z\":-1.0},\"trace_distance\":5000.0,\"trace_channel\":\"Visibility\",\"trace_complex\":true}"),
				TraceStart.X,
				TraceStart.Y,
				TraceStart.Z));
		FHttpSmokeResult TraceResult;
		TestTrue(TEXT("Collision level_trace_world_ray request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, TraceBody, TraceResult));
		TestEqual(TEXT("Collision level_trace_world_ray status code"), TraceResult.StatusCode, 200);

		TSharedPtr<FJsonObject> TraceJson;
		TestTrue(TEXT("Collision level_trace_world_ray parses JSON"), ParseJson(TraceResult.Body, TraceJson));
		TestTrue(TEXT("Collision level_trace_world_ray root ok=true"), TraceJson.IsValid() && TraceJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* TraceData = nullptr;
		TestTrue(TEXT("Collision level_trace_world_ray contains data"), TraceJson.IsValid() && TraceJson->TryGetObjectField(TEXT("data"), TraceData) && TraceData && TraceData->IsValid());
		if (TraceData && TraceData->IsValid())
		{
			TestTrue(TEXT("Collision level_trace_world_ray hit=true"), (*TraceData)->GetBoolField(TEXT("hit")));
			TestEqual(TEXT("Collision level_trace_world_ray actor_name matches"), (*TraceData)->GetStringField(TEXT("actor_name")), ActorName);
			TestEqual(TEXT("Collision level_trace_world_ray actor_id matches label"), (*TraceData)->GetStringField(TEXT("actor_id")), ActorLabel);
		}
	}

	{
		const FVector SweepStart = ActorLocation + FVector(0.0f, -2000.0f, 0.0f);
		const FVector SweepEnd = ActorLocation + FVector(0.0f, 2000.0f, 0.0f);
		const FString SweepBody = MakeExecRequestBody(
			TEXT("smoke-collision-sweep-001"),
			TEXT("level_sweep_capsule"),
			FString::Printf(
				TEXT("{\"start\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"end\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"radius_cm\":34.0,\"half_height_cm\":88.0,\"trace_channel\":\"Pawn\",\"trace_complex\":false,\"find_initial_overlaps\":true}"),
				SweepStart.X,
				SweepStart.Y,
				SweepStart.Z,
				SweepEnd.X,
				SweepEnd.Y,
				SweepEnd.Z));
		FHttpSmokeResult SweepResult;
		TestTrue(TEXT("Collision level_sweep_capsule request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SweepBody, SweepResult));
		TestEqual(TEXT("Collision level_sweep_capsule status code"), SweepResult.StatusCode, 200);

		TSharedPtr<FJsonObject> SweepJson;
		TestTrue(TEXT("Collision level_sweep_capsule parses JSON"), ParseJson(SweepResult.Body, SweepJson));
		TestTrue(TEXT("Collision level_sweep_capsule root ok=true"), SweepJson.IsValid() && SweepJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* SweepData = nullptr;
		TestTrue(TEXT("Collision level_sweep_capsule contains data"), SweepJson.IsValid() && SweepJson->TryGetObjectField(TEXT("data"), SweepData) && SweepData && SweepData->IsValid());
		if (SweepData && SweepData->IsValid())
		{
			TestTrue(TEXT("Collision level_sweep_capsule blocking_hit=true"), (*SweepData)->GetBoolField(TEXT("blocking_hit")));
			TestEqual(TEXT("Collision level_sweep_capsule actor_name matches"), (*SweepData)->GetStringField(TEXT("actor_name")), ActorName);
		}
	}

	{
		const FVector PathStart = ActorLocation + FVector(0.0f, -2000.0f, 0.0f);
		const FVector PathEnd = ActorLocation + FVector(0.0f, 2000.0f, 0.0f);
		const FString PathBody = MakeExecRequestBody(
			TEXT("smoke-collision-sweep-path-001"),
			TEXT("level_sweep_capsule_path"),
			FString::Printf(
				TEXT("{\"points_mode\":\"center\",\"points\":[{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}],\"radius_cm\":34.0,\"half_height_cm\":88.0,\"step_cm\":200.0,\"trace_channel\":\"Pawn\",\"trace_complex\":false,\"find_initial_overlaps\":true,\"stop_on_blocking_hit\":true}"),
				PathStart.X,
				PathStart.Y,
				PathStart.Z,
				PathEnd.X,
				PathEnd.Y,
				PathEnd.Z));
		FHttpSmokeResult PathResult;
		TestTrue(TEXT("Collision level_sweep_capsule_path request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, PathBody, PathResult));
		TestEqual(TEXT("Collision level_sweep_capsule_path status code"), PathResult.StatusCode, 200);

		TSharedPtr<FJsonObject> PathJson;
		TestTrue(TEXT("Collision level_sweep_capsule_path parses JSON"), ParseJson(PathResult.Body, PathJson));
		TestTrue(TEXT("Collision level_sweep_capsule_path root ok=true"), PathJson.IsValid() && PathJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* PathData = nullptr;
		TestTrue(TEXT("Collision level_sweep_capsule_path contains data"), PathJson.IsValid() && PathJson->TryGetObjectField(TEXT("data"), PathData) && PathData && PathData->IsValid());
		if (PathData && PathData->IsValid())
		{
			TestTrue(TEXT("Collision level_sweep_capsule_path blocking_hit=true"), (*PathData)->GetBoolField(TEXT("blocking_hit")));
			TestTrue(TEXT("Collision level_sweep_capsule_path first_blocking_segment_index>=0"), (*PathData)->GetIntegerField(TEXT("first_blocking_segment_index")) >= 0);
			TestEqual(TEXT("Collision level_sweep_capsule_path actor_name matches"), (*PathData)->GetStringField(TEXT("actor_name")), ActorName);
		}
	}

	{
		const FVector ProbeLocation = ActorLocation + FVector(0.0f, 0.0f, 2000.0f);
		const FString ProbeBody = MakeExecRequestBody(
			TEXT("smoke-collision-probe-001"),
			TEXT("level_mark_probe"),
			FString::Printf(TEXT("{\"label\":\"SmokeProbe_%s\",\"location\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}}"), *UniqueSuffix, ProbeLocation.X, ProbeLocation.Y, ProbeLocation.Z));
		FHttpSmokeResult ProbeResult;
		TestTrue(TEXT("Collision level_mark_probe request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ProbeBody, ProbeResult));
		TestEqual(TEXT("Collision level_mark_probe status code"), ProbeResult.StatusCode, 200);

		TSharedPtr<FJsonObject> ProbeJson;
		TestTrue(TEXT("Collision level_mark_probe parses JSON"), ParseJson(ProbeResult.Body, ProbeJson));
		TestTrue(TEXT("Collision level_mark_probe root ok=true"), ProbeJson.IsValid() && ProbeJson->GetBoolField(TEXT("ok")));

		FString ProbeActorName;
		const TSharedPtr<FJsonObject>* ProbeData = nullptr;
		TestTrue(TEXT("Collision level_mark_probe contains data"), ProbeJson.IsValid() && ProbeJson->TryGetObjectField(TEXT("data"), ProbeData) && ProbeData && ProbeData->IsValid());
		if (ProbeData && ProbeData->IsValid())
		{
			const TSharedPtr<FJsonObject>* ProbeInfo = nullptr;
			if ((*ProbeData)->TryGetObjectField(TEXT("probe"), ProbeInfo) && ProbeInfo && ProbeInfo->IsValid())
			{
				ProbeActorName = (*ProbeInfo)->GetStringField(TEXT("name"));
			}
		}
		TestFalse(TEXT("Collision level_mark_probe returned probe actor name"), ProbeActorName.IsEmpty());

		if (!ProbeActorName.IsEmpty())
		{
			const FString SnapBody = MakeExecRequestBody(
				TEXT("smoke-collision-snap-001"),
				TEXT("level_snap_to_surface"),
				FString::Printf(
					TEXT("{\"id\":\"%s\",\"direction\":{\"x\":0.0,\"y\":0.0,\"z\":-1.0},\"trace_distance\":5000.0,\"trace_channel\":\"Visibility\",\"trace_complex\":true}"),
					*ProbeActorName));
			FHttpSmokeResult SnapResult;
			TestTrue(TEXT("Collision level_snap_to_surface request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SnapBody, SnapResult));
			TestEqual(TEXT("Collision level_snap_to_surface status code"), SnapResult.StatusCode, 200);

			TSharedPtr<FJsonObject> SnapJson;
			TestTrue(TEXT("Collision level_snap_to_surface parses JSON"), ParseJson(SnapResult.Body, SnapJson));
			TestTrue(TEXT("Collision level_snap_to_surface root ok=true"), SnapJson.IsValid() && SnapJson->GetBoolField(TEXT("ok")));

			const TSharedPtr<FJsonObject>* SnapData = nullptr;
			TestTrue(TEXT("Collision level_snap_to_surface contains data"), SnapJson.IsValid() && SnapJson->TryGetObjectField(TEXT("data"), SnapData) && SnapData && SnapData->IsValid());
			if (SnapData && SnapData->IsValid())
			{
				TestTrue(TEXT("Collision level_snap_to_surface snapped=true"), (*SnapData)->GetBoolField(TEXT("snapped")));
				TestEqual(TEXT("Collision level_snap_to_surface hit actor matches cube"), (*SnapData)->GetStringField(TEXT("actor_name")), ActorName);
			}

			DestroyActorByName(TEXT("smoke-collision-destroy-probe-001"), ProbeActorName);
		}
	}

	{
		const FString OverlapBody = MakeExecRequestBody(
			TEXT("smoke-collision-overlap-001"),
			TEXT("level_check_overlaps"),
			FString::Printf(
				TEXT("{\"shape\":\"box\",\"center\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"box_extent\":{\"x\":300.0,\"y\":300.0,\"z\":300.0},\"trace_channel\":\"Visibility\",\"limit\":50}"),
				ActorLocation.X,
				ActorLocation.Y,
				ActorLocation.Z));
		FHttpSmokeResult OverlapResult;
		TestTrue(TEXT("Collision level_check_overlaps request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, OverlapBody, OverlapResult));
		TestEqual(TEXT("Collision level_check_overlaps status code"), OverlapResult.StatusCode, 200);

		TSharedPtr<FJsonObject> OverlapJson;
		TestTrue(TEXT("Collision level_check_overlaps parses JSON"), ParseJson(OverlapResult.Body, OverlapJson));
		TestTrue(TEXT("Collision level_check_overlaps root ok=true"), OverlapJson.IsValid() && OverlapJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* OverlapData = nullptr;
		TestTrue(TEXT("Collision level_check_overlaps contains data"), OverlapJson.IsValid() && OverlapJson->TryGetObjectField(TEXT("data"), OverlapData) && OverlapData && OverlapData->IsValid());
		if (OverlapData && OverlapData->IsValid())
		{
			TestTrue(TEXT("Collision level_check_overlaps overlap_count>=1"), static_cast<int32>((*OverlapData)->GetIntegerField(TEXT("overlap_count"))) >= 1);

			const TArray<TSharedPtr<FJsonValue>>* Overlaps = nullptr;
			TestTrue(TEXT("Collision level_check_overlaps contains overlaps array"), (*OverlapData)->TryGetArrayField(TEXT("overlaps"), Overlaps) && Overlaps);
			bool bFound = false;
			if (Overlaps)
			{
				for (const TSharedPtr<FJsonValue>& ItemValue : *Overlaps)
				{
					const TSharedPtr<FJsonObject> ItemObj = ItemValue.IsValid() ? ItemValue->AsObject() : nullptr;
					if (!ItemObj.IsValid())
					{
						continue;
					}

					const TSharedPtr<FJsonObject>* ActorSummary = nullptr;
					if (ItemObj->TryGetObjectField(TEXT("actor"), ActorSummary) && ActorSummary && ActorSummary->IsValid())
					{
						if ((*ActorSummary)->GetStringField(TEXT("name")) == ActorName)
						{
							bFound = true;
							break;
						}
					}
				}
			}
			TestTrue(TEXT("Collision level_check_overlaps includes spawned actor"), bFound);
		}
	}

	// Validate OBB math under non-uniform scale + rotation (common in whitebox blockout).
	{
		const FString RotLabel = FString::Printf(TEXT("SmokeObbRot_%s"), *UniqueSuffix);
		const FVector RotLocation = ActorLocation + FVector(0.0f, 2500.0f, 100.0f);
		const FVector RotScale(4.0f, 8.0f, 0.4f);

		const FString RotSpawnBody = MakeExecRequestBody(
			TEXT("smoke-collision-obb-rot-spawn-001"),
			TEXT("spawn_actor"),
			FString::Printf(
				TEXT("{\"class_path\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"scale\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}}"),
				*RotLabel,
				RotLocation.X,
				RotLocation.Y,
				RotLocation.Z,
				RotScale.X,
				RotScale.Y,
				RotScale.Z));
		FHttpSmokeResult RotSpawnResult;
		TestTrue(TEXT("OBB rot spawn_actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RotSpawnBody, RotSpawnResult));
		TestEqual(TEXT("OBB rot spawn_actor status code"), RotSpawnResult.StatusCode, 200);

		TSharedPtr<FJsonObject> RotSpawnJson;
		TestTrue(TEXT("OBB rot spawn_actor parses JSON"), ParseJson(RotSpawnResult.Body, RotSpawnJson));
		TestTrue(TEXT("OBB rot spawn_actor root ok=true"), RotSpawnJson.IsValid() && RotSpawnJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* RotSpawnData = nullptr;
		TestTrue(TEXT("OBB rot spawn_actor contains data"), RotSpawnJson.IsValid() && RotSpawnJson->TryGetObjectField(TEXT("data"), RotSpawnData) && RotSpawnData && RotSpawnData->IsValid());
		const FString RotActorName = (RotSpawnData && RotSpawnData->IsValid()) ? (*RotSpawnData)->GetStringField(TEXT("name")) : FString();
		TestFalse(TEXT("OBB rot spawn_actor returned name"), RotActorName.IsEmpty());

		if (!RotActorName.IsEmpty())
		{
			const FString RotSetBody = MakeExecRequestBody(
				TEXT("smoke-collision-obb-rot-set-rot-001"),
				TEXT("level_set_actor_rotation"),
				FString::Printf(TEXT("{\"id\":\"%s\",\"rotation\":{\"pitch\":0.0,\"yaw\":0.0,\"roll\":-30.0}}"), *RotActorName));
			FHttpSmokeResult RotSetResult;
			TestTrue(TEXT("OBB rot set_actor_rotation request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RotSetBody, RotSetResult));
			TestEqual(TEXT("OBB rot set_actor_rotation status code"), RotSetResult.StatusCode, 200);

			const FString RotObbBody = MakeExecRequestBody(
				TEXT("smoke-collision-obb-rot-query-001"),
				TEXT("level_get_nearby_actor_obbs"),
				FString::Printf(TEXT("{\"id\":\"%s\",\"radius\":10.0,\"include_self\":true}"), *RotActorName));
			FHttpSmokeResult RotObbResult;
			TestTrue(TEXT("OBB rot nearby obbs request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RotObbBody, RotObbResult));
			TestEqual(TEXT("OBB rot nearby obbs status code"), RotObbResult.StatusCode, 200);

			TSharedPtr<FJsonObject> RotObbJson;
			TestTrue(TEXT("OBB rot nearby obbs parses JSON"), ParseJson(RotObbResult.Body, RotObbJson));
			TestTrue(TEXT("OBB rot nearby obbs root ok=true"), RotObbJson.IsValid() && RotObbJson->GetBoolField(TEXT("ok")));

			const TSharedPtr<FJsonObject>* RotObbData = nullptr;
			TestTrue(TEXT("OBB rot nearby obbs contains data"), RotObbJson.IsValid() && RotObbJson->TryGetObjectField(TEXT("data"), RotObbData) && RotObbData && RotObbData->IsValid());
			if (RotObbData && RotObbData->IsValid())
			{
				const TSharedPtr<FJsonObject>* SourceObb = nullptr;
				TestTrue(TEXT("OBB rot nearby obbs returns source_obb"), (*RotObbData)->TryGetObjectField(TEXT("source_obb"), SourceObb) && SourceObb && SourceObb->IsValid());
				if (SourceObb && SourceObb->IsValid())
				{
					const TSharedPtr<FJsonObject>* HalfLengths = nullptr;
					const TSharedPtr<FJsonObject>* LocalExtent = nullptr;
					TestTrue(TEXT("OBB rot has half_lengths"), (*SourceObb)->TryGetObjectField(TEXT("half_lengths"), HalfLengths) && HalfLengths && HalfLengths->IsValid());
					TestTrue(TEXT("OBB rot has local_extent"), (*SourceObb)->TryGetObjectField(TEXT("local_extent"), LocalExtent) && LocalExtent && LocalExtent->IsValid());
					if ((HalfLengths && HalfLengths->IsValid()) && (LocalExtent && LocalExtent->IsValid()))
					{
						TestTrue(TEXT("OBB rot local_extent.x ~= 50"), FMath::IsNearlyEqual((*LocalExtent)->GetNumberField(TEXT("x")), 50.0, 0.25));
						TestTrue(TEXT("OBB rot local_extent.y ~= 50"), FMath::IsNearlyEqual((*LocalExtent)->GetNumberField(TEXT("y")), 50.0, 0.25));
						TestTrue(TEXT("OBB rot local_extent.z ~= 50"), FMath::IsNearlyEqual((*LocalExtent)->GetNumberField(TEXT("z")), 50.0, 0.25));
						TestTrue(TEXT("OBB rot half_lengths.x ~= 200"), FMath::IsNearlyEqual((*HalfLengths)->GetNumberField(TEXT("x")), 200.0, 0.5));
						TestTrue(TEXT("OBB rot half_lengths.y ~= 400"), FMath::IsNearlyEqual((*HalfLengths)->GetNumberField(TEXT("y")), 400.0, 0.5));
						TestTrue(TEXT("OBB rot half_lengths.z ~= 20"), FMath::IsNearlyEqual((*HalfLengths)->GetNumberField(TEXT("z")), 20.0, 0.25));
					}
				}
			}
		}

		DestroyActorByName(TEXT("smoke-collision-obb-rot-destroy-001"), RotActorName);
	}

	DestroyActorByName(TEXT("smoke-collision-destroy-001"), ActorName);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceNavmeshQueriesSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.NavmeshQueries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceNavmeshQueriesSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FVector BaseLocation(60000.0f, 60000.0f, 0.0f);

	auto SpawnActor = [this, &ServerScope](const FString& RequestId, const FString& ClassPath, const FString& Label, const FVector& Location, const FVector& Scale, const FString& StaticMeshPath, FString& OutActorName) -> bool
	{
		const FString Params = StaticMeshPath.IsEmpty()
			? FString::Printf(
				TEXT("{\"class_path\":\"%s\",\"label\":\"%s\",\"location\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"scale\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}}"),
				*ClassPath,
				*Label,
				Location.X,
				Location.Y,
				Location.Z,
				Scale.X,
				Scale.Y,
				Scale.Z)
			: FString::Printf(
				TEXT("{\"class_path\":\"%s\",\"label\":\"%s\",\"static_mesh\":\"%s\",\"location\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"scale\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}}"),
				*ClassPath,
				*Label,
				*StaticMeshPath,
				Location.X,
				Location.Y,
				Location.Z,
				Scale.X,
				Scale.Y,
				Scale.Z);

		const FString SpawnBody = MakeExecRequestBody(RequestId, TEXT("spawn_actor"), Params);
		FHttpSmokeResult SpawnResult;
		TestTrue(TEXT("Navmesh spawn_actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnBody, SpawnResult, 10.0));
		TestEqual(TEXT("Navmesh spawn_actor status code"), SpawnResult.StatusCode, 200);

		TSharedPtr<FJsonObject> SpawnJson;
		TestTrue(TEXT("Navmesh spawn_actor parses JSON"), ParseJson(SpawnResult.Body, SpawnJson));
		TestTrue(TEXT("Navmesh spawn_actor root ok=true"), SpawnJson.IsValid() && SpawnJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* SpawnData = nullptr;
		TestTrue(TEXT("Navmesh spawn_actor contains data"), SpawnJson.IsValid() && SpawnJson->TryGetObjectField(TEXT("data"), SpawnData) && SpawnData && SpawnData->IsValid());
		if (!(SpawnData && SpawnData->IsValid()))
		{
			return false;
		}

		OutActorName = (*SpawnData)->GetStringField(TEXT("name"));
		TestFalse(TEXT("Navmesh spawn_actor returned name"), OutActorName.IsEmpty());
		return !OutActorName.IsEmpty();
	};

	auto DestroyActorByName = [this, &ServerScope](const FString& RequestId, const FString& InActorName) -> void
	{
		if (InActorName.IsEmpty())
		{
			return;
		}

		const FString DestroyBody = MakeExecRequestBody(
			RequestId,
			TEXT("destroy_actor"),
			FString::Printf(TEXT("{\"id\":\"%s\"}"), *InActorName));
		FHttpSmokeResult DestroyResult;
		ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DestroyBody, DestroyResult, 10.0, false);
	};

	FString FloorActorName;
	const FString FloorLabel = FString::Printf(TEXT("SmokeNavFloor_%s"), *UniqueSuffix);
	const FString BoundsLabel = FString::Printf(TEXT("SmokeNavBounds_%s"), *UniqueSuffix);
	ANavMeshBoundsVolume* NavBoundsVolume = nullptr;

	if (!SpawnActor(
			TEXT("smoke-navmesh-spawn-floor-001"),
			TEXT("/Script/Engine.StaticMeshActor"),
			FloorLabel,
			BaseLocation,
			FVector(50.0f, 50.0f, 1.0f),
			TEXT("/Engine/BasicShapes/Cube.Cube"),
			FloorActorName))
	{
		return false;
	}

	{
		auto SpawnNavBoundsVolume = [this, &BoundsLabel, BaseLocation]() -> ANavMeshBoundsVolume*
		{
			if (!GEditor)
			{
				AddError(TEXT("Navmesh test missing GEditor"));
				return nullptr;
			}

			UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
			if (!EditorWorld)
			{
				AddError(TEXT("Navmesh test missing editor world"));
				return nullptr;
			}

			FActorSpawnParameters SpawnParams;
			SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			SpawnParams.ObjectFlags |= RF_Transactional;

			ANavMeshBoundsVolume* Volume = EditorWorld->SpawnActor<ANavMeshBoundsVolume>(BaseLocation, FRotator::ZeroRotator, SpawnParams);
			if (!Volume)
			{
				AddError(TEXT("Navmesh test failed to spawn NavMeshBoundsVolume"));
				return nullptr;
			}

			Volume->SetActorLabel(BoundsLabel);
			Volume->SetActorLocation(BaseLocation);

			UCubeBuilder* Builder = NewObject<UCubeBuilder>();
			Builder->X = 20000.0f;
			Builder->Y = 20000.0f;
			Builder->Z = 8000.0f;
			UActorFactory::CreateBrushForVolumeActor(Volume, Builder);
			Volume->SetActorLocation(BaseLocation);

			if (UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(EditorWorld))
			{
				NavSys->OnNavigationBoundsUpdated(Volume);
			}

			return Volume;
		};

		if (IsInGameThread())
		{
			NavBoundsVolume = SpawnNavBoundsVolume();
		}
		else
		{
			FEvent* Done = FPlatformProcess::GetSynchEventFromPool(true);
			AsyncTask(ENamedThreads::GameThread, [&NavBoundsVolume, SpawnNavBoundsVolume, Done]()
			{
				NavBoundsVolume = SpawnNavBoundsVolume();
				Done->Trigger();
			});

			const bool bDone = Done->Wait(FTimespan::FromSeconds(10.0));
			FPlatformProcess::ReturnSynchEventToPool(Done);
			if (!bDone)
			{
				AddError(TEXT("Navmesh test timed out waiting for nav bounds spawn"));
				NavBoundsVolume = nullptr;
			}
		}

		if (!NavBoundsVolume)
		{
			DestroyActorByName(TEXT("smoke-navmesh-destroy-floor-early"), FloorActorName);
			return false;
		}
	}

	auto RequestBuild = [this, &ServerScope](const FString& RequestId) -> bool
	{
		const FString BuildBody = MakeExecRequestBody(RequestId, TEXT("navmesh_build"), TEXT("{\"wait_for_finish\":true,\"timeout_seconds\":30}"));
		FHttpSmokeResult BuildResult;
		TestTrue(TEXT("Navmesh navmesh_build request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, BuildBody, BuildResult, 40.0));
		TestEqual(TEXT("Navmesh navmesh_build status code"), BuildResult.StatusCode, 200);

		TSharedPtr<FJsonObject> BuildJson;
		TestTrue(TEXT("Navmesh navmesh_build parses JSON"), ParseJson(BuildResult.Body, BuildJson));
		TestTrue(TEXT("Navmesh navmesh_build root ok=true"), BuildJson.IsValid() && BuildJson->GetBoolField(TEXT("ok")));
		return BuildResult.StatusCode == 200;
	};

	auto RequestProject = [this, &ServerScope](const FString& RequestId, const FVector& Point, bool& OutProjected) -> bool
	{
		const FString ProjectBody = MakeExecRequestBody(
			RequestId,
			TEXT("navmesh_project_point"),
			FString::Printf(
				TEXT("{\"point\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"project_query_extent\":{\"x\":50.0,\"y\":50.0,\"z\":200.0}}"),
				Point.X,
				Point.Y,
				Point.Z));
		FHttpSmokeResult ProjectResult;
		TestTrue(TEXT("Navmesh navmesh_project_point request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ProjectBody, ProjectResult, 30.0));
		TestEqual(TEXT("Navmesh navmesh_project_point status code"), ProjectResult.StatusCode, 200);

		TSharedPtr<FJsonObject> ProjectJson;
		TestTrue(TEXT("Navmesh navmesh_project_point parses JSON"), ParseJson(ProjectResult.Body, ProjectJson));
		TestTrue(TEXT("Navmesh navmesh_project_point root ok=true"), ProjectJson.IsValid() && ProjectJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* ProjectData = nullptr;
		TestTrue(TEXT("Navmesh navmesh_project_point contains data"), ProjectJson.IsValid() && ProjectJson->TryGetObjectField(TEXT("data"), ProjectData) && ProjectData && ProjectData->IsValid());
		OutProjected = (ProjectData && ProjectData->IsValid()) ? (*ProjectData)->GetBoolField(TEXT("projected")) : false;
		return ProjectResult.StatusCode == 200;
	};

	auto RequestFindPath = [this, &ServerScope](const FString& RequestId, const FVector& Start, const FVector& End, bool& OutPathFound) -> bool
	{
		const FString FindBody = MakeExecRequestBody(
			RequestId,
			TEXT("navmesh_find_path"),
			FString::Printf(
				TEXT("{\"start\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"end\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"allow_partial\":false,\"project_to_nav\":true}"),
				Start.X,
				Start.Y,
				Start.Z,
				End.X,
				End.Y,
				End.Z));
		FHttpSmokeResult FindResult;
		TestTrue(TEXT("Navmesh navmesh_find_path request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, FindBody, FindResult, 30.0));
		TestEqual(TEXT("Navmesh navmesh_find_path status code"), FindResult.StatusCode, 200);

		TSharedPtr<FJsonObject> FindJson;
		TestTrue(TEXT("Navmesh navmesh_find_path parses JSON"), ParseJson(FindResult.Body, FindJson));
		TestTrue(TEXT("Navmesh navmesh_find_path root ok=true"), FindJson.IsValid() && FindJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* FindData = nullptr;
		TestTrue(TEXT("Navmesh navmesh_find_path contains data"), FindJson.IsValid() && FindJson->TryGetObjectField(TEXT("data"), FindData) && FindData && FindData->IsValid());
		OutPathFound = (FindData && FindData->IsValid()) ? (*FindData)->GetBoolField(TEXT("path_found")) : false;
		return FindResult.StatusCode == 200;
	};

	bool bPathFound = false;
	const FVector QueryStart = BaseLocation + FVector(-1500.0f, 0.0f, 200.0f);
	const FVector QueryEnd = BaseLocation + FVector(1500.0f, 0.0f, 200.0f);

	RequestBuild(TEXT("smoke-navmesh-build-001"));

	bool bProjectedStart = false;
	RequestProject(TEXT("smoke-navmesh-project-001"), QueryStart, bProjectedStart);
	TestTrue(TEXT("Navmesh projected start point"), bProjectedStart);

	{
		const FString ValidateBody = MakeExecRequestBody(
			TEXT("smoke-navmesh-validate-001"),
			TEXT("level_validate_connectivity"),
			FString::Printf(
				TEXT("{\"points\":[{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}],\"allow_partial\":false,\"project_to_nav\":true}"),
				QueryStart.X,
				QueryStart.Y,
				QueryStart.Z,
				QueryEnd.X,
				QueryEnd.Y,
				QueryEnd.Z));
		FHttpSmokeResult ValidateResult;
		TestTrue(TEXT("Navmesh level_validate_connectivity request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ValidateBody, ValidateResult, 30.0));
		TestEqual(TEXT("Navmesh level_validate_connectivity status code"), ValidateResult.StatusCode, 200);

		TSharedPtr<FJsonObject> ValidateJson;
		TestTrue(TEXT("Navmesh level_validate_connectivity parses JSON"), ParseJson(ValidateResult.Body, ValidateJson));
		TestTrue(TEXT("Navmesh level_validate_connectivity root ok=true"), ValidateJson.IsValid() && ValidateJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* ValidateData = nullptr;
		TestTrue(TEXT("Navmesh level_validate_connectivity contains data"), ValidateJson.IsValid() && ValidateJson->TryGetObjectField(TEXT("data"), ValidateData) && ValidateData && ValidateData->IsValid());
		if (ValidateData && ValidateData->IsValid())
		{
			TestTrue(TEXT("Navmesh level_validate_connectivity all_connected=true"), (*ValidateData)->GetBoolField(TEXT("all_connected")));
		}
	}

	for (int32 Attempt = 0; Attempt < 10 && !bPathFound; ++Attempt)
	{
		bool bAttemptFound = false;
		RequestFindPath(FString::Printf(TEXT("smoke-navmesh-find-%03d"), Attempt + 1), QueryStart, QueryEnd, bAttemptFound);
		bPathFound = bAttemptFound;
		if (!bPathFound)
		{
			FPlatformProcess::Sleep(0.05f);
		}
	}

	TestTrue(TEXT("Navmesh path_found=true"), bPathFound);

	DestroyActorByName(TEXT("smoke-navmesh-destroy-floor-001"), FloorActorName);
	if (NavBoundsVolume)
	{
		ANavMeshBoundsVolume* VolumeToDestroy = NavBoundsVolume;
		if (IsInGameThread())
		{
			VolumeToDestroy->Destroy();
		}
		else
		{
			FEvent* Done = FPlatformProcess::GetSynchEventFromPool(true);
			AsyncTask(ENamedThreads::GameThread, [VolumeToDestroy, Done]()
			{
				if (IsValid(VolumeToDestroy))
				{
					VolumeToDestroy->Destroy();
				}
				Done->Trigger();
			});
			Done->Wait(FTimespan::FromSeconds(10.0));
			FPlatformProcess::ReturnSynchEventToPool(Done);
		}
		NavBoundsVolume = nullptr;
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceViewportFrameFolderSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.ViewportFrameFolder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceViewportFrameFolderSmokeTest::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender())
	{
		AddInfo(TEXT("Skipping viewport frame folder smoke because rendering is disabled in the current automation environment."));
		return true;
	}

	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString FolderPath = FString::Printf(TEXT("__UeAgentInterfaceSmoke/ViewportFrameFolder_%s"), *UniqueSuffix);
	const FString ChildFolderPath = FolderPath + TEXT("/Child");
	const FString ActorLabelA = FString::Printf(TEXT("SmokeViewportFrameFolderA_%s"), *UniqueSuffix);
	const FString ActorLabelB = FString::Printf(TEXT("SmokeViewportFrameFolderB_%s"), *UniqueSuffix);
	const FVector ExpectedCameraLocation(-2400.0f, -1800.0f, 1100.0f);
	const FRotator ExpectedCameraRotation(-18.0f, 42.0f, 0.0f);
	const double ExpectedFov = 60.0;

	const FString SetCameraBody = MakeExecRequestBody(
		TEXT("smoke-viewport-folder-set-camera-001"),
		TEXT("viewport_set_camera"),
		FString::Printf(
			TEXT("{\"location\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"rotation\":{\"pitch\":%.3f,\"yaw\":%.3f,\"roll\":%.3f},\"fov\":%.3f}"),
			ExpectedCameraLocation.X,
			ExpectedCameraLocation.Y,
			ExpectedCameraLocation.Z,
			ExpectedCameraRotation.Pitch,
			ExpectedCameraRotation.Yaw,
			ExpectedCameraRotation.Roll,
			ExpectedFov));
	FHttpSmokeResult SetCameraResult;
	TestTrue(TEXT("Viewport set_camera request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetCameraBody, SetCameraResult));
	TestEqual(TEXT("Viewport set_camera status code"), SetCameraResult.StatusCode, 200);

	auto SpawnCubeActor = [this, &ServerScope](const FString& RequestId, const FString& ActorLabel, const FVector& Location, FString& OutActorName) -> bool
	{
		const FString SpawnBody = MakeExecRequestBody(
			RequestId,
			TEXT("spawn_actor"),
			FString::Printf(
				TEXT("{\"class_path\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}}"),
				*ActorLabel,
				Location.X,
				Location.Y,
				Location.Z));
		FHttpSmokeResult SpawnResult;
		TestTrue(TEXT("Spawn actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnBody, SpawnResult));
		TestEqual(TEXT("Spawn actor status code"), SpawnResult.StatusCode, 200);

		TSharedPtr<FJsonObject> SpawnJson;
		TestTrue(TEXT("Spawn actor response parses as JSON"), ParseJson(SpawnResult.Body, SpawnJson));
		TestTrue(TEXT("Spawn actor root ok=true"), SpawnJson.IsValid() && SpawnJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* SpawnData = nullptr;
		TestTrue(TEXT("Spawn actor contains data object"), SpawnJson.IsValid() && SpawnJson->TryGetObjectField(TEXT("data"), SpawnData) && SpawnData && SpawnData->IsValid());
		if (!(SpawnData && SpawnData->IsValid()))
		{
			return false;
		}

		OutActorName = (*SpawnData)->GetStringField(TEXT("name"));
		TestFalse(TEXT("Spawn actor returned name"), OutActorName.IsEmpty());
		return !OutActorName.IsEmpty();
	};

	auto SetActorFolder = [this, &ServerScope](const FString& RequestId, const FString& ActorName, const FString& TargetFolderPath) -> bool
	{
		const FString FolderBody = MakeExecRequestBody(
			RequestId,
			TEXT("level_set_actor_folder"),
			FString::Printf(TEXT("{\"id\":\"%s\",\"folder_path\":\"%s\"}"), *ActorName, *TargetFolderPath));
		FHttpSmokeResult FolderResult;
		TestTrue(TEXT("Set actor folder request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, FolderBody, FolderResult));
		TestEqual(TEXT("Set actor folder status code"), FolderResult.StatusCode, 200);
		return FolderResult.StatusCode == 200;
	};

	auto DestroyActorByName = [this, &ServerScope](const FString& RequestId, const FString& ActorName) -> void
	{
		if (ActorName.IsEmpty())
		{
			return;
		}

		const FString DestroyBody = MakeExecRequestBody(
			RequestId,
			TEXT("destroy_actor"),
			FString::Printf(TEXT("{\"id\":\"%s\"}"), *ActorName));
		FHttpSmokeResult DestroyResult;
		TestTrue(TEXT("Destroy actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DestroyBody, DestroyResult));
		if (DestroyResult.StatusCode == 400)
		{
			TSharedPtr<FJsonObject> DestroyJson;
			if (ParseJson(DestroyResult.Body, DestroyJson) && DestroyJson.IsValid())
			{
				const FString ErrorCode = DestroyJson->GetStringField(TEXT("error"));
				if (ErrorCode.Equals(TEXT("actor_not_found")))
				{
					return;
				}
			}
		}
		TestEqual(TEXT("Destroy actor status code"), DestroyResult.StatusCode, 200);
	};

	FString ActorNameA;
	FString ActorNameB;
	if (!SpawnCubeActor(TEXT("smoke-viewport-folder-spawn-001"), ActorLabelA, FVector(-400.0f, -100.0f, 50.0f), ActorNameA))
	{
		return false;
	}
	if (!SpawnCubeActor(TEXT("smoke-viewport-folder-spawn-002"), ActorLabelB, FVector(900.0f, 500.0f, 50.0f), ActorNameB))
	{
		DestroyActorByName(TEXT("smoke-viewport-folder-destroy-a-early"), ActorNameA);
		return false;
	}

	SetActorFolder(TEXT("smoke-viewport-folder-folder-001"), ActorNameA, FolderPath);
	SetActorFolder(TEXT("smoke-viewport-folder-folder-002"), ActorNameB, ChildFolderPath);

	const FString FrameFolderBody = MakeExecRequestBody(
		TEXT("smoke-viewport-folder-frame-001"),
		TEXT("viewport_frame_folder"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\"}"), *FolderPath));
	FHttpSmokeResult FrameFolderResult;
	TestTrue(TEXT("Viewport frame_folder request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, FrameFolderBody, FrameFolderResult));
	TestEqual(TEXT("Viewport frame_folder status code"), FrameFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> FrameFolderJson;
	TestTrue(TEXT("Viewport frame_folder response parses as JSON"), ParseJson(FrameFolderResult.Body, FrameFolderJson));
	TestTrue(TEXT("Viewport frame_folder root ok=true"), FrameFolderJson.IsValid() && FrameFolderJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* FrameFolderData = nullptr;
	TestTrue(TEXT("Viewport frame_folder contains data object"), FrameFolderJson.IsValid() && FrameFolderJson->TryGetObjectField(TEXT("data"), FrameFolderData) && FrameFolderData && FrameFolderData->IsValid());
	if (FrameFolderData && FrameFolderData->IsValid())
	{
		TestEqual(TEXT("Viewport frame_folder focused_count"), static_cast<int32>((*FrameFolderData)->GetIntegerField(TEXT("focused_count"))), 2);
		TestTrue(TEXT("Viewport frame_folder include_child_folders=true"), (*FrameFolderData)->GetBoolField(TEXT("include_child_folders")));
		TestTrue(TEXT("Viewport frame_folder contains bounds"), (*FrameFolderData)->HasTypedField<EJson::Object>(TEXT("bounds")));

		const TSharedPtr<FJsonObject>* PreviousLocation = nullptr;
		const TSharedPtr<FJsonObject>* NewLocation = nullptr;
		const TSharedPtr<FJsonObject>* Rotation = nullptr;
		TestTrue(TEXT("Viewport frame_folder contains previous_location"), (*FrameFolderData)->TryGetObjectField(TEXT("previous_location"), PreviousLocation) && PreviousLocation && PreviousLocation->IsValid());
		TestTrue(TEXT("Viewport frame_folder contains new_location"), (*FrameFolderData)->TryGetObjectField(TEXT("new_location"), NewLocation) && NewLocation && NewLocation->IsValid());
		TestTrue(TEXT("Viewport frame_folder contains rotation"), (*FrameFolderData)->TryGetObjectField(TEXT("rotation"), Rotation) && Rotation && Rotation->IsValid());

		if (PreviousLocation && PreviousLocation->IsValid())
		{
			TestTrue(TEXT("Viewport frame_folder previous location x matches"), FMath::IsNearlyEqual(static_cast<float>((*PreviousLocation)->GetNumberField(TEXT("x"))), ExpectedCameraLocation.X, 0.5f));
			TestTrue(TEXT("Viewport frame_folder previous location y matches"), FMath::IsNearlyEqual(static_cast<float>((*PreviousLocation)->GetNumberField(TEXT("y"))), ExpectedCameraLocation.Y, 0.5f));
			TestTrue(TEXT("Viewport frame_folder previous location z matches"), FMath::IsNearlyEqual(static_cast<float>((*PreviousLocation)->GetNumberField(TEXT("z"))), ExpectedCameraLocation.Z, 0.5f));
		}

		if (Rotation && Rotation->IsValid())
		{
			TestTrue(TEXT("Viewport frame_folder rotation pitch preserved"), FMath::IsNearlyEqual(static_cast<float>((*Rotation)->GetNumberField(TEXT("pitch"))), ExpectedCameraRotation.Pitch, 0.1f));
			TestTrue(TEXT("Viewport frame_folder rotation yaw preserved"), FMath::IsNearlyEqual(static_cast<float>((*Rotation)->GetNumberField(TEXT("yaw"))), ExpectedCameraRotation.Yaw, 0.1f));
			TestTrue(TEXT("Viewport frame_folder rotation roll preserved"), FMath::IsNearlyEqual(static_cast<float>((*Rotation)->GetNumberField(TEXT("roll"))), ExpectedCameraRotation.Roll, 0.1f));
		}

		TestTrue(TEXT("Viewport frame_folder fov preserved"), FMath::IsNearlyEqual(static_cast<float>((*FrameFolderData)->GetNumberField(TEXT("fov"))), static_cast<float>(ExpectedFov), 0.1f));

		if (PreviousLocation && PreviousLocation->IsValid() && NewLocation && NewLocation->IsValid())
		{
			const FVector PreviousVector(
				static_cast<float>((*PreviousLocation)->GetNumberField(TEXT("x"))),
				static_cast<float>((*PreviousLocation)->GetNumberField(TEXT("y"))),
				static_cast<float>((*PreviousLocation)->GetNumberField(TEXT("z"))));
			const FVector NewVector(
				static_cast<float>((*NewLocation)->GetNumberField(TEXT("x"))),
				static_cast<float>((*NewLocation)->GetNumberField(TEXT("y"))),
				static_cast<float>((*NewLocation)->GetNumberField(TEXT("z"))));
			TestTrue(TEXT("Viewport frame_folder moved camera"), !PreviousVector.Equals(NewVector, 0.1f));
		}
	}

	const FString DestroyFolderBody = MakeExecRequestBody(
		TEXT("smoke-viewport-folder-destroy-folder-001"),
		TEXT("level_destroy_folder_actors"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\"}"), *FolderPath));
	FHttpSmokeResult DestroyFolderResult;
	TestTrue(TEXT("Level destroy_folder_actors request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DestroyFolderBody, DestroyFolderResult));
	TestEqual(TEXT("Level destroy_folder_actors status code"), DestroyFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> DestroyFolderJson;
	TestTrue(TEXT("Level destroy_folder_actors response parses as JSON"), ParseJson(DestroyFolderResult.Body, DestroyFolderJson));
	TestTrue(TEXT("Level destroy_folder_actors root ok=true"), DestroyFolderJson.IsValid() && DestroyFolderJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* DestroyFolderData = nullptr;
	TestTrue(TEXT("Level destroy_folder_actors contains data object"), DestroyFolderJson.IsValid() && DestroyFolderJson->TryGetObjectField(TEXT("data"), DestroyFolderData) && DestroyFolderData && DestroyFolderData->IsValid());
	if (DestroyFolderData && DestroyFolderData->IsValid())
	{
		TestEqual(TEXT("Level destroy_folder_actors deleted_count"), static_cast<int32>((*DestroyFolderData)->GetIntegerField(TEXT("deleted_count"))), 2);
		TestEqual(TEXT("Level destroy_folder_actors matched_count"), static_cast<int32>((*DestroyFolderData)->GetIntegerField(TEXT("matched_count"))), 2);
	}

	const FString DestroyFolderAgainBody = MakeExecRequestBody(
		TEXT("smoke-viewport-folder-destroy-folder-002"),
		TEXT("level_destroy_folder_actors"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\"}"), *FolderPath));
	FHttpSmokeResult DestroyFolderAgainResult;
	TestTrue(TEXT("Level destroy_folder_actors second request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DestroyFolderAgainBody, DestroyFolderAgainResult));
	TestEqual(TEXT("Level destroy_folder_actors second status code"), DestroyFolderAgainResult.StatusCode, 200);

	TSharedPtr<FJsonObject> DestroyFolderAgainJson;
	TestTrue(TEXT("Level destroy_folder_actors second response parses as JSON"), ParseJson(DestroyFolderAgainResult.Body, DestroyFolderAgainJson));
	TestTrue(TEXT("Level destroy_folder_actors second root ok=true"), DestroyFolderAgainJson.IsValid() && DestroyFolderAgainJson->GetBoolField(TEXT("ok")));
	if (DestroyFolderAgainJson.IsValid())
	{
		const TSharedPtr<FJsonObject>* DestroyFolderAgainData = nullptr;
		if (DestroyFolderAgainJson->TryGetObjectField(TEXT("data"), DestroyFolderAgainData) && DestroyFolderAgainData && DestroyFolderAgainData->IsValid())
		{
			TestEqual(TEXT("Level destroy_folder_actors second deleted_count"), static_cast<int32>((*DestroyFolderAgainData)->GetIntegerField(TEXT("deleted_count"))), 0);
		}
	}

	DestroyActorByName(TEXT("smoke-viewport-folder-destroy-a-fallback"), ActorNameA);
	DestroyActorByName(TEXT("smoke-viewport-folder-destroy-b-fallback"), ActorNameB);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceViewportActorFrameAndPickSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.ViewportActorFrameAndPick",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceViewportActorFrameAndPickSmokeTest::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender())
	{
		AddInfo(TEXT("Skipping viewport frame/pick smoke because rendering is disabled in the current automation environment."));
		return true;
	}

	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ActorLabel = FString::Printf(TEXT("SmokeViewportFrameActor_%s"), *UniqueSuffix);
	const FVector ExpectedCameraLocation(-3600.0f, -1800.0f, 1400.0f);
	const FRotator ExpectedCameraRotation(-14.0f, 28.0f, 0.0f);
	const double ExpectedFov = 62.0;

	const FString SetCameraBody = MakeExecRequestBody(
		TEXT("smoke-viewport-actor-set-camera-001"),
		TEXT("viewport_set_camera"),
		FString::Printf(
			TEXT("{\"location\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"rotation\":{\"pitch\":%.3f,\"yaw\":%.3f,\"roll\":%.3f},\"fov\":%.3f}"),
			ExpectedCameraLocation.X,
			ExpectedCameraLocation.Y,
			ExpectedCameraLocation.Z,
			ExpectedCameraRotation.Pitch,
			ExpectedCameraRotation.Yaw,
			ExpectedCameraRotation.Roll,
			ExpectedFov));
	FHttpSmokeResult SetCameraResult;
	TestTrue(TEXT("Viewport set_camera request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetCameraBody, SetCameraResult));
	TestEqual(TEXT("Viewport set_camera status code"), SetCameraResult.StatusCode, 200);

	auto DestroyActorByName = [this, &ServerScope](const FString& RequestId, const FString& ActorName) -> void
	{
		if (ActorName.IsEmpty())
		{
			return;
		}

		const FString DestroyBody = MakeExecRequestBody(
			RequestId,
			TEXT("destroy_actor"),
			FString::Printf(TEXT("{\"id\":\"%s\"}"), *ActorName));
		FHttpSmokeResult DestroyResult;
		TestTrue(TEXT("Destroy actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DestroyBody, DestroyResult));
		TestEqual(TEXT("Destroy actor status code"), DestroyResult.StatusCode, 200);
	};

	const FString SpawnBody = MakeExecRequestBody(
		TEXT("smoke-viewport-actor-spawn-001"),
		TEXT("spawn_actor"),
		FString::Printf(
			TEXT("{\"class_path\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":900.0,\"y\":650.0,\"z\":220.0},\"scale\":{\"x\":12.0,\"y\":6.0,\"z\":5.0}}"),
			*ActorLabel));
	FHttpSmokeResult SpawnResult;
	TestTrue(TEXT("Spawn actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnBody, SpawnResult));
	TestEqual(TEXT("Spawn actor status code"), SpawnResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SpawnJson;
	TestTrue(TEXT("Spawn actor response parses as JSON"), ParseJson(SpawnResult.Body, SpawnJson));
	TestTrue(TEXT("Spawn actor root ok=true"), SpawnJson.IsValid() && SpawnJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* SpawnData = nullptr;
	TestTrue(TEXT("Spawn actor contains data object"), SpawnJson.IsValid() && SpawnJson->TryGetObjectField(TEXT("data"), SpawnData) && SpawnData && SpawnData->IsValid());
	if (!(SpawnData && SpawnData->IsValid()))
	{
		return false;
	}

	const FString ActorName = (*SpawnData)->GetStringField(TEXT("name"));
	TestFalse(TEXT("Spawn actor returned name"), ActorName.IsEmpty());
	if (ActorName.IsEmpty())
	{
		return false;
	}

	const FString FrameActorBody = MakeExecRequestBody(
		TEXT("smoke-viewport-actor-frame-001"),
		TEXT("viewport_frame_actor"),
		FString::Printf(TEXT("{\"id\":\"%s\",\"padding\":1.15}"), *ActorName));
	FHttpSmokeResult FrameActorResult;
	TestTrue(TEXT("Viewport frame_actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, FrameActorBody, FrameActorResult));
	TestEqual(TEXT("Viewport frame_actor status code"), FrameActorResult.StatusCode, 200);

	TSharedPtr<FJsonObject> FrameActorJson;
	TestTrue(TEXT("Viewport frame_actor response parses as JSON"), ParseJson(FrameActorResult.Body, FrameActorJson));
	TestTrue(TEXT("Viewport frame_actor root ok=true"), FrameActorJson.IsValid() && FrameActorJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* FrameActorData = nullptr;
	TestTrue(TEXT("Viewport frame_actor contains data object"), FrameActorJson.IsValid() && FrameActorJson->TryGetObjectField(TEXT("data"), FrameActorData) && FrameActorData && FrameActorData->IsValid());
	if (FrameActorData && FrameActorData->IsValid())
	{
		const TSharedPtr<FJsonObject>* ActorSummary = nullptr;
		const TSharedPtr<FJsonObject>* PreviousLocation = nullptr;
		const TSharedPtr<FJsonObject>* NewLocation = nullptr;
		const TSharedPtr<FJsonObject>* Rotation = nullptr;
		TestTrue(TEXT("Viewport frame_actor contains actor summary"), (*FrameActorData)->TryGetObjectField(TEXT("actor"), ActorSummary) && ActorSummary && ActorSummary->IsValid());
		TestTrue(TEXT("Viewport frame_actor contains bounds"), (*FrameActorData)->HasTypedField<EJson::Object>(TEXT("bounds")));
		TestTrue(TEXT("Viewport frame_actor contains previous_location"), (*FrameActorData)->TryGetObjectField(TEXT("previous_location"), PreviousLocation) && PreviousLocation && PreviousLocation->IsValid());
		TestTrue(TEXT("Viewport frame_actor contains new_location"), (*FrameActorData)->TryGetObjectField(TEXT("new_location"), NewLocation) && NewLocation && NewLocation->IsValid());
		TestTrue(TEXT("Viewport frame_actor contains rotation"), (*FrameActorData)->TryGetObjectField(TEXT("rotation"), Rotation) && Rotation && Rotation->IsValid());
		TestTrue(TEXT("Viewport frame_actor padding preserved"), FMath::IsNearlyEqual(static_cast<float>((*FrameActorData)->GetNumberField(TEXT("padding"))), 1.15f, 0.01f));

		if (ActorSummary && ActorSummary->IsValid())
		{
			TestEqual(TEXT("Viewport frame_actor actor name matches"), (*ActorSummary)->GetStringField(TEXT("name")), ActorName);
			TestEqual(TEXT("Viewport frame_actor actor label matches"), (*ActorSummary)->GetStringField(TEXT("label")), ActorLabel);
		}

		if (PreviousLocation && PreviousLocation->IsValid())
		{
			TestTrue(TEXT("Viewport frame_actor previous location x matches"), FMath::IsNearlyEqual(static_cast<float>((*PreviousLocation)->GetNumberField(TEXT("x"))), ExpectedCameraLocation.X, 0.5f));
			TestTrue(TEXT("Viewport frame_actor previous location y matches"), FMath::IsNearlyEqual(static_cast<float>((*PreviousLocation)->GetNumberField(TEXT("y"))), ExpectedCameraLocation.Y, 0.5f));
			TestTrue(TEXT("Viewport frame_actor previous location z matches"), FMath::IsNearlyEqual(static_cast<float>((*PreviousLocation)->GetNumberField(TEXT("z"))), ExpectedCameraLocation.Z, 0.5f));
		}

		if (Rotation && Rotation->IsValid())
		{
			TestTrue(TEXT("Viewport frame_actor rotation pitch preserved"), FMath::IsNearlyEqual(static_cast<float>((*Rotation)->GetNumberField(TEXT("pitch"))), ExpectedCameraRotation.Pitch, 0.1f));
			TestTrue(TEXT("Viewport frame_actor rotation yaw preserved"), FMath::IsNearlyEqual(static_cast<float>((*Rotation)->GetNumberField(TEXT("yaw"))), ExpectedCameraRotation.Yaw, 0.1f));
			TestTrue(TEXT("Viewport frame_actor rotation roll preserved"), FMath::IsNearlyEqual(static_cast<float>((*Rotation)->GetNumberField(TEXT("roll"))), ExpectedCameraRotation.Roll, 0.1f));
		}

		TestTrue(TEXT("Viewport frame_actor fov preserved"), FMath::IsNearlyEqual(static_cast<float>((*FrameActorData)->GetNumberField(TEXT("fov"))), static_cast<float>(ExpectedFov), 0.1f));

		if (PreviousLocation && PreviousLocation->IsValid() && NewLocation && NewLocation->IsValid())
		{
			const FVector PreviousVector(
				static_cast<float>((*PreviousLocation)->GetNumberField(TEXT("x"))),
				static_cast<float>((*PreviousLocation)->GetNumberField(TEXT("y"))),
				static_cast<float>((*PreviousLocation)->GetNumberField(TEXT("z"))));
			const FVector NewVector(
				static_cast<float>((*NewLocation)->GetNumberField(TEXT("x"))),
				static_cast<float>((*NewLocation)->GetNumberField(TEXT("y"))),
				static_cast<float>((*NewLocation)->GetNumberField(TEXT("z"))));
			TestTrue(TEXT("Viewport frame_actor moved camera"), !PreviousVector.Equals(NewVector, 0.1f));
		}
	}

	FString WallActorName;
	{
		const FVector TargetLocation(50000.0f, 52000.0f, 220.0f);
		const FVector WallLocation(TargetLocation.X - 300.0f, TargetLocation.Y, TargetLocation.Z);

		const FString RepositionBody = MakeExecRequestBody(
			TEXT("smoke-viewport-actor-collision-reposition-001"),
			TEXT("level_set_actor_transform"),
			FString::Printf(
				TEXT("{\"id\":\"%s\",\"location\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"rotation\":{\"pitch\":0.0,\"yaw\":0.0,\"roll\":0.0},\"scale\":{\"x\":10.0,\"y\":10.0,\"z\":10.0}}"),
				*ActorName,
				TargetLocation.X,
				TargetLocation.Y,
				TargetLocation.Z));
		FHttpSmokeResult RepositionResult;
		TestTrue(TEXT("Reposition actor for collision-aware frame request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RepositionBody, RepositionResult));
		TestEqual(TEXT("Reposition actor for collision-aware frame status code"), RepositionResult.StatusCode, 200);

		const FString WallLabel = FString::Printf(TEXT("SmokeViewportFrameWall_%s"), *UniqueSuffix);
		const FString WallSpawnBody = MakeExecRequestBody(
			TEXT("smoke-viewport-actor-collision-wall-spawn-001"),
			TEXT("spawn_actor"),
			FString::Printf(
				TEXT("{\"class_path\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"scale\":{\"x\":0.5,\"y\":40.0,\"z\":40.0}}"),
				*WallLabel,
				WallLocation.X,
				WallLocation.Y,
				WallLocation.Z));
		FHttpSmokeResult WallSpawnResult;
		TestTrue(TEXT("Spawn wall actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, WallSpawnBody, WallSpawnResult));
		TestEqual(TEXT("Spawn wall actor status code"), WallSpawnResult.StatusCode, 200);

		TSharedPtr<FJsonObject> WallSpawnJson;
		TestTrue(TEXT("Spawn wall actor response parses as JSON"), ParseJson(WallSpawnResult.Body, WallSpawnJson));
		TestTrue(TEXT("Spawn wall actor root ok=true"), WallSpawnJson.IsValid() && WallSpawnJson->GetBoolField(TEXT("ok")));
		if (WallSpawnJson.IsValid())
		{
			const TSharedPtr<FJsonObject>* WallSpawnData = nullptr;
			if (WallSpawnJson->TryGetObjectField(TEXT("data"), WallSpawnData) && WallSpawnData && WallSpawnData->IsValid())
			{
				WallActorName = (*WallSpawnData)->GetStringField(TEXT("name"));
			}
		}
		TestFalse(TEXT("Spawn wall actor returned name"), WallActorName.IsEmpty());

		const FVector CollisionCameraLocation(TargetLocation.X - 2500.0f, TargetLocation.Y, TargetLocation.Z);
		const FString SetCollisionCameraBody = MakeExecRequestBody(
			TEXT("smoke-viewport-actor-collision-set-camera-001"),
			TEXT("viewport_set_camera"),
			FString::Printf(
				TEXT("{\"location\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"rotation\":{\"pitch\":0.0,\"yaw\":0.0,\"roll\":0.0},\"fov\":60.0}"),
				CollisionCameraLocation.X,
				CollisionCameraLocation.Y,
				CollisionCameraLocation.Z));
		FHttpSmokeResult SetCollisionCameraResult;
		TestTrue(TEXT("Viewport set_camera for collision-aware frame request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetCollisionCameraBody, SetCollisionCameraResult));
		TestEqual(TEXT("Viewport set_camera for collision-aware frame status code"), SetCollisionCameraResult.StatusCode, 200);

		const FString FrameCollisionBody = MakeExecRequestBody(
			TEXT("smoke-viewport-actor-collision-frame-001"),
			TEXT("viewport_frame_actor"),
			FString::Printf(TEXT("{\"id\":\"%s\",\"padding\":1.1,\"collision_aware\":true,\"safety_offset_cm\":10.0}"), *ActorName));
		FHttpSmokeResult FrameCollisionResult;
		TestTrue(TEXT("Viewport frame_actor collision-aware request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, FrameCollisionBody, FrameCollisionResult));
		TestEqual(TEXT("Viewport frame_actor collision-aware status code"), FrameCollisionResult.StatusCode, 200);

		TSharedPtr<FJsonObject> FrameCollisionJson;
		TestTrue(TEXT("Viewport frame_actor collision-aware response parses as JSON"), ParseJson(FrameCollisionResult.Body, FrameCollisionJson));
		TestTrue(TEXT("Viewport frame_actor collision-aware root ok=true"), FrameCollisionJson.IsValid() && FrameCollisionJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* FrameCollisionData = nullptr;
		TestTrue(TEXT("Viewport frame_actor collision-aware contains data object"), FrameCollisionJson.IsValid() && FrameCollisionJson->TryGetObjectField(TEXT("data"), FrameCollisionData) && FrameCollisionData && FrameCollisionData->IsValid());
		if (FrameCollisionData && FrameCollisionData->IsValid())
		{
			TestTrue(TEXT("Viewport frame_actor collision-aware flag true"), (*FrameCollisionData)->GetBoolField(TEXT("collision_aware")));
			TestTrue(TEXT("Viewport frame_actor collision-adjusted true"), (*FrameCollisionData)->GetBoolField(TEXT("collision_adjusted")));

			const TSharedPtr<FJsonObject>* DesiredLocation = nullptr;
			const TSharedPtr<FJsonObject>* NewLocation = nullptr;
			const TSharedPtr<FJsonObject>* CollisionTrace = nullptr;
			TestTrue(TEXT("Viewport frame_actor collision-aware contains desired_location"), (*FrameCollisionData)->TryGetObjectField(TEXT("desired_location"), DesiredLocation) && DesiredLocation && DesiredLocation->IsValid());
			TestTrue(TEXT("Viewport frame_actor collision-aware contains new_location"), (*FrameCollisionData)->TryGetObjectField(TEXT("new_location"), NewLocation) && NewLocation && NewLocation->IsValid());
			TestTrue(TEXT("Viewport frame_actor collision-aware contains collision_trace"), (*FrameCollisionData)->TryGetObjectField(TEXT("collision_trace"), CollisionTrace) && CollisionTrace && CollisionTrace->IsValid());
			if (CollisionTrace && CollisionTrace->IsValid())
			{
				TestTrue(TEXT("Viewport frame_actor collision-aware trace hit"), (*CollisionTrace)->GetBoolField(TEXT("hit")));
			}
			if (DesiredLocation && DesiredLocation->IsValid() && NewLocation && NewLocation->IsValid())
			{
				const FVector DesiredVector(
					static_cast<float>((*DesiredLocation)->GetNumberField(TEXT("x"))),
					static_cast<float>((*DesiredLocation)->GetNumberField(TEXT("y"))),
					static_cast<float>((*DesiredLocation)->GetNumberField(TEXT("z"))));
				const FVector NewVector(
					static_cast<float>((*NewLocation)->GetNumberField(TEXT("x"))),
					static_cast<float>((*NewLocation)->GetNumberField(TEXT("y"))),
					static_cast<float>((*NewLocation)->GetNumberField(TEXT("z"))));
				TestTrue(TEXT("Viewport frame_actor collision-aware moved camera off desired"), !DesiredVector.Equals(NewVector, 0.1f));
			}
		}
	}

	const FString RepositionForPickBody = MakeExecRequestBody(
		TEXT("smoke-viewport-actor-reposition-001"),
		TEXT("level_set_actor_transform"),
		FString::Printf(
			TEXT("{\"id\":\"%s\",\"location\":{\"x\":6000.0,\"y\":0.0,\"z\":250.0},\"rotation\":{\"pitch\":0.0,\"yaw\":0.0,\"roll\":0.0},\"scale\":{\"x\":50.0,\"y\":50.0,\"z\":20.0}}"),
			*ActorName));
	FHttpSmokeResult RepositionForPickResult;
	TestTrue(TEXT("Level set_actor_transform request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RepositionForPickBody, RepositionForPickResult));
	TestEqual(TEXT("Level set_actor_transform status code"), RepositionForPickResult.StatusCode, 200);

	const FString SetPickCameraBody = MakeExecRequestBody(
		TEXT("smoke-viewport-pick-set-camera-001"),
		TEXT("viewport_set_camera"),
		TEXT("{\"location\":{\"x\":0.0,\"y\":0.0,\"z\":250.0},\"rotation\":{\"pitch\":0.0,\"yaw\":0.0,\"roll\":0.0},\"fov\":60.0}"));
	FHttpSmokeResult SetPickCameraResult;
	TestTrue(TEXT("Viewport set_camera for pick request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetPickCameraBody, SetPickCameraResult));
	TestEqual(TEXT("Viewport set_camera for pick status code"), SetPickCameraResult.StatusCode, 200);

	const FIntPoint PickScreenPoint(400, 300);
	const FString DeprojectBody = MakeExecRequestBody(
		TEXT("smoke-viewport-pick-deproject-001"),
		TEXT("viewport_deproject_screen_to_world"),
		FString::Printf(TEXT("{\"screen_x\":%d,\"screen_y\":%d}"), PickScreenPoint.X, PickScreenPoint.Y));
	FHttpSmokeResult DeprojectResult;
	TestTrue(TEXT("Viewport deproject request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DeprojectBody, DeprojectResult));
	TestEqual(TEXT("Viewport deproject status code"), DeprojectResult.StatusCode, 200);

	TSharedPtr<FJsonObject> DeprojectJson;
	TestTrue(TEXT("Viewport deproject response parses as JSON"), ParseJson(DeprojectResult.Body, DeprojectJson));
	TestTrue(TEXT("Viewport deproject root ok=true"), DeprojectJson.IsValid() && DeprojectJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* DeprojectData = nullptr;
	TestTrue(TEXT("Viewport deproject contains data object"), DeprojectJson.IsValid() && DeprojectJson->TryGetObjectField(TEXT("data"), DeprojectData) && DeprojectData && DeprojectData->IsValid());

	FVector DeprojectOrigin = FVector::ZeroVector;
	FVector DeprojectDirection = FVector::ForwardVector;
	if (DeprojectData && DeprojectData->IsValid())
	{
		const TSharedPtr<FJsonObject>* OriginObject = nullptr;
		const TSharedPtr<FJsonObject>* DirectionObject = nullptr;
		TestTrue(TEXT("Viewport deproject contains world_origin"), (*DeprojectData)->TryGetObjectField(TEXT("world_origin"), OriginObject) && OriginObject && OriginObject->IsValid());
		TestTrue(TEXT("Viewport deproject contains world_direction"), (*DeprojectData)->TryGetObjectField(TEXT("world_direction"), DirectionObject) && DirectionObject && DirectionObject->IsValid());

		if (OriginObject && OriginObject->IsValid())
		{
			DeprojectOrigin = FVector(
				static_cast<float>((*OriginObject)->GetNumberField(TEXT("x"))),
				static_cast<float>((*OriginObject)->GetNumberField(TEXT("y"))),
				static_cast<float>((*OriginObject)->GetNumberField(TEXT("z"))));
		}
		if (DirectionObject && DirectionObject->IsValid())
		{
			DeprojectDirection = FVector(
				static_cast<float>((*DirectionObject)->GetNumberField(TEXT("x"))),
				static_cast<float>((*DirectionObject)->GetNumberField(TEXT("y"))),
				static_cast<float>((*DirectionObject)->GetNumberField(TEXT("z")))).GetSafeNormal();
		}
	}

	const FVector PickLocation = DeprojectOrigin + (DeprojectDirection * 900.0f);
	const FString RepositionForPrecisePickBody = MakeExecRequestBody(
		TEXT("smoke-viewport-actor-reposition-002"),
		TEXT("level_set_actor_transform"),
		FString::Printf(
			TEXT("{\"id\":\"%s\",\"location\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"rotation\":{\"pitch\":0.0,\"yaw\":0.0,\"roll\":0.0},\"scale\":{\"x\":12.0,\"y\":12.0,\"z\":12.0}}"),
			*ActorName,
			PickLocation.X,
			PickLocation.Y,
			PickLocation.Z));
	FHttpSmokeResult RepositionForPrecisePickResult;
	TestTrue(TEXT("Level set_actor_transform for precise pick request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RepositionForPrecisePickBody, RepositionForPrecisePickResult));
	TestEqual(TEXT("Level set_actor_transform for precise pick status code"), RepositionForPrecisePickResult.StatusCode, 200);

	const FString PickBody = MakeExecRequestBody(
		TEXT("smoke-viewport-pick-actor-001"),
		TEXT("viewport_pick_actor_at_screen"),
		FString::Printf(TEXT("{\"screen_x\":%d,\"screen_y\":%d,\"trace_distance\":50000.0}"), PickScreenPoint.X, PickScreenPoint.Y));
	FHttpSmokeResult PickResult;
	TestTrue(TEXT("Viewport pick_actor_at_screen request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, PickBody, PickResult));
	TestEqual(TEXT("Viewport pick_actor_at_screen status code"), PickResult.StatusCode, 200);

	TSharedPtr<FJsonObject> PickJson;
	TestTrue(TEXT("Viewport pick_actor_at_screen response parses as JSON"), ParseJson(PickResult.Body, PickJson));
	TestTrue(TEXT("Viewport pick_actor_at_screen root ok=true"), PickJson.IsValid() && PickJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* PickData = nullptr;
	TestTrue(TEXT("Viewport pick_actor_at_screen contains data object"), PickJson.IsValid() && PickJson->TryGetObjectField(TEXT("data"), PickData) && PickData && PickData->IsValid());
	if (PickData && PickData->IsValid())
	{
		const TSharedPtr<FJsonObject>* PickedActor = nullptr;
		TestTrue(TEXT("Viewport pick_actor_at_screen contains actor summary"), (*PickData)->TryGetObjectField(TEXT("actor"), PickedActor) && PickedActor && PickedActor->IsValid());
		TestTrue(TEXT("Viewport pick_actor_at_screen contains screen_point"), (*PickData)->HasTypedField<EJson::Object>(TEXT("screen_point")));
		TestTrue(TEXT("Viewport pick_actor_at_screen contains world_origin"), (*PickData)->HasTypedField<EJson::Object>(TEXT("world_origin")));
		TestTrue(TEXT("Viewport pick_actor_at_screen contains world_direction"), (*PickData)->HasTypedField<EJson::Object>(TEXT("world_direction")));
		TestTrue(TEXT("Viewport pick_actor_at_screen contains trace_channel"), (*PickData)->HasTypedField<EJson::String>(TEXT("trace_channel")));
		TestTrue(TEXT("Viewport pick_actor_at_screen contains hit distance"), (*PickData)->HasTypedField<EJson::Number>(TEXT("distance")));
		TestEqual(TEXT("Viewport pick_actor_at_screen actor_name matches"), (*PickData)->GetStringField(TEXT("actor_name")), ActorName);

		if (PickedActor && PickedActor->IsValid())
		{
			TestEqual(TEXT("Viewport pick_actor_at_screen actor summary name matches"), (*PickedActor)->GetStringField(TEXT("name")), ActorName);
			TestEqual(TEXT("Viewport pick_actor_at_screen actor summary label matches"), (*PickedActor)->GetStringField(TEXT("label")), ActorLabel);
		}
	}

	DestroyActorByName(TEXT("smoke-viewport-actor-destroy-001"), ActorName);
	DestroyActorByName(TEXT("smoke-viewport-actor-destroy-wall-001"), WallActorName);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceViewportSelectAndNearbyObbsSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.ViewportSelectAndNearbyObbs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceViewportSelectAndNearbyObbsSmokeTest::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender())
	{
		AddInfo(TEXT("Skipping viewport select/obb smoke because rendering is disabled in the current automation environment."));
		return true;
	}

	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString SubjectLabel = FString::Printf(TEXT("SmokeViewportSelectSubject_%s"), *UniqueSuffix);
	const FString NearbyLabel = FString::Printf(TEXT("SmokeViewportSelectNearby_%s"), *UniqueSuffix);
	const FString FarLabel = FString::Printf(TEXT("SmokeViewportSelectFar_%s"), *UniqueSuffix);

	auto SpawnCubeActor = [this, &ServerScope](const FString& RequestId, const FString& ActorLabel, const FVector& Location, const FVector& Scale, FString& OutActorName) -> bool
	{
		const FString SpawnBody = MakeExecRequestBody(
			RequestId,
			TEXT("spawn_actor"),
			FString::Printf(
				TEXT("{\"class_path\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"scale\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}}"),
				*ActorLabel,
				Location.X, Location.Y, Location.Z,
				Scale.X, Scale.Y, Scale.Z));
		FHttpSmokeResult SpawnResult;
		TestTrue(TEXT("Spawn cube actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnBody, SpawnResult));
		TestEqual(TEXT("Spawn cube actor status code"), SpawnResult.StatusCode, 200);

		TSharedPtr<FJsonObject> SpawnJson;
		TestTrue(TEXT("Spawn cube actor response parses as JSON"), ParseJson(SpawnResult.Body, SpawnJson));
		TestTrue(TEXT("Spawn cube actor root ok=true"), SpawnJson.IsValid() && SpawnJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* SpawnData = nullptr;
		TestTrue(TEXT("Spawn cube actor contains data object"), SpawnJson.IsValid() && SpawnJson->TryGetObjectField(TEXT("data"), SpawnData) && SpawnData && SpawnData->IsValid());
		if (!(SpawnData && SpawnData->IsValid()))
		{
			return false;
		}

		OutActorName = (*SpawnData)->GetStringField(TEXT("name"));
		return !OutActorName.IsEmpty();
	};

	auto DestroyActorByName = [this, &ServerScope](const FString& RequestId, const FString& ActorName) -> void
	{
		if (ActorName.IsEmpty())
		{
			return;
		}

		const FString DestroyBody = MakeExecRequestBody(RequestId, TEXT("destroy_actor"), FString::Printf(TEXT("{\"id\":\"%s\"}"), *ActorName));
		FHttpSmokeResult DestroyResult;
		ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DestroyBody, DestroyResult);
	};

	const FString SetCameraBody = MakeExecRequestBody(
		TEXT("smoke-viewport-select-set-camera-001"),
		TEXT("viewport_set_camera"),
		TEXT("{\"location\":{\"x\":0.0,\"y\":0.0,\"z\":250.0},\"rotation\":{\"pitch\":0.0,\"yaw\":0.0,\"roll\":0.0},\"fov\":60.0}"));
	FHttpSmokeResult SetCameraResult;
	TestTrue(TEXT("Viewport set_camera request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetCameraBody, SetCameraResult));
	TestEqual(TEXT("Viewport set_camera status code"), SetCameraResult.StatusCode, 200);

	const FIntPoint PickScreenPoint(400, 300);
	const FString DeprojectBody = MakeExecRequestBody(
		TEXT("smoke-viewport-select-deproject-001"),
		TEXT("viewport_deproject_screen_to_world"),
		FString::Printf(TEXT("{\"screen_x\":%d,\"screen_y\":%d}"), PickScreenPoint.X, PickScreenPoint.Y));
	FHttpSmokeResult DeprojectResult;
	TestTrue(TEXT("Viewport deproject request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DeprojectBody, DeprojectResult));
	TestEqual(TEXT("Viewport deproject status code"), DeprojectResult.StatusCode, 200);

	TSharedPtr<FJsonObject> DeprojectJson;
	TestTrue(TEXT("Viewport deproject response parses as JSON"), ParseJson(DeprojectResult.Body, DeprojectJson));
	TestTrue(TEXT("Viewport deproject root ok=true"), DeprojectJson.IsValid() && DeprojectJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* DeprojectData = nullptr;
	TestTrue(TEXT("Viewport deproject contains data"), DeprojectJson.IsValid() && DeprojectJson->TryGetObjectField(TEXT("data"), DeprojectData) && DeprojectData && DeprojectData->IsValid());

	FVector DeprojectOrigin = FVector::ZeroVector;
	FVector DeprojectDirection = FVector::ForwardVector;
	if (DeprojectData && DeprojectData->IsValid())
	{
		const TSharedPtr<FJsonObject>* OriginObject = nullptr;
		const TSharedPtr<FJsonObject>* DirectionObject = nullptr;
		if ((*DeprojectData)->TryGetObjectField(TEXT("world_origin"), OriginObject) && OriginObject && OriginObject->IsValid())
		{
			DeprojectOrigin = FVector(
				static_cast<float>((*OriginObject)->GetNumberField(TEXT("x"))),
				static_cast<float>((*OriginObject)->GetNumberField(TEXT("y"))),
				static_cast<float>((*OriginObject)->GetNumberField(TEXT("z"))));
		}
		if ((*DeprojectData)->TryGetObjectField(TEXT("world_direction"), DirectionObject) && DirectionObject && DirectionObject->IsValid())
		{
			DeprojectDirection = FVector(
				static_cast<float>((*DirectionObject)->GetNumberField(TEXT("x"))),
				static_cast<float>((*DirectionObject)->GetNumberField(TEXT("y"))),
				static_cast<float>((*DirectionObject)->GetNumberField(TEXT("z")))).GetSafeNormal();
		}
	}

	const FVector SubjectLocation = DeprojectOrigin + (DeprojectDirection * 900.0f);
	const FVector NearbyLocation = SubjectLocation + FVector(320.0f, 0.0f, 0.0f);
	const FVector FarLocation = SubjectLocation + FVector(1800.0f, 0.0f, 0.0f);

	FString SubjectActorName;
	FString NearbyActorName;
	FString FarActorName;
	if (!SpawnCubeActor(TEXT("smoke-viewport-select-spawn-001"), SubjectLabel, SubjectLocation, FVector(10.0f, 8.0f, 6.0f), SubjectActorName) ||
		!SpawnCubeActor(TEXT("smoke-viewport-select-spawn-002"), NearbyLabel, NearbyLocation, FVector(4.0f, 4.0f, 4.0f), NearbyActorName) ||
		!SpawnCubeActor(TEXT("smoke-viewport-select-spawn-003"), FarLabel, FarLocation, FVector(4.0f, 4.0f, 4.0f), FarActorName))
	{
		DestroyActorByName(TEXT("smoke-viewport-select-destroy-subject-early"), SubjectActorName);
		DestroyActorByName(TEXT("smoke-viewport-select-destroy-nearby-early"), NearbyActorName);
		DestroyActorByName(TEXT("smoke-viewport-select-destroy-far-early"), FarActorName);
		return false;
	}

	const FString SelectBody = MakeExecRequestBody(
		TEXT("smoke-viewport-select-actor-001"),
		TEXT("viewport_select_actor_at_screen"),
		FString::Printf(TEXT("{\"screen_x\":%d,\"screen_y\":%d,\"selection_mode\":\"replace\",\"trace_distance\":50000.0}"), PickScreenPoint.X, PickScreenPoint.Y));
	FHttpSmokeResult SelectResult;
	TestTrue(TEXT("Viewport select_actor_at_screen request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SelectBody, SelectResult));
	TestEqual(TEXT("Viewport select_actor_at_screen status code"), SelectResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SelectJson;
	TestTrue(TEXT("Viewport select_actor_at_screen parses as JSON"), ParseJson(SelectResult.Body, SelectJson));
	TestTrue(TEXT("Viewport select_actor_at_screen root ok=true"), SelectJson.IsValid() && SelectJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* SelectData = nullptr;
	TestTrue(TEXT("Viewport select_actor_at_screen contains data"), SelectJson.IsValid() && SelectJson->TryGetObjectField(TEXT("data"), SelectData) && SelectData && SelectData->IsValid());
	if (SelectData && SelectData->IsValid())
	{
		const TSharedPtr<FJsonObject>* SelectedActor = nullptr;
		TestTrue(TEXT("Viewport select_actor_at_screen returns actor info"), (*SelectData)->TryGetObjectField(TEXT("actor_info"), SelectedActor) && SelectedActor && SelectedActor->IsValid());
		if (SelectedActor && SelectedActor->IsValid())
		{
			TestEqual(TEXT("Viewport select_actor_at_screen selected actor label"), (*SelectedActor)->GetStringField(TEXT("label")), SubjectLabel);
		}
	}

	const FString NearbyObbBody = MakeExecRequestBody(
		TEXT("smoke-level-nearby-obbs-001"),
		TEXT("level_get_nearby_actor_obbs"),
		FString::Printf(TEXT("{\"id\":\"%s\",\"radius\":600.0}"), *SubjectActorName));
	FHttpSmokeResult NearbyObbResult;
	TestTrue(TEXT("Level nearby actor obbs request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, NearbyObbBody, NearbyObbResult));
	TestEqual(TEXT("Level nearby actor obbs status code"), NearbyObbResult.StatusCode, 200);

	TSharedPtr<FJsonObject> NearbyObbJson;
	TestTrue(TEXT("Level nearby actor obbs parses as JSON"), ParseJson(NearbyObbResult.Body, NearbyObbJson));
	TestTrue(TEXT("Level nearby actor obbs root ok=true"), NearbyObbJson.IsValid() && NearbyObbJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* NearbyObbData = nullptr;
	TestTrue(TEXT("Level nearby actor obbs contains data"), NearbyObbJson.IsValid() && NearbyObbJson->TryGetObjectField(TEXT("data"), NearbyObbData) && NearbyObbData && NearbyObbData->IsValid());
	if (NearbyObbData && NearbyObbData->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* NearbyActors = nullptr;
		TestTrue(TEXT("Level nearby actor obbs returns actors array"), (*NearbyObbData)->TryGetArrayField(TEXT("actors"), NearbyActors) && NearbyActors);
		if (NearbyActors)
		{
			TestTrue(TEXT("Level nearby actor obbs returns at least one actor"), NearbyActors->Num() >= 1);
			bool bFoundNearby = false;
			bool bFoundFar = false;
			for (const TSharedPtr<FJsonValue>& ItemValue : *NearbyActors)
			{
				if (!ItemValue.IsValid() || ItemValue->Type != EJson::Object)
				{
					continue;
				}
				const TSharedPtr<FJsonObject> ItemObject = ItemValue->AsObject();
				const TSharedPtr<FJsonObject>* ActorObject = nullptr;
				if (!ItemObject.IsValid() || !ItemObject->TryGetObjectField(TEXT("actor"), ActorObject) || !ActorObject || !ActorObject->IsValid())
				{
					continue;
				}

				const FString Label = (*ActorObject)->GetStringField(TEXT("label"));
				bFoundNearby |= Label == NearbyLabel;
				bFoundFar |= Label == FarLabel;
				TestTrue(TEXT("Level nearby actor obbs entry contains obb"), ItemObject->HasTypedField<EJson::Object>(TEXT("obb")));
			}
			TestTrue(TEXT("Level nearby actor obbs includes nearby actor"), bFoundNearby);
			TestFalse(TEXT("Level nearby actor obbs excludes far actor"), bFoundFar);
		}
	}

	DestroyActorByName(TEXT("smoke-viewport-select-destroy-001"), SubjectActorName);
	DestroyActorByName(TEXT("smoke-viewport-select-destroy-002"), NearbyActorName);
	DestroyActorByName(TEXT("smoke-viewport-select-destroy-003"), FarActorName);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceModelingReadonlySmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.ModelingReadonlyQueries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceModelingReadonlySmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString ActivateBody = MakeExecRequestBody(TEXT("smoke-modeling-activate-001"), TEXT("modeling_activate_mode"));
	FHttpSmokeResult ActivateResult;
	TestTrue(TEXT("Modeling activate request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ActivateBody, ActivateResult));
	TestEqual(TEXT("Modeling activate status code"), ActivateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ActivateJson;
	TestTrue(TEXT("Modeling activate response parses as JSON"), ParseJson(ActivateResult.Body, ActivateJson));
	TestTrue(TEXT("Modeling activate root ok=true"), ActivateJson.IsValid() && ActivateJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* ActivateData = nullptr;
	TestTrue(TEXT("Modeling activate contains data object"), ActivateJson.IsValid() && ActivateJson->TryGetObjectField(TEXT("data"), ActivateData) && ActivateData && ActivateData->IsValid());
	if (ActivateData && ActivateData->IsValid())
	{
		TestTrue(TEXT("Modeling activate reports mode_active"), (*ActivateData)->GetBoolField(TEXT("mode_active")));
		TestTrue(TEXT("Modeling activate returns mode id"), !(*ActivateData)->GetStringField(TEXT("mode_id")).IsEmpty());
	}

	const FString GetSelectionBody = MakeExecRequestBody(TEXT("smoke-modeling-selection-001"), TEXT("modeling_get_selection"));
	FHttpSmokeResult GetSelectionResult;
	TestTrue(TEXT("Modeling get_selection request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetSelectionBody, GetSelectionResult));
	TestEqual(TEXT("Modeling get_selection status code"), GetSelectionResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetSelectionJson;
	TestTrue(TEXT("Modeling get_selection response parses as JSON"), ParseJson(GetSelectionResult.Body, GetSelectionJson));
	TestTrue(TEXT("Modeling get_selection root ok=true"), GetSelectionJson.IsValid() && GetSelectionJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* GetSelectionData = nullptr;
	TestTrue(TEXT("Modeling get_selection contains data object"), GetSelectionJson.IsValid() && GetSelectionJson->TryGetObjectField(TEXT("data"), GetSelectionData) && GetSelectionData && GetSelectionData->IsValid());
	if (GetSelectionData && GetSelectionData->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* SelectedActors = nullptr;
		TestTrue(TEXT("Modeling get_selection contains selected_actors array"), (*GetSelectionData)->TryGetArrayField(TEXT("selected_actors"), SelectedActors) && SelectedActors != nullptr);
		TestTrue(TEXT("Modeling get_selection contains mesh_selection_element_type"), (*GetSelectionData)->HasTypedField<EJson::String>(TEXT("mesh_selection_element_type")));
		TestTrue(TEXT("Modeling get_selection contains mesh_topology_mode"), (*GetSelectionData)->HasTypedField<EJson::String>(TEXT("mesh_topology_mode")));
		TestTrue(TEXT("Modeling get_selection contains mesh_target_count"), (*GetSelectionData)->HasTypedField<EJson::Number>(TEXT("mesh_target_count")));
		TestTrue(TEXT("Modeling get_selection contains mesh_selection_empty"), (*GetSelectionData)->HasTypedField<EJson::Boolean>(TEXT("mesh_selection_empty")));
	}

	const FString GetMeshSelectionInfoBody = MakeExecRequestBody(TEXT("smoke-modeling-mesh-selection-001"), TEXT("modeling_get_mesh_selection_info"));
	FHttpSmokeResult GetMeshSelectionInfoResult;
	TestTrue(TEXT("Modeling get_mesh_selection_info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetMeshSelectionInfoBody, GetMeshSelectionInfoResult));
	TestEqual(TEXT("Modeling get_mesh_selection_info status code"), GetMeshSelectionInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetMeshSelectionInfoJson;
	TestTrue(TEXT("Modeling get_mesh_selection_info response parses as JSON"), ParseJson(GetMeshSelectionInfoResult.Body, GetMeshSelectionInfoJson));
	TestTrue(TEXT("Modeling get_mesh_selection_info root ok=true"), GetMeshSelectionInfoJson.IsValid() && GetMeshSelectionInfoJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* GetMeshSelectionInfoData = nullptr;
	TestTrue(TEXT("Modeling get_mesh_selection_info contains data object"), GetMeshSelectionInfoJson.IsValid() && GetMeshSelectionInfoJson->TryGetObjectField(TEXT("data"), GetMeshSelectionInfoData) && GetMeshSelectionInfoData && GetMeshSelectionInfoData->IsValid());
	if (GetMeshSelectionInfoData && GetMeshSelectionInfoData->IsValid())
	{
		TestTrue(TEXT("Modeling get_mesh_selection_info contains element_type"), (*GetMeshSelectionInfoData)->HasTypedField<EJson::String>(TEXT("element_type")));
		TestTrue(TEXT("Modeling get_mesh_selection_info contains topology_mode"), (*GetMeshSelectionInfoData)->HasTypedField<EJson::String>(TEXT("topology_mode")));
		TestTrue(TEXT("Modeling get_mesh_selection_info contains target_count"), (*GetMeshSelectionInfoData)->HasTypedField<EJson::Number>(TEXT("target_count")));
		TestTrue(TEXT("Modeling get_mesh_selection_info contains is_empty"), (*GetMeshSelectionInfoData)->HasTypedField<EJson::Boolean>(TEXT("is_empty")));
		TestTrue(TEXT("Modeling get_mesh_selection_info contains has_active_targets"), (*GetMeshSelectionInfoData)->HasTypedField<EJson::Boolean>(TEXT("has_active_targets")));
	}

	const FString GetActiveToolBody = MakeExecRequestBody(TEXT("smoke-modeling-active-tool-001"), TEXT("modeling_get_active_tool"));
	FHttpSmokeResult GetActiveToolResult;
	TestTrue(TEXT("Modeling get_active_tool request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetActiveToolBody, GetActiveToolResult));
	TestEqual(TEXT("Modeling get_active_tool status code"), GetActiveToolResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetActiveToolJson;
	TestTrue(TEXT("Modeling get_active_tool response parses as JSON"), ParseJson(GetActiveToolResult.Body, GetActiveToolJson));
	TestTrue(TEXT("Modeling get_active_tool root ok=true"), GetActiveToolJson.IsValid() && GetActiveToolJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* GetActiveToolData = nullptr;
	TestTrue(TEXT("Modeling get_active_tool contains data object"), GetActiveToolJson.IsValid() && GetActiveToolJson->TryGetObjectField(TEXT("data"), GetActiveToolData) && GetActiveToolData && GetActiveToolData->IsValid());
	if (GetActiveToolData && GetActiveToolData->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* PropertySets = nullptr;
		TestTrue(TEXT("Modeling get_active_tool contains has_active_tool"), (*GetActiveToolData)->HasTypedField<EJson::Boolean>(TEXT("has_active_tool")));
		TestTrue(TEXT("Modeling get_active_tool contains active_tool_name"), (*GetActiveToolData)->HasTypedField<EJson::String>(TEXT("active_tool_name")));
		TestTrue(TEXT("Modeling get_active_tool contains can_accept"), (*GetActiveToolData)->HasTypedField<EJson::Boolean>(TEXT("can_accept")));
		TestTrue(TEXT("Modeling get_active_tool contains can_cancel"), (*GetActiveToolData)->HasTypedField<EJson::Boolean>(TEXT("can_cancel")));
		TestTrue(TEXT("Modeling get_active_tool contains tool_has_accept"), (*GetActiveToolData)->HasTypedField<EJson::Boolean>(TEXT("tool_has_accept")));
		TestTrue(TEXT("Modeling get_active_tool contains property_sets array"), (*GetActiveToolData)->TryGetArrayField(TEXT("property_sets"), PropertySets) && PropertySets != nullptr);
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceModelingCreateByBoundsSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.ModelingCreateByBoundsWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceModelingCreateByBoundsSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FVector ExpectedCenter(1234.0f, -567.0f, 210.0f);
	const FVector ExpectedExtent(150.0f, 80.0f, 45.0f);
	const FRotator ExpectedRotation(0.0f, 35.0f, 0.0f);
	const FString FolderPath = FString::Printf(TEXT("Smoke/ModelingBounds/%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));

	auto VerifyCreatedPrimitiveByBounds =
		[this, &ServerScope, &FolderPath](
			const FString& RequestId,
			const FString& Command,
			const FVector& ExpectedCenterLocal,
			const FVector& ExpectedExtentLocal,
			const FRotator& ExpectedRotationLocal) -> bool
	{
		const FString CreateBodyLocal = MakeExecRequestBody(
			RequestId,
			Command,
			FString::Printf(
				TEXT("{\"bounds\":{\"center\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"extent\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}},\"rotation\":{\"pitch\":%.3f,\"yaw\":%.3f,\"roll\":%.3f},\"folder_path\":\"%s\"}"),
				ExpectedCenterLocal.X,
				ExpectedCenterLocal.Y,
				ExpectedCenterLocal.Z,
				ExpectedExtentLocal.X,
				ExpectedExtentLocal.Y,
				ExpectedExtentLocal.Z,
				ExpectedRotationLocal.Pitch,
				ExpectedRotationLocal.Yaw,
				ExpectedRotationLocal.Roll,
				*FolderPath));

		FHttpSmokeResult CreateResultLocal;
		TestTrue(FString::Printf(TEXT("%s request succeeded"), *Command), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateBodyLocal, CreateResultLocal));
		TestEqual(FString::Printf(TEXT("%s status code"), *Command), CreateResultLocal.StatusCode, 200);

		TSharedPtr<FJsonObject> CreateJsonLocal;
		TestTrue(FString::Printf(TEXT("%s response parses as JSON"), *Command), ParseJson(CreateResultLocal.Body, CreateJsonLocal));
		TestTrue(FString::Printf(TEXT("%s root ok=true"), *Command), CreateJsonLocal.IsValid() && CreateJsonLocal->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* CreateDataLocal = nullptr;
		TestTrue(FString::Printf(TEXT("%s contains data object"), *Command), CreateJsonLocal.IsValid() && CreateJsonLocal->TryGetObjectField(TEXT("data"), CreateDataLocal) && CreateDataLocal && CreateDataLocal->IsValid());
		if (!(CreateDataLocal && CreateDataLocal->IsValid()))
		{
			return false;
		}

		FString CreatedActorNameLocal;
		const TSharedPtr<FJsonObject>* CreatedActorLocal = nullptr;
		TestTrue(FString::Printf(TEXT("%s reports used_bounds_placement"), *Command), (*CreateDataLocal)->GetBoolField(TEXT("used_bounds_placement")));
		TestTrue(FString::Printf(TEXT("%s contains created_actor"), *Command), (*CreateDataLocal)->TryGetObjectField(TEXT("created_actor"), CreatedActorLocal) && CreatedActorLocal && CreatedActorLocal->IsValid());
		if (CreatedActorLocal && CreatedActorLocal->IsValid())
		{
			CreatedActorNameLocal = (*CreatedActorLocal)->GetStringField(TEXT("name"));
			TestFalse(FString::Printf(TEXT("%s created_actor name not empty"), *Command), CreatedActorNameLocal.IsEmpty());
		}

		const TSharedPtr<FJsonObject>* DerivedPrimitiveLocal = nullptr;
		TestTrue(FString::Printf(TEXT("%s contains derived_primitive"), *Command), (*CreateDataLocal)->TryGetObjectField(TEXT("derived_primitive"), DerivedPrimitiveLocal) && DerivedPrimitiveLocal && DerivedPrimitiveLocal->IsValid());
		if (DerivedPrimitiveLocal && DerivedPrimitiveLocal->IsValid())
		{
			TestEqual(FString::Printf(TEXT("%s derived depth"), *Command), (*DerivedPrimitiveLocal)->GetNumberField(TEXT("Depth")), static_cast<double>(ExpectedExtentLocal.X * 2.0f));
			TestEqual(FString::Printf(TEXT("%s derived width"), *Command), (*DerivedPrimitiveLocal)->GetNumberField(TEXT("Width")), static_cast<double>(ExpectedExtentLocal.Y * 2.0f));
			TestEqual(FString::Printf(TEXT("%s derived height"), *Command), (*DerivedPrimitiveLocal)->GetNumberField(TEXT("Height")), static_cast<double>(ExpectedExtentLocal.Z * 2.0f));
		}

		if (CreatedActorNameLocal.IsEmpty())
		{
			return false;
		}

		const FString GetTransformBodyLocal = MakeExecRequestBody(
			RequestId + TEXT("-transform"),
			TEXT("level_get_actor_transform"),
			FString::Printf(TEXT("{\"id\":\"%s\"}"), *CreatedActorNameLocal));
		FHttpSmokeResult GetTransformResultLocal;
		TestTrue(FString::Printf(TEXT("%s get_transform request succeeded"), *Command), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetTransformBodyLocal, GetTransformResultLocal));
		TestEqual(FString::Printf(TEXT("%s get_transform status code"), *Command), GetTransformResultLocal.StatusCode, 200);

		TSharedPtr<FJsonObject> GetTransformJsonLocal;
		TestTrue(FString::Printf(TEXT("%s get_transform parses as JSON"), *Command), ParseJson(GetTransformResultLocal.Body, GetTransformJsonLocal));
		TestTrue(FString::Printf(TEXT("%s get_transform root ok=true"), *Command), GetTransformJsonLocal.IsValid() && GetTransformJsonLocal->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* TransformDataLocal = nullptr;
		TestTrue(FString::Printf(TEXT("%s get_transform contains data object"), *Command), GetTransformJsonLocal.IsValid() && GetTransformJsonLocal->TryGetObjectField(TEXT("data"), TransformDataLocal) && TransformDataLocal && TransformDataLocal->IsValid());
		if (TransformDataLocal && TransformDataLocal->IsValid())
		{
			const TSharedPtr<FJsonObject>* LocationLocal = nullptr;
			const TSharedPtr<FJsonObject>* RotationLocal = nullptr;
			TestTrue(FString::Printf(TEXT("%s get_transform contains location"), *Command), (*TransformDataLocal)->TryGetObjectField(TEXT("location"), LocationLocal) && LocationLocal && LocationLocal->IsValid());
			TestTrue(FString::Printf(TEXT("%s get_transform contains rotation"), *Command), (*TransformDataLocal)->TryGetObjectField(TEXT("rotation"), RotationLocal) && RotationLocal && RotationLocal->IsValid());
			if (LocationLocal && LocationLocal->IsValid())
			{
				TestTrue(FString::Printf(TEXT("%s location x matches"), *Command), FMath::IsNearlyEqual((float)(*LocationLocal)->GetNumberField(TEXT("x")), ExpectedCenterLocal.X, 0.5f));
				TestTrue(FString::Printf(TEXT("%s location y matches"), *Command), FMath::IsNearlyEqual((float)(*LocationLocal)->GetNumberField(TEXT("y")), ExpectedCenterLocal.Y, 0.5f));
				TestTrue(FString::Printf(TEXT("%s location z matches"), *Command), FMath::IsNearlyEqual((float)(*LocationLocal)->GetNumberField(TEXT("z")), ExpectedCenterLocal.Z, 0.5f));
			}
			if (RotationLocal && RotationLocal->IsValid())
			{
				TestTrue(FString::Printf(TEXT("%s rotation pitch matches"), *Command), FMath::IsNearlyEqual((float)(*RotationLocal)->GetNumberField(TEXT("pitch")), ExpectedRotationLocal.Pitch, 0.5f));
				TestTrue(FString::Printf(TEXT("%s rotation yaw matches"), *Command), FMath::IsNearlyEqual((float)(*RotationLocal)->GetNumberField(TEXT("yaw")), ExpectedRotationLocal.Yaw, 0.5f));
				TestTrue(FString::Printf(TEXT("%s rotation roll matches"), *Command), FMath::IsNearlyEqual((float)(*RotationLocal)->GetNumberField(TEXT("roll")), ExpectedRotationLocal.Roll, 0.5f));
			}
		}

		return !HasAnyErrors();
	};
	VerifyCreatedPrimitiveByBounds(TEXT("smoke-modeling-create-box-bounds-001"), TEXT("modeling_create_box"), ExpectedCenter, ExpectedExtent, ExpectedRotation);
	VerifyCreatedPrimitiveByBounds(TEXT("smoke-modeling-create-ramp-bounds-001"), TEXT("modeling_create_ramp"), ExpectedCenter + FVector(400.0f, 0.0f, 0.0f), FVector(120.0f, 90.0f, 60.0f), FRotator(0.0f, 90.0f, 0.0f));
	VerifyCreatedPrimitiveByBounds(TEXT("smoke-modeling-create-ramp-corner-bounds-001"), TEXT("modeling_create_ramp_corner"), ExpectedCenter + FVector(-400.0f, 0.0f, 0.0f), FVector(100.0f, 100.0f, 80.0f), FRotator(0.0f, -25.0f, 0.0f));

	const FString DestroyBody = MakeExecRequestBody(
		TEXT("smoke-modeling-create-bounds-destroy-001"),
		TEXT("level_destroy_folder_actors"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\"}"), *FolderPath));
	FHttpSmokeResult DestroyResult;
	TestTrue(TEXT("Modeling bounds workflow destroy request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DestroyBody, DestroyResult));
	TestEqual(TEXT("Modeling bounds workflow destroy status code"), DestroyResult.StatusCode, 200);

	TSharedPtr<FJsonObject> DestroyJson;
	TestTrue(TEXT("Modeling bounds workflow destroy parses as JSON"), ParseJson(DestroyResult.Body, DestroyJson));
	TestTrue(TEXT("Modeling bounds workflow destroy root ok=true"), DestroyJson.IsValid() && DestroyJson->GetBoolField(TEXT("ok")));

	return !HasAnyErrors();
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
	FUeAgentInterfaceSequenceSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.SequenceWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceSequenceSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString RootPath = TEXT("/Game/__UeAgentInterfaceSmoke");
	const FString SequenceAssetPath = MakeAutomationAssetPath(TEXT("SEQ"));

	const FString CreateBody = MakeExecRequestBody(
		TEXT("smoke-sequence-create-001"),
		TEXT("sequence_create_level_sequence"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"start_seconds\":0.5,\"duration_seconds\":3.0,\"display_rate_num\":24,\"display_rate_den\":1,\"open_editor\":false,\"save_after_create\":false}"),
			*SequenceAssetPath));
	FHttpSmokeResult CreateResult;
	TestTrue(TEXT("Sequence create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateBody, CreateResult));
	TestEqual(TEXT("Sequence create status code"), CreateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CreateJson;
	TestTrue(TEXT("Sequence create response parses as JSON"), ParseJson(CreateResult.Body, CreateJson));
	TestTrue(TEXT("Sequence create root ok=true"), CreateJson.IsValid() && CreateJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* CreateData = nullptr;
	TestTrue(TEXT("Sequence create contains data object"), CreateJson.IsValid() && CreateJson->TryGetObjectField(TEXT("data"), CreateData) && CreateData && CreateData->IsValid());
	if (CreateData && CreateData->IsValid())
	{
		TestEqual(TEXT("Sequence create asset path"), (*CreateData)->GetStringField(TEXT("asset_path")), SequenceAssetPath);
		TestEqual(TEXT("Sequence create display rate"), (*CreateData)->GetStringField(TEXT("display_rate")), FString(TEXT("24/1")));
	}

	const FString ListBody = MakeExecRequestBody(
		TEXT("smoke-sequence-list-001"),
		TEXT("sequence_list_level_sequences"),
		FString::Printf(TEXT("{\"root_path\":\"%s\",\"limit\":32}"), *RootPath));
	FHttpSmokeResult ListResult;
	TestTrue(TEXT("Sequence list request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListBody, ListResult));
	TestEqual(TEXT("Sequence list status code"), ListResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ListJson;
	TestTrue(TEXT("Sequence list response parses as JSON"), ParseJson(ListResult.Body, ListJson));
	TestTrue(TEXT("Sequence list root ok=true"), ListJson.IsValid() && ListJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* ListData = nullptr;
	TestTrue(TEXT("Sequence list contains data object"), ListJson.IsValid() && ListJson->TryGetObjectField(TEXT("data"), ListData) && ListData && ListData->IsValid());
	if (ListData && ListData->IsValid())
	{
		TestEqual(TEXT("Sequence list root path"), (*ListData)->GetStringField(TEXT("root_path")), RootPath);
		TestTrue(TEXT("Sequence list returned_count >= 1"), static_cast<int32>((*ListData)->GetIntegerField(TEXT("returned_count"))) >= 1);
	}

	const FString GetInfoBody = MakeExecRequestBody(
		TEXT("smoke-sequence-info-001"),
		TEXT("sequence_get_level_sequence_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SequenceAssetPath));
	FHttpSmokeResult GetInfoResult;
	TestTrue(TEXT("Sequence get_info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoBody, GetInfoResult));
	TestEqual(TEXT("Sequence get_info status code"), GetInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoJson;
	TestTrue(TEXT("Sequence get_info response parses as JSON"), ParseJson(GetInfoResult.Body, GetInfoJson));
	TestTrue(TEXT("Sequence get_info root ok=true"), GetInfoJson.IsValid() && GetInfoJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* GetInfoData = nullptr;
	TestTrue(TEXT("Sequence get_info contains data object"), GetInfoJson.IsValid() && GetInfoJson->TryGetObjectField(TEXT("data"), GetInfoData) && GetInfoData && GetInfoData->IsValid());
	if (GetInfoData && GetInfoData->IsValid())
	{
		TestTrue(TEXT("Sequence get_info has display rate"), !(*GetInfoData)->GetStringField(TEXT("display_rate")).IsEmpty());
		TestEqual(TEXT("Sequence get_info binding count is zero"), static_cast<int32>((*GetInfoData)->GetIntegerField(TEXT("binding_count"))), 0);
	}

	const FString SetRangeBody = MakeExecRequestBody(
		TEXT("smoke-sequence-range-001"),
		TEXT("sequence_set_level_sequence_playback_range"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"start_seconds\":1.25,\"duration_seconds\":2.5,\"save_after_set\":false}"), *SequenceAssetPath));
	FHttpSmokeResult SetRangeResult;
	TestTrue(TEXT("Sequence set playback range request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetRangeBody, SetRangeResult));
	TestEqual(TEXT("Sequence set playback range status code"), SetRangeResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SetRangeJson;
	TestTrue(TEXT("Sequence set playback range response parses as JSON"), ParseJson(SetRangeResult.Body, SetRangeJson));
	TestTrue(TEXT("Sequence set playback range root ok=true"), SetRangeJson.IsValid() && SetRangeJson->GetBoolField(TEXT("ok")));

	const FString SetDisplayRateBody = MakeExecRequestBody(
		TEXT("smoke-sequence-rate-001"),
		TEXT("sequence_set_level_sequence_display_rate"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"display_rate_num\":12,\"display_rate_den\":1,\"save_after_set\":false}"), *SequenceAssetPath));
	FHttpSmokeResult SetDisplayRateResult;
	TestTrue(TEXT("Sequence set display rate request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetDisplayRateBody, SetDisplayRateResult));
	TestEqual(TEXT("Sequence set display rate status code"), SetDisplayRateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SetDisplayRateJson;
	TestTrue(TEXT("Sequence set display rate response parses as JSON"), ParseJson(SetDisplayRateResult.Body, SetDisplayRateJson));
	TestTrue(TEXT("Sequence set display rate root ok=true"), SetDisplayRateJson.IsValid() && SetDisplayRateJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* SetDisplayRateData = nullptr;
	TestTrue(TEXT("Sequence set display rate contains data object"), SetDisplayRateJson.IsValid() && SetDisplayRateJson->TryGetObjectField(TEXT("data"), SetDisplayRateData) && SetDisplayRateData && SetDisplayRateData->IsValid());
	if (SetDisplayRateData && SetDisplayRateData->IsValid())
	{
		TestEqual(TEXT("Sequence set display rate response"), (*SetDisplayRateData)->GetStringField(TEXT("display_rate")), FString(TEXT("12/1")));
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceSequenceFolderSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.SequenceFolderWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceSequenceFolderSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString SequenceAssetPath = MakeAutomationAssetPath(TEXT("SEQFOLDER"));
	const FString ActorLabel = FString::Printf(TEXT("SmokeSeqActor_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString CameraActorLabel = FString::Printf(TEXT("SmokeSeqCamera_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString ChildSequenceAssetPath = MakeAutomationAssetPath(TEXT("SEQCHILD"));
	const FString FolderRoot = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("LevelSequence")));
	const FString FolderPath = FPaths::Combine(FolderRoot, SequenceAssetPath.RightChop(1));

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

	const FString SpawnActorBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-spawn-001"),
		TEXT("spawn_actor"),
		FString::Printf(TEXT("{\"class_path\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":0,\"y\":0,\"z\":120}}"), *ActorLabel));
	FHttpSmokeResult SpawnActorResult;
	TestTrue(TEXT("Sequence folder spawn actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnActorBody, SpawnActorResult));
	TestEqual(TEXT("Sequence folder spawn actor status code"), SpawnActorResult.StatusCode, 200);

	const FString CreateSequenceBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-create-001"),
		TEXT("sequence_create_level_sequence"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"start_seconds\":0.0,\"duration_seconds\":3.0,\"display_rate_num\":24,\"display_rate_den\":1,\"open_editor\":false,\"save_after_create\":false}"), *SequenceAssetPath));
	FHttpSmokeResult CreateSequenceResult;
	TestTrue(TEXT("Sequence folder create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateSequenceBody, CreateSequenceResult));
	TestEqual(TEXT("Sequence folder create status code"), CreateSequenceResult.StatusCode, 200);

	const FString CreateChildSequenceBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-create-child-001"),
		TEXT("sequence_create_level_sequence"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"start_seconds\":0.0,\"duration_seconds\":1.0,\"display_rate_num\":24,\"display_rate_den\":1,\"open_editor\":false,\"save_after_create\":false}"), *ChildSequenceAssetPath));
	FHttpSmokeResult CreateChildSequenceResult;
	TestTrue(TEXT("Sequence folder create child request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateChildSequenceBody, CreateChildSequenceResult));
	TestEqual(TEXT("Sequence folder create child status code"), CreateChildSequenceResult.StatusCode, 200);

	UWorld* DirectEditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	TestNotNull(TEXT("Sequence folder direct editor world available for camera"), DirectEditorWorld);
	ACameraActor* DirectCameraActor = nullptr;
	if (DirectEditorWorld)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.ObjectFlags = RF_Transient;
		SpawnParams.bHideFromSceneOutliner = false;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		DirectCameraActor = DirectEditorWorld->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), FVector(300.0, 0.0, 120.0), FRotator(-10.0, 180.0, 0.0), SpawnParams);
		if (DirectCameraActor)
		{
			DirectCameraActor->SetActorLabel(CameraActorLabel);
		}
	}
	TestNotNull(TEXT("Sequence folder direct camera actor"), DirectCameraActor);

	const FString AddBindingBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-bind-001"),
		TEXT("sequence_add_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *ActorLabel));
	FHttpSmokeResult AddBindingResult;
	TestTrue(TEXT("Sequence folder add binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddBindingBody, AddBindingResult));
	TestEqual(TEXT("Sequence folder add binding status code"), AddBindingResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddBindingJson;
	TestTrue(TEXT("Sequence folder add binding parses JSON"), ParseJson(AddBindingResult.Body, AddBindingJson));
	const TSharedPtr<FJsonObject>* AddBindingData = nullptr;
	TestTrue(TEXT("Sequence folder add binding contains data"), AddBindingJson.IsValid() && AddBindingJson->TryGetObjectField(TEXT("data"), AddBindingData) && AddBindingData && AddBindingData->IsValid());
	const FString BindingGuid = (AddBindingData && AddBindingData->IsValid()) ? (*AddBindingData)->GetStringField(TEXT("binding_guid")) : FString();
	TestTrue(TEXT("Sequence folder binding guid non-empty"), !BindingGuid.IsEmpty());

	const FString AddCameraBindingBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-bind-camera-001"),
		TEXT("sequence_add_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *CameraActorLabel));
	FHttpSmokeResult AddCameraBindingResult;
	TestTrue(TEXT("Sequence folder add camera binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddCameraBindingBody, AddCameraBindingResult));
	TestEqual(TEXT("Sequence folder add camera binding status code"), AddCameraBindingResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddCameraBindingJson;
	TestTrue(TEXT("Sequence folder add camera binding parses JSON"), ParseJson(AddCameraBindingResult.Body, AddCameraBindingJson));
	const TSharedPtr<FJsonObject>* AddCameraBindingData = nullptr;
	TestTrue(TEXT("Sequence folder add camera binding contains data"), AddCameraBindingJson.IsValid() && AddCameraBindingJson->TryGetObjectField(TEXT("data"), AddCameraBindingData) && AddCameraBindingData && AddCameraBindingData->IsValid());
	const FString CameraBindingGuid = (AddCameraBindingData && AddCameraBindingData->IsValid()) ? (*AddCameraBindingData)->GetStringField(TEXT("binding_guid")) : FString();
	TestTrue(TEXT("Sequence folder camera binding guid non-empty"), !CameraBindingGuid.IsEmpty());

	const FString AddTransformTrackBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-transform-track-001"),
		TEXT("sequence_add_transform_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddTransformTrackResult;
	TestTrue(TEXT("Sequence folder add transform track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddTransformTrackBody, AddTransformTrackResult));
	TestEqual(TEXT("Sequence folder add transform track status code"), AddTransformTrackResult.StatusCode, 200);

	const FString AddTransformKeyBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-transform-key-001"),
		TEXT("sequence_add_transform_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"time_seconds\":1.0,\"location\":{\"x\":10,\"y\":20,\"z\":30},\"rotation\":{\"pitch\":0,\"yaw\":45,\"roll\":0},\"scale\":{\"x\":1.0,\"y\":1.0,\"z\":1.5},\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddTransformKeyResult;
	TestTrue(TEXT("Sequence folder add transform key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddTransformKeyBody, AddTransformKeyResult));
	TestEqual(TEXT("Sequence folder add transform key status code"), AddTransformKeyResult.StatusCode, 200);

	const FString AddFloatTrackBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-float-track-001"),
		TEXT("sequence_add_property_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"float\",\"property_name\":\"CustomTimeDilation\",\"property_path\":\"CustomTimeDilation\",\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddFloatTrackResult;
	TestTrue(TEXT("Sequence folder add float track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddFloatTrackBody, AddFloatTrackResult));
	TestEqual(TEXT("Sequence folder add float track status code"), AddFloatTrackResult.StatusCode, 200);

	const FString AddFloatKeyBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-float-key-001"),
		TEXT("sequence_add_property_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"float\",\"property_name\":\"CustomTimeDilation\",\"property_path\":\"CustomTimeDilation\",\"time_seconds\":1.0,\"value\":1.5,\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddFloatKeyResult;
	TestTrue(TEXT("Sequence folder add float key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddFloatKeyBody, AddFloatKeyResult));
	TestEqual(TEXT("Sequence folder add float key status code"), AddFloatKeyResult.StatusCode, 200);

	const FString SequenceObjectPath = FString::Printf(TEXT("%s.%s"), *SequenceAssetPath, *FPackageName::GetLongPackageAssetName(SequenceAssetPath));
	ULevelSequence* SequenceAsset = LoadObject<ULevelSequence>(nullptr, *SequenceObjectPath);
	TestNotNull(TEXT("Sequence folder direct load sequence asset"), SequenceAsset);
	UMovieScene* MovieScene = SequenceAsset ? SequenceAsset->GetMovieScene() : nullptr;
	TestNotNull(TEXT("Sequence folder direct movie scene"), MovieScene);
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	TestNotNull(TEXT("Sequence folder direct editor world"), EditorWorld);
	auto FindActorById = [](UWorld* World, const FString& ActorId) -> AActor*
	{
		if (!World || ActorId.IsEmpty())
		{
			return nullptr;
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor && (Actor->GetName().Equals(ActorId, ESearchCase::IgnoreCase) || Actor->GetActorLabel().Equals(ActorId, ESearchCase::IgnoreCase)))
			{
				return Actor;
			}
		}
		return nullptr;
	};
	AActor* BoundActor = FindActorById(EditorWorld, ActorLabel);
	TestNotNull(TEXT("Sequence folder direct bound actor"), BoundActor);
	UStaticMeshComponent* BoundMeshComponent = BoundActor ? BoundActor->FindComponentByClass<UStaticMeshComponent>() : nullptr;
	TestNotNull(TEXT("Sequence folder direct bound mesh component"), BoundMeshComponent);

	FGuid ActorBindingGuidValue;
	TestTrue(TEXT("Sequence folder parse actor binding guid"), FGuid::Parse(BindingGuid, ActorBindingGuidValue));

	FGuid ComponentBindingGuidValue;
	if (SequenceAsset && MovieScene && BoundMeshComponent)
	{
		ComponentBindingGuidValue = MovieScene->AddPossessable(BoundMeshComponent->GetName(), BoundMeshComponent->GetClass());
		if (FMovieScenePossessable* ComponentPossessable = MovieScene->FindPossessable(ComponentBindingGuidValue))
		{
			ComponentPossessable->SetParent(ActorBindingGuidValue, MovieScene);
		}
		SequenceAsset->BindPossessableObject(ComponentBindingGuidValue, *BoundMeshComponent, BoundActor);
		SequenceAsset->MarkPackageDirty();
	}
	TestTrue(TEXT("Sequence folder component binding guid valid"), ComponentBindingGuidValue.IsValid());
	const FString ComponentBindingGuid = ComponentBindingGuidValue.ToString(EGuidFormats::DigitsWithHyphensLower);

	const FString AddComponentFloatTrackBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-component-float-track-001"),
		TEXT("sequence_add_property_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"float\",\"property_name\":\"BoundsScale\",\"property_path\":\"BoundsScale\",\"save_after_set\":false}"), *SequenceAssetPath, *ComponentBindingGuid));
	FHttpSmokeResult AddComponentFloatTrackResult;
	TestTrue(TEXT("Sequence folder add component float track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddComponentFloatTrackBody, AddComponentFloatTrackResult));
	TestEqual(TEXT("Sequence folder add component float track status code"), AddComponentFloatTrackResult.StatusCode, 200);

	const FString AddComponentFloatKeyBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-component-float-key-001"),
		TEXT("sequence_add_property_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"float\",\"property_name\":\"BoundsScale\",\"property_path\":\"BoundsScale\",\"time_seconds\":1.0,\"value\":1.25,\"save_after_set\":false}"), *SequenceAssetPath, *ComponentBindingGuid));
	FHttpSmokeResult AddComponentFloatKeyResult;
	TestTrue(TEXT("Sequence folder add component float key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddComponentFloatKeyBody, AddComponentFloatKeyResult));
	TestEqual(TEXT("Sequence folder add component float key status code"), AddComponentFloatKeyResult.StatusCode, 200);

	FGuid SpawnableBindingGuidValue;
	if (SequenceAsset && EditorWorld)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.ObjectFlags = RF_Transient;
		SpawnParams.bHideFromSceneOutliner = true;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ACameraActor* TempSpawnableActor = EditorWorld->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), FTransform::Identity, SpawnParams);
		TestNotNull(TEXT("Sequence folder temp spawnable actor"), TempSpawnableActor);
		if (TempSpawnableActor)
		{
			UObject* SpawnableTemplate = MovieSceneHelpers::MakeSpawnableTemplateFromInstance(*TempSpawnableActor, MovieScene, TempSpawnableActor->GetFName());
			TestNotNull(TEXT("Sequence folder spawnable template"), SpawnableTemplate);
			if (SpawnableTemplate)
			{
				SpawnableBindingGuidValue = MovieScene->AddSpawnable(TEXT("SmokeCameraSpawnable"), *SpawnableTemplate);
				if (UMovieSceneSpawnTrack* SpawnTrack = MovieScene->AddTrack<UMovieSceneSpawnTrack>(SpawnableBindingGuidValue))
				{
					SpawnTrack->SetObjectId(SpawnableBindingGuidValue);
					if (SpawnTrack->GetAllSections().Num() == 0)
					{
						if (UMovieSceneSection* NewSection = SpawnTrack->CreateNewSection())
						{
							NewSection->SetRange(TRange<FFrameNumber>::All());
							SpawnTrack->AddSection(*NewSection);
						}
					}
				}
			}
			TempSpawnableActor->Destroy();
		}
	}
	TestTrue(TEXT("Sequence folder spawnable binding guid valid"), SpawnableBindingGuidValue.IsValid());
	const FString SpawnableBindingGuid = SpawnableBindingGuidValue.ToString(EGuidFormats::DigitsWithHyphensLower);

	const FString AddSpawnableTransformTrackBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-spawnable-transform-track-001"),
		TEXT("sequence_add_transform_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *SpawnableBindingGuid));
	FHttpSmokeResult AddSpawnableTransformTrackResult;
	TestTrue(TEXT("Sequence folder add spawnable transform track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddSpawnableTransformTrackBody, AddSpawnableTransformTrackResult));
	TestEqual(TEXT("Sequence folder add spawnable transform track status code"), AddSpawnableTransformTrackResult.StatusCode, 200);

	const FString AddSpawnableTransformKeyBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-spawnable-transform-key-001"),
		TEXT("sequence_add_transform_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"time_seconds\":0.5,\"location\":{\"x\":40,\"y\":5,\"z\":100},\"rotation\":{\"pitch\":0,\"yaw\":15,\"roll\":0},\"scale\":{\"x\":1.0,\"y\":1.0,\"z\":1.0},\"save_after_set\":false}"), *SequenceAssetPath, *SpawnableBindingGuid));
	FHttpSmokeResult AddSpawnableTransformKeyResult;
	TestTrue(TEXT("Sequence folder add spawnable transform key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddSpawnableTransformKeyBody, AddSpawnableTransformKeyResult));
	TestEqual(TEXT("Sequence folder add spawnable transform key status code"), AddSpawnableTransformKeyResult.StatusCode, 200);

	if (MovieScene)
	{
		if (UMovieSceneSpawnTrack* SpawnTrack = MovieScene->FindTrack<UMovieSceneSpawnTrack>(SpawnableBindingGuidValue))
		{
			UMovieSceneSpawnSection* SpawnSection = SpawnTrack->GetAllSections().Num() > 0 ? Cast<UMovieSceneSpawnSection>(SpawnTrack->GetAllSections()[0]) : nullptr;
			if (SpawnSection)
			{
				FMovieSceneBoolChannel& SpawnChannel = SpawnSection->GetChannel();
				SpawnChannel.GetData().Reset();
				SpawnChannel.GetData().UpdateOrAddKey(MovieScene->GetTickResolution().AsFrameTime(0.0).RoundToFrame(), true);
				SpawnChannel.GetData().UpdateOrAddKey(MovieScene->GetTickResolution().AsFrameTime(2.0).RoundToFrame(), false);
			}
		}
	}

	const FString ExportFolderBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-export-001"),
		TEXT("sequence_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clean_output_dir\":true,\"include_validation\":true}"), *SequenceAssetPath));
	FHttpSmokeResult ExportFolderResult;
	TestTrue(TEXT("Sequence folder export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ExportFolderBody, ExportFolderResult));
	TestEqual(TEXT("Sequence folder export status code"), ExportFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SettingsJson;
	TestTrue(TEXT("Sequence folder settings load"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("settings"), TEXT("sequence.json")), SettingsJson));
	if (SettingsJson.IsValid())
	{
		const TSharedPtr<FJsonObject>* PlaybackObj = nullptr;
		TestTrue(TEXT("Sequence folder settings playback exists"), SettingsJson->TryGetObjectField(TEXT("playback"), PlaybackObj) && PlaybackObj && (*PlaybackObj).IsValid());
		if (PlaybackObj && (*PlaybackObj).IsValid())
		{
			(*PlaybackObj)->SetNumberField(TEXT("start_seconds"), 0.25);
			(*PlaybackObj)->SetNumberField(TEXT("duration_seconds"), 4.0);
		}
		const TSharedPtr<FJsonObject>* DisplayRateObj = nullptr;
		TestTrue(TEXT("Sequence folder settings display rate exists"), SettingsJson->TryGetObjectField(TEXT("display_rate"), DisplayRateObj) && DisplayRateObj && (*DisplayRateObj).IsValid());
		if (DisplayRateObj && (*DisplayRateObj).IsValid())
		{
			(*DisplayRateObj)->SetNumberField(TEXT("numerator"), 12);
			(*DisplayRateObj)->SetNumberField(TEXT("denominator"), 1);
		}
	}
	TestTrue(TEXT("Sequence folder settings save"), SaveJsonFile(FPaths::Combine(FolderPath, TEXT("settings"), TEXT("sequence.json")), SettingsJson));

	TSharedPtr<FJsonObject> BindingsIndexJson;
	TestTrue(TEXT("Sequence folder bindings index load"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("bindings"), TEXT("index.json")), BindingsIndexJson));
	TMap<FString, FString> BindingFolderByGuid;
	if (BindingsIndexJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		TestTrue(TEXT("Sequence folder bindings index array exists"), BindingsIndexJson->TryGetArrayField(TEXT("bindings"), Bindings) && Bindings && Bindings->Num() >= 4);
		if (Bindings)
		{
			for (const TSharedPtr<FJsonValue>& BindingValue : *Bindings)
			{
				const TSharedPtr<FJsonObject>* BindingObj = nullptr;
				if (!BindingValue.IsValid() || !BindingValue->TryGetObject(BindingObj) || !BindingObj || !(*BindingObj).IsValid())
				{
					continue;
				}
				BindingFolderByGuid.Add((*BindingObj)->GetStringField(TEXT("binding_guid")), (*BindingObj)->GetStringField(TEXT("folder_name")));
			}
		}
	}
	const FString BindingFolderName = BindingFolderByGuid.FindRef(BindingGuid);
	const FString ComponentBindingFolderName = BindingFolderByGuid.FindRef(ComponentBindingGuid);
	const FString SpawnableBindingFolderName = BindingFolderByGuid.FindRef(SpawnableBindingGuid);
	TestTrue(TEXT("Sequence folder actor binding folder name non-empty"), !BindingFolderName.IsEmpty());
	TestTrue(TEXT("Sequence folder component binding folder name non-empty"), !ComponentBindingFolderName.IsEmpty());
	TestTrue(TEXT("Sequence folder spawnable binding folder name non-empty"), !SpawnableBindingFolderName.IsEmpty());

	auto ModifyTrackFiles = [this, &LoadJsonFile, &SaveJsonFile, &ComponentBindingGuid, &SpawnableBindingGuid](const FString& TracksDir, const FString& ExpectedBindingGuid, bool& bOutModifiedTransform, bool& bOutModifiedFloat, bool& bOutModifiedSpawn)
	{
		TArray<FString> TrackFiles;
		IFileManager::Get().FindFiles(TrackFiles, *(FPaths::Combine(TracksDir, TEXT("*.json"))), true, false);
		TrackFiles.Sort();
		for (const FString& TrackFile : TrackFiles)
		{
			const FString TrackPath = FPaths::Combine(TracksDir, TrackFile);
			TSharedPtr<FJsonObject> TrackJson;
			TestTrue(FString::Printf(TEXT("Sequence folder track load: %s"), *TrackFile), LoadJsonFile(TrackPath, TrackJson));
			if (!TrackJson.IsValid())
			{
				continue;
			}
			const FString TrackType = TrackJson->GetStringField(TEXT("track_type"));
			if (TrackType == TEXT("transform"))
			{
				const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
				if (TrackJson->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
				{
					const TSharedPtr<FJsonObject>* SectionObj = nullptr;
					if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
					{
						const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
						if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys && Keys->Num() == 1)
						{
							const TSharedPtr<FJsonObject>* KeyObj = nullptr;
							if ((*Keys)[0]->TryGetObject(KeyObj) && KeyObj && (*KeyObj).IsValid())
							{
								const TSharedPtr<FJsonObject>* LocationObj = nullptr;
								if ((*KeyObj)->TryGetObjectField(TEXT("location"), LocationObj) && LocationObj && (*LocationObj).IsValid())
								{
									(*LocationObj)->SetNumberField(TEXT("x"), ExpectedBindingGuid == SpawnableBindingGuid ? 55.0 : 110.0);
									bOutModifiedTransform = true;
								}
							}
						}
					}
				}
			}
			else if (TrackType == TEXT("property") && TrackJson->GetStringField(TEXT("property_type")) == TEXT("float"))
			{
				const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
				if (TrackJson->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
				{
					const TSharedPtr<FJsonObject>* SectionObj = nullptr;
					if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
					{
						const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
						if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys && Keys->Num() == 1)
						{
							const TSharedPtr<FJsonObject>* KeyObj = nullptr;
							if ((*Keys)[0]->TryGetObject(KeyObj) && KeyObj && (*KeyObj).IsValid())
							{
								(*KeyObj)->SetNumberField(TEXT("value"), ExpectedBindingGuid == ComponentBindingGuid ? 1.75 : 2.5);
								bOutModifiedFloat = true;
							}
						}
					}
				}
			}
			else if (TrackType == TEXT("spawn"))
			{
				const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
				if (TrackJson->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
				{
					const TSharedPtr<FJsonObject>* SectionObj = nullptr;
					if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
					{
						const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
						if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys && Keys->Num() >= 2)
						{
							const TSharedPtr<FJsonObject>* KeyObj = nullptr;
							if ((*Keys)[1]->TryGetObject(KeyObj) && KeyObj && (*KeyObj).IsValid())
							{
								(*KeyObj)->SetNumberField(TEXT("time_seconds"), 2.5);
								bOutModifiedSpawn = true;
							}
						}
					}
				}
			}
			TestTrue(FString::Printf(TEXT("Sequence folder track save: %s"), *TrackFile), SaveJsonFile(TrackPath, TrackJson));
		}
	};

	bool bModifiedTransform = false;
	bool bModifiedFloat = false;
	bool bModifiedComponentFloat = false;
	bool bModifiedSpawnTransform = false;
	bool bModifiedSpawnTrack = false;
	ModifyTrackFiles(FPaths::Combine(FolderPath, TEXT("bindings"), BindingFolderName, TEXT("tracks")), BindingGuid, bModifiedTransform, bModifiedFloat, bModifiedSpawnTrack);
	ModifyTrackFiles(FPaths::Combine(FolderPath, TEXT("bindings"), ComponentBindingFolderName, TEXT("tracks")), ComponentBindingGuid, bModifiedSpawnTransform, bModifiedComponentFloat, bModifiedSpawnTrack);
	ModifyTrackFiles(FPaths::Combine(FolderPath, TEXT("bindings"), SpawnableBindingFolderName, TEXT("tracks")), SpawnableBindingGuid, bModifiedSpawnTransform, bModifiedComponentFloat, bModifiedSpawnTrack);
	TestTrue(TEXT("Sequence folder modified actor transform track"), bModifiedTransform);
	TestTrue(TEXT("Sequence folder modified actor float track"), bModifiedFloat);
	TestTrue(TEXT("Sequence folder modified component float track"), bModifiedComponentFloat);
	TestTrue(TEXT("Sequence folder modified spawnable transform track"), bModifiedSpawnTransform);
	TestTrue(TEXT("Sequence folder modified spawn track"), bModifiedSpawnTrack);

	TSharedPtr<FJsonObject> MasterTracksJson;
	TestTrue(TEXT("Sequence folder master tracks load"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("master_tracks"), TEXT("index.json")), MasterTracksJson));
	if (MasterTracksJson.IsValid())
	{
		TArray<TSharedPtr<FJsonValue>> MasterTracks;

		TSharedPtr<FJsonObject> CameraCutTrackObj = MakeShared<FJsonObject>();
		CameraCutTrackObj->SetStringField(TEXT("track_type"), TEXT("camera_cut"));
		CameraCutTrackObj->SetBoolField(TEXT("can_blend"), false);
		TSharedPtr<FJsonObject> CameraCutSectionObj = MakeShared<FJsonObject>();
		CameraCutSectionObj->SetStringField(TEXT("section_type"), TEXT("camera_cut"));
		CameraCutSectionObj->SetStringField(TEXT("camera_binding_guid"), CameraBindingGuid);
		CameraCutSectionObj->SetNumberField(TEXT("start_seconds"), 0.0);
		CameraCutSectionObj->SetNumberField(TEXT("duration_seconds"), 2.0);
		CameraCutSectionObj->SetBoolField(TEXT("lock_previous_camera"), false);
		CameraCutTrackObj->SetArrayField(TEXT("sections"), { MakeShared<FJsonValueObject>(CameraCutSectionObj) });
		MasterTracks.Add(MakeShared<FJsonValueObject>(CameraCutTrackObj));

		TSharedPtr<FJsonObject> SubSequenceTrackObj = MakeShared<FJsonObject>();
		SubSequenceTrackObj->SetStringField(TEXT("track_type"), TEXT("sub_sequence"));
		TSharedPtr<FJsonObject> SubSequenceSectionObj = MakeShared<FJsonObject>();
		SubSequenceSectionObj->SetStringField(TEXT("section_type"), TEXT("sub_sequence"));
		SubSequenceSectionObj->SetStringField(TEXT("sequence_asset"), ChildSequenceAssetPath);
		SubSequenceSectionObj->SetNumberField(TEXT("start_seconds"), 2.0);
		SubSequenceSectionObj->SetNumberField(TEXT("duration_seconds"), 1.0);
		SubSequenceSectionObj->SetNumberField(TEXT("row_index"), 0);
		SubSequenceSectionObj->SetNumberField(TEXT("start_frame_offset"), 0);
		SubSequenceSectionObj->SetBoolField(TEXT("can_loop"), false);
		SubSequenceSectionObj->SetNumberField(TEXT("end_frame_offset"), 0);
		SubSequenceSectionObj->SetNumberField(TEXT("first_loop_start_frame_offset"), 0);
		SubSequenceSectionObj->SetNumberField(TEXT("hierarchical_bias"), 100);
		SubSequenceSectionObj->SetNumberField(TEXT("network_mask"), 0);
		SubSequenceSectionObj->SetStringField(TEXT("time_scale_type"), TEXT("fixed"));
		SubSequenceSectionObj->SetNumberField(TEXT("time_scale"), 1.0);
		SubSequenceTrackObj->SetArrayField(TEXT("sections"), { MakeShared<FJsonValueObject>(SubSequenceSectionObj) });
		MasterTracks.Add(MakeShared<FJsonValueObject>(SubSequenceTrackObj));

		MasterTracksJson->SetArrayField(TEXT("tracks"), MasterTracks);
	}
	TestTrue(TEXT("Sequence folder master tracks save"), SaveJsonFile(FPaths::Combine(FolderPath, TEXT("master_tracks"), TEXT("index.json")), MasterTracksJson));

	const FString RemoveBindingBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-remove-binding-001"),
		TEXT("sequence_remove_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult RemoveBindingResult;
	TestTrue(TEXT("Sequence folder remove binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveBindingBody, RemoveBindingResult));
	TestEqual(TEXT("Sequence folder remove binding status code"), RemoveBindingResult.StatusCode, 200);

	const FString RemoveComponentBindingBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-remove-binding-002"),
		TEXT("sequence_remove_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *ComponentBindingGuid));
	FHttpSmokeResult RemoveComponentBindingResult;
	TestTrue(TEXT("Sequence folder remove component binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveComponentBindingBody, RemoveComponentBindingResult));
	TestEqual(TEXT("Sequence folder remove component binding status code"), RemoveComponentBindingResult.StatusCode, 200);

	const FString RemoveSpawnableBindingBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-remove-binding-003"),
		TEXT("sequence_remove_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *SpawnableBindingGuid));
	FHttpSmokeResult RemoveSpawnableBindingResult;
	TestTrue(TEXT("Sequence folder remove spawnable binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveSpawnableBindingBody, RemoveSpawnableBindingResult));
	TestEqual(TEXT("Sequence folder remove spawnable binding status code"), RemoveSpawnableBindingResult.StatusCode, 200);

	const FString ApplyFolderBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-apply-001"),
		TEXT("sequence_apply_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"create_if_missing\":false,\"save_after_apply\":false}"), *SequenceAssetPath));
	FHttpSmokeResult ApplyFolderResult;
	TestTrue(TEXT("Sequence folder apply request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ApplyFolderBody, ApplyFolderResult));
	TestEqual(TEXT("Sequence folder apply status code"), ApplyFolderResult.StatusCode, 200);

	const FString ReExportBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-export-002"),
		TEXT("sequence_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clean_output_dir\":true,\"include_validation\":true}"), *SequenceAssetPath));
	FHttpSmokeResult ReExportResult;
	TestTrue(TEXT("Sequence folder re-export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ReExportBody, ReExportResult));
	TestEqual(TEXT("Sequence folder re-export status code"), ReExportResult.StatusCode, 200);

	TSharedPtr<FJsonObject> VerifySettingsJson;
	TestTrue(TEXT("Sequence folder verify settings load"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("settings"), TEXT("sequence.json")), VerifySettingsJson));
	if (VerifySettingsJson.IsValid())
	{
		const TSharedPtr<FJsonObject>* PlaybackObj = nullptr;
		TestTrue(TEXT("Sequence folder verify playback exists"), VerifySettingsJson->TryGetObjectField(TEXT("playback"), PlaybackObj) && PlaybackObj && (*PlaybackObj).IsValid());
		if (PlaybackObj && (*PlaybackObj).IsValid())
		{
			TestEqual(TEXT("Sequence folder playback start updated"), static_cast<float>((*PlaybackObj)->GetNumberField(TEXT("start_seconds"))), 0.25f);
			TestEqual(TEXT("Sequence folder playback duration updated"), static_cast<float>((*PlaybackObj)->GetNumberField(TEXT("duration_seconds"))), 4.0f);
		}
		const TSharedPtr<FJsonObject>* DisplayRateObj = nullptr;
		TestTrue(TEXT("Sequence folder verify display rate exists"), VerifySettingsJson->TryGetObjectField(TEXT("display_rate"), DisplayRateObj) && DisplayRateObj && (*DisplayRateObj).IsValid());
		if (DisplayRateObj && (*DisplayRateObj).IsValid())
		{
			TestEqual(TEXT("Sequence folder display rate updated"), static_cast<int32>((*DisplayRateObj)->GetIntegerField(TEXT("numerator"))), 12);
		}
	}

	TSharedPtr<FJsonObject> VerifyBindingsIndexJson;
	TestTrue(TEXT("Sequence folder verify bindings index load"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("bindings"), TEXT("index.json")), VerifyBindingsIndexJson));
	TMap<FString, FString> VerifyBindingFolderByGuid;
	if (VerifyBindingsIndexJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		TestTrue(TEXT("Sequence folder verify bindings index array exists"), VerifyBindingsIndexJson->TryGetArrayField(TEXT("bindings"), Bindings) && Bindings && Bindings->Num() >= 4);
		if (Bindings)
		{
			for (const TSharedPtr<FJsonValue>& BindingValue : *Bindings)
			{
				const TSharedPtr<FJsonObject>* BindingObj = nullptr;
				if (!BindingValue.IsValid() || !BindingValue->TryGetObject(BindingObj) || !BindingObj || !(*BindingObj).IsValid())
				{
					continue;
				}
				VerifyBindingFolderByGuid.Add((*BindingObj)->GetStringField(TEXT("binding_guid")), (*BindingObj)->GetStringField(TEXT("folder_name")));
			}
		}
	}
	const FString VerifyActorBindingDir = FPaths::Combine(FolderPath, TEXT("bindings"), VerifyBindingFolderByGuid.FindRef(BindingGuid), TEXT("tracks"));
	const FString VerifyComponentBindingDir = FPaths::Combine(FolderPath, TEXT("bindings"), VerifyBindingFolderByGuid.FindRef(ComponentBindingGuid), TEXT("tracks"));
	const FString VerifySpawnableBindingDir = FPaths::Combine(FolderPath, TEXT("bindings"), VerifyBindingFolderByGuid.FindRef(SpawnableBindingGuid), TEXT("tracks"));
	TestTrue(TEXT("Sequence folder verify actor binding dir exists"), !VerifyBindingFolderByGuid.FindRef(BindingGuid).IsEmpty());
	TestTrue(TEXT("Sequence folder verify component binding dir exists"), !VerifyBindingFolderByGuid.FindRef(ComponentBindingGuid).IsEmpty());
	TestTrue(TEXT("Sequence folder verify spawnable binding dir exists"), !VerifyBindingFolderByGuid.FindRef(SpawnableBindingGuid).IsEmpty());

	auto VerifyTrackFiles = [this, &LoadJsonFile, &ComponentBindingGuid, &SpawnableBindingGuid](const FString& TracksDir, const FString& ExpectedBindingGuid, bool& bOutTransform, bool& bOutFloat, bool& bOutSpawn)
	{
		TArray<FString> TrackFiles;
		IFileManager::Get().FindFiles(TrackFiles, *(FPaths::Combine(TracksDir, TEXT("*.json"))), true, false);
		TrackFiles.Sort();
		for (const FString& TrackFile : TrackFiles)
		{
			TSharedPtr<FJsonObject> TrackJson;
			TestTrue(FString::Printf(TEXT("Sequence folder verify track load: %s"), *TrackFile), LoadJsonFile(FPaths::Combine(TracksDir, TrackFile), TrackJson));
			if (!TrackJson.IsValid())
			{
				continue;
			}
			const FString TrackType = TrackJson->GetStringField(TEXT("track_type"));
			if (TrackType == TEXT("transform"))
			{
				const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
				if (TrackJson->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
				{
					const TSharedPtr<FJsonObject>* SectionObj = nullptr;
					if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
					{
						const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
						if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys && Keys->Num() == 1)
						{
							const TSharedPtr<FJsonObject>* KeyObj = nullptr;
							if ((*Keys)[0]->TryGetObject(KeyObj) && KeyObj && (*KeyObj).IsValid())
							{
								const TSharedPtr<FJsonObject>* LocationObj = nullptr;
								if ((*KeyObj)->TryGetObjectField(TEXT("location"), LocationObj) && LocationObj && (*LocationObj).IsValid())
								{
									const float ExpectedX = ExpectedBindingGuid == SpawnableBindingGuid ? 55.0f : 110.0f;
									bOutTransform = FMath::IsNearlyEqual(static_cast<float>((*LocationObj)->GetNumberField(TEXT("x"))), ExpectedX, 0.01f);
								}
							}
						}
					}
				}
			}
			else if (TrackType == TEXT("property") && TrackJson->GetStringField(TEXT("property_type")) == TEXT("float"))
			{
				const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
				if (TrackJson->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
				{
					const TSharedPtr<FJsonObject>* SectionObj = nullptr;
					if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
					{
						const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
						if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys && Keys->Num() == 1)
						{
							const TSharedPtr<FJsonObject>* KeyObj = nullptr;
							if ((*Keys)[0]->TryGetObject(KeyObj) && KeyObj && (*KeyObj).IsValid())
							{
								const float ExpectedValue = ExpectedBindingGuid == ComponentBindingGuid ? 1.75f : 2.5f;
								bOutFloat = FMath::IsNearlyEqual(static_cast<float>((*KeyObj)->GetNumberField(TEXT("value"))), ExpectedValue, 0.01f);
							}
						}
					}
				}
			}
			else if (TrackType == TEXT("spawn"))
			{
				const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
				if (TrackJson->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
				{
					const TSharedPtr<FJsonObject>* SectionObj = nullptr;
					if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
					{
						const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
						if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys && Keys->Num() >= 2)
						{
							const TSharedPtr<FJsonObject>* KeyObj = nullptr;
							if ((*Keys)[1]->TryGetObject(KeyObj) && KeyObj && (*KeyObj).IsValid())
							{
								bOutSpawn = FMath::IsNearlyEqual(static_cast<float>((*KeyObj)->GetNumberField(TEXT("time_seconds"))), 2.5f, 0.01f);
							}
						}
					}
				}
			}
		}
	};

	bool bVerifiedActorTransform = false;
	bool bVerifiedActorFloat = false;
	bool bVerifiedComponentFloat = false;
	bool bVerifiedSpawnableTransform = false;
	bool bVerifiedSpawnTrack = false;
	VerifyTrackFiles(VerifyActorBindingDir, BindingGuid, bVerifiedActorTransform, bVerifiedActorFloat, bVerifiedSpawnTrack);
	VerifyTrackFiles(VerifyComponentBindingDir, ComponentBindingGuid, bVerifiedSpawnableTransform, bVerifiedComponentFloat, bVerifiedSpawnTrack);
	VerifyTrackFiles(VerifySpawnableBindingDir, SpawnableBindingGuid, bVerifiedSpawnableTransform, bVerifiedComponentFloat, bVerifiedSpawnTrack);
	TestTrue(TEXT("Sequence folder actor transform key updated"), bVerifiedActorTransform);
	TestTrue(TEXT("Sequence folder actor float key updated"), bVerifiedActorFloat);
	TestTrue(TEXT("Sequence folder component float key updated"), bVerifiedComponentFloat);
	TestTrue(TEXT("Sequence folder spawnable transform key updated"), bVerifiedSpawnableTransform);
	TestTrue(TEXT("Sequence folder spawn track updated"), bVerifiedSpawnTrack);

	TSharedPtr<FJsonObject> VerifyMasterTracksJson;
	TestTrue(TEXT("Sequence folder verify master tracks load"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("master_tracks"), TEXT("index.json")), VerifyMasterTracksJson));
	bool bVerifiedCameraCutTrack = false;
	bool bVerifiedSubSequenceTrack = false;
	if (VerifyMasterTracksJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* MasterTracks = nullptr;
		TestTrue(TEXT("Sequence folder verify master tracks array exists"), VerifyMasterTracksJson->TryGetArrayField(TEXT("tracks"), MasterTracks) && MasterTracks != nullptr);
		if (MasterTracks)
		{
			for (const TSharedPtr<FJsonValue>& TrackValue : *MasterTracks)
			{
				const TSharedPtr<FJsonObject>* TrackObj = nullptr;
				if (!TrackValue.IsValid() || !TrackValue->TryGetObject(TrackObj) || !TrackObj || !(*TrackObj).IsValid())
				{
					continue;
				}
				const FString TrackType = (*TrackObj)->GetStringField(TEXT("track_type"));
				if (TrackType == TEXT("camera_cut"))
				{
					const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
					if ((*TrackObj)->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
					{
						const TSharedPtr<FJsonObject>* SectionObj = nullptr;
						if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
						{
							bVerifiedCameraCutTrack =
								(*SectionObj)->GetStringField(TEXT("camera_binding_guid")) == CameraBindingGuid &&
								FMath::IsNearlyEqual(static_cast<float>((*SectionObj)->GetNumberField(TEXT("duration_seconds"))), 2.0f, 0.01f);
						}
					}
				}
				else if (TrackType == TEXT("sub_sequence"))
				{
					const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
					if ((*TrackObj)->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
					{
						const TSharedPtr<FJsonObject>* SectionObj = nullptr;
						if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
						{
							const FString ExportedSequenceAsset = (*SectionObj)->GetStringField(TEXT("sequence_asset"));
							bVerifiedSubSequenceTrack =
								ExportedSequenceAsset.StartsWith(ChildSequenceAssetPath) &&
								FMath::IsNearlyEqual(static_cast<float>((*SectionObj)->GetNumberField(TEXT("start_seconds"))), 2.0f, 0.01f);
						}
					}
				}
			}
		}
	}
	TestTrue(TEXT("Sequence folder camera cut track updated"), bVerifiedCameraCutTrack);
	TestTrue(TEXT("Sequence folder sub sequence track updated"), bVerifiedSubSequenceTrack);

	const FString DestroyActorBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-destroy-001"),
		TEXT("destroy_actor"),
		FString::Printf(TEXT("{\"id\":\"%s\"}"), *ActorLabel));
	FHttpSmokeResult DestroyActorResult;
	TestTrue(TEXT("Sequence folder destroy actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DestroyActorBody, DestroyActorResult));
	TestEqual(TEXT("Sequence folder destroy actor status code"), DestroyActorResult.StatusCode, 200);

	if (DirectCameraActor)
	{
		DirectCameraActor->Destroy();
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceSequenceCinematicShotFolderSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.SequenceCinematicShotFolderWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceSequenceCinematicShotFolderSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString SequenceAssetPath = MakeAutomationAssetPath(TEXT("SEQCINESHOT"));
	const FString ChildSequenceAssetPath = MakeAutomationAssetPath(TEXT("SEQCINESUB"));
	const FString SequenceObjectPath = FString::Printf(TEXT("%s.%s"), *SequenceAssetPath, *FPackageName::GetLongPackageAssetName(SequenceAssetPath));
	const FString FolderRoot = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("LevelSequence")));
	const FString FolderPath = FPaths::Combine(FolderRoot, SequenceAssetPath.RightChop(1));
	const FString ShotDisplayName = TEXT("SmokeShot_A");

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

	const FString CreateSequenceBody = MakeExecRequestBody(
		TEXT("smoke-sequence-cinematic-shot-create-001"),
		TEXT("sequence_create_level_sequence"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"start_seconds\":0.0,\"duration_seconds\":3.0,\"display_rate_num\":24,\"display_rate_den\":1,\"open_editor\":false,\"save_after_create\":false}"), *SequenceAssetPath));
	FHttpSmokeResult CreateSequenceResult;
	TestTrue(TEXT("Sequence cinematic shot create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateSequenceBody, CreateSequenceResult));
	TestEqual(TEXT("Sequence cinematic shot create status code"), CreateSequenceResult.StatusCode, 200);

	const FString CreateChildSequenceBody = MakeExecRequestBody(
		TEXT("smoke-sequence-cinematic-shot-create-child-001"),
		TEXT("sequence_create_level_sequence"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"start_seconds\":0.0,\"duration_seconds\":1.0,\"display_rate_num\":24,\"display_rate_den\":1,\"open_editor\":false,\"save_after_create\":false}"), *ChildSequenceAssetPath));
	FHttpSmokeResult CreateChildSequenceResult;
	TestTrue(TEXT("Sequence cinematic shot create child request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateChildSequenceBody, CreateChildSequenceResult));
	TestEqual(TEXT("Sequence cinematic shot create child status code"), CreateChildSequenceResult.StatusCode, 200);

	const FString ExportFolderBody = MakeExecRequestBody(
		TEXT("smoke-sequence-cinematic-shot-export-001"),
		TEXT("sequence_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clean_output_dir\":true,\"include_validation\":true}"), *SequenceAssetPath));
	FHttpSmokeResult ExportFolderResult;
	TestTrue(TEXT("Sequence cinematic shot export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ExportFolderBody, ExportFolderResult));
	TestEqual(TEXT("Sequence cinematic shot export status code"), ExportFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> MasterTracksJson;
	TestTrue(TEXT("Sequence cinematic shot master tracks load"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("master_tracks"), TEXT("index.json")), MasterTracksJson));
	if (MasterTracksJson.IsValid())
	{
		TArray<TSharedPtr<FJsonValue>> MasterTracks;
		TSharedPtr<FJsonObject> ShotTrackObj = MakeShared<FJsonObject>();
		ShotTrackObj->SetStringField(TEXT("track_type"), TEXT("cinematic_shot"));
		TSharedPtr<FJsonObject> ShotSectionObj = MakeShared<FJsonObject>();
		ShotSectionObj->SetStringField(TEXT("section_type"), TEXT("cinematic_shot"));
		ShotSectionObj->SetStringField(TEXT("sequence_asset"), ChildSequenceAssetPath);
		ShotSectionObj->SetStringField(TEXT("shot_display_name"), ShotDisplayName);
		ShotSectionObj->SetNumberField(TEXT("start_seconds"), 0.5);
		ShotSectionObj->SetNumberField(TEXT("duration_seconds"), 1.25);
		ShotSectionObj->SetNumberField(TEXT("row_index"), 0);
		ShotSectionObj->SetNumberField(TEXT("start_frame_offset"), 0);
		ShotSectionObj->SetBoolField(TEXT("can_loop"), false);
		ShotSectionObj->SetNumberField(TEXT("end_frame_offset"), 0);
		ShotSectionObj->SetNumberField(TEXT("first_loop_start_frame_offset"), 0);
		ShotSectionObj->SetNumberField(TEXT("hierarchical_bias"), 100);
		ShotSectionObj->SetNumberField(TEXT("network_mask"), 0);
		ShotSectionObj->SetStringField(TEXT("time_scale_type"), TEXT("fixed"));
		ShotSectionObj->SetNumberField(TEXT("time_scale"), 1.0);
		ShotTrackObj->SetArrayField(TEXT("sections"), { MakeShared<FJsonValueObject>(ShotSectionObj) });
		MasterTracks.Add(MakeShared<FJsonValueObject>(ShotTrackObj));
		MasterTracksJson->SetArrayField(TEXT("tracks"), MasterTracks);
	}
	TestTrue(TEXT("Sequence cinematic shot master tracks save"), SaveJsonFile(FPaths::Combine(FolderPath, TEXT("master_tracks"), TEXT("index.json")), MasterTracksJson));

	const FString ApplyFolderBody = MakeExecRequestBody(
		TEXT("smoke-sequence-cinematic-shot-apply-001"),
		TEXT("sequence_apply_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"create_if_missing\":false,\"save_after_apply\":false}"), *SequenceAssetPath));
	FHttpSmokeResult ApplyFolderResult;
	TestTrue(TEXT("Sequence cinematic shot apply request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ApplyFolderBody, ApplyFolderResult));
	TestEqual(TEXT("Sequence cinematic shot apply status code"), ApplyFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ApplyFolderJson;
	TestTrue(TEXT("Sequence cinematic shot apply response parses JSON"), ParseJson(ApplyFolderResult.Body, ApplyFolderJson));
	TestTrue(TEXT("Sequence cinematic shot apply root ok=true"), ApplyFolderJson.IsValid() && ApplyFolderJson->GetBoolField(TEXT("ok")));

	const FString ReExportBody = MakeExecRequestBody(
		TEXT("smoke-sequence-cinematic-shot-export-002"),
		TEXT("sequence_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clean_output_dir\":true,\"include_validation\":true}"), *SequenceAssetPath));
	FHttpSmokeResult ReExportResult;
	TestTrue(TEXT("Sequence cinematic shot re-export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ReExportBody, ReExportResult));
	TestEqual(TEXT("Sequence cinematic shot re-export status code"), ReExportResult.StatusCode, 200);

	ULevelSequence* SequenceAsset = LoadObject<ULevelSequence>(nullptr, *SequenceObjectPath);
	TestNotNull(TEXT("Sequence cinematic shot direct load sequence asset"), SequenceAsset);
	UMovieScene* MovieScene = SequenceAsset ? SequenceAsset->GetMovieScene() : nullptr;
	TestNotNull(TEXT("Sequence cinematic shot direct movie scene"), MovieScene);

	UMovieSceneCinematicShotTrack* ShotTrack = MovieScene ? MovieScene->FindTrack<UMovieSceneCinematicShotTrack>() : nullptr;
	TestNotNull(TEXT("Sequence cinematic shot direct track exists"), ShotTrack);
	if (ShotTrack)
	{
		const TArray<UMovieSceneSection*>& ShotSections = ShotTrack->GetAllSections();
		TestEqual(TEXT("Sequence cinematic shot direct section count"), ShotSections.Num(), 1);
		if (ShotSections.Num() == 1)
		{
			UMovieSceneCinematicShotSection* ShotSection = Cast<UMovieSceneCinematicShotSection>(ShotSections[0]);
			TestNotNull(TEXT("Sequence cinematic shot direct section type"), ShotSection);
			if (ShotSection && MovieScene)
			{
				const FString ExportedSequenceAsset = ShotSection->GetSequence() ? ShotSection->GetSequence()->GetPathName() : FString();
				TestTrue(TEXT("Sequence cinematic shot direct child sequence path"), ExportedSequenceAsset.StartsWith(ChildSequenceAssetPath));
				TestEqual(TEXT("Sequence cinematic shot direct display name"), ShotSection->GetShotDisplayName(), ShotDisplayName);
				const FFrameRate TickResolution = MovieScene->GetTickResolution();
				const float StartSeconds = static_cast<float>(TickResolution.AsSeconds(FFrameTime(ShotSection->GetInclusiveStartFrame())));
				const float DurationSeconds = static_cast<float>(TickResolution.AsSeconds(FFrameTime(ShotSection->GetExclusiveEndFrame() - ShotSection->GetInclusiveStartFrame())));
				TestTrue(TEXT("Sequence cinematic shot direct start seconds"), FMath::IsNearlyEqual(StartSeconds, 0.5f, 0.01f));
				TestTrue(TEXT("Sequence cinematic shot direct duration seconds"), FMath::IsNearlyEqual(DurationSeconds, 1.25f, 0.01f));
			}
		}
	}

	TSharedPtr<FJsonObject> VerifyMasterTracksJson;
	TestTrue(TEXT("Sequence cinematic shot verify master tracks load"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("master_tracks"), TEXT("index.json")), VerifyMasterTracksJson));
	bool bVerifiedCinematicShotTrack = false;
	if (VerifyMasterTracksJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* MasterTracks = nullptr;
		TestTrue(TEXT("Sequence cinematic shot verify master tracks array exists"), VerifyMasterTracksJson->TryGetArrayField(TEXT("tracks"), MasterTracks) && MasterTracks != nullptr);
		if (MasterTracks)
		{
			for (const TSharedPtr<FJsonValue>& TrackValue : *MasterTracks)
			{
				const TSharedPtr<FJsonObject>* TrackObj = nullptr;
				if (!TrackValue.IsValid() || !TrackValue->TryGetObject(TrackObj) || !TrackObj || !(*TrackObj).IsValid())
				{
					continue;
				}

				if ((*TrackObj)->GetStringField(TEXT("track_type")) != TEXT("cinematic_shot"))
				{
					continue;
				}

				const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
				if (!(*TrackObj)->TryGetArrayField(TEXT("sections"), Sections) || !Sections || Sections->Num() != 1)
				{
					continue;
				}

				const TSharedPtr<FJsonObject>* SectionObj = nullptr;
				if (!(*Sections)[0]->TryGetObject(SectionObj) || !SectionObj || !(*SectionObj).IsValid())
				{
					continue;
				}

				const FString ExportedSequenceAsset = (*SectionObj)->GetStringField(TEXT("sequence_asset"));
				bVerifiedCinematicShotTrack =
					(*SectionObj)->GetStringField(TEXT("shot_display_name")) == ShotDisplayName &&
					ExportedSequenceAsset.StartsWith(ChildSequenceAssetPath) &&
					FMath::IsNearlyEqual(static_cast<float>((*SectionObj)->GetNumberField(TEXT("start_seconds"))), 0.5f, 0.01f) &&
					FMath::IsNearlyEqual(static_cast<float>((*SectionObj)->GetNumberField(TEXT("duration_seconds"))), 1.25f, 0.01f);
			}
		}
	}
	TestTrue(TEXT("Sequence cinematic shot track updated"), bVerifiedCinematicShotTrack);

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceSequenceOutlinerFoldersSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.SequenceOutlinerFoldersWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceSequenceOutlinerFoldersSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString SequenceAssetPath = MakeAutomationAssetPath(TEXT("SEQOUTLINER"));
	const FString ChildSequenceAssetPath = MakeAutomationAssetPath(TEXT("SEQOUTSUB"));
	const FString ActorLabel = FString::Printf(TEXT("SmokeSeqFolder_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString SequenceObjectPath = FString::Printf(TEXT("%s.%s"), *SequenceAssetPath, *FPackageName::GetLongPackageAssetName(SequenceAssetPath));
	const FString FolderRoot = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("LevelSequence")));
	const FString FolderPath = FPaths::Combine(FolderRoot, SequenceAssetPath.RightChop(1));

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

	const FString SpawnActorBody = MakeExecRequestBody(
		TEXT("smoke-sequence-outliner-spawn-001"),
		TEXT("spawn_actor"),
		FString::Printf(TEXT("{\"class_path\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":0,\"y\":0,\"z\":100}}"), *ActorLabel));
	FHttpSmokeResult SpawnActorResult;
	TestTrue(TEXT("Sequence outliner spawn actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnActorBody, SpawnActorResult));
	TestEqual(TEXT("Sequence outliner spawn actor status code"), SpawnActorResult.StatusCode, 200);

	const FString CreateSequenceBody = MakeExecRequestBody(
		TEXT("smoke-sequence-outliner-create-001"),
		TEXT("sequence_create_level_sequence"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"start_seconds\":0.0,\"duration_seconds\":2.0,\"display_rate_num\":24,\"display_rate_den\":1,\"open_editor\":false,\"save_after_create\":false}"), *SequenceAssetPath));
	FHttpSmokeResult CreateSequenceResult;
	TestTrue(TEXT("Sequence outliner create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateSequenceBody, CreateSequenceResult));
	TestEqual(TEXT("Sequence outliner create status code"), CreateSequenceResult.StatusCode, 200);

	const FString CreateChildSequenceBody = MakeExecRequestBody(
		TEXT("smoke-sequence-outliner-create-child-001"),
		TEXT("sequence_create_level_sequence"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"start_seconds\":0.0,\"duration_seconds\":1.0,\"display_rate_num\":24,\"display_rate_den\":1,\"open_editor\":false,\"save_after_create\":false}"), *ChildSequenceAssetPath));
	FHttpSmokeResult CreateChildSequenceResult;
	TestTrue(TEXT("Sequence outliner create child request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateChildSequenceBody, CreateChildSequenceResult));
	TestEqual(TEXT("Sequence outliner create child status code"), CreateChildSequenceResult.StatusCode, 200);

	const FString AddBindingBody = MakeExecRequestBody(
		TEXT("smoke-sequence-outliner-bind-001"),
		TEXT("sequence_add_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *ActorLabel));
	FHttpSmokeResult AddBindingResult;
	TestTrue(TEXT("Sequence outliner add binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddBindingBody, AddBindingResult));
	TestEqual(TEXT("Sequence outliner add binding status code"), AddBindingResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddBindingJson;
	TestTrue(TEXT("Sequence outliner add binding parses JSON"), ParseJson(AddBindingResult.Body, AddBindingJson));
	const TSharedPtr<FJsonObject>* AddBindingData = nullptr;
	TestTrue(TEXT("Sequence outliner add binding contains data"), AddBindingJson.IsValid() && AddBindingJson->TryGetObjectField(TEXT("data"), AddBindingData) && AddBindingData && AddBindingData->IsValid());
	const FString BindingGuid = (AddBindingData && AddBindingData->IsValid()) ? (*AddBindingData)->GetStringField(TEXT("binding_guid")) : FString();
	TestTrue(TEXT("Sequence outliner binding guid non-empty"), !BindingGuid.IsEmpty());

	FGuid BindingGuidValue;
	TestTrue(TEXT("Sequence outliner parse binding guid"), FGuid::Parse(BindingGuid, BindingGuidValue));

	const FString AddTransformTrackBody = MakeExecRequestBody(
		TEXT("smoke-sequence-outliner-transform-track-001"),
		TEXT("sequence_add_transform_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddTransformTrackResult;
	TestTrue(TEXT("Sequence outliner add transform track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddTransformTrackBody, AddTransformTrackResult));
	TestEqual(TEXT("Sequence outliner add transform track status code"), AddTransformTrackResult.StatusCode, 200);

	const FString AddTransformKeyBody = MakeExecRequestBody(
		TEXT("smoke-sequence-outliner-transform-key-001"),
		TEXT("sequence_add_transform_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"time_seconds\":0.5,\"location\":{\"x\":25,\"y\":0,\"z\":0},\"rotation\":{\"pitch\":0,\"yaw\":15,\"roll\":0},\"scale\":{\"x\":1.0,\"y\":1.0,\"z\":1.0},\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddTransformKeyResult;
	TestTrue(TEXT("Sequence outliner add transform key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddTransformKeyBody, AddTransformKeyResult));
	TestEqual(TEXT("Sequence outliner add transform key status code"), AddTransformKeyResult.StatusCode, 200);

	const FString ExportFolderBody = MakeExecRequestBody(
		TEXT("smoke-sequence-outliner-export-001"),
		TEXT("sequence_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clean_output_dir\":true,\"include_validation\":true}"), *SequenceAssetPath));
	FHttpSmokeResult ExportFolderResult;
	TestTrue(TEXT("Sequence outliner export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ExportFolderBody, ExportFolderResult));
	TestEqual(TEXT("Sequence outliner export status code"), ExportFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> BindingsIndexJson;
	TestTrue(TEXT("Sequence outliner bindings index load"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("bindings"), TEXT("index.json")), BindingsIndexJson));
	FString BindingFolderName;
	if (BindingsIndexJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		TestTrue(TEXT("Sequence outliner bindings index array exists"), BindingsIndexJson->TryGetArrayField(TEXT("bindings"), Bindings) && Bindings != nullptr);
		if (Bindings)
		{
			for (const TSharedPtr<FJsonValue>& BindingValue : *Bindings)
			{
				const TSharedPtr<FJsonObject>* BindingObj = nullptr;
				if (!BindingValue.IsValid() || !BindingValue->TryGetObject(BindingObj) || !BindingObj || !(*BindingObj).IsValid())
				{
					continue;
				}
				if ((*BindingObj)->GetStringField(TEXT("binding_guid")) == BindingGuid)
				{
					BindingFolderName = (*BindingObj)->GetStringField(TEXT("folder_name"));
					break;
				}
			}
		}
	}
	TestTrue(TEXT("Sequence outliner binding folder name found"), !BindingFolderName.IsEmpty());

	FString TransformTrackFileName;
	if (!BindingFolderName.IsEmpty())
	{
		const FString BindingTracksDir = FPaths::Combine(FolderPath, TEXT("bindings"), BindingFolderName, TEXT("tracks"));
		TArray<FString> TrackFiles;
		IFileManager::Get().FindFiles(TrackFiles, *(FPaths::Combine(BindingTracksDir, TEXT("*.json"))), true, false);
		TrackFiles.Sort();
		for (const FString& TrackFile : TrackFiles)
		{
			TSharedPtr<FJsonObject> TrackJson;
			TestTrue(FString::Printf(TEXT("Sequence outliner load binding track file %s"), *TrackFile), LoadJsonFile(FPaths::Combine(BindingTracksDir, TrackFile), TrackJson));
			if (TrackJson.IsValid() && TrackJson->GetStringField(TEXT("track_type")) == TEXT("transform"))
			{
				TransformTrackFileName = TrackFile;
				break;
			}
		}
	}
	TestTrue(TEXT("Sequence outliner transform track file found"), !TransformTrackFileName.IsEmpty());

	TSharedPtr<FJsonObject> MasterTracksJson;
	TestTrue(TEXT("Sequence outliner master tracks load"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("master_tracks"), TEXT("index.json")), MasterTracksJson));
	if (MasterTracksJson.IsValid())
	{
		TArray<TSharedPtr<FJsonValue>> MasterTracks;
		TSharedPtr<FJsonObject> SubSequenceTrackObj = MakeShared<FJsonObject>();
		SubSequenceTrackObj->SetStringField(TEXT("track_type"), TEXT("sub_sequence"));
		TSharedPtr<FJsonObject> SubSequenceSectionObj = MakeShared<FJsonObject>();
		SubSequenceSectionObj->SetStringField(TEXT("section_type"), TEXT("sub_sequence"));
		SubSequenceSectionObj->SetStringField(TEXT("sequence_asset"), ChildSequenceAssetPath);
		SubSequenceSectionObj->SetNumberField(TEXT("start_seconds"), 0.25);
		SubSequenceSectionObj->SetNumberField(TEXT("duration_seconds"), 1.0);
		SubSequenceSectionObj->SetNumberField(TEXT("row_index"), 0);
		SubSequenceSectionObj->SetNumberField(TEXT("start_frame_offset"), 0);
		SubSequenceSectionObj->SetBoolField(TEXT("can_loop"), false);
		SubSequenceSectionObj->SetNumberField(TEXT("end_frame_offset"), 0);
		SubSequenceSectionObj->SetNumberField(TEXT("first_loop_start_frame_offset"), 0);
		SubSequenceSectionObj->SetNumberField(TEXT("hierarchical_bias"), 100);
		SubSequenceSectionObj->SetNumberField(TEXT("network_mask"), 0);
		SubSequenceSectionObj->SetStringField(TEXT("time_scale_type"), TEXT("fixed"));
		SubSequenceSectionObj->SetNumberField(TEXT("time_scale"), 1.0);
		SubSequenceTrackObj->SetArrayField(TEXT("sections"), { MakeShared<FJsonValueObject>(SubSequenceSectionObj) });
		MasterTracks.Add(MakeShared<FJsonValueObject>(SubSequenceTrackObj));
		MasterTracksJson->SetArrayField(TEXT("tracks"), MasterTracks);
	}
	TestTrue(TEXT("Sequence outliner master tracks save"), SaveJsonFile(FPaths::Combine(FolderPath, TEXT("master_tracks"), TEXT("index.json")), MasterTracksJson));

	TSharedPtr<FJsonObject> OutlinerFoldersJson = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> FolderEntries;
	{
		TSharedPtr<FJsonObject> RootFolderObj = MakeShared<FJsonObject>();
		RootFolderObj->SetStringField(TEXT("folder_id"), TEXT("Main"));
		RootFolderObj->SetStringField(TEXT("folder_name"), TEXT("Main"));
		RootFolderObj->SetStringField(TEXT("folder_path"), TEXT("Main"));
		RootFolderObj->SetStringField(TEXT("parent_folder_id"), TEXT(""));
		RootFolderObj->SetNumberField(TEXT("sorting_order"), 10);
		TSharedPtr<FJsonObject> RootColorObj = MakeShared<FJsonObject>();
		RootColorObj->SetNumberField(TEXT("r"), 255);
		RootColorObj->SetNumberField(TEXT("g"), 64);
		RootColorObj->SetNumberField(TEXT("b"), 64);
		RootColorObj->SetNumberField(TEXT("a"), 255);
		RootFolderObj->SetObjectField(TEXT("color"), RootColorObj);
		RootFolderObj->SetArrayField(TEXT("child_binding_guids"), { MakeShared<FJsonValueString>(BindingGuid) });
		TSharedPtr<FJsonObject> RootBindingTrackObj = MakeShared<FJsonObject>();
		RootBindingTrackObj->SetStringField(TEXT("binding_guid"), BindingGuid);
		RootBindingTrackObj->SetStringField(TEXT("track_file"), TransformTrackFileName);
		RootBindingTrackObj->SetStringField(TEXT("track_type"), TEXT("transform"));
		RootFolderObj->SetArrayField(TEXT("child_binding_tracks"), { MakeShared<FJsonValueObject>(RootBindingTrackObj) });
		RootFolderObj->SetArrayField(TEXT("child_master_tracks"), {});
		FolderEntries.Add(MakeShared<FJsonValueObject>(RootFolderObj));

		TSharedPtr<FJsonObject> ChildFolderObj = MakeShared<FJsonObject>();
		ChildFolderObj->SetStringField(TEXT("folder_id"), TEXT("Main/Shots"));
		ChildFolderObj->SetStringField(TEXT("folder_name"), TEXT("Shots"));
		ChildFolderObj->SetStringField(TEXT("folder_path"), TEXT("Main/Shots"));
		ChildFolderObj->SetStringField(TEXT("parent_folder_id"), TEXT("Main"));
		ChildFolderObj->SetNumberField(TEXT("sorting_order"), 20);
		TSharedPtr<FJsonObject> ChildColorObj = MakeShared<FJsonObject>();
		ChildColorObj->SetNumberField(TEXT("r"), 64);
		ChildColorObj->SetNumberField(TEXT("g"), 128);
		ChildColorObj->SetNumberField(TEXT("b"), 255);
		ChildColorObj->SetNumberField(TEXT("a"), 255);
		ChildFolderObj->SetObjectField(TEXT("color"), ChildColorObj);
		ChildFolderObj->SetArrayField(TEXT("child_binding_guids"), {});
		ChildFolderObj->SetArrayField(TEXT("child_binding_tracks"), {});
		TSharedPtr<FJsonObject> ChildTrackObj = MakeShared<FJsonObject>();
		ChildTrackObj->SetStringField(TEXT("track_type"), TEXT("sub_sequence"));
		ChildFolderObj->SetArrayField(TEXT("child_master_tracks"), { MakeShared<FJsonValueObject>(ChildTrackObj) });
		FolderEntries.Add(MakeShared<FJsonValueObject>(ChildFolderObj));
	}
	OutlinerFoldersJson->SetArrayField(TEXT("folders"), FolderEntries);
	TestTrue(TEXT("Sequence outliner folders save"), SaveJsonFile(FPaths::Combine(FolderPath, TEXT("outliner"), TEXT("folders.json")), OutlinerFoldersJson));

	const FString ApplyFolderBody = MakeExecRequestBody(
		TEXT("smoke-sequence-outliner-apply-001"),
		TEXT("sequence_apply_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"create_if_missing\":false,\"save_after_apply\":false}"), *SequenceAssetPath));
	FHttpSmokeResult ApplyFolderResult;
	TestTrue(TEXT("Sequence outliner apply request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ApplyFolderBody, ApplyFolderResult));
	TestEqual(TEXT("Sequence outliner apply status code"), ApplyFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ApplyFolderJson;
	TestTrue(TEXT("Sequence outliner apply response parses JSON"), ParseJson(ApplyFolderResult.Body, ApplyFolderJson));
	TestTrue(TEXT("Sequence outliner apply root ok=true"), ApplyFolderJson.IsValid() && ApplyFolderJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* ApplyFolderData = nullptr;
	TestTrue(TEXT("Sequence outliner apply contains data"), ApplyFolderJson.IsValid() && ApplyFolderJson->TryGetObjectField(TEXT("data"), ApplyFolderData) && ApplyFolderData && (*ApplyFolderData).IsValid());
	if (ApplyFolderData && (*ApplyFolderData).IsValid())
	{
		TestEqual(TEXT("Sequence outliner folders_applied"), static_cast<int32>((*ApplyFolderData)->GetIntegerField(TEXT("folders_applied"))), 2);
	}

	const FString ReExportBody = MakeExecRequestBody(
		TEXT("smoke-sequence-outliner-export-002"),
		TEXT("sequence_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clean_output_dir\":true,\"include_validation\":true}"), *SequenceAssetPath));
	FHttpSmokeResult ReExportResult;
	TestTrue(TEXT("Sequence outliner re-export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ReExportBody, ReExportResult));
	TestEqual(TEXT("Sequence outliner re-export status code"), ReExportResult.StatusCode, 200);

	ULevelSequence* SequenceAsset = LoadObject<ULevelSequence>(nullptr, *SequenceObjectPath);
	TestNotNull(TEXT("Sequence outliner direct load sequence asset"), SequenceAsset);
	UMovieScene* MovieScene = SequenceAsset ? SequenceAsset->GetMovieScene() : nullptr;
	TestNotNull(TEXT("Sequence outliner direct movie scene"), MovieScene);
	UMovieSceneSubTrack* SubSequenceTrack = MovieScene ? MovieScene->FindTrack<UMovieSceneSubTrack>() : nullptr;
	TestNotNull(TEXT("Sequence outliner direct sub sequence track"), SubSequenceTrack);
	UMovieScene3DTransformTrack* BindingTransformTrack = nullptr;
	if (MovieScene)
	{
		if (const FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuidValue))
		{
			for (UMovieSceneTrack* Track : Binding->GetTracks())
			{
				if (UMovieScene3DTransformTrack* TransformTrack = Cast<UMovieScene3DTransformTrack>(Track))
				{
					BindingTransformTrack = TransformTrack;
					break;
				}
			}
		}
	}
	TestNotNull(TEXT("Sequence outliner direct binding transform track"), BindingTransformTrack);

	TArray<UMovieSceneFolder*> RootFolders;
	if (MovieScene)
	{
		MovieScene->GetRootFolders(RootFolders);
	}
	TestEqual(TEXT("Sequence outliner root folder count"), RootFolders.Num(), 1);
	if (RootFolders.Num() == 1 && RootFolders[0])
	{
		UMovieSceneFolder* RootFolder = RootFolders[0];
		TestEqual(TEXT("Sequence outliner root folder name"), RootFolder->GetFolderName().ToString(), FString(TEXT("Main")));
		TestTrue(TEXT("Sequence outliner root contains actor binding"), RootFolder->GetChildObjectBindings().Contains(BindingGuidValue));
		TestTrue(TEXT("Sequence outliner root contains binding transform track"), BindingTransformTrack && RootFolder->GetChildTracks().Contains(BindingTransformTrack));
#if WITH_EDITORONLY_DATA
		TestEqual(TEXT("Sequence outliner root sorting order"), RootFolder->GetSortingOrder(), 10);
		TestEqual(TEXT("Sequence outliner root color r"), static_cast<int32>(RootFolder->GetFolderColor().R), 255);
#endif
		const TArrayView<UMovieSceneFolder* const> ChildFolders = RootFolder->GetChildFolders();
		TestEqual(TEXT("Sequence outliner child folder count"), static_cast<int32>(ChildFolders.Num()), 1);
		if (ChildFolders.Num() == 1 && ChildFolders[0])
		{
			UMovieSceneFolder* ChildFolder = ChildFolders[0];
			TestEqual(TEXT("Sequence outliner child folder name"), ChildFolder->GetFolderName().ToString(), FString(TEXT("Shots")));
			TestTrue(TEXT("Sequence outliner child contains sub sequence track"), ChildFolder->GetChildTracks().Contains(SubSequenceTrack));
#if WITH_EDITORONLY_DATA
			TestEqual(TEXT("Sequence outliner child sorting order"), ChildFolder->GetSortingOrder(), 20);
			TestEqual(TEXT("Sequence outliner child color b"), static_cast<int32>(ChildFolder->GetFolderColor().B), 255);
#endif
		}
	}

	TSharedPtr<FJsonObject> VerifyOutlinerJson;
	TestTrue(TEXT("Sequence outliner verify folders load"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("outliner"), TEXT("folders.json")), VerifyOutlinerJson));
	bool bVerifiedRootFolder = false;
	bool bVerifiedChildFolder = false;
	bool bVerifiedRootBindingTrack = false;
	if (VerifyOutlinerJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Folders = nullptr;
		TestTrue(TEXT("Sequence outliner verify folders array exists"), VerifyOutlinerJson->TryGetArrayField(TEXT("folders"), Folders) && Folders != nullptr);
		if (Folders)
		{
			for (const TSharedPtr<FJsonValue>& FolderValue : *Folders)
			{
				const TSharedPtr<FJsonObject>* FolderObj = nullptr;
				if (!FolderValue.IsValid() || !FolderValue->TryGetObject(FolderObj) || !FolderObj || !(*FolderObj).IsValid())
				{
					continue;
				}

				const FString FolderId = (*FolderObj)->GetStringField(TEXT("folder_id"));
				if (FolderId == TEXT("Main"))
				{
					const TArray<TSharedPtr<FJsonValue>>* ChildBindings = nullptr;
					if ((*FolderObj)->TryGetArrayField(TEXT("child_binding_guids"), ChildBindings) && ChildBindings && ChildBindings->Num() == 1)
					{
						FString ExportedBindingGuid;
						if ((*ChildBindings)[0].IsValid() && (*ChildBindings)[0]->TryGetString(ExportedBindingGuid))
						{
							bVerifiedRootFolder = ExportedBindingGuid == BindingGuid;
						}
					}
					const TArray<TSharedPtr<FJsonValue>>* ChildBindingTracks = nullptr;
					if ((*FolderObj)->TryGetArrayField(TEXT("child_binding_tracks"), ChildBindingTracks) && ChildBindingTracks && ChildBindingTracks->Num() == 1)
					{
						const TSharedPtr<FJsonObject>* TrackObj = nullptr;
						if ((*ChildBindingTracks)[0].IsValid() && (*ChildBindingTracks)[0]->TryGetObject(TrackObj) && TrackObj && (*TrackObj).IsValid())
						{
							bVerifiedRootBindingTrack =
								(*TrackObj)->GetStringField(TEXT("binding_guid")) == BindingGuid &&
								(*TrackObj)->GetStringField(TEXT("track_file")) == TransformTrackFileName &&
								(*TrackObj)->GetStringField(TEXT("track_type")) == TEXT("transform");
						}
					}
				}
				else if (FolderId == TEXT("Main/Shots"))
				{
					const TArray<TSharedPtr<FJsonValue>>* ChildMasterTracks = nullptr;
					if ((*FolderObj)->TryGetArrayField(TEXT("child_master_tracks"), ChildMasterTracks) && ChildMasterTracks && ChildMasterTracks->Num() == 1)
					{
						const TSharedPtr<FJsonObject>* TrackObj = nullptr;
						if ((*ChildMasterTracks)[0].IsValid() && (*ChildMasterTracks)[0]->TryGetObject(TrackObj) && TrackObj && (*TrackObj).IsValid())
						{
							bVerifiedChildFolder =
								(*FolderObj)->GetStringField(TEXT("parent_folder_id")) == TEXT("Main") &&
								(*TrackObj)->GetStringField(TEXT("track_type")) == TEXT("sub_sequence");
						}
					}
				}
			}
		}
	}
	TestTrue(TEXT("Sequence outliner verify root folder export"), bVerifiedRootFolder);
	TestTrue(TEXT("Sequence outliner verify root binding track export"), bVerifiedRootBindingTrack);
	TestTrue(TEXT("Sequence outliner verify child folder export"), bVerifiedChildFolder);

	const FString DestroyActorBody = MakeExecRequestBody(
		TEXT("smoke-sequence-outliner-destroy-001"),
		TEXT("destroy_actor"),
		FString::Printf(TEXT("{\"id\":\"%s\"}"), *ActorLabel));
	FHttpSmokeResult DestroyActorResult;
	TestTrue(TEXT("Sequence outliner destroy actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DestroyActorBody, DestroyActorResult));
	TestEqual(TEXT("Sequence outliner destroy actor status code"), DestroyActorResult.StatusCode, 200);

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceLevelActorWorkflowSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.LevelActorWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceLevelActorWorkflowSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString ActorLabel = FString::Printf(TEXT("SmokeActor_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString TransactionLabel = FString::Printf(TEXT("SmokeTx_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	FString DuplicatedActorName;

	auto GetVectorComponent = [this](const TSharedPtr<FJsonObject>* Object, const TCHAR* FieldName, const TCHAR* ComponentName, double ExpectedValue, double Tolerance, const TCHAR* AssertionLabel)
	{
		if (!Object || !Object->IsValid())
		{
			AddError(FString::Printf(TEXT("%s object invalid"), AssertionLabel));
			return;
		}

		const TSharedPtr<FJsonObject>* VectorObject = nullptr;
		TestTrue(AssertionLabel, (*Object)->TryGetObjectField(FieldName, VectorObject) && VectorObject && VectorObject->IsValid());
		if (VectorObject && VectorObject->IsValid())
		{
			TestTrue(AssertionLabel, FMath::IsNearlyEqual((*VectorObject)->GetNumberField(ComponentName), ExpectedValue, Tolerance));
		}
	};

	auto ActorExistsByLabel = [&](const FString& Label, bool& bExistsOut) -> bool
	{
		const FString ListActorsBody = MakeExecRequestBody(
			TEXT("smoke-level-list-actors-001"),
			TEXT("list_actors"),
			TEXT("{\"limit\":2000}"));
		FHttpSmokeResult ListActorsResult;
		if (!TestTrue(TEXT("Level list_actors request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListActorsBody, ListActorsResult)))
		{
			return false;
		}
		TestEqual(TEXT("Level list_actors status code"), ListActorsResult.StatusCode, 200);

		TSharedPtr<FJsonObject> ListActorsJson;
		if (!TestTrue(TEXT("Level list_actors response parses as JSON"), ParseJson(ListActorsResult.Body, ListActorsJson)))
		{
			return false;
		}
		TestTrue(TEXT("Level list_actors root ok=true"), ListActorsJson.IsValid() && ListActorsJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* ListActorsData = nullptr;
		if (!TestTrue(TEXT("Level list_actors contains data object"), ListActorsJson.IsValid() && ListActorsJson->TryGetObjectField(TEXT("data"), ListActorsData) && ListActorsData && ListActorsData->IsValid()))
		{
			return false;
		}

		bExistsOut = false;
		if (ListActorsData && ListActorsData->IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
			TestTrue(TEXT("Level list_actors contains actors array"), (*ListActorsData)->TryGetArrayField(TEXT("actors"), Actors) && Actors != nullptr);
			if (Actors)
			{
				for (const TSharedPtr<FJsonValue>& ActorValue : *Actors)
				{
					const TSharedPtr<FJsonObject>* ActorObject = nullptr;
					if (!ActorValue.IsValid() || !ActorValue->TryGetObject(ActorObject) || !ActorObject || !ActorObject->IsValid())
					{
						continue;
					}

					if ((*ActorObject)->GetStringField(TEXT("label")) == Label)
					{
						bExistsOut = true;
						break;
					}
				}
			}
		}
		return true;
	};

	const FString SpawnBody = MakeExecRequestBody(
		TEXT("smoke-level-spawn-001"),
		TEXT("spawn_actor"),
		FString::Printf(
			TEXT("{\"class_path\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":0,\"y\":0,\"z\":128},\"scale\":{\"x\":1.0,\"y\":1.0,\"z\":1.0}}"),
			*ActorLabel));
	FHttpSmokeResult SpawnResult;
	TestTrue(TEXT("Level spawn_actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnBody, SpawnResult));
	TestEqual(TEXT("Level spawn_actor status code"), SpawnResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SpawnJson;
	TestTrue(TEXT("Level spawn_actor response parses as JSON"), ParseJson(SpawnResult.Body, SpawnJson));
	TestTrue(TEXT("Level spawn_actor root ok=true"), SpawnJson.IsValid() && SpawnJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* SpawnData = nullptr;
	TestTrue(TEXT("Level spawn_actor contains data object"), SpawnJson.IsValid() && SpawnJson->TryGetObjectField(TEXT("data"), SpawnData) && SpawnData && SpawnData->IsValid());
	if (SpawnData && SpawnData->IsValid())
	{
		TestEqual(TEXT("Level spawn_actor label"), (*SpawnData)->GetStringField(TEXT("label")), ActorLabel);
		TestEqual(TEXT("Level spawn_actor class"), (*SpawnData)->GetStringField(TEXT("class")), FString(TEXT("/Script/Engine.StaticMeshActor")));
	}

	const FString SetSelectionBody = MakeExecRequestBody(
		TEXT("smoke-level-selection-001"),
		TEXT("level_set_selection"),
		FString::Printf(TEXT("{\"actor_ids\":[\"%s\"],\"mode\":\"replace\"}"), *ActorLabel));
	FHttpSmokeResult SetSelectionResult;
	TestTrue(TEXT("Level set_selection request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetSelectionBody, SetSelectionResult));
	TestEqual(TEXT("Level set_selection status code"), SetSelectionResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SetSelectionJson;
	TestTrue(TEXT("Level set_selection response parses as JSON"), ParseJson(SetSelectionResult.Body, SetSelectionJson));
	TestTrue(TEXT("Level set_selection root ok=true"), SetSelectionJson.IsValid() && SetSelectionJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* SetSelectionData = nullptr;
	TestTrue(TEXT("Level set_selection contains data object"), SetSelectionJson.IsValid() && SetSelectionJson->TryGetObjectField(TEXT("data"), SetSelectionData) && SetSelectionData && SetSelectionData->IsValid());
	if (SetSelectionData && SetSelectionData->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
		bool bFoundSelectedActor = false;
		TestTrue(TEXT("Level set_selection contains actors array"), (*SetSelectionData)->TryGetArrayField(TEXT("actors"), Actors) && Actors != nullptr);
		if (Actors)
		{
			for (const TSharedPtr<FJsonValue>& ActorValue : *Actors)
			{
				const TSharedPtr<FJsonObject>* ActorObject = nullptr;
				if (!ActorValue.IsValid() || !ActorValue->TryGetObject(ActorObject) || !ActorObject || !ActorObject->IsValid())
				{
					continue;
				}

				if ((*ActorObject)->GetStringField(TEXT("label")) == ActorLabel)
				{
					bFoundSelectedActor = true;
					break;
				}
			}
		}
		TestTrue(TEXT("Level set_selection selected actor present"), bFoundSelectedActor);
	}

	const FString SetTransformBody = MakeExecRequestBody(
		TEXT("smoke-level-transform-001"),
		TEXT("level_set_actor_transform"),
		FString::Printf(
			TEXT("{\"id\":\"%s\",\"location\":{\"x\":123,\"y\":45,\"z\":67},\"rotation\":{\"pitch\":0,\"yaw\":33,\"roll\":0},\"scale\":{\"x\":1.25,\"y\":1.0,\"z\":0.75}}"),
			*ActorLabel));
	FHttpSmokeResult SetTransformResult;
	TestTrue(TEXT("Level set_actor_transform request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetTransformBody, SetTransformResult));
	TestEqual(TEXT("Level set_actor_transform status code"), SetTransformResult.StatusCode, 200);

	const FString GetTransformBody = MakeExecRequestBody(
		TEXT("smoke-level-get-transform-001"),
		TEXT("level_get_actor_transform"),
		FString::Printf(TEXT("{\"id\":\"%s\"}"), *ActorLabel));
	FHttpSmokeResult GetTransformResult;
	TestTrue(TEXT("Level get_actor_transform request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetTransformBody, GetTransformResult));
	TestEqual(TEXT("Level get_actor_transform status code"), GetTransformResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetTransformJson;
	TestTrue(TEXT("Level get_actor_transform response parses as JSON"), ParseJson(GetTransformResult.Body, GetTransformJson));
	TestTrue(TEXT("Level get_actor_transform root ok=true"), GetTransformJson.IsValid() && GetTransformJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* GetTransformData = nullptr;
	TestTrue(TEXT("Level get_actor_transform contains data object"), GetTransformJson.IsValid() && GetTransformJson->TryGetObjectField(TEXT("data"), GetTransformData) && GetTransformData && GetTransformData->IsValid());
	GetVectorComponent(GetTransformData, TEXT("location"), TEXT("x"), 123.0, 0.1, TEXT("Level get_actor_transform location.x"));
	GetVectorComponent(GetTransformData, TEXT("location"), TEXT("z"), 67.0, 0.1, TEXT("Level get_actor_transform location.z"));

	const FString BeginTransactionBody = MakeExecRequestBody(
		TEXT("smoke-level-begin-transaction-001"),
		TEXT("begin_transaction"),
		FString::Printf(TEXT("{\"label\":\"%s\"}"), *TransactionLabel));
	FHttpSmokeResult BeginTransactionResult;
	TestTrue(TEXT("Level begin_transaction request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, BeginTransactionBody, BeginTransactionResult));
	TestEqual(TEXT("Level begin_transaction status code"), BeginTransactionResult.StatusCode, 200);

	const FString SetLocationBody = MakeExecRequestBody(
		TEXT("smoke-level-location-001"),
		TEXT("level_set_actor_location"),
		FString::Printf(TEXT("{\"id\":\"%s\",\"location\":{\"x\":200,\"y\":300,\"z\":400}}"), *ActorLabel));
	FHttpSmokeResult SetLocationResult;
	TestTrue(TEXT("Level set_actor_location request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetLocationBody, SetLocationResult));
	TestEqual(TEXT("Level set_actor_location status code"), SetLocationResult.StatusCode, 200);

	const FString EndTransactionBody = MakeExecRequestBody(
		TEXT("smoke-level-end-transaction-001"),
		TEXT("end_transaction"),
		TEXT("{\"commit\":true}"));
	FHttpSmokeResult EndTransactionResult;
	TestTrue(TEXT("Level end_transaction request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, EndTransactionBody, EndTransactionResult));
	TestEqual(TEXT("Level end_transaction status code"), EndTransactionResult.StatusCode, 200);

	const FString GetTransformMovedBody = MakeExecRequestBody(
		TEXT("smoke-level-get-transform-002"),
		TEXT("level_get_actor_transform"),
		FString::Printf(TEXT("{\"id\":\"%s\"}"), *ActorLabel));
	FHttpSmokeResult GetTransformMovedResult;
	TestTrue(TEXT("Level get_actor_transform moved request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetTransformMovedBody, GetTransformMovedResult));
	TestEqual(TEXT("Level get_actor_transform moved status code"), GetTransformMovedResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetTransformMovedJson;
	TestTrue(TEXT("Level get_actor_transform moved response parses as JSON"), ParseJson(GetTransformMovedResult.Body, GetTransformMovedJson));
	TestTrue(TEXT("Level get_actor_transform moved root ok=true"), GetTransformMovedJson.IsValid() && GetTransformMovedJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* GetTransformMovedData = nullptr;
	TestTrue(TEXT("Level get_actor_transform moved contains data object"), GetTransformMovedJson.IsValid() && GetTransformMovedJson->TryGetObjectField(TEXT("data"), GetTransformMovedData) && GetTransformMovedData && GetTransformMovedData->IsValid());
	GetVectorComponent(GetTransformMovedData, TEXT("location"), TEXT("x"), 200.0, 0.1, TEXT("Level moved transform location.x"));
	GetVectorComponent(GetTransformMovedData, TEXT("location"), TEXT("y"), 300.0, 0.1, TEXT("Level moved transform location.y"));

	const FString UndoBody = MakeExecRequestBody(TEXT("smoke-level-undo-001"), TEXT("undo"));
	FHttpSmokeResult UndoResult;
	TestTrue(TEXT("Level undo request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, UndoBody, UndoResult));
	TestEqual(TEXT("Level undo status code"), UndoResult.StatusCode, 200);

	const FString GetTransformUndoBody = MakeExecRequestBody(
		TEXT("smoke-level-get-transform-003"),
		TEXT("level_get_actor_transform"),
		FString::Printf(TEXT("{\"id\":\"%s\"}"), *ActorLabel));
	FHttpSmokeResult GetTransformUndoResult;
	TestTrue(TEXT("Level get_actor_transform after undo request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetTransformUndoBody, GetTransformUndoResult));
	TestEqual(TEXT("Level get_actor_transform after undo status code"), GetTransformUndoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetTransformUndoJson;
	TestTrue(TEXT("Level get_actor_transform after undo response parses as JSON"), ParseJson(GetTransformUndoResult.Body, GetTransformUndoJson));
	TestTrue(TEXT("Level get_actor_transform after undo root ok=true"), GetTransformUndoJson.IsValid() && GetTransformUndoJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* GetTransformUndoData = nullptr;
	TestTrue(TEXT("Level get_actor_transform after undo contains data object"), GetTransformUndoJson.IsValid() && GetTransformUndoJson->TryGetObjectField(TEXT("data"), GetTransformUndoData) && GetTransformUndoData && GetTransformUndoData->IsValid());
	GetVectorComponent(GetTransformUndoData, TEXT("location"), TEXT("x"), 123.0, 0.1, TEXT("Level undo transform location.x"));
	GetVectorComponent(GetTransformUndoData, TEXT("location"), TEXT("z"), 67.0, 0.1, TEXT("Level undo transform location.z"));

	const FString RedoBody = MakeExecRequestBody(TEXT("smoke-level-redo-001"), TEXT("redo"));
	FHttpSmokeResult RedoResult;
	TestTrue(TEXT("Level redo request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RedoBody, RedoResult));
	TestEqual(TEXT("Level redo status code"), RedoResult.StatusCode, 200);

	const FString GetTransformRedoBody = MakeExecRequestBody(
		TEXT("smoke-level-get-transform-004"),
		TEXT("level_get_actor_transform"),
		FString::Printf(TEXT("{\"id\":\"%s\"}"), *ActorLabel));
	FHttpSmokeResult GetTransformRedoResult;
	TestTrue(TEXT("Level get_actor_transform after redo request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetTransformRedoBody, GetTransformRedoResult));
	TestEqual(TEXT("Level get_actor_transform after redo status code"), GetTransformRedoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetTransformRedoJson;
	TestTrue(TEXT("Level get_actor_transform after redo response parses as JSON"), ParseJson(GetTransformRedoResult.Body, GetTransformRedoJson));
	TestTrue(TEXT("Level get_actor_transform after redo root ok=true"), GetTransformRedoJson.IsValid() && GetTransformRedoJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* GetTransformRedoData = nullptr;
	TestTrue(TEXT("Level get_actor_transform after redo contains data object"), GetTransformRedoJson.IsValid() && GetTransformRedoJson->TryGetObjectField(TEXT("data"), GetTransformRedoData) && GetTransformRedoData && GetTransformRedoData->IsValid());
	GetVectorComponent(GetTransformRedoData, TEXT("location"), TEXT("x"), 200.0, 0.1, TEXT("Level redo transform location.x"));
	GetVectorComponent(GetTransformRedoData, TEXT("location"), TEXT("y"), 300.0, 0.1, TEXT("Level redo transform location.y"));

	const FString DuplicateBody = MakeExecRequestBody(
		TEXT("smoke-level-duplicate-001"),
		TEXT("level_duplicate_actor"),
		FString::Printf(TEXT("{\"id\":\"%s\",\"offset\":{\"x\":100,\"y\":0,\"z\":0}}"), *ActorLabel));
	FHttpSmokeResult DuplicateResult;
	TestTrue(TEXT("Level duplicate_actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DuplicateBody, DuplicateResult));
	TestEqual(TEXT("Level duplicate_actor status code"), DuplicateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> DuplicateJson;
	TestTrue(TEXT("Level duplicate_actor response parses as JSON"), ParseJson(DuplicateResult.Body, DuplicateJson));
	TestTrue(TEXT("Level duplicate_actor root ok=true"), DuplicateJson.IsValid() && DuplicateJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* DuplicateData = nullptr;
	TestTrue(TEXT("Level duplicate_actor contains data object"), DuplicateJson.IsValid() && DuplicateJson->TryGetObjectField(TEXT("data"), DuplicateData) && DuplicateData && DuplicateData->IsValid());
	if (DuplicateData && DuplicateData->IsValid())
	{
		const TSharedPtr<FJsonObject>* DuplicatedActor = nullptr;
		TestTrue(TEXT("Level duplicate_actor contains duplicated_actor object"), (*DuplicateData)->TryGetObjectField(TEXT("duplicated_actor"), DuplicatedActor) && DuplicatedActor && DuplicatedActor->IsValid());
		if (DuplicatedActor && DuplicatedActor->IsValid())
		{
			DuplicatedActorName = (*DuplicatedActor)->GetStringField(TEXT("name"));
			TestTrue(TEXT("Level duplicate_actor duplicated name non-empty"), !DuplicatedActorName.IsEmpty());
		}
	}

	const FString DestroyDuplicateBody = MakeExecRequestBody(
		TEXT("smoke-level-destroy-duplicate-001"),
		TEXT("destroy_actor"),
		FString::Printf(TEXT("{\"id\":\"%s\"}"), *DuplicatedActorName));
	FHttpSmokeResult DestroyDuplicateResult;
	TestTrue(TEXT("Level destroy duplicate request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DestroyDuplicateBody, DestroyDuplicateResult));
	TestEqual(TEXT("Level destroy duplicate status code"), DestroyDuplicateResult.StatusCode, 200);

	const FString DestroySourceBody = MakeExecRequestBody(
		TEXT("smoke-level-destroy-source-001"),
		TEXT("destroy_actor"),
		FString::Printf(TEXT("{\"id\":\"%s\"}"), *ActorLabel));
	FHttpSmokeResult DestroySourceResult;
	TestTrue(TEXT("Level destroy source request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DestroySourceBody, DestroySourceResult));
	TestEqual(TEXT("Level destroy source status code"), DestroySourceResult.StatusCode, 200);

	bool bSourceActorStillExists = true;
	if (ActorExistsByLabel(ActorLabel, bSourceActorStillExists))
	{
		TestFalse(TEXT("Level source actor removed from world"), bSourceActorStillExists);
	}

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
		TEXT("{\n  \"graph\": {\n    \"name\": \"MaterialGraph\",\n    \"graph_kind\": \"material_graph\"\n  },\n  \"expressions\": [\n    {\n      \"id\": \"param_color_main\",\n      \"expression_class\": \"/Script/Engine.MaterialExpressionVectorParameter\",\n      \"expression_name\": \"SmokeColorParam\",\n      \"pos\": { \"x\": -280, \"y\": 0 },\n      \"properties\": [\n        { \"property_name\": \"ParameterName\", \"value_text\": \"Color_Main\" },\n        { \"property_name\": \"DefaultValue\", \"value_text\": \"(R=0.200000,G=0.400000,B=1.000000,A=1.000000)\" },\n        { \"property_name\": \"Group\", \"value_text\": \"Surface\" },\n        { \"property_name\": \"SortPriority\", \"value_text\": \"0\" }\n      ]\n    }\n  ],\n  \"edges\": [],\n  \"root_inputs\": [\n    {\n      \"material_property\": \"BaseColor\",\n      \"from\": { \"node\": \"param_color_main\", \"output\": \"\" }\n    }\n  ]\n}\n")))
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
						break;
					}
				}
				TestTrue(TEXT("Material folder exported Color_Main parameter"), bFoundColorMain);
			}
		}
	}

	TSharedPtr<FJsonObject> ExportedSettingsJson;
	{
		const FString SettingsJsonPath = FPaths::Combine(FolderPath, TEXT("settings"), TEXT("material.json"));
		FString ExportedSettingsText;
		TestTrue(TEXT("Material folder exported material.json exists"), FFileHelper::LoadFileToString(ExportedSettingsText, *SettingsJsonPath));
		TestTrue(TEXT("Material folder exported material.json parses"), ParseJson(ExportedSettingsText, ExportedSettingsJson));
		if (ExportedSettingsJson.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* PropertiesArray = nullptr;
			TestTrue(TEXT("Material folder exported settings properties array exists"), ExportedSettingsJson->TryGetArrayField(TEXT("properties"), PropertiesArray) && PropertiesArray);
			if (PropertiesArray)
			{
				bool bFoundTwoSided = false;
				for (const TSharedPtr<FJsonValue>& PropertyValue : *PropertiesArray)
				{
					const TSharedPtr<FJsonObject>* PropertyObj = nullptr;
					if (!PropertyValue.IsValid() || !PropertyValue->TryGetObject(PropertyObj) || !PropertyObj || !PropertyObj->IsValid())
					{
						continue;
					}
					if ((*PropertyObj)->GetStringField(TEXT("property_name")) == TEXT("TwoSided"))
					{
						bFoundTwoSided = true;
						break;
					}
				}
				TestTrue(TEXT("Material folder exported TwoSided setting"), bFoundTwoSided);
			}
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

	TSharedPtr<FJsonObject> ExportedGraphJson;
	TestTrue(TEXT("Material function folder exported graph parses"), ParseJson(ExportedGraphText, ExportedGraphJson));
	if (ExportedGraphJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Expressions = nullptr;
		TestTrue(TEXT("Material function folder exported graph has expressions"), ExportedGraphJson->TryGetArrayField(TEXT("expressions"), Expressions) && Expressions);
		if (Expressions)
		{
			TestTrue(TEXT("Material function folder exported expression count >= 2"), Expressions->Num() >= 2);
		}
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
	FUeAgentInterfaceSequenceBindingSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.SequenceBindingWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceSequenceBindingSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString ActorLabel = FString::Printf(TEXT("SmokeSeqActor_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString SequenceAssetPath = MakeAutomationAssetPath(TEXT("SeqBind"));

	const FString SpawnActorBody = MakeExecRequestBody(
		TEXT("smoke-seqbinding-spawn-001"),
		TEXT("spawn_actor"),
		FString::Printf(TEXT("{\"class_path\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":256,\"y\":128,\"z\":64}}"), *ActorLabel));
	FHttpSmokeResult SpawnActorResult;
	TestTrue(TEXT("Sequence binding spawn_actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnActorBody, SpawnActorResult));
	TestEqual(TEXT("Sequence binding spawn_actor status code"), SpawnActorResult.StatusCode, 200);

	const FString CreateSequenceBody = MakeExecRequestBody(
		TEXT("smoke-seqbinding-create-seq-001"),
		TEXT("sequence_create_level_sequence"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"start_seconds\":0,\"duration_seconds\":2.0,\"display_rate_num\":30,\"display_rate_den\":1,\"open_editor\":false,\"save_after_create\":false}"), *SequenceAssetPath));
	FHttpSmokeResult CreateSequenceResult;
	TestTrue(TEXT("Sequence binding create_level_sequence request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateSequenceBody, CreateSequenceResult));
	TestEqual(TEXT("Sequence binding create_level_sequence status code"), CreateSequenceResult.StatusCode, 200);

	const FString AddBindingBody = MakeExecRequestBody(
		TEXT("smoke-seqbinding-add-binding-001"),
		TEXT("sequence_add_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *ActorLabel));
	FHttpSmokeResult AddBindingResult;
	TestTrue(TEXT("Sequence binding add_actor_binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddBindingBody, AddBindingResult));
	TestEqual(TEXT("Sequence binding add_actor_binding status code"), AddBindingResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddBindingJson;
	TestTrue(TEXT("Sequence binding add_actor_binding response parses as JSON"), ParseJson(AddBindingResult.Body, AddBindingJson));
	TestTrue(TEXT("Sequence binding add_actor_binding root ok=true"), AddBindingJson.IsValid() && AddBindingJson->GetBoolField(TEXT("ok")));

	FString BindingGuid;
	const TSharedPtr<FJsonObject>* AddBindingData = nullptr;
	TestTrue(TEXT("Sequence binding add_actor_binding contains data object"), AddBindingJson.IsValid() && AddBindingJson->TryGetObjectField(TEXT("data"), AddBindingData) && AddBindingData && AddBindingData->IsValid());
	if (AddBindingData && AddBindingData->IsValid())
	{
		BindingGuid = (*AddBindingData)->GetStringField(TEXT("binding_guid"));
		TestTrue(TEXT("Sequence binding guid non-empty"), !BindingGuid.IsEmpty());
		TestEqual(TEXT("Sequence binding actor label"), (*AddBindingData)->GetStringField(TEXT("actor_label")), ActorLabel);
	}

	const FString AddTransformTrackBody = MakeExecRequestBody(
		TEXT("smoke-seqbinding-add-track-001"),
		TEXT("sequence_add_transform_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddTransformTrackResult;
	TestTrue(TEXT("Sequence binding add_transform_track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddTransformTrackBody, AddTransformTrackResult));
	TestEqual(TEXT("Sequence binding add_transform_track status code"), AddTransformTrackResult.StatusCode, 200);

	const FString AddTransformKeyBody = MakeExecRequestBody(
		TEXT("smoke-seqbinding-add-key-001"),
		TEXT("sequence_add_transform_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"time_seconds\":0.5,\"location\":{\"x\":512,\"y\":128,\"z\":96},\"rotation\":{\"pitch\":0,\"yaw\":45,\"roll\":0},\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddTransformKeyResult;
	TestTrue(TEXT("Sequence binding add_transform_key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddTransformKeyBody, AddTransformKeyResult));
	TestEqual(TEXT("Sequence binding add_transform_key status code"), AddTransformKeyResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddTransformKeyJson;
	TestTrue(TEXT("Sequence binding add_transform_key response parses as JSON"), ParseJson(AddTransformKeyResult.Body, AddTransformKeyJson));
	TestTrue(TEXT("Sequence binding add_transform_key root ok=true"), AddTransformKeyJson.IsValid() && AddTransformKeyJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* AddTransformKeyData = nullptr;
	TestTrue(TEXT("Sequence binding add_transform_key contains data object"), AddTransformKeyJson.IsValid() && AddTransformKeyJson->TryGetObjectField(TEXT("data"), AddTransformKeyData) && AddTransformKeyData && AddTransformKeyData->IsValid());
	if (AddTransformKeyData && AddTransformKeyData->IsValid())
	{
		TestEqual(TEXT("Sequence binding add_transform_key binding guid"), (*AddTransformKeyData)->GetStringField(TEXT("binding_guid")), BindingGuid);
		TestTrue(TEXT("Sequence binding add_transform_key frame number non-negative"), static_cast<int32>((*AddTransformKeyData)->GetIntegerField(TEXT("frame_number"))) >= 0);
	}

	const FString GetInfoAfterAddBody = MakeExecRequestBody(
		TEXT("smoke-seqbinding-info-001"),
		TEXT("sequence_get_level_sequence_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SequenceAssetPath));
	FHttpSmokeResult GetInfoAfterAddResult;
	TestTrue(TEXT("Sequence binding get_info after add request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoAfterAddBody, GetInfoAfterAddResult));
	TestEqual(TEXT("Sequence binding get_info after add status code"), GetInfoAfterAddResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoAfterAddJson;
	TestTrue(TEXT("Sequence binding get_info after add response parses as JSON"), ParseJson(GetInfoAfterAddResult.Body, GetInfoAfterAddJson));
	TestTrue(TEXT("Sequence binding get_info after add root ok=true"), GetInfoAfterAddJson.IsValid() && GetInfoAfterAddJson->GetBoolField(TEXT("ok")));

	bool bFoundBinding = false;
	const TSharedPtr<FJsonObject>* GetInfoAfterAddData = nullptr;
	TestTrue(TEXT("Sequence binding get_info after add contains data object"), GetInfoAfterAddJson.IsValid() && GetInfoAfterAddJson->TryGetObjectField(TEXT("data"), GetInfoAfterAddData) && GetInfoAfterAddData && GetInfoAfterAddData->IsValid());
	if (GetInfoAfterAddData && GetInfoAfterAddData->IsValid())
	{
		TestTrue(TEXT("Sequence binding count after add >= 1"), static_cast<int32>((*GetInfoAfterAddData)->GetIntegerField(TEXT("binding_count"))) >= 1);
		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		TestTrue(TEXT("Sequence binding info contains bindings array"), (*GetInfoAfterAddData)->TryGetArrayField(TEXT("bindings"), Bindings) && Bindings != nullptr);
		if (Bindings)
		{
			for (const TSharedPtr<FJsonValue>& BindingValue : *Bindings)
			{
				const TSharedPtr<FJsonObject>* BindingObject = nullptr;
				if (!BindingValue.IsValid() || !BindingValue->TryGetObject(BindingObject) || !BindingObject || !BindingObject->IsValid())
				{
					continue;
				}

				if ((*BindingObject)->GetStringField(TEXT("guid")) == BindingGuid)
				{
					bFoundBinding = true;
					break;
				}
			}
		}
	}
	TestTrue(TEXT("Sequence binding present after add"), bFoundBinding);

	const FString RemoveBindingBody = MakeExecRequestBody(
		TEXT("smoke-seqbinding-remove-binding-001"),
		TEXT("sequence_remove_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult RemoveBindingResult;
	TestTrue(TEXT("Sequence binding remove_actor_binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveBindingBody, RemoveBindingResult));
	TestEqual(TEXT("Sequence binding remove_actor_binding status code"), RemoveBindingResult.StatusCode, 200);

	const FString GetInfoAfterRemoveBody = MakeExecRequestBody(
		TEXT("smoke-seqbinding-info-002"),
		TEXT("sequence_get_level_sequence_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SequenceAssetPath));
	FHttpSmokeResult GetInfoAfterRemoveResult;
	TestTrue(TEXT("Sequence binding get_info after remove request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoAfterRemoveBody, GetInfoAfterRemoveResult));
	TestEqual(TEXT("Sequence binding get_info after remove status code"), GetInfoAfterRemoveResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoAfterRemoveJson;
	TestTrue(TEXT("Sequence binding get_info after remove response parses as JSON"), ParseJson(GetInfoAfterRemoveResult.Body, GetInfoAfterRemoveJson));
	TestTrue(TEXT("Sequence binding get_info after remove root ok=true"), GetInfoAfterRemoveJson.IsValid() && GetInfoAfterRemoveJson->GetBoolField(TEXT("ok")));

	bool bFoundBindingAfterRemove = false;
	const TSharedPtr<FJsonObject>* GetInfoAfterRemoveData = nullptr;
	TestTrue(TEXT("Sequence binding get_info after remove contains data object"), GetInfoAfterRemoveJson.IsValid() && GetInfoAfterRemoveJson->TryGetObjectField(TEXT("data"), GetInfoAfterRemoveData) && GetInfoAfterRemoveData && GetInfoAfterRemoveData->IsValid());
	if (GetInfoAfterRemoveData && GetInfoAfterRemoveData->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		TestTrue(TEXT("Sequence binding info after remove contains bindings array"), (*GetInfoAfterRemoveData)->TryGetArrayField(TEXT("bindings"), Bindings) && Bindings != nullptr);
		if (Bindings)
		{
			for (const TSharedPtr<FJsonValue>& BindingValue : *Bindings)
			{
				const TSharedPtr<FJsonObject>* BindingObject = nullptr;
				if (!BindingValue.IsValid() || !BindingValue->TryGetObject(BindingObject) || !BindingObject || !BindingObject->IsValid())
				{
					continue;
				}

				if ((*BindingObject)->GetStringField(TEXT("guid")) == BindingGuid)
				{
					bFoundBindingAfterRemove = true;
					break;
				}
			}
		}
	}
	TestFalse(TEXT("Sequence binding removed from sequence"), bFoundBindingAfterRemove);

	const FString DestroyActorBody = MakeExecRequestBody(
		TEXT("smoke-seqbinding-destroy-actor-001"),
		TEXT("destroy_actor"),
		FString::Printf(TEXT("{\"id\":\"%s\"}"), *ActorLabel));
	FHttpSmokeResult DestroyActorResult;
	TestTrue(TEXT("Sequence binding destroy source actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DestroyActorBody, DestroyActorResult));
	TestEqual(TEXT("Sequence binding destroy source actor status code"), DestroyActorResult.StatusCode, 200);

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceSequenceSkeletalAnimationSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.SequenceSkeletalAnimationWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceSequencePropertyTrackSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.SequencePropertyTrackWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceSequenceGenericPropertyTrackSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.SequenceGenericPropertyTrackWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceSequencePropertyTrackSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString ActorLabel = FString::Printf(TEXT("SmokeSeqPropActor_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString SequenceAssetPath = MakeAutomationAssetPath(TEXT("SeqProp"));
	const FString BoolPropertyName = TEXT("bActorEnableCollision");
	const FString FloatPropertyName = TEXT("CustomTimeDilation");
	const FString IntegerPropertyName = TEXT("CustomDepthStencilValue");
	const FString IntegerPropertyPath = TEXT("StaticMeshComponent.CustomDepthStencilValue");

	auto FindTrackByClassNeedle = [](const TSharedPtr<FJsonObject>* InfoData, const FString& BindingGuid, const FString& ClassNeedle, TSharedPtr<FJsonObject>& OutTrackObject) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("bindings"), Bindings) || !Bindings)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& BindingValue : *Bindings)
		{
			const TSharedPtr<FJsonObject>* BindingObject = nullptr;
			if (!BindingValue.IsValid() || !BindingValue->TryGetObject(BindingObject) || !BindingObject || !BindingObject->IsValid())
			{
				continue;
			}

			if ((*BindingObject)->GetStringField(TEXT("guid")) != BindingGuid)
			{
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
			if (!(*BindingObject)->TryGetArrayField(TEXT("tracks"), Tracks) || !Tracks)
			{
				return false;
			}

			for (const TSharedPtr<FJsonValue>& TrackValue : *Tracks)
			{
				const TSharedPtr<FJsonObject>* TrackObject = nullptr;
				if (!TrackValue.IsValid() || !TrackValue->TryGetObject(TrackObject) || !TrackObject || !TrackObject->IsValid())
				{
					continue;
				}

				if ((*TrackObject)->GetStringField(TEXT("class")).Contains(ClassNeedle))
				{
					OutTrackObject = *TrackObject;
					return true;
				}
			}

			return false;
		}

		return false;
	};

	const FString SpawnActorBody = MakeExecRequestBody(
		TEXT("smoke-seqprop-spawn-001"),
		TEXT("spawn_actor"),
		FString::Printf(TEXT("{\"class_path\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":640,\"y\":96,\"z\":64}}"), *ActorLabel));
	FHttpSmokeResult SpawnActorResult;
	TestTrue(TEXT("Sequence property spawn_actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnActorBody, SpawnActorResult));
	TestEqual(TEXT("Sequence property spawn_actor status code"), SpawnActorResult.StatusCode, 200);

	const FString CreateSequenceBody = MakeExecRequestBody(
		TEXT("smoke-seqprop-create-seq-001"),
		TEXT("sequence_create_level_sequence"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"start_seconds\":0,\"duration_seconds\":2.0,\"display_rate_num\":30,\"display_rate_den\":1,\"open_editor\":false,\"save_after_create\":false}"), *SequenceAssetPath));
	FHttpSmokeResult CreateSequenceResult;
	TestTrue(TEXT("Sequence property create_level_sequence request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateSequenceBody, CreateSequenceResult));
	TestEqual(TEXT("Sequence property create_level_sequence status code"), CreateSequenceResult.StatusCode, 200);

	const FString AddBindingBody = MakeExecRequestBody(
		TEXT("smoke-seqprop-add-binding-001"),
		TEXT("sequence_add_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *ActorLabel));
	FHttpSmokeResult AddBindingResult;
	TestTrue(TEXT("Sequence property add_actor_binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddBindingBody, AddBindingResult));
	TestEqual(TEXT("Sequence property add_actor_binding status code"), AddBindingResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddBindingJson;
	TestTrue(TEXT("Sequence property add_actor_binding parses JSON"), ParseJson(AddBindingResult.Body, AddBindingJson));
	FString BindingGuid;
	const TSharedPtr<FJsonObject>* AddBindingData = nullptr;
	TestTrue(TEXT("Sequence property add_actor_binding contains data"), AddBindingJson.IsValid() && AddBindingJson->TryGetObjectField(TEXT("data"), AddBindingData) && AddBindingData && AddBindingData->IsValid());
	if (AddBindingData && AddBindingData->IsValid())
	{
		BindingGuid = (*AddBindingData)->GetStringField(TEXT("binding_guid"));
	}
	TestTrue(TEXT("Sequence property binding guid non-empty"), !BindingGuid.IsEmpty());

	const FString AddBoolTrackBody = MakeExecRequestBody(
		TEXT("smoke-seqprop-add-bool-track-001"),
		TEXT("sequence_add_bool_property_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_name\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid, *BoolPropertyName));
	FHttpSmokeResult AddBoolTrackResult;
	TestTrue(TEXT("Sequence property add_bool_property_track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddBoolTrackBody, AddBoolTrackResult));
	TestEqual(TEXT("Sequence property add_bool_property_track status code"), AddBoolTrackResult.StatusCode, 200);

	const FString AddBoolKeyBody = MakeExecRequestBody(
		TEXT("smoke-seqprop-add-bool-key-001"),
		TEXT("sequence_add_bool_property_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_name\":\"%s\",\"time_seconds\":0.25,\"value\":false,\"value_before_key\":true,\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid, *BoolPropertyName));
	FHttpSmokeResult AddBoolKeyResult;
	TestTrue(TEXT("Sequence property add_bool_property_key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddBoolKeyBody, AddBoolKeyResult));
	TestEqual(TEXT("Sequence property add_bool_property_key status code"), AddBoolKeyResult.StatusCode, 200);

	const FString AddFloatTrackBody = MakeExecRequestBody(
		TEXT("smoke-seqprop-add-float-track-001"),
		TEXT("sequence_add_float_property_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_name\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid, *FloatPropertyName));
	FHttpSmokeResult AddFloatTrackResult;
	TestTrue(TEXT("Sequence property add_float_property_track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddFloatTrackBody, AddFloatTrackResult));
	TestEqual(TEXT("Sequence property add_float_property_track status code"), AddFloatTrackResult.StatusCode, 200);

	const FString AddFloatKeyBody = MakeExecRequestBody(
		TEXT("smoke-seqprop-add-float-key-001"),
		TEXT("sequence_add_float_property_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_name\":\"%s\",\"time_seconds\":0.5,\"value\":2.0,\"value_before_key\":1.0,\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid, *FloatPropertyName));
	FHttpSmokeResult AddFloatKeyResult;
	TestTrue(TEXT("Sequence property add_float_property_key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddFloatKeyBody, AddFloatKeyResult));
	TestEqual(TEXT("Sequence property add_float_property_key status code"), AddFloatKeyResult.StatusCode, 200);

	const FString GetInfoBody = MakeExecRequestBody(
		TEXT("smoke-seqprop-info-001"),
		TEXT("sequence_get_level_sequence_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SequenceAssetPath));
	FHttpSmokeResult GetInfoResult;
	TestTrue(TEXT("Sequence property get_level_sequence_info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoBody, GetInfoResult));
	TestEqual(TEXT("Sequence property get_level_sequence_info status code"), GetInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoJson;
	TestTrue(TEXT("Sequence property get_level_sequence_info parses JSON"), ParseJson(GetInfoResult.Body, GetInfoJson));
	const TSharedPtr<FJsonObject>* GetInfoData = nullptr;
	TestTrue(TEXT("Sequence property get_level_sequence_info contains data"), GetInfoJson.IsValid() && GetInfoJson->TryGetObjectField(TEXT("data"), GetInfoData) && GetInfoData && GetInfoData->IsValid());
	if (GetInfoData && GetInfoData->IsValid())
	{
		TSharedPtr<FJsonObject> BoolTrackObject;
		TestTrue(TEXT("Sequence bool property track summary exists"), FindTrackByClassNeedle(GetInfoData, BindingGuid, TEXT("MovieSceneBoolTrack"), BoolTrackObject));
		if (BoolTrackObject.IsValid())
		{
			TestEqual(TEXT("Sequence bool property_name matches"), BoolTrackObject->GetStringField(TEXT("property_name")), BoolPropertyName);
			TestEqual(TEXT("Sequence bool property_path matches"), BoolTrackObject->GetStringField(TEXT("property_path")), BoolPropertyName);

			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			TestTrue(TEXT("Sequence bool track contains sections"), BoolTrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() > 0);
			if (Sections && Sections->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* SectionObject = nullptr;
				TestTrue(TEXT("Sequence bool section parses"), (*Sections)[0].IsValid() && (*Sections)[0]->TryGetObject(SectionObject) && SectionObject && SectionObject->IsValid());
				if (SectionObject && SectionObject->IsValid())
				{
					TestEqual(TEXT("Sequence bool section type"), (*SectionObject)->GetStringField(TEXT("section_type")), FString(TEXT("bool")));
					TestTrue(TEXT("Sequence bool section has keys"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("key_count"))) >= 1);
				}
			}
		}

		TSharedPtr<FJsonObject> FloatTrackObject;
		TestTrue(TEXT("Sequence float property track summary exists"), FindTrackByClassNeedle(GetInfoData, BindingGuid, TEXT("MovieSceneFloatTrack"), FloatTrackObject));
		if (FloatTrackObject.IsValid())
		{
			TestEqual(TEXT("Sequence float property_name matches"), FloatTrackObject->GetStringField(TEXT("property_name")), FloatPropertyName);
			TestEqual(TEXT("Sequence float property_path matches"), FloatTrackObject->GetStringField(TEXT("property_path")), FloatPropertyName);

			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			TestTrue(TEXT("Sequence float track contains sections"), FloatTrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() > 0);
			if (Sections && Sections->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* SectionObject = nullptr;
				TestTrue(TEXT("Sequence float section parses"), (*Sections)[0].IsValid() && (*Sections)[0]->TryGetObject(SectionObject) && SectionObject && SectionObject->IsValid());
				if (SectionObject && SectionObject->IsValid())
				{
					TestEqual(TEXT("Sequence float section type"), (*SectionObject)->GetStringField(TEXT("section_type")), FString(TEXT("float")));
					TestTrue(TEXT("Sequence float section has keys"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("key_count"))) >= 1);
				}
			}
		}

		const FString AddIntegerTrackBody = MakeExecRequestBody(
			TEXT("smoke-seqprop-add-int-track-001"),
			TEXT("sequence_add_integer_property_track"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_name\":\"%s\",\"property_path\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid, *IntegerPropertyName, *IntegerPropertyPath));
		FHttpSmokeResult AddIntegerTrackResult;
		TestTrue(TEXT("Sequence property add_integer_property_track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddIntegerTrackBody, AddIntegerTrackResult));
		TestEqual(TEXT("Sequence property add_integer_property_track status code"), AddIntegerTrackResult.StatusCode, 200);

		const FString AddIntegerKeyBody = MakeExecRequestBody(
			TEXT("smoke-seqprop-add-int-key-001"),
			TEXT("sequence_add_integer_property_key"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_name\":\"%s\",\"property_path\":\"%s\",\"time_seconds\":0.75,\"value\":7,\"value_before_key\":0,\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid, *IntegerPropertyName, *IntegerPropertyPath));
		FHttpSmokeResult AddIntegerKeyResult;
		TestTrue(TEXT("Sequence property add_integer_property_key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddIntegerKeyBody, AddIntegerKeyResult));
		TestEqual(TEXT("Sequence property add_integer_property_key status code"), AddIntegerKeyResult.StatusCode, 200);

		const FString GetInfoAfterIntegerBody = MakeExecRequestBody(
			TEXT("smoke-seqprop-info-002"),
			TEXT("sequence_get_level_sequence_info"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SequenceAssetPath));
		FHttpSmokeResult GetInfoAfterIntegerResult;
		TestTrue(TEXT("Sequence property get_level_sequence_info after integer request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoAfterIntegerBody, GetInfoAfterIntegerResult));
		TestEqual(TEXT("Sequence property get_level_sequence_info after integer status code"), GetInfoAfterIntegerResult.StatusCode, 200);

		TSharedPtr<FJsonObject> GetInfoAfterIntegerJson;
		TestTrue(TEXT("Sequence property get_level_sequence_info after integer parses JSON"), ParseJson(GetInfoAfterIntegerResult.Body, GetInfoAfterIntegerJson));
		const TSharedPtr<FJsonObject>* GetInfoAfterIntegerData = nullptr;
		TestTrue(TEXT("Sequence property get_level_sequence_info after integer contains data"), GetInfoAfterIntegerJson.IsValid() && GetInfoAfterIntegerJson->TryGetObjectField(TEXT("data"), GetInfoAfterIntegerData) && GetInfoAfterIntegerData && GetInfoAfterIntegerData->IsValid());
		if (GetInfoAfterIntegerData && GetInfoAfterIntegerData->IsValid())
		{
			TSharedPtr<FJsonObject> IntegerTrackObject;
			TestTrue(TEXT("Sequence integer property track summary exists"), FindTrackByClassNeedle(GetInfoAfterIntegerData, BindingGuid, TEXT("MovieSceneIntegerTrack"), IntegerTrackObject));
			if (IntegerTrackObject.IsValid())
			{
				TestEqual(TEXT("Sequence integer property_name matches"), IntegerTrackObject->GetStringField(TEXT("property_name")), IntegerPropertyName);
				TestEqual(TEXT("Sequence integer property_path matches"), IntegerTrackObject->GetStringField(TEXT("property_path")), IntegerPropertyPath);

				const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
				TestTrue(TEXT("Sequence integer track contains sections"), IntegerTrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() > 0);
				if (Sections && Sections->Num() > 0)
				{
					const TSharedPtr<FJsonObject>* SectionObject = nullptr;
					TestTrue(TEXT("Sequence integer section parses"), (*Sections)[0].IsValid() && (*Sections)[0]->TryGetObject(SectionObject) && SectionObject && SectionObject->IsValid());
					if (SectionObject && SectionObject->IsValid())
					{
						TestEqual(TEXT("Sequence integer section type"), (*SectionObject)->GetStringField(TEXT("section_type")), FString(TEXT("integer")));
						TestTrue(TEXT("Sequence integer section has keys"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("key_count"))) >= 1);
					}
				}
			}
		}
	}

	return !HasAnyErrors();
}

bool FUeAgentInterfaceSequenceGenericPropertyTrackSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString LightActorLabel = FString::Printf(TEXT("SmokeSeqGenLight_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString MeshActorLabel = FString::Printf(TEXT("SmokeSeqGenMesh_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString StateActorLabel = FString::Printf(TEXT("SmokeSeqGenState_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString TerminalActorLabel = FString::Printf(TEXT("SmokeSeqGenTerminal_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString DocActorLabel = FString::Printf(TEXT("SmokeSeqGenDoc_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString CharacterActorLabel = FString::Printf(TEXT("SmokeSeqGenChar_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString BonfireActorLabel = FString::Printf(TEXT("SmokeSeqGenBonfire_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString DoorActorLabel = FString::Printf(TEXT("SmokeSeqGenDoor_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString SequenceAssetPath = MakeAutomationAssetPath(TEXT("SeqPropGen"));
	const FString FolderRoot = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("LevelSequence")));
	const FString FolderPath = FPaths::Combine(FolderRoot, SequenceAssetPath.RightChop(1));

	auto FindTrackByClassNeedle = [](const TSharedPtr<FJsonObject>* InfoData, const FString& BindingGuid, const FString& ClassNeedle, TSharedPtr<FJsonObject>& OutTrackObject) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("bindings"), Bindings) || !Bindings)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& BindingValue : *Bindings)
		{
			const TSharedPtr<FJsonObject>* BindingObject = nullptr;
			if (!BindingValue.IsValid() || !BindingValue->TryGetObject(BindingObject) || !BindingObject || !BindingObject->IsValid())
			{
				continue;
			}

			if ((*BindingObject)->GetStringField(TEXT("guid")) != BindingGuid)
			{
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
			if (!(*BindingObject)->TryGetArrayField(TEXT("tracks"), Tracks) || !Tracks)
			{
				return false;
			}

			for (const TSharedPtr<FJsonValue>& TrackValue : *Tracks)
			{
				const TSharedPtr<FJsonObject>* TrackObject = nullptr;
				if (!TrackValue.IsValid() || !TrackValue->TryGetObject(TrackObject) || !TrackObject || !TrackObject->IsValid())
				{
					continue;
				}

				if ((*TrackObject)->GetStringField(TEXT("class")).Contains(ClassNeedle))
				{
					OutTrackObject = *TrackObject;
					return true;
				}
			}

			return false;
		}

		return false;
	};

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

	auto FindBindingFolderName = [this](const TSharedPtr<FJsonObject>& BindingsIndexJson, const FString& TargetBindingGuid, FString& OutFolderName) -> bool
	{
		OutFolderName.Reset();
		if (!BindingsIndexJson.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		if (!BindingsIndexJson->TryGetArrayField(TEXT("bindings"), Bindings) || !Bindings)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& BindingValue : *Bindings)
		{
			const TSharedPtr<FJsonObject>* BindingObj = nullptr;
			if (!BindingValue.IsValid() || !BindingValue->TryGetObject(BindingObj) || !BindingObj || !(*BindingObj).IsValid())
			{
				continue;
			}
			if ((*BindingObj)->GetStringField(TEXT("binding_guid")) == TargetBindingGuid)
			{
				OutFolderName = (*BindingObj)->GetStringField(TEXT("folder_name"));
				return !OutFolderName.IsEmpty();
			}
		}
		return false;
	};

	auto FindTrackFileInDir = [this, &LoadJsonFile](const FString& TracksDir, auto Predicate, FString& OutTrackFileName, TSharedPtr<FJsonObject>& OutTrackJson) -> bool
	{
		OutTrackFileName.Reset();
		OutTrackJson.Reset();
		TArray<FString> TrackFiles;
		IFileManager::Get().FindFiles(TrackFiles, *(FPaths::Combine(TracksDir, TEXT("*.json"))), true, false);
		TrackFiles.Sort();
		for (const FString& TrackFile : TrackFiles)
		{
			TSharedPtr<FJsonObject> TrackJson;
			if (!LoadJsonFile(FPaths::Combine(TracksDir, TrackFile), TrackJson) || !TrackJson.IsValid())
			{
				continue;
			}
			if (Predicate(TrackJson))
			{
				OutTrackFileName = TrackFile;
				OutTrackJson = TrackJson;
				return true;
			}
		}
		return false;
	};

	const FString SpawnLightBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-spawn-light-001"),
		TEXT("spawn_actor"),
		FString::Printf(TEXT("{\"class_path\":\"/Script/Engine.PointLight\",\"label\":\"%s\",\"location\":{\"x\":980,\"y\":96,\"z\":160}}"), *LightActorLabel));
	FHttpSmokeResult SpawnLightResult;
	TestTrue(TEXT("Sequence generic spawn light request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnLightBody, SpawnLightResult));
	TestEqual(TEXT("Sequence generic spawn light status code"), SpawnLightResult.StatusCode, 200);

	const FString SpawnMeshActorBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-spawn-mesh-001"),
		TEXT("spawn_actor"),
		FString::Printf(TEXT("{\"class_path\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":1040,\"y\":96,\"z\":96}}"), *MeshActorLabel));
	FHttpSmokeResult SpawnMeshActorResult;
	TestTrue(TEXT("Sequence generic spawn mesh actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnMeshActorBody, SpawnMeshActorResult));
	TestEqual(TEXT("Sequence generic spawn mesh actor status code"), SpawnMeshActorResult.StatusCode, 200);

	const FString SpawnStateActorBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-spawn-state-001"),
		TEXT("spawn_actor"),
		FString::Printf(TEXT("{\"class_path\":\"/Script/Engine.GameStateBase\",\"label\":\"%s\"}"), *StateActorLabel));
	FHttpSmokeResult SpawnStateActorResult;
	TestTrue(TEXT("Sequence generic spawn state actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnStateActorBody, SpawnStateActorResult));
	TestEqual(TEXT("Sequence generic spawn state actor status code"), SpawnStateActorResult.StatusCode, 200);

	const FString SpawnTerminalActorBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-spawn-terminal-001"),
		TEXT("spawn_actor"),
		FString::Printf(TEXT("{\"class_path\":\"/Script/GptProjectTest.FacilityControlTerminalActor\",\"label\":\"%s\",\"location\":{\"x\":1180,\"y\":96,\"z\":64}}"), *TerminalActorLabel));
	FHttpSmokeResult SpawnTerminalActorResult;
	TestTrue(TEXT("Sequence generic spawn terminal actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnTerminalActorBody, SpawnTerminalActorResult));
	TestEqual(TEXT("Sequence generic spawn terminal actor status code"), SpawnTerminalActorResult.StatusCode, 200);

	const FString SpawnDocActorBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-spawn-doc-001"),
		TEXT("spawn_actor"),
		FString::Printf(TEXT("{\"class_path\":\"/Script/Engine.DocumentationActor\",\"label\":\"%s\",\"location\":{\"x\":1300,\"y\":96,\"z\":64}}"), *DocActorLabel));
	FHttpSmokeResult SpawnDocActorResult;
	TestTrue(TEXT("Sequence generic spawn documentation actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnDocActorBody, SpawnDocActorResult));
	TestEqual(TEXT("Sequence generic spawn documentation actor status code"), SpawnDocActorResult.StatusCode, 200);

	const FString SpawnCharacterActorBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-spawn-char-001"),
		TEXT("spawn_actor"),
		FString::Printf(TEXT("{\"class_path\":\"/Script/GptProjectTest.GptProjectTestCharacter\",\"label\":\"%s\",\"location\":{\"x\":1400,\"y\":96,\"z\":120}}"), *CharacterActorLabel));
	FHttpSmokeResult SpawnCharacterActorResult;
	TestTrue(TEXT("Sequence generic spawn character actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnCharacterActorBody, SpawnCharacterActorResult));
	TestEqual(TEXT("Sequence generic spawn character actor status code"), SpawnCharacterActorResult.StatusCode, 200);

	const FString SpawnBonfireActorBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-spawn-bonfire-001"),
		TEXT("spawn_actor"),
		FString::Printf(TEXT("{\"class_path\":\"/Script/GptProjectTest.FacilityBonfireActor\",\"label\":\"%s\",\"location\":{\"x\":1500,\"y\":96,\"z\":64}}"), *BonfireActorLabel));
	FHttpSmokeResult SpawnBonfireActorResult;
	TestTrue(TEXT("Sequence generic spawn bonfire actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnBonfireActorBody, SpawnBonfireActorResult));
	TestEqual(TEXT("Sequence generic spawn bonfire actor status code"), SpawnBonfireActorResult.StatusCode, 200);

	const FString SpawnDoorActorBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-spawn-door-001"),
		TEXT("spawn_actor"),
		FString::Printf(TEXT("{\"class_path\":\"/Script/GptProjectTest.FacilityDoorActor\",\"label\":\"%s\",\"location\":{\"x\":1600,\"y\":96,\"z\":64}}"), *DoorActorLabel));
	FHttpSmokeResult SpawnDoorActorResult;
	TestTrue(TEXT("Sequence generic spawn door actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnDoorActorBody, SpawnDoorActorResult));
	TestEqual(TEXT("Sequence generic spawn door actor status code"), SpawnDoorActorResult.StatusCode, 200);

	const FString CreateSequenceBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-create-seq-001"),
		TEXT("sequence_create_level_sequence"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"start_seconds\":0,\"duration_seconds\":2.0,\"display_rate_num\":30,\"display_rate_den\":1,\"open_editor\":false,\"save_after_create\":false}"), *SequenceAssetPath));
	FHttpSmokeResult CreateSequenceResult;
	TestTrue(TEXT("Sequence generic create_level_sequence request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateSequenceBody, CreateSequenceResult));
	TestEqual(TEXT("Sequence generic create_level_sequence status code"), CreateSequenceResult.StatusCode, 200);

	const FString AddBindingBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-binding-001"),
		TEXT("sequence_add_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *LightActorLabel));
	FHttpSmokeResult AddBindingResult;
	TestTrue(TEXT("Sequence generic add_actor_binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddBindingBody, AddBindingResult));
	TestEqual(TEXT("Sequence generic add_actor_binding status code"), AddBindingResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddBindingJson;
	TestTrue(TEXT("Sequence generic add_actor_binding parses JSON"), ParseJson(AddBindingResult.Body, AddBindingJson));
	FString BindingGuid;
	const TSharedPtr<FJsonObject>* AddBindingData = nullptr;
	TestTrue(TEXT("Sequence generic add_actor_binding contains data"), AddBindingJson.IsValid() && AddBindingJson->TryGetObjectField(TEXT("data"), AddBindingData) && AddBindingData && AddBindingData->IsValid());
	if (AddBindingData && AddBindingData->IsValid())
	{
		BindingGuid = (*AddBindingData)->GetStringField(TEXT("binding_guid"));
	}
	TestTrue(TEXT("Sequence generic binding guid non-empty"), !BindingGuid.IsEmpty());

	const FString AddMeshBindingBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-mesh-binding-001"),
		TEXT("sequence_add_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *MeshActorLabel));
	FHttpSmokeResult AddMeshBindingResult;
	TestTrue(TEXT("Sequence generic add mesh actor binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddMeshBindingBody, AddMeshBindingResult));
	TestEqual(TEXT("Sequence generic add mesh actor binding status code"), AddMeshBindingResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddMeshBindingJson;
	TestTrue(TEXT("Sequence generic add mesh actor binding parses JSON"), ParseJson(AddMeshBindingResult.Body, AddMeshBindingJson));
	FString MeshBindingGuid;
	const TSharedPtr<FJsonObject>* AddMeshBindingData = nullptr;
	TestTrue(TEXT("Sequence generic add mesh actor binding contains data"), AddMeshBindingJson.IsValid() && AddMeshBindingJson->TryGetObjectField(TEXT("data"), AddMeshBindingData) && AddMeshBindingData && AddMeshBindingData->IsValid());
	if (AddMeshBindingData && AddMeshBindingData->IsValid())
	{
		MeshBindingGuid = (*AddMeshBindingData)->GetStringField(TEXT("binding_guid"));
	}
	TestTrue(TEXT("Sequence generic mesh binding guid non-empty"), !MeshBindingGuid.IsEmpty());

	const FString AddStateBindingBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-state-binding-001"),
		TEXT("sequence_add_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *StateActorLabel));
	FHttpSmokeResult AddStateBindingResult;
	TestTrue(TEXT("Sequence generic add state actor binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddStateBindingBody, AddStateBindingResult));
	TestEqual(TEXT("Sequence generic add state actor binding status code"), AddStateBindingResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddStateBindingJson;
	TestTrue(TEXT("Sequence generic add state actor binding parses JSON"), ParseJson(AddStateBindingResult.Body, AddStateBindingJson));
	FString StateBindingGuid;
	const TSharedPtr<FJsonObject>* AddStateBindingData = nullptr;
	TestTrue(TEXT("Sequence generic add state actor binding contains data"), AddStateBindingJson.IsValid() && AddStateBindingJson->TryGetObjectField(TEXT("data"), AddStateBindingData) && AddStateBindingData && AddStateBindingData->IsValid());
	if (AddStateBindingData && AddStateBindingData->IsValid())
	{
		StateBindingGuid = (*AddStateBindingData)->GetStringField(TEXT("binding_guid"));
	}
	TestTrue(TEXT("Sequence generic state binding guid non-empty"), !StateBindingGuid.IsEmpty());

	const FString AddTerminalBindingBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-terminal-binding-001"),
		TEXT("sequence_add_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *TerminalActorLabel));
	FHttpSmokeResult AddTerminalBindingResult;
	TestTrue(TEXT("Sequence generic add terminal actor binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddTerminalBindingBody, AddTerminalBindingResult));
	TestEqual(TEXT("Sequence generic add terminal actor binding status code"), AddTerminalBindingResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddTerminalBindingJson;
	TestTrue(TEXT("Sequence generic add terminal actor binding parses JSON"), ParseJson(AddTerminalBindingResult.Body, AddTerminalBindingJson));
	FString TerminalBindingGuid;
	const TSharedPtr<FJsonObject>* AddTerminalBindingData = nullptr;
	TestTrue(TEXT("Sequence generic add terminal actor binding contains data"), AddTerminalBindingJson.IsValid() && AddTerminalBindingJson->TryGetObjectField(TEXT("data"), AddTerminalBindingData) && AddTerminalBindingData && AddTerminalBindingData->IsValid());
	if (AddTerminalBindingData && AddTerminalBindingData->IsValid())
	{
		TerminalBindingGuid = (*AddTerminalBindingData)->GetStringField(TEXT("binding_guid"));
	}
	TestTrue(TEXT("Sequence generic terminal binding guid non-empty"), !TerminalBindingGuid.IsEmpty());

	const FString AddDocBindingBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-doc-binding-001"),
		TEXT("sequence_add_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *DocActorLabel));
	FHttpSmokeResult AddDocBindingResult;
	TestTrue(TEXT("Sequence generic add documentation actor binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddDocBindingBody, AddDocBindingResult));
	TestEqual(TEXT("Sequence generic add documentation actor binding status code"), AddDocBindingResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddDocBindingJson;
	TestTrue(TEXT("Sequence generic add documentation actor binding parses JSON"), ParseJson(AddDocBindingResult.Body, AddDocBindingJson));
	FString DocBindingGuid;
	const TSharedPtr<FJsonObject>* AddDocBindingData = nullptr;
	TestTrue(TEXT("Sequence generic add documentation actor binding contains data"), AddDocBindingJson.IsValid() && AddDocBindingJson->TryGetObjectField(TEXT("data"), AddDocBindingData) && AddDocBindingData && AddDocBindingData->IsValid());
	if (AddDocBindingData && AddDocBindingData->IsValid())
	{
		DocBindingGuid = (*AddDocBindingData)->GetStringField(TEXT("binding_guid"));
	}
	TestTrue(TEXT("Sequence generic documentation binding guid non-empty"), !DocBindingGuid.IsEmpty());

	const FString AddCharacterBindingBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-char-binding-001"),
		TEXT("sequence_add_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *CharacterActorLabel));
	FHttpSmokeResult AddCharacterBindingResult;
	TestTrue(TEXT("Sequence generic add character actor binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddCharacterBindingBody, AddCharacterBindingResult));
	TestEqual(TEXT("Sequence generic add character actor binding status code"), AddCharacterBindingResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddCharacterBindingJson;
	TestTrue(TEXT("Sequence generic add character actor binding parses JSON"), ParseJson(AddCharacterBindingResult.Body, AddCharacterBindingJson));
	FString CharacterBindingGuid;
	const TSharedPtr<FJsonObject>* AddCharacterBindingData = nullptr;
	TestTrue(TEXT("Sequence generic add character actor binding contains data"), AddCharacterBindingJson.IsValid() && AddCharacterBindingJson->TryGetObjectField(TEXT("data"), AddCharacterBindingData) && AddCharacterBindingData && AddCharacterBindingData->IsValid());
	if (AddCharacterBindingData && AddCharacterBindingData->IsValid())
	{
		CharacterBindingGuid = (*AddCharacterBindingData)->GetStringField(TEXT("binding_guid"));
	}
	TestTrue(TEXT("Sequence generic character binding guid non-empty"), !CharacterBindingGuid.IsEmpty());

	const FString AddBonfireBindingBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-bonfire-binding-001"),
		TEXT("sequence_add_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *BonfireActorLabel));
	FHttpSmokeResult AddBonfireBindingResult;
	TestTrue(TEXT("Sequence generic add bonfire actor binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddBonfireBindingBody, AddBonfireBindingResult));
	TestEqual(TEXT("Sequence generic add bonfire actor binding status code"), AddBonfireBindingResult.StatusCode, 200);
	TSharedPtr<FJsonObject> AddBonfireBindingJson;
	TestTrue(TEXT("Sequence generic add bonfire actor binding parses JSON"), ParseJson(AddBonfireBindingResult.Body, AddBonfireBindingJson));
	FString BonfireBindingGuid;
	const TSharedPtr<FJsonObject>* AddBonfireBindingData = nullptr;
	TestTrue(TEXT("Sequence generic add bonfire actor binding contains data"), AddBonfireBindingJson.IsValid() && AddBonfireBindingJson->TryGetObjectField(TEXT("data"), AddBonfireBindingData) && AddBonfireBindingData && AddBonfireBindingData->IsValid());
	if (AddBonfireBindingData && AddBonfireBindingData->IsValid())
	{
		BonfireBindingGuid = (*AddBonfireBindingData)->GetStringField(TEXT("binding_guid"));
	}
	TestTrue(TEXT("Sequence generic bonfire binding guid non-empty"), !BonfireBindingGuid.IsEmpty());

	const FString AddDoorBindingBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-door-binding-001"),
		TEXT("sequence_add_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *DoorActorLabel));
	FHttpSmokeResult AddDoorBindingResult;
	TestTrue(TEXT("Sequence generic add door actor binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddDoorBindingBody, AddDoorBindingResult));
	TestEqual(TEXT("Sequence generic add door actor binding status code"), AddDoorBindingResult.StatusCode, 200);
	TSharedPtr<FJsonObject> AddDoorBindingJson;
	TestTrue(TEXT("Sequence generic add door actor binding parses JSON"), ParseJson(AddDoorBindingResult.Body, AddDoorBindingJson));
	FString DoorBindingGuid;
	const TSharedPtr<FJsonObject>* AddDoorBindingData = nullptr;
	TestTrue(TEXT("Sequence generic add door actor binding contains data"), AddDoorBindingJson.IsValid() && AddDoorBindingJson->TryGetObjectField(TEXT("data"), AddDoorBindingData) && AddDoorBindingData && AddDoorBindingData->IsValid());
	if (AddDoorBindingData && AddDoorBindingData->IsValid())
	{
		DoorBindingGuid = (*AddDoorBindingData)->GetStringField(TEXT("binding_guid"));
	}
	TestTrue(TEXT("Sequence generic door binding guid non-empty"), !DoorBindingGuid.IsEmpty());

	const FString AddBoolTrackBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-bool-track-001"),
		TEXT("sequence_add_property_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"bool\",\"property_name\":\"bActorEnableCollision\",\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddBoolTrackResult;
	TestTrue(TEXT("Sequence generic add_property_track bool request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddBoolTrackBody, AddBoolTrackResult));
	TestEqual(TEXT("Sequence generic add_property_track bool status code"), AddBoolTrackResult.StatusCode, 200);

	const FString AddBoolKeyBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-bool-key-001"),
		TEXT("sequence_add_property_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"bool\",\"property_name\":\"bActorEnableCollision\",\"time_seconds\":0.25,\"value\":false,\"value_before_key\":true,\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddBoolKeyResult;
	TestTrue(TEXT("Sequence generic add_property_key bool request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddBoolKeyBody, AddBoolKeyResult));
	TestEqual(TEXT("Sequence generic add_property_key bool status code"), AddBoolKeyResult.StatusCode, 200);

	const FString AddColorTrackBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-color-track-001"),
		TEXT("sequence_add_property_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"color\",\"property_name\":\"LightColor\",\"property_path\":\"LightComponent.LightColor\",\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddColorTrackResult;
	TestTrue(TEXT("Sequence generic add_property_track color request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddColorTrackBody, AddColorTrackResult));
	TestEqual(TEXT("Sequence generic add_property_track color status code"), AddColorTrackResult.StatusCode, 200);

	const FString AddColorKeyBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-color-key-001"),
		TEXT("sequence_add_property_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"color\",\"property_name\":\"LightColor\",\"property_path\":\"LightComponent.LightColor\",\"time_seconds\":0.5,\"red\":0.2,\"green\":0.6,\"blue\":1.0,\"alpha\":1.0,\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddColorKeyResult;
	TestTrue(TEXT("Sequence generic add_property_key color request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddColorKeyBody, AddColorKeyResult));
	TestEqual(TEXT("Sequence generic add_property_key color status code"), AddColorKeyResult.StatusCode, 200);

	const FString AddObjectTrackBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-object-track-001"),
		TEXT("sequence_add_property_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"object\",\"property_name\":\"StaticMesh\",\"property_path\":\"StaticMeshComponent.StaticMesh\",\"property_class_path\":\"/Script/Engine.StaticMesh\",\"save_after_set\":false}"), *SequenceAssetPath, *MeshBindingGuid));
	FHttpSmokeResult AddObjectTrackResult;
	TestTrue(TEXT("Sequence generic add_property_track object request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddObjectTrackBody, AddObjectTrackResult));
	TestEqual(TEXT("Sequence generic add_property_track object status code"), AddObjectTrackResult.StatusCode, 200);

	const FString AddObjectKeyBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-object-key-001"),
		TEXT("sequence_add_property_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"object\",\"property_name\":\"StaticMesh\",\"property_path\":\"StaticMeshComponent.StaticMesh\",\"property_class_path\":\"/Script/Engine.StaticMesh\",\"time_seconds\":0.75,\"value_path\":\"/Engine/BasicShapes/Sphere.Sphere\",\"value_before_key_path\":\"/Engine/BasicShapes/Cube.Cube\",\"save_after_set\":false}"), *SequenceAssetPath, *MeshBindingGuid));
	FHttpSmokeResult AddObjectKeyResult;
	TestTrue(TEXT("Sequence generic add_property_key object request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddObjectKeyBody, AddObjectKeyResult));
	TestEqual(TEXT("Sequence generic add_property_key object status code"), AddObjectKeyResult.StatusCode, 200);

	const FString AddVectorTrackBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-vector-track-001"),
		TEXT("sequence_add_property_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"vector3d\",\"property_name\":\"RelativeScale3D\",\"property_path\":\"StaticMeshComponent.RelativeScale3D\",\"save_after_set\":false}"), *SequenceAssetPath, *MeshBindingGuid));
	FHttpSmokeResult AddVectorTrackResult;
	TestTrue(TEXT("Sequence generic add_property_track vector request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddVectorTrackBody, AddVectorTrackResult));
	TestEqual(TEXT("Sequence generic add_property_track vector status code"), AddVectorTrackResult.StatusCode, 200);

	const FString AddVectorKeyBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-vector-key-001"),
		TEXT("sequence_add_property_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"vector3d\",\"property_name\":\"RelativeScale3D\",\"property_path\":\"StaticMeshComponent.RelativeScale3D\",\"time_seconds\":0.7,\"value\":{\"x\":1.5,\"y\":2.0,\"z\":0.75},\"value_before_key\":{\"x\":1.0,\"y\":1.0,\"z\":1.0},\"save_after_set\":false}"), *SequenceAssetPath, *MeshBindingGuid));
	FHttpSmokeResult AddVectorKeyResult;
	TestTrue(TEXT("Sequence generic add_property_key vector request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddVectorKeyBody, AddVectorKeyResult));
	TestEqual(TEXT("Sequence generic add_property_key vector status code"), AddVectorKeyResult.StatusCode, 200);

	const FString AddRotatorTrackBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-rotator-track-001"),
		TEXT("sequence_add_property_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"rotator\",\"property_name\":\"RelativeRotation\",\"property_path\":\"StaticMeshComponent.RelativeRotation\",\"save_after_set\":false}"), *SequenceAssetPath, *MeshBindingGuid));
	FHttpSmokeResult AddRotatorTrackResult;
	TestTrue(TEXT("Sequence generic add_property_track rotator request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddRotatorTrackBody, AddRotatorTrackResult));
	TestEqual(TEXT("Sequence generic add_property_track rotator status code"), AddRotatorTrackResult.StatusCode, 200);

	const FString AddRotatorKeyBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-rotator-key-001"),
		TEXT("sequence_add_property_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"rotator\",\"property_name\":\"RelativeRotation\",\"property_path\":\"StaticMeshComponent.RelativeRotation\",\"time_seconds\":0.72,\"value\":{\"pitch\":10.0,\"yaw\":45.0,\"roll\":5.0},\"value_before_key\":{\"pitch\":0.0,\"yaw\":0.0,\"roll\":0.0},\"save_after_set\":false}"), *SequenceAssetPath, *MeshBindingGuid));
	FHttpSmokeResult AddRotatorKeyResult;
	TestTrue(TEXT("Sequence generic add_property_key rotator request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddRotatorKeyBody, AddRotatorKeyResult));
	TestEqual(TEXT("Sequence generic add_property_key rotator status code"), AddRotatorKeyResult.StatusCode, 200);

	const FString AddDoubleTrackBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-double-track-001"),
		TEXT("sequence_add_property_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"double\",\"property_name\":\"ReplicatedWorldTimeSecondsDouble\",\"save_after_set\":false}"), *SequenceAssetPath, *StateBindingGuid));
	FHttpSmokeResult AddDoubleTrackResult;
	TestTrue(TEXT("Sequence generic add_property_track double request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddDoubleTrackBody, AddDoubleTrackResult));
	TestEqual(TEXT("Sequence generic add_property_track double status code"), AddDoubleTrackResult.StatusCode, 200);

	const FString AddDoubleKeyBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-double-key-001"),
		TEXT("sequence_add_property_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"double\",\"property_name\":\"ReplicatedWorldTimeSecondsDouble\",\"time_seconds\":0.6,\"value\":12.5,\"value_before_key\":0.0,\"save_after_set\":false}"), *SequenceAssetPath, *StateBindingGuid));
	FHttpSmokeResult AddDoubleKeyResult;
	TestTrue(TEXT("Sequence generic add_property_key double request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddDoubleKeyBody, AddDoubleKeyResult));
	TestEqual(TEXT("Sequence generic add_property_key double status code"), AddDoubleKeyResult.StatusCode, 200);

	const FString AddByteTrackBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-byte-track-001"),
		TEXT("sequence_add_property_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"byte\",\"property_name\":\"TerminalAction\",\"enum_path\":\"/Script/GptProjectTest.EFacilityTerminalAction\",\"save_after_set\":false}"), *SequenceAssetPath, *TerminalBindingGuid));
	FHttpSmokeResult AddByteTrackResult;
	TestTrue(TEXT("Sequence generic add_property_track byte request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddByteTrackBody, AddByteTrackResult));
	TestEqual(TEXT("Sequence generic add_property_track byte status code"), AddByteTrackResult.StatusCode, 200);

	const FString AddByteKeyBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-byte-key-001"),
		TEXT("sequence_add_property_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"byte\",\"property_name\":\"TerminalAction\",\"enum_path\":\"/Script/GptProjectTest.EFacilityTerminalAction\",\"time_seconds\":0.8,\"value_name\":\"MoveTargetToEnd\",\"value_before_key_name\":\"ActivateTarget\",\"save_after_set\":false}"), *SequenceAssetPath, *TerminalBindingGuid));
	FHttpSmokeResult AddByteKeyResult;
	TestTrue(TEXT("Sequence generic add_property_key byte request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddByteKeyBody, AddByteKeyResult));
	TestEqual(TEXT("Sequence generic add_property_key byte status code"), AddByteKeyResult.StatusCode, 200);

	const FString AddStringTrackBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-string-track-001"),
		TEXT("sequence_add_property_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"string\",\"property_name\":\"DocumentLink\",\"save_after_set\":false}"), *SequenceAssetPath, *DocBindingGuid));
	FHttpSmokeResult AddStringTrackResult;
	TestTrue(TEXT("Sequence generic add_property_track string request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddStringTrackBody, AddStringTrackResult));
	TestEqual(TEXT("Sequence generic add_property_track string status code"), AddStringTrackResult.StatusCode, 200);

	const FString AddStringKeyBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-string-key-001"),
		TEXT("sequence_add_property_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"string\",\"property_name\":\"DocumentLink\",\"time_seconds\":0.9,\"value\":\"https://dev.epicgames.com/updated\",\"value_before_key\":\"https://dev.epicgames.com/original\",\"save_after_set\":false}"), *SequenceAssetPath, *DocBindingGuid));
	FHttpSmokeResult AddStringKeyResult;
	TestTrue(TEXT("Sequence generic add_property_key string request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddStringKeyBody, AddStringKeyResult));
	TestEqual(TEXT("Sequence generic add_property_key string status code"), AddStringKeyResult.StatusCode, 200);

	const FString AddActorReferenceTrackBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-actorref-track-001"),
		TEXT("sequence_add_property_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"actor_reference\",\"property_name\":\"CurrentInteractableFacility\",\"save_after_set\":false}"), *SequenceAssetPath, *CharacterBindingGuid));
	FHttpSmokeResult AddActorReferenceTrackResult;
	TestTrue(TEXT("Sequence generic add_property_track actor_reference request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddActorReferenceTrackBody, AddActorReferenceTrackResult));
	TestEqual(TEXT("Sequence generic add_property_track actor_reference status code"), AddActorReferenceTrackResult.StatusCode, 200);

	const FString AddActorReferenceKeyBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-actorref-key-001"),
		TEXT("sequence_add_property_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"actor_reference\",\"property_name\":\"CurrentInteractableFacility\",\"time_seconds\":1.0,\"value_actor_id\":\"%s\",\"value_before_key_actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *CharacterBindingGuid, *BonfireActorLabel, *DoorActorLabel));
	FHttpSmokeResult AddActorReferenceKeyResult;
	TestTrue(TEXT("Sequence generic add_property_key actor_reference request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddActorReferenceKeyBody, AddActorReferenceKeyResult));
	TestEqual(TEXT("Sequence generic add_property_key actor_reference status code"), AddActorReferenceKeyResult.StatusCode, 200);

	const FString GetInfoBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-info-001"),
		TEXT("sequence_get_level_sequence_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SequenceAssetPath));
	FHttpSmokeResult GetInfoResult;
	TestTrue(TEXT("Sequence generic get_level_sequence_info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoBody, GetInfoResult));
	TestEqual(TEXT("Sequence generic get_level_sequence_info status code"), GetInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoJson;
	TestTrue(TEXT("Sequence generic get_level_sequence_info parses JSON"), ParseJson(GetInfoResult.Body, GetInfoJson));
	const TSharedPtr<FJsonObject>* GetInfoData = nullptr;
	TestTrue(TEXT("Sequence generic get_level_sequence_info contains data"), GetInfoJson.IsValid() && GetInfoJson->TryGetObjectField(TEXT("data"), GetInfoData) && GetInfoData && GetInfoData->IsValid());
	if (GetInfoData && GetInfoData->IsValid())
	{
		TSharedPtr<FJsonObject> BoolTrackObject;
		TestTrue(TEXT("Sequence generic bool track exists"), FindTrackByClassNeedle(GetInfoData, BindingGuid, TEXT("MovieSceneBoolTrack"), BoolTrackObject));
		if (BoolTrackObject.IsValid())
		{
			TestEqual(TEXT("Sequence generic bool property name matches"), BoolTrackObject->GetStringField(TEXT("property_name")), FString(TEXT("bActorEnableCollision")));
		}

		TSharedPtr<FJsonObject> ColorTrackObject;
		TestTrue(TEXT("Sequence generic color track exists"), FindTrackByClassNeedle(GetInfoData, BindingGuid, TEXT("MovieSceneColorTrack"), ColorTrackObject));
		if (ColorTrackObject.IsValid())
		{
			TestEqual(TEXT("Sequence generic color property name matches"), ColorTrackObject->GetStringField(TEXT("property_name")), FString(TEXT("LightColor")));
			TestEqual(TEXT("Sequence generic color property path matches"), ColorTrackObject->GetStringField(TEXT("property_path")), FString(TEXT("LightComponent.LightColor")));

			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			TestTrue(TEXT("Sequence generic color track contains sections"), ColorTrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() > 0);
			if (Sections && Sections->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* SectionObject = nullptr;
				TestTrue(TEXT("Sequence generic color section parses"), (*Sections)[0].IsValid() && (*Sections)[0]->TryGetObject(SectionObject) && SectionObject && SectionObject->IsValid());
				if (SectionObject && SectionObject->IsValid())
				{
					TestEqual(TEXT("Sequence generic color section type"), (*SectionObject)->GetStringField(TEXT("section_type")), FString(TEXT("color")));
					TestEqual(TEXT("Sequence generic color red key count"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("red_key_count"))), 1);
					TestEqual(TEXT("Sequence generic color alpha key count"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("alpha_key_count"))), 1);
				}
			}
		}

		TSharedPtr<FJsonObject> ObjectTrackObject;
		TestTrue(TEXT("Sequence generic object track exists"), FindTrackByClassNeedle(GetInfoData, MeshBindingGuid, TEXT("MovieSceneObjectPropertyTrack"), ObjectTrackObject));
		if (ObjectTrackObject.IsValid())
		{
			TestEqual(TEXT("Sequence generic object property name matches"), ObjectTrackObject->GetStringField(TEXT("property_name")), FString(TEXT("StaticMesh")));
			TestEqual(TEXT("Sequence generic object property path matches"), ObjectTrackObject->GetStringField(TEXT("property_path")), FString(TEXT("StaticMeshComponent.StaticMesh")));
			TestEqual(TEXT("Sequence generic object property class path matches"), ObjectTrackObject->GetStringField(TEXT("property_class_path")), FString(TEXT("/Script/Engine.StaticMesh")));

			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			TestTrue(TEXT("Sequence generic object track contains sections"), ObjectTrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() > 0);
			if (Sections && Sections->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* SectionObject = nullptr;
				TestTrue(TEXT("Sequence generic object section parses"), (*Sections)[0].IsValid() && (*Sections)[0]->TryGetObject(SectionObject) && SectionObject && SectionObject->IsValid());
				if (SectionObject && SectionObject->IsValid())
				{
					TestEqual(TEXT("Sequence generic object section type"), (*SectionObject)->GetStringField(TEXT("section_type")), FString(TEXT("object")));
					TestEqual(TEXT("Sequence generic object key count"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("key_count"))), 2);
				}
			}
		}

		TSharedPtr<FJsonObject> VectorTrackObject;
		TestTrue(TEXT("Sequence generic vector track exists"), FindTrackByClassNeedle(GetInfoData, MeshBindingGuid, TEXT("MovieSceneDoubleVectorTrack"), VectorTrackObject));
		if (VectorTrackObject.IsValid())
		{
			TestEqual(TEXT("Sequence generic vector property name matches"), VectorTrackObject->GetStringField(TEXT("property_name")), FString(TEXT("RelativeScale3D")));
			TestEqual(TEXT("Sequence generic vector property path matches"), VectorTrackObject->GetStringField(TEXT("property_path")), FString(TEXT("StaticMeshComponent.RelativeScale3D")));
			TestEqual(TEXT("Sequence generic vector precision matches"), VectorTrackObject->GetStringField(TEXT("vector_precision")), FString(TEXT("double")));
			TestEqual(TEXT("Sequence generic vector channels used matches"), static_cast<int32>(VectorTrackObject->GetIntegerField(TEXT("channels_used"))), 3);

			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			TestTrue(TEXT("Sequence generic vector track contains sections"), VectorTrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() > 0);
			if (Sections && Sections->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* SectionObject = nullptr;
				TestTrue(TEXT("Sequence generic vector section parses"), (*Sections)[0].IsValid() && (*Sections)[0]->TryGetObject(SectionObject) && SectionObject && SectionObject->IsValid());
				if (SectionObject && SectionObject->IsValid())
				{
					TestEqual(TEXT("Sequence generic vector section type"), (*SectionObject)->GetStringField(TEXT("section_type")), FString(TEXT("vector")));
					TestEqual(TEXT("Sequence generic vector section precision"), (*SectionObject)->GetStringField(TEXT("vector_precision")), FString(TEXT("double")));
					TestEqual(TEXT("Sequence generic vector channels used"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("channels_used"))), 3);
					TestTrue(TEXT("Sequence generic vector x key count"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("x_key_count"))) >= 1);
					TestTrue(TEXT("Sequence generic vector z key count"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("z_key_count"))) >= 1);
				}
			}
		}

		TSharedPtr<FJsonObject> RotatorTrackObject;
		TestTrue(TEXT("Sequence generic rotator track exists"), FindTrackByClassNeedle(GetInfoData, MeshBindingGuid, TEXT("MovieSceneRotatorTrack"), RotatorTrackObject));
		if (RotatorTrackObject.IsValid())
		{
			TestEqual(TEXT("Sequence generic rotator property name matches"), RotatorTrackObject->GetStringField(TEXT("property_name")), FString(TEXT("RelativeRotation")));
			TestEqual(TEXT("Sequence generic rotator property path matches"), RotatorTrackObject->GetStringField(TEXT("property_path")), FString(TEXT("StaticMeshComponent.RelativeRotation")));

			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			TestTrue(TEXT("Sequence generic rotator track contains sections"), RotatorTrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() > 0);
			if (Sections && Sections->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* SectionObject = nullptr;
				TestTrue(TEXT("Sequence generic rotator section parses"), (*Sections)[0].IsValid() && (*Sections)[0]->TryGetObject(SectionObject) && SectionObject && SectionObject->IsValid());
				if (SectionObject && SectionObject->IsValid())
				{
					TestEqual(TEXT("Sequence generic rotator section type"), (*SectionObject)->GetStringField(TEXT("section_type")), FString(TEXT("rotator")));
					TestTrue(TEXT("Sequence generic rotator pitch key count"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("pitch_key_count"))) >= 1);
					TestTrue(TEXT("Sequence generic rotator yaw key count"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("yaw_key_count"))) >= 1);
					TestTrue(TEXT("Sequence generic rotator roll key count"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("roll_key_count"))) >= 1);
				}
			}
		}

		TSharedPtr<FJsonObject> DoubleTrackObject;
		TestTrue(TEXT("Sequence generic double track exists"), FindTrackByClassNeedle(GetInfoData, StateBindingGuid, TEXT("MovieSceneDoubleTrack"), DoubleTrackObject));
		if (DoubleTrackObject.IsValid())
		{
			TestEqual(TEXT("Sequence generic double property name matches"), DoubleTrackObject->GetStringField(TEXT("property_name")), FString(TEXT("ReplicatedWorldTimeSecondsDouble")));

			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			TestTrue(TEXT("Sequence generic double track contains sections"), DoubleTrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() > 0);
			if (Sections && Sections->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* SectionObject = nullptr;
				TestTrue(TEXT("Sequence generic double section parses"), (*Sections)[0].IsValid() && (*Sections)[0]->TryGetObject(SectionObject) && SectionObject && SectionObject->IsValid());
				if (SectionObject && SectionObject->IsValid())
				{
					TestEqual(TEXT("Sequence generic double section type"), (*SectionObject)->GetStringField(TEXT("section_type")), FString(TEXT("double")));
					TestTrue(TEXT("Sequence generic double key count"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("key_count"))) >= 1);
				}
			}
		}

		TSharedPtr<FJsonObject> ByteTrackObject;
		TestTrue(TEXT("Sequence generic byte track exists"), FindTrackByClassNeedle(GetInfoData, TerminalBindingGuid, TEXT("MovieSceneByteTrack"), ByteTrackObject));
		if (ByteTrackObject.IsValid())
		{
			TestEqual(TEXT("Sequence generic byte property name matches"), ByteTrackObject->GetStringField(TEXT("property_name")), FString(TEXT("TerminalAction")));
			TestEqual(TEXT("Sequence generic byte enum path matches"), ByteTrackObject->GetStringField(TEXT("enum_path")), FString(TEXT("/Script/GptProjectTest.EFacilityTerminalAction")));

			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			TestTrue(TEXT("Sequence generic byte track contains sections"), ByteTrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() > 0);
			if (Sections && Sections->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* SectionObject = nullptr;
				TestTrue(TEXT("Sequence generic byte section parses"), (*Sections)[0].IsValid() && (*Sections)[0]->TryGetObject(SectionObject) && SectionObject && SectionObject->IsValid());
				if (SectionObject && SectionObject->IsValid())
				{
					TestEqual(TEXT("Sequence generic byte section type"), (*SectionObject)->GetStringField(TEXT("section_type")), FString(TEXT("byte")));
					TestTrue(TEXT("Sequence generic byte key count"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("key_count"))) >= 1);
				}
			}
		}

		TSharedPtr<FJsonObject> StringTrackObject;
		TestTrue(TEXT("Sequence generic string track exists"), FindTrackByClassNeedle(GetInfoData, DocBindingGuid, TEXT("MovieSceneStringTrack"), StringTrackObject));
		if (StringTrackObject.IsValid())
		{
			TestEqual(TEXT("Sequence generic string property name matches"), StringTrackObject->GetStringField(TEXT("property_name")), FString(TEXT("DocumentLink")));

			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			TestTrue(TEXT("Sequence generic string track contains sections"), StringTrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() > 0);
			if (Sections && Sections->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* SectionObject = nullptr;
				TestTrue(TEXT("Sequence generic string section parses"), (*Sections)[0].IsValid() && (*Sections)[0]->TryGetObject(SectionObject) && SectionObject && SectionObject->IsValid());
				if (SectionObject && SectionObject->IsValid())
				{
					TestEqual(TEXT("Sequence generic string section type"), (*SectionObject)->GetStringField(TEXT("section_type")), FString(TEXT("string")));
					TestTrue(TEXT("Sequence generic string key count"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("key_count"))) >= 1);
				}
			}
		}

		TSharedPtr<FJsonObject> ActorReferenceTrackObject;
		TestTrue(TEXT("Sequence generic actor reference track exists"), FindTrackByClassNeedle(GetInfoData, CharacterBindingGuid, TEXT("MovieSceneActorReferenceTrack"), ActorReferenceTrackObject));
		if (ActorReferenceTrackObject.IsValid())
		{
			TestEqual(TEXT("Sequence generic actor reference property name matches"), ActorReferenceTrackObject->GetStringField(TEXT("property_name")), FString(TEXT("CurrentInteractableFacility")));

			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			TestTrue(TEXT("Sequence generic actor reference track contains sections"), ActorReferenceTrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() > 0);
			if (Sections && Sections->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* SectionObject = nullptr;
				TestTrue(TEXT("Sequence generic actor reference section parses"), (*Sections)[0].IsValid() && (*Sections)[0]->TryGetObject(SectionObject) && SectionObject && SectionObject->IsValid());
				if (SectionObject && SectionObject->IsValid())
				{
					TestEqual(TEXT("Sequence generic actor reference section type"), (*SectionObject)->GetStringField(TEXT("section_type")), FString(TEXT("actor_reference")));
					TestTrue(TEXT("Sequence generic actor reference key count"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("key_count"))) >= 1);
				}
			}
		}
	}

	const FString ExportFolderBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-folder-export-001"),
		TEXT("sequence_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clean_output_dir\":true,\"include_validation\":true}"), *SequenceAssetPath));
	FHttpSmokeResult ExportFolderResult;
	TestTrue(TEXT("Sequence generic folder export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ExportFolderBody, ExportFolderResult));
	TestEqual(TEXT("Sequence generic folder export status code"), ExportFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> BindingsIndexJson;
	TestTrue(TEXT("Sequence generic folder bindings index load"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("bindings"), TEXT("index.json")), BindingsIndexJson));
	FString MeshBindingFolderName;
	FString TerminalBindingFolderName;
	FString DocBindingFolderName;
	FString CharacterBindingFolderName;
	TestTrue(TEXT("Sequence generic folder mesh binding folder found"), FindBindingFolderName(BindingsIndexJson, MeshBindingGuid, MeshBindingFolderName));
	TestTrue(TEXT("Sequence generic folder terminal binding folder found"), FindBindingFolderName(BindingsIndexJson, TerminalBindingGuid, TerminalBindingFolderName));
	TestTrue(TEXT("Sequence generic folder doc binding folder found"), FindBindingFolderName(BindingsIndexJson, DocBindingGuid, DocBindingFolderName));
	TestTrue(TEXT("Sequence generic folder character binding folder found"), FindBindingFolderName(BindingsIndexJson, CharacterBindingGuid, CharacterBindingFolderName));

	const FString MeshTracksDir = FPaths::Combine(FolderPath, TEXT("bindings"), MeshBindingFolderName, TEXT("tracks"));
	const FString TerminalTracksDir = FPaths::Combine(FolderPath, TEXT("bindings"), TerminalBindingFolderName, TEXT("tracks"));
	const FString DocTracksDir = FPaths::Combine(FolderPath, TEXT("bindings"), DocBindingFolderName, TEXT("tracks"));
	const FString CharacterTracksDir = FPaths::Combine(FolderPath, TEXT("bindings"), CharacterBindingFolderName, TEXT("tracks"));

	FString ObjectTrackFile;
	FString VectorTrackFile;
	FString RotatorTrackFile;
	FString ByteTrackFile;
	FString StringTrackFile;
	FString ActorReferenceTrackFile;
	TSharedPtr<FJsonObject> ObjectTrackJson;
	TSharedPtr<FJsonObject> VectorTrackJson;
	TSharedPtr<FJsonObject> RotatorTrackJson;
	TSharedPtr<FJsonObject> ByteTrackJson;
	TSharedPtr<FJsonObject> StringTrackJson;
	TSharedPtr<FJsonObject> ActorReferenceTrackJson;

	TestTrue(TEXT("Sequence generic folder object track file found"), FindTrackFileInDir(MeshTracksDir, [](const TSharedPtr<FJsonObject>& TrackJson){ return TrackJson->GetStringField(TEXT("track_type")) == TEXT("property") && TrackJson->GetStringField(TEXT("property_type")) == TEXT("object"); }, ObjectTrackFile, ObjectTrackJson));
	TestTrue(TEXT("Sequence generic folder vector track file found"), FindTrackFileInDir(MeshTracksDir, [](const TSharedPtr<FJsonObject>& TrackJson){ return TrackJson->GetStringField(TEXT("track_type")) == TEXT("property") && TrackJson->GetStringField(TEXT("property_type")) == TEXT("vector"); }, VectorTrackFile, VectorTrackJson));
	TestTrue(TEXT("Sequence generic folder rotator track file found"), FindTrackFileInDir(MeshTracksDir, [](const TSharedPtr<FJsonObject>& TrackJson){ return TrackJson->GetStringField(TEXT("track_type")) == TEXT("property") && TrackJson->GetStringField(TEXT("property_type")) == TEXT("rotator"); }, RotatorTrackFile, RotatorTrackJson));
	TestTrue(TEXT("Sequence generic folder byte track file found"), FindTrackFileInDir(TerminalTracksDir, [](const TSharedPtr<FJsonObject>& TrackJson){ return TrackJson->GetStringField(TEXT("track_type")) == TEXT("property") && TrackJson->GetStringField(TEXT("property_type")) == TEXT("byte"); }, ByteTrackFile, ByteTrackJson));
	TestTrue(TEXT("Sequence generic folder string track file found"), FindTrackFileInDir(DocTracksDir, [](const TSharedPtr<FJsonObject>& TrackJson){ return TrackJson->GetStringField(TEXT("track_type")) == TEXT("property") && TrackJson->GetStringField(TEXT("property_type")) == TEXT("string"); }, StringTrackFile, StringTrackJson));
	TestTrue(TEXT("Sequence generic folder actor reference track file found"), FindTrackFileInDir(CharacterTracksDir, [](const TSharedPtr<FJsonObject>& TrackJson){ return TrackJson->GetStringField(TEXT("track_type")) == TEXT("property") && TrackJson->GetStringField(TEXT("property_type")) == TEXT("actor_reference"); }, ActorReferenceTrackFile, ActorReferenceTrackJson));

	if (ObjectTrackJson.IsValid())
	{
		TestEqual(TEXT("Sequence generic folder object class path"), ObjectTrackJson->GetStringField(TEXT("property_class_path")), FString(TEXT("/Script/Engine.StaticMesh")));
	}
	if (VectorTrackJson.IsValid())
	{
		TestEqual(TEXT("Sequence generic folder vector precision"), VectorTrackJson->GetStringField(TEXT("vector_precision")), FString(TEXT("double")));
		TestEqual(TEXT("Sequence generic folder vector channels"), static_cast<int32>(VectorTrackJson->GetIntegerField(TEXT("channels_used"))), 3);
	}
	if (ByteTrackJson.IsValid())
	{
		TestEqual(TEXT("Sequence generic folder byte enum path"), ByteTrackJson->GetStringField(TEXT("enum_path")), FString(TEXT("/Script/GptProjectTest.EFacilityTerminalAction")));
	}

	if (ObjectTrackJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (ObjectTrackJson->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
		{
			const TSharedPtr<FJsonObject>* SectionObj = nullptr;
			if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
				if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys && Keys->Num() >= 2)
				{
					const TSharedPtr<FJsonObject>* KeyObj = nullptr;
					if ((*Keys)[1]->TryGetObject(KeyObj) && KeyObj && (*KeyObj).IsValid())
					{
						(*KeyObj)->SetStringField(TEXT("value_path"), TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
					}
				}
			}
		}
		TestTrue(TEXT("Sequence generic folder object track save"), SaveJsonFile(FPaths::Combine(MeshTracksDir, ObjectTrackFile), ObjectTrackJson));
	}
	if (VectorTrackJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (VectorTrackJson->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
		{
			const TSharedPtr<FJsonObject>* SectionObj = nullptr;
			if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
				if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys && Keys->Num() >= 2)
				{
					const TSharedPtr<FJsonObject>* KeyObj = nullptr;
					if ((*Keys)[1]->TryGetObject(KeyObj) && KeyObj && (*KeyObj).IsValid())
					{
						const TSharedPtr<FJsonObject>* ValueObj = nullptr;
						if ((*KeyObj)->TryGetObjectField(TEXT("value"), ValueObj) && ValueObj && (*ValueObj).IsValid())
						{
							(*ValueObj)->SetNumberField(TEXT("x"), 2.25);
						}
					}
				}
			}
		}
		TestTrue(TEXT("Sequence generic folder vector track save"), SaveJsonFile(FPaths::Combine(MeshTracksDir, VectorTrackFile), VectorTrackJson));
	}
	if (RotatorTrackJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (RotatorTrackJson->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
		{
			const TSharedPtr<FJsonObject>* SectionObj = nullptr;
			if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
				if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys && Keys->Num() >= 2)
				{
					const TSharedPtr<FJsonObject>* KeyObj = nullptr;
					if ((*Keys)[1]->TryGetObject(KeyObj) && KeyObj && (*KeyObj).IsValid())
					{
						const TSharedPtr<FJsonObject>* ValueObj = nullptr;
						if ((*KeyObj)->TryGetObjectField(TEXT("value"), ValueObj) && ValueObj && (*ValueObj).IsValid())
						{
							(*ValueObj)->SetNumberField(TEXT("yaw"), 90.0);
						}
					}
				}
			}
		}
		TestTrue(TEXT("Sequence generic folder rotator track save"), SaveJsonFile(FPaths::Combine(MeshTracksDir, RotatorTrackFile), RotatorTrackJson));
	}
	if (ByteTrackJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (ByteTrackJson->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
		{
			const TSharedPtr<FJsonObject>* SectionObj = nullptr;
			if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
				if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys && Keys->Num() >= 2)
				{
					const TSharedPtr<FJsonObject>* KeyObj = nullptr;
					if ((*Keys)[1]->TryGetObject(KeyObj) && KeyObj && (*KeyObj).IsValid())
					{
						(*KeyObj)->SetStringField(TEXT("value_name"), TEXT("ActivateTarget"));
					}
				}
			}
		}
		TestTrue(TEXT("Sequence generic folder byte track save"), SaveJsonFile(FPaths::Combine(TerminalTracksDir, ByteTrackFile), ByteTrackJson));
	}
	if (StringTrackJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (StringTrackJson->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
		{
			const TSharedPtr<FJsonObject>* SectionObj = nullptr;
			if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
				if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys && Keys->Num() >= 2)
				{
					const TSharedPtr<FJsonObject>* KeyObj = nullptr;
					if ((*Keys)[1]->TryGetObject(KeyObj) && KeyObj && (*KeyObj).IsValid())
					{
						(*KeyObj)->SetStringField(TEXT("value"), TEXT("https://dev.epicgames.com/folder-updated"));
					}
				}
			}
		}
		TestTrue(TEXT("Sequence generic folder string track save"), SaveJsonFile(FPaths::Combine(DocTracksDir, StringTrackFile), StringTrackJson));
	}
	if (ActorReferenceTrackJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (ActorReferenceTrackJson->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
		{
			const TSharedPtr<FJsonObject>* SectionObj = nullptr;
			if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
				if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys && Keys->Num() >= 2)
				{
					const TSharedPtr<FJsonObject>* KeyObj = nullptr;
					if ((*Keys)[1]->TryGetObject(KeyObj) && KeyObj && (*KeyObj).IsValid())
					{
						(*KeyObj)->SetStringField(TEXT("value_binding_guid"), DoorBindingGuid);
						(*KeyObj)->SetStringField(TEXT("component_name"), TEXT(""));
						(*KeyObj)->SetStringField(TEXT("socket_name"), TEXT(""));
					}
				}
			}
		}
		TestTrue(TEXT("Sequence generic folder actor reference track save"), SaveJsonFile(FPaths::Combine(CharacterTracksDir, ActorReferenceTrackFile), ActorReferenceTrackJson));
	}

	const FString ApplyFolderBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-folder-apply-001"),
		TEXT("sequence_apply_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"create_if_missing\":false,\"save_after_apply\":false}"), *SequenceAssetPath));
	FHttpSmokeResult ApplyFolderResult;
	TestTrue(TEXT("Sequence generic folder apply request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ApplyFolderBody, ApplyFolderResult));
	TestEqual(TEXT("Sequence generic folder apply status code"), ApplyFolderResult.StatusCode, 200);

	const FString ReExportFolderBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-folder-export-002"),
		TEXT("sequence_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clean_output_dir\":true,\"include_validation\":true}"), *SequenceAssetPath));
	FHttpSmokeResult ReExportFolderResult;
	TestTrue(TEXT("Sequence generic folder re-export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ReExportFolderBody, ReExportFolderResult));
	TestEqual(TEXT("Sequence generic folder re-export status code"), ReExportFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ReObjectTrackJson;
	TSharedPtr<FJsonObject> ReVectorTrackJson;
	TSharedPtr<FJsonObject> ReRotatorTrackJson;
	TSharedPtr<FJsonObject> ReByteTrackJson;
	TSharedPtr<FJsonObject> ReStringTrackJson;
	TSharedPtr<FJsonObject> ReActorReferenceTrackJson;
	TestTrue(TEXT("Sequence generic folder reload object track"), LoadJsonFile(FPaths::Combine(MeshTracksDir, ObjectTrackFile), ReObjectTrackJson));
	TestTrue(TEXT("Sequence generic folder reload vector track"), LoadJsonFile(FPaths::Combine(MeshTracksDir, VectorTrackFile), ReVectorTrackJson));
	TestTrue(TEXT("Sequence generic folder reload rotator track"), LoadJsonFile(FPaths::Combine(MeshTracksDir, RotatorTrackFile), ReRotatorTrackJson));
	TestTrue(TEXT("Sequence generic folder reload byte track"), LoadJsonFile(FPaths::Combine(TerminalTracksDir, ByteTrackFile), ReByteTrackJson));
	TestTrue(TEXT("Sequence generic folder reload string track"), LoadJsonFile(FPaths::Combine(DocTracksDir, StringTrackFile), ReStringTrackJson));
	TestTrue(TEXT("Sequence generic folder reload actor reference track"), LoadJsonFile(FPaths::Combine(CharacterTracksDir, ActorReferenceTrackFile), ReActorReferenceTrackJson));

	auto VerifySecondKeyString = [this](const TSharedPtr<FJsonObject>& TrackJson, const FString& ExpectedValue, const TCHAR* JsonField) -> bool
	{
		if (!TrackJson.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (!TrackJson->TryGetArrayField(TEXT("sections"), Sections) || !Sections || Sections->Num() != 1)
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* SectionObj = nullptr;
		if (!(*Sections)[0]->TryGetObject(SectionObj) || !SectionObj || !(*SectionObj).IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
		if (!(*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) || !Keys || Keys->Num() < 2)
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* KeyObj = nullptr;
		if (!(*Keys)[1]->TryGetObject(KeyObj) || !KeyObj || !(*KeyObj).IsValid())
		{
			return false;
		}
		return (*KeyObj)->GetStringField(JsonField) == ExpectedValue;
	};

	TestTrue(TEXT("Sequence generic folder object value updated"), VerifySecondKeyString(ReObjectTrackJson, FString(TEXT("/Engine/BasicShapes/Cylinder.Cylinder")), TEXT("value_path")));
	TestTrue(TEXT("Sequence generic folder byte value updated"), VerifySecondKeyString(ReByteTrackJson, FString(TEXT("ActivateTarget")), TEXT("value_name")));
	TestTrue(TEXT("Sequence generic folder string value updated"), VerifySecondKeyString(ReStringTrackJson, FString(TEXT("https://dev.epicgames.com/folder-updated")), TEXT("value")));
	TestTrue(TEXT("Sequence generic folder actor reference value updated"), VerifySecondKeyString(ReActorReferenceTrackJson, DoorBindingGuid, TEXT("value_binding_guid")));

	if (ReVectorTrackJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (ReVectorTrackJson->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
		{
			const TSharedPtr<FJsonObject>* SectionObj = nullptr;
			if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
				if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys && Keys->Num() >= 2)
				{
					const TSharedPtr<FJsonObject>* KeyObj = nullptr;
					if ((*Keys)[1]->TryGetObject(KeyObj) && KeyObj && (*KeyObj).IsValid())
					{
						const TSharedPtr<FJsonObject>* ValueObj = nullptr;
						if ((*KeyObj)->TryGetObjectField(TEXT("value"), ValueObj) && ValueObj && (*ValueObj).IsValid())
						{
							TestTrue(TEXT("Sequence generic folder vector value updated"), FMath::IsNearlyEqual(static_cast<float>((*ValueObj)->GetNumberField(TEXT("x"))), 2.25f, 0.01f));
						}
					}
				}
			}
		}
	}

	if (ReRotatorTrackJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (ReRotatorTrackJson->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
		{
			const TSharedPtr<FJsonObject>* SectionObj = nullptr;
			if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
				if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys && Keys->Num() >= 2)
				{
					const TSharedPtr<FJsonObject>* KeyObj = nullptr;
					if ((*Keys)[1]->TryGetObject(KeyObj) && KeyObj && (*KeyObj).IsValid())
					{
						const TSharedPtr<FJsonObject>* ValueObj = nullptr;
						if ((*KeyObj)->TryGetObjectField(TEXT("value"), ValueObj) && ValueObj && (*ValueObj).IsValid())
						{
							TestTrue(TEXT("Sequence generic folder rotator value updated"), FMath::IsNearlyEqual(static_cast<float>((*ValueObj)->GetNumberField(TEXT("yaw"))), 90.0f, 0.01f));
						}
					}
				}
			}
		}
	}

	return !HasAnyErrors();
}

bool FUeAgentInterfaceSequenceSkeletalAnimationSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString ActorLabel = FString::Printf(TEXT("SmokeSeqSkel_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString SequenceAssetPath = MakeAutomationAssetPath(TEXT("SeqSkel"));
	const FString AnimationAssetPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle.Tutorial_Idle");

	auto FindTrackByClassNeedle = [](const TSharedPtr<FJsonObject>* InfoData, const FString& BindingGuid, const FString& ClassNeedle, TSharedPtr<FJsonObject>& OutTrackObject) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("bindings"), Bindings) || !Bindings)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& BindingValue : *Bindings)
		{
			const TSharedPtr<FJsonObject>* BindingObject = nullptr;
			if (!BindingValue.IsValid() || !BindingValue->TryGetObject(BindingObject) || !BindingObject || !BindingObject->IsValid())
			{
				continue;
			}
			if ((*BindingObject)->GetStringField(TEXT("guid")) != BindingGuid)
			{
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
			if (!(*BindingObject)->TryGetArrayField(TEXT("tracks"), Tracks) || !Tracks)
			{
				return false;
			}

			for (const TSharedPtr<FJsonValue>& TrackValue : *Tracks)
			{
				const TSharedPtr<FJsonObject>* TrackObject = nullptr;
				if (!TrackValue.IsValid() || !TrackValue->TryGetObject(TrackObject) || !TrackObject || !TrackObject->IsValid())
				{
					continue;
				}
				if ((*TrackObject)->GetStringField(TEXT("class")).Contains(ClassNeedle))
				{
					OutTrackObject = *TrackObject;
					return true;
				}
			}
		}

		return false;
	};

	const FString SpawnActorBody = MakeExecRequestBody(
		TEXT("smoke-seqskel-spawn-001"),
		TEXT("spawn_actor"),
		FString::Printf(TEXT("{\"class_path\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":128,\"y\":-256,\"z\":96}}"), *ActorLabel));
	FHttpSmokeResult SpawnActorResult;
	TestTrue(TEXT("Sequence skeletal spawn_actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnActorBody, SpawnActorResult));
	TestEqual(TEXT("Sequence skeletal spawn_actor status code"), SpawnActorResult.StatusCode, 200);

	const FString CreateSequenceBody = MakeExecRequestBody(
		TEXT("smoke-seqskel-create-seq-001"),
		TEXT("sequence_create_level_sequence"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"start_seconds\":0,\"duration_seconds\":2.0,\"display_rate_num\":30,\"display_rate_den\":1,\"open_editor\":false,\"save_after_create\":false}"), *SequenceAssetPath));
	FHttpSmokeResult CreateSequenceResult;
	TestTrue(TEXT("Sequence skeletal create_level_sequence request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateSequenceBody, CreateSequenceResult));
	TestEqual(TEXT("Sequence skeletal create_level_sequence status code"), CreateSequenceResult.StatusCode, 200);

	const FString AddBindingBody = MakeExecRequestBody(
		TEXT("smoke-seqskel-add-binding-001"),
		TEXT("sequence_add_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *ActorLabel));
	FHttpSmokeResult AddBindingResult;
	TestTrue(TEXT("Sequence skeletal add_actor_binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddBindingBody, AddBindingResult));
	TestEqual(TEXT("Sequence skeletal add_actor_binding status code"), AddBindingResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddBindingJson;
	TestTrue(TEXT("Sequence skeletal add_actor_binding parses JSON"), ParseJson(AddBindingResult.Body, AddBindingJson));
	FString BindingGuid;
	const TSharedPtr<FJsonObject>* AddBindingData = nullptr;
	TestTrue(TEXT("Sequence skeletal add_actor_binding contains data"), AddBindingJson.IsValid() && AddBindingJson->TryGetObjectField(TEXT("data"), AddBindingData) && AddBindingData && AddBindingData->IsValid());
	if (AddBindingData && AddBindingData->IsValid())
	{
		BindingGuid = (*AddBindingData)->GetStringField(TEXT("binding_guid"));
		TestTrue(TEXT("Sequence skeletal binding guid non-empty"), !BindingGuid.IsEmpty());
	}

	const FString AddTrackBody = MakeExecRequestBody(
		TEXT("smoke-seqskel-add-track-001"),
		TEXT("sequence_add_skeletal_animation_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddTrackResult;
	TestTrue(TEXT("Sequence skeletal add_skeletal_animation_track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddTrackBody, AddTrackResult));
	TestEqual(TEXT("Sequence skeletal add_skeletal_animation_track status code"), AddTrackResult.StatusCode, 200);

	const FString AddSectionBody = MakeExecRequestBody(
		TEXT("smoke-seqskel-add-section-001"),
		TEXT("sequence_add_skeletal_animation_section"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"animation_asset\":\"%s\",\"start_seconds\":0.25,\"duration_seconds\":1.0,\"play_rate\":1.0,\"reverse\":false,\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid, *AnimationAssetPath));
	FHttpSmokeResult AddSectionResult;
	TestTrue(TEXT("Sequence skeletal add_skeletal_animation_section request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddSectionBody, AddSectionResult));
	TestEqual(TEXT("Sequence skeletal add_skeletal_animation_section status code"), AddSectionResult.StatusCode, 200);

	const FString UpdateSectionBody = MakeExecRequestBody(
		TEXT("smoke-seqskel-update-section-001"),
		TEXT("sequence_update_skeletal_animation_section"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"section_index\":0,\"start_seconds\":0.4,\"duration_seconds\":0.75,\"play_rate\":1.25,\"slot_name\":\"UpperBody\",\"row_index\":2,\"skip_anim_notifiers\":true,\"force_custom_mode\":true,\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult UpdateSectionResult;
	TestTrue(TEXT("Sequence skeletal update_skeletal_animation_section request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, UpdateSectionBody, UpdateSectionResult));
	TestEqual(TEXT("Sequence skeletal update_skeletal_animation_section status code"), UpdateSectionResult.StatusCode, 200);

	const FString AddVisibilityTrackBody = MakeExecRequestBody(
		TEXT("smoke-seqskel-add-visibility-track-001"),
		TEXT("sequence_add_visibility_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddVisibilityTrackResult;
	TestTrue(TEXT("Sequence skeletal add visibility track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddVisibilityTrackBody, AddVisibilityTrackResult));
	TestEqual(TEXT("Sequence skeletal add visibility track status code"), AddVisibilityTrackResult.StatusCode, 200);

	const FString AddVisibilityKeyBody = MakeExecRequestBody(
		TEXT("smoke-seqskel-add-visibility-key-001"),
		TEXT("sequence_add_visibility_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"time_seconds\":0.6,\"visible\":false,\"visible_before_key\":true,\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddVisibilityKeyResult;
	TestTrue(TEXT("Sequence skeletal add visibility key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddVisibilityKeyBody, AddVisibilityKeyResult));
	TestEqual(TEXT("Sequence skeletal add visibility key status code"), AddVisibilityKeyResult.StatusCode, 200);

	const FString GetInfoBody = MakeExecRequestBody(
		TEXT("smoke-seqskel-info-001"),
		TEXT("sequence_get_level_sequence_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SequenceAssetPath));
	FHttpSmokeResult GetInfoResult;
	TestTrue(TEXT("Sequence skeletal get_level_sequence_info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoBody, GetInfoResult));
	TestEqual(TEXT("Sequence skeletal get_level_sequence_info status code"), GetInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoJson;
	TestTrue(TEXT("Sequence skeletal get_level_sequence_info parses JSON"), ParseJson(GetInfoResult.Body, GetInfoJson));
	const TSharedPtr<FJsonObject>* GetInfoData = nullptr;
	TestTrue(TEXT("Sequence skeletal get_level_sequence_info contains data"), GetInfoJson.IsValid() && GetInfoJson->TryGetObjectField(TEXT("data"), GetInfoData) && GetInfoData && GetInfoData->IsValid());
	if (GetInfoData && GetInfoData->IsValid())
	{
		TSharedPtr<FJsonObject> TrackObject;
		TestTrue(TEXT("Sequence skeletal track summary exists"), FindTrackByClassNeedle(GetInfoData, BindingGuid, TEXT("MovieSceneSkeletalAnimationTrack"), TrackObject));
		if (TrackObject.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			TestTrue(TEXT("Sequence skeletal track summary contains sections"), TrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections != nullptr);
			if (Sections && Sections->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* SectionObject = nullptr;
				TestTrue(TEXT("Sequence skeletal section summary parses"), (*Sections)[0].IsValid() && (*Sections)[0]->TryGetObject(SectionObject) && SectionObject && SectionObject->IsValid());
				if (SectionObject && SectionObject->IsValid())
				{
					TestEqual(TEXT("Sequence skeletal section animation asset"), (*SectionObject)->GetStringField(TEXT("animation_asset")), AnimationAssetPath);
					TestEqual(TEXT("Sequence skeletal section slot name updated"), (*SectionObject)->GetStringField(TEXT("slot_name")), FString(TEXT("UpperBody")));
					TestEqual(TEXT("Sequence skeletal section row index updated"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("row_index"))), 2);
					TestEqual(TEXT("Sequence skeletal section skip notifiers updated"), (*SectionObject)->GetBoolField(TEXT("skip_anim_notifiers")), true);
					TestEqual(TEXT("Sequence skeletal section force custom mode updated"), (*SectionObject)->GetBoolField(TEXT("force_custom_mode")), true);
				}
			}
		}

		TSharedPtr<FJsonObject> VisibilityTrackObject;
		TestTrue(TEXT("Sequence visibility track summary exists"), FindTrackByClassNeedle(GetInfoData, BindingGuid, TEXT("MovieSceneVisibilityTrack"), VisibilityTrackObject));
		if (VisibilityTrackObject.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			TestTrue(TEXT("Sequence visibility track summary contains sections"), VisibilityTrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections != nullptr);
			if (Sections && Sections->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* SectionObject = nullptr;
				TestTrue(TEXT("Sequence visibility section summary parses"), (*Sections)[0].IsValid() && (*Sections)[0]->TryGetObject(SectionObject) && SectionObject && SectionObject->IsValid());
				if (SectionObject && SectionObject->IsValid())
				{
					TestEqual(TEXT("Sequence visibility section type"), (*SectionObject)->GetStringField(TEXT("section_type")), FString(TEXT("visibility")));
					TestTrue(TEXT("Sequence visibility section has keys"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("key_count"))) >= 1);
				}
			}
		}
	}

	const FString RemoveSectionBody = MakeExecRequestBody(
		TEXT("smoke-seqskel-remove-section-001"),
		TEXT("sequence_remove_skeletal_animation_section"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"section_index\":0,\"save_after_remove\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult RemoveSectionResult;
	TestTrue(TEXT("Sequence skeletal remove_skeletal_animation_section request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveSectionBody, RemoveSectionResult));
	TestEqual(TEXT("Sequence skeletal remove_skeletal_animation_section status code"), RemoveSectionResult.StatusCode, 200);

	const FString GetInfoAfterRemoveBody = MakeExecRequestBody(
		TEXT("smoke-seqskel-info-002"),
		TEXT("sequence_get_level_sequence_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SequenceAssetPath));
	FHttpSmokeResult GetInfoAfterRemoveResult;
	TestTrue(TEXT("Sequence skeletal get info after remove request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoAfterRemoveBody, GetInfoAfterRemoveResult));
	TestEqual(TEXT("Sequence skeletal get info after remove status code"), GetInfoAfterRemoveResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoAfterRemoveJson;
	TestTrue(TEXT("Sequence skeletal get info after remove parses JSON"), ParseJson(GetInfoAfterRemoveResult.Body, GetInfoAfterRemoveJson));
	const TSharedPtr<FJsonObject>* GetInfoAfterRemoveData = nullptr;
	TestTrue(TEXT("Sequence skeletal get info after remove contains data"), GetInfoAfterRemoveJson.IsValid() && GetInfoAfterRemoveJson->TryGetObjectField(TEXT("data"), GetInfoAfterRemoveData) && GetInfoAfterRemoveData && GetInfoAfterRemoveData->IsValid());
	if (GetInfoAfterRemoveData && GetInfoAfterRemoveData->IsValid())
	{
		TSharedPtr<FJsonObject> TrackObject;
		TestTrue(TEXT("Sequence skeletal track summary still exists after remove"), FindTrackByClassNeedle(GetInfoAfterRemoveData, BindingGuid, TEXT("MovieSceneSkeletalAnimationTrack"), TrackObject));
		if (TrackObject.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			TestTrue(TEXT("Sequence skeletal track summary contains sections array after remove"), TrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections != nullptr);
			if (Sections)
			{
				TestEqual(TEXT("Sequence skeletal section count after remove"), Sections->Num(), 0);
			}
		}
	}

	const FString DestroyActorBody = MakeExecRequestBody(
		TEXT("smoke-seqskel-destroy-001"),
		TEXT("destroy_actor"),
		FString::Printf(TEXT("{\"id\":\"%s\"}"), *ActorLabel));
	FHttpSmokeResult DestroyActorResult;
	TestTrue(TEXT("Sequence skeletal destroy_actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DestroyActorBody, DestroyActorResult));
	TestEqual(TEXT("Sequence skeletal destroy_actor status code"), DestroyActorResult.StatusCode, 200);

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceSequenceUmgAnimationSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.SequenceUmgAnimationWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceSequenceUmgAnimationSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString WidgetBlueprintAssetPath = MakeAutomationAssetPath(TEXT("WBPSeqAnim"));
	const FString WidgetName = TEXT("BtnSeqAnim");
	const FString RequestedAnimationName = TEXT("IntroAnim");
	const FString RenamedAnimationName = TEXT("IntroAnimRenamed");

	auto FindBindingTrackByProperty = [](const TSharedPtr<FJsonObject>* InfoData, const FString& WidgetNameNeedle, const FString& PropertyName, TSharedPtr<FJsonObject>& OutTrackObject) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("bindings"), Bindings) || !Bindings)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& BindingValue : *Bindings)
		{
			const TSharedPtr<FJsonObject>* BindingObject = nullptr;
			if (!BindingValue.IsValid() || !BindingValue->TryGetObject(BindingObject) || !BindingObject || !BindingObject->IsValid())
			{
				continue;
			}
			if ((*BindingObject)->GetStringField(TEXT("widget_name")) != WidgetNameNeedle)
			{
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
			if (!(*BindingObject)->TryGetArrayField(TEXT("tracks"), Tracks) || !Tracks)
			{
				return false;
			}

			for (const TSharedPtr<FJsonValue>& TrackValue : *Tracks)
			{
				const TSharedPtr<FJsonObject>* TrackObject = nullptr;
				if (!TrackValue.IsValid() || !TrackValue->TryGetObject(TrackObject) || !TrackObject || !TrackObject->IsValid())
				{
					continue;
				}

				if ((*TrackObject)->GetStringField(TEXT("property_name")).Equals(PropertyName, ESearchCase::IgnoreCase))
				{
					OutTrackObject = *TrackObject;
					return true;
				}
			}
		}

		return false;
	};

	auto ListContainsAnimation = [](const TSharedPtr<FJsonObject>* ListData, const FString& AnimationName, bool& bFoundOut) -> bool
	{
		bFoundOut = false;
		if (!ListData || !ListData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Animations = nullptr;
		if (!(*ListData)->TryGetArrayField(TEXT("animations"), Animations) || !Animations)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& AnimationValue : *Animations)
		{
			const TSharedPtr<FJsonObject>* AnimationObject = nullptr;
			if (!AnimationValue.IsValid() || !AnimationValue->TryGetObject(AnimationObject) || !AnimationObject || !AnimationObject->IsValid())
			{
				continue;
			}
			if ((*AnimationObject)->GetStringField(TEXT("name")) == AnimationName)
			{
				bFoundOut = true;
				break;
			}
		}

		return true;
	};

	const FString CreateBody = MakeExecRequestBody(
		TEXT("smoke-umgseq-create-001"),
		TEXT("umg_create_widget_blueprint"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"parent_class\":\"/Script/UMG.UserWidget\",\"create_default_root\":true,\"compile_after_create\":false,\"open_editor\":false,\"save_after_create\":false}"), *WidgetBlueprintAssetPath));
	FHttpSmokeResult CreateResult;
	TestTrue(TEXT("Sequence UMG create widget blueprint request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateBody, CreateResult));
	TestEqual(TEXT("Sequence UMG create widget blueprint status code"), CreateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CreateJson;
	TestTrue(TEXT("Sequence UMG create widget blueprint parses JSON"), ParseJson(CreateResult.Body, CreateJson));
	FString RootWidgetName;
	const TSharedPtr<FJsonObject>* CreateData = nullptr;
	TestTrue(TEXT("Sequence UMG create widget blueprint contains data"), CreateJson.IsValid() && CreateJson->TryGetObjectField(TEXT("data"), CreateData) && CreateData && CreateData->IsValid());
	if (CreateData && CreateData->IsValid())
	{
		RootWidgetName = (*CreateData)->GetStringField(TEXT("root_widget"));
	}

	const FString AddWidgetBody = MakeExecRequestBody(
		TEXT("smoke-umgseq-add-widget-001"),
		TEXT("umg_add_widget"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"widget_class\":\"/Script/UMG.Button\",\"widget_name\":\"%s\",\"parent_widget\":\"%s\",\"make_variable\":true,\"compile_after_add\":false,\"save_after_add\":false}"), *WidgetBlueprintAssetPath, *WidgetName, *RootWidgetName));
	FHttpSmokeResult AddWidgetResult;
	TestTrue(TEXT("Sequence UMG add widget request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddWidgetBody, AddWidgetResult));
	TestEqual(TEXT("Sequence UMG add widget status code"), AddWidgetResult.StatusCode, 200);

	const FString CreateAnimationBody = MakeExecRequestBody(
		TEXT("smoke-umgseq-create-anim-001"),
		TEXT("sequence_create_umg_animation"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"animation_name\":\"%s\",\"start_seconds\":0.0,\"duration_seconds\":1.0,\"compile_after_create\":false,\"save_after_create\":false}"), *WidgetBlueprintAssetPath, *RequestedAnimationName));
	FHttpSmokeResult CreateAnimationResult;
	TestTrue(TEXT("Sequence UMG create animation request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateAnimationBody, CreateAnimationResult));
	TestEqual(TEXT("Sequence UMG create animation status code"), CreateAnimationResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CreateAnimationJson;
	TestTrue(TEXT("Sequence UMG create animation parses JSON"), ParseJson(CreateAnimationResult.Body, CreateAnimationJson));
	FString AnimationName;
	const TSharedPtr<FJsonObject>* CreateAnimationData = nullptr;
	TestTrue(TEXT("Sequence UMG create animation contains data"), CreateAnimationJson.IsValid() && CreateAnimationJson->TryGetObjectField(TEXT("data"), CreateAnimationData) && CreateAnimationData && CreateAnimationData->IsValid());
	if (CreateAnimationData && CreateAnimationData->IsValid())
	{
		AnimationName = (*CreateAnimationData)->GetStringField(TEXT("animation_name"));
		TestTrue(TEXT("Sequence UMG create animation actual name non-empty"), !AnimationName.IsEmpty());
	}

	const FString ListAnimationsBody = MakeExecRequestBody(
		TEXT("smoke-umgseq-list-anim-001"),
		TEXT("sequence_list_umg_animations"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *WidgetBlueprintAssetPath));
	FHttpSmokeResult ListAnimationsResult;
	TestTrue(TEXT("Sequence UMG list animations request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListAnimationsBody, ListAnimationsResult));
	TestEqual(TEXT("Sequence UMG list animations status code"), ListAnimationsResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ListAnimationsJson;
	TestTrue(TEXT("Sequence UMG list animations parses JSON"), ParseJson(ListAnimationsResult.Body, ListAnimationsJson));
	const TSharedPtr<FJsonObject>* ListAnimationsData = nullptr;
	TestTrue(TEXT("Sequence UMG list animations contains data"), ListAnimationsJson.IsValid() && ListAnimationsJson->TryGetObjectField(TEXT("data"), ListAnimationsData) && ListAnimationsData && ListAnimationsData->IsValid());
	if (ListAnimationsData && ListAnimationsData->IsValid())
	{
		bool bFoundAnimation = false;
		TestTrue(TEXT("Sequence UMG list animations array exists"), ListContainsAnimation(ListAnimationsData, AnimationName, bFoundAnimation));
		TestTrue(TEXT("Sequence UMG list animations contains created animation"), bFoundAnimation);
	}

	const FString SetPlaybackRangeBody = MakeExecRequestBody(
		TEXT("smoke-umgseq-range-001"),
		TEXT("sequence_set_umg_animation_playback_range"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"animation_name\":\"%s\",\"start_seconds\":0.1,\"duration_seconds\":1.4,\"save_after_set\":false}"), *WidgetBlueprintAssetPath, *AnimationName));
	FHttpSmokeResult SetPlaybackRangeResult;
	TestTrue(TEXT("Sequence UMG set playback range request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetPlaybackRangeBody, SetPlaybackRangeResult));
	TestEqual(TEXT("Sequence UMG set playback range status code"), SetPlaybackRangeResult.StatusCode, 200);

	const FString SetDisplayRateBody = MakeExecRequestBody(
		TEXT("smoke-umgseq-rate-001"),
		TEXT("sequence_set_umg_animation_display_rate"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"animation_name\":\"%s\",\"display_rate_num\":24,\"display_rate_den\":1,\"save_after_set\":false}"), *WidgetBlueprintAssetPath, *AnimationName));
	FHttpSmokeResult SetDisplayRateResult;
	TestTrue(TEXT("Sequence UMG set display rate request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetDisplayRateBody, SetDisplayRateResult));
	TestEqual(TEXT("Sequence UMG set display rate status code"), SetDisplayRateResult.StatusCode, 200);

	const FString RenameAnimationBody = MakeExecRequestBody(
		TEXT("smoke-umgseq-rename-001"),
		TEXT("sequence_rename_umg_animation"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"animation_name\":\"%s\",\"new_animation_name\":\"%s\",\"compile_after_rename\":false,\"save_after_rename\":false}"), *WidgetBlueprintAssetPath, *AnimationName, *RenamedAnimationName));
	FHttpSmokeResult RenameAnimationResult;
	TestTrue(TEXT("Sequence UMG rename animation request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RenameAnimationBody, RenameAnimationResult));
	TestEqual(TEXT("Sequence UMG rename animation status code"), RenameAnimationResult.StatusCode, 200);
	AnimationName = RenamedAnimationName;

	const FString AddTransformKeyBody = MakeExecRequestBody(
		TEXT("smoke-umgseq-transform-key-001"),
		TEXT("sequence_add_umg_widget_transform_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"animation_name\":\"%s\",\"widget_name\":\"%s\",\"time_seconds\":0.0,\"translation_x\":16.0,\"translation_y\":8.0,\"scale_x\":1.1,\"scale_y\":0.9,\"angle\":12.0,\"compile_after_set\":false,\"save_after_set\":false}"), *WidgetBlueprintAssetPath, *AnimationName, *WidgetName));
	FHttpSmokeResult AddTransformKeyResult;
	TestTrue(TEXT("Sequence UMG add transform key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddTransformKeyBody, AddTransformKeyResult));
	TestEqual(TEXT("Sequence UMG add transform key status code"), AddTransformKeyResult.StatusCode, 200);

	const FString AddOpacityKeyBody = MakeExecRequestBody(
		TEXT("smoke-umgseq-opacity-key-001"),
		TEXT("sequence_add_umg_widget_opacity_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"animation_name\":\"%s\",\"widget_name\":\"%s\",\"time_seconds\":0.25,\"opacity\":0.4,\"compile_after_set\":false,\"save_after_set\":false}"), *WidgetBlueprintAssetPath, *AnimationName, *WidgetName));
	FHttpSmokeResult AddOpacityKeyResult;
	TestTrue(TEXT("Sequence UMG add opacity key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddOpacityKeyBody, AddOpacityKeyResult));
	TestEqual(TEXT("Sequence UMG add opacity key status code"), AddOpacityKeyResult.StatusCode, 200);

	const FString AddColorKeyBody = MakeExecRequestBody(
		TEXT("smoke-umgseq-color-key-001"),
		TEXT("sequence_add_umg_widget_color_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"animation_name\":\"%s\",\"widget_name\":\"%s\",\"time_seconds\":0.5,\"red\":0.2,\"green\":0.4,\"blue\":0.6,\"alpha\":0.8,\"compile_after_set\":false,\"save_after_set\":false}"), *WidgetBlueprintAssetPath, *AnimationName, *WidgetName));
	FHttpSmokeResult AddColorKeyResult;
	TestTrue(TEXT("Sequence UMG add color key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddColorKeyBody, AddColorKeyResult));
	TestEqual(TEXT("Sequence UMG add color key status code"), AddColorKeyResult.StatusCode, 200);

	const FString GetAnimationInfoBody = MakeExecRequestBody(
		TEXT("smoke-umgseq-info-001"),
		TEXT("sequence_get_umg_animation_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"animation_name\":\"%s\"}"), *WidgetBlueprintAssetPath, *AnimationName));
	FHttpSmokeResult GetAnimationInfoResult;
	TestTrue(TEXT("Sequence UMG get animation info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetAnimationInfoBody, GetAnimationInfoResult));
	TestEqual(TEXT("Sequence UMG get animation info status code"), GetAnimationInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetAnimationInfoJson;
	TestTrue(TEXT("Sequence UMG get animation info parses JSON"), ParseJson(GetAnimationInfoResult.Body, GetAnimationInfoJson));
	const TSharedPtr<FJsonObject>* GetAnimationInfoData = nullptr;
	TestTrue(TEXT("Sequence UMG get animation info contains data"), GetAnimationInfoJson.IsValid() && GetAnimationInfoJson->TryGetObjectField(TEXT("data"), GetAnimationInfoData) && GetAnimationInfoData && GetAnimationInfoData->IsValid());
	if (GetAnimationInfoData && GetAnimationInfoData->IsValid())
	{
		TestEqual(TEXT("Sequence UMG info display rate updated"), (*GetAnimationInfoData)->GetStringField(TEXT("display_rate")), FString(TEXT("24 fps")));
		TSharedPtr<FJsonObject> TransformTrackObject;
		TSharedPtr<FJsonObject> OpacityTrackObject;
		TSharedPtr<FJsonObject> ColorTrackObject;
		TestTrue(TEXT("Sequence UMG transform track exists"), FindBindingTrackByProperty(GetAnimationInfoData, WidgetName, TEXT("RenderTransform"), TransformTrackObject));
		TestTrue(TEXT("Sequence UMG opacity track exists"), FindBindingTrackByProperty(GetAnimationInfoData, WidgetName, TEXT("RenderOpacity"), OpacityTrackObject));
		TestTrue(TEXT("Sequence UMG color track exists"), FindBindingTrackByProperty(GetAnimationInfoData, WidgetName, TEXT("ColorAndOpacity"), ColorTrackObject));
		if (ColorTrackObject.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			TestTrue(TEXT("Sequence UMG color track contains sections"), ColorTrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections != nullptr);
			if (Sections && Sections->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* SectionObject = nullptr;
				TestTrue(TEXT("Sequence UMG color section parses"), (*Sections)[0].IsValid() && (*Sections)[0]->TryGetObject(SectionObject) && SectionObject && SectionObject->IsValid());
				if (SectionObject && SectionObject->IsValid())
				{
					TestEqual(TEXT("Sequence UMG color section red key count"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("red_key_count"))), 1);
					TestEqual(TEXT("Sequence UMG color section alpha key count"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("alpha_key_count"))), 1);
				}
			}
		}
	}

	const FString RemoveAnimationBody = MakeExecRequestBody(
		TEXT("smoke-umgseq-remove-001"),
		TEXT("sequence_remove_umg_animation"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"animation_name\":\"%s\",\"compile_after_remove\":false,\"save_after_remove\":false}"), *WidgetBlueprintAssetPath, *AnimationName));
	FHttpSmokeResult RemoveAnimationResult;
	TestTrue(TEXT("Sequence UMG remove animation request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveAnimationBody, RemoveAnimationResult));
	TestEqual(TEXT("Sequence UMG remove animation status code"), RemoveAnimationResult.StatusCode, 200);

	const FString ListAnimationsAfterRemoveBody = MakeExecRequestBody(
		TEXT("smoke-umgseq-list-anim-002"),
		TEXT("sequence_list_umg_animations"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *WidgetBlueprintAssetPath));
	FHttpSmokeResult ListAnimationsAfterRemoveResult;
	TestTrue(TEXT("Sequence UMG list animations after remove request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListAnimationsAfterRemoveBody, ListAnimationsAfterRemoveResult));
	TestEqual(TEXT("Sequence UMG list animations after remove status code"), ListAnimationsAfterRemoveResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ListAnimationsAfterRemoveJson;
	TestTrue(TEXT("Sequence UMG list animations after remove parses JSON"), ParseJson(ListAnimationsAfterRemoveResult.Body, ListAnimationsAfterRemoveJson));
	const TSharedPtr<FJsonObject>* ListAnimationsAfterRemoveData = nullptr;
	TestTrue(TEXT("Sequence UMG list animations after remove contains data"), ListAnimationsAfterRemoveJson.IsValid() && ListAnimationsAfterRemoveJson->TryGetObjectField(TEXT("data"), ListAnimationsAfterRemoveData) && ListAnimationsAfterRemoveData && ListAnimationsAfterRemoveData->IsValid());
	if (ListAnimationsAfterRemoveData && ListAnimationsAfterRemoveData->IsValid())
	{
		bool bFoundAnimationAfterRemove = false;
		TestTrue(TEXT("Sequence UMG list animations after remove array exists"), ListContainsAnimation(ListAnimationsAfterRemoveData, AnimationName, bFoundAnimationAfterRemove));
		TestFalse(TEXT("Sequence UMG animation removed from list"), bFoundAnimationAfterRemove);
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceMontageSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.MontageWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceMontageSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString MontageAssetPath = MakeAutomationAssetPath(TEXT("MNT"));
	const FString SkeletonAssetPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton");
	const FString PreviewMeshAssetPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP");
	const FString IdleAnimationAssetPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle.Tutorial_Idle");
	const FString WalkAnimationAssetPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Walk_Fwd.Tutorial_Walk_Fwd");
	const FString AddedSlotName = TEXT("UpperBody");
	const FString RenamedSlotName = TEXT("UpperBodyAim");
	const FString AddedSectionName = TEXT("Loop");
	const FString RenamedSectionName = TEXT("LoopRenamed");
	const FString NotifyTrackName = TEXT("MontageNotifyTrack");
	const FString NotifyName = TEXT("SmokeNotify");
	const FString SkeletonSlotName = FString::Printf(TEXT("SmokeSlot_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(6));
	const FString RenamedSkeletonSlotName = SkeletonSlotName + TEXT("_Renamed");
	const FString SkeletonSlotGroupName = TEXT("SmokeMontageGroup");
	const FString SyncGroupName = TEXT("SmokeSyncGroup");

	auto ListContainsMontage = [](const TSharedPtr<FJsonObject>* ListData, const FString& AssetPath, bool& bFoundOut) -> bool
	{
		bFoundOut = false;
		if (!ListData || !ListData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!(*ListData)->TryGetArrayField(TEXT("items"), Items) || !Items)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& ItemValue : *Items)
		{
			const TSharedPtr<FJsonObject>* ItemObject = nullptr;
			if (!ItemValue.IsValid() || !ItemValue->TryGetObject(ItemObject) || !ItemObject || !ItemObject->IsValid())
			{
				continue;
			}
			if ((*ItemObject)->GetStringField(TEXT("package_name")) == AssetPath)
			{
				bFoundOut = true;
				break;
			}
		}

		return true;
	};

	auto FindSlotTrackEntry = [](const TSharedPtr<FJsonObject>* InfoData, const FString& SlotName, TSharedPtr<FJsonObject>& OutEntryObject) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* SlotTracks = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("slot_tracks"), SlotTracks) || !SlotTracks)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& SlotTrackValue : *SlotTracks)
		{
			const TSharedPtr<FJsonObject>* SlotTrackObject = nullptr;
			if (!SlotTrackValue.IsValid() || !SlotTrackValue->TryGetObject(SlotTrackObject) || !SlotTrackObject || !SlotTrackObject->IsValid())
			{
				continue;
			}
			if ((*SlotTrackObject)->GetStringField(TEXT("slot_name")) == SlotName)
			{
				OutEntryObject = *SlotTrackObject;
				return true;
			}
		}

		return false;
	};

	auto FindSectionEntry = [](const TSharedPtr<FJsonObject>* InfoData, const FString& SectionName, TSharedPtr<FJsonObject>& OutEntryObject) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("sections"), Sections) || !Sections)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
		{
			const TSharedPtr<FJsonObject>* SectionObject = nullptr;
			if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObject) || !SectionObject || !SectionObject->IsValid())
			{
				continue;
			}
			if ((*SectionObject)->GetStringField(TEXT("section_name")) == SectionName)
			{
				OutEntryObject = *SectionObject;
				return true;
			}
		}

		return false;
	};

	auto FindNotifyTrackEntry = [](const TSharedPtr<FJsonObject>* InfoData, const FString& TrackName, TSharedPtr<FJsonObject>& OutEntryObject) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("notify_tracks"), Tracks) || !Tracks)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& TrackValue : *Tracks)
		{
			const TSharedPtr<FJsonObject>* TrackObject = nullptr;
			if (!TrackValue.IsValid() || !TrackValue->TryGetObject(TrackObject) || !TrackObject || !TrackObject->IsValid())
			{
				continue;
			}
			if ((*TrackObject)->GetStringField(TEXT("track_name")) == TrackName)
			{
				OutEntryObject = *TrackObject;
				return true;
			}
		}

		return false;
	};

	auto FindNotifyEntry = [](const TSharedPtr<FJsonObject>* InfoData, const FString& NotifyNameNeedle, const bool bRequireState, TSharedPtr<FJsonObject>& OutEntryObject) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Notifies = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("notifies"), Notifies) || !Notifies)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& NotifyValue : *Notifies)
		{
			const TSharedPtr<FJsonObject>* NotifyObject = nullptr;
			if (!NotifyValue.IsValid() || !NotifyValue->TryGetObject(NotifyObject) || !NotifyObject || !NotifyObject->IsValid())
			{
				continue;
			}
			if ((*NotifyObject)->GetStringField(TEXT("notify_name")) == NotifyNameNeedle
				&& (*NotifyObject)->GetBoolField(TEXT("is_state")) == bRequireState)
			{
				OutEntryObject = *NotifyObject;
				return true;
			}
		}

		return false;
	};

	auto SkeletonGroupContainsSlot = [](const TSharedPtr<FJsonObject>* DataObject, const FString& GroupName, const FString& SlotName, bool& bFoundOut) -> bool
	{
		bFoundOut = false;
		if (!DataObject || !DataObject->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* SlotGroups = nullptr;
		if (!(*DataObject)->TryGetArrayField(TEXT("slot_groups"), SlotGroups) || !SlotGroups)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& GroupValue : *SlotGroups)
		{
			const TSharedPtr<FJsonObject>* GroupObject = nullptr;
			if (!GroupValue.IsValid() || !GroupValue->TryGetObject(GroupObject) || !GroupObject || !GroupObject->IsValid())
			{
				continue;
			}
			if ((*GroupObject)->GetStringField(TEXT("group_name")) != GroupName)
			{
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* SlotNames = nullptr;
			if (!(*GroupObject)->TryGetArrayField(TEXT("slot_names"), SlotNames) || !SlotNames)
			{
				return false;
			}
			for (const TSharedPtr<FJsonValue>& SlotValue : *SlotNames)
			{
				if (SlotValue.IsValid() && SlotValue->AsString() == SlotName)
				{
					bFoundOut = true;
					return true;
				}
			}
			return true;
		}

		return true;
	};

	const FString CreateMontageBody = MakeExecRequestBody(
		TEXT("smoke-montage-create-001"),
		TEXT("montage_create"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"target_skeleton\":\"%s\",\"source_animation\":\"%s\",\"preview_skeletal_mesh\":\"%s\",\"open_editor\":false,\"save_after_create\":false}"), *MontageAssetPath, *SkeletonAssetPath, *IdleAnimationAssetPath, *PreviewMeshAssetPath));
	FHttpSmokeResult CreateMontageResult;
	TestTrue(TEXT("Montage create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateMontageBody, CreateMontageResult));
	TestEqual(TEXT("Montage create status code"), CreateMontageResult.StatusCode, 200);

	const FString ListMontagesBody = MakeExecRequestBody(
		TEXT("smoke-montage-list-001"),
		TEXT("montage_list_montages"),
		TEXT("{\"root_path\":\"/Game/__UeAgentInterfaceSmoke\",\"limit\":64}"));
	FHttpSmokeResult ListMontagesResult;
	TestTrue(TEXT("Montage list request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListMontagesBody, ListMontagesResult));
	TestEqual(TEXT("Montage list status code"), ListMontagesResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ListMontagesJson;
	TestTrue(TEXT("Montage list parses JSON"), ParseJson(ListMontagesResult.Body, ListMontagesJson));
	const TSharedPtr<FJsonObject>* ListMontagesData = nullptr;
	TestTrue(TEXT("Montage list contains data"), ListMontagesJson.IsValid() && ListMontagesJson->TryGetObjectField(TEXT("data"), ListMontagesData) && ListMontagesData && ListMontagesData->IsValid());
	if (ListMontagesData && ListMontagesData->IsValid())
	{
		bool bFoundMontage = false;
		TestTrue(TEXT("Montage list items array exists"), ListContainsMontage(ListMontagesData, MontageAssetPath, bFoundMontage));
		TestTrue(TEXT("Montage list contains created montage"), bFoundMontage);
	}

	const FString ClearPreviewMeshBody = MakeExecRequestBody(
		TEXT("smoke-montage-preview-clear-001"),
		TEXT("montage_set_preview_mesh"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clear_preview_mesh\":true,\"save_after_set\":false}"), *MontageAssetPath));
	FHttpSmokeResult ClearPreviewMeshResult;
	TestTrue(TEXT("Montage clear preview mesh request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ClearPreviewMeshBody, ClearPreviewMeshResult));
	TestEqual(TEXT("Montage clear preview mesh status code"), ClearPreviewMeshResult.StatusCode, 200);

	const FString RestorePreviewMeshBody = MakeExecRequestBody(
		TEXT("smoke-montage-preview-restore-001"),
		TEXT("montage_set_preview_mesh"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"skeletal_mesh_path\":\"%s\",\"save_after_set\":false}"), *MontageAssetPath, *PreviewMeshAssetPath));
	FHttpSmokeResult RestorePreviewMeshResult;
	TestTrue(TEXT("Montage restore preview mesh request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RestorePreviewMeshBody, RestorePreviewMeshResult));
	TestEqual(TEXT("Montage restore preview mesh status code"), RestorePreviewMeshResult.StatusCode, 200);

	const FString AddSlotTrackBody = MakeExecRequestBody(
		TEXT("smoke-montage-add-slot-001"),
		TEXT("montage_add_slot_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"slot_name\":\"%s\",\"save_after_add\":false}"), *MontageAssetPath, *AddedSlotName));
	FHttpSmokeResult AddSlotTrackResult;
	TestTrue(TEXT("Montage add slot track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddSlotTrackBody, AddSlotTrackResult));
	TestEqual(TEXT("Montage add slot track status code"), AddSlotTrackResult.StatusCode, 200);

	const FString RenameSlotTrackBody = MakeExecRequestBody(
		TEXT("smoke-montage-rename-slot-001"),
		TEXT("montage_rename_slot_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"slot_name\":\"%s\",\"new_slot_name\":\"%s\",\"save_after_rename\":false}"), *MontageAssetPath, *AddedSlotName, *RenamedSlotName));
	FHttpSmokeResult RenameSlotTrackResult;
	TestTrue(TEXT("Montage rename slot track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RenameSlotTrackBody, RenameSlotTrackResult));
	TestEqual(TEXT("Montage rename slot track status code"), RenameSlotTrackResult.StatusCode, 200);

	const FString AddSegmentBody = MakeExecRequestBody(
		TEXT("smoke-montage-add-segment-001"),
		TEXT("montage_add_segment"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"slot_name\":\"%s\",\"animation_asset\":\"%s\",\"start_pos\":0.0,\"play_rate\":1.0,\"loop_count\":1,\"save_after_add\":false}"), *MontageAssetPath, *RenamedSlotName, *WalkAnimationAssetPath));
	FHttpSmokeResult AddSegmentResult;
	TestTrue(TEXT("Montage add segment request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddSegmentBody, AddSegmentResult));
	TestEqual(TEXT("Montage add segment status code"), AddSegmentResult.StatusCode, 200);

	const FString UpdateSegmentBody = MakeExecRequestBody(
		TEXT("smoke-montage-update-segment-001"),
		TEXT("montage_update_segment"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"slot_name\":\"%s\",\"segment_index\":0,\"play_rate\":1.5,\"loop_count\":2,\"save_after_set\":false}"), *MontageAssetPath, *RenamedSlotName));
	FHttpSmokeResult UpdateSegmentResult;
	TestTrue(TEXT("Montage update segment request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, UpdateSegmentBody, UpdateSegmentResult));
	TestEqual(TEXT("Montage update segment status code"), UpdateSegmentResult.StatusCode, 200);

	const FString AddSectionBody = MakeExecRequestBody(
		TEXT("smoke-montage-add-section-001"),
		TEXT("montage_add_section"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"section_name\":\"%s\",\"time_seconds\":0.5,\"save_after_add\":false}"), *MontageAssetPath, *AddedSectionName));
	FHttpSmokeResult AddSectionResult;
	TestTrue(TEXT("Montage add section request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddSectionBody, AddSectionResult));
	TestEqual(TEXT("Montage add section status code"), AddSectionResult.StatusCode, 200);

	const FString RenameSectionBody = MakeExecRequestBody(
		TEXT("smoke-montage-rename-section-001"),
		TEXT("montage_rename_section"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"section_name\":\"%s\",\"new_section_name\":\"%s\",\"save_after_rename\":false}"), *MontageAssetPath, *AddedSectionName, *RenamedSectionName));
	FHttpSmokeResult RenameSectionResult;
	TestTrue(TEXT("Montage rename section request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RenameSectionBody, RenameSectionResult));
	TestEqual(TEXT("Montage rename section status code"), RenameSectionResult.StatusCode, 200);

	const FString SetSectionTimeBody = MakeExecRequestBody(
		TEXT("smoke-montage-set-section-time-001"),
		TEXT("montage_set_section_time"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"section_name\":\"%s\",\"time_seconds\":0.75,\"save_after_set\":false}"), *MontageAssetPath, *RenamedSectionName));
	FHttpSmokeResult SetSectionTimeResult;
	TestTrue(TEXT("Montage set section time request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetSectionTimeBody, SetSectionTimeResult));
	TestEqual(TEXT("Montage set section time status code"), SetSectionTimeResult.StatusCode, 200);

	const FString SetNextSectionBody = MakeExecRequestBody(
		TEXT("smoke-montage-set-next-001"),
		TEXT("montage_set_next_section"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"section_name\":\"Default\",\"next_section_name\":\"%s\",\"save_after_set\":false}"), *MontageAssetPath, *RenamedSectionName));
	FHttpSmokeResult SetNextSectionResult;
	TestTrue(TEXT("Montage set next section request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetNextSectionBody, SetNextSectionResult));
	TestEqual(TEXT("Montage set next section status code"), SetNextSectionResult.StatusCode, 200);

	const FString SetBlendOptionsBody = MakeExecRequestBody(
		TEXT("smoke-montage-blend-001"),
		TEXT("montage_set_blend_options"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"blend_in_time\":0.15,\"blend_out_time\":0.4,\"enable_auto_blend_out\":false,\"blend_mode_in\":\"standard\",\"blend_mode_out\":\"standard\",\"save_after_set\":false}"), *MontageAssetPath));
	FHttpSmokeResult SetBlendOptionsResult;
	TestTrue(TEXT("Montage set blend options request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetBlendOptionsBody, SetBlendOptionsResult));
	TestEqual(TEXT("Montage set blend options status code"), SetBlendOptionsResult.StatusCode, 200);

	const FString AddNotifyTrackBody = MakeExecRequestBody(
		TEXT("smoke-montage-add-notify-track-001"),
		TEXT("montage_add_notify_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"track_name\":\"%s\",\"track_color\":{\"r\":0.15,\"g\":0.35,\"b\":0.65,\"a\":1.0},\"save_after_add\":false}"), *MontageAssetPath, *NotifyTrackName));
	FHttpSmokeResult AddNotifyTrackResult;
	TestTrue(TEXT("Montage add notify track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddNotifyTrackBody, AddNotifyTrackResult));
	TestEqual(TEXT("Montage add notify track status code"), AddNotifyTrackResult.StatusCode, 200);

	const FString AddNotifyBody = MakeExecRequestBody(
		TEXT("smoke-montage-add-notify-001"),
		TEXT("montage_add_notify"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"track_name\":\"%s\",\"notify_name\":\"%s\",\"time_seconds\":0.2,\"notify_color\":{\"r\":0.20,\"g\":0.80,\"b\":0.25,\"a\":1.0},\"trigger_weight_threshold\":0.35,\"notify_trigger_chance\":0.55,\"notify_filter_type\":\"lod\",\"notify_filter_lod\":2,\"can_be_filtered_via_request\":true,\"trigger_on_dedicated_server\":false,\"trigger_on_follower\":true,\"save_after_add\":false}"), *MontageAssetPath, *NotifyTrackName, *NotifyName));
	FHttpSmokeResult AddNotifyResult;
	TestTrue(TEXT("Montage add notify request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddNotifyBody, AddNotifyResult));
	TestEqual(TEXT("Montage add notify status code"), AddNotifyResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddNotifyJson;
	TestTrue(TEXT("Montage add notify parses JSON"), ParseJson(AddNotifyResult.Body, AddNotifyJson));
	int32 NotifyIndex = INDEX_NONE;
	const TSharedPtr<FJsonObject>* AddNotifyData = nullptr;
	TestTrue(TEXT("Montage add notify contains data"), AddNotifyJson.IsValid() && AddNotifyJson->TryGetObjectField(TEXT("data"), AddNotifyData) && AddNotifyData && AddNotifyData->IsValid());
	if (AddNotifyData && AddNotifyData->IsValid())
	{
		NotifyIndex = static_cast<int32>((*AddNotifyData)->GetIntegerField(TEXT("notify_index")));
	}

	const FString AddNotifyStateBody = MakeExecRequestBody(
		TEXT("smoke-montage-add-notify-state-001"),
		TEXT("montage_add_notify_state"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"track_name\":\"%s\",\"notify_state_class\":\"/Script/Engine.AnimNotifyState_DisableRootMotion\",\"time_seconds\":0.3,\"duration_seconds\":0.4,\"notify_color\":{\"r\":0.20,\"g\":0.40,\"b\":0.90,\"a\":1.0},\"trigger_weight_threshold\":0.25,\"notify_trigger_chance\":0.90,\"notify_filter_type\":\"none\",\"can_be_filtered_via_request\":false,\"trigger_on_dedicated_server\":true,\"trigger_on_follower\":false,\"save_after_add\":false}"), *MontageAssetPath, *NotifyTrackName));
	FHttpSmokeResult AddNotifyStateResult;
	TestTrue(TEXT("Montage add notify state request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddNotifyStateBody, AddNotifyStateResult));
	TestEqual(TEXT("Montage add notify state status code"), AddNotifyStateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddNotifyStateJson;
	TestTrue(TEXT("Montage add notify state parses JSON"), ParseJson(AddNotifyStateResult.Body, AddNotifyStateJson));
	int32 NotifyStateIndex = INDEX_NONE;
	FString NotifyStateName;
	const TSharedPtr<FJsonObject>* AddNotifyStateData = nullptr;
	TestTrue(TEXT("Montage add notify state contains data"), AddNotifyStateJson.IsValid() && AddNotifyStateJson->TryGetObjectField(TEXT("data"), AddNotifyStateData) && AddNotifyStateData && AddNotifyStateData->IsValid());
	if (AddNotifyStateData && AddNotifyStateData->IsValid())
	{
		NotifyStateIndex = static_cast<int32>((*AddNotifyStateData)->GetIntegerField(TEXT("notify_index")));
		NotifyStateName = (*AddNotifyStateData)->GetStringField(TEXT("notify_name"));
	}

	const FString UpdateNotifyBody = MakeExecRequestBody(
		TEXT("smoke-montage-update-notify-001"),
		TEXT("montage_update_notify"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"notify_index\":%d,\"time_seconds\":0.25,\"tick_type\":\"branching_point\",\"notify_color\":{\"r\":0.90,\"g\":0.30,\"b\":0.10,\"a\":1.0},\"trigger_weight_threshold\":0.60,\"notify_trigger_chance\":0.80,\"notify_filter_type\":\"lod\",\"notify_filter_lod\":1,\"can_be_filtered_via_request\":true,\"trigger_on_dedicated_server\":false,\"trigger_on_follower\":true,\"save_after_set\":false}"), *MontageAssetPath, NotifyIndex));
	FHttpSmokeResult UpdateNotifyResult;
	TestTrue(TEXT("Montage update notify request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, UpdateNotifyBody, UpdateNotifyResult));
	TestEqual(TEXT("Montage update notify status code"), UpdateNotifyResult.StatusCode, 200);

	const FString SetSkeletonSlotGroupBody = MakeExecRequestBody(
		TEXT("smoke-montage-set-skeleton-slot-group-001"),
		TEXT("montage_set_skeleton_slot_group"),
		FString::Printf(TEXT("{\"skeleton_path\":\"%s\",\"slot_name\":\"%s\",\"group_name\":\"%s\",\"save_skeleton\":false}"), *SkeletonAssetPath, *SkeletonSlotName, *SkeletonSlotGroupName));
	FHttpSmokeResult SetSkeletonSlotGroupResult;
	TestTrue(TEXT("Montage set skeleton slot group request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetSkeletonSlotGroupBody, SetSkeletonSlotGroupResult));
	TestEqual(TEXT("Montage set skeleton slot group status code"), SetSkeletonSlotGroupResult.StatusCode, 200);

	const FString RenameSkeletonSlotBody = MakeExecRequestBody(
		TEXT("smoke-montage-rename-skeleton-slot-001"),
		TEXT("montage_rename_skeleton_slot"),
		FString::Printf(TEXT("{\"skeleton_path\":\"%s\",\"slot_name\":\"%s\",\"new_slot_name\":\"%s\",\"save_skeleton\":false}"), *SkeletonAssetPath, *SkeletonSlotName, *RenamedSkeletonSlotName));
	FHttpSmokeResult RenameSkeletonSlotResult;
	TestTrue(TEXT("Montage rename skeleton slot request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RenameSkeletonSlotBody, RenameSkeletonSlotResult));
	TestEqual(TEXT("Montage rename skeleton slot status code"), RenameSkeletonSlotResult.StatusCode, 200);

	const FString ListSkeletonSlotsBody = MakeExecRequestBody(
		TEXT("smoke-montage-list-skeleton-slots-001"),
		TEXT("montage_list_skeleton_slots"),
		FString::Printf(TEXT("{\"skeleton_path\":\"%s\"}"), *SkeletonAssetPath));
	FHttpSmokeResult ListSkeletonSlotsResult;
	TestTrue(TEXT("Montage list skeleton slots request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListSkeletonSlotsBody, ListSkeletonSlotsResult));
	TestEqual(TEXT("Montage list skeleton slots status code"), ListSkeletonSlotsResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ListSkeletonSlotsJson;
	TestTrue(TEXT("Montage list skeleton slots parses JSON"), ParseJson(ListSkeletonSlotsResult.Body, ListSkeletonSlotsJson));
	const TSharedPtr<FJsonObject>* ListSkeletonSlotsData = nullptr;
	TestTrue(TEXT("Montage list skeleton slots contains data"), ListSkeletonSlotsJson.IsValid() && ListSkeletonSlotsJson->TryGetObjectField(TEXT("data"), ListSkeletonSlotsData) && ListSkeletonSlotsData && ListSkeletonSlotsData->IsValid());
	if (ListSkeletonSlotsData && ListSkeletonSlotsData->IsValid())
	{
		bool bSkeletonSlotFound = false;
		TestTrue(TEXT("Montage skeleton slot group query succeeds"), SkeletonGroupContainsSlot(ListSkeletonSlotsData, SkeletonSlotGroupName, RenamedSkeletonSlotName, bSkeletonSlotFound));
		TestTrue(TEXT("Montage skeleton slot found in target group"), bSkeletonSlotFound);
	}

	const FString SetSyncGroupBody = MakeExecRequestBody(
		TEXT("smoke-montage-set-sync-group-001"),
		TEXT("montage_set_sync_group"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"sync_group_name\":\"%s\",\"sync_slot_name\":\"%s\",\"save_after_set\":false}"), *MontageAssetPath, *SyncGroupName, *RenamedSlotName));
	FHttpSmokeResult SetSyncGroupResult;
	TestTrue(TEXT("Montage set sync group request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetSyncGroupBody, SetSyncGroupResult));
	TestEqual(TEXT("Montage set sync group status code"), SetSyncGroupResult.StatusCode, 200);

	const FString GetInfoBody = MakeExecRequestBody(
		TEXT("smoke-montage-info-001"),
		TEXT("montage_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *MontageAssetPath));
	FHttpSmokeResult GetInfoResult;
	TestTrue(TEXT("Montage get info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoBody, GetInfoResult));
	TestEqual(TEXT("Montage get info status code"), GetInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoJson;
	TestTrue(TEXT("Montage get info parses JSON"), ParseJson(GetInfoResult.Body, GetInfoJson));
	const TSharedPtr<FJsonObject>* GetInfoData = nullptr;
	TestTrue(TEXT("Montage get info contains data"), GetInfoJson.IsValid() && GetInfoJson->TryGetObjectField(TEXT("data"), GetInfoData) && GetInfoData && GetInfoData->IsValid());
	if (GetInfoData && GetInfoData->IsValid())
	{
		TestEqual(TEXT("Montage preview mesh matches"), (*GetInfoData)->GetStringField(TEXT("preview_skeletal_mesh")), PreviewMeshAssetPath);
		TestEqual(TEXT("Montage auto blend out updated"), (*GetInfoData)->GetBoolField(TEXT("enable_auto_blend_out")), false);
		TestEqual(TEXT("Montage sync group updated"), (*GetInfoData)->GetStringField(TEXT("sync_group_name")), SyncGroupName);
		TestEqual(TEXT("Montage sync slot updated"), (*GetInfoData)->GetStringField(TEXT("sync_slot_name")), RenamedSlotName);
		TestEqual(TEXT("Montage sync slot valid"), (*GetInfoData)->GetBoolField(TEXT("sync_slot_valid")), true);
		TestTrue(TEXT("Montage sync marker count is non-negative"), static_cast<int32>((*GetInfoData)->GetIntegerField(TEXT("sync_marker_count"))) >= 0);

		TSharedPtr<FJsonObject> SlotTrackEntry;
		TSharedPtr<FJsonObject> SectionEntry;
		TSharedPtr<FJsonObject> NotifyTrackEntry;
		TSharedPtr<FJsonObject> NotifyEntry;
		TSharedPtr<FJsonObject> NotifyStateEntry;
		TestTrue(TEXT("Montage renamed slot exists"), FindSlotTrackEntry(GetInfoData, RenamedSlotName, SlotTrackEntry));
		TestTrue(TEXT("Montage renamed section exists"), FindSectionEntry(GetInfoData, RenamedSectionName, SectionEntry));
		TestTrue(TEXT("Montage notify track exists"), FindNotifyTrackEntry(GetInfoData, NotifyTrackName, NotifyTrackEntry));
		TestTrue(TEXT("Montage named notify exists"), FindNotifyEntry(GetInfoData, NotifyName, false, NotifyEntry));
		TestTrue(TEXT("Montage notify state exists"), FindNotifyEntry(GetInfoData, NotifyStateName, true, NotifyStateEntry));
		if (SlotTrackEntry.IsValid())
		{
			TestEqual(TEXT("Montage renamed slot segment count"), static_cast<int32>(SlotTrackEntry->GetIntegerField(TEXT("segment_count"))), 1);
			TestEqual(TEXT("Montage renamed slot registered on skeleton"), SlotTrackEntry->GetBoolField(TEXT("skeleton_slot_registered")), false);
		}
		if (SectionEntry.IsValid())
		{
			TestEqual(TEXT("Montage section next name remains empty"), SectionEntry->GetStringField(TEXT("next_section_name")), FString(TEXT("")));
			TestEqual(TEXT("Montage section time updated"), static_cast<float>(SectionEntry->GetNumberField(TEXT("time_seconds"))), 0.75f);
		}

		TSharedPtr<FJsonObject> DefaultSectionEntry;
		TestTrue(TEXT("Montage default section exists"), FindSectionEntry(GetInfoData, TEXT("Default"), DefaultSectionEntry));
		if (DefaultSectionEntry.IsValid())
		{
			TestEqual(TEXT("Montage default section next points to renamed section"), DefaultSectionEntry->GetStringField(TEXT("next_section_name")), RenamedSectionName);
		}
		if (NotifyTrackEntry.IsValid())
		{
			TestEqual(TEXT("Montage notify track count updated"), static_cast<int32>(NotifyTrackEntry->GetIntegerField(TEXT("notify_count"))), 2);
			CheckSmokeLinearColorField(this, NotifyTrackEntry, TEXT("track_color_linear"), FLinearColor(0.15f, 0.35f, 0.65f, 1.0f), TEXT("Montage notify track"));
		}
		if (NotifyEntry.IsValid())
		{
			TestEqual(TEXT("Montage notify updated to branching point"), NotifyEntry->GetBoolField(TEXT("branching_point")), true);
			TestEqual(TEXT("Montage notify tick type updated"), NotifyEntry->GetStringField(TEXT("tick_type")), FString(TEXT("branching_point")));
			TestEqual(TEXT("Montage notify filter type updated"), NotifyEntry->GetStringField(TEXT("notify_filter_type")), FString(TEXT("lod")));
			TestEqual(TEXT("Montage notify filter lod updated"), static_cast<int32>(NotifyEntry->GetIntegerField(TEXT("notify_filter_lod"))), 1);
			TestEqual(TEXT("Montage notify trigger weight threshold updated"), static_cast<float>(NotifyEntry->GetNumberField(TEXT("trigger_weight_threshold"))), 0.60f);
			TestEqual(TEXT("Montage notify trigger chance updated"), static_cast<float>(NotifyEntry->GetNumberField(TEXT("notify_trigger_chance"))), 0.80f);
			TestEqual(TEXT("Montage notify can be filtered via request updated"), NotifyEntry->GetBoolField(TEXT("can_be_filtered_via_request")), true);
			TestEqual(TEXT("Montage notify trigger on dedicated server updated"), NotifyEntry->GetBoolField(TEXT("trigger_on_dedicated_server")), false);
			TestEqual(TEXT("Montage notify trigger on follower updated"), NotifyEntry->GetBoolField(TEXT("trigger_on_follower")), true);
			CheckSmokeLinearColorField(this, NotifyEntry, TEXT("notify_color_linear"), FLinearColor(0.90f, 0.30f, 0.10f, 1.0f), TEXT("Montage notify"));
		}
		if (NotifyStateEntry.IsValid())
		{
			TestEqual(TEXT("Montage notify state duration updated"), static_cast<float>(NotifyStateEntry->GetNumberField(TEXT("duration_seconds"))), 0.4f);
			TestEqual(TEXT("Montage notify state filter type updated"), NotifyStateEntry->GetStringField(TEXT("notify_filter_type")), FString(TEXT("none")));
			TestEqual(TEXT("Montage notify state trigger chance updated"), static_cast<float>(NotifyStateEntry->GetNumberField(TEXT("notify_trigger_chance"))), 0.90f);
			TestEqual(TEXT("Montage notify state can be filtered via request updated"), NotifyStateEntry->GetBoolField(TEXT("can_be_filtered_via_request")), false);
			TestEqual(TEXT("Montage notify state trigger on dedicated server updated"), NotifyStateEntry->GetBoolField(TEXT("trigger_on_dedicated_server")), true);
			TestEqual(TEXT("Montage notify state trigger on follower updated"), NotifyStateEntry->GetBoolField(TEXT("trigger_on_follower")), false);
			CheckSmokeLinearColorField(this, NotifyStateEntry, TEXT("notify_color_linear"), FLinearColor(0.20f, 0.40f, 0.90f, 1.0f), TEXT("Montage notify state"));
		}
	}

	const int32 RemoveNotifyStateFirstIndex = FMath::Max(NotifyIndex, NotifyStateIndex);
	const int32 RemoveNotifySecondIndex = FMath::Min(NotifyIndex, NotifyStateIndex);

	const FString RemoveNotifyStateBody = MakeExecRequestBody(
		TEXT("smoke-montage-remove-notify-state-001"),
		TEXT("montage_remove_notify"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"notify_index\":%d,\"save_after_remove\":false}"), *MontageAssetPath, RemoveNotifyStateFirstIndex));
	FHttpSmokeResult RemoveNotifyStateResult;
	TestTrue(TEXT("Montage remove notify state request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveNotifyStateBody, RemoveNotifyStateResult));
	TestEqual(TEXT("Montage remove notify state status code"), RemoveNotifyStateResult.StatusCode, 200);

	const FString RemoveNotifyBody = MakeExecRequestBody(
		TEXT("smoke-montage-remove-notify-001"),
		TEXT("montage_remove_notify"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"notify_index\":%d,\"save_after_remove\":false}"), *MontageAssetPath, RemoveNotifySecondIndex == RemoveNotifyStateFirstIndex ? 0 : RemoveNotifySecondIndex));
	FHttpSmokeResult RemoveNotifyResult;
	TestTrue(TEXT("Montage remove notify request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveNotifyBody, RemoveNotifyResult));
	TestEqual(TEXT("Montage remove notify status code"), RemoveNotifyResult.StatusCode, 200);

	const FString RemoveNotifyTrackBody = MakeExecRequestBody(
		TEXT("smoke-montage-remove-notify-track-001"),
		TEXT("montage_remove_notify_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"track_name\":\"%s\",\"save_after_remove\":false}"), *MontageAssetPath, *NotifyTrackName));
	FHttpSmokeResult RemoveNotifyTrackResult;
	TestTrue(TEXT("Montage remove notify track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveNotifyTrackBody, RemoveNotifyTrackResult));
	TestEqual(TEXT("Montage remove notify track status code"), RemoveNotifyTrackResult.StatusCode, 200);

	const FString RemoveSkeletonSlotBody = MakeExecRequestBody(
		TEXT("smoke-montage-remove-skeleton-slot-001"),
		TEXT("montage_remove_skeleton_slot"),
		FString::Printf(TEXT("{\"skeleton_path\":\"%s\",\"slot_name\":\"%s\",\"save_skeleton\":false}"), *SkeletonAssetPath, *RenamedSkeletonSlotName));
	FHttpSmokeResult RemoveSkeletonSlotResult;
	TestTrue(TEXT("Montage remove skeleton slot request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveSkeletonSlotBody, RemoveSkeletonSlotResult));
	TestEqual(TEXT("Montage remove skeleton slot status code"), RemoveSkeletonSlotResult.StatusCode, 200);

	const FString ResolveSkeletonDirtyBody = MakeExecRequestBody(
		TEXT("smoke-montage-resolve-skeleton-dirty-001"),
		TEXT("editor_resolve_dirty_resources"),
		FString::Printf(TEXT("{\"discard_resource_paths\":[\"%s\"]}"), *SkeletonAssetPath));
	FHttpSmokeResult ResolveSkeletonDirtyResult;
	TestTrue(TEXT("Montage resolve skeleton dirty request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ResolveSkeletonDirtyBody, ResolveSkeletonDirtyResult));
	TestEqual(TEXT("Montage resolve skeleton dirty status code"), ResolveSkeletonDirtyResult.StatusCode, 200);

	const FString RemoveSectionBody = MakeExecRequestBody(
		TEXT("smoke-montage-remove-section-001"),
		TEXT("montage_remove_section"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"section_name\":\"%s\",\"save_after_remove\":false}"), *MontageAssetPath, *RenamedSectionName));
	FHttpSmokeResult RemoveSectionResult;
	TestTrue(TEXT("Montage remove section request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveSectionBody, RemoveSectionResult));
	TestEqual(TEXT("Montage remove section status code"), RemoveSectionResult.StatusCode, 200);

	const FString RemoveSegmentBody = MakeExecRequestBody(
		TEXT("smoke-montage-remove-segment-001"),
		TEXT("montage_remove_segment"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"slot_name\":\"%s\",\"segment_index\":0,\"save_after_remove\":false}"), *MontageAssetPath, *RenamedSlotName));
	FHttpSmokeResult RemoveSegmentResult;
	TestTrue(TEXT("Montage remove segment request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveSegmentBody, RemoveSegmentResult));
	TestEqual(TEXT("Montage remove segment status code"), RemoveSegmentResult.StatusCode, 200);

	const FString RemoveSlotTrackBody = MakeExecRequestBody(
		TEXT("smoke-montage-remove-slot-001"),
		TEXT("montage_remove_slot_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"slot_name\":\"%s\",\"save_after_remove\":false}"), *MontageAssetPath, *RenamedSlotName));
	FHttpSmokeResult RemoveSlotTrackResult;
	TestTrue(TEXT("Montage remove slot track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveSlotTrackBody, RemoveSlotTrackResult));
	TestEqual(TEXT("Montage remove slot track status code"), RemoveSlotTrackResult.StatusCode, 200);

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceMontageJsonSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.MontageJsonWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceMontageJsonSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString MontageAssetPath = MakeAutomationAssetPath(TEXT("MNTJSON"));
	const FString SkeletonAssetPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton");
	const FString PreviewMeshAssetPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP");
	const FString IdleAnimationAssetPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle.Tutorial_Idle");
	const FString WalkAnimationAssetPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Walk_Fwd.Tutorial_Walk_Fwd");
	const FString AddedSlotName = TEXT("JsonUpperBody");
	const FString AddedSectionName = TEXT("JsonLoop");
	const FString NotifyTrackName = TEXT("JsonNotifyTrack");
	const FString NotifyName = TEXT("JsonNotify");
	const FString SyncGroupName = TEXT("JsonSyncGroup");
	const FString JsonDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("MontageJson"));
	const FString JsonPath = FPaths::Combine(JsonDir, TEXT("Montage.json"));

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

	IFileManager::Get().MakeDirectory(*JsonDir, true);

	const FString CreateMontageBody = MakeExecRequestBody(
		TEXT("smoke-montage-json-create-001"),
		TEXT("montage_create"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"target_skeleton\":\"%s\",\"source_animation\":\"%s\",\"preview_skeletal_mesh\":\"%s\",\"open_editor\":false,\"save_after_create\":false}"), *MontageAssetPath, *SkeletonAssetPath, *IdleAnimationAssetPath, *PreviewMeshAssetPath));
	FHttpSmokeResult CreateMontageResult;
	TestTrue(TEXT("Montage json create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateMontageBody, CreateMontageResult));
	TestEqual(TEXT("Montage json create status code"), CreateMontageResult.StatusCode, 200);

	const FString ExportBody = MakeExecRequestBody(
		TEXT("smoke-montage-json-export-001"),
		TEXT("montage_export_json"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"output_file\":\"%s\"}"), *MontageAssetPath, *JsonPath.Replace(TEXT("\\"), TEXT("/"))));
	FHttpSmokeResult ExportResult;
	TestTrue(TEXT("Montage json export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ExportBody, ExportResult));
	TestEqual(TEXT("Montage json export status code"), ExportResult.StatusCode, 200);

	TSharedPtr<FJsonObject> MontageJson;
	TestTrue(TEXT("Montage json file loads"), LoadJsonFile(JsonPath, MontageJson));
	if (MontageJson.IsValid())
	{
		MontageJson->SetArrayField(TEXT("skeleton_slots"), TArray<TSharedPtr<FJsonValue>>());

		const TSharedPtr<FJsonObject>* SettingsObj = nullptr;
		TestTrue(TEXT("Montage json settings exists"), MontageJson->TryGetObjectField(TEXT("settings"), SettingsObj) && SettingsObj && (*SettingsObj).IsValid());
		if (SettingsObj && (*SettingsObj).IsValid())
		{
			(*SettingsObj)->SetNumberField(TEXT("blend_in_time"), 0.11);
			(*SettingsObj)->SetNumberField(TEXT("blend_out_time"), 0.33);
			(*SettingsObj)->SetBoolField(TEXT("enable_auto_blend_out"), false);
		}

		const TSharedPtr<FJsonObject>* SyncObj = nullptr;
		TestTrue(TEXT("Montage json sync exists"), MontageJson->TryGetObjectField(TEXT("sync"), SyncObj) && SyncObj && (*SyncObj).IsValid());
		if (SyncObj && (*SyncObj).IsValid())
		{
			(*SyncObj)->SetStringField(TEXT("sync_group_name"), SyncGroupName);
			(*SyncObj)->SetStringField(TEXT("sync_slot_name"), AddedSlotName);
		}

		const TArray<TSharedPtr<FJsonValue>>* SlotTracks = nullptr;
		TestTrue(TEXT("Montage json slot tracks exists"), MontageJson->TryGetArrayField(TEXT("slot_tracks"), SlotTracks) && SlotTracks != nullptr);
		if (SlotTracks)
		{
			TSharedPtr<FJsonObject> AddedSlotObj = MakeShared<FJsonObject>();
			AddedSlotObj->SetStringField(TEXT("slot_name"), AddedSlotName);
			TArray<TSharedPtr<FJsonValue>> Segments;
			TSharedPtr<FJsonObject> SegmentObj = MakeShared<FJsonObject>();
			SegmentObj->SetStringField(TEXT("animation_asset"), WalkAnimationAssetPath);
			SegmentObj->SetNumberField(TEXT("start_pos"), 0.0);
			SegmentObj->SetNumberField(TEXT("anim_start_time"), 0.0);
			SegmentObj->SetNumberField(TEXT("anim_end_time"), 0.6);
			SegmentObj->SetNumberField(TEXT("play_rate"), 1.25);
			SegmentObj->SetNumberField(TEXT("loop_count"), 1);
			Segments.Add(MakeShared<FJsonValueObject>(SegmentObj));
			AddedSlotObj->SetArrayField(TEXT("segments"), Segments);
			const_cast<TArray<TSharedPtr<FJsonValue>>*>(SlotTracks)->Add(MakeShared<FJsonValueObject>(AddedSlotObj));
		}

		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		TestTrue(TEXT("Montage json sections exists"), MontageJson->TryGetArrayField(TEXT("sections"), Sections) && Sections != nullptr);
		if (Sections)
		{
			for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
			{
				const TSharedPtr<FJsonObject>* SectionObj = nullptr;
				if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObj) || !SectionObj || !(*SectionObj).IsValid())
				{
					continue;
				}
				if ((*SectionObj)->GetStringField(TEXT("section_name")) == TEXT("Default"))
				{
					(*SectionObj)->SetStringField(TEXT("next_section_name"), AddedSectionName);
				}
			}

			TSharedPtr<FJsonObject> AddedSectionObj = MakeShared<FJsonObject>();
			AddedSectionObj->SetStringField(TEXT("section_name"), AddedSectionName);
			AddedSectionObj->SetNumberField(TEXT("time_seconds"), 0.4);
			AddedSectionObj->SetStringField(TEXT("next_section_name"), TEXT(""));
			const_cast<TArray<TSharedPtr<FJsonValue>>*>(Sections)->Add(MakeShared<FJsonValueObject>(AddedSectionObj));
		}

		const TArray<TSharedPtr<FJsonValue>>* NotifyTracks = nullptr;
		TestTrue(TEXT("Montage json notify tracks exists"), MontageJson->TryGetArrayField(TEXT("notify_tracks"), NotifyTracks) && NotifyTracks != nullptr);
		if (NotifyTracks)
		{
			TSharedPtr<FJsonObject> TrackObj = MakeShared<FJsonObject>();
			TrackObj->SetStringField(TEXT("track_name"), NotifyTrackName);
			TSharedPtr<FJsonObject> TrackColor = MakeShared<FJsonObject>();
			TrackColor->SetNumberField(TEXT("r"), 0.25);
			TrackColor->SetNumberField(TEXT("g"), 0.5);
			TrackColor->SetNumberField(TEXT("b"), 0.75);
			TrackColor->SetNumberField(TEXT("a"), 1.0);
			TrackObj->SetObjectField(TEXT("track_color_linear"), TrackColor);
			const_cast<TArray<TSharedPtr<FJsonValue>>*>(NotifyTracks)->Add(MakeShared<FJsonValueObject>(TrackObj));
		}

		const TArray<TSharedPtr<FJsonValue>>* Notifies = nullptr;
		TestTrue(TEXT("Montage json notifies exists"), MontageJson->TryGetArrayField(TEXT("notifies"), Notifies) && Notifies != nullptr);
		if (Notifies)
		{
			TSharedPtr<FJsonObject> NotifyObj = MakeShared<FJsonObject>();
			NotifyObj->SetStringField(TEXT("kind"), TEXT("notify"));
			NotifyObj->SetStringField(TEXT("notify_name"), NotifyName);
			NotifyObj->SetStringField(TEXT("track_name"), NotifyTrackName);
			NotifyObj->SetNumberField(TEXT("time_seconds"), 0.2);
			NotifyObj->SetBoolField(TEXT("branching_point"), false);
			TSharedPtr<FJsonObject> NotifyColor = MakeShared<FJsonObject>();
			NotifyColor->SetNumberField(TEXT("r"), 0.8);
			NotifyColor->SetNumberField(TEXT("g"), 0.2);
			NotifyColor->SetNumberField(TEXT("b"), 0.1);
			NotifyColor->SetNumberField(TEXT("a"), 1.0);
			NotifyObj->SetObjectField(TEXT("notify_color_linear"), NotifyColor);
			const_cast<TArray<TSharedPtr<FJsonValue>>*>(Notifies)->Add(MakeShared<FJsonValueObject>(NotifyObj));
		}
	}
	TestTrue(TEXT("Montage json file saves"), SaveJsonFile(JsonPath, MontageJson));

	const FString ApplyBody = MakeExecRequestBody(
		TEXT("smoke-montage-json-apply-001"),
		TEXT("montage_apply_json"),
		FString::Printf(TEXT("{\"json_file\":\"%s\",\"save_after_apply\":false}"), *JsonPath.Replace(TEXT("\\"), TEXT("/"))));
	FHttpSmokeResult ApplyResult;
	TestTrue(TEXT("Montage json apply request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ApplyBody, ApplyResult));
	TestEqual(TEXT("Montage json apply status code"), ApplyResult.StatusCode, 200);

	const FString GetInfoBody = MakeExecRequestBody(
		TEXT("smoke-montage-json-info-001"),
		TEXT("montage_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *MontageAssetPath));
	FHttpSmokeResult GetInfoResult;
	TestTrue(TEXT("Montage json get info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoBody, GetInfoResult));
	TestEqual(TEXT("Montage json get info status code"), GetInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoJson;
	TestTrue(TEXT("Montage json get info parses JSON"), ParseJson(GetInfoResult.Body, GetInfoJson));
	const TSharedPtr<FJsonObject>* GetInfoData = nullptr;
	TestTrue(TEXT("Montage json get info contains data"), GetInfoJson.IsValid() && GetInfoJson->TryGetObjectField(TEXT("data"), GetInfoData) && GetInfoData && GetInfoData->IsValid());
	if (GetInfoData && GetInfoData->IsValid())
	{
		TestEqual(TEXT("Montage json auto blend out updated"), (*GetInfoData)->GetBoolField(TEXT("enable_auto_blend_out")), false);
		TestEqual(TEXT("Montage json sync group updated"), (*GetInfoData)->GetStringField(TEXT("sync_group_name")), SyncGroupName);
		TestEqual(TEXT("Montage json sync slot updated"), (*GetInfoData)->GetStringField(TEXT("sync_slot_name")), AddedSlotName);
		TestTrue(TEXT("Montage json slot track count >= 2"), static_cast<int32>((*GetInfoData)->GetIntegerField(TEXT("slot_track_count"))) >= 2);
		TestTrue(TEXT("Montage json section count >= 2"), static_cast<int32>((*GetInfoData)->GetIntegerField(TEXT("section_count"))) >= 2);

		const TArray<TSharedPtr<FJsonValue>>* SlotTracks = nullptr;
		TestTrue(TEXT("Montage json slot tracks array exists"), (*GetInfoData)->TryGetArrayField(TEXT("slot_tracks"), SlotTracks) && SlotTracks != nullptr);
		bool bFoundAddedSlot = false;
		if (SlotTracks)
		{
			for (const TSharedPtr<FJsonValue>& SlotTrackValue : *SlotTracks)
			{
				const TSharedPtr<FJsonObject>* SlotTrackObj = nullptr;
				if (!SlotTrackValue.IsValid() || !SlotTrackValue->TryGetObject(SlotTrackObj) || !SlotTrackObj || !(*SlotTrackObj).IsValid())
				{
					continue;
				}
				if ((*SlotTrackObj)->GetStringField(TEXT("slot_name")) == AddedSlotName)
				{
					bFoundAddedSlot = true;
				}
			}
		}
		TestTrue(TEXT("Montage json added slot exists"), bFoundAddedSlot);

		const TArray<TSharedPtr<FJsonValue>>* Notifies = nullptr;
		TestTrue(TEXT("Montage json notifies array exists"), (*GetInfoData)->TryGetArrayField(TEXT("notifies"), Notifies) && Notifies != nullptr);
		bool bFoundNotify = false;
		if (Notifies)
		{
			for (const TSharedPtr<FJsonValue>& NotifyValue : *Notifies)
			{
				const TSharedPtr<FJsonObject>* NotifyObj = nullptr;
				if (!NotifyValue.IsValid() || !NotifyValue->TryGetObject(NotifyObj) || !NotifyObj || !(*NotifyObj).IsValid())
				{
					continue;
				}
				if ((*NotifyObj)->GetStringField(TEXT("notify_name")) == NotifyName)
				{
					bFoundNotify = true;
				}
			}
		}
		TestTrue(TEXT("Montage json notify exists"), bFoundNotify);
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceLandscapeSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.LandscapeWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceLandscapeSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString LandscapeLabel = FString::Printf(TEXT("SmokeLandscape_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));

	auto QueryWorldActors = [&]() -> TSharedPtr<FJsonObject>
	{
		const FString ListBody = MakeExecRequestBody(
			TEXT("smoke-landscape-list-actors-001"),
			TEXT("list_actors"),
			TEXT("{\"limit\":2000}"));
		FHttpSmokeResult ListResult;
		TestTrue(TEXT("Landscape list_actors request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListBody, ListResult));
		TestEqual(TEXT("Landscape list_actors status code"), ListResult.StatusCode, 200);

		TSharedPtr<FJsonObject> ListJson;
		TestTrue(TEXT("Landscape list_actors response parses as JSON"), ParseJson(ListResult.Body, ListJson));
		TestTrue(TEXT("Landscape list_actors root ok=true"), ListJson.IsValid() && ListJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* ListData = nullptr;
		TestTrue(TEXT("Landscape list_actors contains data object"), ListJson.IsValid() && ListJson->TryGetObjectField(TEXT("data"), ListData) && ListData && ListData->IsValid());
		return (ListData && ListData->IsValid()) ? *ListData : nullptr;
	};

	auto ListActorsContainsLabel = [&](const FString& Label, bool& bFoundOut)
	{
		const TSharedPtr<FJsonObject> ListData = QueryWorldActors();

		bFoundOut = false;
		if (ListData.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
			TestTrue(TEXT("Landscape list_actors contains actors array"), ListData->TryGetArrayField(TEXT("actors"), Actors) && Actors != nullptr);
			if (Actors)
			{
				for (const TSharedPtr<FJsonValue>& ActorValue : *Actors)
				{
					const TSharedPtr<FJsonObject>* ActorObject = nullptr;
					if (!ActorValue.IsValid() || !ActorValue->TryGetObject(ActorObject) || !ActorObject || !ActorObject->IsValid())
					{
						continue;
					}

					if ((*ActorObject)->GetStringField(TEXT("label")) == Label)
					{
						bFoundOut = true;
						break;
					}
				}
			}
		}
	};

	double LandscapeCenterX = 0.0;
	double LandscapeCenterY = 0.0;
	double LandscapeCenterZ = 0.0;
	FString ActiveLandscapeLabel = LandscapeLabel;
	bool bReusedExistingLandscape = false;

	const TSharedPtr<FJsonObject> InitialActorsData = QueryWorldActors();
	if (InitialActorsData.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
		TestTrue(TEXT("Landscape initial actor query contains actors array"), InitialActorsData->TryGetArrayField(TEXT("actors"), Actors) && Actors != nullptr);
		if (Actors)
		{
			for (const TSharedPtr<FJsonValue>& ActorValue : *Actors)
			{
				const TSharedPtr<FJsonObject>* ActorObject = nullptr;
				if (!ActorValue.IsValid() || !ActorValue->TryGetObject(ActorObject) || !ActorObject || !ActorObject->IsValid())
				{
					continue;
				}

				FString ActorClass;
				(*ActorObject)->TryGetStringField(TEXT("class"), ActorClass);
				if (!ActorClass.Contains(TEXT("Landscape")))
				{
					continue;
				}

				(*ActorObject)->TryGetStringField(TEXT("label"), ActiveLandscapeLabel);
				const TSharedPtr<FJsonObject>* LocationObject = nullptr;
				if ((*ActorObject)->TryGetObjectField(TEXT("location"), LocationObject) && LocationObject && LocationObject->IsValid())
				{
					LandscapeCenterX = (*LocationObject)->GetNumberField(TEXT("x"));
					LandscapeCenterY = (*LocationObject)->GetNumberField(TEXT("y"));
					LandscapeCenterZ = (*LocationObject)->GetNumberField(TEXT("z"));
				}

				bReusedExistingLandscape = true;
				break;
			}
		}
	}

	if (!bReusedExistingLandscape)
	{
		const FString CreateLandscapeBody = MakeExecRequestBody(
			TEXT("smoke-landscape-create-001"),
			TEXT("landscape_create"),
			FString::Printf(TEXT("{\"label\":\"%s\",\"quads_per_section\":7,\"sections_per_component\":1,\"component_count_x\":1,\"component_count_y\":1,\"location\":{\"x\":200000,\"y\":0,\"z\":0},\"scale\":{\"x\":100,\"y\":100,\"z\":100}}"), *LandscapeLabel));
		FHttpSmokeResult CreateLandscapeResult;
		TestTrue(TEXT("Landscape create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateLandscapeBody, CreateLandscapeResult));
		TestEqual(TEXT("Landscape create status code"), CreateLandscapeResult.StatusCode, 200);

		TSharedPtr<FJsonObject> CreateLandscapeJson;
		TestTrue(TEXT("Landscape create response parses as JSON"), ParseJson(CreateLandscapeResult.Body, CreateLandscapeJson));
		TestTrue(TEXT("Landscape create root ok=true"), CreateLandscapeJson.IsValid() && CreateLandscapeJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* CreateLandscapeData = nullptr;
		TestTrue(TEXT("Landscape create contains data object"), CreateLandscapeJson.IsValid() && CreateLandscapeJson->TryGetObjectField(TEXT("data"), CreateLandscapeData) && CreateLandscapeData && CreateLandscapeData->IsValid());
		if (CreateLandscapeData && CreateLandscapeData->IsValid())
		{
			(*CreateLandscapeData)->TryGetStringField(TEXT("label"), ActiveLandscapeLabel);
			TestEqual(TEXT("Landscape create label"), ActiveLandscapeLabel, LandscapeLabel);
			const TSharedPtr<FJsonObject>* LocationObject = nullptr;
			TestTrue(TEXT("Landscape create contains location object"), (*CreateLandscapeData)->TryGetObjectField(TEXT("location"), LocationObject) && LocationObject && LocationObject->IsValid());
			if (LocationObject && LocationObject->IsValid())
			{
				LandscapeCenterX = (*LocationObject)->GetNumberField(TEXT("x"));
				LandscapeCenterY = (*LocationObject)->GetNumberField(TEXT("y"));
				LandscapeCenterZ = (*LocationObject)->GetNumberField(TEXT("z"));
			}
		}
	}

	bool bLandscapeExistsAfterCreate = false;
	ListActorsContainsLabel(ActiveLandscapeLabel, bLandscapeExistsAfterCreate);
	TestTrue(TEXT("Landscape actor present after create or reuse"), bLandscapeExistsAfterCreate);

	const FString RaiseLandscapeBody = MakeExecRequestBody(
		TEXT("smoke-landscape-raise-001"),
		TEXT("landscape_raise_circle"),
		FString::Printf(TEXT("{\"center\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"radius_cm\":300.0,\"strength_cm\":40.0,\"falloff\":1.0}"), LandscapeCenterX, LandscapeCenterY, LandscapeCenterZ));
	FHttpSmokeResult RaiseLandscapeResult;
	TestTrue(TEXT("Landscape raise_circle request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RaiseLandscapeBody, RaiseLandscapeResult));
	TestEqual(TEXT("Landscape raise_circle status code"), RaiseLandscapeResult.StatusCode, 200);

	TSharedPtr<FJsonObject> RaiseLandscapeJson;
	TestTrue(TEXT("Landscape raise_circle response parses as JSON"), ParseJson(RaiseLandscapeResult.Body, RaiseLandscapeJson));
	TestTrue(TEXT("Landscape raise_circle root ok=true"), RaiseLandscapeJson.IsValid() && RaiseLandscapeJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* RaiseLandscapeData = nullptr;
	TestTrue(TEXT("Landscape raise_circle contains data object"), RaiseLandscapeJson.IsValid() && RaiseLandscapeJson->TryGetObjectField(TEXT("data"), RaiseLandscapeData) && RaiseLandscapeData && RaiseLandscapeData->IsValid());
	if (RaiseLandscapeData && RaiseLandscapeData->IsValid())
	{
		TestTrue(TEXT("Landscape raise_circle region_x2 >= region_x1"), static_cast<int32>((*RaiseLandscapeData)->GetIntegerField(TEXT("region_x2"))) >= static_cast<int32>((*RaiseLandscapeData)->GetIntegerField(TEXT("region_x1"))));
		TestTrue(TEXT("Landscape raise_circle region_y2 >= region_y1"), static_cast<int32>((*RaiseLandscapeData)->GetIntegerField(TEXT("region_y2"))) >= static_cast<int32>((*RaiseLandscapeData)->GetIntegerField(TEXT("region_y1"))));
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceAnimBlueprintSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.AnimBlueprintWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceAnimBlueprintSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString SkeletonObjectPath = TEXT("/Engine/EngineMeshes/SkeletalCube_Skeleton.SkeletalCube_Skeleton");
	const FString SkeletalMeshObjectPath = TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube");
	const FString AnimBlueprintAssetPath = MakeAutomationAssetPath(TEXT("ABP"));
	const FString PreviewAnimBlueprintAssetPath = MakeAutomationAssetPath(TEXT("ABPPrev"));
	const FString TemplateAnimBlueprintAssetPath = MakeAutomationAssetPath(TEXT("ABPTemplate"));
	const FString PreviewTag = TEXT("SmokePreviewTag");
	const FString StructureSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString SmokeLayerName = FString::Printf(TEXT("SmokeLayer_%s"), *StructureSuffix);
	const FString SmokeStateMachineName = FString::Printf(TEXT("SmokeStateMachine_%s"), *StructureSuffix);

	auto FindAnimGraphEntry = [](const TSharedPtr<FJsonObject>* ListData, TSharedPtr<FJsonObject>& OutGraphObject) -> bool
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

			if ((*GraphObject)->GetBoolField(TEXT("is_anim_graph")))
			{
				OutGraphObject = *GraphObject;
				return true;
			}
		}

		return false;
	};

	auto FindNamedStateMachineEntry = [](const TSharedPtr<FJsonObject>* ListData, const FString& ExpectedName, TSharedPtr<FJsonObject>& OutEntryObject) -> bool
	{
		if (!ListData || !ListData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!(*ListData)->TryGetArrayField(TEXT("state_machines"), Entries) || !Entries)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
		{
			const TSharedPtr<FJsonObject>* EntryObject = nullptr;
			if (!EntryValue.IsValid() || !EntryValue->TryGetObject(EntryObject) || !EntryObject || !EntryObject->IsValid())
			{
				continue;
			}

			if ((*EntryObject)->GetStringField(TEXT("state_machine_name")) == ExpectedName)
			{
				OutEntryObject = *EntryObject;
				return true;
			}
		}

		return false;
	};

	auto FindNamedAnimLayerEntry = [](const TSharedPtr<FJsonObject>* ListData, const FString& ExpectedName, TSharedPtr<FJsonObject>& OutEntryObject) -> bool
	{
		if (!ListData || !ListData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!(*ListData)->TryGetArrayField(TEXT("anim_layers"), Entries) || !Entries)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
		{
			const TSharedPtr<FJsonObject>* EntryObject = nullptr;
			if (!EntryValue.IsValid() || !EntryValue->TryGetObject(EntryObject) || !EntryObject || !EntryObject->IsValid())
			{
				continue;
			}

			if ((*EntryObject)->GetStringField(TEXT("layer_name")) == ExpectedName)
			{
				OutEntryObject = *EntryObject;
				return true;
			}
		}

		return false;
	};

	const FString CreateBody = MakeExecRequestBody(
		TEXT("smoke-animbp-create-001"),
		TEXT("anim_blueprint_create"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"parent_class\":\"/Script/Engine.AnimInstance\",\"target_skeleton\":\"%s\",\"preview_skeletal_mesh\":\"%s\",\"compile_after_create\":false,\"open_editor\":false,\"save_after_create\":false}"),
			*AnimBlueprintAssetPath,
			*SkeletonObjectPath,
			*SkeletalMeshObjectPath));
	FHttpSmokeResult CreateResult;
	TestTrue(TEXT("AnimBlueprint create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateBody, CreateResult));
	TestEqual(TEXT("AnimBlueprint create status code"), CreateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CreateJson;
	TestTrue(TEXT("AnimBlueprint create response parses as JSON"), ParseJson(CreateResult.Body, CreateJson));
	TestTrue(TEXT("AnimBlueprint create root ok=true"), CreateJson.IsValid() && CreateJson->GetBoolField(TEXT("ok")));

	FString AnimBlueprintObjectPath;
	const TSharedPtr<FJsonObject>* CreateData = nullptr;
	TestTrue(TEXT("AnimBlueprint create contains data object"), CreateJson.IsValid() && CreateJson->TryGetObjectField(TEXT("data"), CreateData) && CreateData && CreateData->IsValid());
	if (CreateData && CreateData->IsValid())
	{
		AnimBlueprintObjectPath = (*CreateData)->GetStringField(TEXT("object_path"));
		TestEqual(TEXT("AnimBlueprint create asset path"), (*CreateData)->GetStringField(TEXT("asset_path")), AnimBlueprintAssetPath);
		TestFalse(TEXT("AnimBlueprint create object path non-empty"), AnimBlueprintObjectPath.IsEmpty());
		TestEqual(TEXT("AnimBlueprint create target skeleton"), (*CreateData)->GetStringField(TEXT("target_skeleton")), SkeletonObjectPath);
		TestEqual(TEXT("AnimBlueprint create preview mesh"), (*CreateData)->GetStringField(TEXT("preview_skeletal_mesh")), SkeletalMeshObjectPath);
		TestFalse(TEXT("AnimBlueprint create not template"), (*CreateData)->GetBoolField(TEXT("is_template")));
	}

	const FString CreatePreviewBody = MakeExecRequestBody(
		TEXT("smoke-animbp-create-002"),
		TEXT("anim_blueprint_create"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"target_skeleton\":\"%s\",\"compile_after_create\":false,\"open_editor\":false,\"save_after_create\":false}"),
			*PreviewAnimBlueprintAssetPath,
			*SkeletonObjectPath));
	FHttpSmokeResult CreatePreviewResult;
	TestTrue(TEXT("AnimBlueprint preview create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreatePreviewBody, CreatePreviewResult));
	TestEqual(TEXT("AnimBlueprint preview create status code"), CreatePreviewResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CreatePreviewJson;
	TestTrue(TEXT("AnimBlueprint preview create response parses as JSON"), ParseJson(CreatePreviewResult.Body, CreatePreviewJson));
	TestTrue(TEXT("AnimBlueprint preview create root ok=true"), CreatePreviewJson.IsValid() && CreatePreviewJson->GetBoolField(TEXT("ok")));

	FString PreviewAnimBlueprintObjectPath;
	FString PreviewGeneratedClassPath;
	const TSharedPtr<FJsonObject>* CreatePreviewData = nullptr;
	TestTrue(TEXT("AnimBlueprint preview create contains data object"), CreatePreviewJson.IsValid() && CreatePreviewJson->TryGetObjectField(TEXT("data"), CreatePreviewData) && CreatePreviewData && CreatePreviewData->IsValid());
	if (CreatePreviewData && CreatePreviewData->IsValid())
	{
		PreviewAnimBlueprintObjectPath = (*CreatePreviewData)->GetStringField(TEXT("object_path"));
		PreviewGeneratedClassPath = (*CreatePreviewData)->GetStringField(TEXT("generated_class"));
		TestFalse(TEXT("AnimBlueprint preview object path non-empty"), PreviewAnimBlueprintObjectPath.IsEmpty());
		TestFalse(TEXT("AnimBlueprint preview generated class non-empty"), PreviewGeneratedClassPath.IsEmpty());
	}

	const FString CreateTemplateBody = MakeExecRequestBody(
		TEXT("smoke-animbp-create-003"),
		TEXT("anim_blueprint_create"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"template\":true,\"compile_after_create\":false,\"open_editor\":false,\"save_after_create\":false}"),
			*TemplateAnimBlueprintAssetPath));
	FHttpSmokeResult CreateTemplateResult;
	TestTrue(TEXT("AnimBlueprint template create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateTemplateBody, CreateTemplateResult));
	TestEqual(TEXT("AnimBlueprint template create status code"), CreateTemplateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CreateTemplateJson;
	TestTrue(TEXT("AnimBlueprint template create response parses as JSON"), ParseJson(CreateTemplateResult.Body, CreateTemplateJson));
	TestTrue(TEXT("AnimBlueprint template create root ok=true"), CreateTemplateJson.IsValid() && CreateTemplateJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* CreateTemplateData = nullptr;
	TestTrue(TEXT("AnimBlueprint template create contains data object"), CreateTemplateJson.IsValid() && CreateTemplateJson->TryGetObjectField(TEXT("data"), CreateTemplateData) && CreateTemplateData && CreateTemplateData->IsValid());
	if (CreateTemplateData && CreateTemplateData->IsValid())
	{
		TestTrue(TEXT("AnimBlueprint template create is template"), (*CreateTemplateData)->GetBoolField(TEXT("is_template")));
		TestEqual(TEXT("AnimBlueprint template target skeleton empty"), (*CreateTemplateData)->GetStringField(TEXT("target_skeleton")), FString(TEXT("")));
	}

	const FString GetInfoBody = MakeExecRequestBody(
		TEXT("smoke-animbp-info-001"),
		TEXT("anim_blueprint_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult GetInfoResult;
	TestTrue(TEXT("AnimBlueprint get_info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoBody, GetInfoResult));
	TestEqual(TEXT("AnimBlueprint get_info status code"), GetInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoJson;
	TestTrue(TEXT("AnimBlueprint get_info response parses as JSON"), ParseJson(GetInfoResult.Body, GetInfoJson));
	TestTrue(TEXT("AnimBlueprint get_info root ok=true"), GetInfoJson.IsValid() && GetInfoJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* GetInfoData = nullptr;
	TestTrue(TEXT("AnimBlueprint get_info contains data object"), GetInfoJson.IsValid() && GetInfoJson->TryGetObjectField(TEXT("data"), GetInfoData) && GetInfoData && GetInfoData->IsValid());
	if (GetInfoData && GetInfoData->IsValid())
	{
		TestEqual(TEXT("AnimBlueprint get_info target skeleton"), (*GetInfoData)->GetStringField(TEXT("target_skeleton")), SkeletonObjectPath);
		TestEqual(TEXT("AnimBlueprint get_info preview mesh"), (*GetInfoData)->GetStringField(TEXT("preview_skeletal_mesh")), SkeletalMeshObjectPath);
		TestTrue(TEXT("AnimBlueprint get_info generated class non-empty"), !(*GetInfoData)->GetStringField(TEXT("generated_class")).IsEmpty());
		TestTrue(TEXT("AnimBlueprint get_info supports anim layers"), (*GetInfoData)->GetBoolField(TEXT("supports_anim_layers")));
	}

	const FString ListGraphsBody = MakeExecRequestBody(
		TEXT("smoke-animbp-list-001"),
		TEXT("anim_blueprint_list_graphs"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult ListGraphsResult;
	TestTrue(TEXT("AnimBlueprint list_graphs request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListGraphsBody, ListGraphsResult));
	TestEqual(TEXT("AnimBlueprint list_graphs status code"), ListGraphsResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ListGraphsJson;
	TestTrue(TEXT("AnimBlueprint list_graphs response parses as JSON"), ParseJson(ListGraphsResult.Body, ListGraphsJson));
	TestTrue(TEXT("AnimBlueprint list_graphs root ok=true"), ListGraphsJson.IsValid() && ListGraphsJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* ListGraphsData = nullptr;
	TestTrue(TEXT("AnimBlueprint list_graphs contains data object"), ListGraphsJson.IsValid() && ListGraphsJson->TryGetObjectField(TEXT("data"), ListGraphsData) && ListGraphsData && ListGraphsData->IsValid());
	if (ListGraphsData && ListGraphsData->IsValid())
	{
		TestTrue(TEXT("AnimBlueprint list_graphs graph_count>=1"), static_cast<int32>((*ListGraphsData)->GetIntegerField(TEXT("graph_count"))) >= 1);
		TestTrue(TEXT("AnimBlueprint list_graphs anim_graph_count>=1"), static_cast<int32>((*ListGraphsData)->GetIntegerField(TEXT("anim_graph_count"))) >= 1);

		TSharedPtr<FJsonObject> AnimGraphObject;
		TestTrue(TEXT("AnimBlueprint list_graphs contains anim graph entry"), FindAnimGraphEntry(ListGraphsData, AnimGraphObject));
		if (AnimGraphObject.IsValid())
		{
			TestEqual(TEXT("AnimBlueprint list_graphs anim graph kind"), AnimGraphObject->GetStringField(TEXT("graph_kind")), FString(TEXT("anim")));
		}
	}

	const FString AddGenericAnimNodeBody = MakeExecRequestBody(
		TEXT("smoke-animbp-generic-node-001"),
		TEXT("anim_blueprint_add_node_by_class"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"AnimGraph\",\"node_class\":\"/Script/AnimGraph.AnimGraphNode_IdentityPose\",\"pos_x\":240,\"pos_y\":128,\"compile_after_add\":false,\"save_after_add\":false}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult AddGenericAnimNodeResult;
	TestTrue(TEXT("AnimBlueprint add_node_by_class request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddGenericAnimNodeBody, AddGenericAnimNodeResult));
	TestEqual(TEXT("AnimBlueprint add_node_by_class status code"), AddGenericAnimNodeResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddGenericAnimNodeJson;
	TestTrue(TEXT("AnimBlueprint add_node_by_class parses JSON"), ParseJson(AddGenericAnimNodeResult.Body, AddGenericAnimNodeJson));
	TestTrue(TEXT("AnimBlueprint add_node_by_class root ok=true"), AddGenericAnimNodeJson.IsValid() && AddGenericAnimNodeJson->GetBoolField(TEXT("ok")));

	FString GenericAnimNodeGuid;
	const TSharedPtr<FJsonObject>* AddGenericAnimNodeData = nullptr;
	TestTrue(TEXT("AnimBlueprint add_node_by_class contains data object"), AddGenericAnimNodeJson.IsValid() && AddGenericAnimNodeJson->TryGetObjectField(TEXT("data"), AddGenericAnimNodeData) && AddGenericAnimNodeData && AddGenericAnimNodeData->IsValid());
	if (AddGenericAnimNodeData && AddGenericAnimNodeData->IsValid())
	{
		GenericAnimNodeGuid = (*AddGenericAnimNodeData)->GetStringField(TEXT("node_guid"));
		TestTrue(TEXT("AnimBlueprint generic node guid non-empty"), !GenericAnimNodeGuid.IsEmpty());
		TestEqual(TEXT("AnimBlueprint generic node graph name"), (*AddGenericAnimNodeData)->GetStringField(TEXT("graph_name")), FString(TEXT("AnimGraph")));
		TestTrue(TEXT("AnimBlueprint generic node graph path non-empty"), !(*AddGenericAnimNodeData)->GetStringField(TEXT("graph_path")).IsEmpty());
	}

	const FString InspectAnimGraphBody = MakeExecRequestBody(
		TEXT("smoke-animbp-inspect-anim-001"),
		TEXT("anim_blueprint_inspect_nodes"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"AnimGraph\",\"include_pins\":true,\"limit_per_graph\":64}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult InspectAnimGraphResult;
	TestTrue(TEXT("AnimBlueprint inspect_nodes on anim graph request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, InspectAnimGraphBody, InspectAnimGraphResult));
	TestEqual(TEXT("AnimBlueprint inspect_nodes on anim graph status code"), InspectAnimGraphResult.StatusCode, 200);

	TSharedPtr<FJsonObject> InspectAnimGraphJson;
	TestTrue(TEXT("AnimBlueprint inspect_nodes on anim graph parses JSON"), ParseJson(InspectAnimGraphResult.Body, InspectAnimGraphJson));
	TestTrue(TEXT("AnimBlueprint inspect_nodes on anim graph root ok=true"), InspectAnimGraphJson.IsValid() && InspectAnimGraphJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* InspectAnimGraphData = nullptr;
	TestTrue(TEXT("AnimBlueprint inspect_nodes on anim graph contains data object"), InspectAnimGraphJson.IsValid() && InspectAnimGraphJson->TryGetObjectField(TEXT("data"), InspectAnimGraphData) && InspectAnimGraphData && InspectAnimGraphData->IsValid());

	auto FindAnimGraphNodeInInspect = [](const TSharedPtr<FJsonObject>* InspectData, const FString& NodeGuid, TSharedPtr<FJsonObject>& OutNodeObject) -> bool
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

	TSharedPtr<FJsonObject> GenericAnimNodeObject;
	TestTrue(TEXT("AnimBlueprint inspect_nodes finds generic anim node"), FindAnimGraphNodeInInspect(InspectAnimGraphData, GenericAnimNodeGuid, GenericAnimNodeObject));
	if (GenericAnimNodeObject.IsValid())
	{
		TestTrue(TEXT("AnimBlueprint generic node class contains identity pose"), GenericAnimNodeObject->GetStringField(TEXT("class")).Contains(TEXT("AnimGraphNode_IdentityPose")));
	}

	UAnimBlueprint* EditableAnimBlueprint = AnimBlueprintObjectPath.IsEmpty() ? nullptr : LoadObject<UAnimBlueprint>(nullptr, *AnimBlueprintObjectPath);
	TestNotNull(TEXT("AnimBlueprint smoke local asset loads"), EditableAnimBlueprint);
	if (EditableAnimBlueprint)
	{
		UEdGraph* DefaultAnimGraph = nullptr;
		for (UEdGraph* Graph : EditableAnimBlueprint->FunctionGraphs)
		{
			if (Graph && AnimationEditorUtils::IsAnimGraph(Graph) && Graph->GetFName() == UEdGraphSchema_K2::GN_AnimGraph)
			{
				DefaultAnimGraph = Graph;
				break;
			}
		}

		TestNotNull(TEXT("AnimBlueprint smoke default anim graph exists"), DefaultAnimGraph);

		UEdGraph* LayerGraph = FBlueprintEditorUtils::CreateNewGraph(EditableAnimBlueprint, FName(*SmokeLayerName), UAnimationGraph::StaticClass(), UAnimationGraphSchema::StaticClass());
		TestNotNull(TEXT("AnimBlueprint smoke layer graph created"), LayerGraph);
		if (LayerGraph)
		{
			FBlueprintEditorUtils::AddDomainSpecificGraph(EditableAnimBlueprint, LayerGraph);
		}

		UAnimGraphNode_StateMachine* StateMachineNode = DefaultAnimGraph ? AddAutomationNodeToGraph<UAnimGraphNode_StateMachine>(DefaultAnimGraph) : nullptr;
		TestNotNull(TEXT("AnimBlueprint smoke state machine node created"), StateMachineNode);

		UAnimationStateMachineGraph* StateMachineGraph = StateMachineNode ? StateMachineNode->EditorStateMachineGraph : nullptr;
		TestNotNull(TEXT("AnimBlueprint smoke state machine graph created"), StateMachineGraph);
		if (StateMachineGraph)
		{
			FBlueprintEditorUtils::RenameGraph(StateMachineGraph, SmokeStateMachineName);
		}

		UAnimStateNode* StateNode = StateMachineGraph ? AddAutomationNodeToGraph<UAnimStateNode>(StateMachineGraph) : nullptr;
		TestNotNull(TEXT("AnimBlueprint smoke state node created"), StateNode);
		if (StateMachineGraph && StateMachineGraph->EntryNode && StateNode && StateMachineGraph->EntryNode->Pins.Num() > 0)
		{
			StateNode->AutowireNewNode(StateMachineGraph->EntryNode->Pins[0]);
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(EditableAnimBlueprint);
		FKismetEditorUtilities::CompileBlueprint(EditableAnimBlueprint);
		TestTrue(TEXT("AnimBlueprint smoke structure compile has no error"), EditableAnimBlueprint->Status != BS_Error);
	}

	const FString ListStateMachinesBody = MakeExecRequestBody(
		TEXT("smoke-animbp-state-machines-001"),
		TEXT("anim_blueprint_list_state_machines"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult ListStateMachinesResult;
	TestTrue(TEXT("AnimBlueprint list_state_machines request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListStateMachinesBody, ListStateMachinesResult));
	TestEqual(TEXT("AnimBlueprint list_state_machines status code"), ListStateMachinesResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ListStateMachinesJson;
	TestTrue(TEXT("AnimBlueprint list_state_machines parses JSON"), ParseJson(ListStateMachinesResult.Body, ListStateMachinesJson));
	TestTrue(TEXT("AnimBlueprint list_state_machines root ok=true"), ListStateMachinesJson.IsValid() && ListStateMachinesJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* ListStateMachinesData = nullptr;
	TestTrue(TEXT("AnimBlueprint list_state_machines contains data object"), ListStateMachinesJson.IsValid() && ListStateMachinesJson->TryGetObjectField(TEXT("data"), ListStateMachinesData) && ListStateMachinesData && ListStateMachinesData->IsValid());
	if (ListStateMachinesData && ListStateMachinesData->IsValid())
	{
		TestTrue(TEXT("AnimBlueprint list_state_machines count>=1"), static_cast<int32>((*ListStateMachinesData)->GetIntegerField(TEXT("state_machine_count"))) >= 1);
		TestTrue(TEXT("AnimBlueprint list_state_machines compiled_count field exists"), (*ListStateMachinesData)->HasField(TEXT("compiled_state_machine_count")));

		TSharedPtr<FJsonObject> StateMachineEntry;
		TestTrue(TEXT("AnimBlueprint list_state_machines contains created state machine"), FindNamedStateMachineEntry(ListStateMachinesData, SmokeStateMachineName, StateMachineEntry));
		if (StateMachineEntry.IsValid())
		{
			TestEqual(TEXT("AnimBlueprint list_state_machines owner anim graph"), StateMachineEntry->GetStringField(TEXT("owner_anim_graph_name")), FString(TEXT("AnimGraph")));
			TestTrue(TEXT("AnimBlueprint list_state_machines state_count>=1"), static_cast<int32>(StateMachineEntry->GetIntegerField(TEXT("state_count"))) >= 1);
			TestTrue(TEXT("AnimBlueprint list_state_machines has entry node"), StateMachineEntry->GetBoolField(TEXT("has_entry_node")));
			TestTrue(TEXT("AnimBlueprint list_state_machines compiled flag exists"), StateMachineEntry->HasField(TEXT("compiled_machine_found")));
		}
	}

	const FString ListAnimLayersBody = MakeExecRequestBody(
		TEXT("smoke-animbp-layers-001"),
		TEXT("anim_blueprint_list_anim_layers"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult ListAnimLayersResult;
	TestTrue(TEXT("AnimBlueprint list_anim_layers request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListAnimLayersBody, ListAnimLayersResult));
	TestEqual(TEXT("AnimBlueprint list_anim_layers status code"), ListAnimLayersResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ListAnimLayersJson;
	TestTrue(TEXT("AnimBlueprint list_anim_layers parses JSON"), ParseJson(ListAnimLayersResult.Body, ListAnimLayersJson));
	TestTrue(TEXT("AnimBlueprint list_anim_layers root ok=true"), ListAnimLayersJson.IsValid() && ListAnimLayersJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* ListAnimLayersData = nullptr;
	TestTrue(TEXT("AnimBlueprint list_anim_layers contains data object"), ListAnimLayersJson.IsValid() && ListAnimLayersJson->TryGetObjectField(TEXT("data"), ListAnimLayersData) && ListAnimLayersData && ListAnimLayersData->IsValid());
	if (ListAnimLayersData && ListAnimLayersData->IsValid())
	{
		TestTrue(TEXT("AnimBlueprint list_anim_layers count>=1"), static_cast<int32>((*ListAnimLayersData)->GetIntegerField(TEXT("layer_count"))) >= 1);
		TestTrue(TEXT("AnimBlueprint list_anim_layers graph_count>=1"), static_cast<int32>((*ListAnimLayersData)->GetIntegerField(TEXT("layer_graph_count"))) >= 1);
		TestTrue(TEXT("AnimBlueprint list_anim_layers compiled_count>=1"), static_cast<int32>((*ListAnimLayersData)->GetIntegerField(TEXT("compiled_layer_function_count"))) >= 1);

		TSharedPtr<FJsonObject> LayerEntry;
		TestTrue(TEXT("AnimBlueprint list_anim_layers contains created layer"), FindNamedAnimLayerEntry(ListAnimLayersData, SmokeLayerName, LayerEntry));
		if (LayerEntry.IsValid())
		{
			TestTrue(TEXT("AnimBlueprint list_anim_layers has graph"), LayerEntry->GetBoolField(TEXT("has_graph")));
			TestTrue(TEXT("AnimBlueprint list_anim_layers compiled function found"), LayerEntry->GetBoolField(TEXT("compiled_function_found")));
			TestTrue(TEXT("AnimBlueprint list_anim_layers implemented field exists"), LayerEntry->HasField(TEXT("is_implemented")));
		}
	}

	const FString ClearPreviewMeshBody = MakeExecRequestBody(
		TEXT("smoke-animbp-clear-preview-mesh-001"),
		TEXT("anim_blueprint_set_preview_mesh"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clear_preview_mesh\":true,\"save_after_set\":false}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult ClearPreviewMeshResult;
	TestTrue(TEXT("AnimBlueprint clear preview mesh request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ClearPreviewMeshBody, ClearPreviewMeshResult));
	TestEqual(TEXT("AnimBlueprint clear preview mesh status code"), ClearPreviewMeshResult.StatusCode, 200);

	const FString SetPreviewMeshBody = MakeExecRequestBody(
		TEXT("smoke-animbp-set-preview-mesh-001"),
		TEXT("anim_blueprint_set_preview_mesh"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"skeletal_mesh_path\":\"%s\",\"save_after_set\":false}"), *AnimBlueprintAssetPath, *SkeletalMeshObjectPath));
	FHttpSmokeResult SetPreviewMeshResult;
	TestTrue(TEXT("AnimBlueprint set preview mesh request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetPreviewMeshBody, SetPreviewMeshResult));
	TestEqual(TEXT("AnimBlueprint set preview mesh status code"), SetPreviewMeshResult.StatusCode, 200);

	const FString SetPreviewAnimBlueprintBody = MakeExecRequestBody(
		TEXT("smoke-animbp-set-preview-abp-001"),
		TEXT("anim_blueprint_set_preview_animation_blueprint"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"preview_anim_blueprint_path\":\"%s\",\"preview_application_method\":\"linked_anim_graph\",\"preview_animation_blueprint_tag\":\"%s\",\"save_after_set\":false}"), *AnimBlueprintAssetPath, *PreviewAnimBlueprintAssetPath, *PreviewTag));
	FHttpSmokeResult SetPreviewAnimBlueprintResult;
	TestTrue(TEXT("AnimBlueprint set preview anim blueprint request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetPreviewAnimBlueprintBody, SetPreviewAnimBlueprintResult));
	TestEqual(TEXT("AnimBlueprint set preview anim blueprint status code"), SetPreviewAnimBlueprintResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SetPreviewAnimBlueprintJson;
	TestTrue(TEXT("AnimBlueprint set preview anim blueprint response parses as JSON"), ParseJson(SetPreviewAnimBlueprintResult.Body, SetPreviewAnimBlueprintJson));
	TestTrue(TEXT("AnimBlueprint set preview anim blueprint root ok=true"), SetPreviewAnimBlueprintJson.IsValid() && SetPreviewAnimBlueprintJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* SetPreviewAnimBlueprintData = nullptr;
	TestTrue(TEXT("AnimBlueprint set preview anim blueprint contains data object"), SetPreviewAnimBlueprintJson.IsValid() && SetPreviewAnimBlueprintJson->TryGetObjectField(TEXT("data"), SetPreviewAnimBlueprintData) && SetPreviewAnimBlueprintData && SetPreviewAnimBlueprintData->IsValid());
	if (SetPreviewAnimBlueprintData && SetPreviewAnimBlueprintData->IsValid())
	{
		TestEqual(TEXT("AnimBlueprint set preview anim blueprint object path"), (*SetPreviewAnimBlueprintData)->GetStringField(TEXT("preview_animation_blueprint")), PreviewAnimBlueprintObjectPath);
		TestEqual(TEXT("AnimBlueprint set preview anim blueprint method"), (*SetPreviewAnimBlueprintData)->GetStringField(TEXT("preview_application_method")), FString(TEXT("linked_anim_graph")));
		TestEqual(TEXT("AnimBlueprint set preview anim blueprint tag"), (*SetPreviewAnimBlueprintData)->GetStringField(TEXT("preview_animation_blueprint_tag")), PreviewTag);
	}

	const FString GetInfoAfterPreviewBody = MakeExecRequestBody(
		TEXT("smoke-animbp-info-002"),
		TEXT("anim_blueprint_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult GetInfoAfterPreviewResult;
	TestTrue(TEXT("AnimBlueprint get_info after preview request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoAfterPreviewBody, GetInfoAfterPreviewResult));
	TestEqual(TEXT("AnimBlueprint get_info after preview status code"), GetInfoAfterPreviewResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoAfterPreviewJson;
	TestTrue(TEXT("AnimBlueprint get_info after preview response parses as JSON"), ParseJson(GetInfoAfterPreviewResult.Body, GetInfoAfterPreviewJson));
	TestTrue(TEXT("AnimBlueprint get_info after preview root ok=true"), GetInfoAfterPreviewJson.IsValid() && GetInfoAfterPreviewJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* GetInfoAfterPreviewData = nullptr;
	TestTrue(TEXT("AnimBlueprint get_info after preview contains data object"), GetInfoAfterPreviewJson.IsValid() && GetInfoAfterPreviewJson->TryGetObjectField(TEXT("data"), GetInfoAfterPreviewData) && GetInfoAfterPreviewData && GetInfoAfterPreviewData->IsValid());
	if (GetInfoAfterPreviewData && GetInfoAfterPreviewData->IsValid())
	{
		TestEqual(TEXT("AnimBlueprint get_info after preview mesh restored"), (*GetInfoAfterPreviewData)->GetStringField(TEXT("preview_skeletal_mesh")), SkeletalMeshObjectPath);
		TestEqual(TEXT("AnimBlueprint get_info after preview anim blueprint"), (*GetInfoAfterPreviewData)->GetStringField(TEXT("preview_animation_blueprint")), PreviewAnimBlueprintObjectPath);
		TestEqual(TEXT("AnimBlueprint get_info after preview method"), (*GetInfoAfterPreviewData)->GetStringField(TEXT("preview_application_method")), FString(TEXT("linked_anim_graph")));
		TestEqual(TEXT("AnimBlueprint get_info after preview tag"), (*GetInfoAfterPreviewData)->GetStringField(TEXT("preview_animation_blueprint_tag")), PreviewTag);
	}

	const FString ReparentBody = MakeExecRequestBody(
		TEXT("smoke-animbp-reparent-001"),
		TEXT("anim_blueprint_reparent"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"parent_class\":\"%s\",\"compile_after_reparent\":false,\"save_after_reparent\":false}"), *AnimBlueprintAssetPath, *PreviewGeneratedClassPath));
	FHttpSmokeResult ReparentResult;
	TestTrue(TEXT("AnimBlueprint reparent request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ReparentBody, ReparentResult));
	TestEqual(TEXT("AnimBlueprint reparent status code"), ReparentResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ReparentJson;
	TestTrue(TEXT("AnimBlueprint reparent response parses as JSON"), ParseJson(ReparentResult.Body, ReparentJson));
	TestTrue(TEXT("AnimBlueprint reparent root ok=true"), ReparentJson.IsValid() && ReparentJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* ReparentData = nullptr;
	TestTrue(TEXT("AnimBlueprint reparent contains data object"), ReparentJson.IsValid() && ReparentJson->TryGetObjectField(TEXT("data"), ReparentData) && ReparentData && ReparentData->IsValid());
	if (ReparentData && ReparentData->IsValid())
	{
		TestEqual(TEXT("AnimBlueprint reparent parent class"), (*ReparentData)->GetStringField(TEXT("parent_class")), PreviewGeneratedClassPath);
	}

	const FString CompileBody = MakeExecRequestBody(
		TEXT("smoke-animbp-compile-001"),
		TEXT("anim_blueprint_compile"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_messages\":true,\"severity_filter\":\"all\",\"max_messages\":16,\"save_after_compile\":false}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult CompileResult;
	TestTrue(TEXT("AnimBlueprint compile request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CompileBody, CompileResult));
	TestEqual(TEXT("AnimBlueprint compile status code"), CompileResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CompileJson;
	TestTrue(TEXT("AnimBlueprint compile response parses as JSON"), ParseJson(CompileResult.Body, CompileJson));
	TestTrue(TEXT("AnimBlueprint compile root ok=true"), CompileJson.IsValid() && CompileJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* CompileData = nullptr;
	TestTrue(TEXT("AnimBlueprint compile contains data object"), CompileJson.IsValid() && CompileJson->TryGetObjectField(TEXT("data"), CompileData) && CompileData && CompileData->IsValid());
	if (CompileData && CompileData->IsValid())
	{
		TestFalse(TEXT("AnimBlueprint compile reports no error"), (*CompileData)->GetBoolField(TEXT("has_error")));
		TestTrue(TEXT("AnimBlueprint compile error count is zero"), static_cast<int32>((*CompileData)->GetIntegerField(TEXT("error_count"))) == 0);
	}

	const FString CompileLogBody = MakeExecRequestBody(
		TEXT("smoke-animbp-compile-log-001"),
		TEXT("anim_blueprint_get_compile_log"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"severity_filter\":\"all\",\"max_messages\":16,\"save_after_compile\":false}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult CompileLogResult;
	TestTrue(TEXT("AnimBlueprint get_compile_log request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CompileLogBody, CompileLogResult));
	TestEqual(TEXT("AnimBlueprint get_compile_log status code"), CompileLogResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CompileLogJson;
	TestTrue(TEXT("AnimBlueprint get_compile_log response parses as JSON"), ParseJson(CompileLogResult.Body, CompileLogJson));
	TestTrue(TEXT("AnimBlueprint get_compile_log root ok=true"), CompileLogJson.IsValid() && CompileLogJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* CompileLogData = nullptr;
	TestTrue(TEXT("AnimBlueprint get_compile_log contains data object"), CompileLogJson.IsValid() && CompileLogJson->TryGetObjectField(TEXT("data"), CompileLogData) && CompileLogData && CompileLogData->IsValid());
	if (CompileLogData && CompileLogData->IsValid())
	{
		TestFalse(TEXT("AnimBlueprint get_compile_log reports no error"), (*CompileLogData)->GetBoolField(TEXT("has_error")));
		TestTrue(TEXT("AnimBlueprint get_compile_log error count is zero"), static_cast<int32>((*CompileLogData)->GetIntegerField(TEXT("error_count"))) == 0);
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceAnimBlueprintCommandsSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.AnimBlueprintCommandsWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceAnimBlueprintCommandsSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString SkeletonObjectPath = TEXT("/Engine/EngineMeshes/SkeletalCube_Skeleton.SkeletalCube_Skeleton");
	const FString SkeletalMeshObjectPath = TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube");
	const FString AnimBlueprintAssetPath = MakeAutomationAssetPath(TEXT("ABPFull"));
	const FString LayerInterfaceAssetPath = MakeAutomationAssetPath(TEXT("ABPInterface"));
	const FString VariableName = TEXT("SmokeFlag");
	const FString LogicFunctionName = TEXT("SmokeLogicFunction");
	const FString LocalLayerName = TEXT("SmokeLocalLayer");
	const FString RenamedLocalLayerName = TEXT("SmokeLocalLayerRenamed");
	const FString InterfaceLayerName = TEXT("SmokeInterfaceLayer");
	const FString StateMachineName = TEXT("SmokeMachine");
	const FString RenamedStateMachineName = TEXT("SmokeMachineRenamed");
	const FString IdleStateName = TEXT("Idle");
	const FString RunStateName = TEXT("Run");
	const FString RenamedRunStateName = TEXT("RunFast");
	const FString ConduitName = TEXT("Gate");
	const FString AliasName = TEXT("IdleAlias");
	const FString CustomEventName = TEXT("SmokeAnimEvent");
	const FString StringValue = TEXT("SmokeAnimMessage");

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

	auto FindGraphByName = [](const TSharedPtr<FJsonObject>* ListData, const FString& GraphName) -> bool
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

			if ((*GraphObject)->GetStringField(TEXT("graph_name")) == GraphName)
			{
				return true;
			}
		}

		return false;
	};

	auto FindNamedStateMachineEntry = [](const TSharedPtr<FJsonObject>* ListData, const FString& ExpectedName, TSharedPtr<FJsonObject>& OutEntryObject) -> bool
	{
		if (!ListData || !ListData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!(*ListData)->TryGetArrayField(TEXT("state_machines"), Entries) || !Entries)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
		{
			const TSharedPtr<FJsonObject>* EntryObject = nullptr;
			if (!EntryValue.IsValid() || !EntryValue->TryGetObject(EntryObject) || !EntryObject || !EntryObject->IsValid())
			{
				continue;
			}

			if ((*EntryObject)->GetStringField(TEXT("state_machine_name")) == ExpectedName)
			{
				OutEntryObject = *EntryObject;
				return true;
			}
		}

		return false;
	};

	auto FindNamedAnimLayerEntry = [](const TSharedPtr<FJsonObject>* ListData, const FString& ExpectedName, TSharedPtr<FJsonObject>& OutEntryObject) -> bool
	{
		if (!ListData || !ListData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!(*ListData)->TryGetArrayField(TEXT("anim_layers"), Entries) || !Entries)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
		{
			const TSharedPtr<FJsonObject>* EntryObject = nullptr;
			if (!EntryValue.IsValid() || !EntryValue->TryGetObject(EntryObject) || !EntryObject || !EntryObject->IsValid())
			{
				continue;
			}

			if ((*EntryObject)->GetStringField(TEXT("layer_name")) == ExpectedName)
			{
				OutEntryObject = *EntryObject;
				return true;
			}
		}

		return false;
	};

	auto FindLayerInterfaceEntry = [](const TSharedPtr<FJsonObject>* ListData, const FString& InterfaceClassPath, TSharedPtr<FJsonObject>& OutEntryObject) -> bool
	{
		if (!ListData || !ListData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!(*ListData)->TryGetArrayField(TEXT("interfaces"), Entries) || !Entries)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
		{
			const TSharedPtr<FJsonObject>* EntryObject = nullptr;
			if (!EntryValue.IsValid() || !EntryValue->TryGetObject(EntryObject) || !EntryObject || !EntryObject->IsValid())
			{
				continue;
			}

			if ((*EntryObject)->GetStringField(TEXT("interface_class")) == InterfaceClassPath)
			{
				OutEntryObject = *EntryObject;
				return true;
			}
		}

		return false;
	};

	auto FindStateNodeEntry = [](const TSharedPtr<FJsonObject>& StateMachineEntry, const FString& ExpectedStateName, TSharedPtr<FJsonObject>& OutEntryObject) -> bool
	{
		if (!StateMachineEntry.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* StateNodes = nullptr;
		if (!StateMachineEntry->TryGetArrayField(TEXT("state_nodes"), StateNodes) || !StateNodes)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& NodeValue : *StateNodes)
		{
			const TSharedPtr<FJsonObject>* NodeObject = nullptr;
			if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObject) || !NodeObject || !NodeObject->IsValid())
			{
				continue;
			}

			if ((*NodeObject)->GetStringField(TEXT("state_name")) == ExpectedStateName)
			{
				OutEntryObject = *NodeObject;
				return true;
			}
		}

		return false;
	};

	auto FindTransitionEntry = [](const TSharedPtr<FJsonObject>& StateMachineEntry, const FString& SourceStateName, const FString& TargetStateName, TSharedPtr<FJsonObject>& OutEntryObject) -> bool
	{
		if (!StateMachineEntry.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* TransitionNodes = nullptr;
		if (!StateMachineEntry->TryGetArrayField(TEXT("transition_nodes"), TransitionNodes) || !TransitionNodes)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& NodeValue : *TransitionNodes)
		{
			const TSharedPtr<FJsonObject>* NodeObject = nullptr;
			if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObject) || !NodeObject || !NodeObject->IsValid())
			{
				continue;
			}

			if ((*NodeObject)->GetStringField(TEXT("source_state_name")) == SourceStateName
				&& (*NodeObject)->GetStringField(TEXT("target_state_name")) == TargetStateName)
			{
				OutEntryObject = *NodeObject;
				return true;
			}
		}

		return false;
	};

	const FString CreateInterfaceBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-create-interface-001"),
		TEXT("anim_blueprint_create_layer_interface"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"compile_after_create\":false,\"open_editor\":false,\"save_after_create\":false}"), *LayerInterfaceAssetPath));
	FHttpSmokeResult CreateInterfaceResult;
	TestTrue(TEXT("AnimBlueprint create_layer_interface request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateInterfaceBody, CreateInterfaceResult));
	TestEqual(TEXT("AnimBlueprint create_layer_interface status code"), CreateInterfaceResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CreateInterfaceJson;
	TestTrue(TEXT("AnimBlueprint create_layer_interface response parses as JSON"), ParseJson(CreateInterfaceResult.Body, CreateInterfaceJson));
	TestTrue(TEXT("AnimBlueprint create_layer_interface root ok=true"), CreateInterfaceJson.IsValid() && CreateInterfaceJson->GetBoolField(TEXT("ok")));

	FString LayerInterfaceGeneratedClassPath;
	const TSharedPtr<FJsonObject>* CreateInterfaceData = nullptr;
	TestTrue(TEXT("AnimBlueprint create_layer_interface contains data object"), CreateInterfaceJson.IsValid() && CreateInterfaceJson->TryGetObjectField(TEXT("data"), CreateInterfaceData) && CreateInterfaceData && CreateInterfaceData->IsValid());
	if (CreateInterfaceData && CreateInterfaceData->IsValid())
	{
		LayerInterfaceGeneratedClassPath = (*CreateInterfaceData)->GetStringField(TEXT("generated_class"));
		TestTrue(TEXT("AnimBlueprint create_layer_interface is interface"), (*CreateInterfaceData)->GetBoolField(TEXT("is_layer_interface")));
		TestFalse(TEXT("AnimBlueprint create_layer_interface generated class non-empty"), LayerInterfaceGeneratedClassPath.IsEmpty());
	}

	const FString AddInterfaceLayerBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-add-interface-layer-001"),
		TEXT("anim_blueprint_add_anim_layer"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"layer_name\":\"%s\",\"compile_after_add\":false,\"save_after_add\":false}"), *LayerInterfaceAssetPath, *InterfaceLayerName));
	FHttpSmokeResult AddInterfaceLayerResult;
	TestTrue(TEXT("AnimBlueprint add_anim_layer on interface request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddInterfaceLayerBody, AddInterfaceLayerResult));
	TestEqual(TEXT("AnimBlueprint add_anim_layer on interface status code"), AddInterfaceLayerResult.StatusCode, 200);

	const FString CompileInterfaceBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-compile-interface-001"),
		TEXT("anim_blueprint_compile"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_messages\":true,\"max_messages\":16,\"save_after_compile\":false}"), *LayerInterfaceAssetPath));
	FHttpSmokeResult CompileInterfaceResult;
	TestTrue(TEXT("AnimBlueprint compile interface request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CompileInterfaceBody, CompileInterfaceResult));
	TestEqual(TEXT("AnimBlueprint compile interface status code"), CompileInterfaceResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CompileInterfaceJson;
	TestTrue(TEXT("AnimBlueprint compile interface response parses as JSON"), ParseJson(CompileInterfaceResult.Body, CompileInterfaceJson));
	TestTrue(TEXT("AnimBlueprint compile interface root ok=true"), CompileInterfaceJson.IsValid() && CompileInterfaceJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* CompileInterfaceData = nullptr;
	TestTrue(TEXT("AnimBlueprint compile interface contains data object"), CompileInterfaceJson.IsValid() && CompileInterfaceJson->TryGetObjectField(TEXT("data"), CompileInterfaceData) && CompileInterfaceData && CompileInterfaceData->IsValid());
	if (CompileInterfaceData && CompileInterfaceData->IsValid())
	{
		TestFalse(TEXT("AnimBlueprint compile interface reports no error"), (*CompileInterfaceData)->GetBoolField(TEXT("has_error")));
	}

	const FString CreateAnimBlueprintBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-create-001"),
		TEXT("anim_blueprint_create"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"parent_class\":\"/Script/Engine.AnimInstance\",\"target_skeleton\":\"%s\",\"preview_skeletal_mesh\":\"%s\",\"compile_after_create\":false,\"open_editor\":false,\"save_after_create\":false}"), *AnimBlueprintAssetPath, *SkeletonObjectPath, *SkeletalMeshObjectPath));
	FHttpSmokeResult CreateAnimBlueprintResult;
	TestTrue(TEXT("AnimBlueprint create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateAnimBlueprintBody, CreateAnimBlueprintResult));
	TestEqual(TEXT("AnimBlueprint create status code"), CreateAnimBlueprintResult.StatusCode, 200);

	const FString AddVariableBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-add-variable-001"),
		TEXT("anim_blueprint_add_variable"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"variable_name\":\"%s\",\"pin_category\":\"bool\",\"compile_after_add\":false,\"save_after_add\":false}"), *AnimBlueprintAssetPath, *VariableName));
	FHttpSmokeResult AddVariableResult;
	TestTrue(TEXT("AnimBlueprint add_variable request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddVariableBody, AddVariableResult));
	TestEqual(TEXT("AnimBlueprint add_variable status code"), AddVariableResult.StatusCode, 200);

	const FString AddCustomEventBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-custom-event-001"),
		TEXT("anim_blueprint_add_custom_event_node"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"event_name\":\"%s\",\"graph_name\":\"EventGraph\",\"pos_x\":-320,\"pos_y\":0,\"compile_after_add\":false,\"save_after_add\":false}"), *AnimBlueprintAssetPath, *CustomEventName));
	FHttpSmokeResult AddCustomEventResult;
	TestTrue(TEXT("AnimBlueprint add_custom_event_node request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddCustomEventBody, AddCustomEventResult));
	TestEqual(TEXT("AnimBlueprint add_custom_event_node status code"), AddCustomEventResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddCustomEventJson;
	TestTrue(TEXT("AnimBlueprint add_custom_event_node response parses as JSON"), ParseJson(AddCustomEventResult.Body, AddCustomEventJson));
	FString CustomEventNodeGuid;
	const TSharedPtr<FJsonObject>* AddCustomEventData = nullptr;
	TestTrue(TEXT("AnimBlueprint add_custom_event_node contains data object"), AddCustomEventJson.IsValid() && AddCustomEventJson->TryGetObjectField(TEXT("data"), AddCustomEventData) && AddCustomEventData && AddCustomEventData->IsValid());
	if (AddCustomEventData && AddCustomEventData->IsValid())
	{
		CustomEventNodeGuid = (*AddCustomEventData)->GetStringField(TEXT("node_guid"));
	}

	const FString AddCallFunctionBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-call-function-001"),
		TEXT("anim_blueprint_add_call_function_node"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"EventGraph\",\"function_owner_class\":\"/Script/Engine.KismetSystemLibrary\",\"function_name\":\"PrintString\",\"pos_x\":48,\"pos_y\":0,\"compile_after_add\":false,\"save_after_add\":false}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult AddCallFunctionResult;
	TestTrue(TEXT("AnimBlueprint add_call_function_node request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddCallFunctionBody, AddCallFunctionResult));
	TestEqual(TEXT("AnimBlueprint add_call_function_node status code"), AddCallFunctionResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddCallFunctionJson;
	TestTrue(TEXT("AnimBlueprint add_call_function_node response parses as JSON"), ParseJson(AddCallFunctionResult.Body, AddCallFunctionJson));
	FString CallFunctionNodeGuid;
	const TSharedPtr<FJsonObject>* AddCallFunctionData = nullptr;
	TestTrue(TEXT("AnimBlueprint add_call_function_node contains data object"), AddCallFunctionJson.IsValid() && AddCallFunctionJson->TryGetObjectField(TEXT("data"), AddCallFunctionData) && AddCallFunctionData && AddCallFunctionData->IsValid());
	if (AddCallFunctionData && AddCallFunctionData->IsValid())
	{
		CallFunctionNodeGuid = (*AddCallFunctionData)->GetStringField(TEXT("node_guid"));
	}

	const FString AddVariableNodeBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-variable-node-001"),
		TEXT("anim_blueprint_add_variable_node"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"EventGraph\",\"variable_name\":\"%s\",\"node_type\":\"get\",\"pos_x\":-160,\"pos_y\":120,\"compile_after_add\":false,\"save_after_add\":false}"), *AnimBlueprintAssetPath, *VariableName));
	FHttpSmokeResult AddVariableNodeResult;
	TestTrue(TEXT("AnimBlueprint add_variable_node request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddVariableNodeBody, AddVariableNodeResult));
	TestEqual(TEXT("AnimBlueprint add_variable_node status code"), AddVariableNodeResult.StatusCode, 200);

	const FString InspectEventGraphBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-inspect-eventgraph-001"),
		TEXT("anim_blueprint_inspect_nodes"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"EventGraph\",\"include_pins\":true,\"limit_per_graph\":64}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult InspectEventGraphResult;
	TestTrue(TEXT("AnimBlueprint inspect_nodes on event graph request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, InspectEventGraphBody, InspectEventGraphResult));
	TestEqual(TEXT("AnimBlueprint inspect_nodes on event graph status code"), InspectEventGraphResult.StatusCode, 200);

	TSharedPtr<FJsonObject> InspectEventGraphJson;
	TestTrue(TEXT("AnimBlueprint inspect_nodes on event graph parses JSON"), ParseJson(InspectEventGraphResult.Body, InspectEventGraphJson));
	const TSharedPtr<FJsonObject>* InspectEventGraphData = nullptr;
	TestTrue(TEXT("AnimBlueprint inspect_nodes on event graph contains data object"), InspectEventGraphJson.IsValid() && InspectEventGraphJson->TryGetObjectField(TEXT("data"), InspectEventGraphData) && InspectEventGraphData && InspectEventGraphData->IsValid());

	TSharedPtr<FJsonObject> CustomEventNodeObject;
	TSharedPtr<FJsonObject> CallFunctionNodeObject;
	TestTrue(TEXT("AnimBlueprint inspect_nodes finds custom event"), FindNodeInInspect(InspectEventGraphData, CustomEventNodeGuid, CustomEventNodeObject));
	TestTrue(TEXT("AnimBlueprint inspect_nodes finds call function"), FindNodeInInspect(InspectEventGraphData, CallFunctionNodeGuid, CallFunctionNodeObject));

	FString EventExecPinName;
	FString CallExecPinName;
	FString CallStringPinName;
	TestTrue(TEXT("AnimBlueprint custom event has exec output pin"), FindPinName(CustomEventNodeObject, TEXT("output"), TEXT("exec"), EventExecPinName));
	TestTrue(TEXT("AnimBlueprint call function has exec input pin"), FindPinName(CallFunctionNodeObject, TEXT("input"), TEXT("exec"), CallExecPinName));
	TestTrue(TEXT("AnimBlueprint call function has string input pin"), FindPinName(CallFunctionNodeObject, TEXT("input"), TEXT("string"), CallStringPinName));

	const FString ConnectPinsBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-connect-001"),
		TEXT("anim_blueprint_connect_pins"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"EventGraph\",\"from_node_guid\":\"%s\",\"from_pin\":\"%s\",\"to_node_guid\":\"%s\",\"to_pin\":\"%s\",\"compile_after_connect\":false,\"save_after_connect\":false}"), *AnimBlueprintAssetPath, *CustomEventNodeGuid, *EventExecPinName, *CallFunctionNodeGuid, *CallExecPinName));
	FHttpSmokeResult ConnectPinsResult;
	TestTrue(TEXT("AnimBlueprint connect_pins request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ConnectPinsBody, ConnectPinsResult));
	TestEqual(TEXT("AnimBlueprint connect_pins status code"), ConnectPinsResult.StatusCode, 200);

	const FString DisconnectPinsBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-disconnect-001"),
		TEXT("anim_blueprint_disconnect_pins"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"EventGraph\",\"from_node_guid\":\"%s\",\"from_pin\":\"%s\",\"to_node_guid\":\"%s\",\"to_pin\":\"%s\",\"compile_after_disconnect\":false,\"save_after_disconnect\":false}"), *AnimBlueprintAssetPath, *CustomEventNodeGuid, *EventExecPinName, *CallFunctionNodeGuid, *CallExecPinName));
	FHttpSmokeResult DisconnectPinsResult;
	TestTrue(TEXT("AnimBlueprint disconnect_pins request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DisconnectPinsBody, DisconnectPinsResult));
	TestEqual(TEXT("AnimBlueprint disconnect_pins status code"), DisconnectPinsResult.StatusCode, 200);

	const FString ReconnectPinsBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-connect-002"),
		TEXT("anim_blueprint_connect_pins"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"EventGraph\",\"from_node_guid\":\"%s\",\"from_pin\":\"%s\",\"to_node_guid\":\"%s\",\"to_pin\":\"%s\",\"compile_after_connect\":false,\"save_after_connect\":false}"), *AnimBlueprintAssetPath, *CustomEventNodeGuid, *EventExecPinName, *CallFunctionNodeGuid, *CallExecPinName));
	FHttpSmokeResult ReconnectPinsResult;
	TestTrue(TEXT("AnimBlueprint reconnect_pins request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ReconnectPinsBody, ReconnectPinsResult));
	TestEqual(TEXT("AnimBlueprint reconnect_pins status code"), ReconnectPinsResult.StatusCode, 200);

	const FString SetPinDefaultBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-set-pin-001"),
		TEXT("anim_blueprint_set_pin_default_value"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"EventGraph\",\"node_guid\":\"%s\",\"pin_name\":\"%s\",\"default_value\":\"%s\",\"compile_after_set\":false,\"save_after_set\":false}"), *AnimBlueprintAssetPath, *CallFunctionNodeGuid, *CallStringPinName, *StringValue));
	FHttpSmokeResult SetPinDefaultResult;
	TestTrue(TEXT("AnimBlueprint set_pin_default_value request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetPinDefaultBody, SetPinDefaultResult));
	TestEqual(TEXT("AnimBlueprint set_pin_default_value status code"), SetPinDefaultResult.StatusCode, 200);

	const FString AddFunctionGraphBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-add-function-001"),
		TEXT("anim_blueprint_add_function_graph"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"function_name\":\"%s\",\"compile_after_add\":false,\"save_after_add\":false}"), *AnimBlueprintAssetPath, *LogicFunctionName));
	FHttpSmokeResult AddFunctionGraphResult;
	TestTrue(TEXT("AnimBlueprint add_function_graph request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddFunctionGraphBody, AddFunctionGraphResult));
	TestEqual(TEXT("AnimBlueprint add_function_graph status code"), AddFunctionGraphResult.StatusCode, 200);

	const FString AddLocalLayerBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-add-layer-001"),
		TEXT("anim_blueprint_add_anim_layer"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"layer_name\":\"%s\",\"compile_after_add\":false,\"save_after_add\":false}"), *AnimBlueprintAssetPath, *LocalLayerName));
	FHttpSmokeResult AddLocalLayerResult;
	TestTrue(TEXT("AnimBlueprint add_anim_layer request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddLocalLayerBody, AddLocalLayerResult));
	TestEqual(TEXT("AnimBlueprint add_anim_layer status code"), AddLocalLayerResult.StatusCode, 200);

	const FString RenameLocalLayerBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-rename-layer-001"),
		TEXT("anim_blueprint_rename_anim_layer"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"layer_name\":\"%s\",\"new_layer_name\":\"%s\",\"compile_after_rename\":false,\"save_after_rename\":false}"), *AnimBlueprintAssetPath, *LocalLayerName, *RenamedLocalLayerName));
	FHttpSmokeResult RenameLocalLayerResult;
	TestTrue(TEXT("AnimBlueprint rename_anim_layer request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RenameLocalLayerBody, RenameLocalLayerResult));
	TestEqual(TEXT("AnimBlueprint rename_anim_layer status code"), RenameLocalLayerResult.StatusCode, 200);

	const FString ImplementLayerInterfaceBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-implement-interface-001"),
		TEXT("anim_blueprint_implement_layer_interface"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"interface_class\":\"%s\",\"compile_after_add\":false,\"save_after_add\":false}"), *AnimBlueprintAssetPath, *LayerInterfaceGeneratedClassPath));
	FHttpSmokeResult ImplementLayerInterfaceResult;
	TestTrue(TEXT("AnimBlueprint implement_layer_interface request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ImplementLayerInterfaceBody, ImplementLayerInterfaceResult));
	TestEqual(TEXT("AnimBlueprint implement_layer_interface status code"), ImplementLayerInterfaceResult.StatusCode, 200);

	const FString AddStateMachineBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-add-sm-001"),
		TEXT("anim_blueprint_add_state_machine"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"anim_graph_name\":\"AnimGraph\",\"pos_x\":-400,\"pos_y\":80,\"compile_after_add\":false,\"save_after_add\":false}"), *AnimBlueprintAssetPath, *StateMachineName));
	FHttpSmokeResult AddStateMachineResult;
	TestTrue(TEXT("AnimBlueprint add_state_machine request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddStateMachineBody, AddStateMachineResult));
	TestEqual(TEXT("AnimBlueprint add_state_machine status code"), AddStateMachineResult.StatusCode, 200);

	const FString AddIdleStateBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-add-state-idle-001"),
		TEXT("anim_blueprint_add_state"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"state_name\":\"%s\",\"pos_x\":-128,\"pos_y\":0,\"compile_after_add\":false,\"save_after_add\":false}"), *AnimBlueprintAssetPath, *StateMachineName, *IdleStateName));
	FHttpSmokeResult AddIdleStateResult;
	TestTrue(TEXT("AnimBlueprint add_state idle request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddIdleStateBody, AddIdleStateResult));
	TestEqual(TEXT("AnimBlueprint add_state idle status code"), AddIdleStateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddIdleStateJson;
	TestTrue(TEXT("AnimBlueprint add_state idle response parses as JSON"), ParseJson(AddIdleStateResult.Body, AddIdleStateJson));
	FString IdleStateNodeGuid;
	const TSharedPtr<FJsonObject>* AddIdleStateData = nullptr;
	TestTrue(TEXT("AnimBlueprint add_state idle contains data object"), AddIdleStateJson.IsValid() && AddIdleStateJson->TryGetObjectField(TEXT("data"), AddIdleStateData) && AddIdleStateData && AddIdleStateData->IsValid());
	if (AddIdleStateData && AddIdleStateData->IsValid())
	{
		IdleStateNodeGuid = (*AddIdleStateData)->GetStringField(TEXT("node_guid"));
	}

	const FString AddRunStateBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-add-state-run-001"),
		TEXT("anim_blueprint_add_state"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"state_name\":\"%s\",\"pos_x\":160,\"pos_y\":0,\"compile_after_add\":false,\"save_after_add\":false}"), *AnimBlueprintAssetPath, *StateMachineName, *RunStateName));
	FHttpSmokeResult AddRunStateResult;
	TestTrue(TEXT("AnimBlueprint add_state run request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddRunStateBody, AddRunStateResult));
	TestEqual(TEXT("AnimBlueprint add_state run status code"), AddRunStateResult.StatusCode, 200);

	const FString SetEntryStateBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-set-entry-001"),
		TEXT("anim_blueprint_set_entry_state"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"state_name\":\"%s\",\"compile_after_set\":false,\"save_after_set\":false}"), *AnimBlueprintAssetPath, *StateMachineName, *IdleStateName));
	FHttpSmokeResult SetEntryStateResult;
	TestTrue(TEXT("AnimBlueprint set_entry_state request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetEntryStateBody, SetEntryStateResult));
	TestEqual(TEXT("AnimBlueprint set_entry_state status code"), SetEntryStateResult.StatusCode, 200);

	const FString SetStatePropertiesBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-set-state-props-001"),
		TEXT("anim_blueprint_set_state_properties"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"state_name\":\"%s\",\"pos_x\":192,\"pos_y\":-32,\"always_reset_on_entry\":true,\"state_type\":\"blend_graph\",\"compile_after_set\":false,\"save_after_set\":false}"), *AnimBlueprintAssetPath, *StateMachineName, *RunStateName));
	FHttpSmokeResult SetStatePropertiesResult;
	TestTrue(TEXT("AnimBlueprint set_state_properties request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetStatePropertiesBody, SetStatePropertiesResult));
	TestEqual(TEXT("AnimBlueprint set_state_properties status code"), SetStatePropertiesResult.StatusCode, 200);

	const FString AddConduitBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-add-conduit-001"),
		TEXT("anim_blueprint_add_conduit"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"conduit_name\":\"%s\",\"pos_x\":16,\"pos_y\":160,\"compile_after_add\":false,\"save_after_add\":false}"), *AnimBlueprintAssetPath, *StateMachineName, *ConduitName));
	FHttpSmokeResult AddConduitResult;
	TestTrue(TEXT("AnimBlueprint add_conduit request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddConduitBody, AddConduitResult));
	TestEqual(TEXT("AnimBlueprint add_conduit status code"), AddConduitResult.StatusCode, 200);

	const FString AddAliasBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-add-alias-001"),
		TEXT("anim_blueprint_add_state_alias"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"alias_name\":\"%s\",\"pos_x\":16,\"pos_y\":-160,\"compile_after_add\":false,\"save_after_add\":false}"), *AnimBlueprintAssetPath, *StateMachineName, *AliasName));
	FHttpSmokeResult AddAliasResult;
	TestTrue(TEXT("AnimBlueprint add_state_alias request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddAliasBody, AddAliasResult));
	TestEqual(TEXT("AnimBlueprint add_state_alias status code"), AddAliasResult.StatusCode, 200);

	const FString SetAliasTargetsBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-set-alias-targets-001"),
		TEXT("anim_blueprint_set_state_alias_targets"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"alias_name\":\"%s\",\"global_alias\":false,\"alias_target_states\":[\"%s\"],\"compile_after_set\":false,\"save_after_set\":false}"), *AnimBlueprintAssetPath, *StateMachineName, *AliasName, *IdleStateName));
	FHttpSmokeResult SetAliasTargetsResult;
	TestTrue(TEXT("AnimBlueprint set_state_alias_targets request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetAliasTargetsBody, SetAliasTargetsResult));
	TestEqual(TEXT("AnimBlueprint set_state_alias_targets status code"), SetAliasTargetsResult.StatusCode, 200);

	const FString AddTransitionBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-add-transition-001"),
		TEXT("anim_blueprint_add_transition"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"source_state_name\":\"%s\",\"target_state_name\":\"%s\",\"priority_order\":2,\"crossfade_duration\":0.35,\"compile_after_add\":false,\"save_after_add\":false}"), *AnimBlueprintAssetPath, *StateMachineName, *IdleStateName, *RunStateName));
	FHttpSmokeResult AddTransitionResult;
	TestTrue(TEXT("AnimBlueprint add_transition request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddTransitionBody, AddTransitionResult));
	TestEqual(TEXT("AnimBlueprint add_transition status code"), AddTransitionResult.StatusCode, 200);

	const FString SetTransitionPropertiesBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-set-transition-props-001"),
		TEXT("anim_blueprint_set_transition_properties"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"source_state_name\":\"%s\",\"target_state_name\":\"%s\",\"priority_order\":3,\"crossfade_duration\":0.5,\"disabled\":true,\"automatic_rule\":true,\"pos_x\":12,\"pos_y\":44,\"compile_after_set\":false,\"save_after_set\":false}"), *AnimBlueprintAssetPath, *StateMachineName, *IdleStateName, *RunStateName));
	FHttpSmokeResult SetTransitionPropertiesResult;
	TestTrue(TEXT("AnimBlueprint set_transition_properties request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetTransitionPropertiesBody, SetTransitionPropertiesResult));
	TestEqual(TEXT("AnimBlueprint set_transition_properties status code"), SetTransitionPropertiesResult.StatusCode, 200);

	const FString CompileMainBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-compile-main-001"),
		TEXT("anim_blueprint_compile"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_messages\":true,\"max_messages\":24,\"save_after_compile\":false}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult CompileMainResult;
	TestTrue(TEXT("AnimBlueprint compile main request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CompileMainBody, CompileMainResult));
	TestEqual(TEXT("AnimBlueprint compile main status code"), CompileMainResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CompileMainJson;
	TestTrue(TEXT("AnimBlueprint compile main response parses as JSON"), ParseJson(CompileMainResult.Body, CompileMainJson));
	TestTrue(TEXT("AnimBlueprint compile main root ok=true"), CompileMainJson.IsValid() && CompileMainJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* CompileMainData = nullptr;
	TestTrue(TEXT("AnimBlueprint compile main contains data object"), CompileMainJson.IsValid() && CompileMainJson->TryGetObjectField(TEXT("data"), CompileMainData) && CompileMainData && CompileMainData->IsValid());
	if (CompileMainData && CompileMainData->IsValid())
	{
		TestFalse(TEXT("AnimBlueprint compile main reports no error"), (*CompileMainData)->GetBoolField(TEXT("has_error")));
	}

	const FString SetCdoPropertyBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-set-cdo-001"),
		TEXT("anim_blueprint_set_cdo_property"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"property_name\":\"%s\",\"value_text\":\"true\"}"), *AnimBlueprintAssetPath, *VariableName));
	FHttpSmokeResult SetCdoPropertyResult;
	TestTrue(TEXT("AnimBlueprint set_cdo_property request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetCdoPropertyBody, SetCdoPropertyResult));
	TestEqual(TEXT("AnimBlueprint set_cdo_property status code"), SetCdoPropertyResult.StatusCode, 200);

	const FString ListGraphsBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-list-graphs-001"),
		TEXT("anim_blueprint_list_graphs"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult ListGraphsResult;
	TestTrue(TEXT("AnimBlueprint list_graphs request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListGraphsBody, ListGraphsResult));
	TestEqual(TEXT("AnimBlueprint list_graphs status code"), ListGraphsResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ListGraphsJson;
	TestTrue(TEXT("AnimBlueprint list_graphs response parses as JSON"), ParseJson(ListGraphsResult.Body, ListGraphsJson));
	const TSharedPtr<FJsonObject>* ListGraphsData = nullptr;
	TestTrue(TEXT("AnimBlueprint list_graphs contains data object"), ListGraphsJson.IsValid() && ListGraphsJson->TryGetObjectField(TEXT("data"), ListGraphsData) && ListGraphsData && ListGraphsData->IsValid());
	if (ListGraphsData && ListGraphsData->IsValid())
	{
		TestTrue(TEXT("AnimBlueprint list_graphs includes logic function"), FindGraphByName(ListGraphsData, LogicFunctionName));
		TestTrue(TEXT("AnimBlueprint list_graphs includes local anim layer"), FindGraphByName(ListGraphsData, RenamedLocalLayerName));
		TestTrue(TEXT("AnimBlueprint list_graphs function_count>=2"), CountGraphsByType(ListGraphsData, TEXT("function")) >= 2);
	}

	const FString ListLayerInterfacesBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-list-interfaces-001"),
		TEXT("anim_blueprint_list_layer_interfaces"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult ListLayerInterfacesResult;
	TestTrue(TEXT("AnimBlueprint list_layer_interfaces request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListLayerInterfacesBody, ListLayerInterfacesResult));
	TestEqual(TEXT("AnimBlueprint list_layer_interfaces status code"), ListLayerInterfacesResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ListLayerInterfacesJson;
	TestTrue(TEXT("AnimBlueprint list_layer_interfaces response parses as JSON"), ParseJson(ListLayerInterfacesResult.Body, ListLayerInterfacesJson));
	const TSharedPtr<FJsonObject>* ListLayerInterfacesData = nullptr;
	TestTrue(TEXT("AnimBlueprint list_layer_interfaces contains data object"), ListLayerInterfacesJson.IsValid() && ListLayerInterfacesJson->TryGetObjectField(TEXT("data"), ListLayerInterfacesData) && ListLayerInterfacesData && ListLayerInterfacesData->IsValid());
	if (ListLayerInterfacesData && ListLayerInterfacesData->IsValid())
	{
		TestTrue(TEXT("AnimBlueprint list_layer_interfaces count>=1"), static_cast<int32>((*ListLayerInterfacesData)->GetIntegerField(TEXT("implemented_interface_count"))) >= 1);

		TSharedPtr<FJsonObject> InterfaceEntry;
		TestTrue(TEXT("AnimBlueprint list_layer_interfaces contains implemented interface"), FindLayerInterfaceEntry(ListLayerInterfacesData, LayerInterfaceGeneratedClassPath, InterfaceEntry));
		if (InterfaceEntry.IsValid())
		{
			TestTrue(TEXT("AnimBlueprint list_layer_interfaces graph_count>=1"), static_cast<int32>(InterfaceEntry->GetIntegerField(TEXT("graph_count"))) >= 1);
		}
	}

	const FString ListAnimLayersBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-list-layers-001"),
		TEXT("anim_blueprint_list_anim_layers"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult ListAnimLayersResult;
	TestTrue(TEXT("AnimBlueprint list_anim_layers request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListAnimLayersBody, ListAnimLayersResult));
	TestEqual(TEXT("AnimBlueprint list_anim_layers status code"), ListAnimLayersResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ListAnimLayersJson;
	TestTrue(TEXT("AnimBlueprint list_anim_layers response parses as JSON"), ParseJson(ListAnimLayersResult.Body, ListAnimLayersJson));
	const TSharedPtr<FJsonObject>* ListAnimLayersData = nullptr;
	TestTrue(TEXT("AnimBlueprint list_anim_layers contains data object"), ListAnimLayersJson.IsValid() && ListAnimLayersJson->TryGetObjectField(TEXT("data"), ListAnimLayersData) && ListAnimLayersData && ListAnimLayersData->IsValid());
	if (ListAnimLayersData && ListAnimLayersData->IsValid())
	{
		TSharedPtr<FJsonObject> LocalLayerEntry;
		TSharedPtr<FJsonObject> InterfaceLayerEntry;
		TestTrue(TEXT("AnimBlueprint list_anim_layers contains renamed local layer"), FindNamedAnimLayerEntry(ListAnimLayersData, RenamedLocalLayerName, LocalLayerEntry));
		TestTrue(TEXT("AnimBlueprint list_anim_layers contains interface layer"), FindNamedAnimLayerEntry(ListAnimLayersData, InterfaceLayerName, InterfaceLayerEntry));
		if (LocalLayerEntry.IsValid())
		{
			TestTrue(TEXT("AnimBlueprint local layer has graph"), LocalLayerEntry->GetBoolField(TEXT("has_graph")));
		}
		if (InterfaceLayerEntry.IsValid())
		{
			TestTrue(TEXT("AnimBlueprint interface layer marked as interface layer"), InterfaceLayerEntry->GetBoolField(TEXT("is_interface_layer")));
		}
	}

	const FString ListStateMachinesBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-list-sm-001"),
		TEXT("anim_blueprint_list_state_machines"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult ListStateMachinesResult;
	TestTrue(TEXT("AnimBlueprint list_state_machines request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListStateMachinesBody, ListStateMachinesResult));
	TestEqual(TEXT("AnimBlueprint list_state_machines status code"), ListStateMachinesResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ListStateMachinesJson;
	TestTrue(TEXT("AnimBlueprint list_state_machines response parses as JSON"), ParseJson(ListStateMachinesResult.Body, ListStateMachinesJson));
	const TSharedPtr<FJsonObject>* ListStateMachinesData = nullptr;
	TestTrue(TEXT("AnimBlueprint list_state_machines contains data object"), ListStateMachinesJson.IsValid() && ListStateMachinesJson->TryGetObjectField(TEXT("data"), ListStateMachinesData) && ListStateMachinesData && ListStateMachinesData->IsValid());
	FString IdleStateGraphPath;
	FString TransitionGraphPath;
	if (ListStateMachinesData && ListStateMachinesData->IsValid())
	{
		TSharedPtr<FJsonObject> StateMachineEntry;
		TestTrue(TEXT("AnimBlueprint list_state_machines contains machine"), FindNamedStateMachineEntry(ListStateMachinesData, StateMachineName, StateMachineEntry));
		if (StateMachineEntry.IsValid())
		{
			TestTrue(TEXT("AnimBlueprint state machine state_count>=2"), static_cast<int32>(StateMachineEntry->GetIntegerField(TEXT("state_count"))) >= 2);
			TestTrue(TEXT("AnimBlueprint state machine conduit_count>=1"), static_cast<int32>(StateMachineEntry->GetIntegerField(TEXT("conduit_count"))) >= 1);
			TestTrue(TEXT("AnimBlueprint state machine alias_count>=1"), static_cast<int32>(StateMachineEntry->GetIntegerField(TEXT("alias_count"))) >= 1);
			TestTrue(TEXT("AnimBlueprint state machine transition_count>=1"), static_cast<int32>(StateMachineEntry->GetIntegerField(TEXT("transition_count"))) >= 1);
			TestEqual(TEXT("AnimBlueprint state machine entry state is idle"), StateMachineEntry->GetStringField(TEXT("entry_connected_state_name")), IdleStateName);

			const TArray<TSharedPtr<FJsonValue>>* StateNodes = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* TransitionNodes = nullptr;
			TestTrue(TEXT("AnimBlueprint state machine includes state_nodes array"), StateMachineEntry->TryGetArrayField(TEXT("state_nodes"), StateNodes) && StateNodes != nullptr);
			TestTrue(TEXT("AnimBlueprint state machine includes transition_nodes array"), StateMachineEntry->TryGetArrayField(TEXT("transition_nodes"), TransitionNodes) && TransitionNodes != nullptr);

			TSharedPtr<FJsonObject> IdleStateEntry;
			TSharedPtr<FJsonObject> RunStateEntry;
			TSharedPtr<FJsonObject> TransitionEntry;
			TestTrue(TEXT("AnimBlueprint state machine contains idle state entry"), FindStateNodeEntry(StateMachineEntry, IdleStateName, IdleStateEntry));
			TestTrue(TEXT("AnimBlueprint state machine contains run state entry"), FindStateNodeEntry(StateMachineEntry, RunStateName, RunStateEntry));
			TestTrue(TEXT("AnimBlueprint state machine contains transition entry"), FindTransitionEntry(StateMachineEntry, IdleStateName, RunStateName, TransitionEntry));
			if (IdleStateEntry.IsValid())
			{
				IdleStateGraphPath = IdleStateEntry->GetStringField(TEXT("bound_graph_path"));
				TestFalse(TEXT("AnimBlueprint idle state bound graph path non-empty"), IdleStateGraphPath.IsEmpty());
			}
			if (RunStateEntry.IsValid())
			{
				TestEqual(TEXT("AnimBlueprint run state type updated"), RunStateEntry->GetStringField(TEXT("state_type")), FString(TEXT("blend_graph")));
				TestTrue(TEXT("AnimBlueprint run state always_reset_on_entry updated"), RunStateEntry->GetBoolField(TEXT("always_reset_on_entry")));
			}
			if (TransitionEntry.IsValid())
			{
				TransitionGraphPath = TransitionEntry->GetStringField(TEXT("bound_graph_path"));
				TestFalse(TEXT("AnimBlueprint transition bound graph path non-empty"), TransitionGraphPath.IsEmpty());
				TestTrue(TEXT("AnimBlueprint transition disabled updated"), TransitionEntry->GetBoolField(TEXT("disabled")));
				TestTrue(TEXT("AnimBlueprint transition automatic_rule updated"), TransitionEntry->GetBoolField(TEXT("automatic_rule")));
				TestEqual(TEXT("AnimBlueprint transition priority updated"), static_cast<int32>(TransitionEntry->GetIntegerField(TEXT("priority_order"))), 3);
			}
		}
	}

	const FString InspectStateMachineBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-inspect-sm-001"),
		TEXT("anim_blueprint_inspect_nodes"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"%s\",\"include_pins\":true,\"limit_per_graph\":128}"), *AnimBlueprintAssetPath, *StateMachineName));
	FHttpSmokeResult InspectStateMachineResult;
	TestTrue(TEXT("AnimBlueprint inspect_nodes on state machine request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, InspectStateMachineBody, InspectStateMachineResult));
	TestEqual(TEXT("AnimBlueprint inspect_nodes on state machine status code"), InspectStateMachineResult.StatusCode, 200);

	TSharedPtr<FJsonObject> InspectStateMachineJson;
	TestTrue(TEXT("AnimBlueprint inspect_nodes on state machine response parses as JSON"), ParseJson(InspectStateMachineResult.Body, InspectStateMachineJson));
	const TSharedPtr<FJsonObject>* InspectStateMachineData = nullptr;
	TestTrue(TEXT("AnimBlueprint inspect_nodes on state machine contains data object"), InspectStateMachineJson.IsValid() && InspectStateMachineJson->TryGetObjectField(TEXT("data"), InspectStateMachineData) && InspectStateMachineData && InspectStateMachineData->IsValid());
	if (InspectStateMachineData && InspectStateMachineData->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
		TestTrue(TEXT("AnimBlueprint inspect_nodes on state machine returns graph array"), (*InspectStateMachineData)->TryGetArrayField(TEXT("graphs"), Graphs) && Graphs != nullptr);
		if (Graphs)
		{
			TestTrue(TEXT("AnimBlueprint inspect_nodes on state machine graph_count>=1"), Graphs->Num() >= 1);
		}
	}

	const FString InspectIdleStateGraphBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-inspect-state-graph-001"),
		TEXT("anim_blueprint_inspect_nodes"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"%s\",\"include_pins\":true,\"limit_per_graph\":64}"), *AnimBlueprintAssetPath, *IdleStateGraphPath));
	FHttpSmokeResult InspectIdleStateGraphResult;
	TestTrue(TEXT("AnimBlueprint inspect_nodes on state graph request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, InspectIdleStateGraphBody, InspectIdleStateGraphResult));
	TestEqual(TEXT("AnimBlueprint inspect_nodes on state graph status code"), InspectIdleStateGraphResult.StatusCode, 200);

	const FString InspectTransitionGraphBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-inspect-transition-graph-001"),
		TEXT("anim_blueprint_inspect_nodes"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"%s\",\"include_pins\":true,\"limit_per_graph\":64}"), *AnimBlueprintAssetPath, *TransitionGraphPath));
	FHttpSmokeResult InspectTransitionGraphResult;
	TestTrue(TEXT("AnimBlueprint inspect_nodes on transition graph request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, InspectTransitionGraphBody, InspectTransitionGraphResult));
	TestEqual(TEXT("AnimBlueprint inspect_nodes on transition graph status code"), InspectTransitionGraphResult.StatusCode, 200);

	TSharedPtr<FJsonObject> InspectTransitionGraphJson;
	TestTrue(TEXT("AnimBlueprint inspect_nodes on transition graph parses JSON"), ParseJson(InspectTransitionGraphResult.Body, InspectTransitionGraphJson));
	const TSharedPtr<FJsonObject>* InspectTransitionGraphData = nullptr;
	TestTrue(TEXT("AnimBlueprint inspect_nodes on transition graph contains data object"), InspectTransitionGraphJson.IsValid() && InspectTransitionGraphJson->TryGetObjectField(TEXT("data"), InspectTransitionGraphData) && InspectTransitionGraphData && InspectTransitionGraphData->IsValid());

	TSharedPtr<FJsonObject> TransitionResultNodeObject;
	FString TransitionResultNodeGuid;
	FString TransitionResultPinName;
	TestTrue(TEXT("AnimBlueprint transition graph exposes result node"), FindNodeByClassContains(InspectTransitionGraphData, TEXT("TransitionResult"), TransitionResultNodeObject));
	if (TransitionResultNodeObject.IsValid())
	{
		TransitionResultNodeGuid = TransitionResultNodeObject->GetStringField(TEXT("node_guid"));
		TestTrue(TEXT("AnimBlueprint transition result has bool input pin"), FindPinName(TransitionResultNodeObject, TEXT("input"), TEXT("bool"), TransitionResultPinName));
	}

	const FString SetTransitionRulePinBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-set-transition-rule-pin-001"),
		TEXT("anim_blueprint_set_pin_default_value"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"graph_name\":\"%s\",\"node_guid\":\"%s\",\"pin_name\":\"%s\",\"default_value\":\"false\",\"compile_after_set\":false,\"save_after_set\":false}"), *AnimBlueprintAssetPath, *TransitionGraphPath, *TransitionResultNodeGuid, *TransitionResultPinName));
	FHttpSmokeResult SetTransitionRulePinResult;
	TestTrue(TEXT("AnimBlueprint set_pin_default_value on transition graph request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetTransitionRulePinBody, SetTransitionRulePinResult));
	TestEqual(TEXT("AnimBlueprint set_pin_default_value on transition graph status code"), SetTransitionRulePinResult.StatusCode, 200);

	const FString RenameStateNodeBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-rename-state-001"),
		TEXT("anim_blueprint_rename_state_node"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"state_name\":\"%s\",\"new_state_name\":\"%s\",\"compile_after_rename\":false,\"save_after_rename\":false}"), *AnimBlueprintAssetPath, *StateMachineName, *RunStateName, *RenamedRunStateName));
	FHttpSmokeResult RenameStateNodeResult;
	TestTrue(TEXT("AnimBlueprint rename_state_node request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RenameStateNodeBody, RenameStateNodeResult));
	TestEqual(TEXT("AnimBlueprint rename_state_node status code"), RenameStateNodeResult.StatusCode, 200);

	const FString RenameStateMachineBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-rename-sm-001"),
		TEXT("anim_blueprint_rename_state_machine"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"new_state_machine_name\":\"%s\",\"compile_after_rename\":false,\"save_after_rename\":false}"), *AnimBlueprintAssetPath, *StateMachineName, *RenamedStateMachineName));
	FHttpSmokeResult RenameStateMachineResult;
	TestTrue(TEXT("AnimBlueprint rename_state_machine request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RenameStateMachineBody, RenameStateMachineResult));
	TestEqual(TEXT("AnimBlueprint rename_state_machine status code"), RenameStateMachineResult.StatusCode, 200);

	const FString ClearEntryStateBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-clear-entry-001"),
		TEXT("anim_blueprint_clear_entry_state"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"compile_after_clear\":false,\"save_after_clear\":false}"), *AnimBlueprintAssetPath, *RenamedStateMachineName));
	FHttpSmokeResult ClearEntryStateResult;
	TestTrue(TEXT("AnimBlueprint clear_entry_state request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ClearEntryStateBody, ClearEntryStateResult));
	TestEqual(TEXT("AnimBlueprint clear_entry_state status code"), ClearEntryStateResult.StatusCode, 200);

	const FString RemoveTransitionBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-remove-transition-001"),
		TEXT("anim_blueprint_remove_transition"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"source_state_name\":\"%s\",\"target_state_name\":\"%s\",\"save_after_remove\":false}"), *AnimBlueprintAssetPath, *RenamedStateMachineName, *IdleStateName, *RenamedRunStateName));
	FHttpSmokeResult RemoveTransitionResult;
	TestTrue(TEXT("AnimBlueprint remove_transition request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveTransitionBody, RemoveTransitionResult));
	TestEqual(TEXT("AnimBlueprint remove_transition status code"), RemoveTransitionResult.StatusCode, 200);

	const FString RemoveAliasBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-remove-alias-001"),
		TEXT("anim_blueprint_remove_state_node"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"state_name\":\"%s\",\"save_after_remove\":false}"), *AnimBlueprintAssetPath, *RenamedStateMachineName, *AliasName));
	FHttpSmokeResult RemoveAliasResult;
	TestTrue(TEXT("AnimBlueprint remove_state_node alias request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveAliasBody, RemoveAliasResult));
	TestEqual(TEXT("AnimBlueprint remove_state_node alias status code"), RemoveAliasResult.StatusCode, 200);

	const FString RemoveConduitBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-remove-conduit-001"),
		TEXT("anim_blueprint_remove_state_node"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"state_name\":\"%s\",\"save_after_remove\":false}"), *AnimBlueprintAssetPath, *RenamedStateMachineName, *ConduitName));
	FHttpSmokeResult RemoveConduitResult;
	TestTrue(TEXT("AnimBlueprint remove_state_node conduit request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveConduitBody, RemoveConduitResult));
	TestEqual(TEXT("AnimBlueprint remove_state_node conduit status code"), RemoveConduitResult.StatusCode, 200);

	const FString RemoveLocalLayerBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-remove-layer-001"),
		TEXT("anim_blueprint_remove_anim_layer"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"layer_name\":\"%s\",\"compile_after_remove\":false,\"save_after_remove\":false}"), *AnimBlueprintAssetPath, *RenamedLocalLayerName));
	FHttpSmokeResult RemoveLocalLayerResult;
	TestTrue(TEXT("AnimBlueprint remove_anim_layer request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveLocalLayerBody, RemoveLocalLayerResult));
	TestEqual(TEXT("AnimBlueprint remove_anim_layer status code"), RemoveLocalLayerResult.StatusCode, 200);

	const FString RemoveLayerInterfaceBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-remove-interface-001"),
		TEXT("anim_blueprint_remove_layer_interface"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"interface_class\":\"%s\",\"compile_after_remove\":false,\"save_after_remove\":false}"), *AnimBlueprintAssetPath, *LayerInterfaceGeneratedClassPath));
	FHttpSmokeResult RemoveLayerInterfaceResult;
	TestTrue(TEXT("AnimBlueprint remove_layer_interface request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveLayerInterfaceBody, RemoveLayerInterfaceResult));
	TestEqual(TEXT("AnimBlueprint remove_layer_interface status code"), RemoveLayerInterfaceResult.StatusCode, 200);

	const FString RemoveStateMachineBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-remove-sm-001"),
		TEXT("anim_blueprint_remove_state_machine"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"state_machine_name\":\"%s\",\"save_after_remove\":false}"), *AnimBlueprintAssetPath, *RenamedStateMachineName));
	FHttpSmokeResult RemoveStateMachineResult;
	TestTrue(TEXT("AnimBlueprint remove_state_machine request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveStateMachineBody, RemoveStateMachineResult));
	TestEqual(TEXT("AnimBlueprint remove_state_machine status code"), RemoveStateMachineResult.StatusCode, 200);

	const FString FinalCompileBody = MakeExecRequestBody(
		TEXT("smoke-animbp-commands-final-compile-001"),
		TEXT("anim_blueprint_compile"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_messages\":true,\"max_messages\":24,\"save_after_compile\":false}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult FinalCompileResult;
	TestTrue(TEXT("AnimBlueprint final compile request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, FinalCompileBody, FinalCompileResult));
	TestEqual(TEXT("AnimBlueprint final compile status code"), FinalCompileResult.StatusCode, 200);

	TSharedPtr<FJsonObject> FinalCompileJson;
	TestTrue(TEXT("AnimBlueprint final compile response parses as JSON"), ParseJson(FinalCompileResult.Body, FinalCompileJson));
	TestTrue(TEXT("AnimBlueprint final compile root ok=true"), FinalCompileJson.IsValid() && FinalCompileJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* FinalCompileData = nullptr;
	TestTrue(TEXT("AnimBlueprint final compile contains data object"), FinalCompileJson.IsValid() && FinalCompileJson->TryGetObjectField(TEXT("data"), FinalCompileData) && FinalCompileData && FinalCompileData->IsValid());
	if (FinalCompileData && FinalCompileData->IsValid())
	{
		TestFalse(TEXT("AnimBlueprint final compile reports no error"), (*FinalCompileData)->GetBoolField(TEXT("has_error")));
		TestTrue(TEXT("AnimBlueprint final compile error count is zero"), static_cast<int32>((*FinalCompileData)->GetIntegerField(TEXT("error_count"))) == 0);
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceUmgPropertySmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.UmgPropertyWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceAnimBlueprintFolderSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.AnimBlueprintFolderWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceAnimBlueprintFolderSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString SkeletonObjectPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton.TutorialTPP_Skeleton");
	const FString SkeletalMeshObjectPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP.TutorialTPP");
	const FString BlendSpaceObjectPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/NewBlendSpace1D.NewBlendSpace1D");
	const FString AnimBlueprintAssetPath = MakeAutomationAssetPath(TEXT("ABPFolder"));
	FString RelativeFolder = AnimBlueprintAssetPath;
	while (RelativeFolder.StartsWith(TEXT("/")))
	{
		RelativeFolder.RightChopInline(1);
	}
	const FString FolderPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("AnimBlueprint"), RelativeFolder));
	IFileManager::Get().DeleteDirectory(*FolderPath, false, true);

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
		if (!Obj.IsValid())
		{
			AddError(FString::Printf(TEXT("Save json failed, invalid object: %s"), *FilePath));
			return false;
		}

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
			AddError(FString::Printf(TEXT("Write file failed: %s"), *FilePath));
			return false;
		}

		return true;
	};

	auto FindNodeObjectByClass = [](const TSharedPtr<FJsonObject>& GraphRoot, const FString& NodeClassPath, TSharedPtr<FJsonObject>& OutNodeObj) -> bool
	{
		if (!GraphRoot.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
		if (!GraphRoot->TryGetArrayField(TEXT("nodes"), NodesArray) || !NodesArray)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& NodeValue : *NodesArray)
		{
			const TSharedPtr<FJsonObject>* NodeObjPtr = nullptr;
			if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObjPtr) || !NodeObjPtr || !NodeObjPtr->IsValid())
			{
				continue;
			}

			FString NodeClass;
			if ((*NodeObjPtr)->TryGetStringField(TEXT("node_class"), NodeClass) && NodeClass.Equals(NodeClassPath, ESearchCase::IgnoreCase))
			{
				OutNodeObj = *NodeObjPtr;
				return true;
			}
		}

		return false;
	};

	auto FindPropertyObject = [](const TSharedPtr<FJsonObject>& NodeObj, const FString& PropertyName, TSharedPtr<FJsonObject>& OutPropertyObj) -> bool
	{
		if (!NodeObj.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* PropertiesArray = nullptr;
		if (!NodeObj->TryGetArrayField(TEXT("properties"), PropertiesArray) || !PropertiesArray)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& PropertyValue : *PropertiesArray)
		{
			const TSharedPtr<FJsonObject>* PropertyObjPtr = nullptr;
			if (!PropertyValue.IsValid() || !PropertyValue->TryGetObject(PropertyObjPtr) || !PropertyObjPtr || !PropertyObjPtr->IsValid())
			{
				continue;
			}

			FString CandidateName;
			if ((*PropertyObjPtr)->TryGetStringField(TEXT("property_name"), CandidateName) && CandidateName == PropertyName)
			{
				OutPropertyObj = *PropertyObjPtr;
				return true;
			}
		}

		return false;
	};

	auto FindBlendSpacePlayerNode = [](UAnimBlueprint* AnimBlueprint) -> UAnimGraphNode_BlendSpacePlayer*
	{
		if (!AnimBlueprint)
		{
			return nullptr;
		}

		TArray<UEdGraph*> AllGraphs;
		AnimBlueprint->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (!Graph)
			{
				continue;
			}

			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (UAnimGraphNode_BlendSpacePlayer* BlendSpaceNode = Cast<UAnimGraphNode_BlendSpacePlayer>(Node))
				{
					return BlendSpaceNode;
				}
			}
		}

		return nullptr;
	};

	const FString CreateBody = MakeExecRequestBody(
		TEXT("smoke-animbp-folder-create-001"),
		TEXT("anim_blueprint_create"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"parent_class\":\"/Script/Engine.AnimInstance\",\"target_skeleton\":\"%s\",\"preview_skeletal_mesh\":\"%s\",\"compile_after_create\":false,\"open_editor\":false,\"save_after_create\":false}"),
			*AnimBlueprintAssetPath,
			*SkeletonObjectPath,
			*SkeletalMeshObjectPath));
	FHttpSmokeResult CreateResult;
	TestTrue(TEXT("AnimBlueprint folder create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateBody, CreateResult));
	TestEqual(TEXT("AnimBlueprint folder create status code"), CreateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CreateJson;
	TestTrue(TEXT("AnimBlueprint folder create parses JSON"), ParseJson(CreateResult.Body, CreateJson));
	TestTrue(TEXT("AnimBlueprint folder create root ok=true"), CreateJson.IsValid() && CreateJson->GetBoolField(TEXT("ok")));

	FString AnimBlueprintObjectPath;
	const TSharedPtr<FJsonObject>* CreateData = nullptr;
	TestTrue(TEXT("AnimBlueprint folder create contains data object"), CreateJson.IsValid() && CreateJson->TryGetObjectField(TEXT("data"), CreateData) && CreateData && CreateData->IsValid());
	if (CreateData && CreateData->IsValid())
	{
		AnimBlueprintObjectPath = (*CreateData)->GetStringField(TEXT("object_path"));
	}

	UAnimBlueprint* EditableAnimBlueprint = AnimBlueprintObjectPath.IsEmpty() ? nullptr : LoadObject<UAnimBlueprint>(nullptr, *AnimBlueprintObjectPath);
	TestNotNull(TEXT("AnimBlueprint folder smoke local asset loads"), EditableAnimBlueprint);
	UBlendSpace* BlendSpaceAsset = LoadObject<UBlendSpace>(nullptr, *BlendSpaceObjectPath);
	TestNotNull(TEXT("AnimBlueprint folder smoke BlendSpace asset loads"), BlendSpaceAsset);
	if (EditableAnimBlueprint && BlendSpaceAsset)
	{
		UEdGraph* DefaultAnimGraph = nullptr;
		for (UEdGraph* Graph : EditableAnimBlueprint->FunctionGraphs)
		{
			if (Graph && AnimationEditorUtils::IsAnimGraph(Graph) && Graph->GetFName() == UEdGraphSchema_K2::GN_AnimGraph)
			{
				DefaultAnimGraph = Graph;
				break;
			}
		}

		TestNotNull(TEXT("AnimBlueprint folder smoke default anim graph exists"), DefaultAnimGraph);
		UAnimGraphNode_Root* RootNode = nullptr;
		if (DefaultAnimGraph)
		{
			for (UEdGraphNode* Node : DefaultAnimGraph->Nodes)
			{
				if (UAnimGraphNode_Root* CandidateRoot = Cast<UAnimGraphNode_Root>(Node))
				{
					RootNode = CandidateRoot;
					break;
				}
			}
		}
		TestNotNull(TEXT("AnimBlueprint folder smoke root node exists"), RootNode);

		UAnimGraphNode_BlendSpacePlayer* BlendSpaceNode = DefaultAnimGraph ? AddAutomationNodeToGraph<UAnimGraphNode_BlendSpacePlayer>(DefaultAnimGraph) : nullptr;
		TestNotNull(TEXT("AnimBlueprint folder smoke blendspace player node created"), BlendSpaceNode);
		if (BlendSpaceNode)
		{
			BlendSpaceNode->Node.SetBlendSpace(BlendSpaceAsset);
			BlendSpaceNode->Node.SetPosition(FVector(25.0f, 0.0f, 0.0f));
			BlendSpaceNode->NodePosX = 240;
			BlendSpaceNode->NodePosY = 120;
		}

		if (BlendSpaceNode && RootNode)
		{
			UEdGraphPin* OutputPin = nullptr;
			UEdGraphPin* InputPin = nullptr;
			for (UEdGraphPin* Pin : BlendSpaceNode->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Output)
				{
					OutputPin = Pin;
					break;
				}
			}
			for (UEdGraphPin* Pin : RootNode->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input)
				{
					InputPin = Pin;
					break;
				}
			}

			TestNotNull(TEXT("AnimBlueprint folder smoke blendspace output pin exists"), OutputPin);
			TestNotNull(TEXT("AnimBlueprint folder smoke root input pin exists"), InputPin);
			if (OutputPin && InputPin)
			{
				if (const UEdGraphSchema* Schema = DefaultAnimGraph->GetSchema())
				{
					Schema->TryCreateConnection(OutputPin, InputPin);
				}
				else
				{
					OutputPin->MakeLinkTo(InputPin);
				}
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(EditableAnimBlueprint);
		FKismetEditorUtilities::CompileBlueprint(EditableAnimBlueprint);
	}

	const FString ExportBody = MakeExecRequestBody(
		TEXT("smoke-animbp-folder-export-001"),
		TEXT("anim_blueprint_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clean_output_dir\":true,\"include_validation\":false}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult ExportResult;
	TestTrue(TEXT("AnimBlueprint folder export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ExportBody, ExportResult));
	TestEqual(TEXT("AnimBlueprint folder export status code"), ExportResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ExportJson;
	TestTrue(TEXT("AnimBlueprint folder export parses JSON"), ParseJson(ExportResult.Body, ExportJson));
	TestTrue(TEXT("AnimBlueprint folder export root ok=true"), ExportJson.IsValid() && ExportJson->GetBoolField(TEXT("ok")));

	const FString AnimGraphJsonPath = FPaths::Combine(FolderPath, TEXT("graphs"), TEXT("AnimLayer_AnimGraph.json"));
	TSharedPtr<FJsonObject> AnimGraphJson;
	TestTrue(TEXT("AnimBlueprint folder exported AnimGraph json loads"), LoadJsonFile(AnimGraphJsonPath, AnimGraphJson));

	TSharedPtr<FJsonObject> BlendSpaceNodeJson;
	TestTrue(
		TEXT("AnimBlueprint folder exported graph contains BlendSpace player node"),
		FindNodeObjectByClass(AnimGraphJson, TEXT("/Script/AnimGraph.AnimGraphNode_BlendSpacePlayer"), BlendSpaceNodeJson));

	TSharedPtr<FJsonObject> BlendSpacePropertyJson;
	FString OriginalBlendSpaceValueText;
	TestTrue(
		TEXT("AnimBlueprint folder exported BlendSpace property exists"),
		FindPropertyObject(BlendSpaceNodeJson, TEXT("Node.BlendSpace"), BlendSpacePropertyJson));
	if (BlendSpacePropertyJson.IsValid())
	{
		OriginalBlendSpaceValueText = BlendSpacePropertyJson->GetStringField(TEXT("value_text"));
		TestTrue(TEXT("AnimBlueprint folder exported BlendSpace property references asset"), OriginalBlendSpaceValueText.Contains(TEXT("NewBlendSpace1D")));
		BlendSpacePropertyJson->SetStringField(TEXT("value_text"), TEXT("None"));
	}

	TestTrue(TEXT("AnimBlueprint folder mutated AnimGraph json saves"), SaveJsonFile(AnimGraphJsonPath, AnimGraphJson));

	const FString ApplyNoneBody = MakeExecRequestBody(
		TEXT("smoke-animbp-folder-apply-001"),
		TEXT("anim_blueprint_apply_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"create_if_missing\":false,\"compile_after_apply\":false,\"save_after_apply\":false}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult ApplyNoneResult;
	TestTrue(TEXT("AnimBlueprint folder apply request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ApplyNoneBody, ApplyNoneResult));
	TestEqual(TEXT("AnimBlueprint folder apply status code"), ApplyNoneResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ApplyNoneJson;
	TestTrue(TEXT("AnimBlueprint folder apply parses JSON"), ParseJson(ApplyNoneResult.Body, ApplyNoneJson));
	TestTrue(TEXT("AnimBlueprint folder apply root ok=true"), ApplyNoneJson.IsValid() && ApplyNoneJson->GetBoolField(TEXT("ok")));

	UAnimBlueprint* AppliedNoneAnimBlueprint = LoadObject<UAnimBlueprint>(nullptr, *AnimBlueprintObjectPath);
	TestNotNull(TEXT("AnimBlueprint folder smoke reload after apply none"), AppliedNoneAnimBlueprint);
	if (UAnimGraphNode_BlendSpacePlayer* BlendSpaceNodeAfterNone = FindBlendSpacePlayerNode(AppliedNoneAnimBlueprint))
	{
		TestTrue(TEXT("AnimBlueprint folder apply sets BlendSpace to None"), BlendSpaceNodeAfterNone->Node.GetBlendSpace() == nullptr);
	}
	else
	{
		AddError(TEXT("AnimBlueprint folder apply could not find BlendSpace player node after apply."));
	}

	if (BlendSpacePropertyJson.IsValid())
	{
		BlendSpacePropertyJson->SetStringField(TEXT("value_text"), OriginalBlendSpaceValueText);
	}
	TestTrue(TEXT("AnimBlueprint folder restored AnimGraph json saves"), SaveJsonFile(AnimGraphJsonPath, AnimGraphJson));

	const FString ApplyRestoreBody = MakeExecRequestBody(
		TEXT("smoke-animbp-folder-apply-002"),
		TEXT("anim_blueprint_apply_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"create_if_missing\":false,\"compile_after_apply\":true,\"save_after_apply\":false}"), *AnimBlueprintAssetPath));
	FHttpSmokeResult ApplyRestoreResult;
	TestTrue(TEXT("AnimBlueprint folder apply restore request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ApplyRestoreBody, ApplyRestoreResult));
	TestEqual(TEXT("AnimBlueprint folder apply restore status code"), ApplyRestoreResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ApplyRestoreJson;
	TestTrue(TEXT("AnimBlueprint folder apply restore parses JSON"), ParseJson(ApplyRestoreResult.Body, ApplyRestoreJson));
	TestTrue(TEXT("AnimBlueprint folder apply restore root ok=true"), ApplyRestoreJson.IsValid() && ApplyRestoreJson->GetBoolField(TEXT("ok")));

	UAnimBlueprint* AppliedRestoreAnimBlueprint = LoadObject<UAnimBlueprint>(nullptr, *AnimBlueprintObjectPath);
	TestNotNull(TEXT("AnimBlueprint folder smoke reload after restore"), AppliedRestoreAnimBlueprint);
	if (UAnimGraphNode_BlendSpacePlayer* BlendSpaceNodeAfterRestore = FindBlendSpacePlayerNode(AppliedRestoreAnimBlueprint))
	{
		const UBlendSpace* RestoredBlendSpace = BlendSpaceNodeAfterRestore->Node.GetBlendSpace();
		TestNotNull(TEXT("AnimBlueprint folder apply restores BlendSpace asset"), RestoredBlendSpace);
		if (RestoredBlendSpace)
		{
			TestEqual(TEXT("AnimBlueprint folder restored BlendSpace asset path matches"), RestoredBlendSpace->GetPathName(), BlendSpaceObjectPath);
		}
	}
	else
	{
		AddError(TEXT("AnimBlueprint folder restore could not find BlendSpace player node after apply."));
	}

	return !HasAnyErrors();
}

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
	FHttpSmokeResult DeleteExpressionResult;
	TestTrue(TEXT("Material lifecycle delete_expression request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DeleteExpressionBody, DeleteExpressionResult));
	TestEqual(TEXT("Material lifecycle delete_expression status code"), DeleteExpressionResult.StatusCode, 200);

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceAnimationAssetsSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.AnimationAssetsWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceAnimationAssetsSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString SourceAnimSequencePath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle");
	const FString SourceSkeletonPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton");
	const FString SourceSkeletalMeshPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP");
	const FString AnimMetadataClassPath = TEXT("/Script/GptProjectTest.GptAnimMetaTag");
	const FString BoneCompressionSettingsPath = TEXT("/Engine/Animation/DefaultRecorderBoneCompression");
	const FString CurveCompressionSettingsPath = TEXT("/Engine/Animation/DefaultAnimCurveCompressionSettings");
	const FString VariableFrameStrippingSettingsPath = TEXT("/Engine/Animation/DefaultVariableFrameStrippingSettings");
	const FString SequenceAssetPath = MakeAutomationAssetPath(TEXT("AnimSeq"));
	const FString SkeletonAssetPath = MakeAutomationAssetPath(TEXT("Skeleton"));
	const FString NotifyTrackName = TEXT("Stage20Markers");
	const FString NotifyName = TEXT("Stage22Notify");
	const FString FloatCurveName = TEXT("Stage23FloatCurve");
	const FString MarkerA = TEXT("Stage20MarkerA");
	const FString MarkerB = TEXT("Stage20MarkerB");
	const FString SocketName = TEXT("Stage20Socket");
	FString VirtualBoneName;

	auto FindNotifyTrack = [](const TSharedPtr<FJsonObject>* InfoData, const FString& TrackName, TSharedPtr<FJsonObject>& OutTrackObject) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("notify_tracks"), Tracks) || !Tracks)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& TrackValue : *Tracks)
		{
			const TSharedPtr<FJsonObject>* TrackObject = nullptr;
			if (!TrackValue.IsValid() || !TrackValue->TryGetObject(TrackObject) || !TrackObject || !TrackObject->IsValid())
			{
				continue;
			}
			if ((*TrackObject)->GetStringField(TEXT("track_name")) == TrackName)
			{
				OutTrackObject = *TrackObject;
				return true;
			}
		}

		return false;
	};

	auto FindNotifyNamed = [](const TSharedPtr<FJsonObject>* InfoData, const FString& InNotifyName, TSharedPtr<FJsonObject>& OutNotifyObject) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Notifies = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("notifies"), Notifies) || !Notifies)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& NotifyValue : *Notifies)
		{
			const TSharedPtr<FJsonObject>* NotifyObject = nullptr;
			if (!NotifyValue.IsValid() || !NotifyValue->TryGetObject(NotifyObject) || !NotifyObject || !NotifyObject->IsValid())
			{
				continue;
			}
			if ((*NotifyObject)->GetStringField(TEXT("notify_name")) == InNotifyName)
			{
				OutNotifyObject = *NotifyObject;
				return true;
			}
		}
		return false;
	};

	auto FindCurveNamed = [](const TSharedPtr<FJsonObject>* InfoData, const FString& InCurveName, TSharedPtr<FJsonObject>& OutCurveObject) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Curves = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("curves"), Curves) || !Curves)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& CurveValue : *Curves)
		{
			const TSharedPtr<FJsonObject>* CurveObject = nullptr;
			if (!CurveValue.IsValid() || !CurveValue->TryGetObject(CurveObject) || !CurveObject || !CurveObject->IsValid())
			{
				continue;
			}
			if ((*CurveObject)->GetStringField(TEXT("curve_name")) == InCurveName)
			{
				OutCurveObject = *CurveObject;
				return true;
			}
		}
		return false;
	};

	auto GetTrackNames = [](const TSharedPtr<FJsonObject>* InfoData, TArray<FString>& OutTrackNames) -> bool
	{
		OutTrackNames.Reset();
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* TrackNames = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("track_names"), TrackNames) || !TrackNames)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *TrackNames)
		{
			if (Value.IsValid())
			{
				OutTrackNames.Add(Value->AsString());
			}
		}
		return true;
	};

	auto HasMetadataClass = [](const TSharedPtr<FJsonObject>* InfoData, const FString& InClassPath) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* MetaDataArray = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("metadata"), MetaDataArray) || !MetaDataArray)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& MetaValue : *MetaDataArray)
		{
			const TSharedPtr<FJsonObject>* MetaObject = nullptr;
			if (!MetaValue.IsValid() || !MetaValue->TryGetObject(MetaObject) || !MetaObject || !MetaObject->IsValid())
			{
				continue;
			}
			if ((*MetaObject)->GetStringField(TEXT("class")) == InClassPath)
			{
				return true;
			}
		}
		return false;
	};

	auto FindMetadataByClass = [](const TSharedPtr<FJsonObject>* InfoData, const FString& InClassPath, TSharedPtr<FJsonObject>& OutMetadataObject) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* MetaDataArray = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("metadata"), MetaDataArray) || !MetaDataArray)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& MetaValue : *MetaDataArray)
		{
			const TSharedPtr<FJsonObject>* MetaObject = nullptr;
			if (!MetaValue.IsValid() || !MetaValue->TryGetObject(MetaObject) || !MetaObject || !MetaObject->IsValid())
			{
				continue;
			}
			if ((*MetaObject)->GetStringField(TEXT("class")) == InClassPath)
			{
				OutMetadataObject = *MetaObject;
				return true;
			}
		}
		return false;
	};

	auto HasMarkerNamed = [](const TSharedPtr<FJsonObject>* InfoData, const FString& MarkerName) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Markers = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("sync_markers"), Markers) || !Markers)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& MarkerValue : *Markers)
		{
			const TSharedPtr<FJsonObject>* MarkerObject = nullptr;
			if (!MarkerValue.IsValid() || !MarkerValue->TryGetObject(MarkerObject) || !MarkerObject || !MarkerObject->IsValid())
			{
				continue;
			}
			if ((*MarkerObject)->GetStringField(TEXT("marker_name")) == MarkerName)
			{
				return true;
			}
		}
		return false;
	};

	auto FindSocketNamed = [](const TSharedPtr<FJsonObject>* InfoData, const FString& InSocketName, TSharedPtr<FJsonObject>& OutSocketObject) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Sockets = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("sockets"), Sockets) || !Sockets)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& SocketValue : *Sockets)
		{
			const TSharedPtr<FJsonObject>* SocketObject = nullptr;
			if (!SocketValue.IsValid() || !SocketValue->TryGetObject(SocketObject) || !SocketObject || !SocketObject->IsValid())
			{
				continue;
			}
			if ((*SocketObject)->GetStringField(TEXT("socket_name")) == InSocketName)
			{
				OutSocketObject = *SocketObject;
				return true;
			}
		}
		return false;
	};

	auto FindVirtualBoneNamed = [](const TSharedPtr<FJsonObject>* InfoData, const FString& InVirtualBoneName) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* VirtualBones = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("virtual_bones"), VirtualBones) || !VirtualBones)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& VirtualBoneValue : *VirtualBones)
		{
			const TSharedPtr<FJsonObject>* VirtualBoneObject = nullptr;
			if (!VirtualBoneValue.IsValid() || !VirtualBoneValue->TryGetObject(VirtualBoneObject) || !VirtualBoneObject || !VirtualBoneObject->IsValid())
			{
				continue;
			}
			if ((*VirtualBoneObject)->GetStringField(TEXT("virtual_bone_name")) == InVirtualBoneName)
			{
				return true;
			}
		}
		return false;
	};

	const FString DuplicateSequenceBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-dup-seq-001"),
		TEXT("asset_duplicate"),
		FString::Printf(TEXT("{\"source_asset_path\":\"%s\",\"destination_asset_path\":\"%s\",\"save_after_duplicate\":false}"), *SourceAnimSequencePath, *SequenceAssetPath));
	FHttpSmokeResult DuplicateSequenceResult;
	TestTrue(TEXT("Animation assets duplicate sequence request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DuplicateSequenceBody, DuplicateSequenceResult));
	TestEqual(TEXT("Animation assets duplicate sequence status code"), DuplicateSequenceResult.StatusCode, 200);

	const FString DuplicateSkeletonBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-dup-skeleton-001"),
		TEXT("asset_duplicate"),
		FString::Printf(TEXT("{\"source_asset_path\":\"%s\",\"destination_asset_path\":\"%s\",\"save_after_duplicate\":false}"), *SourceSkeletonPath, *SkeletonAssetPath));
	FHttpSmokeResult DuplicateSkeletonResult;
	TestTrue(TEXT("Animation assets duplicate skeleton request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DuplicateSkeletonBody, DuplicateSkeletonResult));
	TestEqual(TEXT("Animation assets duplicate skeleton status code"), DuplicateSkeletonResult.StatusCode, 200);

	const FString SequenceInfoBeforeBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-seq-info-before-001"),
		TEXT("anim_sequence_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SequenceAssetPath));
	FHttpSmokeResult SequenceInfoBeforeResult;
	TestTrue(TEXT("Animation assets anim_sequence_get_info before request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SequenceInfoBeforeBody, SequenceInfoBeforeResult));
	TestEqual(TEXT("Animation assets anim_sequence_get_info before status code"), SequenceInfoBeforeResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SequenceInfoBeforeJson;
	TestTrue(TEXT("Animation assets anim_sequence_get_info before parses JSON"), ParseJson(SequenceInfoBeforeResult.Body, SequenceInfoBeforeJson));
	TestTrue(TEXT("Animation assets anim_sequence_get_info before root ok=true"), SequenceInfoBeforeJson.IsValid() && SequenceInfoBeforeJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* SequenceInfoBeforeData = nullptr;
	TestTrue(TEXT("Animation assets anim_sequence_get_info before contains data object"), SequenceInfoBeforeJson.IsValid() && SequenceInfoBeforeJson->TryGetObjectField(TEXT("data"), SequenceInfoBeforeData) && SequenceInfoBeforeData && SequenceInfoBeforeData->IsValid());
	if (SequenceInfoBeforeData && SequenceInfoBeforeData->IsValid())
	{
		TestTrue(TEXT("Animation assets sequence length > 0"), (*SequenceInfoBeforeData)->GetNumberField(TEXT("sequence_length")) > 0.0);
		TestTrue(TEXT("Animation assets sequence frames > 0"), static_cast<int32>((*SequenceInfoBeforeData)->GetIntegerField(TEXT("num_frames"))) > 0);
	}

	TArray<FString> InitialTrackNames;
	TestTrue(TEXT("Animation assets initial track names exist"), GetTrackNames(SequenceInfoBeforeData, InitialTrackNames));
	FString TrackToRemove;
	for (const FString& TrackName : InitialTrackNames)
	{
		if (!TrackName.Equals(TEXT("Root"), ESearchCase::IgnoreCase))
		{
			TrackToRemove = TrackName;
			break;
		}
	}
	if (TrackToRemove.IsEmpty() && InitialTrackNames.Num() > 0)
	{
		TrackToRemove = InitialTrackNames[0];
	}
	TestTrue(TEXT("Animation assets selected bone track to remove is non-empty"), !TrackToRemove.IsEmpty());

	const FString SetSequenceSettingsBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-seq-settings-001"),
		TEXT("anim_sequence_set_settings"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"rate_scale\":1.25,\"enable_root_motion\":true,\"force_root_lock\":true,\"root_motion_lock_type\":\"zero\",\"interpolation_type\":\"step\",\"additive_base_pose_type\":\"anim_frame\",\"base_pose_sequence_path\":\"%s\",\"base_pose_frame_index\":3,\"retarget_source_asset_path\":\"%s\",\"bone_compression_settings_path\":\"%s\",\"curve_compression_settings_path\":\"%s\",\"variable_frame_stripping_settings_path\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *SourceAnimSequencePath, *SourceSkeletalMeshPath, *BoneCompressionSettingsPath, *CurveCompressionSettingsPath, *VariableFrameStrippingSettingsPath));
	FHttpSmokeResult SetSequenceSettingsResult;
	TestTrue(TEXT("Animation assets anim_sequence_set_settings request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetSequenceSettingsBody, SetSequenceSettingsResult));
	TestEqual(TEXT("Animation assets anim_sequence_set_settings status code"), SetSequenceSettingsResult.StatusCode, 200);

	const FString SetSequencePreviewMeshBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-seq-preview-001"),
		TEXT("anim_sequence_set_preview_mesh"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"skeletal_mesh_path\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *SourceSkeletalMeshPath));
	FHttpSmokeResult SetSequencePreviewMeshResult;
	TestTrue(TEXT("Animation assets anim_sequence_set_preview_mesh request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetSequencePreviewMeshBody, SetSequencePreviewMeshResult));
	TestEqual(TEXT("Animation assets anim_sequence_set_preview_mesh status code"), SetSequencePreviewMeshResult.StatusCode, 200);

	const FString SetNotifyTrackBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-notify-track-001"),
		TEXT("anim_sequence_set_notify_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"track_name\":\"%s\",\"track_color\":{\"r\":0.25,\"g\":0.45,\"b\":0.80,\"a\":1.0},\"save_after_set\":false}"), *SequenceAssetPath, *NotifyTrackName));
	FHttpSmokeResult SetNotifyTrackResult;
	TestTrue(TEXT("Animation assets anim_sequence_set_notify_track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetNotifyTrackBody, SetNotifyTrackResult));
	TestEqual(TEXT("Animation assets anim_sequence_set_notify_track status code"), SetNotifyTrackResult.StatusCode, 200);

	const FString AddNotifyBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-notify-001"),
		TEXT("anim_sequence_set_notify"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"track_name\":\"%s\",\"time_seconds\":0.20,\"notify_name\":\"%s\",\"trigger_weight_threshold\":0.25,\"notify_trigger_chance\":1.0,\"notify_filter_type\":\"lod\",\"notify_filter_lod\":1,\"notify_color\":{\"r\":0.9,\"g\":0.3,\"b\":0.2,\"a\":1.0},\"save_after_set\":false}"), *SequenceAssetPath, *NotifyTrackName, *NotifyName));
	FHttpSmokeResult AddNotifyResult;
	TestTrue(TEXT("Animation assets anim_sequence_set_notify add request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddNotifyBody, AddNotifyResult));
	TestEqual(TEXT("Animation assets anim_sequence_set_notify add status code"), AddNotifyResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddNotifyJson;
	TestTrue(TEXT("Animation assets anim_sequence_set_notify add parses JSON"), ParseJson(AddNotifyResult.Body, AddNotifyJson));
	const TSharedPtr<FJsonObject>* AddNotifyData = nullptr;
	TestTrue(TEXT("Animation assets anim_sequence_set_notify add contains data object"), AddNotifyJson.IsValid() && AddNotifyJson->TryGetObjectField(TEXT("data"), AddNotifyData) && AddNotifyData && AddNotifyData->IsValid());
	int32 NotifyIndex = INDEX_NONE;
	if (AddNotifyData && AddNotifyData->IsValid())
	{
		const TSharedPtr<FJsonObject>* NotifyObject = nullptr;
		TestTrue(TEXT("Animation assets anim_sequence_set_notify add contains notify object"), (*AddNotifyData)->TryGetObjectField(TEXT("notify"), NotifyObject) && NotifyObject && NotifyObject->IsValid());
		if (NotifyObject && NotifyObject->IsValid())
		{
			NotifyIndex = static_cast<int32>((*NotifyObject)->GetIntegerField(TEXT("notify_index")));
			TestEqual(TEXT("Animation assets notify add name"), (*NotifyObject)->GetStringField(TEXT("notify_name")), NotifyName);
		}
	}

	const FString UpdateNotifyBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-notify-002"),
		TEXT("anim_sequence_set_notify"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"notify_index\":%d,\"time_seconds\":0.24,\"notify_color\":{\"r\":0.15,\"g\":0.65,\"b\":0.95,\"a\":1.0},\"save_after_set\":false}"), *SequenceAssetPath, NotifyIndex));
	FHttpSmokeResult UpdateNotifyResult;
	TestTrue(TEXT("Animation assets anim_sequence_set_notify update request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, UpdateNotifyBody, UpdateNotifyResult));
	TestEqual(TEXT("Animation assets anim_sequence_set_notify update status code"), UpdateNotifyResult.StatusCode, 200);

	const FString AddFloatCurveBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-curve-001"),
		TEXT("anim_sequence_set_curve"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"curve_name\":\"%s\",\"curve_type\":\"float\",\"clear_existing_keys\":true,\"keys\":[{\"time_seconds\":0.10,\"value\":1.0},{\"time_seconds\":0.20,\"value\":2.0}],\"save_after_set\":false}"), *SequenceAssetPath, *FloatCurveName));
	FHttpSmokeResult AddFloatCurveResult;
	TestTrue(TEXT("Animation assets anim_sequence_set_curve float request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddFloatCurveBody, AddFloatCurveResult));
	TestEqual(TEXT("Animation assets anim_sequence_set_curve float status code"), AddFloatCurveResult.StatusCode, 200);

	const FString AddMetadataBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-metadata-001"),
		TEXT("anim_sequence_set_metadata"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"metadata_class_path\":\"%s\",\"metadata_values\":{\"tag\":\"Stage26Tag\",\"note\":\"Stage26Note\"},\"save_after_set\":false}"), *SequenceAssetPath, *AnimMetadataClassPath));
	FHttpSmokeResult AddMetadataResult;
	TestTrue(TEXT("Animation assets anim_sequence_set_metadata add request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddMetadataBody, AddMetadataResult));
	TestEqual(TEXT("Animation assets anim_sequence_set_metadata add status code"), AddMetadataResult.StatusCode, 200);

	const FString UpdateMetadataBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-metadata-001b"),
		TEXT("anim_sequence_set_metadata"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"metadata_class_path\":\"%s\",\"metadata_values\":{\"tag\":\"Stage26Tag\",\"note\":\"Stage26NoteUpdated\"},\"save_after_set\":false}"), *SequenceAssetPath, *AnimMetadataClassPath));
	FHttpSmokeResult UpdateMetadataResult;
	TestTrue(TEXT("Animation assets anim_sequence_set_metadata update request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, UpdateMetadataBody, UpdateMetadataResult));
	TestEqual(TEXT("Animation assets anim_sequence_set_metadata update status code"), UpdateMetadataResult.StatusCode, 200);

	const FString RemoveBoneTrackBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-bones-001"),
		TEXT("anim_sequence_set_bones"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"remove_bone_names\":[\"%s\"],\"include_children\":false,\"finalize_after_set\":true,\"save_after_set\":false}"), *SequenceAssetPath, *TrackToRemove));
	FHttpSmokeResult RemoveBoneTrackResult;
	TestTrue(TEXT("Animation assets anim_sequence_set_bones request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveBoneTrackBody, RemoveBoneTrackResult));
	TestEqual(TEXT("Animation assets anim_sequence_set_bones status code"), RemoveBoneTrackResult.StatusCode, 200);

	const FString SetSyncMarkersBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-sync-markers-001"),
		TEXT("anim_sequence_set_sync_markers"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"add_markers\":[{\"marker_name\":\"%s\",\"time_seconds\":0.10,\"notify_track_name\":\"%s\"},{\"marker_name\":\"%s\",\"time_seconds\":0.30,\"notify_track_name\":\"%s\"}],\"save_after_set\":false}"), *SequenceAssetPath, *MarkerA, *NotifyTrackName, *MarkerB, *NotifyTrackName));
	FHttpSmokeResult SetSyncMarkersResult;
	TestTrue(TEXT("Animation assets anim_sequence_set_sync_markers add request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetSyncMarkersBody, SetSyncMarkersResult));
	TestEqual(TEXT("Animation assets anim_sequence_set_sync_markers add status code"), SetSyncMarkersResult.StatusCode, 200);

	const FString SequenceInfoAfterBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-seq-info-after-001"),
		TEXT("anim_sequence_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_curve_keys\":true}"), *SequenceAssetPath));
	FHttpSmokeResult SequenceInfoAfterResult;
	TestTrue(TEXT("Animation assets anim_sequence_get_info after request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SequenceInfoAfterBody, SequenceInfoAfterResult));
	TestEqual(TEXT("Animation assets anim_sequence_get_info after status code"), SequenceInfoAfterResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SequenceInfoAfterJson;
	TestTrue(TEXT("Animation assets anim_sequence_get_info after parses JSON"), ParseJson(SequenceInfoAfterResult.Body, SequenceInfoAfterJson));
	const TSharedPtr<FJsonObject>* SequenceInfoAfterData = nullptr;
	TestTrue(TEXT("Animation assets anim_sequence_get_info after contains data object"), SequenceInfoAfterJson.IsValid() && SequenceInfoAfterJson->TryGetObjectField(TEXT("data"), SequenceInfoAfterData) && SequenceInfoAfterData && SequenceInfoAfterData->IsValid());
	if (SequenceInfoAfterData && SequenceInfoAfterData->IsValid())
	{
		TestEqual(TEXT("Animation assets sequence preview mesh updated"), (*SequenceInfoAfterData)->GetStringField(TEXT("preview_skeletal_mesh")), SourceSkeletalMeshPath + TEXT(".TutorialTPP"));
		TestTrue(TEXT("Animation assets sequence rate scale updated"), FMath::IsNearlyEqual(static_cast<float>((*SequenceInfoAfterData)->GetNumberField(TEXT("rate_scale"))), 1.25f, 0.001f));
		TestTrue(TEXT("Animation assets sequence root motion enabled"), (*SequenceInfoAfterData)->GetBoolField(TEXT("root_motion_enabled")));
		TestTrue(TEXT("Animation assets sequence force root lock enabled"), (*SequenceInfoAfterData)->GetBoolField(TEXT("force_root_lock")));
		TestEqual(TEXT("Animation assets sequence root motion lock type updated"), (*SequenceInfoAfterData)->GetStringField(TEXT("root_motion_lock_type")), FString(TEXT("zero")));
		TestEqual(TEXT("Animation assets sequence interpolation updated"), (*SequenceInfoAfterData)->GetStringField(TEXT("interpolation_type")), FString(TEXT("step")));
		TestTrue(TEXT("Animation assets sequence base pose sequence updated"), (*SequenceInfoAfterData)->GetStringField(TEXT("additive_base_pose_sequence")).StartsWith(SourceAnimSequencePath));
		TestEqual(TEXT("Animation assets sequence base pose frame updated"), static_cast<int32>((*SequenceInfoAfterData)->GetIntegerField(TEXT("additive_base_pose_frame_index"))), 3);
		TestTrue(TEXT("Animation assets sequence retarget source asset updated"), (*SequenceInfoAfterData)->GetStringField(TEXT("retarget_source_asset")).StartsWith(SourceSkeletalMeshPath));
		TestTrue(TEXT("Animation assets sequence bone compression settings updated"), (*SequenceInfoAfterData)->GetStringField(TEXT("bone_compression_settings")).StartsWith(BoneCompressionSettingsPath));
		TestTrue(TEXT("Animation assets sequence curve compression settings updated"), (*SequenceInfoAfterData)->GetStringField(TEXT("curve_compression_settings")).StartsWith(CurveCompressionSettingsPath));
		TestTrue(TEXT("Animation assets sequence variable frame stripping settings updated"), (*SequenceInfoAfterData)->GetStringField(TEXT("variable_frame_stripping_settings")).StartsWith(VariableFrameStrippingSettingsPath));
		TArray<FString> TrackNamesAfter;
		TestTrue(TEXT("Animation assets track names after exist"), GetTrackNames(SequenceInfoAfterData, TrackNamesAfter));
		TestFalse(TEXT("Animation assets removed bone track absent after"), TrackNamesAfter.Contains(TrackToRemove));
		TestTrue(TEXT("Animation assets metadata class exists"), HasMetadataClass(SequenceInfoAfterData, AnimMetadataClassPath));
		TSharedPtr<FJsonObject> MetadataObject;
		TestTrue(TEXT("Animation assets metadata object exists"), FindMetadataByClass(SequenceInfoAfterData, AnimMetadataClassPath, MetadataObject));
		if (MetadataObject.IsValid())
		{
			const TSharedPtr<FJsonObject>* PropertiesObject = nullptr;
			TestTrue(TEXT("Animation assets metadata object has properties"), MetadataObject->TryGetObjectField(TEXT("properties"), PropertiesObject) && PropertiesObject && PropertiesObject->IsValid());
			if (PropertiesObject && PropertiesObject->IsValid())
			{
				TestEqual(TEXT("Animation assets metadata Tag property"), (*PropertiesObject)->GetStringField(TEXT("Tag")), FString(TEXT("Stage26Tag")));
				TestEqual(TEXT("Animation assets metadata Note property"), (*PropertiesObject)->GetStringField(TEXT("Note")), FString(TEXT("Stage26NoteUpdated")));
			}
		}
		TSharedPtr<FJsonObject> NotifyTrackObject;
		TestTrue(TEXT("Animation assets notify track exists"), FindNotifyTrack(SequenceInfoAfterData, NotifyTrackName, NotifyTrackObject));
		TSharedPtr<FJsonObject> NotifyObject;
		TestTrue(TEXT("Animation assets notify exists"), FindNotifyNamed(SequenceInfoAfterData, NotifyName, NotifyObject));
		if (NotifyObject.IsValid())
		{
			TestEqual(TEXT("Animation assets notify track name"), NotifyObject->GetStringField(TEXT("track_name")), NotifyTrackName);
			TestTrue(TEXT("Animation assets notify moved time updated"), FMath::IsNearlyEqual(static_cast<float>(NotifyObject->GetNumberField(TEXT("time_seconds"))), 0.24f, 0.001f));
		}
		TSharedPtr<FJsonObject> FloatCurveObject;
		TestTrue(TEXT("Animation assets float curve exists"), FindCurveNamed(SequenceInfoAfterData, FloatCurveName, FloatCurveObject));
		if (FloatCurveObject.IsValid())
		{
			TestEqual(TEXT("Animation assets float curve type"), FloatCurveObject->GetStringField(TEXT("curve_type")), FString(TEXT("float")));
			TestEqual(TEXT("Animation assets float curve key count"), static_cast<int32>(FloatCurveObject->GetIntegerField(TEXT("key_count"))), 2);
		}
		TestTrue(TEXT("Animation assets marker A exists"), HasMarkerNamed(SequenceInfoAfterData, MarkerA));
		TestTrue(TEXT("Animation assets marker B exists"), HasMarkerNamed(SequenceInfoAfterData, MarkerB));
	}

	const FString RemoveMarkersBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-sync-markers-002"),
		TEXT("anim_sequence_set_sync_markers"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"remove_marker_names\":[\"%s\"],\"remove_notify_track_names\":[\"%s\"],\"save_after_set\":false}"), *SequenceAssetPath, *MarkerA, *NotifyTrackName));
	FHttpSmokeResult RemoveMarkersResult;
	TestTrue(TEXT("Animation assets anim_sequence_set_sync_markers remove request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveMarkersBody, RemoveMarkersResult));
	TestEqual(TEXT("Animation assets anim_sequence_set_sync_markers remove status code"), RemoveMarkersResult.StatusCode, 200);

	const FString RemoveNotifyBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-notify-003"),
		TEXT("anim_sequence_set_notify"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"notify_index\":%d,\"remove\":true,\"save_after_set\":false}"), *SequenceAssetPath, NotifyIndex));
	FHttpSmokeResult RemoveNotifyResult;
	TestTrue(TEXT("Animation assets anim_sequence_set_notify remove request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveNotifyBody, RemoveNotifyResult));
	TestEqual(TEXT("Animation assets anim_sequence_set_notify remove status code"), RemoveNotifyResult.StatusCode, 200);

	const FString RemoveMetadataBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-metadata-002"),
		TEXT("anim_sequence_set_metadata"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"metadata_class_path\":\"%s\",\"remove\":true,\"save_after_set\":false}"), *SequenceAssetPath, *AnimMetadataClassPath));
	FHttpSmokeResult RemoveMetadataResult;
	TestTrue(TEXT("Animation assets anim_sequence_set_metadata remove request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveMetadataBody, RemoveMetadataResult));
	TestEqual(TEXT("Animation assets anim_sequence_set_metadata remove status code"), RemoveMetadataResult.StatusCode, 200);

	const FString RemoveNotifyTrackBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-notify-track-002"),
		TEXT("anim_sequence_set_notify_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"track_name\":\"%s\",\"remove\":true,\"save_after_set\":false}"), *SequenceAssetPath, *NotifyTrackName));
	FHttpSmokeResult RemoveNotifyTrackResult;
	TestTrue(TEXT("Animation assets anim_sequence_set_notify_track remove request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveNotifyTrackBody, RemoveNotifyTrackResult));
	TestEqual(TEXT("Animation assets anim_sequence_set_notify_track remove status code"), RemoveNotifyTrackResult.StatusCode, 200);

	const FString SkeletonBonesBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-skeleton-bones-001"),
		TEXT("skeleton_list_bones"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SkeletonAssetPath));
	FHttpSmokeResult SkeletonBonesResult;
	TestTrue(TEXT("Animation assets skeleton_list_bones request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SkeletonBonesBody, SkeletonBonesResult));
	TestEqual(TEXT("Animation assets skeleton_list_bones status code"), SkeletonBonesResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SkeletonBonesJson;
	TestTrue(TEXT("Animation assets skeleton_list_bones parses JSON"), ParseJson(SkeletonBonesResult.Body, SkeletonBonesJson));
	const TSharedPtr<FJsonObject>* SkeletonBonesData = nullptr;
	TestTrue(TEXT("Animation assets skeleton_list_bones contains data object"), SkeletonBonesJson.IsValid() && SkeletonBonesJson->TryGetObjectField(TEXT("data"), SkeletonBonesData) && SkeletonBonesData && SkeletonBonesData->IsValid());

	FString RootBoneName;
	FString SecondaryBoneName;
	if (SkeletonBonesData && SkeletonBonesData->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Bones = nullptr;
		TestTrue(TEXT("Animation assets skeleton_list_bones contains bones array"), (*SkeletonBonesData)->TryGetArrayField(TEXT("bones"), Bones) && Bones != nullptr);
		if (Bones && Bones->Num() > 0)
		{
			const TSharedPtr<FJsonObject>* RootBoneObject = nullptr;
			if ((*Bones)[0].IsValid() && (*Bones)[0]->TryGetObject(RootBoneObject) && RootBoneObject && RootBoneObject->IsValid())
			{
				RootBoneName = (*RootBoneObject)->GetStringField(TEXT("bone_name"));
			}
			if (Bones->Num() > 1)
			{
				const TSharedPtr<FJsonObject>* SecondaryBoneObject = nullptr;
				if ((*Bones)[1].IsValid() && (*Bones)[1]->TryGetObject(SecondaryBoneObject) && SecondaryBoneObject && SecondaryBoneObject->IsValid())
				{
					SecondaryBoneName = (*SecondaryBoneObject)->GetStringField(TEXT("bone_name"));
				}
			}
		}
	}
	TestTrue(TEXT("Animation assets skeleton root bone name is non-empty"), !RootBoneName.IsEmpty());
	TestTrue(TEXT("Animation assets skeleton secondary bone name is non-empty"), !SecondaryBoneName.IsEmpty());

	const FString SetSkeletonPreviewMeshBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-skeleton-preview-001"),
		TEXT("skeleton_set_preview_mesh"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"skeletal_mesh_path\":\"%s\",\"save_after_set\":false}"), *SkeletonAssetPath, *SourceSkeletalMeshPath));
	FHttpSmokeResult SetSkeletonPreviewMeshResult;
	TestTrue(TEXT("Animation assets skeleton_set_preview_mesh request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetSkeletonPreviewMeshBody, SetSkeletonPreviewMeshResult));
	TestEqual(TEXT("Animation assets skeleton_set_preview_mesh status code"), SetSkeletonPreviewMeshResult.StatusCode, 200);

	const FString SetCompatibleSkeletonsBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-compatible-skeletons-001"),
		TEXT("skeleton_set_compatible_skeletons"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"add_compatible_skeleton_paths\":[\"%s\"],\"use_retarget_modes_from_compatible_skeleton\":true,\"save_after_set\":false}"), *SkeletonAssetPath, *SourceSkeletonPath));
	FHttpSmokeResult SetCompatibleSkeletonsResult;
	TestTrue(TEXT("Animation assets skeleton_set_compatible_skeletons request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetCompatibleSkeletonsBody, SetCompatibleSkeletonsResult));
	TestEqual(TEXT("Animation assets skeleton_set_compatible_skeletons status code"), SetCompatibleSkeletonsResult.StatusCode, 200);

	const FString SetSkeletonSocketBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-socket-001"),
		TEXT("skeleton_set_socket"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"socket_name\":\"%s\",\"bone_name\":\"%s\",\"relative_location\":{\"x\":3.0,\"y\":4.0,\"z\":5.0},\"save_after_set\":false}"), *SkeletonAssetPath, *SocketName, *RootBoneName));
	FHttpSmokeResult SetSkeletonSocketResult;
	TestTrue(TEXT("Animation assets skeleton_set_socket request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetSkeletonSocketBody, SetSkeletonSocketResult));
	TestEqual(TEXT("Animation assets skeleton_set_socket status code"), SetSkeletonSocketResult.StatusCode, 200);

	const FString SetVirtualBoneBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-virtual-bone-001"),
		TEXT("skeleton_set_virtual_bone"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"source_bone_name\":\"%s\",\"target_bone_name\":\"%s\",\"save_after_set\":false}"), *SkeletonAssetPath, *RootBoneName, *SecondaryBoneName));
	FHttpSmokeResult SetVirtualBoneResult;
	TestTrue(TEXT("Animation assets skeleton_set_virtual_bone request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetVirtualBoneBody, SetVirtualBoneResult));
	TestEqual(TEXT("Animation assets skeleton_set_virtual_bone status code"), SetVirtualBoneResult.StatusCode, 200);
	TSharedPtr<FJsonObject> SetVirtualBoneJson;
	TestTrue(TEXT("Animation assets skeleton_set_virtual_bone parses JSON"), ParseJson(SetVirtualBoneResult.Body, SetVirtualBoneJson));
	const TSharedPtr<FJsonObject>* SetVirtualBoneData = nullptr;
	TestTrue(TEXT("Animation assets skeleton_set_virtual_bone contains data object"), SetVirtualBoneJson.IsValid() && SetVirtualBoneJson->TryGetObjectField(TEXT("data"), SetVirtualBoneData) && SetVirtualBoneData && SetVirtualBoneData->IsValid());
	if (SetVirtualBoneData && SetVirtualBoneData->IsValid())
	{
		VirtualBoneName = (*SetVirtualBoneData)->GetStringField(TEXT("virtual_bone_name"));
		TestTrue(TEXT("Animation assets virtual bone name non-empty"), !VirtualBoneName.IsEmpty());
	}

	const FString SkeletonInfoBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-skeleton-info-001"),
		TEXT("skeleton_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SkeletonAssetPath));
	FHttpSmokeResult SkeletonInfoResult;
	TestTrue(TEXT("Animation assets skeleton_get_info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SkeletonInfoBody, SkeletonInfoResult));
	TestEqual(TEXT("Animation assets skeleton_get_info status code"), SkeletonInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SkeletonInfoJson;
	TestTrue(TEXT("Animation assets skeleton_get_info parses JSON"), ParseJson(SkeletonInfoResult.Body, SkeletonInfoJson));
	const TSharedPtr<FJsonObject>* SkeletonInfoData = nullptr;
	TestTrue(TEXT("Animation assets skeleton_get_info contains data object"), SkeletonInfoJson.IsValid() && SkeletonInfoJson->TryGetObjectField(TEXT("data"), SkeletonInfoData) && SkeletonInfoData && SkeletonInfoData->IsValid());
	if (SkeletonInfoData && SkeletonInfoData->IsValid())
	{
		TSharedPtr<FJsonObject> SocketObject;
		TestTrue(TEXT("Animation assets skeleton socket exists"), FindSocketNamed(SkeletonInfoData, SocketName, SocketObject));
		if (SocketObject.IsValid())
		{
			TestEqual(TEXT("Animation assets skeleton socket bone name"), SocketObject->GetStringField(TEXT("bone_name")), RootBoneName);
		}
		TestTrue(TEXT("Animation assets skeleton virtual bone exists"), FindVirtualBoneNamed(SkeletonInfoData, VirtualBoneName));
		const TArray<TSharedPtr<FJsonValue>>* CompatibleSkeletons = nullptr;
		TestTrue(TEXT("Animation assets skeleton compatible skeleton array exists"), (*SkeletonInfoData)->TryGetArrayField(TEXT("compatible_skeletons"), CompatibleSkeletons) && CompatibleSkeletons != nullptr);
		bool bContainsSourceSkeleton = false;
		if (CompatibleSkeletons)
		{
			for (const TSharedPtr<FJsonValue>& CompatibleValue : *CompatibleSkeletons)
			{
				if (CompatibleValue.IsValid() && CompatibleValue->AsString().StartsWith(SourceSkeletonPath))
				{
					bContainsSourceSkeleton = true;
					break;
				}
			}
		}
		TestTrue(TEXT("Animation assets skeleton compatible skeletons contains source"), bContainsSourceSkeleton);
		TestTrue(TEXT("Animation assets skeleton use compatible retarget modes enabled"), (*SkeletonInfoData)->GetBoolField(TEXT("use_retarget_modes_from_compatible_skeleton")));
	}

	const FString SkeletonFolderPath = MakeSmokeAssetFolderPath(TEXT("Skeleton"), SkeletonAssetPath);
	IFileManager::Get().DeleteDirectory(*SkeletonFolderPath, false, true);
	const FString SkeletonFolderJsonPath = ToSmokeJsonPath(SkeletonFolderPath);

	const FString SkeletonExportFolderBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-skeleton-folder-export-001"),
		TEXT("skeleton_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SkeletonAssetPath));
	FHttpSmokeResult SkeletonExportFolderResult;
	TestTrue(TEXT("Animation assets skeleton_export_folder request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SkeletonExportFolderBody, SkeletonExportFolderResult));
	TestEqual(TEXT("Animation assets skeleton_export_folder status code"), SkeletonExportFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SkeletonExportFolderJson;
	TestTrue(TEXT("Animation assets skeleton_export_folder parses JSON"), ParseJson(SkeletonExportFolderResult.Body, SkeletonExportFolderJson));
	const TSharedPtr<FJsonObject>* SkeletonExportFolderData = nullptr;
	TestTrue(TEXT("Animation assets skeleton_export_folder contains data object"), SkeletonExportFolderJson.IsValid() && SkeletonExportFolderJson->TryGetObjectField(TEXT("data"), SkeletonExportFolderData) && SkeletonExportFolderData && SkeletonExportFolderData->IsValid());
	if (SkeletonExportFolderData && SkeletonExportFolderData->IsValid())
	{
		TestTrue(TEXT("Animation assets skeleton folder export marked complete"), (*SkeletonExportFolderData)->GetBoolField(TEXT("is_complete_target_schema")));
		TestTrue(TEXT("Animation assets skeleton folder export wrote files"), static_cast<int32>((*SkeletonExportFolderData)->GetIntegerField(TEXT("file_count"))) >= 10);
	}

	TSharedPtr<FJsonObject> SkeletonAssetFolderJson;
	TestTrue(TEXT("Animation assets skeleton folder asset.json exists"), LoadSmokeJsonFile(this, FPaths::Combine(SkeletonFolderPath, TEXT("asset.json")), SkeletonAssetFolderJson, TEXT("Skeleton folder asset.json")));
	TSharedPtr<FJsonObject> SkeletonCoverageJson;
	TestTrue(TEXT("Animation assets skeleton folder coverage exists"), LoadSmokeJsonFile(this, FPaths::Combine(SkeletonFolderPath, TEXT("validation"), TEXT("coverage_report.json")), SkeletonCoverageJson, TEXT("Skeleton folder coverage_report.json")));

	const FString SkeletonValidateFolderBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-skeleton-folder-validate-001"),
		TEXT("skeleton_validate_folder"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\"}"), *SkeletonFolderJsonPath));
	FHttpSmokeResult SkeletonValidateFolderResult;
	TestTrue(TEXT("Animation assets skeleton_validate_folder request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SkeletonValidateFolderBody, SkeletonValidateFolderResult));
	TestEqual(TEXT("Animation assets skeleton_validate_folder status code"), SkeletonValidateFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SkeletonValidateFolderJson;
	TestTrue(TEXT("Animation assets skeleton_validate_folder parses JSON"), ParseJson(SkeletonValidateFolderResult.Body, SkeletonValidateFolderJson));
	const TSharedPtr<FJsonObject>* SkeletonValidateFolderData = nullptr;
	TestTrue(TEXT("Animation assets skeleton_validate_folder contains data object"), SkeletonValidateFolderJson.IsValid() && SkeletonValidateFolderJson->TryGetObjectField(TEXT("data"), SkeletonValidateFolderData) && SkeletonValidateFolderData && SkeletonValidateFolderData->IsValid());
	if (SkeletonValidateFolderData && SkeletonValidateFolderData->IsValid())
	{
		TestTrue(TEXT("Animation assets skeleton_validate_folder is dry run"), (*SkeletonValidateFolderData)->GetBoolField(TEXT("dry_run")));
		TestEqual(TEXT("Animation assets skeleton_validate_folder has no JSON issues"), static_cast<int32>((*SkeletonValidateFolderData)->GetIntegerField(TEXT("json_issue_count"))), 0);
	}

	const FString SkeletonApplyFolderBody = MakeExecRequestBody(
		TEXT("smoke-anim-assets-skeleton-folder-apply-001"),
		TEXT("skeleton_apply_folder"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\",\"save_after_apply\":false}"), *SkeletonFolderJsonPath));
	FHttpSmokeResult SkeletonApplyFolderResult;
	TestTrue(TEXT("Animation assets skeleton_apply_folder request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SkeletonApplyFolderBody, SkeletonApplyFolderResult));
	TestEqual(TEXT("Animation assets skeleton_apply_folder status code"), SkeletonApplyFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SkeletonApplyFolderJson;
	TestTrue(TEXT("Animation assets skeleton_apply_folder parses JSON"), ParseJson(SkeletonApplyFolderResult.Body, SkeletonApplyFolderJson));
	const TSharedPtr<FJsonObject>* SkeletonApplyFolderData = nullptr;
	TestTrue(TEXT("Animation assets skeleton_apply_folder contains data object"), SkeletonApplyFolderJson.IsValid() && SkeletonApplyFolderJson->TryGetObjectField(TEXT("data"), SkeletonApplyFolderData) && SkeletonApplyFolderData && SkeletonApplyFolderData->IsValid());
	if (SkeletonApplyFolderData && SkeletonApplyFolderData->IsValid())
	{
		TestTrue(TEXT("Animation assets skeleton_apply_folder applied"), (*SkeletonApplyFolderData)->GetBoolField(TEXT("applied")));
		TestEqual(TEXT("Animation assets skeleton_apply_folder has no JSON issues"), static_cast<int32>((*SkeletonApplyFolderData)->GetIntegerField(TEXT("json_issue_count"))), 0);
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceIKAssetsSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.IKAssetsWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceIKAssetsSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString SourceSkeletonPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP_Skeleton");
	const FString SourceSkeletalMeshPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/TutorialTPP");
	const FString SourceAnimationAssetPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle");
	const FString TargetMeshAssetPath = MakeAutomationAssetPath(TEXT("TargetMesh"));
	const FString SourceIKRigAssetPath = MakeAutomationAssetPath(TEXT("IKRigSrc"));
	const FString TargetIKRigAssetPath = MakeAutomationAssetPath(TEXT("IKRigDst"));
	const FString AutoIKRigAssetPath = MakeAutomationAssetPath(TEXT("IKRigAuto"));
	const FString RetargeterAssetPath = MakeAutomationAssetPath(TEXT("RTG"));
	const FString RetargetOutputFolder = TEXT("/Game/__UeAgentInterfaceSmoke/RetargetStage20");
	const FString GoalName = TEXT("Stage20Goal");
	const FString ChainName = TEXT("Stage20Chain");
	const FString TargetPoseName = TEXT("Stage22Pose");
	const FString TargetPoseCopyName = TEXT("Stage22PoseCopy");
	const FString TargetPoseRenamed = TEXT("Stage22PoseRenamed");
	const FString BodyMoverSolverType = TEXT("/Script/IKRig.IKRigBodyMoverSolver");
	const FString FBIKSolverType = TEXT("/Script/IKRig.IKRigFullBodyIKSolver");

	const FString SkeletonBonesBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-bones-001"),
		TEXT("skeleton_list_bones"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SourceSkeletonPath));
	FHttpSmokeResult SkeletonBonesResult;
	TestTrue(TEXT("IK assets skeleton_list_bones request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SkeletonBonesBody, SkeletonBonesResult));
	TestEqual(TEXT("IK assets skeleton_list_bones status code"), SkeletonBonesResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SkeletonBonesJson;
	TestTrue(TEXT("IK assets skeleton_list_bones parses JSON"), ParseJson(SkeletonBonesResult.Body, SkeletonBonesJson));
	const TSharedPtr<FJsonObject>* SkeletonBonesData = nullptr;
	TestTrue(TEXT("IK assets skeleton_list_bones contains data object"), SkeletonBonesJson.IsValid() && SkeletonBonesJson->TryGetObjectField(TEXT("data"), SkeletonBonesData) && SkeletonBonesData && SkeletonBonesData->IsValid());

	FString RootBoneName;
	FString SecondaryBoneName;
	if (SkeletonBonesData && SkeletonBonesData->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Bones = nullptr;
		TestTrue(TEXT("IK assets skeleton_list_bones contains bones array"), (*SkeletonBonesData)->TryGetArrayField(TEXT("bones"), Bones) && Bones != nullptr);
		if (Bones && Bones->Num() > 0)
		{
			const TSharedPtr<FJsonObject>* BoneObject = nullptr;
			if ((*Bones)[0].IsValid() && (*Bones)[0]->TryGetObject(BoneObject) && BoneObject && BoneObject->IsValid())
			{
				RootBoneName = (*BoneObject)->GetStringField(TEXT("bone_name"));
			}
		}
		if (Bones && Bones->Num() > 1)
		{
			const TSharedPtr<FJsonObject>* BoneObject = nullptr;
			if ((*Bones)[1].IsValid() && (*Bones)[1]->TryGetObject(BoneObject) && BoneObject && BoneObject->IsValid())
			{
				SecondaryBoneName = (*BoneObject)->GetStringField(TEXT("bone_name"));
			}
		}
	}
	TestTrue(TEXT("IK assets root bone name is non-empty"), !RootBoneName.IsEmpty());
	TestTrue(TEXT("IK assets secondary bone name is non-empty"), !SecondaryBoneName.IsEmpty());

	const FString DuplicateTargetMeshBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-dup-mesh-001"),
		TEXT("asset_duplicate"),
		FString::Printf(TEXT("{\"source_asset_path\":\"%s\",\"destination_asset_path\":\"%s\",\"save_after_duplicate\":false}"), *SourceSkeletalMeshPath, *TargetMeshAssetPath));
	FHttpSmokeResult DuplicateTargetMeshResult;
	TestTrue(TEXT("IK assets duplicate target mesh request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DuplicateTargetMeshBody, DuplicateTargetMeshResult));
	TestEqual(TEXT("IK assets duplicate target mesh status code"), DuplicateTargetMeshResult.StatusCode, 200);

	const FString SkeletalMeshInfoBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-skelmesh-info-001"),
		TEXT("skeletal_mesh_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *TargetMeshAssetPath));
	FHttpSmokeResult SkeletalMeshInfoResult;
	TestTrue(TEXT("IK assets skeletal_mesh_get_info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SkeletalMeshInfoBody, SkeletalMeshInfoResult));
	TestEqual(TEXT("IK assets skeletal_mesh_get_info status code"), SkeletalMeshInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SkeletalMeshInfoJson;
	TestTrue(TEXT("IK assets skeletal_mesh_get_info parses JSON"), ParseJson(SkeletalMeshInfoResult.Body, SkeletalMeshInfoJson));
	const TSharedPtr<FJsonObject>* SkeletalMeshInfoData = nullptr;
	TestTrue(TEXT("IK assets skeletal_mesh_get_info contains data object"), SkeletalMeshInfoJson.IsValid() && SkeletalMeshInfoJson->TryGetObjectField(TEXT("data"), SkeletalMeshInfoData) && SkeletalMeshInfoData && SkeletalMeshInfoData->IsValid());
	if (SkeletalMeshInfoData && SkeletalMeshInfoData->IsValid())
	{
		TestTrue(TEXT("IK assets skeletal mesh has materials"), static_cast<int32>((*SkeletalMeshInfoData)->GetIntegerField(TEXT("material_slot_count"))) >= 1);
		TestTrue(TEXT("IK assets skeletal mesh has lods"), static_cast<int32>((*SkeletalMeshInfoData)->GetIntegerField(TEXT("lod_count"))) >= 1);
	}

	const FString SkeletalMeshFolderPath = MakeSmokeAssetFolderPath(TEXT("SkeletalMesh"), TargetMeshAssetPath);
	IFileManager::Get().DeleteDirectory(*SkeletalMeshFolderPath, false, true);
	const FString SkeletalMeshFolderJsonPath = ToSmokeJsonPath(SkeletalMeshFolderPath);

	const FString SkeletalMeshExportFolderBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-skelmesh-folder-export-001"),
		TEXT("skeletal_mesh_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *TargetMeshAssetPath));
	FHttpSmokeResult SkeletalMeshExportFolderResult;
	TestTrue(TEXT("IK assets skeletal_mesh_export_folder request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SkeletalMeshExportFolderBody, SkeletalMeshExportFolderResult));
	TestEqual(TEXT("IK assets skeletal_mesh_export_folder status code"), SkeletalMeshExportFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SkeletalMeshExportFolderJson;
	TestTrue(TEXT("IK assets skeletal_mesh_export_folder parses JSON"), ParseJson(SkeletalMeshExportFolderResult.Body, SkeletalMeshExportFolderJson));
	const TSharedPtr<FJsonObject>* SkeletalMeshExportFolderData = nullptr;
	TestTrue(TEXT("IK assets skeletal_mesh_export_folder contains data object"), SkeletalMeshExportFolderJson.IsValid() && SkeletalMeshExportFolderJson->TryGetObjectField(TEXT("data"), SkeletalMeshExportFolderData) && SkeletalMeshExportFolderData && SkeletalMeshExportFolderData->IsValid());
	if (SkeletalMeshExportFolderData && SkeletalMeshExportFolderData->IsValid())
	{
		TestTrue(TEXT("IK assets skeletal mesh folder export marked complete"), (*SkeletalMeshExportFolderData)->GetBoolField(TEXT("is_complete_target_schema")));
		TestTrue(TEXT("IK assets skeletal mesh folder export wrote files"), static_cast<int32>((*SkeletalMeshExportFolderData)->GetIntegerField(TEXT("file_count"))) >= 10);
	}

	TSharedPtr<FJsonObject> SkeletalMeshAssetFolderJson;
	TestTrue(TEXT("IK assets skeletal mesh folder asset.json exists"), LoadSmokeJsonFile(this, FPaths::Combine(SkeletalMeshFolderPath, TEXT("asset.json")), SkeletalMeshAssetFolderJson, TEXT("SkeletalMesh folder asset.json")));
	TSharedPtr<FJsonObject> SkeletalMeshCoverageJson;
	TestTrue(TEXT("IK assets skeletal mesh folder coverage exists"), LoadSmokeJsonFile(this, FPaths::Combine(SkeletalMeshFolderPath, TEXT("validation"), TEXT("coverage_report.json")), SkeletalMeshCoverageJson, TEXT("SkeletalMesh folder coverage_report.json")));

	const FString SkeletalMeshValidateFolderBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-skelmesh-folder-validate-001"),
		TEXT("skeletal_mesh_validate_folder"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\"}"), *SkeletalMeshFolderJsonPath));
	FHttpSmokeResult SkeletalMeshValidateFolderResult;
	TestTrue(TEXT("IK assets skeletal_mesh_validate_folder request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SkeletalMeshValidateFolderBody, SkeletalMeshValidateFolderResult));
	TestEqual(TEXT("IK assets skeletal_mesh_validate_folder status code"), SkeletalMeshValidateFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SkeletalMeshValidateFolderJson;
	TestTrue(TEXT("IK assets skeletal_mesh_validate_folder parses JSON"), ParseJson(SkeletalMeshValidateFolderResult.Body, SkeletalMeshValidateFolderJson));
	const TSharedPtr<FJsonObject>* SkeletalMeshValidateFolderData = nullptr;
	TestTrue(TEXT("IK assets skeletal_mesh_validate_folder contains data object"), SkeletalMeshValidateFolderJson.IsValid() && SkeletalMeshValidateFolderJson->TryGetObjectField(TEXT("data"), SkeletalMeshValidateFolderData) && SkeletalMeshValidateFolderData && SkeletalMeshValidateFolderData->IsValid());
	if (SkeletalMeshValidateFolderData && SkeletalMeshValidateFolderData->IsValid())
	{
		TestTrue(TEXT("IK assets skeletal_mesh_validate_folder is dry run"), (*SkeletalMeshValidateFolderData)->GetBoolField(TEXT("dry_run")));
		TestEqual(TEXT("IK assets skeletal_mesh_validate_folder has no JSON issues"), static_cast<int32>((*SkeletalMeshValidateFolderData)->GetIntegerField(TEXT("json_issue_count"))), 0);
	}

	const FString SkeletalMeshApplyFolderBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-skelmesh-folder-apply-001"),
		TEXT("skeletal_mesh_apply_folder"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\",\"save_after_apply\":false}"), *SkeletalMeshFolderJsonPath));
	FHttpSmokeResult SkeletalMeshApplyFolderResult;
	TestTrue(TEXT("IK assets skeletal_mesh_apply_folder request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SkeletalMeshApplyFolderBody, SkeletalMeshApplyFolderResult));
	TestEqual(TEXT("IK assets skeletal_mesh_apply_folder status code"), SkeletalMeshApplyFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SkeletalMeshApplyFolderJson;
	TestTrue(TEXT("IK assets skeletal_mesh_apply_folder parses JSON"), ParseJson(SkeletalMeshApplyFolderResult.Body, SkeletalMeshApplyFolderJson));
	const TSharedPtr<FJsonObject>* SkeletalMeshApplyFolderData = nullptr;
	TestTrue(TEXT("IK assets skeletal_mesh_apply_folder contains data object"), SkeletalMeshApplyFolderJson.IsValid() && SkeletalMeshApplyFolderJson->TryGetObjectField(TEXT("data"), SkeletalMeshApplyFolderData) && SkeletalMeshApplyFolderData && SkeletalMeshApplyFolderData->IsValid());
	if (SkeletalMeshApplyFolderData && SkeletalMeshApplyFolderData->IsValid())
	{
		TestTrue(TEXT("IK assets skeletal_mesh_apply_folder applied"), (*SkeletalMeshApplyFolderData)->GetBoolField(TEXT("applied")));
		TestEqual(TEXT("IK assets skeletal_mesh_apply_folder has no JSON issues"), static_cast<int32>((*SkeletalMeshApplyFolderData)->GetIntegerField(TEXT("json_issue_count"))), 0);
	}

	const FString CreateAutoIKRigBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-create-auto-rig-001"),
		TEXT("ik_rig_create"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"preview_skeletal_mesh\":\"%s\",\"save_after_create\":false}"), *AutoIKRigAssetPath, *SourceSkeletalMeshPath));
	FHttpSmokeResult CreateAutoIKRigResult;
	TestTrue(TEXT("IK assets create auto IK rig request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateAutoIKRigBody, CreateAutoIKRigResult));
	TestEqual(TEXT("IK assets create auto IK rig status code"), CreateAutoIKRigResult.StatusCode, 200);

	const FString ApplyAutoDefinitionBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-auto-def-001"),
		TEXT("ik_rig_apply_auto_retarget_definition"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"save_after_set\":false}"), *AutoIKRigAssetPath));
	FHttpSmokeResult ApplyAutoDefinitionResult;
	TestTrue(TEXT("IK assets apply auto retarget definition request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ApplyAutoDefinitionBody, ApplyAutoDefinitionResult));
	TestEqual(TEXT("IK assets apply auto retarget definition status code"), ApplyAutoDefinitionResult.StatusCode, 200);

	const FString CreateSourceIKRigBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-create-src-rig-001"),
		TEXT("ik_rig_create"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"preview_skeletal_mesh\":\"%s\",\"save_after_create\":false}"), *SourceIKRigAssetPath, *SourceSkeletalMeshPath));
	FHttpSmokeResult CreateSourceIKRigResult;
	TestTrue(TEXT("IK assets create source IK rig request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateSourceIKRigBody, CreateSourceIKRigResult));
	TestEqual(TEXT("IK assets create source IK rig status code"), CreateSourceIKRigResult.StatusCode, 200);

	const FString CreateTargetIKRigBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-create-dst-rig-001"),
		TEXT("ik_rig_create"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"preview_skeletal_mesh\":\"%s\",\"save_after_create\":false}"), *TargetIKRigAssetPath, *TargetMeshAssetPath));
	FHttpSmokeResult CreateTargetIKRigResult;
	TestTrue(TEXT("IK assets create target IK rig request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateTargetIKRigBody, CreateTargetIKRigResult));
	TestEqual(TEXT("IK assets create target IK rig status code"), CreateTargetIKRigResult.StatusCode, 200);

	const FString SetSourceRootBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-src-root-001"),
		TEXT("ik_rig_set_retarget_root"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"root_bone_name\":\"%s\",\"save_after_set\":false}"), *SourceIKRigAssetPath, *RootBoneName));
	FHttpSmokeResult SetSourceRootResult;
	TestTrue(TEXT("IK assets set source retarget root request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetSourceRootBody, SetSourceRootResult));
	TestEqual(TEXT("IK assets set source retarget root status code"), SetSourceRootResult.StatusCode, 200);

	const FString SetTargetRootBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-dst-root-001"),
		TEXT("ik_rig_set_retarget_root"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"root_bone_name\":\"%s\",\"save_after_set\":false}"), *TargetIKRigAssetPath, *RootBoneName));
	FHttpSmokeResult SetTargetRootResult;
	TestTrue(TEXT("IK assets set target retarget root request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetTargetRootBody, SetTargetRootResult));
	TestEqual(TEXT("IK assets set target retarget root status code"), SetTargetRootResult.StatusCode, 200);

	const FString SetGoalBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-goal-001"),
		TEXT("ik_rig_set_goal"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"goal_name\":\"%s\",\"bone_name\":\"%s\",\"position_alpha\":0.25,\"rotation_alpha\":0.75,\"position\":{\"x\":11.0,\"y\":22.0,\"z\":33.0},\"rotation\":{\"pitch\":10.0,\"yaw\":20.0,\"roll\":30.0},\"save_after_set\":false}"), *SourceIKRigAssetPath, *GoalName, *SecondaryBoneName));
	FHttpSmokeResult SetGoalResult;
	TestTrue(TEXT("IK assets set goal request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetGoalBody, SetGoalResult));
	TestEqual(TEXT("IK assets set goal status code"), SetGoalResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SetGoalJson;
	TestTrue(TEXT("IK assets set goal parses JSON"), ParseJson(SetGoalResult.Body, SetGoalJson));
	const TSharedPtr<FJsonObject>* SetGoalData = nullptr;
	TestTrue(TEXT("IK assets set goal contains data object"), SetGoalJson.IsValid() && SetGoalJson->TryGetObjectField(TEXT("data"), SetGoalData) && SetGoalData && SetGoalData->IsValid());
	if (SetGoalData && SetGoalData->IsValid())
	{
		TestEqual(TEXT("IK assets set goal response bone name updated"), (*SetGoalData)->GetStringField(TEXT("bone_name")), SecondaryBoneName);
		TestTrue(TEXT("IK assets set goal response position alpha updated"), FMath::IsNearlyEqual(static_cast<float>((*SetGoalData)->GetNumberField(TEXT("position_alpha"))), 0.25f));
		TestTrue(TEXT("IK assets set goal response rotation alpha updated"), FMath::IsNearlyEqual(static_cast<float>((*SetGoalData)->GetNumberField(TEXT("rotation_alpha"))), 0.75f));
		const TSharedPtr<FJsonObject>* GoalLocation = nullptr;
		TestTrue(TEXT("IK assets set goal response current location exists"), (*SetGoalData)->TryGetObjectField(TEXT("current_location"), GoalLocation) && GoalLocation && GoalLocation->IsValid());
		if (GoalLocation && GoalLocation->IsValid())
		{
			TestTrue(TEXT("IK assets set goal response current location x updated"), FMath::IsNearlyEqual(static_cast<float>((*GoalLocation)->GetNumberField(TEXT("x"))), 11.0f));
			TestTrue(TEXT("IK assets set goal response current location z updated"), FMath::IsNearlyEqual(static_cast<float>((*GoalLocation)->GetNumberField(TEXT("z"))), 33.0f));
		}
		const TSharedPtr<FJsonObject>* GoalRotation = nullptr;
		TestTrue(TEXT("IK assets set goal response current rotation exists"), (*SetGoalData)->TryGetObjectField(TEXT("current_rotation"), GoalRotation) && GoalRotation && GoalRotation->IsValid());
		if (GoalRotation && GoalRotation->IsValid())
		{
			TestTrue(TEXT("IK assets set goal response current rotation yaw updated"), FMath::IsNearlyEqual(static_cast<float>((*GoalRotation)->GetNumberField(TEXT("yaw"))), 20.0f));
		}
	}

	const FString SetSourceChainBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-src-chain-001"),
		TEXT("ik_rig_set_retarget_chain"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"chain_name\":\"%s\",\"start_bone_name\":\"%s\",\"end_bone_name\":\"%s\",\"goal_name\":\"%s\",\"save_after_set\":false}"), *SourceIKRigAssetPath, *ChainName, *RootBoneName, *SecondaryBoneName, *GoalName));
	FHttpSmokeResult SetSourceChainResult;
	TestTrue(TEXT("IK assets set source chain request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetSourceChainBody, SetSourceChainResult));
	TestEqual(TEXT("IK assets set source chain status code"), SetSourceChainResult.StatusCode, 200);

	const FString AddSourceSolverBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-src-solver-001"),
		TEXT("ik_rig_set_solver"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"solver_type\":\"%s\",\"start_bone_name\":\"%s\",\"enabled\":true,\"connect_goal_names\":[\"%s\"],\"settings\":{\"position_alpha\":0.5,\"position_positive_x\":0.6,\"position_negative_x\":0.7,\"rotation_alpha\":0.8,\"rotate_z_alpha\":0.9},\"goal_settings\":[{\"goal_name\":\"%s\",\"influence_multiplier\":2.25}],\"save_after_set\":false}"), *SourceIKRigAssetPath, *BodyMoverSolverType, *RootBoneName, *GoalName, *GoalName));
	FHttpSmokeResult AddSourceSolverResult;
	TestTrue(TEXT("IK assets add source solver request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddSourceSolverBody, AddSourceSolverResult));
	TestEqual(TEXT("IK assets add source solver status code"), AddSourceSolverResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddSourceSolverJson;
	TestTrue(TEXT("IK assets add source solver parses JSON"), ParseJson(AddSourceSolverResult.Body, AddSourceSolverJson));
	int32 SourceSolverIndex = INDEX_NONE;
	const TSharedPtr<FJsonObject>* AddSourceSolverData = nullptr;
	TestTrue(TEXT("IK assets add source solver contains data object"), AddSourceSolverJson.IsValid() && AddSourceSolverJson->TryGetObjectField(TEXT("data"), AddSourceSolverData) && AddSourceSolverData && AddSourceSolverData->IsValid());
	if (AddSourceSolverData && AddSourceSolverData->IsValid())
	{
		SourceSolverIndex = static_cast<int32>((*AddSourceSolverData)->GetIntegerField(TEXT("solver_index")));
		TestTrue(TEXT("IK assets source solver struct contains body mover"), (*AddSourceSolverData)->GetStringField(TEXT("solver_struct")).Contains(TEXT("IKRigBodyMoverSolver")));
		TestEqual(TEXT("IK assets source solver kind is body mover"), (*AddSourceSolverData)->GetStringField(TEXT("solver_kind")), FString(TEXT("body_mover")));
		const TSharedPtr<FJsonObject>* SolverSettings = nullptr;
		TestTrue(TEXT("IK assets source solver settings object exists"), (*AddSourceSolverData)->TryGetObjectField(TEXT("settings"), SolverSettings) && SolverSettings && SolverSettings->IsValid());
		if (SolverSettings && SolverSettings->IsValid())
		{
			TestTrue(TEXT("IK assets source solver position alpha updated"), FMath::IsNearlyEqual(static_cast<float>((*SolverSettings)->GetNumberField(TEXT("position_alpha"))), 0.5f));
			TestTrue(TEXT("IK assets source solver rotate z alpha updated"), FMath::IsNearlyEqual(static_cast<float>((*SolverSettings)->GetNumberField(TEXT("rotate_z_alpha"))), 0.9f));
		}
		const TArray<TSharedPtr<FJsonValue>>* GoalSettings = nullptr;
		TestTrue(TEXT("IK assets source solver goal settings array exists"), (*AddSourceSolverData)->TryGetArrayField(TEXT("goal_settings"), GoalSettings) && GoalSettings != nullptr);
		if (GoalSettings && GoalSettings->Num() > 0)
		{
			const TSharedPtr<FJsonObject>* GoalSettingsObject = nullptr;
			TestTrue(TEXT("IK assets source solver first goal settings parses"), (*GoalSettings)[0].IsValid() && (*GoalSettings)[0]->TryGetObject(GoalSettingsObject) && GoalSettingsObject && GoalSettingsObject->IsValid());
			if (GoalSettingsObject && GoalSettingsObject->IsValid())
			{
				TestEqual(TEXT("IK assets source solver goal name updated"), (*GoalSettingsObject)->GetStringField(TEXT("goal_name")), GoalName);
				TestTrue(TEXT("IK assets source solver goal influence updated"), FMath::IsNearlyEqual(static_cast<float>((*GoalSettingsObject)->GetNumberField(TEXT("influence_multiplier"))), 2.25f));
			}
		}
	}

	const FString DisableSourceSolverBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-src-solver-002"),
		TEXT("ik_rig_set_solver"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"solver_index\":%d,\"enabled\":false,\"save_after_set\":false}"), *SourceIKRigAssetPath, SourceSolverIndex));
	FHttpSmokeResult DisableSourceSolverResult;
	TestTrue(TEXT("IK assets disable source solver request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DisableSourceSolverBody, DisableSourceSolverResult));
	TestEqual(TEXT("IK assets disable source solver status code"), DisableSourceSolverResult.StatusCode, 200);

	const FString AddFBIKSolverBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-src-solver-003"),
		TEXT("ik_rig_set_solver"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"solver_type\":\"%s\",\"start_bone_name\":\"%s\",\"enabled\":true,\"connect_goal_names\":[\"%s\"],\"settings\":{\"iterations\":16,\"sub_iterations\":4,\"mass_multiplier\":1.5,\"allow_stretch\":true,\"root_behavior\":\"free\",\"global_pull_chain_alpha\":0.85,\"max_angle\":25.0,\"over_relaxation\":1.1},\"goal_settings\":[{\"goal_name\":\"%s\",\"chain_depth\":2,\"strength_alpha\":0.6,\"pull_chain_alpha\":0.7,\"pin_rotation\":0.8}],\"bone_settings\":[{\"bone_name\":\"%s\",\"rotation_stiffness\":0.3,\"position_stiffness\":0.4,\"use_preferred_angles\":true,\"preferred_angles\":{\"x\":5.0,\"y\":10.0,\"z\":15.0}}],\"save_after_set\":false}"), *SourceIKRigAssetPath, *FBIKSolverType, *RootBoneName, *GoalName, *GoalName, *RootBoneName));
	FHttpSmokeResult AddFBIKSolverResult;
	TestTrue(TEXT("IK assets add FBIK solver request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddFBIKSolverBody, AddFBIKSolverResult));
	TestEqual(TEXT("IK assets add FBIK solver status code"), AddFBIKSolverResult.StatusCode, 200);

	const FString SetTargetChainBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-dst-chain-001"),
		TEXT("ik_rig_set_retarget_chain"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"chain_name\":\"%s\",\"start_bone_name\":\"%s\",\"end_bone_name\":\"%s\",\"save_after_set\":false}"), *TargetIKRigAssetPath, *ChainName, *RootBoneName, *SecondaryBoneName));
	FHttpSmokeResult SetTargetChainResult;
	TestTrue(TEXT("IK assets set target chain request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetTargetChainBody, SetTargetChainResult));
	TestEqual(TEXT("IK assets set target chain status code"), SetTargetChainResult.StatusCode, 200);

	const FString SourceIKRigInfoBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-src-info-001"),
		TEXT("ik_rig_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SourceIKRigAssetPath));
	FHttpSmokeResult SourceIKRigInfoResult;
	TestTrue(TEXT("IK assets source ik rig get_info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SourceIKRigInfoBody, SourceIKRigInfoResult));
	TestEqual(TEXT("IK assets source ik rig get_info status code"), SourceIKRigInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SourceIKRigInfoJson;
	TestTrue(TEXT("IK assets source ik rig get_info parses JSON"), ParseJson(SourceIKRigInfoResult.Body, SourceIKRigInfoJson));
	const TSharedPtr<FJsonObject>* SourceIKRigInfoData = nullptr;
	TestTrue(TEXT("IK assets source ik rig get_info contains data object"), SourceIKRigInfoJson.IsValid() && SourceIKRigInfoJson->TryGetObjectField(TEXT("data"), SourceIKRigInfoData) && SourceIKRigInfoData && SourceIKRigInfoData->IsValid());
	if (SourceIKRigInfoData && SourceIKRigInfoData->IsValid())
	{
		TestEqual(TEXT("IK assets source ik rig solver count >= 1"), static_cast<int32>((*SourceIKRigInfoData)->GetIntegerField(TEXT("solver_count"))) >= 1, true);
		const TArray<TSharedPtr<FJsonValue>>* Goals = nullptr;
		TestTrue(TEXT("IK assets source ik rig contains goals array"), (*SourceIKRigInfoData)->TryGetArrayField(TEXT("goals"), Goals) && Goals != nullptr);
		if (Goals && Goals->Num() > 0)
		{
			const TSharedPtr<FJsonObject>* GoalObject = nullptr;
			if ((*Goals)[0].IsValid() && (*Goals)[0]->TryGetObject(GoalObject) && GoalObject && GoalObject->IsValid())
			{
				TestEqual(TEXT("IK assets source goal name in info"), (*GoalObject)->GetStringField(TEXT("goal_name")), GoalName);
				TestTrue(TEXT("IK assets source goal position alpha updated"), FMath::IsNearlyEqual(static_cast<float>((*GoalObject)->GetNumberField(TEXT("position_alpha"))), 0.25f));
				TestTrue(TEXT("IK assets source goal rotation alpha updated"), FMath::IsNearlyEqual(static_cast<float>((*GoalObject)->GetNumberField(TEXT("rotation_alpha"))), 0.75f));
				const TSharedPtr<FJsonObject>* GoalLocation = nullptr;
				TestTrue(TEXT("IK assets source goal current location exists"), (*GoalObject)->TryGetObjectField(TEXT("current_location"), GoalLocation) && GoalLocation && GoalLocation->IsValid());
				const TSharedPtr<FJsonObject>* GoalRotation = nullptr;
				TestTrue(TEXT("IK assets source goal current rotation exists"), (*GoalObject)->TryGetObjectField(TEXT("current_rotation"), GoalRotation) && GoalRotation && GoalRotation->IsValid());
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* Solvers = nullptr;
		TestTrue(TEXT("IK assets source ik rig contains solvers array"), (*SourceIKRigInfoData)->TryGetArrayField(TEXT("solvers"), Solvers) && Solvers != nullptr);
		if (Solvers && Solvers->Num() > 0)
		{
			bool bFoundBodyMover = false;
			bool bFoundFBIK = false;
			for (const TSharedPtr<FJsonValue>& SolverValue : *Solvers)
			{
				const TSharedPtr<FJsonObject>* SolverObject = nullptr;
				if (!SolverValue.IsValid() || !SolverValue->TryGetObject(SolverObject) || !SolverObject || !SolverObject->IsValid())
				{
					continue;
				}

				const FString SolverStructPath = (*SolverObject)->GetStringField(TEXT("solver_struct"));
				if (SolverStructPath.Contains(TEXT("IKRigBodyMoverSolver")))
				{
					bFoundBodyMover = true;
					TestEqual(TEXT("IK assets source solver disabled in info"), (*SolverObject)->GetBoolField(TEXT("enabled")), false);
					const TSharedPtr<FJsonObject>* SolverSettings = nullptr;
					TestTrue(TEXT("IK assets source solver settings in info exist"), (*SolverObject)->TryGetObjectField(TEXT("settings"), SolverSettings) && SolverSettings && SolverSettings->IsValid());
					if (SolverSettings && SolverSettings->IsValid())
					{
						TestTrue(TEXT("IK assets source solver position alpha in info updated"), FMath::IsNearlyEqual(static_cast<float>((*SolverSettings)->GetNumberField(TEXT("position_alpha"))), 0.5f));
					}
					const TArray<TSharedPtr<FJsonValue>>* GoalSettings = nullptr;
					TestTrue(TEXT("IK assets source solver goal settings in info exist"), (*SolverObject)->TryGetArrayField(TEXT("goal_settings"), GoalSettings) && GoalSettings != nullptr);
					if (GoalSettings && GoalSettings->Num() > 0)
					{
						const TSharedPtr<FJsonObject>* GoalSettingsObject = nullptr;
						TestTrue(TEXT("IK assets source solver goal settings info parses"), (*GoalSettings)[0].IsValid() && (*GoalSettings)[0]->TryGetObject(GoalSettingsObject) && GoalSettingsObject && GoalSettingsObject->IsValid());
						if (GoalSettingsObject && GoalSettingsObject->IsValid())
						{
							TestTrue(TEXT("IK assets source solver goal influence in info updated"), FMath::IsNearlyEqual(static_cast<float>((*GoalSettingsObject)->GetNumberField(TEXT("influence_multiplier"))), 2.25f));
						}
					}
				}
				else if (SolverStructPath.Contains(TEXT("IKRigFullBodyIKSolver")))
				{
					bFoundFBIK = true;
					TestEqual(TEXT("IK assets source FBIK solver kind in info"), (*SolverObject)->GetStringField(TEXT("solver_kind")), FString(TEXT("full_body_ik")));
					const TSharedPtr<FJsonObject>* SolverSettings = nullptr;
					TestTrue(TEXT("IK assets source FBIK settings in info exist"), (*SolverObject)->TryGetObjectField(TEXT("settings"), SolverSettings) && SolverSettings && SolverSettings->IsValid());
					if (SolverSettings && SolverSettings->IsValid())
					{
						TestEqual(TEXT("IK assets source FBIK root behavior in info"), (*SolverSettings)->GetStringField(TEXT("root_behavior")), FString(TEXT("free")));
						TestTrue(TEXT("IK assets source FBIK iterations in info updated"), static_cast<int32>((*SolverSettings)->GetIntegerField(TEXT("iterations"))) == 16);
						TestTrue(TEXT("IK assets source FBIK allow stretch in info updated"), (*SolverSettings)->GetBoolField(TEXT("allow_stretch")));
					}
					const TArray<TSharedPtr<FJsonValue>>* GoalSettings = nullptr;
					TestTrue(TEXT("IK assets source FBIK goal settings in info exist"), (*SolverObject)->TryGetArrayField(TEXT("goal_settings"), GoalSettings) && GoalSettings != nullptr);
					if (GoalSettings && GoalSettings->Num() > 0)
					{
						const TSharedPtr<FJsonObject>* GoalSettingsObject = nullptr;
						TestTrue(TEXT("IK assets source FBIK goal settings info parses"), (*GoalSettings)[0].IsValid() && (*GoalSettings)[0]->TryGetObject(GoalSettingsObject) && GoalSettingsObject && GoalSettingsObject->IsValid());
						if (GoalSettingsObject && GoalSettingsObject->IsValid())
						{
							TestTrue(TEXT("IK assets source FBIK chain depth in info updated"), static_cast<int32>((*GoalSettingsObject)->GetIntegerField(TEXT("chain_depth"))) == 2);
							TestTrue(TEXT("IK assets source FBIK strength alpha in info updated"), FMath::IsNearlyEqual(static_cast<float>((*GoalSettingsObject)->GetNumberField(TEXT("strength_alpha"))), 0.6f));
						}
					}

					const TArray<TSharedPtr<FJsonValue>>* BoneSettings = nullptr;
					TestTrue(TEXT("IK assets source FBIK bone settings in info exist"), (*SolverObject)->TryGetArrayField(TEXT("bone_settings"), BoneSettings) && BoneSettings != nullptr);
					if (BoneSettings && BoneSettings->Num() > 0)
					{
						const TSharedPtr<FJsonObject>* BoneSettingsObject = nullptr;
						TestTrue(TEXT("IK assets source FBIK bone settings info parses"), (*BoneSettings)[0].IsValid() && (*BoneSettings)[0]->TryGetObject(BoneSettingsObject) && BoneSettingsObject && BoneSettingsObject->IsValid());
						if (BoneSettingsObject && BoneSettingsObject->IsValid())
						{
							TestEqual(TEXT("IK assets source FBIK bone settings root bone"), (*BoneSettingsObject)->GetStringField(TEXT("bone_name")), RootBoneName);
							TestTrue(TEXT("IK assets source FBIK bone rotation stiffness in info updated"), FMath::IsNearlyEqual(static_cast<float>((*BoneSettingsObject)->GetNumberField(TEXT("rotation_stiffness"))), 0.3f));
						}
					}
				}
			}
			TestTrue(TEXT("IK assets source body mover solver found in info"), bFoundBodyMover);
			TestTrue(TEXT("IK assets source FBIK solver found in info"), bFoundFBIK);
		}
	}

	const FString IKRigFolderPath = MakeSmokeAssetFolderPath(TEXT("IKRig"), SourceIKRigAssetPath);
	IFileManager::Get().DeleteDirectory(*IKRigFolderPath, false, true);
	const FString IKRigFolderJsonPath = ToSmokeJsonPath(IKRigFolderPath);

	const FString IKRigExportFolderBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rig-folder-export-001"),
		TEXT("ik_rig_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SourceIKRigAssetPath));
	FHttpSmokeResult IKRigExportFolderResult;
	TestTrue(TEXT("IK assets ik_rig_export_folder request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, IKRigExportFolderBody, IKRigExportFolderResult));
	TestEqual(TEXT("IK assets ik_rig_export_folder status code"), IKRigExportFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> IKRigExportFolderJson;
	TestTrue(TEXT("IK assets ik_rig_export_folder parses JSON"), ParseJson(IKRigExportFolderResult.Body, IKRigExportFolderJson));
	const TSharedPtr<FJsonObject>* IKRigExportFolderData = nullptr;
	TestTrue(TEXT("IK assets ik_rig_export_folder contains data object"), IKRigExportFolderJson.IsValid() && IKRigExportFolderJson->TryGetObjectField(TEXT("data"), IKRigExportFolderData) && IKRigExportFolderData && IKRigExportFolderData->IsValid());
	if (IKRigExportFolderData && IKRigExportFolderData->IsValid())
	{
		TestTrue(TEXT("IK assets ik rig folder export marked complete"), (*IKRigExportFolderData)->GetBoolField(TEXT("is_complete_target_schema")));
		TestTrue(TEXT("IK assets ik rig folder export wrote files"), static_cast<int32>((*IKRigExportFolderData)->GetIntegerField(TEXT("file_count"))) >= 8);
	}

	TSharedPtr<FJsonObject> IKRigAssetFolderJson;
	TestTrue(TEXT("IK assets ik rig folder asset.json exists"), LoadSmokeJsonFile(this, FPaths::Combine(IKRigFolderPath, TEXT("asset.json")), IKRigAssetFolderJson, TEXT("IKRig folder asset.json")));
	TSharedPtr<FJsonObject> IKRigCoverageJson;
	TestTrue(TEXT("IK assets ik rig folder coverage exists"), LoadSmokeJsonFile(this, FPaths::Combine(IKRigFolderPath, TEXT("validation"), TEXT("coverage_report.json")), IKRigCoverageJson, TEXT("IKRig folder coverage_report.json")));

	const FString IKRigValidateFolderBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rig-folder-validate-001"),
		TEXT("ik_rig_validate_folder"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\"}"), *IKRigFolderJsonPath));
	FHttpSmokeResult IKRigValidateFolderResult;
	TestTrue(TEXT("IK assets ik_rig_validate_folder request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, IKRigValidateFolderBody, IKRigValidateFolderResult));
	TestEqual(TEXT("IK assets ik_rig_validate_folder status code"), IKRigValidateFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> IKRigValidateFolderJson;
	TestTrue(TEXT("IK assets ik_rig_validate_folder parses JSON"), ParseJson(IKRigValidateFolderResult.Body, IKRigValidateFolderJson));
	const TSharedPtr<FJsonObject>* IKRigValidateFolderData = nullptr;
	TestTrue(TEXT("IK assets ik_rig_validate_folder contains data object"), IKRigValidateFolderJson.IsValid() && IKRigValidateFolderJson->TryGetObjectField(TEXT("data"), IKRigValidateFolderData) && IKRigValidateFolderData && IKRigValidateFolderData->IsValid());
	if (IKRigValidateFolderData && IKRigValidateFolderData->IsValid())
	{
		TestTrue(TEXT("IK assets ik_rig_validate_folder is dry run"), (*IKRigValidateFolderData)->GetBoolField(TEXT("dry_run")));
		TestEqual(TEXT("IK assets ik_rig_validate_folder has no JSON issues"), static_cast<int32>((*IKRigValidateFolderData)->GetIntegerField(TEXT("json_issue_count"))), 0);
	}

	const FString IKRigApplyFolderBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rig-folder-apply-001"),
		TEXT("ik_rig_apply_folder"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\",\"save_after_apply\":false}"), *IKRigFolderJsonPath));
	FHttpSmokeResult IKRigApplyFolderResult;
	TestTrue(TEXT("IK assets ik_rig_apply_folder request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, IKRigApplyFolderBody, IKRigApplyFolderResult));
	TestEqual(TEXT("IK assets ik_rig_apply_folder status code"), IKRigApplyFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> IKRigApplyFolderJson;
	TestTrue(TEXT("IK assets ik_rig_apply_folder parses JSON"), ParseJson(IKRigApplyFolderResult.Body, IKRigApplyFolderJson));
	const TSharedPtr<FJsonObject>* IKRigApplyFolderData = nullptr;
	TestTrue(TEXT("IK assets ik_rig_apply_folder contains data object"), IKRigApplyFolderJson.IsValid() && IKRigApplyFolderJson->TryGetObjectField(TEXT("data"), IKRigApplyFolderData) && IKRigApplyFolderData && IKRigApplyFolderData->IsValid());
	if (IKRigApplyFolderData && IKRigApplyFolderData->IsValid())
	{
		TestTrue(TEXT("IK assets ik_rig_apply_folder applied"), (*IKRigApplyFolderData)->GetBoolField(TEXT("applied")));
		TestEqual(TEXT("IK assets ik_rig_apply_folder has no JSON issues"), static_cast<int32>((*IKRigApplyFolderData)->GetIntegerField(TEXT("json_issue_count"))), 0);
	}

	const FString CreateRetargeterBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-create-rtg-001"),
		TEXT("ik_retargeter_create"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"save_after_create\":false}"), *RetargeterAssetPath));
	FHttpSmokeResult CreateRetargeterResult;
	TestTrue(TEXT("IK assets create retargeter request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateRetargeterBody, CreateRetargeterResult));
	TestEqual(TEXT("IK assets create retargeter status code"), CreateRetargeterResult.StatusCode, 200);

	const FString SetRetargeterSourceRigBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-src-rig-001"),
		TEXT("ik_retargeter_set_ik_rig"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"source_or_target\":\"source\",\"ik_rig_path\":\"%s\",\"save_after_set\":false}"), *RetargeterAssetPath, *SourceIKRigAssetPath));
	FHttpSmokeResult SetRetargeterSourceRigResult;
	TestTrue(TEXT("IK assets retargeter set source rig request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetRetargeterSourceRigBody, SetRetargeterSourceRigResult));
	TestEqual(TEXT("IK assets retargeter set source rig status code"), SetRetargeterSourceRigResult.StatusCode, 200);

	const FString SetRetargeterTargetRigBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-dst-rig-001"),
		TEXT("ik_retargeter_set_ik_rig"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"source_or_target\":\"target\",\"ik_rig_path\":\"%s\",\"save_after_set\":false}"), *RetargeterAssetPath, *TargetIKRigAssetPath));
	FHttpSmokeResult SetRetargeterTargetRigResult;
	TestTrue(TEXT("IK assets retargeter set target rig request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetRetargeterTargetRigBody, SetRetargeterTargetRigResult));
	TestEqual(TEXT("IK assets retargeter set target rig status code"), SetRetargeterTargetRigResult.StatusCode, 200);

	const FString SetRetargeterSourceMeshBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-src-mesh-001"),
		TEXT("ik_retargeter_set_preview_mesh"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"source_or_target\":\"source\",\"skeletal_mesh_path\":\"%s\",\"save_after_set\":false}"), *RetargeterAssetPath, *SourceSkeletalMeshPath));
	FHttpSmokeResult SetRetargeterSourceMeshResult;
	TestTrue(TEXT("IK assets retargeter set source mesh request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetRetargeterSourceMeshBody, SetRetargeterSourceMeshResult));
	TestEqual(TEXT("IK assets retargeter set source mesh status code"), SetRetargeterSourceMeshResult.StatusCode, 200);

	const FString SetRetargeterTargetMeshBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-dst-mesh-001"),
		TEXT("ik_retargeter_set_preview_mesh"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"source_or_target\":\"target\",\"skeletal_mesh_path\":\"%s\",\"save_after_set\":false}"), *RetargeterAssetPath, *TargetMeshAssetPath));
	FHttpSmokeResult SetRetargeterTargetMeshResult;
	TestTrue(TEXT("IK assets retargeter set target mesh request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetRetargeterTargetMeshBody, SetRetargeterTargetMeshResult));
	TestEqual(TEXT("IK assets retargeter set target mesh status code"), SetRetargeterTargetMeshResult.StatusCode, 200);

	const FString AutoMapChainsBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-automap-001"),
		TEXT("ik_retargeter_auto_map_chains"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"auto_map_type\":\"exact\",\"force_remap\":true,\"save_after_set\":false}"), *RetargeterAssetPath));
	FHttpSmokeResult AutoMapChainsResult;
	TestTrue(TEXT("IK assets retargeter auto map chains request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AutoMapChainsBody, AutoMapChainsResult));
	TestEqual(TEXT("IK assets retargeter auto map chains status code"), AutoMapChainsResult.StatusCode, 200);

	const FString SetRetargeterSettingsBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-settings-001"),
		TEXT("ik_retargeter_set_settings"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"global_settings\":{\"enable_root\":false,\"enable_fk\":false,\"copy_base_pose\":true,\"copy_base_pose_root\":\"%s\",\"source_scale_factor\":1.25,\"warping\":true,\"direction_source\":\"chain\",\"forward_direction\":\"y\",\"direction_chain\":\"%s\",\"warp_forwards\":1.15,\"sideways_offset\":2.5,\"warp_splay\":1.2},\"root_settings\":{\"rotation_alpha\":0.75,\"translation_alpha\":0.6,\"blend_to_source\":0.25,\"blend_to_source_weights\":{\"x\":1.0,\"y\":0.5,\"z\":0.25},\"scale_horizontal\":1.1,\"scale_vertical\":0.9,\"translation_offset\":{\"x\":4.0,\"y\":5.0,\"z\":6.0},\"rotation_offset\":{\"pitch\":7.0,\"yaw\":8.0,\"roll\":9.0},\"affect_ik_horizontal\":0.4,\"affect_ik_vertical\":0.3},\"save_after_set\":false}"),
			*RetargeterAssetPath,
			*RootBoneName,
			*ChainName));
	FHttpSmokeResult SetRetargeterSettingsResult;
	TestTrue(TEXT("IK assets retargeter set settings request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetRetargeterSettingsBody, SetRetargeterSettingsResult));
	TestEqual(TEXT("IK assets retargeter set settings status code"), SetRetargeterSettingsResult.StatusCode, 200);

	const FString CreateTargetPoseBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-pose-001"),
		TEXT("ik_retargeter_set_pose"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"source_or_target\":\"target\",\"pose_name\":\"%s\",\"create_if_missing\":true,\"set_current\":true,\"save_after_set\":false}"), *RetargeterAssetPath, *TargetPoseName));
	FHttpSmokeResult CreateTargetPoseResult;
	TestTrue(TEXT("IK assets retargeter create pose request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateTargetPoseBody, CreateTargetPoseResult));
	TestEqual(TEXT("IK assets retargeter create pose status code"), CreateTargetPoseResult.StatusCode, 200);

	const FString UpdateTargetPoseBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-pose-002"),
		TEXT("ik_retargeter_set_pose"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"source_or_target\":\"target\",\"pose_name\":\"%s\",\"set_current\":true,\"root_offset\":{\"x\":1.0,\"y\":2.0,\"z\":3.0},\"bone_rotation_offsets\":[{\"bone_name\":\"%s\",\"rotation_offset\":{\"pitch\":5.0,\"yaw\":10.0,\"roll\":15.0}}],\"save_after_set\":false}"), *RetargeterAssetPath, *TargetPoseName, *SecondaryBoneName));
	FHttpSmokeResult UpdateTargetPoseResult;
	TestTrue(TEXT("IK assets retargeter update pose request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, UpdateTargetPoseBody, UpdateTargetPoseResult));
	TestEqual(TEXT("IK assets retargeter update pose status code"), UpdateTargetPoseResult.StatusCode, 200);

	const FString DuplicateTargetPoseBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-pose-003"),
		TEXT("ik_retargeter_set_pose"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"source_or_target\":\"target\",\"pose_name\":\"%s\",\"duplicate_from_pose\":\"%s\",\"save_after_set\":false}"), *RetargeterAssetPath, *TargetPoseCopyName, *TargetPoseName));
	FHttpSmokeResult DuplicateTargetPoseResult;
	TestTrue(TEXT("IK assets retargeter duplicate pose request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DuplicateTargetPoseBody, DuplicateTargetPoseResult));
	TestEqual(TEXT("IK assets retargeter duplicate pose status code"), DuplicateTargetPoseResult.StatusCode, 200);

	const FString RenameTargetPoseBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-pose-004"),
		TEXT("ik_retargeter_set_pose"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"source_or_target\":\"target\",\"pose_name\":\"%s\",\"rename_from_pose\":\"%s\",\"set_current\":true,\"save_after_set\":false}"), *RetargeterAssetPath, *TargetPoseRenamed, *TargetPoseCopyName));
	FHttpSmokeResult RenameTargetPoseResult;
	TestTrue(TEXT("IK assets retargeter rename pose request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RenameTargetPoseBody, RenameTargetPoseResult));
	TestEqual(TEXT("IK assets retargeter rename pose status code"), RenameTargetPoseResult.StatusCode, 200);

	const FString UpdateRenamedTargetPoseBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-pose-004b"),
		TEXT("ik_retargeter_set_pose"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"source_or_target\":\"target\",\"pose_name\":\"%s\",\"set_current\":true,\"root_offset\":{\"x\":1.0,\"y\":2.0,\"z\":3.0},\"bone_rotation_offsets\":[{\"bone_name\":\"%s\",\"rotation_offset\":{\"pitch\":5.0,\"yaw\":10.0,\"roll\":15.0}}],\"save_after_set\":false}"), *RetargeterAssetPath, *TargetPoseRenamed, *SecondaryBoneName));
	FHttpSmokeResult UpdateRenamedTargetPoseResult;
	TestTrue(TEXT("IK assets retargeter update renamed pose request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, UpdateRenamedTargetPoseBody, UpdateRenamedTargetPoseResult));
	TestEqual(TEXT("IK assets retargeter update renamed pose status code"), UpdateRenamedTargetPoseResult.StatusCode, 200);

	const FString RemoveOriginalTargetPoseBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-pose-005"),
		TEXT("ik_retargeter_set_pose"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"source_or_target\":\"target\",\"pose_name\":\"%s\",\"remove\":true,\"save_after_set\":false}"), *RetargeterAssetPath, *TargetPoseName));
	FHttpSmokeResult RemoveOriginalTargetPoseResult;
	TestTrue(TEXT("IK assets retargeter remove pose request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveOriginalTargetPoseBody, RemoveOriginalTargetPoseResult));
	TestEqual(TEXT("IK assets retargeter remove pose status code"), RemoveOriginalTargetPoseResult.StatusCode, 200);

	const FString RetargeterInfoBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-info-001"),
		TEXT("ik_retargeter_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *RetargeterAssetPath));
	FHttpSmokeResult RetargeterInfoResult;
	TestTrue(TEXT("IK assets retargeter get_info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RetargeterInfoBody, RetargeterInfoResult));
	TestEqual(TEXT("IK assets retargeter get_info status code"), RetargeterInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> RetargeterInfoJson;
	TestTrue(TEXT("IK assets retargeter get_info parses JSON"), ParseJson(RetargeterInfoResult.Body, RetargeterInfoJson));
	const TSharedPtr<FJsonObject>* RetargeterInfoData = nullptr;
	TestTrue(TEXT("IK assets retargeter get_info contains data object"), RetargeterInfoJson.IsValid() && RetargeterInfoJson->TryGetObjectField(TEXT("data"), RetargeterInfoData) && RetargeterInfoData && RetargeterInfoData->IsValid());
	if (RetargeterInfoData && RetargeterInfoData->IsValid())
	{
		TestTrue(TEXT("IK assets retargeter source rig updated"), (*RetargeterInfoData)->GetStringField(TEXT("source_ik_rig")).StartsWith(SourceIKRigAssetPath));
		TestTrue(TEXT("IK assets retargeter target rig updated"), (*RetargeterInfoData)->GetStringField(TEXT("target_ik_rig")).StartsWith(TargetIKRigAssetPath));
		TestTrue(TEXT("IK assets retargeter op count >= 1"), static_cast<int32>((*RetargeterInfoData)->GetIntegerField(TEXT("retarget_op_count"))) >= 1);
		TestEqual(TEXT("IK assets retargeter current target pose updated"), (*RetargeterInfoData)->GetStringField(TEXT("target_retarget_pose")), TargetPoseRenamed);
		const TArray<TSharedPtr<FJsonValue>>* TargetPoseNames = nullptr;
		TestTrue(TEXT("IK assets retargeter target pose name array exists"), (*RetargeterInfoData)->TryGetArrayField(TEXT("target_pose_names"), TargetPoseNames) && TargetPoseNames != nullptr);
		bool bHasRenamedPose = false;
		bool bHasRemovedOriginalPose = false;
		if (TargetPoseNames)
		{
			for (const TSharedPtr<FJsonValue>& PoseValue : *TargetPoseNames)
			{
				if (!PoseValue.IsValid())
				{
					continue;
				}
				const FString PoseName = PoseValue->AsString();
				bHasRenamedPose |= (PoseName == TargetPoseRenamed);
				bHasRemovedOriginalPose |= (PoseName == TargetPoseName);
			}
		}
		TestTrue(TEXT("IK assets retargeter renamed pose exists"), bHasRenamedPose);
		TestFalse(TEXT("IK assets retargeter original pose removed"), bHasRemovedOriginalPose);
		const TSharedPtr<FJsonObject>* CurrentTargetPose = nullptr;
		TestTrue(TEXT("IK assets retargeter current target pose data exists"), (*RetargeterInfoData)->TryGetObjectField(TEXT("current_target_pose"), CurrentTargetPose) && CurrentTargetPose && CurrentTargetPose->IsValid());
		if (CurrentTargetPose && CurrentTargetPose->IsValid())
		{
			const TSharedPtr<FJsonObject>* RootOffset = nullptr;
			TestTrue(TEXT("IK assets retargeter current target pose root offset exists"), (*CurrentTargetPose)->TryGetObjectField(TEXT("root_offset"), RootOffset) && RootOffset && RootOffset->IsValid());
			TestTrue(TEXT("IK assets retargeter target pose has bone offsets"), static_cast<int32>((*CurrentTargetPose)->GetIntegerField(TEXT("bone_rotation_offset_count"))) >= 1);
		}

		const TSharedPtr<FJsonObject>* GlobalSettings = nullptr;
		TestTrue(TEXT("IK assets retargeter global settings exists"), (*RetargeterInfoData)->TryGetObjectField(TEXT("global_settings"), GlobalSettings) && GlobalSettings && GlobalSettings->IsValid());
		if (GlobalSettings && GlobalSettings->IsValid())
		{
			TestFalse(TEXT("IK assets retargeter global enable_root updated"), (*GlobalSettings)->GetBoolField(TEXT("enable_root")));
			TestFalse(TEXT("IK assets retargeter global enable_fk updated"), (*GlobalSettings)->GetBoolField(TEXT("enable_fk")));
			TestTrue(TEXT("IK assets retargeter global copy_base_pose updated"), (*GlobalSettings)->GetBoolField(TEXT("copy_base_pose")));
			TestEqual(TEXT("IK assets retargeter global direction source updated"), (*GlobalSettings)->GetStringField(TEXT("direction_source")), FString(TEXT("chain")));
			TestEqual(TEXT("IK assets retargeter global direction chain updated"), (*GlobalSettings)->GetStringField(TEXT("direction_chain")), ChainName);
		}

		const TSharedPtr<FJsonObject>* RootSettings = nullptr;
		TestTrue(TEXT("IK assets retargeter root settings exists"), (*RetargeterInfoData)->TryGetObjectField(TEXT("root_settings"), RootSettings) && RootSettings && RootSettings->IsValid());
		if (RootSettings && RootSettings->IsValid())
		{
			TestTrue(TEXT("IK assets retargeter root rotation alpha updated"), FMath::IsNearlyEqual(static_cast<float>((*RootSettings)->GetNumberField(TEXT("rotation_alpha"))), 0.75f));
			TestTrue(TEXT("IK assets retargeter root translation alpha updated"), FMath::IsNearlyEqual(static_cast<float>((*RootSettings)->GetNumberField(TEXT("translation_alpha"))), 0.6f));
			const TSharedPtr<FJsonObject>* TranslationOffset = nullptr;
			TestTrue(TEXT("IK assets retargeter root translation offset exists"), (*RootSettings)->TryGetObjectField(TEXT("translation_offset"), TranslationOffset) && TranslationOffset && TranslationOffset->IsValid());
		}
	}

	const FString IKRetargeterFolderPath = MakeSmokeAssetFolderPath(TEXT("IKRetargeter"), RetargeterAssetPath);
	IFileManager::Get().DeleteDirectory(*IKRetargeterFolderPath, false, true);
	const FString IKRetargeterFolderJsonPath = ToSmokeJsonPath(IKRetargeterFolderPath);

	const FString IKRetargeterExportFolderBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-folder-export-001"),
		TEXT("ik_retargeter_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *RetargeterAssetPath));
	FHttpSmokeResult IKRetargeterExportFolderResult;
	TestTrue(TEXT("IK assets ik_retargeter_export_folder request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, IKRetargeterExportFolderBody, IKRetargeterExportFolderResult));
	TestEqual(TEXT("IK assets ik_retargeter_export_folder status code"), IKRetargeterExportFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> IKRetargeterExportFolderJson;
	TestTrue(TEXT("IK assets ik_retargeter_export_folder parses JSON"), ParseJson(IKRetargeterExportFolderResult.Body, IKRetargeterExportFolderJson));
	const TSharedPtr<FJsonObject>* IKRetargeterExportFolderData = nullptr;
	TestTrue(TEXT("IK assets ik_retargeter_export_folder contains data object"), IKRetargeterExportFolderJson.IsValid() && IKRetargeterExportFolderJson->TryGetObjectField(TEXT("data"), IKRetargeterExportFolderData) && IKRetargeterExportFolderData && IKRetargeterExportFolderData->IsValid());
	if (IKRetargeterExportFolderData && IKRetargeterExportFolderData->IsValid())
	{
		TestTrue(TEXT("IK assets ik retargeter folder export marked complete"), (*IKRetargeterExportFolderData)->GetBoolField(TEXT("is_complete_target_schema")));
		TestTrue(TEXT("IK assets ik retargeter folder export wrote files"), static_cast<int32>((*IKRetargeterExportFolderData)->GetIntegerField(TEXT("file_count"))) >= 12);
	}

	TSharedPtr<FJsonObject> IKRetargeterAssetFolderJson;
	TestTrue(TEXT("IK assets ik retargeter folder asset.json exists"), LoadSmokeJsonFile(this, FPaths::Combine(IKRetargeterFolderPath, TEXT("asset.json")), IKRetargeterAssetFolderJson, TEXT("IKRetargeter folder asset.json")));
	TSharedPtr<FJsonObject> IKRetargeterCoverageJson;
	TestTrue(TEXT("IK assets ik retargeter folder coverage exists"), LoadSmokeJsonFile(this, FPaths::Combine(IKRetargeterFolderPath, TEXT("validation"), TEXT("coverage_report.json")), IKRetargeterCoverageJson, TEXT("IKRetargeter folder coverage_report.json")));

	const FString IKRetargeterValidateFolderBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-folder-validate-001"),
		TEXT("ik_retargeter_validate_folder"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\"}"), *IKRetargeterFolderJsonPath));
	FHttpSmokeResult IKRetargeterValidateFolderResult;
	TestTrue(TEXT("IK assets ik_retargeter_validate_folder request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, IKRetargeterValidateFolderBody, IKRetargeterValidateFolderResult));
	TestEqual(TEXT("IK assets ik_retargeter_validate_folder status code"), IKRetargeterValidateFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> IKRetargeterValidateFolderJson;
	TestTrue(TEXT("IK assets ik_retargeter_validate_folder parses JSON"), ParseJson(IKRetargeterValidateFolderResult.Body, IKRetargeterValidateFolderJson));
	const TSharedPtr<FJsonObject>* IKRetargeterValidateFolderData = nullptr;
	TestTrue(TEXT("IK assets ik_retargeter_validate_folder contains data object"), IKRetargeterValidateFolderJson.IsValid() && IKRetargeterValidateFolderJson->TryGetObjectField(TEXT("data"), IKRetargeterValidateFolderData) && IKRetargeterValidateFolderData && IKRetargeterValidateFolderData->IsValid());
	if (IKRetargeterValidateFolderData && IKRetargeterValidateFolderData->IsValid())
	{
		TestTrue(TEXT("IK assets ik_retargeter_validate_folder is dry run"), (*IKRetargeterValidateFolderData)->GetBoolField(TEXT("dry_run")));
		TestEqual(TEXT("IK assets ik_retargeter_validate_folder has no JSON issues"), static_cast<int32>((*IKRetargeterValidateFolderData)->GetIntegerField(TEXT("json_issue_count"))), 0);
	}

	const FString IKRetargeterApplyFolderBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-rtg-folder-apply-001"),
		TEXT("ik_retargeter_apply_folder"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\",\"save_after_apply\":false}"), *IKRetargeterFolderJsonPath));
	FHttpSmokeResult IKRetargeterApplyFolderResult;
	TestTrue(TEXT("IK assets ik_retargeter_apply_folder request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, IKRetargeterApplyFolderBody, IKRetargeterApplyFolderResult));
	TestEqual(TEXT("IK assets ik_retargeter_apply_folder status code"), IKRetargeterApplyFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> IKRetargeterApplyFolderJson;
	TestTrue(TEXT("IK assets ik_retargeter_apply_folder parses JSON"), ParseJson(IKRetargeterApplyFolderResult.Body, IKRetargeterApplyFolderJson));
	const TSharedPtr<FJsonObject>* IKRetargeterApplyFolderData = nullptr;
	TestTrue(TEXT("IK assets ik_retargeter_apply_folder contains data object"), IKRetargeterApplyFolderJson.IsValid() && IKRetargeterApplyFolderJson->TryGetObjectField(TEXT("data"), IKRetargeterApplyFolderData) && IKRetargeterApplyFolderData && IKRetargeterApplyFolderData->IsValid());
	if (IKRetargeterApplyFolderData && IKRetargeterApplyFolderData->IsValid())
	{
		TestTrue(TEXT("IK assets ik_retargeter_apply_folder applied"), (*IKRetargeterApplyFolderData)->GetBoolField(TEXT("applied")));
		TestEqual(TEXT("IK assets ik_retargeter_apply_folder has no JSON issues"), static_cast<int32>((*IKRetargeterApplyFolderData)->GetIntegerField(TEXT("json_issue_count"))), 0);
	}

	const FString DuplicateAndRetargetBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-retarget-001"),
		TEXT("ik_retargeter_duplicate_and_retarget"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"asset_paths\":[\"%s\"],\"source_mesh_path\":\"%s\",\"target_mesh_path\":\"%s\",\"output_folder\":\"%s\",\"prefix\":\"RTG20_\",\"include_referenced_assets\":false}"), *RetargeterAssetPath, *SourceAnimationAssetPath, *SourceSkeletalMeshPath, *TargetMeshAssetPath, *RetargetOutputFolder));
	FHttpSmokeResult DuplicateAndRetargetResult;
	TestTrue(TEXT("IK assets duplicate_and_retarget request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DuplicateAndRetargetBody, DuplicateAndRetargetResult));
	TestEqual(TEXT("IK assets duplicate_and_retarget status code"), DuplicateAndRetargetResult.StatusCode, 200);

	TSharedPtr<FJsonObject> DuplicateAndRetargetJson;
	TestTrue(TEXT("IK assets duplicate_and_retarget parses JSON"), ParseJson(DuplicateAndRetargetResult.Body, DuplicateAndRetargetJson));
	const TSharedPtr<FJsonObject>* DuplicateAndRetargetData = nullptr;
	TestTrue(TEXT("IK assets duplicate_and_retarget contains data object"), DuplicateAndRetargetJson.IsValid() && DuplicateAndRetargetJson->TryGetObjectField(TEXT("data"), DuplicateAndRetargetData) && DuplicateAndRetargetData && DuplicateAndRetargetData->IsValid());
	if (DuplicateAndRetargetData && DuplicateAndRetargetData->IsValid())
	{
		TestTrue(TEXT("IK assets duplicate_and_retarget created assets"), static_cast<int32>((*DuplicateAndRetargetData)->GetIntegerField(TEXT("created_asset_count"))) >= 1);
	}

	const FString RetargetBatchOutputFolder = FString::Printf(TEXT("/Game/__UeAgentInterfaceSmoke/RetargetBatch_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString RetargetBatchJsonFile = ToSmokeJsonPath(FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("RetargetBatch"), FString::Printf(TEXT("Batch_%s.json"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)))));

	const FString RetargetBatchExportJsonBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-retarget-batch-export-001"),
		TEXT("retarget_batch_export_json"),
		FString::Printf(
			TEXT("{\"output_file\":\"%s\",\"retargeter\":\"%s\",\"asset_paths\":[\"%s\"],\"source_mesh\":\"%s\",\"target_mesh\":\"%s\",\"output_folder\":\"%s\",\"prefix\":\"RTGBatch_\"}"),
			*RetargetBatchJsonFile,
			*RetargeterAssetPath,
			*SourceAnimationAssetPath,
			*SourceSkeletalMeshPath,
			*TargetMeshAssetPath,
			*RetargetBatchOutputFolder));
	FHttpSmokeResult RetargetBatchExportJsonResult;
	TestTrue(TEXT("IK assets retarget_batch_export_json request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RetargetBatchExportJsonBody, RetargetBatchExportJsonResult));
	TestEqual(TEXT("IK assets retarget_batch_export_json status code"), RetargetBatchExportJsonResult.StatusCode, 200);

	TSharedPtr<FJsonObject> RetargetBatchFileJson;
	TestTrue(TEXT("IK assets retarget batch file exists"), LoadSmokeJsonFile(this, RetargetBatchJsonFile, RetargetBatchFileJson, TEXT("Retarget batch json")));

	const FString RetargetBatchValidateJsonBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-retarget-batch-validate-001"),
		TEXT("retarget_batch_validate_json"),
		FString::Printf(TEXT("{\"json_file\":\"%s\"}"), *RetargetBatchJsonFile));
	FHttpSmokeResult RetargetBatchValidateJsonResult;
	TestTrue(TEXT("IK assets retarget_batch_validate_json request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RetargetBatchValidateJsonBody, RetargetBatchValidateJsonResult));
	TestEqual(TEXT("IK assets retarget_batch_validate_json status code"), RetargetBatchValidateJsonResult.StatusCode, 200);

	TSharedPtr<FJsonObject> RetargetBatchValidateJson;
	TestTrue(TEXT("IK assets retarget_batch_validate_json parses JSON"), ParseJson(RetargetBatchValidateJsonResult.Body, RetargetBatchValidateJson));
	const TSharedPtr<FJsonObject>* RetargetBatchValidateData = nullptr;
	TestTrue(TEXT("IK assets retarget_batch_validate_json contains data object"), RetargetBatchValidateJson.IsValid() && RetargetBatchValidateJson->TryGetObjectField(TEXT("data"), RetargetBatchValidateData) && RetargetBatchValidateData && RetargetBatchValidateData->IsValid());
	if (RetargetBatchValidateData && RetargetBatchValidateData->IsValid())
	{
		TestTrue(TEXT("IK assets retarget_batch_validate_json valid"), (*RetargetBatchValidateData)->GetBoolField(TEXT("valid")));
		TestEqual(TEXT("IK assets retarget_batch_validate_json has no JSON issues"), static_cast<int32>((*RetargetBatchValidateData)->GetIntegerField(TEXT("json_issue_count"))), 0);
	}

	const FString RetargetBatchApplyJsonBody = MakeExecRequestBody(
		TEXT("smoke-ik-assets-retarget-batch-apply-001"),
		TEXT("retarget_batch_apply_json"),
		FString::Printf(TEXT("{\"json_file\":\"%s\"}"), *RetargetBatchJsonFile));
	FHttpSmokeResult RetargetBatchApplyJsonResult;
	TestTrue(TEXT("IK assets retarget_batch_apply_json request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RetargetBatchApplyJsonBody, RetargetBatchApplyJsonResult));
	TestEqual(TEXT("IK assets retarget_batch_apply_json status code"), RetargetBatchApplyJsonResult.StatusCode, 200);

	TSharedPtr<FJsonObject> RetargetBatchApplyJson;
	TestTrue(TEXT("IK assets retarget_batch_apply_json parses JSON"), ParseJson(RetargetBatchApplyJsonResult.Body, RetargetBatchApplyJson));
	const TSharedPtr<FJsonObject>* RetargetBatchApplyData = nullptr;
	TestTrue(TEXT("IK assets retarget_batch_apply_json contains data object"), RetargetBatchApplyJson.IsValid() && RetargetBatchApplyJson->TryGetObjectField(TEXT("data"), RetargetBatchApplyData) && RetargetBatchApplyData && RetargetBatchApplyData->IsValid());
	if (RetargetBatchApplyData && RetargetBatchApplyData->IsValid())
	{
		TestTrue(TEXT("IK assets retarget_batch_apply_json created assets"), static_cast<int32>((*RetargetBatchApplyData)->GetIntegerField(TEXT("created_asset_count"))) >= 1);
		TestEqual(TEXT("IK assets retarget_batch_apply_json has no JSON issues"), static_cast<int32>((*RetargetBatchApplyData)->GetIntegerField(TEXT("json_issue_count"))), 0);
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceNiagaraFolderSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.NiagaraFolderWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceNiagaraFolderSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString SystemAssetPath = MakeAutomationAssetPath(TEXT("NSFolder"));
	const FString EmitterAssetPath = MakeAutomationAssetPath(TEXT("NEFolder"));
	const FString AppliedSystemAssetPath = MakeAutomationAssetPath(TEXT("NSFolderApplied"));
	const FString AppliedEmitterAssetPath = MakeAutomationAssetPath(TEXT("NEFolderApplied"));
	const FString ScriptAssetPath = TEXT("/Niagara/Modules/Arrays/FillFloatArray");
	const FString AppliedScriptAssetPath = MakeAutomationAssetPath(TEXT("NMScriptFolderApplied"));
	const FString DeleteRecreateEmitterAssetPath = MakeAutomationAssetPath(TEXT("NEDeleteRecreate"));

	FString RelativeFolder = SystemAssetPath;
	while (RelativeFolder.StartsWith(TEXT("/")))
	{
		RelativeFolder.RightChopInline(1);
	}
	const FString FolderPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("NiagaraSystem"), RelativeFolder));
	IFileManager::Get().DeleteDirectory(*FolderPath, false, true);

	FString RelativeEmitterFolder = EmitterAssetPath;
	while (RelativeEmitterFolder.StartsWith(TEXT("/")))
	{
		RelativeEmitterFolder.RightChopInline(1);
	}
	const FString EmitterFolderPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("NiagaraEmitter"), RelativeEmitterFolder));
	IFileManager::Get().DeleteDirectory(*EmitterFolderPath, false, true);

	const FString ScriptFolderPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("NiagaraScript"), TEXT("Smoke"), TEXT("FillFloatArray")));
	IFileManager::Get().DeleteDirectory(*ScriptFolderPath, false, true);

	auto LoadJsonFile = [this](const FString& FilePath, TSharedPtr<FJsonObject>& OutObj) -> bool
	{
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
		{
			AddError(FString::Printf(TEXT("Niagara folder load failed: %s"), *FilePath));
			return false;
		}

		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutObj) || !OutObj.IsValid())
		{
			AddError(FString::Printf(TEXT("Niagara folder parse failed: %s"), *FilePath));
			return false;
		}
		return true;
	};

	const FString CreateEmitterBody = MakeExecRequestBody(
		TEXT("smoke-niagara-folder-create-emitter-001"),
		TEXT("niagara_create_emitter"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"add_default_modules_and_renderers\":true,\"save_after_create\":false}"), *EmitterAssetPath));
	FHttpSmokeResult CreateEmitterResult;
	TestTrue(TEXT("Niagara folder create emitter request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateEmitterBody, CreateEmitterResult, 20.0));
	TestEqual(TEXT("Niagara folder create emitter status code"), CreateEmitterResult.StatusCode, 200);

	const FString AddRendererBody = MakeExecRequestBody(
		TEXT("smoke-niagara-folder-add-renderer-001"),
		TEXT("niagara_emitter_add_renderer"),
		FString::Printf(TEXT("{\"emitter_asset_path\":\"%s\",\"renderer_type\":\"sprite\",\"renderer_name\":\"FolderSmokeSprite\",\"compile_after_set\":false,\"save_after_set\":false}"), *EmitterAssetPath));
	FHttpSmokeResult AddRendererResult;
	TestTrue(TEXT("Niagara folder add renderer request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddRendererBody, AddRendererResult, 20.0));
	TestEqual(TEXT("Niagara folder add renderer status code"), AddRendererResult.StatusCode, 200);

	const FString EmitterExportBody = MakeExecRequestBody(
		TEXT("smoke-niagara-emitter-folder-export-001"),
		TEXT("niagara_emitter_export_folder"),
		FString::Printf(TEXT("{\"emitter_asset_path\":\"%s\",\"clean_output_dir\":true}"), *EmitterAssetPath));
	FHttpSmokeResult EmitterExportResult;
	TestTrue(TEXT("Niagara emitter folder export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, EmitterExportBody, EmitterExportResult, 30.0));
	TestEqual(TEXT("Niagara emitter folder export status code"), EmitterExportResult.StatusCode, 200);

	TSharedPtr<FJsonObject> EmitterExportJson;
	TestTrue(TEXT("Niagara emitter folder export parses JSON"), ParseJson(EmitterExportResult.Body, EmitterExportJson));
	TestTrue(TEXT("Niagara emitter folder export root ok=true"), EmitterExportJson.IsValid() && EmitterExportJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* EmitterExportData = nullptr;
	TestTrue(TEXT("Niagara emitter folder export contains data object"), EmitterExportJson.IsValid() && EmitterExportJson->TryGetObjectField(TEXT("data"), EmitterExportData) && EmitterExportData && EmitterExportData->IsValid());
	if (EmitterExportData && EmitterExportData->IsValid())
	{
		TestEqual(TEXT("Niagara emitter folder export folder path"), (*EmitterExportData)->GetStringField(TEXT("folder_path")), EmitterFolderPath);
		TestTrue(TEXT("Niagara emitter folder export has files"), static_cast<int32>((*EmitterExportData)->GetIntegerField(TEXT("file_count"))) >= 8);
	}

	TSharedPtr<FJsonObject> EmitterCoverageJson;
	TestTrue(TEXT("Niagara emitter folder exported coverage report exists"), LoadJsonFile(FPaths::Combine(EmitterFolderPath, TEXT("validation"), TEXT("coverage_report.json")), EmitterCoverageJson));
	if (EmitterCoverageJson.IsValid())
	{
		TestEqual(TEXT("Niagara emitter folder implementation status"), EmitterCoverageJson->GetStringField(TEXT("implementation_status")), FString(TEXT("complete_folder_profile")));
		TestTrue(TEXT("Niagara emitter folder complete target schema is true"), EmitterCoverageJson->GetBoolField(TEXT("is_complete_target_schema")));
		TestTrue(TEXT("Niagara emitter folder lossless roundtrip is true"), EmitterCoverageJson->GetBoolField(TEXT("is_lossless_roundtrip")));
		const TArray<TSharedPtr<FJsonValue>>* PendingProfiles = nullptr;
		TestTrue(TEXT("Niagara emitter folder pending profiles empty"), EmitterCoverageJson->TryGetArrayField(TEXT("pending_profiles"), PendingProfiles) && PendingProfiles && PendingProfiles->Num() == 0);
	}

	const FString EmitterApplyBody = MakeExecRequestBody(
		TEXT("smoke-niagara-emitter-folder-apply-001"),
		TEXT("niagara_emitter_apply_folder"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\",\"emitter_asset_path\":\"%s\",\"create_if_missing\":true,\"save_after_apply\":false,\"strict\":false}"), *EmitterFolderPath.Replace(TEXT("\\"), TEXT("/")), *AppliedEmitterAssetPath));
	FHttpSmokeResult EmitterApplyResult;
	TestTrue(TEXT("Niagara emitter folder apply request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, EmitterApplyBody, EmitterApplyResult, 30.0));
	TestEqual(TEXT("Niagara emitter folder apply status code"), EmitterApplyResult.StatusCode, 200);

	TSharedPtr<FJsonObject> EmitterApplyJson;
	TestTrue(TEXT("Niagara emitter folder apply parses JSON"), ParseJson(EmitterApplyResult.Body, EmitterApplyJson));
	TestTrue(TEXT("Niagara emitter folder apply root ok=true"), EmitterApplyJson.IsValid() && EmitterApplyJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* EmitterApplyData = nullptr;
	TestTrue(TEXT("Niagara emitter folder apply contains data object"), EmitterApplyJson.IsValid() && EmitterApplyJson->TryGetObjectField(TEXT("data"), EmitterApplyData) && EmitterApplyData && EmitterApplyData->IsValid());
	if (EmitterApplyData && EmitterApplyData->IsValid())
	{
		TestTrue(TEXT("Niagara emitter folder apply created emitter"), (*EmitterApplyData)->GetBoolField(TEXT("created")));
		TestTrue(TEXT("Niagara emitter folder apply property count"), static_cast<int32>((*EmitterApplyData)->GetIntegerField(TEXT("emitter_properties_applied"))) >= 1);
	}

	const FString AppliedEmitterRenderersBody = MakeExecRequestBody(
		TEXT("smoke-niagara-emitter-folder-applied-renderers-001"),
		TEXT("niagara_emitter_list_renderers"),
		FString::Printf(TEXT("{\"emitter_asset_path\":\"%s\"}"), *AppliedEmitterAssetPath));
	FHttpSmokeResult AppliedEmitterRenderersResult;
	TestTrue(TEXT("Niagara emitter folder applied list renderers request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AppliedEmitterRenderersBody, AppliedEmitterRenderersResult, 20.0));
	TestEqual(TEXT("Niagara emitter folder applied list renderers status code"), AppliedEmitterRenderersResult.StatusCode, 200);
	TSharedPtr<FJsonObject> AppliedEmitterRenderersJson;
	TestTrue(TEXT("Niagara emitter folder applied list renderers parses JSON"), ParseJson(AppliedEmitterRenderersResult.Body, AppliedEmitterRenderersJson));
	const TSharedPtr<FJsonObject>* AppliedEmitterRenderersData = nullptr;
	TestTrue(TEXT("Niagara emitter folder applied list renderers contains data object"), AppliedEmitterRenderersJson.IsValid() && AppliedEmitterRenderersJson->TryGetObjectField(TEXT("data"), AppliedEmitterRenderersData) && AppliedEmitterRenderersData && AppliedEmitterRenderersData->IsValid());
	if (AppliedEmitterRenderersData && AppliedEmitterRenderersData->IsValid())
	{
		TestTrue(TEXT("Niagara emitter folder applied has renderer"), static_cast<int32>((*AppliedEmitterRenderersData)->GetIntegerField(TEXT("renderer_count"))) >= 1);
	}

	const FString ScriptExportBody = MakeExecRequestBody(
		TEXT("smoke-niagara-script-folder-export-001"),
		TEXT("niagara_script_export_folder"),
		FString::Printf(TEXT("{\"script_asset_path\":\"%s\",\"folder_path\":\"%s\",\"clean_output_dir\":true}"), *ScriptAssetPath, *ScriptFolderPath.Replace(TEXT("\\"), TEXT("/"))));
	FHttpSmokeResult ScriptExportResult;
	TestTrue(TEXT("Niagara script folder export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ScriptExportBody, ScriptExportResult, 30.0));
	TestEqual(TEXT("Niagara script folder export status code"), ScriptExportResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ScriptExportJson;
	TestTrue(TEXT("Niagara script folder export parses JSON"), ParseJson(ScriptExportResult.Body, ScriptExportJson));
	TestTrue(TEXT("Niagara script folder export root ok=true"), ScriptExportJson.IsValid() && ScriptExportJson->GetBoolField(TEXT("ok")));

	TSharedPtr<FJsonObject> ScriptCoverageJson;
	TestTrue(TEXT("Niagara script folder exported coverage report exists"), LoadJsonFile(FPaths::Combine(ScriptFolderPath, TEXT("validation"), TEXT("coverage_report.json")), ScriptCoverageJson));
	if (ScriptCoverageJson.IsValid())
	{
		TestEqual(TEXT("Niagara script folder implementation status"), ScriptCoverageJson->GetStringField(TEXT("implementation_status")), FString(TEXT("complete_folder_profile")));
		TestTrue(TEXT("Niagara script folder complete target schema is true"), ScriptCoverageJson->GetBoolField(TEXT("is_complete_target_schema")));
	}

	const FString ScriptApplyBody = MakeExecRequestBody(
		TEXT("smoke-niagara-script-folder-apply-001"),
		TEXT("niagara_script_apply_folder"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\",\"script_asset_path\":\"%s\",\"create_if_missing\":true,\"compile_after_apply\":false,\"save_after_apply\":false,\"strict\":false}"), *ScriptFolderPath.Replace(TEXT("\\"), TEXT("/")), *AppliedScriptAssetPath));
	FHttpSmokeResult ScriptApplyResult;
	TestTrue(TEXT("Niagara script folder apply request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ScriptApplyBody, ScriptApplyResult, 30.0));
	TestEqual(TEXT("Niagara script folder apply status code"), ScriptApplyResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ScriptApplyJson;
	TestTrue(TEXT("Niagara script folder apply parses JSON"), ParseJson(ScriptApplyResult.Body, ScriptApplyJson));
	TestTrue(TEXT("Niagara script folder apply root ok=true"), ScriptApplyJson.IsValid() && ScriptApplyJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* ScriptApplyData = nullptr;
	TestTrue(TEXT("Niagara script folder apply contains data object"), ScriptApplyJson.IsValid() && ScriptApplyJson->TryGetObjectField(TEXT("data"), ScriptApplyData) && ScriptApplyData && ScriptApplyData->IsValid());
	if (ScriptApplyData && ScriptApplyData->IsValid())
	{
		TestTrue(TEXT("Niagara script folder apply created script"), (*ScriptApplyData)->GetBoolField(TEXT("created")));
	}

	const FString CreateSystemBody = MakeExecRequestBody(
		TEXT("smoke-niagara-folder-create-system-001"),
		TEXT("niagara_create_system"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"create_default_nodes\":true,\"save_after_create\":false}"), *SystemAssetPath));
	FHttpSmokeResult CreateSystemResult;
	TestTrue(TEXT("Niagara folder create system request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateSystemBody, CreateSystemResult, 20.0));
	TestEqual(TEXT("Niagara folder create system status code"), CreateSystemResult.StatusCode, 200);

	const FString AddEmitterBody = MakeExecRequestBody(
		TEXT("smoke-niagara-folder-add-emitter-001"),
		TEXT("niagara_system_add_emitter"),
		FString::Printf(TEXT("{\"system_asset_path\":\"%s\",\"emitter_asset_path\":\"%s\",\"emitter_name\":\"FolderSmokeEmitter\",\"compile_after_set\":false,\"save_after_set\":false}"), *SystemAssetPath, *EmitterAssetPath));
	FHttpSmokeResult AddEmitterResult;
	TestTrue(TEXT("Niagara folder add emitter request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddEmitterBody, AddEmitterResult, 20.0));
	TestEqual(TEXT("Niagara folder add emitter status code"), AddEmitterResult.StatusCode, 200);

	const FString AddUserParameterBody = MakeExecRequestBody(
		TEXT("smoke-niagara-folder-add-user-param-001"),
		TEXT("niagara_user_parameter_add"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"parameter_name\":\"FolderSmokeFloat\",\"parameter_type\":\"float\",\"default_value_text\":\"3.500000\",\"compile_after_set\":false,\"save_after_set\":false}"), *SystemAssetPath));
	FHttpSmokeResult AddUserParameterResult;
	TestTrue(TEXT("Niagara folder add user parameter request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddUserParameterBody, AddUserParameterResult, 20.0));
	TestEqual(TEXT("Niagara folder add user parameter status code"), AddUserParameterResult.StatusCode, 200);

	const FString ExportBody = MakeExecRequestBody(
		TEXT("smoke-niagara-folder-export-001"),
		TEXT("niagara_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clean_output_dir\":true}"), *SystemAssetPath));
	FHttpSmokeResult ExportResult;
	TestTrue(TEXT("Niagara folder export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ExportBody, ExportResult, 30.0));
	TestEqual(TEXT("Niagara folder export status code"), ExportResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ExportJson;
	TestTrue(TEXT("Niagara folder export parses JSON"), ParseJson(ExportResult.Body, ExportJson));
	TestTrue(TEXT("Niagara folder export root ok=true"), ExportJson.IsValid() && ExportJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* ExportData = nullptr;
	TestTrue(TEXT("Niagara folder export contains data object"), ExportJson.IsValid() && ExportJson->TryGetObjectField(TEXT("data"), ExportData) && ExportData && ExportData->IsValid());
	if (ExportData && ExportData->IsValid())
	{
		TestEqual(TEXT("Niagara folder export folder path"), (*ExportData)->GetStringField(TEXT("folder_path")), FolderPath);
		TestTrue(TEXT("Niagara folder export has files"), static_cast<int32>((*ExportData)->GetIntegerField(TEXT("file_count"))) >= 12);
		TestTrue(TEXT("Niagara folder export has emitter"), static_cast<int32>((*ExportData)->GetIntegerField(TEXT("emitter_count"))) >= 1);
		TestTrue(TEXT("Niagara folder export has user parameter"), static_cast<int32>((*ExportData)->GetIntegerField(TEXT("user_parameter_count"))) >= 1);
	}

	TSharedPtr<FJsonObject> AssetJson;
	TestTrue(TEXT("Niagara folder exported asset.json exists"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson));
	if (AssetJson.IsValid())
	{
		TestEqual(TEXT("Niagara folder exported asset kind"), AssetJson->GetStringField(TEXT("asset_kind")), FString(TEXT("niagara_system")));
		TestEqual(TEXT("Niagara folder exported asset path"), AssetJson->GetStringField(TEXT("asset_path")), SystemAssetPath);
	}

	TSharedPtr<FJsonObject> EmittersIndexJson;
	TestTrue(TEXT("Niagara folder exported emitters index exists"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("emitters"), TEXT("index.json")), EmittersIndexJson));
	if (EmittersIndexJson.IsValid())
	{
		TestTrue(TEXT("Niagara folder exported emitters count >= 1"), static_cast<int32>(EmittersIndexJson->GetIntegerField(TEXT("emitter_count"))) >= 1);
	}

	TSharedPtr<FJsonObject> CoverageJson;
	TestTrue(TEXT("Niagara folder exported coverage report exists"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("coverage_report.json")), CoverageJson));
	if (CoverageJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* CoverageAreas = nullptr;
		TestTrue(TEXT("Niagara folder coverage areas exist"), CoverageJson->TryGetArrayField(TEXT("coverage_areas"), CoverageAreas) && CoverageAreas != nullptr);
		TestTrue(TEXT("Niagara folder coverage area count reasonable"), CoverageAreas && CoverageAreas->Num() >= 8);
		TestEqual(TEXT("Niagara folder implementation status"), CoverageJson->GetStringField(TEXT("implementation_status")), FString(TEXT("complete_folder_profile")));
		TestTrue(TEXT("Niagara folder complete target schema is true"), CoverageJson->GetBoolField(TEXT("is_complete_target_schema")));
		TestTrue(TEXT("Niagara folder lossless roundtrip is true"), CoverageJson->GetBoolField(TEXT("is_lossless_roundtrip")));
		const TArray<TSharedPtr<FJsonValue>>* BlockingGaps = nullptr;
		TestTrue(TEXT("Niagara folder blocking gaps empty"), CoverageJson->TryGetArrayField(TEXT("blocking_gaps"), BlockingGaps) && BlockingGaps && BlockingGaps->Num() == 0);
	}

	const FString ApplyBody = MakeExecRequestBody(
		TEXT("smoke-niagara-folder-apply-001"),
		TEXT("niagara_apply_folder"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\",\"asset_path\":\"%s\",\"create_if_missing\":true,\"apply_referenced_emitters\":true,\"compile_after_apply\":false,\"save_after_apply\":false,\"strict\":false}"), *FolderPath.Replace(TEXT("\\"), TEXT("/")), *AppliedSystemAssetPath));
	FHttpSmokeResult ApplyResult;
	TestTrue(TEXT("Niagara folder apply request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ApplyBody, ApplyResult, 30.0));
	TestEqual(TEXT("Niagara folder apply status code"), ApplyResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ApplyJson;
	TestTrue(TEXT("Niagara folder apply parses JSON"), ParseJson(ApplyResult.Body, ApplyJson));
	TestTrue(TEXT("Niagara folder apply root ok=true"), ApplyJson.IsValid() && ApplyJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* ApplyData = nullptr;
	TestTrue(TEXT("Niagara folder apply contains data object"), ApplyJson.IsValid() && ApplyJson->TryGetObjectField(TEXT("data"), ApplyData) && ApplyData && ApplyData->IsValid());
	if (ApplyData && ApplyData->IsValid())
	{
		TestTrue(TEXT("Niagara folder apply created system"), (*ApplyData)->GetBoolField(TEXT("created")));
		TestTrue(TEXT("Niagara folder apply user parameters"), static_cast<int32>((*ApplyData)->GetIntegerField(TEXT("user_parameters_applied"))) >= 1);
		TestTrue(TEXT("Niagara folder apply emitter handles"), static_cast<int32>((*ApplyData)->GetIntegerField(TEXT("emitter_handles_applied"))) >= 1);
	}

	const FString AppliedInfoBody = MakeExecRequestBody(
		TEXT("smoke-niagara-folder-applied-info-001"),
		TEXT("niagara_get_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *AppliedSystemAssetPath));
	FHttpSmokeResult AppliedInfoResult;
	TestTrue(TEXT("Niagara folder applied get_info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AppliedInfoBody, AppliedInfoResult, 20.0));
	TestEqual(TEXT("Niagara folder applied get_info status code"), AppliedInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AppliedInfoJson;
	TestTrue(TEXT("Niagara folder applied get_info parses JSON"), ParseJson(AppliedInfoResult.Body, AppliedInfoJson));
	const TSharedPtr<FJsonObject>* AppliedInfoData = nullptr;
	TestTrue(TEXT("Niagara folder applied get_info contains data object"), AppliedInfoJson.IsValid() && AppliedInfoJson->TryGetObjectField(TEXT("data"), AppliedInfoData) && AppliedInfoData && AppliedInfoData->IsValid());
	if (AppliedInfoData && AppliedInfoData->IsValid())
	{
		TestTrue(TEXT("Niagara folder applied system has emitter"), static_cast<int32>((*AppliedInfoData)->GetIntegerField(TEXT("emitter_handle_count"))) >= 1);
		TestTrue(TEXT("Niagara folder applied system has user parameter"), static_cast<int32>((*AppliedInfoData)->GetIntegerField(TEXT("user_parameter_count"))) >= 1);
	}

	const FString DeleteRecreateCreateBody = MakeExecRequestBody(
		TEXT("smoke-niagara-delete-recreate-create-001"),
		TEXT("niagara_create_emitter"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"add_default_modules_and_renderers\":true,\"save_after_create\":false}"), *DeleteRecreateEmitterAssetPath));
	FHttpSmokeResult DeleteRecreateCreateResult;
	TestTrue(TEXT("Niagara delete recreate initial create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DeleteRecreateCreateBody, DeleteRecreateCreateResult, 20.0));
	TestEqual(TEXT("Niagara delete recreate initial create status code"), DeleteRecreateCreateResult.StatusCode, 200);

	const FString DeleteRecreateDeleteBody = MakeExecRequestBody(
		TEXT("smoke-niagara-delete-recreate-delete-001"),
		TEXT("niagara_delete_asset"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"force_delete\":true}"), *DeleteRecreateEmitterAssetPath));
	FHttpSmokeResult DeleteRecreateDeleteResult;
	TestTrue(TEXT("Niagara delete recreate delete request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DeleteRecreateDeleteBody, DeleteRecreateDeleteResult, 20.0));
	TestEqual(TEXT("Niagara delete recreate delete status code"), DeleteRecreateDeleteResult.StatusCode, 200);

	TSharedPtr<FJsonObject> DeleteRecreateDeleteJson;
	TestTrue(TEXT("Niagara delete recreate delete parses JSON"), ParseJson(DeleteRecreateDeleteResult.Body, DeleteRecreateDeleteJson));
	const TSharedPtr<FJsonObject>* DeleteRecreateDeleteData = nullptr;
	TestTrue(TEXT("Niagara delete recreate delete contains data object"), DeleteRecreateDeleteJson.IsValid() && DeleteRecreateDeleteJson->TryGetObjectField(TEXT("data"), DeleteRecreateDeleteData) && DeleteRecreateDeleteData && DeleteRecreateDeleteData->IsValid());
	if (DeleteRecreateDeleteData && DeleteRecreateDeleteData->IsValid())
	{
		TestEqual(TEXT("Niagara delete recreate delete strategy"), (*DeleteRecreateDeleteData)->GetStringField(TEXT("delete_strategy")), FString(TEXT("delete_objects_unchecked")));
	}

	const FString DeleteRecreateCreateAgainBody = MakeExecRequestBody(
		TEXT("smoke-niagara-delete-recreate-create-002"),
		TEXT("niagara_create_emitter"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"add_default_modules_and_renderers\":true,\"save_after_create\":false}"), *DeleteRecreateEmitterAssetPath));
	FHttpSmokeResult DeleteRecreateCreateAgainResult;
	TestTrue(TEXT("Niagara delete recreate second create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DeleteRecreateCreateAgainBody, DeleteRecreateCreateAgainResult, 20.0));
	TestEqual(TEXT("Niagara delete recreate second create status code"), DeleteRecreateCreateAgainResult.StatusCode, 200);

	const FString DeleteRecreateCleanupBody = MakeExecRequestBody(
		TEXT("smoke-niagara-delete-recreate-cleanup-001"),
		TEXT("niagara_delete_asset"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"force_delete\":true}"), *DeleteRecreateEmitterAssetPath));
	FHttpSmokeResult DeleteRecreateCleanupResult;
	TestTrue(TEXT("Niagara delete recreate cleanup request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DeleteRecreateCleanupBody, DeleteRecreateCleanupResult, 20.0));
	TestEqual(TEXT("Niagara delete recreate cleanup status code"), DeleteRecreateCleanupResult.StatusCode, 200);

	return !HasAnyErrors();
}

#endif
