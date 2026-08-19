// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/PCG/Authoring/UeAgentPCGGraphAuthoring.h"

#include "Domains/PCG/UeAgentPCGDomainBackend.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringDirectApply.h"
#include "Utilities/UeAgentDetailsAuthoring.h"

namespace UeAgentPCGAuthoring
{
	namespace
	{
		bool IsValidIdentityKey(const FString& Value)
		{
			if (Value.IsEmpty()) return false;
			for (const TCHAR Character : Value)
			{
				const bool bAllowed = FChar::IsAlnum(Character)
					|| Character == TEXT('_')
					|| Character == TEXT('-')
					|| Character == TEXT('.');
				if (!bAllowed) return false;
			}
			return true;
		}

		TSharedRef<FJsonObject> CloneObject(const TSharedPtr<FJsonObject>& Source)
		{
			return Source.IsValid()
				? UeAgentDetailsAuthoring::CloneObjectTree(Source).ToSharedRef()
				: MakeShared<FJsonObject>();
		}

		TSharedRef<FJsonObject> BuildPinMap(const TArray<TObjectPtr<UPCGPin>>& Pins)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			for (const TObjectPtr<UPCGPin>& PinPtr : Pins)
			{
				const UPCGPin* Pin = PinPtr.Get();
				if (!Pin || Pin->Properties.Label.IsNone()) continue;
				TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
				Value->SetStringField(
					TEXT("allowed_types"),
					StaticEnum<EPCGDataType>()->GetNameStringByValue(
						static_cast<int64>(Pin->Properties.AllowedTypes)));
				Value->SetStringField(
					TEXT("usage"),
					StaticEnum<EPCGPinUsage>()->GetNameStringByValue(
						static_cast<int64>(Pin->Properties.Usage)));
				Value->SetStringField(
					TEXT("status"),
					StaticEnum<EPCGPinStatus>()->GetNameStringByValue(
						static_cast<int64>(Pin->Properties.PinStatus)));
				Value->SetBoolField(TEXT("allow_multiple_data"), Pin->Properties.bAllowMultipleData);
				Value->SetBoolField(
					TEXT("allow_multiple_connections"),
					Pin->Properties.AllowsMultipleConnections());
				Value->SetBoolField(TEXT("advanced"), Pin->Properties.IsAdvancedPin());
				Value->SetBoolField(TEXT("required"), Pin->Properties.IsRequiredPin());
				Result->SetObjectField(Pin->Properties.Label.ToString(), Value);
			}
			return Result;
		}

		TSharedRef<FJsonObject> BuildNodeState(
			UPCGGraph& Graph,
			UPCGNode& Node,
			const FString& NodeId)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			if (&Node == Graph.GetInputNode())
			{
				Result->SetStringField(TEXT("role"), TEXT("input"));
			}
			else if (&Node == Graph.GetOutputNode())
			{
				Result->SetStringField(TEXT("role"), TEXT("output"));
			}
			else if (const UPCGSettings* Settings = Node.GetSettings())
			{
				Result->SetStringField(TEXT("settings_class"), Settings->GetClass()->GetPathName());
			}

