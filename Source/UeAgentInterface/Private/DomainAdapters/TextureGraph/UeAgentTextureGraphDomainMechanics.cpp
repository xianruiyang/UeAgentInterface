#include "DomainAdapters/TextureGraph/UeAgentTextureGraphDomainMechanics.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Core/ContentDiscovery/UeAgentReadOnlyCandidateBatch.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Core/HjsonWorkspace/UeAgentSemanticWorkspaceIO.h"

#include "Composition/HjsonImport/UeAgentSemanticWorkspaceImport.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringRuntimeResolvers.h"
#include "DomainAdapters/TextureGraph/UeAgentTextureGraphAuthoringApply.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/DomainCapability/OfficialPluginAvailability/UeAgentOfficialPluginAvailability.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Core/HjsonWorkspace/UeAgentSnapshotComposition.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadNative.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeExit.h"
#include "Containers/UnrealString.h"
#include "Misc/PackageName.h"
#include "OperationOwners/TextureGraph/UeAgentTextureGraphOperationOwner.h"
#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphTextureGraphNativeHook.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UObjectGlobals.h"
#include "Expressions/TG_Expression.h"
#include "Expressions/Output/TG_Expression_Output.h"
#include "TextureGraph.h"
#include "TG_Graph.h"
#include "TG_Node.h"

namespace UE::UAI::DomainAdapters::TextureGraph
{
	using namespace CommandContract;
	using namespace ContextResource;
	using namespace DomainAdapters::ContentGraphRead;
	using namespace HjsonWorkspace;
	using namespace HjsonWorkspace::DomainAuthoring;
	using namespace DomainAdapters::AudioMediaGraph;

	namespace
	{
		const FString SchemaId = GetSemanticWorkspaceSchemaId();
		const FString TextureGraphType = TEXT("/Script/TextureGraph.TextureGraph");

		class FNeverCancelled final : public Contract::IUeAgentCancellationSignal
		{
		public:
			bool IsCancelled() const override { return false; }
		};

		TSharedPtr<FJsonValue> TextureDomainValue(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value) return MakeShared<FJsonValueString>(TEXT("<missing>"));
			if (Value->Type != EJson::Object && Value->Type != EJson::Array) return Value;
			auto Shape = MakeShared<FJsonObject>();
			Shape->SetStringField(TEXT("type"), Value->Type == EJson::Object ? TEXT("object") : TEXT("array"));
			Shape->SetNumberField(TEXT("count"), Value->Type == EJson::Object
				? (Value->AsObject() ? Value->AsObject()->Values.Num() : 0) : Value->AsArray().Num());
			return MakeShared<FJsonValueObject>(Shape);
		}

