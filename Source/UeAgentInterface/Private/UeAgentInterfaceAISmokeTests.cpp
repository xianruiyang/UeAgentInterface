#if WITH_DEV_AUTOMATION_TESTS

#include "UeAgentHttpServer.h"
#include "UeAgentInterfaceSettings.h"

#include "AIController.h"
#include "Async/TaskGraphInterfaces.h"
#include "BehaviorTree/BehaviorTree.h"
#include "Components/StateTreeComponent.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "Perception/AIPerceptionComponent.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SmartObjectComponent.h"
#include "SmartObjectDefinition.h"
#include "StateTree.h"

DEFINE_LOG_CATEGORY_STATIC(LogUeAgentInterfaceSmoke, Log, All);

#include "UeAgentInterfaceSmokeTestCommon.inl"

namespace
{
	static FString ToObjectPathForSmoke(const FString& AssetPath)
	{
		if (AssetPath.Contains(TEXT(".")))
		{
			return AssetPath;
		}
		return FString::Printf(TEXT("%s.%s"), *AssetPath, *FPackageName::GetLongPackageAssetName(AssetPath));
	}

	static bool ExecSmokeCommand(
		FAutomationTestBase* Test,
		FScopedUeAgentHttpServer& ServerScope,
		const FString& RequestId,
		const FString& Command,
		const FString& ParamsJson,
		TSharedPtr<FJsonObject>& OutData,
		const double TimeoutSeconds = 30.0)
	{
		FHttpSmokeResult Result;
		const FString Body = MakeExecRequestBody(RequestId, Command, ParamsJson);
		if (!Test->TestTrue(Command + TEXT(" request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, Body, Result, TimeoutSeconds)))
		{
			return false;
		}
		if (!Test->TestEqual(Command + TEXT(" http status"), Result.StatusCode, 200))
		{
			Test->AddError(Result.Body.Left(512));
			return false;
		}

		TSharedPtr<FJsonObject> Root;
		if (!Test->TestTrue(Command + TEXT(" response parses"), ParseJson(Result.Body, Root)) || !Root.IsValid())
		{
			return false;
		}
		const bool bOk = Root->GetBoolField(TEXT("ok"));
		if (!Test->TestTrue(Command + TEXT(" ok=true"), bOk))
		{
			Test->AddError(Root->HasField(TEXT("error")) ? Root->GetStringField(TEXT("error")) : Result.Body.Left(512));
			return false;
		}
		const TSharedPtr<FJsonObject>* Data = nullptr;
		if (Root->TryGetObjectField(TEXT("data"), Data) && Data && Data->IsValid())
		{
			OutData = *Data;
		}
		else
		{
			OutData = MakeShared<FJsonObject>();
		}
		return true;
	}

	static bool JsonIssuesContainCode(const TSharedPtr<FJsonObject>& Data, const FString& ExpectedCode)
	{
		if (!Data.IsValid() || ExpectedCode.IsEmpty())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Issues = nullptr;
		if (!Data->TryGetArrayField(TEXT("json_issues"), Issues) || !Issues)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& IssueValue : *Issues)
		{
			const TSharedPtr<FJsonObject>* IssueObject = nullptr;
			if (!IssueValue.IsValid() || !IssueValue->TryGetObject(IssueObject) || !IssueObject || !IssueObject->IsValid())
			{
				continue;
			}
			FString Code;
			if ((*IssueObject)->TryGetStringField(TEXT("code"), Code) && (Code == ExpectedCode || Code.Contains(ExpectedCode)))
			{
				return true;
			}
		}
		return false;
	}

	static bool ExecSmokeCommandExpectFailure(
		FAutomationTestBase* Test,
		FScopedUeAgentHttpServer& ServerScope,
		const FString& RequestId,
		const FString& Command,
		const FString& ParamsJson,
		const FString& ExpectedToken,
		TSharedPtr<FJsonObject>& OutData,
		const double TimeoutSeconds = 30.0)
	{
		FHttpSmokeResult Result;
		const FString Body = MakeExecRequestBody(RequestId, Command, ParamsJson);
		if (!Test->TestTrue(Command + TEXT(" request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, Body, Result, TimeoutSeconds)))
		{
			return false;
		}
		if (!Test->TestTrue(Command + TEXT(" http status is command failure"), Result.StatusCode == 200 || Result.StatusCode == 400))
		{
			Test->AddError(Result.Body.Left(512));
			return false;
		}

		TSharedPtr<FJsonObject> Root;
		if (!Test->TestTrue(Command + TEXT(" response parses"), ParseJson(Result.Body, Root)) || !Root.IsValid())
		{
			return false;
		}

		bool bOk = true;
		Root->TryGetBoolField(TEXT("ok"), bOk);
		if (!Test->TestFalse(Command + TEXT(" ok=false"), bOk))
		{
			Test->AddError(Result.Body.Left(1024));
			return false;
		}

		const TSharedPtr<FJsonObject>* Data = nullptr;
		if (Root->TryGetObjectField(TEXT("data"), Data) && Data && Data->IsValid())
		{
			OutData = *Data;
		}
		else
		{
			OutData = MakeShared<FJsonObject>();
		}

		FString Error;
		Root->TryGetStringField(TEXT("error"), Error);
		FString Status;
		OutData->TryGetStringField(TEXT("status"), Status);
		const bool bMatched =
			ExpectedToken.IsEmpty() ||
			Error == ExpectedToken ||
			Error.Contains(ExpectedToken) ||
			Status == ExpectedToken ||
			Status.Contains(ExpectedToken) ||
			JsonIssuesContainCode(OutData, ExpectedToken);
		if (!Test->TestTrue(Command + TEXT(" expected failure token ") + ExpectedToken, bMatched))
		{
			Test->AddError(Result.Body.Left(2048));
			return false;
		}
		return true;
	}

	static bool SaveSmokeUtf8File(FAutomationTestBase* Test, const FString& FilePath, const FString& Text)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
		const bool bSaved = FFileHelper::SaveStringToFile(Text, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		return Test->TestTrue(FString::Printf(TEXT("save smoke file %s"), *FilePath), bSaved);
	}

	static FString JsonObjectToStringForSmoke(const TSharedPtr<FJsonObject>& Object)
	{
		if (!Object.IsValid())
		{
			return TEXT("{}");
		}
		FString Output;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		return Output;
	}

	static bool HasCompleteCoverageReport(const TSharedPtr<FJsonObject>& Object)
	{
		const TSharedPtr<FJsonObject>* Coverage = nullptr;
		bool bComplete = false;
		return Object.IsValid() &&
			Object->TryGetObjectField(TEXT("coverage_report"), Coverage) &&
			Coverage && Coverage->IsValid() &&
			(*Coverage)->TryGetBoolField(TEXT("is_complete_target_schema"), bComplete) &&
			bComplete &&
			(*Coverage)->HasTypedField<EJson::Array>(TEXT("pending_profiles")) &&
			(*Coverage)->HasTypedField<EJson::Array>(TEXT("blocking_gaps"));
	}

	static AActor* SpawnSmokeActor(UWorld* World, const FString& Label, const FVector& Location)
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.ObjectFlags |= RF_Transactional;
		AActor* Actor = World ? World->SpawnActor<AActor>(AActor::StaticClass(), Location, FRotator::ZeroRotator, Params) : nullptr;
		if (Actor)
		{
			Actor->SetActorLabel(Label);
		}
		return Actor;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceAIBehaviorStackSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.AIBehaviorStackCommands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceAIBehaviorStackSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!TestNotNull(TEXT("Editor world"), World))
	{
		return false;
	}

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString BlackboardPath = MakeAutomationAssetPath(TEXT("BB_AIBehavior"));
	const FString BehaviorTreePath = MakeAutomationAssetPath(TEXT("BT_AIBehavior"));
	const FString StateTreePath = MakeAutomationAssetPath(TEXT("ST_AIBehavior"));
	const FString EqsPath = MakeAutomationAssetPath(TEXT("EQS_AIBehavior"));
	const FString SmartObjectPath = MakeAutomationAssetPath(TEXT("SO_AIBehavior"));
	const FString BlackboardName = FPackageName::GetLongPackageAssetName(BlackboardPath);
	const FString BehaviorTreeName = FPackageName::GetLongPackageAssetName(BehaviorTreePath);
	const FString StateTreeName = FPackageName::GetLongPackageAssetName(StateTreePath);
	const FString EqsName = FPackageName::GetLongPackageAssetName(EqsPath);
	const FString SmartObjectName = FPackageName::GetLongPackageAssetName(SmartObjectPath);
	const FString ExportRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAgentInterfaceAISmoke"), Suffix));
	IFileManager::Get().MakeDirectory(*ExportRoot, true);
	const FString BlackboardJson = FPaths::Combine(ExportRoot, TEXT("blackboard.json")).Replace(TEXT("\\"), TEXT("/"));
	const FString NavJson = FPaths::Combine(ExportRoot, TEXT("navigation.json")).Replace(TEXT("\\"), TEXT("/"));
	const FString BTFolder = FPaths::Combine(ExportRoot, TEXT("bt")).Replace(TEXT("\\"), TEXT("/"));
	const FString StateTreeFolder = FPaths::Combine(ExportRoot, TEXT("statetree")).Replace(TEXT("\\"), TEXT("/"));
	const FString EqsFolder = FPaths::Combine(ExportRoot, TEXT("eqs")).Replace(TEXT("\\"), TEXT("/"));
	const FString StackFolder = FPaths::Combine(ExportRoot, TEXT("stack")).Replace(TEXT("\\"), TEXT("/"));

	TSharedPtr<FJsonObject> Data;
	TestTrue(TEXT("blackboard_create"), ExecSmokeCommand(this, ServerScope, TEXT("ai-bb-create"), TEXT("blackboard_create"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"keys\":[{\"name\":\"TargetActor\",\"key_type\":\"object\",\"base_class\":\"/Script/Engine.Actor\"},{\"name\":\"IsAlerted\",\"type\":\"bool\"},{\"name\":\"MoveLocation\",\"type\":\"vector\"}],\"save_after_create\":false}"), *BlackboardPath), Data));
	TestTrue(TEXT("blackboard_get_info"), ExecSmokeCommand(this, ServerScope, TEXT("ai-bb-info"), TEXT("blackboard_get_info"), FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *BlackboardPath), Data));
	TestTrue(TEXT("blackboard_get_info exports key_type alias"), Data.IsValid() && Data->HasTypedField<EJson::Array>(TEXT("keys")) && JsonObjectToStringForSmoke(Data).Contains(TEXT("\"key_type\"")));
	TestTrue(TEXT("blackboard_export_json"), ExecSmokeCommand(this, ServerScope, TEXT("ai-bb-export"), TEXT("blackboard_export_json"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"output_file\":\"%s\"}"), *BlackboardPath, *BlackboardJson), Data));
	TestTrue(TEXT("blackboard_export_json returns coverage_report"), HasCompleteCoverageReport(Data));
	TestTrue(TEXT("blackboard_validate_json"), ExecSmokeCommand(this, ServerScope, TEXT("ai-bb-validate"), TEXT("blackboard_validate_json"), FString::Printf(TEXT("{\"json_file\":\"%s\",\"strict\":true}"), *BlackboardJson), Data));
	TestTrue(TEXT("blackboard_validate_json returns coverage_report"), HasCompleteCoverageReport(Data));
	TestTrue(TEXT("blackboard_apply_json"), ExecSmokeCommand(this, ServerScope, TEXT("ai-bb-apply"), TEXT("blackboard_apply_json"), FString::Printf(TEXT("{\"json_file\":\"%s\",\"asset_path\":\"%s\",\"allow_destructive\":true,\"save_after_apply\":false}"), *BlackboardJson, *BlackboardPath), Data));
	TestTrue(TEXT("blackboard_apply_json returns coverage_report"), HasCompleteCoverageReport(Data));

	TestTrue(TEXT("behavior_tree_create"), ExecSmokeCommand(this, ServerScope, TEXT("ai-bt-create"), TEXT("behavior_tree_create"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"blackboard_asset\":\"%s\",\"root_composite\":\"sequence\",\"save_after_create\":false}"), *BehaviorTreePath, *BlackboardPath), Data));
	TestTrue(TEXT("behavior_tree_get_info"), ExecSmokeCommand(this, ServerScope, TEXT("ai-bt-info"), TEXT("behavior_tree_get_info"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_nodes\":true}"), *BehaviorTreePath), Data));
	TestTrue(TEXT("behavior_tree_export_folder"), ExecSmokeCommand(this, ServerScope, TEXT("ai-bt-export"), TEXT("behavior_tree_export_folder"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"folder_path\":\"%s\",\"clean_output_dir\":true}"), *BehaviorTreePath, *BTFolder), Data));
	TestTrue(TEXT("behavior_tree_export_folder returns coverage_report"), HasCompleteCoverageReport(Data));
	TestTrue(TEXT("behavior_tree_export_folder writes validation coverage"), IFileManager::Get().FileExists(*FPaths::Combine(BTFolder, TEXT("validation/coverage_report.json"))));
	TestTrue(TEXT("behavior_tree_validate_folder"), ExecSmokeCommand(this, ServerScope, TEXT("ai-bt-validate"), TEXT("behavior_tree_validate_folder"), FString::Printf(TEXT("{\"folder_path\":\"%s\",\"validate_blackboard_keys\":true}"), *BTFolder), Data));
	TestTrue(TEXT("behavior_tree_validate_folder returns coverage_report"), HasCompleteCoverageReport(Data));
	TestTrue(TEXT("behavior_tree_apply_folder"), ExecSmokeCommand(this, ServerScope, TEXT("ai-bt-apply"), TEXT("behavior_tree_apply_folder"), FString::Printf(TEXT("{\"folder_path\":\"%s\",\"asset_path\":\"%s\",\"allow_destructive\":true,\"save_after_apply\":false}"), *BTFolder, *BehaviorTreePath), Data));
	TestTrue(TEXT("behavior_tree_apply_folder returns coverage_report"), HasCompleteCoverageReport(Data));
	TestTrue(TEXT("behavior_tree_graph_get_view"), ExecSmokeCommand(this, ServerScope, TEXT("ai-bt-view-get"), TEXT("behavior_tree_graph_get_view"), FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *BehaviorTreePath), Data));
	TestTrue(TEXT("behavior_tree_graph_set_view"), ExecSmokeCommand(this, ServerScope, TEXT("ai-bt-view-set"), TEXT("behavior_tree_graph_set_view"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"view_x\":25,\"view_y\":50,\"zoom\":0.7}"), *BehaviorTreePath), Data));

	TestTrue(TEXT("state_tree_create"), ExecSmokeCommand(this, ServerScope, TEXT("ai-st-create"), TEXT("state_tree_create"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"schema_class\":\"/Script/GameplayStateTreeModule.StateTreeComponentSchema\",\"root_state_name\":\"Root\",\"save_after_create\":false}"), *StateTreePath), Data));
	TestTrue(TEXT("state_tree_get_info"), ExecSmokeCommand(this, ServerScope, TEXT("ai-st-info"), TEXT("state_tree_get_info"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_states\":true}"), *StateTreePath), Data));
	TestTrue(TEXT("state_tree_export_folder"), ExecSmokeCommand(this, ServerScope, TEXT("ai-st-export"), TEXT("state_tree_export_folder"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"folder_path\":\"%s\",\"clean_output_dir\":true}"), *StateTreePath, *StateTreeFolder), Data));
	TestTrue(TEXT("state_tree_export_folder returns coverage_report"), HasCompleteCoverageReport(Data));
	TestTrue(TEXT("state_tree_export_folder writes schema"), IFileManager::Get().FileExists(*FPaths::Combine(StateTreeFolder, TEXT("schema.json"))));
	TestTrue(TEXT("state_tree_export_folder writes transitions"), IFileManager::Get().FileExists(*FPaths::Combine(StateTreeFolder, TEXT("transitions.json"))));
	TestTrue(TEXT("state_tree_export_folder writes tasks"), IFileManager::Get().FileExists(*FPaths::Combine(StateTreeFolder, TEXT("tasks.json"))));
	TestTrue(TEXT("state_tree_export_folder writes conditions"), IFileManager::Get().FileExists(*FPaths::Combine(StateTreeFolder, TEXT("conditions.json"))));
	TestTrue(TEXT("state_tree_validate_folder"), ExecSmokeCommand(this, ServerScope, TEXT("ai-st-validate"), TEXT("state_tree_validate_folder"), FString::Printf(TEXT("{\"folder_path\":\"%s\"}"), *StateTreeFolder), Data));
	TestTrue(TEXT("state_tree_validate_folder returns coverage_report"), HasCompleteCoverageReport(Data));
	TestTrue(TEXT("state_tree_apply_folder"), ExecSmokeCommand(this, ServerScope, TEXT("ai-st-apply"), TEXT("state_tree_apply_folder"), FString::Printf(TEXT("{\"folder_path\":\"%s\",\"asset_path\":\"%s\",\"allow_destructive\":true,\"save_after_apply\":false}"), *StateTreeFolder, *StateTreePath), Data));
	TestTrue(TEXT("state_tree_apply_folder returns coverage_report"), HasCompleteCoverageReport(Data));

	TestTrue(TEXT("eqs_create"), ExecSmokeCommand(this, ServerScope, TEXT("ai-eqs-create"), TEXT("eqs_create"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"template\":\"simple_grid\",\"save_after_create\":false}"), *EqsPath), Data));
	TestTrue(TEXT("eqs_get_info"), ExecSmokeCommand(this, ServerScope, TEXT("ai-eqs-info"), TEXT("eqs_get_info"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"include_tests\":true}"), *EqsPath), Data));
	TestTrue(TEXT("eqs_export_folder"), ExecSmokeCommand(this, ServerScope, TEXT("ai-eqs-export"), TEXT("eqs_export_folder"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"folder_path\":\"%s\",\"clean_output_dir\":true}"), *EqsPath, *EqsFolder), Data));
	TestTrue(TEXT("eqs_export_folder returns coverage_report"), HasCompleteCoverageReport(Data));
	TestTrue(TEXT("eqs_export_folder writes generators"), IFileManager::Get().FileExists(*FPaths::Combine(EqsFolder, TEXT("generators.json"))));
	TestTrue(TEXT("eqs_export_folder writes tests"), IFileManager::Get().FileExists(*FPaths::Combine(EqsFolder, TEXT("tests.json"))));
	TestTrue(TEXT("eqs_export_folder writes contexts"), IFileManager::Get().FileExists(*FPaths::Combine(EqsFolder, TEXT("contexts.json"))));
	TestTrue(TEXT("eqs_validate_folder"), ExecSmokeCommand(this, ServerScope, TEXT("ai-eqs-validate"), TEXT("eqs_validate_folder"), FString::Printf(TEXT("{\"folder_path\":\"%s\"}"), *EqsFolder), Data));
	TestTrue(TEXT("eqs_validate_folder returns coverage_report"), HasCompleteCoverageReport(Data));
	TestTrue(TEXT("eqs_apply_folder"), ExecSmokeCommand(this, ServerScope, TEXT("ai-eqs-apply"), TEXT("eqs_apply_folder"), FString::Printf(TEXT("{\"folder_path\":\"%s\",\"asset_path\":\"%s\",\"allow_destructive\":true,\"save_after_apply\":false}"), *EqsFolder, *EqsPath), Data));
	TestTrue(TEXT("eqs_apply_folder returns coverage_report"), HasCompleteCoverageReport(Data));
	TestTrue(TEXT("eqs_run_query"), ExecSmokeCommand(this, ServerScope, TEXT("ai-eqs-run"), TEXT("eqs_run_query"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"run_mode\":\"all_matching\",\"max_results\":4}"), *EqsPath), Data));
	TestTrue(TEXT("eqs_debug_snapshot"), ExecSmokeCommand(this, ServerScope, TEXT("ai-eqs-debug"), TEXT("eqs_debug_snapshot"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"run_mode\":\"single\",\"max_results\":1}"), *EqsPath), Data));

	AAIController* Controller = World->SpawnActor<AAIController>(AAIController::StaticClass(), FVector(4200.0, 0.0, 150.0), FRotator::ZeroRotator);
	TestNotNull(TEXT("AI controller actor"), Controller);
	if (Controller)
	{
		Controller->SetActorLabel(TEXT("UAI_AIBehaviorSmoke_Controller"));
		UAIPerceptionComponent* Perception = NewObject<UAIPerceptionComponent>(Controller, TEXT("UAI_PerceptionSmoke"), RF_Transactional);
		Controller->AddOwnedComponent(Perception);
		Perception->RegisterComponent();
		if (UBehaviorTree* BehaviorTree = LoadObject<UBehaviorTree>(nullptr, *ToObjectPathForSmoke(BehaviorTreePath)))
		{
			Controller->RunBehaviorTree(BehaviorTree);
		}
	}
	TestTrue(TEXT("ai_perception_get_component_info"), ExecSmokeCommand(this, ServerScope, TEXT("ai-perception-info"), TEXT("ai_perception_get_component_info"), TEXT("{\"actor\":\"UAI_AIBehaviorSmoke_Controller\"}"), Data));
	TestTrue(TEXT("ai_perception_validate_setup"), ExecSmokeCommand(this, ServerScope, TEXT("ai-perception-validate"), TEXT("ai_perception_validate_setup"), TEXT("{\"actor\":\"UAI_AIBehaviorSmoke_Controller\"}"), Data));
	TestTrue(TEXT("ai_perception_runtime_snapshot"), ExecSmokeCommand(this, ServerScope, TEXT("ai-perception-snapshot"), TEXT("ai_perception_runtime_snapshot"), TEXT("{\"controller\":\"UAI_AIBehaviorSmoke_Controller\"}"), Data));
	TestTrue(TEXT("ai_perception_runtime_probe"), ExecSmokeCommand(this, ServerScope, TEXT("ai-perception-probe"), TEXT("ai_perception_runtime_probe"), TEXT("{\"controller\":\"UAI_AIBehaviorSmoke_Controller\",\"expected_sensed\":false}"), Data));
	TestTrue(TEXT("ai_perception_runtime_probe exposes samples and sensed flag"), Data.IsValid() && Data->HasTypedField<EJson::Array>(TEXT("samples")) && Data->HasField(TEXT("successfully_sensed")));
	TestTrue(TEXT("behavior_tree_runtime_snapshot"), ExecSmokeCommand(this, ServerScope, TEXT("ai-bt-runtime"), TEXT("behavior_tree_runtime_snapshot"), TEXT("{\"controller\":\"UAI_AIBehaviorSmoke_Controller\",\"include_blackboard\":true}"), Data));
	TestTrue(TEXT("behavior_tree_runtime_snapshot exposes runtime arrays"), Data.IsValid() && Data->HasTypedField<EJson::Array>(TEXT("active_path")) && Data->HasTypedField<EJson::Array>(TEXT("service_status")));

	AActor* StateActor = SpawnSmokeActor(World, TEXT("UAI_AIBehaviorSmoke_StateTreeActor"), FVector(4300.0, 0.0, 150.0));
	if (StateActor)
	{
		UStateTreeComponent* Component = NewObject<UStateTreeComponent>(StateActor, TEXT("UAI_StateTreeSmoke"), RF_Transactional);
		StateActor->AddInstanceComponent(Component);
		Component->RegisterComponent();
		if (UStateTree* StateTree = LoadObject<UStateTree>(nullptr, *ToObjectPathForSmoke(StateTreePath)))
		{
			Component->SetStateTree(StateTree);
			Component->StartLogic();
		}
	}
	TestTrue(TEXT("state_tree_runtime_snapshot"), ExecSmokeCommand(this, ServerScope, TEXT("ai-st-runtime"), TEXT("state_tree_runtime_snapshot"), TEXT("{\"actor\":\"UAI_AIBehaviorSmoke_StateTreeActor\"}"), Data));
	TestTrue(TEXT("state_tree_runtime_snapshot exposes conservative arrays"), Data.IsValid() && Data->HasTypedField<EJson::Array>(TEXT("active_states")) && Data->HasTypedField<EJson::Array>(TEXT("running_tasks")));

	ANavMeshBoundsVolume* NavBounds = World->SpawnActor<ANavMeshBoundsVolume>(ANavMeshBoundsVolume::StaticClass(), FVector(4250.0, 0.0, 0.0), FRotator::ZeroRotator);
	if (NavBounds)
	{
		NavBounds->SetActorLabel(TEXT("UAI_AIBehaviorSmoke_NavBounds"));
		NavBounds->SetActorScale3D(FVector(8.0, 8.0, 2.0));
	}
	TestTrue(TEXT("navigation_get_info"), ExecSmokeCommand(this, ServerScope, TEXT("ai-nav-info"), TEXT("navigation_get_info"), TEXT("{}"), Data));
	TestTrue(TEXT("navigation_export_config_json"), ExecSmokeCommand(this, ServerScope, TEXT("ai-nav-export"), TEXT("navigation_export_config_json"), FString::Printf(TEXT("{\"output_file\":\"%s\"}"), *NavJson), Data));
	TestTrue(TEXT("navigation_validate_level"), ExecSmokeCommand(this, ServerScope, TEXT("ai-nav-validate"), TEXT("navigation_validate_level"), TEXT("{}"), Data));
	TestTrue(TEXT("navigation_path_probe"), ExecSmokeCommand(this, ServerScope, TEXT("ai-nav-path-probe"), TEXT("navigation_path_probe"), TEXT("{\"start\":{\"x\":4200,\"y\":0,\"z\":150},\"targets\":[{\"id\":\"near\",\"location\":{\"x\":4300,\"y\":0,\"z\":150}}],\"accept_partial_path\":true}"), Data));
	TestTrue(TEXT("navigation_area_cost_probe"), ExecSmokeCommand(this, ServerScope, TEXT("ai-nav-area-cost"), TEXT("navigation_area_cost_probe"), TEXT("{\"run_path_probe\":false,\"expected_areas\":[\"/Script/NavigationSystem.NavArea_Default\"]}"), Data));
	TestTrue(TEXT("navigation_runtime_snapshot"), ExecSmokeCommand(this, ServerScope, TEXT("ai-nav-snapshot"), TEXT("navigation_runtime_snapshot"), TEXT("{}"), Data));

	TestTrue(TEXT("smart_object_definition_create"), ExecSmokeCommand(this, ServerScope, TEXT("ai-so-create"), TEXT("smart_object_definition_create"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"tags\":[\"AI.Activity.Test\"],\"slots\":[{\"name\":\"Use\",\"local_transform\":{\"translation\":{\"x\":0,\"y\":0,\"z\":0},\"rotation\":{\"pitch\":0,\"yaw\":0,\"roll\":0},\"scale\":{\"x\":1,\"y\":1,\"z\":1}},\"tags\":[\"AI.Slot.Test\"],\"enabled\":true,\"behavior_definitions\":[{\"class\":\"/Script/GameplayBehaviorSmartObjectsModule.GameplayBehaviorSmartObjectBehaviorDefinition\"}]}],\"save_after_create\":false}"), *SmartObjectPath), Data));
	TestTrue(TEXT("smart_object_definition_get_info"), ExecSmokeCommand(this, ServerScope, TEXT("ai-so-info"), TEXT("smart_object_definition_get_info"), FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SmartObjectPath), Data));
	TestTrue(TEXT("smart_object_definition_get_info exports tags and local_transform aliases"), Data.IsValid() && JsonObjectToStringForSmoke(Data).Contains(TEXT("\"local_transform\"")) && JsonObjectToStringForSmoke(Data).Contains(TEXT("\"tags\"")));
	const FString SmartObjectJson = FPaths::Combine(ExportRoot, TEXT("smart_object.json")).Replace(TEXT("\\"), TEXT("/"));
	TestTrue(TEXT("smart_object_definition_export_json"), ExecSmokeCommand(this, ServerScope, TEXT("ai-so-export"), TEXT("smart_object_definition_export_json"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"output_file\":\"%s\"}"), *SmartObjectPath, *SmartObjectJson), Data));
	TestTrue(TEXT("smart_object_definition_export_json returns coverage_report"), HasCompleteCoverageReport(Data));
	TestTrue(TEXT("smart_object_definition_validate_json"), ExecSmokeCommand(this, ServerScope, TEXT("ai-so-validate-json"), TEXT("smart_object_definition_validate_json"), FString::Printf(TEXT("{\"json_file\":\"%s\"}"), *SmartObjectJson), Data));
	TestTrue(TEXT("smart_object_definition_validate_json returns coverage_report"), HasCompleteCoverageReport(Data));
	TestTrue(TEXT("smart_object_definition_apply_json"), ExecSmokeCommand(this, ServerScope, TEXT("ai-so-apply"), TEXT("smart_object_definition_apply_json"), FString::Printf(TEXT("{\"json_file\":\"%s\",\"asset_path\":\"%s\",\"allow_destructive\":true,\"save_after_apply\":false}"), *SmartObjectJson, *SmartObjectPath), Data));
	TestTrue(TEXT("smart_object_definition_apply_json returns coverage_report"), HasCompleteCoverageReport(Data));

	const FVector SmartObjectLocation(4400.0, 0.0, 150.0);
	AActor* SmartObjectActor = SpawnSmokeActor(World, TEXT("UAI_AIBehaviorSmoke_SmartObject"), SmartObjectLocation);
	if (SmartObjectActor)
	{
		USmartObjectComponent* Component = NewObject<USmartObjectComponent>(SmartObjectActor, TEXT("UAI_SmartObjectSmoke"), RF_Transactional);
		SmartObjectActor->AddInstanceComponent(Component);
		SmartObjectActor->SetRootComponent(Component);
		SmartObjectActor->SetActorLocation(SmartObjectLocation, false, nullptr, ETeleportType::TeleportPhysics);
		Component->SetWorldLocation(SmartObjectLocation);
		Component->UpdateComponentToWorld();
		if (USmartObjectDefinition* Definition = LoadObject<USmartObjectDefinition>(nullptr, *ToObjectPathForSmoke(SmartObjectPath)))
		{
			Component->SetDefinition(Definition);
		}
		Component->RegisterComponent();
	}
	TestTrue(TEXT("smart_object_validate_setup"), ExecSmokeCommand(this, ServerScope, TEXT("ai-so-validate-setup"), TEXT("smart_object_validate_setup"), FString::Printf(TEXT("{\"definition_asset\":\"%s\",\"owner_actor_or_blueprint\":\"UAI_AIBehaviorSmoke_SmartObject\"}"), *SmartObjectPath), Data));
	if (TestTrue(TEXT("smart_object_find"), ExecSmokeCommand(this, ServerScope, TEXT("ai-so-find"), TEXT("smart_object_find"), TEXT("{\"query_bounds\":{\"center\":{\"x\":4400,\"y\":0,\"z\":150},\"extent\":{\"x\":500,\"y\":500,\"z\":500}},\"max_results\":4}"), Data)))
	{
		const int32 SmartObjectResultCount = Data.IsValid() && Data->HasTypedField<EJson::Number>(TEXT("result_count")) ? static_cast<int32>(Data->GetNumberField(TEXT("result_count"))) : 0;
		if (!TestTrue(TEXT("Smart Object find returned at least one result"), SmartObjectResultCount > 0))
		{
			AddError(JsonObjectToStringForSmoke(Data).Left(2048));
		}
	}
	TestTrue(TEXT("smart_object_claim"), ExecSmokeCommand(this, ServerScope, TEXT("ai-so-claim"), TEXT("smart_object_claim"), TEXT("{\"result_id\":\"result_0\"}"), Data));
	const FString ClaimHandle = Data.IsValid() && Data->HasField(TEXT("claim_handle")) ? Data->GetStringField(TEXT("claim_handle")) : FString();
	TestFalse(TEXT("Smart Object claim handle empty"), ClaimHandle.IsEmpty());
	if (!ClaimHandle.IsEmpty())
	{
		TestTrue(TEXT("smart_object_release"), ExecSmokeCommand(this, ServerScope, TEXT("ai-so-release"), TEXT("smart_object_release"), FString::Printf(TEXT("{\"claim_handle\":\"%s\"}"), *ClaimHandle), Data));
	}
	TestTrue(TEXT("smart_object_runtime_snapshot"), ExecSmokeCommand(this, ServerScope, TEXT("ai-so-snapshot"), TEXT("smart_object_runtime_snapshot"), TEXT("{}"), Data));
	TestTrue(TEXT("smart_object_runtime_probe"), ExecSmokeCommand(this, ServerScope, TEXT("ai-so-probe"), TEXT("smart_object_runtime_probe"), TEXT("{\"query_bounds\":{\"center\":{\"x\":4400,\"y\":0,\"z\":150},\"extent\":{\"x\":500,\"y\":500,\"z\":500}},\"expect_claimable\":true}"), Data));
	TestTrue(TEXT("smart_object_runtime_probe exposes find claim navigation cleanup fields"), Data.IsValid() && Data->HasField(TEXT("find_result")) && Data->HasField(TEXT("claim_result")) && Data->HasField(TEXT("navigation_result")) && Data->HasTypedField<EJson::Array>(TEXT("cleanup_results")));

	TestTrue(TEXT("ai_behavior_stack_export_folder"), ExecSmokeCommand(this, ServerScope, TEXT("ai-stack-export"), TEXT("ai_behavior_stack_export_folder"), FString::Printf(TEXT("{\"folder_path\":\"%s\",\"name\":\"AIBehaviorSmoke\",\"blackboard_asset\":\"%s\",\"behavior_tree_asset\":\"%s\",\"state_tree_asset\":\"%s\",\"eqs_asset\":\"%s\",\"smart_object_definition_asset\":\"%s\"}"), *StackFolder, *BlackboardPath, *BehaviorTreePath, *StateTreePath, *EqsPath, *SmartObjectPath), Data));
	TestTrue(TEXT("ai_behavior_stack_export_folder returns coverage_report"), HasCompleteCoverageReport(Data));
	TestTrue(TEXT("ai_behavior_stack_export_folder writes manifest"), IFileManager::Get().FileExists(*FPaths::Combine(StackFolder, TEXT("manifest.json"))));
	TestTrue(TEXT("ai_behavior_stack_export_folder writes blackboard"), IFileManager::Get().FileExists(*FPaths::Combine(StackFolder, TEXT("blackboard"), BlackboardName + TEXT(".json"))));
	TestTrue(TEXT("ai_behavior_stack_export_folder writes BT nodes"), IFileManager::Get().FileExists(*FPaths::Combine(StackFolder, TEXT("behavior_tree"), BehaviorTreeName, TEXT("nodes.json"))));
	TestTrue(TEXT("ai_behavior_stack_export_folder writes StateTree states"), IFileManager::Get().FileExists(*FPaths::Combine(StackFolder, TEXT("state_tree"), StateTreeName, TEXT("states.json"))));
	TestTrue(TEXT("ai_behavior_stack_export_folder writes EQS split tests"), IFileManager::Get().FileExists(*FPaths::Combine(StackFolder, TEXT("eqs"), EqsName, TEXT("tests.json"))));
	TestTrue(TEXT("ai_behavior_stack_export_folder writes Smart Object definition"), IFileManager::Get().FileExists(*FPaths::Combine(StackFolder, TEXT("smart_objects"), SmartObjectName + TEXT(".json"))));
	TestTrue(TEXT("ai_behavior_stack_export_folder writes validation checks"), IFileManager::Get().FileExists(*FPaths::Combine(StackFolder, TEXT("validation/checks.json"))));
	TestTrue(TEXT("ai_behavior_stack_validate_folder"), ExecSmokeCommand(this, ServerScope, TEXT("ai-stack-validate"), TEXT("ai_behavior_stack_validate_folder"), FString::Printf(TEXT("{\"folder_path\":\"%s\"}"), *StackFolder), Data));
	TestTrue(TEXT("ai_behavior_stack_validate_folder returns coverage_report"), HasCompleteCoverageReport(Data));
	TestTrue(TEXT("ai_behavior_stack_runtime_probe"), ExecSmokeCommand(this, ServerScope, TEXT("ai-stack-runtime"), TEXT("ai_behavior_stack_runtime_probe"), TEXT("{\"controller\":\"UAI_AIBehaviorSmoke_Controller\",\"actor\":\"UAI_AIBehaviorSmoke_StateTreeActor\",\"expected_blackboard_keys\":[\"TargetActor\"]}"), Data));
	TestTrue(TEXT("ai_behavior_stack_runtime_probe exposes samples and expectations"), Data.IsValid() && Data->HasTypedField<EJson::Array>(TEXT("samples")) && Data->HasTypedField<EJson::Array>(TEXT("expectation_results")) && Data->HasTypedField<EJson::Array>(TEXT("cleanup_results")));

	TestTrue(TEXT("bt_node_blueprint_get_info"), ExecSmokeCommand(this, ServerScope, TEXT("ai-bt-node-info"), TEXT("bt_node_blueprint_get_info"), TEXT("{\"asset_path\":\"/Script/AIModule.BTTask_BlueprintBase\"}"), Data));
	TestTrue(TEXT("bt_node_blueprint_get_info exposes class hierarchy"), Data.IsValid() && Data->HasField(TEXT("generated_class")) && Data->HasField(TEXT("parent_class")) && Data->HasField(TEXT("introspection_level")));
	TestTrue(TEXT("state_tree_node_blueprint_get_info"), ExecSmokeCommand(this, ServerScope, TEXT("ai-st-node-info"), TEXT("state_tree_node_blueprint_get_info"), TEXT("{\"asset_path\":\"/Script/StateTreeModule.StateTreeTaskBlueprintBase\"}"), Data));
	TestTrue(TEXT("state_tree_node_blueprint_get_info exposes class hierarchy"), Data.IsValid() && Data->HasField(TEXT("generated_class")) && Data->HasField(TEXT("parent_class")) && Data->HasField(TEXT("introspection_level")));

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceAIBehaviorStackFailureSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.AIBehaviorStackFailurePaths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceAIBehaviorStackFailureSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ExportRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAgentInterfaceAINegativeSmoke"), Suffix));
	IFileManager::Get().MakeDirectory(*ExportRoot, true);

	const FString BlackboardPath = MakeAutomationAssetPath(TEXT("BB_AINegative"));
	const FString BehaviorTreePath = MakeAutomationAssetPath(TEXT("BT_AINegative"));
	const FString StateTreePath = MakeAutomationAssetPath(TEXT("ST_AINegative"));
	const FString EqsPath = MakeAutomationAssetPath(TEXT("EQS_AINegative"));
	const FString SmartObjectPath = MakeAutomationAssetPath(TEXT("SO_AINegative"));

	TSharedPtr<FJsonObject> Data;
	const FString BadBlackboardJson = FPaths::Combine(ExportRoot, TEXT("blackboard_bad_syntax.json")).Replace(TEXT("\\"), TEXT("/"));
	SaveSmokeUtf8File(this, BadBlackboardJson, TEXT("{\"profile\":\"ue_agent_interface.blackboard.v1\","));
	TestTrue(TEXT("blackboard_validate_json rejects json syntax errors"), ExecSmokeCommandExpectFailure(this, ServerScope, TEXT("ai-neg-bb-bad-json"), TEXT("blackboard_validate_json"), FString::Printf(TEXT("{\"json_file\":\"%s\"}"), *BadBlackboardJson), TEXT("json_parse_failed"), Data));

	const FString UnknownBlackboardJson = FPaths::Combine(ExportRoot, TEXT("blackboard_unknown_field.json")).Replace(TEXT("\\"), TEXT("/"));
	SaveSmokeUtf8File(this, UnknownBlackboardJson, FString::Printf(TEXT("{\"profile\":\"ue_agent_interface.blackboard.v1\",\"schema_version\":1,\"asset_path\":\"%s\",\"keys\":[],\"unexpected\":true}"), *BlackboardPath));
	TestTrue(TEXT("blackboard_validate_json rejects strict unknown fields"), ExecSmokeCommandExpectFailure(this, ServerScope, TEXT("ai-neg-bb-unknown"), TEXT("blackboard_validate_json"), FString::Printf(TEXT("{\"json_file\":\"%s\",\"strict\":true}"), *UnknownBlackboardJson), TEXT("unknown_field"), Data));

	const FString DuplicateBlackboardJson = FPaths::Combine(ExportRoot, TEXT("blackboard_duplicate.json")).Replace(TEXT("\\"), TEXT("/"));
	SaveSmokeUtf8File(this, DuplicateBlackboardJson, FString::Printf(TEXT("{\"profile\":\"ue_agent_interface.blackboard.v1\",\"schema_version\":1,\"asset_path\":\"%s\",\"keys\":[{\"name\":\"Target\",\"type\":\"bool\"},{\"name\":\"Target\",\"type\":\"bool\"}]}"), *BlackboardPath));
	TestTrue(TEXT("blackboard_validate_json rejects duplicate keys"), ExecSmokeCommandExpectFailure(this, ServerScope, TEXT("ai-neg-bb-duplicate"), TEXT("blackboard_validate_json"), FString::Printf(TEXT("{\"json_file\":\"%s\"}"), *DuplicateBlackboardJson), TEXT("duplicate_id"), Data));

	TestTrue(TEXT("blackboard_create negative setup"), ExecSmokeCommand(this, ServerScope, TEXT("ai-neg-bb-create"), TEXT("blackboard_create"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"keys\":[{\"name\":\"Keep\",\"type\":\"bool\"},{\"name\":\"DeleteMe\",\"type\":\"bool\"}],\"save_after_create\":false}"), *BlackboardPath), Data));
	const FString DestructiveBlackboardJson = FPaths::Combine(ExportRoot, TEXT("blackboard_delete_missing.json")).Replace(TEXT("\\"), TEXT("/"));
	SaveSmokeUtf8File(this, DestructiveBlackboardJson, FString::Printf(TEXT("{\"profile\":\"ue_agent_interface.blackboard.v1\",\"schema_version\":1,\"asset_path\":\"%s\",\"delete_missing\":true,\"keys\":[{\"name\":\"Keep\",\"type\":\"bool\"}]}"), *BlackboardPath));
	TestTrue(TEXT("blackboard_apply_json requires destructive opt-in for deletion"), ExecSmokeCommandExpectFailure(this, ServerScope, TEXT("ai-neg-bb-delete"), TEXT("blackboard_apply_json"), FString::Printf(TEXT("{\"json_file\":\"%s\",\"asset_path\":\"%s\"}"), *DestructiveBlackboardJson, *BlackboardPath), TEXT("blackboard_key_delete_requires_opt_in"), Data));

	const FString DryRunBlackboardPath = MakeAutomationAssetPath(TEXT("BB_AINegativeDryRun"));
	const FString DryRunBlackboardJson = FPaths::Combine(ExportRoot, TEXT("blackboard_dry_run_missing.json")).Replace(TEXT("\\"), TEXT("/"));
	SaveSmokeUtf8File(this, DryRunBlackboardJson, FString::Printf(TEXT("{\"profile\":\"ue_agent_interface.blackboard.v1\",\"schema_version\":1,\"asset_path\":\"%s\",\"keys\":[{\"name\":\"WouldCreate\",\"type\":\"bool\"}]}"), *DryRunBlackboardPath));
	TestTrue(TEXT("blackboard_apply_json dry_run succeeds"), ExecSmokeCommand(this, ServerScope, TEXT("ai-neg-bb-dry-run"), TEXT("blackboard_apply_json"), FString::Printf(TEXT("{\"json_file\":\"%s\",\"asset_path\":\"%s\",\"create_if_missing\":true,\"dry_run\":true}"), *DryRunBlackboardJson, *DryRunBlackboardPath), Data));
	TestTrue(TEXT("blackboard_apply_json dry_run does not create asset"), ExecSmokeCommandExpectFailure(this, ServerScope, TEXT("ai-neg-bb-dry-run-readback"), TEXT("blackboard_get_info"), FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *DryRunBlackboardPath), TEXT("blackboard_not_found"), Data));
	TestTrue(TEXT("blackboard_apply_json validate_only succeeds"), ExecSmokeCommand(this, ServerScope, TEXT("ai-neg-bb-validate-only"), TEXT("blackboard_apply_json"), FString::Printf(TEXT("{\"json_file\":\"%s\",\"asset_path\":\"%s\",\"create_if_missing\":true,\"validate_only\":true}"), *DryRunBlackboardJson, *DryRunBlackboardPath), Data));
	TestTrue(TEXT("blackboard_apply_json validate_only does not create asset"), ExecSmokeCommandExpectFailure(this, ServerScope, TEXT("ai-neg-bb-validate-only-readback"), TEXT("blackboard_get_info"), FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *DryRunBlackboardPath), TEXT("blackboard_not_found"), Data));
	TestTrue(TEXT("blackboard_apply_json defaults to not creating missing assets"), ExecSmokeCommandExpectFailure(this, ServerScope, TEXT("ai-neg-bb-default-create-disabled"), TEXT("blackboard_apply_json"), FString::Printf(TEXT("{\"json_file\":\"%s\",\"asset_path\":\"%s\"}"), *DryRunBlackboardJson, *DryRunBlackboardPath), TEXT("blackboard_not_found"), Data));

	TestTrue(TEXT("behavior_tree_create negative setup"), ExecSmokeCommand(this, ServerScope, TEXT("ai-neg-bt-create"), TEXT("behavior_tree_create"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"blackboard_asset\":\"%s\",\"root_composite\":\"sequence\",\"save_after_create\":false}"), *BehaviorTreePath, *BlackboardPath), Data));
	const FString BehaviorTreeFolder = FPaths::Combine(ExportRoot, TEXT("bt_valid")).Replace(TEXT("\\"), TEXT("/"));
	TestTrue(TEXT("behavior_tree_export_folder negative setup"), ExecSmokeCommand(this, ServerScope, TEXT("ai-neg-bt-export"), TEXT("behavior_tree_export_folder"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"folder_path\":\"%s\",\"clean_output_dir\":true}"), *BehaviorTreePath, *BehaviorTreeFolder), Data));
	TestTrue(TEXT("behavior_tree_apply_folder requires destructive opt-in"), ExecSmokeCommandExpectFailure(this, ServerScope, TEXT("ai-neg-bt-destructive"), TEXT("behavior_tree_apply_folder"), FString::Printf(TEXT("{\"folder_path\":\"%s\",\"asset_path\":\"%s\"}"), *BehaviorTreeFolder, *BehaviorTreePath), TEXT("bt_destructive_change_requires_opt_in"), Data));
	const FString BadBehaviorTreeFolder = FPaths::Combine(ExportRoot, TEXT("bt_duplicate")).Replace(TEXT("\\"), TEXT("/"));
	SaveSmokeUtf8File(this, FPaths::Combine(BadBehaviorTreeFolder, TEXT("asset.json")), FString::Printf(TEXT("{\"profile\":\"ue_agent_interface.behavior_tree.v1\",\"schema_version\":1,\"asset_path\":\"%s\",\"blackboard_asset\":\"%s\"}"), *BehaviorTreePath, *BlackboardPath));
	SaveSmokeUtf8File(this, FPaths::Combine(BadBehaviorTreeFolder, TEXT("nodes.json")), TEXT("{\"root_id\":\"root\",\"nodes\":[{\"id\":\"root\",\"kind\":\"composite\",\"class\":\"sequence\"},{\"id\":\"root\",\"kind\":\"task\",\"class\":\"wait\"}]}"));
	TestTrue(TEXT("behavior_tree_validate_folder rejects duplicate node ids"), ExecSmokeCommandExpectFailure(this, ServerScope, TEXT("ai-neg-bt-duplicate"), TEXT("behavior_tree_validate_folder"), FString::Printf(TEXT("{\"folder_path\":\"%s\"}"), *BadBehaviorTreeFolder), TEXT("duplicate_id"), Data));
	const FString DryRunBehaviorTreePath = MakeAutomationAssetPath(TEXT("BT_AINegativeDryRun"));
	TestTrue(TEXT("behavior_tree_apply_folder dry_run succeeds"), ExecSmokeCommand(this, ServerScope, TEXT("ai-neg-bt-dry-run"), TEXT("behavior_tree_apply_folder"), FString::Printf(TEXT("{\"folder_path\":\"%s\",\"asset_path\":\"%s\",\"create_if_missing\":true,\"dry_run\":true}"), *BehaviorTreeFolder, *DryRunBehaviorTreePath), Data));
	TestTrue(TEXT("behavior_tree_apply_folder dry_run does not create asset"), ExecSmokeCommandExpectFailure(this, ServerScope, TEXT("ai-neg-bt-dry-run-readback"), TEXT("behavior_tree_get_info"), FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *DryRunBehaviorTreePath), TEXT("behavior_tree_not_found"), Data));
	TestTrue(TEXT("behavior_tree_apply_folder validate_only succeeds"), ExecSmokeCommand(this, ServerScope, TEXT("ai-neg-bt-validate-only"), TEXT("behavior_tree_apply_folder"), FString::Printf(TEXT("{\"folder_path\":\"%s\",\"asset_path\":\"%s\",\"create_if_missing\":true,\"validate_only\":true}"), *BehaviorTreeFolder, *DryRunBehaviorTreePath), Data));
	TestTrue(TEXT("behavior_tree_apply_folder validate_only does not create asset"), ExecSmokeCommandExpectFailure(this, ServerScope, TEXT("ai-neg-bt-validate-only-readback"), TEXT("behavior_tree_get_info"), FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *DryRunBehaviorTreePath), TEXT("behavior_tree_not_found"), Data));
	TestTrue(TEXT("behavior_tree_apply_folder defaults to not creating missing assets"), ExecSmokeCommandExpectFailure(this, ServerScope, TEXT("ai-neg-bt-default-create-disabled"), TEXT("behavior_tree_apply_folder"), FString::Printf(TEXT("{\"folder_path\":\"%s\",\"asset_path\":\"%s\"}"), *BehaviorTreeFolder, *DryRunBehaviorTreePath), TEXT("behavior_tree_not_found"), Data));

	TestTrue(TEXT("state_tree_create negative setup"), ExecSmokeCommand(this, ServerScope, TEXT("ai-neg-st-create"), TEXT("state_tree_create"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"schema_class\":\"/Script/GameplayStateTreeModule.StateTreeComponentSchema\",\"root_state_name\":\"Root\",\"save_after_create\":false}"), *StateTreePath), Data));
	TestTrue(TEXT("state_tree_create rejects invalid schema class"), ExecSmokeCommandExpectFailure(this, ServerScope, TEXT("ai-neg-st-schema"), TEXT("state_tree_create"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"schema_class\":\"/Script/StateTreeModule.DoesNotExist\"}"), *MakeAutomationAssetPath(TEXT("ST_AINegativeBadSchema"))), TEXT("invalid_state_tree_schema_class"), Data));
	const FString StateTreeFolder = FPaths::Combine(ExportRoot, TEXT("st_valid")).Replace(TEXT("\\"), TEXT("/"));
	TestTrue(TEXT("state_tree_export_folder negative setup"), ExecSmokeCommand(this, ServerScope, TEXT("ai-neg-st-export"), TEXT("state_tree_export_folder"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"folder_path\":\"%s\",\"clean_output_dir\":true}"), *StateTreePath, *StateTreeFolder), Data));
	TestTrue(TEXT("state_tree_apply_folder requires destructive opt-in"), ExecSmokeCommandExpectFailure(this, ServerScope, TEXT("ai-neg-st-destructive"), TEXT("state_tree_apply_folder"), FString::Printf(TEXT("{\"folder_path\":\"%s\",\"asset_path\":\"%s\"}"), *StateTreeFolder, *StateTreePath), TEXT("state_tree_rebuild_requires_allow_destructive"), Data));
	const FString BadStateTreeFolder = FPaths::Combine(ExportRoot, TEXT("st_duplicate")).Replace(TEXT("\\"), TEXT("/"));
	SaveSmokeUtf8File(this, FPaths::Combine(BadStateTreeFolder, TEXT("asset.json")), TEXT("{\"profile\":\"ue_agent_interface.state_tree.v1\",\"schema_version\":1,\"schema_class\":\"/Script/GameplayStateTreeModule.StateTreeComponentSchema\"}"));
	SaveSmokeUtf8File(this, FPaths::Combine(BadStateTreeFolder, TEXT("states.json")), TEXT("{\"states\":[{\"id\":\"root\",\"name\":\"Root\"},{\"id\":\"root\",\"name\":\"Other\"}]}"));
	TestTrue(TEXT("state_tree_validate_folder rejects duplicate state ids"), ExecSmokeCommandExpectFailure(this, ServerScope, TEXT("ai-neg-st-duplicate"), TEXT("state_tree_validate_folder"), FString::Printf(TEXT("{\"folder_path\":\"%s\"}"), *BadStateTreeFolder), TEXT("duplicate_id"), Data));
	const FString DryRunStateTreePath = MakeAutomationAssetPath(TEXT("ST_AINegativeDryRun"));
	TestTrue(TEXT("state_tree_apply_folder dry_run succeeds"), ExecSmokeCommand(this, ServerScope, TEXT("ai-neg-st-dry-run"), TEXT("state_tree_apply_folder"), FString::Printf(TEXT("{\"folder_path\":\"%s\",\"asset_path\":\"%s\",\"create_if_missing\":true,\"dry_run\":true}"), *StateTreeFolder, *DryRunStateTreePath), Data));
	TestTrue(TEXT("state_tree_apply_folder dry_run does not create asset"), ExecSmokeCommandExpectFailure(this, ServerScope, TEXT("ai-neg-st-dry-run-readback"), TEXT("state_tree_get_info"), FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *DryRunStateTreePath), TEXT("state_tree_not_found"), Data));
	TestTrue(TEXT("state_tree_apply_folder validate_only succeeds"), ExecSmokeCommand(this, ServerScope, TEXT("ai-neg-st-validate-only"), TEXT("state_tree_apply_folder"), FString::Printf(TEXT("{\"folder_path\":\"%s\",\"asset_path\":\"%s\",\"create_if_missing\":true,\"validate_only\":true}"), *StateTreeFolder, *DryRunStateTreePath), Data));
	TestTrue(TEXT("state_tree_apply_folder validate_only does not create asset"), ExecSmokeCommandExpectFailure(this, ServerScope, TEXT("ai-neg-st-validate-only-readback"), TEXT("state_tree_get_info"), FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *DryRunStateTreePath), TEXT("state_tree_not_found"), Data));

	TestTrue(TEXT("eqs_create negative setup"), ExecSmokeCommand(this, ServerScope, TEXT("ai-neg-eqs-create"), TEXT("eqs_create"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"template\":\"simple_grid\",\"save_after_create\":false}"), *EqsPath), Data));
	const FString EqsFolder = FPaths::Combine(ExportRoot, TEXT("eqs_valid")).Replace(TEXT("\\"), TEXT("/"));
	TestTrue(TEXT("eqs_export_folder negative setup"), ExecSmokeCommand(this, ServerScope, TEXT("ai-neg-eqs-export"), TEXT("eqs_export_folder"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"folder_path\":\"%s\",\"clean_output_dir\":true}"), *EqsPath, *EqsFolder), Data));
	TestTrue(TEXT("eqs_apply_folder requires destructive opt-in"), ExecSmokeCommandExpectFailure(this, ServerScope, TEXT("ai-neg-eqs-destructive"), TEXT("eqs_apply_folder"), FString::Printf(TEXT("{\"folder_path\":\"%s\",\"asset_path\":\"%s\"}"), *EqsFolder, *EqsPath), TEXT("eqs_rebuild_requires_allow_destructive"), Data));
	const FString BadEqsFolder = FPaths::Combine(ExportRoot, TEXT("eqs_bad_generator")).Replace(TEXT("\\"), TEXT("/"));
	SaveSmokeUtf8File(this, FPaths::Combine(BadEqsFolder, TEXT("asset.json")), FString::Printf(TEXT("{\"profile\":\"ue_agent_interface.eqs.v1\",\"schema_version\":1,\"asset_path\":\"%s\"}"), *EqsPath));
	SaveSmokeUtf8File(this, FPaths::Combine(BadEqsFolder, TEXT("options.json")), TEXT("{\"options\":[{\"generator_class\":\"/Script/AIModule.DoesNotExist\"}]}"));
	TestTrue(TEXT("eqs_validate_folder rejects invalid generator class"), ExecSmokeCommandExpectFailure(this, ServerScope, TEXT("ai-neg-eqs-generator"), TEXT("eqs_validate_folder"), FString::Printf(TEXT("{\"folder_path\":\"%s\"}"), *BadEqsFolder), TEXT("eqs_generator_class_not_found"), Data));
	const FString DryRunEqsPath = MakeAutomationAssetPath(TEXT("EQS_AINegativeDryRun"));
	TestTrue(TEXT("eqs_apply_folder dry_run succeeds"), ExecSmokeCommand(this, ServerScope, TEXT("ai-neg-eqs-dry-run"), TEXT("eqs_apply_folder"), FString::Printf(TEXT("{\"folder_path\":\"%s\",\"asset_path\":\"%s\",\"create_if_missing\":true,\"dry_run\":true}"), *EqsFolder, *DryRunEqsPath), Data));
	TestTrue(TEXT("eqs_apply_folder dry_run does not create asset"), ExecSmokeCommandExpectFailure(this, ServerScope, TEXT("ai-neg-eqs-dry-run-readback"), TEXT("eqs_get_info"), FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *DryRunEqsPath), TEXT("eqs_query_not_found"), Data));
	TestTrue(TEXT("eqs_apply_folder validate_only succeeds"), ExecSmokeCommand(this, ServerScope, TEXT("ai-neg-eqs-validate-only"), TEXT("eqs_apply_folder"), FString::Printf(TEXT("{\"folder_path\":\"%s\",\"asset_path\":\"%s\",\"create_if_missing\":true,\"validate_only\":true}"), *EqsFolder, *DryRunEqsPath), Data));
	TestTrue(TEXT("eqs_apply_folder validate_only does not create asset"), ExecSmokeCommandExpectFailure(this, ServerScope, TEXT("ai-neg-eqs-validate-only-readback"), TEXT("eqs_get_info"), FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *DryRunEqsPath), TEXT("eqs_query_not_found"), Data));

	TestTrue(TEXT("smart_object_definition_create negative setup"), ExecSmokeCommand(this, ServerScope, TEXT("ai-neg-so-create"), TEXT("smart_object_definition_create"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"slots\":[{\"name\":\"Use\",\"offset\":{\"x\":0,\"y\":0,\"z\":0},\"rotation\":{\"pitch\":0,\"yaw\":0,\"roll\":0},\"enabled\":true,\"behavior_definitions\":[{\"class\":\"/Script/GameplayBehaviorSmartObjectsModule.GameplayBehaviorSmartObjectBehaviorDefinition\"}]}],\"save_after_create\":false}"), *SmartObjectPath), Data));
	const FString SmartObjectJson = FPaths::Combine(ExportRoot, TEXT("smart_object_valid.json")).Replace(TEXT("\\"), TEXT("/"));
	TestTrue(TEXT("smart_object_definition_export_json negative setup"), ExecSmokeCommand(this, ServerScope, TEXT("ai-neg-so-export"), TEXT("smart_object_definition_export_json"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"output_file\":\"%s\"}"), *SmartObjectPath, *SmartObjectJson), Data));
	TestTrue(TEXT("smart_object_definition_apply_json requires destructive opt-in"), ExecSmokeCommandExpectFailure(this, ServerScope, TEXT("ai-neg-so-destructive"), TEXT("smart_object_definition_apply_json"), FString::Printf(TEXT("{\"json_file\":\"%s\",\"asset_path\":\"%s\"}"), *SmartObjectJson, *SmartObjectPath), TEXT("smart_object_slot_rebuild_requires_allow_destructive"), Data));
	const FString DryRunSmartObjectPath = MakeAutomationAssetPath(TEXT("SO_AINegativeDryRun"));
	TestTrue(TEXT("smart_object_definition_apply_json dry_run succeeds"), ExecSmokeCommand(this, ServerScope, TEXT("ai-neg-so-dry-run"), TEXT("smart_object_definition_apply_json"), FString::Printf(TEXT("{\"json_file\":\"%s\",\"asset_path\":\"%s\",\"create_if_missing\":true,\"dry_run\":true}"), *SmartObjectJson, *DryRunSmartObjectPath), Data));
	TestTrue(TEXT("smart_object_definition_apply_json dry_run does not create asset"), ExecSmokeCommandExpectFailure(this, ServerScope, TEXT("ai-neg-so-dry-run-readback"), TEXT("smart_object_definition_get_info"), FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *DryRunSmartObjectPath), TEXT("smart_object_definition_not_found"), Data));
	TestTrue(TEXT("smart_object_definition_apply_json validate_only succeeds"), ExecSmokeCommand(this, ServerScope, TEXT("ai-neg-so-validate-only"), TEXT("smart_object_definition_apply_json"), FString::Printf(TEXT("{\"json_file\":\"%s\",\"asset_path\":\"%s\",\"create_if_missing\":true,\"validate_only\":true}"), *SmartObjectJson, *DryRunSmartObjectPath), Data));
	TestTrue(TEXT("smart_object_definition_apply_json validate_only does not create asset"), ExecSmokeCommandExpectFailure(this, ServerScope, TEXT("ai-neg-so-validate-only-readback"), TEXT("smart_object_definition_get_info"), FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *DryRunSmartObjectPath), TEXT("smart_object_definition_not_found"), Data));
	const FString BadSmartObjectJson = FPaths::Combine(ExportRoot, TEXT("smart_object_bad_behavior.json")).Replace(TEXT("\\"), TEXT("/"));
	SaveSmokeUtf8File(this, BadSmartObjectJson, FString::Printf(TEXT("{\"profile\":\"ue_agent_interface.smart_object_definition.v1\",\"schema_version\":1,\"asset_path\":\"%s\",\"slots\":[{\"name\":\"Use\",\"behavior_definitions\":[{\"class\":\"/Script/Engine.Actor\"}]}]}"), *SmartObjectPath));
	TestTrue(TEXT("smart_object_definition_validate_json rejects invalid behavior definition class"), ExecSmokeCommandExpectFailure(this, ServerScope, TEXT("ai-neg-so-behavior"), TEXT("smart_object_definition_validate_json"), FString::Printf(TEXT("{\"json_file\":\"%s\"}"), *BadSmartObjectJson), TEXT("smart_object_behavior_definition_invalid"), Data));

	const FString BadStackFolder = FPaths::Combine(ExportRoot, TEXT("stack_missing_asset")).Replace(TEXT("\\"), TEXT("/"));
	SaveSmokeUtf8File(this, FPaths::Combine(BadStackFolder, TEXT("manifest.json")), TEXT("{\"profile\":\"ue_agent_interface.ai_behavior_stack.v1\",\"schema_version\":1,\"blackboard_asset\":\"/Game/__UeAgentInterfaceSmoke/MissingBlackboard\"}"));
	TestTrue(TEXT("ai_behavior_stack_validate_folder rejects missing assets"), ExecSmokeCommandExpectFailure(this, ServerScope, TEXT("ai-neg-stack-missing"), TEXT("ai_behavior_stack_validate_folder"), FString::Printf(TEXT("{\"folder_path\":\"%s\"}"), *BadStackFolder), TEXT("asset_missing_or_type_mismatch"), Data));

	return !HasAnyErrors();
}

#endif
