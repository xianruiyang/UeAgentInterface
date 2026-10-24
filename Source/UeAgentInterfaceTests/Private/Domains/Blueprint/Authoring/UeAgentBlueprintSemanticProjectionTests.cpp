#include "Misc/AutomationTest.h"

#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringDescriptors.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringOps.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintSemanticProjection.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraphSchema_K2.h"
#include "Math/Color.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentBlueprintSemanticProjectionTest,
	"UeAgentInterface.Domains.Blueprint.Authoring.SemanticProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentBlueprintSemanticProjectionTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::StateSemantic;

	FUeAgentResourceTracker Resources = MakeSemanticWorkspaceResourceTracker();
	auto Canonical = [&Resources](const TSharedPtr<FJsonValue>& Json)
	{
		FUeAgentCanonicalValuePtr Result;
		FString Error;
		check(CanonicalJson::TryFromJsonValue(Json, Resources, Result, Error));
		return Result.ToSharedRef();
	};
	FString IdentityError;
	check(UeAgentBlueprintAuthoring::RegisterDescriptors(IdentityError));
	UeAgentAuthoringSemanticRegistry::FSemanticDescriptor PinDefaultSemantic;
	TestTrue(
		TEXT("Pin defaults retain their reflected scalar or struct value type"),
		UeAgentAuthoringSemanticRegistry::FRegistry::Get().FindSemantic(
			UeAgentBlueprintAuthoring::PinDefaultSemanticId,
			PinDefaultSemantic)
			&& PinDefaultSemantic.ValueType == TEXT("*"));
	auto IdentityKey = [&IdentityError](
		const FString& ModelId,
		const FString& TargetKind,
		const TMap<FString, FString>& Selectors)
	{
		UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
		check(UeAgentAuthoringIdentity::BuildCanonicalIdentity(
			ModelId, TargetKind, Selectors, Identity, IdentityError));
		return Identity.CanonicalKey;
	};
	const FString AssetPath = TEXT("/Game/UAI/Tests/BP_SemanticProjection");
	const FString GraphGuid = TEXT("11111111-2222-3333-4444-555555555555");
	const FString ComponentGuid = TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
	const FString NodeGuid = TEXT("12345678-90ab-cdef-1234-567890abcdef");
	const FString PinGuid = TEXT("fedcba98-7654-3210-fedc-ba9876543210");
	const TMap<FString, FString> GraphSelectors = {
		{TEXT("asset_path"), AssetPath},
		{TEXT("graph_guid"), GraphGuid},
		{TEXT("graph_kind"), TEXT("event_graph")},
		{TEXT("graph_name"), TEXT("EventGraph")},
	};
	const TMap<FString, FString> ComponentSelectors = {
		{TEXT("asset_path"), AssetPath},
		{TEXT("component_guid"), ComponentGuid},
		{TEXT("component_kind"), TEXT("SceneComponent")},
		{TEXT("component_name"), TEXT("DefaultSceneRoot")},
	};
	TMap<FString, FString> NodeSelectors = GraphSelectors;
	NodeSelectors.Add(TEXT("node_guid"), NodeGuid);
	NodeSelectors.Add(TEXT("node_kind"), TEXT("call_function"));
	NodeSelectors.Add(TEXT("node_name"), TEXT("PrintString"));
	NodeSelectors.Add(TEXT("node_occurrence"), TEXT("0"));
	TMap<FString, FString> PinSelectors = NodeSelectors;
	PinSelectors.Add(TEXT("pin_guid"), PinGuid);
	PinSelectors.Add(TEXT("pin_name"), TEXT("TextColor"));
	PinSelectors.Add(TEXT("pin_occurrence"), TEXT("0"));
	PinSelectors.Add(TEXT("property_path"), TEXT("pin_default.TextColor"));

	const TSharedRef<FJsonObject> Graph = MakeShared<FJsonObject>();
	Graph->SetStringField(TEXT("name"), TEXT("EventGraph"));
	Graph->SetStringField(TEXT("graph_kind"), TEXT("event_graph"));
	const TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("name"), TEXT("Print String"));
	Node->SetNumberField(TEXT("x"), 240.0);
	TArray<TSharedPtr<FJsonValue>> Nodes;
	Nodes.Add(MakeShared<FJsonValueObject>(Node));

	TMap<FString, FUeAgentCanonicalValueRef> GraphValues;
	GraphValues.Add(TEXT("graph"), Canonical(MakeShared<FJsonValueObject>(Graph)));
	GraphValues.Add(TEXT("nodes"), Canonical(MakeShared<FJsonValueArray>(Nodes)));
	const FString TargetId = TEXT("target:blueprint:fixture");
	const FString GraphId = TEXT("graph:blueprint:EventGraph");
	const FString ComponentTreeId = TEXT("structure:blueprint:components");
	const FString MemberCollectionId = TEXT("structure:blueprint:members");
	const FString ComponentId = TEXT("component:blueprint:DefaultSceneRoot");
	const FString ComponentPropertyId = TEXT("property:blueprint:component:Mobility");
	const FString NodeId = TEXT("node:blueprint:PrintString");
	const FString NodePropertyId = TEXT("property:blueprint:node:NodeComment");
	const FString PinDefaultId = TEXT("property:blueprint:node:InString");
	const FString ClassDefaultId = TEXT("property:blueprint:SpriteScale");
	TArray<FSemanticUnit> Units;
	const TSharedRef<FJsonObject> ExistingComponent = MakeShared<FJsonObject>();
	ExistingComponent->SetStringField(TEXT("operation"), TEXT("update"));
	ExistingComponent->SetStringField(TEXT("name"), TEXT("DefaultSceneRoot"));
	ExistingComponent->SetStringField(TEXT("class"), TEXT("SceneComponent"));
	const TSharedRef<FJsonObject> NewComponent = MakeShared<FJsonObject>();
	NewComponent->SetStringField(TEXT("operation"), TEXT("create"));
	const TSharedRef<FJsonObject> NewCandidate = MakeShared<FJsonObject>();
	NewCandidate->SetStringField(TEXT("name"), TEXT("PointLightComponent"));
	NewComponent->SetObjectField(TEXT("candidate"), NewCandidate);
	const TSharedRef<FJsonObject> ComponentRows = MakeShared<FJsonObject>();
	ComponentRows->SetObjectField(TEXT("exported-slot"), ExistingComponent);
	ComponentRows->SetObjectField(TEXT("SceneComponent1"), NewComponent);
	Units.Emplace(
		ComponentTreeId,
		ESemanticUnitKind::StructureState,
		TargetId,
		TEXT("uai.semantic.blueprint.component_tree"),
		TEXT("component_tree"),
		FString(),
		TMap<FString, FUeAgentCanonicalValueRef>{
			{TEXT("components"), Canonical(MakeShared<FJsonValueObject>(ComponentRows))},
		},
		TArray<FString>{ComponentId}, TArray<FString>{}, FString(), FString(),
		TArray<FString>{}, TArray<FString>{TEXT("edit_bundle")},
		TEXT("fixture:semantic-projection"));
	const TSharedRef<FJsonObject> VariableType = MakeShared<FJsonObject>();
	VariableType->SetStringField(TEXT("pin_category"), TEXT("real"));
	VariableType->SetStringField(TEXT("pin_subcategory"), TEXT("float"));
	const TSharedRef<FJsonObject> VariableSpec = MakeShared<FJsonObject>();
	VariableSpec->SetObjectField(TEXT("type"), VariableType);
	const TSharedRef<FJsonObject> Variables = MakeShared<FJsonObject>();
	Variables->SetObjectField(TEXT("ApproachSign"), VariableSpec);
	Units.Emplace(
		MemberCollectionId,
		ESemanticUnitKind::StructureState,
		TargetId,
		TEXT("uai.authoring.surface.member_collection"),
		TEXT("member_collection"),
		FString(),
		TMap<FString, FUeAgentCanonicalValueRef>{
			{TEXT("variables"), Canonical(MakeShared<FJsonValueObject>(Variables))},
		},
		TArray<FString>{}, TArray<FString>{}, FString(), FString(),
		TArray<FString>{}, TArray<FString>{TEXT("edit_bundle")},
		TEXT("fixture:semantic-projection"));
	Units.Emplace(
		GraphId,
		ESemanticUnitKind::StructureState,
		TargetId,
		TEXT("uai.semantic.blueprint.graph"),
		TEXT("event_graph"),
		FString(),
		MoveTemp(GraphValues),
		TArray<FString>{NodeId},
		TArray<FString>{},
		FString(),
		FString(),
		TArray<FString>{},
		TArray<FString>{TEXT("edit_bundle")},
		TEXT("fixture:semantic-projection"),
		IdentityKey(
			UeAgentBlueprintAuthoring::GraphIdentityModelId,
			TEXT("blueprint_graph"),
			GraphSelectors));
	Units.Emplace(
		ComponentId,
		ESemanticUnitKind::ItemState,
		ComponentTreeId,
		UeAgentBlueprintAuthoring::ComponentSemanticId,
		TEXT("component_tree"),
		FString(),
		TMap<FString, FUeAgentCanonicalValueRef>{
			{TEXT("name"), Canonical(MakeShared<FJsonValueString>(TEXT("DefaultSceneRoot")))},
		},
		TArray<FString>{ComponentPropertyId},
		TArray<FString>{},
		FString(), FString(), TArray<FString>{},
		TArray<FString>{TEXT("edit_bundle")},
		TEXT("fixture:semantic-projection"),
		IdentityKey(
			UeAgentBlueprintAuthoring::ComponentIdentityModelId,
			TEXT("blueprint_component"),
			ComponentSelectors));
	TMap<FString, FString> ComponentPropertySelectors = ComponentSelectors;
	ComponentPropertySelectors.Add(TEXT("property_path"), TEXT("Mobility"));
	Units.Emplace(
		ComponentPropertyId,
		ESemanticUnitKind::PropertySet,
		ComponentId,
		UeAgentBlueprintAuthoring::ComponentPropertySemanticId,
		TEXT("component_tree"),
		TEXT("Mobility"),
		TMap<FString, FUeAgentCanonicalValueRef>{
			{TEXT("selected"), Canonical(MakeShared<FJsonValueString>(TEXT("Movable")))},
		},
		TArray<FString>{}, TArray<FString>{}, FString(), FString(),
		TArray<FString>{}, TArray<FString>{TEXT("edit_bundle")},
		TEXT("fixture:semantic-projection"),
		IdentityKey(
			UeAgentBlueprintAuthoring::ComponentPropertyIdentityModelId,
			TEXT("blueprint_component_property"),
			ComponentPropertySelectors));
	Units.Emplace(
		NodeId,
		ESemanticUnitKind::ItemState,
		GraphId,
		UeAgentBlueprintAuthoring::NodeSemanticId,
		TEXT("event_graph"),
		FString(),
		TMap<FString, FUeAgentCanonicalValueRef>{},
		TArray<FString>{NodePropertyId, PinDefaultId}, TArray<FString>{}, FString(), FString(),
		TArray<FString>{}, TArray<FString>{TEXT("edit_bundle")},
		TEXT("fixture:semantic-projection"),
		IdentityKey(
			UeAgentBlueprintAuthoring::NodeIdentityModelId,
			TEXT("blueprint_node"),
			NodeSelectors));
	TMap<FString, FString> NodePropertySelectors = NodeSelectors;
	NodePropertySelectors.Add(TEXT("property_path"), TEXT("NodeComment"));
	Units.Emplace(
		NodePropertyId,
		ESemanticUnitKind::PropertySet,
		NodeId,
		UeAgentBlueprintAuthoring::NodePropertySemanticId,
		TEXT("event_graph"),
		TEXT("NodeComment"),
		TMap<FString, FUeAgentCanonicalValueRef>{
			{TEXT("literal"), Canonical(MakeShared<FJsonValueString>(TEXT("Verified")))},
		},
		TArray<FString>{}, TArray<FString>{}, FString(), FString(),
		TArray<FString>{}, TArray<FString>{TEXT("edit_bundle")},
		TEXT("fixture:semantic-projection"),
		IdentityKey(
			UeAgentBlueprintAuthoring::NodePropertyIdentityModelId,
			TEXT("blueprint_node_property"),
			NodePropertySelectors));
	Units.Emplace(
		PinDefaultId,
		ESemanticUnitKind::PropertySet,
		NodeId,
		UeAgentBlueprintAuthoring::PinDefaultSemanticId,
		TEXT("event_graph"),
		TEXT("pin_default.TextColor"),
		TMap<FString, FUeAgentCanonicalValueRef>{
			{TEXT("r"), Canonical(MakeShared<FJsonValueNumber>(0.25))},
			{TEXT("g"), Canonical(MakeShared<FJsonValueNumber>(0.5))},
			{TEXT("b"), Canonical(MakeShared<FJsonValueNumber>(0.75))},
			{TEXT("a"), Canonical(MakeShared<FJsonValueNumber>(1.0))},
		},
		TArray<FString>{}, TArray<FString>{}, FString(), FString(),
		TArray<FString>{}, TArray<FString>{TEXT("edit_bundle")},
		TEXT("fixture:semantic-projection"),
		IdentityKey(
			UeAgentBlueprintAuthoring::PinDefaultIdentityModelId,
			TEXT("blueprint_pin_default"),
			PinSelectors));
	Units.Emplace(
		ClassDefaultId,
		ESemanticUnitKind::PropertySet,
		TargetId,
		TEXT("uai.semantic.blueprint.class_default.property"),
		TEXT("class_defaults"),
		TEXT("SpriteScale"),
		TMap<FString, FUeAgentCanonicalValueRef>{
			{TEXT("literal"), Canonical(MakeShared<FJsonValueNumber>(1.25))}},
		TArray<FString>{},
		TArray<FString>{},
		FString(),
		FString(),
		TArray<FString>{},
		TArray<FString>{TEXT("edit_bundle")},
		TEXT("fixture:semantic-projection"),
		TEXT("fixture:class-default:SpriteScale"));
	Units.Emplace(
		TargetId,
		ESemanticUnitKind::TargetState,
		FString(),
		TEXT("ue.blueprint.asset"),
		TEXT("authoring_target"),
		FString(),
		TMap<FString, FUeAgentCanonicalValueRef>{},
		TArray<FString>{GraphId, ComponentTreeId, MemberCollectionId, ClassDefaultId},
		TArray<FString>{},
		FString(),
		FString(),
		TArray<FString>{},
		TArray<FString>{TEXT("edit_bundle")},
		TEXT("fixture:semantic-projection"));

	FAuthoringModelPtr Model;
	FWorkspaceError Error;
	if (!TestTrue(
		TEXT("the semantic Blueprint fixture compiles"),
		TryCompileAuthoringModel(
			TEXT("uai.fixture.blueprint.semantic_projection"),
			1,
			TEXT("edit_bundle"),
			TEXT("fixture-revision-1"),
			MoveTemp(Units),
			TArray<FString>{TargetId},
			Resources,
			Model,
			Error)))
	{
		AddError(Error.GetCode());
		return false;
	}

	TSharedPtr<FJsonObject> Extension;
	if (!TestTrue(
		TEXT("model-friendly graph values project into provider_extension"),
		UeAgentBlueprintAuthoring::TryProjectSemanticProviderExtension(
			Model.ToSharedRef(),
			TArray<FString>{GraphId},
			{},
			{},
			Extension,
			Error)))
	{
		AddError(Error.GetCode());
		return false;
	}
	const TSharedPtr<FJsonObject>* Graphs = nullptr;
	TestTrue(
		TEXT("projection owns one graphs map"),
		Extension.IsValid()
			&& Extension->TryGetObjectField(TEXT("graphs"), Graphs)
			&& Graphs && (*Graphs)->Values.Num() == 1);
	if (!Graphs || (*Graphs)->Values.IsEmpty())
	{
		return false;
	}
	auto GraphIterator = (*Graphs)->Values.CreateConstIterator();
	const TSharedPtr<FJsonObject> GraphSpec = GraphIterator.Value()->AsObject();
	TestEqual(
		TEXT("an exported graph defaults to update"),
		GraphSpec->GetStringField(TEXT("operation")),
		FString(TEXT("update")));
	const TSharedPtr<FJsonObject>* ProjectedNodes = nullptr;
	TestTrue(
		TEXT("node array becomes the stable provider node map"),
		GraphSpec->TryGetObjectField(TEXT("nodes"), ProjectedNodes)
			&& ProjectedNodes && (*ProjectedNodes)->Values.Num() == 1);
	if (ProjectedNodes && !(*ProjectedNodes)->Values.IsEmpty())
	{
		auto NodeIterator = (*ProjectedNodes)->Values.CreateConstIterator();
		const TSharedPtr<FJsonObject> NodeSpec = NodeIterator.Value()->AsObject();
		const TSharedPtr<FJsonObject>* Candidate = nullptr;
		TestTrue(
			TEXT("a name-only model selector becomes a provider candidate"),
			NodeSpec->TryGetObjectField(TEXT("candidate"), Candidate)
				&& Candidate
				&& (*Candidate)->GetStringField(TEXT("name"))
					== TEXT("Print String")
				&& !(*Candidate)->HasField(TEXT("id")));
		TestEqual(
			TEXT("node defaults to create"),
			NodeSpec->GetStringField(TEXT("operation")),
			FString(TEXT("create")));
		TestFalse(
			TEXT("semantic projection leaves candidate kind to the candidate resolver"),
			NodeSpec->HasField(TEXT("node_type")));
	}

	Extension.Reset();
	Error.Reset();
	TestTrue(
		TEXT("an edited existing node projects through its persisted GUID identity"),
		UeAgentBlueprintAuthoring::TryProjectSemanticProviderExtension(
			Model.ToSharedRef(), TArray<FString>{NodeId}, {}, {}, Extension, Error));
	const TSharedPtr<FJsonObject>* ExistingNodeGraphs = nullptr;
	TestTrue(
		TEXT("an edited existing node retains one provider graph"),
		Extension.IsValid()
			&& Extension->TryGetObjectField(TEXT("graphs"), ExistingNodeGraphs)
			&& ExistingNodeGraphs && (*ExistingNodeGraphs)->Values.Num() == 1);
	if (ExistingNodeGraphs && (*ExistingNodeGraphs)->Values.Num() == 1)
	{
		auto ExistingNodeGraphIterator = (*ExistingNodeGraphs)->Values.CreateConstIterator();
		const TSharedPtr<FJsonObject>* ExistingNodes = nullptr;
		TestTrue(
			TEXT("an edited existing node shares the provider node map with graph-authored nodes"),
			ExistingNodeGraphIterator.Value()->AsObject()->TryGetObjectField(
				TEXT("nodes"), ExistingNodes)
				&& ExistingNodes && (*ExistingNodes)->Values.Num() == 2);
		if (ExistingNodes)
		{
			FGuid ExpectedNodeGuid;
			check(FGuid::Parse(NodeGuid, ExpectedNodeGuid));
			const FString ExpectedNodeKey =
				ExpectedNodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
			const TSharedPtr<FJsonObject>* ExistingNodeSpec = nullptr;
			TestTrue(
				TEXT("an edited existing node keeps the exact persisted GUID key"),
				(*ExistingNodes)->TryGetObjectField(ExpectedNodeKey, ExistingNodeSpec)
					&& ExistingNodeSpec);
			if (ExistingNodeSpec)
			{
				TestEqual(
					TEXT("an edited existing node materializes the persisted GUID in its mutation spec"),
					(*ExistingNodeSpec)->GetStringField(TEXT("node_guid")),
					NodeGuid);
			}
		}
	}

	Extension.Reset();
	Error.Reset();
	TestTrue(
		TEXT("component-tree desired state projects into provider components"),
		UeAgentBlueprintAuthoring::TryProjectSemanticProviderExtension(
			Model.ToSharedRef(), TArray<FString>{ComponentTreeId}, {}, {},
			Extension, Error));
	const TSharedPtr<FJsonObject>* DesiredComponents = nullptr;
	TestTrue(
		TEXT("all desired component rows are retained"),
		Extension.IsValid()
			&& Extension->TryGetObjectField(TEXT("components"), DesiredComponents)
			&& DesiredComponents && (*DesiredComponents)->Values.Num() == 2);
	if (DesiredComponents)
	{
		const TSharedPtr<FJsonObject>* ExistingSpec = nullptr;
		TestTrue(
			TEXT("existing component update uses its top-level instance name"),
			(*DesiredComponents)->TryGetObjectField(
				TEXT("component:DefaultSceneRoot"), ExistingSpec)
				&& ExistingSpec
				&& (*ExistingSpec)->GetStringField(TEXT("class"))
					== TEXT("SceneComponent"));
		const TSharedPtr<FJsonObject>* NewSpec = nullptr;
		TestTrue(
			TEXT("new component uses the local map key as its stable instance key"),
			(*DesiredComponents)->TryGetObjectField(
				TEXT("component:SceneComponent1"), NewSpec)
				&& NewSpec);
		if (NewSpec)
		{
			const TSharedPtr<FJsonObject>* Candidate = nullptr;
			TestTrue(
				TEXT("minimal candidate selector is preserved"),
				(*NewSpec)->TryGetObjectField(TEXT("candidate"), Candidate)
					&& Candidate
					&& (*Candidate)->GetStringField(TEXT("name"))
						== TEXT("PointLightComponent"));
			TestFalse(
				TEXT("candidate display name is not forced into the instance name"),
				(*NewSpec)->HasField(TEXT("name")));
		}
	}

	Extension.Reset();
	Error.Reset();
	TestTrue(
		TEXT("member collection desired state projects into provider members"),
		UeAgentBlueprintAuthoring::TryProjectSemanticProviderExtension(
			Model.ToSharedRef(), TArray<FString>{MemberCollectionId}, {}, {},
			Extension, Error));
	const TSharedPtr<FJsonObject>* ProjectedMembers = nullptr;
	const TSharedPtr<FJsonObject>* ProjectedVariables = nullptr;
	const TSharedPtr<FJsonObject>* ProjectedApproachSign = nullptr;
	TestTrue(
		TEXT("member variables retain readable names and pin types"),
		Extension.IsValid()
			&& Extension->TryGetObjectField(TEXT("members"), ProjectedMembers)
			&& ProjectedMembers
			&& (*ProjectedMembers)->TryGetObjectField(
				TEXT("variables"), ProjectedVariables)
			&& ProjectedVariables
			&& (*ProjectedVariables)->TryGetObjectField(
				TEXT("ApproachSign"), ProjectedApproachSign)
			&& ProjectedApproachSign
			&& (*ProjectedApproachSign)->HasTypedField<EJson::Object>(TEXT("type")));

	const TSharedRef<FJsonObject> ClassDefaultDescriptor = MakeShared<FJsonObject>();
	ClassDefaultDescriptor->SetStringField(TEXT("property_path"), TEXT("SpriteScale"));
	ClassDefaultDescriptor->SetNumberField(TEXT("value"), 1.25);
	Extension.Reset();
	Error.Reset();
	TestTrue(
		TEXT("class-default semantic properties use the existing provider extension"),
		UeAgentBlueprintAuthoring::TryProjectSemanticProviderExtension(
			Model.ToSharedRef(),
			TArray<FString>{ClassDefaultId},
			{{ClassDefaultId, MakeShared<FJsonValueObject>(ClassDefaultDescriptor)}},
			{},
			Extension,
			Error));
	const TArray<TSharedPtr<FJsonValue>>* ClassDefaults = nullptr;
	TestTrue(
		TEXT("class-default projection owns one public property descriptor"),
		Extension.IsValid()
			&& Extension->TryGetArrayField(TEXT("class_defaults"), ClassDefaults)
			&& ClassDefaults && ClassDefaults->Num() == 1);
	if (ClassDefaults && ClassDefaults->Num() == 1)
	{
		const TSharedPtr<FJsonObject> Descriptor = (*ClassDefaults)[0]->AsObject();
		TestEqual(
			TEXT("class-default property path is preserved"),
			Descriptor->GetStringField(TEXT("property_path")),
			FString(TEXT("SpriteScale")));
		TestEqual(
			TEXT("class-default property value is preserved"),
			Descriptor->GetNumberField(TEXT("value")),
			1.25);
	}

	const TSharedRef<FJsonObject> ComponentValue = MakeShared<FJsonObject>();
	ComponentValue->SetStringField(TEXT("selected"), TEXT("Movable"));
	const TSharedRef<FJsonObject> ComponentDescriptor = MakeShared<FJsonObject>();
	ComponentDescriptor->SetStringField(TEXT("property_path"), TEXT("Mobility"));
	ComponentDescriptor->SetObjectField(TEXT("value"), ComponentValue);
	const TSharedRef<FJsonObject> NodeValue = MakeShared<FJsonObject>();
	NodeValue->SetStringField(TEXT("literal"), TEXT("Verified"));
	const TSharedRef<FJsonObject> NodeDescriptor = MakeShared<FJsonObject>();
	NodeDescriptor->SetStringField(TEXT("property_path"), TEXT("NodeComment"));
	NodeDescriptor->SetObjectField(TEXT("value"), NodeValue);
	Extension.Reset();
	Error.Reset();
	TestTrue(
		TEXT("a component-only property update retains its GUID identity"),
		UeAgentBlueprintAuthoring::TryProjectSemanticProviderExtension(
			Model.ToSharedRef(),
			TArray<FString>{ComponentPropertyId},
			{{ComponentPropertyId, MakeShared<FJsonValueObject>(ComponentDescriptor)}},
			{},
			Extension,
			Error));
	const TSharedPtr<FJsonObject>* GuidOnlyComponents = nullptr;
	FGuid ExpectedComponentGuid;
	check(FGuid::Parse(ComponentGuid, ExpectedComponentGuid));
	const FString ExpectedComponentGuidKey =
		ExpectedComponentGuid.ToString(EGuidFormats::DigitsWithHyphens);
	TestTrue(
		TEXT("a component-only property update uses the persisted GUID key"),
		Extension.IsValid()
			&& Extension->TryGetObjectField(TEXT("components"), GuidOnlyComponents)
			&& GuidOnlyComponents
			&& (*GuidOnlyComponents)->Values.Num() == 1
			&& (*GuidOnlyComponents)->HasField(ExpectedComponentGuidKey));

	Extension.Reset();
	Error.Reset();
	TestTrue(
		TEXT("component and node properties reuse the existing provider extension"),
		UeAgentBlueprintAuthoring::TryProjectSemanticProviderExtension(
			Model.ToSharedRef(),
			TArray<FString>{ComponentTreeId, ComponentPropertyId, NodePropertyId},
			{
				{ComponentPropertyId, MakeShared<FJsonValueObject>(ComponentDescriptor)},
				{NodePropertyId, MakeShared<FJsonValueObject>(NodeDescriptor)},
			},
			{},
			Extension,
			Error));
	const TSharedPtr<FJsonObject>* Components = nullptr;
	TestTrue(
		TEXT("component property merges with its exported readable component row"),
		Extension.IsValid()
			&& Extension->TryGetObjectField(TEXT("components"), Components)
			&& Components && (*Components)->Values.Num() == 2);
	if (Components && (*Components)->Values.Num() == 2)
	{
		const TSharedPtr<FJsonObject>* ComponentSpecPtr = nullptr;
		TestTrue(TEXT("property descriptor merges with the desired alias component"),
			(*Components)->TryGetObjectField(
				TEXT("component:DefaultSceneRoot"), ComponentSpecPtr)
				&& ComponentSpecPtr);
		const TSharedPtr<FJsonObject> ComponentSpec = ComponentSpecPtr
			? *ComponentSpecPtr : MakeShared<FJsonObject>();
		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		TestTrue(
			TEXT("component property uses the common property descriptor list"),
			ComponentSpec->TryGetArrayField(TEXT("properties"), Properties)
				&& Properties && Properties->Num() == 1);
		TestEqual(
			TEXT("component update keeps the component name required by the provider"),
			ComponentSpec->GetStringField(TEXT("name")),
			FString(TEXT("DefaultSceneRoot")));
	}
	const TSharedPtr<FJsonObject>* PropertyGraphs = nullptr;
	TestTrue(
		TEXT("node property projects under one stable graph"),
		Extension.IsValid()
			&& Extension->TryGetObjectField(TEXT("graphs"), PropertyGraphs)
			&& PropertyGraphs && (*PropertyGraphs)->Values.Num() == 1);
	if (PropertyGraphs && (*PropertyGraphs)->Values.Num() == 1)
	{
		auto PropertyGraphIterator = (*PropertyGraphs)->Values.CreateConstIterator();
		const TSharedPtr<FJsonObject> PropertyGraphSpec =
			PropertyGraphIterator.Value()->AsObject();
		const TSharedPtr<FJsonObject>* PropertyNodes = nullptr;
		TestTrue(
			TEXT("node property uses the provider graph-node nesting"),
			PropertyGraphSpec->TryGetObjectField(TEXT("nodes"), PropertyNodes)
				&& PropertyNodes && (*PropertyNodes)->Values.Num() == 1);
		if (PropertyNodes && (*PropertyNodes)->Values.Num() == 1)
		{
			auto PropertyNodeIterator = (*PropertyNodes)->Values.CreateConstIterator();
			FGuid ExpectedGuid;
			check(FGuid::Parse(NodeGuid, ExpectedGuid));
			TestEqual(
				TEXT("node property keeps the exported GUID key"),
				PropertyNodeIterator.Key(),
				ExpectedGuid.ToString(EGuidFormats::DigitsWithHyphens));
			const TSharedPtr<FJsonObject> PropertyNodeSpec =
				PropertyNodeIterator.Value()->AsObject();
			TestEqual(
				TEXT("node property materializes the persisted node GUID"),
				PropertyNodeSpec->GetStringField(TEXT("node_guid")),
				NodeGuid);
			const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
			TestTrue(
				TEXT("node property uses the common property descriptor list"),
				PropertyNodeSpec->TryGetArrayField(TEXT("properties"), Properties)
					&& Properties && Properties->Num() == 1);
		}
	}

	Extension.Reset();
	Error.Reset();
	const bool bPinDefaultProjected =
		UeAgentBlueprintAuthoring::TryProjectSemanticProviderExtension(
			Model.ToSharedRef(),
			TArray<FString>{PinDefaultId},
			{},
			{},
			Extension,
			Error);
	TestTrue(
		TEXT("pin-default semantic values reuse the existing provider extension"),
		bPinDefaultProjected);
	if (!bPinDefaultProjected)
	{
		AddError(Error.GetCode());
	}
	const TSharedPtr<FJsonObject>* PinGraphs = nullptr;
	TestTrue(
		TEXT("pin default projects under one stable graph"),
		Extension.IsValid()
			&& Extension->TryGetObjectField(TEXT("graphs"), PinGraphs)
			&& PinGraphs && (*PinGraphs)->Values.Num() == 1);
	if (PinGraphs && (*PinGraphs)->Values.Num() == 1)
	{
		auto PinGraphIterator = (*PinGraphs)->Values.CreateConstIterator();
		const TSharedPtr<FJsonObject> PinGraphSpec =
			PinGraphIterator.Value()->AsObject();
		const TSharedPtr<FJsonObject>* PinNodes = nullptr;
		TestTrue(
			TEXT("pin default keeps the provider graph-node nesting"),
			PinGraphSpec->TryGetObjectField(TEXT("nodes"), PinNodes)
				&& PinNodes && (*PinNodes)->Values.Num() == 1);
		if (PinNodes && (*PinNodes)->Values.Num() == 1)
		{
			auto PinNodeIterator = (*PinNodes)->Values.CreateConstIterator();
			const TSharedPtr<FJsonObject> PinNodeSpec =
				PinNodeIterator.Value()->AsObject();
			TestEqual(
				TEXT("pin default materializes the persisted node GUID"),
				PinNodeSpec->GetStringField(TEXT("node_guid")),
				NodeGuid);
			const TSharedPtr<FJsonObject>* PinDefaults = nullptr;
			TestTrue(
				TEXT("pin default enters the existing pin_defaults mechanics"),
				PinNodeSpec->TryGetObjectField(TEXT("pin_defaults"), PinDefaults)
					&& PinDefaults && (*PinDefaults)->Values.Num() == 1);
			if (PinDefaults && (*PinDefaults)->Values.Num() == 1)
			{
				auto PinDefaultIterator = (*PinDefaults)->Values.CreateConstIterator();
				const TSharedPtr<FJsonObject> PinSpec =
					PinDefaultIterator.Value()->AsObject();
				TestEqual(TEXT("pin semantic name is retained"),
					PinSpec->GetStringField(TEXT("pin_name")), FString(TEXT("TextColor")));
				TestEqual(TEXT("pin occurrence is retained"),
					PinSpec->GetNumberField(TEXT("pin_occurrence")), 0.0);
				TestEqual(TEXT("direct struct value becomes schema default text"),
					PinSpec->GetStringField(TEXT("default_value")),
					FString(TEXT("(R=0.25,G=0.5,B=0.75,A=1)")));
			}
		}
	}

	const FStructuralEdit SetOrder(
		TEXT("order:graph"),
		EStructuralEditKind::SetOrder,
		GraphId,
		FString(),
		FString(),
		FString(),
		{TEXT("node:a"), TEXT("node:b")});
	Extension.Reset();
	Error.Reset();
	TestFalse(
		TEXT("unsupported ordering is never silently discarded"),
		UeAgentBlueprintAuthoring::TryProjectSemanticProviderExtension(
			Model.ToSharedRef(),
			TArray<FString>{GraphId},
			{},
			{SetOrder},
			Extension,
			Error));
	TestEqual(
		TEXT("ordering reports the precise unsupported contract"),
		Error.GetCode(),
		FString(TEXT("blueprint_semantic_set_order_unsupported")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentBlueprintPinDefaultValueProjectionTest,
	"UeAgentInterface.Domains.Blueprint.Authoring.PinDefaultValueProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentBlueprintPinDefaultValueProjectionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	auto Describe = [this](
		const FEdGraphPinType& PinType,
		const FString& DefaultValue,
		const FString& ExpectedKind,
		TSharedPtr<FJsonValue>& OutValue)
	{
		UeAgentAuthoringTypeDescriptors::FDescriptor Descriptor;
		FString Error;
		const bool bDescribed =
			UeAgentBlueprintAuthoringOps::TryDescribePinDefault(
				PinType, DefaultValue, Descriptor, OutValue, Error);
		TestTrue(TEXT("pin default describes through the Blueprint type owner"), bDescribed);
		TestEqual(TEXT("pin default keeps its precise public type"),
			Descriptor.Kind, ExpectedKind);
		if (!bDescribed) AddError(Error);
		return Descriptor;
	};

	FEdGraphPinType BoolType;
	BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	TSharedPtr<FJsonValue> BoolValue;
	Describe(BoolType, TEXT("true"), TEXT("boolean"), BoolValue);
	TestTrue(TEXT("bool default becomes a direct boolean"),
		BoolValue.IsValid() && BoolValue->Type == EJson::Boolean && BoolValue->AsBool());

	FEdGraphPinType FloatType;
	FloatType.PinCategory = UEdGraphSchema_K2::PC_Real;
	FloatType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
	TSharedPtr<FJsonValue> FloatValue;
	Describe(FloatType, TEXT("2.5"), TEXT("float"), FloatValue);
	TestTrue(TEXT("float default becomes a direct number"),
		FloatValue.IsValid() && FloatValue->Type == EJson::Number);

	FEdGraphPinType StringType;
	StringType.PinCategory = UEdGraphSchema_K2::PC_String;
	TSharedPtr<FJsonValue> StringValue;
	Describe(StringType, TEXT("T165"), TEXT("string"), StringValue);
	TestTrue(TEXT("string default becomes a direct string"),
		StringValue.IsValid() && StringValue->Type == EJson::String
			&& StringValue->AsString() == TEXT("T165"));

	FEdGraphPinType ColorType;
	ColorType.PinCategory = UEdGraphSchema_K2::PC_Struct;
	ColorType.PinSubCategoryObject = TBaseStructure<FLinearColor>::Get();
	TSharedPtr<FJsonValue> ColorValue;
	const UeAgentAuthoringTypeDescriptors::FDescriptor ColorDescriptor =
		Describe(ColorType, TEXT("(R=1,G=0.5,B=0.25,A=1)"), TEXT("linear_color"), ColorValue);
	TestEqual(TEXT("LinearColor keeps its struct identity"),
		ColorDescriptor.StructPath, FString(TEXT("/Script/CoreUObject.LinearColor")));
	const TSharedPtr<FJsonObject> ColorObject =
		ColorValue.IsValid() && ColorValue->Type == EJson::Object
			? ColorValue->AsObject() : nullptr;
	TestTrue(TEXT("LinearColor default uses the shared struct codec"),
		ColorObject.IsValid()
			&& ColorObject->GetArrayField(TEXT("components")).Num() == 4
			&& ColorObject->GetArrayField(TEXT("components"))[0]->AsNumber() == 1.0
			&& ColorObject->GetArrayField(TEXT("components"))[1]->AsNumber() == 0.5
			&& ColorObject->GetArrayField(TEXT("components"))[2]->AsNumber() == 0.25
			&& ColorObject->GetArrayField(TEXT("components"))[3]->AsNumber() == 1.0);

	FEdGraphPinType ObjectType;
	ObjectType.PinCategory = UEdGraphSchema_K2::PC_Object;
	TSharedPtr<FJsonValue> ObjectValue;
	Describe(ObjectType, TEXT("self"), TEXT("string"), ObjectValue);
	TestTrue(TEXT("object defaults retain their schema text for round-trip"),
		ObjectValue.IsValid() && ObjectValue->Type == EJson::String
			&& ObjectValue->AsString() == TEXT("self"));

	FEdGraphPinType RotatorType;
	RotatorType.PinCategory = UEdGraphSchema_K2::PC_Struct;
	RotatorType.PinSubCategoryObject = TBaseStructure<FRotator>::Get();
	TSharedPtr<FJsonValue> RotatorValue;
	Describe(
		RotatorType,
		TEXT("(Pitch=0.000000,Yaw=90.000000,Roll=0.000000)"),
		TEXT("rotator"),
		RotatorValue);
	TestTrue(TEXT("Rotator defaults use typed components"),
		RotatorValue.IsValid() && RotatorValue->Type == EJson::Object
			&& RotatorValue->AsObject()->GetArrayField(TEXT("components")).Num() == 3
			&& RotatorValue->AsObject()->GetArrayField(TEXT("components"))[1]->AsNumber() == 90.0);
	return true;
}