		UE::UAI::Contract::FDiagnostic TextureDomainFact(const FString& Code, const FString& Reason,
			const FString& Pointer, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& SourceKind = FString())
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("authoring_apply"));
			Diagnostic.SourceKind = SourceKind;
			if (SourceKind == TEXT("native_result")) Diagnostic.PropertyPath = Pointer;
			else if (SourceKind == TEXT("command_input")) Diagnostic.Source.JsonPath = Pointer;
			else Diagnostic.SemanticPointer = Pointer;
			Diagnostic.ActualValue = MoveTemp(Actual);
			Diagnostic.ExpectedValue = MoveTemp(Expected);
			return Diagnostic;
		}

		UE::UAI::Contract::FDiagnostic TextureRequiredString(const FString& Code, const FString& Reason,
			const FJsonObject* Object, const TArray<FString>& Fields, const FString& Prefix, bool bNonempty = false,
			const FString& SourceKind = FString())
		{
			for (const auto& Field : Fields)
			{
				FString Value;
				if (!Object || !Object->TryGetStringField(Field, Value) || (bNonempty && Value.IsEmpty()))
					return TextureDomainFact(Code, Reason, Prefix + TEXT("/") + EscapeJsonPointerSegment(Field),
						TextureDomainValue(Object ? Object->TryGetField(Field) : nullptr),
						MakeShared<FJsonValueString>(bNonempty ? TEXT("non-empty string") : TEXT("string")), SourceKind);
			}
			return {};
		}

		void LocateTextureSemanticFailure(FWorkspaceError& Error, const FResolvedSemanticWorkspace* Workspace,
			const FString& UnitId)
		{
			if (!Workspace || Error.GetDiagnostic().IsEmpty()) return;
			auto Diagnostic = Error.GetDiagnostic();
			auto Locate = [&](auto&& Self, UE::UAI::Contract::FDiagnostic& Item) -> void
			{
				if (Item.SourceKind != TEXT("native_result") && Item.SourceKind != TEXT("command_input")
					&& (Item.Source.SourceFile.IsEmpty() || Item.Source.Line <= 0))
				{
					if (Item.Source.SourceUnit.IsEmpty()) Item.Source.SourceUnit = UnitId;
					if (Item.Source.InternalJsonPath.IsEmpty())
						Item.Source.InternalJsonPath = Item.SemanticPointer.IsEmpty() ? Item.Source.JsonPath : Item.SemanticPointer;
					Workspace->LocateDocumentDiagnostic({}, {}, Item, UnitId);
				}
				for (auto& Child : Item.Diagnostics) Self(Self, Child);
				if (Item.PublicCause)
				{
					auto Cause = MakeShared<UE::UAI::Contract::FDiagnostic>(*Item.PublicCause);
					Self(Self, *Cause);
					Item.PublicCause = MoveTemp(Cause);
				}
			};
			Locate(Locate, Diagnostic);
			Error.SetFailure(MoveTemp(Diagnostic));
		}

		bool Fail(FWorkspaceError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic)
		{
			OutError.SetFailure(MoveTemp(Diagnostic));
			return false;
		}


		FString ObjectPathFromPackage(const FString& Target)
		{
			if (Target.Contains(TEXT(".")))
			{
				return Target;
			}
			const FString AssetName =
				FPackageName::GetLongPackageAssetName(Target);
			return AssetName.IsEmpty()
				? Target : Target + TEXT(".") + AssetName;
		}

		bool CanonicalObject(
			const TSharedPtr<FJsonObject>& Object,
			FString& Out,
			FWorkspaceError& OutError)
		{
			UE::UAI::Contract::FDiagnostic Error;
			if (!Json::TryCanonicalize(Object, Out, Error))
				return Fail(OutError, MoveTemp(Error));
			return true;
		}

		bool TryCapture(
			const FString& Target,
			FUeAgentContentGraphCapture& OutCapture,
			FWorkspaceError& OutError)
		{
			UObject* Owner = StaticLoadObject(
				UObject::StaticClass(),
				nullptr,
				*ObjectPathFromPackage(Target));
			if (!Owner)
			{
				{
					auto Diagnostic = TextureDomainFact(TEXT("texture_graph_domain_target_unresolved"),
						TEXT("The TextureGraph target could not be loaded."), TEXT("/target"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("native_result"));
					Diagnostic.TargetPath = Target;
					return Fail(OutError, MoveTemp(Diagnostic));
				}
			}
			if (Owner->GetClass()->GetPathName() != TextureGraphType)
			{
				{
					auto Diagnostic = TextureDomainFact(TEXT("texture_graph_domain_target_type_invalid"),
						TEXT("The loaded target is not a TextureGraph asset."), TEXT("/class"), MakeShared<FJsonValueString>(Owner->GetClass()->GetPathName()), MakeShared<FJsonValueString>(TextureGraphType), TEXT("native_result"));
					Diagnostic.TargetPath = Target;
					return Fail(OutError, MoveTemp(Diagnostic));
				}
			}
			FUeAgentContentGraphReadError CaptureError;
			if (!FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(
					*Owner, OutCapture, CaptureError))
			{
				if (CaptureError.IsEmpty()) CaptureError = TextureDomainFact(TEXT("texture_graph_domain_readback_failed"),
					TEXT("Native TextureGraph capture failed without returning a diagnostic."), TEXT("/capture"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("native_result"));
				if (CaptureError.TargetPath.IsEmpty()) CaptureError.TargetPath = Target;
				return Fail(OutError, MoveTemp(CaptureError));
			}
			return true;
		}

		TSharedPtr<FJsonObject> BuildInternalReadback(
			const FUeAgentContentGraphCapture& Capture)
		{
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Diagnostics;
			for (const FUeAgentContentGraphDiagnostic& Diagnostic
				: Capture.Diagnostics)
			{
				const auto Message = MakeShared<FJsonObject>();
				Message->SetStringField(TEXT("severity"), Diagnostic.Severity);
				Message->SetStringField(TEXT("code"), Diagnostic.Code);
				Message->SetStringField(TEXT("message"), Diagnostic.Message);
				Message->SetBoolField(TEXT("resolved"), Diagnostic.bResolved);
				Diagnostics.Add(MakeShared<FJsonValueObject>(Message));
			}
			Result->SetArrayField(TEXT("diagnostics"), MoveTemp(Diagnostics));
			return Result;
		}

		bool TryInternalReadback(
			const FString& Target,
			FString& OutPayload,
			FString& OutRevision,
			FWorkspaceError& OutError)
		{
			FUeAgentContentGraphCapture Capture;
			if (!TryCapture(Target, Capture, OutError)) return false;
			OutRevision = Capture.Revision;
			return CanonicalObject(
				BuildInternalReadback(Capture), OutPayload, OutError);
		}

		bool TryCanonicalValueToJson(
			const StateSemantic::FUeAgentCanonicalValueRef& Value,
			TSharedPtr<FJsonValue>& OutValue,
			FWorkspaceError& OutError)
		{
			OutValue.Reset();
			UE::UAI::Contract::FDiagnostic ConversionError;
			if (!StateSemantic::CanonicalJson::TryToJsonValue(*Value, OutValue, ConversionError))
				return Fail(OutError, MoveTemp(ConversionError));
			return true;
		}

		bool TryReadCanonicalObjectField(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* Field,
			TSharedPtr<FJsonObject>& OutObject,
			FWorkspaceError& OutError)
		{
			OutObject.Reset();
			const TSharedPtr<FJsonObject>* Value = nullptr;
			if (!Object.IsValid()
				|| !Object->TryGetObjectField(Field, Value)
				|| Value == nullptr || !Value->IsValid())
			{
				{
					auto Diagnostic = TextureDomainFact(TEXT("texture_graph_domain_semantic_graph_shape_invalid"),
						TEXT("The semantic graph field must be an object."), TEXT("/") + FString(Field), TextureDomainValue(Object ? Object->TryGetField(Field) : nullptr), MakeShared<FJsonValueString>(TEXT("object")));
					return Fail(OutError, MoveTemp(Diagnostic));
				}
			}
			OutObject = *Value;
			return true;
		}

		using FExpressionClasses = TArray<TPair<FString, FString>>;
		TSharedPtr<FExpressionClasses> PrepareExpressionClasses(
			ContentDiscovery::FReadOnlyCandidateBatch& Batch, const UObject& Target, bool bPublicNames)
		{
			return ContentDiscovery::FReadOnlyCandidateBatch::Prepare<FExpressionClasses>(
				&Batch, &Target, bPublicNames ? TEXT("candidate") : TEXT("node_type"),
				[bPublicNames](FExpressionClasses& Classes) {
					for (TObjectIterator<UClass> It; It; ++It)
					{
						UClass* Class = *It;
						if (!Class || !Class->IsChildOf(UTG_Expression::StaticClass())) continue;
						if (bPublicNames)
						{
							if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_Hidden)) continue;
							const auto* Default = Class->GetDefaultObject<UTG_Expression>();
							if (Default) Classes.Emplace(Default->GetDefaultName().ToString(), Class->GetPathName());
						}
						else Classes.Emplace(Class->GetName(), Class->GetPathName());
					}
					return true;
				});
		}

		bool TryTextureGraphExpressionClass(
			const FJsonObject& Node,
			const FString* ExistingClassPath,
			FString& OutClass,
			bool& bOutClassChanged,
			FWorkspaceError& OutError,
			ContentDiscovery::FReadOnlyCandidateBatch& Batch, const UObject& Target)
		{
			OutClass.Reset();
			bOutClassChanged = false;
			if (Node.HasField(TEXT("candidate")))
			{
				const TSharedPtr<FJsonObject>* CandidateObject = nullptr;
				if (!Node.TryGetObjectField(TEXT("candidate"), CandidateObject)
					|| !CandidateObject || !CandidateObject->IsValid())
					{
						auto Diagnostic = TextureDomainFact(TEXT("texture_graph_candidate_invalid"),
							TEXT("The TextureGraph node candidate must be an object."), TEXT("/candidate"), TextureDomainValue(Node.TryGetField(TEXT("candidate"))), MakeShared<FJsonValueString>(TEXT("object")));
						return Fail(OutError, MoveTemp(Diagnostic));
					}
				FDomainHjsonCandidateSelector Selector;
				if (!TryReadCandidateSelector(*CandidateObject, TEXT("/candidate"), Selector, OutError))
					return false;
				if (!Selector.Select.IsEmpty())
					{
						auto ActualSelect = MakeShared<FJsonObject>();
						for (const auto& Pair : Selector.Select) ActualSelect->SetStringField(Pair.Key, Pair.Value);
						auto Diagnostic = TextureDomainFact(TEXT("texture_graph_candidate_select_not_supported"),
							TEXT("TextureGraph expression candidates do not accept a select discriminator."), TEXT("/candidate/select"),
							MakeShared<FJsonValueObject>(ActualSelect), MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));
						Diagnostic.CandidateName = Selector.Name;
						return Fail(OutError, MoveTemp(Diagnostic));
					}
				FString Match;
				int32 MatchCount = 0;
				const auto Classes = PrepareExpressionClasses(Batch, Target, true);
				for (const auto& Class : *Classes)
				{
					if (Class.Key.Equals(Selector.Name, ESearchCase::CaseSensitive))
					{ Match = Class.Value; ++MatchCount; }
				}
				if (MatchCount != 1)
					{
						auto Diagnostic = TextureDomainFact(MatchCount == 0 ? TEXT("texture_graph_candidate_unknown") : TEXT("texture_graph_candidate_ambiguous"),
							TEXT("The TextureGraph candidate name must resolve to exactly one expression class."), TEXT("/candidate/name"), MakeShared<FJsonValueNumber>(MatchCount), MakeShared<FJsonValueNumber>(1));
						Diagnostic.CandidateName = Selector.Name;
						return Fail(OutError, MoveTemp(Diagnostic));
					}
				OutClass = Match;
				bOutClassChanged = ExistingClassPath && *ExistingClassPath != OutClass;
				return true;
			}
			FString NodeType;
			if (!Node.TryGetStringField(TEXT("node_type"), NodeType))
			{
				return Fail(OutError, TextureRequiredString(TEXT("texture_graph_domain_semantic_node_type_missing"),
					TEXT("The TextureGraph node_type field must be a string."), &Node, {TEXT("node_type")}, FString()));
			}
			if (NodeType.Contains(TEXT("/")))
			{
				{
					auto Diagnostic = TextureDomainFact(TEXT("texture_graph_domain_semantic_node_type_requires_short_name"),
						TEXT("TextureGraph node_type requires a short class name rather than a class path."), TEXT("/node_type"), MakeShared<FJsonValueString>(NodeType), MakeShared<FJsonValueString>(TEXT("short expression class name")));
					return Fail(OutError, MoveTemp(Diagnostic));
				}
			}
			if (ExistingClassPath && !ExistingClassPath->IsEmpty())
			{
				// This path came from the actual expression capture. Keep its class
				// without loading it again for every retained context node.
				if (FPackageName::ObjectPathToObjectName(*ExistingClassPath) == NodeType)
				{
					OutClass = *ExistingClassPath;
					return true;
				}
				bOutClassChanged = true;
			}
			FString Match;
			int32 MatchCount = 0;
			const auto Classes = PrepareExpressionClasses(Batch, Target, false);
			for (const auto& Class : *Classes)
			{
				if (Class.Key == NodeType)
				{
					Match = Class.Value;
					++MatchCount;
				}
			}
			if (MatchCount != 1)
			{
				{
					auto Diagnostic = TextureDomainFact(MatchCount == 0 ? TEXT("texture_graph_domain_semantic_node_type_unresolved") : TEXT("texture_graph_domain_semantic_node_type_ambiguous"),
						TEXT("The TextureGraph node_type must resolve to exactly one expression class."), TEXT("/node_type"), MakeShared<FJsonValueNumber>(MatchCount), MakeShared<FJsonValueNumber>(1));
					Diagnostic.CandidateName = NodeType;
					return Fail(OutError, MoveTemp(Diagnostic));
				}
			}
			OutClass = Match;
			return true;
		}

		bool TryBuildSemanticTextureGraphComposite(
			const FAuthoringModelRef& Model,
			const FUeAgentContentGraphCapture& CurrentCapture,
			const UTextureGraph& CurrentAsset,
			TArray<TSharedPtr<FJsonObject>>& OutNodes,
			TArray<TSharedPtr<FJsonObject>>& OutLinks,
			TArray<TSharedPtr<FJsonObject>>& OutParameters,
			FAudioMediaGraphTextureGraphNodeBindings& OutBindings,
			FWorkspaceError& OutError,
			const FResolvedSemanticWorkspace* Workspace)
		{
			OutNodes.Reset();
			OutLinks.Reset();
			OutParameters.Reset();
			OutBindings.NodeIndices.Reset();
			OutError.Reset();
			const FSemanticUnit* GraphUnit = nullptr;
			for (const FSemanticUnit* Unit : Model->FindUnitsForSemanticType(TEXT("uai.semantic.texture_graph.graph")))
			{
				if (Unit->GetUnitKind() == ESemanticUnitKind::StructureState)
				{
					GraphUnit = Unit;
					break;
				}
			}
			if (GraphUnit == nullptr)
			{
				{
					auto Diagnostic = TextureDomainFact(TEXT("texture_graph_domain_semantic_graph_missing"),
						TEXT("The resolved semantic model has no TextureGraph graph structure unit."), TEXT("/graph"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("native_result"));
					return Fail(OutError, MoveTemp(Diagnostic));
				}
			}
			ON_SCOPE_EXIT { LocateTextureSemanticFailure(OutError, Workspace, GraphUnit->GetUnitId()); };

			const StateSemantic::FUeAgentCanonicalValueRef* NodesCanonical =
				GraphUnit->GetValues().Find(TEXT("nodes"));
			const StateSemantic::FUeAgentCanonicalValueRef* EdgesCanonical =
				GraphUnit->GetValues().Find(TEXT("edges"));
			if (!NodesCanonical || !EdgesCanonical)
			{
				{
					auto Diagnostic = TextureDomainFact(TEXT("texture_graph_domain_semantic_graph_fields_missing"),
						TEXT("The TextureGraph graph unit requires nodes and edges fields."), TEXT("/"), MakeShared<FJsonValueString>(!NodesCanonical ? TEXT("nodes missing") : TEXT("edges missing")), MakeShared<FJsonValueString>(TEXT("nodes and edges")));
					return Fail(OutError, MoveTemp(Diagnostic));
				}
			}
			TSharedPtr<FJsonValue> NodesValue;
			TSharedPtr<FJsonValue> EdgesValue;
			if (!TryCanonicalValueToJson(
					*NodesCanonical,
					NodesValue, OutError)
				|| !TryCanonicalValueToJson(
					*EdgesCanonical,
					EdgesValue, OutError))
			{
				return false;
			}
			const TSharedPtr<FJsonObject> NodesObject = NodesValue->AsObject();
			const TSharedPtr<FJsonObject> EdgesObject = EdgesValue->AsObject();
			if (!NodesObject.IsValid() || !EdgesObject.IsValid())
			{
				{
					auto Diagnostic = TextureDomainFact(TEXT("texture_graph_domain_semantic_graph_shape_invalid"),
						TEXT("The TextureGraph nodes and edges fields must be objects."), !NodesObject.IsValid() ? TEXT("/nodes") : TEXT("/edges"), TextureDomainValue(!NodesObject.IsValid() ? NodesValue : EdgesValue), MakeShared<FJsonValueString>(TEXT("object")));
					return Fail(OutError, MoveTemp(Diagnostic));
				}
			}

			TMap<FString, FString> ElementByNodeId;
			TMap<FString, int32> IndexByNodeId;
			TMap<FString, FString> OperationByAlias;
			TMap<FString, FString> ExistingClassByNodeId;
			TMap<FString, const FUeAgentContentGraphItem*> ExistingNodes;
			TSet<FString> ReplacedElements;
			for (const FUeAgentContentGraphItem& Item : CurrentCapture.Items)
			{
				if (Item.ItemKind == TEXT("texture_graph_node"))
				{
					ExistingClassByNodeId.Add(Item.ItemId, Item.ClassPath);
					ExistingNodes.Add(Item.ItemId, &Item);
					int32 Index = INDEX_NONE;
					if (LexTryParseString(Index, *Item.SemanticIdentity.Selectors.FindRef(TEXT("node_index"))) && Index >= 0)
						IndexByNodeId.Add(Item.ItemId, Index);
				}
			}
			if (const UTG_Graph* NativeGraph = CurrentCapture.Items.IsEmpty() ? CurrentAsset.Graph() : nullptr)
			{
				NativeGraph->ForEachNodes(
					[&IndexByNodeId](const UTG_Node* NativeNode, const uint32)
					{
						if (!NativeNode || !NativeNode->GetExpression()) return;
						const FString NodeId = FString::Printf(
							TEXT("texture_graph.node:%d"),
							NativeNode->GetId().NodeIdx());
						IndexByNodeId.Add(NodeId, NativeNode->GetId().NodeIdx());
					});
			}
			ContentDiscovery::FReadOnlyCandidateBatch CandidateBatch;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair
				: NodesObject->Values)
			{
				const TSharedPtr<FJsonObject> Node = Pair.Value->AsObject();
				if (!Node.IsValid())
				{
					{
						auto Diagnostic = TextureDomainFact(TEXT("texture_graph_domain_semantic_node_invalid"),
							TEXT("Each TextureGraph node entry must be an object."), TEXT("/nodes/") + EscapeJsonPointerSegment(Pair.Key), TextureDomainValue(Pair.Value), MakeShared<FJsonValueString>(TEXT("object")));
						return Fail(OutError, MoveTemp(Diagnostic));
					}
				}
				FString NodeId;
				if (!Node->TryGetStringField(TEXT("id"), NodeId)
					|| NodeId.IsEmpty())
				{
					return Fail(OutError, TextureRequiredString(TEXT("texture_graph_domain_semantic_node_identity_missing"),
						TEXT("The semantic TextureGraph node requires a non-empty id."), Node.Get(), {TEXT("id")}, TEXT("/nodes/") + EscapeJsonPointerSegment(Pair.Key), true));
				}
				FString Operation;
				Node->TryGetStringField(TEXT("operation"), Operation);
				if (Operation.IsEmpty()) Operation = TEXT("update");
				OperationByAlias.Add(Pair.Key, Operation);
				const FString ElementId = Pair.Key;
				ElementByNodeId.Add(NodeId, ElementId);
				if (const int32* Index = IndexByNodeId.Find(NodeId))
					OutBindings.NodeIndices.Add(ElementId, *Index);

				FString ExpressionClass;
				bool bClassChanged = false;
				if (!TryTextureGraphExpressionClass(
						*Node, ExistingClassByNodeId.Find(NodeId),
						ExpressionClass, bClassChanged, OutError, CandidateBatch, CurrentAsset))
				{
					auto Diagnostic = OutError.GetDiagnostic();
					UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Diagnostic, TEXT("/nodes/") + EscapeJsonPointerSegment(Pair.Key));
					OutError.SetFailure(MoveTemp(Diagnostic));
					return false;
				}
				if (Operation == TEXT("update") && !bClassChanged && !Node->HasField(TEXT("properties"))) continue;
				const TSharedRef<FJsonObject> Mutation = MakeShared<FJsonObject>();
				Mutation->SetStringField(TEXT("element_id"), ElementId);
				Mutation->SetStringField(TEXT("expression_class_ref"), ExpressionClass);
				if (bClassChanged && Operation != TEXT("remove"))
				{
					Operation = TEXT("replace");
					ReplacedElements.Add(ElementId);
				}
				Mutation->SetStringField(
					TEXT("intent"), Operation == TEXT("remove") ? TEXT("delete") : Operation);
				if (Workspace) Workspace->AttachInlinePropertySources(GraphUnit->GetUnitId(),
					TEXT("/nodes/") + HjsonWorkspace::EscapeJsonPointerSegment(Pair.Key), *Node);
				if (const auto Properties = Node->TryGetField(TEXT("properties")))
					Mutation->SetField(TEXT("properties"), Properties);
				OutNodes.Add(Mutation);
			}

			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair
				: EdgesObject->Values)
			{
				const TSharedPtr<FJsonObject> Edge = Pair.Value->AsObject();
				if (!Edge.IsValid())
				{
					{
						auto Diagnostic = TextureDomainFact(TEXT("texture_graph_domain_semantic_edge_invalid"),
							TEXT("Each TextureGraph edge entry must be an object."), TEXT("/edges/") + EscapeJsonPointerSegment(Pair.Key), TextureDomainValue(Pair.Value), MakeShared<FJsonValueString>(TEXT("object")));
						return Fail(OutError, MoveTemp(Diagnostic));
					}
				}
				const TSharedPtr<FJsonObject>* From = nullptr;
				const TSharedPtr<FJsonObject>* To = nullptr;
				if (!Edge->TryGetObjectField(TEXT("from"), From)
					|| !From || !From->IsValid()
					|| !Edge->TryGetObjectField(TEXT("to"), To)
					|| !To || !To->IsValid())
				{
					{
						auto Diagnostic = TextureDomainFact(TEXT("texture_graph_domain_semantic_edge_endpoint_invalid"),
							TEXT("The TextureGraph edge requires from and to endpoint objects."), TEXT("/edges/") + EscapeJsonPointerSegment(Pair.Key) + (!From || !From->IsValid() ? TEXT("/from") : TEXT("/to")), TextureDomainValue(Edge->TryGetField(!From || !From->IsValid() ? TEXT("from") : TEXT("to"))), MakeShared<FJsonValueString>(TEXT("object")));
						return Fail(OutError, MoveTemp(Diagnostic));
					}
				}
				FString FromId, FromPin, ToId, ToPin, Operation;
				if (!(*From)->TryGetStringField(TEXT("node"), FromId)
					|| !(*From)->TryGetStringField(TEXT("pin"), FromPin)
					|| !(*To)->TryGetStringField(TEXT("node"), ToId)
					|| !(*To)->TryGetStringField(TEXT("pin"), ToPin))
				{
					auto Diagnostic = TextureRequiredString(TEXT("texture_graph_domain_semantic_edge_endpoint_missing"),
						TEXT("The TextureGraph edge endpoint requires string node and pin fields."), From->Get(),
						{TEXT("node"), TEXT("pin")}, TEXT("/edges/") + EscapeJsonPointerSegment(Pair.Key) + TEXT("/from"));
					if (Diagnostic.IsEmpty()) Diagnostic = TextureRequiredString(TEXT("texture_graph_domain_semantic_edge_endpoint_missing"),
						TEXT("The TextureGraph edge endpoint requires string node and pin fields."), To->Get(),
						{TEXT("node"), TEXT("pin")}, TEXT("/edges/") + EscapeJsonPointerSegment(Pair.Key) + TEXT("/to"));
					return Fail(OutError, MoveTemp(Diagnostic));
				}
				if (!ElementByNodeId.Contains(FromId) || !ElementByNodeId.Contains(ToId))
				{
					{
						auto Diagnostic = TextureDomainFact(TEXT("texture_graph_domain_semantic_edge_node_unresolved"),
							TEXT("The TextureGraph edge endpoint does not identify a node in this graph."), TEXT("/edges/") + EscapeJsonPointerSegment(Pair.Key) + (!ElementByNodeId.Contains(FromId) ? TEXT("/from/node") : TEXT("/to/node")), MakeShared<FJsonValueString>(!ElementByNodeId.Contains(FromId) ? FromId : ToId), MakeShared<FJsonValueString>(TEXT("existing node id")));
						return Fail(OutError, MoveTemp(Diagnostic));
					}
				}
				Edge->TryGetStringField(TEXT("operation"), Operation);
				if (Operation.IsEmpty()) Operation = TEXT("update");
				const FString FromElement = ElementByNodeId.FindChecked(FromId);
				const FString ToElement = ElementByNodeId.FindChecked(ToId);
				if (Operation == TEXT("update")
					&& !ReplacedElements.Contains(FromElement)
					&& !ReplacedElements.Contains(ToElement))
				{
					continue;
				}
				const TSharedRef<FJsonObject> Mutation = MakeShared<FJsonObject>();
				const TSharedRef<FJsonObject> FromObject = MakeShared<FJsonObject>();
				const TSharedRef<FJsonObject> ToObject = MakeShared<FJsonObject>();
				FromObject->SetStringField(TEXT("direction"), TEXT("output"));
				FromObject->SetStringField(TEXT("element_id"), FromElement);
				FromObject->SetStringField(TEXT("pin_name"), FromPin);
				ToObject->SetStringField(TEXT("direction"), TEXT("input"));
				ToObject->SetStringField(TEXT("element_id"), ToElement);
				ToObject->SetStringField(TEXT("pin_name"), ToPin);
				Mutation->SetObjectField(TEXT("from"), FromObject);
				Mutation->SetObjectField(TEXT("to"), ToObject);
				Mutation->SetStringField(
					TEXT("intent"), Operation == TEXT("remove") ? TEXT("delete") : TEXT("create"));
				OutLinks.Add(Mutation);
			}

			for (const TPair<FString, TSharedPtr<FJsonValue>>& NodePair
				: NodesObject->Values)
			{
				const TSharedPtr<FJsonObject> Node = NodePair.Value->AsObject();
				UE::UAI::Contract::FDiagnostic FieldError;
				if (Node && !UeAgentAuthoringJsonFields::ValidateOptionalFields(*Node,
					{{TEXT("inputs"), EJson::Object}}, FieldError))
				{
					UeAgentAuthoringDiagnostics::PrefixDocumentFailure(FieldError, TEXT("/nodes/") + EscapeJsonPointerSegment(NodePair.Key));
					return Fail(OutError, MoveTemp(FieldError));
				}
				const TSharedPtr<FJsonValue>* InputsValue = Node.IsValid()
					? Node->Values.Find(TEXT("inputs")) : nullptr;
				if (!InputsValue || !InputsValue->IsValid()
					|| (*InputsValue)->Type != EJson::Object) continue;
				const TSharedPtr<FJsonObject> Inputs = (*InputsValue)->AsObject();
				if (!Inputs.IsValid()) continue;
				const FString Operation = OperationByAlias.FindRef(NodePair.Key);
				if (Operation == TEXT("remove")) continue;
				FString NodeId;
				if (!Node->TryGetStringField(TEXT("id"), NodeId)
					|| !ElementByNodeId.Contains(NodeId))
				{
					{
						auto Diagnostic = TextureDomainFact(TEXT("texture_graph_domain_semantic_node_identity_missing"),
							TEXT("The TextureGraph parameter owner must identify a node in this graph."), TEXT("/nodes/") + EscapeJsonPointerSegment(NodePair.Key) + TEXT("/id"), TextureDomainValue(Node->TryGetField(TEXT("id"))), MakeShared<FJsonValueString>(TEXT("existing node id")));
						return Fail(OutError, MoveTemp(Diagnostic));
					}
				}
				for (const TPair<FString, TSharedPtr<FJsonValue>>& InputPair : Inputs->Values)
				{
					bool bUnchangedInput = false;
					if (Operation == TEXT("update") && !ReplacedElements.Contains(ElementByNodeId.FindChecked(NodeId))) {
						const auto* Existing = ExistingNodes.FindRef(NodeId);
						const StateSemantic::FUeAgentCanonicalValue* OldInputs = nullptr;
						if (Existing && Existing->State) for (const auto& Field : Existing->State->GetObjectItems())
							if (Field.Key == TEXT("inputs")) { OldInputs = &Field.Value.Get(); break; }
						if (OldInputs && OldInputs->GetKind() == StateSemantic::EUeAgentValueKind::Object)
							for (const auto& Old : OldInputs->GetObjectItems()) if (Old.Key == InputPair.Key) {
								TSharedPtr<FJsonValue> OldValue;
								if (!TryCanonicalValueToJson(Old.Value, OldValue, OutError)) return false;
								bUnchangedInput = StateSemantic::CanonicalJson::AreJsonValuesEquivalent(OldValue, InputPair.Value);
								break;
							}
					}
					if (bUnchangedInput) continue;
					if (!InputPair.Value.IsValid() || InputPair.Value->Type != EJson::Object)
						{
							auto Diagnostic = TextureDomainFact(TEXT("texture_graph_domain_semantic_parameter_invalid"),
								TEXT("Each TextureGraph input entry must be an object."), TEXT("/nodes/") + EscapeJsonPointerSegment(NodePair.Key) + TEXT("/inputs/") + EscapeJsonPointerSegment(InputPair.Key), TextureDomainValue(InputPair.Value), MakeShared<FJsonValueString>(TEXT("object")));
							return Fail(OutError, MoveTemp(Diagnostic));
						}
					const TSharedPtr<FJsonObject> Input = InputPair.Value->AsObject();
					const TSharedPtr<FJsonValue>* Value = Input.IsValid()
						? Input->Values.Find(TEXT("value")) : nullptr;
					if (!Input.IsValid() || !UeAgentAuthoringJsonFields::ValidateOptionalFields(*Input,
						{{TEXT("alias"), EJson::String}}, FieldError))
					{
						if (FieldError.IsEmpty()) FieldError = TextureDomainFact(TEXT("texture_graph_domain_semantic_parameter_invalid"),
							TEXT("The TextureGraph input entry must be an object."), FString(), TextureDomainValue(InputPair.Value), MakeShared<FJsonValueString>(TEXT("object")));
						UeAgentAuthoringDiagnostics::PrefixDocumentFailure(FieldError,
							TEXT("/nodes/") + EscapeJsonPointerSegment(NodePair.Key) + TEXT("/inputs/") + EscapeJsonPointerSegment(InputPair.Key));
						return Fail(OutError, MoveTemp(FieldError));
					}
					if (!Value && !Input->HasField(TEXT("alias")))
						return Fail(OutError, TextureDomainFact(TEXT("texture_graph_domain_semantic_parameter_invalid"),
							TEXT("The TextureGraph input entry requires a value or alias."),
							TEXT("/nodes/") + EscapeJsonPointerSegment(NodePair.Key) + TEXT("/inputs/") + EscapeJsonPointerSegment(InputPair.Key),
							MakeShared<FJsonValueString>(TEXT("value and alias missing")), MakeShared<FJsonValueString>(TEXT("value or alias"))));
					const TSharedRef<FJsonObject> Mutation = MakeShared<FJsonObject>();
					Mutation->SetStringField(
						TEXT("element_id"), ElementByNodeId.FindChecked(NodeId));
					Mutation->SetStringField(TEXT("pin_name"), InputPair.Key);
					FString Alias;
					Input->TryGetStringField(TEXT("alias"), Alias);
					Mutation->SetStringField(TEXT("alias"), Alias.IsEmpty() ? InputPair.Key : Alias);
					Mutation->SetStringField(TEXT("intent"), TEXT("update"));
					if (Value) Mutation->SetField(TEXT("value"), *Value);
					if (Workspace)
					{
						auto Escape = [](const FString& Text) { return Text.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")); };
						const FString Pointer = TEXT("/nodes/") + Escape(NodePair.Key) + TEXT("/inputs/") + Escape(InputPair.Key);
						const FSourceProvenance* Source = Workspace->FindValueSource(GraphUnit->GetUnitId(), Pointer + (Value ? TEXT("/value") : TEXT("/alias")));
						if (!Source) Source = Workspace->FindValueSource(GraphUnit->GetUnitId(), Pointer);
						if (Source)
						{
							const auto Location = MakeShared<FJsonObject>();
							Location->SetStringField(TEXT("source_file"), Source->GetSourceUnit());
							Location->SetStringField(TEXT("json_pointer"), Source->GetJsonPointer());
							Location->SetNumberField(TEXT("line"), Source->GetLine());
							Location->SetNumberField(TEXT("column"), Source->GetColumn());
							Mutation->SetObjectField(TEXT("authoring_source"), Location);
						}
					}
					OutParameters.Add(Mutation);
				}
			}
			return true;
		}

		bool TryBuildTypedSemanticTextureGraphComposite(
			const FString& Target,
			const TArray<TSharedPtr<FJsonObject>>& NodeObjects,
			const TArray<TSharedPtr<FJsonObject>>& LinkObjects,
			const TArray<TSharedPtr<FJsonObject>>& ParameterObjects,
			Composition::GraphCompound::FTextureGraphSemanticCompositeRequest& OutRequest,
			FWorkspaceError& OutError)
		{
			OutRequest = Composition::GraphCompound::FTextureGraphSemanticCompositeRequest();
			OutRequest.TargetRef = Target;
			for (const TSharedPtr<FJsonObject>& Node : NodeObjects)
			{
				FString ElementId, ClassRef, Intent;
				if (!Node.IsValid()
					|| !Node->TryGetStringField(TEXT("element_id"), ElementId)
					|| !Node->TryGetStringField(TEXT("expression_class_ref"), ClassRef)
					|| !Node->TryGetStringField(TEXT("intent"), Intent))
				{
					return Fail(OutError, TextureRequiredString(TEXT("texture_graph_domain_semantic_node_mutation_invalid"),
						TEXT("The projected TextureGraph node mutation requires string fields."), Node.Get(),
						{TEXT("element_id"), TEXT("expression_class_ref"), TEXT("intent")}, TEXT("/nodes"), false, TEXT("native_result")));
				}
				EAudioMediaGraphMutationIntent MutationIntent;
				if (Intent == TEXT("create"))
				{
					MutationIntent = EAudioMediaGraphMutationIntent::Create;
				}
				else if (Intent == TEXT("update"))
				{
					MutationIntent = EAudioMediaGraphMutationIntent::Update;
				}
				else if (Intent == TEXT("replace"))
				{
					MutationIntent = EAudioMediaGraphMutationIntent::Replace;
				}
				else if (Intent == TEXT("delete"))
				{
					MutationIntent = EAudioMediaGraphMutationIntent::Delete;
				}
				else
				{
					{
						auto Diagnostic = TextureDomainFact(TEXT("texture_graph_domain_semantic_node_intent_invalid"),
							TEXT("The TextureGraph node operation is unsupported."), TEXT("/nodes/") + EscapeJsonPointerSegment(ElementId) + TEXT("/operation"), MakeShared<FJsonValueString>(Intent), MakeShared<FJsonValueString>(TEXT("create, update, replace, remove or delete")));
						return Fail(OutError, MoveTemp(Diagnostic));
					}
				}
				TSharedPtr<FJsonObject> Properties;
				UE::UAI::Contract::FDiagnostic Detail;
				if (!UeAgentDetailsAuthoringService::ReadPropertyPayload(Node, TEXT("properties"), Properties, Detail))
				{
					UeAgentAuthoringDiagnostics::PrefixDocumentFailure(Detail,
						TEXT("/nodes/") + HjsonWorkspace::EscapeJsonPointerSegment(ElementId) + TEXT("/properties"));
					auto Diagnostic = Detail;
					if (Diagnostic.Stage.IsEmpty()) Diagnostic.Stage = TEXT("node_properties");
					OutError.SetFailure(Diagnostic, FString());
					return false;
				}
				if (Properties && !Properties->GetArrayField(TEXT("properties")).IsEmpty())
				{
					if (MutationIntent == EAudioMediaGraphMutationIntent::Delete)
						{
							auto Diagnostic = TextureDomainFact(TEXT("texture_graph_setting_owner_removed"),
								TEXT("Node settings cannot be applied to a node selected for deletion."), TEXT("/nodes/") + EscapeJsonPointerSegment(ElementId) + TEXT("/properties"), MakeShared<FJsonValueString>(TEXT("delete")), MakeShared<FJsonValueString>(TEXT("a retained node")));
							return Fail(OutError, MoveTemp(Diagnostic));
						}
					OutRequest.ElementSettings.Add(ElementId, Properties->GetArrayField(TEXT("properties")));
				}
				// Retained context has no node setter. Its existence/class/ports are
				// checked from the observed before scope, not sent as fake updates.
				if (MutationIntent != EAudioMediaGraphMutationIntent::Update)
					OutRequest.Nodes.Emplace(MoveTemp(ElementId), MoveTemp(ClassRef), MutationIntent);
			}
			for (const TSharedPtr<FJsonObject>& Link : LinkObjects)
			{
				const TSharedPtr<FJsonObject>* From = nullptr;
				const TSharedPtr<FJsonObject>* To = nullptr;
				FString FromId, FromPin, ToId, ToPin, Intent;
				if (!Link.IsValid() || !Link->TryGetObjectField(TEXT("from"), From)
					|| !From || !From->IsValid() || !Link->TryGetObjectField(TEXT("to"), To)
					|| !To || !To->IsValid()
					|| !(*From)->TryGetStringField(TEXT("element_id"), FromId)
					|| !(*From)->TryGetStringField(TEXT("pin_name"), FromPin)
					|| !(*To)->TryGetStringField(TEXT("element_id"), ToId)
					|| !(*To)->TryGetStringField(TEXT("pin_name"), ToPin)
					|| !Link->TryGetStringField(TEXT("intent"), Intent))
				{
					UE::UAI::Contract::FDiagnostic Diagnostic;
					if (!Link || !From || !From->IsValid() || !To || !To->IsValid())
					{
						const FString Path = !Link ? TEXT("/link") : !From || !From->IsValid() ? TEXT("/from") : TEXT("/to");
						Diagnostic = TextureDomainFact(TEXT("texture_graph_domain_semantic_link_mutation_invalid"),
							TEXT("The projected TextureGraph link requires from and to endpoint objects."), Path,
							TextureDomainValue(Link ? Link->TryGetField(!From || !From->IsValid() ? TEXT("from") : TEXT("to")) : nullptr),
							MakeShared<FJsonValueString>(TEXT("object")), TEXT("native_result"));
					}
					else Diagnostic = TextureRequiredString(TEXT("texture_graph_domain_semantic_link_mutation_invalid"),
						TEXT("The projected TextureGraph link requires string element_id and pin_name fields."),
						From->Get(), {TEXT("element_id"), TEXT("pin_name")}, TEXT("/from"), false, TEXT("native_result"));
					if (Diagnostic.IsEmpty()) Diagnostic = TextureRequiredString(TEXT("texture_graph_domain_semantic_link_mutation_invalid"),
						TEXT("The projected TextureGraph link requires string element_id and pin_name fields."),
						To && To->IsValid() ? To->Get() : nullptr, {TEXT("element_id"), TEXT("pin_name")}, TEXT("/to"), false, TEXT("native_result"));
					if (Diagnostic.IsEmpty()) Diagnostic = TextureRequiredString(TEXT("texture_graph_domain_semantic_link_mutation_invalid"),
						TEXT("The projected TextureGraph link requires a string intent."), Link.Get(), {TEXT("intent")}, FString(), false, TEXT("native_result"));
					return Fail(OutError, MoveTemp(Diagnostic));
				}
				EAudioMediaGraphMutationIntent MutationIntent;
				if (Intent == TEXT("delete"))
				{
					MutationIntent = EAudioMediaGraphMutationIntent::Delete;
				}
				else if (Intent == TEXT("create"))
				{
					MutationIntent = EAudioMediaGraphMutationIntent::Create;
				}
				else
				{
					{
						auto Diagnostic = TextureDomainFact(TEXT("texture_graph_domain_semantic_link_intent_invalid"),
							TEXT("The projected TextureGraph link intent is unsupported."), TEXT("/intent"), MakeShared<FJsonValueString>(Intent), MakeShared<FJsonValueString>(TEXT("create or delete")), TEXT("native_result"));
						return Fail(OutError, MoveTemp(Diagnostic));
					}
				}
				OutRequest.Links.Emplace(
					FAudioMediaGraphTextureGraphEndpoint(MoveTemp(FromId), MoveTemp(FromPin)),
					FAudioMediaGraphTextureGraphEndpoint(MoveTemp(ToId), MoveTemp(ToPin)),
					MutationIntent);
			}
			for (const TSharedPtr<FJsonObject>& Parameter : ParameterObjects)
			{
				FString ElementId, PinName, Alias, Intent;
				const TSharedPtr<FJsonValue>* Value = Parameter.IsValid()
					? Parameter->Values.Find(TEXT("value")) : nullptr;
				if (!Parameter.IsValid()
					|| !Parameter->TryGetStringField(TEXT("element_id"), ElementId)
					|| !Parameter->TryGetStringField(TEXT("pin_name"), PinName)
					|| !Parameter->TryGetStringField(TEXT("alias"), Alias)
					|| !Parameter->TryGetStringField(TEXT("intent"), Intent))
				{
					return Fail(OutError, TextureRequiredString(TEXT("texture_graph_domain_semantic_parameter_invalid"),
						TEXT("The projected TextureGraph parameter requires string fields."), Parameter.Get(),
						{TEXT("element_id"), TEXT("pin_name"), TEXT("alias"), TEXT("intent")}, TEXT("/parameter"), false, TEXT("native_result")));
				}
				if (Intent != TEXT("replace") && Intent != TEXT("update"))
				{
					auto Diagnostic = TextureDomainFact(TEXT("texture_graph_domain_semantic_parameter_intent_invalid"),
							TEXT("The projected TextureGraph parameter intent is unsupported."), TEXT("/intent"), MakeShared<FJsonValueString>(Intent), MakeShared<FJsonValueString>(TEXT("replace or update")), TEXT("native_result"));
						return Fail(OutError, MoveTemp(Diagnostic));
					}
				const EAudioMediaGraphMutationIntent MutationIntent =
					Intent == TEXT("replace") ? EAudioMediaGraphMutationIntent::Replace : EAudioMediaGraphMutationIntent::Update;
				if (!Value)
				{
					OutRequest.Parameters.Emplace(MoveTemp(ElementId), MoveTemp(PinName), MoveTemp(Alias), MutationIntent);
					UeAgentAuthoringDiagnostics::FDiagnostic Location; UeAgentAuthoringDiagnostics::LocateAuthoringSource(Location, *Parameter);
					OutRequest.Parameters.Last().SetSource(MoveTemp(Location.Source));
					continue;
				}
				if (!(*Value).IsValid()) return Fail(OutError, TextureDomainFact(TEXT("texture_graph_domain_semantic_parameter_invalid"),
					TEXT("The TextureGraph input value is unavailable."), TEXT("/nodes/") + EscapeJsonPointerSegment(ElementId) + TEXT("/inputs/") + EscapeJsonPointerSegment(PinName) + TEXT("/value"),
					MakeShared<FJsonValueString>(TEXT("<missing>")), MakeShared<FJsonValueString>(TEXT("number, boolean, string or four-component number array"))));
				FAudioMediaGraphTextureGraphValue TypedValue;
				switch ((*Value)->Type)
				{
				case EJson::Number: TypedValue = FAudioMediaGraphTextureGraphValue::Number(*Value); break;
				case EJson::Boolean: TypedValue = FAudioMediaGraphTextureGraphValue::Boolean((*Value)->AsBool()); break;
				case EJson::String: TypedValue = FAudioMediaGraphTextureGraphValue::String((*Value)->AsString()); break;
				case EJson::Array:
				{
					TArray<double> Values;
					const auto& Items = (*Value)->AsArray();
					for (int32 Index = 0; Index < Items.Num(); ++Index)
					{
						const auto& Item = Items[Index];
						if (!Item.IsValid() || Item->Type != EJson::Number) return Fail(OutError, TextureDomainFact(TEXT("texture_graph_domain_semantic_parameter_type_unsupported"),
							TEXT("Every TextureGraph color/vector component must be a number."),
							TEXT("/nodes/") + EscapeJsonPointerSegment(ElementId) + TEXT("/inputs/") + EscapeJsonPointerSegment(PinName) + TEXT("/value/") + ::LexToString(Index),
							TextureDomainValue(Item), MakeShared<FJsonValueString>(TEXT("number"))));
						Values.Add(Item->AsNumber());
					}
					if (Values.Num() != 4) return Fail(OutError, TextureDomainFact(TEXT("texture_graph_domain_semantic_parameter_type_unsupported"),
						TEXT("A TextureGraph color/vector input requires exactly four components."),
						TEXT("/nodes/") + EscapeJsonPointerSegment(ElementId) + TEXT("/inputs/") + EscapeJsonPointerSegment(PinName) + TEXT("/value"),
						MakeShared<FJsonValueNumber>(Values.Num()), MakeShared<FJsonValueNumber>(4)));
					TypedValue = FAudioMediaGraphTextureGraphValue::NumberArray(MoveTemp(Values));
					break;
				}
				default: return Fail(OutError, TextureDomainFact(TEXT("texture_graph_domain_semantic_parameter_type_unsupported"),
					TEXT("The TextureGraph input value type is unsupported."),
					TEXT("/nodes/") + EscapeJsonPointerSegment(ElementId) + TEXT("/inputs/") + EscapeJsonPointerSegment(PinName) + TEXT("/value"),
					TextureDomainValue(*Value), MakeShared<FJsonValueString>(TEXT("number, boolean, string or four-component number array"))));
				}
				OutRequest.Parameters.Emplace(MoveTemp(ElementId), MoveTemp(PinName), MoveTemp(Alias), MoveTemp(TypedValue), MutationIntent);
				UeAgentAuthoringDiagnostics::FDiagnostic Location; UeAgentAuthoringDiagnostics::LocateAuthoringSource(Location, *Parameter);
				OutRequest.Parameters.Last().SetSource(MoveTemp(Location.Source));
			}
			// Reflected settings are attached by the prepared-change consumer.
			// Validate the final request there, after all real mutations are bound.
			return true;
		}

		bool TryImport(
			const FString& Target,
			const FString& CanonicalParamsJson,
			FString& OutPayload,
			FWorkspaceError& OutError)
		{
			FResolvedDomainHjsonDocument Document;
			if (!TryDecodeResolvedDocument(
					Target,
					CanonicalParamsJson,
					FDomainHjsonDocumentContract(
						TEXT("texture_graph"), SchemaId, TextureGraphType),
					Document,
					OutError))
			{
				return false;
			}
			UE::UAI::HjsonWorkspace::DomainAuthoring::FScopedDomainHjsonDiagnosticSource DiagnosticSource(Document, OutError);
			if (Document.bSemanticWorkspaceDirectory)
			{
				using namespace UE::UAI::Composition::HjsonImport;
				if (!Document.SemanticWorkspace.IsValid())
				{
					{
						auto Diagnostic = TextureDomainFact(TEXT("texture_graph_domain_semantic_workspace_missing"),
							TEXT("The decoded directory import has no resolved semantic workspace."), TEXT("/workspace"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("native_result"));
						Diagnostic.TargetPath = Target;
						return Fail(OutError, MoveTemp(Diagnostic));
					}
				}
				UObject* Asset = nullptr;
				FUeAgentContentGraphCapture TargetCapture;
				TMap<int32, TSet<FString>> SettingFields;
				TArray<const FSemanticUnit*> UnaddressedSettings;
				// Compilation is a storage choice, not the edit's native scope.
				bool bSettingsOnly = Document.SemanticWorkspace->GetStructuralEdits().IsEmpty()
					&& Document.SemanticWorkspace->GetEditedUnitIds().IsSet()
					&& !Document.SemanticWorkspace->GetEditedUnitIds()->IsEmpty();
				TArray<const FSemanticUnit*> Candidates;
				if (Document.SemanticWorkspace->GetEditedUnitIds().IsSet()) {
					for (const auto& Id : *Document.SemanticWorkspace->GetEditedUnitIds())
						Candidates.Add(Document.SemanticWorkspace->GetModel()->FindUnit(Id));
				} else Candidates = Document.SemanticWorkspace->GetModel()->FindUnitsForSemanticType(TEXT("uai.semantic.texture_graph.setting"));
				for (const auto* Unit : Candidates) {
					const auto* Owner = Unit ? Document.SemanticWorkspace->GetModel()->FindUnit(Unit->GetOwnerRef()) : nullptr;
					const auto* NativeId = Owner ? Owner->GetValues().Find(TEXT("id")) : nullptr;
					if (!NativeId && Owner) if (const auto* BaselineOwner = Document.SemanticWorkspace->FindBaselineUnit(Owner->GetUnitId()))
						NativeId = BaselineOwner->GetValues().Find(TEXT("id"));
					FString Text = NativeId && (*NativeId)->GetKind() == StateSemantic::EUeAgentValueKind::String ? (*NativeId)->GetText() : FString();
					int32 NodeIndex = INDEX_NONE;
					if (!Unit || Unit->GetSemanticTypeRef() != TEXT("uai.semantic.texture_graph.setting")) { bSettingsOnly = false; continue; }
					if (!Text.RemoveFromStart(TEXT("texture_graph.node:")) || !LexTryParseString(NodeIndex, *Text) || NodeIndex < 0) {
						UnaddressedSettings.Add(Unit);
						continue;
					}
					SettingFields.FindOrAdd(NodeIndex).Add(Unit->GetFieldSetRef());
				}
				if (!TryLoadObjectTarget(Target, TextureGraphType, Asset, OutError)) return false;
				if (!UnaddressedSettings.IsEmpty()) {
					// Legacy rows may omit the readable node id. Recover only their
					// native identity directory, never every expression setting value.
					FUeAgentContentGraphCapture AddressCapture;
					FUeAgentContentGraphReadError Detail;
					StateSemantic::FUeAgentCanonicalSnapshotPtr Addresses;
					auto AddressResources = MakeSemanticWorkspaceResourceTrackerOwner();
					if (!FUeAgentContentGraphReadProductionEnvironment::TryCaptureTextureGraphSettings(*Asset, {}, AddressCapture, Detail, true)
						|| !TryBuildContentGraphAuthoringSnapshot(AddressCapture, AddressResources, Addresses, Detail)) {
						OutError.SetFailure(MoveTemp(Detail)); return false;
					}
					for (const auto* Setting : UnaddressedSettings) {
						const auto* Owner = Document.SemanticWorkspace->GetModel()->FindUnit(Setting->GetOwnerRef());
						bool bFound = false;
						if (Owner) for (const auto& Entry : Addresses->GetEntries()) {
							if (!Entry.GetIdentity().WithCapturedBinding(Owner->GetTargetIdentityKey()).IsSet()) continue;
							int32 Index = INDEX_NONE;
							if (!LexTryParseString(Index, *Entry.GetIdentity().GetSelector(TEXT("node_index"))) || Index < 0) continue;
							SettingFields.FindOrAdd(Index).Add(Setting->GetFieldSetRef()); bFound = true; break;
						}
						if (!bFound) return Fail(OutError, TextureDomainFact(TEXT("texture_graph_setting_identity_unresolved"),
							TEXT("The setting's captured native node binding no longer resolves; inspect the node identity before retrying."),
							TEXT("/setting/owner"), MakeShared<FJsonValueString>(Setting->GetOwnerRef()),
							MakeShared<FJsonValueString>(TEXT("an existing matching native node")), TEXT("native_result")));
					}
				}
				FUeAgentContentGraphReadError CurrentError;
				if (!FUeAgentContentGraphReadProductionEnvironment::TryCaptureTextureGraphSettings(
					*Asset, SettingFields, TargetCapture, CurrentError, !bSettingsOnly)) {
					OutError.SetFailure(MoveTemp(CurrentError)); return false;
				}
				const FString ActualRootType = Asset
					? Asset->GetClass()->GetPathName() : FString();
				if (ActualRootType != TextureGraphType)
				{
					{
						auto Diagnostic = TextureDomainFact(TEXT("texture_graph_domain_semantic_target_type_invalid"),
							TEXT("The semantic import target is not a TextureGraph asset."), TEXT("/class"), MakeShared<FJsonValueString>(ActualRootType), MakeShared<FJsonValueString>(TextureGraphType), TEXT("native_result"));
						Diagnostic.TargetPath = Target;
						return Fail(OutError, MoveTemp(Diagnostic));
					}
				}
				const FUeAgentResourceTrackerOwner Resources =
					MakeSemanticWorkspaceResourceTrackerOwner();
				TSharedPtr<const StateSemantic::FUeAgentCanonicalSnapshot>
					CanonicalSnapshot;
				FAuthoringModelPtr Model;
				FUeAgentContentGraphReadError SnapshotError;
				if (!TryBuildContentGraphAuthoringSnapshot(
						TargetCapture,
						Resources,
						CanonicalSnapshot,
						SnapshotError))
				{
					OutError.SetFailure(SnapshotError);
					return false;
				}
				FCompositionUnitSet Observed;
				if (!TryComposeScopedAuthoringUnits(TEXT("ue.texture_graph.asset"), TEXT("llm_authoring"),
					CanonicalSnapshot, *Resources, Observed, OutError, &Document.SemanticWorkspace->GetModel().Get()))
				{
					return false;
				}
				Model = FAuthoringModel::MakePropertySourceView(MoveTemp(Observed.ContractId), Observed.ContractVersion,
					MoveTemp(Observed.ProfileId), MoveTemp(Observed.BaseRevision), MoveTemp(Observed.Units),
					MoveTemp(Observed.TargetUnitIds), CanonicalSnapshot->GetSnapshotId());
				UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=texture_current_scope setting_nodes=%d topology=%s observed_units=%d current_compile=false"),
					SettingFields.Num(), bSettingsOnly ? TEXT("false") : TEXT("true"), Model->GetUnitCount());
				const FString TargetObjectPath = ObjectPathFromPackage(Target);
				const UTextureGraph* TextureGraph = Cast<UTextureGraph>(Asset);
				if (!TextureGraph)
				{
					{
						auto Diagnostic = TextureDomainFact(TEXT("texture_graph_domain_semantic_target_type_invalid"),
							TEXT("The semantic import target is not a TextureGraph asset."), TEXT("/class"), MakeShared<FJsonValueString>(ActualRootType), MakeShared<FJsonValueString>(TextureGraphType), TEXT("native_result"));
						Diagnostic.TargetPath = Target;
						return Fail(OutError, MoveTemp(Diagnostic));
					}
				}
				Composition::GraphCompound::FTextureGraphSemanticCompositeRequest
					Composite;
				if (bSettingsOnly) {
					Composite.TargetRef = TargetObjectPath;
					for (const auto& Pair : SettingFields) Composite.NodeBindings.NodeIndices.Add(
						FString::Printf(TEXT("texture_graph.node:%d"), Pair.Key), Pair.Key);
				}
				return TryCoordinateSemanticWorkspaceImport(
					Document.SemanticWorkspace.ToSharedRef(),
					Model.ToSharedRef(),
					CanonicalSnapshot.ToSharedRef(),
					BuildSemanticWorkspaceImportDescriptor(TEXT("texture_graph.semantic_workspace"), *Model),
					TargetObjectPath,
					*Resources,
					{
						TEXT("texture-graph-domain-provider"),
						Target,
						TargetObjectPath,
						TEXT("texture-graph-semantic"),
						TEXT("texture_graph_domain_semantic_prepare_failed"),
						TEXT("texture_graph_domain_semantic_apply_failed"),
						TEXT("texture_graph_domain_semantic_readback_payload_invalid"),
						TEXT("texture_graph_domain_compile_not_settled"), {},
						{TEXT("uai.semantic.texture_graph.graph")}},
					[&Asset, &Composite, &TargetObjectPath, &TargetCapture, &CanonicalSnapshot, bSettingsOnly](
						const FString& RequestId,
						const Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
						Composition::HjsonImport::FSemanticWorkspaceChangeProjection&& Projection,
						FRootPropertyApplyOutcome& OutOutcome)
					{
						UE::UAI::Contract::FDiagnostic SettingError;
						if (!bSettingsOnly) {
							FWorkspaceError CompositeError;
							if (!TryBuildSemanticComposite(Prepared.GetWorkspace()->GetModel(), TargetCapture,
								*CastChecked<UTextureGraph>(Asset), TargetObjectPath, Composite, CompositeError, &Prepared.GetWorkspace().Get())) {
								OutOutcome.Phase = TEXT("semantic_projection"); OutOutcome.Diagnostic = CompositeError.GetDiagnostic(); return false;
							}
							Composite.BeforeSnapshot = CanonicalSnapshot;
						}
						if (!TryBindSettingChanges(Prepared, Projection, Composite, SettingError))
						{ OutOutcome.Phase = TEXT("semantic_projection"); OutOutcome.Diagnostic = SettingError; return false; }
						UE_LOG(LogTemp, Display, TEXT("UAI import timing phase=texture_prepared_composite nodes=%d links=%d parameters=%d setting_nodes=%d context_node_updates=0"),
							Composite.Nodes.Num(), Composite.Links.Num(), Composite.Parameters.Num(), Composite.Settings.Num());
						auto PlanningResources = MakeSemanticWorkspaceResourceTracker();
						PlanningPreflight::FPlanningPreflightPlanPtr Plan;
						if (!Prepared.TryGetExecutionPlan(PlanningResources, Plan, SettingError)) {
							OutOutcome.Phase = TEXT("semantic_preparation"); OutOutcome.Diagnostic = SettingError; return false;
						}
						if (Asset == nullptr || !Plan.IsValid())
						{
							auto Facts = MakeShared<FJsonObject>();
							Facts->SetBoolField(TEXT("asset_present"), Asset != nullptr);
							Facts->SetBoolField(TEXT("plan_present"), Plan.IsValid());
							OutOutcome.Diagnostic = TextureDomainFact(TEXT("texture_graph_domain_semantic_asset_missing"),
								TEXT("The TextureGraph asset or prepared plan is unavailable."), TEXT("/apply"),
								MakeShared<FJsonValueObject>(Facts), MakeShared<FJsonValueString>(TEXT("an asset and prepared plan")), TEXT("native_result"));
							OutOutcome.Diagnostic.TargetPath = TargetObjectPath;
							return false;
						}
						UeAgentAuthoringOperationApply::FApplyWorkspaceResult Result;
						UE::UAI::Contract::FDiagnostic ApplyError;
						UeAgentAuthoringOperationApply::FBackendOptions BackendOptions;
						BackendOptions.FinalReadback = OutOutcome.FinalReadback;
						const bool bApplied =
							OperationOwners::TextureGraph::TryApplySemanticTextureGraphComposite(
								Composite,
								Plan.ToSharedRef(),
								MakeShared<FNeverCancelled>(),
								Result,
								ApplyError,
								BackendOptions);
						OutOutcome.bApplied = bApplied;
						OutOutcome.Phase = Result.Execution.Phase;
						UeAgentAuthoringOperationApply::FApplyFailureProjection Failure;
						if (UeAgentAuthoringOperationApply::TryProjectApplyFailure(Result, Failure))
						{
							OutOutcome.Phase = Failure.Phase;
							OutOutcome.Diagnostic = Failure.Diagnostic;
						}
						else if (!bApplied) OutOutcome.Diagnostic = ApplyError;
						Asset = nullptr;
						return bApplied;
					},
					[&Target](
						FString& OutReadbackPayload,
						FString& OutRevision,
						FWorkspaceError& OutReadError)
					{
						return TryInternalReadback(
							Target,
							OutReadbackPayload,
							OutRevision,
							OutReadError);
					},
					OutPayload,
					OutError);
			}

			if (Document.ExpectedRevision != TEXT("missing") || !Document.Root.IsValid())
			{
				auto Diagnostic = Document.ExpectedRevision != TEXT("missing")
					? TextureDomainFact(TEXT("texture_graph_semantic_workspace_required"),
						TEXT("An inline TextureGraph import can only initialize a missing target; updates require a semantic workspace."),
						TEXT("/expected_revision"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("command_input"))
					: TextureDomainFact(TEXT("texture_graph_semantic_workspace_required"),
						TEXT("TextureGraph initialization requires a root object."), TEXT("/root"),
						MakeShared<FJsonValueString>(TEXT("<missing>")), MakeShared<FJsonValueString>(TEXT("object")));
				Diagnostic.TargetPath = Target;
				return Fail(OutError, MoveTemp(Diagnostic));
			}
			for (const auto& Field : Document.Root->Values)
				if (Field.Key != TEXT("candidate") && Field.Key != TEXT("graph"))
					{
						auto Diagnostic = TextureDomainFact(TEXT("texture_graph_initial_root_field_unsupported"),
							TEXT("TextureGraph initialization root accepts only candidate and graph fields."), TEXT("/root/") + EscapeJsonPointerSegment(Field.Key), MakeShared<FJsonValueString>(Field.Key), MakeShared<FJsonValueString>(TEXT("candidate or graph")));
						return Fail(OutError, MoveTemp(Diagnostic));
					}
			const TSharedPtr<FJsonObject>* Candidate = nullptr;
			const TSharedPtr<FJsonObject>* InitialGraph = nullptr;
			FDomainHjsonCandidateSelector Selector;
			if (!Document.Root->TryGetObjectField(TEXT("candidate"), Candidate) || !Candidate
				|| !Document.Root->TryGetObjectField(TEXT("graph"), InitialGraph) || !InitialGraph)
				{
					auto Diagnostic = TextureDomainFact(TEXT("texture_graph_initial_document_invalid"),
						TEXT("TextureGraph initialization requires candidate and graph objects."), !Candidate ? TEXT("/root/candidate") : TEXT("/root/graph"), TextureDomainValue(Document.Root->TryGetField(!Candidate ? TEXT("candidate") : TEXT("graph"))), MakeShared<FJsonValueString>(TEXT("object")));
					return Fail(OutError, MoveTemp(Diagnostic));
				}
			if (!TryReadCandidateSelector(*Candidate, TEXT("/root/candidate"), Selector, OutError)) return false;
			DomainAdapters::AssetDataSimpleEffect::FUeAgentAssetDataSimpleEffectFactoryBinding Factory;
			ExecutionMutation::FExecutionMutationError FactoryError;
			if (!Composition::SimpleAuthoring::TryResolveGenericAssetFactory(Selector.Name, Selector.Select, Factory, FactoryError))
			{
				OutError.SetFailure(FactoryError, Document.SourceUnit);
				return false;
			}
			UeAgentAuthoringOperationApply::FBackendOptions Options;
			FString Readback, Revision;
			Options.FinalReadback = [&](UeAgentAuthoringWorkspaceTransaction::FOperationFailure& Failure)
			{
				FWorkspaceError ReadError;
				if (!TryInternalReadback(Target, Readback, Revision, ReadError))
				{ Failure.Diagnostic = ReadError.GetDiagnostic(); return false; }
				TSharedPtr<FJsonObject> Payload; UE::UAI::Contract::FDiagnostic Error;
				if (!Json::TryParseObject(Readback, Payload, Error))
				{ Failure.Diagnostic = MoveTemp(Error); return false; }
				if (!TryValidateReadbackDiagnostics(Payload, Target, TEXT("texture_graph_domain_compile_not_settled"), ReadError))
				{ Failure.Diagnostic = ReadError.GetDiagnostic(); return false; }
				return true;
			};
			UeAgentAuthoringOperationApply::FApplyWorkspaceResult Result; UE::UAI::Contract::FDiagnostic Error; FString AuthorizationId;
			if (!Document.Params->TryGetStringField(TEXT("authorization_id"), AuthorizationId) || AuthorizationId.IsEmpty())
				{
					auto Diagnostic = TextureDomainFact(TEXT("texture_graph_initialization_authorization_missing"),
						TEXT("TextureGraph initialization requires a non-empty impact authorization."), TEXT("/authorization_id"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("command_input"));
					Diagnostic.TargetPath = Target;
					return Fail(OutError, MoveTemp(Diagnostic));
				}
			if (!ApplyInitialGraph(ObjectPathFromPackage(Target), (*InitialGraph).ToSharedRef(), Factory, AuthorizationId,
				Options, Result, Error, Document.AstDocument.Get()))
			{
				OutError.SetFailure(Error, Document.SourceUnit);
				return false;
			}
			OutPayload = MoveTemp(Readback);
			return true;
		}

	}

	bool TryBuildSemanticComposite(
		const FAuthoringModelRef& ResolvedModel,
		const FUeAgentContentGraphCapture& CurrentCapture,
		const UTextureGraph& CurrentAsset,
		const FString& TargetObjectPath,
		Composition::GraphCompound::FTextureGraphSemanticCompositeRequest&
			OutComposite,
		FWorkspaceError& OutError,
		const FResolvedSemanticWorkspace* Workspace)
	{
		TArray<TSharedPtr<FJsonObject>> Nodes;
		TArray<TSharedPtr<FJsonObject>> Links;
		TArray<TSharedPtr<FJsonObject>> Parameters;
		FAudioMediaGraphTextureGraphNodeBindings Bindings;
		const bool bBuilt = TryBuildSemanticTextureGraphComposite(
			ResolvedModel,
			CurrentCapture,
			CurrentAsset,
			Nodes,
			Links,
			Parameters,
			Bindings,
			OutError, Workspace)
			&& TryBuildTypedSemanticTextureGraphComposite(
				TargetObjectPath,
				Nodes,
				Links,
				Parameters,
				OutComposite,
				OutError);
		if (bBuilt) OutComposite.NodeBindings = MoveTemp(Bindings);
		else if (Workspace)
		{
			for (const auto* Unit : ResolvedModel->FindUnitsForSemanticType(TEXT("uai.semantic.texture_graph.graph")))
			{
				if (Unit->GetUnitKind() == ESemanticUnitKind::StructureState)
				{
					LocateTextureSemanticFailure(OutError, Workspace, Unit->GetUnitId());
					break;
				}
			}
		}
		if (!bBuilt && !OutError.GetDiagnostic().IsEmpty())
		{
			auto Diagnostic = OutError.GetDiagnostic();
			if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = TargetObjectPath;
			OutError.SetFailure(MoveTemp(Diagnostic));
		}
		return bBuilt;
	}

	bool TryBindSettingChanges(const Composition::HjsonImport::FPreparedAuthoringImport& Prepared,
		const Composition::HjsonImport::FSemanticWorkspaceChangeProjection& Projection,
		Composition::GraphCompound::FTextureGraphSemanticCompositeRequest& Composite, UE::UAI::Contract::FDiagnostic& Error)
	{
		Composite.Settings.Reset();
		if (!Projection.RootObjectProperties.IsEmpty())
		{
			Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("texture_graph_domain_semantic_property_edit_unsupported"), TEXT("TextureGraph imports accept node setting edits; root object property edits are unsupported."), TEXT("input_decode"));
			const auto& First = Projection.RootObjectProperties[0];
			if (First && First->Type == EJson::Object) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *First->AsObject());
			return false;
		}
		for (const auto& Pair : Projection.PropertyChangesByUnitId)
		{
			const auto* Unit = Prepared.GetWorkspace()->GetModel()->FindUnit(Pair.Key);
			auto FailSetting = [&](const FString& Code, const FString& Reason)
			{
				Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("input_decode"), FString(), Pair.Key);
				if (Pair.Value && Pair.Value->Type == EJson::Object)
					UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Pair.Value->AsObject());
				if (Error.Source.SourceFile.IsEmpty())
					if (const auto* Source = Prepared.GetWorkspace()->GetUnitSources().Find(Pair.Key))
					{
						FWorkspaceError Located; Located.SetFailure(Error); Located.Locate(*Source); Error = Located.GetDiagnostic();
					}
				return false;
			};
			if (!Unit || Unit->GetSemanticTypeRef() != TEXT("uai.semantic.texture_graph.setting"))
				return FailSetting(TEXT("texture_graph_domain_semantic_property_edit_unsupported"), TEXT("The property edit must belong to an existing texture_graph.setting unit."));
			bool Bound = false;
			for (const auto& Change : Prepared.GetChangeSetDraft().GetChanges())
			{
				if (Change.GetIdentity().GetCanonicalKey() != Unit->GetTargetIdentityKey()) continue;
				int32 Index = INDEX_NONE;
				if (!LexTryParseString(Index, *Change.GetIdentity().GetSelector(TEXT("node_index"))) || Index < 0)
					return FailSetting(TEXT("texture_graph_setting_identity_invalid"), TEXT("The setting node_index must be a non-negative integer; received '") + Change.GetIdentity().GetSelector(TEXT("node_index")) + TEXT("'."));
				Composite.Settings.FindOrAdd(Index).Add(Pair.Value); Bound = true; break;
			}
			if (!Bound) return FailSetting(TEXT("texture_graph_setting_identity_unresolved"), TEXT("The setting unit has no matching change identity for its target node."));
		}
		return true;
	}

	bool TryExecuteContentGraph(
		const FString& CommandId,
		const FString& TargetIdentity,
		const FString& CanonicalParamsJson,
		FString& OutPayloadJson,
		bool& bOutReadOnly,
		FWorkspaceError& OutError)
	{
		OutPayloadJson.Reset();
		bOutReadOnly = false;
		OutError.Reset();
		const DomainCapability::OfficialPluginAvailability::
			FOfficialPluginAvailabilityObservation Availability =
				DomainCapability::OfficialPluginAvailability::Inspect(
					TEXT("TextureGraph"));
		if (!Availability.IsAvailable())
		{
			{
				auto Diagnostic = TextureDomainFact(TEXT("texture_graph_official_plugin_unavailable"),
					TEXT("The official TextureGraph plugin is unavailable."), TEXT("/official_plugin"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("native_result"));
				Diagnostic.CandidateName = TEXT("TextureGraph");
			Diagnostic.InternalContext = Availability.GetReason();
				return Fail(OutError, MoveTemp(Diagnostic));
			}
		}
		TSharedPtr<FJsonObject> Params;
		UE::UAI::Contract::FDiagnostic JsonError;
		if (!Json::TryParseObject(CanonicalParamsJson, Params, JsonError))
		{
			return Fail(OutError, MoveTemp(JsonError));
		}
		FString Target;
		if (!Params->TryGetStringField(TEXT("target"), Target)
			|| Target != TargetIdentity)
		{
			{
				auto Diagnostic = TextureDomainFact(TEXT("texture_graph_domain_target_identity_mismatch"),
					TEXT("The command target must match the prepared TextureGraph target."), TEXT("/target"), TextureDomainValue(Params->TryGetField(TEXT("target"))), MakeShared<FJsonValueString>(TargetIdentity), TEXT("command_input"));
				Diagnostic.TargetPath = TargetIdentity;
				return Fail(OutError, MoveTemp(Diagnostic));
			}
		}
		if (CommandId == TEXT("content_graph.import_workspace"))
		{
			return TryImport(
				Target, CanonicalParamsJson, OutPayloadJson, OutError);
		}
		// content_graph.read.* is owned by the shared query service and its
		// ContentGraphRead projections. TextureGraph supplies capture data through
		// the production environment; it does not expose a second read projection.
		{
			auto Diagnostic = TextureDomainFact(TEXT("texture_graph_domain_command_not_supported"),
				TEXT("This TextureGraph domain entry only executes workspace imports."), TEXT("/command"), MakeShared<FJsonValueString>(CommandId), MakeShared<FJsonValueString>(TEXT("content_graph.import_workspace")), TEXT("command_input"));
			return Fail(OutError, MoveTemp(Diagnostic));
		}
	}
}
