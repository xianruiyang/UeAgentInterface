#include "DomainAdapters/Blueprint/Surfaces/UeAgentBlueprintCandidateBindingNormalizer.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "DomainAdapters/Blueprint/Node/UeAgentBlueprintNodeMechanics.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringIdentity.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/ScopeExit.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

namespace UE::UAI::DomainAdapters::Blueprint::Surfaces
{
	using namespace HjsonWorkspace;

	namespace
	{
		FString EscapeBindingKey(FString Key)
		{
			return Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		}

		TSharedPtr<FJsonValue> BindingFieldValue(const TSharedPtr<FJsonObject>& Object, const FString& Field)
		{
			if (!Object.IsValid()) return nullptr;
			const auto* Value = Object->Values.Find(Field);
			if (!Value || !Value->IsValid()) return nullptr;
			if ((*Value)->Type != EJson::Object && (*Value)->Type != EJson::Array) return *Value;
			auto Type = MakeShared<FJsonObject>();
			Type->SetStringField(TEXT("type"), (*Value)->Type == EJson::Object ? TEXT("object") : TEXT("array"));
			return MakeShared<FJsonValueObject>(Type);
		}

		UE::UAI::Contract::FDiagnostic BindingFact(const FString& Code, const FString& Reason,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("candidate_resolve"));
			Diagnostic.ActualValuePresent = Actual.IsValid();
			Diagnostic.ActualValue = Actual.IsValid() ? MoveTemp(Actual) : MakeShared<FJsonValueNull>();
			Diagnostic.ExpectedValue = MoveTemp(Expected);
			return Diagnostic;
		}

		void BindInputSource(UE::UAI::Contract::FDiagnostic& Diagnostic, const FJsonObject& Object, const FString& Prefix)
		{
			if (Diagnostic.SourceKind == TEXT("command_input")) return;
			const FString& Pointer = Diagnostic.PropertyPath;
			const FString Relative = Pointer == Prefix ? TEXT("/")
				: Pointer.StartsWith(Prefix + TEXT("/")) ? Pointer.Mid(Prefix.Len()) : TEXT("/");
			if (Diagnostic.SourceKind == TEXT("native_result"))
			{
				// Preserve the native cause and add only a proven authored context.
				for (const auto& Related : Diagnostic.RelatedSources)
				{
					const TSharedPtr<FJsonObject>* Source = nullptr;
					FString File;
					if (Related.IsValid() && Related->TryGetObject(Source) && Source && Source->IsValid()
						&& (*Source)->TryGetStringField(TEXT("source_file"), File) && !File.IsEmpty()) return;
				}
				UE::UAI::Contract::FDiagnostic InputLocation;
				InputLocation.Source.InternalJsonPath = Relative;
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(InputLocation, Object);
				const auto& Source = InputLocation.Source;
				if (Source.SourceFile.IsEmpty()) return;
				auto Related = MakeShared<FJsonObject>();
				Related->SetStringField(TEXT("source_kind"), TEXT("related_input"));
				Related->SetStringField(TEXT("source_file"), Source.SourceFile);
				if (!Source.JsonPath.IsEmpty()) Related->SetStringField(TEXT("json_pointer"), Source.JsonPath);
				if (Source.Line > 0) Related->SetNumberField(TEXT("line"), Source.Line);
				if (Source.Column > 0) Related->SetNumberField(TEXT("column"), Source.Column);
				Diagnostic.RelatedSources.Add(MakeShared<FJsonValueObject>(Related));
				return;
			}
			if (Diagnostic.Source.SourceFile.IsEmpty() && Diagnostic.Source.InternalJsonPath.IsEmpty())
			{
				Diagnostic.Source.InternalJsonPath = Relative;
			}
			UeAgentAuthoringDiagnostics::LocateAuthoringSource(Diagnostic, Object);
		}

		bool Fail(FWorkspaceError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic,
			const FString& TargetIdentity, const FString& Pointer)
		{
			if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = TargetIdentity;
			if (Diagnostic.PropertyPath.IsEmpty()) Diagnostic.PropertyPath = Pointer;
			if (Diagnostic.SemanticPointer.IsEmpty()) Diagnostic.SemanticPointer = Pointer;
			OutError.SetFailure(MoveTemp(Diagnostic));
			return false;
		}

