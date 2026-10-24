#include "DomainAdapters/Blueprint/Node/UeAgentBlueprintNodeMechanics.h"

#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Core/ArtifactPublisher/UeAgentVerifiedExport.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "DomainAdapters/BlueprintUmgGraph/UeAgentBlueprintUmgGraphNativeHook.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringIdentity.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeExit.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::DomainAdapters::Blueprint::Node
{
	using namespace UE::UAI::ArtifactPublisher;
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::StateSemantic;

	namespace
	{
		const FString BlueprintType = TEXT("/Script/Engine.Blueprint");
		const FString SchemaId = TEXT("uai.blueprint.node.edit/1");

		FString Normalize(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		bool ForwardDiagnostic(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = FString())
		{
			OutError = MoveTemp(Diagnostic);
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
			return false;
		}

		UE::UAI::Contract::FDiagnostic QueryFact(const FString& Code, const FString& Target,
			const FString& Reason, const FString& Path, TSharedPtr<FJsonValue> Actual,
			TSharedPtr<FJsonValue> Expected, const FString& Name = FString(), const FString& Kind = TEXT("native_result"))
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("candidate_query"));
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			Diagnostic.TargetPath = Target;
			Diagnostic.CandidateName = Name;
			Diagnostic.PropertyPath = Path;
			Diagnostic.SourceKind = Kind;
			Diagnostic.ActualValue = MoveTemp(Actual);
			Diagnostic.ExpectedValue = MoveTemp(Expected);
			return Diagnostic;
		}

		FString Sha256Bytes(const TArray<uint8>& Bytes)
		{
			uint8 Digest[SHA256_DIGEST_LENGTH];
			SHA256(
				reinterpret_cast<const unsigned char*>(Bytes.GetData()),
				static_cast<size_t>(Bytes.Num()),
				Digest);
			FString Result(TEXT("sha256:"));
			for (const uint8 Byte : Digest)
			{
				Result += FString::Printf(TEXT("%02x"), Byte);
			}
			return Result;
		}

		FString Sha256(const FString& Text)
		{
			FTCHARToUTF8 Utf8(*Text);
			TArray<uint8> Bytes;
			Bytes.Append(
				reinterpret_cast<const uint8*>(Utf8.Get()),
				Utf8.Length());
			return Sha256Bytes(Bytes);
		}

		

		FUeAgentResourceTracker MakeResources()
		{
			return FUeAgentResourceTracker();
		}

		FString GuidText(const FGuid& Guid)
		{
			return Guid.IsValid()
				? Guid.ToString(EGuidFormats::DigitsWithHyphensLower)
				: FString();
		}

		using UeAgentBlueprintAuthoring::GraphIdentity;

		void AppendEditableGraphs(
			UBlueprint& Blueprint,
			TArray<UEdGraph*>& OutGraphs)
		{
			OutGraphs.Reset();
			Blueprint.GetAllGraphs(OutGraphs);
			OutGraphs.Remove(nullptr);
		}

		UEdGraph* FindGraph(UBlueprint& Blueprint, const FString& GraphId)
		{
			TArray<UEdGraph*> Graphs;
			AppendEditableGraphs(Blueprint, Graphs);
			for (UEdGraph* Graph : Graphs)
			{
				if (GraphIdentity(*Graph) == GraphId)
				{
					return Graph;
				}
			}
			return nullptr;
		}

		FString LogicalNodeId(const UEdGraphNode& Node)
		{
			const FString Prefix = TEXT("uai_id:");
			return Node.NodeComment.StartsWith(Prefix)
				? Node.NodeComment.Mid(Prefix.Len())
				: GuidText(Node.NodeGuid);
		}

		FString GraphRevision(const UEdGraph& Graph)
		{
			TArray<FString> Facts;
			Facts.Add(GraphIdentity(Graph));
			for (const UEdGraphNode* Node : Graph.Nodes)
			{
				if (!Node)
				{
					continue;
				}
				Facts.Add(FString::Printf(
					TEXT("node|%s|%s|%s|%d|%d"),
					*GuidText(Node->NodeGuid),
					*Node->GetClass()->GetPathName(),
					*Node->NodeComment,
					Node->NodePosX,
					Node->NodePosY));
				for (const UEdGraphPin* Pin : Node->Pins)
				{
					if (!Pin)
					{
						continue;
					}
					for (const UEdGraphPin* Linked : Pin->LinkedTo)
					{
						if (Linked && Linked->GetOwningNode())
						{
							Facts.Add(FString::Printf(
								TEXT("link|%s|%s|%s|%s"),
								*GuidText(Node->NodeGuid),
								*Pin->PinName.ToString(),
								*GuidText(Linked->GetOwningNode()->NodeGuid),
								*Linked->PinName.ToString()));
						}
					}
				}
			}
			Facts.Sort();
			return Sha256(FString::Join(Facts, TEXT("\n")));
		}

		UBlueprint* LoadBlueprint(
			const FString& Target,
			FUeAgentBlueprintNodeMechanicsError& OutError)
		{
			UBlueprint* Blueprint = FindObject<UBlueprint>(nullptr, *Target);
			if (!Blueprint)
			{
				Blueprint = LoadObject<UBlueprint>(nullptr, *Target);
			}
			if (!Blueprint)
			{
				ForwardDiagnostic(OutError, QueryFact(TEXT("blueprint_node_target_unresolved"), Target, TEXT("The Blueprint target could not be loaded."),
					TEXT("/target/available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), Target);
			}
			return Blueprint;
		}

		TSharedPtr<FJsonObject> BuildGraphObject(const UEdGraph& Graph)
		{
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("id"), GraphIdentity(Graph));
			Result->SetStringField(TEXT("name"), Graph.GetName());
			Result->SetStringField(
				TEXT("kind"),
				Graph.GetTypedOuter<UBlueprint>()
					&& Graph.GetTypedOuter<UBlueprint>()->UbergraphPages.Contains(
						const_cast<UEdGraph*>(&Graph))
					? TEXT("ubergraph")
					: TEXT("function_or_macro"));
			return Result;
		}

		TSharedPtr<FJsonObject> BuildReadback(
			UBlueprint& Blueprint,
			UEdGraph& Graph,
			const bool bReloaded)
		{
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("target"), Blueprint.GetPathName());
			Result->SetStringField(TEXT("root_type"), BlueprintType);
			Result->SetObjectField(TEXT("graph"), BuildGraphObject(Graph));
			Result->SetStringField(TEXT("graph_revision"), GraphRevision(Graph));
			Result->SetBoolField(TEXT("complete"), true);
			Result->SetBoolField(TEXT("has_more"), false);
			Result->SetBoolField(TEXT("reloaded"), bReloaded);

			TArray<TSharedPtr<FJsonValue>> Nodes;
			for (UEdGraphNode* Node : Graph.Nodes)
			{
				if (!Node)
				{
					continue;
				}
				const TSharedPtr<FJsonObject> NodeObject = MakeShared<FJsonObject>();
				NodeObject->SetStringField(TEXT("id"), LogicalNodeId(*Node));
				NodeObject->SetStringField(TEXT("node_guid"), GuidText(Node->NodeGuid));
				const TSharedPtr<FJsonObject> Candidate = MakeShared<FJsonObject>();
				Candidate->SetStringField(
					TEXT("name"), Node->GetClass()->GetPathName());
				NodeObject->SetObjectField(TEXT("candidate"), Candidate);
				const TSharedPtr<FJsonObject> Position = MakeShared<FJsonObject>();
				Position->SetNumberField(TEXT("x"), Node->NodePosX);
				Position->SetNumberField(TEXT("y"), Node->NodePosY);
				NodeObject->SetObjectField(TEXT("position"), Position);
				TArray<TSharedPtr<FJsonValue>> Pins;
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (!Pin)
					{
						continue;
					}
					const TSharedPtr<FJsonObject> PinObject = MakeShared<FJsonObject>();
					PinObject->SetStringField(TEXT("name"), Pin->PinName.ToString());
					PinObject->SetStringField(
						TEXT("direction"),
						Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
					PinObject->SetStringField(
						TEXT("category"), Pin->PinType.PinCategory.ToString());
					Pins.Add(MakeShared<FJsonValueObject>(PinObject));
				}
				NodeObject->SetArrayField(TEXT("pins"), MoveTemp(Pins));
				Nodes.Add(MakeShared<FJsonValueObject>(NodeObject));
			}
			Result->SetArrayField(TEXT("nodes"), MoveTemp(Nodes));

			TArray<TSharedPtr<FJsonValue>> Connections;
			TSet<FString> Seen;
			for (UEdGraphNode* Node : Graph.Nodes)
			{
				if (!Node)
				{
					continue;
				}
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (!Pin || Pin->Direction != EGPD_Output)
					{
						continue;
					}
					for (UEdGraphPin* Linked : Pin->LinkedTo)
					{
						UEdGraphNode* TargetNode = Linked
							? Linked->GetOwningNode()
							: nullptr;
						if (!TargetNode)
						{
							continue;
						}
						const FString Key = GuidText(Node->NodeGuid)
							+ TEXT("|") + Pin->PinName.ToString()
							+ TEXT("|") + GuidText(TargetNode->NodeGuid)
							+ TEXT("|") + Linked->PinName.ToString();
						if (Seen.Contains(Key))
						{
							continue;
						}
						Seen.Add(Key);
						const TSharedPtr<FJsonObject> Link = MakeShared<FJsonObject>();
						Link->SetStringField(TEXT("from_node"), LogicalNodeId(*Node));
						Link->SetStringField(TEXT("from_pin"), Pin->PinName.ToString());
						Link->SetStringField(TEXT("to_node"), LogicalNodeId(*TargetNode));
						Link->SetStringField(TEXT("to_pin"), Linked->PinName.ToString());
						Connections.Add(MakeShared<FJsonValueObject>(Link));
					}
				}
			}
			Result->SetArrayField(TEXT("connections"), MoveTemp(Connections));
			Result->SetNumberField(TEXT("node_count"), Graph.Nodes.Num());
			Result->SetNumberField(TEXT("connection_count"), Seen.Num());
			UE::UAI::HjsonWorkspace::DomainAuthoring::AddStoredBlueprintDiagnostics(Blueprint, Result.ToSharedRef());
			return Result;
		}

		class FVerifiedBlueprintExportSource final
			: public IVerifiedExportWorkspaceSource
		{
		public:
			FVerifiedBlueprintExportSource(
				FString InTarget,
				FString InGraphId,
				FString InDocument)
				: Target(MoveTemp(InTarget))
				, GraphId(MoveTemp(InGraphId))
				, Document(MoveTemp(InDocument))
			{
			}

			virtual bool TryFreezeTarget(
				FUeAgentStateRevision& OutRevision,
				FArtifactPublishError& OutError) override
			{
				return ReadRevision(OutRevision, OutError);
			}

			virtual bool TryCaptureWorkspace(
				const FUeAgentStateRevision& FrozenRevision,
				FExportWorkspacePlanPtr& OutPlan,
				FExportSemanticSignature& OutSemantic,
				FArtifactPublishError& OutError) override
			{
				FTCHARToUTF8 Utf8(*Document);
				TArray<uint8> Bytes;
				Bytes.Append(
					reinterpret_cast<const uint8*>(Utf8.Get()),
					Utf8.Length());
				const FString FileHash = Sha256Bytes(Bytes);
				OutSemantic = Signature(Bytes);
				OutPlan = MakeShared<FExportWorkspacePlan>(
					EWorkspaceKind::Directory,
					TEXT("blueprint.hjson"),
					Target,
					FrozenRevision,
					Sha256(Target + TEXT("|") + FileHash),
					TArray<FExportFilePlan>{
						FExportFilePlan(TEXT("blueprint.hjson"), Bytes, FileHash),
					},
					Sha256(FileHash + TEXT("|") + FrozenRevision.GetGeneration()),
					FMath::Max<int64>(1, Bytes.Num() / 4));
				return true;
			}

			virtual bool TryRebuildStagedWorkspace(
				const FString& StagedWorkspacePath,
				const EWorkspaceKind WorkspaceKind,
				const FString& EntryFile,
				FUeAgentResourceTracker& Resources,
				FExportSemanticSignature& OutSemantic,
				FArtifactPublishError& OutError) override
			{
				TArray<uint8> Bytes;
				const FString Path = StagedWorkspacePath / EntryFile;
				if (!FFileHelper::LoadFileToArray(Bytes, *Path))
				{
					OutError = QueryFact(TEXT("blueprint_node_export_rebuild_failed"), Target, TEXT("The staged export entry file could not be read."),
						TEXT("/workspace/entry/readable"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), EntryFile);
					OutError.Stage = TEXT("artifact_publish");
					OutError.ActualEvidence = MakeShared<FJsonValueString>(Path);
					return false;
				}
				OutSemantic = Signature(Bytes);
				return true;
			}

			virtual bool TryReadCurrentTargetRevision(
				FUeAgentStateRevision& OutRevision,
				FArtifactPublishError& OutError) override
			{
				return ReadRevision(OutRevision, OutError);
			}

		private:
			FExportSemanticSignature Signature(
				const TArray<uint8>& Bytes) const
			{
				const FString FileHash = Sha256Bytes(Bytes);
				return FExportSemanticSignature(
					Sha256(TEXT("blueprint.hjson")),
					Sha256(TEXT("blueprint-node-reference-closure|") + FileHash),
					FileHash,
					Sha256(SchemaId),
					Sha256(TEXT("blueprint-node-business|") + FileHash));
			}

			bool ReadRevision(
				FUeAgentStateRevision& OutRevision,
				FArtifactPublishError& OutError) const
			{
				UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Target);
				UEdGraph* Graph = Blueprint ? FindGraph(*Blueprint, GraphId) : nullptr;
				if (!Blueprint || !Graph)
				{
					OutError = QueryFact(TEXT("blueprint_node_export_target_missing"), Target,
						Blueprint ? TEXT("The selected graph no longer exists in the Blueprint export target.") : TEXT("The Blueprint export target could not be loaded."),
						Blueprint ? TEXT("/graph/available") : TEXT("/target/available"), MakeShared<FJsonValueBoolean>(false),
						MakeShared<FJsonValueBoolean>(true), Blueprint ? GraphId : FString());
					OutError.Stage = TEXT("artifact_publish");
					return false;
				}
				OutRevision = FUeAgentStateRevision(
					TEXT("blueprint-node-export"),
					GraphRevision(*Graph),
					1);
				return true;
			}

			FString Target;
			FString GraphId;
			FString Document;
		};

		TSharedPtr<FJsonObject> BuildExportDocument(
			UBlueprint& Blueprint,
			UEdGraph& Graph)
		{
			const TSharedPtr<FJsonObject> Document = MakeShared<FJsonObject>();
			Document->SetStringField(TEXT("schema"), SchemaId);
			const TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("type"), BlueprintType);
			Root->SetStringField(TEXT("target"), Blueprint.GetPathName());
			Root->SetStringField(TEXT("graph_id"), GraphIdentity(Graph));
			Root->SetObjectField(
				TEXT("readback"), BuildReadback(Blueprint, Graph, false));
			Document->SetObjectField(TEXT("root"), Root);
			return Document;
		}
	}

	bool TryGetFirstEditableGraphIdentity(
		UBlueprint& Blueprint,
		FString& OutGraphId,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutGraphId.Reset();
		OutError.Reset();
		TArray<UEdGraph*> Graphs;
		AppendEditableGraphs(Blueprint, Graphs);
		if (Graphs.Num() == 0)
		{
			OutError = QueryFact(TEXT("blueprint_node_graph_unresolved"), Blueprint.GetPathName(), TEXT("The Blueprint contains no editable graph."),
				TEXT("/editable_graph_count"), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one editable graph")));
			return false;
		}
		OutGraphId = GraphIdentity(*Graphs[0]);
		return true;
	}

	bool TryQueryBlueprintNodeCandidates(
		const FUeAgentBlueprintNodeCandidateQuery& Query,
		FString& OutPayloadJson,
		FUeAgentBlueprintNodeMechanicsError& OutError)
	{
		OutPayloadJson.Reset();
		OutError = {};
		ON_SCOPE_EXIT
		{
			if (OutError.IsValid() && OutError.TargetPath.IsEmpty()) OutError.TargetPath = Query.Target.TrimStartAndEnd();
		};
		if ((Query.NodeOccurrence.IsSet()
				&& Query.NodeOccurrence.GetValue() < 0)
			|| (Query.PinOccurrence.IsSet()
				&& Query.PinOccurrence.GetValue() < 0))
		{
			const bool bNodeInvalid = Query.NodeOccurrence.IsSet() && Query.NodeOccurrence.GetValue() < 0;
			auto Diagnostic = QueryFact(TEXT("blueprint_node_candidate_query_invalid"), Query.Target,
				TEXT("Node and pin occurrence indices must be nonnegative."),
				bNodeInvalid ? TEXT("/node_occurrence") : TEXT("/pin_occurrence"),
				MakeShared<FJsonValueNumber>(bNodeInvalid ? Query.NodeOccurrence.GetValue() : Query.PinOccurrence.GetValue()),
				MakeShared<FJsonValueString>(TEXT("nonnegative index")), Query.GraphName, TEXT("command_input"));
			Diagnostic.Source.JsonPath = Diagnostic.PropertyPath;
			return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Query.GraphName);
		}
		const FString Target = Normalize(Query.Target);
		const FString GraphName = Normalize(Query.GraphName);
		UBlueprint* Blueprint = LoadBlueprint(Target, OutError);
		if (!Blueprint)
		{
			return false;
		}
		TArray<UEdGraph*> EditableGraphs;
		AppendEditableGraphs(*Blueprint, EditableGraphs);
		TArray<UEdGraph*> MatchingGraphs;
		for (UEdGraph* CandidateGraph : EditableGraphs)
		{
			if (CandidateGraph && (CandidateGraph->GetName().Equals(GraphName, ESearchCase::IgnoreCase)
				|| CandidateGraph->GetPathName().Equals(GraphName, ESearchCase::IgnoreCase)))
			{
				MatchingGraphs.AddUnique(CandidateGraph);
			}
		}
		if (MatchingGraphs.Num() != 1)
		{
			return ForwardDiagnostic(OutError, QueryFact(MatchingGraphs.IsEmpty() ? TEXT("blueprint_node_graph_unresolved") : TEXT("blueprint_node_graph_ambiguous"),
				Target, TEXT("The graph name must identify exactly one editable graph."), TEXT("/graph/match_count"),
				MakeShared<FJsonValueNumber>(MatchingGraphs.Num()), MakeShared<FJsonValueNumber>(1), GraphName), GraphName);
		}
		UEdGraph* Graph = MatchingGraphs[0];
		if (Query.NodeName.IsEmpty() != Query.PinName.IsEmpty())
		{
			auto Actual = MakeShared<FJsonObject>();
			Actual->SetStringField(TEXT("node"), Query.NodeName);
			Actual->SetStringField(TEXT("pin"), Query.PinName);
			return ForwardDiagnostic(OutError, QueryFact(TEXT("blueprint_node_pin_context_incomplete"), Target,
				TEXT("Pin context requires both a node name and a pin name."), TEXT("/candidate_context"),
				MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("both node and pin names")), GraphName, TEXT("command_input")), GraphName);
		}
		UeAgentPaletteSearch::FSearchParams SearchParams;
		SearchParams.CatalogType = TEXT("blueprint_node");
		SearchParams.SearchText = Normalize(Query.Query);
		SearchParams.AssetPath = Blueprint->GetPathName();
		SearchParams.GraphName = Graph->GetPathName();
		TSharedPtr<FJsonObject> CandidateContext;
		if (!Query.NodeName.IsEmpty())
		{
			using namespace UE::UAI::DomainAdapters::BlueprintUmgGraph;
			FBlueprintUmgGraphNativeHook Hook;
			FBlueprintUmgGraphTopology Topology;
			FBlueprintUmgGraphAdapterError CaptureError;
			FString Revision, Evidence;
			if (!Hook.TryCaptureBlueprintGraph(*Blueprint, *Graph, Blueprint->GetPathName(),
				UeAgentBlueprintAuthoring::GraphIdentity(*Graph), Topology, Revision, Evidence, CaptureError))
				return ForwardDiagnostic(OutError, CaptureError, GraphName);
			TMap<FString, UEdGraphNode*> NodesByGuid;
			for (UEdGraphNode* Node : Graph->Nodes)
				if (Node && Node->NodeGuid.IsValid())
					NodesByGuid.Add(Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens), Node);
			TArray<FGraphAuthoringNodeAliasInput> AliasInputs;
			TMap<FString, UEdGraphNode*> NodesByKey;
			for (const auto& Item : Topology.GetItems())
			{
				UEdGraphNode* Node = NodesByGuid.FindRef(Item.GetNativeGuid());
				AliasInputs.Add({Item.GetItemKey(), Node
					? UeAgentBlueprintAuthoring::NodeSemanticName(*Node)
					: (Item.GetStableBuiltinRole().IsEmpty() ? TEXT("builtin") : Item.GetStableBuiltinRole()),
					Node ? UeAgentBlueprintAuthoring::NodeReference(Node) : Item.GetItemKey()});
				if (Node) NodesByKey.Add(Item.GetItemKey(), Node);
			}
			FGraphAuthoringAliasProjection Aliases;
			FWorkspaceError AliasError;
			if (!TryBuildGraphAuthoringAliasProjection(AliasInputs, Aliases, AliasError))
				return ForwardDiagnostic(OutError, AliasError.GetDiagnostic(), GraphName);
			TArray<UEdGraphNode*> MatchingNodes;
			if (!Query.NodeOccurrence.IsSet())
				if (const FString* Id = Aliases.SourceIdByAlias.Find(Query.NodeName))
					if (UEdGraphNode* Node = NodesByKey.FindRef(*Id)) MatchingNodes.Add(Node);
			if (MatchingNodes.IsEmpty())
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && UeAgentBlueprintAuthoring::NodeSemanticName(*Node).Equals(
						Query.NodeName, ESearchCase::IgnoreCase))
				{
					MatchingNodes.Add(Node);
				}
			}
			const int32 NodeIndex = Query.NodeOccurrence.Get(0);
			if (MatchingNodes.IsEmpty()
				|| (!Query.NodeOccurrence.IsSet() && MatchingNodes.Num() != 1)
				|| !MatchingNodes.IsValidIndex(NodeIndex))
			{
				auto Actual = MakeShared<FJsonObject>();
				Actual->SetNumberField(TEXT("match_count"), MatchingNodes.Num());
				if (Query.NodeOccurrence.IsSet()) Actual->SetNumberField(TEXT("occurrence"), NodeIndex);
				const FString Reason = MatchingNodes.IsEmpty() ? TEXT("No node matches the context node name or alias.")
					: !Query.NodeOccurrence.IsSet() ? TEXT("The context node name matches multiple nodes; an occurrence is required.")
					: TEXT("The node occurrence is outside the matching nodes.");
				return ForwardDiagnostic(OutError, QueryFact(MatchingNodes.IsEmpty() ? TEXT("blueprint_node_context_not_found") : TEXT("blueprint_node_context_ambiguous"),
					Target, Reason, TEXT("/candidate_context/node"), MakeShared<FJsonValueObject>(Actual),
					MakeShared<FJsonValueString>(TEXT("one match, or an occurrence within match_count")), Query.NodeName), Query.NodeName);
			}
			UEdGraphNode* ContextNode = MatchingNodes[NodeIndex];
			TArray<UEdGraphPin*> MatchingPins;
			TMap<FString, UEdGraphPin*> PinAliases;
			UE::UAI::Contract::FDiagnostic PinAliasError;
			if (!UeAgentBlueprintAuthoring::BuildPinAliasLookup(*ContextNode, PinAliases, PinAliasError))
				return ForwardDiagnostic(OutError, PinAliasError, Query.PinName);
			if (!Query.PinOccurrence.IsSet())
				if (UEdGraphPin* Pin = PinAliases.FindRef(Query.PinName)) MatchingPins.Add(Pin);
			if (MatchingPins.IsEmpty())
			for (UEdGraphPin* Pin : ContextNode->Pins)
			{
				if (Pin && Pin->PinName.ToString().Equals(
						Query.PinName, ESearchCase::IgnoreCase))
				{
					MatchingPins.Add(Pin);
				}
			}
			const int32 PinIndex = Query.PinOccurrence.Get(0);
			if (MatchingPins.IsEmpty()
				|| (!Query.PinOccurrence.IsSet() && MatchingPins.Num() != 1)
				|| !MatchingPins.IsValidIndex(PinIndex))
			{
				auto Actual = MakeShared<FJsonObject>();
				Actual->SetNumberField(TEXT("match_count"), MatchingPins.Num());
				if (Query.PinOccurrence.IsSet()) Actual->SetNumberField(TEXT("occurrence"), PinIndex);
				const FString Reason = MatchingPins.IsEmpty() ? TEXT("No pin matches the context pin name or alias.")
					: !Query.PinOccurrence.IsSet() ? TEXT("The context pin name matches multiple pins; an occurrence is required.")
					: TEXT("The pin occurrence is outside the matching pins.");
				return ForwardDiagnostic(OutError, QueryFact(MatchingPins.IsEmpty() ? TEXT("blueprint_pin_context_not_found") : TEXT("blueprint_pin_context_ambiguous"),
					Target, Reason, TEXT("/candidate_context/pin"), MakeShared<FJsonValueObject>(Actual),
					MakeShared<FJsonValueString>(TEXT("one match, or an occurrence within match_count")), Query.PinName), Query.PinName);
			}
			SearchParams.NodeGuid = ContextNode->NodeGuid.ToString(
				EGuidFormats::DigitsWithHyphensLower);
			SearchParams.PinGuid = MatchingPins[PinIndex]->PinId.ToString(
				EGuidFormats::DigitsWithHyphensLower);
			SearchParams.PinName = MatchingPins[PinIndex]->PinName.ToString();
			// Return the same public aliases consumed by graph authoring, including
			// occurrence disambiguation. The caller must not translate native names.
			FString NodeAlias, PinAlias;
			for (const auto& Alias : Aliases.SourceIdByAlias)
				if (NodesByKey.FindRef(Alias.Value) == ContextNode) { NodeAlias = Alias.Key; break; }
			for (const auto& Alias : PinAliases)
				if (Alias.Value == MatchingPins[PinIndex]) { PinAlias = Alias.Key; break; }
			if (NodeAlias.IsEmpty() || PinAlias.IsEmpty())
			{
				auto Actual = MakeShared<FJsonObject>();
				Actual->SetBoolField(TEXT("node_alias_available"), !NodeAlias.IsEmpty());
				Actual->SetBoolField(TEXT("pin_alias_available"), !PinAlias.IsEmpty());
				auto Expected = MakeShared<FJsonObject>();
				Expected->SetBoolField(TEXT("node_alias_available"), true);
				Expected->SetBoolField(TEXT("pin_alias_available"), true);
				return ForwardDiagnostic(OutError, QueryFact(TEXT("blueprint_node_context_alias_unresolved"), Target,
					TEXT("The resolved native node or pin has no public authoring alias."), TEXT("/candidate_context/aliases"),
					MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected), GraphName), GraphName);
			}
			CandidateContext = MakeShared<FJsonObject>();
			CandidateContext->SetStringField(TEXT("node"), NodeAlias);
			CandidateContext->SetStringField(TEXT("pin"), PinAlias);
		}
		SearchParams.bContextSensitive = true;
		SearchParams.bIncludeNameContext = true;
		UeAgentPaletteSearch::FSearchResult SearchResult;
		UE::UAI::Contract::FDiagnostic SearchError;
		if (!UeAgentPaletteSearch::ExecuteSearch(
				SearchParams,
				SearchResult,
				SearchError))
		{
			return ForwardDiagnostic(OutError, MoveTemp(SearchError), GraphName);
		}
		const TArray<UeAgentPaletteSearch::FCandidate>& FullCandidates = SearchResult.NameContextCandidates;
		TArray<ContentDiscovery::FNameSelectionEntry> FullEntries;
		TMultiMap<FString, int32> FullSemanticIndices;
		FullEntries.Reserve(FullCandidates.Num());
		for (const UeAgentPaletteSearch::FCandidate& Match : FullCandidates)
		{
			if (Match.SupportDiagnostic.Code == TEXT("native_view_action_is_navigation_not_creation")) continue;
			const FString& SemanticIdentity = Match.Identity.CanonicalKey;
			ContentDiscovery::FScopedNameDiscoveryEntry Entry;
			// Name context includes actions outside this query. It supplies readable
			// disambiguation scopes; only actual matches can fail the query below.
			if (!UeAgentPaletteSearch::TryBuildBlueprintActionScopedDiscoveryEntry(Match, Entry)) continue;
			const int32 FullIndex = FullEntries.Add({MoveTemp(Entry.Name), SemanticIdentity, {}, MoveTemp(Entry.Scopes)});
			FullSemanticIndices.Add(SemanticIdentity, FullIndex);
		}
		UE::UAI::Contract::FDiagnostic ProjectionError;
		TArray<int32> MatchedIndices;
		auto ReadableNameMatchesQuery = [&Query](const FString& Name)
		{
			auto Compact = [](const FString& Value)
			{
				FString Result;
				for (const TCHAR Character : Value.ToLower())
				{
					if (FChar::IsAlnum(Character))
					{
						Result.AppendChar(Character);
					}
				}
				return Result;
			};
			const FString CompactName = Compact(Name);
			const FString CompactQuery = Compact(Query.Query);
			if (!CompactQuery.IsEmpty() && CompactName.Contains(CompactQuery))
			{
				return true;
			}
			TArray<FString> QueryWords;
			Query.Query.ParseIntoArrayWS(QueryWords);
			for (const FString& QueryWord : QueryWords)
			{
				const FString CompactWord = Compact(QueryWord);
				if (!CompactWord.IsEmpty() && !CompactName.Contains(CompactWord))
				{
					return false;
				}
			}
			return !QueryWords.IsEmpty();
		};
		for (const UeAgentPaletteSearch::FCandidate& Match : SearchResult.MatchingCandidates)
		{
			if (Match.SupportDiagnostic.Code == TEXT("native_view_action_is_navigation_not_creation")) continue;
			const FString PublicName = UeAgentPaletteSearch::GetBlueprintActionPublicName(Match);
			if (!ContentDiscovery::IsReadableDiscoveryName(PublicName))
			{
				auto Diagnostic = QueryFact(TEXT("candidate_name_invalid"), Target,
					TEXT("A matched native Blueprint creation action has no readable public name."), TEXT("/candidate/name"),
					MakeShared<FJsonValueString>(PublicName), MakeShared<FJsonValueString>(TEXT("readable native action name")), GraphName);
				Diagnostic.InternalContext = Match.Identity.CanonicalKey;
				return ForwardDiagnostic(OutError, Diagnostic, GraphName);
			}
			TArray<int32> FullIndices;
			FullSemanticIndices.MultiFind(Match.Identity.CanonicalKey, FullIndices, true);
			if (FullIndices.IsEmpty())
			{
				auto Diagnostic = QueryFact(TEXT("candidate_projection_identity_missing"), Target,
					TEXT("A matched Blueprint action is missing from the catalog used for public projection."), TEXT("/candidate/catalog_membership"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Match.Name);
				Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Match.Identity.CanonicalKey);
				return ForwardDiagnostic(OutError, MoveTemp(Diagnostic), Match.Name);
			}
			// Native menu text and keywords are valid discovery terms even when
			// the stable authoring name differs. Keep the native match; return
			// the stable name; actual selection resolves any semantic ambiguity.
			MatchedIndices.Append(FullIndices);
		}
		// Match all declared semantic search fields before grouping public names.
		for (int32 Index = 0; Index < FullEntries.Num(); ++Index)
		{
			const ContentDiscovery::FNameSelectionEntry& Entry = FullEntries[Index];
			FString SearchableFields = Entry.Name;
			for (const FString& Scope : Entry.SearchTerms)
			{
				SearchableFields += TEXT(" ");
				SearchableFields += Scope;
			}
			if (ReadableNameMatchesQuery(SearchableFields))
			{
				MatchedIndices.Add(Index);
			}
		}
		if (!ContentDiscovery::TryBuildCandidateNamesOnlyPayloadForIndices(
				FullEntries, MatchedIndices,
				ContentDiscovery::ENameComparison::IgnoreCase,
				OutPayloadJson,
				ProjectionError))
		{
			return ForwardDiagnostic(OutError, MoveTemp(ProjectionError), FString());
		}
		if (CandidateContext.IsValid())
		{
			TSharedPtr<FJsonObject> Result;
			if (!Json::TryParseObject(OutPayloadJson, Result, ProjectionError))
				return ForwardDiagnostic(OutError, MoveTemp(ProjectionError), GraphName);
			Result->SetObjectField(TEXT("candidate_context"), CandidateContext);
			OutPayloadJson.Reset();
			const auto Writer = TJsonWriterFactory<>::Create(&OutPayloadJson);
			if (!FJsonSerializer::Serialize(Result.ToSharedRef(), Writer))
				return ForwardDiagnostic(OutError, QueryFact(TEXT("blueprint_node_candidate_payload_invalid"), Target,
					TEXT("The candidate payload with its resolved context could not be serialized."), TEXT("/candidate_payload/serialized"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), GraphName), GraphName);
		}
		const auto Scope = MakeShared<FJsonObject>();
		Scope->SetStringField(TEXT("target"), Target);
		Scope->SetStringField(TEXT("graph_name"), GraphName);
		if (!ContentDiscovery::TryAddDiscoveryScope(Scope, OutPayloadJson, ProjectionError))
			return ForwardDiagnostic(OutError, MoveTemp(ProjectionError), GraphName);
		return true;
	}

}
