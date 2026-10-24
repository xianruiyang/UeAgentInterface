#include "Misc/AutomationTest.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundAudioMediaBindings.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadNative.h"
#include "DomainAdapters/TextureGraph/UeAgentTextureGraphDomainMechanics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Expressions/Input/TG_Expression_Scalar.h"
#include "Expressions/Output/TG_Expression_Output.h"
#include "TextureGraph.h"
#include "UObject/UObjectGlobals.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentTextureGraphCommonGraphAdapterTest,
	"UeAgentInterface.DomainAdapters.TextureGraph.CommonGraphAdapter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentTextureGraphCommonGraphAdapterTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI::Composition::GraphCompound;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::DomainAdapters;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::StateSemantic;

	const TMap<EUeAgentResourceDimension, int64> Limits{
		{EUeAgentResourceDimension::InputBytes, 4 * 1024 * 1024},
		{EUeAgentResourceDimension::OutputBytes, 4 * 1024 * 1024},
		{EUeAgentResourceDimension::StringBytes, 4 * 1024 * 1024},
		{EUeAgentResourceDimension::Nodes, 4096},
		{EUeAgentResourceDimension::Items, 4096},
		{EUeAgentResourceDimension::Files, 4096},
		{EUeAgentResourceDimension::ReferenceEdges, 4096},
		{EUeAgentResourceDimension::WorkUnits, 32768},
		{EUeAgentResourceDimension::EstimatedMemoryBytes, 32 * 1024 * 1024}};
	FUeAgentResourceTracker Resources;
	FTargetCompositionContract CompositionContract;
	TestTrue(TEXT("TextureGraph registers its formal composition contract"),
		FTargetCompositionRegistry::Get().FindContract(
			TEXT("ue.texture_graph.asset"), CompositionContract));
	FCompositionSemanticRouteContract GraphRoute;
	if (!TestTrue(TEXT("TextureGraph graph semantic resolves through the common route"),
		TryResolveTargetCompositionSemanticRoute(
			TEXT("ue.texture_graph.asset"),
			TEXT("uai.semantic.texture_graph.graph"), GraphRoute)))
	{
		return false;
	}
	TestEqual(TEXT("TextureGraph graph route uses StructureState"),
		GraphRoute.UnitKind, ESemanticUnitKind::StructureState);
	TestEqual(TEXT("TextureGraph graph route uses the common Graph role"),
		GraphRoute.Role, ECompositionSemanticRouteRole::Graph);

	auto MakeNode = [](const FString& NativeId, const UClass& ExpressionClass)
	{
		TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("id"), NativeId);
		Node->SetStringField(TEXT("node_type"), ExpressionClass.GetName());
		Node->SetStringField(TEXT("operation"), TEXT("create"));
		return Node;
	};

	TSharedRef<FJsonObject> Scalar = MakeNode(
		TEXT("native-scalar"), *UTG_Expression_Scalar::StaticClass());
	TSharedRef<FJsonObject> ScalarInputs = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> ScalarValue = MakeShared<FJsonObject>();
	ScalarValue->SetStringField(TEXT("alias"), TEXT("Strength"));
	ScalarValue->SetNumberField(TEXT("value"), 0.25);
	ScalarInputs->SetObjectField(TEXT("Value"), ScalarValue);
	Scalar->SetObjectField(TEXT("inputs"), ScalarInputs);

	TSharedRef<FJsonObject> Output = MakeNode(
		TEXT("native-output"), *UTG_Expression_Output::StaticClass());
	TSharedRef<FJsonObject> Nodes = MakeShared<FJsonObject>();
	Nodes->SetObjectField(TEXT("Scalar"), Scalar);
	Nodes->SetObjectField(TEXT("Output"), Output);

	TSharedRef<FJsonObject> From = MakeShared<FJsonObject>();
	From->SetStringField(TEXT("node"), TEXT("native-scalar"));
	From->SetStringField(TEXT("pin"), TEXT("ValueOut"));
	TSharedRef<FJsonObject> To = MakeShared<FJsonObject>();
	To->SetStringField(TEXT("node"), TEXT("native-output"));
	To->SetStringField(TEXT("pin"), TEXT("Source"));
	TSharedRef<FJsonObject> Edge = MakeShared<FJsonObject>();
	Edge->SetObjectField(TEXT("from"), From);
	Edge->SetObjectField(TEXT("to"), To);
	Edge->SetStringField(TEXT("operation"), TEXT("create"));
	TSharedRef<FJsonObject> Edges = MakeShared<FJsonObject>();
	Edges->SetObjectField(TEXT("machine-edge"), Edge);

	FUeAgentCanonicalValuePtr CanonicalNodes;
	FUeAgentCanonicalValuePtr CanonicalEdges;
	FString CanonicalError;
	if (!TestTrue(TEXT("resolved common nodes become canonical state"),
		CanonicalJson::TryFromJsonValue(
			MakeShared<FJsonValueObject>(Nodes), Resources,
			CanonicalNodes, CanonicalError))
		|| !TestTrue(TEXT("resolved common edges become canonical state"),
			CanonicalJson::TryFromJsonValue(
				MakeShared<FJsonValueObject>(Edges), Resources,
				CanonicalEdges, CanonicalError)))
	{
		AddError(CanonicalError);
		return false;
	}

	TMap<FString, FUeAgentCanonicalValueRef> GraphValues;
	GraphValues.Add(TEXT("nodes"), CanonicalNodes.ToSharedRef());
	GraphValues.Add(TEXT("edges"), CanonicalEdges.ToSharedRef());
	TArray<FSemanticUnit> Units;
	Units.Emplace(
		TEXT("target:texture-graph"), ESemanticUnitKind::TargetState,
		FString(), TEXT("ue.texture_graph.asset"), TEXT("authoring_target"),
		FString(), TMap<FString, FUeAgentCanonicalValueRef>(),
		TArray<FString>({TEXT("graph:texture-graph")}), TArray<FString>(),
		FString(), FString(), TArray<FString>(),
		TArray<FString>({TEXT("state")}), TEXT("fixture:texture-graph"));
	Units.Emplace(
		TEXT("graph:texture-graph"), ESemanticUnitKind::StructureState,
		TEXT("target:texture-graph"), TEXT("uai.semantic.texture_graph.graph"),
		TEXT("graph_collection"), FString(), MoveTemp(GraphValues),
		TArray<FString>(), TArray<FString>(), FString(), FString(),
		TArray<FString>(), TArray<FString>({TEXT("state")}),
		TEXT("fixture:texture-graph"));

	FAuthoringModelPtr Model;
	FWorkspaceError Error;
	if (!TestTrue(TEXT("resolved TextureGraph model compiles"),
		TryCompileAuthoringModel(
			TEXT("uai.authoring.composition/2"), 2, TEXT("llm_authoring"),
			TEXT("fixture:texture-graph"), MoveTemp(Units),
			{TEXT("target:texture-graph")}, Resources, Model, Error))
		|| !Model.IsValid())
	{
		AddError(Error.GetCode() + TEXT(":") + Error.GetSourceUnit());
		return false;
	}

	UTextureGraph* CurrentAsset = NewObject<UTextureGraph>(GetTransientPackage());
	if (!TestNotNull(TEXT("transient TextureGraph exists for adapter context"),
		CurrentAsset))
	{
		return false;
	}
	ContentGraphRead::FUeAgentContentGraphCapture CurrentCapture;
	FTextureGraphSemanticCompositeRequest Composite;
	if (!TestTrue(TEXT("TextureGraph consumes the resolved common graph unit"),
		TextureGraph::TryBuildSemanticComposite(
			Model.ToSharedRef(), CurrentCapture, *CurrentAsset,
			TEXT("/Game/Test/T_TextureGraph.T_TextureGraph"), Composite, Error)))
	{
		AddError(Error.GetCode() + TEXT(":") + Error.GetSourceUnit());
		return false;
	}

	TestEqual(TEXT("common nodes become typed node mutations"),
		Composite.Nodes.Num(), 2);
	TestEqual(TEXT("common connection becomes one typed link mutation"),
		Composite.Links.Num(), 1);
	TestEqual(TEXT("visible typed input becomes one parameter mutation"),
		Composite.Parameters.Num(), 1);
	if (Composite.Links.Num() == 1)
	{
		TestEqual(TEXT("source node identity resolves to typed element"),
			Composite.Links[0].GetFrom().GetElementId(), FString(TEXT("Scalar")));
		TestEqual(TEXT("source Pin name reaches typed endpoint"),
			Composite.Links[0].GetFrom().GetPinName(), FString(TEXT("ValueOut")));
		TestEqual(TEXT("target node identity resolves to typed element"),
			Composite.Links[0].GetTo().GetElementId(), FString(TEXT("Output")));
		TestEqual(TEXT("target Pin name reaches typed endpoint"),
			Composite.Links[0].GetTo().GetPinName(), FString(TEXT("Source")));
	}
	return !HasAnyErrors();
}