		bool TryGetRequiredString(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			FString& OutValue,
			const FString& TargetIdentity,
			const FString& Pointer,
			FWorkspaceError& OutError)
		{
			if (!Object.IsValid()
				|| !Object->TryGetStringField(Field, OutValue)
				|| OutValue.IsEmpty())
			{
				return Fail(OutError, BindingFact(TEXT("blueprint_surface_candidate_field_required"),
					FString::Printf(TEXT("Candidate field '%s' must be a nonempty string."), Field),
					BindingFieldValue(Object, Field), MakeShared<FJsonValueString>(TEXT("nonempty string"))), TargetIdentity, Pointer + TEXT("/") + EscapeBindingKey(Field));
			}
			return true;
		}

		bool TrySetExactString(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			const FString& Expected,
			const FString& TargetIdentity,
			const FString& Pointer,
			FWorkspaceError& OutError)
		{
			FString Existing;
			if (Object->HasField(Field)
				&& (!Object->TryGetStringField(Field, Existing)
					|| Existing != Expected))
			{
				return Fail(OutError, BindingFact(TEXT("blueprint_surface_candidate_normalization_conflict"),
					FString::Printf(TEXT("Field '%s' conflicts with the resolved candidate."), Field),
					BindingFieldValue(Object, Field), MakeShared<FJsonValueString>(Expected)), TargetIdentity, Pointer + TEXT("/") + EscapeBindingKey(Field));
			}
			Object->SetStringField(Field, Expected);
			return true;
		}

		bool TryValidateOptionalCandidateField(
			const TSharedPtr<FJsonObject>& Candidate,
			const TCHAR* Field,
			const FString& Expected,
			const FString& TargetIdentity,
			const FString& Pointer,
			FWorkspaceError& OutError)
		{
			FString Actual;
			if (!Candidate->HasField(Field))
			{
				return true;
			}
			if (!Candidate->TryGetStringField(Field, Actual)
				|| !Actual.Equals(Expected, ESearchCase::CaseSensitive))
			{
				return Fail(OutError, BindingFact(TEXT("blueprint_surface_component_candidate_metadata_mismatch"),
					FString::Printf(TEXT("Candidate metadata '%s' must exactly match the resolved component metadata."), Field),
					BindingFieldValue(Candidate, Field), MakeShared<FJsonValueString>(Expected)), TargetIdentity, Pointer + TEXT("/") + EscapeBindingKey(Field));
			}
			return true;
		}

