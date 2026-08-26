#include "DomainAdapters/Blueprint/Node/UeAgentBlueprintNodeMechanics.h"

#include "BlueprintActionDatabase.h"
#include "BlueprintNodeSpawner.h"
#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Core/ArtifactPublisher/UeAgentVerifiedExport.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "K2Node_IfThenElse.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

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
		const FString BranchCandidate =
			TEXT("/Script/BlueprintGraph.K2Node_IfThenElse");
		const FString SchemaId = TEXT("uai.blueprint.node.edit/1");

		struct FPlannedNode final
		{
			FString Id;
			FString CandidateName;
			FString BindingRevision;
			int32 X = 0;
			int32 Y = 0;
		};

		struct FPlannedConnection final
		{
			FString FromNode;
			FString FromPin;
			FString ToNode;
			FString ToPin;
		};

		struct FPreparedImport final
		{
			FString Target;
			FString ExpectedRevision;
			FString GraphId;
			FString InitialRevision;
			TArray<FPlannedNode> Nodes;
			TArray<FPlannedConnection> Connections;
			UBlueprint* Blueprint = nullptr;
			UEdGraph* Graph = nullptr;
		};

		FString Normalize(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		bool Fail(
			FUeAgentBlueprintNodeMechanicsError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
			return false;
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

		FString Canonical(const TSharedPtr<FJsonObject>& Object)
		{
			FString Result;
			FString Error;
			return Json::TryCanonicalize(Object, Result, Error)
				? Result
				: FString(TEXT("{}"));
		}

		FUeAgentResourceTracker MakeResources()
		{
			return FUeAgentResourceTracker(FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::StringBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 4096},
				{EUeAgentResourceDimension::Items, 65536},
				{EUeAgentResourceDimension::Files, 4096},
				{EUeAgentResourceDimension::ReferenceEdges, 65536},
				{EUeAgentResourceDimension::WorkUnits, 262144},
				{EUeAgentResourceDimension::EstimatedMemoryBytes, 128 * 1024 * 1024},
			}));
		}

		FString GuidText(const FGuid& Guid)
		{
			return Guid.IsValid()
				? Guid.ToString(EGuidFormats::DigitsWithHyphensLower)
				: FString();
		}

		FString GraphIdentity(const UEdGraph& Graph)
		{
			const FString Guid = GuidText(Graph.GraphGuid);
			return !Guid.IsEmpty()
				? TEXT("graph:") + Guid
				: TEXT("graph:name:") + Graph.GetName();
		}

		void AppendEditableGraphs(
			UBlueprint& Blueprint,
			TArray<UEdGraph*>& OutGraphs)
		{
			OutGraphs.Reset();
			for (UEdGraph* Graph : Blueprint.UbergraphPages)
			{
				if (Graph)
				{
					OutGraphs.Add(Graph);
				}
			}
			for (UEdGraph* Graph : Blueprint.FunctionGraphs)
			{
				if (Graph)
				{
					OutGraphs.Add(Graph);
				}
			}
			for (UEdGraph* Graph : Blueprint.MacroGraphs)
			{
				if (Graph)
				{
					OutGraphs.Add(Graph);
				}
			}
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

		FString BindingRevision(
			const FString& Target,
			const FString& GraphId,
			const FString& CandidateName,
			const FString& GraphRevisionValue)
		{
			return Sha256(
				Target + TEXT("|") + GraphId + TEXT("|")
				+ CandidateName + TEXT("|") + GraphRevisionValue);
		}

		bool ResolveBranchSpawner(
			UEdGraph& Graph,
			UBlueprintNodeSpawner*& OutSpawner,
			FUeAgentBlueprintNodeMechanicsError& OutError)
		{
			OutSpawner = nullptr;
			TArray<UBlueprintNodeSpawner*> Matches;
			for (const TPair<FObjectKey, FBlueprintActionDatabase::FActionList>& Entry
				: FBlueprintActionDatabase::Get().GetAllActions())
			{
				for (UBlueprintNodeSpawner* Spawner : Entry.Value)
				{
					if (Spawner
						&& Spawner->NodeClass == UK2Node_IfThenElse::StaticClass())
					{
						Matches.AddUnique(Spawner);
					}
				}
			}
			if (Matches.Num() == 0)
			{
				return Fail(
					OutError,
					TEXT("blueprint_node_candidate_unresolved"),
					BranchCandidate,
					TEXT("the real Blueprint action database has no Branch spawner"));
			}
			if (Matches.Num() > 1)
			{
				return Fail(
					OutError,
					TEXT("blueprint_node_candidate_ambiguous"),
					BranchCandidate,
					FString::FromInt(Matches.Num()));
			}
			Matches[0]->PrimeDefaultUiSpec(&Graph);
			OutSpawner = Matches[0];
			return true;
		}

		UBlueprint* LoadBlueprint(
			const FString& Target,
			FUeAgentBlueprintNodeMechanicsError& OutError)
		{
			UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Target);
			if (!Blueprint)
			{
				Fail(
					OutError,
					TEXT("blueprint_node_target_unresolved"),
					Target);
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
			Result->SetStringField(
				TEXT("compile_status"),
				Blueprint.Status == BS_Error ? TEXT("error") : TEXT("valid"));
			return Result;
		}

		const FHjsonNode* RequiredObjectField(
			const FHjsonNode& Object,
			const FString& Name)
		{
			const FHjsonNode* Value = Object.Get(Name);
			return Value && Value->GetKind() == EHjsonNodeKind::Object
				? Value
				: nullptr;
		}

		bool StringField(
			const FHjsonNode& Object,
			const FString& Name,
			FString& Out)
		{
			const FHjsonNode* Value = Object.Get(Name);
			if (!Value || Value->GetKind() != EHjsonNodeKind::String)
			{
				Out.Reset();
				return false;
			}
			Out = Normalize(Value->GetText());
			return !Out.IsEmpty();
		}

		bool IntegerField(
			const FHjsonNode& Object,
			const FString& Name,
			int32& Out)
		{
			const FHjsonNode* Value = Object.Get(Name);
			if (!Value || Value->GetKind() != EHjsonNodeKind::Integer)
			{
				return false;
			}
			Out = FCString::Atoi(*Value->GetLexeme());
			return true;
		}

		bool TryReadResolvedDocument(
			const TSharedPtr<FJsonObject>& Params,
			const FString& ExpectedTarget,
			FString& OutDocument,
			FUeAgentBlueprintNodeMechanicsError& OutError)
		{
			OutDocument.Reset();
			if (!Params.IsValid()
				|| Params->HasField(TEXT("carrier"))
				|| Params->HasField(TEXT("hjson_document")))
			{
				return Fail(
					OutError,
					TEXT("content_graph_legacy_source_contract_rejected"),
					ExpectedTarget);
			}
			const TSharedPtr<FJsonObject>* Source = nullptr;
			if (!Params->TryGetObjectField(TEXT("source"), Source)
				|| !Source || !Source->IsValid())
			{
				return Fail(
					OutError,
					TEXT("content_graph_source_required"),
					ExpectedTarget);
			}
			FString Kind;
			FString TargetIdentity;
			FString EntryFile;
			const TArray<TSharedPtr<FJsonValue>>* Documents = nullptr;
			if (!(*Source)->TryGetStringField(TEXT("kind"), Kind)
				|| Kind != TEXT("resolved")
				|| !(*Source)->TryGetStringField(
					TEXT("target_identity"), TargetIdentity)
				|| TargetIdentity != ExpectedTarget
				|| !(*Source)->TryGetStringField(TEXT("entry_file"), EntryFile)
				|| EntryFile.IsEmpty()
				|| !(*Source)->TryGetArrayField(TEXT("documents"), Documents)
				|| !Documents || Documents->IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("content_graph_resolved_source_invalid"),
					ExpectedTarget);
			}
			int32 EntryMatches = 0;
			for (const TSharedPtr<FJsonValue>& Value : *Documents)
			{
				const TSharedPtr<FJsonObject> Document = Value.IsValid()
					? Value->AsObject()
					: nullptr;
				FString RelativePath;
				FString DocumentText;
				if (!Document.IsValid()
					|| !Document->TryGetStringField(
						TEXT("path"), RelativePath)
					|| !Document->TryGetStringField(
						TEXT("document"), DocumentText))
				{
					return Fail(
						OutError,
						TEXT("content_graph_resolved_source_invalid"),
						ExpectedTarget);
				}
				if (RelativePath == EntryFile)
				{
					OutDocument = MoveTemp(DocumentText);
					++EntryMatches;
				}
			}
			if (EntryMatches != 1 || OutDocument.IsEmpty())
			{
				OutDocument.Reset();
				return Fail(
					OutError,
					TEXT("content_graph_resolved_source_invalid"),
					EntryFile,
					FString::FromInt(EntryMatches));
			}
			return true;
		}

		bool TryPrepareImport(
			const TSharedPtr<FJsonObject>& Params,
			FPreparedImport& Out,
			FUeAgentBlueprintNodeMechanicsError& OutError)
		{
			Out = {};
			FString DocumentText;
			if (!Params.IsValid()
				|| !Params->TryGetStringField(TEXT("target"), Out.Target)
				|| Normalize(Out.Target).IsEmpty()
				|| !Params->TryGetStringField(
					TEXT("expected_revision"), Out.ExpectedRevision))
			{
				return Fail(
					OutError,
					TEXT("blueprint_node_import_params_invalid"));
			}
			Out.Target = Normalize(Out.Target);
			Out.ExpectedRevision = Normalize(Out.ExpectedRevision);
			if (!TryReadResolvedDocument(
					Params, Out.Target, DocumentText, OutError))
			{
				return false;
			}
			Out.Blueprint = LoadBlueprint(Out.Target, OutError);
			if (!Out.Blueprint)
			{
				return false;
			}
			if (Out.Blueprint->GetOutermost()->IsDirty())
			{
				return Fail(
					OutError,
					TEXT("blueprint_node_target_dirty"),
					Out.Target,
					TEXT("save or discard the exact target before HJSON authoring"));
			}

			FUeAgentResourceTracker Resources = MakeResources();
			FHjsonDocumentPtr Document;
			FWorkspaceError WorkspaceError;
			if (!TryParseHjsonDocument(
					DocumentText,
					TEXT("inline:blueprint-node"),
					Resources,
					Document,
					WorkspaceError)
				|| !Document.IsValid()
				|| Document->GetRoot()->GetKind() != EHjsonNodeKind::Object)
			{
				return Fail(
					OutError,
					WorkspaceError.GetCode().IsEmpty()
						? FString(TEXT("blueprint_node_hjson_invalid"))
						: WorkspaceError.GetCode(),
					WorkspaceError.GetSourceUnit(),
					WorkspaceError.GetJsonPointer());
			}
			const FHjsonNode& DocumentRoot = *Document->GetRoot();
			FString DocumentSchema;
			if (!StringField(DocumentRoot, TEXT("schema"), DocumentSchema)
				|| DocumentSchema != SchemaId)
			{
				return Fail(
					OutError,
					TEXT("blueprint_node_schema_invalid"),
					DocumentSchema);
			}
			const FHjsonNode* Root = RequiredObjectField(DocumentRoot, TEXT("root"));
			FString RootType;
			if (!Root || !StringField(*Root, TEXT("type"), RootType)
				|| RootType != BlueprintType
				|| !StringField(*Root, TEXT("graph_id"), Out.GraphId))
			{
				return Fail(
					OutError,
					TEXT("blueprint_node_root_identity_invalid"),
					Out.Target);
			}
			Out.Graph = FindGraph(*Out.Blueprint, Out.GraphId);
			if (!Out.Graph)
			{
				return Fail(
					OutError,
					TEXT("blueprint_node_graph_unresolved"),
					Out.GraphId);
			}
			Out.InitialRevision = GraphRevision(*Out.Graph);
			if (Out.ExpectedRevision != Out.InitialRevision)
			{
				return Fail(
					OutError,
					TEXT("blueprint_node_stale_revision"),
					Out.GraphId,
					Out.InitialRevision);
			}

			UBlueprintNodeSpawner* Spawner = nullptr;
			if (!ResolveBranchSpawner(*Out.Graph, Spawner, OutError))
			{
				return false;
			}
			const FString CurrentBinding = BindingRevision(
				Out.Target,
				Out.GraphId,
				BranchCandidate,
				Out.InitialRevision);
			const FHjsonNode* Nodes = Root->Get(TEXT("nodes"));
			if (!Nodes || Nodes->GetKind() != EHjsonNodeKind::Array
				|| Nodes->GetItems().Num() == 0)
			{
				return Fail(
					OutError,
					TEXT("blueprint_node_nodes_required"),
					Out.GraphId);
			}
			TSet<FString> Ids;
			for (const FHjsonNodeRef& NodeValue : Nodes->GetItems())
			{
				if (NodeValue->GetKind() != EHjsonNodeKind::Object)
				{
					return Fail(OutError, TEXT("blueprint_node_entry_invalid"));
				}
				FPlannedNode Planned;
				const FHjsonNode* Candidate = RequiredObjectField(
					*NodeValue, TEXT("candidate"));
				const FHjsonNode* Position = RequiredObjectField(
					*NodeValue, TEXT("position"));
				if (!StringField(*NodeValue, TEXT("id"), Planned.Id)
					|| Ids.Contains(Planned.Id)
					|| !Candidate
					|| !StringField(
						*Candidate, TEXT("name"), Planned.CandidateName)
					|| !StringField(
						*Candidate,
						TEXT("binding_revision"),
						Planned.BindingRevision)
					|| Planned.CandidateName != BranchCandidate
					|| Planned.BindingRevision != CurrentBinding
					|| !Position
					|| !IntegerField(*Position, TEXT("x"), Planned.X)
					|| !IntegerField(*Position, TEXT("y"), Planned.Y))
				{
					return Fail(
						OutError,
						TEXT("blueprint_node_candidate_binding_invalid"),
						Planned.Id);
				}
				Ids.Add(Planned.Id);
				Out.Nodes.Add(MoveTemp(Planned));
			}

			const FHjsonNode* Connections = Root->Get(TEXT("connections"));
			if (Connections)
			{
				if (Connections->GetKind() != EHjsonNodeKind::Array)
				{
					return Fail(
						OutError,
						TEXT("blueprint_node_connections_invalid"));
				}
				for (const FHjsonNodeRef& LinkValue : Connections->GetItems())
				{
					if (LinkValue->GetKind() != EHjsonNodeKind::Object)
					{
						return Fail(
							OutError,
							TEXT("blueprint_node_connection_invalid"));
					}
					FPlannedConnection Link;
					if (!StringField(*LinkValue, TEXT("from_node"), Link.FromNode)
						|| !StringField(*LinkValue, TEXT("from_pin"), Link.FromPin)
						|| !StringField(*LinkValue, TEXT("to_node"), Link.ToNode)
						|| !StringField(*LinkValue, TEXT("to_pin"), Link.ToPin)
						|| !Ids.Contains(Link.FromNode)
						|| !Ids.Contains(Link.ToNode)
						|| Link.FromNode == Link.ToNode
						|| (Link.FromPin != TEXT("then")
							&& Link.FromPin != TEXT("else"))
						|| Link.ToPin != TEXT("execute"))
					{
						return Fail(
							OutError,
							TEXT("blueprint_node_connection_incompatible"),
							Link.FromNode + TEXT("->") + Link.ToNode);
					}
					Out.Connections.Add(MoveTemp(Link));
				}
			}
			return true;
		}

		UEdGraphPin* FindPin(
			UEdGraphNode& Node,
			const FString& Name,
			const EEdGraphPinDirection Direction)
		{
			for (UEdGraphPin* Pin : Node.Pins)
			{
				if (Pin && Pin->Direction == Direction
					&& Pin->PinName.ToString() == Name)
				{
					return Pin;
				}
			}
			return nullptr;
		}

		bool SaveBlueprint(
			UBlueprint& Blueprint,
			FUeAgentBlueprintNodeMechanicsError& OutError)
		{
			UPackage* Package = Blueprint.GetOutermost();
			const FString Filename = FPackageName::LongPackageNameToFilename(
				Package->GetName(), FPackageName::GetAssetPackageExtension());
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			Args.SaveFlags = SAVE_NoError;
			if (!UPackage::SavePackage(Package, &Blueprint, *Filename, Args))
			{
				return Fail(
					OutError,
					TEXT("blueprint_node_save_failed"),
					Blueprint.GetPathName(),
					Filename);
			}
			return true;
		}

		void ReloadForRollback(UPackage* Package)
		{
			if (!Package)
			{
				return;
			}
			FText Error;
			UPackageTools::ReloadPackages(
				{Package},
				Error,
				EReloadPackagesInteractionMode::AssumeNegative);
		}

		bool ExecuteImport(
			const TSharedPtr<FJsonObject>& Params,
			FString& OutPayloadJson,
			FUeAgentBlueprintNodeMechanicsError& OutError)
		{
			FPreparedImport Prepared;
			if (!TryPrepareImport(Params, Prepared, OutError))
			{
				return false;
			}

			TSharedPtr<const FImportInitializationDomainProviderRegistry>
				ProviderRegistry;
			FWorkspaceError WorkspaceError;
			if (!TryFreezeProcessImportInitializationDomainProviders(
					ProviderRegistry, WorkspaceError)
				|| !ProviderRegistry.IsValid())
			{
				return Fail(
					OutError,
					WorkspaceError.GetCode().IsEmpty()
						? FString(TEXT("blueprint_node_provider_registry_unavailable"))
						: WorkspaceError.GetCode());
			}
			for (const FPlannedNode& Planned : Prepared.Nodes)
			{
				FImportInitializationDomainResolution Resolution;
				if (!ProviderRegistry->TryResolve(
						FImportInitializationDomainRequest(
							TEXT("content_graph.import_workspace"),
							SchemaId,
							Prepared.Target + TEXT("#") + Planned.Id,
							Planned.CandidateName,
							EInitializationNodeRole::OwnedObject,
							TEXT("{}")),
						Resolution,
						WorkspaceError)
					|| !Resolution.IsValid()
					|| Resolution.GetInitializationContractId()
						!= TEXT("uai.init.blueprint.node/1"))
				{
					return Fail(
						OutError,
						TEXT("blueprint_node_initialization_contract_unresolved"),
						Planned.Id);
				}
			}

			UPackage* Package = Prepared.Blueprint->GetOutermost();
			Prepared.Blueprint->Modify();
			Prepared.Graph->Modify();
			TMap<FString, UEdGraphNode*> NodesById;
			for (UEdGraphNode* Existing : Prepared.Graph->Nodes)
			{
				if (Existing && Existing->NodeComment.StartsWith(TEXT("uai_id:")))
				{
					NodesById.Add(LogicalNodeId(*Existing), Existing);
				}
			}
			UBlueprintNodeSpawner* DatabaseSpawner = nullptr;
			if (!ResolveBranchSpawner(
					*Prepared.Graph, DatabaseSpawner, OutError))
			{
				return false;
			}
			for (const FPlannedNode& Planned : Prepared.Nodes)
			{
				UEdGraphNode* Node = NodesById.FindRef(Planned.Id);
				if (Node && Node->GetClass() != UK2Node_IfThenElse::StaticClass())
				{
					ReloadForRollback(Package);
					return Fail(
						OutError,
						TEXT("blueprint_node_existing_type_conflict"),
						Planned.Id);
				}
				if (!Node)
				{
					Node = DatabaseSpawner->Invoke(
						Prepared.Graph,
						IBlueprintNodeBinder::FBindingSet(),
						FVector2D(Planned.X, Planned.Y));
					if (!Node)
					{
						ReloadForRollback(Package);
						return Fail(
							OutError,
							TEXT("blueprint_node_spawn_failed"),
							Planned.Id);
					}
					Node->NodeComment = TEXT("uai_id:") + Planned.Id;
					NodesById.Add(Planned.Id, Node);
				}
				Node->Modify();
				Node->NodePosX = Planned.X;
				Node->NodePosY = Planned.Y;
				Node->NodeComment = TEXT("uai_id:") + Planned.Id;
			}
			for (const FPlannedConnection& Link : Prepared.Connections)
			{
				UEdGraphNode* FromNode = NodesById.FindRef(Link.FromNode);
				UEdGraphNode* ToNode = NodesById.FindRef(Link.ToNode);
				UEdGraphPin* FromPin = FromNode
					? FindPin(*FromNode, Link.FromPin, EGPD_Output)
					: nullptr;
				UEdGraphPin* ToPin = ToNode
					? FindPin(*ToNode, Link.ToPin, EGPD_Input)
					: nullptr;
				if (!FromPin || !ToPin
					|| (!FromPin->LinkedTo.Contains(ToPin)
						&& !Prepared.Graph->GetSchema()->TryCreateConnection(
							FromPin, ToPin)))
				{
					ReloadForRollback(Package);
					return Fail(
						OutError,
						TEXT("blueprint_node_connection_failed"),
						Link.FromNode + TEXT("->") + Link.ToNode);
				}
			}
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(
				Prepared.Blueprint);
			FKismetEditorUtilities::CompileBlueprint(
				Prepared.Blueprint,
				EBlueprintCompileOptions::SkipGarbageCollection);
			if (Prepared.Blueprint->Status == BS_Error)
			{
				ReloadForRollback(Package);
				return Fail(
					OutError,
					TEXT("blueprint_node_compile_failed"),
					Prepared.Target);
			}
			if (!SaveBlueprint(*Prepared.Blueprint, OutError))
			{
				ReloadForRollback(Package);
				return false;
			}

			const FString Target = Prepared.Target;
			const FString GraphId = Prepared.GraphId;
			Prepared.Graph = nullptr;
			Prepared.Blueprint = nullptr;
			FText UnloadError;
			if (!UPackageTools::UnloadPackages({Package}, UnloadError))
			{
				return Fail(
					OutError,
					TEXT("blueprint_node_unload_failed"),
					Target,
					UnloadError.ToString());
			}
			UBlueprint* Reloaded = LoadBlueprint(Target, OutError);
			UEdGraph* ReloadedGraph = Reloaded
				? FindGraph(*Reloaded, GraphId)
				: nullptr;
			if (!Reloaded || !ReloadedGraph)
			{
				return Fail(
					OutError,
					TEXT("blueprint_node_reload_failed"),
					Target);
			}
			const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
			Payload->SetStringField(
				TEXT("command_id"), TEXT("content_graph.import_workspace"));
			Payload->SetStringField(TEXT("terminal_status"), TEXT("succeeded"));
			Payload->SetBoolField(TEXT("mutation_started"), true);
			Payload->SetStringField(
				TEXT("persistence_receipt_id"),
				Sha256(Target + TEXT("|") + GraphRevision(*ReloadedGraph)));
			Payload->SetObjectField(
				TEXT("independent_readback"),
				BuildReadback(*Reloaded, *ReloadedGraph, true));
			OutPayloadJson = Canonical(Payload);
			return true;
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
					OutError.Set(TEXT("blueprint_node_export_rebuild_failed"), Path);
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
					OutError.Set(TEXT("blueprint_node_export_target_missing"), Target);
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
		FString& OutError)
	{
		OutGraphId.Reset();
		OutError.Reset();
		TArray<UEdGraph*> Graphs;
		AppendEditableGraphs(Blueprint, Graphs);
		if (Graphs.Num() == 0)
		{
			OutError = TEXT("blueprint_node_graph_unresolved");
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
		if (Query.Limit < 1 || Query.Limit > 20 || Query.Cursor < 0)
		{
			return Fail(
				OutError,
				TEXT("blueprint_node_candidate_page_invalid"),
				Query.GraphId);
		}
		const FString Target = Normalize(Query.Target);
		const FString GraphId = Normalize(Query.GraphId);
		UBlueprint* Blueprint = LoadBlueprint(Target, OutError);
		UEdGraph* Graph = Blueprint
			? FindGraph(*Blueprint, GraphId)
			: nullptr;
		if (!Blueprint)
		{
			return false;
		}
		if (!Graph)
		{
			return Fail(
				OutError,
				TEXT("blueprint_node_graph_unresolved"),
				GraphId);
		}
		UeAgentPaletteSearch::FSearchParams SearchParams;
		SearchParams.CatalogType = TEXT("blueprint_node");
		SearchParams.SearchText = Normalize(Query.Query);
		SearchParams.CandidateId = Normalize(Query.CandidateId);
		SearchParams.AssetPath = Blueprint->GetPathName();
		SearchParams.GraphName = Graph->GetName();
		SearchParams.NodeGuid = Normalize(Query.NodeGuid);
		SearchParams.PinGuid = Normalize(Query.PinGuid);
		SearchParams.PinName = Normalize(Query.PinName);
		SearchParams.bContextSensitive = true;
		SearchParams.bIncludeItems = true;
		SearchParams.bIncludeGroups = false;
		SearchParams.bIncludeCategoryTree = false;
		SearchParams.Limit = Query.Limit;
		SearchParams.Offset = Query.Cursor;
		UeAgentPaletteSearch::FSearchResult SearchResult;
		FString SearchError;
		if (!UeAgentPaletteSearch::ExecuteSearch(
				SearchParams,
				SearchResult,
				SearchError))
		{
			return Fail(
				OutError,
				TEXT("blueprint_node_candidate_query_failed"),
				GraphId,
				SearchError);
		}
		const FString Revision = GraphRevision(*Graph);
		const int32 StartIndex = FMath::Min(
			Query.Cursor, SearchResult.MatchingCandidates.Num());
		const int32 EndIndex = FMath::Min(
			StartIndex + Query.Limit,
			SearchResult.MatchingCandidates.Num());
		const bool bHasMore = EndIndex < SearchResult.TotalMatching;

		const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(
			TEXT("command_id"), TEXT("blueprint.node.candidates"));
		Payload->SetBoolField(TEXT("read_only"), true);
		Payload->SetStringField(TEXT("target"), Target);
		Payload->SetObjectField(TEXT("graph"), BuildGraphObject(*Graph));
		Payload->SetStringField(TEXT("graph_revision"), Revision);
		const TSharedPtr<FJsonObject> Context = MakeShared<FJsonObject>();
		Context->SetStringField(TEXT("graph_id"), GraphId);
		if (!Query.NodeGuid.IsEmpty())
		{
			Context->SetStringField(TEXT("node_guid"), Query.NodeGuid);
		}
		if (!Query.PinGuid.IsEmpty())
		{
			Context->SetStringField(TEXT("pin_guid"), Query.PinGuid);
		}
		if (!Query.PinName.IsEmpty())
		{
			Context->SetStringField(TEXT("pin_name"), Query.PinName);
		}
		Payload->SetObjectField(TEXT("context"), Context);
		Payload->SetBoolField(TEXT("complete"), !bHasMore);
		Payload->SetBoolField(TEXT("has_more"), bHasMore);
		Payload->SetNumberField(TEXT("total_matching"), SearchResult.TotalMatching);
		Payload->SetNumberField(
			TEXT("next_cursor"), bHasMore ? EndIndex : -1);
		TArray<TSharedPtr<FJsonValue>> Candidates;
		for (int32 Index = StartIndex; Index < EndIndex; ++Index)
		{
			const UeAgentPaletteSearch::FCandidate& Match =
				SearchResult.MatchingCandidates[Index];
			const TSharedPtr<FJsonObject> Candidate = MakeShared<FJsonObject>();
			Candidate->SetStringField(
				TEXT("name"),
				Match.Name.IsEmpty() ? Match.DisplayName : Match.Name);
			Candidate->SetStringField(TEXT("id"), Match.GetPublicId());
			Candidate->SetStringField(
				TEXT("display_name"),
				Match.DisplayName.IsEmpty() ? Match.Name : Match.DisplayName);
			Candidate->SetStringField(TEXT("kind"), Match.Kind);
			if (!Match.CategoryPath.IsEmpty())
			{
				Candidate->SetStringField(
					TEXT("category"), Match.CategoryPath);
			}
			Candidates.Add(MakeShared<FJsonValueObject>(Candidate));
		}
		Payload->SetArrayField(TEXT("candidates"), MoveTemp(Candidates));
		OutPayloadJson = Canonical(Payload);
		return true;
	}

	bool TryQueryBlueprintNodeCandidates(
		const FString& Target,
		const FString& GraphId,
		const FString& Query,
		FString& OutPayloadJson,
		FUeAgentBlueprintNodeMechanicsError& OutError)
	{
		FUeAgentBlueprintNodeCandidateQuery Request;
		Request.Target = Target;
		Request.GraphId = GraphId;
		Request.Query = Query;
		return TryQueryBlueprintNodeCandidates(
			Request, OutPayloadJson, OutError);
	}

	bool TryValidateLegacyBlueprintNodeCandidateBinding(
		const FString& Target,
		const FString& GraphId,
		const FString& Name,
		const FString& CandidateId,
		const FString& BindingRevisionValue,
		FUeAgentBlueprintNodeMechanicsError& OutError)
	{
		OutError = {};
		UBlueprint* Blueprint = LoadBlueprint(Normalize(Target), OutError);
		UEdGraph* Graph = Blueprint
			? FindGraph(*Blueprint, Normalize(GraphId))
			: nullptr;
		if (!Blueprint)
		{
			return false;
		}
		if (!Graph)
		{
			return Fail(
				OutError,
				TEXT("blueprint_node_graph_unresolved"),
				GraphId);
		}

		UeAgentPaletteSearch::FSearchParams Params;
		Params.CatalogType = TEXT("blueprint_node");
		Params.AssetPath = Blueprint->GetPathName();
		Params.GraphName = Graph->GetName();
		Params.bContextSensitive = true;
		UeAgentPaletteSearch::FCandidate Candidate;
		FString ResolveError;
		if (!UeAgentPaletteSearch::TryResolveBlueprintActionCandidate(
				Params, Name, CandidateId, Candidate, ResolveError))
		{
			return Fail(
				OutError,
				ResolveError.IsEmpty()
					? FString(TEXT("blueprint_action_candidate_not_found"))
					: ResolveError,
				Name);
		}
		const FString CurrentBinding = BindingRevision(
			Target, GraphId, Name, GraphRevision(*Graph));
		if (CurrentBinding != BindingRevisionValue)
		{
			return Fail(
				OutError,
				TEXT("blueprint_surface_node_candidate_binding_stale"),
				Name);
		}
		return true;
	}

}
