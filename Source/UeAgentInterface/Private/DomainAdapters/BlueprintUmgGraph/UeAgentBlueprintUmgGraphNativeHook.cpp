#include "DomainAdapters/BlueprintUmgGraph/UeAgentBlueprintUmgGraphNativeHook.h"

#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Knot.h"
#include "K2Node_Tunnel.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "KismetCompiler.h"
#include "UObject/UnrealType.h"
#include "WidgetBlueprint.h"

namespace UE::UAI::DomainAdapters::BlueprintUmgGraph
{
	namespace
	{
		FString NewEvidence(const TCHAR* Stage, const UObject* Subject)
		{
			return FString::Printf(
				TEXT("native:%s:%s:%s"),
				Stage,
				Subject ? *Subject->GetPathName() : TEXT("none"),
				*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		}

		FString GuidText(const FGuid& Guid)
		{
			return Guid.IsValid()
				? Guid.ToString(EGuidFormats::DigitsWithHyphens)
				: FString();
		}

		FString NodeBuiltinRole(const UEdGraphNode& Node)
		{
			if (const UK2Node_Event* Event = Cast<UK2Node_Event>(&Node))
			{
				return TEXT("event:")
					+ Event->EventReference.GetMemberName().ToString();
			}
			if (Node.IsA<UK2Node_FunctionEntry>())
			{
				return TEXT("function_entry:")
					+ (Node.GetGraph() ? Node.GetGraph()->GetName() : TEXT("unknown"));
			}
			if (Node.IsA<UK2Node_FunctionResult>())
			{
				return TEXT("function_result:")
					+ (Node.GetGraph() ? Node.GetGraph()->GetName() : TEXT("unknown"));
			}
			if (Node.IsA<UK2Node_Tunnel>())
			{
				return TEXT("tunnel:") + Node.GetName();
			}
			return FString();
		}

		FString NodeKey(const UEdGraphNode& Node)
		{
			if (Node.NodeGuid.IsValid())
			{
				return TEXT("node:") + GuidText(Node.NodeGuid);
			}
			const FString Role = NodeBuiltinRole(Node);
			return Role.IsEmpty() ? FString() : TEXT("node:builtin:") + Role;
		}

		FString PinKey(
			const FString& OwnerKey,
			const UEdGraphPin& Pin,
			const int32 PinIndex)
		{
			return FString::Printf(
				TEXT("pin:%s:%s:%s:%d"),
				*OwnerKey,
				*Pin.PinName.ToString(),
				Pin.Direction == EGPD_Output ? TEXT("out") : TEXT("in"),
				PinIndex);
		}

		class FTopologyHasher final
		{
		public:
			void Mix(const FString& Value)
			{
				const FTCHARToUTF8 Utf8(*Value);
				for (int32 Index = 0; Index < Utf8.Length(); ++Index)
				{
					Hash ^= static_cast<uint8>(Utf8.Get()[Index]);
					Hash *= 1099511628211ull;
				}
				Hash ^= 0xff;
				Hash *= 1099511628211ull;
			}

			FString Revision() const
			{
				return FString::Printf(TEXT("native-revision:%016llx"), Hash);
			}

		private:
			uint64 Hash = 1469598103934665603ull;
		};

		FString TopologyRevision(const FBlueprintUmgGraphTopology& Topology)
		{
			TArray<FString> Facts;
			Facts.Add(Topology.GetTargetKind());
			Facts.Add(Topology.GetTargetIdentity());
			Facts.Add(Topology.GetStructureIdentity());
			for (const FBlueprintUmgGraphItemIdentity& Item : Topology.GetItems())
			{
				Facts.Add(TEXT("item|") + Item.GetItemKey() + TEXT("|")
					+ Item.GetNativeGuid() + TEXT("|")
					+ Item.GetStableBuiltinRole());
			}
			for (const FBlueprintUmgGraphPinIdentity& Pin : Topology.GetPins())
			{
				Facts.Add(TEXT("pin|") + Pin.GetPinKey() + TEXT("|")
					+ Pin.GetOwnerItemKey() + TEXT("|") + Pin.GetNativeName()
					+ (Pin.GetDirection() == EBlueprintUmgGraphPinDirection::Output
						? TEXT("|out") : TEXT("|in")));
			}
			for (const FBlueprintUmgGraphLinkIdentity& Link : Topology.GetLinks())
			{
				Facts.Add(TEXT("link|") + Link.GetLinkKey() + TEXT("|")
					+ Link.GetFromPinKey() + TEXT("|") + Link.GetToPinKey());
			}
			for (const FBlueprintUmgGraphHierarchyRelation& Relation
				: Topology.GetHierarchy())
			{
				Facts.Add(TEXT("hierarchy|") + Relation.GetRelationKey()
					+ TEXT("|") + Relation.GetItemKey() + TEXT("|")
					+ Relation.GetParentKey() + TEXT("|")
					+ Relation.GetOrderingKey());
			}
			for (const FBlueprintUmgGraphBindingIdentity& Binding
				: Topology.GetBindings())
			{
				Facts.Add(TEXT("binding|") + Binding.GetBindingKey()
					+ TEXT("|") + Binding.GetSourceIdentity() + TEXT("|")
					+ Binding.GetSourceField() + TEXT("|")
					+ Binding.GetTargetIdentity() + TEXT("|")
					+ Binding.GetTargetField());
			}
			Facts.Sort();
			FTopologyHasher Hasher;
			for (const FString& Fact : Facts)
			{
				Hasher.Mix(Fact);
			}
			return Hasher.Revision();
		}

		void SetMutationReceipt(
			const UObject& Subject,
			FBlueprintUmgGraphNativeMutationReceipt& OutReceipt)
		{
			OutReceipt.SetEvidenceId(NewEvidence(TEXT("apply"), &Subject));
		}
	}

