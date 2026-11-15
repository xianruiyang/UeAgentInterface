#include "Domains/Eqs/Authoring/UeAgentEqsAuthoringState.h"
#include "Core/Contract/UeAgentDiagnostic.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Eqs/Authoring/UeAgentEqsNativeGraph.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"

namespace UeAgentEqsAuthoring
{
	namespace
	{
		constexpr const TCHAR* AccessorId = TEXT("eqs.graph.state");
		constexpr const TCHAR* GraphKind = TEXT("eqs.graph");
		constexpr const TCHAR* NodeKind = TEXT("eqs.node");
		constexpr const TCHAR* PropertyKind = TEXT("eqs.node_property");
		constexpr const TCHAR* EdgeKind = TEXT("eqs.edge");

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

		UE::UAI::Contract::FDiagnostic StateFact(const UeAgentAuthoringState::FReadRequest& Request,
			const TCHAR* Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("authoring_state"));
			Error.TargetPath = !Request.RootOwnerPath.IsEmpty() ? Request.RootOwnerPath
				: Request.RootObject ? Request.RootObject->GetPathName() : FString();
			Error.PropertyPath = Path;
			Error.SemanticPointer = Path;
			Error.SourceKind = TEXT("native_result");
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			Error.InternalContext = Request.TargetIdentity.CanonicalKey;
			if (const auto* Node = Cast<UEdGraphNode>(Request.RootObject))
				Error.CandidateName = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
			return Error;
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
			TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
			if (Request.SemanticId == GraphSemanticId)
			{
				const auto* Query = Cast<UEnvQuery>(Request.RootObject);
				if (!Query)
				{
					OutError = StateFact(Request, TEXT("eqs_graph_target_missing"),
						TEXT("The EQS graph state request does not reference a UEnvQuery asset."), TEXT("request.root_object.class"),
						MakeShared<FJsonValueString>(Request.RootObject ? Request.RootObject->GetClass()->GetPathName() : TEXT("missing")),
						MakeShared<FJsonValueString>(UEnvQuery::StaticClass()->GetPathName()));
					return EOutcome::Failed;
				}
				const auto Graph = MakeShared<FJsonObject>();
				Graph->SetStringField(TEXT("name"), Query->GetName());
				Graph->SetStringField(TEXT("graph_kind"), TEXT("eqs"));
				State->SetObjectField(TEXT("graph"), Graph);
			}
			else if (Request.SemanticId == NodeSemanticId)
			{
				const auto* Node = Cast<UEdGraphNode>(Request.RootObject);
				if (!Node)
				{
					OutError = StateFact(Request, TEXT("eqs_node_target_missing"),
						TEXT("The EQS node state request does not reference an editor graph node."), TEXT("request.root_object.class"),
						MakeShared<FJsonValueString>(Request.RootObject ? Request.RootObject->GetClass()->GetPathName() : TEXT("missing")),
						MakeShared<FJsonValueString>(UEdGraphNode::StaticClass()->GetPathName()));
					return EOutcome::Failed;
				}
				if (!CaptureNodeState(*Node, State, OutError)) return EOutcome::Failed;
				State->RemoveField(TEXT("node_class"));
			}
			else if (Request.SemanticId == EdgeSemanticId)
			{
				const auto* Node = Cast<UEdGraphNode>(Request.RootObject);
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
					auto Actual = MakeShared<FJsonObject>();
					Actual->SetBoolField(TEXT("source_node_present"), Node != nullptr);
					Actual->SetNumberField(TEXT("source_pin_index"), SourceIndex);
					Actual->SetNumberField(TEXT("source_pin_count"), Node ? Node->Pins.Num() : 0);
					Actual->SetBoolField(TEXT("source_pin_present"), Pin != nullptr);
					Actual->SetNumberField(TEXT("target_pin_index"), TargetIndex);
					if (Pin)
					{
						Actual->SetStringField(TEXT("source_pin"), Pin->PinName.ToString());
						Actual->SetStringField(TEXT("source_direction"), Pin->Direction == EGPD_Input ? TEXT("input")
							: Pin->Direction == EGPD_Output ? TEXT("output") : TEXT("invalid"));
						Actual->SetNumberField(TEXT("link_count"), Pin->LinkedTo.Num());
						if (Pin->Direction != EGPD_Input && Pin->Direction != EGPD_Output)
							Actual->SetNumberField(TEXT("source_direction_value"), static_cast<int32>(Pin->Direction));
					}
					const FString Reason = !Node ? TEXT("The EQS edge state request has no source graph node.")
						: !Node->Pins.IsValidIndex(SourceIndex) ? TEXT("The EQS edge source pin index is outside the node's pin array.")
						: !Pin ? TEXT("The EQS edge source pin is null.")
						: Pin->Direction != EGPD_Output ? TEXT("The EQS edge source pin is not an output pin.")
						: TEXT("The EQS output pin has no link to the requested target node and pin.");
					OutError = StateFact(Request, TEXT("eqs_edge_target_missing"), Reason,
						TEXT("edge.endpoints"), MakeShared<FJsonValueObject>(Actual),
						MakeShared<FJsonValueString>(TEXT("source output pin linked to the requested target node/pin")));
					return EOutcome::Failed;
				}
				State->SetObjectField(TEXT("from"), Endpoint(Request.TargetIdentity.GetSelector(TEXT("source_node")), *Pin));
				State->SetObjectField(TEXT("to"), Endpoint(TargetId, *TargetPin));
			}
			else
			{
				TArray<TSharedPtr<FJsonValue>> Expected;
				Expected.Add(MakeShared<FJsonValueString>(GraphSemanticId));
				Expected.Add(MakeShared<FJsonValueString>(NodeSemanticId));
				Expected.Add(MakeShared<FJsonValueString>(EdgeSemanticId));
				OutError = StateFact(Request, TEXT("eqs_state_semantic_unsupported"),
					TEXT("The EQS state accessor does not support this semantic surface."), TEXT("request.semantic_id"),
					MakeShared<FJsonValueString>(Request.SemanticId), MakeShared<FJsonValueArray>(MoveTemp(Expected)));
				return EOutcome::Failed;
			}
			OutValue.Value = MakeShared<FJsonValueObject>(State);
			OutValue.bAvailable = true;
			return EOutcome::Resolved;
		}
	}

	bool RegisterGraphState(UE::UAI::Contract::FDiagnostic& OutError)
	{
		struct FContract { const TCHAR* Semantic; const TCHAR* Kind; TArray<FString> Keys; TArray<FString> Extra; };
		const TArray<FContract> Contracts = {
			{GraphSemanticId, GraphKind, {TEXT("asset_path"), TEXT("graph_name")}, {}},
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
			const bool bProperty = Contract.Kind == FString(PropertyKind);
			Semantic.ValueType = bProperty ? TEXT("*") : TEXT("object");
			Semantic.OwnerModel = TEXT("eqs_graph_owner");
			Semantic.IdentityModel = Identity.ModelId;
			Semantic.StateAccessor = bProperty ? TEXT("core.property") : AccessorId;
			Semantic.WriteCapability = bProperty ? UeAgentAuthoringSemanticRegistry::EWriteCapability::PropertyAssignment
				: UeAgentAuthoringSemanticRegistry::EWriteCapability::DomainMutation;
			Semantic.ConstraintSource = TEXT("eqs.graph.schema");
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

	bool AppendGraphExportRequests(UEnvQuery& Query,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, UE::UAI::Contract::FDiagnostic& OutError)
	{
		FGraphCapture Capture;
	return CaptureGraph(Query, Capture, OutError) && AppendGraphExportRequests(Query, Capture, OutRequests, OutError);
}

bool AppendGraphExportRequests(UEnvQuery& Query, const FGraphCapture& Capture,
	TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, UE::UAI::Contract::FDiagnostic& OutError)
{
	const FString AssetPath = Query.GetOutermost()->GetName();
		if (!AddRequest(GraphSemanticId, GraphKind, {{TEXT("asset_path"), AssetPath}, {TEXT("graph_name"), TEXT("main")}},
			&Query, OutRequests, OutError)) return false;
		TMap<FString, const UEdGraphNode*> Nodes;
		for (const FNodeCapture& Node : Capture.Nodes)
		{
			Nodes.Add(Node.Id, Node.NativeNode);
			TMap<FString, FString> Selectors = {{TEXT("asset_path"), AssetPath}, {TEXT("graph_name"), TEXT("main")},
				{TEXT("node_id"), Node.Id}, {TEXT("item_id"), TEXT("main|") + Node.Id}};
			if (!Node.ParentId.IsEmpty()) Selectors.Add(TEXT("owner_item_id"), TEXT("main|") + Node.ParentId);
			if (!AddRequest(NodeSemanticId, NodeKind, Selectors, Node.NativeNode, OutRequests, OutError)) return false;
			const UObject* PropertyObject = GetNodePropertyObject(*Node.NativeNode);
			if (!PropertyObject) continue;
			UeAgentAuthoringState::FEditablePropertyReadOptions Options;
			Options.SemanticId = NodePropertySemanticId;
			Options.Limits.bShowDefaultsOnly = true;
			Options.BuildSnapshotIdentity = [AssetPath, NodeId = Node.Id](const FString& Path,
				UeAgentAuthoringIdentity::FCanonicalIdentity& Identity, UE::UAI::Contract::FDiagnostic& Error) {
				return UeAgentAuthoringIdentity::BuildCanonicalIdentity(IdentityModel(PropertyKind), PropertyKind,
					{{TEXT("asset_path"), AssetPath}, {TEXT("graph_name"), TEXT("main")}, {TEXT("node_id"), NodeId}, {TEXT("property_path"), Path}}, Identity, Error);
			};
			if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(PropertyObject, Options, OutRequests, OutError)) return false;
		}
		for (const FConnectionCapture& Edge : Capture.Connections)
			if (!AddRequest(EdgeSemanticId, EdgeKind,
				{{TEXT("asset_path"), AssetPath}, {TEXT("graph_name"), TEXT("main")}, {TEXT("edge_key"), Edge.Id},
					{TEXT("source_node"), Edge.SourceId}, {TEXT("target_node"), Edge.TargetId},
					{TEXT("source_pin"), FString::FromInt(Edge.SourcePin)}, {TEXT("target_pin"), FString::FromInt(Edge.TargetPin)}},
				Nodes.FindChecked(Edge.SourceId), OutRequests, OutError)) return false;
		return true;
	}
}
