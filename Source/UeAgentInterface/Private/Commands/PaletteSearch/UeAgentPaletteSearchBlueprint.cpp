// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"

#include "Commands/PaletteSearch/UeAgentPaletteSearchAuthoring.h"
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
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node.h"
#include "Kismet2/KismetEditorUtilities.h"
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
			return Graph && (CleanName.IsEmpty()
				|| Graph->GetName().Equals(CleanName, ESearchCase::IgnoreCase)
				|| Graph->GetPathName().Equals(CleanName, ESearchCase::IgnoreCase));
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

	static UEdGraphPin* FindPin(UEdGraph* Graph, UEdGraphNode* Node, const FString& PinGuidText, const FString& PinName)
	{
		if (!Graph)
		{
			return nullptr;
		}

		FGuid PinGuid;
		const bool bHasPinGuid = FGuid::Parse(PinGuidText, PinGuid);
		auto MatchesPin = [bHasPinGuid, PinGuid, &PinName](UEdGraphPin* Pin)
		{
			return Pin && ((bHasPinGuid && Pin->PinId == PinGuid) || PinNameMatches(Pin, PinName));
		};

		if (Node)
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (MatchesPin(Pin))
				{
					return Pin;
				}
			}
			return nullptr;
		}

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
					return Pin;
				}
			}
		}
		return nullptr;
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

	static TSharedRef<FJsonObject> BuildGenericNodeAuthoring(const FSearchParams& Params, const UClass* NodeClass)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("operation"), TEXT("blueprint_add_node_by_class"));
		Obj->SetStringField(TEXT("asset_path"), Params.AssetPath);
		Obj->SetStringField(TEXT("graph_name"), Params.GraphName.IsEmpty() ? TEXT("EventGraph") : Params.GraphName);
		Obj->SetStringField(TEXT("node_class"), NodeClass ? NodeClass->GetPathName() : FString());
		Obj->SetNumberField(TEXT("node_pos_x"), 0);
		Obj->SetNumberField(TEXT("node_pos_y"), 0);
		return Obj;
	}

	static TSharedRef<FJsonObject> BuildFunctionAuthoring(const FSearchParams& Params, const UFunction* Function)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("operation"), TEXT("blueprint_add_call_function_node"));
		Obj->SetStringField(TEXT("asset_path"), Params.AssetPath);
		Obj->SetStringField(TEXT("graph_name"), Params.GraphName.IsEmpty() ? TEXT("EventGraph") : Params.GraphName);
		Obj->SetStringField(TEXT("function_owner_class"), Function && Function->GetOwnerClass() ? Function->GetOwnerClass()->GetPathName() : FString());
		Obj->SetStringField(TEXT("function_name"), Function ? Function->GetName() : FString());
		Obj->SetNumberField(TEXT("node_pos_x"), 0);
		Obj->SetNumberField(TEXT("node_pos_y"), 0);
		return Obj;
	}

	static TSharedRef<FJsonObject> BuildEventAuthoring(const FSearchParams& Params, const UFunction* Function)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("operation"), TEXT("blueprint_add_event_node"));
		Obj->SetStringField(TEXT("asset_path"), Params.AssetPath);
		Obj->SetStringField(TEXT("graph_name"), Params.GraphName.IsEmpty() ? TEXT("EventGraph") : Params.GraphName);
		Obj->SetStringField(TEXT("event_class"), Function && Function->GetOwnerClass() ? Function->GetOwnerClass()->GetPathName() : FString());
		Obj->SetStringField(TEXT("event_name"), Function ? Function->GetName() : FString());
		Obj->SetNumberField(TEXT("node_pos_x"), 0);
		Obj->SetNumberField(TEXT("node_pos_y"), 0);
		return Obj;
	}

	static TSharedRef<FJsonObject> BuildVariableAuthoring(const FSearchParams& Params, const UBlueprintVariableNodeSpawner* VariableSpawner)
	{
		const FProperty* VarProperty = VariableSpawner ? VariableSpawner->GetVarProperty() : nullptr;
		const FString NodeClassName = VariableSpawner && VariableSpawner->NodeClass ? VariableSpawner->NodeClass->GetName() : FString();
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("operation"), TEXT("blueprint_add_variable_node"));
		Obj->SetStringField(TEXT("asset_path"), Params.AssetPath);
		Obj->SetStringField(TEXT("graph_name"), Params.GraphName.IsEmpty() ? TEXT("EventGraph") : Params.GraphName);
		Obj->SetStringField(TEXT("variable_name"), VarProperty ? VarProperty->GetName() : FString());
		Obj->SetStringField(TEXT("node_type"), NodeClassName.Contains(TEXT("Set")) ? TEXT("set") : TEXT("get"));
		if (VarProperty && VarProperty->GetOwnerClass())
		{
			Obj->SetStringField(TEXT("member_parent_class"), VarProperty->GetOwnerClass()->GetPathName());
		}
		Obj->SetNumberField(TEXT("node_pos_x"), 0);
		Obj->SetNumberField(TEXT("node_pos_y"), 0);
		return Obj;
	}

	static void AddBlueprintActionCandidate(const FSearchParams& Params, const TSharedPtr<FEdGraphSchemaAction>& Action, int32 UiOrder, TArray<FCandidate>& OutCandidates)
	{
		if (!Action.IsValid())
		{
			return;
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
			return;
		}

		FString Type = TEXT("blueprint_node");
		FString StableId = NodeClass->GetPathName();
		FString CanonicalKey = NodeClass->GetName();
		FString WriteSupport = TEXT("blueprint_add_node_by_class");
		FString SupportReason = TEXT("Can be written with node_class via blueprint_add_node_by_class; some actions may need specialized fields.");
		TSharedPtr<FJsonObject> RecommendedAuthoring = BuildGenericNodeAuthoring(Params, NodeClass);

		if (const UBlueprintFunctionNodeSpawner* FunctionSpawner = Cast<const UBlueprintFunctionNodeSpawner>(Spawner))
		{
			if (const UFunction* Function = FunctionSpawner->GetFunction())
			{
				Type = TEXT("blueprint_call_function");
				StableId = Function->GetPathName();
				CanonicalKey = Function->GetName();
				WriteSupport = TEXT("blueprint_add_call_function_node");
				SupportReason = TEXT("Can be written with function_owner_class and function_name via blueprint_add_call_function_node.");
				RecommendedAuthoring = BuildFunctionAuthoring(Params, Function);
			}
		}
		else if (const UBlueprintEventNodeSpawner* EventSpawner = Cast<const UBlueprintEventNodeSpawner>(Spawner))
		{
			if (const UFunction* Function = EventSpawner->GetEventFunction())
			{
				Type = TEXT("blueprint_event");
				StableId = Function->GetPathName();
				CanonicalKey = Function->GetName();
				WriteSupport = TEXT("blueprint_add_event_node");
				SupportReason = TEXT("Can be written with event_class and event_name via blueprint_add_event_node.");
				RecommendedAuthoring = BuildEventAuthoring(Params, Function);
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
				WriteSupport = TEXT("blueprint_add_variable_node");
				SupportReason = TEXT("Can be written with variable_name and node_type via blueprint_add_variable_node.");
				RecommendedAuthoring = BuildVariableAuthoring(Params, VariableSpawner);
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
		Candidate.CandidateId = Candidate.Kind + TEXT(":") + Candidate.FullName;
		Candidate.StableId = StableId;
		Candidate.CanonicalKey = CanonicalKey;
		Candidate.DisplayName = Candidate.Name;
		Candidate.Tooltip = Action->GetTooltipDescription().ToString();
		Candidate.Keywords = Action->GetKeywords().ToString();
		Candidate.CategoryPath = NormalizeCategoryPath(*Action);
		Candidate.Section = Params.PinGuid.IsEmpty() && Params.PinName.IsEmpty() ? TEXT("Context") : TEXT("Pin Context");
		Candidate.Type = Type;
		Candidate.SourceOrigin = ClassSourceOrigin(NodeClass);
		Candidate.SourceLabel = NodeClass->GetOutermost()->GetName();
		Candidate.ObjectPath = NodeClass->GetPathName();
		Candidate.WriteSupport = WriteSupport;
		Candidate.SupportReason = SupportReason;
		Candidate.bInLibrary = true;
		Candidate.bSupportedInContext = true;
		Candidate.UiOrder = UiOrder;
		Candidate.JsonAuthoring = BuildJsonAuthoringPatch(Candidate);
		Candidate.LegacyAuthoring = RecommendedAuthoring;
		OutCandidates.Add(MoveTemp(Candidate));
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
					(Candidate.CanonicalKey.Equals(VariableName) || Candidate.Name.Equals(VariableName));
			});
			if (bAlreadyAdded)
			{
				continue;
			}

			const FString VariablePath = FString::Printf(TEXT("%s.%s"), *BlueprintPath, *VariableName);
			FCandidate Candidate;
			Candidate.CandidateId = FString::Printf(TEXT("blueprint_variable_node:%s:%s"), *BlueprintPath, *VariableName);
			Candidate.Kind = TEXT("blueprint_variable_node");
			Candidate.Type = Candidate.Kind;
			Candidate.Name = VariableName;
			Candidate.FullName = VariablePath;
			Candidate.StableId = VariablePath;
			Candidate.CanonicalKey = VariableName;
			Candidate.DisplayName = FName::NameToDisplayString(VariableName, false);
			Candidate.CategoryPath = TEXT("Blueprint|Variables");
			Candidate.Section = TEXT("Blueprint");
			Candidate.SourceOrigin = TEXT("asset");
			Candidate.SourceLabel = Blueprint->GetOutermost() ? Blueprint->GetOutermost()->GetName() : BlueprintPath;
			Candidate.PackagePath = MemberParentClass;
			Candidate.ObjectPath = VariablePath;
			Candidate.WriteSupport = TEXT("blueprint_add_variable_node");
			Candidate.SupportReason = TEXT("Can be written with variable_name and node_type via blueprint_add_variable_node.");
			Candidate.bInLibrary = false;
			Candidate.bSupportedInContext = true;
			Candidate.UiOrder = UiOrderBase + VariableIndex++;
			Candidate.JsonAuthoring = BuildJsonAuthoringPatch(Candidate);
			OutCandidates.Add(MoveTemp(Candidate));
		}
	}
}