	bool FBlueprintUmgGraphNativeHook::TryCreateBlueprint(
		UObject& Outer,
		const FName AssetName,
		UClass& ParentClass,
		UBlueprint*& OutBlueprint,
		FBlueprintUmgGraphAdapterError& OutError) const
	{
		OutBlueprint = nullptr;
		OutError.Reset();
		if (AssetName.IsNone())
		{
			OutError.Set(TEXT("blueprint_umg_graph_asset_name_missing"));
			return false;
		}
		OutBlueprint = FKismetEditorUtilities::CreateBlueprint(
			&ParentClass,
			&Outer,
			AssetName,
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			NAME_None);
		if (!OutBlueprint)
		{
			OutError.Set(TEXT("blueprint_umg_graph_blueprint_create_failed"));
			return false;
		}
		return true;
	}

	bool FBlueprintUmgGraphNativeHook::TryCreateWidgetBlueprint(
		UObject& Outer,
		const FName AssetName,
		UWidgetBlueprint*& OutBlueprint,
		FBlueprintUmgGraphAdapterError& OutError) const
	{
		OutBlueprint = nullptr;
		OutError.Reset();
		UBlueprint* Created = FKismetEditorUtilities::CreateBlueprint(
			UUserWidget::StaticClass(),
			&Outer,
			AssetName,
			BPTYPE_Normal,
			UWidgetBlueprint::StaticClass(),
			UWidgetBlueprintGeneratedClass::StaticClass(),
			NAME_None);
		OutBlueprint = Cast<UWidgetBlueprint>(Created);
		if (!OutBlueprint || !OutBlueprint->WidgetTree)
		{
			OutError.Set(TEXT("blueprint_umg_graph_widget_blueprint_create_failed"));
			return false;
		}
		return true;
	}

	bool FBlueprintUmgGraphNativeHook::TryCreateUberGraph(
		UBlueprint& Blueprint,
		const FName GraphName,
		UEdGraph*& OutGraph,
		FBlueprintUmgGraphNativeMutationReceipt& OutReceipt,
		FBlueprintUmgGraphAdapterError& OutError) const
	{
		OutGraph = nullptr;
		OutError.Reset();
		if (GraphName.IsNone())
		{
			OutError.Set(TEXT("blueprint_umg_graph_graph_name_missing"));
			return false;
		}
		OutGraph = FBlueprintEditorUtils::CreateNewGraph(
			&Blueprint,
			GraphName,
			UEdGraph::StaticClass(),
			UEdGraphSchema_K2::StaticClass());
		if (!OutGraph)
		{
			OutError.Set(TEXT("blueprint_umg_graph_graph_create_failed"));
			return false;
		}
		FBlueprintEditorUtils::AddUbergraphPage(&Blueprint, OutGraph);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
		SetMutationReceipt(Blueprint, OutReceipt);
		return true;
	}

