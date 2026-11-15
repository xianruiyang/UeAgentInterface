#include "Domains/Eqs/Authoring/UeAgentEqsGraphDocument.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringGraphLinks.h"

#include "AIGraphNode.h"
#include "AIGraphSchema.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Eqs/Authoring/UeAgentEqsNativeGraph.h"
#include "Domains/Eqs/Authoring/UeAgentEqsAuthoringState.h"
#include "EdGraph/EdGraphPin.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryTest.h"
#include "EnvironmentQueryGraph.h"
#include "EnvironmentQueryGraphNode_Option.h"
#include "EnvironmentQueryGraphNode_Root.h"
#include "Misc/ScopeExit.h"
#include "QueryOwners/AiBehavior/UeAgentAiBehaviorCandidateQuery.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"

namespace UeAgentEqsAuthoring
{
	namespace
	{
		bool Fail(UE::UAI::Contract::FDiagnostic& Error, const TCHAR* Code, const FString& Reason,
			const FString& Path = FString(), TSharedPtr<FJsonValue> Actual = nullptr, TSharedPtr<FJsonValue> Expected = nullptr)
		{
			Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("eqs_document"));
			Error.Source.InternalJsonPath = Path;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			if (Error.ActualValue || Error.ExpectedValue) Error.ActualValuePresent = Error.ActualValue.IsValid();
			if (Error.ExpectedValue) Error.ExpectedValuePresent = true;
			return false;
		}

		bool NativeFail(UE::UAI::Contract::FDiagnostic& Error, const TCHAR* Code, const FString& Reason,
			const FString& Context = FString())
		{
			Fail(Error, Code, Reason);
			Error.SourceKind = TEXT("native_result");
			Error.InternalContext = Context;
			return false;
		}

		FString NodePath(const FString& Identity)
		{
			return TEXT("/nodes/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Identity);
		}

		FString ParentReference(const FJsonObject& Node)
		{
			FString Parent;
			Node.TryGetStringField(TEXT("parent"), Parent);
			return Parent;
		}

		bool ForwardAt(UE::UAI::Contract::FDiagnostic& Error, const FString& Path)
		{
			UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Error, Path);
			return false;
		}