bool CollectBlueprintCandidates(const FSearchParams& Params, TArray<FCandidate>& OutCandidates, TArray<FString>& OutWarnings, FString& OutError)
{
	if (!Params.CatalogType.Equals(TEXT("blueprint_node")) &&
		!Params.CatalogType.Equals(TEXT("blueprint_call_function")) &&
		!Params.CatalogType.Equals(TEXT("blueprint_event")) &&
		!Params.CatalogType.Equals(TEXT("blueprint_variable_node")))
	{
		OutError = FString::Printf(TEXT("unsupported Blueprint catalog_type '%s'."), *Params.CatalogType);
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
		if (!Graph && (!Params.GraphName.IsEmpty() || !Params.PinGuid.IsEmpty() || !Params.PinName.IsEmpty()))
		{
			OutError = TEXT("graph_not_found");
			return false;
		}
		if (Graph && (!Params.PinGuid.IsEmpty() || !Params.PinName.IsEmpty()))
		{
			UEdGraphNode* Node = FindNodeByGuid(Graph, Params.NodeGuid);
			Pin = FindPin(Graph, Node, Params.PinGuid, Params.PinName);
			if (!Pin)
			{
				OutError = TEXT("pin_not_found");
				return false;
			}
		}
	}
	else
	{
		const FName TempBlueprintName = MakeUniqueObjectName(GetTransientPackage(), UBlueprint::StaticClass(), TEXT("UAI_NodeCatalog_TempBlueprint"));
		Blueprint = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(),
			GetTransientPackage(),
			TempBlueprintName,
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass());
		Graph = ResolveGraph(Blueprint, TEXT(""));
		OutWarnings.Add(TEXT("Blueprint search ran without asset_path; a transient Actor Blueprint context was used, so project variables, components, selected actors, and exact pin context are omitted."));
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
		FBlueprintActionMenuUtils::MakeContextMenu(Context, Params.bContextSensitive, TargetMask, Builder);
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
		AddBlueprintActionCandidate(Params, Builder.GetSchemaAction(Index), Index, OutCandidates);
	}
	AddBlueprintVariableDescriptionCandidates(Params, Blueprint, Builder.GetNumActions(), OutCandidates);

	if (!Params.CatalogType.Equals(TEXT("blueprint_node")))
	{
		OutCandidates.RemoveAll([&Params](const FCandidate& Candidate)
		{
			return !Candidate.Type.Equals(Params.CatalogType);
		});
	}

	if (OutCandidates.IsEmpty())
	{
		OutWarnings.Add(TEXT("Blueprint action menu returned no supported candidates for the requested context."));
	}
	return true;
}
}
