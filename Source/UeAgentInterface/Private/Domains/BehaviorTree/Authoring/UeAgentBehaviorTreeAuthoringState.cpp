#include "Domains/BehaviorTree/Authoring/UeAgentBehaviorTreeAuthoringState.h"

#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_CompositeDecorator.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/BehaviorTree/Authoring/UeAgentBehaviorTreeNativeGraph.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraph.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "UObject/Class.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"

namespace UeAgentBehaviorTreeAuthoring
{
	namespace
	{
		constexpr const TCHAR* AccessorId = TEXT("behavior_tree.graph.state");
		constexpr const TCHAR* GraphKind = TEXT("behavior_tree.graph");
		constexpr const TCHAR* NodeKind = TEXT("behavior_tree.node");
		constexpr const TCHAR* PropertyKind = TEXT("behavior_tree.node_property");
		constexpr const TCHAR* EdgeKind = TEXT("behavior_tree.edge");

		FString IdentityModel(const FString& Kind) { return Kind + TEXT(".identity"); }

		TSharedRef<FJsonObject> Endpoint(const FString& NodeId, const UEdGraphPin& Pin)
		{
			int32 Occurrence = 0;
			for (const UEdGraphPin* Other : Pin.GetOwningNode()->Pins)
			{
				if (Other == &Pin) break;
				if (Other && Other->Direction == Pin.Direction && Other->PinName == Pin.PinName) ++Occurrence;
			}
			const auto Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("node"), NodeId);
			Value->SetStringField(TEXT("pin"), Pin.PinName.ToString());
			Value->SetNumberField(TEXT("pin_occurrence"), Occurrence);
			return Value;
		}

		bool AddRequest(const TCHAR* Semantic, const TCHAR* Kind,
			const TMap<FString, FString>& Selectors, const UObject* Object,
			TArray<UeAgentAuthoringState::FReadRequest>& Requests, UE::UAI::Contract::FDiagnostic& Error)
		{
			UeAgentAuthoringState::FReadRequest Request;
			Request.SemanticId = Semantic;
			if (!UeAgentAuthoringIdentity::BuildCanonicalIdentity(
				IdentityModel(Kind), Kind, Selectors, Request.TargetIdentity, Error)) return false;
			Request.TargetQuery.ModelId = Request.TargetIdentity.ModelId;
			Request.TargetQuery.TargetKind = Request.TargetIdentity.TargetKind;
			Request.TargetQuery.CanonicalKey = Request.TargetIdentity.CanonicalKey;
			Request.RootObject = Object;
			Request.RootOwnerPath = Selectors.FindRef(TEXT("asset_path"));
			Requests.Add(MoveTemp(Request));
			return true;
		}