			const FString AuthoredTitle = Node.GetAuthoredTitleName().ToString();
			if (!AuthoredTitle.IsEmpty() && AuthoredTitle != TEXT("None") && AuthoredTitle != NodeId)
			{
				Result->SetStringField(TEXT("authored_title"), AuthoredTitle);
			}
#if WITH_EDITOR
			Result->SetArrayField(TEXT("position"), {
				MakeShared<FJsonValueNumber>(Node.PositionX),
				MakeShared<FJsonValueNumber>(Node.PositionY),
			});
#endif
			Result->SetObjectField(TEXT("input_pins"), BuildPinMap(Node.GetInputPins()));
			Result->SetObjectField(TEXT("output_pins"), BuildPinMap(Node.GetOutputPins()));
			return Result;
		}

		TSharedRef<FJsonObject> PinMapToArrayContainer(
			const TSharedPtr<FJsonObject>& Node,
			const TCHAR* Field,
			FString& OutError)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			const TSharedPtr<FJsonObject>* Pins = nullptr;
			if (!Node->TryGetObjectField(Field, Pins)) return Result;
			if (!Pins || !Pins->IsValid())
			{
				OutError = FString::Printf(TEXT("pcg_%s_invalid"), Field);
				return Result;
			}
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Pins)->Values)
			{
				const TSharedPtr<FJsonObject> Pin = Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
				if (!Pin.IsValid() || Pair.Key.IsEmpty())
				{
					OutError = FString::Printf(TEXT("pcg_%s_entry_invalid"), Field);
					return Result;
				}
				TSharedRef<FJsonObject> Copy = CloneObject(Pin);
				Copy->SetStringField(TEXT("label"), Pair.Key);
				Values.Add(MakeShared<FJsonValueObject>(Copy));
			}
			Result->SetArrayField(Field, Values);
			return Result;
		}

		bool ConvertNode(
			const FString& NodeId,
			const TSharedPtr<FJsonObject>& Source,
			TSharedPtr<FJsonObject>& OutNode,
			FString& OutError)
		{
			if (!IsValidIdentityKey(NodeId) || !Source.IsValid())
			{
				OutError = TEXT("pcg_node_identity_invalid:") + NodeId;
				return false;
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Source->Values)
			{
				if (Field.Key != TEXT("role")
					&& Field.Key != TEXT("settings_class")
					&& Field.Key != TEXT("authored_title")
					&& Field.Key != TEXT("position")
					&& Field.Key != TEXT("properties")
					&& Field.Key != TEXT("input_pins")
					&& Field.Key != TEXT("output_pins"))
				{
					OutError = TEXT("pcg_node_unknown_field:") + NodeId + TEXT(":") + Field.Key;
					return false;
				}
			}

			TSharedRef<FJsonObject> Node = CloneObject(Source);
			Node->SetStringField(TEXT("id"), NodeId);
			FString Role;
			Node->TryGetStringField(TEXT("role"), Role);
			if (Role != TEXT("input") && Role != TEXT("output"))
			{
				FString SettingsClass;
				if (!Node->TryGetStringField(TEXT("settings_class"), SettingsClass)
					|| !UeAgentPCG::ResolvePCGSettingsClass(SettingsClass))
				{
					OutError = TEXT("pcg_node_settings_class_invalid:") + NodeId;
					return false;
				}
				FString AuthoredTitle;
				if (Node->TryGetStringField(TEXT("authored_title"), AuthoredTitle)
					&& AuthoredTitle != NodeId)
				{
					OutError = TEXT("pcg_node_identity_title_conflict:") + NodeId;
					return false;
				}
				Node->SetStringField(TEXT("authored_title"), NodeId);
			}

			const TArray<TSharedPtr<FJsonValue>>* Position = nullptr;
			if (Node->TryGetArrayField(TEXT("position"), Position))
			{
				if (!Position || Position->Num() != 2
					|| !(*Position)[0].IsValid() || !(*Position)[1].IsValid()
					|| (*Position)[0]->Type != EJson::Number || (*Position)[1]->Type != EJson::Number)
				{
					OutError = TEXT("pcg_node_position_invalid:") + NodeId;
					return false;
				}
				Node->SetNumberField(TEXT("pos_x"), (*Position)[0]->AsNumber());
				Node->SetNumberField(TEXT("pos_y"), (*Position)[1]->AsNumber());
				Node->RemoveField(TEXT("position"));
			}

			for (const TCHAR* Field : { TEXT("input_pins"), TEXT("output_pins") })
			{
				TSharedRef<FJsonObject> Container = PinMapToArrayContainer(Node, Field, OutError);
				if (!OutError.IsEmpty()) return false;
				const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
				if (Container->TryGetArrayField(Field, Pins) && Pins)
				{
					Node->SetArrayField(Field, *Pins);
				}
			}

			Node->RemoveField(TEXT("properties"));
			OutNode = Node;
			return true;
		}

		bool ApplyNodeProperties(
			UPCGGraph& Graph,
			const TSharedPtr<const FJsonObject>& CanonicalDocument,
			int32& OutApplied,
			FString& OutError)
		{
			OutApplied = 0;
			const TSharedPtr<FJsonObject>* Nodes = nullptr;
			if (!CanonicalDocument->TryGetObjectField(TEXT("nodes"), Nodes)) return true;
			if (!Nodes || !Nodes->IsValid())
			{
				OutError = TEXT("pcg_nodes_invalid");
				return false;
			}

			const TMap<FString, UPCGNode*> NodeById = UeAgentPCG::BuildExportNodeMapById(&Graph);
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Nodes)->Values)
			{
				const TSharedPtr<FJsonObject> NodeDocument = Pair.Value.IsValid()
					? Pair.Value->AsObject()
					: nullptr;
				const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
				if (!NodeDocument.IsValid()
					|| !NodeDocument->TryGetArrayField(TEXT("properties"), Properties))
				{
					continue;
				}
				UPCGNode* const* NodePtr = NodeById.Find(Pair.Key);
				UPCGSettings* Settings = NodePtr && *NodePtr ? (*NodePtr)->GetSettings() : nullptr;
				if (!Settings || !Properties)
				{
					OutError = TEXT("pcg_node_properties_target_missing:") + Pair.Key;
					return false;
				}
				UeAgentAuthoringObjectApply::FOptions Options;
				Options.bAllowDomainAuthorizedOwners = true;
				UeAgentAuthoringDirectApply::FResult ApplyResult;
				if (!UeAgentAuthoringDirectApply::ApplyObjectPatch(
					Settings, *Properties, Options, ApplyResult))
				{
					OutError = ApplyResult.ErrorCode + TEXT(":") + ApplyResult.ErrorDetail;
					return false;
				}
				OutApplied += ApplyResult.PropertiesSeen;
				(*NodePtr)->UpdateAfterSettingsChangeDuringCreation();
			}
			Graph.Recompile();
			Graph.MarkPackageDirty();
			return true;
		}
	}

	FString MakeEdgeKey(
		const FString& FromNode,
		const FString& FromPin,
		const FString& ToNode,
		const FString& ToPin)
	{
		return FromNode + TEXT(":") + FromPin + TEXT("->") + ToNode + TEXT(":") + ToPin;
	}

	bool BuildGraphDocument(
		UPCGGraph& Graph,
		TSharedPtr<FJsonObject>& OutDocument,
		FString& OutError)
	{
		OutDocument = MakeShared<FJsonObject>();
		OutError.Reset();
		TSharedRef<FJsonObject> Nodes = MakeShared<FJsonObject>();
		TSharedRef<FJsonObject> Edges = MakeShared<FJsonObject>();

		const TMap<FString, UPCGNode*> NodeById = UeAgentPCG::BuildExportNodeMapById(&Graph);
		TMap<UPCGNode*, FString> IdByNode;
		for (const TPair<FString, UPCGNode*>& Pair : NodeById)
		{
			if (!Pair.Value || !IsValidIdentityKey(Pair.Key))
			{
				OutError = TEXT("pcg_export_node_identity_invalid:") + Pair.Key;
				return false;
			}
			IdByNode.Add(Pair.Value, Pair.Key);
			Nodes->SetObjectField(Pair.Key, BuildNodeState(Graph, *Pair.Value, Pair.Key));
		}

		for (const TPair<FString, UPCGNode*>& Pair : NodeById)
		{
			UPCGNode* FromNode = Pair.Value;
			if (!FromNode) continue;
			for (const TObjectPtr<UPCGPin>& PinPtr : FromNode->GetOutputPins())
			{
				UPCGPin* FromPin = PinPtr.Get();
				if (!FromPin) continue;
				for (const TObjectPtr<UPCGEdge>& EdgePtr : FromPin->Edges)
				{
					UPCGEdge* Edge = EdgePtr.Get();
					if (!Edge || Edge->InputPin != FromPin || !Edge->OutputPin || !Edge->OutputPin->Node)
					{
						continue;
					}
					const FString* ToId = IdByNode.Find(Edge->OutputPin->Node);
					if (!ToId) continue;
					const FString FromLabel = FromPin->Properties.Label.ToString();
					const FString ToLabel = Edge->OutputPin->Properties.Label.ToString();
					TSharedRef<FJsonObject> From = MakeShared<FJsonObject>();
					From->SetStringField(TEXT("node"), Pair.Key);
					From->SetStringField(TEXT("pin"), FromLabel);
					TSharedRef<FJsonObject> To = MakeShared<FJsonObject>();
					To->SetStringField(TEXT("node"), *ToId);
					To->SetStringField(TEXT("pin"), ToLabel);
					TSharedRef<FJsonObject> EdgeState = MakeShared<FJsonObject>();
					EdgeState->SetObjectField(TEXT("from"), From);
					EdgeState->SetObjectField(TEXT("to"), To);
					Edges->SetObjectField(
						MakeEdgeKey(Pair.Key, FromLabel, *ToId, ToLabel),
						EdgeState);
				}
			}
		}

		OutDocument->SetObjectField(TEXT("nodes"), Nodes);
		OutDocument->SetObjectField(TEXT("edges"), Edges);
		OutDocument->SetObjectField(
			TEXT("parameters"),
			UeAgentPCG::ExportParameterBagToJson(Graph.GetUserParametersStruct()));
		return true;
	}

	bool BuildBackendGraphDocument(
		const TSharedPtr<const FJsonObject>& CanonicalDocument,
		TSharedPtr<FJsonObject>& OutBackendDocument,
		FString& OutError)
	{
		OutBackendDocument = MakeShared<FJsonObject>();
		OutError.Reset();
		if (!CanonicalDocument.IsValid())
		{
			OutError = TEXT("pcg_graph_document_invalid");
			return false;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : CanonicalDocument->Values)
		{
			if (Field.Key != TEXT("nodes") && Field.Key != TEXT("edges") && Field.Key != TEXT("parameters"))
			{
				OutError = TEXT("pcg_graph_unknown_field:") + Field.Key;
				return false;
			}
		}

		const TSharedPtr<FJsonObject>* NodeMap = nullptr;
		if (!CanonicalDocument->TryGetObjectField(TEXT("nodes"), NodeMap) || !NodeMap || !NodeMap->IsValid())
		{
			OutError = TEXT("pcg_nodes_required");
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Nodes;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*NodeMap)->Values)
		{
			TSharedPtr<FJsonObject> Converted;
			if (!ConvertNode(Pair.Key, Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr, Converted, OutError))
			{
				return false;
			}
			Nodes.Add(MakeShared<FJsonValueObject>(Converted));
		}

		const TSharedPtr<FJsonObject>* EdgeMap = nullptr;
		if (!CanonicalDocument->TryGetObjectField(TEXT("edges"), EdgeMap) || !EdgeMap || !EdgeMap->IsValid())
		{
			OutError = TEXT("pcg_edges_required");
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Edges;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*EdgeMap)->Values)
		{
			const TSharedPtr<FJsonObject> Edge = Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
			const TSharedPtr<FJsonObject>* From = nullptr;
			const TSharedPtr<FJsonObject>* To = nullptr;
			FString FromNode;
			FString FromPin;
			FString ToNode;
			FString ToPin;
			if (!Edge.IsValid()
				|| !Edge->TryGetObjectField(TEXT("from"), From) || !From || !From->IsValid()
				|| !Edge->TryGetObjectField(TEXT("to"), To) || !To || !To->IsValid()
				|| !(*From)->TryGetStringField(TEXT("node"), FromNode)
				|| !(*From)->TryGetStringField(TEXT("pin"), FromPin)
				|| !(*To)->TryGetStringField(TEXT("node"), ToNode)
				|| !(*To)->TryGetStringField(TEXT("pin"), ToPin)
				|| !(*NodeMap)->HasField(FromNode) || !(*NodeMap)->HasField(ToNode)
				|| Pair.Key != MakeEdgeKey(FromNode, FromPin, ToNode, ToPin))
			{
				OutError = TEXT("pcg_edge_invalid:") + Pair.Key;
				return false;
			}
			TSharedRef<FJsonObject> Converted = MakeShared<FJsonObject>();
			Converted->SetStringField(TEXT("from"), FromNode);
			Converted->SetStringField(TEXT("from_pin"), FromPin);
			Converted->SetStringField(TEXT("to"), ToNode);
			Converted->SetStringField(TEXT("to_pin"), ToPin);
			Edges.Add(MakeShared<FJsonValueObject>(Converted));
		}

		OutBackendDocument->SetArrayField(TEXT("nodes"), Nodes);
		OutBackendDocument->SetArrayField(TEXT("edges"), Edges);
		const TSharedPtr<FJsonObject>* Parameters = nullptr;
		if (CanonicalDocument->TryGetObjectField(TEXT("parameters"), Parameters))
		{
			if (!Parameters || !Parameters->IsValid())
			{
				OutError = TEXT("pcg_parameters_invalid");
				return false;
			}
			OutBackendDocument->SetObjectField(TEXT("parameters"), CloneObject(*Parameters));
		}
		return true;
	}

	bool ApplyGraphDocument(
		UPCGGraph& Graph,
		const TSharedPtr<const FJsonObject>& CanonicalDocument,
		const bool bReplaceNodes,
		FGraphApplyResult& OutResult,
		FString& OutError)
	{
		OutResult = FGraphApplyResult();
		OutError.Reset();
		TSharedPtr<FJsonObject> BackendDocument;
		if (!BuildBackendGraphDocument(CanonicalDocument, BackendDocument, OutError)) return false;
		TArray<TSharedPtr<FJsonValue>> Issues;
		if (!UeAgentPCG::ApplyGraphJson(
			&Graph,
			BackendDocument,
			bReplaceNodes,
			Issues,
			OutResult.NodeCount,
			OutResult.EdgeCount))
		{
			OutError = TEXT("pcg_graph_apply_failed");
			if (!Issues.IsEmpty())
			{
				const TSharedPtr<FJsonObject> First = Issues[0].IsValid() ? Issues[0]->AsObject() : nullptr;
				FString Code;
				if (First.IsValid() && First->TryGetStringField(TEXT("code"), Code))
				{
					OutError += TEXT(":") + Code;
				}
			}
			return false;
		}
		return ApplyNodeProperties(
			Graph, CanonicalDocument, OutResult.NodePropertiesApplied, OutError);
	}

	bool PreflightGraphDocument(
		UPCGGraph* ExistingGraph,
		const TSharedPtr<const FJsonObject>& CanonicalDocument,
		const bool bReplaceNodes,
		FGraphApplyResult& OutResult,
		FString& OutError)
	{
		TStrongObjectPtr<UPCGGraph> Preview(
			ExistingGraph
				? DuplicateObject<UPCGGraph>(ExistingGraph, GetTransientPackage())
				: NewObject<UPCGGraph>(GetTransientPackage(), NAME_None, RF_Transient | RF_Transactional));
		if (!Preview.IsValid())
		{
			OutError = TEXT("pcg_preflight_graph_create_failed");
			return false;
		}
		return ApplyGraphDocument(
			*Preview.Get(), CanonicalDocument, bReplaceNodes, OutResult, OutError);
	}
}
