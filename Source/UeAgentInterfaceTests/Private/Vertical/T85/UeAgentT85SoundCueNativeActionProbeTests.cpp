#include "Misc/AutomationTest.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphSchema.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNodeDelay.h"
#include "SoundCueGraph/SoundCueGraphNode.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentT85SoundCueNativeActionProbeTest,
	"UAI.Root.T85.SoundCue.NativeActionProbe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentT85SoundCueNativeActionProbeTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;

	USoundCue* const Cue = NewObject<USoundCue>(GetTransientPackage());
	if (!TestNotNull(TEXT("a transient Sound Cue is created"), Cue))
	{
		return false;
	}
	Cue->CreateGraph();
	UEdGraph* const Graph = Cue->GetGraph();
	if (!TestNotNull(TEXT("the Sound Cue owns its editor graph"), Graph)
		|| !TestNotNull(TEXT("the Sound Cue graph has a schema"),
			Graph ? Graph->GetSchema() : nullptr))
	{
		return false;
	}

	FGraphContextMenuBuilder MenuBuilder(Graph);
	Graph->GetSchema()->GetGraphContextActions(MenuBuilder);
	AddInfo(FString::Printf(
		TEXT("SoundCue native background action count=%d"),
		MenuBuilder.GetNumActions()));

	const FName NewNodeActionType(
		TEXT("FSoundCueGraphSchemaAction_NewNode"));
	UScriptStruct* const NewNodeActionStruct = FindObject<UScriptStruct>(
		nullptr,
		TEXT("/Script/AudioEditor.SoundCueGraphSchemaAction_NewNode"));
	FClassProperty* const SoundNodeClassProperty =
		NewNodeActionStruct
			? FindFProperty<FClassProperty>(
				NewNodeActionStruct, TEXT("SoundNodeClass"))
			: nullptr;
	if (!TestNotNull(TEXT("the native Sound Cue action type resolves"),
		NewNodeActionStruct)
		|| !TestNotNull(TEXT("the action exposes SoundNodeClass"),
			SoundNodeClassProperty))
	{
		return false;
	}
	TSharedPtr<FEdGraphSchemaAction> DelayAction;
	for (int32 Index = 0; Index < MenuBuilder.GetNumActions(); ++Index)
	{
		const TSharedPtr<FEdGraphSchemaAction>& Action =
			MenuBuilder.GetSchemaAction(Index);
		if (Action.IsValid() && Action->GetTypeId() == NewNodeActionType)
		{
			UObject* const SoundNodeClassValue =
				SoundNodeClassProperty->GetObjectPropertyValue_InContainer(
					Action.Get());
			if (SoundNodeClassValue == USoundNodeDelay::StaticClass())
			{
				DelayAction = Action;
				break;
			}
		}
	}
	if (!TestTrue(TEXT("the native menu exposes a Delay node action"),
		DelayAction.IsValid()))
	{
		return false;
	}
	AddInfo(FString::Printf(
		TEXT("SoundCue selected action type=%s class=%s"),
		*DelayAction->GetTypeId().ToString(),
		*USoundNodeDelay::StaticClass()->GetPathName()));

	const int32 NodesBefore = Cue->AllNodes.Num();
	const FVector2f Location(320.0f, 144.0f);
	UEdGraphNode* const Returned = DelayAction->PerformAction(
		Graph, nullptr, Location, false);
	USoundCueGraphNode* const GraphNode = Cast<USoundCueGraphNode>(Returned);
	if (!TestNotNull(TEXT("PerformAction returns a Sound Cue graph node"),
		GraphNode))
	{
		return false;
	}
	TestTrue(TEXT("the returned node belongs to the Sound Cue graph"),
		GraphNode->GetGraph() == Graph && Graph->Nodes.Contains(GraphNode));
	TestEqual(TEXT("the native action adds one Sound Cue node"),
		Cue->AllNodes.Num(), NodesBefore + 1);
	TestNotNull(TEXT("the graph node owns a runtime Sound Node"),
		GraphNode->SoundNode.Get());
	if (GraphNode->SoundNode)
	{
		TestTrue(TEXT("the created Sound Node is exactly Delay"),
			GraphNode->SoundNode->GetClass() == USoundNodeDelay::StaticClass());
		TestTrue(TEXT("the Sound Cue contains the created Sound Node"),
			Cue->AllNodes.Contains(GraphNode->SoundNode));
		TestTrue(TEXT("the created Sound Node is owned by the Sound Cue"),
			GraphNode->SoundNode->GetOuter() == Cue);
	}
	TestEqual(TEXT("the native action preserves X placement"),
		GraphNode->NodePosX, 320);
	TestEqual(TEXT("the native action preserves Y placement"),
		GraphNode->NodePosY, 144);
	AddInfo(FString::Printf(
		TEXT("SoundCue nodes before=%d after=%d position=(%d,%d)"),
		NodesBefore, Cue->AllNodes.Num(),
		GraphNode->NodePosX, GraphNode->NodePosY));
	return !HasAnyErrors();
}