		UeAgentAuthoringState::EReadOutcome ReadState(const UeAgentAuthoringState::FReadRequest& Request,
			UeAgentAuthoringState::FStateValue& OutValue, UE::UAI::Contract::FDiagnostic& OutError)
		{
			using EOutcome = UeAgentAuthoringState::EReadOutcome;
			OutValue = {};
			OutValue.TargetIdentity = Request.TargetIdentity;
			OutValue.Type.Kind = TEXT("object");
			OutValue.Provenance = AccessorId;
			OutError.Reset();
			const auto Failure = [&](const TCHAR* Code, const TCHAR* Reason, const FString& Path,
				TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
			{
				OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Path, Expected, Actual, TEXT("authoring_state"), Reason);
				OutError.SourceKind = TEXT("native_result");
				OutError.TargetPath = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
				return EOutcome::Failed;
			};
			TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
			if (Request.SemanticId == GraphSemanticId)
			{
				const auto* Tree = Cast<UBehaviorTree>(Request.RootObject);
				const auto* NativeGraph = Cast<UEdGraph>(Request.RootObject);
				const auto* Owner = NativeGraph ? Cast<UBehaviorTreeGraphNode_CompositeDecorator>(NativeGraph->GetOuter()) : nullptr;
				if (!Tree && !Owner)
				{
					const auto Actual = MakeShared<FJsonObject>();
					if (Request.RootObject) Actual->SetStringField(TEXT("root_type"), Request.RootObject->GetClass()->GetPathName());
					if (NativeGraph && NativeGraph->GetOuter()) Actual->SetStringField(TEXT("graph_owner_type"), NativeGraph->GetOuter()->GetClass()->GetPathName());
					return Failure(TEXT("behavior_tree_graph_target_missing"), TEXT("Graph capture requires a BehaviorTree asset or a graph owned by a composite decorator."),
						TEXT("/root_object"), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("BehaviorTree or composite-decorator graph")));
				}
				if (NativeGraph) OutValue.bEditable = NativeGraph->bEditable;
				const auto Graph = MakeShared<FJsonObject>();
				Graph->SetStringField(TEXT("name"), Tree ? Tree->GetName() : Owner->CompositeName);
				Graph->SetStringField(TEXT("graph_kind"), Tree ? TEXT("behavior_tree") : TEXT("behavior_tree_decorator"));
				State->SetObjectField(TEXT("graph"), Graph);
				if (NativeGraph && !NativeGraph->bEditable) State->SetBoolField(TEXT("read_only"), true);
				if (Tree) State->SetField(TEXT("blackboard"), Tree->BlackboardAsset
					? StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueString>(Tree->BlackboardAsset->GetPathName()))
					: StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueNull>()));
			}
			else if (Request.SemanticId == NodeSemanticId)
			{
				const auto* Node = Cast<UEdGraphNode>(Request.RootObject);
				FNodeCapture Captured;
				if (!Node || !CaptureNode(*Node, Captured, OutError))
				{
					if (OutError.IsEmpty()) return Failure(TEXT("behavior_tree_node_target_missing"),
						!Node ? TEXT("Node capture requires an EdGraphNode object.") : TEXT("The Behavior Tree node capture failed without providing a diagnostic."),
						TEXT("/root_object"), Request.RootObject ? MakeShared<FJsonValueString>(Request.RootObject->GetClass()->GetPathName()) : TSharedPtr<FJsonValue>(),
						MakeShared<FJsonValueString>(TEXT("capturable EdGraphNode")));
					return EOutcome::Failed;
				}
				State = Captured.State;
				bool bReadOnly = false;
				State->TryGetBoolField(TEXT("read_only"), bReadOnly);
				OutValue.bEditable = !bReadOnly && (!Node->GetGraph() || Node->GetGraph()->bEditable);
				State->SetStringField(TEXT("id"), Captured.Id);
				State->SetStringField(TEXT("class_path"), Captured.PayloadClass);
				State->RemoveField(TEXT("node_class"));
				State->RemoveField(TEXT("native_guid"));
				State->RemoveField(TEXT("properties"));
				TArray<TSharedPtr<FJsonValue>> Pins = State->GetArrayField(TEXT("pins"));
				TMap<FString, int32> Occurrences;
				for (const auto& Value : Pins)
				{
					const auto Pin = Value->AsObject();
					const FString Key = Pin->GetStringField(TEXT("direction")) + TEXT(":") + Pin->GetStringField(TEXT("name"));
					Pin->RemoveField(TEXT("index"));
					Pin->SetNumberField(TEXT("occurrence"), Occurrences.FindOrAdd(Key)++);
				}
				const auto Interface = MakeShared<FJsonObject>();
				const bool bGraphReadOnly = Node->GetGraph() && !Node->GetGraph()->bEditable;
				FString Logic;
				if (State->TryGetStringField(TEXT("logic"), Logic) && (Logic == TEXT("And") || Logic == TEXT("Or")))
				{
					int32 Inputs = 0;
					for (const auto& Pin : Pins) Inputs += Pin->AsObject()->GetStringField(TEXT("direction")) == TEXT("input");
					const auto Counts = MakeShared<FJsonObject>();
					Counts->SetNumberField(TEXT("inputs"), Inputs);
					State->SetObjectField(TEXT("pin_groups"), Counts);
					const auto Group = MakeShared<FJsonObject>();
					Group->SetStringField(TEXT("id"), TEXT("inputs"));
					Group->SetNumberField(TEXT("current_count"), Inputs);
					Group->SetNumberField(TEXT("min_count"), 2);
					Group->SetBoolField(TEXT("can_add"), !bGraphReadOnly);
					Group->SetBoolField(TEXT("can_remove"), !bGraphReadOnly && Inputs > 2);
					Interface->SetArrayField(TEXT("pin_groups"), {MakeShared<FJsonValueObject>(Group)});
				}
				Interface->SetArrayField(TEXT("pins"), MoveTemp(Pins));
				State->RemoveField(TEXT("pins"));
				State->SetObjectField(TEXT("pin_interface"), Interface);
			}
			else if (Request.SemanticId == EdgeSemanticId)
			{
				const auto* Node = Cast<UEdGraphNode>(Request.RootObject);
				if (Node && Node->GetGraph()) OutValue.bEditable = Node->GetGraph()->bEditable;
				const int32 SourceIndex = FCString::Atoi(*Request.TargetIdentity.GetSelector(TEXT("source_pin")));
				const int32 TargetIndex = FCString::Atoi(*Request.TargetIdentity.GetSelector(TEXT("target_pin")));
				const FString TargetId = Request.TargetIdentity.GetSelector(TEXT("target_node"));
				const UEdGraphPin* Pin = Node && Node->Pins.IsValidIndex(SourceIndex) ? Node->Pins[SourceIndex] : nullptr;
				const UEdGraphPin* TargetPin = nullptr;
				if (Pin && Pin->Direction == EGPD_Output)
					for (const UEdGraphPin* Linked : Pin->LinkedTo)
					{
						if (!Linked) continue;
						const UEdGraphNode* Target = Linked->GetOwningNode();
						const FString Id = TEXT("node:") + Target->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
						if (Id == TargetId && Target->Pins.IndexOfByKey(Linked) == TargetIndex) TargetPin = Linked;
					}
				if (!TargetPin)
				{
					const auto Actual = MakeShared<FJsonObject>();
					Actual->SetStringField(TEXT("source_node"), Request.TargetIdentity.GetSelector(TEXT("source_node")));
					Actual->SetNumberField(TEXT("source_pin_index"), SourceIndex);
					Actual->SetStringField(TEXT("target_node"), TargetId);
					Actual->SetNumberField(TEXT("target_pin_index"), TargetIndex);
					if (Node) Actual->SetNumberField(TEXT("source_pin_count"), Node->Pins.Num());
					return Failure(TEXT("behavior_tree_edge_target_missing"),
						!Node ? TEXT("The edge source object is not an EdGraphNode.") : !Pin ? TEXT("The edge source pin index does not identify an available pin.")
							: Pin->Direction != EGPD_Output ? TEXT("The edge source pin is not an output pin.") : TEXT("The requested target node and pin are not linked from the source pin."),
						TEXT("/edge"), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("existing output-pin connection to the requested target")));
				}
				State->SetObjectField(TEXT("from"), Endpoint(Request.TargetIdentity.GetSelector(TEXT("source_node")), *Pin));
				State->SetObjectField(TEXT("to"), Endpoint(TargetId, *TargetPin));
			}
			else return Failure(TEXT("behavior_tree_state_semantic_unsupported"), TEXT("The requested state semantic is not supported by the Behavior Tree reader."),
				TEXT("/semantic_id"), MakeShared<FJsonValueString>(Request.SemanticId), MakeShared<FJsonValueString>(TEXT("Behavior Tree graph, node, or edge semantic")));
			OutValue.Value = MakeShared<FJsonValueObject>(State);
			OutValue.bAvailable = true;
			return EOutcome::Resolved;
		}
	}

	bool RegisterGraphState(UE::UAI::Contract::FDiagnostic& OutError)
	{
		struct FContract { const TCHAR* Semantic; const TCHAR* Kind; TArray<FString> Keys; TArray<FString> Extra; };
		const TArray<FContract> Contracts = {
			{GraphSemanticId, GraphKind, {TEXT("asset_path"), TEXT("graph_name")}, {TEXT("owner_item_id")}},
			{NodeSemanticId, NodeKind, {TEXT("asset_path"), TEXT("graph_name"), TEXT("node_id")}, {TEXT("item_id"), TEXT("owner_item_id")}},
			{NodePropertySemanticId, PropertyKind, {TEXT("asset_path"), TEXT("graph_name"), TEXT("node_id"), TEXT("property_path")}, {}},
			{EdgeSemanticId, EdgeKind, {TEXT("asset_path"), TEXT("graph_name"), TEXT("edge_key")},
				{TEXT("source_node"), TEXT("source_pin"), TEXT("target_node"), TEXT("target_pin")}}
		};
		for (const auto& Contract : Contracts)
		{
			UeAgentAuthoringIdentity::FModelDescriptor Identity;
			Identity.ModelId = IdentityModel(Contract.Kind);
			Identity.TargetKind = Contract.Kind;
			int32 Priority = 100;
			for (const FString& Key : Contract.Keys) Identity.Selectors.Add({Key, Priority++, false});
			for (const FString& Key : Contract.Extra) Identity.Selectors.Add({Key, Priority++, false});
			Identity.KeyPatterns = {{TEXT("scoped"), 100, Contract.Keys}};
			UeAgentAuthoringIdentity::ERegistrationResult IdentityResult;
			if (!UeAgentAuthoringIdentity::FRegistry::Get().RegisterModel(Identity, IdentityResult, OutError)) return false;
			UeAgentAuthoringSemanticRegistry::FSemanticDescriptor Semantic;
			Semantic.SemanticId = Contract.Semantic;
			Semantic.DomainId = UeAgentReflectionAuthoring::DomainId;
			Semantic.TargetKind = Contract.Kind;
			Semantic.ValueType = Contract.Kind == FString(PropertyKind) ? TEXT("*") : TEXT("object");
			Semantic.OwnerModel = TEXT("behavior_tree_graph_owner");
			Semantic.IdentityModel = Identity.ModelId;
			const bool bProperty = Contract.Kind == FString(PropertyKind);
			Semantic.StateAccessor = bProperty ? TEXT("core.property") : AccessorId;
			Semantic.WriteCapability = bProperty ? UeAgentAuthoringSemanticRegistry::EWriteCapability::PropertyAssignment
				: UeAgentAuthoringSemanticRegistry::EWriteCapability::DomainMutation;
			Semantic.ConstraintSource = TEXT("behavior_tree.graph.schema");
			Semantic.AvailabilitySource = Semantic.StateAccessor;
			Semantic.Comparator = bProperty ? TEXT("uai.compare.canonical_value") : TEXT("uai.compare.graph_int32_position");
			UeAgentAuthoringSemanticRegistry::ERegistrationResult Result;
			if (!UeAgentAuthoringSemanticRegistry::FRegistry::Get().RegisterSemantic(Semantic, Result, OutError)) return false;
		}
		UeAgentAuthoringState::FStateAccessorDescriptor Accessor;
		Accessor.AccessorId = AccessorId;
		Accessor.TargetKinds = {GraphKind, NodeKind, EdgeKind};
		UeAgentAuthoringState::ERegistrationResult Result;
		return UeAgentAuthoringState::FStateAccessorRegistry::Get().RegisterAccessor(Accessor, ReadState, Result, OutError);
	}

	bool AppendGraphExportRequests(UBehaviorTree& Tree,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, UE::UAI::Contract::FDiagnostic& OutError)
	{
		const FString AssetPath = Tree.GetOutermost()->GetName();
		FGraphCapture Capture;
		if (!CaptureGraph(Tree, Capture, OutError)) return false;
		auto AppendGraph = [&](const FGraphContents& Contents, const FString& GraphId, const UObject* GraphObject,
			const FString& OwnerId) -> bool {
			TMap<FString, FString> GraphSelectors = {{TEXT("asset_path"), AssetPath}, {TEXT("graph_name"), GraphId}};
			if (!OwnerId.IsEmpty()) GraphSelectors.Add(TEXT("owner_item_id"), TEXT("main|") + OwnerId);
			if (!AddRequest(GraphSemanticId, GraphKind, GraphSelectors, GraphObject, OutRequests, OutError)) return false;
			TMap<FString, const UEdGraphNode*> Nodes;
			for (const FNodeCapture& Node : Contents.Nodes)
			{
				Nodes.Add(Node.Id, Node.NativeNode);
				TMap<FString, FString> Selectors = {{TEXT("asset_path"), AssetPath}, {TEXT("graph_name"), GraphId},
					{TEXT("node_id"), Node.Id}, {TEXT("item_id"), GraphId + TEXT("|") + Node.Id}};
				if (!Node.ParentId.IsEmpty()) Selectors.Add(TEXT("owner_item_id"), GraphId + TEXT("|") + Node.ParentId);
				if (!AddRequest(NodeSemanticId, NodeKind, Selectors, Node.NativeNode, OutRequests, OutError)) return false;
				if (const auto* BehaviorNode = Cast<UBehaviorTreeGraphNode>(Node.NativeNode); BehaviorNode && BehaviorNode->bInjectedNode)
					continue;
				if (Node.NativeNode->GetGraph() && !Node.NativeNode->GetGraph()->bEditable) continue;
				const UObject* PropertyObject = GetNodePropertyObject(*Node.NativeNode);
				if (!PropertyObject) continue;
				UeAgentAuthoringState::FEditablePropertyReadOptions Options;
				Options.SemanticId = NodePropertySemanticId;
				Options.BuildSnapshotIdentity = [AssetPath, GraphId, NodeId = Node.Id](const FString& Path,
					UeAgentAuthoringIdentity::FCanonicalIdentity& Identity, UE::UAI::Contract::FDiagnostic& Error) {
					return UeAgentAuthoringIdentity::BuildCanonicalIdentity(IdentityModel(PropertyKind), PropertyKind,
						{{TEXT("asset_path"), AssetPath}, {TEXT("graph_name"), GraphId}, {TEXT("node_id"), NodeId}, {TEXT("property_path"), Path}}, Identity, Error);
				};
				if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(PropertyObject, Options, OutRequests, OutError)) return false;
			}
			for (const FConnectionCapture& Edge : Contents.Connections)
				if (!AddRequest(EdgeSemanticId, EdgeKind,
					{{TEXT("asset_path"), AssetPath}, {TEXT("graph_name"), GraphId}, {TEXT("edge_key"), Edge.Id},
						{TEXT("source_node"), Edge.SourceId}, {TEXT("target_node"), Edge.TargetId},
						{TEXT("source_pin"), FString::FromInt(Edge.State->GetIntegerField(TEXT("source_pin")))},
						{TEXT("target_pin"), FString::FromInt(Edge.State->GetIntegerField(TEXT("target_pin")))}},
					Nodes.FindChecked(Edge.SourceId), OutRequests, OutError)) return false;
			return true;
		};
		if (!AppendGraph(Capture, TEXT("main"), &Tree, FString())) return false;
		for (const FOwnedGraphCapture& Graph : Capture.OwnedGraphs)
			if (!AppendGraph(Graph, Graph.Id, Graph.NativeGraph, Graph.OwnerId)) return false;
		return true;
	}
}
