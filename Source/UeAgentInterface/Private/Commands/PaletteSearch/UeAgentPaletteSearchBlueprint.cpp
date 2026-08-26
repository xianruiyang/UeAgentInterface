// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"

#include "Commands/PaletteSearch/UeAgentPaletteSearchNaming.h"
#include "BlueprintActionFilter.h"
#include "BlueprintActionMenuBuilder.h"
#include "BlueprintActionMenuItem.h"
#include "BlueprintActionMenuUtils.h"
#include "BlueprintEventNodeSpawner.h"
#include "BlueprintFieldNodeSpawner.h"
#include "BlueprintFunctionNodeSpawner.h"
#include "BlueprintNodeSpawner.h"
#include "BlueprintVariableNodeSpawner.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/SecureHash.h"
#include "Misc/PackageName.h"

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

	static UBlueprint* LoadBlueprint(const FString& AssetPath)
	{
		return Cast<UBlueprint>(LoadObjectFromPath(AssetPath));
	}

	static UEdGraph* ResolveGraph(UBlueprint* Blueprint, const FString& GraphName)
	{
		if (!Blueprint)
		{
			return nullptr;
		}

		const FString CleanName = GraphName.TrimStartAndEnd();
		auto MatchesGraph = [&CleanName](UEdGraph* Graph)
		{
			if (!Graph)
			{
				return false;
			}
			const FString GuidIdentity = Graph->GraphGuid.IsValid()
				? TEXT("graph:") + Graph->GraphGuid.ToString(
					EGuidFormats::DigitsWithHyphensLower)
				: FString();
			return CleanName.IsEmpty()
				|| Graph->GetName().Equals(CleanName, ESearchCase::IgnoreCase)
				|| Graph->GetPathName().Equals(CleanName, ESearchCase::IgnoreCase)
				|| (!GuidIdentity.IsEmpty()
					&& GuidIdentity.Equals(CleanName, ESearchCase::IgnoreCase))
				|| (TEXT("graph:name:") + Graph->GetName()).Equals(
					CleanName, ESearchCase::IgnoreCase);
		};

		auto FindFromList = [&MatchesGraph](const TArray<UEdGraph*>& Graphs) -> UEdGraph*
		{
			for (UEdGraph* Graph : Graphs)
			{
				if (MatchesGraph(Graph))
				{
					return Graph;
				}
			}
			return nullptr;
		};

		if (UEdGraph* Graph = FindFromList(Blueprint->UbergraphPages))
		{
			return Graph;
		}
		if (UEdGraph* Graph = FindFromList(Blueprint->FunctionGraphs))
		{
			return Graph;
		}
		if (UEdGraph* Graph = FindFromList(Blueprint->MacroGraphs))
		{
			return Graph;
		}
		if (UEdGraph* Graph = FindFromList(Blueprint->DelegateSignatureGraphs))
		{
			return Graph;
		}

		TArray<UEdGraph*> AllGraphs;
		Blueprint->GetAllGraphs(AllGraphs);
		return FindFromList(AllGraphs);
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
			OutError = TEXT("graph_not_found");
			return false;
		}

		FGuid PinGuid;
		const FString CleanPinGuid = PinGuidText.TrimStartAndEnd();
		const FString CleanPinName = PinName.TrimStartAndEnd();
		const bool bHasPinGuid = !CleanPinGuid.IsEmpty();
		if (bHasPinGuid && !FGuid::Parse(CleanPinGuid, PinGuid))
		{
			OutError = TEXT("pin_guid_invalid");
			return false;
		}
		if (!bHasPinGuid && CleanPinName.IsEmpty())
		{
			OutError = TEXT("pin_selector_required");
			return false;
		}
		auto MatchesPin = [bHasPinGuid, PinGuid, &PinName](UEdGraphPin* Pin)
		{
			return Pin
				&& (!bHasPinGuid || Pin->PinId == PinGuid)
				&& (PinName.TrimStartAndEnd().IsEmpty()
					|| PinNameMatches(Pin, PinName));
		};

		TArray<UEdGraphPin*> Matches;
		auto AppendMatches = [&Matches, &MatchesPin](UEdGraphNode* CandidateNode)
		{
			if (!CandidateNode)
			{
				return;
			}
			for (UEdGraphPin* Pin : CandidateNode->Pins)
			{
				if (MatchesPin(Pin))
				{
					Matches.Add(Pin);
				}
			}
		};
		if (Node)
		{
			AppendMatches(Node);
		}
		else
		{
			for (UEdGraphNode* GraphNode : Graph->Nodes)
			{
				AppendMatches(GraphNode);
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

	static FString BuildNativeActionSignature(
		const FEdGraphSchemaAction& Action,
		const UBlueprintNodeSpawner& Spawner)
	{
		const FString Material = FString::Printf(
			TEXT("type=%s\nspawner_class=%s\nspawner=%s\nmenu=%s\ncategory=%s\ntooltip=%s\nkeywords=%s\ngrouping=%d\nsection=%d"),
			*Action.GetTypeId().ToString(),
			*Spawner.GetClass()->GetPathName(),
			*Spawner.GetSpawnerSignature().ToString(),
			*Action.GetMenuDescription().ToString(),
			*Action.GetCategory().ToString(),
			*Action.GetTooltipDescription().ToString(),
			*Action.GetKeywords().ToString(),
			Action.GetGrouping(),
			Action.GetSectionID());
		return TEXT("native-action:") + FMD5::HashAnsiString(*Material).ToLower();
	}

	static FString NormalizeCategoryPath(const FEdGraphSchemaAction& Action)
	{
		FString Category = Action.GetCategory().ToString();
		Category.ReplaceInline(TEXT("|"), TEXT("/"));
		Category.TrimStartAndEndInline();
		return Category.IsEmpty() ? TEXT("Blueprint") : Category;
	}

	static FString ClassSourceOrigin(const UClass* Class)
	{
		if (!Class)
		{
			return TEXT("unknown");
		}
		const FString PackageName = Class->GetOutermost()->GetName();
		if (PackageName.StartsWith(TEXT("/Script/")) || PackageName.StartsWith(TEXT("/Engine/")))
		{
			return TEXT("engine");
		}
		if (PackageName.StartsWith(TEXT("/Game/")))
		{
			return TEXT("project");
		}
		return TEXT("plugins");
	}

	struct FBlueprintActionEntry final
	{
		FCandidate Candidate;
		TSharedPtr<FEdGraphSchemaAction> Action;
		UEdGraph* Graph = nullptr;
		UEdGraphPin* Pin = nullptr;
	};

	static bool TryBuildBlueprintActionCandidate(
		const FSearchParams& Params,
		const TSharedPtr<FEdGraphSchemaAction>& Action,
		int32 UiOrder,
		FCandidate& OutCandidate,
		FString& OutError)
	{
		OutError.Reset();
		if (!Action.IsValid())
		{
			return false;
		}

		const FBlueprintActionMenuItem* MenuItem = nullptr;
		const UBlueprintNodeSpawner* Spawner = nullptr;
		if (Action->GetTypeId() == FBlueprintActionMenuItem::StaticGetTypeId())
		{
			const TSharedPtr<FBlueprintActionMenuItem> TypedAction = StaticCastSharedPtr<FBlueprintActionMenuItem>(Action);
			MenuItem = TypedAction.Get();
			Spawner = MenuItem ? MenuItem->GetRawAction() : nullptr;
		}

		const UK2Node* TemplateNode = FBlueprintActionMenuUtils::ExtractNodeTemplateFromAction(Action);
		const UClass* NodeClass = Spawner && Spawner->NodeClass ? Spawner->NodeClass.Get() : nullptr;
		if (!NodeClass && TemplateNode)
		{
			NodeClass = TemplateNode->GetClass();
		}
		if (!NodeClass)
		{
			return false;
		}

		FString Type = TEXT("blueprint_node");
		FString StableId = NodeClass->GetPathName();
		FString CanonicalKey = NodeClass->GetName();
		FString WriteSupport = TEXT("blueprint.graphs[].items[].candidate");
		FString SupportReason = TEXT("Use node_class as the item candidate in the Blueprint graph authoring surface; specialized candidates may require additional fields.");
		TMap<FString, FString> AuthoringSelectors;
		AuthoringSelectors.Add(TEXT("node_class"), NodeClass->GetPathName());
		const FString NativeActionSignature = Spawner
			? BuildNativeActionSignature(*Action, *Spawner)
			: FString();
		if (NativeActionSignature.IsEmpty())
		{
			return false;
		}
		AuthoringSelectors.Add(
			TEXT("native_action_signature"), NativeActionSignature);

		if (const UBlueprintFunctionNodeSpawner* FunctionSpawner = Cast<const UBlueprintFunctionNodeSpawner>(Spawner))
		{
			if (const UFunction* Function = FunctionSpawner->GetFunction())
			{
				Type = TEXT("blueprint_call_function");
				StableId = Function->GetPathName();
				CanonicalKey = Function->GetName();
				WriteSupport = TEXT("blueprint.graphs[].items[].candidate");
				SupportReason = TEXT("Use function_owner_class and function_name as the call-function item candidate fields.");
				AuthoringSelectors.Reset();
				AuthoringSelectors.Add(
					TEXT("native_action_signature"), NativeActionSignature);
				if (Function->GetOwnerClass()) AuthoringSelectors.Add(TEXT("function_owner_class"), Function->GetOwnerClass()->GetPathName());
				AuthoringSelectors.Add(TEXT("function_name"), Function->GetName());
			}
		}
		else if (const UBlueprintEventNodeSpawner* EventSpawner = Cast<const UBlueprintEventNodeSpawner>(Spawner))
		{
			if (const UFunction* Function = EventSpawner->GetEventFunction())
			{
				Type = TEXT("blueprint_event");
				StableId = Function->GetPathName();
				CanonicalKey = Function->GetName();
				WriteSupport = TEXT("blueprint.graphs[].items[].candidate");
				SupportReason = TEXT("Use event_class and event_name as the event item candidate fields.");
				AuthoringSelectors.Reset();
				AuthoringSelectors.Add(
					TEXT("native_action_signature"), NativeActionSignature);
				if (Function->GetOwnerClass()) AuthoringSelectors.Add(TEXT("event_class"), Function->GetOwnerClass()->GetPathName());
				AuthoringSelectors.Add(TEXT("event_name"), Function->GetName());
			}
		}
		else if (const UBlueprintVariableNodeSpawner* VariableSpawner = Cast<const UBlueprintVariableNodeSpawner>(Spawner))
		{
			const FProperty* VarProperty = VariableSpawner->GetVarProperty();
			if (VarProperty)
			{
				Type = TEXT("blueprint_variable_node");
				StableId = VarProperty->GetPathName();
				CanonicalKey = VarProperty->GetName();
				WriteSupport = TEXT("blueprint.graphs[].items[].candidate");
				SupportReason = TEXT("Use variable_name and node_type as the variable item candidate fields.");
				AuthoringSelectors.Reset();
				AuthoringSelectors.Add(
					TEXT("native_action_signature"), NativeActionSignature);
				AuthoringSelectors.Add(TEXT("variable_name"), VarProperty->GetName());
				const FString NodeClassName = VariableSpawner->NodeClass ? VariableSpawner->NodeClass->GetName() : FString();
				AuthoringSelectors.Add(TEXT("node_type"), NodeClassName.Contains(TEXT("Set")) ? TEXT("set") : TEXT("get"));
				if (VarProperty->GetOwnerClass()) AuthoringSelectors.Add(TEXT("member_parent_class"), VarProperty->GetOwnerClass()->GetPathName());
			}
		}
		else if (const UBlueprintFieldNodeSpawner* FieldSpawner = Cast<const UBlueprintFieldNodeSpawner>(Spawner))
		{
			const FFieldVariant Field = FieldSpawner->GetField();
			if (const FField* RawField = Field.ToField())
			{
				StableId = RawField->GetPathName();
				CanonicalKey = RawField->GetName();
			}
		}

		FCandidate Candidate;
		Candidate.Kind = Type;
		Candidate.Name = MenuNameFromText(Action->GetMenuDescription(), CanonicalKey);
		Candidate.FullName = StableId;
		Candidate.DisplayName = Candidate.Name;
		Candidate.Tooltip = Action->GetTooltipDescription().ToString();
		Candidate.Keywords = Action->GetKeywords().ToString();
		Candidate.CategoryPath = NormalizeCategoryPath(*Action);
		Candidate.Section = Params.PinGuid.IsEmpty() && Params.PinName.IsEmpty() ? TEXT("Context") : TEXT("Pin Context");
		Candidate.Type = Type;
		Candidate.SourceOrigin = ClassSourceOrigin(NodeClass);
		Candidate.SourceLabel = NodeClass->GetOutermost()->GetName();
		Candidate.ObjectPath = NodeClass->GetPathName();
		Candidate.AuthoringSelectors = MoveTemp(AuthoringSelectors);
		Candidate.WriteSupport = WriteSupport;
		Candidate.SupportReason = SupportReason;
		Candidate.bInLibrary = true;
		Candidate.bSupportedInContext = true;
		Candidate.UiOrder = UiOrder;
		FString IdentityError;
		if (!SetCandidateIdentity(
				Params.CatalogType,
				NativeActionSignature,
				CanonicalKey,
				Candidate,
				IdentityError))
		{
			OutError = TEXT("blueprint_action_candidate_identity_failed:")
				+ IdentityError;
			return false;
		}
		OutCandidate = MoveTemp(Candidate);
		return true;
	}

	static void AddBlueprintVariableDescriptionCandidates(const FSearchParams& Params, const UBlueprint* Blueprint, int32 UiOrderBase, TArray<FCandidate>& OutCandidates)
	{
		if (!Blueprint || !Params.CatalogType.Equals(TEXT("blueprint_variable_node")))
		{
			return;
		}

		const FString BlueprintPath = Blueprint->GetPathName();
		const FString MemberParentClass = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetPathName()
			: (Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass->GetPathName() : BlueprintPath);

		int32 VariableIndex = 0;
		for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
		{
			const FString VariableName = Variable.VarName.ToString();
			if (VariableName.IsEmpty())
			{
				continue;
			}

			const bool bAlreadyAdded = OutCandidates.ContainsByPredicate([&VariableName](const FCandidate& Candidate)
			{
				return Candidate.Kind.Equals(TEXT("blueprint_variable_node")) &&
					(Candidate.SearchKey.Equals(VariableName) || Candidate.Name.Equals(VariableName));
			});
			if (bAlreadyAdded)
			{
				continue;
			}

			const FString VariablePath = FString::Printf(TEXT("%s.%s"), *BlueprintPath, *VariableName);
			FCandidate Candidate;
			Candidate.Kind = TEXT("blueprint_variable_node");
			Candidate.Type = Candidate.Kind;
			Candidate.Name = VariableName;
			Candidate.FullName = VariablePath;
			Candidate.DisplayName = FName::NameToDisplayString(VariableName, false);
			Candidate.CategoryPath = TEXT("Blueprint|Variables");
			Candidate.Section = TEXT("Blueprint");
			Candidate.SourceOrigin = TEXT("asset");
			Candidate.SourceLabel = Blueprint->GetOutermost() ? Blueprint->GetOutermost()->GetName() : BlueprintPath;
			Candidate.PackagePath = MemberParentClass;
			Candidate.ObjectPath = VariablePath;
			Candidate.AuthoringSelectors.Add(TEXT("variable_name"), VariableName);
			Candidate.AuthoringSelectors.Add(TEXT("member_parent_class"), MemberParentClass);
			Candidate.WriteSupport = TEXT("blueprint.graphs[].items[].candidate");
			Candidate.SupportReason = TEXT("Use variable_name, member_parent_class and node_type as variable item candidate fields.");
			Candidate.bInLibrary = false;
			Candidate.bSupportedInContext = true;
			Candidate.UiOrder = UiOrderBase + VariableIndex++;
			FString IdentityError;
			ensureAlwaysMsgf(SetCandidateIdentity(Params.CatalogType, VariablePath, VariableName, Candidate, IdentityError), TEXT("Blueprint variable catalog identity failed: %s"), *IdentityError);
			OutCandidates.Add(MoveTemp(Candidate));
		}
	}
}

bool UeAgentPaletteSearch::IsPublicBlueprintCandidateSemanticField(
	const FString& Field)
{
	return Field == TEXT("function_name")
		|| Field == TEXT("function_owner_class")
		|| Field == TEXT("event_name")
		|| Field == TEXT("event_class")
		|| Field == TEXT("variable_name")
		|| Field == TEXT("node_type")
		|| Field == TEXT("member_parent_class")
		|| Field == TEXT("node_class");
}

namespace
{
	bool IsBlueprintCatalogType(const FString& CatalogType)
	{
		return CatalogType.Equals(TEXT("blueprint_node"))
			|| CatalogType.Equals(TEXT("blueprint_call_function"))
			|| CatalogType.Equals(TEXT("blueprint_event"))
			|| CatalogType.Equals(TEXT("blueprint_variable_node"));
	}

	bool CollectBlueprintActionEntries(
		const FSearchParams& Params,
		TArray<FBlueprintActionEntry>& OutEntries,
		TArray<FString>& OutWarnings,
		FString& OutError)
	{
		OutEntries.Reset();
		if (!IsBlueprintCatalogType(Params.CatalogType))
		{
			OutError = FString::Printf(
				TEXT("unsupported Blueprint catalog_type '%s'."),
				*Params.CatalogType);
			return false;
		}
		if (!Params.NodeGuid.TrimStartAndEnd().IsEmpty()
			&& Params.PinGuid.TrimStartAndEnd().IsEmpty()
			&& Params.PinName.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("pin_selector_required");
			return false;
		}

		UBlueprint* Blueprint = nullptr;
		UEdGraph* Graph = nullptr;
		UEdGraphPin* Pin = nullptr;
		if (!Params.AssetPath.IsEmpty())
		{
			Blueprint = LoadBlueprint(Params.AssetPath);
			if (!Blueprint)
			{
				OutError = TEXT("blueprint_not_found");
				return false;
			}
			Graph = ResolveGraph(Blueprint, Params.GraphName);
			if (!Graph && (!Params.GraphName.IsEmpty()
				|| !Params.PinGuid.IsEmpty()
				|| !Params.PinName.IsEmpty()))
			{
				OutError = TEXT("graph_not_found");
				return false;
			}
			if (Graph && (!Params.PinGuid.IsEmpty() || !Params.PinName.IsEmpty()))
			{
				UEdGraphNode* Node = nullptr;
				if (!Params.NodeGuid.TrimStartAndEnd().IsEmpty())
				{
					FGuid ParsedNodeGuid;
					if (!FGuid::Parse(Params.NodeGuid, ParsedNodeGuid))
					{
						OutError = TEXT("node_guid_invalid");
						return false;
					}
					Node = FindNodeByGuid(Graph, Params.NodeGuid);
					if (!Node)
					{
						OutError = TEXT("node_not_found");
						return false;
					}
				}
				if (!TryResolvePin(
						Graph,
						Node,
						Params.PinGuid,
						Params.PinName,
						Pin,
						OutError))
				{
					return false;
				}
			}
		}
		else
		{
			const FName TempBlueprintName = MakeUniqueObjectName(
				GetTransientPackage(),
				UBlueprint::StaticClass(),
				TEXT("UAI_NodeCatalog_TempBlueprint"));
			Blueprint = FKismetEditorUtilities::CreateBlueprint(
				AActor::StaticClass(),
				GetTransientPackage(),
				TempBlueprintName,
				BPTYPE_Normal,
				UBlueprint::StaticClass(),
				UBlueprintGeneratedClass::StaticClass());
			Graph = ResolveGraph(Blueprint, TEXT(""));
			OutWarnings.Add(TEXT(
				"Blueprint search ran without asset_path; a transient Actor "
				"Blueprint context was used, so project variables, components, "
				"selected actors, and exact pin context are omitted."));
		}

		FBlueprintActionContext Context;
		if (Blueprint)
		{
			Context.Blueprints.Add(Blueprint);
		}
		if (Graph)
		{
			Context.Graphs.Add(Graph);
		}
		if (Pin)
		{
			Context.Pins.Add(Pin);
		}

		FBlueprintActionMenuBuilder Builder;
		if (Blueprint || Graph || Pin)
		{
			const uint32 TargetMask =
				EContextTargetFlags::TARGET_Blueprint |
				EContextTargetFlags::TARGET_SubComponents |
				EContextTargetFlags::TARGET_NodeTarget |
				EContextTargetFlags::TARGET_PinObject |
				EContextTargetFlags::TARGET_SiblingPinObjects |
				EContextTargetFlags::TARGET_BlueprintLibraries;
			FBlueprintActionMenuUtils::MakeContextMenu(
				Context, Params.bContextSensitive, TargetMask, Builder);
		}
		else
		{
			FBlueprintActionMenuUtils::MakePaletteMenu(Context, nullptr, Builder);
		}
		while (Builder.ProcessPendingActions())
		{
		}

		for (int32 Index = 0; Index < Builder.GetNumActions(); ++Index)
		{
			const TSharedPtr<FEdGraphSchemaAction> Action =
				Builder.GetSchemaAction(Index);
			FCandidate Candidate;
			FString CandidateError;
			if (!TryBuildBlueprintActionCandidate(
					Params, Action, Index, Candidate, CandidateError))
			{
				if (!CandidateError.IsEmpty())
				{
					OutError = MoveTemp(CandidateError);
					return false;
				}
				continue;
			}
			if (!Params.CatalogType.Equals(TEXT("blueprint_node"))
				&& !Candidate.Type.Equals(Params.CatalogType))
			{
				continue;
			}
			FBlueprintActionEntry& Entry = OutEntries.AddDefaulted_GetRef();
			Entry.Candidate = MoveTemp(Candidate);
			Entry.Action = Action;
			Entry.Graph = Graph;
			Entry.Pin = Pin;
		}
		return true;
	}

	bool MatchesExactName(const FCandidate& Candidate, const FString& Name)
	{
		if (Name.TrimStartAndEnd().IsEmpty())
		{
			return true;
		}
		const auto Matches = [&Name](const FString& Value)
		{
			return !Value.IsEmpty()
				&& Value.Equals(Name, ESearchCase::IgnoreCase);
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

	bool TryResolveBlueprintActionEntry(
		const FSearchParams& Params,
		const FString& Name,
		const FString& Id,
		FBlueprintActionEntry& OutEntry,
		FString& OutError)
	{
		OutEntry = {};
		if (Name.TrimStartAndEnd().IsEmpty()
			&& Id.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("blueprint_action_candidate_selector_required");
			return false;
		}
		TArray<FBlueprintActionEntry> Entries;
		TArray<FString> Warnings;
		if (!CollectBlueprintActionEntries(
				Params, Entries, Warnings, OutError))
		{
			return false;
		}

		TMap<FString, int32> MatchingCanonicalKeys;
		for (int32 Index = 0; Index < Entries.Num(); ++Index)
		{
			const FCandidate& Candidate = Entries[Index].Candidate;
			const bool bIdMatches = Id.TrimStartAndEnd().IsEmpty()
				|| Candidate.GetPublicId().Equals(Id, ESearchCase::IgnoreCase)
				|| Candidate.GetCandidateId().Equals(Id, ESearchCase::CaseSensitive);
			if (!bIdMatches || !MatchesExactName(Candidate, Name))
			{
				continue;
			}
			MatchingCanonicalKeys.FindOrAdd(
				Candidate.GetCandidateId(), Index);
		}
		if (MatchingCanonicalKeys.IsEmpty())
		{
			OutError = TEXT("blueprint_action_candidate_not_found");
			return false;
		}
		if (MatchingCanonicalKeys.Num() != 1)
		{
			OutError = FString::Printf(
				TEXT("blueprint_action_candidate_ambiguous:%d"),
				MatchingCanonicalKeys.Num());
			return false;
		}
		for (const TPair<FString, int32>& Match : MatchingCanonicalKeys)
		{
			OutEntry = Entries[Match.Value];
			break;
		}
		OutError.Reset();
		return true;
	}
}

bool TryResolveBlueprintGraphIdentity(
	const FString& AssetPath,
	const FString& GraphSelector,
	FString& OutGraphId,
	FString& OutGraphName,
	FString& OutError)
{
	OutGraphId.Reset();
	OutGraphName.Reset();
	UBlueprint* Blueprint = LoadBlueprint(AssetPath);
	if (!Blueprint)
	{
		OutError = TEXT("blueprint_not_found");
		return false;
	}
	UEdGraph* Graph = ResolveGraph(Blueprint, GraphSelector);
	if (!Graph)
	{
		OutError = TEXT("graph_not_found");
		return false;
	}
	if (!Graph->GraphGuid.IsValid())
	{
		OutError = TEXT("graph_identity_invalid");
		return false;
	}
	OutGraphId = TEXT("graph:") + Graph->GraphGuid.ToString(
		EGuidFormats::DigitsWithHyphensLower);
	OutGraphName = Graph->GetName();
	OutError.Reset();
	return true;
}

bool CollectBlueprintCandidates(
	const FSearchParams& Params,
	TArray<FCandidate>& OutCandidates,
	TArray<FString>& OutWarnings,
	FString& OutError)
{
	OutCandidates.Reset();
	TArray<FBlueprintActionEntry> Entries;
	if (!CollectBlueprintActionEntries(
			Params, Entries, OutWarnings, OutError))
	{
		return false;
	}
	for (FBlueprintActionEntry& Entry : Entries)
	{
		OutCandidates.Add(MoveTemp(Entry.Candidate));
	}

	UBlueprint* Blueprint = Params.AssetPath.IsEmpty()
		? nullptr
		: LoadBlueprint(Params.AssetPath);
	AddBlueprintVariableDescriptionCandidates(
		Params, Blueprint, OutCandidates.Num(), OutCandidates);
	if (OutCandidates.IsEmpty())
	{
		OutWarnings.Add(TEXT(
			"Blueprint action menu returned no supported candidates for the "
			"requested context."));
	}
	return true;
}

bool TryResolveBlueprintActionCandidate(
	const FSearchParams& Params,
	const FString& Name,
	const FString& Id,
	FCandidate& OutCandidate,
	FString& OutError)
{
	FBlueprintActionEntry Entry;
	if (!TryResolveBlueprintActionEntry(
			Params, Name, Id, Entry, OutError))
	{
		OutCandidate = {};
		return false;
	}
	OutCandidate = MoveTemp(Entry.Candidate);
	return true;
}

bool TryPerformBlueprintActionCandidate(
	const FSearchParams& Params,
	const FString& Name,
	const FString& Id,
	const FVector2f& Location,
	UEdGraphNode*& OutNode,
	FCandidate& OutCandidate,
	FString& OutError)
{
	OutNode = nullptr;
	FBlueprintActionEntry Entry;
	if (!TryResolveBlueprintActionEntry(
			Params, Name, Id, Entry, OutError))
	{
		OutCandidate = {};
		return false;
	}
	if (!Entry.Action.IsValid() || !Entry.Graph)
	{
		OutError = TEXT("blueprint_action_candidate_not_executable");
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
	OutNode = Entry.Action->PerformAction(
		Entry.Graph, Entry.Pin, Location, false);
	if (!OutNode)
	{
		OutError = TEXT("blueprint_action_candidate_create_failed");
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
	if (CreatedNodes.Num() != 1 || CreatedNodes[0] != OutNode)
	{
		OutNode = nullptr;
		OutError = FString::Printf(
			TEXT("blueprint_action_candidate_result_shape_unsupported:%d"),
			CreatedNodes.Num());
		return false;
	}
	OutCandidate = MoveTemp(Entry.Candidate);
	OutError.Reset();
	return true;
}
}
