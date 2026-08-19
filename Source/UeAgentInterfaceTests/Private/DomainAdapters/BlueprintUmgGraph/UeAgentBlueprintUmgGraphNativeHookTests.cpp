#include "Misc/AutomationTest.h"

#include "DomainAdapters/BlueprintUmgGraph/UeAgentBlueprintUmgGraphNativeHook.h"

#include "Components/Button.h"
#include "Components/BoxComponent.h"
#include "Components/CanvasPanel.h"
#include "Components/PanelWidget.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentBlueprintUmgGraphNativeHookTest,
	"UeAgentInterface.DomainAdapters.BlueprintUmgGraph.E2.Native."
		"OwnedBlueprintScsWidgetTreeCompileAndReadback",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentBlueprintUmgGraphNativeHookTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::DomainAdapters::BlueprintUmgGraph;

	FBlueprintUmgGraphNativeHook Hook;
	FBlueprintUmgGraphAdapterError Error;
	UPackage* Package = CreatePackage(*FString::Printf(
		TEXT("/Temp/UAI/BlueprintUmgGraph/%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	if (!TestNotNull(TEXT("owned transient package exists"), Package))
	{
		return false;
	}
	Package->SetDirtyFlag(false);

	UBlueprint* Blueprint = nullptr;
	if (!TestTrue(
			TEXT("native hook creates an owned Blueprint"),
			Hook.TryCreateBlueprint(
				*Package,
				TEXT("BP_GC003_Owned"),
				*AActor::StaticClass(),
				Blueprint,
				Error)))
	{
		AddError(Error.GetCode() + TEXT(":") + Error.GetDetail());
		Package->MarkAsGarbage();
		return false;
	}

	UEdGraph* Graph = nullptr;
	FBlueprintUmgGraphNativeMutationReceipt ApplyReceipt;
	TestTrue(
		TEXT("native hook creates the Blueprint graph"),
		Hook.TryCreateUberGraph(
			*Blueprint,
			TEXT("GC003Graph"),
			Graph,
			ApplyReceipt,
			Error));
	if (!Graph)
	{
		AddError(Error.GetCode() + TEXT(":") + Error.GetDetail());
		Package->MarkAsGarbage();
		return false;
	}

	FBlueprintUmgGraphTopology BeforeGraph;
	FString BeforeRevision;
	FString BeforeReadbackEvidence;
	TestTrue(
		TEXT("pre-mutation graph is captured from UE state"),
		Hook.TryCaptureBlueprintGraph(
			*Blueprint,
			*Graph,
			TEXT("blueprint:owned"),
			TEXT("graph:GC003Graph"),
			BeforeGraph,
			BeforeRevision,
			BeforeReadbackEvidence,
			Error));

	const FGuid FirstNodeGuid = FGuid::NewGuid();
	const FGuid SecondNodeGuid = FGuid::NewGuid();
	UEdGraphNode* FirstNode = nullptr;
	UEdGraphNode* SecondNode = nullptr;
	TestTrue(
		TEXT("first typed knot node is written through the thin hook"),
		Hook.TryAddKnotNode(
			*Graph,
			FirstNodeGuid,
			120,
			80,
			FirstNode,
			ApplyReceipt,
			Error));
	TestTrue(
		TEXT("second typed knot node is written through the thin hook"),
		Hook.TryAddKnotNode(
			*Graph,
			SecondNodeGuid,
			420,
			80,
			SecondNode,
			ApplyReceipt,
			Error));
	if (!FirstNode || !SecondNode)
	{
		AddError(Error.GetCode() + TEXT(":") + Error.GetDetail());
		Package->MarkAsGarbage();
		return false;
	}
	UEdGraphPin* FromPin = FirstNode->FindPin(TEXT("OutputPin"));
	UEdGraphPin* ToPin = SecondNode->FindPin(TEXT("InputPin"));
	TestNotNull(TEXT("first knot output pin exists"), FromPin);
	TestNotNull(TEXT("second knot input pin exists"), ToPin);
	if (FromPin && ToPin)
	{
		TestTrue(
			TEXT("exact native pins are connected through graph schema"),
			Hook.TryConnectPins(
				*FromPin,
				*ToPin,
				ApplyReceipt,
				Error));
	}

	USCS_Node* RootComponent = nullptr;
	const FGuid ComponentGuid = FGuid::NewGuid();
	TestTrue(
		TEXT("SCS component is written through the same Adapter hook"),
		Hook.TryAddSCSComponent(
			*Blueprint,
			*UBoxComponent::StaticClass(),
			TEXT("GC003Root"),
			ComponentGuid,
			nullptr,
			RootComponent,
			ApplyReceipt,
			Error));

	FBlueprintUmgGraphCompileSettleReceipt BlueprintSettle;
	TestTrue(
		TEXT("Blueprint compile and settle complete"),
		Hook.TryCompileAndSettle(
			*Blueprint,
			BlueprintSettle,
			Error));
	TestTrue(TEXT("Blueprint compile receipt is complete"),
		BlueprintSettle.IsCompileComplete());
	TestEqual(TEXT("Blueprint compile has no errors"),
		BlueprintSettle.GetErrorCount(), 0);

	FBlueprintUmgGraphTopology GraphReadback;
	FString GraphRevision;
	FString GraphReadbackEvidence;
	TestTrue(
		TEXT("graph readback is independently recaptured from UE"),
		Hook.TryCaptureBlueprintGraph(
			*Blueprint,
			*Graph,
			TEXT("blueprint:owned"),
			TEXT("graph:GC003Graph"),
			GraphReadback,
			GraphRevision,
			GraphReadbackEvidence,
			Error));
	TestNotEqual(TEXT("graph mutation advances structural revision"),
		BeforeRevision, GraphRevision);
	TestTrue(TEXT("graph readback sees both real nodes"),
		GraphReadback.GetItems().Num() >= 2);
	TestEqual(TEXT("graph readback sees the exact native link"),
		GraphReadback.GetLinks().Num(), 1);

	const FBlueprintUmgGraphLifecycleEvidence BlueprintLifecycle(
		BeforeRevision,
		ApplyReceipt.GetEvidenceId(),
		BlueprintSettle.GetSettleEvidenceId(),
		GraphRevision,
		GraphReadbackEvidence,
		BlueprintSettle.IsCompileComplete(),
		BlueprintSettle.GetErrorCount());
	TestTrue(
		TEXT("real Blueprint apply/settle/readback evidence is valid"),
		TryValidateBlueprintUmgGraphLifecycleEvidence(
			BlueprintLifecycle,
			Error));

	FBlueprintUmgGraphTopology ScsReadback;
	FString ScsRevision;
	FString ScsEvidence;
	TestTrue(
		TEXT("SCS hierarchy is independently recaptured"),
		Hook.TryCaptureBlueprintScs(
			*Blueprint,
			TEXT("blueprint:owned"),
			ScsReadback,
			ScsRevision,
			ScsEvidence,
			Error));
	TestTrue(TEXT("SCS readback contains requested component GUID"),
		ScsReadback.GetItems().ContainsByPredicate(
			[&](const FBlueprintUmgGraphItemIdentity& Item)
			{
				return Item.GetNativeGuid()
					== ComponentGuid.ToString(EGuidFormats::DigitsWithHyphens);
			}));

	TArray<FBlueprintUmgGraphEventIdentity> ComponentEvents;
	if (TestNotNull(
			TEXT("SCS component has a real component template"),
			RootComponent ? RootComponent->ComponentTemplate.Get() : nullptr))
	{
		TestTrue(
			TEXT("assignable component events are discovered from reflection"),
			Hook.TryDiscoverAssignableEvents(
				*RootComponent->ComponentTemplate,
				TEXT("component:GC003Root"),
				ComponentEvents,
				Error));
		TestTrue(TEXT("component overlap event is independently discovered"),
			ComponentEvents.ContainsByPredicate(
				[](const FBlueprintUmgGraphEventIdentity& Event)
				{
					return Event.GetNativeFieldName()
						== TEXT("OnComponentBeginOverlap");
				}));
	}

	UWidgetBlueprint* WidgetBlueprint = nullptr;
	TestTrue(
		TEXT("native hook creates an owned WidgetBlueprint"),
		Hook.TryCreateWidgetBlueprint(
			*Package,
			TEXT("WBP_GC003_Owned"),
			WidgetBlueprint,
			Error));
	if (!WidgetBlueprint)
	{
		AddError(Error.GetCode() + TEXT(":") + Error.GetDetail());
		Package->MarkAsGarbage();
		return false;
	}

	UWidget* RootWidget = nullptr;
	UWidget* ButtonWidget = nullptr;
	const FGuid RootWidgetGuid = FGuid::NewGuid();
	const FGuid ButtonGuid = FGuid::NewGuid();
	TestTrue(
		TEXT("WidgetTree root is written through the thin hook"),
		Hook.TryAddWidget(
			*WidgetBlueprint,
			*UCanvasPanel::StaticClass(),
			TEXT("RootCanvas"),
			RootWidgetGuid,
			nullptr,
			INDEX_NONE,
			RootWidget,
			ApplyReceipt,
			Error));
	TestTrue(
		TEXT("WidgetTree child is written through the thin hook"),
		Hook.TryAddWidget(
			*WidgetBlueprint,
			*UButton::StaticClass(),
			TEXT("ConfirmButton"),
			ButtonGuid,
			Cast<UPanelWidget>(RootWidget),
			0,
			ButtonWidget,
			ApplyReceipt,
			Error));

	FBlueprintUmgGraphCompileSettleReceipt WidgetSettle;
	TestTrue(
		TEXT("WidgetBlueprint compile and settle complete"),
		Hook.TryCompileAndSettle(
			*WidgetBlueprint,
			WidgetSettle,
			Error));

	FBlueprintUmgGraphTopology WidgetReadback;
	FString WidgetRevision;
	FString WidgetReadbackEvidence;
	TestTrue(
		TEXT("WidgetTree readback is independently recaptured"),
		Hook.TryCaptureWidgetTree(
			*WidgetBlueprint,
			TEXT("widget_blueprint:owned"),
			WidgetReadback,
			WidgetRevision,
			WidgetReadbackEvidence,
			Error));
	TestEqual(TEXT("WidgetTree readback sees root and child"),
		WidgetReadback.GetItems().Num(), 2);
	TestEqual(TEXT("WidgetTree readback sees exact parent relation"),
		WidgetReadback.GetHierarchy().Num(), 1);

	TArray<FBlueprintUmgGraphEventIdentity> WidgetEvents;
	TestTrue(
		TEXT("assignable widget events are discovered from reflection"),
		Hook.TryDiscoverAssignableEvents(
			*ButtonWidget,
			TEXT("widget:ConfirmButton"),
			WidgetEvents,
			Error));
	TestTrue(TEXT("UButton OnClicked is independently discovered"),
		WidgetEvents.ContainsByPredicate(
			[](const FBlueprintUmgGraphEventIdentity& Event)
			{
				return Event.GetNativeFieldName() == TEXT("OnClicked");
			}));

	Package->SetDirtyFlag(false);
	Package->MarkAsGarbage();
	return !HasAnyErrors();
}
