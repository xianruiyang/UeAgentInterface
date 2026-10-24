#include "Misc/AutomationTest.h"

#include "Containers/StringConv.h"
#include "Core/ArtifactPublisher/UeAgentArtifactPublisher.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceResolver.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

namespace
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::ArtifactPublisher;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::StateSemantic;

	FUeAgentCanonicalValueRef StringValue(
		const FString& Text,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryString(Text, Resources, Value, Error));
		return Value.ToSharedRef();
	}

	FUeAgentCanonicalValueRef IntegerValue(
		const int32 Number,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryInteger(
			LexToString(Number), Resources, Value, Error));
		return Value.ToSharedRef();
	}

	FUeAgentCanonicalValueRef DecimalValue(
		const FString& Number,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryDecimal(
			Number, Resources, Value, Error));
		return Value.ToSharedRef();
	}

	FUeAgentCanonicalValueRef ObjectValue(
		TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryObject(Fields, Resources, Value, Error));
		return Value.ToSharedRef();
	}

	FUeAgentCanonicalValueRef ArrayValue(
		TArray<FUeAgentCanonicalValueRef> Items,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryArray(Items, Resources, Value, Error));
		return Value.ToSharedRef();
	}

	const FUeAgentCanonicalValue* FindField(
		const FUeAgentCanonicalValue& Object,
		const FString& Name)
	{
		for (const auto& Field : Object.GetObjectItems())
		{
			if (Field.Key == Name)
			{
				return &Field.Value.Get();
			}
		}
		return nullptr;
	}

	FString ReadString(
		const FUeAgentCanonicalValue& Object,
		const FString& Name)
	{
		const FUeAgentCanonicalValue* Value = FindField(Object, Name);
		return Value != nullptr ? Value->GetText() : FString();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphAuthoringNativeContextTest,
	"UeAgentInterface.Core.HjsonWorkspace.GraphAuthoring.NativeContext",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphAuthoringNativeContextTest::RunTest(const FString& Parameters)
{
	auto Resources = MakeSemanticWorkspaceResourceTracker();
	const auto CapturedNodes = ObjectValue({
		{TEXT("Parent"), ObjectValue({{TEXT("id"), StringValue(TEXT("p"), Resources)},
			{TEXT("name"), StringValue(TEXT("Parent"), Resources)}}, Resources)},
		{TEXT("Child"), ObjectValue({{TEXT("id"), StringValue(TEXT("c"), Resources)},
			{TEXT("name"), StringValue(TEXT("Child"), Resources)},
			{TEXT("parent"), StringValue(TEXT("p"), Resources)}}, Resources)}}, Resources);
	const auto Captured = ObjectValue({{TEXT("nodes"), CapturedNodes},
		{TEXT("edges"), ObjectValue({}, Resources)}}, Resources);
	const auto Visible = [&](const FString& ChildName) {
		return ObjectValue({{TEXT("nodes"), ObjectValue({{TEXT("Child"), ObjectValue({
			{TEXT("name"), StringValue(ChildName, Resources)},
			{TEXT("parent"), StringValue(TEXT("Parent"), Resources)}}, Resources)}}, Resources)},
			{TEXT("connections"), ArrayValue({}, Resources)}}, Resources);
	};
	TMap<FString, FUeAgentCanonicalValueRef> Resolved;
	FWorkspaceError Error;
	if (TestTrue(TEXT("unchanged owned child follows deleted owner"), TryResolveGraphAuthoringProjection(
		Visible(TEXT("Child")).Get(), Captured.Get(), TEXT("graph"), Resources, Resolved, Error)))
	{
		TestEqual(TEXT("child becomes remove operation"), ReadString(*FindField(Resolved.FindChecked(TEXT("nodes")).Get(),
			TEXT("Child")), TEXT("operation")), FString(TEXT("remove")));
	}
	Resolved.Reset(); Error.Reset();
	TestFalse(TEXT("explicit edit to removed owner's child is not discarded"), TryResolveGraphAuthoringProjection(
		Visible(TEXT("Edited child")).Get(), Captured.Get(), TEXT("graph"), Resources, Resolved, Error));

	const auto Pins = [&](const FString& A, const FString& B) {
		return ObjectValue({{TEXT("A"), ObjectValue({{TEXT("default_value"), StringValue(A, Resources)}}, Resources)},
			{TEXT("B"), ObjectValue({{TEXT("default_value"), StringValue(B, Resources)}}, Resources)}}, Resources);
	};
	const auto Node = [&](bool bOperation, const FString& B) {
		TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields = {
			{TEXT("id"), StringValue(TEXT("n"), Resources)}, {TEXT("pin_defaults"), Pins(TEXT("1"), B)}};
		if (bOperation) Fields.Add({TEXT("operation"), StringValue(TEXT("update"), Resources)});
		return ObjectValue(Fields, Resources);
	};
	TMap<FString, FUeAgentCanonicalValueRef> Before = {{TEXT("nodes"), ObjectValue({{TEXT("N"), Node(false, TEXT("2"))}}, Resources)}};
	TMap<FString, FUeAgentCanonicalValueRef> After = {{TEXT("nodes"), ObjectValue({{TEXT("N"), Node(true, TEXT("2"))}}, Resources)}};
	TSharedPtr<FJsonObject> Changes;
	Error.Reset();
	if (TestTrue(TEXT("operation projection compares original nodes"), TryProjectGraphAuthoringNodeChanges(TEXT("graph"), Before, After, Changes, Error)))
		TestEqual(TEXT("unchanged nodes are context only"), Changes->Values.Num(), 0);
	After.Add(TEXT("nodes"), ObjectValue({{TEXT("N"), Node(true, TEXT("3"))}}, Resources));
	if (TestTrue(TEXT("explicit pin edit projects"), TryProjectGraphAuthoringNodeChanges(TEXT("graph"), Before, After, Changes, Error)))
	{
		const auto Defaults = Changes->GetObjectField(TEXT("N"))->GetObjectField(TEXT("pin_defaults"));
		TestFalse(TEXT("unchanged pin is not restored after reconstruction"), Defaults->HasField(TEXT("A")));
		TestTrue(TEXT("changed pin intent remains"), Defaults->HasField(TEXT("B")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphAuthoringNodeReferencesTest,
	"UeAgentInterface.Core.HjsonWorkspace.GraphAuthoring.NodeReferences",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphAuthoringNodeReferencesTest::RunTest(const FString& Parameters)
{
	auto Resources = MakeSemanticWorkspaceResourceTracker();
	const auto Captured = ObjectValue({
		{TEXT("graph"), ObjectValue({{TEXT("node_references"), ObjectValue({
			{TEXT("root"), StringValue(TEXT("native-a"), Resources)}}, Resources)}}, Resources)},
		{TEXT("nodes"), ObjectValue({{TEXT("A"), ObjectValue({
			{TEXT("id"), StringValue(TEXT("native-a"), Resources)},
			{TEXT("name"), StringValue(TEXT("A"), Resources)}}, Resources)}}, Resources)},
		{TEXT("edges"), ObjectValue({}, Resources)}}, Resources);
	const auto Visible = [&](const bool KeepNode, const FString& Slot, const FString& Reference)
	{
		TArray<TPair<FString, FUeAgentCanonicalValueRef>> References;
		if (!Slot.IsEmpty()) References.Add({Slot, StringValue(Reference, Resources)});
		TArray<TPair<FString, FUeAgentCanonicalValueRef>> Nodes;
		if (KeepNode) Nodes.Add({TEXT("A"), ObjectValue({
			{TEXT("name"), StringValue(TEXT("A"), Resources)}}, Resources)});
		return ObjectValue({
			{TEXT("graph"), ObjectValue({{TEXT("node_references"), ObjectValue(References, Resources)}}, Resources)},
			{TEXT("nodes"), ObjectValue(Nodes, Resources)},
			{TEXT("connections"), ArrayValue({}, Resources)}}, Resources);
	};
	TMap<FString, FUeAgentCanonicalValueRef> Resolved;
	FWorkspaceError Error;
	const auto Resolve = [&](const bool KeepNode, const FString& Slot, const FString& Reference)
	{
		Resolved.Reset();
		Error = FWorkspaceError();
		return TryResolveGraphAuthoringProjection(Visible(KeepNode, Slot, Reference).Get(),
			Captured.Get(), TEXT("graph"), Resources, Resolved, Error);
	};
	if (TestTrue(TEXT("visible root alias resolves"), Resolve(true, TEXT("root"), TEXT("A"))))
	{
		const auto* References = FindField(Resolved.FindChecked(TEXT("graph")).Get(), TEXT("node_references"));
		TestEqual(TEXT("root keeps native identity"), ReadString(*References, TEXT("root")), FString(TEXT("native-a")));
	}
	TestTrue(TEXT("omitted slot preserves captured root"), Resolve(true, TEXT(""), TEXT("")));
	if (TestTrue(TEXT("deleting referenced node clears unchanged nullable root"), Resolve(false, TEXT(""), TEXT(""))))
	{
		const auto* References = FindField(Resolved.FindChecked(TEXT("graph")).Get(), TEXT("node_references"));
		TestEqual(TEXT("deleted root is empty"), ReadString(*References, TEXT("root")), FString());
	}
	TestTrue(TEXT("unchanged visible root also follows deletion"), Resolve(false, TEXT("root"), TEXT("A")));
	TestTrue(TEXT("explicit empty root permits node deletion"), Resolve(false, TEXT("root"), TEXT("")));
	TestFalse(TEXT("undeclared reference slot rejects"), Resolve(true, TEXT("other"), TEXT("A")));
	TestFalse(TEXT("absent target alias rejects"), Resolve(true, TEXT("root"), TEXT("Missing")));

	TSharedPtr<FJsonValue> CapturedJson;
	FString JsonError;
	if (!TestTrue(TEXT("capture converts for export"), CanonicalJson::TryToJsonValue(Captured.Get(), CapturedJson, JsonError)))
		return false;
	const auto Compact = MakeShared<FJsonObject>();
	Compact->SetField(TEXT("values"), CapturedJson);
	TSharedRef<FJsonObject> Exported = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Interface;
	if (!TestTrue(TEXT("root reference exports"), TryProjectGraphAuthoringVisibleRow(
		TEXT("graph"), Compact, Exported, Interface, Error))) return false;
	TestEqual(TEXT("export uses readable alias"), Exported->GetObjectField(TEXT("graph"))
		->GetObjectField(TEXT("node_references"))->GetStringField(TEXT("root")), FString(TEXT("A")));
	TestEqual(TEXT("export does not mutate captured identity"), CapturedJson->AsObject()->GetObjectField(TEXT("graph"))
		->GetObjectField(TEXT("node_references"))->GetStringField(TEXT("root")), FString(TEXT("native-a")));

	const auto NewNode = MakeShared<FJsonObject>();
	NewNode->SetStringField(TEXT("name"), TEXT("Mixer"));
	Exported->GetObjectField(TEXT("nodes"))->SetObjectField(TEXT("NewMixer"), NewNode);
	Exported->GetObjectField(TEXT("graph"))->GetObjectField(TEXT("node_references"))->SetStringField(TEXT("root"), TEXT("NewMixer"));
	FUeAgentCanonicalValuePtr NewVisible;
	if (!TestTrue(TEXT("new node document converts"), CanonicalJson::TryFromJsonValue(
		MakeShared<FJsonValueObject>(Exported), Resources, NewVisible, JsonError))) return false;
	if (TestTrue(TEXT("same import can reference a new node"), TryResolveGraphAuthoringProjection(
		*NewVisible, Captured.Get(), TEXT("graph"), Resources, Resolved, Error)))
	{
		const auto* References = FindField(Resolved.FindChecked(TEXT("graph")).Get(), TEXT("node_references"));
		TestEqual(TEXT("new root uses new node local identity"), ReadString(*References, TEXT("root")), FString(TEXT("NewMixer")));
	}
	Exported->GetObjectField(TEXT("graph"))->GetObjectField(TEXT("node_references"))->SetNumberField(TEXT("root"), 1);
	if (!TestTrue(TEXT("invalid reference document converts"), CanonicalJson::TryFromJsonValue(
		MakeShared<FJsonValueObject>(Exported), Resources, NewVisible, JsonError))) return false;
	TestFalse(TEXT("non-string reference rejects"), TryResolveGraphAuthoringProjection(
		*NewVisible, Captured.Get(), TEXT("graph"), Resources, Resolved, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphAuthoringAliasProjectionTest,
	"UeAgentInterface.Core.HjsonWorkspace.GraphAuthoring.AliasProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphAuthoringAliasProjectionTest::RunTest(
	const FString& Parameters)
{
	FGraphAuthoringAliasProjection Projection;
	FWorkspaceError Error;
	TestTrue(
		TEXT("readable aliases and fallback aliases are projected"),
		TryBuildGraphAuthoringAliasProjection(
			{
				{TEXT("stable-b"), TEXT("sample-node")},
				{TEXT("stable-a"), TEXT("  sample node  ")},
				{TEXT("stable-c"), FString()},
			},
			Projection,
			Error));
	TestEqual(
		TEXT("first semantic alias needs no disambiguation suffix"),
		Projection.AliasBySourceId.FindRef(TEXT("stable-a")),
		FString(TEXT("SampleNode")));
	TestEqual(
		TEXT("duplicate readable names receive a deterministic occurrence"),
		Projection.AliasBySourceId.FindRef(TEXT("stable-b")),
		FString(TEXT("SampleNode#2")));
	TestEqual(
		TEXT("empty semantic name uses the first node fallback"),
		Projection.AliasBySourceId.FindRef(TEXT("stable-c")),
		FString(TEXT("Node")));
	TestEqual(
		TEXT("reverse map retains the stable source id"),
		Projection.SourceIdByAlias.FindRef(TEXT("SampleNode")),
		FString(TEXT("stable-a")));

	Projection = {};
	Error.Reset();
	TestFalse(
		TEXT("duplicate source identities are rejected"),
		TryBuildGraphAuthoringAliasProjection(
			{
				{TEXT("stable-a"), TEXT("sample node")},
				{TEXT("stable-a"), TEXT("other node")},
			},
			Projection,
			Error));
	TestEqual(
		TEXT("source collision has a structured workspace error"),
		Error.GetCode(),
		FString(TEXT("graph_authoring_source_id_collision")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphAuthoringPinAliasProjectionTest,
	"UeAgentInterface.Core.HjsonWorkspace.GraphAuthoring.PinAliasProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphAuthoringPinAliasProjectionTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FGraphAuthoringPinAliasProjection Projection;
	FWorkspaceError Error;
	TestTrue(
		TEXT("node-local Pin aliases are projected from semantic occurrence"),
		TryBuildGraphAuthoringPinAliasProjection(
			{
				{TEXT("pin-a"), TEXT("Value"), 0,
					EGraphAuthoringEndpointDirection::Input},
				{TEXT("pin-b"), TEXT("Value"), 1,
					EGraphAuthoringEndpointDirection::Input},
				{TEXT("pin-c"), FString(), 0,
					EGraphAuthoringEndpointDirection::Output},
			},
			Projection,
			Error));
	TestEqual(
		TEXT("the first semantic Pin keeps its readable name"),
		Projection.AliasBySourceId.FindRef(TEXT("pin-a")),
		FString(TEXT("Value")));
	TestEqual(
		TEXT("a repeated semantic Pin receives a readable occurrence"),
		Projection.AliasBySourceId.FindRef(TEXT("pin-b")),
		FString(TEXT("Value#2")));
	TestEqual(
		TEXT("an unnamed output receives the directional fallback"),
		Projection.AliasBySourceId.FindRef(TEXT("pin-c")),
		FString(TEXT("Output")));
	TestEqual(
		TEXT("the reverse projection remains machine-owned"),
		Projection.SourceIdByAlias.FindRef(TEXT("Value#2")),
		FString(TEXT("pin-b")));

	Projection = {};
	Error.Reset();
	TestTrue(
		TEXT("readable aliases that would collapse different Pins are disambiguated"),
		TryBuildGraphAuthoringPinAliasProjection(
			{
				{TEXT("pin-a"), TEXT("Value#2"), 0,
					EGraphAuthoringEndpointDirection::Input},
				{TEXT("pin-b"), TEXT("Value"), 1,
					EGraphAuthoringEndpointDirection::Input},
			},
			Projection,
			Error));
	TestEqual(
		TEXT("the colliding alias receives a short readable suffix"),
		Projection.AliasBySourceId.FindRef(TEXT("pin-b")),
		FString(TEXT("Value#3")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphAuthoringEndpointProjectionTest,
	"UeAgentInterface.Core.HjsonWorkspace.GraphAuthoring.EndpointProjection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphAuthoringEndpointProjectionTest::RunTest(
	const FString& Parameters)
{
	FString Endpoint;
	FWorkspaceError Error;
	TestTrue(
		TEXT("first named pin has no occurrence suffix"),
		TryFormatGraphAuthoringEndpoint(
			TEXT("SampleNode1"),
			TEXT("result value"),
			0,
			EGraphAuthoringEndpointDirection::Output,
			Endpoint,
			Error));
	TestEqual(
		TEXT("named endpoint preserves the domain pin spelling"),
		Endpoint,
		FString(TEXT("SampleNode1.result value")));

	TestTrue(
		TEXT("unnamed repeated input uses direction fallback"),
		TryFormatGraphAuthoringEndpoint(
			TEXT("Node2"),
			FString(),
			1,
			EGraphAuthoringEndpointDirection::Input,
			Endpoint,
			Error));
	TestEqual(
		TEXT("fallback occurrence is one-based"),
		Endpoint,
		FString(TEXT("Node2.Input#2")));

	FString NodeAlias;
	FString PinAlias;
	TestTrue(
		TEXT("formatted endpoint parses through the same common owner"),
		TryParseGraphAuthoringEndpoint(
			Endpoint, NodeAlias, PinAlias, Error));
	TestEqual(TEXT("parsed node alias"), NodeAlias, FString(TEXT("Node2")));
	TestEqual(TEXT("parsed Pin alias"), PinAlias, FString(TEXT("Input#2")));
	TestFalse(
		TEXT("endpoint without a Pin separator is rejected"),
		TryParseGraphAuthoringEndpoint(
			TEXT("Node2"), NodeAlias, PinAlias, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphAuthoringVisibleRowTest,
	"UeAgentInterface.Core.HjsonWorkspace.GraphAuthoring.VisibleRow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphAuthoringVisibleRowTest::RunTest(const FString& Parameters)
{
	const FString MachineEdgeKey =
		TEXT("edge:0123456789abcdef0123456789abcdef0123456789abcdef");
	TSharedRef<FJsonObject> Graph = MakeShared<FJsonObject>();
	Graph->SetStringField(TEXT("id"), TEXT("graph:machine-guid"));
	Graph->SetStringField(TEXT("guid"), TEXT("machine-guid"));
	Graph->SetStringField(TEXT("graph_guid"), TEXT("machine-guid"));
	Graph->SetStringField(TEXT("asset_path"), TEXT("/Game/Machine/Graph"));
	Graph->SetStringField(TEXT("operation"), TEXT("update"));
	Graph->SetStringField(TEXT("graph_kind"), TEXT("event"));
	Graph->SetStringField(TEXT("name"), TEXT("Readable Graph"));
	TSharedRef<FJsonObject> NodeA = MakeShared<FJsonObject>();
	NodeA->SetStringField(TEXT("id"), TEXT("node:machine-a"));
	NodeA->SetStringField(TEXT("expression_path"), TEXT("/Game/Machine/Expression"));
	NodeA->SetStringField(TEXT("native_object_path"), TEXT("/Game/Machine.Native"));
	NodeA->SetStringField(TEXT("native_key"), TEXT("opaque-native-key"));
	NodeA->SetStringField(TEXT("settings_class"), TEXT("/Script/PCG.PCGSettings"));
	NodeA->SetStringField(TEXT("expression_class"), TEXT("/Script/Engine.MaterialExpression"));
	NodeA->SetStringField(TEXT("material_function_path"), TEXT("/Game/Machine/MF"));
	NodeA->SetStringField(TEXT("script_struct_ref"), TEXT("/Script/RigVM.RigUnit"));
	NodeA->SetNumberField(TEXT("node_index"), 7);
	NodeA->SetStringField(TEXT("operation"), TEXT("update"));
	NodeA->SetStringField(TEXT("name"), TEXT("Readable A"));
	NodeA->SetArrayField(TEXT("pin_groups"),
		{MakeShared<FJsonValueString>(TEXT("WritableGroup"))});
	TSharedRef<FJsonObject> PinInterface = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> ValuePin = MakeShared<FJsonObject>();
	ValuePin->SetStringField(TEXT("name"), TEXT("Value"));
	ValuePin->SetNumberField(TEXT("occurrence"), 0);
	ValuePin->SetStringField(TEXT("direction"), TEXT("output"));
	TSharedRef<FJsonObject> RepeatedValuePin = MakeShared<FJsonObject>();
	RepeatedValuePin->SetStringField(TEXT("name"), TEXT("Value"));
	RepeatedValuePin->SetNumberField(TEXT("occurrence"), 1);
	RepeatedValuePin->SetStringField(TEXT("direction"), TEXT("output"));
	TSharedRef<FJsonObject> NumberedValuePin = MakeShared<FJsonObject>();
	NumberedValuePin->SetStringField(TEXT("name"), TEXT("Value2"));
	NumberedValuePin->SetNumberField(TEXT("occurrence"), 0);
	NumberedValuePin->SetStringField(TEXT("direction"), TEXT("output"));
	TSharedRef<FJsonObject> InputValuePin = MakeShared<FJsonObject>();
	InputValuePin->SetStringField(TEXT("name"), TEXT("InputValue"));
	InputValuePin->SetNumberField(TEXT("occurrence"), 0);
	InputValuePin->SetStringField(TEXT("direction"), TEXT("input"));
	PinInterface->SetArrayField(TEXT("pins"),
		{MakeShared<FJsonValueObject>(ValuePin),
			MakeShared<FJsonValueObject>(RepeatedValuePin),
			MakeShared<FJsonValueObject>(NumberedValuePin),
			MakeShared<FJsonValueObject>(InputValuePin)});
	PinInterface->SetArrayField(TEXT("pin_groups"),
		{MakeShared<FJsonValueString>(TEXT("ReadOnlyGroup"))});
	NodeA->SetObjectField(TEXT("pin_interface"), PinInterface);
	TSharedRef<FJsonObject> CapturedPinDefault = MakeShared<FJsonObject>();
	CapturedPinDefault->SetStringField(TEXT("pin_name"), TEXT("InputValue"));
	CapturedPinDefault->SetNumberField(TEXT("pin_occurrence"), 0);
	CapturedPinDefault->SetStringField(TEXT("pin_guid"), TEXT("machine-pin-guid"));
	CapturedPinDefault->SetStringField(TEXT("default_value"), TEXT("42"));
	TSharedRef<FJsonObject> CapturedPinDefaults = MakeShared<FJsonObject>();
	CapturedPinDefaults->SetObjectField(
		TEXT("machine-pin-identity"), CapturedPinDefault);
	NodeA->SetObjectField(TEXT("pin_defaults"), CapturedPinDefaults);
	TSharedRef<FJsonObject> NodeB = MakeShared<FJsonObject>();
	NodeB->SetStringField(TEXT("id"), TEXT("node:machine-b"));
	NodeB->SetStringField(TEXT("name"), TEXT("Readable B"));
	TSharedRef<FJsonObject> Nodes = MakeShared<FJsonObject>();
	Nodes->SetObjectField(TEXT("ReadableA1"), NodeA);
	Nodes->SetObjectField(TEXT("ReadableB1"), NodeB);
	TSharedRef<FJsonObject> From = MakeShared<FJsonObject>();
	From->SetStringField(TEXT("node"), TEXT("node:machine-a"));
	From->SetStringField(TEXT("pin"), TEXT("Value2"));
	TSharedRef<FJsonObject> To = MakeShared<FJsonObject>();
	To->SetStringField(TEXT("node"), TEXT("node:machine-b"));
	To->SetStringField(TEXT("pin"), TEXT("Execute"));
	TSharedRef<FJsonObject> Edge = MakeShared<FJsonObject>();
	Edge->SetObjectField(TEXT("from"), From);
	Edge->SetObjectField(TEXT("to"), To);
	Edge->SetStringField(TEXT("operation"), TEXT("update"));
	TSharedRef<FJsonObject> Edges = MakeShared<FJsonObject>();
	Edges->SetObjectField(MachineEdgeKey, Edge);
	TSharedRef<FJsonObject> ReadableEdge = MakeShared<FJsonObject>();
	ReadableEdge->SetStringField(TEXT("from"), TEXT("ReadableB1.Out"));
	ReadableEdge->SetStringField(TEXT("to"), TEXT("ReadableA1.In"));
	Edges->SetObjectField(TEXT("edge:readable"), ReadableEdge);
	TSharedRef<FJsonObject> Values = MakeShared<FJsonObject>();
	Values->SetObjectField(TEXT("graph"), Graph);
	Values->SetObjectField(TEXT("nodes"), Nodes);
	Values->SetObjectField(TEXT("edges"), Edges);
	TSharedRef<FJsonObject> Compact = MakeShared<FJsonObject>();
	Compact->SetObjectField(TEXT("values"), Values);

	TSharedRef<FJsonObject> Visible = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Interface;
	FWorkspaceError Error;
	TestTrue(TEXT("visible graph projection succeeds"),
		TryProjectGraphAuthoringVisibleRow(
			TEXT("unit:test"), Compact, Visible, Interface, Error));
	const TSharedPtr<FJsonObject>* VisibleGraph = nullptr;
	const TSharedPtr<FJsonObject>* VisibleNodes = nullptr;
	TestTrue(TEXT("visible graph metadata exists"),
		Visible->TryGetObjectField(TEXT("graph"), VisibleGraph));
	TestFalse(TEXT("graph machine id is hidden"), (*VisibleGraph)->HasField(TEXT("id")));
	TestFalse(TEXT("graph guid is hidden"), (*VisibleGraph)->HasField(TEXT("guid")));
	TestFalse(TEXT("graph graph_guid is hidden"),
		(*VisibleGraph)->HasField(TEXT("graph_guid")));
	TestFalse(TEXT("graph asset path is hidden"),
		(*VisibleGraph)->HasField(TEXT("asset_path")));
	TestFalse(TEXT("graph operation is hidden"),
		(*VisibleGraph)->HasField(TEXT("operation")));
	TestEqual(TEXT("graph name remains visible"),
		(*VisibleGraph)->GetStringField(TEXT("name")), FString(TEXT("Readable Graph")));
	TestTrue(TEXT("visible nodes exist"),
		Visible->TryGetObjectField(TEXT("nodes"), VisibleNodes));
	const TSharedPtr<FJsonObject>* VisibleNodeA = nullptr;
	(*VisibleNodes)->TryGetObjectField(TEXT("ReadableA1"), VisibleNodeA);
	TestFalse(TEXT("node machine id is hidden"), (*VisibleNodeA)->HasField(TEXT("id")));
	TestFalse(TEXT("node expression path is hidden"),
		(*VisibleNodeA)->HasField(TEXT("expression_path")));
	TestFalse(TEXT("node native object path is hidden"),
		(*VisibleNodeA)->HasField(TEXT("native_object_path")));
	TestFalse(TEXT("node native key is hidden"),
		(*VisibleNodeA)->HasField(TEXT("native_key")));
	TestFalse(TEXT("PCG settings class is hidden"),
		(*VisibleNodeA)->HasField(TEXT("settings_class")));
	TestFalse(TEXT("Material expression class is hidden"),
		(*VisibleNodeA)->HasField(TEXT("expression_class")));
	TestFalse(TEXT("Material function path is hidden"),
		(*VisibleNodeA)->HasField(TEXT("material_function_path")));
	TestFalse(TEXT("RigVM script struct reference is hidden"),
		(*VisibleNodeA)->HasField(TEXT("script_struct_ref")));
	TestFalse(TEXT("node index is hidden"),
		(*VisibleNodeA)->HasField(TEXT("node_index")));
	TestFalse(TEXT("pin interface is excluded from the writable graph"),
		(*VisibleNodeA)->HasField(TEXT("pin_interface")));
	TestTrue(TEXT("authored node pin groups remain writable"),
		(*VisibleNodeA)->HasField(TEXT("pin_groups")));
	const TSharedPtr<FJsonObject>* VisiblePinDefaults = nullptr;
	TestTrue(TEXT("pin defaults remain writable through readable aliases"),
		(*VisibleNodeA)->TryGetObjectField(
			TEXT("pin_defaults"), VisiblePinDefaults));
	if (VisiblePinDefaults != nullptr)
	{
		TestTrue(TEXT("the readable Pin alias replaces the machine map key"),
			(*VisiblePinDefaults)->HasField(TEXT("InputValue")));
		TestFalse(TEXT("the machine Pin map key is hidden"),
			(*VisiblePinDefaults)->HasField(TEXT("machine-pin-identity")));
		const TSharedPtr<FJsonObject>* VisiblePinDefault = nullptr;
		(*VisiblePinDefaults)->TryGetObjectField(
			TEXT("InputValue"), VisiblePinDefault);
		if (VisiblePinDefault != nullptr)
		{
			TestEqual(TEXT("the writable default payload remains visible"),
				(*VisiblePinDefault)->GetStringField(TEXT("default_value")),
				FString(TEXT("42")));
			TestFalse(TEXT("the Pin name is represented only by its alias key"),
				(*VisiblePinDefault)->HasField(TEXT("pin_name")));
			TestFalse(TEXT("the Pin identity stays machine-side"),
				(*VisiblePinDefault)->HasField(TEXT("pin_guid")));
			TestFalse(TEXT("the Pin occurrence stays machine-side"),
				(*VisiblePinDefault)->HasField(TEXT("pin_occurrence")));
		}
	}
	TestTrue(TEXT("read-only interface companion is projected"), Interface.IsValid());
	if (Interface.IsValid())
	{
		const TSharedPtr<FJsonObject>* InterfaceGraph = nullptr;
		const TSharedPtr<FJsonObject>* InterfaceNodes = nullptr;
		TestTrue(TEXT("interface graph metadata exists"),
			Interface->TryGetObjectField(TEXT("graph"), InterfaceGraph));
		TestEqual(TEXT("interface graph name matches the main graph"),
			(*InterfaceGraph)->GetStringField(TEXT("name")),
			FString(TEXT("Readable Graph")));
		TestTrue(TEXT("interface node aliases exist"),
			Interface->TryGetObjectField(TEXT("nodes"), InterfaceNodes));
		TestTrue(TEXT("interface uses the main graph node alias"),
			(*InterfaceNodes)->HasField(TEXT("ReadableA1")));
		const TSharedPtr<FJsonObject>* InterfaceNode = nullptr;
		(*InterfaceNodes)->TryGetObjectField(TEXT("ReadableA1"), InterfaceNode);
		TestTrue(TEXT("interface pins stay in the companion"),
			(*InterfaceNode)->HasField(TEXT("pins")));
		const TArray<TSharedPtr<FJsonValue>>* InterfacePins = nullptr;
		TestTrue(TEXT("interface exposes readable Pin aliases"),
			(*InterfaceNode)->TryGetArrayField(TEXT("pins"), InterfacePins));
		if (InterfacePins != nullptr && InterfacePins->Num() == 4)
		{
			TestEqual(TEXT("natural numbered Pin remains distinct"),
				(*InterfacePins)[2]->AsObject()->GetStringField(TEXT("alias")),
				FString(TEXT("Value2")));
			TestFalse(TEXT("machine-facing occurrence is hidden behind the alias"),
				(*InterfacePins)[2]->AsObject()->HasField(TEXT("occurrence")));
		}
		TestTrue(TEXT("interface pin groups stay in the companion"),
			(*InterfaceNode)->HasField(TEXT("pin_groups")));
	}
	const TArray<TSharedPtr<FJsonValue>>* Connections = nullptr;
	TestTrue(TEXT("connections are visible"),
		Visible->TryGetArrayField(TEXT("connections"), Connections));
	TestEqual(TEXT("connection count"), Connections->Num(), 2);
	const TSharedPtr<FJsonObject> Connection = (*Connections)[0]->AsObject();
	TestEqual(TEXT("readable source endpoint"),
		Connection->GetStringField(TEXT("from")), FString(TEXT("ReadableA1.Value2")));
	TestFalse(TEXT("long machine edge key is not visible"),
		Visible->HasField(MachineEdgeKey) || Connection->HasField(MachineEdgeKey));
	TestFalse(TEXT("edge operation is hidden"), Connection->HasField(TEXT("operation")));
	const TSharedPtr<FJsonObject> ReadableConnection = (*Connections)[1]->AsObject();
	TestEqual(TEXT("preformatted source endpoint remains readable"),
		ReadableConnection->GetStringField(TEXT("from")),
		FString(TEXT("ReadableB1.Out")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphAuthoringInterfaceExportPlanTest,
	"UeAgentInterface.Core.HjsonWorkspace.GraphAuthoring.InterfaceExportPlan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphAuthoringInterfaceExportPlanTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	auto Resources = MakeSemanticWorkspaceResourceTracker();
	const auto ReadOnlyPin = ObjectValue({
		{TEXT("name"), StringValue(TEXT("ReadOnlyPin"), Resources)},
		{TEXT("occurrence"), IntegerValue(0, Resources)},
		{TEXT("direction"), StringValue(TEXT("input"), Resources)}}, Resources);
	const auto PinInterface = ObjectValue({
		{TEXT("pins"), ArrayValue(
			{ReadOnlyPin}, Resources)},
		{TEXT("pin_groups"), ArrayValue(
			{StringValue(TEXT("ReadOnlyGroup"), Resources)}, Resources)}}, Resources);
	const auto Node = ObjectValue({
		{TEXT("id"), StringValue(TEXT("node:fixture"), Resources)},
		{TEXT("name"), StringValue(TEXT("Fixture Node"), Resources)},
		{TEXT("pin_groups"), ArrayValue(
			{StringValue(TEXT("WritableGroup"), Resources)}, Resources)},
		{TEXT("pin_interface"), PinInterface}}, Resources);
	const auto Graph = ObjectValue({
		{TEXT("name"), StringValue(TEXT("Fixture Graph"), Resources)},
		{TEXT("graph_kind"), StringValue(TEXT("event"), Resources)}}, Resources);
	const auto EmptyObject = ObjectValue({}, Resources);

	FSemanticUnit TargetUnit(
		TEXT("target:fixture"), ESemanticUnitKind::TargetState, FString(),
		TEXT("uai.test.target"), TEXT("target"), FString(),
		{{TEXT("name"), StringValue(TEXT("Fixture"), Resources)}},
		{}, {}, FString(), FString(), {}, {TEXT("llm_authoring")},
		TEXT("fixture"), TEXT("fixture-target"));
	FSemanticUnit GraphUnit(
		TEXT("graph:fixture"), ESemanticUnitKind::StructureState,
		TEXT("target:fixture"), TEXT("uai.test.graph.structure"),
		TEXT("graph"), FString(),
		{
			{TEXT("graph"), Graph},
			{TEXT("nodes"), ObjectValue({{TEXT("FixtureNode1"), Node}}, Resources)},
			{TEXT("edges"), EmptyObject},
			{TEXT("interfaces"), ArrayValue({ObjectValue({
				{TEXT("name"), StringValue(TEXT("GraphInterfaceFixture"), Resources)}}, Resources)}, Resources)},
		},
		{}, {}, FString(), FString(), {}, {TEXT("llm_authoring")},
		TEXT("fixture"));

	FAuthoringModelPtr Model;
	FWorkspaceError Error;
	TestTrue(TEXT("sidecar fixture model compiles"), TryCompileAuthoringModel(
		TEXT("uai.authoring.model"), 1, TEXT("llm_authoring"), TEXT("r1"),
		{MoveTemp(TargetUnit), MoveTemp(GraphUnit)}, {TEXT("target:fixture")},
		Resources, Model, Error));
	if (!Model.IsValid())
	{
		return false;
	}

	FExportWorkspacePlanPtr Plan;
	TestTrue(TEXT("sidecar fixture export plan builds"),
		TryBuildCompositionExportPlan(Model.ToSharedRef(), {}, Resources, Plan, Error));
	if (!Plan.IsValid())
	{
		return false;
	}

	const FExportFilePlan* InterfaceFile = Plan->GetFiles().FindByPredicate(
		[](const FExportFilePlan& File)
		{
			return File.GetRelativePath().EndsWith(TEXT(".interfaces.hjson"));
		});
	TestNotNull(TEXT("the same plan contains the interface companion"), InterfaceFile);
	if (InterfaceFile == nullptr)
	{
		return false;
	}
	const FUTF8ToTCHAR InterfaceConversion(
		reinterpret_cast<const ANSICHAR*>(
			InterfaceFile->GetCanonicalUtf8().GetData()),
		InterfaceFile->GetCanonicalUtf8().Num());
	const FString InterfaceText(
		InterfaceConversion.Length(), InterfaceConversion.Get());
	TestTrue(TEXT("companion contains graph metadata and read-only pin arrays"),
		InterfaceText.Contains(TEXT("Fixture Graph"))
		&& InterfaceText.Contains(TEXT("graph_kind"))
		&& InterfaceText.Contains(TEXT("ReadOnlyPin"))
		&& InterfaceText.Contains(TEXT("ReadOnlyGroup")));
	TestFalse(TEXT("companion hides machine-facing pin occurrence"),
		InterfaceText.Contains(TEXT("occurrence")));
	TestTrue(TEXT("companion keeps the main graph node alias"),
		InterfaceText.Contains(TEXT("FixtureNode1")));
	FString MainPath = InterfaceFile->GetRelativePath();
	MainPath.RemoveFromEnd(TEXT(".interfaces.hjson"));
	MainPath += TEXT(".hjson");
	const FExportFilePlan* MainFile = Plan->GetFiles().FindByPredicate(
		[&MainPath](const FExportFilePlan& File)
		{
			return File.GetRelativePath() == MainPath;
		});
	TestNotNull(TEXT("the same plan contains the writable main graph"), MainFile);
	if (MainFile != nullptr)
	{
		const FUTF8ToTCHAR MainConversion(
			reinterpret_cast<const ANSICHAR*>(
				MainFile->GetCanonicalUtf8().GetData()),
			MainFile->GetCanonicalUtf8().Num());
		const FString MainText(MainConversion.Length(), MainConversion.Get());
		const FString InterfaceName = FPaths::GetCleanFilename(
			InterfaceFile->GetRelativePath());
		TestTrue(TEXT("main graph contains the relative interface ref"),
			MainText.Contains(TEXT("interface_ref"))
			&& MainText.Contains(InterfaceName));
		TestFalse(TEXT("main graph excludes the read-only pin interface"),
			MainText.Contains(TEXT("pin_interface")));
		TestTrue(TEXT("main graph retains writable pin groups"),
			MainText.Contains(TEXT("WritableGroup")));
		TestTrue(TEXT("graph metadata is not mistaken for a keyed collection"),
			MainText.Contains(TEXT("GraphInterfaceFixture")));
	}
	const FString OutputRoot = FPaths::ProjectSavedDir()
		/ TEXT("Automation/T204/GraphInterfaceRoundTrip");
	IFileManager::Get().DeleteDirectory(*OutputRoot, false, true);
	FArtifactPublishReceipt Receipt;
	FArtifactPublishError PublishError;
	TestTrue(TEXT("graph plan publishes with its interface companion"),
		TryPublishExportPlan(
			FArtifactPublishRequest(
				Plan.ToSharedRef(), OutputRoot,
				TEXT("graph-interface-roundtrip")),
			Resources, Receipt, PublishError));
	FUeAgentResourceTracker ResolveResources = MakeSemanticWorkspaceResourceTracker();
	FResolvedSemanticWorkspacePtr Resolved;
	FWorkspaceError ResolveError;
	const bool bResolved = TryResolveEditedSemanticWorkspace(
		FWorkspaceSource::Directory(OutputRoot),
		ESemanticWorkspaceMode::State,
		ResolveResources,
		Resolved,
		ResolveError);
	TestTrue(TEXT("exported graph interface companion reparses with the workspace"),
		bResolved);
	if (!bResolved)
	{
		AddError(ResolveError.GetCode());
	}
	IFileManager::Get().DeleteDirectory(*OutputRoot, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphAuthoringResolveTest,
	"UeAgentInterface.Core.HjsonWorkspace.GraphAuthoring.Resolve",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphAuthoringResolveTest::RunTest(const FString& Parameters)
{
	auto Resources = MakeSemanticWorkspaceResourceTracker();
	const auto Endpoint = [&](const FString& Node, const FString& Pin,
		const int32 Occurrence = 0)
	{
		TArray<TPair<FString, FUeAgentCanonicalValueRef>> Fields = {
			{TEXT("node"), StringValue(Node, Resources)},
			{TEXT("pin"), StringValue(Pin, Resources)}};
		if (Occurrence > 0)
		{
			Fields.Add({TEXT("pin_occurrence"), IntegerValue(Occurrence, Resources)});
		}
		return ObjectValue(MoveTemp(Fields), Resources);
	};
	const auto CapturedNode = [&](const FString& Id, const FString& Name)
	{
		return ObjectValue({
			{TEXT("id"), StringValue(Id, Resources)},
			{TEXT("name"), StringValue(Name, Resources)}}, Resources);
	};
	const auto VisibleNode = [&](const FString& Name)
	{
		return ObjectValue({{TEXT("name"), StringValue(Name, Resources)}}, Resources);
	};
	const auto CapturedPin = [&](const FString& Name, const FString& Direction,
		const int32 Occurrence = 0)
	{
		return ObjectValue({
			{TEXT("name"), StringValue(Name, Resources)},
			{TEXT("direction"), StringValue(Direction, Resources)},
			{TEXT("occurrence"), IntegerValue(Occurrence, Resources)}}, Resources);
	};
	const auto CapturedNodeA = ObjectValue({
		{TEXT("id"), StringValue(TEXT("node:a"), Resources)},
		{TEXT("name"), StringValue(TEXT("A"), Resources)},
		{TEXT("position"), ArrayValue({
			DecimalValue(TEXT("10.5"), Resources),
			IntegerValue(20, Resources)}, Resources)},
		{TEXT("pin_defaults"), ObjectValue({
			{TEXT("captured-pin-key"), ObjectValue({
				{TEXT("pin_name"), StringValue(TEXT("In"), Resources)},
				{TEXT("pin_occurrence"), IntegerValue(0, Resources)},
				{TEXT("pin_guid"), StringValue(TEXT("machine-pin-guid"), Resources)},
				{TEXT("default_value"), StringValue(TEXT("old"), Resources)}},
				Resources)}}, Resources)},
		{TEXT("pin_interface"), ObjectValue({
			{TEXT("pins"), ArrayValue({
				CapturedPin(TEXT("Value"), TEXT("output")),
				CapturedPin(TEXT("Value"), TEXT("output"), 1),
				CapturedPin(TEXT("Value2"), TEXT("output")),
				CapturedPin(TEXT("In"), TEXT("input"))}, Resources)}}, Resources)}},
		Resources);
	const auto Captured = ObjectValue({
		{TEXT("graph"), ObjectValue({
			{TEXT("id"), StringValue(TEXT("graph:machine"), Resources)},
			{TEXT("name"), StringValue(TEXT("Readable Graph"), Resources)}}, Resources)},
		{TEXT("nodes"), ObjectValue({
			{TEXT("A1"), CapturedNodeA},
			{TEXT("B1"), CapturedNode(TEXT("node:b"), TEXT("B"))},
			{TEXT("C1"), CapturedNode(TEXT("node:c"), TEXT("C"))}}, Resources)},
		{TEXT("edges"), ObjectValue({
			{TEXT("edge:machine"), ObjectValue({
				{TEXT("from"), Endpoint(TEXT("node:a"), TEXT("Value2"))},
				{TEXT("to"), Endpoint(TEXT("node:c"), TEXT("In"))}}, Resources)}}, Resources)}},
		Resources);
	const auto ExistingConnection = ObjectValue({
		{TEXT("from"), StringValue(TEXT("A1.Value2"), Resources)},
		{TEXT("to"), StringValue(TEXT("C1.In"), Resources)}}, Resources);
	const auto NewConnection = ObjectValue({
		{TEXT("from"), StringValue(TEXT("D1.Out"), Resources)},
		{TEXT("to"), StringValue(TEXT("A1.In"), Resources)}}, Resources);
	const auto ContextualCandidate = ObjectValue({
		{TEXT("candidate"), ObjectValue({
			{TEXT("name"), StringValue(TEXT("New D"), Resources)}}, Resources)},
		{TEXT("candidate_context"), ObjectValue({
			{TEXT("node"), StringValue(TEXT("A1"), Resources)},
			{TEXT("pin"), StringValue(TEXT("Value2"), Resources)}}, Resources)}},
		Resources);
	const auto VisibleNodeA = ObjectValue({
		{TEXT("name"), StringValue(TEXT("A"), Resources)},
		{TEXT("editor_position"), ObjectValue({
			{TEXT("x"), DecimalValue(TEXT("11.25"), Resources)},
			{TEXT("y"), IntegerValue(20, Resources)}}, Resources)},
		{TEXT("pin_defaults"), ObjectValue({
			{TEXT("In"), ObjectValue({
				{TEXT("default_value"), StringValue(TEXT("updated"), Resources)}},
				Resources)},
			{TEXT("NewExposed"), ObjectValue({
				{TEXT("default_value"), StringValue(TEXT("true"), Resources)}},
				Resources)}}, Resources)}}, Resources);
	const auto Visible = ObjectValue({
		{TEXT("interface_ref"), StringValue(TEXT("graph.interfaces.hjson"), Resources)},
		{TEXT("graph"), ObjectValue({
			{TEXT("name"), StringValue(TEXT("Readable Graph"), Resources)}}, Resources)},
		{TEXT("nodes"), ObjectValue({
			{TEXT("A1"), VisibleNodeA},
			{TEXT("C1"), VisibleNode(TEXT("C"))},
			{TEXT("D1"), ContextualCandidate},
			{TEXT("E1"), VisibleNode(TEXT("New E"))}}, Resources)},
		{TEXT("connections"), ArrayValue(
			{ExistingConnection, NewConnection}, Resources)}}, Resources);

	TMap<FString, FUeAgentCanonicalValueRef> Resolved;
	FWorkspaceError Error;
	TestTrue(TEXT("visible edit resolves"), TryResolveGraphAuthoringProjection(
		Visible.Get(), Captured.Get(), TEXT("test"), Resources, Resolved, Error));
	TestFalse(TEXT("interface ref is not writable semantic state"),
		Resolved.Contains(TEXT("interface_ref")));
	const FUeAgentCanonicalValue& Nodes = Resolved.FindChecked(TEXT("nodes")).Get();
	const FUeAgentCanonicalValue* ResolvedNodeA = FindField(Nodes, TEXT("A1"));
	const FUeAgentCanonicalValue* ResolvedPosition =
		ResolvedNodeA != nullptr ? FindField(*ResolvedNodeA, TEXT("pos")) : nullptr;
	TestTrue(TEXT("position aliases normalize to the common pos object"),
		ResolvedPosition != nullptr
			&& ResolvedPosition->GetKind() == EUeAgentValueKind::Object);
	if (ResolvedPosition != nullptr
		&& ResolvedPosition->GetKind() == EUeAgentValueKind::Object)
	{
		const FUeAgentCanonicalValue* X = FindField(*ResolvedPosition, TEXT("x"));
		const FUeAgentCanonicalValue* Y = FindField(*ResolvedPosition, TEXT("y"));
		TestEqual(TEXT("fractional graph coordinates remain exact"),
			X != nullptr ? X->GetLexeme() : FString(), FString(TEXT("11.25")));
		TestEqual(TEXT("integer graph coordinates remain valid"),
			Y != nullptr ? Y->GetLexeme() : FString(), FString(TEXT("20")));
	}
	TestTrue(TEXT("legacy position alias is removed"),
		ResolvedNodeA != nullptr
			&& FindField(*ResolvedNodeA, TEXT("position")) == nullptr);
	TestTrue(TEXT("editor position alias is removed"),
		ResolvedNodeA != nullptr
			&& FindField(*ResolvedNodeA, TEXT("editor_position")) == nullptr);
	TestEqual(TEXT("unchanged node resolves update"),
		ReadString(*FindField(Nodes, TEXT("A1")), TEXT("operation")),
		FString(TEXT("update")));
	const FUeAgentCanonicalValue* ResolvedPinDefaults =
		FindField(*FindField(Nodes, TEXT("A1")), TEXT("pin_defaults"));
	TestTrue(TEXT("readable Pin defaults resolve to machine-side state"),
		ResolvedPinDefaults != nullptr);
	if (ResolvedPinDefaults != nullptr)
	{
		const FUeAgentCanonicalValue* ResolvedPinDefault =
			FindField(*ResolvedPinDefaults, TEXT("captured-pin-key"));
		TestTrue(TEXT("the existing captured Pin identity is preserved"),
			ResolvedPinDefault != nullptr);
		if (ResolvedPinDefault != nullptr)
		{
			TestEqual(TEXT("the Pin alias restores its semantic name"),
				ReadString(*ResolvedPinDefault, TEXT("pin_name")),
				FString(TEXT("In")));
			TestEqual(TEXT("the Pin alias restores its occurrence"),
				ReadString(*ResolvedPinDefault, TEXT("pin_occurrence")),
				FString(TEXT("0")));
			TestEqual(TEXT("the readable edit updates only the payload"),
				ReadString(*ResolvedPinDefault, TEXT("default_value")),
				FString(TEXT("updated")));
			TestEqual(TEXT("the hidden captured Pin identity remains machine-side"),
				ReadString(*ResolvedPinDefault, TEXT("pin_guid")),
				FString(TEXT("machine-pin-guid")));
		}
		const FUeAgentCanonicalValue* NewExposedDefault =
			FindField(*ResolvedPinDefaults, TEXT("pin:NewExposed:0"));
		TestTrue(TEXT("a newly derived Pin alias gains machine-side selectors"),
			NewExposedDefault != nullptr);
		if (NewExposedDefault != nullptr)
		{
			TestEqual(TEXT("the new Pin name is restored without model identity"),
				ReadString(*NewExposedDefault, TEXT("pin_name")),
				FString(TEXT("NewExposed")));
			TestEqual(TEXT("the new Pin occurrence defaults machine-side"),
				ReadString(*NewExposedDefault, TEXT("pin_occurrence")),
				FString(TEXT("0")));
		}
	}
	TestEqual(TEXT("deleted node resolves remove"),
		ReadString(*FindField(Nodes, TEXT("B1")), TEXT("operation")),
		FString(TEXT("remove")));
	TestEqual(TEXT("new readable candidate node resolves create"),
		ReadString(*FindField(Nodes, TEXT("D1")), TEXT("operation")),
		FString(TEXT("create")));
	const FUeAgentCanonicalValue* Candidate =
		FindField(*FindField(Nodes, TEXT("D1")), TEXT("candidate"));
	TestTrue(TEXT("candidate object is restored for the domain provider"),
		Candidate != nullptr);
	if (Candidate != nullptr)
	{
		TestEqual(TEXT("candidate name is preserved"),
			ReadString(*Candidate, TEXT("name")),
			FString(TEXT("New D")));
	}
	const FUeAgentCanonicalValue* CandidateContext =
		FindField(*FindField(Nodes, TEXT("D1")), TEXT("candidate_context"));
	TestTrue(TEXT("candidate context remains structured"), CandidateContext != nullptr);
	if (CandidateContext != nullptr)
	{
		TestEqual(TEXT("candidate node alias resolves only on the machine side"),
			ReadString(*CandidateContext, TEXT("node")), FString(TEXT("node:a")));
		TestEqual(TEXT("candidate context keeps its readable source alias"),
			ReadString(*CandidateContext, TEXT("alias")), FString(TEXT("A1")));
		TestEqual(TEXT("candidate context keeps the readable Pin alias"),
			ReadString(*CandidateContext, TEXT("pin")), FString(TEXT("Value2")));
	}
	TestEqual(TEXT("new named node also resolves create"),
		ReadString(*FindField(Nodes, TEXT("E1")), TEXT("operation")),
		FString(TEXT("create")));
	const FUeAgentCanonicalValue* NamedCandidate =
		FindField(*FindField(Nodes, TEXT("E1")), TEXT("candidate"));
	TestTrue(TEXT("name candidate object is restored"), NamedCandidate != nullptr);
	if (NamedCandidate != nullptr)
	{
		TestEqual(TEXT("candidate name is preserved"),
			ReadString(*NamedCandidate, TEXT("name")), FString(TEXT("New E")));
	}
	const FUeAgentCanonicalValue& Edges = Resolved.FindChecked(TEXT("edges")).Get();
	TestEqual(TEXT("unchanged edge resolves update"),
		ReadString(*FindField(Edges, TEXT("edge:machine")), TEXT("operation")),
		FString(TEXT("update")));
	TestEqual(TEXT("new connection resolves create"),
		ReadString(*FindField(Edges, TEXT("connection:1")), TEXT("operation")),
		FString(TEXT("create")));
	TSharedRef<FJsonObject> ConsumerDocument = MakeShared<FJsonObject>();
	TestTrue(TEXT("resolved graph projects through the shared consumer boundary"),
		TryProjectGraphAuthoringConsumerDocument(
			TEXT("test"), Resolved, ConsumerDocument, Error));
	const TSharedPtr<FJsonObject>* ConsumerNodes = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* ConsumerConnections = nullptr;
	TestTrue(TEXT("consumer graph retains its desired nodes"),
		ConsumerDocument->TryGetObjectField(TEXT("nodes"), ConsumerNodes)
			&& ConsumerNodes != nullptr && ConsumerNodes->IsValid());
	if (ConsumerNodes != nullptr && ConsumerNodes->IsValid())
	{
		TestFalse(TEXT("removed nodes do not reach typed consumers"),
			(*ConsumerNodes)->HasField(TEXT("B1")));
		TestTrue(TEXT("created nodes reach typed consumers by readable alias"),
			(*ConsumerNodes)->HasField(TEXT("D1")));
	}
	TestTrue(TEXT("consumer graph retains only desired connections"),
		ConsumerDocument->TryGetArrayField(
			TEXT("connections"), ConsumerConnections)
			&& ConsumerConnections != nullptr);
	if (ConsumerConnections != nullptr)
	{
		TestEqual(TEXT("removed connections do not reach typed consumers"),
			ConsumerConnections->Num(), 2);
	}

	const auto VisibleWithoutExistingConnection = ObjectValue({
		{TEXT("graph"), ObjectValue({
			{TEXT("name"), StringValue(TEXT("Readable Graph"), Resources)}}, Resources)},
		{TEXT("nodes"), ObjectValue({
			{TEXT("A1"), VisibleNode(TEXT("A"))},
			{TEXT("C1"), VisibleNode(TEXT("C"))}}, Resources)},
		{TEXT("connections"), ArrayValue({}, Resources)}}, Resources);
	Resolved.Reset();
	TestTrue(TEXT("connection deletion resolves"), TryResolveGraphAuthoringProjection(
		VisibleWithoutExistingConnection.Get(), Captured.Get(), TEXT("test"),
		Resources, Resolved, Error));
	TestEqual(TEXT("deleted edge resolves remove"),
		ReadString(*FindField(Resolved.FindChecked(TEXT("edges")).Get(),
			TEXT("edge:machine")), TEXT("operation")), FString(TEXT("remove")));

	const auto Forged = ObjectValue({
		{TEXT("graph"), ObjectValue({
			{TEXT("id"), StringValue(TEXT("forged"), Resources)}}, Resources)},
		{TEXT("nodes"), ObjectValue({}, Resources)},
		{TEXT("connections"), ArrayValue({}, Resources)}}, Resources);
	Resolved.Reset();
	TestFalse(TEXT("machine fields are rejected"),
		TryResolveGraphAuthoringProjection(
			Forged.Get(), Captured.Get(), TEXT("test"), Resources, Resolved, Error));
	TestEqual(TEXT("machine field rejection is structured"), Error.GetCode(),
		FString(TEXT("semantic_workspace_graph_machine_field_forbidden")));

	const auto AmbiguousPosition = ObjectValue({
		{TEXT("graph"), ObjectValue({
			{TEXT("name"), StringValue(TEXT("Readable Graph"), Resources)}}, Resources)},
		{TEXT("nodes"), ObjectValue({
			{TEXT("A1"), ObjectValue({
				{TEXT("pos"), ArrayValue({
					IntegerValue(1, Resources), IntegerValue(2, Resources)}, Resources)},
				{TEXT("position"), ArrayValue({
					IntegerValue(3, Resources), IntegerValue(4, Resources)}, Resources)}},
				Resources)}}, Resources)},
		{TEXT("connections"), ArrayValue({}, Resources)}}, Resources);
	Resolved.Reset();
	Error.Reset();
	TestFalse(TEXT("multiple position aliases are rejected"),
		TryResolveGraphAuthoringProjection(
			AmbiguousPosition.Get(), Captured.Get(), TEXT("test"),
			Resources, Resolved, Error));
	TestEqual(TEXT("position ambiguity has a stable error code"),
		Error.GetCode(),
		FString(TEXT("semantic_workspace_graph_position_ambiguous")));

	const auto VisibleWithCandidateId = ObjectValue({
		{TEXT("graph"), ObjectValue({
			{TEXT("name"), StringValue(TEXT("Readable Graph"), Resources)}}, Resources)},
		{TEXT("nodes"), ObjectValue({
			{TEXT("A1"), VisibleNode(TEXT("A"))},
			{TEXT("NewNode"), ObjectValue({
				{TEXT("candidate"), ObjectValue({
					{TEXT("id"), StringValue(TEXT("opaque-candidate-id"), Resources)}},
					Resources)}}, Resources)}}, Resources)},
		{TEXT("connections"), ArrayValue({}, Resources)}}, Resources);
	Resolved.Reset();
	Error.Reset();
	TestFalse(TEXT("model-authored candidate ids are rejected"),
		TryResolveGraphAuthoringProjection(
			VisibleWithCandidateId.Get(), Captured.Get(), TEXT("test"),
			Resources, Resolved, Error));
	TestEqual(TEXT("candidate ids use the common machine-field error"),
		Error.GetCode(),
		FString(TEXT("semantic_workspace_graph_machine_field_forbidden")));

	const auto MissingAliasConnection = ObjectValue({
		{TEXT("from"), StringValue(TEXT("MissingAlias.Out"), Resources)},
		{TEXT("to"), StringValue(TEXT("A1.In"), Resources)}}, Resources);
	const auto VisibleWithMissingAliasConnection = ObjectValue({
		{TEXT("graph"), ObjectValue({
			{TEXT("name"), StringValue(TEXT("Readable Graph"), Resources)}}, Resources)},
		{TEXT("nodes"), ObjectValue({
			{TEXT("A1"), VisibleNode(TEXT("A"))}}, Resources)},
		{TEXT("connections"), ArrayValue(
			{MissingAliasConnection}, Resources)}}, Resources);
	Resolved.Reset();
	TestFalse(TEXT("missing connection alias is rejected"),
		TryResolveGraphAuthoringProjection(
			VisibleWithMissingAliasConnection.Get(), Captured.Get(), TEXT("test"),
			Resources, Resolved, Error));
	TestEqual(TEXT("missing connection alias keeps stable error code"),
		Error.GetCode(),
		FString(TEXT("semantic_workspace_graph_connection_invalid")));
	TestTrue(TEXT("connection error identifies missing alias"),
		Error.ToPublicDetail().Contains(TEXT("MissingAlias")));
	TestEqual(TEXT("connection source remains the real source unit"), Error.GetSourceUnit(), FString(TEXT("test")));
	TestEqual(TEXT("connection error identifies endpoint pointer"),
		Error.GetJsonPointer(), FString(TEXT("/connections/0/from")));

	const auto ForgedPinDefault = ObjectValue({
		{TEXT("default_value"), StringValue(TEXT("forged"), Resources)},
		{TEXT("pin_guid"), StringValue(TEXT("forged-machine-pin"), Resources)}},
		Resources);
	const auto VisibleWithForgedPinIdentity = ObjectValue({
		{TEXT("graph"), ObjectValue({
			{TEXT("name"), StringValue(TEXT("Readable Graph"), Resources)}}, Resources)},
		{TEXT("nodes"), ObjectValue({
			{TEXT("A1"), ObjectValue({
				{TEXT("name"), StringValue(TEXT("A"), Resources)},
				{TEXT("pin_defaults"), ObjectValue({
					{TEXT("In"), ForgedPinDefault}}, Resources)}}, Resources)}}, Resources)},
		{TEXT("connections"), ArrayValue({}, Resources)}}, Resources);
	Resolved.Reset();
	Error.Reset();
	TestFalse(TEXT("nested model-authored Pin identity is rejected"),
		TryResolveGraphAuthoringProjection(
			VisibleWithForgedPinIdentity.Get(), Captured.Get(), TEXT("test"),
			Resources, Resolved, Error));
	TestEqual(TEXT("nested Pin identity uses the common machine-field error"),
		Error.GetCode(),
		FString(TEXT("semantic_workspace_graph_machine_field_forbidden")));
	return true;
}