		UClass* ConcreteClass(const FString& Path, UClass* Base)
		{
			auto* Class = FSoftClassPath(Path).TryLoadClass<UObject>();
			return Class && Class->IsChildOf(Base) && !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists) ? Class : nullptr;
		}
		TSharedRef<FJsonObject> Endpoint(const FString& Id, const UEdGraphNode& Node, int32 Index)
		{
			const auto* Pin = Node.Pins[Index];
			int32 Occurrence = 0;
			for (int32 I = 0; I < Index; ++I)
				if (Node.Pins[I] && Node.Pins[I]->Direction == Pin->Direction && Node.Pins[I]->PinName == Pin->PinName) ++Occurrence;
			const auto Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("node"), Id);
			Result->SetStringField(TEXT("pin"), Pin->PinName.ToString());
			Result->SetNumberField(TEXT("pin_occurrence"), Occurrence);
			return Result;
		}
		UEdGraphPin* ResolvePin(UEdGraphNode* Node, const TSharedPtr<FJsonObject>& Endpoint,
			EEdGraphPinDirection Direction, UE::UAI::Contract::FDiagnostic& OutError)
		{
			using namespace UE::UAI::HjsonWorkspace;
			FString Name;
			if (!Node)
			{
				NativeFail(OutError, TEXT("eqs_graph_pin_unresolved"), TEXT("The connection's native graph node is unavailable."));
				return nullptr;
			}
			if (!Endpoint || !Endpoint->TryGetStringField(TEXT("pin"), Name))
			{
				Fail(OutError, TEXT("eqs_graph_pin_unresolved"), TEXT("A connection endpoint requires a pin name string."), TEXT("/pin"),
					Endpoint ? Endpoint->TryGetField(TEXT("pin")) : nullptr, MakeShared<FJsonValueString>(TEXT("pin name string")));
				return nullptr;
			}
			double Occurrence = 0;
			const bool bHasOccurrence = Endpoint->TryGetNumberField(TEXT("pin_occurrence"), Occurrence);
			if (!bHasOccurrence)
			{
				TArray<FGraphAuthoringPinAliasInput> Inputs;
				TMap<FString, int32> Occurrences;
				for (int32 Index = 0; Index < Node->Pins.Num(); ++Index)
				{
					const auto* Pin = Node->Pins[Index];
					if (!Pin) continue;
					const FString Key = FString::FromInt(Pin->Direction) + TEXT(":") + Pin->PinName.ToString();
					Inputs.Add({FString::FromInt(Index), Pin->PinName.ToString(), Occurrences.FindOrAdd(Key)++,
						Pin->Direction == EGPD_Input ? EGraphAuthoringEndpointDirection::Input : EGraphAuthoringEndpointDirection::Output});
				}
				FGraphAuthoringPinAliasProjection Projection;
				FWorkspaceError ProjectionError;
				if (!TryBuildGraphAuthoringPinAliasProjection(Inputs, Projection, ProjectionError))
				{
					OutError = ProjectionError.GetDiagnostic();
					OutError.SourceKind = TEXT("native_result");
					return nullptr;
				}
				const FString* Index = Projection.SourceIdByAlias.Find(Name);
				UEdGraphPin* Pin = Index ? Node->Pins[FCString::Atoi(**Index)] : nullptr;
				if (Pin && Pin->Direction == Direction) return Pin;
			}
			else
			{
				int32 Seen = 0;
				for (UEdGraphPin* Pin : Node->Pins)
					if (Pin && Pin->Direction == Direction && Pin->PinName.ToString() == Name && Seen++ == Occurrence) return Pin;
			}
			auto Requested = MakeShared<FJsonObject>();
			Requested->SetStringField(TEXT("pin"), Name);
			if (bHasOccurrence) Requested->SetNumberField(TEXT("pin_occurrence"), Occurrence);
			Requested->SetStringField(TEXT("direction"), Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
			Fail(OutError, TEXT("eqs_graph_pin_unresolved"), TEXT("No native pin matches the requested name, occurrence and direction."),
				TEXT("/pin"), MakeShared<FJsonValueObject>(Requested), MakeShared<FJsonValueString>(TEXT("available pin in the requested direction")));
			return nullptr;
		}

	}

	bool BuildGraphDocumentFromOptionChanges(UEnvQuery& Query,
		const TMap<FString, UClass*>& NewOptions, const TMap<FString, int32>& Positions,
		TSharedPtr<FJsonObject>& OutDocument, UE::UAI::Contract::FDiagnostic& Error)
	{
		TSharedPtr<FJsonValue> Captured;
		if (!ReadGraphDocument(Query, Captured, Error)) return false;
		OutDocument = Captured->AsObject();
		const auto Nodes = OutDocument->GetObjectField(TEXT("nodes"));
		auto Connections = OutDocument->GetArrayField(TEXT("connections"));
		TStrongObjectPtr<UEnvironmentQueryGraph> TemplateGraph(NewObject<UEnvironmentQueryGraph>(GetTransientPackage()));
		auto* Root = NewObject<UEnvironmentQueryGraphNode_Root>(TemplateGraph.Get());
		Root->AllocateDefaultPins();
		auto MakeNode = [](UClass* Class, const FString& Payload) {
			const auto Node = MakeShared<FJsonObject>();
			Node->SetStringField(TEXT("node_class"), Class->GetPathName());
			Node->SetStringField(TEXT("parent"), FString());
			if (!Payload.IsEmpty()) Node->SetStringField(TEXT("payload_class"), Payload);
			const auto Position = MakeShared<FJsonObject>();
			Position->SetNumberField(TEXT("x"), 0);
			Position->SetNumberField(TEXT("y"), 0);
			Node->SetObjectField(TEXT("pos"), Position);
			return Node;
		};
		if (!NewOptions.IsEmpty() && !Nodes->HasField(TEXT("node:root")) && !AppendDefaultGraphNodes(Nodes, Error)) return false;
		for (const auto& Pair : NewOptions)
		{
			if (Nodes->HasField(Pair.Key))
				return NativeFail(Error, TEXT("eqs_option_change_invalid"), TEXT("The requested option identity already exists in the captured graph."), Pair.Key);
			if (!Pair.Value || !Pair.Value->IsChildOf(UEnvQueryGenerator::StaticClass()))
				return NativeFail(Error, TEXT("eqs_option_change_invalid"), TEXT("The requested option has no compatible EQS generator class."), Pair.Key);
			auto* Option = NewObject<UEnvironmentQueryGraphNode_Option>(TemplateGraph.Get());
			Option->AllocateDefaultPins();
			if (!Root->GetOutputPin() || !Option->GetInputPin()) return NativeFail(Error, TEXT("eqs_option_default_pin_missing"), TEXT("The native root or option node did not allocate its required default connection pin."));
			Nodes->SetObjectField(Pair.Key, MakeNode(Option->GetClass(), Pair.Value->GetPathName()));
			const auto Edge = MakeShared<FJsonObject>();
			Edge->SetObjectField(TEXT("from"), Endpoint(TEXT("node:root"), *Root, Root->Pins.IndexOfByKey(Root->GetOutputPin())));
			Edge->SetObjectField(TEXT("to"), Endpoint(Pair.Key, *Option, Option->Pins.IndexOfByKey(Option->GetInputPin())));
			Connections.Add(MakeShared<FJsonValueObject>(Edge));
		}
		for (const auto& Pair : Positions)
		{
			const TSharedPtr<FJsonObject>* Node = nullptr;
			if (!Nodes->TryGetObjectField(Pair.Key, Node)
				|| (*Node)->GetStringField(TEXT("node_class")) != UEnvironmentQueryGraphNode_Option::StaticClass()->GetPathName())
				return NativeFail(Error, TEXT("eqs_option_position_owner_invalid"), TEXT("The requested position does not identify a captured EQS option node."));
			(*Node)->GetObjectField(TEXT("pos"))->SetNumberField(TEXT("x"), Pair.Value);
		}
		OutDocument->SetArrayField(TEXT("connections"), Connections);
		return ValidateGraphDocument(MakeShared<FJsonValueObject>(OutDocument), Error);
	}

	bool AppendDefaultGraphNodes(const TSharedPtr<FJsonObject>& Nodes, UE::UAI::Contract::FDiagnostic& Error)
	{
		TStrongObjectPtr<UEnvironmentQueryGraph> Defaults(NewObject<UEnvironmentQueryGraph>(GetTransientPackage()));
		Defaults->GetSchema()->CreateDefaultNodesForGraph(*Defaults);
		if (Defaults->Nodes.Num() != 1 || !Defaults->Nodes[0]->IsA<UEnvironmentQueryGraphNode_Root>())
			return NativeFail(Error, TEXT("eqs_default_graph_shape_invalid"), TEXT("The native EQS default graph must contain exactly one root node."));
		TSharedPtr<FJsonObject> Root;
		if (!CaptureNodeState(*Defaults->Nodes[0], Root, Error)) return false;
		Root->RemoveField(TEXT("id"));
		Root->RemoveField(TEXT("pin_interface"));
		Root->SetStringField(TEXT("parent"), FString());
		if (Nodes->HasField(TEXT("node:root"))) return NativeFail(Error, TEXT("eqs_default_root_conflict"), TEXT("A root node already exists when default EQS nodes are appended."));
		Nodes->SetObjectField(TEXT("node:root"), Root);
		return true;
	}

	bool ValidateGraphDocument(const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error)
	{
		Error.Reset();
		const TSharedPtr<FJsonObject>* Nodes = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
		if (!Value || Value->Type != EJson::Object)
			return NativeFail(Error, TEXT("eqs_graph_document_invalid"), TEXT("The prepared EQS graph document is missing or is not an object."));
		if (!Value->AsObject()->TryGetObjectField(TEXT("nodes"), Nodes))
			return NativeFail(Error, TEXT("eqs_graph_document_invalid"), TEXT("The prepared graph has no object-valued node collection."));
		if (!Value->AsObject()->TryGetArrayField(TEXT("connections"), Edges))
			return NativeFail(Error, TEXT("eqs_graph_document_invalid"), TEXT("The prepared graph has no array-valued connection collection."));
		if (!(*Nodes)->Values.IsEmpty() && !(*Nodes)->HasField(TEXT("node:root")))
			return NativeFail(Error, TEXT("eqs_graph_root_missing"), TEXT("A nonempty EQS graph must contain a native root node."));
		for (const auto& Pair : (*Nodes)->Values)
		{
			const FString Path = NodePath(Pair.Key);
			if (!Pair.Value || Pair.Value->Type != EJson::Object)
				return NativeFail(Error, TEXT("eqs_graph_node_invalid"), TEXT("A prepared graph node is missing or is not an object."), Pair.Key);
			const auto Node = Pair.Value->AsObject();
			FString ClassPath, PayloadPath, Parent;
			const TSharedPtr<FJsonObject>* Pos = nullptr;
			int32 Order = 0;
			if (!Node->TryGetStringField(TEXT("node_class"), ClassPath))
				return NativeFail(Error, TEXT("eqs_graph_node_shape_invalid"), TEXT("The prepared EQS node has no native class reference string."), Pair.Key);
			if (Node->HasField(TEXT("parent")) && !Node->TryGetStringField(TEXT("parent"), Parent))
				return Fail(Error, TEXT("eqs_graph_node_shape_invalid"), TEXT("The parent reference must be a string when present."),
					Path + TEXT("/parent"), Node->TryGetField(TEXT("parent")), MakeShared<FJsonValueString>(TEXT("node reference string")));
			if (!Node->TryGetObjectField(TEXT("pos"), Pos))
				return Fail(Error, TEXT("eqs_graph_node_shape_invalid"), TEXT("The node position must be an object with x and y coordinates."), Path + TEXT("/pos"),
					Node->TryGetField(TEXT("pos")), MakeShared<FJsonValueString>(TEXT("position object")));
			for (const TCHAR* Axis : {TEXT("x"), TEXT("y")})
			{
				double Coordinate = 0;
				if (!(*Pos)->TryGetNumberField(Axis, Coordinate) || !FMath::IsFinite(Coordinate) || Coordinate < MIN_int32 || Coordinate > MAX_int32)
					return Fail(Error, TEXT("eqs_graph_node_shape_invalid"), TEXT("A graph coordinate must be a finite number in the signed 32-bit range."),
						Path + TEXT("/pos/") + Axis, (*Pos)->TryGetField(Axis), MakeShared<FJsonValueString>(TEXT("finite number in [-2147483648, 2147483647]")));
			}
			UClass* NodeClass = ConcreteClass(ClassPath, UAIGraphNode::StaticClass());
			if (Pair.Key == TEXT("node:root"))
			{
				if (NodeClass != UEnvironmentQueryGraphNode_Root::StaticClass())
					return NativeFail(Error, TEXT("eqs_graph_root_invalid"), TEXT("The graph root does not have the native EQS root class."), ClassPath);
				if (!Parent.IsEmpty()) return Fail(Error, TEXT("eqs_graph_root_invalid"), TEXT("The EQS root node cannot have an attachment parent."), Path + TEXT("/parent"));
				continue;
			}
			FGuid Guid;
			if (!Pair.Key.StartsWith(TEXT("node:")) || !FGuid::Parse(Pair.Key.Mid(5), Guid) || !Guid.IsValid())
				return NativeFail(Error, TEXT("eqs_graph_node_identity_invalid"), TEXT("The captured EQS node identity is not a valid node GUID reference."), Pair.Key);
			if (!NodeClass) return NativeFail(Error, TEXT("eqs_graph_node_identity_invalid"), TEXT("The prepared node class cannot be loaded as a concrete AI graph-node class."), ClassPath);
			if (!Node->TryGetStringField(TEXT("payload_class"), PayloadPath))
				return NativeFail(Error, TEXT("eqs_graph_node_identity_invalid"), TEXT("The prepared EQS node has no payload class reference string."), Pair.Key);
			if (Parent.IsEmpty())
			{
				if (NodeClass != UEnvironmentQueryGraphNode_Option::StaticClass())
					return NativeFail(Error, TEXT("eqs_graph_option_invalid"), TEXT("A top-level EQS node must have the native option node class."), ClassPath);
				if (!ConcreteClass(PayloadPath, UEnvQueryGenerator::StaticClass()))
					return NativeFail(Error, TEXT("eqs_graph_option_invalid"), TEXT("The option payload cannot be loaded as a concrete EQS generator."), PayloadPath);
			}
			else
			{
				bool Enabled = false;
				const TSharedPtr<FJsonObject>* ParentNode = nullptr;
				FString ParentClass;
				if (!UeAgentAuthoringJsonFields::ReadInt32Field(*Node, TEXT("order"), Order, Error)) return ForwardAt(Error, Path + TEXT("/order"));
				if (ClassPath != TEXT("/Script/EnvironmentQueryEditor.EnvironmentQueryGraphNode_Test"))
					return NativeFail(Error, TEXT("eqs_graph_test_invalid"), TEXT("An attached EQS node must have the native test node class."), ClassPath);
				if (!ConcreteClass(PayloadPath, UEnvQueryTest::StaticClass()))
					return NativeFail(Error, TEXT("eqs_graph_test_invalid"), TEXT("The attached node payload cannot be loaded as a concrete EQS test."), PayloadPath);
				if (!Node->TryGetBoolField(TEXT("enabled"), Enabled))
					return Fail(Error, TEXT("eqs_graph_test_invalid"), TEXT("An EQS test requires a boolean enabled value."), Path + TEXT("/enabled"),
						Node->TryGetField(TEXT("enabled")), MakeShared<FJsonValueString>(TEXT("boolean")));
				if (!Node->HasField(TEXT("order")) || Order < 0)
					return Fail(Error, TEXT("eqs_graph_test_invalid"), TEXT("An EQS test requires a nonnegative integer order."), Path + TEXT("/order"),
						Node->TryGetField(TEXT("order")), MakeShared<FJsonValueString>(TEXT("nonnegative integer")));
				if (!(*Nodes)->TryGetObjectField(Parent, ParentNode) || !(*ParentNode)->TryGetStringField(TEXT("node_class"), ParentClass)
					|| ParentClass != UEnvironmentQueryGraphNode_Option::StaticClass()->GetPathName())
					return Fail(Error, TEXT("eqs_graph_test_invalid"), TEXT("An EQS test must reference an available option node as its parent."), Path + TEXT("/parent"));
			}
		}
		for (int32 EdgeIndex = 0; EdgeIndex < Edges->Num(); ++EdgeIndex)
		{
			const auto& Edge = (*Edges)[EdgeIndex];
			const FString Path = TEXT("/connections/") + FString::FromInt(EdgeIndex);
			if (!Edge || Edge->Type != EJson::Object)
				return NativeFail(Error, TEXT("eqs_graph_connection_invalid"), TEXT("A prepared graph connection is missing or is not an object."), Path);
			for (const TCHAR* Field : {TEXT("from"), TEXT("to")})
			{
				const TSharedPtr<FJsonObject>* Ref = nullptr;
				FString Id, Pin;
				const FString EndpointPath = Path + TEXT("/") + Field;
				if (!Edge->AsObject()->TryGetObjectField(Field, Ref))
					return NativeFail(Error, TEXT("eqs_graph_endpoint_invalid"), TEXT("The prepared connection endpoint is not an object."), EndpointPath);
				if (!(*Ref)->TryGetStringField(TEXT("node"), Id) || !(*Nodes)->HasField(Id))
					return Fail(Error, TEXT("eqs_graph_endpoint_invalid"), TEXT("The connection endpoint does not identify an available graph node."), EndpointPath + TEXT("/node"));
				if (!(*Ref)->TryGetStringField(TEXT("pin"), Pin))
					return Fail(Error, TEXT("eqs_graph_endpoint_invalid"), TEXT("The connection endpoint requires a pin name string."), EndpointPath + TEXT("/pin"),
						(*Ref)->TryGetField(TEXT("pin")), MakeShared<FJsonValueString>(TEXT("pin name string")));
			}
		}
		return true;
	}

	bool ReadGraphDocument(UEnvQuery& Query, TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& Error)
	{
		return ReadGraphDocument(Query, OutValue, Error, nullptr);
	}

	bool ReadGraphDocument(UEnvQuery& Query, TSharedPtr<FJsonValue>& OutValue, UE::UAI::Contract::FDiagnostic& Error,
		TOptional<UeAgentAuthoringState::FStateSnapshot>* OutStructure)
	{
		FGraphCapture Capture;
		if (!CaptureGraph(Query, Capture, Error)) return false;
		TMap<FString, TSharedPtr<FJsonObject>> ObservedNodes;
		TMap<FString, TSharedPtr<FJsonValue>> ObservedEdges;
		TMap<FString, TArray<UeAgentAuthoringState::FStateEntry>> ObservedProperties;
		if (OutStructure)
		{
			TArray<UeAgentAuthoringState::FReadRequest> Requests;
			if (!AppendGraphExportRequests(Query, Capture, Requests, Error)) return false;
			UeAgentAuthoringState::FCaptureResult Public;
			if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Public)) { Error = Public.Diagnostic; return false; }
			for (const auto& Entry : Public.Snapshot.ReadEntries())
			{
				const FString& NodeId = Entry.TargetIdentity.GetSelector(TEXT("node_id"));
				if (Entry.SemanticId == NodePropertySemanticId) ObservedProperties.FindOrAdd(NodeId).Add(Entry);
				else if (Entry.SemanticId == NodeSemanticId || Entry.SemanticId == EdgeSemanticId)
				{
					if (!Entry.Value || Entry.Value->Type != EJson::Object)
						return NativeFail(Error, TEXT("eqs_graph_observation_missing"),
							TEXT("The native graph observation is missing its node or connection object."), Entry.TargetIdentity.CanonicalKey);
					if (Entry.SemanticId == NodeSemanticId) ObservedNodes.Add(NodeId, Entry.Value->AsObject());
					else ObservedEdges.Add(Entry.TargetIdentity.GetSelector(TEXT("edge_key")), Entry.Value);
				}
			}
			OutStructure->Emplace(MoveTemp(Public.Snapshot));
		}
		const auto Document = MakeShared<FJsonObject>();
		const auto Nodes = MakeShared<FJsonObject>();
		TMap<FString, const UEdGraphNode*> Native;
		for (const auto& Item : Capture.Nodes)
		{
			Native.Add(Item.Id, Item.NativeNode);
			TSharedPtr<FJsonObject> State;
			if (OutStructure)
			{
				const auto* Observed = ObservedNodes.Find(Item.Id);
				if (!Observed) return NativeFail(Error, TEXT("eqs_graph_observation_missing"),
					TEXT("The captured native node has no matching public node state."), Item.Id);
				// The public node omits its graph-node class. The verifier still
				// owns it, and drops only the pin interface from its own copy.
				State = MakeShared<FJsonObject>(**Observed);
				State->SetStringField(TEXT("node_class"), Item.NativeNode->GetClass()->GetPathName());
			}
			else if (!CaptureNodeState(*Item.NativeNode, State, Error)) return false;
			State->RemoveField(TEXT("pin_interface"));
			State->SetStringField(TEXT("payload_class"), State->GetStringField(TEXT("class_path")));
			if (const UObject* Payload = GetNodePropertyObject(*Item.NativeNode))
			{
				TSharedPtr<FJsonObject> Properties;
				if (OutStructure)
				{
					if (!UeAgentDetailsAuthoringService::BuildPropertiesDocument(
						ObservedProperties.FindOrAdd(Item.Id), {}, Properties, Error)) return false;
				}
				else
				{
					UeAgentDetailsAuthoringService::FCapturePolicy Policy;
					Policy.bShowDefaultsOnly = true;
					UeAgentAuthoringState::FStateSnapshot Snapshot;
					if (!UeAgentDetailsAuthoringService::CaptureObjectProperties(Payload, Policy, Snapshot, Properties, Error)) return false;
					Snapshot.ReleaseNativeValues();
				}
				State->SetObjectField(TEXT("properties"), Properties);
			}
			Nodes->SetObjectField(Item.Id, State);
		}
		TArray<TSharedPtr<FJsonValue>> Edges;
		for (const auto& Item : Capture.Connections)
		{
			if (OutStructure)
			{
				const auto* Edge = ObservedEdges.Find(Item.Id);
				if (!Edge) return NativeFail(Error, TEXT("eqs_graph_observation_missing"),
					TEXT("The captured native connection has no matching public endpoints."), Item.Id);
				Edges.Add(*Edge);
				continue;
			}
			const auto Edge = MakeShared<FJsonObject>();
			Edge->SetObjectField(TEXT("from"), Endpoint(Item.SourceId, *Native.FindChecked(Item.SourceId), Item.SourcePin));
			Edge->SetObjectField(TEXT("to"), Endpoint(Item.TargetId, *Native.FindChecked(Item.TargetId), Item.TargetPin));
			Edges.Add(MakeShared<FJsonValueObject>(Edge));
		}
		Document->SetObjectField(TEXT("nodes"), Nodes);
		Document->SetArrayField(TEXT("connections"), MoveTemp(Edges));
		OutValue = MakeShared<FJsonValueObject>(Document);
		return true;
	}

	bool ApplyGraphDocument(UEnvQuery& Query, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error)
	{
		return ApplyGraphDocument(Query, Value, Error, nullptr);
	}

	bool ApplyGraphDocument(UEnvQuery& Query, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error,
		TArray<UeAgentDetailsAuthoringService::FScopedPropertyIntent>* OutPropertyIntents)
	{
		if (OutPropertyIntents) OutPropertyIntents->Reset();
		using namespace UE::UAI::QueryOwners::AiBehavior;
		if (!ValidateGraphDocument(Value, Error)) return false;
		FGraphCapture Before;
		if (!CaptureGraph(Query, Before, Error)) return false;
		const auto Document = Value->AsObject();
		const auto Desired = Document->GetObjectField(TEXT("nodes"));
		TMap<FString, TSharedPtr<FJsonObject>> PropertyDocuments, PropertyChanges;
		for (const auto& Pair : Desired->Values)
		{
			TSharedPtr<FJsonObject> Properties, Changes;
			if (!UeAgentDetailsAuthoringService::ReadPropertyPayload(Pair.Value->AsObject(), TEXT("properties"), Properties, Error))
				return ForwardAt(Error, NodePath(Pair.Key) + TEXT("/properties"));
			if (!UeAgentDetailsAuthoringService::ReadPropertyPayload(Pair.Value->AsObject(), TEXT("property_changes"), Changes, Error))
				return ForwardAt(Error, NodePath(Pair.Key) + TEXT("/property_changes"));
			if (Properties) PropertyDocuments.Add(Pair.Key, Properties);
			if (Changes) PropertyChanges.Add(Pair.Key, Changes);
		}
		auto* Graph = Cast<UEnvironmentQueryGraph>(Query.EdGraph);
		if (!Graph && Desired->Values.IsEmpty()) return true;
		Query.Modify();
		if (!Graph)
		{
			Graph = NewObject<UEnvironmentQueryGraph>(&Query, NAME_None, RF_Transactional);
			Query.EdGraph = Graph;
			Graph->Initialize();
		}
		if (Graph->IsLocked()) return NativeFail(Error, TEXT("eqs_graph_updates_locked"), TEXT("The native EQS graph is already locked for updates."));
		Graph->Modify();
		Graph->LockUpdates();
		ON_SCOPE_EXIT { Graph->UnlockUpdates(); };
		TMap<FString, UAIGraphNode*> Nodes;
		TMap<FString, FString> OldParents;
		for (const auto& Node : Before.Nodes)
		{
			Nodes.Add(Node.Id, const_cast<UAIGraphNode*>(Cast<UAIGraphNode>(Node.NativeNode)));
			OldParents.Add(Node.Id, Node.ParentId);
		}
		TArray<FString> Ids;
		Desired->Values.GetKeys(Ids);
		Ids.StableSort([&](const FString& A, const FString& B) {
			return ParentReference(*Desired->GetObjectField(A)).IsEmpty()
				&& !ParentReference(*Desired->GetObjectField(B)).IsEmpty();
		});
		for (const FString& Id : Ids)
		{
			const auto Spec = Desired->GetObjectField(Id);
			const FString ParentId = ParentReference(*Spec);
			UClass* NodeClass = ConcreteClass(Spec->GetStringField(TEXT("node_class")), UAIGraphNode::StaticClass());
			UClass* PayloadClass = Id == TEXT("node:root") ? nullptr : ConcreteClass(Spec->GetStringField(TEXT("payload_class")), UObject::StaticClass());
			UAIGraphNode* Node = Nodes.FindRef(Id);
			if (Node)
			{
				const auto* Payload = GetNodePropertyObject(*Node);
				if (Node->GetClass() != NodeClass || (PayloadClass && (!Payload || Payload->GetClass() != PayloadClass)))
					return NativeFail(Error, TEXT("eqs_graph_existing_type_changed"), TEXT("An existing EQS node or payload no longer has the requested class."));
				continue;
			}
			const auto Pos = Spec->GetObjectField(TEXT("pos"));
			const FVector2f Location(Pos->GetNumberField(TEXT("x")), Pos->GetNumberField(TEXT("y")));
			FString Name;
			if (Spec->TryGetStringField(TEXT("candidate_name"), Name))
			{
				FAiBehaviorCandidateContext Context;
				Context.Parent = Nodes.FindRef(ParentId);
				if (!ParentId.IsEmpty()) Context.AttachmentKind = TEXT("test");
				TMap<FString, FString> Select;
				const TSharedPtr<FJsonObject>* Selection = nullptr;
				if (Spec->TryGetObjectField(TEXT("candidate_select"), Selection))
					for (const auto& Field : (*Selection)->Values)
					{
						FString Text;
						if (!Field.Value->TryGetString(Text)) return Fail(Error, TEXT("eqs_graph_select_invalid"), TEXT("Each candidate selection value must be a string."), NodePath(Id) + TEXT("/candidate_select/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Field.Key), Field.Value, MakeShared<FJsonValueString>(TEXT("string")));
						Select.Add(Field.Key, Text);
					}
				UEdGraphNode* Created = nullptr;
				FAiBehaviorCandidate Candidate;
				FAiBehaviorCandidateQueryError Failure;
				if (!TryPerformAiGraphCandidate(Query, Name, FString(), Location, Created, Candidate, Failure, Select, Context))
				{
					Error = MoveTemp(Failure);
					return ForwardAt(Error, NodePath(Id)
						+ (Error.PropertyPath == TEXT("x") || Error.PropertyPath == TEXT("y") ? TEXT("/pos") : TEXT("/candidate")));
				}
				Node = Cast<UAIGraphNode>(Created);
			}
			else
			{
				// Typed state is also used for rollback; restore through native node lifecycle.
				Node = NewObject<UAIGraphNode>(Graph, NodeClass, NAME_None, RF_Transactional);
				if (PayloadClass) Node->ClassData = FGraphNodeClassData(PayloadClass, FString());
				if (ParentId.IsEmpty())
				{
					Node->CreateNewGuid();
					Node->PostPlacedNewNode();
					Node->AllocateDefaultPins();
					Graph->AddNode(Node, false, false);
				}
				else Nodes.FindChecked(ParentId)->AddSubNode(Node, Graph);
			}
			if (!Node) return NativeFail(Error, TEXT("eqs_graph_create_failed"), TEXT("The native candidate operation did not create a supported EQS graph node."));
			if (Id != TEXT("node:root")) FGuid::Parse(Id.Mid(5), Node->NodeGuid);
			Nodes.Add(Id, Node);
			OldParents.Add(Id, ParentId);
		}
		for (const auto& Item : Before.Nodes)
			if (!Desired->HasField(Item.Id))
			{
				auto* Node = Nodes.FindChecked(Item.Id);
				if (auto* Parent = Nodes.FindRef(Item.ParentId)) Parent->RemoveSubNode(Node);
				Node->DestroyNode();
				Nodes.Remove(Item.Id);
			}
		for (const FString& Id : Ids)
		{
			auto* Node = Nodes.FindChecked(Id);
			const auto Spec = Desired->GetObjectField(Id);
			auto* Parent = Nodes.FindRef(ParentReference(*Spec));
			auto* OldParent = Nodes.FindRef(OldParents.FindRef(Id));
			Node->Modify();
			if (Parent != OldParent)
			{
				if (OldParent) OldParent->RemoveSubNode(Node);
				if (Parent) Parent->InsertSubNodeAt(Node, 0);
			}
			Node->ParentNode = Parent;
			const auto Pos = Spec->GetObjectField(TEXT("pos"));
			Node->NodePosX = Pos->GetIntegerField(TEXT("x"));
			Node->NodePosY = Pos->GetIntegerField(TEXT("y"));
			if (Parent)
			{
				auto* Enabled = FindFProperty<FBoolProperty>(Node->GetClass(), TEXT("bTestEnabled"));
				if (!Enabled) return NativeFail(Error, TEXT("eqs_graph_enable_field_missing"), TEXT("The native EQS test node has no bTestEnabled boolean property."));
				Enabled->SetPropertyValue_InContainer(Node, Spec->GetBoolField(TEXT("enabled")));
			}
			UObject* Payload = const_cast<UObject*>(GetNodePropertyObject(*Node));
			UeAgentAuthoringObjectApply::FOptions Options;
			Options.bAllowDomainAuthorizedOwners = true;
			Options.bShowDefaultsOnly = true;
			UeAgentAuthoringDirectApply::FResult Result;
			if (const auto Properties = PropertyDocuments.FindRef(Id))
			{
				if (!Payload) return NativeFail(Error, TEXT("eqs_graph_properties_target_missing"), TEXT("The node has property edits but no native object that owns those properties."));
				if (!UeAgentDetailsAuthoringService::ApplyObjectProperties(Payload, Properties, Options, Result)) { Error = Result.Diagnostic; return ForwardAt(Error, NodePath(Id) + TEXT("/properties")); }
				if (OutPropertyIntents) UeAgentDetailsAuthoringService::AppendScopedObjectPropertyIntents(
					Result.PropertyIntents, NodePropertySemanticId, TEXT("node_id"), Id, *OutPropertyIntents,
					{{TEXT("graph_name"), TEXT("main")}});
			}
			if (const auto Changes = PropertyChanges.FindRef(Id))
			{
				Options.bCompactValues = true;
				if (!Payload) return NativeFail(Error, TEXT("eqs_graph_properties_target_missing"), TEXT("The node has property edits but no native object that owns those properties."));
				if (!UeAgentDetailsAuthoringService::ApplyObjectProperties(Payload, Changes, Options, Result)) { Error = Result.Diagnostic; return ForwardAt(Error, NodePath(Id) + TEXT("/property_changes")); }
				if (OutPropertyIntents) UeAgentDetailsAuthoringService::AppendScopedObjectPropertyIntents(
					Result.PropertyIntents, NodePropertySemanticId, TEXT("node_id"), Id, *OutPropertyIntents,
					{{TEXT("graph_name"), TEXT("main")}});
			}
		}
		for (const auto& Pair : Nodes)
		{
			Pair.Value->SubNodes.StableSort([&](UAIGraphNode& A, UAIGraphNode& B) {
				const auto Order = [&](UAIGraphNode* Child) {
					const FString* Id = Nodes.FindKey(Child);
					return Id ? Desired->GetObjectField(*Id)->GetIntegerField(TEXT("order")) : MAX_int32;
				};
				return Order(&A) < Order(&B);
			});
			for (UEdGraphPin* Pin : Pair.Value->Pins) if (Pin) Pin->BreakAllPinLinks();
		}
		UeAgentAuthoringGraphLinks::FIntent ConnectionIntent;
		int32 ConnectionIndex = 0;
		for (const auto& Edge : Document->GetArrayField(TEXT("connections")))
		{
			const auto From = Edge->AsObject()->GetObjectField(TEXT("from"));
			const auto To = Edge->AsObject()->GetObjectField(TEXT("to"));
			auto* Source = ResolvePin(Nodes.FindRef(From->GetStringField(TEXT("node"))), From, EGPD_Output, Error);
			if (!Source) return ForwardAt(Error, TEXT("/connections/") + FString::FromInt(ConnectionIndex) + TEXT("/from"));
			auto* Target = ResolvePin(Nodes.FindRef(To->GetStringField(TEXT("node"))), To, EGPD_Input, Error);
			if (!Target) return ForwardAt(Error, TEXT("/connections/") + FString::FromInt(ConnectionIndex) + TEXT("/to"));
			if (!Graph->GetSchema()->TryCreateConnection(Source, Target))
				return Fail(Error, TEXT("eqs_graph_connection_rejected"), TEXT("The native graph schema rejected the connection. ")
					+ Graph->GetSchema()->CanCreateConnection(Source, Target).Message.ToString(), TEXT("/connections/") + FString::FromInt(ConnectionIndex));
			ConnectionIntent.Expect(Source, Target, true, TEXT("/connections/") + FString::FromInt(ConnectionIndex++));
		}
		Graph->UnlockUpdates();
		Graph->UpdateAsset();
		Graph->NotifyGraphChanged();
		Query.PostEditChange();
		ConnectionIntent.Observe();
		Query.MarkPackageDirty();
		return true;
	}
}
