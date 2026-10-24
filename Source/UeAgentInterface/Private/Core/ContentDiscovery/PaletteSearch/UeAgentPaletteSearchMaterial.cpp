// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Domains/Material/Authoring/UeAgentMaterialGraphAuthoring.h"

#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchNaming.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "MaterialEditorUtilities.h"
#include "MaterialEditingLibrary.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphNode_Comment.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "Misc/PackageName.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialFunction.h"
#include "ScopedTransaction.h"
#include "SNodePanel.h"

namespace UeAgentPaletteSearch
{
namespace
{
	static FString NormalizeObjectPath(const FString& InPath)
	{
		FString ObjectPath = InPath.TrimStartAndEnd();
		if (ObjectPath.IsEmpty() || ObjectPath.Contains(TEXT(".")))
		{
			return ObjectPath;
		}

		const FString AssetName = FPackageName::GetLongPackageAssetName(ObjectPath);
		if (AssetName.IsEmpty())
		{
			return FString();
		}
		return FString::Printf(TEXT("%s.%s"), *ObjectPath, *AssetName);
	}

	static UObject* LoadObjectFromPath(const FString& InPath)
	{
		const FString ObjectPath = NormalizeObjectPath(InPath);
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}
		if (UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath))
		{
			return Existing;
		}
		return LoadObject<UObject>(nullptr, *ObjectPath);
	}

	static UMaterialGraph* CreateTransientMaterialGraph(
		UMaterial* Material,
		FDiagnostic& OutError)
	{
		if (!Material)
		{
			OutError = FDiagnostic::Failure(TEXT("material_required"), TEXT("A material is required to build the transient candidate graph."), TEXT("candidate_resolve")); OutError.ExpectedValuePresent = true; OutError.ActualValuePresent = false;
			return nullptr;
		}
		UMaterial* PreviewMaterial = Cast<UMaterial>(StaticDuplicateObject(
			Material,
			GetTransientPackage(),
			NAME_None,
			~RF_Standalone,
			Material->GetClass()));
		if (!PreviewMaterial)
		{
			OutError = FDiagnostic::Failure(TEXT("material_preview_duplicate_failed"), TEXT("Unreal could not duplicate the material for a transient candidate graph."), TEXT("candidate_resolve")); OutError.TargetPath = Material->GetPathName(); OutError.SourceKind = TEXT("native_result");
			return nullptr;
		}
		PreviewMaterial->SetFlags(RF_Transactional | RF_Transient);
		UMaterialGraph* Graph = CastChecked<UMaterialGraph>(
			FBlueprintEditorUtils::CreateNewGraph(
				PreviewMaterial,
				NAME_None,
				UMaterialGraph::StaticClass(),
				UMaterialGraphSchema::StaticClass()));
		PreviewMaterial->MaterialGraph = Graph;
		Graph->Material = PreviewMaterial;
		Graph->MaterialFunction = nullptr;
		Graph->OriginalMaterialFullName = Material->GetName();
		Graph->RebuildGraph();
		return Graph;
	}

	static UMaterialGraph* CreateTransientMaterialFunctionGraph(UMaterialFunction* Function, FDiagnostic& OutError)
	{
		if (!Function)
		{
			OutError = FDiagnostic::Failure(TEXT("material_function_required"), TEXT("A material function is required to build the transient candidate graph."), TEXT("candidate_resolve")); OutError.ExpectedValuePresent = true; OutError.ActualValuePresent = false;
			return nullptr;
		}

		UMaterial* PreviewMaterial = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transactional | RF_Transient);
		if (!PreviewMaterial)
		{
			OutError = FDiagnostic::Failure(TEXT("material_function_preview_material_create_failed"), TEXT("Unreal could not create the transient material needed to preview the material function's candidate graph."), TEXT("candidate_resolve")); OutError.TargetPath = Function->GetPathName(); OutError.SourceKind = TEXT("native_result");
			return nullptr;
		}
		PreviewMaterial->SetShadingModel(MSM_Unlit);
		PreviewMaterial->MaterialDomain = Function->PreviewMaterialDomain;

		UMaterialFunction* PreviewFunction = Cast<UMaterialFunction>(
			StaticDuplicateObject(
				Function,
				GetTransientPackage(),
				NAME_None,
				~RF_Standalone,
				Function->GetClass()));
		if (!PreviewFunction)
		{
			OutError = FDiagnostic::Failure(TEXT("material_function_preview_duplicate_failed"), TEXT("Unreal could not duplicate the material function for a transient candidate graph."), TEXT("candidate_resolve")); OutError.TargetPath = Function->GetPathName(); OutError.SourceKind = TEXT("native_result");
			return nullptr;
		}
		PreviewFunction->ParentFunction = Function;
		PreviewFunction->SetFlags(RF_Transactional);

		UMaterialGraph* Graph = CastChecked<UMaterialGraph>(
			FBlueprintEditorUtils::CreateNewGraph(
				PreviewMaterial,
				NAME_None,
				UMaterialGraph::StaticClass(),
				UMaterialGraphSchema::StaticClass()));
		PreviewMaterial->MaterialGraph = Graph;
		Graph->Material = PreviewMaterial;
		Graph->MaterialFunction = PreviewFunction;
		Graph->OriginalMaterialFullName = Function->GetName();
		PreviewFunction->MaterialGraph = Graph;
		PreviewFunction->EditorMaterial = PreviewMaterial;

		PreviewMaterial->AssignExpressionCollection(PreviewFunction->GetExpressionCollection());
		PreviewMaterial->bEnableNewHLSLGenerator = PreviewFunction->IsUsingNewHLSLGenerator();
		for (UMaterialExpression* Expression : PreviewMaterial->GetExpressions())
		{
			if (Expression)
			{
				Expression->Function = nullptr;
				Expression->Material = PreviewMaterial;
			}
		}

		Graph->RebuildGraph();
		return Graph;
	}

	static UMaterialGraph* ResolveMaterialGraph(
		UObject* Asset,
		const bool bRequirePersistentGraph,
		FDiagnostic& OutError)
	{
		if (!Asset)
		{
			OutError = FDiagnostic::Failure(TEXT("material_asset_not_found"), TEXT("The requested material asset could not be loaded."), TEXT("candidate_resolve")); OutError.PropertyPath = TEXT("asset_path");
			return nullptr;
		}

		UMaterialGraph* Graph = nullptr;
		bool bNeedsRebuild = false;
		if (UMaterial* Material = Cast<UMaterial>(Asset))
		{
			if (!bRequirePersistentGraph)
			{
				return CreateTransientMaterialGraph(Material, OutError);
			}
			if (!Material->MaterialGraph)
			{
				Material->MaterialGraph = CastChecked<UMaterialGraph>(
					FBlueprintEditorUtils::CreateNewGraph(
						Material,
						NAME_None,
						UMaterialGraph::StaticClass(),
						UMaterialGraphSchema::StaticClass()));
				bNeedsRebuild = true;
			}
			Graph = Material->MaterialGraph;
			if (Graph)
			{
				Graph->Material = Material;
				Graph->MaterialFunction = nullptr;
				Graph->OriginalMaterialFullName = Material->GetName();
			}
		}
		else if (UMaterialFunction* Function = Cast<UMaterialFunction>(Asset))
		{
			Graph = CreateTransientMaterialFunctionGraph(Function, OutError);
		}
		else
		{
			OutError = FDiagnostic::Failure(TEXT("unsupported_material_asset_type"), TEXT("Candidate discovery requires a material or material function asset."), TEXT("candidate_resolve")); OutError.TargetPath = Asset->GetPathName(); OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("Material or MaterialFunction")); OutError.ActualValue = MakeShared<FJsonValueString>(Asset->GetClass()->GetName());
			return nullptr;
		}

		if (!Graph)
		{
			if (OutError.IsEmpty()) OutError = FDiagnostic::Failure(TEXT("material_graph_not_found"), TEXT("The material asset has no native graph after graph preparation."), TEXT("candidate_resolve"));
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Asset->GetPathName();
			return nullptr;
		}

		if (bNeedsRebuild)
		{
			Graph->RebuildGraph();
		}
		return Graph;
	}

	static UEdGraphNode* FindNodeByGuid(UEdGraph* Graph, const FString& NodeGuidText)
	{
		if (!Graph || NodeGuidText.TrimStartAndEnd().IsEmpty())
		{
			return nullptr;
		}

		FGuid NodeGuid;
		if (!FGuid::Parse(NodeGuidText, NodeGuid))
		{
			return nullptr;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node->NodeGuid == NodeGuid)
			{
				return Node;
			}
		}
		return nullptr;
	}

	static bool PinNameMatches(const UEdGraphPin* Pin, const FString& PinName)
	{
		if (!Pin || PinName.TrimStartAndEnd().IsEmpty())
		{
			return false;
		}

		return Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase)
			|| Pin->GetDisplayName().ToString().Equals(PinName, ESearchCase::IgnoreCase);
	}

	static bool TryResolvePin(
		UEdGraph* Graph,
		UEdGraphNode* Node,
		const FString& PinGuidText,
		const FString& PinName,
		UEdGraphPin*& OutPin,
		FDiagnostic& OutError)
	{
		OutPin = nullptr;
		if (!Graph)
		{
			OutError = FDiagnostic::Failure(TEXT("material_graph_not_found"), TEXT("A material graph is required to resolve the requested pin."), TEXT("candidate_resolve")); OutError.CandidateName = PinName; OutError.PropertyPath = TEXT("graph");
			return false;
		}

		FGuid PinGuid;
		const FString CleanPinGuid = PinGuidText.TrimStartAndEnd();
		const FString CleanPinName = PinName.TrimStartAndEnd();
		const bool bHasPinGuid = FGuid::Parse(CleanPinGuid, PinGuid);
		if (!CleanPinGuid.IsEmpty() && !bHasPinGuid)
		{
			OutError = FDiagnostic::Failure(TEXT("material_pin_guid_invalid"), TEXT("The supplied material pin identity is not a valid GUID."), TEXT("candidate_resolve")); OutError.PropertyPath = TEXT("pin_guid"); OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("GUID")); OutError.ActualEvidence = MakeShared<FJsonValueString>(PinGuidText);
			return false;
		}
		if (!bHasPinGuid && CleanPinName.IsEmpty())
		{
			OutError = FDiagnostic::Failure(TEXT("material_pin_selector_required"), TEXT("A non-empty pin name or pin GUID is required to select a material pin."), TEXT("candidate_resolve")); OutError.PropertyPath = TEXT("pin_name"); OutError.ExpectedValuePresent = true; OutError.ActualValuePresent = false;
			return false;
		}
		auto MatchesPin = [bHasPinGuid, PinGuid, &CleanPinName](UEdGraphPin* Pin)
		{
			return Pin && ((bHasPinGuid && Pin->PinId == PinGuid)
				|| PinNameMatches(Pin, CleanPinName));
		};

		TArray<UEdGraphPin*> Matches;
		if (Node)
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (MatchesPin(Pin))
				{
					Matches.Add(Pin);
				}
			}
		}
		else
		{
			for (UEdGraphNode* GraphNode : Graph->Nodes)
			{
				if (!GraphNode)
				{
					continue;
				}
				for (UEdGraphPin* Pin : GraphNode->Pins)
				{
					if (MatchesPin(Pin))
					{
						Matches.Add(Pin);
					}
				}
			}
		}
		if (Matches.IsEmpty())
		{
			OutError = FDiagnostic::Failure(TEXT("pin_not_found"), TEXT("No pin in the selected material node or graph matches the supplied pin selectors."), TEXT("candidate_resolve")); OutError.CandidateName = PinName; OutError.PropertyPath = bHasPinGuid ? TEXT("pin_guid") : TEXT("pin_name"); OutError.ActualEvidence = MakeShared<FJsonValueString>(PinGuidText); OutError.ActualValue = MakeShared<FJsonValueNumber>(0);
			return false;
		}
		if (Matches.Num() != 1)
		{
			OutError = FDiagnostic::Failure(TEXT("pin_ambiguous"), FString::Printf(TEXT("pin ambiguous: %d"), Matches.Num()), TEXT("candidate_resolve"));
			OutError.CandidateName = PinName; OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1); OutError.ActualValue = MakeShared<FJsonValueNumber>(Matches.Num());
			return false;
		}
		OutPin = Matches[0];
		OutError.Reset();
		return true;
	}

	static bool WantsMaterialExpressions(const FString& CatalogType)
	{
		return CatalogType.Equals(TEXT("material_node")) || CatalogType.Equals(TEXT("material_expression"));
	}

	static bool WantsMaterialFunctions(const FString& CatalogType)
	{
		return CatalogType.Equals(TEXT("material_node")) || CatalogType.Equals(TEXT("material_function")) || CatalogType.Equals(TEXT("material_function_call"));
	}

	static FString BuildActionCategoryPath(const FString& RootCategory, const FEdGraphSchemaAction& Action)
	{
		const FString Category = Action.GetCategory().ToString().TrimStartAndEnd();
		if (Category.IsEmpty())
		{
			return RootCategory;
		}
		return RootCategory + TEXT("/") + Category.Replace(TEXT("|"), TEXT("/"));
	}

	struct FMaterialActionEntry
	{
		FCandidate Candidate;
		TSharedPtr<FEdGraphSchemaAction> Action;
		UMaterialGraph* Graph = nullptr;
		UEdGraphPin* Pin = nullptr;
	};

	static FString BuildNativeActionSignature(
		const FEdGraphSchemaAction& Action,
		const FString& NativeTarget)
	{
		// These supported actions create a class or a function call. Menu placement
		// and localized presentation describe search hits, not another native action.
		return FString::Printf(TEXT("type=%s\ntarget=%s"),
			*Action.GetTypeId().ToString(), *NativeTarget);
	}

	static bool TryBuildActionEntry(
		const FSearchParams& Params,
		const TSharedPtr<FEdGraphSchemaAction>& Action,
		const int32 UiOrder,
		UMaterialGraph* Graph,
		UEdGraphPin* Pin,
		const bool bPersistentMaterialContext,
		FMaterialActionEntry& OutEntry,
		FDiagnostic& OutError)
	{
		OutEntry = {};
		OutError.Reset();
		if (!Action.IsValid())
		{
			return false;
		}

		FCandidate Candidate;
		FString NativeTarget;
		if (Action->GetTypeId() == FMaterialGraphSchemaAction_NewNode::StaticGetTypeId()
			|| Action->GetTypeId() == FMaterialGraphSchemaAction_NewComment::StaticGetTypeId())
		{
			if (!WantsMaterialExpressions(Params.CatalogType))
			{
				return false;
			}

			const bool bComment = Action->GetTypeId() == FMaterialGraphSchemaAction_NewComment::StaticGetTypeId();
			UClass* ExpressionClass = bComment ? UMaterialExpressionComment::StaticClass()
				: StaticCastSharedPtr<FMaterialGraphSchemaAction_NewNode>(Action)->MaterialExpressionClass.Get();
			if (!ExpressionClass)
			{
				return false;
			}

			Candidate.Kind = TEXT("material_expression");
			FString ExpressionName = ExpressionClass->GetName();
			ExpressionName.RemoveFromStart(
				TEXT("MaterialExpression"), ESearchCase::CaseSensitive);
			Candidate.Name = ReadableSemanticName(ExpressionName);
			Candidate.FullName = ExpressionClass->GetPathName();
			Candidate.SourceOrigin = ExpressionClass->GetOutermost()->GetName().StartsWith(TEXT("/Script/")) ? TEXT("engine") : TEXT("project");
			Candidate.SourceLabel = ExpressionClass->GetOutermost()->GetName();
			Candidate.ObjectPath = ExpressionClass->GetPathName();
			Candidate.AuthoringSelectors.Add(
				TEXT("expression_class"), ExpressionClass->GetPathName());
			NativeTarget = ExpressionClass->GetPathName();
		}
		else if (Action->GetTypeId() == FMaterialGraphSchemaAction_NewFunctionCall::StaticGetTypeId())
		{
			if (!WantsMaterialFunctions(Params.CatalogType))
			{
				return false;
			}

			const TSharedPtr<FMaterialGraphSchemaAction_NewFunctionCall> FunctionAction = StaticCastSharedPtr<FMaterialGraphSchemaAction_NewFunctionCall>(Action);
			const FString FunctionPath = FunctionAction.IsValid() ? FunctionAction->FunctionPath : FString();
			if (FunctionPath.IsEmpty())
			{
				return false;
			}

			Candidate.Kind = TEXT("material_function_call");
			FString FunctionName = FPackageName::ObjectPathToObjectName(FunctionPath);
			if (FunctionName.IsEmpty())
			{
				FunctionName = FPackageName::GetLongPackageAssetName(FunctionPath);
			}
			Candidate.Name = ReadableSemanticName(FunctionName);
			Candidate.FullName = FunctionPath;
			Candidate.SourceOrigin = FunctionPath.StartsWith(TEXT("/Engine/")) ? TEXT("engine") : TEXT("project");
			Candidate.SourceLabel = FunctionPath;
			Candidate.PackagePath = FunctionPath;
			Candidate.ObjectPath = FunctionPath;
			Candidate.AuthoringSelectors.Add(
				TEXT("expression_class"),
				UMaterialExpressionMaterialFunctionCall::StaticClass()
					->GetPathName());
			Candidate.AuthoringSelectors.Add(TEXT("function_path"), FunctionPath);
			NativeTarget = FunctionPath;
		}
		else
		{
			return false;
		}

		Candidate.DisplayName = MenuNameFromText(Action->GetMenuDescription(), Candidate.Name);
		Candidate.Tooltip = Action->GetTooltipDescription().ToString();
		Candidate.Keywords = Action->GetKeywords().ToString();
		Candidate.CategoryPath = BuildActionCategoryPath(
			Candidate.Kind == TEXT("material_expression")
				? FString(TEXT("Expressions"))
				: FString(TEXT("Functions")),
			*Action);
		Candidate.Section = Pin ? TEXT("Pin Context") : TEXT("Graph Context");
		Candidate.Type = Candidate.Kind;
		Candidate.WriteSupport = bPersistentMaterialContext
			? TEXT("nodes.<alias>.candidate")
			: TEXT("unsupported");
		Candidate.SupportReason = TEXT("Use candidate.name and the returned candidate.select when needed in the target workspace; the shared Material/Function authoring owner resolves this native Graph/Pin action and creates its expression in the target collection.");
		Candidate.bInLibrary = true;
		Candidate.bSupportedInContext = bPersistentMaterialContext;
		Candidate.UiOrder = UiOrder;
		const FString NativeActionSignature =
			BuildNativeActionSignature(*Action, NativeTarget);
		if (!bPersistentMaterialContext)
		{
			Candidate.SupportDiagnostic = FDiagnostic::Failure(
				TEXT("material_action_candidate_not_executable_in_context"),
				TEXT("This material candidate has no persistent Material or MaterialFunction context. Query with a target asset to obtain an executable authoring context."),
				TEXT("candidate_resolve"), NativeActionSignature);
			Candidate.SupportDiagnostic.CandidateName = Candidate.Name;
			Candidate.SupportDiagnostic.TargetPath = Params.AssetPath;
			Candidate.SupportDiagnostic.SourceKind = TEXT("native_result");
			Candidate.SupportDiagnostic.PropertyPath = TEXT("persistent_material_context");
			Candidate.SupportDiagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
			Candidate.SupportDiagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			Candidate.SupportReason = Candidate.SupportDiagnostic.PublicMessage;
		}
		Candidate.AuthoringSelectors.Add(
			TEXT("native_action_signature"), NativeActionSignature);
		if (!SetCandidateIdentity(
				Params.CatalogType,
				NativeActionSignature,
				Candidate.Name,
				Candidate,
				OutError))
		{
			return false;
		}
		if (Graph)
		{
			Candidate.AuthoringSelectors.Add(
				TEXT("context_graph_name"), Graph->GetName());
		}
		if (Pin)
		{
			if (UEdGraphNode* OwningNode = Pin->GetOwningNode())
			{
				Candidate.AuthoringSelectors.Add(
					TEXT("context_node_guid"),
					OwningNode->NodeGuid.ToString(
						EGuidFormats::DigitsWithHyphensLower));
			}
			Candidate.AuthoringSelectors.Add(
				TEXT("context_pin_guid"),
				Pin->PinId.ToString(EGuidFormats::DigitsWithHyphensLower));
			Candidate.AuthoringSelectors.Add(
				TEXT("context_pin_name"), Pin->PinName.ToString());
		}
		OutEntry.Candidate = MoveTemp(Candidate);
		OutEntry.Action = Action;
		OutEntry.Graph = Graph;
		OutEntry.Pin = Pin;
		return true;
	}

	static bool MatchesExactName(
		const FCandidate& Candidate,
		const FString& Name)
	{
		const FString CleanName = Name.TrimStartAndEnd();
		if (CleanName.IsEmpty())
		{
			return true;
		}
		const auto Matches = [&CleanName](const FString& Value)
		{
			return !Value.IsEmpty()
				&& Value.Equals(CleanName, ESearchCase::IgnoreCase);
		};
		if (Matches(Candidate.Name)
			|| Matches(Candidate.DisplayName)
			|| Matches(Candidate.FullName)
			|| Matches(Candidate.SearchKey)
			|| Matches(Candidate.GetStableId())
			|| Matches(Candidate.ObjectPath))
		{
			return true;
		}
		for (const TPair<FString, FString>& Selector
			: Candidate.AuthoringSelectors)
		{
			if (Matches(Selector.Value))
			{
				return true;
			}
		}
		return false;
	}

	static bool CollectMaterialActionEntries(
		const FSearchParams& Params,
		const bool bRequirePersistentMaterialGraph,
		TArray<FMaterialActionEntry>& OutEntries,
		TArray<FString>& OutWarnings,
		FDiagnostic& OutError)
	{
		OutEntries.Reset();
		if (!Params.CatalogType.Equals(TEXT("material_node"))
			&& !Params.CatalogType.Equals(TEXT("material_expression"))
			&& !Params.CatalogType.Equals(TEXT("material_function"))
			&& !Params.CatalogType.Equals(TEXT("material_function_call")))
		{
			OutError = FDiagnostic::Failure(TEXT("catalog_type_unsupported"), FString::Printf(TEXT("unsupported Material catalog_type '%s'."), *Params.CatalogType), TEXT("candidate_resolve"));
			return false;
		}

		const UMaterialGraphSchema* Schema =
			UMaterialGraphSchema::StaticClass()
				->GetDefaultObject<UMaterialGraphSchema>();
		if (!Schema)
		{
			OutError = FDiagnostic::Failure(TEXT("material_graph_schema_unavailable"), TEXT("Unreal did not provide the default native material graph schema."), TEXT("candidate_resolve")); OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("MaterialGraphSchema default object")); OutError.ActualValuePresent = false; OutError.SourceKind = TEXT("native_result");
			return false;
		}

		UObject* Asset = nullptr;
		const UObject* MaterialContext = UMaterial::StaticClass()->GetDefaultObject();
		UMaterialGraph* ContextGraph = nullptr;
		UEdGraphPin* ContextPin = nullptr;
		bool bPersistentMaterialContext = false;
		if (!Params.AssetPath.IsEmpty())
		{
			Asset = LoadObjectFromPath(Params.AssetPath);
			ContextGraph = ResolveMaterialGraph(
				Asset, bRequirePersistentMaterialGraph, OutError);
			if (!ContextGraph)
			{
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Params.AssetPath;
				return false;
			}
			const bool bMatchesSemanticGraphName =
				(Cast<UMaterial>(Asset)
					&& Params.GraphName.Equals(
						TEXT("MaterialGraph"), ESearchCase::IgnoreCase))
				|| (Cast<UMaterialFunction>(Asset)
					&& Params.GraphName.Equals(
						TEXT("MaterialFunctionGraph"),
						ESearchCase::IgnoreCase));
			if (!Params.GraphName.IsEmpty()
				&& !ContextGraph->GetName().Equals(
					Params.GraphName, ESearchCase::IgnoreCase)
				&& !ContextGraph->GetPathName().Equals(
					Params.GraphName, ESearchCase::IgnoreCase)
				&& !bMatchesSemanticGraphName)
			{
				OutError = FDiagnostic::Failure(TEXT("material_graph_not_found"), TEXT("The requested graph selector does not match the material asset's graph."), TEXT("candidate_resolve")); OutError.TargetPath = Params.AssetPath; OutError.PropertyPath = TEXT("graph_name"); OutError.ActualValue = MakeShared<FJsonValueString>(Params.GraphName); OutError.ExpectedValue = MakeShared<FJsonValueString>(Cast<UMaterial>(Asset) ? TEXT("MaterialGraph") : TEXT("MaterialFunctionGraph"));
				return false;
			}
			const bool bHasPinSelector = !Params.PinGuid.TrimStartAndEnd().IsEmpty()
				|| !Params.PinName.TrimStartAndEnd().IsEmpty();
			if (!Params.NodeGuid.TrimStartAndEnd().IsEmpty()
				&& !bHasPinSelector)
			{
				OutError = FDiagnostic::Failure(TEXT("material_pin_selector_required"), TEXT("A node-specific material candidate query also requires a pin name or pin GUID."), TEXT("candidate_resolve")); OutError.TargetPath = Params.AssetPath; OutError.PropertyPath = TEXT("pin_name"); OutError.ExpectedValuePresent = true; OutError.ActualValuePresent = false;
				return false;
			}
			if (bHasPinSelector)
			{
				UEdGraphNode* Node = nullptr;
				if (!Params.NodeGuid.TrimStartAndEnd().IsEmpty())
				{
					FGuid NodeGuid;
					if (!FGuid::Parse(Params.NodeGuid, NodeGuid))
					{
						OutError = FDiagnostic::Failure(TEXT("material_node_guid_invalid"), TEXT("The supplied material node identity is not a valid GUID."), TEXT("candidate_resolve")); OutError.TargetPath = Params.AssetPath; OutError.PropertyPath = TEXT("node_guid"); OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("GUID")); OutError.ActualEvidence = MakeShared<FJsonValueString>(Params.NodeGuid);
						return false;
					}
					Node = FindNodeByGuid(ContextGraph, Params.NodeGuid);
					if (!Node)
					{
						OutError = FDiagnostic::Failure(TEXT("material_node_not_found"), TEXT("The selected material graph has no node with the supplied identity."), TEXT("candidate_resolve")); OutError.TargetPath = Params.AssetPath; OutError.PropertyPath = TEXT("node_guid"); OutError.CandidateName = Params.GraphName; OutError.ActualEvidence = MakeShared<FJsonValueString>(Params.NodeGuid);
						return false;
					}
				}
				if (!TryResolvePin(
						ContextGraph,
						Node,
						Params.PinGuid,
						Params.PinName,
						ContextPin,
						OutError))
				{
					return false;
				}
			}
			MaterialContext = ContextGraph->GetMaterialOrFunction();
			bPersistentMaterialContext = Cast<UMaterial>(Asset) != nullptr || Cast<UMaterialFunction>(Asset) != nullptr;
		}
		else
		{
			OutWarnings.Add(TEXT(
				"Material search ran without asset_path; the result is a generic "
				"palette projection and is not executable HJSON context."));
		}

		TArray<TSharedPtr<FEdGraphSchemaAction>> Actions;
		if (ContextGraph)
		{
			FGraphContextMenuBuilder ActionBuilder(ContextGraph);
			ActionBuilder.FromPin = ContextPin;
			Schema->GetGraphContextActions(ActionBuilder);
			for (int32 Index = 0; Index < ActionBuilder.GetNumActions(); ++Index)
			{
				Actions.Add(ActionBuilder.GetSchemaAction(Index));
			}
		}
		else
		{
			FGraphActionMenuBuilder ActionBuilder;
			Schema->GetPaletteActions(ActionBuilder, FString(), MaterialContext);
			for (int32 Index = 0; Index < ActionBuilder.GetNumActions(); ++Index)
			{
				Actions.Add(ActionBuilder.GetSchemaAction(Index));
			}
		}

		for (int32 Index = 0; Index < Actions.Num(); ++Index)
		{
			FMaterialActionEntry Entry;
			FDiagnostic CandidateError;
			if (TryBuildActionEntry(
					Params,
					Actions[Index],
					Index,
					ContextGraph,
					ContextPin,
					bPersistentMaterialContext,
					Entry,
					CandidateError))
			{
				OutEntries.Add(MoveTemp(Entry));
			}
			else if (!CandidateError.IsEmpty())
			{
				OutError = MoveTemp(CandidateError);
				return false;
			}
		}
		if (OutEntries.IsEmpty())
		{
			OutWarnings.Add(ContextPin
				? TEXT("Material pin context returned no expression/function creation actions.")
				: TEXT("Material graph context returned no expression/function creation actions."));
		}
		OutError.Reset();
		return true;
	}

	static bool TryResolveMaterialActionEntry(
		const FSearchParams& Params,
		const FString& Name,
		const TMap<FString, FString>& Select,
		const FString& Id,
		const bool bRequirePersistentMaterialGraph,
		FMaterialActionEntry& OutEntry,
		FDiagnostic& OutError,
		const UE::UAI::ContentDiscovery::FNameSelectionInputContext* InputContext = nullptr)
	{
		OutEntry = {};
		const FString CleanName = Name.TrimStartAndEnd();
		const FString CleanId = Id.TrimStartAndEnd();
		if (!CleanId.IsEmpty())
		{
			OutError = FDiagnostic::Failure(TEXT("material_action_candidate_machine_selector_not_supported"), TEXT("Material candidates must be selected by their public name and semantic fields; a machine identity selector is unsupported."), TEXT("candidate_resolve")); OutError.TargetPath = Params.AssetPath; OutError.PropertyPath = TEXT("id"); OutError.ActualEvidence = MakeShared<FJsonValueString>(Id); OutError.ActualValuePresent = true; OutError.ExpectedValuePresent = false;
			return false;
		}
		if (CleanName.IsEmpty())
		{
			OutError = FDiagnostic::Failure(TEXT("material_action_candidate_selector_required"), TEXT("A non-empty public material candidate name is required."), TEXT("candidate_resolve")); OutError.TargetPath = Params.AssetPath; OutError.PropertyPath = TEXT("name"); OutError.ExpectedValuePresent = true; OutError.ActualValuePresent = false;
			return false;
		}
		TArray<FMaterialActionEntry> Entries;
		TArray<FString> Warnings;
		if (!CollectMaterialActionEntries(
				Params,
				bRequirePersistentMaterialGraph,
				Entries,
				Warnings,
				OutError))
		{
			return false;
		}
		TArray<UE::UAI::ContentDiscovery::FNameSelectionEntry> SelectionEntries;
		SelectionEntries.Reserve(Entries.Num());
		for (const FMaterialActionEntry& Entry : Entries)
		{
			UE::UAI::ContentDiscovery::FNameSelectionEntry SelectionEntry;
			SelectionEntry.Name = Entry.Candidate.Name;
			SelectionEntry.Identity = Entry.Candidate.Identity.CanonicalKey;
			SelectionEntry.Select.Add(
				TEXT("form"),
				Entry.Candidate.Kind.Equals(TEXT("material_function_call"), ESearchCase::IgnoreCase)
					? TEXT("function") : TEXT("expression"));
			const FString Source = UE::UAI::ContentDiscovery::MakeReadableDiscoveryPath(Entry.Candidate.ObjectPath);
			if (!Source.IsEmpty()) SelectionEntry.Select.Add(TEXT("source"), Source);
			SelectionEntries.Add(MoveTemp(SelectionEntry));
		}
		int32 ResolvedIndex = INDEX_NONE;
		if (!UE::UAI::ContentDiscovery::TryResolveNameSelection(
				SelectionEntries, CleanName, Select,
				ResolvedIndex, OutError, UE::UAI::ContentDiscovery::ENameComparison::CaseSensitive, InputContext)
			|| !Entries.IsValidIndex(ResolvedIndex))
		{
			OutError = UeAgentAuthoringDiagnostics::CandidateFailure(OutError.Code, CleanName,
				Params.AssetPath, FString::Printf(TEXT("material graph '%s', pin '%s'"),
					Params.GraphName.IsEmpty() ? TEXT("default") : *Params.GraphName,
					Params.PinName.IsEmpty() ? TEXT("none (background menu)") : *Params.PinName),
				TEXT("material.expression.candidates"), OutError);
			return false;
		}
		OutEntry = Entries[ResolvedIndex];
		OutError.Reset();
		return true;
	}
}

