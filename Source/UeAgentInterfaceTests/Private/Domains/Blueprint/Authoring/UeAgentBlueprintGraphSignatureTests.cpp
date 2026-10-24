// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "BlueprintEditorLibrary.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintGraphSignature.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintMutationModel.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Tunnel.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

namespace
{
	TSharedPtr<FJsonValue> MakePin(
		const TCHAR* Name,
		const TCHAR* Category,
		const TCHAR* Default = nullptr)
	{
		TSharedRef<FJsonObject> Type = MakeShared<FJsonObject>();
		Type->SetStringField(TEXT("pin_category"), Category);
		TSharedRef<FJsonObject> Pin = MakeShared<FJsonObject>();
		Pin->SetStringField(TEXT("name"), Name);
		Pin->SetObjectField(TEXT("type"), Type);
		if (Default)
		{
			Pin->SetStringField(TEXT("default_value"), Default);
		}
		return MakeShared<FJsonValueObject>(Pin);
	}

	TSharedRef<FJsonObject> MakeSignature(
		TArray<TSharedPtr<FJsonValue>> Inputs,
		TArray<TSharedPtr<FJsonValue>> Outputs)
	{
		TSharedRef<FJsonObject> Signature = MakeShared<FJsonObject>();
		Signature->SetArrayField(TEXT("inputs"), MoveTemp(Inputs));
		Signature->SetArrayField(TEXT("outputs"), MoveTemp(Outputs));
		return Signature;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentBlueprintCustomEventPhasedCallerTest,
	"UAI.Blueprint.Authoring.GraphSignature.CustomEventPhasedCaller",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentBlueprintCustomEventPhasedCallerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), GetTransientPackage(),
		*FString(TEXT("BP_UAI_CustomEventPhase_") + FGuid::NewGuid().ToString()),
		BPTYPE_Normal, UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(), TEXT("UeAgentBlueprintGraphSignatureTests"));
	UEdGraph* EventGraph = Blueprint && !Blueprint->UbergraphPages.IsEmpty()
		? Blueprint->UbergraphPages[0] : nullptr;
	if (!TestNotNull(TEXT("the transient Blueprint exists"), Blueprint)
		|| !TestNotNull(TEXT("the EventGraph exists"), EventGraph))
	{
		return false;
	}
	TSharedRef<FJsonObject> Definition = MakeShared<FJsonObject>();
	Definition->SetStringField(TEXT("operation"), TEXT("create"));
	Definition->SetStringField(TEXT("node_type"), TEXT("custom_event"));
	Definition->SetStringField(TEXT("event_name"), TEXT("NotifyStatus"));
	Definition->SetObjectField(TEXT("signature"), MakeSignature(
		{MakePin(TEXT("Enabled"), TEXT("bool")), MakePin(TEXT("Count"), TEXT("int"))}, {}));
	TSharedRef<FJsonObject> CallerSpec = MakeShared<FJsonObject>();
	CallerSpec->SetStringField(TEXT("operation"), TEXT("create"));
	CallerSpec->SetStringField(TEXT("node_type"), TEXT("call_custom_event"));
	CallerSpec->SetStringField(TEXT("custom_event"), TEXT("NotifyStatus"));
	TSharedRef<FJsonObject> Nodes = MakeShared<FJsonObject>();
	Nodes->SetObjectField(TEXT("custom_event:NotifyStatus"), Definition);
	Nodes->SetObjectField(TEXT("call_custom_event:NotifyStatus"), CallerSpec);
	TSharedRef<FJsonObject> Header = MakeShared<FJsonObject>();
	Header->SetStringField(TEXT("name"), EventGraph->GetName());
	Header->SetStringField(TEXT("graph_kind"), TEXT("event_graph"));
	TSharedRef<FJsonObject> Graph = MakeShared<FJsonObject>();
	Graph->SetStringField(TEXT("operation"), TEXT("update"));
	Graph->SetObjectField(TEXT("graph"), Header);
	Graph->SetObjectField(TEXT("nodes"), Nodes);
	TSharedRef<FJsonObject> Graphs = MakeShared<FJsonObject>();
	Graphs->SetObjectField(TEXT("event_graph:EventGraph"), Graph);
	TSharedRef<FJsonObject> Extension = MakeShared<FJsonObject>();
	Extension->SetObjectField(TEXT("graphs"), Graphs);
	TUniquePtr<UeAgentBlueprintAuthoring::FPreparedBlueprintMutation> Mutation;
	UeAgentBlueprintAuthoring::FMutationCounters Counters;
	FString Error;
	if (!TestTrue(TEXT("the custom event settles before its caller"),
		UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::Prepare(
			*Blueprint, Extension, Mutation, Counters, Error)))
	{
		AddError(Error);
		return false;
	}
	UK2Node_CallFunction* Caller = nullptr;
	for (UEdGraphNode* Node : EventGraph->Nodes)
	{
		UK2Node_CallFunction* Candidate = Cast<UK2Node_CallFunction>(Node);
		if (Candidate && Candidate->GetTargetFunction()
			&& Candidate->GetTargetFunction()->GetFName() == TEXT("NotifyStatus"))
		{
			Caller = Candidate;
			break;
		}
	}
	return TestNotNull(TEXT("the real custom-event caller is created"), Caller)
		&& TestNotNull(TEXT("the first settled event parameter exists"), Caller->FindPin(TEXT("Enabled")))
		&& TestNotNull(TEXT("the second settled event parameter exists"), Caller->FindPin(TEXT("Count")));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentBlueprintCustomEventSignatureRoundTripTest,
	"UAI.Blueprint.Authoring.GraphSignature.CustomEventRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentBlueprintCustomEventSignatureRoundTripTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), GetTransientPackage(),
		*FString(TEXT("BP_UAI_CustomEventSignature_") + FGuid::NewGuid().ToString()),
		BPTYPE_Normal, UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(), TEXT("UeAgentBlueprintGraphSignatureTests"));
	UEdGraph* EventGraph = Blueprint && !Blueprint->UbergraphPages.IsEmpty()
		? Blueprint->UbergraphPages[0] : nullptr;
	if (!TestNotNull(TEXT("the EventGraph exists"), EventGraph))
	{
		return false;
	}
	TSharedRef<FJsonObject> Definition = MakeShared<FJsonObject>();
	Definition->SetStringField(TEXT("operation"), TEXT("create"));
	Definition->SetStringField(TEXT("node_type"), TEXT("custom_event"));
	Definition->SetStringField(TEXT("event_name"), TEXT("NotifyStatus"));
	Definition->SetObjectField(TEXT("signature"), MakeSignature(
		{MakePin(TEXT("Message"), TEXT("string")), MakePin(TEXT("Count"), TEXT("int"))}, {}));
	TSharedRef<FJsonObject> Nodes = MakeShared<FJsonObject>();
	Nodes->SetObjectField(TEXT("custom_event:NotifyStatus"), Definition);
	TSharedRef<FJsonObject> Header = MakeShared<FJsonObject>();
	Header->SetStringField(TEXT("name"), EventGraph->GetName());
	Header->SetStringField(TEXT("graph_kind"), TEXT("event_graph"));
	TSharedRef<FJsonObject> Graph = MakeShared<FJsonObject>();
	Graph->SetStringField(TEXT("operation"), TEXT("update"));
	Graph->SetObjectField(TEXT("graph"), Header);
	Graph->SetObjectField(TEXT("nodes"), Nodes);
	TSharedRef<FJsonObject> Graphs = MakeShared<FJsonObject>();
	Graphs->SetObjectField(TEXT("event_graph:EventGraph"), Graph);
	TSharedRef<FJsonObject> Extension = MakeShared<FJsonObject>();
	Extension->SetObjectField(TEXT("graphs"), Graphs);
	TUniquePtr<UeAgentBlueprintAuthoring::FPreparedBlueprintMutation> Mutation;
	UeAgentBlueprintAuthoring::FMutationCounters Counters;
	FString Error;
	if (!TestTrue(TEXT("the custom event definition owns its signature"),
		UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::Prepare(
			*Blueprint, Extension, Mutation, Counters, Error)))
	{
		AddError(Error);
		return false;
	}
	UK2Node_CustomEvent* Event = nullptr;
	for (UEdGraphNode* Node : EventGraph->Nodes)
	{
		UK2Node_CustomEvent* Candidate = Cast<UK2Node_CustomEvent>(Node);
		if (Candidate && Candidate->CustomFunctionName == TEXT("NotifyStatus"))
		{
			Event = Candidate;
			break;
		}
	}
	if (!TestNotNull(TEXT("the custom event definition exists"), Event))
	{
		return false;
	}
	TSharedRef<FJsonObject> Captured = MakeShared<FJsonObject>();
	if (!TestTrue(TEXT("the definition signature captures"),
		UeAgentBlueprintAuthoring::CaptureCustomEventSignature(*Event, Captured, Error)))
	{
		AddError(Error);
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Outputs = nullptr;
	return Captured->TryGetArrayField(TEXT("inputs"), Inputs)
		&& Captured->TryGetArrayField(TEXT("outputs"), Outputs)
		&& TestEqual(TEXT("both event parameters survive"), Inputs->Num(), 2)
		&& TestEqual(TEXT("custom events expose no outputs"), Outputs->Num(), 0)
		&& TestEqual(TEXT("event parameter order survives"),
			(*Inputs)[0]->AsObject()->GetStringField(TEXT("name")), FString(TEXT("Message")))
		&& TestFalse(TEXT("the event signature has no GUID"), Captured->HasField(TEXT("guid")))
		&& TestFalse(TEXT("the event signature has no path"), Captured->HasField(TEXT("path")));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentBlueprintDelegateSignaturePhasedApplyTest,
	"UAI.Blueprint.Authoring.GraphSignature.DelegatePhasedCaller",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentBlueprintDelegateSignaturePhasedApplyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), GetTransientPackage(),
		*FString(TEXT("BP_UAI_DelegatePhase_") + FGuid::NewGuid().ToString()),
		BPTYPE_Normal, UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(), TEXT("UeAgentBlueprintGraphSignatureTests"));
	UEdGraph* EventGraph = Blueprint && !Blueprint->UbergraphPages.IsEmpty()
		? Blueprint->UbergraphPages[0] : nullptr;
	if (!TestNotNull(TEXT("the transient Blueprint exists"), Blueprint)
		|| !TestNotNull(TEXT("the EventGraph exists"), EventGraph))
	{
		return false;
	}

	TSharedRef<FJsonObject> Delegate = MakeShared<FJsonObject>();
	Delegate->SetStringField(TEXT("operation"), TEXT("create"));
	Delegate->SetObjectField(
		TEXT("signature"),
		MakeSignature(
			{MakePin(TEXT("Enabled"), TEXT("bool")), MakePin(TEXT("Count"), TEXT("int"))},
			{}));
	TSharedRef<FJsonObject> Delegates = MakeShared<FJsonObject>();
	Delegates->SetObjectField(TEXT("StatusChanged"), Delegate);
	TSharedRef<FJsonObject> Members = MakeShared<FJsonObject>();
	Members->SetObjectField(TEXT("delegates"), Delegates);

	TSharedRef<FJsonObject> Call = MakeShared<FJsonObject>();
	Call->SetStringField(TEXT("operation"), TEXT("create"));
	Call->SetStringField(TEXT("node_type"), TEXT("call_delegate"));
	Call->SetStringField(TEXT("delegate_member"), TEXT("StatusChanged"));
	TSharedRef<FJsonObject> Nodes = MakeShared<FJsonObject>();
	Nodes->SetObjectField(TEXT("call_delegate:StatusChanged"), Call);
	TSharedRef<FJsonObject> EventHeader = MakeShared<FJsonObject>();
	EventHeader->SetStringField(TEXT("name"), EventGraph->GetName());
	EventHeader->SetStringField(TEXT("graph_kind"), TEXT("event_graph"));
	TSharedRef<FJsonObject> EventSpec = MakeShared<FJsonObject>();
	EventSpec->SetStringField(TEXT("operation"), TEXT("update"));
	EventSpec->SetObjectField(TEXT("graph"), EventHeader);
	EventSpec->SetObjectField(TEXT("nodes"), Nodes);
	TSharedRef<FJsonObject> Graphs = MakeShared<FJsonObject>();
	Graphs->SetObjectField(TEXT("event_graph:EventGraph"), EventSpec);
	TSharedRef<FJsonObject> Extension = MakeShared<FJsonObject>();
	Extension->SetObjectField(TEXT("members"), Members);
	Extension->SetObjectField(TEXT("graphs"), Graphs);

	TUniquePtr<UeAgentBlueprintAuthoring::FPreparedBlueprintMutation> Mutation;
	UeAgentBlueprintAuthoring::FMutationCounters Counters;
	FString Error;
	if (!TestTrue(TEXT("the delegate signature settles before its caller"),
		UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::Prepare(
			*Blueprint, Extension, Mutation, Counters, Error)))
	{
		AddError(Error);
		return false;
	}
	UK2Node_CallDelegate* Caller = nullptr;
	for (UEdGraphNode* Node : EventGraph->Nodes)
	{
		if (UK2Node_CallDelegate* Candidate = Cast<UK2Node_CallDelegate>(Node))
		{
			Caller = Candidate;
			break;
		}
	}
	return TestNotNull(TEXT("the real delegate caller is created"), Caller)
		&& TestEqual(TEXT("the readable member name binds the caller"),
			Caller->GetPropertyName(), FName(TEXT("StatusChanged")))
		&& TestNotNull(TEXT("the first settled signature pin exists"), Caller->FindPin(TEXT("Enabled")))
		&& TestNotNull(TEXT("the second settled signature pin exists"), Caller->FindPin(TEXT("Count")));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentBlueprintDelegateSignatureRoundTripTest,
	"UAI.Blueprint.Authoring.GraphSignature.DelegateRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentBlueprintDelegateSignatureRoundTripTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), GetTransientPackage(),
		*FString(TEXT("BP_UAI_DelegateSignature_") + FGuid::NewGuid().ToString()),
		BPTYPE_Normal, UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(), TEXT("UeAgentBlueprintGraphSignatureTests"));
	if (!TestNotNull(TEXT("the transient Blueprint exists"), Blueprint))
	{
		return false;
	}
	TSharedRef<FJsonObject> Delegate = MakeShared<FJsonObject>();
	Delegate->SetStringField(TEXT("operation"), TEXT("create"));
	Delegate->SetObjectField(
		TEXT("signature"),
		MakeSignature(
			{MakePin(TEXT("Message"), TEXT("string")), MakePin(TEXT("Count"), TEXT("int"))},
			{}));
	TSharedRef<FJsonObject> Delegates = MakeShared<FJsonObject>();
	Delegates->SetObjectField(TEXT("StatusChanged"), Delegate);
	TSharedRef<FJsonObject> Members = MakeShared<FJsonObject>();
	Members->SetObjectField(TEXT("delegates"), Delegates);
	TSharedRef<FJsonObject> Extension = MakeShared<FJsonObject>();
	Extension->SetObjectField(TEXT("members"), Members);
	TUniquePtr<UeAgentBlueprintAuthoring::FPreparedBlueprintMutation> Mutation;
	UeAgentBlueprintAuthoring::FMutationCounters Counters;
	FString Error;
	if (!TestTrue(TEXT("the member-owned delegate signature applies"),
		UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::Prepare(
			*Blueprint, Extension, Mutation, Counters, Error)))
	{
		AddError(Error);
		return false;
	}
	UEdGraph* SignatureGraph = nullptr;
	for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
	{
		if (Graph && Graph->GetFName() == TEXT("StatusChanged"))
		{
			SignatureGraph = Graph;
			break;
		}
	}
	if (!TestNotNull(TEXT("UE owns one derived signature graph"), SignatureGraph))
	{
		return false;
	}
	TSharedRef<FJsonObject> Captured = MakeShared<FJsonObject>();
	if (!TestTrue(TEXT("the derived graph recaptures into the member signature"),
		UeAgentBlueprintAuthoring::CaptureDelegateSignatureGraph(
			*SignatureGraph, Captured, Error)))
	{
		AddError(Error);
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Outputs = nullptr;
	return TestTrue(TEXT("delegate inputs are ordered"), Captured->TryGetArrayField(TEXT("inputs"), Inputs))
		&& TestTrue(TEXT("delegate outputs are explicit"), Captured->TryGetArrayField(TEXT("outputs"), Outputs))
		&& TestEqual(TEXT("both delegate parameters survive"), Inputs->Num(), 2)
		&& TestEqual(TEXT("multicast delegates have no outputs"), Outputs->Num(), 0)
		&& TestEqual(TEXT("the first parameter order survives"),
			(*Inputs)[0]->AsObject()->GetStringField(TEXT("name")), FString(TEXT("Message")))
		&& TestEqual(TEXT("the second parameter order survives"),
			(*Inputs)[1]->AsObject()->GetStringField(TEXT("name")), FString(TEXT("Count")))
		&& TestFalse(TEXT("the member signature has no GUID"), Captured->HasField(TEXT("guid")))
		&& TestFalse(TEXT("the member signature has no path"), Captured->HasField(TEXT("path")));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentBlueprintMacroGraphSignaturePhasedApplyTest,
	"UAI.Blueprint.Authoring.GraphSignature.MacroGraphPhasedApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentBlueprintMacroGraphSignaturePhasedApplyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), GetTransientPackage(),
		*FString(TEXT("BP_UAI_MacroPhase_") + FGuid::NewGuid().ToString()),
		BPTYPE_Normal, UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(), TEXT("UeAgentBlueprintGraphSignatureTests"));
	UEdGraph* EventGraph = Blueprint && !Blueprint->UbergraphPages.IsEmpty()
		? Blueprint->UbergraphPages[0] : nullptr;
	if (!TestNotNull(TEXT("the transient Blueprint exists"), Blueprint)
		|| !TestNotNull(TEXT("the EventGraph exists"), EventGraph))
	{
		return false;
	}

	TSharedRef<FJsonObject> MacroHeader = MakeShared<FJsonObject>();
	MacroHeader->SetStringField(TEXT("name"), TEXT("SelectStatus"));
	MacroHeader->SetStringField(TEXT("graph_kind"), TEXT("macro_graph"));
	MacroHeader->SetObjectField(
		TEXT("signature"),
		MakeSignature(
			{MakePin(TEXT("Enabled"), TEXT("bool"))},
			{MakePin(TEXT("Succeeded"), TEXT("bool"))}));
	TSharedRef<FJsonObject> MacroSpec = MakeShared<FJsonObject>();
	MacroSpec->SetStringField(TEXT("operation"), TEXT("create"));
	MacroSpec->SetObjectField(TEXT("graph"), MacroHeader);

	TSharedRef<FJsonObject> Instance = MakeShared<FJsonObject>();
	Instance->SetStringField(TEXT("operation"), TEXT("create"));
	Instance->SetStringField(TEXT("node_type"), TEXT("macro_instance"));
	Instance->SetStringField(TEXT("macro_graph"), TEXT("SelectStatus"));
	TSharedRef<FJsonObject> EventNodes = MakeShared<FJsonObject>();
	EventNodes->SetObjectField(TEXT("macro_instance:SelectStatus"), Instance);
	TSharedRef<FJsonObject> EventHeader = MakeShared<FJsonObject>();
	EventHeader->SetStringField(TEXT("name"), EventGraph->GetName());
	EventHeader->SetStringField(TEXT("graph_kind"), TEXT("event_graph"));
	TSharedRef<FJsonObject> EventSpec = MakeShared<FJsonObject>();
	EventSpec->SetStringField(TEXT("operation"), TEXT("update"));
	EventSpec->SetObjectField(TEXT("graph"), EventHeader);
	EventSpec->SetObjectField(TEXT("nodes"), EventNodes);

	TSharedRef<FJsonObject> Graphs = MakeShared<FJsonObject>();
	Graphs->SetObjectField(TEXT("macro_graph:SelectStatus"), MacroSpec);
	Graphs->SetObjectField(TEXT("event_graph:EventGraph"), EventSpec);
	TSharedRef<FJsonObject> Extension = MakeShared<FJsonObject>();
	Extension->SetObjectField(TEXT("graphs"), Graphs);
	TUniquePtr<UeAgentBlueprintAuthoring::FPreparedBlueprintMutation> Mutation;
	UeAgentBlueprintAuthoring::FMutationCounters Counters;
	FString Error;
	if (!TestTrue(TEXT("the same import settles the macro signature before its instance"),
		UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::Prepare(
			*Blueprint, Extension, Mutation, Counters, Error)))
	{
		AddError(Error);
		return false;
	}

	UK2Node_MacroInstance* MacroInstance = nullptr;
	for (UEdGraphNode* Node : EventGraph->Nodes)
	{
		if (UK2Node_MacroInstance* Candidate = Cast<UK2Node_MacroInstance>(Node))
		{
			if (Candidate->GetMacroGraph()
				&& Candidate->GetMacroGraph()->GetName() == TEXT("SelectStatus"))
			{
				MacroInstance = Candidate;
				break;
			}
		}
	}
	UEdGraphPin* InputPin = MacroInstance
		? MacroInstance->FindPin(TEXT("Enabled")) : nullptr;
	UEdGraphPin* OutputPin = MacroInstance
		? MacroInstance->FindPin(TEXT("Succeeded")) : nullptr;
	return TestNotNull(TEXT("the readable macro reference creates an instance"), MacroInstance)
		&& TestNotNull(TEXT("the settled input pin is available"), InputPin)
		&& TestEqual(TEXT("the macro entry output becomes an instance input"),
			InputPin->Direction, EGPD_Input)
		&& TestNotNull(TEXT("the settled output pin is available"), OutputPin)
		&& TestEqual(TEXT("the macro result input becomes an instance output"),
			OutputPin->Direction, EGPD_Output);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentBlueprintMacroGraphSignatureTest,
	"UAI.Blueprint.Authoring.GraphSignature.MacroGraphRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentBlueprintMacroGraphSignatureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), GetTransientPackage(),
		*FString(TEXT("BP_UAI_MacroSignature_") + FGuid::NewGuid().ToString()),
		BPTYPE_Normal, UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(), TEXT("UeAgentBlueprintGraphSignatureTests"));
	UEdGraph* Graph = Blueprint ? FBlueprintEditorUtils::CreateNewGraph(
		Blueprint, TEXT("SelectStatus"), UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass()) : nullptr;
	if (Graph)
	{
		FBlueprintEditorUtils::AddMacroGraph(Blueprint, Graph, true, nullptr);
	}
	if (!TestNotNull(TEXT("the macro graph exists"), Graph)
		|| !TestTrue(TEXT("the macro signature is writable"),
			UeAgentBlueprintAuthoring::CanAuthorMacroGraphSignature(*Graph)))
	{
		return false;
	}

	FString Error;
	if (!TestTrue(TEXT("the ordered macro signature applies"),
		UeAgentBlueprintAuthoring::ApplyMacroGraphSignature(
			*Graph,
			MakeSignature(
				{MakePin(TEXT("Enabled"), TEXT("bool")), MakePin(TEXT("Count"), TEXT("int"))},
				{MakePin(TEXT("Succeeded"), TEXT("bool"))}),
			Error)))
	{
		AddError(Error);
		return false;
	}

	TSharedRef<FJsonObject> Captured = MakeShared<FJsonObject>();
	if (!TestTrue(TEXT("the macro signature captures"),
		UeAgentBlueprintAuthoring::CaptureMacroGraphSignature(*Graph, Captured, Error)))
	{
		AddError(Error);
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* Outputs = nullptr;
	return TestTrue(TEXT("macro inputs are an ordered list"), Captured->TryGetArrayField(TEXT("inputs"), Inputs))
		&& TestTrue(TEXT("macro outputs are an ordered list"), Captured->TryGetArrayField(TEXT("outputs"), Outputs))
		&& TestEqual(TEXT("both macro inputs survive"), Inputs->Num(), 2)
		&& TestEqual(TEXT("the macro output survives once"), Outputs->Num(), 1)
		&& TestEqual(TEXT("the macro input order is preserved"),
			(*Inputs)[0]->AsObject()->GetStringField(TEXT("name")), FString(TEXT("Enabled")))
		&& TestEqual(TEXT("the second macro input follows"),
			(*Inputs)[1]->AsObject()->GetStringField(TEXT("name")), FString(TEXT("Count")))
		&& TestEqual(TEXT("the macro output survives"),
			(*Outputs)[0]->AsObject()->GetStringField(TEXT("name")), FString(TEXT("Succeeded")))
		&& TestFalse(TEXT("the macro signature has no graph GUID"), Captured->HasField(TEXT("guid")))
		&& TestFalse(TEXT("the macro signature has no machine path"), Captured->HasField(TEXT("path")));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentBlueprintGraphSignaturePhasedApplyTest,
	"UAI.Blueprint.Authoring.GraphSignature.FunctionGraphPhasedApply",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentBlueprintGraphSignaturePhasedApplyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), GetTransientPackage(),
		*FString(TEXT("BP_UAI_GraphPhase_") + FGuid::NewGuid().ToString()),
		BPTYPE_Normal, UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(), TEXT("UeAgentBlueprintGraphSignatureTests"));
	UEdGraph* EventGraph = Blueprint && !Blueprint->UbergraphPages.IsEmpty()
		? Blueprint->UbergraphPages[0] : nullptr;
	if (!TestNotNull(TEXT("the transient Blueprint exists"), Blueprint)
		|| !TestNotNull(TEXT("the EventGraph exists"), EventGraph))
	{
		return false;
	}

	TSharedRef<FJsonObject> FunctionHeader = MakeShared<FJsonObject>();
	FunctionHeader->SetStringField(TEXT("name"), TEXT("ComputeStatus"));
	FunctionHeader->SetStringField(TEXT("graph_kind"), TEXT("function_graph"));
	FunctionHeader->SetObjectField(
		TEXT("signature"),
		MakeSignature(
			{MakePin(TEXT("Enabled"), TEXT("bool"))},
			{MakePin(TEXT("Succeeded"), TEXT("bool"))}));
	TSharedRef<FJsonObject> FunctionSpec = MakeShared<FJsonObject>();
	FunctionSpec->SetStringField(TEXT("operation"), TEXT("create"));
	FunctionSpec->SetObjectField(TEXT("graph"), FunctionHeader);

	const FString CallNodeId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	TSharedRef<FJsonObject> CallNode = MakeShared<FJsonObject>();
	CallNode->SetStringField(TEXT("operation"), TEXT("create"));
	CallNode->SetStringField(TEXT("node_type"), TEXT("call_function"));
	CallNode->SetStringField(TEXT("function_graph"), TEXT("ComputeStatus"));
	TSharedRef<FJsonObject> EventNodes = MakeShared<FJsonObject>();
	EventNodes->SetObjectField(CallNodeId, CallNode);
	TSharedRef<FJsonObject> EventHeader = MakeShared<FJsonObject>();
	EventHeader->SetStringField(TEXT("name"), EventGraph->GetName());
	EventHeader->SetStringField(TEXT("graph_kind"), TEXT("event_graph"));
	TSharedRef<FJsonObject> EventSpec = MakeShared<FJsonObject>();
	EventSpec->SetStringField(TEXT("operation"), TEXT("update"));
	EventSpec->SetObjectField(TEXT("graph"), EventHeader);
	EventSpec->SetObjectField(TEXT("nodes"), EventNodes);

	TSharedRef<FJsonObject> Graphs = MakeShared<FJsonObject>();
	// Graph maps use their readable stable fallback identity as the key. The
	// same-Blueprint function reference itself remains the plain graph name.
	Graphs->SetObjectField(TEXT("function_graph:ComputeStatus"), FunctionSpec);
	Graphs->SetObjectField(TEXT("event_graph:EventGraph"), EventSpec);
	TSharedRef<FJsonObject> Extension = MakeShared<FJsonObject>();
	Extension->SetObjectField(TEXT("graphs"), Graphs);

	TUniquePtr<UeAgentBlueprintAuthoring::FPreparedBlueprintMutation> Mutation;
	UeAgentBlueprintAuthoring::FMutationCounters Counters;
	FString Error;
	if (!TestTrue(TEXT("the same import settles signatures before graph bodies"),
		UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::Prepare(
			*Blueprint, Extension, Mutation, Counters, Error)))
	{
		AddError(Error);
		return false;
	}

	UK2Node_CallFunction* Call = nullptr;
	FGuid CallGuid;
	FGuid::Parse(CallNodeId, CallGuid);
	for (UEdGraphNode* Node : EventGraph->Nodes)
	{
		if (Node && Node->NodeGuid == CallGuid)
		{
			Call = Cast<UK2Node_CallFunction>(Node);
			break;
		}
	}
	if (!TestNotNull(TEXT("the local function call is created"), Call)
		|| !TestNotNull(TEXT("the call resolves a real generated function"),
			Call ? Call->GetTargetFunction() : nullptr))
	{
		return false;
	}
	return TestEqual(TEXT("the readable graph reference binds the intended function"),
		Call->GetTargetFunction()->GetName(), FString(TEXT("ComputeStatus")))
		&& TestNotNull(TEXT("the reconstructed input Pin is available"),
			Call->FindPin(TEXT("Enabled")))
		&& TestNotNull(TEXT("the reconstructed output Pin is available"),
			Call->FindPin(TEXT("Succeeded")));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentBlueprintGraphSignatureTest,
	"UAI.Blueprint.Authoring.GraphSignature.FunctionGraphRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentBlueprintGraphSignatureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), GetTransientPackage(),
		*FString(TEXT("BP_UAI_GraphSignature_") + FGuid::NewGuid().ToString()),
		BPTYPE_Normal, UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(), TEXT("UeAgentBlueprintGraphSignatureTests"));
	UEdGraph* Graph = Blueprint
		? UBlueprintEditorLibrary::AddFunctionGraph(Blueprint, TEXT("ComputeStatus"))
		: nullptr;
	if (!TestNotNull(TEXT("the transient Blueprint exists"), Blueprint)
		|| !TestNotNull(TEXT("the function graph exists"), Graph))
	{
		return false;
	}

	TArray<UK2Node_FunctionResult*> Results;
	Graph->GetNodesOfClass(Results);
	TestEqual(TEXT("a void function initially has no result node"), Results.Num(), 0);
	TestTrue(TEXT("a user function signature is writable"),
		UeAgentBlueprintAuthoring::CanAuthorFunctionGraphSignature(*Graph));
	if (UEdGraph* ConstructionScript =
		FBlueprintEditorUtils::FindUserConstructionScript(Blueprint))
	{
		TestFalse(TEXT("the construction script is not a user function signature"),
			UeAgentBlueprintAuthoring::CanAuthorFunctionGraphSignature(*ConstructionScript));
	}

	FString Error;
	if (!TestTrue(TEXT("the ordered signature applies"),
		UeAgentBlueprintAuthoring::ApplyFunctionGraphSignature(
			*Graph,
			MakeSignature(
				{MakePin(TEXT("Enabled"), TEXT("bool"), TEXT("true")),
					MakePin(TEXT("Count"), TEXT("int"), TEXT("3"))},
				{MakePin(TEXT("Message"), TEXT("string"))}),
			Error)))
	{
		AddError(Error);
		return false;
	}
	Results.Reset();
	Graph->GetNodesOfClass(Results);
	TestEqual(TEXT("a result node is created only when outputs require one"),
		Results.Num(), 1);

	TSharedRef<FJsonObject> Captured = MakeShared<FJsonObject>();
	if (!TestTrue(TEXT("the applied signature is captured"),
		UeAgentBlueprintAuthoring::CaptureFunctionGraphSignature(*Graph, Captured, Error)))
	{
		AddError(Error);
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* CapturedInputs = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* CapturedOutputs = nullptr;
	if (!TestTrue(TEXT("inputs capture as an ordered list"),
		Captured->TryGetArrayField(TEXT("inputs"), CapturedInputs))
		|| !TestTrue(TEXT("outputs capture as an ordered list"),
			Captured->TryGetArrayField(TEXT("outputs"), CapturedOutputs))
		|| !TestEqual(TEXT("input order is preserved"), CapturedInputs->Num(), 2)
		|| !TestEqual(TEXT("first input name survives capture"),
			(*CapturedInputs)[0]->AsObject()->GetStringField(TEXT("name")),
			FString(TEXT("Enabled")))
		|| !TestEqual(TEXT("second input name survives capture"),
			(*CapturedInputs)[1]->AsObject()->GetStringField(TEXT("name")),
			FString(TEXT("Count")))
		|| !TestEqual(TEXT("output name survives capture"),
			(*CapturedOutputs)[0]->AsObject()->GetStringField(TEXT("name")),
			FString(TEXT("Message")))
		|| !TestEqual(TEXT("the input default round-trips"),
			(*CapturedInputs)[0]->AsObject()->GetStringField(TEXT("default_value")),
			FString(TEXT("true")))
		|| !TestFalse(TEXT("the signature has no graph GUID"), Captured->HasField(TEXT("guid")))
		|| !TestFalse(TEXT("the signature has no machine path"), Captured->HasField(TEXT("path"))))
	{
		return false;
	}

	if (!TestTrue(TEXT("the changed signature reapplies"),
		UeAgentBlueprintAuthoring::ApplyFunctionGraphSignature(
			*Graph,
			MakeSignature(
				{MakePin(TEXT("Count"), TEXT("int"), TEXT("5"))},
				{MakePin(TEXT("Succeeded"), TEXT("bool"))}),
			Error)))
	{
		AddError(Error);
		return false;
	}
	Captured = MakeShared<FJsonObject>();
	if (!TestTrue(TEXT("the changed signature recaptures"),
		UeAgentBlueprintAuthoring::CaptureFunctionGraphSignature(*Graph, Captured, Error)))
	{
		AddError(Error);
		return false;
	}
	Captured->TryGetArrayField(TEXT("inputs"), CapturedInputs);
	Captured->TryGetArrayField(TEXT("outputs"), CapturedOutputs);
	return TestEqual(TEXT("only the changed input remains"), CapturedInputs->Num(), 1)
		&& TestEqual(TEXT("the changed input is present"),
			(*CapturedInputs)[0]->AsObject()->GetStringField(TEXT("name")),
			FString(TEXT("Count")))
		&& TestEqual(TEXT("only the changed output remains"), CapturedOutputs->Num(), 1)
		&& TestEqual(TEXT("the changed output is present"),
			(*CapturedOutputs)[0]->AsObject()->GetStringField(TEXT("name")),
			FString(TEXT("Succeeded")));
}
