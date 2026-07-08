#if WITH_DEV_AUTOMATION_TESTS

#include "UeAgentSmokeTestSharedIncludes.inl"

#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"

namespace
{
	static FString QuoteNodeCatalogOriginJson(const FString& Text)
	{
		FString Out = Text;
		Out.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Out.ReplaceInline(TEXT("\""), TEXT("\\\""));
		Out.ReplaceInline(TEXT("\n"), TEXT("\\n"));
		Out.ReplaceInline(TEXT("\r"), TEXT("\\r"));
		Out.ReplaceInline(TEXT("\t"), TEXT("\\t"));
		return FString::Printf(TEXT("\"%s\""), *Out);
	}

	static bool ExecNodeCatalogOriginCommand(
		FAutomationTestBase* Test,
		const FScopedUeAgentHttpServer& ServerScope,
		const FString& RequestId,
		const FString& Command,
		const FString& ParamsJson,
		TSharedPtr<FJsonObject>& OutData,
		const int32 ExpectedStatus = 200)
	{
		FHttpSmokeResult Result;
		const FString Body = MakeExecRequestBody(RequestId, Command, ParamsJson);
		if (!Test->TestTrue(Command + TEXT(" request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, Body, Result, 30.0)))
		{
			return false;
		}
		Test->TestEqual(Command + TEXT(" status"), Result.StatusCode, ExpectedStatus);

		TSharedPtr<FJsonObject> Root;
		if (!Test->TestTrue(Command + TEXT(" response parses"), ParseJson(Result.Body, Root)) || !Root.IsValid())
		{
			return false;
		}

		const bool bExpectedOk = ExpectedStatus == 200;
		Test->TestEqual(Command + TEXT(" ok state"), Root->GetBoolField(TEXT("ok")), bExpectedOk);

		const TSharedPtr<FJsonObject>* DataObj = nullptr;
		if (!Test->TestTrue(Command + TEXT(" contains data"), Root->TryGetObjectField(TEXT("data"), DataObj) && DataObj && DataObj->IsValid()))
		{
			return false;
		}

		OutData = *DataObj;
		return true;
	}

	static TSharedPtr<FJsonObject> FirstCatalogItem(const TSharedPtr<FJsonObject>& Data)
	{
		if (!Data.IsValid())
		{
			return nullptr;
		}

		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!Data->TryGetArrayField(TEXT("items"), Items) || !Items || Items->IsEmpty())
		{
			return nullptr;
		}
		return (*Items)[0]->AsObject();
	}

	static bool HasNonEmptyStringField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName)
	{
		FString Value;
		return Obj.IsValid() && Obj->TryGetStringField(FieldName, Value) && !Value.IsEmpty();
	}

	static bool HasNonEmptyArrayField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		return Obj.IsValid() && Obj->TryGetArrayField(FieldName, Values) && Values && !Values->IsEmpty();
	}