bool CollectMaterialCandidates(const FSearchParams& Params, TArray<FCandidate>& OutCandidates, TArray<FString>& OutWarnings, FDiagnostic& OutError)
{
	OutCandidates.Reset();
	TArray<FMaterialActionEntry> Entries;
	if (!CollectMaterialActionEntries(
			Params, false, Entries, OutWarnings, OutError))
	{
		return false;
	}
	for (FMaterialActionEntry& Entry : Entries)
	{
		OutCandidates.Add(MoveTemp(Entry.Candidate));
	}
	return true;
}

bool TryResolveMaterialActionCandidate(
	const FSearchParams& Params,
	const FString& Name,
	const TMap<FString, FString>& Select,
	const FString& Id,
	FCandidate& OutCandidate,
	FDiagnostic& OutError,
	const UE::UAI::ContentDiscovery::FNameSelectionInputContext* InputContext)
{
	FMaterialActionEntry Entry;
	if (!TryResolveMaterialActionEntry(
			Params, Name, Select, Id, false, Entry, OutError, InputContext))
	{
		OutCandidate = {};
		return false;
	}
	OutCandidate = MoveTemp(Entry.Candidate);
	return true;
}

bool TryPerformMaterialActionCandidate(
	const FSearchParams& Params,
	const FString& Name,
	const TMap<FString, FString>& Select,
	const FString& Id,
	const FVector2f& Location,
	UMaterialExpression*& OutExpression,
	FCandidate& OutCandidate,
	FDiagnostic& OutError)
{
	OutExpression = nullptr;
	FMaterialActionEntry Entry;
	if (!TryResolveMaterialActionEntry(
			Params, Name, Select, Id, true, Entry, OutError))
	{
		OutCandidate = {};
		return false;
	}
	if (!Entry.Candidate.bSupportedInContext)
	{
		OutError = FDiagnostic::WrapFailure(Entry.Graph && Entry.Graph->MaterialFunction
			? TEXT("material_function_native_action_lifecycle_unsupported")
			: TEXT("material_action_candidate_not_executable_in_context"), Entry.Candidate.SupportDiagnostic);
		return false;
	}
	if (!Entry.Action.IsValid() || !Entry.Graph)
	{
		OutError = FDiagnostic::Failure(TEXT("material_action_candidate_not_executable_in_context"), !Entry.Action.IsValid() ? TEXT("The selected material candidate has no native action to execute.") : TEXT("The selected material action has no target graph."), TEXT("candidate_resolve")); OutError.CandidateName = Name; OutError.TargetPath = Params.AssetPath;
		return false;
	}
	UMaterial* const Material = Entry.Graph->Material;
	if (!Material || Entry.Graph->MaterialFunction)
	{
		OutError = FDiagnostic::Failure(TEXT("material_action_candidate_persistent_owner_required"), !Material ? TEXT("The material candidate graph has no material owner.") : TEXT("Native action execution requires a persistent material owner; this graph belongs to a transient material-function preview."), TEXT("candidate_resolve")); OutError.CandidateName = Name; OutError.TargetPath = Params.AssetPath;
		return false;
	}
	TSet<UEdGraphNode*> ExistingNodes;
	for (UEdGraphNode* ExistingNode : Entry.Graph->Nodes)
	{
		if (ExistingNode)
		{
			ExistingNodes.Add(ExistingNode);
		}
	}
	const auto RemoveFailedExpression =
		[Material](UMaterialExpression* Expression)
		{
			if (!Expression)
			{
				return;
			}
			if (UEdGraphNode* const GraphNode = Expression->GraphNode)
			{
				GraphNode->BreakAllNodeLinks();
				FBlueprintEditorUtils::RemoveNode(nullptr, GraphNode, true);
			}
			UMaterialEditingLibrary::DeleteMaterialExpression(
				Material, Expression);
			if (auto* Comment = Cast<UMaterialExpressionComment>(Expression))
				Material->GetExpressionCollection().RemoveComment(Comment);
		};

	// UE's public PerformAction path delegates creation to an already-open
	// IMaterialEditor and returns null in headless or background operation.
	// Execute the exact re-enumerated action through the same official lower
	// primitives so authoring never has to open, focus, or depend on user UI.
	const FScopedTransaction Transaction(NSLOCTEXT(
		"UnrealEd", "MaterialEditorNewExpression",
		"Material Editor: New Expression"));
	Material->Modify();
	Entry.Graph->Modify();
	UMaterialExpression* CreatedExpression = nullptr;
	if (Entry.Action->GetTypeId() == FMaterialGraphSchemaAction_NewComment::StaticGetTypeId())
	{
		auto* Comment = Cast<UMaterialExpressionComment>(UeAgentMaterialAuthoring::CreateExpression(
			*Material, UMaterialExpressionComment::StaticClass(),
			FMath::RoundToInt(Location.X), FMath::RoundToInt(Location.Y)));
		if (!Comment || !Entry.Graph->AddComment(Comment, false))
		{
			OutError = FDiagnostic::Failure(TEXT("material_action_candidate_create_failed"), !Comment ? TEXT("Unreal could not create the material comment expression.") : TEXT("The material graph rejected the new comment expression."), TEXT("candidate_apply")); OutError.CandidateName = Name; OutError.TargetPath = Params.AssetPath; OutError.SourceKind = TEXT("native_result");
			RemoveFailedExpression(Comment);
			return false;
		}
		CreatedExpression = Comment;
	}
	else if (Entry.Action->GetTypeId()
		== FMaterialGraphSchemaAction_NewNode::StaticGetTypeId())
	{
		const TSharedPtr<FMaterialGraphSchemaAction_NewNode> NewNodeAction =
			StaticCastSharedPtr<FMaterialGraphSchemaAction_NewNode>(Entry.Action);
		UClass* const ExpressionClass = NewNodeAction.IsValid()
			? NewNodeAction->MaterialExpressionClass.Get()
			: nullptr;
		if (!ExpressionClass)
		{
			OutError = FDiagnostic::Failure(TEXT("material_action_expression_class_missing"), TEXT("The selected native material action has no expression class to instantiate."), TEXT("candidate_resolve")); OutError.CandidateName = Name; OutError.TargetPath = Params.AssetPath; OutError.SourceKind = TEXT("native_result");
			return false;
		}
		CreatedExpression = UMaterialEditingLibrary::CreateMaterialExpressionEx(
			Material,
			nullptr,
			ExpressionClass,
			nullptr,
			FMath::RoundToInt(Location.X),
			FMath::RoundToInt(Location.Y),
			true);
		UMaterialGraphNode* const CreatedMaterialNode = CreatedExpression
			? Entry.Graph->AddExpression(CreatedExpression, false)
			: nullptr;
		if (!CreatedMaterialNode)
		{
			OutError = FDiagnostic::Failure(TEXT("material_action_candidate_create_failed"), !CreatedExpression ? TEXT("Unreal could not instantiate the material expression class.") : TEXT("The material graph rejected the new expression."), TEXT("candidate_apply")); OutError.CandidateName = Name; OutError.TargetPath = Params.AssetPath; OutError.ExpectedValue = MakeShared<FJsonValueString>(ExpressionClass->GetName()); OutError.SourceKind = TEXT("native_result");
			RemoveFailedExpression(CreatedExpression);
			return false;
		}
		CreatedMaterialNode->SnapToGrid(SNodePanel::GetSnapGridSize());
		CreatedExpression->MaterialExpressionEditorX =
			CreatedMaterialNode->NodePosX;
		CreatedExpression->MaterialExpressionEditorY =
			CreatedMaterialNode->NodePosY;
		NewNodeAction->PostCreationDelegate.ExecuteIfBound(CreatedExpression);
		if (ExpressionClass
			== UMaterialExpressionFunctionInput::StaticClass()
			&& Entry.Pin)
		{
			NewNodeAction->SetFunctionInputType(
				CastChecked<UMaterialExpressionFunctionInput>(CreatedExpression),
				UMaterialGraphSchema::GetMaterialValueType(Entry.Pin));
		}
		CreatedMaterialNode->AutowireNewNode(Entry.Pin);
	}
	else if (Entry.Action->GetTypeId()
		== FMaterialGraphSchemaAction_NewFunctionCall::StaticGetTypeId())
	{
		const TSharedPtr<FMaterialGraphSchemaAction_NewFunctionCall>
			FunctionAction = StaticCastSharedPtr<
				FMaterialGraphSchemaAction_NewFunctionCall>(Entry.Action);
		UMaterialFunction* const Function = FunctionAction.IsValid()
			? LoadObject<UMaterialFunction>(
				nullptr, *FunctionAction->FunctionPath)
			: nullptr;
		UMaterialExpressionMaterialFunctionCall* const FunctionCall =
			Function
				? Cast<UMaterialExpressionMaterialFunctionCall>(
					UMaterialEditingLibrary::CreateMaterialExpressionEx(
						Material,
						nullptr,
						UMaterialExpressionMaterialFunctionCall::StaticClass(),
						nullptr,
						FMath::RoundToInt(Location.X),
						FMath::RoundToInt(Location.Y),
						true))
				: nullptr;
		UMaterialGraphNode* const CreatedMaterialNode = FunctionCall
			? Entry.Graph->AddExpression(FunctionCall, false)
			: nullptr;
		if (!FunctionCall || !CreatedMaterialNode
			|| !FunctionCall->SetMaterialFunction(Function))
		{
			OutError = FDiagnostic::Failure(TEXT("material_function_action_create_failed"), !Function ? TEXT("The material function selected by the native action could not be loaded.") : !FunctionCall ? TEXT("Unreal could not create the material-function call expression.") : !CreatedMaterialNode ? TEXT("The material graph rejected the new function-call expression.") : TEXT("The new call expression rejected the selected material function."), TEXT("candidate_apply"));
			OutError.CandidateName = Name; OutError.TargetPath = Params.AssetPath; OutError.ActualValue = MakeShared<FJsonValueString>(FunctionAction.IsValid() ? FunctionAction->FunctionPath : FString()); OutError.SourceKind = TEXT("native_result");
			RemoveFailedExpression(FunctionCall);
			return false;
		}
		CreatedMaterialNode->SnapToGrid(SNodePanel::GetSnapGridSize());
		FunctionCall->MaterialExpressionEditorX = CreatedMaterialNode->NodePosX;
		FunctionCall->MaterialExpressionEditorY = CreatedMaterialNode->NodePosY;
		FunctionCall->PostEditChange();
		FMaterialEditorUtilities::UpdateSearchResults(Entry.Graph);
		CreatedMaterialNode->AutowireNewNode(Entry.Pin);
		CreatedExpression = FunctionCall;
	}
	else
	{
		OutError = FDiagnostic::Failure(TEXT("material_action_type_not_executable"), TEXT("The selected native material action is not a supported comment, expression or function-call action."), TEXT("candidate_resolve")); OutError.CandidateName = Name; OutError.TargetPath = Params.AssetPath; OutError.ActualValue = MakeShared<FJsonValueString>(Entry.Action->GetTypeId().ToString());
		return false;
	}
	Entry.Graph->LinkMaterialExpressionsFromGraph();
	Entry.Graph->NotifyGraphChanged();
	Material->MarkPackageDirty();
	UEdGraphNode* MaterialNode = CreatedExpression
		? CreatedExpression->GraphNode
		: nullptr;
	if (!MaterialNode || !CreatedExpression)
	{
		OutError = FDiagnostic::Failure(TEXT("material_action_candidate_create_failed"), !CreatedExpression ? TEXT("The native material action completed without an expression.") : TEXT("The created material expression has no graph node after graph synchronization."), TEXT("candidate_apply")); OutError.CandidateName = Name; OutError.TargetPath = Params.AssetPath; OutError.SourceKind = TEXT("native_result");
		return false;
	}
	TArray<UEdGraphNode*> CreatedNodes;
	for (UEdGraphNode* CurrentNode : Entry.Graph->Nodes)
	{
		if (CurrentNode && !ExistingNodes.Contains(CurrentNode))
		{
			CreatedNodes.Add(CurrentNode);
		}
	}
	if (CreatedNodes.Num() != 1 || CreatedNodes[0] != MaterialNode)
	{
		OutError = FDiagnostic::Failure(TEXT("material_action_candidate_result_shape_unsupported"), FString::Printf(TEXT("material action candidate result shape unsupported: %d"), CreatedNodes.Num()), TEXT("candidate_resolve"));
		return false;
	}
	OutExpression = CreatedExpression;
	OutCandidate = MoveTemp(Entry.Candidate);
	OutError.Reset();
	return true;
}
}
