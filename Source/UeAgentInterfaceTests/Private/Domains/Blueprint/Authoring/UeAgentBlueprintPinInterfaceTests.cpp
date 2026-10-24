// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "DomainAdapters/Blueprint/Node/UeAgentBlueprintNodeMechanics.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringState.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintMutationModel.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_MakeArray.h"
#include "K2Node_Select.h"
#include "K2Node_SpawnActorFromClass.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	TSharedRef<FJsonObject> MakeGraphHeader(const UEdGraph& Graph)
	{
		TSharedRef<FJsonObject> Header = MakeShared<FJsonObject>();
		Header->SetStringField(TEXT("name"), Graph.GetName());
		Header->SetStringField(TEXT("graph_kind"), TEXT("event_graph"));
		Header->SetStringField(
			TEXT("guid"), Graph.GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
		return Header;
	}

	TSharedRef<FJsonObject> MakePinGroupNode(
		const UClass& NodeClass,
		const TCHAR* GroupId,
		const int32 Count,
		const TCHAR* Operation = TEXT("create"))
	{
		TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("operation"), Operation);
		Node->SetStringField(TEXT("node_type"), TEXT("node_by_class"));
		Node->SetStringField(TEXT("node_class"), NodeClass.GetPathName());
		TSharedRef<FJsonObject> Groups = MakeShared<FJsonObject>();
		Groups->SetNumberField(GroupId, Count);
		Node->SetObjectField(TEXT("pin_groups"), Groups);
		return Node;
	}

	TSharedRef<FJsonObject> MakeCandidatePinGroupNode(
		const TCHAR* CandidateName,
		const TCHAR* GroupId,
		const int32 Count)
	{
		TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("operation"), TEXT("create"));
		Node->SetStringField(TEXT("node_type"), TEXT("native_action"));
		TSharedRef<FJsonObject> Candidate = MakeShared<FJsonObject>();
		Candidate->SetStringField(TEXT("name"), CandidateName);
		Node->SetObjectField(TEXT("candidate"), Candidate);
		TSharedRef<FJsonObject> Groups = MakeShared<FJsonObject>();
		Groups->SetNumberField(GroupId, Count);
		Node->SetObjectField(TEXT("pin_groups"), Groups);
		return Node;
	}

	TSharedRef<FJsonObject> MakeExtension(
		const UEdGraph& Graph,
		const TSharedRef<FJsonObject>& Nodes,
		const TSharedPtr<FJsonObject>& Edges = nullptr)
	{
		TSharedRef<FJsonObject> GraphSpec = MakeShared<FJsonObject>();
		GraphSpec->SetStringField(TEXT("operation"), TEXT("update"));
		GraphSpec->SetObjectField(TEXT("graph"), MakeGraphHeader(Graph));
		GraphSpec->SetObjectField(TEXT("nodes"), Nodes);
		if (Edges.IsValid())
		{
			GraphSpec->SetObjectField(TEXT("edges"), Edges.ToSharedRef());
		}
		TSharedRef<FJsonObject> Graphs = MakeShared<FJsonObject>();
		Graphs->SetObjectField(
			Graph.GraphGuid.ToString(EGuidFormats::DigitsWithHyphensLower), GraphSpec);
		TSharedRef<FJsonObject> Extension = MakeShared<FJsonObject>();
		Extension->SetObjectField(TEXT("graphs"), Graphs);
		return Extension;
	}

	TSharedPtr<FJsonObject> FindPinGroup(
		const TArray<TSharedPtr<FJsonValue>>& Groups,
		const FString& Id)
	{
		for (const TSharedPtr<FJsonValue>& Value : Groups)
		{
			if (!Value.IsValid() || Value->Type != EJson::Object)
			{
				continue;
			}
			const TSharedPtr<FJsonObject>& Group = Value->AsObject();
			FString CandidateId;
			if (Group.IsValid()
				&& Group->TryGetStringField(TEXT("id"), CandidateId)
				&& CandidateId == Id)
			{
				return Group;
			}
		}
		return {};
	}

	TSharedRef<FJsonObject> MakePinDefault(const FString& DefaultValue)
	{
		TSharedRef<FJsonObject> PinDefault = MakeShared<FJsonObject>();
		PinDefault->SetStringField(TEXT("default_value"), DefaultValue);
		return PinDefault;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentBlueprintPinInterfaceTest,
	"UAI.Blueprint.Authoring.PinInterface.DynamicGroups",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentBlueprintPinInterfaceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), GetTransientPackage(),
		*FString(TEXT("BP_UAI_PinInterface_") + Suffix), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(),
		TEXT("UeAgentBlueprintPinInterfaceTests"));
	UEdGraph* Graph = Blueprint && !Blueprint->UbergraphPages.IsEmpty()
		? Blueprint->UbergraphPages[0]
		: nullptr;
	if (!TestNotNull(TEXT("the transient Blueprint exists"), Blueprint)
		|| !TestNotNull(TEXT("the transient EventGraph exists"), Graph))
	{
		return false;
	}

	using namespace UE::UAI::DomainAdapters::Blueprint::Node;
	FUeAgentBlueprintNodeCandidateQuery Query;
	Query.Target = Blueprint->GetPathName();
	Query.GraphName = Graph->GetName();
	Query.Query = TEXT("Sequence");
	FString DiscoveryJson;
	FUeAgentBlueprintNodeMechanicsError DiscoveryError;
	if (!TestTrue(
			TEXT("the real EventGraph context discovers Sequence by name"),
			TryQueryBlueprintNodeCandidates(
				Query, DiscoveryJson, DiscoveryError)))
	{
		AddError(DiscoveryError.Code + TEXT(":") + DiscoveryError.PublicMessage);
		return false;
	}
	TSharedPtr<FJsonObject> Discovery;
	const TSharedRef<TJsonReader<>> Reader =
		TJsonReaderFactory<>::Create(DiscoveryJson);
	const TArray<TSharedPtr<FJsonValue>>* Names = nullptr;
	if (!TestTrue(
			TEXT("the discovery payload is valid JSON"),
			FJsonSerializer::Deserialize(Reader, Discovery) && Discovery.IsValid())
		|| !TestTrue(
			TEXT("candidate discovery projects only names"),
			Discovery->TryGetArrayField(TEXT("names"), Names)
				&& Names && !Names->IsEmpty())
		|| !TestFalse(
			TEXT("candidate objects are not projected"),
			Discovery->HasField(TEXT("candidates")))
		|| !TestEqual(
			TEXT("names are the entire ordinary discovery payload"),
			Discovery->Values.Num(), 1))
	{
		return false;
	}
	FString SequenceCandidateName;
	for (const TSharedPtr<FJsonValue>& Name : *Names)
	{
		if (Name.IsValid() && Name->Type == EJson::String)
		{
			const FString PublicName = Name->AsString();
			if (PublicName.Equals(
					TEXT("Execution Sequence"), ESearchCase::IgnoreCase)
				|| PublicName.StartsWith(
					TEXT("Execution Sequence ["), ESearchCase::IgnoreCase))
			{
				SequenceCandidateName = PublicName;
				break;
			}
		}
	}
	if (!TestTrue(
			TEXT("the names-only result contains the stable Execution Sequence name"),
			!SequenceCandidateName.IsEmpty()))
	{
		return false;
	}

	const FString SequenceId =
		TEXT("11111111-1111-1111-1111-111111111111");
	const FString MakeArrayId =
		TEXT("22222222-2222-2222-2222-222222222222");
	const FString TargetSequenceId =
		TEXT("33333333-3333-3333-3333-333333333333");
	TSharedRef<FJsonObject> Nodes = MakeShared<FJsonObject>();
	Nodes->SetObjectField(
		SequenceId,
		MakeCandidatePinGroupNode(
			*SequenceCandidateName,
			TEXT("execution_outputs"), 4));
	Nodes->SetObjectField(
		MakeArrayId,
		MakePinGroupNode(
			*UK2Node_MakeArray::StaticClass(), TEXT("inputs"), 3));
	Nodes->SetObjectField(
		TargetSequenceId,
		MakeCandidatePinGroupNode(
			*SequenceCandidateName, TEXT("execution_outputs"), 2));

	TUniquePtr<UeAgentBlueprintAuthoring::FPreparedBlueprintMutation> Mutation;
	UeAgentBlueprintAuthoring::FMutationCounters Counters;
	FString Error;
	if (!TestTrue(
			TEXT("Sequence and Make Array variable pin groups pass preflight"),
			UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::Prepare(
				*Blueprint, MakeExtension(*Graph, Nodes), Mutation, Counters, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("both native pin groups resize"), Counters.PinGroupsResized, 2);

	UK2Node_ExecutionSequence* Sequence = nullptr;
	UK2Node_ExecutionSequence* TargetSequence = nullptr;
	UK2Node_MakeArray* MakeArray = nullptr;
	FGuid SequenceGuid;
	FGuid TargetSequenceGuid;
	FGuid::Parse(SequenceId, SequenceGuid);
	FGuid::Parse(TargetSequenceId, TargetSequenceGuid);
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->NodeGuid == SequenceGuid)
		{
			Sequence = Cast<UK2Node_ExecutionSequence>(Node);
		}
		if (Node && Node->NodeGuid == TargetSequenceGuid)
		{
			TargetSequence = Cast<UK2Node_ExecutionSequence>(Node);
		}
		if (!MakeArray)
		{
			MakeArray = Cast<UK2Node_MakeArray>(Node);
		}
	}
	if (!TestNotNull(TEXT("the Sequence node is materialized"), Sequence)
		|| !TestNotNull(TEXT("the target Sequence node is materialized"), TargetSequence)
		|| !TestNotNull(TEXT("the Make Array node is materialized"), MakeArray))
	{
		return false;
	}

	auto VerifyState = [this](
		const UEdGraphNode& Node,
		const TCHAR* GroupId,
		const int32 ExpectedCount)
	{
		FJsonObject State;
		UeAgentBlueprintAuthoring::AppendGraphNodeSemanticState(Node, State);
		const TSharedPtr<FJsonObject>* WritableGroups = nullptr;
		double WritableCount = 0.0;
		const TSharedPtr<FJsonObject>* Interface = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* InterfaceGroups = nullptr;
		if (!TestTrue(TEXT("writable pin_groups are exported"),
				State.TryGetObjectField(TEXT("pin_groups"), WritableGroups))
			|| !TestTrue(TEXT("the writable group count exists"),
				(*WritableGroups)->TryGetNumberField(GroupId, WritableCount))
			|| !TestEqual(TEXT("the writable group count matches the instance"),
				static_cast<int32>(FMath::RoundToDouble(WritableCount)), ExpectedCount)
			|| !TestTrue(TEXT("the read-only pin interface is exported"),
				State.TryGetObjectField(TEXT("pin_interface"), Interface))
			|| !TestTrue(TEXT("the instance pins are exported"),
				(*Interface)->TryGetArrayField(TEXT("pins"), Pins))
			|| !TestTrue(TEXT("the variable groups are exported"),
				(*Interface)->TryGetArrayField(TEXT("pin_groups"), InterfaceGroups)))
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Group = FindPinGroup(*InterfaceGroups, GroupId);
		double CurrentCount = 0.0;
		const TArray<TSharedPtr<FJsonValue>>* Members = nullptr;
		return TestTrue(TEXT("the expected variable group is present"), Group.IsValid())
			&& TestTrue(TEXT("the instance group count is readable"),
				Group->TryGetNumberField(TEXT("current_count"), CurrentCount))
			&& TestEqual(TEXT("the interface group count matches the instance"),
				static_cast<int32>(FMath::RoundToDouble(CurrentCount)), ExpectedCount)
			&& TestTrue(TEXT("the current group members are exported"),
				Group->TryGetArrayField(TEXT("members"), Members))
			&& TestEqual(TEXT("the member list matches the current group size"),
				Members->Num(), ExpectedCount)
			&& TestTrue(TEXT("the node exposes at least the group members"),
				Pins->Num() >= Members->Num());
	};

	if (!VerifyState(*Sequence, TEXT("execution_outputs"), 4)
		|| !VerifyState(*MakeArray, TEXT("inputs"), 3))
	{
		return false;
	}

	TSharedRef<FJsonObject> ResizeNodes = MakeShared<FJsonObject>();
	ResizeNodes->SetObjectField(
		SequenceId,
		MakePinGroupNode(
			*UK2Node_ExecutionSequence::StaticClass(),
			TEXT("execution_outputs"), 5, TEXT("update")));
	TSharedRef<FJsonObject> From = MakeShared<FJsonObject>();
	From->SetStringField(TEXT("node"), SequenceId);
	From->SetStringField(TEXT("pin"), TEXT("Then_4"));
	TSharedRef<FJsonObject> To = MakeShared<FJsonObject>();
	To->SetStringField(TEXT("node"), TargetSequenceId);
	To->SetStringField(TEXT("pin"), TargetSequence->GetExecPin()->PinName.ToString());
	TSharedRef<FJsonObject> Edge = MakeShared<FJsonObject>();
	Edge->SetStringField(TEXT("operation"), TEXT("create"));
	Edge->SetObjectField(TEXT("from"), From);
	Edge->SetObjectField(TEXT("to"), To);
	TSharedRef<FJsonObject> Edges = MakeShared<FJsonObject>();
	Edges->SetObjectField(TEXT("new_dynamic_pin_edge"), Edge);
	Mutation.Reset();
	Counters = {};
	Error.Reset();
	if (!TestTrue(
		TEXT("a new connection resolves the just-resized real pin interface"),
		UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::Prepare(
			*Blueprint, MakeExtension(*Graph, ResizeNodes, Edges),
			Mutation, Counters, Error)))
	{
		AddError(Error);
		return false;
	}
	UEdGraphPin* Then4 = Sequence->FindPin(TEXT("Then_4"));
	UEdGraphPin* TargetExec = TargetSequence->GetExecPin();
	if (!TestNotNull(TEXT("the resized Sequence exposes Then_4"), Then4)
		|| !TestNotNull(TEXT("the target Sequence exposes its exec input"), TargetExec)
		|| !TestTrue(
			TEXT("Then_4 is really linked to the target Sequence exec input"),
			Then4->LinkedTo.Contains(TargetExec)
				&& TargetExec->LinkedTo.Contains(Then4)))
	{
		return false;
	}

	TSharedRef<FJsonObject> NonzeroFrom = MakeShared<FJsonObject>();
	NonzeroFrom->SetStringField(TEXT("node"), SequenceId);
	NonzeroFrom->SetStringField(TEXT("pin"), TEXT("Then_4"));
	NonzeroFrom->SetNumberField(TEXT("pin_occurrence"), 1);
	TSharedRef<FJsonObject> NonzeroEdge = MakeShared<FJsonObject>();
	NonzeroEdge->SetStringField(TEXT("operation"), TEXT("create"));
	NonzeroEdge->SetObjectField(TEXT("from"), NonzeroFrom);
	NonzeroEdge->SetObjectField(TEXT("to"), To);
	TSharedRef<FJsonObject> NonzeroEdges = MakeShared<FJsonObject>();
	NonzeroEdges->SetObjectField(TEXT("nonzero_pin_occurrence"), NonzeroEdge);
	Mutation.Reset();
	Counters = {};
	Error.Reset();
	TestFalse(
		TEXT("a nonzero occurrence is resolved against the real matching pin list"),
		UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::Prepare(
			*Blueprint, MakeExtension(*Graph, MakeShared<FJsonObject>(), NonzeroEdges),
			Mutation, Counters, Error));
	TestTrue(
		TEXT("a missing nonzero occurrence has a typed diagnostic"),
		Error.StartsWith(TEXT("blueprint_pin_occurrence_not_found")));

	TSharedRef<FJsonObject> RetiredNodes = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> RetiredSequence = MakeShared<FJsonObject>();
	RetiredSequence->SetStringField(TEXT("operation"), TEXT("update"));
	RetiredSequence->SetStringField(TEXT("node_type"), TEXT("node_by_class"));
	RetiredSequence->SetStringField(
		TEXT("node_class"), UK2Node_ExecutionSequence::StaticClass()->GetPathName());
	RetiredSequence->SetNumberField(TEXT("exec_output_pin_count"), 6);
	RetiredNodes->SetObjectField(SequenceId, RetiredSequence);
	Mutation.Reset();
	Counters = {};
	Error.Reset();
	TestFalse(
		TEXT("the retired Sequence-only field fails closed"),
		UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::Prepare(
			*Blueprint, MakeExtension(*Graph, RetiredNodes),
			Mutation, Counters, Error));
	TestTrue(
		TEXT("the retired field has an explicit diagnostic"),
		Error.StartsWith(TEXT("blueprint_exec_output_pin_count_retired")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentBlueprintPropertyDrivenPinInterfaceTest,
	"UAI.Blueprint.Authoring.PinInterface.PropertyDrivenInterface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentBlueprintPropertyDrivenPinInterfaceTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), GetTransientPackage(),
		*FString(TEXT("BP_UAI_PropertyDrivenPinInterface_") + Suffix),
		BPTYPE_Normal, UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		TEXT("UeAgentBlueprintPinInterfaceTests"));
	UEdGraph* Graph = Blueprint && !Blueprint->UbergraphPages.IsEmpty()
		? Blueprint->UbergraphPages[0]
		: nullptr;
	if (!TestNotNull(TEXT("the transient Blueprint exists"), Blueprint)
		|| !TestNotNull(TEXT("the transient EventGraph exists"), Graph))
	{
		return false;
	}

	UClass* LevelSequenceActorClass = LoadObject<UClass>(
		nullptr, TEXT("/Script/LevelSequence.LevelSequenceActor"));
	if (!TestNotNull(
		TEXT("the Level Sequence Actor class is available"),
		LevelSequenceActorClass))
	{
		return false;
	}

	UK2Node_SpawnActorFromClass* UnconfiguredSpawn =
		NewObject<UK2Node_SpawnActorFromClass>(Graph);
	UnconfiguredSpawn->AllocateDefaultPins();
	if (!TestNull(
		TEXT("the old unconfigured Spawn Actor interface has no exposed property pin"),
		UnconfiguredSpawn->FindPin(TEXT("bReplicatePlayback"))))
	{
		return false;
	}

	const FString SpawnId =
		TEXT("44444444-4444-4444-4444-444444444444");
	TSharedRef<FJsonObject> Spawn = MakeShared<FJsonObject>();
	Spawn->SetStringField(TEXT("operation"), TEXT("create"));
	Spawn->SetStringField(TEXT("node_type"), TEXT("node_by_class"));
	Spawn->SetStringField(
		TEXT("node_class"), UK2Node_SpawnActorFromClass::StaticClass()->GetPathName());

	// FString ordering visits bReplicatePlayback before Class. The mutation must
	// defer that unresolved Pin, let Class settle the real node interface, and retry.
	TSharedRef<FJsonObject> PinDefaults = MakeShared<FJsonObject>();
	PinDefaults->SetObjectField(
		TEXT("pin:Class"), MakePinDefault(LevelSequenceActorClass->GetPathName()));
	PinDefaults->SetObjectField(
		TEXT("pin:bReplicatePlayback"), MakePinDefault(TEXT("true")));
	Spawn->SetObjectField(TEXT("pin_defaults"), PinDefaults);

	TSharedRef<FJsonObject> Nodes = MakeShared<FJsonObject>();
	Nodes->SetObjectField(SpawnId, Spawn);
	TUniquePtr<UeAgentBlueprintAuthoring::FPreparedBlueprintMutation> Mutation;
	UeAgentBlueprintAuthoring::FMutationCounters Counters;
	FString Error;
	if (!TestTrue(
		TEXT("one mutation settles Class before applying the exposed property default"),
		UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::Prepare(
			*Blueprint, MakeExtension(*Graph, Nodes),
			Mutation, Counters, Error)))
	{
		AddError(Error);
		return false;
	}

	FGuid SpawnGuid;
	FGuid::Parse(SpawnId, SpawnGuid);
	UK2Node_SpawnActorFromClass* SpawnNode = nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->NodeGuid == SpawnGuid)
		{
			SpawnNode = Cast<UK2Node_SpawnActorFromClass>(Node);
			break;
		}
	}
	if (!TestNotNull(TEXT("the real Spawn Actor node is materialized"), SpawnNode))
	{
		return false;
	}

	UEdGraphPin* ClassPin = SpawnNode->FindPin(TEXT("Class"));
	UEdGraphPin* ScaleMethodPin =
		SpawnNode->FindPin(TEXT("TransformScaleMethod"));
	UEdGraphPin* ReplicatePlaybackPin =
		SpawnNode->FindPin(TEXT("bReplicatePlayback"));
	return TestNotNull(TEXT("the Spawn Actor Class pin remains available"), ClassPin)
		&& TestTrue(
			TEXT("the Class default selected Level Sequence Actor"),
			ClassPin->DefaultObject == LevelSequenceActorClass)
		&& TestNotNull(
			TEXT("default pins exist before Spawn Actor post-placement initialization"),
			ScaleMethodPin)
		&& TestEqual(
			TEXT("post-placement initialization updates its prepared scale-method pin"),
			ScaleMethodPin->DefaultValue, FString(TEXT("MultiplyWithRoot")))
		&& TestNotNull(
			TEXT("the selected class exposes the readable Replicate Playback pin"),
			ReplicatePlaybackPin)
		&& TestEqual(
			TEXT("the new interface receives its default in the same mutation"),
			ReplicatePlaybackPin->DefaultValue, FString(TEXT("true")))
		&& TestEqual(
			TEXT("both pin defaults passed through the mutation mechanics"),
			Counters.PinDefaultsApplied, 2);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentBlueprintConnectionDrivenPinInterfaceTest,
	"UAI.Blueprint.Authoring.PinInterface.ConnectionDrivenInterface",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentBlueprintConnectionDrivenPinInterfaceTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), GetTransientPackage(),
		*FString(TEXT("BP_UAI_ConnectionDrivenPinInterface_") + Suffix),
		BPTYPE_Normal, UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		TEXT("UeAgentBlueprintPinInterfaceTests"));
	UEdGraph* Graph = Blueprint && !Blueprint->UbergraphPages.IsEmpty()
		? Blueprint->UbergraphPages[0]
		: nullptr;
	if (!TestNotNull(TEXT("the transient Blueprint exists"), Blueprint)
		|| !TestNotNull(TEXT("the transient EventGraph exists"), Graph))
	{
		return false;
	}

	FEdGraphPinType IntegerType;
	IntegerType.PinCategory = UEdGraphSchema_K2::PC_Int;
	if (!TestTrue(TEXT("the source integer member is created"),
			FBlueprintEditorUtils::AddMemberVariable(
				Blueprint, TEXT("SourceValue"), IntegerType))
		|| !TestTrue(TEXT("the target integer member is created"),
			FBlueprintEditorUtils::AddMemberVariable(
				Blueprint, TEXT("TargetValue"), IntegerType)))
	{
		return false;
	}

	const FString GetId = TEXT("55555555-5555-5555-5555-555555555555");
	const FString SelectId = TEXT("66666666-6666-6666-6666-666666666666");
	const FString SetId = TEXT("77777777-7777-7777-7777-777777777777");
	auto MakeVariableNode = [](const TCHAR* Name, const TCHAR* Access)
	{
		TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("operation"), TEXT("create"));
		Node->SetStringField(TEXT("node_type"), TEXT("variable_node"));
		Node->SetStringField(TEXT("variable_name"), Name);
		Node->SetStringField(TEXT("access"), Access);
		return Node;
	};
	TSharedRef<FJsonObject> Select = MakeShared<FJsonObject>();
	Select->SetStringField(TEXT("operation"), TEXT("create"));
	Select->SetStringField(TEXT("node_type"), TEXT("node_by_class"));
	Select->SetStringField(
		TEXT("node_class"), UK2Node_Select::StaticClass()->GetPathName());
	TSharedRef<FJsonObject> SelectDefaults = MakeShared<FJsonObject>();
	SelectDefaults->SetObjectField(
		TEXT("pin:Option 1"), MakePinDefault(TEXT("7")));
	Select->SetObjectField(TEXT("pin_defaults"), SelectDefaults);

	TSharedRef<FJsonObject> Nodes = MakeShared<FJsonObject>();
	Nodes->SetObjectField(GetId, MakeVariableNode(TEXT("SourceValue"), TEXT("get")));
	Nodes->SetObjectField(SelectId, Select);
	Nodes->SetObjectField(SetId, MakeVariableNode(TEXT("TargetValue"), TEXT("set")));
	auto MakeEndpoint = [](const FString& Node, const TCHAR* Pin)
	{
		TSharedRef<FJsonObject> Endpoint = MakeShared<FJsonObject>();
		Endpoint->SetStringField(TEXT("node"), Node);
		Endpoint->SetStringField(TEXT("pin"), Pin);
		return Endpoint;
	};
	auto MakeEdge = [&](const FString& FromNode, const TCHAR* FromPin,
		const FString& ToNode, const TCHAR* ToPin)
	{
		TSharedRef<FJsonObject> Edge = MakeShared<FJsonObject>();
		Edge->SetObjectField(TEXT("from"), MakeEndpoint(FromNode, FromPin));
		Edge->SetObjectField(TEXT("to"), MakeEndpoint(ToNode, ToPin));
		return Edge;
	};
	TSharedRef<FJsonObject> Edges = MakeShared<FJsonObject>();
	Edges->SetObjectField(TEXT("edge:a-return"), MakeEdge(
		SelectId, TEXT("ReturnValue"), SetId, TEXT("TargetValue")));
	Edges->SetObjectField(TEXT("edge:z-option"), MakeEdge(
		GetId, TEXT("SourceValue"), SelectId, TEXT("Option 0")));

	TUniquePtr<UeAgentBlueprintAuthoring::FPreparedBlueprintMutation> Mutation;
	UeAgentBlueprintAuthoring::FMutationCounters Counters;
	FString Error;
	if (!TestTrue(
		TEXT("one mutation settles wildcard connections and a typed default"),
		UeAgentBlueprintAuthoring::FPreparedBlueprintMutation::Prepare(
			*Blueprint, MakeExtension(*Graph, Nodes, Edges),
			Mutation, Counters, Error)))
	{
		AddError(Error);
		return false;
	}

	FGuid SelectGuid;
	FGuid::Parse(SelectId, SelectGuid);
	UK2Node_Select* SelectNode = nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->NodeGuid == SelectGuid)
		{
			SelectNode = Cast<UK2Node_Select>(Node);
			break;
		}
	}
	if (!TestNotNull(TEXT("the real Select node is materialized"), SelectNode))
	{
		return false;
	}
	TArray<UEdGraphPin*> OptionPins;
	SelectNode->GetOptionPins(OptionPins);
	UEdGraphPin* ReturnPin = SelectNode->GetReturnValuePin();
	return TestNotNull(TEXT("Select keeps its return Pin"), ReturnPin)
		&& TestTrue(TEXT("Select keeps both option Pins"), OptionPins.Num() >= 2)
		&& TestEqual(TEXT("the return Pin settles to integer"),
			ReturnPin->PinType.PinCategory, UEdGraphSchema_K2::PC_Int)
		&& TestEqual(TEXT("the first option settles to integer"),
			OptionPins[0]->PinType.PinCategory, UEdGraphSchema_K2::PC_Int)
		&& TestEqual(TEXT("the second option settles to integer"),
			OptionPins[1]->PinType.PinCategory, UEdGraphSchema_K2::PC_Int)
		&& TestEqual(TEXT("the pre-connection default survives type settle"),
			OptionPins[1]->DefaultValue, FString(TEXT("7")))
		&& TestEqual(TEXT("both dependent connections are applied"),
			Counters.EdgesAdded, 2);
}
