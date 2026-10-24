#include "DomainAdapters/BlueprintUmgGraph/UeAgentBlueprintUmgGraphNativeHook.h"

#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelWidget.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
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
#include "Kismet2/CompilerResultsLog.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringOps.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

namespace UE::UAI::DomainAdapters::BlueprintUmgGraph
{
	namespace
	{

		void SetNativeContext(FBlueprintUmgGraphAdapterError& Error, const UObject& Object)
		{
			const UBlueprint* Blueprint = Cast<UBlueprint>(&Object);
			if (!Blueprint) Blueprint = Object.GetTypedOuter<UBlueprint>();
			if (Error.TargetPath.IsEmpty() && Blueprint)
				Error.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Blueprint);
			if (Error.InternalContext.IsEmpty()) Error.InternalContext = Object.GetPathName();
		}

		FBlueprintUmgGraphAdapterError NativeFact(
			const FString& Code, const FString& Reason, const UObject* Object, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Name = FString())
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("graph_adapter"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Path;
			Error.SemanticPointer = Path;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			Error.CandidateName = Name;
			if (Object) SetNativeContext(Error, *Object);
			return Error;
		}

		FBlueprintUmgGraphAdapterError NativeFact(
			const FString& Code, const FString& Reason, const UObject& Object, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Name = FString())
		{
			return NativeFact(Code, Reason, &Object, Path, MoveTemp(Actual), MoveTemp(Expected), Name);
		}