		bool TryNormalizeComponentCandidates(
			const FString& TargetIdentity,
			const TSharedPtr<FJsonObject>& ProviderExtension,
			FWorkspaceError& OutError)
		{
			const TSharedPtr<FJsonObject>* Components = nullptr;
			if (!ProviderExtension->HasField(TEXT("components")))
			{
				return true;
			}
			if (!ProviderExtension->TryGetObjectField(
					TEXT("components"), Components)
				|| !Components || !Components->IsValid())
			{
				return Fail(OutError, BindingFact(TEXT("blueprint_surface_components_invalid"), TEXT("components must be an object map."),
					BindingFieldValue(ProviderExtension, TEXT("components")), MakeShared<FJsonValueString>(TEXT("object"))), TargetIdentity, TEXT("/root/document/provider_extension/components"));
			}

			for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry
				: (*Components)->Values)
			{
				const TSharedPtr<FJsonObject> Spec = Entry.Value.IsValid()
					? Entry.Value->AsObject()
					: nullptr;
				const FString Pointer =
					TEXT("/root/document/provider_extension/components/")
					+ EscapeBindingKey(Entry.Key);
				if (!Spec.IsValid())
				{
					return Fail(OutError, BindingFact(TEXT("blueprint_surface_component_spec_invalid"), TEXT("The component definition must be an object."),
						BindingFieldValue(*Components, Entry.Key),
						MakeShared<FJsonValueString>(TEXT("object"))), TargetIdentity, Pointer);
				}
				ON_SCOPE_EXIT
				{
					if (!OutError.HasErrors()) return;
					auto Diagnostic = OutError.GetDiagnostic();
					BindInputSource(Diagnostic, *Spec, Pointer);
					OutError.SetFailure(MoveTemp(Diagnostic));
				};
				const TSharedPtr<FJsonObject>* CandidateField = nullptr;
				if (!Spec->HasField(TEXT("candidate")))
				{
					continue;
				}
				if (!Spec->TryGetObjectField(
						TEXT("candidate"), CandidateField)
					|| !CandidateField || !CandidateField->IsValid())
				{
					return Fail(OutError, BindingFact(TEXT("blueprint_surface_component_candidate_invalid"), TEXT("The component candidate must be an object."),
						BindingFieldValue(Spec, TEXT("candidate")), MakeShared<FJsonValueString>(TEXT("object"))), TargetIdentity, Pointer + TEXT("/candidate"));
				}
				const TSharedPtr<FJsonObject> Candidate = CandidateField
					? *CandidateField
					: nullptr;
				DomainAuthoring::FDomainHjsonCandidateSelector Selector;
				if (!DomainAuthoring::TryReadCandidateSelector(
					Candidate, Pointer + TEXT("/candidate"), Selector, OutError))
				{
					return false;
				}
				UeAgentPaletteSearch::FCandidate ResolvedCandidate;
				UE::UAI::Contract::FDiagnostic ResolveError;
				if (!UeAgentPaletteSearch::TryResolveActorComponentCandidate(
					Selector.Name,
					FString(),
						ResolvedCandidate,
						ResolveError, Selector.Select))
				{
					auto Diagnostic = UeAgentAuthoringDiagnostics::CandidateFailure(
                        ResolveError.IsEmpty() ? TEXT("blueprint_surface_component_candidate_query_failed") : ResolveError.Code,
                        Selector.Name, TargetIdentity, TEXT("component palette"),
                        TEXT("blueprint.component.candidates"), ResolveError);
                    Diagnostic.PropertyPath = Pointer + TEXT("/candidate");
                    BindInputSource(Diagnostic, *Spec, Pointer);
                    OutError.SetFailure(MoveTemp(Diagnostic));
					return false;
				}
				const FString* ResolvedClass =
					ResolvedCandidate.AuthoringSelectors.Find(TEXT("component_class"));
				if (!ResolvedClass || ResolvedClass->IsEmpty())
				{
					auto Diagnostic = BindingFact(TEXT("blueprint_surface_component_candidate_class_missing"), TEXT("The resolved component candidate has no component_class selector."),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					Diagnostic.CandidateName = Selector.Name;
					return Fail(OutError, MoveTemp(Diagnostic), TargetIdentity, Pointer + TEXT("/candidate"));
				}
				if (!TryValidateOptionalCandidateField(
						Candidate, TEXT("class_path"), *ResolvedClass,
						TargetIdentity, Pointer + TEXT("/candidate"), OutError)
					|| !TryValidateOptionalCandidateField(
						Candidate, TEXT("component_class"), *ResolvedClass,
						TargetIdentity, Pointer + TEXT("/candidate"), OutError))
				{
					return false;
				}
				const FString ResolvedAsset =
					ResolvedCandidate.AuthoringSelectors.FindRef(TEXT("asset_override"));
				if ((Candidate->HasField(TEXT("asset_path"))
						&& !TryValidateOptionalCandidateField(
							Candidate, TEXT("asset_path"), ResolvedAsset,
							TargetIdentity, Pointer + TEXT("/candidate"), OutError))
					|| (Candidate->HasField(TEXT("asset_override"))
						&& !TryValidateOptionalCandidateField(
							Candidate, TEXT("asset_override"), ResolvedAsset,
							TargetIdentity, Pointer + TEXT("/candidate"), OutError)))
				{
					return false;
				}
				if (!Candidate->HasField(TEXT("name")))
				{
					Candidate->SetStringField(
						TEXT("name"), ResolvedCandidate.Name);
				}
				if (!TrySetExactString(
						Spec, TEXT("class"), *ResolvedClass,
						TargetIdentity, Pointer, OutError))
				{
					return false;
				}
			}
			return true;
		}

