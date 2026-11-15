// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/PCG/Authoring/UeAgentPCGAuthoringState.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"

#include "Domains/PCG/Authoring/UeAgentPCGAuthoringDescriptors.h"
#include "Domains/PCG/Authoring/UeAgentPCGGraphAuthoring.h"
#include "Domains/PCG/UeAgentPCGDomainBackend.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Class.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringNativeDefaults.h"
#include "Utilities/UeAgentDetailsAuthoring.h"

namespace UeAgentPCGAuthoring
{
	static bool AppendGraphBodyReadRequests(UPCGGraph& Graph, const FString& AssetPath,
		const TSharedPtr<FJsonObject>& Document, TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		UE::UAI::Contract::FDiagnostic& OutError);
	namespace
	{
		UE::UAI::Contract::FDiagnostic NativeFailure(const TCHAR* Code, const TCHAR* Reason,
			const FString& Field, TSharedPtr<FJsonValue> Actual, const TCHAR* Expected,
			const FString& Target = FString())
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("state_capture"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Field;
			Error.ActualValue = MoveTemp(Actual);
			Error.ActualValuePresent = Error.ActualValue.IsValid();
			Error.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			Error.TargetPath = Target;
			return Error;
		}

		bool BuildIdentity(
			const FString& ModelId,
			const FString& TargetKind,
			const TMap<FString, FString>& Selectors,
			UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
			UE::UAI::Contract::FDiagnostic& OutError)
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
			UE::UAI::Contract::FDiagnostic& OutError)
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