	bool FBlueprintUmgGraphNativeHook::TryAddKnotNode(
		UEdGraph& Graph,
		const FGuid& NodeGuid,
		const int32 PositionX,
		const int32 PositionY,
		UEdGraphNode*& OutNode,
		FBlueprintUmgGraphNativeMutationReceipt& OutReceipt,
		FBlueprintUmgGraphAdapterError& OutError) const
	{
		OutNode = nullptr;
		OutError.Reset();
		if (!NodeGuid.IsValid())
		{
			OutError.Set(TEXT("blueprint_umg_graph_node_guid_invalid"));
			return false;
		}
		FGraphNodeCreator<UK2Node_Knot> Creator(Graph);
		UK2Node_Knot* Node = Creator.CreateNode();
		if (!Node)
		{
			OutError.Set(TEXT("blueprint_umg_graph_node_create_failed"));
			return false;
		}
		Node->NodeGuid = NodeGuid;
		Node->NodePosX = PositionX;
		Node->NodePosY = PositionY;
		Node->AllocateDefaultPins();
		Creator.Finalize();
		Node->NodeGuid = NodeGuid;
		OutNode = Node;
		if (UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(&Graph))
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		}
		SetMutationReceipt(Graph, OutReceipt);
		return true;
	}

	bool FBlueprintUmgGraphNativeHook::TryConnectPins(
		UEdGraphPin& FromPin,
		UEdGraphPin& ToPin,
		FBlueprintUmgGraphNativeMutationReceipt& OutReceipt,
		FBlueprintUmgGraphAdapterError& OutError) const
	{
		OutError.Reset();
		if (FromPin.Direction != EGPD_Output || ToPin.Direction != EGPD_Input)
		{
			OutError.Set(TEXT("blueprint_umg_graph_link_direction_invalid"));
			return false;
		}
		UEdGraphNode* FromNode = FromPin.GetOwningNode();
		UEdGraphNode* ToNode = ToPin.GetOwningNode();
		UEdGraph* Graph = FromNode ? FromNode->GetGraph() : nullptr;
		if (!Graph || !ToNode || ToNode->GetGraph() != Graph || !Graph->GetSchema())
		{
			OutError.Set(TEXT("blueprint_umg_graph_link_endpoint_unresolved"));
			return false;
		}
		if (!FromPin.LinkedTo.Contains(&ToPin)
			&& !Graph->GetSchema()->TryCreateConnection(&FromPin, &ToPin))
		{
			OutError.Set(TEXT("blueprint_umg_graph_link_connection_rejected"));
			return false;
		}
		if (UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph))
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		}
		SetMutationReceipt(*Graph, OutReceipt);
		return true;
	}

	bool FBlueprintUmgGraphNativeHook::TryAddSCSComponent(
		UBlueprint& Blueprint,
		UClass& ComponentClass,
		const FName VariableName,
		const FGuid& VariableGuid,
		USCS_Node* Parent,
		USCS_Node*& OutNode,
		FBlueprintUmgGraphNativeMutationReceipt& OutReceipt,
		FBlueprintUmgGraphAdapterError& OutError) const
	{
		OutNode = nullptr;
		OutError.Reset();
		if (!Blueprint.SimpleConstructionScript
			|| VariableName.IsNone()
			|| !VariableGuid.IsValid())
		{
			OutError.Set(TEXT("blueprint_umg_graph_scs_input_invalid"));
			return false;
		}
		USCS_Node* Node = Blueprint.SimpleConstructionScript->CreateNode(
			&ComponentClass, VariableName);
		if (!Node)
		{
			OutError.Set(TEXT("blueprint_umg_graph_scs_component_create_failed"));
			return false;
		}
		Node->VariableGuid = VariableGuid;
		if (Parent)
		{
			Parent->AddChildNode(Node);
		}
		else
		{
			Blueprint.SimpleConstructionScript->AddNode(Node);
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
		OutNode = Node;
		SetMutationReceipt(Blueprint, OutReceipt);
		return true;
	}

	bool FBlueprintUmgGraphNativeHook::TryAddWidget(
		UWidgetBlueprint& Blueprint,
		UClass& WidgetClass,
		const FName WidgetName,
		const FGuid& WidgetGuid,
		UPanelWidget* Parent,
		const int32 InsertIndex,
		UWidget*& OutWidget,
		FBlueprintUmgGraphNativeMutationReceipt& OutReceipt,
		FBlueprintUmgGraphAdapterError& OutError) const
	{
		OutWidget = nullptr;
		OutError.Reset();
		if (!Blueprint.WidgetTree || WidgetName.IsNone()
			|| !WidgetGuid.IsValid() || !WidgetClass.IsChildOf(UWidget::StaticClass()))
		{
			OutError.Set(TEXT("blueprint_umg_graph_widget_input_invalid"));
			return false;
		}
		if (Blueprint.WidgetTree->FindWidget(WidgetName))
		{
			OutError.Set(TEXT("blueprint_umg_graph_widget_identity_duplicate"));
			return false;
		}
		UWidget* Widget = Blueprint.WidgetTree->ConstructWidget<UWidget>(
			&WidgetClass, WidgetName);
		if (!Widget)
		{
			OutError.Set(TEXT("blueprint_umg_graph_widget_create_failed"));
			return false;
		}
		if (Parent)
		{
			if (InsertIndex >= 0)
			{
				if (!Parent->InsertChildAt(InsertIndex, Widget))
				{
					OutError.Set(TEXT("blueprint_umg_graph_widget_attach_failed"));
					return false;
				}
			}
			else if (!Parent->AddChild(Widget))
			{
				OutError.Set(TEXT("blueprint_umg_graph_widget_attach_failed"));
				return false;
			}
		}
		else
		{
			if (Blueprint.WidgetTree->RootWidget)
			{
				OutError.Set(TEXT("blueprint_umg_graph_widget_root_exists"));
				return false;
			}
			Blueprint.WidgetTree->RootWidget = Widget;
		}
		Widget->bIsVariable = true;
		Blueprint.OnVariableAdded(WidgetName);
		Blueprint.WidgetVariableNameToGuidMap.Add(WidgetName, WidgetGuid);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
		OutWidget = Widget;
		SetMutationReceipt(Blueprint, OutReceipt);
		return true;
	}

	bool FBlueprintUmgGraphNativeHook::TrySetBlueprintDescription(
		UBlueprint& Blueprint,
		const FString& Description,
		FBlueprintUmgGraphNativeMutationReceipt& OutReceipt,
		FBlueprintUmgGraphAdapterError& OutError) const
	{
		OutReceipt = {};
		OutError.Reset();
		Blueprint.Modify();
		Blueprint.BlueprintDescription = Description;
		FBlueprintEditorUtils::MarkBlueprintAsModified(&Blueprint);
		Blueprint.MarkPackageDirty();
		SetMutationReceipt(Blueprint, OutReceipt);
		return !OutReceipt.GetEvidenceId().IsEmpty();
	}

	bool FBlueprintUmgGraphNativeHook::TryCompileAndSettle(
		UBlueprint& Blueprint,
		FBlueprintUmgGraphCompileSettleReceipt& OutReceipt,
		FBlueprintUmgGraphAdapterError& OutError) const
	{
		OutReceipt = {};
		OutError.Reset();
		FBlueprintEditorUtils::RefreshAllNodes(&Blueprint);
		FCompilerResultsLog CompilerLog;
		CompilerLog.bLogInfoOnly = false;
		FKismetEditorUtilities::CompileBlueprint(
			&Blueprint,
			EBlueprintCompileOptions::None,
			&CompilerLog);
		OutReceipt.ErrorCount = CompilerLog.NumErrors;
		OutReceipt.bCompileComplete =
			CompilerLog.NumErrors == 0 && Blueprint.Status != BS_Error;
		OutReceipt.CompileEvidenceId = NewEvidence(TEXT("compile"), &Blueprint);
		OutReceipt.SettleEvidenceId = NewEvidence(TEXT("settle"), &Blueprint);
		if (!OutReceipt.bCompileComplete)
		{
			OutError.Set(
				TEXT("blueprint_umg_graph_compile_incomplete"),
				LexToString(CompilerLog.NumErrors));
			return false;
		}
		return true;
	}

	bool FBlueprintUmgGraphNativeHook::TryCaptureBlueprintGraph(
		UBlueprint& Blueprint,
		UEdGraph& Graph,
		const FString& TargetIdentity,
		const FString& StructureIdentity,
		FBlueprintUmgGraphTopology& OutTopology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FBlueprintUmgGraphAdapterError& OutError) const
	{
		OutError.Reset();
		TArray<FBlueprintUmgGraphItemIdentity> Items;
		TArray<FBlueprintUmgGraphPinIdentity> Pins;
		TArray<FBlueprintUmgGraphLinkIdentity> Links;
		TMap<const UEdGraphPin*, FString> PinKeys;
		for (UEdGraphNode* Node : Graph.Nodes)
		{
			if (!Node)
			{
				continue;
			}
			const FString ItemKey = NodeKey(*Node);
			if (ItemKey.IsEmpty())
			{
				OutError.Set(
					TEXT("blueprint_umg_graph_builtin_identity_missing"),
					Node->GetClass()->GetPathName());
				return false;
			}
			Items.Emplace(ItemKey, GuidText(Node->NodeGuid), NodeBuiltinRole(*Node));
			for (int32 PinIndex = 0; PinIndex < Node->Pins.Num(); ++PinIndex)
			{
				UEdGraphPin* Pin = Node->Pins[PinIndex];
				if (!Pin)
				{
					continue;
				}
				const FString Identity = PinKey(ItemKey, *Pin, PinIndex);
				PinKeys.Add(Pin, Identity);
				Pins.Emplace(
					Identity,
					ItemKey,
					Pin->PinName.ToString(),
					Pin->Direction == EGPD_Output
						? EBlueprintUmgGraphPinDirection::Output
						: EBlueprintUmgGraphPinDirection::Input);
			}
		}
		for (const TPair<const UEdGraphPin*, FString>& Pair : PinKeys)
		{
			const UEdGraphPin* Pin = Pair.Key;
			if (!Pin || Pin->Direction != EGPD_Output)
			{
				continue;
			}
			for (const UEdGraphPin* Linked : Pin->LinkedTo)
			{
				const FString* TargetPinKey = PinKeys.Find(Linked);
				if (!TargetPinKey)
				{
					OutError.Set(TEXT("blueprint_umg_graph_link_endpoint_unresolved"));
					return false;
				}
				Links.Emplace(
					TEXT("link:") + Pair.Value + TEXT("->") + *TargetPinKey,
					Pair.Value,
					*TargetPinKey);
			}
		}
		OutTopology = FBlueprintUmgGraphTopology(
			TEXT("blueprint"), TargetIdentity, StructureIdentity,
			MoveTemp(Items), MoveTemp(Pins), MoveTemp(Links), {}, {});
		if (!TryValidateBlueprintUmgGraphTopology(OutTopology, OutError))
		{
			return false;
		}
		OutRevision = TopologyRevision(OutTopology);
		OutEvidenceId = NewEvidence(TEXT("readback"), &Graph);
		return true;
	}

	bool FBlueprintUmgGraphNativeHook::TryCaptureBlueprintScs(
		UBlueprint& Blueprint,
		const FString& TargetIdentity,
		FBlueprintUmgGraphTopology& OutTopology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FBlueprintUmgGraphAdapterError& OutError) const
	{
		OutError.Reset();
		if (!Blueprint.SimpleConstructionScript)
		{
			OutError.Set(TEXT("blueprint_umg_graph_scs_missing"));
			return false;
		}
		TArray<FBlueprintUmgGraphItemIdentity> Items;
		TArray<FBlueprintUmgGraphHierarchyRelation> Hierarchy;
		TMap<const USCS_Node*, FString> Keys;
		for (USCS_Node* Node : Blueprint.SimpleConstructionScript->GetAllNodes())
		{
			if (!Node)
			{
				continue;
			}
			const FString Key = TEXT("component:")
				+ Node->GetVariableName().ToString();
			Keys.Add(Node, Key);
			Items.Emplace(
				Key,
				GuidText(Node->VariableGuid),
				Node->VariableGuid.IsValid()
					? FString() : TEXT("default_scene_root"));
		}
		for (const TPair<const USCS_Node*, FString>& Pair : Keys)
		{
			const USCS_Node* Node = Pair.Key;
			for (int32 ChildIndex = 0; ChildIndex < Node->GetChildNodes().Num(); ++ChildIndex)
			{
				const USCS_Node* Child = Node->GetChildNodes()[ChildIndex];
				const FString* ChildKey = Keys.Find(Child);
				if (!ChildKey)
				{
					OutError.Set(TEXT("blueprint_umg_graph_hierarchy_endpoint_unresolved"));
					return false;
				}
				Hierarchy.Emplace(
					TEXT("parent:") + *ChildKey,
					*ChildKey,
					Pair.Value,
					FString::Printf(TEXT("order:%s:%d"), *Pair.Value, ChildIndex));
			}
		}
		OutTopology = FBlueprintUmgGraphTopology(
			TEXT("blueprint"), TargetIdentity, TEXT("tree:SimpleConstructionScript"),
			MoveTemp(Items), {}, {}, MoveTemp(Hierarchy), {});
		if (!TryValidateBlueprintUmgGraphTopology(OutTopology, OutError))
		{
			return false;
		}
		OutRevision = TopologyRevision(OutTopology);
		OutEvidenceId = NewEvidence(TEXT("readback"), Blueprint.SimpleConstructionScript);
		return true;
	}

	bool FBlueprintUmgGraphNativeHook::TryCaptureWidgetTree(
		UWidgetBlueprint& Blueprint,
		const FString& TargetIdentity,
		FBlueprintUmgGraphTopology& OutTopology,
		FString& OutRevision,
		FString& OutEvidenceId,
		FBlueprintUmgGraphAdapterError& OutError) const
	{
		OutError.Reset();
		if (!Blueprint.WidgetTree)
		{
			OutError.Set(TEXT("blueprint_umg_graph_widget_tree_missing"));
			return false;
		}
		TArray<FBlueprintUmgGraphItemIdentity> Items;
		TArray<FBlueprintUmgGraphHierarchyRelation> Hierarchy;
		TMap<const UWidget*, FString> Keys;
		Blueprint.WidgetTree->ForEachWidget([&](UWidget* Widget)
		{
			if (!Widget)
			{
				return;
			}
			const FString Key = TEXT("widget:") + Widget->GetName();
			Keys.Add(Widget, Key);
			const FGuid* Guid = Blueprint.WidgetVariableNameToGuidMap.Find(
				Widget->GetFName());
			Items.Emplace(
				Key,
				Guid ? GuidText(*Guid) : FString(),
				Widget == Blueprint.WidgetTree->RootWidget
					? TEXT("root_widget") : FString());
		});
		for (const TPair<const UWidget*, FString>& Pair : Keys)
		{
			const UWidget* Widget = Pair.Key;
			const UPanelWidget* Parent = Widget && Widget->Slot
				? Widget->Slot->Parent : nullptr;
			if (!Parent)
			{
				continue;
			}
			const FString* ParentKey = Keys.Find(Parent);
			if (!ParentKey)
			{
				OutError.Set(TEXT("blueprint_umg_graph_hierarchy_endpoint_unresolved"));
				return false;
			}
			Hierarchy.Emplace(
				TEXT("parent:") + Pair.Value,
				Pair.Value,
				*ParentKey,
				FString::Printf(
					TEXT("order:%s:%d"),
					**ParentKey,
					Parent->GetChildIndex(const_cast<UWidget*>(Widget))));
		}
		OutTopology = FBlueprintUmgGraphTopology(
			TEXT("widget_blueprint"), TargetIdentity, TEXT("tree:WidgetTree"),
			MoveTemp(Items), {}, {}, MoveTemp(Hierarchy), {});
		if (!TryValidateBlueprintUmgGraphTopology(OutTopology, OutError))
		{
			return false;
		}
		OutRevision = TopologyRevision(OutTopology);
		OutEvidenceId = NewEvidence(TEXT("readback"), Blueprint.WidgetTree);
		return true;
	}

	bool FBlueprintUmgGraphNativeHook::TryDiscoverAssignableEvents(
		const UObject& Owner,
		const FString& OwnerItemIdentity,
		TArray<FBlueprintUmgGraphEventIdentity>& OutEvents,
		FBlueprintUmgGraphAdapterError& OutError) const
	{
		OutEvents.Reset();
		OutError.Reset();
		if (OwnerItemIdentity.IsEmpty())
		{
			OutError.Set(TEXT("blueprint_umg_graph_event_owner_identity_missing"));
			return false;
		}
		UClass* OwnerClass = Owner.GetClass();
		if (!OwnerClass)
		{
			OutError.Set(TEXT("blueprint_umg_graph_event_owner_class_missing"));
			return false;
		}
		for (TFieldIterator<FMulticastDelegateProperty> It(
			OwnerClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			const FMulticastDelegateProperty* Property = *It;
			if (!Property
				|| Property->HasAnyPropertyFlags(CPF_Parm)
				|| !Property->HasAllPropertyFlags(CPF_BlueprintAssignable))
			{
				continue;
			}
			const UFunction* Signature = Property->SignatureFunction;
			OutEvents.Emplace(
				TEXT("event:") + OwnerItemIdentity + TEXT(":")
					+ Property->GetName(),
				OwnerItemIdentity,
				OwnerClass->GetPathName(),
				Property->GetName(),
				Signature ? Signature->GetPathName() : FString());
		}
		OutEvents.Sort([](
			const FBlueprintUmgGraphEventIdentity& Left,
			const FBlueprintUmgGraphEventIdentity& Right)
		{
			return Left.GetEventIdentity() < Right.GetEventIdentity();
		});
		return true;
	}
}
