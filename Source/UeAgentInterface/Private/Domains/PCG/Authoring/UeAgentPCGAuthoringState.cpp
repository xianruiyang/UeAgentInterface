// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/PCG/Authoring/UeAgentPCGAuthoringState.h"

#include "Domains/PCG/Authoring/UeAgentPCGAuthoringDescriptors.h"
#include "Domains/PCG/Authoring/UeAgentPCGGraphAuthoring.h"
#include "Domains/PCG/UeAgentPCGDomainBackend.h"
#include "Dom/JsonObject.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringNativeDefaults.h"
#include "Utilities/UeAgentDetailsAuthoring.h"

namespace UeAgentPCGAuthoring
{
	namespace
	{
		bool BuildIdentity(
			const FString& ModelId,
			const FString& TargetKind,
			const TMap<FString, FString>& Selectors,
			UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
			FString& OutError)
		{
			return UeAgentAuthoringIdentity::BuildCanonicalIdentity(
				ModelId, TargetKind, Selectors, OutIdentity, OutError);
		}

		bool AddReadRequest(
			const FString& SemanticId,
			const FString& ModelId,
			const FString& TargetKind,
			const TMap<FString, FString>& Selectors,
			const UObject* RootObject,
			const TMap<FString, FString>& Context,
			TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
			FString& OutError)
		{
			UeAgentAuthoringState::FReadRequest Request;
			Request.SemanticId = SemanticId;
			if (!BuildIdentity(ModelId, TargetKind, Selectors, Request.TargetIdentity, OutError)) return false;
			Request.TargetQuery.ModelId = Request.TargetIdentity.ModelId;
			Request.TargetQuery.TargetKind = Request.TargetIdentity.TargetKind;
			Request.TargetQuery.CanonicalKey = Request.TargetIdentity.CanonicalKey;
			Request.RootObject = RootObject;
			Request.RootOwnerPath = Selectors.FindRef(TEXT("asset_path"));
			Request.Context = Context;
			OutRequests.Add(MoveTemp(Request));
			return true;
		}

		TSharedRef<FJsonObject> CloneObject(const TSharedPtr<FJsonObject>& Source)
		{
			return Source.IsValid()
				? UeAgentDetailsAuthoring::CloneObjectTree(Source).ToSharedRef()
				: MakeShared<FJsonObject>();
		}

		TSharedRef<FJsonObject> BuildPinState(const UPCGPin& Pin)
		{
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			State->SetStringField(TEXT("allowed_types"), StaticEnum<EPCGDataType>()->GetNameStringByValue(
				static_cast<int64>(Pin.Properties.AllowedTypes)));
			State->SetStringField(TEXT("usage"), StaticEnum<EPCGPinUsage>()->GetNameStringByValue(
				static_cast<int64>(Pin.Properties.Usage)));
			State->SetStringField(TEXT("status"), StaticEnum<EPCGPinStatus>()->GetNameStringByValue(
				static_cast<int64>(Pin.Properties.PinStatus)));
			State->SetBoolField(TEXT("allow_multiple_data"), Pin.Properties.bAllowMultipleData);
			State->SetBoolField(TEXT("allow_multiple_connections"), Pin.Properties.AllowsMultipleConnections());
			State->SetBoolField(TEXT("advanced"), Pin.Properties.IsAdvancedPin());
			State->SetBoolField(TEXT("required"), Pin.Properties.IsRequiredPin());
			return State;
		}

		UPCGGraph* ResolveGraph(const UeAgentAuthoringState::FReadRequest& Request)
		{
			if (UPCGGraph* Graph = const_cast<UPCGGraph*>(Cast<UPCGGraph>(Request.RootObject))) return Graph;
			FString Path = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
			return UeAgentPCG::LoadAsset<UPCGGraph>(Path);
		}