		TSharedRef<FJsonObject> BuildPinInterface(
			const TSharedPtr<FJsonObject>& Node)
		{
			TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			if (!Node.IsValid()) return Result;
			TArray<TSharedPtr<FJsonValue>> PinValues;
			TMap<FString, int32> Occurrences;
			for (const TCHAR* Field : { TEXT("input_pins"), TEXT("output_pins") })
			{
				const TSharedPtr<FJsonObject>* PinMap = nullptr;
				if (!Node->TryGetObjectField(Field, PinMap) || !PinMap || !PinMap->IsValid()) continue;
				const bool bInput = FCString::Strcmp(Field, TEXT("input_pins")) == 0;
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PinMap)->Values)
				{
					const TSharedPtr<FJsonObject> Pin = Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
					if (!Pin.IsValid()) continue;
					TSharedRef<FJsonObject> Copy = MakeShared<FJsonObject>();
					Copy->SetStringField(TEXT("name"), Pair.Key);
					Copy->SetStringField(TEXT("direction"), bInput ? TEXT("input") : TEXT("output"));
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Value : Pin->Values)
					{
						Copy->SetField(Value.Key, Value.Value);
					}
					const FString OccurrenceKey = Pair.Key + (bInput ? TEXT(":input") : TEXT(":output"));
					Copy->SetNumberField(TEXT("occurrence"), Occurrences.FindOrAdd(OccurrenceKey)++);
					PinValues.Add(MakeShared<FJsonValueObject>(Copy));
				}
			}
			Result->SetArrayField(TEXT("pins"), MoveTemp(PinValues));
			return Result;
		}

		UPCGGraph* ResolveGraph(const UeAgentAuthoringState::FReadRequest& Request)
		{
			if (UPCGGraph* Graph = const_cast<UPCGGraph*>(Cast<UPCGGraph>(Request.RootObject))) return Graph;
			if (const UPCGNode* Node = Cast<UPCGNode>(Request.RootObject)) return Node->GetGraph();
			FString Path = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
			return UeAgentPCG::LoadAsset<UPCGGraph>(Path);
		}

		UeAgentAuthoringState::EReadOutcome ReadState(
			const UeAgentAuthoringState::FReadRequest& Request,
			UeAgentAuthoringState::FStateValue& OutValue,
			UE::UAI::Contract::FDiagnostic& OutError)
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
					OutError = NativeFailure(TEXT("pcg_export_pin_target_invalid"),
						TEXT("The PCG pin export target is missing or is not a PCG pin."), TEXT("target_class"),
						Request.RootObject ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Request.RootObject->GetClass()->GetPathName())) : nullptr,
						TEXT("PCGPin"), Request.TargetIdentity.GetSelector(TEXT("asset_path")));
					OutError.SubjectId = Request.TargetIdentity.GetSelector(TEXT("node_id")) + TEXT(".")
						+ Request.TargetIdentity.GetSelector(TEXT("pin_label"));
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
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			if (Request.SemanticId == GraphSemanticId)
			{
				TSharedPtr<FJsonObject> Document;
				if (!BuildGraphDocument(*Graph, Document, OutError, AssetPath))
					return UeAgentAuthoringState::EReadOutcome::Failed;
				State->SetStringField(TEXT("asset_path"), AssetPath);
				State->SetStringField(TEXT("asset_kind"), TEXT("pcg_graph"));
				TSharedRef<FJsonObject> GraphMetadata = MakeShared<FJsonObject>();
				GraphMetadata->SetStringField(TEXT("name"), AssetPath);
				GraphMetadata->SetStringField(TEXT("graph_kind"), TEXT("pcg_graph"));
				State->SetObjectField(TEXT("graph"), GraphMetadata);
				if (Request.bCaptureAuthoringProjection && Request.Context.Contains(TEXT("observed_graph_body")))
				{
					TArray<UeAgentAuthoringState::FReadRequest> Requests;
					if (!AddReadRequest(GraphSemanticId, GraphIdentityModelId, TEXT("pcg_graph"),
						{{TEXT("asset_path"), AssetPath}}, Graph, {}, Requests, OutError)
						|| !AddReadRequest(ExportTargetSemanticId, GraphIdentityModelId, TEXT("pcg_graph"),
							{{TEXT("asset_path"), AssetPath}}, Graph, {}, Requests, OutError)
						|| !AppendGraphBodyReadRequests(*Graph, AssetPath, Document, Requests, OutError))
						return UeAgentAuthoringState::EReadOutcome::Failed;
					UeAgentAuthoringState::FCaptureResult Capture;
					if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
					{
						OutError = Capture.Diagnostic;
						return UeAgentAuthoringState::EReadOutcome::Failed;
					}
					auto Resources = UE::UAI::HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
					UE::UAI::HjsonWorkspace::FWorkspaceError Error;
					if (!UE::UAI::HjsonWorkspace::TryBuildVisibleAuthoringProjection(TEXT("ue.pcg_graph.asset"),
						TEXT("llm_authoring"), Capture.Snapshot, Resources, OutValue.AuthoringProjection, Error))
					{
						OutError = Error.GetDiagnostic();
						return UeAgentAuthoringState::EReadOutcome::Failed;
					}
				}
			}
			else if (Request.SemanticId == ExportTargetSemanticId)
			{
				State->SetStringField(TEXT("asset_path"), AssetPath);
				State->SetStringField(TEXT("asset_kind"), TEXT("pcg_graph"));
			}
			else if (Request.SemanticId == ExportNodeSemanticId)
			{
				const FString NodeId = Request.TargetIdentity.GetSelector(TEXT("node_id"));
				UPCGNode* Node = UeAgentPCG::BuildExportNodeMapById(Graph).FindRef(NodeId);
				const UPCGNode* BoundNode = Cast<UPCGNode>(Request.RootObject);
				if (!Node || (BoundNode && Node != BoundNode))
				{
					OutError = NativeFailure(TEXT("pcg_export_node_target_missing"),
						TEXT("The native graph does not contain the requested PCG node binding."),
						TEXT("nodes.") + NodeId, nullptr, TEXT("a node bound to this graph identity"), AssetPath);
					OutError.SubjectId = NodeId;
					return UeAgentAuthoringState::EReadOutcome::Failed;
				}
				State = BuildNodeState(*Graph, *Node);
				// Machine-side node identity is retained for the common resolver;
				// the export projection removes this field from model-visible state.
				State->SetStringField(
					TEXT("id"),
					NodeId);
				TSharedRef<FJsonObject> PinInterface = BuildPinInterface(State);
				State->RemoveField(TEXT("input_pins"));
				State->RemoveField(TEXT("output_pins"));
				State->SetObjectField(TEXT("pin_interface"), PinInterface);
			}
			else if (Request.SemanticId == ExportParametersSemanticId)
			{
				OutValue.bEditable = true;
				OutValue.DisplayName = TEXT("Parameters");
				State = CloneObject(UeAgentPCG::ExportParameterBagToJson(Graph->GetUserParametersStruct()));
			}
			else
			{
				OutError = NativeFailure(TEXT("pcg_state_semantic_unsupported"),
					TEXT("This PCG state accessor does not support the requested semantic."),
					TEXT("semantic_id"), MakeShared<FJsonValueString>(Request.SemanticId),
					TEXT("a registered PCG graph, export target, node, edge, pin or parameters semantic"), AssetPath);
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
			UE::UAI::Contract::FDiagnostic& OutError)
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
					UE::UAI::Contract::FDiagnostic& IdentityError)
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
			UE::UAI::Contract::FDiagnostic& OutError)
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

	bool CaptureAssetExportSnapshot(
		UPCGGraph& Graph,
		UeAgentAuthoringState::FStateSnapshot& OutSnapshot,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutSnapshot = UeAgentAuthoringState::FStateSnapshot();
		OutError.Reset();
		UE::UAI::Contract::FDiagnostic RegistrationError;
		if (!RegisterDescriptors(RegistrationError) || !RegisterStateAccessor(RegistrationError))
		{
			OutError = MoveTemp(RegistrationError);
			return false;
		}
		TArray<UeAgentAuthoringState::FReadRequest> Requests;
		if (!BuildAssetExportSnapshotReadRequests(Graph, Requests, OutError)) return false;
		UeAgentAuthoringState::FCaptureResult Capture;
		if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
		{
			OutError = Capture.Diagnostic;
			return false;
		}
		OutSnapshot = MoveTemp(Capture.Snapshot);
		return true;
	}

	UPCGGraph* CreateFactorySeed(
		UObject* Outer,
		const FName Name,
		const EObjectFlags Flags,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		if (!Outer)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_factory_outer_missing"),
				TEXT("Creating a PCG graph requires an owning object or package."), TEXT("state_capture"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("outer");
			OutError.ActualValuePresent = false;
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("an owning object or package"));
			return nullptr;
		}
		UPCGGraph* Graph = NewObject<UPCGGraph>(Outer, Name, Flags);
		if (!Graph)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_factory_create_failed"),
				TEXT("Unreal could not allocate the PCG graph in its owning object."), TEXT("state_capture"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("graph_available");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			OutError.InternalContext = Outer->GetPathName();
		}
		return Graph;
	}

	bool BuildAssetSnapshotReadRequests(
		UPCGGraph* Graph,
		const FString& CanonicalAssetPath,
		const bool bExpectedAbsent,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		if (CanonicalAssetPath.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_snapshot_identity_invalid"),
				TEXT("A PCG snapshot read request requires a non-empty canonical asset path."), TEXT("state_capture"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("asset_path");
			OutError.ActualValue = MakeShared<FJsonValueString>(CanonicalAssetPath);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("a non-empty canonical PCG graph asset path"));
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRequests.Reset();
		OutError.Reset();
		const FString AssetPath = Graph.GetOutermost()->GetName();
		if (!AddReadRequest(GraphSemanticId, GraphIdentityModelId, TEXT("pcg_graph"),
			{ { TEXT("asset_path"), AssetPath } }, &Graph, {}, OutRequests, OutError)
			|| !AddReadRequest(ExportTargetSemanticId, GraphIdentityModelId, TEXT("pcg_graph"),
			{ { TEXT("asset_path"), AssetPath } }, &Graph, {}, OutRequests, OutError)) return false;

		OutRequests.Last().MarkExportTarget();
		UeAgentAuthoringState::FEditablePropertyReadOptions RootOptions;
		RootOptions.SemanticId = TEXT("core.property");
		RootOptions.ExcludedProperties.Add(TEXT("UserParameters"));
		if (!UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(
			&Graph, RootOptions, OutRequests, OutError)) return false;

		TSharedPtr<FJsonObject> Document;
		if (!BuildGraphDocument(Graph, Document, OutError, AssetPath)) return false;
		return AppendGraphBodyReadRequests(Graph, AssetPath, Document, OutRequests, OutError)
			&& AppendNodePropertyRequests(Graph, AssetPath, OutRequests, OutError);
	}

	static bool AppendGraphBodyReadRequests(UPCGGraph& Graph, const FString& AssetPath,
		const TSharedPtr<FJsonObject>& Document, TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		const TMap<FString, UPCGNode*> NodesById = UeAgentPCG::BuildExportNodeMapById(&Graph);
		const TSharedPtr<FJsonObject>* Nodes = nullptr;
		if (!Document->TryGetObjectField(TEXT("nodes"), Nodes) || !Nodes || !Nodes->IsValid())
		{
			OutError = NativeFailure(TEXT("pcg_export_nodes_missing"),
				TEXT("The PCG graph export did not produce a nodes object."), TEXT("nodes"),
				Document->TryGetField(TEXT("nodes")), TEXT("an object containing exported PCG nodes"), AssetPath);
			return false;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Nodes)->Values)
		{
			UPCGNode* const* NodePtr = NodesById.Find(Pair.Key);
			UPCGNode* Node = NodePtr ? *NodePtr : nullptr;
			if (!Node)
			{
				OutError = NativeFailure(TEXT("pcg_export_native_node_missing"),
					TEXT("An exported PCG node has no corresponding node in the native graph."),
					TEXT("node_id"), MakeShared<FJsonValueString>(Pair.Key),
					TEXT("a node identifier present in the native graph"), AssetPath);
				OutError.SubjectId = Pair.Key;
				return false;
			}
			if (!AddReadRequest(ExportNodeSemanticId, NodeIdentityModelId, TEXT("pcg_node"),
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
					const FString Field = !Edge.IsValid() ? FString()
						: (!From || !From->IsValid()) ? TEXT("from") : TEXT("to");
					OutError = NativeFailure(TEXT("pcg_export_edge_invalid"),
						TEXT("An exported PCG edge must contain object-valued from and to endpoints."),
						TEXT("edges.") + Pair.Key + (Field.IsEmpty() ? FString() : TEXT(".") + Field),
						Field.IsEmpty() ? Pair.Value : Edge->TryGetField(Field),
						TEXT("an object"), AssetPath);
					OutError.SubjectId = Pair.Key;
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

	bool RegisterStateAccessor(UE::UAI::Contract::FDiagnostic& OutError)
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

	bool RegisterNativeDefaultProvider(UE::UAI::Contract::FDiagnostic& OutError)
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