		TSharedPtr<FJsonValue> PinFacts(const UEdGraphPin* Pin)
		{
			if (!Pin) return MakeShared<FJsonValueNull>();
			auto Facts = MakeShared<FJsonObject>();
			const UEdGraphNode* Node = Pin->GetOwningNode();
			const UEdGraph* Graph = Node ? Node->GetGraph() : nullptr;
			Facts->SetStringField(TEXT("graph"), Graph ? Graph->GetName() : FString());
			Facts->SetStringField(TEXT("node"), Node ? Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString() : FString());
			Facts->SetStringField(TEXT("pin"), Pin->PinName.ToString());
			Facts->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Output ? TEXT("output")
				: Pin->Direction == EGPD_Input ? TEXT("input") : *FString::Printf(TEXT("invalid(%d)"), static_cast<int32>(Pin->Direction)));
			return MakeShared<FJsonValueObject>(Facts);
		}

		TSharedPtr<FJsonValue> LinkFacts(const UEdGraphPin& From, const UEdGraphPin& To)
		{
			auto Facts = MakeShared<FJsonObject>();
			Facts->SetField(TEXT("from"), PinFacts(&From));
			Facts->SetField(TEXT("to"), PinFacts(&To));
			return MakeShared<FJsonValueObject>(Facts);
		}
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
			OutError = NativeFact(TEXT("blueprint_umg_graph_asset_name_missing"), TEXT("A Blueprint asset name is required."), Outer,
				TEXT("/asset_name"), MakeShared<FJsonValueString>(AssetName.ToString()), MakeShared<FJsonValueString>(TEXT("non-None asset name")));
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
			OutError = NativeFact(TEXT("blueprint_umg_graph_blueprint_create_failed"),
				FString::Printf(TEXT("CreateBlueprint returned no asset for parent class '%s'."), *ParentClass.GetPathName()), Outer,
				TEXT("/blueprint/created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), AssetName.ToString());
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
			const bool bAssetMissing = OutBlueprint == nullptr;
			OutError = NativeFact(TEXT("blueprint_umg_graph_widget_blueprint_create_failed"),
				bAssetMissing ? TEXT("CreateBlueprint did not return a Widget Blueprint.") : TEXT("The newly created Widget Blueprint has no widget tree."), Outer,
				bAssetMissing ? TEXT("/widget_blueprint/type") : TEXT("/widget_tree/available"),
				bAssetMissing ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Created ? Created->GetClass()->GetPathName() : TEXT("null")))
					: TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)),
				bAssetMissing ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(UWidgetBlueprint::StaticClass()->GetPathName()))
					: TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true)), AssetName.ToString());
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
			OutError = NativeFact(TEXT("blueprint_umg_graph_graph_name_missing"), TEXT("An ubergraph name is required."), Blueprint,
				TEXT("/graph_name"), MakeShared<FJsonValueString>(GraphName.ToString()), MakeShared<FJsonValueString>(TEXT("non-None graph name")));
			return false;
		}
		OutGraph = FBlueprintEditorUtils::CreateNewGraph(
			&Blueprint,
			GraphName,
			UEdGraph::StaticClass(),
			UEdGraphSchema_K2::StaticClass());
		if (!OutGraph)
		{
			OutError = NativeFact(TEXT("blueprint_umg_graph_graph_create_failed"), TEXT("CreateNewGraph returned no K2 ubergraph."), Blueprint,
				TEXT("/graph/created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), GraphName.ToString());
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
			OutError = NativeFact(TEXT("blueprint_umg_graph_node_guid_invalid"), TEXT("The reroute node requires a valid native GUID."), Graph,
				TEXT("/node/guid/valid"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("reroute"));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(NodeGuid.ToString());
			return false;
		}
		FGraphNodeCreator<UK2Node_Knot> Creator(Graph);
		UK2Node_Knot* Node = Creator.CreateNode();
		if (!Node)
		{
			OutError = NativeFact(TEXT("blueprint_umg_graph_node_create_failed"), TEXT("The graph node creator returned no reroute node."), Graph,
				TEXT("/node/created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("reroute"));
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
			OutError = NativeFact(TEXT("blueprint_umg_graph_link_direction_invalid"), TEXT("A graph connection requires an output source pin and an input destination pin."),
				FromPin.GetOwningNode(), TEXT("/link/direction"), LinkFacts(FromPin, ToPin), MakeShared<FJsonValueString>(TEXT("output -> input")));
			return false;
		}
		UEdGraphNode* FromNode = FromPin.GetOwningNode();
		UEdGraphNode* ToNode = ToPin.GetOwningNode();
		UEdGraph* Graph = FromNode ? FromNode->GetGraph() : nullptr;
		if (!Graph || !ToNode || ToNode->GetGraph() != Graph || !Graph->GetSchema())
		{
			const UObject* Context = FromNode ? FromNode : ToNode;
			const bool bDifferentGraphs = Graph && ToNode && ToNode->GetGraph() != Graph;
			OutError = NativeFact(TEXT("blueprint_umg_graph_link_endpoint_unresolved"),
				!Graph ? TEXT("The source pin has no owning graph.")
					: !ToNode ? TEXT("The destination pin has no owning node.")
					: bDifferentGraphs ? TEXT("The connection's pins belong to different graphs.")
					: TEXT("The owning graph has no connection schema."),
				Context, TEXT("/link/endpoints"), LinkFacts(FromPin, ToPin),
				MakeShared<FJsonValueString>(TEXT("two nodes in one graph with a connection schema")));
			return false;
		}
		UE::UAI::Contract::FDiagnostic ConnectionFailure;
		const FString RequestedLinkContext = FString::Printf(TEXT("%s: %s.%s -> %s.%s"), *Graph->GetName(),
			*FromNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString(), *FromPin.PinName.ToString(),
			*ToNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString(), *ToPin.PinName.ToString());
		if (!FromPin.LinkedTo.Contains(&ToPin)
			&& !UeAgentBlueprintAuthoringOps::TryCreateGraphConnection(*Graph->GetSchema(), &FromPin, &ToPin, ConnectionFailure))
		{
			OutError = NativeFact(TEXT("blueprint_umg_graph_link_connection_rejected"), TEXT("The graph schema rejected creation of the requested pin connection."),
				*Graph, TEXT("/link/connected"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
				RequestedLinkContext);
			OutError.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(MoveTemp(ConnectionFailure));
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
			const bool bScsMissing = Blueprint.SimpleConstructionScript == nullptr;
			const bool bNameMissing = VariableName.IsNone();
			OutError = NativeFact(TEXT("blueprint_umg_graph_scs_input_invalid"),
				bScsMissing ? TEXT("The Blueprint has no Simple Construction Script.")
					: bNameMissing ? TEXT("The component variable name is required.") : TEXT("The component variable requires a valid native GUID."),
				Blueprint, bScsMissing ? TEXT("/scs/available") : bNameMissing ? TEXT("/component/variable_name/available") : TEXT("/component/guid/valid"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), VariableName.ToString());
			if (!bScsMissing && !bNameMissing) OutError.ActualEvidence = MakeShared<FJsonValueString>(VariableGuid.ToString());
			return false;
		}
		USCS_Node* Node = Blueprint.SimpleConstructionScript->CreateNode(
			&ComponentClass, VariableName);
		if (!Node)
		{
			OutError = NativeFact(TEXT("blueprint_umg_graph_scs_component_create_failed"),
				FString::Printf(TEXT("Simple Construction Script returned no component node for class '%s'."), *ComponentClass.GetPathName()), Blueprint,
				TEXT("/component/created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), VariableName.ToString());
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
			const bool bTreeMissing = Blueprint.WidgetTree == nullptr;
			const bool bNameMissing = WidgetName.IsNone();
			const bool bGuidInvalid = !WidgetGuid.IsValid();
			OutError = NativeFact(TEXT("blueprint_umg_graph_widget_input_invalid"),
				bTreeMissing ? TEXT("The Widget Blueprint has no widget tree.")
					: bNameMissing ? TEXT("The widget name is required.")
					: bGuidInvalid ? TEXT("The widget requires a valid native GUID.") : TEXT("The requested class does not derive from UWidget."),
				Blueprint, bTreeMissing ? TEXT("/widget_tree/available") : bNameMissing ? TEXT("/widget/name/available")
					: bGuidInvalid ? TEXT("/widget/guid/valid") : TEXT("/widget/class"),
				bTreeMissing || bNameMissing || bGuidInvalid ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false))
					: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(WidgetClass.GetPathName())),
				bTreeMissing || bNameMissing || bGuidInvalid ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true))
					: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(UWidget::StaticClass()->GetPathName())), WidgetName.ToString());
			if (!bTreeMissing && !bNameMissing && bGuidInvalid) OutError.ActualEvidence = MakeShared<FJsonValueString>(WidgetGuid.ToString());
			return false;
		}
		if (Blueprint.WidgetTree->FindWidget(WidgetName))
		{
			OutError = NativeFact(TEXT("blueprint_umg_graph_widget_identity_duplicate"), TEXT("A widget with this name already exists in the widget tree."), Blueprint,
				TEXT("/widget/name"), MakeShared<FJsonValueString>(WidgetName.ToString()), MakeShared<FJsonValueString>(TEXT("unique widget name")), WidgetName.ToString());
			return false;
		}
		UWidget* Widget = Blueprint.WidgetTree->ConstructWidget<UWidget>(
			&WidgetClass, WidgetName);
		if (!Widget)
		{
			OutError = NativeFact(TEXT("blueprint_umg_graph_widget_create_failed"),
				FString::Printf(TEXT("The widget tree returned no widget for class '%s'."), *WidgetClass.GetPathName()), Blueprint,
				TEXT("/widget/created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), WidgetName.ToString());
			return false;
		}
		if (Parent)
		{
			if (InsertIndex >= 0)
			{
				if (!Parent->InsertChildAt(InsertIndex, Widget))
				{
					auto Actual = MakeShared<FJsonObject>();
					Actual->SetStringField(TEXT("parent"), Parent->GetName());
					Actual->SetNumberField(TEXT("insert_index"), InsertIndex);
					Actual->SetNumberField(TEXT("child_count"), Parent->GetChildrenCount());
					Actual->SetBoolField(TEXT("attached"), false);
					auto Expected = MakeShared<FJsonObject>();
					Expected->SetBoolField(TEXT("attached"), true);
					OutError = NativeFact(TEXT("blueprint_umg_graph_widget_attach_failed"), TEXT("The parent panel rejected insertion of the widget at the requested index."), Blueprint,
						TEXT("/widget/parent/insert"), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected), WidgetName.ToString());
					return false;
				}
			}
			else if (!Parent->AddChild(Widget))
			{
				OutError = NativeFact(TEXT("blueprint_umg_graph_widget_attach_failed"),
					FString::Printf(TEXT("The parent panel '%s' rejected the widget."), *Parent->GetName()), Blueprint,
					TEXT("/widget/parent/attached"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), WidgetName.ToString());
				return false;
			}
		}
		else
		{
			if (Blueprint.WidgetTree->RootWidget)
			{
				OutError = NativeFact(TEXT("blueprint_umg_graph_widget_root_exists"), TEXT("The widget tree already has a root widget."), Blueprint,
					TEXT("/widget_tree/root"), MakeShared<FJsonValueString>(Blueprint.WidgetTree->RootWidget->GetName()),
					MakeShared<FJsonValueNull>(), WidgetName.ToString());
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
		if (OutReceipt.GetEvidenceId().IsEmpty())
		{
			OutError = NativeFact(TEXT("blueprint_umg_graph_mutation_evidence_missing"), TEXT("The description mutation produced no native receipt evidence."), Blueprint,
				TEXT("/mutation/evidence/available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		return true;
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
			OutError = UeAgentAuthoringDiagnostics::MakeCompileFailure(TEXT("blueprint_umg_graph_compile_incomplete"),
				UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), CompilerLog, &Blueprint);
			auto Actual = MakeShared<FJsonObject>();
			Actual->SetNumberField(TEXT("error_count"), CompilerLog.NumErrors);
			Actual->SetBoolField(TEXT("status_is_error"), Blueprint.Status == BS_Error);
			auto Expected = MakeShared<FJsonObject>();
			Expected->SetNumberField(TEXT("error_count"), 0);
			Expected->SetBoolField(TEXT("status_is_error"), false);
			OutError.PropertyPath = TEXT("/compile");
			OutError.SemanticPointer = TEXT("/compile");
			OutError.SourceKind = TEXT("native_result");
			OutError.ActualValue = MakeShared<FJsonValueObject>(Actual);
			OutError.ExpectedValue = MakeShared<FJsonValueObject>(Expected);
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
		for (int32 NodeIndex = 0; NodeIndex < Graph.Nodes.Num(); ++NodeIndex)
		{
			UEdGraphNode* Node = Graph.Nodes[NodeIndex];
			if (!Node)
			{
				continue;
			}
			const FString ItemKey = NodeKey(*Node);
			if (ItemKey.IsEmpty())
			{
				OutError = NativeFact(TEXT("blueprint_umg_graph_builtin_identity_missing"),
					FString::Printf(TEXT("The node of class '%s' has neither a native GUID nor a supported builtin role."), *Node->GetClass()->GetPathName()), Blueprint,
					FString::Printf(TEXT("/graph/nodes/%d/identity/available"), NodeIndex), MakeShared<FJsonValueBoolean>(false),
					MakeShared<FJsonValueBoolean>(true), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Node->NodeGuid.ToString());
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
					OutError = NativeFact(TEXT("blueprint_umg_graph_link_endpoint_unresolved"),
						TEXT("The output pin links to a pin absent from this graph's captured topology."), Blueprint,
						TEXT("/graph/link/target"), PinFacts(Linked), MakeShared<FJsonValueString>(TEXT("pin captured in the same graph")),
						Pin->PinName.ToString());
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
			SetNativeContext(OutError, Blueprint);
			for (UEdGraphNode* Node : Graph.Nodes)
				if (Node && NodeKey(*Node) == OutError.SubjectId && OutError.CandidateName.IsEmpty())
					OutError.CandidateName = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
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
			OutError = NativeFact(TEXT("blueprint_umg_graph_scs_missing"), TEXT("The Blueprint has no Simple Construction Script to capture."), Blueprint,
				TEXT("/scs/available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
					OutError = NativeFact(TEXT("blueprint_umg_graph_hierarchy_endpoint_unresolved"),
						FString::Printf(TEXT("Component '%s' refers to a child not captured in this Simple Construction Script."), *Node->GetVariableName().ToString()), Blueprint,
						FString::Printf(TEXT("/scs/children/%d"), ChildIndex),
						Child ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Child->GetVariableName().ToString())) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
						MakeShared<FJsonValueString>(TEXT("component captured in the same Simple Construction Script")), Node->GetVariableName().ToString());
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
			SetNativeContext(OutError, Blueprint);
			for (const auto& Pair : Keys)
				if (Pair.Value == OutError.SubjectId && OutError.CandidateName.IsEmpty())
					OutError.CandidateName = Pair.Key->GetVariableName().ToString();
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
			OutError = NativeFact(TEXT("blueprint_umg_graph_widget_tree_missing"), TEXT("The Widget Blueprint has no widget tree to capture."), Blueprint,
				TEXT("/widget_tree/available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
				OutError = NativeFact(TEXT("blueprint_umg_graph_hierarchy_endpoint_unresolved"), TEXT("The widget's parent panel is absent from the captured widget tree."), Blueprint,
					TEXT("/widget/parent"), MakeShared<FJsonValueString>(Parent->GetName()),
					MakeShared<FJsonValueString>(TEXT("panel captured in the same widget tree")), Widget->GetName());
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
			SetNativeContext(OutError, Blueprint);
			for (const auto& Pair : Keys)
				if (Pair.Value == OutError.SubjectId && OutError.CandidateName.IsEmpty())
					OutError.CandidateName = Pair.Key->GetName();
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
			OutError = NativeFact(TEXT("blueprint_umg_graph_event_owner_identity_missing"), TEXT("Event discovery requires an owner item identity."), Owner,
				TEXT("/event_owner/identity/available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Owner.GetName());
			return false;
		}
		UClass* OwnerClass = Owner.GetClass();
		if (!OwnerClass)
		{
			OutError = NativeFact(TEXT("blueprint_umg_graph_event_owner_class_missing"), TEXT("The event owner has no class from which assignable events can be discovered."), Owner,
				TEXT("/event_owner/class/available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Owner.GetName());
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