		bool TryNormalizeNodeCandidates(
			const FString& TargetIdentity,
			const TSharedPtr<FJsonObject>& ProviderExtension,
			FWorkspaceError& OutError)
		{
			UeAgentPaletteSearch::FBlueprintActionBatchScope Batch;
			const TSharedPtr<FJsonObject>* Graphs = nullptr;
			if (!ProviderExtension->HasField(TEXT("graphs")))
			{
				return true;
			}
			if (!ProviderExtension->TryGetObjectField(TEXT("graphs"), Graphs)
				|| !Graphs || !Graphs->IsValid())
			{
				return Fail(OutError, BindingFact(TEXT("blueprint_surface_graphs_invalid"), TEXT("graphs must be an object map."),
					BindingFieldValue(ProviderExtension, TEXT("graphs")), MakeShared<FJsonValueString>(TEXT("object"))), TargetIdentity, TEXT("/root/document/provider_extension/graphs"));
			}

			for (const TPair<FString, TSharedPtr<FJsonValue>>& GraphEntry
				: (*Graphs)->Values)
			{
				const TSharedPtr<FJsonObject> GraphSpec = GraphEntry.Value.IsValid()
					? GraphEntry.Value->AsObject()
					: nullptr;
				const FString GraphPointer =
					TEXT("/root/document/provider_extension/graphs/")
					+ EscapeBindingKey(GraphEntry.Key);
				if (!GraphSpec.IsValid())
				{
					return Fail(OutError, BindingFact(TEXT("blueprint_surface_graph_spec_invalid"), TEXT("The graph definition must be an object."),
						BindingFieldValue(*Graphs, GraphEntry.Key), MakeShared<FJsonValueString>(TEXT("object"))), TargetIdentity, GraphPointer);
				}
				ON_SCOPE_EXIT
				{
					if (!OutError.HasErrors()) return;
					auto Diagnostic = OutError.GetDiagnostic();
					BindInputSource(Diagnostic, *GraphSpec, GraphPointer);
					OutError.SetFailure(MoveTemp(Diagnostic));
				};
				const TSharedPtr<FJsonObject>* Nodes = nullptr;
				if (!GraphSpec->HasField(TEXT("nodes"))) continue;
				if (!GraphSpec->TryGetObjectField(TEXT("nodes"), Nodes)
					|| !Nodes || !Nodes->IsValid())
				{
					return Fail(OutError, BindingFact(TEXT("blueprint_surface_nodes_invalid"), TEXT("The graph's nodes must be an object map."),
						BindingFieldValue(GraphSpec, TEXT("nodes")), MakeShared<FJsonValueString>(TEXT("object"))), TargetIdentity, GraphPointer + TEXT("/nodes"));
				}
				TArray<TPair<FString, TSharedPtr<FJsonObject>>> CandidateNodes;
				TSet<FString> PendingNodeIds;
				for (const auto& NodeEntry : (*Nodes)->Values)
				{
					if (!NodeEntry.Value.IsValid() || NodeEntry.Value->Type != EJson::Object)
					{
						return Fail(OutError, BindingFact(TEXT("blueprint_surface_node_spec_invalid"), TEXT("The node definition must be an object."),
							BindingFieldValue(*Nodes, NodeEntry.Key), MakeShared<FJsonValueString>(TEXT("object"))), TargetIdentity, GraphPointer + TEXT("/nodes/") + EscapeBindingKey(NodeEntry.Key));
					}
					const TSharedPtr<FJsonObject> Spec = NodeEntry.Value->AsObject();
					FString Id = NodeEntry.Key, Operation;
					Spec->TryGetStringField(TEXT("id"), Id);
					Spec->TryGetStringField(TEXT("operation"), Operation);
					if (!Operation.Equals(TEXT("remove"), ESearchCase::IgnoreCase))
						PendingNodeIds.Add(Id);
					if (Spec->HasField(TEXT("candidate"))) CandidateNodes.Emplace(NodeEntry.Key, Spec);
				}
				// Ordinary GUID-bound node/property updates need no candidate context.
				if (CandidateNodes.IsEmpty()) continue;
				FString GraphSelector;
				const TSharedPtr<FJsonObject>* GraphHeader = nullptr;
				if (GraphSpec->TryGetObjectField(TEXT("graph"), GraphHeader)
					&& GraphHeader && GraphHeader->IsValid())
				{
					FString CapturedGuidText;
					FGuid CapturedGuid;
					if ((*GraphHeader)->TryGetStringField(TEXT("graph_guid"), CapturedGuidText)
						&& FGuid::Parse(CapturedGuidText, CapturedGuid) && CapturedGuid.IsValid())
						GraphSelector = TEXT("graph:") + CapturedGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
					else
						(*GraphHeader)->TryGetStringField(TEXT("name"), GraphSelector);
				}
				else
				{
					GraphSpec->TryGetStringField(TEXT("name"), GraphSelector);
				}
				if (GraphSelector.IsEmpty())
				{
					FGuid OpaqueGuid;
					if (!FGuid::Parse(GraphEntry.Key, OpaqueGuid))
					{
						int32 Separator = INDEX_NONE;
						GraphSelector = GraphEntry.Key.FindChar(
							TEXT(':'), Separator)
							? GraphEntry.Key.Mid(Separator + 1)
							: GraphEntry.Key;
					}
				}
				GraphSelector.TrimStartAndEndInline();
				if (GraphSelector.IsEmpty())
				{
					return Fail(OutError, BindingFact(TEXT("blueprint_surface_node_candidate_graph_required"), TEXT("Candidate nodes require a graph name or valid graph selector."),
						MakeShared<FJsonValueString>(GraphSelector), MakeShared<FJsonValueString>(TEXT("nonempty graph selector"))), TargetIdentity, GraphPointer);
				}
				FString ResolvedGraphId;
				FString ResolvedGraphName;
				UE::UAI::Contract::FDiagnostic GraphResolveError;
				if (!UeAgentPaletteSearch::TryResolveBlueprintGraphIdentity(
						TargetIdentity,
						GraphSelector,
						ResolvedGraphId,
						ResolvedGraphName,
						GraphResolveError))
				{
					return Fail(OutError, MoveTemp(GraphResolveError), TargetIdentity, GraphPointer);
				}
				UBlueprint* ResolvedBlueprint = LoadObject<UBlueprint>(
					nullptr, *TargetIdentity);
				bool bDeclaresNewVariables = false;
				const TSharedPtr<FJsonObject>* Members = nullptr;
				const TSharedPtr<FJsonObject>* Variables = nullptr;
				if (ResolvedBlueprint
					&& ProviderExtension->TryGetObjectField(TEXT("members"), Members)
					&& Members && (*Members)->TryGetObjectField(TEXT("variables"), Variables)
					&& Variables)
				{
					for (const auto& Variable : (*Variables)->Values)
					{
						if (!Variable.Value.IsValid() || Variable.Value->Type != EJson::Object) continue;
						const auto Definition = Variable.Value->AsObject();
						FString Operation, Name = Variable.Key;
						Definition->TryGetStringField(TEXT("operation"), Operation);
						Definition->TryGetStringField(TEXT("name"), Name);
						if (!Operation.Equals(TEXT("remove"), ESearchCase::IgnoreCase)
							&& FBlueprintEditorUtils::FindNewVariableIndex(ResolvedBlueprint, FName(*Name)) == INDEX_NONE)
						{
							bDeclaresNewVariables = true;
							break;
						}
					}
				}
				TArray<UEdGraph*> ResolvedGraphs;
				if (ResolvedBlueprint)
				{
					TArray<UEdGraph*> AllGraphs;
					ResolvedBlueprint->GetAllGraphs(AllGraphs);
					for (UEdGraph* Graph : AllGraphs)
					{
						if (Graph && UeAgentBlueprintAuthoring::GraphIdentity(*Graph) == ResolvedGraphId)
						{
							ResolvedGraphs.Add(Graph);
						}
					}
				}
				if (ResolvedGraphs.Num() != 1)
				{
					auto Diagnostic = BindingFact(ResolvedGraphs.IsEmpty() ? TEXT("blueprint_surface_graph_not_found") : TEXT("blueprint_surface_graph_ambiguous"),
						TEXT("The resolved graph selector must identify exactly one native graph."),
						MakeShared<FJsonValueNumber>(ResolvedGraphs.Num()), MakeShared<FJsonValueNumber>(1));
					Diagnostic.CandidateName = GraphSelector;
					Diagnostic.SourceKind = TEXT("native_result");
					return Fail(OutError, MoveTemp(Diagnostic), TargetIdentity, GraphPointer);
				}
				GraphSelector = ResolvedGraphId;
				for (const auto& NodeEntry : CandidateNodes)
				{
					const TSharedPtr<FJsonObject>& Spec = NodeEntry.Value;
					ON_SCOPE_EXIT
					{
						if (OutError.GetCode().IsEmpty()) return;
						auto Diagnostic = OutError.GetDiagnostic();
                        BindInputSource(Diagnostic, *Spec, GraphPointer + TEXT("/nodes/") + EscapeBindingKey(NodeEntry.Key));
                        OutError.SetFailure(MoveTemp(Diagnostic));
					};
					const FString Pointer = GraphPointer + TEXT("/nodes/")
						+ EscapeBindingKey(NodeEntry.Key);
					const TSharedPtr<FJsonObject>* CandidateField = nullptr;
					if (!Spec->TryGetObjectField(
							TEXT("candidate"), CandidateField)
						|| !CandidateField || !CandidateField->IsValid())
					{
						return Fail(OutError, BindingFact(TEXT("blueprint_surface_node_candidate_invalid"), TEXT("The node candidate must be an object."),
							BindingFieldValue(Spec, TEXT("candidate")), MakeShared<FJsonValueString>(TEXT("object"))), TargetIdentity, Pointer + TEXT("/candidate"));
					}
					const TSharedPtr<FJsonObject> Candidate = CandidateField
						? *CandidateField
						: nullptr;
					DomainAuthoring::FDomainHjsonCandidateSelector Selector;
					if (!DomainAuthoring::TryReadCandidateSelector(
							Candidate,
							Pointer + TEXT("/candidate"),
							Selector,
							OutError))
					{
						auto CandidateDiagnostic = OutError.GetDiagnostic();
                        BindInputSource(CandidateDiagnostic, *Spec, Pointer);
                        OutError.SetFailure(MoveTemp(CandidateDiagnostic));
						return false;
					}

					UeAgentPaletteSearch::FSearchParams Params;
					Params.CatalogType = TEXT("blueprint_node");
					Params.AssetPath = TargetIdentity;
					Params.GraphName = GraphSelector;
					Params.bContextSensitive = true;
					Params.CandidateSelect = Selector.Select;
					const TSharedPtr<FJsonObject>* CandidateContext = nullptr;
					if (Spec->HasField(TEXT("candidate_context")))
					{
						if (!Spec->TryGetObjectField(
								TEXT("candidate_context"), CandidateContext)
							|| !CandidateContext
							|| !CandidateContext->IsValid())
						{
							return Fail(OutError, BindingFact(TEXT("blueprint_surface_candidate_context_invalid"), TEXT("candidate_context must be an object."),
								BindingFieldValue(Spec, TEXT("candidate_context")), MakeShared<FJsonValueString>(TEXT("object"))), TargetIdentity, Pointer + TEXT("/candidate_context"));
						}
						for (const TPair<FString, TSharedPtr<FJsonValue>>& Field :
							(*CandidateContext)->Values)
						{
							if (Field.Key != TEXT("node")
								&& Field.Key != TEXT("alias")
								&& Field.Key != TEXT("pin"))
							{
								return Fail(OutError, BindingFact(TEXT("blueprint_surface_candidate_context_field_unsupported"), TEXT("candidate_context supports only node, alias and pin fields."),
									MakeShared<FJsonValueString>(Field.Key), MakeShared<FJsonValueString>(TEXT("node | alias | pin"))), TargetIdentity, Pointer + TEXT("/candidate_context/") + EscapeBindingKey(Field.Key));
							}
						}
						FString NodeIdentity;
						FString PinAlias;
						if (!(*CandidateContext)->TryGetStringField(
								TEXT("node"), NodeIdentity)
							|| !(*CandidateContext)->TryGetStringField(
								TEXT("pin"), PinAlias))
						{
							const bool bNodeInvalid = !(*CandidateContext)->TryGetStringField(TEXT("node"), NodeIdentity);
							const FString Field = bNodeInvalid ? TEXT("node") : TEXT("pin");
							return Fail(OutError, BindingFact(TEXT("blueprint_surface_candidate_context_invalid"),
								FString::Printf(TEXT("candidate_context.%s must be a string."), *Field), BindingFieldValue(*CandidateContext, Field),
								MakeShared<FJsonValueString>(TEXT("string"))), TargetIdentity, Pointer + TEXT("/candidate_context/") + Field);
						}
						// Provider map keys are presentation keys; explicit ids are the
						// mutation owner's producer identities. Resolve pending interfaces
						// only after those producers have been created or updated.
						if (PendingNodeIds.Contains(NodeIdentity)) continue;
						FGuid NodeGuid;
						if (!FGuid::Parse(NodeIdentity, NodeGuid))
						{
							return Fail(OutError, BindingFact(TEXT("blueprint_surface_candidate_context_node_unresolved"), TEXT("The context node must be a pending producer identity or a native node GUID."),
								MakeShared<FJsonValueString>(NodeIdentity), MakeShared<FJsonValueString>(TEXT("pending node identity or GUID"))), TargetIdentity, Pointer + TEXT("/candidate_context/node"));
						}
						UEdGraphNode* ContextNode = nullptr;
						for (UEdGraphNode* Node : ResolvedGraphs[0]->Nodes)
						{
							if (Node && Node->NodeGuid == NodeGuid)
							{
								ContextNode = Node;
								break;
							}
						}
						if (!ContextNode)
						{
							return Fail(OutError, BindingFact(TEXT("blueprint_surface_node_context_invalid"), TEXT("The context node GUID is absent from the selected graph."),
								MakeShared<FJsonValueString>(NodeIdentity), MakeShared<FJsonValueString>(TEXT("node in the selected graph"))), TargetIdentity, Pointer + TEXT("/candidate_context/node"));
						}
						TMap<FString, UEdGraphPin*> PinAliases;
						UE::UAI::Contract::FDiagnostic PinAliasError;
						if (!UeAgentBlueprintAuthoring::BuildPinAliasLookup(
							*ContextNode, PinAliases, PinAliasError))
							return Fail(OutError, PinAliasError, TargetIdentity,
								Pointer + TEXT("/candidate_context/pin"));
						UEdGraphPin* ContextPin = PinAliases.FindRef(PinAlias);
						if (!ContextPin)
						{
							auto Choices = MakeShared<FJsonObject>();
							for (const auto& Alias : PinAliases) Choices->SetBoolField(Alias.Key, true);
							auto Diagnostic = BindingFact(TEXT("blueprint_surface_pin_context_invalid"), TEXT("The context pin alias is absent from the selected node."),
								MakeShared<FJsonValueString>(PinAlias), MakeShared<FJsonValueString>(TEXT("alias from the selected node")));
							Diagnostic.CandidateChoices = MakeShared<FJsonValueObject>(Choices);
							return Fail(OutError, MoveTemp(Diagnostic), TargetIdentity, Pointer + TEXT("/candidate_context/pin"));
						}
						Params.NodeGuid = ContextNode->NodeGuid.ToString(
							EGuidFormats::DigitsWithHyphensLower);
						Params.PinGuid = ContextPin->PinId.ToString(
							EGuidFormats::DigitsWithHyphensLower);
						Params.PinName = ContextPin->PinName.ToString();
					}
					UeAgentPaletteSearch::FCandidate ResolvedCandidate;
					UE::UAI::Contract::FDiagnostic ResolveError;
					if (!UeAgentPaletteSearch::TryResolveBlueprintActionCandidate(
							Params,
							Selector.Name,
							FString(),
							ResolvedCandidate,
							ResolveError))
					{
						auto CandidateDiagnostic = ResolveError;
                        const FString& ErrorCode = CandidateDiagnostic.Code;
						if (ErrorCode == TEXT("blueprint_action_candidate_not_found")
							&& bDeclaresNewVariables)
						{
							// The old asset's catalog cannot decide a name against the
							// pending member interface. Keep the name and all selectors for
							// the common mutation preflight: members, compile/refresh,
							// then exact native action resolution on the scratch graph.
							// Unknown names still fail there before any persistent apply.
							if (!TrySetExactString(
									Spec, TEXT("node_type"), TEXT("native_action"),
									TargetIdentity, Pointer, OutError))
							{
								return false;
							}
							continue;
						}
                        if (CandidateDiagnostic.IsEmpty())
                            CandidateDiagnostic = UE::UAI::Contract::FDiagnostic::Failure(
                                TEXT("blueprint_surface_node_candidate_query_failed"),
                                TEXT("The requested Blueprint node candidate could not be resolved."), TEXT("candidate_resolve"));
                        if (CandidateDiagnostic.PropertyPath.IsEmpty())
                            CandidateDiagnostic.PropertyPath = Pointer + TEXT("/candidate");
                        if (CandidateDiagnostic.TargetPath.IsEmpty()) CandidateDiagnostic.TargetPath = TargetIdentity;
                        BindInputSource(CandidateDiagnostic, *Spec, Pointer);
                        OutError.SetFailure(MoveTemp(CandidateDiagnostic));
                        return false;
                    }

					if (!TrySetExactString(
							Spec,
							TEXT("node_type"),
							ResolvedCandidate.Kind.Equals(
								TEXT("blueprint_component_event"),
								ESearchCase::CaseSensitive)
								? TEXT("component_bound_event")
								: TEXT("native_action"),
							TargetIdentity,
							Pointer,
							OutError))
					{
						return false;
					}
					if (ResolvedCandidate.Kind.Equals(
							TEXT("blueprint_component_event"),
						ESearchCase::CaseSensitive))
					{
						const FString ComponentName = ResolvedCandidate.AuthoringSelectors.FindRef(
							TEXT("component_name"));
						const FString DelegateName = ResolvedCandidate.AuthoringSelectors.FindRef(
							TEXT("delegate_property_name"));
						if (ComponentName.IsEmpty() || DelegateName.IsEmpty())
						{
							auto Actual = MakeShared<FJsonObject>();
							Actual->SetStringField(TEXT("component_name"), ComponentName);
							Actual->SetStringField(TEXT("delegate_property_name"), DelegateName);
							auto Diagnostic = BindingFact(TEXT("blueprint_surface_component_event_identity_missing"), TEXT("The resolved component event requires both component_name and delegate_property_name."),
								MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("nonempty component and delegate names")));
							Diagnostic.CandidateName = Selector.Name;
							return Fail(OutError, MoveTemp(Diagnostic), TargetIdentity, Pointer + TEXT("/candidate"));
						}
						if (!TrySetExactString(
								Spec, TEXT("component_name"), ComponentName,
								TargetIdentity, Pointer, OutError)
							|| !TrySetExactString(
								Spec, TEXT("delegate_property_name"), DelegateName,
								TargetIdentity, Pointer, OutError))
						{
							return false;
						}
						const FString DelegateOwnerClass = ResolvedCandidate.AuthoringSelectors.FindRef(
							TEXT("delegate_owner_class"));
						if (!DelegateOwnerClass.IsEmpty()
							&& !TrySetExactString(
								Spec, TEXT("delegate_owner_class"), DelegateOwnerClass,
								TargetIdentity, Pointer, OutError))
						{
							return false;
						}
					}
				}
			}
			return true;
		}
	}

	bool TryNormalizeBlueprintCandidateBindings(
		const FString& TargetIdentity,
		const TSharedPtr<FJsonObject>& ProviderExtension,
		FWorkspaceError& OutError)
	{
		if (!ProviderExtension.IsValid())
		{
			return Fail(OutError, BindingFact(TEXT("blueprint_surface_provider_extension_invalid"), TEXT("Blueprint candidate normalization requires a provider extension object."),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), TargetIdentity, TEXT("/root/document/provider_extension"));
		}
		return TryNormalizeComponentCandidates(
				TargetIdentity, ProviderExtension, OutError)
			&& TryNormalizeNodeCandidates(
				TargetIdentity, ProviderExtension, OutError);
	}
}
