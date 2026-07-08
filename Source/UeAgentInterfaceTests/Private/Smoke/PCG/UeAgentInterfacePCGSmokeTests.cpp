#if WITH_DEV_AUTOMATION_TESTS

#include "../UeAgentSmokeTestSharedIncludes.inl"

#include "Smoke/PCG/UeAgentInterfacePCGSmokeBlueprintElement.h"

#include "Components/BoxComponent.h"
#include "GameFramework/Actor.h"
#include "Misc/PackageName.h"
#include "Utilities/UeAgentDetailsAuthoring.h"

namespace
{
	static bool ExecPCGSmokeCommand(
		FAutomationTestBase* Test,
		const FScopedUeAgentHttpServer& ServerScope,
		const FString& RequestId,
		const FString& Command,
		const FString& ParamsJson,
		const int32 ExpectedStatusCode,
		TSharedPtr<FJsonObject>& OutRoot,
		TSharedPtr<FJsonObject>& OutData,
		const double TimeoutSeconds = 45.0)
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
		Test->TestTrue(
			FString::Printf(TEXT("%s ok flag matches status"), *RequestId),
			OutRoot.IsValid() && OutRoot->GetBoolField(TEXT("ok")) == bExpectOk);

		OutData.Reset();
		const TSharedPtr<FJsonObject>* DataObject = nullptr;
		if (OutRoot.IsValid() && OutRoot->TryGetObjectField(TEXT("data"), DataObject) && DataObject && DataObject->IsValid())
		{
			OutData = *DataObject;
		}
		if (!OutData.IsValid() && !bExpectOk)
		{
			OutData = MakeShared<FJsonObject>();
		}
		return bRequestOk && Result.StatusCode == ExpectedStatusCode && OutRoot.IsValid() && (!bExpectOk || OutData.IsValid());
	}

	static bool WritePCGSmokeJsonFile(FAutomationTestBase* Test, const FString& FilePath, const FString& JsonText)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
		const bool bSaved = FFileHelper::SaveStringToFile(JsonText, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		Test->TestTrue(FString::Printf(TEXT("write json %s"), *FilePath), bSaved);
		return bSaved;
	}

	static bool WritePCGSmokeDetailsPackage(
		FAutomationTestBase* Test,
		const FString& DetailsRoot,
		const FString& OwnerKind,
		const FString& OwnerId,
		const FString& FieldsJson)
	{
		const FString IndexJson = FString::Printf(
			TEXT("{\"schema\":\"uai.details_index.v1\",\"profile\":\"authoring\",\"owner_ref\":{\"owner_kind\":\"%s\",\"owner_id\":\"%s\"},\"groups\":[{\"file\":\"Settings.json\"}]}"),
			*OwnerKind,
			*OwnerId);
		const FString GroupJson = FString::Printf(
			TEXT("{\"schema\":\"uai.details_group.v1\",\"owner_ref\":{\"owner_kind\":\"%s\",\"owner_id\":\"%s\"},\"group_key\":\"Smoke\",\"display_name\":\"Smoke\",\"fields\":%s}"),
			*OwnerKind,
			*OwnerId,
			*FieldsJson);
		return
			WritePCGSmokeJsonFile(Test, FPaths::Combine(DetailsRoot, TEXT("index.json")), IndexJson) &&
			WritePCGSmokeJsonFile(Test, FPaths::Combine(DetailsRoot, TEXT("Settings.json")), GroupJson);
	}

	static bool LoadPCGSmokeJsonFile(FAutomationTestBase* Test, const FString& FilePath, TSharedPtr<FJsonObject>& OutJson)
	{
		FString JsonText;
		const bool bLoaded = FFileHelper::LoadFileToString(JsonText, *FilePath);
		Test->TestTrue(FString::Printf(TEXT("load json %s"), *FilePath), bLoaded);
		if (!bLoaded)
		{
			return false;
		}
		const bool bParsed = ParseJson(JsonText, OutJson);
		Test->TestTrue(FString::Printf(TEXT("parse json %s"), *FilePath), bParsed);
		return bParsed;
	}

	static bool PCGSmokeJsonFileTreeContainsText(const FString& RootFolder, const FString& Needle, FString& OutFile)
	{
		TArray<FString> JsonFiles;
		IFileManager::Get().FindFilesRecursive(JsonFiles, *RootFolder, TEXT("*.json"), true, false);
		for (const FString& JsonFile : JsonFiles)
		{
			FString JsonText;
			if (FFileHelper::LoadFileToString(JsonText, *JsonFile) && JsonText.Contains(Needle))
			{
				OutFile = JsonFile;
				return true;
			}
		}
		OutFile.Reset();
		return false;
	}

	static bool PCGSmokeObjectHasFieldRecursive(const TSharedPtr<FJsonObject>& Object, const FString& FieldName)
	{
		if (!Object.IsValid())
		{
			return false;
		}
		if (Object->HasField(FieldName))
		{
			return true;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
		{
			if (!Field.Value.IsValid())
			{
				continue;
			}
			if (Field.Value->Type == EJson::Object)
			{
				if (PCGSmokeObjectHasFieldRecursive(Field.Value->AsObject(), FieldName))
				{
					return true;
				}
			}
			else if (Field.Value->Type == EJson::Array)
			{
				for (const TSharedPtr<FJsonValue>& Child : Field.Value->AsArray())
				{
					if (Child.IsValid() && Child->Type == EJson::Object && PCGSmokeObjectHasFieldRecursive(Child->AsObject(), FieldName))
					{
						return true;
					}
				}
			}
		}
		return false;
	}

	static bool PCGSmokeDetailsRefHasReadableGroup(
		FAutomationTestBase* Test,
		const FString& EntryFilePath,
		const TSharedPtr<FJsonObject>& OwnerObject,
		const TCHAR* RefFieldName,
		const FString& ExpectedOwnerKind)
	{
		const TSharedPtr<FJsonObject>* DetailsRef = nullptr;
		if (!OwnerObject.IsValid() || !OwnerObject->TryGetObjectField(RefFieldName, DetailsRef) || !DetailsRef || !DetailsRef->IsValid())
		{
			return false;
		}

		Test->TestEqual(FString::Printf(TEXT("%s schema"), RefFieldName), (*DetailsRef)->GetStringField(TEXT("schema")), FString(TEXT("uai.details_ref.v1")));
		if (!ExpectedOwnerKind.IsEmpty())
		{
			Test->TestEqual(FString::Printf(TEXT("%s owner kind"), RefFieldName), (*DetailsRef)->GetStringField(TEXT("owner_kind")), ExpectedOwnerKind);
		}

		FString IndexFile;
		if (!(*DetailsRef)->TryGetStringField(TEXT("index_file"), IndexFile) || IndexFile.IsEmpty())
		{
			return false;
		}

		TSharedPtr<FJsonObject> IndexJson;
		const FString IndexPath = FPaths::Combine(FPaths::GetPath(EntryFilePath), IndexFile);
		if (!LoadPCGSmokeJsonFile(Test, IndexPath, IndexJson))
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Groups = nullptr;
		if (!IndexJson.IsValid() || !IndexJson->TryGetArrayField(TEXT("groups"), Groups) || !Groups || Groups->Num() <= 0)
		{
			return false;
		}

		const TSharedPtr<FJsonObject> GroupEntry = (*Groups)[0].IsValid() ? (*Groups)[0]->AsObject() : nullptr;
		FString GroupFile;
		if (!GroupEntry.IsValid() || !GroupEntry->TryGetStringField(TEXT("file"), GroupFile) || GroupFile.IsEmpty())
		{
			return false;
		}

		TSharedPtr<FJsonObject> GroupJson;
		const FString GroupPath = FPaths::Combine(FPaths::GetPath(IndexPath), GroupFile);
		if (!LoadPCGSmokeJsonFile(Test, GroupPath, GroupJson))
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
		if (!GroupJson.IsValid() || !GroupJson->TryGetArrayField(TEXT("fields"), Fields) || !Fields || Fields->Num() <= 0)
		{
			return false;
		}

		const TSharedPtr<FJsonObject> FirstField = (*Fields)[0].IsValid() ? (*Fields)[0]->AsObject() : nullptr;
		return FirstField.IsValid()
			&& FirstField->HasField(TEXT("value_text"))
			&& FirstField->HasTypedField<EJson::Array>(TEXT("operations"));
	}

	static FString MakePCGSmokeFolder(const FString& Name)
	{
		return ToSmokeJsonPath(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("UAIPCG"), Name));
	}

	static FString MakeObjectPathFromAssetPath(const FString& AssetPath)
	{
		return FString::Printf(TEXT("%s.%s"), *AssetPath, *FPackageName::GetLongPackageAssetName(AssetPath));
	}

	static TSharedPtr<FJsonObject> FindPCGSmokeNodeById(const TSharedPtr<FJsonObject>& Data, const FString& NodeId)
	{
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (!Data.IsValid() || !Data->TryGetArrayField(TEXT("nodes"), Nodes))
		{
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& NodeValue : *Nodes)
		{
			const TSharedPtr<FJsonObject>* NodeObject = nullptr;
			FString Id;
			if (NodeValue.IsValid() &&
				NodeValue->TryGetObject(NodeObject) &&
				NodeObject &&
				NodeObject->IsValid() &&
				(*NodeObject)->TryGetStringField(TEXT("id"), Id) &&
				Id == NodeId)
			{
				return *NodeObject;
			}
		}

		return nullptr;
	}

	static bool PCGSmokeNodeHasPinLabel(const TSharedPtr<FJsonObject>& Node, const TCHAR* PinArrayField, const FString& Label)
	{
		const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
		if (!Node.IsValid() || !Node->TryGetArrayField(PinArrayField, Pins))
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& PinValue : *Pins)
		{
			const TSharedPtr<FJsonObject>* PinObject = nullptr;
			FString PinLabel;
			if (PinValue.IsValid() &&
				PinValue->TryGetObject(PinObject) &&
				PinObject &&
				PinObject->IsValid() &&
				(*PinObject)->TryGetStringField(TEXT("label"), PinLabel) &&
				PinLabel == Label)
			{
				return true;
			}
		}

		return false;
	}

	static bool PCGSmokeNodeSettingPropertyContains(const TSharedPtr<FJsonObject>& Node, const FString& PropertyName, const FString& ExpectedValueText)
	{
		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (!Node.IsValid() || !Node->TryGetArrayField(TEXT("settings_properties"), Properties))
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& PropertyValue : *Properties)
		{
			const TSharedPtr<FJsonObject>* PropertyObject = nullptr;
			FString Name;
			FString ValueText;
			if (PropertyValue.IsValid() &&
				PropertyValue->TryGetObject(PropertyObject) &&
				PropertyObject &&
				PropertyObject->IsValid() &&
				(*PropertyObject)->TryGetStringField(TEXT("name"), Name) &&
				Name == PropertyName &&
				(*PropertyObject)->TryGetStringField(TEXT("value_text"), ValueText) &&
				ValueText.Contains(ExpectedValueText))
			{
				return true;
			}
		}

		return false;
	}

	static AActor* SpawnPCGSmokeActor(UWorld* World, const FString& Label)
	{
		if (!World)
		{
			return nullptr;
		}
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.ObjectFlags |= RF_Transactional;
		AActor* Actor = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
		if (Actor)
		{
			UBoxComponent* BoundsComponent = NewObject<UBoxComponent>(Actor, TEXT("PCGSmokeBounds"));
			BoundsComponent->SetBoxExtent(FVector(500.0, 500.0, 250.0));
			BoundsComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			BoundsComponent->SetHiddenInGame(true);
			Actor->SetRootComponent(BoundsComponent);
			Actor->AddInstanceComponent(BoundsComponent);
			BoundsComponent->RegisterComponent();
			Actor->SetActorLabel(Label);
		}
		return Actor;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfacePCGSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.PCG.CoreWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfacePCGSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString GraphPath = MakeAutomationAssetPath(TEXT("PCG_SmokeGraph"));
	const FString GraphInstancePath = MakeAutomationAssetPath(TEXT("PCGI_SmokeGraph"));
	const FString InvalidGraphInstancePath = MakeAutomationAssetPath(TEXT("PCGI_InvalidSource"));
	const FString DataAssetPath = MakeAutomationAssetPath(TEXT("PCGDA_Smoke"));
	const FString InputFolder = MakePCGSmokeFolder(TEXT("InputGraph"));
	const FString InvalidFolder = MakePCGSmokeFolder(TEXT("InvalidGraph"));
	const FString LegacyFolder = MakePCGSmokeFolder(TEXT("LegacyGraph"));
	const FString ExportFolder = MakePCGSmokeFolder(TEXT("ExportGraph"));
	const FString ActorLabel = FString::Printf(TEXT("UAI_PCG_Smoke_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().DeleteDirectory(*InputFolder, false, true);
	IFileManager::Get().DeleteDirectory(*InvalidFolder, false, true);
	IFileManager::Get().DeleteDirectory(*LegacyFolder, false, true);
	IFileManager::Get().DeleteDirectory(*ExportFolder, false, true);

	const FString AssetJsonPath = FPaths::Combine(InputFolder, TEXT("asset.json"));
	const FString GraphJsonPath = FPaths::Combine(InputFolder, TEXT("graph.json"));
	const FString AssetJson = FString::Printf(
		TEXT("{\"schema\":\"ue_agent_interface.pcg.graph_folder.v1\",\"asset_path\":\"%s\",\"asset_class\":\"/Script/PCG.PCGGraph\"}"),
		*GraphPath);
	const FString GraphJson =
		TEXT("{\"schema\":\"ue_agent_interface.pcg.graph_folder.v1\",")
		TEXT("\"parameters\":{\"parameters\":[{\"name\":\"SmokeDensity\",\"type\":\"Float\",\"value_text\":\"0.25\"}]},")
		TEXT("\"nodes\":[")
		TEXT("{\"id\":\"__input__\",\"role\":\"input\"},")
		TEXT("{\"id\":\"smoke_param_get\",\"settings_class\":\"/Script/PCG.PCGUserParameterGetSettings\",\"authored_title\":\"SmokeParamGet\",\"pos_x\":100,\"pos_y\":-220,")
		TEXT("\"output_pins\":[{\"label\":\"SmokeDensity\",\"allowed_types\":\"Param\"}]},")
		TEXT("{\"id\":\"smoke_grid\",\"settings_class\":\"/Script/PCG.PCGCreatePointsGridSettings\",\"authored_title\":\"SmokeGrid\",\"pos_x\":100,\"pos_y\":0,")
		TEXT("\"settings_details\":{\"schema\":\"uai.details_ref.v1\",\"index_file\":\"details/smoke_grid/index.json\",\"ref_field\":\"settings_details\",\"owner_kind\":\"pcg_node_settings\",\"owner_id\":\"smoke_grid\"}},")
		TEXT("{\"id\":\"smoke_blueprint\",\"settings_class\":\"/Script/PCG.PCGBlueprintSettings\",\"authored_title\":\"SmokeBlueprint\",\"pos_x\":100,\"pos_y\":180},")
		TEXT("{\"id\":\"smoke_add_tag\",\"settings_class\":\"/Script/PCG.PCGAddTagSettings\",\"authored_title\":\"SmokeAddTag\",\"pos_x\":400,\"pos_y\":0},")
		TEXT("{\"id\":\"smoke_spawner\",\"settings_class\":\"/Script/PCG.PCGStaticMeshSpawnerSettings\",\"authored_title\":\"SmokeSpawner\",\"pos_x\":700,\"pos_y\":0},")
		TEXT("{\"id\":\"__output__\",\"role\":\"output\"}],")
		TEXT("\"edges\":[{\"from\":\"smoke_grid\",\"from_pin\":\"Out\",\"to\":\"smoke_add_tag\",\"to_pin\":\"In\"},{\"from\":\"smoke_add_tag\",\"from_pin\":\"Out\",\"to\":\"__output__\",\"to_pin\":\"\"}],\"node_count\":7,\"edge_count\":2}");
	if (!WritePCGSmokeJsonFile(this, AssetJsonPath, AssetJson) ||
		!WritePCGSmokeJsonFile(this, GraphJsonPath, GraphJson) ||
		!WritePCGSmokeDetailsPackage(
			this,
			FPaths::Combine(InputFolder, TEXT("details"), TEXT("smoke_grid")),
			TEXT("pcg_node_settings"),
			TEXT("smoke_grid"),
			TEXT("[{\"property_path\":\"GridExtents\",\"value_text\":\"(X=100.000000,Y=100.000000,Z=50.000000)\",\"operations\":[\"set\"]},{\"property_path\":\"CellSize\",\"value_text\":\"(X=100.000000,Y=100.000000,Z=100.000000)\",\"operations\":[\"set\"]}]")))
	{
		return false;
	}

	const FString InvalidAssetJsonPath = FPaths::Combine(InvalidFolder, TEXT("asset.json"));
	const FString InvalidGraphJsonPath = FPaths::Combine(InvalidFolder, TEXT("graph.json"));
	const FString InvalidGraphJson =
		TEXT("{\"schema\":\"ue_agent_interface.pcg.graph_folder.v1\",")
		TEXT("\"nodes\":[")
		TEXT("{\"id\":\"__input__\",\"role\":\"input\"},")
		TEXT("{\"id\":\"bad_node\",\"settings_class\":\"/Script/PCG.PCGNoSuchSettings\"},")
		TEXT("{\"id\":\"__output__\",\"role\":\"output\"}],")
		TEXT("\"edges\":[],\"node_count\":3,\"edge_count\":0}");
	if (!WritePCGSmokeJsonFile(this, InvalidAssetJsonPath, AssetJson) ||
		!WritePCGSmokeJsonFile(this, InvalidGraphJsonPath, InvalidGraphJson))
	{
		return false;
	}

	const FString LegacyAssetJsonPath = FPaths::Combine(LegacyFolder, TEXT("asset.json"));
	const FString LegacyGraphJsonPath = FPaths::Combine(LegacyFolder, TEXT("graph.json"));
	const FString LegacyGraphJson =
		TEXT("{\"schema\":\"ue_agent_interface.pcg.graph_folder.v1\",")
		TEXT("\"nodes\":[")
		TEXT("{\"id\":\"__input__\",\"role\":\"input\"},")
		TEXT("{\"id\":\"legacy_grid\",\"settings_class\":\"/Script/PCG.PCGCreatePointsGridSettings\",\"settings_properties\":[{\"name\":\"CellSize\",\"value_text\":\"(X=25.000000,Y=25.000000,Z=25.000000)\"}]},")
		TEXT("{\"id\":\"__output__\",\"role\":\"output\"}],")
		TEXT("\"edges\":[],\"node_count\":3,\"edge_count\":0}");
	if (!WritePCGSmokeJsonFile(this, LegacyAssetJsonPath, AssetJson) ||
		!WritePCGSmokeJsonFile(this, LegacyGraphJsonPath, LegacyGraphJson))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	TSharedPtr<FJsonObject> Data;
	TestTrue(TEXT("pcg_capabilities_query succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-capabilities"), TEXT("pcg_capabilities_query"), TEXT("{}"), 200, Root, Data));
	TestTrue(TEXT("pcg_node_catalog_export succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-catalog"), TEXT("pcg_node_catalog_export"), TEXT("{}"), 200, Root, Data));
	TestTrue(TEXT("catalog has PCG settings"), Data.IsValid() && Data->GetIntegerField(TEXT("node_count")) > 0);

	const FString FolderParams = FString::Printf(TEXT("{\"folder_path\":\"%s\",\"asset_path\":\"%s\",\"save\":false}"), *InputFolder, *GraphPath);
	TestTrue(TEXT("pcg_graph_validate_folder succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-validate-folder"), TEXT("pcg_graph_validate_folder"), FolderParams, 200, Root, Data));
	TestTrue(TEXT("pcg_graph_plan_folder succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-plan-folder"), TEXT("pcg_graph_plan_folder"), FolderParams, 200, Root, Data));
	TestTrue(TEXT("pcg_graph_apply_folder succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-apply-folder"), TEXT("pcg_graph_apply_folder"), FolderParams, 200, Root, Data));
	TestTrue(TEXT("pcg_graph_compile succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-compile"), TEXT("pcg_graph_compile"), FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *GraphPath), 200, Root, Data));
	TestTrue(TEXT("pcg_graph_snapshot succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-snapshot"), TEXT("pcg_graph_snapshot"), FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *GraphPath), 200, Root, Data));
	const int32 InitialGraphNodeCount = Data.IsValid() ? Data->GetIntegerField(TEXT("node_count")) : -1;
	TestTrue(TEXT("pcg_graph_apply_folder restores Get Graph Parameter output pins from folder JSON"), PCGSmokeNodeHasPinLabel(FindPCGSmokeNodeById(Data, TEXT("SmokeParamGet")), TEXT("output_pins"), TEXT("SmokeDensity")));
	TestTrue(TEXT("pcg_graph_apply_folder applies SmokeGrid CellSize from grouped details"), PCGSmokeNodeSettingPropertyContains(FindPCGSmokeNodeById(Data, TEXT("SmokeGrid")), TEXT("CellSize"), TEXT("X=100.000000")));
	const FString LegacyFolderParams = FString::Printf(TEXT("{\"folder_path\":\"%s\",\"asset_path\":\"%s\",\"save\":false}"), *LegacyFolder, *GraphPath);
	TestTrue(TEXT("pcg_graph_apply_folder rejects legacy settings_properties"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-apply-legacy-settings-properties"), TEXT("pcg_graph_apply_folder"), LegacyFolderParams, 400, Root, Data));
	TestTrue(TEXT("pcg legacy rejection data returned"), Data.IsValid());
	if (Data.IsValid())
	{
		TestEqual(TEXT("pcg legacy rejection schema"), Data->GetStringField(TEXT("schema")), FString(TEXT("uai.legacy_rejection.v1")));
		TestEqual(TEXT("pcg legacy rejection error code"), Data->GetStringField(TEXT("error_code")), FString(TEXT("legacy_settings_properties_not_supported")));
		TestEqual(TEXT("pcg legacy rejection surface"), Data->GetStringField(TEXT("legacy_surface")), FString(TEXT("settings_properties[]")));
		TestEqual(TEXT("pcg legacy rejection owner kind"), Data->GetStringField(TEXT("owner_kind")), FString(TEXT("pcg_node_settings")));
		TestFalse(TEXT("pcg legacy rejection not dropped"), Data->GetBoolField(TEXT("dropped")));
	}
	const FString GraphLayoutParams = FString::Printf(
		TEXT("{\"asset_path\":\"%s\",\"layout_options\":{\"horizontal_spacing\":360,\"vertical_spacing\":96,\"node_padding\":16,\"insert_reroute_nodes\":false,\"replace_existing_reroute_nodes\":false},\"save\":false}"),
		*GraphPath);
	TestTrue(TEXT("pcg_graph_layout succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-graph-layout"), TEXT("pcg_graph_layout"), GraphLayoutParams, 200, Root, Data, 60.0));
	TestEqual(TEXT("pcg_graph_layout delegates to node_graph_layout"), Data.IsValid() ? Data->GetStringField(TEXT("delegated_command")) : FString(), FString(TEXT("node_graph_layout")));
	TestEqual(TEXT("pcg_graph_layout uses shared Blueprint layout core"), Data.IsValid() ? Data->GetStringField(TEXT("pcg_graph_layout_strategy")) : FString(), FString(TEXT("shared_node_graph_blueprint_layout_core")));
	TestTrue(TEXT("pcg_graph_layout uses real offscreen geometry"), Data.IsValid() && Data->GetBoolField(TEXT("geometry_used_offscreen_graph_editor")));
	TestEqual(TEXT("pcg_graph_layout keeps PCG node count"), Data.IsValid() ? Data->GetIntegerField(TEXT("node_count")) : -1, InitialGraphNodeCount);
	const FString InvalidFolderParams = FString::Printf(TEXT("{\"folder_path\":\"%s\",\"asset_path\":\"%s\",\"save\":false}"), *InvalidFolder, *GraphPath);
	TestTrue(TEXT("pcg_graph_apply_folder rejects invalid folder before mutating asset"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-apply-invalid-folder"), TEXT("pcg_graph_apply_folder"), InvalidFolderParams, 400, Root, Data));
	TestTrue(TEXT("invalid folder apply reports errors"), Data.IsValid() && Data->GetIntegerField(TEXT("error_count")) > 0);
	TestTrue(TEXT("pcg_graph_snapshot after invalid folder succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-snapshot-after-invalid"), TEXT("pcg_graph_snapshot"), FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *GraphPath), 200, Root, Data));
	TestEqual(TEXT("invalid folder apply keeps existing graph node count"), Data.IsValid() ? Data->GetIntegerField(TEXT("node_count")) : -1, InitialGraphNodeCount);
	TestTrue(TEXT("pcg_asset_info succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-asset-info"), TEXT("pcg_asset_info"), FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *GraphPath), 200, Root, Data));
	TestTrue(TEXT("pcg_graph_diff_folder succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-diff-folder"), TEXT("pcg_graph_diff_folder"), FolderParams, 200, Root, Data));
	TestTrue(TEXT("pcg_graph_export_folder succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-export-folder"), TEXT("pcg_graph_export_folder"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"folder_path\":\"%s\"}"), *GraphPath, *ExportFolder), 200, Root, Data));
	TestEqual(TEXT("pcg_graph_export_folder includes input/output nodes"), Data.IsValid() ? Data->GetIntegerField(TEXT("node_count")) : -1, 7);
	TestEqual(TEXT("pcg_graph_export_folder preserves edge to output"), Data.IsValid() ? Data->GetIntegerField(TEXT("edge_count")) : -1, 2);
	TSharedPtr<FJsonObject> ExportedPCGGraphJson;
	const FString ExportedGraphJsonPath = FPaths::Combine(ExportFolder, TEXT("graph.json"));
	TestTrue(TEXT("pcg_graph_export_folder graph.json authoring load"), LoadPCGSmokeJsonFile(this, ExportedGraphJsonPath, ExportedPCGGraphJson));
	FString LegacySettingsPropertiesFile;
	TestFalse(
		TEXT("pcg_graph_export_folder omits settings_properties from default json files"),
		PCGSmokeJsonFileTreeContainsText(ExportFolder, TEXT("\"settings_properties\""), LegacySettingsPropertiesFile));
	if (!LegacySettingsPropertiesFile.IsEmpty())
	{
		AddError(FString::Printf(TEXT("settings_properties found in %s"), *LegacySettingsPropertiesFile));
	}
	FString ExportedGraphText;
	TestTrue(TEXT("pcg_graph_export_folder graph.json text load"), FFileHelper::LoadFileToString(ExportedGraphText, *ExportedGraphJsonPath));
	if (!ExportedGraphText.IsEmpty())
	{
		TestFalse(TEXT("pcg graph settings_details refs stay compact"), ExportedGraphText.Contains(TEXT("\"settings_details\": {\n")));
		TestFalse(TEXT("pcg graph graph_settings details ref stays compact"), ExportedGraphText.Contains(TEXT("\"details\": {\n")));
	}
	if (ExportedPCGGraphJson.IsValid())
	{
		TestFalse(TEXT("pcg graph default export has no legacy settings_properties fields"), PCGSmokeObjectHasFieldRecursive(ExportedPCGGraphJson, TEXT("settings_properties")));
		const TSharedPtr<FJsonObject>* GraphSettings = nullptr;
		TestTrue(TEXT("pcg graph settings object exists"), ExportedPCGGraphJson->TryGetObjectField(TEXT("graph_settings"), GraphSettings) && GraphSettings && GraphSettings->IsValid());
		if (GraphSettings && GraphSettings->IsValid())
		{
			TestFalse(TEXT("pcg graph settings omits legacy properties"), (*GraphSettings)->HasField(TEXT("properties")));
			TestTrue(TEXT("pcg graph settings details readable"), PCGSmokeDetailsRefHasReadableGroup(this, ExportedGraphJsonPath, *GraphSettings, TEXT("details"), TEXT("pcg_graph_settings")));
		}

		const TArray<TSharedPtr<FJsonValue>>* ExportedNodes = nullptr;
		TestTrue(TEXT("pcg graph export nodes array exists"), ExportedPCGGraphJson->TryGetArrayField(TEXT("nodes"), ExportedNodes) && ExportedNodes);
		bool bFoundNodeAuthoringMetadata = false;
		bool bFoundGroupedSettingsDetails = false;
		if (ExportedNodes)
		{
			for (const TSharedPtr<FJsonValue>& NodeValue : *ExportedNodes)
			{
				const TSharedPtr<FJsonObject>* NodeObj = nullptr;
				if (NodeValue.IsValid() && NodeValue->TryGetObject(NodeObj) && NodeObj && NodeObj->IsValid())
				{
					bFoundGroupedSettingsDetails = bFoundGroupedSettingsDetails || (*NodeObj)->HasTypedField<EJson::Object>(TEXT("settings_details"));
				}
			}
		}
		TestTrue(TEXT("pcg graph settings details grouped"), bFoundGroupedSettingsDetails);
		const TSharedPtr<FJsonObject> SmokeGridNode = FindPCGSmokeNodeById(ExportedPCGGraphJson, TEXT("SmokeGrid"));
		TestTrue(TEXT("pcg graph SmokeGrid settings details readable"), PCGSmokeDetailsRefHasReadableGroup(this, ExportedGraphJsonPath, SmokeGridNode, TEXT("settings_details"), TEXT("pcg_node_settings")));

		FString RehydrateError;
		TestTrue(
			TEXT("pcg graph grouped details rehydrate"),
			UeAgentDetailsAuthoring::RehydrateGroupedDetailsInObjectTree(ExportedGraphJsonPath, ExportedPCGGraphJson, RehydrateError));

		bool bFoundPropertyAuthoringMetadata = false;
		ExportedNodes = nullptr;
		ExportedPCGGraphJson->TryGetArrayField(TEXT("nodes"), ExportedNodes);
		if (ExportedNodes)
		{
			for (const TSharedPtr<FJsonValue>& NodeValue : *ExportedNodes)
			{
				const TSharedPtr<FJsonObject>* NodeObj = nullptr;
				if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObj) || !NodeObj || !NodeObj->IsValid())
				{
					continue;
				}
				bFoundNodeAuthoringMetadata = bFoundNodeAuthoringMetadata
					|| ((*NodeObj)->HasTypedField<EJson::Object>(TEXT("selector")) && (*NodeObj)->HasTypedField<EJson::Array>(TEXT("operations")));
				const TArray<TSharedPtr<FJsonValue>>* SettingsProperties = nullptr;
				if ((*NodeObj)->TryGetArrayField(TEXT("settings_properties"), SettingsProperties) && SettingsProperties)
				{
					for (const TSharedPtr<FJsonValue>& PropertyValue : *SettingsProperties)
					{
						const TSharedPtr<FJsonObject>* PropertyObj = nullptr;
						if (PropertyValue.IsValid() && PropertyValue->TryGetObject(PropertyObj) && PropertyObj && PropertyObj->IsValid())
						{
							const bool bHasPropertyIdentity =
								(*PropertyObj)->HasField(TEXT("property_path")) ||
								(*PropertyObj)->HasTypedField<EJson::Object>(TEXT("selector"));
							bFoundPropertyAuthoringMetadata = bFoundPropertyAuthoringMetadata
								|| (bHasPropertyIdentity && (*PropertyObj)->HasTypedField<EJson::Array>(TEXT("operations")));
						}
					}
				}
			}
		}
		TestTrue(TEXT("pcg graph node exposes authoring metadata"), bFoundNodeAuthoringMetadata);
		TestTrue(TEXT("pcg graph settings property exposes authoring metadata"), bFoundPropertyAuthoringMetadata);
	}

	TestTrue(TEXT("pcg_graph_parameters_query succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-params-query"), TEXT("pcg_graph_parameters_query"), FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *GraphPath), 200, Root, Data));
	TestEqual(TEXT("pcg_graph_apply_folder imports graph parameter bag"), Data.IsValid() ? Data->GetIntegerField(TEXT("parameter_count")) : -1, 1);
	TestTrue(TEXT("pcg_graph_parameters_apply_json succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-params-apply"), TEXT("pcg_graph_parameters_apply_json"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"save\":false,\"parameters\":[{\"name\":\"SmokeDensity\",\"type\":\"Float\",\"value_text\":\"0.25\"}]}"), *GraphPath), 200, Root, Data));
	const int32 ParameterCountBeforeInvalidApply = Data.IsValid() ? Data->GetIntegerField(TEXT("parameter_count")) : -1;
	TestTrue(TEXT("pcg_graph_parameters_apply_json rejects bad parameter before mutating"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-params-apply-invalid"), TEXT("pcg_graph_parameters_apply_json"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"save\":false,\"parameters\":[{\"type\":\"Float\",\"value_text\":\"1.0\"}]}"), *GraphPath), 400, Root, Data));
	TestTrue(TEXT("bad graph parameter reports errors"), Data.IsValid() && Data->GetIntegerField(TEXT("error_count")) > 0);
	TestTrue(TEXT("pcg_graph_parameters_query after bad parameter succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-params-query-after-invalid"), TEXT("pcg_graph_parameters_query"), FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *GraphPath), 200, Root, Data));
	TestEqual(TEXT("bad graph parameter did not change parameter count"), Data.IsValid() ? Data->GetIntegerField(TEXT("parameter_count")) : -1, ParameterCountBeforeInvalidApply);
	TestTrue(TEXT("pcg_graph_instance_json succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-instance"), TEXT("pcg_graph_instance_json"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"create\":true,\"graph\":\"%s\",\"save\":false,\"parameters\":[{\"name\":\"SmokeDensity\",\"type\":\"Float\",\"value_text\":\"0.5\"}]}"), *GraphInstancePath, *GraphPath), 200, Root, Data));
	TestTrue(TEXT("pcg_graph_instance_json rejects unknown parent parameter"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-instance-unknown-param"), TEXT("pcg_graph_instance_json"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"create\":false,\"save\":false,\"parameters\":[{\"name\":\"UnknownDensity\",\"type\":\"Float\",\"value_text\":\"1.0\"}]}"), *GraphInstancePath), 400, Root, Data));
	TestTrue(TEXT("unknown GraphInstance parameter reports error_count"), Data.IsValid() && Data->GetIntegerField(TEXT("error_count")) > 0);
	TestTrue(TEXT("pcg_graph_instance_json rejects invalid source before creating asset"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-instance-invalid-source"), TEXT("pcg_graph_instance_json"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"create\":true,\"graph\":\"/Game/UAI_Automation/PCG_NoSuchGraph\",\"save\":false}"), *InvalidGraphInstancePath), 400, Root, Data));
	TestNull(TEXT("invalid source GraphInstance asset was not created"), FindObject<UObject>(nullptr, *MakeObjectPathFromAssetPath(InvalidGraphInstancePath)));

	TestTrue(TEXT("pcg_gpu_capabilities_query succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-gpu"), TEXT("pcg_gpu_capabilities_query"), TEXT("{}"), 200, Root, Data));
	TestTrue(TEXT("pcg_compute_graph_validate succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-compute"), TEXT("pcg_compute_graph_validate"), FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *GraphPath), 200, Root, Data));
	TestTrue(TEXT("pcg_hlsl_validate succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-hlsl"), TEXT("pcg_hlsl_validate"), FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *GraphPath), 200, Root, Data));

	TestTrue(TEXT("pcg_data_asset_apply_json succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-data-asset-apply"), TEXT("pcg_data_asset_apply_json"), FString::Printf(TEXT("{\"asset_path\":\"%s\",\"create\":true,\"save\":false,\"name\":\"SmokeData\"}"), *DataAssetPath), 200, Root, Data));
	TestTrue(TEXT("pcg_data_asset_export_json succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-data-asset-export"), TEXT("pcg_data_asset_export_json"), FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *DataAssetPath), 200, Root, Data));

	TestTrue(TEXT("pcg_world_actor_query succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-world-query"), TEXT("pcg_world_actor_query"), TEXT("{}"), 200, Root, Data));
	const int32 WorldActorCountBeforeInvalidApply = Data.IsValid() ? Data->GetIntegerField(TEXT("world_actor_count")) : -1;
	TestTrue(TEXT("pcg_world_actor_apply_json rejects invalid grid size"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-world-apply-invalid-grid"), TEXT("pcg_world_actor_apply_json"), TEXT("{\"partition_grid_size\":-1}"), 400, Root, Data));
	TestTrue(TEXT("pcg_world_actor_query after invalid grid succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-world-query-after-invalid-grid"), TEXT("pcg_world_actor_query"), TEXT("{}"), 200, Root, Data));
	TestEqual(TEXT("invalid grid apply does not create PCGWorldActor"), Data.IsValid() ? Data->GetIntegerField(TEXT("world_actor_count")) : -1, WorldActorCountBeforeInvalidApply);
	TestTrue(TEXT("pcg_world_actor_apply_json succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-world-apply"), TEXT("pcg_world_actor_apply_json"), TEXT("{\"partition_grid_size\":25600,\"enable_world_partition_generation_sources\":true}"), 200, Root, Data));
	TestTrue(TEXT("pcg_partition_probe succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-partition-probe"), TEXT("pcg_partition_probe"), TEXT("{}"), 200, Root, Data));

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	AActor* SmokeActor = SpawnPCGSmokeActor(World, ActorLabel);
	TestNotNull(TEXT("spawn smoke actor"), SmokeActor);
	if (!SmokeActor)
	{
		return false;
	}

	TestTrue(TEXT("pcg_component_apply_json rejects invalid graph before creating component"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-component-invalid-graph"), TEXT("pcg_component_apply_json"), FString::Printf(TEXT("{\"actor\":\"%s\",\"component\":\"PCGInvalidGraphComponent\",\"graph\":\"/Game/UAI_Automation/PCG_NoSuchGraph\"}"), *ActorLabel), 400, Root, Data));
	TestTrue(TEXT("invalid graph component query fails"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-component-invalid-graph-query"), TEXT("pcg_component_query"), FString::Printf(TEXT("{\"actor\":\"%s\",\"component\":\"PCGInvalidGraphComponent\"}"), *ActorLabel), 400, Root, Data));
	TestTrue(TEXT("pcg_component_apply_json rejects invalid property before creating component"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-component-invalid-property"), TEXT("pcg_component_apply_json"), FString::Printf(TEXT("{\"actor\":\"%s\",\"component\":\"PCGInvalidPropertyComponent\",\"properties\":[{\"name\":\"NoSuchProperty\",\"value_text\":\"1\"}]}"), *ActorLabel), 400, Root, Data));
	TestTrue(TEXT("invalid property component query fails"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-component-invalid-property-query"), TEXT("pcg_component_query"), FString::Printf(TEXT("{\"actor\":\"%s\",\"component\":\"PCGInvalidPropertyComponent\"}"), *ActorLabel), 400, Root, Data));
	TestTrue(TEXT("pcg_component_apply_json rejects invalid generation trigger before creating component"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-component-invalid-trigger"), TEXT("pcg_component_apply_json"), FString::Printf(TEXT("{\"actor\":\"%s\",\"component\":\"PCGInvalidTriggerComponent\",\"generation_trigger\":\"DefinitelyNotATrigger\"}"), *ActorLabel), 400, Root, Data));
	TestTrue(TEXT("invalid trigger component query fails"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-component-invalid-trigger-query"), TEXT("pcg_component_query"), FString::Printf(TEXT("{\"actor\":\"%s\",\"component\":\"PCGInvalidTriggerComponent\"}"), *ActorLabel), 400, Root, Data));

	const FString ComponentParams = FString::Printf(TEXT("{\"actor\":\"%s\",\"component\":\"PCGSmokeComponent\",\"graph\":\"%s\",\"seed\":77,\"generation_trigger\":\"GenerateOnDemand\",\"partitioned\":false}"), *ActorLabel, *GraphPath);
	TestTrue(TEXT("pcg_component_apply_json succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-component-apply"), TEXT("pcg_component_apply_json"), ComponentParams, 200, Root, Data));
	TestTrue(TEXT("pcg_component_query succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-component-query"), TEXT("pcg_component_query"), FString::Printf(TEXT("{\"actor\":\"%s\",\"component\":\"PCGSmokeComponent\"}"), *ActorLabel), 200, Root, Data));
	TestTrue(TEXT("pcg_component_snapshot_json succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-component-snapshot"), TEXT("pcg_component_snapshot_json"), FString::Printf(TEXT("{\"actor\":\"%s\",\"component\":\"PCGSmokeComponent\"}"), *ActorLabel), 200, Root, Data));
	TestTrue(TEXT("pcg_runtime_generation_probe succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-runtime-probe"), TEXT("pcg_runtime_generation_probe"), FString::Printf(TEXT("{\"actor\":\"%s\",\"component\":\"PCGSmokeComponent\"}"), *ActorLabel), 200, Root, Data));
	TestTrue(TEXT("pcg_component_runtime_probe succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-component-runtime-probe"), TEXT("pcg_component_runtime_probe"), FString::Printf(TEXT("{\"actor\":\"%s\",\"component\":\"PCGSmokeComponent\"}"), *ActorLabel), 200, Root, Data));
	TestTrue(TEXT("pcg_generation_status succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-generation-status"), TEXT("pcg_generation_status"), FString::Printf(TEXT("{\"actor\":\"%s\",\"component\":\"PCGSmokeComponent\"}"), *ActorLabel), 200, Root, Data));
	TestTrue(TEXT("pcg_inspection_set succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-inspection-set"), TEXT("pcg_inspection_set"), FString::Printf(TEXT("{\"actor\":\"%s\",\"component\":\"PCGSmokeComponent\",\"enabled\":true,\"clear\":true}"), *ActorLabel), 200, Root, Data));
	TestTrue(TEXT("pcg_node_inspect succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-node-inspect"), TEXT("pcg_node_inspect"), FString::Printf(TEXT("{\"actor\":\"%s\",\"component\":\"PCGSmokeComponent\"}"), *ActorLabel), 200, Root, Data));
	TestTrue(TEXT("pcg_data_export_json succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-data-export"), TEXT("pcg_data_export_json"), FString::Printf(TEXT("{\"actor\":\"%s\",\"component\":\"PCGSmokeComponent\",\"max_points_per_data\":1}"), *ActorLabel), 200, Root, Data));
	TestTrue(TEXT("pcg_data_assert_json succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-data-assert"), TEXT("pcg_data_assert_json"), FString::Printf(TEXT("{\"actor\":\"%s\",\"component\":\"PCGSmokeComponent\",\"min_data_count\":0,\"min_total_point_count\":0}"), *ActorLabel), 200, Root, Data));
	TestTrue(TEXT("pcg_profile_report component succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-profile-component"), TEXT("pcg_profile_report"), FString::Printf(TEXT("{\"actor\":\"%s\",\"component\":\"PCGSmokeComponent\"}"), *ActorLabel), 200, Root, Data));
	TestTrue(TEXT("pcg_profile_report graph succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-profile-graph"), TEXT("pcg_profile_report"), FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *GraphPath), 200, Root, Data));
	TestTrue(TEXT("pcg_generate rejects invalid generation trigger"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-generate-invalid-trigger"), TEXT("pcg_generate"), FString::Printf(TEXT("{\"actor\":\"%s\",\"component\":\"PCGSmokeComponent\",\"generation_trigger\":\"DefinitelyNotATrigger\"}"), *ActorLabel), 400, Root, Data));
	TestTrue(TEXT("pcg_generate succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-generate"), TEXT("pcg_generate"), FString::Printf(TEXT("{\"actor\":\"%s\",\"component\":\"PCGSmokeComponent\",\"force\":true,\"generation_trigger\":\"GenerateOnDemand\"}"), *ActorLabel), 200, Root, Data));
	TestTrue(TEXT("pcg_wait succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-wait"), TEXT("pcg_wait"), FString::Printf(TEXT("{\"actor\":\"%s\",\"component\":\"PCGSmokeComponent\",\"timeout_seconds\":5}"), *ActorLabel), 200, Root, Data));
	TestTrue(TEXT("pcg_data_export_json after generate succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-data-export-generated"), TEXT("pcg_data_export_json"), FString::Printf(TEXT("{\"actor\":\"%s\",\"component\":\"PCGSmokeComponent\",\"max_points_per_data\":3}"), *ActorLabel), 200, Root, Data));
	TestTrue(TEXT("generated export has point data"), Data.IsValid() && Data->GetIntegerField(TEXT("point_data_count")) > 0);
	TestTrue(TEXT("generated export has points"), Data.IsValid() && Data->GetIntegerField(TEXT("total_point_count")) > 0);
	TestTrue(TEXT("pcg_data_assert_json after generate succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-data-assert-generated"), TEXT("pcg_data_assert_json"), FString::Printf(TEXT("{\"actor\":\"%s\",\"component\":\"PCGSmokeComponent\",\"min_data_count\":1,\"min_total_point_count\":1}"), *ActorLabel), 200, Root, Data));
	TestTrue(TEXT("pcg_cancel_generation succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-cancel"), TEXT("pcg_cancel_generation"), FString::Printf(TEXT("{\"actor\":\"%s\",\"component\":\"PCGSmokeComponent\"}"), *ActorLabel), 200, Root, Data));
	TestTrue(TEXT("pcg_cleanup succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-cleanup"), TEXT("pcg_cleanup"), FString::Printf(TEXT("{\"actor\":\"%s\",\"component\":\"PCGSmokeComponent\",\"remove_components\":true}"), *ActorLabel), 200, Root, Data));
	TestTrue(TEXT("pcg_wait after cleanup succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-wait-cleanup"), TEXT("pcg_wait"), FString::Printf(TEXT("{\"actor\":\"%s\",\"component\":\"PCGSmokeComponent\",\"timeout_seconds\":5}"), *ActorLabel), 200, Root, Data));
	TestTrue(TEXT("pcg_regenerate succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-regenerate"), TEXT("pcg_regenerate"), FString::Printf(TEXT("{\"actor\":\"%s\",\"component\":\"PCGSmokeComponent\",\"force\":true,\"generation_trigger\":\"GenerateOnDemand\"}"), *ActorLabel), 200, Root, Data));
	TestTrue(TEXT("pcg_wait after regenerate succeeds"), ExecPCGSmokeCommand(this, ServerScope, TEXT("smoke-pcg-wait-regenerate"), TEXT("pcg_wait"), FString::Printf(TEXT("{\"actor\":\"%s\",\"component\":\"PCGSmokeComponent\",\"timeout_seconds\":5}"), *ActorLabel), 200, Root, Data));

	if (World && SmokeActor)
	{
		World->DestroyActor(SmokeActor);
	}

	return !HasAnyErrors();
}

#endif
