#include "Misc/AutomationTest.h"
#include "Core/ContentDiscovery/UeAgentNativeNodeAction.h"
#include "Core/AssetPreview/UeAgentAssetPreviewState.h"
#include "AIGraphTypes.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/Tasks/BTTask_Wait.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode_Task.h"
#include "BlueprintActionFilter.h"
#include "BlueprintActionMenuItem.h"
#include "BlueprintBoundEventNodeSpawner.h"
#include "BlueprintEventNodeSpawner.h"
#include "BlueprintNodeSpawner.h"
#include "Components/BoxComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphSchema_K2_Actions.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "GraphEditAction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopeExit.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentBackgroundNativeActionTest,
	"UeAgentInterface.Core.ContentDiscovery.BackgroundNativeActions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentBackgroundNativeActionTest::RunTest(const FString&)
{
	using namespace UE::UAI::ContentDiscovery;
	using FBindings = IBlueprintNodeBinder::FBindingSet;
	const TStrongObjectPtr<UBlueprint> BP(FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), GetTransientPackage(),
		MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), TEXT("UAI_BackgroundUi")),
		BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass()));
	if (!TestNotNull(TEXT("native Blueprint"), BP.Get())) return false;
	// Native creation already supplies the event graph and may supply default
	// events. Use that graph so the per-Blueprint event lookup has one owner.
	if (!TestEqual(TEXT("native event graph count"), BP->UbergraphPages.Num(), 1)) return false;
	UEdGraph* Graph = BP->UbergraphPages[0];
	auto* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	const int32 BeforeEditors = Editors->GetAllOpenEditors().Num();
	int32 Selections = 0;
	const auto Changed = Graph->AddOnGraphChangedHandler(FOnGraphChanged::FDelegate::CreateLambda(
		[&](const FEdGraphEditAction& Action) { if (Action.Action & GRAPHACTION_SelectNode) ++Selections; }));
	ON_SCOPE_EXIT { Graph->RemoveOnGraphChangedHandler(Changed); Editors->CloseAllEditorsForAsset(BP.Get()); };
	const TStrongObjectPtr<UBlueprintEventNodeSpawner> Event(UBlueprintEventNodeSpawner::Create(
		AActor::StaticClass()->FindFunctionByName(TEXT("ReceiveBeginPlay"))));
	UEdGraphNode* Ghost = Event->Invoke(Graph, FBindings(), FVector2D::ZeroVector);
	if (!TestNotNull(TEXT("native ghost fixture"), Ghost)) return false;
	Ghost->MakeAutomaticallyPlacedGhostNode();
	const int32 GhostCount = Graph->Nodes.Num();
	UEdGraphNode* Replacement = NativeNodeAction::Invoke(*Event, *Graph, nullptr, FVector2f(101, 201), FBindings());
	if (!TestNotNull(TEXT("replacement"), Replacement)) return false;
	TestTrue(TEXT("new identity despite unchanged count"), Replacement != Ghost && Graph->Nodes.Num() == GhostCount);
	TestFalse(TEXT("replacement is real"), Replacement->IsAutomaticallyPlacedGhostNode());
	TestTrue(TEXT("native existing-event reuse"), NativeNodeAction::Invoke(*Event, *Graph, nullptr,
		FVector2f(500, 500), FBindings()) == Replacement);
	TestEqual(TEXT("replacement/reuse do not open toolkit"), Editors->GetAllOpenEditors().Num(), BeforeEditors);

	// Native automatic connection and notification still run outside the UI menu.
	const TStrongObjectPtr<UBlueprintNodeSpawner> Sequence(UBlueprintNodeSpawner::Create(UK2Node_ExecutionSequence::StaticClass()));
	auto* Next = NativeNodeAction::Invoke(*Sequence, *Graph,
		Replacement->FindPin(UEdGraphSchema_K2::PN_Then), FVector2f(300, 201), FBindings());
	TestNotNull(TEXT("native autowire node"), Next);
	TestTrue(TEXT("native autowire link"), Next && Replacement->FindPin(UEdGraphSchema_K2::PN_Then)->LinkedTo.Contains(
		Next->FindPin(UEdGraphSchema_K2::PN_Execute)));

	BP->SimpleConstructionScript->AddNode(BP->SimpleConstructionScript->CreateNode(UBoxComponent::StaticClass(), TEXT("LeftBox")));
	BP->SimpleConstructionScript->AddNode(BP->SimpleConstructionScript->CreateNode(UBoxComponent::StaticClass(), TEXT("RightBox")));
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP.Get());
	auto* Left = FindFProperty<FObjectProperty>(BP->SkeletonGeneratedClass, TEXT("LeftBox"));
	auto* Right = FindFProperty<FObjectProperty>(BP->SkeletonGeneratedClass, TEXT("RightBox"));
	if (!TestNotNull(TEXT("left binding field"), Left) || !TestNotNull(TEXT("right binding field"), Right)) return false;
	auto* Delegate = FindFProperty<FMulticastDelegateProperty>(UPrimitiveComponent::StaticClass(), TEXT("OnComponentBeginOverlap"));
	const TStrongObjectPtr<UBlueprintBoundEventNodeSpawner> Bound(UBlueprintBoundEventNodeSpawner::Create(
		UK2Node_ComponentBoundEvent::StaticClass(), Delegate));
	FBlueprintActionContext Context;
	Context.Blueprints.Add(BP.Get()); Context.Graphs.Add(Graph);
	FBindings Bindings{ FBindingObject(Left), FBindingObject(Right) };
	FBlueprintActionMenuItem Item(Bound.Get(), Bound->GetUiSpec(Context, Bindings), Bindings);
	const FText OriginalTitle = Item.GetMenuDescription();
	int32 OriginalDelegateCalls = 0;
	Bound->DynamicUiSignatureGetter = UBlueprintNodeSpawner::FUiSpecOverrideDelegate::CreateLambda(
		[&](const FBlueprintActionContext&, const FBindings&, FBlueprintActionUiSpec*) { ++OriginalDelegateCalls; });
	FBindings Read;
	FString Error;
	if (!TestTrue(TEXT("read exact consolidated binding set"), NativeNodeAction::ReadBindings(Item, Context, Read, Error)))
		{ AddError(Error); return false; }
	TestTrue(TEXT("both distinct fields retained"), Read.Num() == 2 && Read.Contains(FBindingObject(Left)) && Read.Contains(FBindingObject(Right)));
	TestTrue(TEXT("original item UI unchanged"), Item.GetMenuDescription().EqualTo(OriginalTitle));
	TestEqual(TEXT("original delegate preserved while observing"), OriginalDelegateCalls, 1);
	Bound->GetUiSpec(Context, Bindings);
	TestEqual(TEXT("original delegate restored"), OriginalDelegateCalls, 2);
	if (!TestNotNull(TEXT("bound native creation"), NativeNodeAction::Invoke(*Bound, *Graph, nullptr, FVector2f(500, 100), Read))) return false;
	TSet<FName> Components;
	for (UEdGraphNode* Node : Graph->Nodes)
		if (const auto* BoundNode = Cast<UK2Node_ComponentBoundEvent>(Node)) Components.Add(BoundNode->ComponentPropertyName);
	TestTrue(TEXT("native field binding applied to both nodes"), Components.Contains(TEXT("LeftBox")) && Components.Contains(TEXT("RightBox")));
	TestEqual(TEXT("background spawning does not request selection"), Selections, 0);
	TestEqual(TEXT("bound creation does not open toolkit"), Editors->GetAllOpenEditors().Num(), BeforeEditors);
	FEdGraphSchemaAction_K2NewNode Goto;
	Goto.NodeTemplate = NewObject<UK2Node_ExecutionSequence>();
	Goto.bGotoNode = true;
	TestNotNull(TEXT("native K2 creation with Goto suppressed"), NativeNodeAction::InvokeSchemaAction(
		Goto, *Graph, nullptr, FVector2f(800, 100)));
	TestTrue(TEXT("original Goto flag restored"), Goto.bGotoNode);
	FEdGraphSchemaAction_K2ViewNode View;
	View.NodePtr = CastChecked<UK2Node>(Replacement);
	TestNull(TEXT("view action cannot create"), NativeNodeAction::InvokeSchemaAction(View, *Graph, nullptr, FVector2f::ZeroVector));
	TestEqual(TEXT("Goto/view do not open toolkit"), Editors->GetAllOpenEditors().Num(), BeforeEditors);

	const TStrongObjectPtr<UBehaviorTree> Tree(NewObject<UBehaviorTree>());
	auto* AiGraph = NewObject<UBehaviorTreeGraph>(Tree.Get());
	auto* Task = NewObject<UBehaviorTreeGraphNode_Task>();
	Task->ClassData = FGraphNodeClassData(UBTTask_Wait::StaticClass(), FString());
	int32 AiSelections = 0;
	const auto AiChanged = AiGraph->AddOnGraphChangedHandler(FOnGraphChanged::FDelegate::CreateLambda(
		[&](const FEdGraphEditAction& Action) { if (Action.Action & GRAPHACTION_SelectNode) ++AiSelections; }));
	ON_SCOPE_EXIT { AiGraph->RemoveOnGraphChangedHandler(AiChanged); };
	TestTrue(TEXT("AI native template applied"), NativeNodeAction::InvokeTemplate(*Task, *AiGraph, nullptr,
		FVector2f(100, 200), NativeNodeAction::EPinInitialization::AfterPlacement) == Task);
	TestTrue(TEXT("AI native instance initialization preserved"), Task->NodeInstance && Task->NodeInstance->IsA<UBTTask_Wait>());
	TestEqual(TEXT("AI selection suppressed"), AiSelections, 0);

	// Reusing a preview carrier does not issue another native editor-open request.
	UObject* PreviewAsset = BP.Get();
	TestTrue(TEXT("explicit native opening"), UE::UAI::AssetPreview::OpenAssetEditor(PreviewAsset, true));
	IAssetEditorInstance* Toolkit = Editors->FindEditorForAsset(PreviewAsset, false);
	TestNotNull(TEXT("native preview carrier"), Toolkit);
	TestTrue(TEXT("background carrier reuse"), UE::UAI::AssetPreview::OpenAssetEditor(PreviewAsset));
	TestTrue(TEXT("same toolkit reused"), Editors->FindEditorForAsset(PreviewAsset, false) == Toolkit);
	return true;
}
