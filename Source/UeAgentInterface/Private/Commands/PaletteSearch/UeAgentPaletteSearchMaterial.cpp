// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"

#include "Commands/PaletteSearch/UeAgentPaletteSearchNaming.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "MaterialEditorUtilities.h"
#include "MaterialEditingLibrary.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "Misc/PackageName.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
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

	static UMaterialGraph* CreateTransientMaterialFunctionGraph(UMaterialFunction* Function, FString& OutError)
	{
		if (!Function)
		{
			OutError = TEXT("material_function_required");
			return nullptr;
		}

		UMaterial* PreviewMaterial = NewObject<UMaterial>(GetTransientPackage(), NAME_None, RF_Transactional | RF_Transient);
		if (!PreviewMaterial)
		{
			OutError = TEXT("material_function_preview_material_create_failed");
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
			OutError = TEXT("material_function_preview_duplicate_failed");
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

	static UMaterialGraph* ResolveMaterialGraph(UObject* Asset, FString& OutError)
	{
		if (!Asset)
		{
			OutError = TEXT("material_asset_not_found");
			return nullptr;
		}

		UMaterialGraph* Graph = nullptr;
		bool bNeedsRebuild = false;
		if (UMaterial* Material = Cast<UMaterial>(Asset))
		{
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
			OutError = TEXT("unsupported_material_asset_type");
			return nullptr;
		}

		if (!Graph)
		{
			OutError = TEXT("material_graph_not_found");
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
		FString& OutError)
	{
		OutPin = nullptr;
		if (!Graph)
		{
			OutError = TEXT("material_graph_not_found");
			return false;
		}

		FGuid PinGuid;
		const FString CleanPinGuid = PinGuidText.TrimStartAndEnd();
		const FString CleanPinName = PinName.TrimStartAndEnd();
		const bool bHasPinGuid = FGuid::Parse(CleanPinGuid, PinGuid);
		if (!CleanPinGuid.IsEmpty() && !bHasPinGuid)
		{
			OutError = TEXT("material_pin_guid_invalid");
			return false;
		}
		if (!bHasPinGuid && CleanPinName.IsEmpty())
		{
			OutError = TEXT("material_pin_selector_required");
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
			OutError = TEXT("pin_not_found");
			return false;
		}
		if (Matches.Num() != 1)
		{
			OutError = FString::Printf(TEXT("pin_ambiguous:%d"), Matches.Num());
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
		return FString::Printf(
			TEXT("type=%s\ntarget=%s\nmenu=%s\ncategory=%s\ntooltip=%s\n")
			TEXT("keywords=%s\ngrouping=%d\nsection=%d"),
			*Action.GetTypeId().ToString(),
			*NativeTarget,
			*Action.GetMenuDescription().ToString(),
			*Action.GetCategory().ToString(),
			*Action.GetTooltipDescription().ToString(),
			*Action.GetKeywords().ToString(),
			Action.GetGrouping(),
			Action.GetSectionID());
	}

	static bool TryBuildActionEntry(
		const FSearchParams& Params,
		const TSharedPtr<FEdGraphSchemaAction>& Action,
		const int32 UiOrder,
		UMaterialGraph* Graph,
		UEdGraphPin* Pin,
		const bool bPersistentMaterialContext,
		FMaterialActionEntry& OutEntry,
		FString& OutError)
	{
		OutEntry = {};
		OutError.Reset();
		if (!Action.IsValid())
		{
			return false;
		}

		FCandidate Candidate;
		FString NativeTarget;
		if (Action->GetTypeId() == FMaterialGraphSchemaAction_NewNode::StaticGetTypeId())
		{
			if (!WantsMaterialExpressions(Params.CatalogType))
			{
				return false;
			}

			const TSharedPtr<FMaterialGraphSchemaAction_NewNode> NewNodeAction = StaticCastSharedPtr<FMaterialGraphSchemaAction_NewNode>(Action);
			UClass* ExpressionClass = NewNodeAction.IsValid() ? NewNodeAction->MaterialExpressionClass.Get() : nullptr;
			if (!ExpressionClass)
			{
				return false;
			}

			Candidate.Kind = TEXT("material_expression");
			Candidate.Name = MenuNameFromText(Action->GetMenuDescription(), ExpressionClass->GetName());
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
			Candidate.Name = MenuNameFromText(Action->GetMenuDescription(), FunctionPath);
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

		Candidate.DisplayName = Candidate.Name;
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
			? TEXT("material.graph.edits[].value.candidate")
			: TEXT("unsupported");
		Candidate.SupportReason = bPersistentMaterialContext
			? TEXT("Use candidate.name or candidate.id; the server re-enumerates this native Material action in the same Graph/Pin context and invokes PerformAction.")
			: TEXT("Material Function native actions are searchable, but their transient preview/apply persistence lifecycle is not yet supported for HJSON creation.");
		Candidate.bInLibrary = true;
		Candidate.bSupportedInContext = bPersistentMaterialContext;
		Candidate.UiOrder = UiOrder;
		const FString NativeActionSignature =
			BuildNativeActionSignature(*Action, NativeTarget);
		Candidate.AuthoringSelectors.Add(
			TEXT("native_action_signature"), NativeActionSignature);
		if (!SetCandidateIdentity(
				Params.CatalogType,
				NativeActionSignature,
				Candidate.Name,
				Candidate,
				OutError))
		{
			OutError = TEXT("material_action_candidate_identity_failed:") + OutError;
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
		TArray<FMaterialActionEntry>& OutEntries,
		TArray<FString>& OutWarnings,
		FString& OutError)
	{
		OutEntries.Reset();
		if (!Params.CatalogType.Equals(TEXT("material_node"))
			&& !Params.CatalogType.Equals(TEXT("material_expression"))
			&& !Params.CatalogType.Equals(TEXT("material_function"))
			&& !Params.CatalogType.Equals(TEXT("material_function_call")))
		{
			OutError = FString::Printf(
				TEXT("unsupported Material catalog_type '%s'."),
				*Params.CatalogType);
			return false;
		}

		const UMaterialGraphSchema* Schema =
			UMaterialGraphSchema::StaticClass()
				->GetDefaultObject<UMaterialGraphSchema>();
		if (!Schema)
		{
			OutError = TEXT("material_graph_schema_unavailable");
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
			ContextGraph = ResolveMaterialGraph(Asset, OutError);
			if (!ContextGraph)
			{
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
				OutError = TEXT("material_graph_not_found");
				return false;
			}
			const bool bHasPinSelector = !Params.PinGuid.TrimStartAndEnd().IsEmpty()
				|| !Params.PinName.TrimStartAndEnd().IsEmpty();
			if (!Params.NodeGuid.TrimStartAndEnd().IsEmpty()
				&& !bHasPinSelector)
			{
				OutError = TEXT("material_pin_selector_required");
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
						OutError = TEXT("material_node_guid_invalid");
						return false;
					}
					Node = FindNodeByGuid(ContextGraph, Params.NodeGuid);
					if (!Node)
					{
						OutError = TEXT("material_node_not_found");
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
			bPersistentMaterialContext = Cast<UMaterial>(Asset) != nullptr;
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
			FString CandidateError;
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
		if (Cast<UMaterialFunction>(Asset))
		{
			OutWarnings.Add(TEXT(
				"Material Function candidates are read-only until the transient "
				"preview/apply persistence lifecycle is implemented."));
		}
		OutError.Reset();
		return true;
	}

	static bool TryResolveMaterialActionEntry(
		const FSearchParams& Params,
		const FString& Name,
		const FString& Id,
		FMaterialActionEntry& OutEntry,
		FString& OutError)
	{
		OutEntry = {};
		const FString CleanName = Name.TrimStartAndEnd();
		const FString CleanId = Id.TrimStartAndEnd();
		if (CleanName.IsEmpty() && CleanId.IsEmpty())
		{
			OutError = TEXT("material_action_candidate_selector_required");
			return false;
		}
		TArray<FMaterialActionEntry> Entries;
		TArray<FString> Warnings;
		if (!CollectMaterialActionEntries(
				Params, Entries, Warnings, OutError))
		{
			return false;
		}
		TArray<int32> Matches;
		for (int32 Index = 0; Index < Entries.Num(); ++Index)
		{
			const FCandidate& Candidate = Entries[Index].Candidate;
			const bool bIdMatches = CleanId.IsEmpty()
				|| Candidate.GetPublicId().Equals(
					CleanId, ESearchCase::IgnoreCase)
				|| Candidate.GetCandidateId().Equals(
					CleanId, ESearchCase::CaseSensitive);
			if (bIdMatches && MatchesExactName(Candidate, CleanName))
			{
				Matches.Add(Index);
			}
		}
		if (Matches.IsEmpty())
		{
			OutError = TEXT("material_action_candidate_not_found_in_native_context");
			return false;
		}
		if (Matches.Num() != 1)
		{
			OutError = FString::Printf(
				TEXT("material_action_candidate_ambiguous:%d"), Matches.Num());
			return false;
		}
		OutEntry = Entries[Matches[0]];
		OutError.Reset();
		return true;
	}
}

bool CollectMaterialCandidates(const FSearchParams& Params, TArray<FCandidate>& OutCandidates, TArray<FString>& OutWarnings, FString& OutError)
{
	OutCandidates.Reset();
	TArray<FMaterialActionEntry> Entries;
	if (!CollectMaterialActionEntries(
			Params, Entries, OutWarnings, OutError))
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
	const FString& Id,
	FCandidate& OutCandidate,
	FString& OutError)
{
	FMaterialActionEntry Entry;
	if (!TryResolveMaterialActionEntry(
			Params, Name, Id, Entry, OutError))
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
	const FString& Id,
	const FVector2f& Location,
	UMaterialExpression*& OutExpression,
	FCandidate& OutCandidate,
	FString& OutError)
{
	OutExpression = nullptr;
	FMaterialActionEntry Entry;
	if (!TryResolveMaterialActionEntry(
			Params, Name, Id, Entry, OutError))
	{
		OutCandidate = {};
		return false;
	}
	if (!Entry.Candidate.bSupportedInContext)
	{
		OutError = Entry.Graph && Entry.Graph->MaterialFunction
			? TEXT("material_function_native_action_lifecycle_unsupported")
			: TEXT("material_action_candidate_not_executable_in_context");
		return false;
	}
	if (!Entry.Action.IsValid() || !Entry.Graph)
	{
		OutError = TEXT("material_action_candidate_not_executable_in_context");
		return false;
	}
	UMaterial* const Material = Entry.Graph->Material;
	if (!Material || Entry.Graph->MaterialFunction)
	{
		OutError = TEXT("material_action_candidate_persistent_owner_required");
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
	if (Entry.Action->GetTypeId()
		== FMaterialGraphSchemaAction_NewNode::StaticGetTypeId())
	{
		const TSharedPtr<FMaterialGraphSchemaAction_NewNode> NewNodeAction =
			StaticCastSharedPtr<FMaterialGraphSchemaAction_NewNode>(Entry.Action);
		UClass* const ExpressionClass = NewNodeAction.IsValid()
			? NewNodeAction->MaterialExpressionClass.Get()
			: nullptr;
		if (!ExpressionClass)
		{
			OutError = TEXT("material_action_expression_class_missing");
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
			RemoveFailedExpression(CreatedExpression);
			OutError = TEXT("material_action_candidate_create_failed");
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
			RemoveFailedExpression(FunctionCall);
			OutError = TEXT("material_function_action_create_failed");
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
		OutError = TEXT("material_action_type_not_executable");
		return false;
	}
	Entry.Graph->LinkMaterialExpressionsFromGraph();
	Entry.Graph->NotifyGraphChanged();
	Material->MarkPackageDirty();
	UMaterialGraphNode* MaterialNode = CreatedExpression
		? Cast<UMaterialGraphNode>(CreatedExpression->GraphNode)
		: nullptr;
	if (!MaterialNode || !CreatedExpression)
	{
		OutError = TEXT("material_action_candidate_create_failed");
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
		OutError = FString::Printf(
			TEXT("material_action_candidate_result_shape_unsupported:%d"),
			CreatedNodes.Num());
		return false;
	}
	OutExpression = CreatedExpression;
	OutCandidate = MoveTemp(Entry.Candidate);
	OutError.Reset();
	return true;
}
}