		UeAgentAuthoringState::EReadOutcome ReadState(
			const UeAgentAuthoringState::FReadRequest& Request,
			UeAgentAuthoringState::FStateValue& OutValue,
			FString& OutError)
		{
			OutValue = UeAgentAuthoringState::FStateValue();
			OutValue.TargetIdentity = Request.TargetIdentity;
			OutValue.Type.Kind = TEXT("object");
			OutValue.Provenance = StateAccessorId;
			OutError.Reset();
			if (Request.Context.FindRef(TEXT("expected_absent")).Equals(TEXT("true"), ESearchCase::IgnoreCase))
			{
				OutValue.bAvailable = false;
				OutValue.AvailabilityReason = TEXT("target_not_found");
				return UeAgentAuthoringState::EReadOutcome::Unavailable;
			}

			if (Request.SemanticId == ExportPinSemanticId)
			{
				const UPCGPin* Pin = Cast<UPCGPin>(Request.RootObject);
				if (!Pin)
				{
					OutError = TEXT("pcg_export_pin_target_invalid");
					return UeAgentAuthoringState::EReadOutcome::Failed;
				}
				OutValue.Value = MakeShared<FJsonValueObject>(BuildPinState(*Pin));
				OutValue.bAvailable = true;
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}
			if (Request.SemanticId == ExportEdgeSemanticId)
			{
				TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
				TSharedRef<FJsonObject> From = MakeShared<FJsonObject>();
				From->SetStringField(TEXT("node"), Request.Context.FindRef(TEXT("from_node")));
				From->SetStringField(TEXT("pin"), Request.Context.FindRef(TEXT("from_pin")));
				TSharedRef<FJsonObject> To = MakeShared<FJsonObject>();
				To->SetStringField(TEXT("node"), Request.Context.FindRef(TEXT("to_node")));
				To->SetStringField(TEXT("pin"), Request.Context.FindRef(TEXT("to_pin")));
				State->SetObjectField(TEXT("from"), From);
				State->SetObjectField(TEXT("to"), To);
				OutValue.Value = MakeShared<FJsonValueObject>(State);
				OutValue.bAvailable = true;
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}

			UPCGGraph* Graph = ResolveGraph(Request);
			if (!Graph)
			{
				OutValue.bAvailable = false;
				OutValue.AvailabilityReason = TEXT("target_not_found");
				return UeAgentAuthoringState::EReadOutcome::Unavailable;
			}
			const FString AssetPath = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
			TSharedPtr<FJsonObject> Document;
			if (!BuildGraphDocument(*Graph, Document, OutError))
			{
				return UeAgentAuthoringState::EReadOutcome::Failed;
			}

			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			if (Request.SemanticId == GraphSemanticId)
			{
				State->SetStringField(TEXT("asset_path"), AssetPath);
				State->SetStringField(TEXT("asset_kind"), TEXT("pcg_graph"));
				State->SetObjectField(TEXT("graph"), CloneObject(Document));
			}
			else if (Request.SemanticId == ExportTargetSemanticId)
			{
				State->SetStringField(TEXT("asset_path"), AssetPath);
				State->SetStringField(TEXT("asset_kind"), TEXT("pcg_graph"));
			}
			else if (Request.SemanticId == ExportNodeSemanticId)
			{
				const TSharedPtr<FJsonObject>* Nodes = nullptr;
				const TSharedPtr<FJsonObject>* Node = nullptr;
				const FString NodeId = Request.TargetIdentity.GetSelector(TEXT("node_id"));
				if (!Document->TryGetObjectField(TEXT("nodes"), Nodes) || !Nodes || !Nodes->IsValid()
					|| !(*Nodes)->TryGetObjectField(NodeId, Node) || !Node || !Node->IsValid())
				{
					OutError = TEXT("pcg_export_node_target_missing:") + NodeId;
					return UeAgentAuthoringState::EReadOutcome::Failed;
				}
				State = CloneObject(*Node);
				State->RemoveField(TEXT("input_pins"));
				State->RemoveField(TEXT("output_pins"));
			}
			else if (Request.SemanticId == ExportParametersSemanticId)
			{
				const TSharedPtr<FJsonObject>* Parameters = nullptr;
				if (Document->TryGetObjectField(TEXT("parameters"), Parameters) && Parameters && Parameters->IsValid())
				{
					State = CloneObject(*Parameters);
				}
			}
			else
			{
				OutError = TEXT("pcg_state_semantic_unsupported:") + Request.SemanticId;
				return UeAgentAuthoringState::EReadOutcome::Failed;
			}
			OutValue.Value = MakeShared<FJsonValueObject>(State);
			OutValue.bAvailable = true;
			return UeAgentAuthoringState::EReadOutcome::Resolved;
		}

		bool AppendNodePropertyRequests(
			UPCGGraph& Graph,
			const FString& AssetPath,
			TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
			FString& OutError)
		{
			const TMap<FString, UPCGNode*> Nodes = UeAgentPCG::BuildExportNodeMapById(&Graph);
			for (const TPair<FString, UPCGNode*>& Pair : Nodes)
			{
				UPCGSettings* Settings = Pair.Value ? Pair.Value->GetSettings() : nullptr;
				if (!Settings) continue;
				UeAgentAuthoringState::FEditablePropertyReadOptions Options;
				Options.SemanticId = ExportNodePropertySemanticId;
				Options.BuildSnapshotIdentity = [AssetPath, NodeId = Pair.Key](
					const FString& PropertyPath,
					UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
					FString& IdentityError)
				{
					return BuildIdentity(
						NodePropertyIdentityModelId,
						TEXT("pcg_node_property"),
						{ { TEXT("asset_path"), AssetPath }, { TEXT("node_id"), NodeId },
							{ TEXT("property_path"), PropertyPath } },
						OutIdentity,
						IdentityError);
				};
				if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(
					Settings, Options, OutRequests, OutError))
				{
					return false;
				}
			}
			return true;
		}