	static bool HasArrayField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		return Obj.IsValid() && Obj->TryGetArrayField(FieldName, Values) && Values;
	}

	static bool HasObjectField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName)
	{
		const TSharedPtr<FJsonObject>* Value = nullptr;
		return Obj.IsValid() && Obj->TryGetObjectField(FieldName, Value) && Value && Value->IsValid();
	}

	static bool GetJsonAuthoringSeed(
		FAutomationTestBase* Test,
		const FString& Label,
		const TSharedPtr<FJsonObject>& Item,
		TSharedPtr<FJsonObject>& OutAuthoring,
		TSharedPtr<FJsonObject>& OutSeed)
	{
		if (!Test->TestTrue(Label + TEXT(" item exists"), Item.IsValid()))
		{
			return false;
		}

		Test->TestTrue(Label + TEXT(" item has kind"), HasNonEmptyStringField(Item, TEXT("kind")));
		Test->TestTrue(Label + TEXT(" item has name"), HasNonEmptyStringField(Item, TEXT("name")));
		Test->TestTrue(Label + TEXT(" item has full_name"), HasNonEmptyStringField(Item, TEXT("full_name")));

		const TSharedPtr<FJsonObject>* JsonAuthoringObj = nullptr;
		if (!Test->TestTrue(Label + TEXT(" item has json_authoring"),
			Item->TryGetObjectField(TEXT("json_authoring"), JsonAuthoringObj) && JsonAuthoringObj && JsonAuthoringObj->IsValid()))
		{
			return false;
		}
		OutAuthoring = *JsonAuthoringObj;

		Test->TestEqual(Label + TEXT(" authoring schema"),
			OutAuthoring->GetStringField(TEXT("schema")),
			FString(TEXT("uai.catalog_authoring_seed.v1")));
		Test->TestTrue(Label + TEXT(" authoring route"), HasNonEmptyStringField(OutAuthoring, TEXT("route")));
		Test->TestTrue(Label + TEXT(" authoring target"), HasNonEmptyStringField(OutAuthoring, TEXT("target")));
		Test->TestTrue(Label + TEXT(" authoring profile_path"), HasNonEmptyStringField(OutAuthoring, TEXT("profile_path")));

		const TSharedPtr<FJsonObject>* SeedObj = nullptr;
		if (!Test->TestTrue(Label + TEXT(" authoring has seed"),
			OutAuthoring->TryGetObjectField(TEXT("seed"), SeedObj) && SeedObj && SeedObj->IsValid()))
		{
			return false;
		}
		OutSeed = *SeedObj;
		return true;
	}

	static bool ValidateNameOnlySeed(
		FAutomationTestBase* Test,
		const FString& Label,
		const TSharedPtr<FJsonObject>& Item,
		const TSharedPtr<FJsonObject>& Seed)
	{
		return Test->TestEqual(Label + TEXT(" seed name"), Seed->GetStringField(TEXT("name")), Item->GetStringField(TEXT("name"))) &&
			Test->TestFalse(Label + TEXT(" seed omits kind"), Seed->HasField(TEXT("kind"))) &&
			Test->TestFalse(Label + TEXT(" seed omits full_name"), Seed->HasField(TEXT("full_name"))) &&
			Test->TestFalse(Label + TEXT(" seed omits class_path"), Seed->HasField(TEXT("class_path")));
	}

	static bool ValidateCatalogSeedForMinimalAuthoring(
		FAutomationTestBase* Test,
		const FString& Label,
		const TSharedPtr<FJsonObject>& Item)
	{
		TSharedPtr<FJsonObject> Authoring;
		TSharedPtr<FJsonObject> Seed;
		if (!GetJsonAuthoringSeed(Test, Label, Item, Authoring, Seed))
		{
			return false;
		}

		const FString Kind = Item->GetStringField(TEXT("kind"));
		if (Kind == TEXT("niagara_module"))
		{
			Test->TestEqual(Label + TEXT(" route"), Authoring->GetStringField(TEXT("route")), FString(TEXT("niagara_authoring_profile")));
			Test->TestEqual(Label + TEXT(" target"), Authoring->GetStringField(TEXT("target")), FString(TEXT("niagara.modules[]")));
			Test->TestEqual(Label + TEXT(" profile_path"), Authoring->GetStringField(TEXT("profile_path")), FString(TEXT("stages[].modules[]")));
			return ValidateNameOnlySeed(Test, Label, Item, Seed) &&
				Test->TestEqual(Label + TEXT(" operation"), Seed->GetStringField(TEXT("operation")), FString(TEXT("upsert"))) &&
				Test->TestTrue(Label + TEXT(" seed inputs array"), HasArrayField(Seed, TEXT("inputs")));
		}

		if (Kind == TEXT("niagara_dynamic_input"))
		{
			Test->TestEqual(Label + TEXT(" route"), Authoring->GetStringField(TEXT("route")), FString(TEXT("niagara_authoring_profile")));
			Test->TestEqual(Label + TEXT(" target"), Authoring->GetStringField(TEXT("target")), FString(TEXT("niagara.modules[].inputs[].value")));
			Test->TestEqual(Label + TEXT(" profile_path"), Authoring->GetStringField(TEXT("profile_path")), FString(TEXT("stages[].modules[].inputs[].value")));
			const TSharedPtr<FJsonObject>* DynamicInputObj = nullptr;
			return Test->TestEqual(Label + TEXT(" source"), Seed->GetStringField(TEXT("source")), FString(TEXT("dynamic_input"))) &&
				Test->TestTrue(Label + TEXT(" dynamic_input object"), Seed->TryGetObjectField(TEXT("dynamic_input"), DynamicInputObj) && DynamicInputObj && DynamicInputObj->IsValid()) &&
				ValidateNameOnlySeed(Test, Label + TEXT(" dynamic_input"), Item, *DynamicInputObj) &&
				Test->TestTrue(Label + TEXT(" dynamic_input inputs array"), HasArrayField(*DynamicInputObj, TEXT("inputs")));
		}

		if (Kind == TEXT("niagara_graph_function"))
		{
			Test->TestEqual(Label + TEXT(" route"), Authoring->GetStringField(TEXT("route")), FString(TEXT("niagara_script_authoring_profile")));
			Test->TestEqual(Label + TEXT(" target"), Authoring->GetStringField(TEXT("target")), FString(TEXT("niagara_script.graph.nodes[]")));
			Test->TestEqual(Label + TEXT(" profile_path"), Authoring->GetStringField(TEXT("profile_path")), FString(TEXT("graph.nodes[]")));
			return ValidateNameOnlySeed(Test, Label, Item, Seed) &&
				Test->TestTrue(Label + TEXT(" seed node_id"), HasNonEmptyStringField(Seed, TEXT("node_id"))) &&
				Test->TestTrue(Label + TEXT(" seed layout"), HasObjectField(Seed, TEXT("layout")));
		}

		if (Kind == TEXT("material_expression") || Kind == TEXT("material_function_call"))
		{
			Test->TestEqual(Label + TEXT(" route"), Authoring->GetStringField(TEXT("route")), FString(TEXT("material_authoring_profile")));
			Test->TestEqual(Label + TEXT(" target"), Authoring->GetStringField(TEXT("target")), FString(TEXT("material.graph.nodes[]")));
			Test->TestEqual(Label + TEXT(" profile_path"), Authoring->GetStringField(TEXT("profile_path")), FString(TEXT("material_graph.nodes[]")));
			return ValidateNameOnlySeed(Test, Label, Item, Seed) &&
				Test->TestTrue(Label + TEXT(" seed node_id"), HasNonEmptyStringField(Seed, TEXT("node_id"))) &&
				Test->TestTrue(Label + TEXT(" seed layout"), HasObjectField(Seed, TEXT("layout"))) &&
				Test->TestTrue(Label + TEXT(" seed properties"), HasObjectField(Seed, TEXT("properties")));
		}

		if (Kind.StartsWith(TEXT("blueprint_")))
		{
			Test->TestEqual(Label + TEXT(" route"), Authoring->GetStringField(TEXT("route")), FString(TEXT("blueprint_umg_authoring_profile")));
			Test->TestEqual(Label + TEXT(" target"), Authoring->GetStringField(TEXT("target")), FString(TEXT("blueprint.graph.nodes[]")));
			Test->TestEqual(Label + TEXT(" profile_path"), Authoring->GetStringField(TEXT("profile_path")), FString(TEXT("blueprint_graphs[].nodes[]")));
			return ValidateNameOnlySeed(Test, Label, Item, Seed) &&
				Test->TestTrue(Label + TEXT(" seed node_id"), HasNonEmptyStringField(Seed, TEXT("node_id"))) &&
				Test->TestTrue(Label + TEXT(" seed layout"), HasObjectField(Seed, TEXT("layout"))) &&
				Test->TestTrue(Label + TEXT(" seed pin_defaults array"), HasArrayField(Seed, TEXT("pin_defaults")));
		}

		if (Kind == TEXT("actor_component"))
		{
			Test->TestEqual(Label + TEXT(" route"), Authoring->GetStringField(TEXT("route")), FString(TEXT("blueprint_umg_authoring_profile")));
			Test->TestEqual(Label + TEXT(" target"), Authoring->GetStringField(TEXT("target")), FString(TEXT("blueprint.components[]")));
			Test->TestEqual(Label + TEXT(" profile_path"), Authoring->GetStringField(TEXT("profile_path")), FString(TEXT("components[]")));
			return ValidateNameOnlySeed(Test, Label, Item, Seed) &&
				Test->TestTrue(Label + TEXT(" seed component_id"), HasNonEmptyStringField(Seed, TEXT("component_id"))) &&
				Test->TestTrue(Label + TEXT(" seed parent_component_id"), Seed->HasField(TEXT("parent_component_id"))) &&
				Test->TestTrue(Label + TEXT(" seed properties"), HasObjectField(Seed, TEXT("properties")));
		}

		if (Kind == TEXT("umg_widget"))
		{
			Test->TestEqual(Label + TEXT(" route"), Authoring->GetStringField(TEXT("route")), FString(TEXT("blueprint_umg_authoring_profile")));
			Test->TestEqual(Label + TEXT(" target"), Authoring->GetStringField(TEXT("target")), FString(TEXT("umg.widget_tree.widgets[]")));
			Test->TestEqual(Label + TEXT(" profile_path"), Authoring->GetStringField(TEXT("profile_path")), FString(TEXT("widget_tree.widgets[]")));
			return ValidateNameOnlySeed(Test, Label, Item, Seed) &&
				Test->TestTrue(Label + TEXT(" seed widget_id"), HasNonEmptyStringField(Seed, TEXT("widget_id"))) &&
				Test->TestTrue(Label + TEXT(" seed parent_widget_id"), Seed->HasField(TEXT("parent_widget_id"))) &&
				Test->TestTrue(Label + TEXT(" seed make_variable"), Seed->HasField(TEXT("make_variable"))) &&
				Test->TestTrue(Label + TEXT(" seed properties"), HasObjectField(Seed, TEXT("properties"))) &&
				Test->TestTrue(Label + TEXT(" seed children_widget_ids array"), HasArrayField(Seed, TEXT("children_widget_ids")));
		}

		if (Kind == TEXT("placeable_actor"))
		{
			Test->TestEqual(Label + TEXT(" route"), Authoring->GetStringField(TEXT("route")), FString(TEXT("level_folder_package")));
			Test->TestEqual(Label + TEXT(" target"), Authoring->GetStringField(TEXT("target")), FString(TEXT("level.actors.items[]")));
			Test->TestTrue(Label + TEXT(" profile_path actor json"), Authoring->GetStringField(TEXT("profile_path")).Contains(TEXT("/actor.json")));
			return ValidateNameOnlySeed(Test, Label, Item, Seed) &&
				Test->TestTrue(Label + TEXT(" seed actor_id"), HasNonEmptyStringField(Seed, TEXT("actor_id"))) &&
				Test->TestTrue(Label + TEXT(" seed transform"), HasObjectField(Seed, TEXT("transform")));
		}

		Test->AddError(Label + TEXT(" unhandled catalog item kind: ") + Kind);
		return false;
	}

	static bool SearchAndValidateCatalogSeed(
		FAutomationTestBase* Test,
		const FScopedUeAgentHttpServer& ServerScope,
		const FString& Label,
		const FString& ParamsJson)
	{
		TSharedPtr<FJsonObject> Data;
		if (!ExecNodeCatalogOriginCommand(Test, ServerScope, Label, TEXT("node_catalog_search"), ParamsJson, Data))
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Item = FirstCatalogItem(Data);
		if (!Test->TestTrue(Label + TEXT(" returns sample item"), Item.IsValid()))
		{
			return false;
		}
		return ValidateCatalogSeedForMinimalAuthoring(Test, Label, Item);
	}

	static UBlueprint* CreateBlueprintVariableCatalogFixture(FAutomationTestBase* Test)
	{
		const FName TempBlueprintName = MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), TEXT("UAI_NodeCatalog_VariableBlueprint"));
		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(),
			GetTransientPackage(),
			TempBlueprintName,
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass());
		if (!Test->TestNotNull(TEXT("blueprint variable catalog fixture created"), Blueprint))
		{
			return nullptr;
		}

		FEdGraphPinType VarType;
		VarType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(TEXT("CatalogSeedBool")), VarType);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		return Blueprint;
	}

	static bool CreateMaterialPinContextFixture(
		FAutomationTestBase* Test,
		UMaterial*& OutMaterial,
		UMaterialGraphNode*& OutNode,
		UEdGraphPin*& OutPin)
	{
		OutMaterial = NewObject<UMaterial>(
			GetTransientPackage(),
			MakeUniqueObjectName(GetTransientPackage(), UMaterial::StaticClass(), TEXT("UAI_NodeCatalog_PinContextMaterial")),
			RF_Transactional | RF_Transient);
		if (!Test->TestNotNull(TEXT("material pin context fixture material created"), OutMaterial))
		{
			return false;
		}

		OutMaterial->SetShadingModel(MSM_Unlit);
		OutMaterial->MaterialGraph = CastChecked<UMaterialGraph>(
			FBlueprintEditorUtils::CreateNewGraph(
				OutMaterial,
				NAME_None,
				UMaterialGraph::StaticClass(),
				UMaterialGraphSchema::StaticClass()));
		OutMaterial->MaterialGraph->Material = OutMaterial;
		OutMaterial->MaterialGraph->MaterialFunction = nullptr;
		OutMaterial->MaterialGraph->OriginalMaterialFullName = OutMaterial->GetName();
		OutMaterial->MaterialGraph->RebuildGraph();

		UMaterialExpressionConstant* Constant = NewObject<UMaterialExpressionConstant>(OutMaterial, NAME_None, RF_Transactional);
		if (!Test->TestNotNull(TEXT("material pin context fixture constant created"), Constant))
		{
			return false;
		}
		Constant->Material = OutMaterial;
		Constant->Function = nullptr;
		Constant->MaterialExpressionEditorX = -200;
		Constant->MaterialExpressionEditorY = 0;
		OutMaterial->GetExpressionCollection().AddExpression(Constant);
		OutMaterial->MaterialGraph->RebuildGraph();

		for (UEdGraphNode* GraphNode : OutMaterial->MaterialGraph->Nodes)
		{
			UMaterialGraphNode* MaterialNode = Cast<UMaterialGraphNode>(GraphNode);
			if (MaterialNode && MaterialNode->MaterialExpression == Constant)
			{
				OutNode = MaterialNode;
				break;
			}
		}
		if (!Test->TestNotNull(TEXT("material pin context fixture graph node created"), OutNode))
		{
			return false;
		}

		for (UEdGraphPin* Pin : OutNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output)
			{
				OutPin = Pin;
				break;
			}
		}

		return Test->TestNotNull(TEXT("material pin context fixture output pin created"), OutPin);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceNodeCatalogOriginSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.NodeCatalogOrigin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceNodeCatalogOriginSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	TSharedPtr<FJsonObject> SearchData;
	TestTrue(TEXT("node_catalog_search v3 default succeeds"), ExecNodeCatalogOriginCommand(
		this,
		ServerScope,
		TEXT("smoke-node-catalog-search-v3"),
		TEXT("node_catalog_search"),
		TEXT("{\"catalog_type\":\"niagara_module\",\"query\":{\"text\":\"System Location\"},\"page\":{\"limit\":5}}"),
		SearchData));
	TestEqual(TEXT("node_catalog_search schema"), SearchData.IsValid() ? SearchData->GetStringField(TEXT("schema")) : FString(), FString(TEXT("uai.node_catalog.search.v3")));

	const TSharedPtr<FJsonObject> FirstItem = FirstCatalogItem(SearchData);
	TestTrue(TEXT("node_catalog_search returns at least one item"), FirstItem.IsValid());
	if (!FirstItem.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("catalog item uses niagara module kind"), FirstItem->GetStringField(TEXT("kind")), FString(TEXT("niagara_module")));
	TestTrue(TEXT("catalog item has name"), HasNonEmptyStringField(FirstItem, TEXT("name")));
	TestTrue(TEXT("catalog item has full_name"), HasNonEmptyStringField(FirstItem, TEXT("full_name")));
	TestTrue(TEXT("catalog item has candidate_id"), HasNonEmptyStringField(FirstItem, TEXT("candidate_id")));
	TestTrue(TEXT("catalog item has json_authoring"), FirstItem->HasTypedField<EJson::Object>(TEXT("json_authoring")));
	const TSharedPtr<FJsonObject>* FirstJsonAuthoringObj = nullptr;
	TestTrue(TEXT("catalog json_authoring is seed schema"),
		FirstItem->TryGetObjectField(TEXT("json_authoring"), FirstJsonAuthoringObj) &&
		FirstJsonAuthoringObj && FirstJsonAuthoringObj->IsValid() &&
		(*FirstJsonAuthoringObj)->GetStringField(TEXT("schema")) == TEXT("uai.catalog_authoring_seed.v1"));
	const TSharedPtr<FJsonObject>* FirstSeedObj = nullptr;
	TestTrue(TEXT("catalog json_authoring has seed"),
		FirstJsonAuthoringObj && FirstJsonAuthoringObj->IsValid() &&
		(*FirstJsonAuthoringObj)->TryGetObjectField(TEXT("seed"), FirstSeedObj) &&
		FirstSeedObj && FirstSeedObj->IsValid());
	if (FirstSeedObj && FirstSeedObj->IsValid())
	{
		TestTrue(TEXT("catalog seed keeps name only"), HasNonEmptyStringField(*FirstSeedObj, TEXT("name")));
		TestFalse(TEXT("catalog seed omits kind"), (*FirstSeedObj)->HasField(TEXT("kind")));
		TestFalse(TEXT("catalog seed omits full_name"), (*FirstSeedObj)->HasField(TEXT("full_name")));
	}
	TestFalse(TEXT("default catalog item omits legacy_authoring"), FirstItem->HasField(TEXT("legacy_authoring")));
	TestFalse(TEXT("default catalog item omits recommended_authoring"), FirstItem->HasField(TEXT("recommended_authoring")));
	TestFalse(TEXT("default catalog item omits stable_id"), FirstItem->HasField(TEXT("stable_id")));

	const FString CandidateId = FirstItem->GetStringField(TEXT("candidate_id"));
	const FString CandidateName = FirstItem->GetStringField(TEXT("name"));
	const FString CandidateFullName = FirstItem->GetStringField(TEXT("full_name"));

	TSharedPtr<FJsonObject> ExpandedData;
	TestTrue(TEXT("node_catalog_search candidate expansion succeeds"), ExecNodeCatalogOriginCommand(
		this,
		ServerScope,
		TEXT("smoke-node-catalog-search-expanded"),
		TEXT("node_catalog_search"),
		FString::Printf(
			TEXT("{\"catalog_type\":\"niagara_module\",\"candidate_id\":%s,\"view\":{\"include_legacy_authoring\":true,\"include_debug_fields\":true},\"page\":{\"limit\":1}}"),
			*QuoteNodeCatalogOriginJson(CandidateId)),
		ExpandedData));

	const TSharedPtr<FJsonObject> ExpandedItem = FirstCatalogItem(ExpandedData);
	TestTrue(TEXT("expanded catalog returns item"), ExpandedItem.IsValid());
	if (ExpandedItem.IsValid())
	{
		TestTrue(TEXT("expanded catalog includes legacy_authoring"), ExpandedItem->HasTypedField<EJson::Object>(TEXT("legacy_authoring")));
		TestTrue(TEXT("expanded catalog includes recommended_authoring alias"), ExpandedItem->HasTypedField<EJson::Object>(TEXT("recommended_authoring")));
		TestTrue(TEXT("expanded catalog includes debug stable_id"), HasNonEmptyStringField(ExpandedItem, TEXT("stable_id")));
	}

	TSharedPtr<FJsonObject> CategoriesData;
	TestTrue(TEXT("node_catalog_categories succeeds"), ExecNodeCatalogOriginCommand(
		this,
		ServerScope,
		TEXT("smoke-node-catalog-categories"),
		TEXT("node_catalog_categories"),
		TEXT("{\"catalog_type\":\"niagara_module\",\"query\":{\"text\":\"Location\"},\"page\":{\"limit\":20}}"),
		CategoriesData));
	TestEqual(TEXT("node_catalog_categories schema"), CategoriesData.IsValid() ? CategoriesData->GetStringField(TEXT("schema")) : FString(), FString(TEXT("uai.node_catalog.categories.v3")));
	TestTrue(TEXT("node_catalog_categories returns category tree"), HasNonEmptyArrayField(CategoriesData, TEXT("category_tree")));

	TestTrue(TEXT("node catalog seed contract niagara module"), SearchAndValidateCatalogSeed(
		this,
		ServerScope,
		TEXT("seed-contract-niagara-module"),
		TEXT("{\"catalog_type\":\"niagara_module\",\"query\":{\"text\":\"Location\"},\"page\":{\"limit\":5}}")));
	TestTrue(TEXT("node catalog seed contract niagara dynamic input"), SearchAndValidateCatalogSeed(
		this,
		ServerScope,
		TEXT("seed-contract-niagara-dynamic-input"),
		TEXT("{\"catalog_type\":\"niagara_dynamic_input\",\"page\":{\"limit\":5}}")));
	TestTrue(TEXT("node catalog seed contract niagara input value alias"), SearchAndValidateCatalogSeed(
		this,
		ServerScope,
		TEXT("seed-contract-niagara-input-value-alias"),
		TEXT("{\"catalog_type\":\"niagara_input_value_source\",\"page\":{\"limit\":5}}")));
	TestTrue(TEXT("node catalog seed contract niagara graph node"), SearchAndValidateCatalogSeed(
		this,
		ServerScope,
		TEXT("seed-contract-niagara-graph-node"),
		TEXT("{\"catalog_type\":\"niagara_graph_node\",\"page\":{\"limit\":5}}")));
	TestTrue(TEXT("node catalog seed contract niagara function alias"), SearchAndValidateCatalogSeed(
		this,
		ServerScope,
		TEXT("seed-contract-niagara-function-alias"),
		TEXT("{\"catalog_type\":\"niagara_function\",\"page\":{\"limit\":5}}")));
	TestTrue(TEXT("node catalog seed contract material node"), SearchAndValidateCatalogSeed(
		this,
		ServerScope,
		TEXT("seed-contract-material-node"),
		TEXT("{\"catalog_type\":\"material_node\",\"query\":{\"text\":\"Multiply\"},\"page\":{\"limit\":5}}")));
	TestTrue(TEXT("node catalog seed contract material expression"), SearchAndValidateCatalogSeed(
		this,
		ServerScope,
		TEXT("seed-contract-material-expression"),
		TEXT("{\"catalog_type\":\"material_expression\",\"query\":{\"text\":\"Multiply\"},\"page\":{\"limit\":5}}")));
	TestTrue(TEXT("node catalog seed contract material function call"), SearchAndValidateCatalogSeed(
		this,
		ServerScope,
		TEXT("seed-contract-material-function-call"),
		TEXT("{\"catalog_type\":\"material_function_call\",\"page\":{\"limit\":5}}")));
	TestTrue(TEXT("node catalog seed contract material function alias"), SearchAndValidateCatalogSeed(
		this,
		ServerScope,
		TEXT("seed-contract-material-function-alias"),
		TEXT("{\"catalog_type\":\"material_function\",\"page\":{\"limit\":5}}")));
	TestTrue(TEXT("node catalog seed contract blueprint node"), SearchAndValidateCatalogSeed(
		this,
		ServerScope,
		TEXT("seed-contract-blueprint-node"),
		TEXT("{\"catalog_type\":\"blueprint_node\",\"query\":{\"text\":\"Print String\"},\"page\":{\"limit\":5}}")));
	TestTrue(TEXT("node catalog seed contract blueprint call function"), SearchAndValidateCatalogSeed(
		this,
		ServerScope,
		TEXT("seed-contract-blueprint-call-function"),
		TEXT("{\"catalog_type\":\"blueprint_call_function\",\"query\":{\"text\":\"Print String\"},\"page\":{\"limit\":5}}")));
	TestTrue(TEXT("node catalog seed contract blueprint event"), SearchAndValidateCatalogSeed(
		this,
		ServerScope,
		TEXT("seed-contract-blueprint-event"),
		TEXT("{\"catalog_type\":\"blueprint_event\",\"query\":{\"text\":\"BeginPlay\"},\"page\":{\"limit\":5}}")));
	if (UBlueprint* VariableBlueprint = CreateBlueprintVariableCatalogFixture(this))
	{
		TestTrue(TEXT("node catalog seed contract blueprint variable"), SearchAndValidateCatalogSeed(
			this,
			ServerScope,
			TEXT("seed-contract-blueprint-variable"),
			FString::Printf(
				TEXT("{\"catalog_type\":\"blueprint_variable_node\",\"query\":{\"text\":\"CatalogSeedBool\"},\"context\":{\"asset_path\":%s,\"graph_name\":\"EventGraph\"},\"page\":{\"limit\":5}}"),
				*QuoteNodeCatalogOriginJson(VariableBlueprint->GetPathName()))));
	}
	TestTrue(TEXT("node catalog seed contract actor component"), SearchAndValidateCatalogSeed(
		this,
		ServerScope,
		TEXT("seed-contract-actor-component"),
		TEXT("{\"catalog_type\":\"actor_component\",\"query\":{\"text\":\"Static Mesh\"},\"page\":{\"limit\":5}}")));
	TestTrue(TEXT("node catalog seed contract umg widget"), SearchAndValidateCatalogSeed(
		this,
		ServerScope,
		TEXT("seed-contract-umg-widget"),
		TEXT("{\"catalog_type\":\"umg_widget\",\"query\":{\"text\":\"Button\"},\"page\":{\"limit\":5}}")));
	TestTrue(TEXT("node catalog seed contract placeable actor"), SearchAndValidateCatalogSeed(
		this,
		ServerScope,
		TEXT("seed-contract-placeable-actor"),
		TEXT("{\"catalog_type\":\"placeable_actor\",\"query\":{\"text\":\"Point Light\"},\"page\":{\"limit\":5}}")));
	TestTrue(TEXT("node catalog seed contract level actor alias"), SearchAndValidateCatalogSeed(
		this,
		ServerScope,
		TEXT("seed-contract-level-actor-alias"),
		TEXT("{\"catalog_type\":\"level_actor\",\"query\":{\"text\":\"Text Render\"},\"page\":{\"limit\":5}}")));
	TestTrue(TEXT("node catalog seed contract actor placeable alias"), SearchAndValidateCatalogSeed(
		this,
		ServerScope,
		TEXT("seed-contract-actor-placeable-alias"),
		TEXT("{\"catalog_type\":\"actor_placeable\",\"query\":{\"text\":\"Player Start\"},\"page\":{\"limit\":5}}")));

	TSharedPtr<FJsonObject> ComponentSearchData;
	TestTrue(TEXT("node_catalog_search actor component succeeds"), ExecNodeCatalogOriginCommand(
		this,
		ServerScope,
		TEXT("smoke-node-catalog-actor-component"),
		TEXT("node_catalog_search"),
		TEXT("{\"catalog_type\":\"actor_component\",\"query\":{\"text\":\"Static Mesh\"},\"page\":{\"limit\":5}}"),
		ComponentSearchData));
	const TSharedPtr<FJsonObject> ComponentItem = FirstCatalogItem(ComponentSearchData);
	TestTrue(TEXT("actor component catalog returns item"), ComponentItem.IsValid());
	if (ComponentItem.IsValid())
	{
		TestEqual(TEXT("actor component item kind"), ComponentItem->GetStringField(TEXT("kind")), FString(TEXT("actor_component")));
		TestTrue(TEXT("actor component item has name"), HasNonEmptyStringField(ComponentItem, TEXT("name")));
		TestTrue(TEXT("actor component item has full_name"), HasNonEmptyStringField(ComponentItem, TEXT("full_name")));
		TestTrue(TEXT("actor component item has json_authoring"), ComponentItem->HasTypedField<EJson::Object>(TEXT("json_authoring")));
		const TSharedPtr<FJsonObject>* ComponentJsonAuthoringObj = nullptr;
		const TSharedPtr<FJsonObject>* ComponentSeedObj = nullptr;
		TestTrue(TEXT("actor component json_authoring has component seed"),
			ComponentItem->TryGetObjectField(TEXT("json_authoring"), ComponentJsonAuthoringObj) &&
			ComponentJsonAuthoringObj && ComponentJsonAuthoringObj->IsValid() &&
			(*ComponentJsonAuthoringObj)->TryGetObjectField(TEXT("seed"), ComponentSeedObj) &&
			ComponentSeedObj && ComponentSeedObj->IsValid() &&
			HasNonEmptyStringField(*ComponentSeedObj, TEXT("component_id")) &&
			HasNonEmptyStringField(*ComponentSeedObj, TEXT("name")) &&
			!(*ComponentSeedObj)->HasField(TEXT("kind")));
	}

	TSharedPtr<FJsonObject> WidgetSearchData;
	TestTrue(TEXT("node_catalog_search umg widget succeeds"), ExecNodeCatalogOriginCommand(
		this,
		ServerScope,
		TEXT("smoke-node-catalog-umg-widget"),
		TEXT("node_catalog_search"),
		TEXT("{\"catalog_type\":\"umg_widget\",\"query\":{\"text\":\"Button\"},\"page\":{\"limit\":5}}"),
		WidgetSearchData));
	const TSharedPtr<FJsonObject> WidgetItem = FirstCatalogItem(WidgetSearchData);
	TestTrue(TEXT("umg widget catalog returns item"), WidgetItem.IsValid());
	if (WidgetItem.IsValid())
	{
		TestEqual(TEXT("umg widget item kind"), WidgetItem->GetStringField(TEXT("kind")), FString(TEXT("umg_widget")));
		TestTrue(TEXT("umg widget item has name"), HasNonEmptyStringField(WidgetItem, TEXT("name")));
		TestTrue(TEXT("umg widget item has full_name"), HasNonEmptyStringField(WidgetItem, TEXT("full_name")));
		TestTrue(TEXT("umg widget item has json_authoring"), WidgetItem->HasTypedField<EJson::Object>(TEXT("json_authoring")));
		const TSharedPtr<FJsonObject>* WidgetJsonAuthoringObj = nullptr;
		const TSharedPtr<FJsonObject>* WidgetSeedObj = nullptr;
		TestTrue(TEXT("umg widget json_authoring has widget seed"),
			WidgetItem->TryGetObjectField(TEXT("json_authoring"), WidgetJsonAuthoringObj) &&
			WidgetJsonAuthoringObj && WidgetJsonAuthoringObj->IsValid() &&
			(*WidgetJsonAuthoringObj)->TryGetObjectField(TEXT("seed"), WidgetSeedObj) &&
			WidgetSeedObj && WidgetSeedObj->IsValid() &&
			HasNonEmptyStringField(*WidgetSeedObj, TEXT("widget_id")) &&
			HasNonEmptyStringField(*WidgetSeedObj, TEXT("name")) &&
			!(*WidgetSeedObj)->HasField(TEXT("kind")));
	}

	UMaterial* PinContextMaterial = nullptr;
	UMaterialGraphNode* PinContextNode = nullptr;
	UEdGraphPin* PinContextPin = nullptr;
	if (CreateMaterialPinContextFixture(this, PinContextMaterial, PinContextNode, PinContextPin))
	{
		TSharedPtr<FJsonObject> MaterialPinSearchData;
		TestTrue(TEXT("node_catalog_search material pin context succeeds"), ExecNodeCatalogOriginCommand(
			this,
			ServerScope,
			TEXT("smoke-node-catalog-material-pin"),
			TEXT("node_catalog_search"),
			FString::Printf(
				TEXT("{\"catalog_type\":\"material_node\",\"query\":{\"text\":\"Multiply\"},\"context\":{\"asset_path\":%s,\"graph_name\":%s,\"node_guid\":%s,\"pin_guid\":%s},\"page\":{\"limit\":5}}"),
				*QuoteNodeCatalogOriginJson(PinContextMaterial->GetPathName()),
				*QuoteNodeCatalogOriginJson(PinContextMaterial->MaterialGraph->GetName()),
				*QuoteNodeCatalogOriginJson(PinContextNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)),
				*QuoteNodeCatalogOriginJson(PinContextPin->PinId.ToString(EGuidFormats::DigitsWithHyphens))),
			MaterialPinSearchData));

		const TSharedPtr<FJsonObject> MaterialPinItem = FirstCatalogItem(MaterialPinSearchData);
		TestTrue(TEXT("material pin context returns item"), MaterialPinItem.IsValid());
		if (MaterialPinItem.IsValid())
		{
			TestEqual(TEXT("material pin context item kind"), MaterialPinItem->GetStringField(TEXT("kind")), FString(TEXT("material_expression")));
			TestEqual(TEXT("material pin context section"), MaterialPinItem->GetStringField(TEXT("section")), FString(TEXT("Pin Context")));
			TestTrue(TEXT("material pin context item has name"), HasNonEmptyStringField(MaterialPinItem, TEXT("name")));
			TestTrue(TEXT("material pin context item has full_name"), HasNonEmptyStringField(MaterialPinItem, TEXT("full_name")));
			TestTrue(TEXT("material pin context item has json_authoring"), MaterialPinItem->HasTypedField<EJson::Object>(TEXT("json_authoring")));
			const TSharedPtr<FJsonObject>* MaterialJsonAuthoringObj = nullptr;
			const TSharedPtr<FJsonObject>* MaterialSeedObj = nullptr;
			TestTrue(TEXT("material pin context json_authoring has node seed"),
				MaterialPinItem->TryGetObjectField(TEXT("json_authoring"), MaterialJsonAuthoringObj) &&
				MaterialJsonAuthoringObj && MaterialJsonAuthoringObj->IsValid() &&
				(*MaterialJsonAuthoringObj)->TryGetObjectField(TEXT("seed"), MaterialSeedObj) &&
				MaterialSeedObj && MaterialSeedObj->IsValid() &&
				HasNonEmptyStringField(*MaterialSeedObj, TEXT("node_id")) &&
				HasNonEmptyStringField(*MaterialSeedObj, TEXT("name")) &&
				!(*MaterialSeedObj)->HasField(TEXT("kind")));
		}
	}

	TSharedPtr<FJsonObject> AssetOriginData;
	TestTrue(TEXT("node_origin_resolve asset succeeds"), ExecNodeCatalogOriginCommand(
		this,
		ServerScope,
		TEXT("smoke-node-origin-asset"),
		TEXT("node_origin_resolve"),
		FString::Printf(
			TEXT("{\"target\":{\"source\":\"name_reference\",\"kind\":\"niagara_module\",\"name\":%s,\"full_name\":%s}}"),
			*QuoteNodeCatalogOriginJson(CandidateName),
			*QuoteNodeCatalogOriginJson(CandidateFullName)),
		AssetOriginData));
	TestTrue(TEXT("node_origin_resolve asset resolved"), AssetOriginData.IsValid() && AssetOriginData->GetBoolField(TEXT("resolved")));
	const TSharedPtr<FJsonObject>* AssetOriginObj = nullptr;
	TestTrue(TEXT("node_origin_resolve asset contains origin"), AssetOriginData.IsValid() && AssetOriginData->TryGetObjectField(TEXT("origin"), AssetOriginObj) && AssetOriginObj && AssetOriginObj->IsValid());
	if (AssetOriginObj && AssetOriginObj->IsValid())
	{
		TestTrue(TEXT("node_origin_resolve asset returns asset or package path"),
			HasNonEmptyArrayField(*AssetOriginObj, TEXT("asset_paths")) || HasNonEmptyArrayField(*AssetOriginObj, TEXT("package_paths")));
	}

	const FString PrintStringFullName = TEXT("/Script/Engine.KismetSystemLibrary.PrintString");
	TSharedPtr<FJsonObject> NativeOriginData;
	TestTrue(TEXT("node_origin_resolve native succeeds"), ExecNodeCatalogOriginCommand(
		this,
		ServerScope,
		TEXT("smoke-node-origin-native"),
		TEXT("node_origin_resolve"),
		FString::Printf(
			TEXT("{\"target\":{\"source\":\"name_reference\",\"kind\":\"blueprint_call_function\",\"name\":\"Print String\",\"full_name\":%s}}"),
			*QuoteNodeCatalogOriginJson(PrintStringFullName)),
		NativeOriginData));
	TestTrue(TEXT("node_origin_resolve native resolved"), NativeOriginData.IsValid() && NativeOriginData->GetBoolField(TEXT("resolved")));
	const TSharedPtr<FJsonObject>* NativeOriginObj = nullptr;
	TestTrue(TEXT("node_origin_resolve native contains origin"), NativeOriginData.IsValid() && NativeOriginData->TryGetObjectField(TEXT("origin"), NativeOriginObj) && NativeOriginObj && NativeOriginObj->IsValid());
	if (NativeOriginObj && NativeOriginObj->IsValid())
	{
		TestTrue(TEXT("node_origin_resolve native returns code path"), HasNonEmptyArrayField(*NativeOriginObj, TEXT("code_paths")));
	}

	const FString JsonFixtureDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("UeAgentInterface"));
	IFileManager::Get().MakeDirectory(*JsonFixtureDir, true);
	const FString JsonFixturePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(JsonFixtureDir, TEXT("node_origin_resolve_identity.json")));
	const FString JsonFixture = FString::Printf(
		TEXT("{\"nodes\":[{\"kind\":\"blueprint_call_function\",\"name\":\"Print String\",\"full_name\":%s},{\"function_owner_class\":\"/Script/Engine.KismetSystemLibrary\",\"function_name\":\"PrintString\"},{\"component_class\":\"/Script/Engine.StaticMeshComponent\",\"component_name\":\"Mesh\"},{\"widget_class\":\"/Script/UMG.Button\",\"widget_name\":\"Button\"}]}"),
		*QuoteNodeCatalogOriginJson(PrintStringFullName));
	TestTrue(TEXT("node_origin_resolve json fixture saved"), FFileHelper::SaveStringToFile(JsonFixture, *JsonFixturePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	const FString JsonFixturePathForJson = JsonFixturePath.Replace(TEXT("\\"), TEXT("/"));

	TSharedPtr<FJsonObject> JsonV3OriginData;
	TestTrue(TEXT("node_origin_resolve json v3 succeeds"), ExecNodeCatalogOriginCommand(
		this,
		ServerScope,
		TEXT("smoke-node-origin-json-v3"),
		TEXT("node_origin_resolve"),
		FString::Printf(
			TEXT("{\"target\":{\"source\":\"json_reference\",\"json_file\":%s,\"json_pointer\":\"/nodes/0\"}}"),
			*QuoteNodeCatalogOriginJson(JsonFixturePathForJson)),
		JsonV3OriginData));
	TestTrue(TEXT("node_origin_resolve json v3 resolved"), JsonV3OriginData.IsValid() && JsonV3OriginData->GetBoolField(TEXT("resolved")));

	TSharedPtr<FJsonObject> JsonLegacyOriginData;
	TestTrue(TEXT("node_origin_resolve json legacy succeeds"), ExecNodeCatalogOriginCommand(
		this,
		ServerScope,
		TEXT("smoke-node-origin-json-legacy"),
		TEXT("node_origin_resolve"),
		FString::Printf(
			TEXT("{\"target\":{\"source\":\"json_reference\",\"json_file\":%s,\"json_pointer\":\"/nodes/1\"}}"),
			*QuoteNodeCatalogOriginJson(JsonFixturePathForJson)),
		JsonLegacyOriginData));
	TestTrue(TEXT("node_origin_resolve json legacy resolved"), JsonLegacyOriginData.IsValid() && JsonLegacyOriginData->GetBoolField(TEXT("resolved")));

	TSharedPtr<FJsonObject> JsonComponentLegacyOriginData;
	TestTrue(TEXT("node_origin_resolve json component legacy succeeds"), ExecNodeCatalogOriginCommand(
		this,
		ServerScope,
		TEXT("smoke-node-origin-json-component-legacy"),
		TEXT("node_origin_resolve"),
		FString::Printf(
			TEXT("{\"target\":{\"source\":\"json_reference\",\"json_file\":%s,\"json_pointer\":\"/nodes/2\"}}"),
			*QuoteNodeCatalogOriginJson(JsonFixturePathForJson)),
		JsonComponentLegacyOriginData));
	TestTrue(TEXT("node_origin_resolve json component legacy resolved"), JsonComponentLegacyOriginData.IsValid() && JsonComponentLegacyOriginData->GetBoolField(TEXT("resolved")));

	TSharedPtr<FJsonObject> JsonWidgetLegacyOriginData;
	TestTrue(TEXT("node_origin_resolve json widget legacy succeeds"), ExecNodeCatalogOriginCommand(
		this,
		ServerScope,
		TEXT("smoke-node-origin-json-widget-legacy"),
		TEXT("node_origin_resolve"),
		FString::Printf(
			TEXT("{\"target\":{\"source\":\"json_reference\",\"json_file\":%s,\"json_pointer\":\"/nodes/3\"}}"),
			*QuoteNodeCatalogOriginJson(JsonFixturePathForJson)),
		JsonWidgetLegacyOriginData));
	TestTrue(TEXT("node_origin_resolve json widget legacy resolved"), JsonWidgetLegacyOriginData.IsValid() && JsonWidgetLegacyOriginData->GetBoolField(TEXT("resolved")));

	return !HasAnyErrors();
}

#endif