		UeAgentAuthoringNativeDefaults::EProbeOutcome ProbeNativeDefault(
			const UeAgentAuthoringNativeDefaults::FRequest&,
			UeAgentAuthoringNativeDefaults::FScratchScope& Scratch,
			UeAgentAuthoringNativeDefaults::FProbeValue& OutValue,
			FString& OutError)
		{
			OutValue = UeAgentAuthoringNativeDefaults::FProbeValue();
			UPackage* Package = Scratch.CreateScratchPackage(OutError);
			if (!Package) return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;
			UPCGGraph* Seed = CreateFactorySeed(Package, NAME_None, RF_Transactional, OutError);
			if (!Seed) return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;
			TSharedPtr<FJsonObject> Document;
			if (!BuildGraphDocument(*Seed, Document, OutError))
			{
				return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;
			}
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			State->SetStringField(TEXT("asset_kind"), TEXT("pcg_graph"));
			State->SetObjectField(TEXT("graph"), Document);
			OutValue.Value = MakeShared<FJsonValueObject>(State);
			OutValue.Provenance = FactoryDefaultProviderId;
			return UeAgentAuthoringNativeDefaults::EProbeOutcome::Resolved;
		}
	}

	UPCGGraph* CreateFactorySeed(
		UObject* Outer,
		const FName Name,
		const EObjectFlags Flags,
		FString& OutError)
	{
		OutError.Reset();
		if (!Outer)
		{
			OutError = TEXT("pcg_factory_outer_missing");
			return nullptr;
		}
		UPCGGraph* Graph = NewObject<UPCGGraph>(Outer, Name, Flags);
		if (!Graph) OutError = TEXT("pcg_factory_create_failed");
		return Graph;
	}

	bool BuildAssetSnapshotReadRequests(
		UPCGGraph* Graph,
		const FString& CanonicalAssetPath,
		const bool bExpectedAbsent,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FString& OutError)
	{
		OutError.Reset();
		if (CanonicalAssetPath.IsEmpty())
		{
			OutError = TEXT("pcg_snapshot_identity_invalid");
			return false;
		}
		if (!AddReadRequest(GraphSemanticId, GraphIdentityModelId, TEXT("pcg_graph"),
			{ { TEXT("asset_path"), CanonicalAssetPath } }, Graph,
			bExpectedAbsent ? TMap<FString, FString>{ { TEXT("expected_absent"), TEXT("true") } }
				: TMap<FString, FString>(), OutRequests, OutError))
		{
			return false;
		}
		return bExpectedAbsent || !Graph
			? true
			: AppendNodePropertyRequests(*Graph, CanonicalAssetPath, OutRequests, OutError);
	}

	bool BuildAssetExportSnapshotReadRequests(
		UPCGGraph& Graph,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FString& OutError)
	{
		OutRequests.Reset();
		OutError.Reset();
		const FString AssetPath = Graph.GetOutermost()->GetName();
		if (!AddReadRequest(ExportTargetSemanticId, GraphIdentityModelId, TEXT("pcg_graph"),
			{ { TEXT("asset_path"), AssetPath } }, &Graph, {}, OutRequests, OutError)) return false;

		UeAgentAuthoringState::FEditablePropertyReadOptions RootOptions;
		RootOptions.SemanticId = TEXT("core.property");
		if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(
			&Graph, RootOptions, OutRequests, OutError)) return false;

		TSharedPtr<FJsonObject> Document;
		if (!BuildGraphDocument(Graph, Document, OutError)) return false;
		const TMap<FString, UPCGNode*> NodesById = UeAgentPCG::BuildExportNodeMapById(&Graph);
		const TSharedPtr<FJsonObject>* Nodes = nullptr;
		if (!Document->TryGetObjectField(TEXT("nodes"), Nodes) || !Nodes || !Nodes->IsValid())
		{
			OutError = TEXT("pcg_export_nodes_missing");
			return false;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Nodes)->Values)
		{
			UPCGNode* const* NodePtr = NodesById.Find(Pair.Key);
			UPCGNode* Node = NodePtr ? *NodePtr : nullptr;
			if (!Node || !AddReadRequest(ExportNodeSemanticId, NodeIdentityModelId, TEXT("pcg_node"),
				{ { TEXT("asset_path"), AssetPath }, { TEXT("node_id"), Pair.Key } },
				Node, {}, OutRequests, OutError)) return false;
			auto AddPinRequests = [
				&AssetPath, &OutRequests, &OutError, &Pair](
					const TArray<TObjectPtr<UPCGPin>>& Pins,
					const FString& Direction)
			{
				for (const TObjectPtr<UPCGPin>& PinPtr : Pins)
				{
					UPCGPin* Pin = PinPtr.Get();
					if (!Pin || Pin->Properties.Label.IsNone()) continue;
					if (!AddReadRequest(ExportPinSemanticId, PinIdentityModelId, TEXT("pcg_pin"),
						{ { TEXT("asset_path"), AssetPath }, { TEXT("node_id"), Pair.Key },
							{ TEXT("direction"), Direction },
							{ TEXT("pin_label"), Pin->Properties.Label.ToString() } },
						Pin, {}, OutRequests, OutError)) return false;
				}
				return true;
			};
			if (!AddPinRequests(Node->GetInputPins(), TEXT("input"))
				|| !AddPinRequests(Node->GetOutputPins(), TEXT("output"))) return false;
		}
		if (!AppendNodePropertyRequests(Graph, AssetPath, OutRequests, OutError)) return false;

		const TSharedPtr<FJsonObject>* Edges = nullptr;
		if (Document->TryGetObjectField(TEXT("edges"), Edges) && Edges && Edges->IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Edges)->Values)
			{
				const TSharedPtr<FJsonObject> Edge = Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
				const TSharedPtr<FJsonObject>* From = nullptr;
				const TSharedPtr<FJsonObject>* To = nullptr;
				TMap<FString, FString> Context;
				if (!Edge.IsValid() || !Edge->TryGetObjectField(TEXT("from"), From) || !From || !From->IsValid()
					|| !Edge->TryGetObjectField(TEXT("to"), To) || !To || !To->IsValid())
				{
					OutError = TEXT("pcg_export_edge_invalid:") + Pair.Key;
					return false;
				}
				(*From)->TryGetStringField(TEXT("node"), Context.FindOrAdd(TEXT("from_node")));
				(*From)->TryGetStringField(TEXT("pin"), Context.FindOrAdd(TEXT("from_pin")));
				(*To)->TryGetStringField(TEXT("node"), Context.FindOrAdd(TEXT("to_node")));
				(*To)->TryGetStringField(TEXT("pin"), Context.FindOrAdd(TEXT("to_pin")));
				if (!AddReadRequest(ExportEdgeSemanticId, EdgeIdentityModelId, TEXT("pcg_edge"),
					{ { TEXT("asset_path"), AssetPath }, { TEXT("edge_key"), Pair.Key } },
					&Graph, Context, OutRequests, OutError)) return false;
			}
		}
		return AddReadRequest(ExportParametersSemanticId, ParametersIdentityModelId, TEXT("pcg_parameters"),
			{ { TEXT("asset_path"), AssetPath } }, &Graph, {}, OutRequests, OutError);
	}

	bool RegisterStateAccessor(FString& OutError)
	{
		UeAgentAuthoringState::FStateAccessorDescriptor Descriptor;
		Descriptor.AccessorId = StateAccessorId;
		Descriptor.TargetKinds = {
			TEXT("pcg_graph"), TEXT("pcg_node"), TEXT("pcg_pin"),
			TEXT("pcg_edge"), TEXT("pcg_parameters")
		};
		Descriptor.CapabilityTags = { TEXT("asset"), TEXT("graph"), TEXT("node"), TEXT("pin") };
		UeAgentAuthoringState::ERegistrationResult Result;
		return UeAgentAuthoringState::FStateAccessorRegistry::Get().RegisterAccessor(
			Descriptor, ReadState, Result, OutError);
	}

	bool RegisterNativeDefaultProvider(FString& OutError)
	{
		UeAgentAuthoringNativeDefaults::FProviderDescriptor Descriptor;
		Descriptor.ProviderId = FactoryDefaultProviderId;
		Descriptor.SourceKind = UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::Factory;
		Descriptor.TargetKinds = { TEXT("pcg_graph") };
		Descriptor.CapabilityTags = { TEXT("factory"), TEXT("asset"), TEXT("graph") };
		Descriptor.bRequiresScratch = true;
		UeAgentAuthoringNativeDefaults::ERegistrationResult Result;
		return UeAgentAuthoringNativeDefaults::FProviderRegistry::Get().RegisterProvider(
			Descriptor, ProbeNativeDefault, Result, OutError);
	}
}
