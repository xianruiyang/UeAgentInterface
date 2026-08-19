// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringState.h"

#include "Commands/Blueprint/UeAgentHttpServer_BlueprintOps.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringDescriptors.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "EnhancedInputSubsystems.h"
#include "Factories/BlueprintFactory.h"
#include "GameFramework/Actor.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_EnhancedInputAction.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Tunnel.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Subsystems/SubsystemBlueprintLibrary.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringNativeDefaults.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"
#include "UObject/UnrealType.h"

namespace UeAgentBlueprintAuthoring
{
	namespace
	{
		FString GuidText(const FGuid& Guid)
		{
			return Guid.IsValid()
				? Guid.ToString(EGuidFormats::DigitsWithHyphensLower)
				: FString();
		}

		bool BuildIdentity(
			const FString& ModelId,
			const FString& TargetKind,
			const TMap<FString, FString>& Selectors,
			UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
			FString& OutError)
		{
			return UeAgentAuthoringIdentity::BuildCanonicalIdentity(
				ModelId, TargetKind, Selectors, OutIdentity, OutError);
		}

		bool AddReadRequest(
			const FString& SemanticId,
			const FString& ModelId,
			const FString& TargetKind,
			const TMap<FString, FString>& Selectors,
			const UObject* RootObject,
			const TMap<FString, FString>& Context,
			TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
			FString& OutError)
		{
			UeAgentAuthoringState::FReadRequest Request;
			Request.SemanticId = SemanticId;
			if (!BuildIdentity(ModelId, TargetKind, Selectors, Request.TargetIdentity, OutError))
			{
				return false;
			}
			Request.TargetQuery.ModelId = Request.TargetIdentity.ModelId;
			Request.TargetQuery.TargetKind = Request.TargetIdentity.TargetKind;
			Request.TargetQuery.CanonicalKey = Request.TargetIdentity.CanonicalKey;
			Request.RootObject = RootObject;
			Request.RootOwnerPath = Selectors.FindRef(TEXT("asset_path"));
			Request.Context = Context;
			OutRequests.Add(MoveTemp(Request));
			return true;
		}

		FString PinContainerTypeToString(const EPinContainerType ContainerType)
		{
			switch (ContainerType)
			{
			case EPinContainerType::Array: return TEXT("array");
			case EPinContainerType::Set: return TEXT("set");
			case EPinContainerType::Map: return TEXT("map");
			default: return FString();
			}
		}

		TSharedRef<FJsonObject> BuildPinTypeState(const FEdGraphPinType& PinType)
		{
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			State->SetStringField(TEXT("pin_category"), PinType.PinCategory.ToString());
			if (!PinType.PinSubCategory.IsNone())
			{
				State->SetStringField(TEXT("pin_subcategory"), PinType.PinSubCategory.ToString());
			}
			if (const UObject* Subcategory = PinType.PinSubCategoryObject.Get())
			{
				State->SetStringField(TEXT("pin_subcategory_object"), Subcategory->GetPathName());
			}
			const FString Container = PinContainerTypeToString(PinType.ContainerType);
			if (!Container.IsEmpty()) State->SetStringField(TEXT("container_type"), Container);
			if (PinType.ContainerType == EPinContainerType::Map
				&& !PinType.PinValueType.TerminalCategory.IsNone())
			{
				TSharedRef<FJsonObject> ValueType = MakeShared<FJsonObject>();
				ValueType->SetStringField(
					TEXT("pin_category"), PinType.PinValueType.TerminalCategory.ToString());
				if (!PinType.PinValueType.TerminalSubCategory.IsNone())
				{
					ValueType->SetStringField(
						TEXT("pin_subcategory"),
						PinType.PinValueType.TerminalSubCategory.ToString());
				}
				if (const UObject* ValueSubcategory =
					PinType.PinValueType.TerminalSubCategoryObject.Get())
				{
					ValueType->SetStringField(
						TEXT("pin_subcategory_object"), ValueSubcategory->GetPathName());
				}
				State->SetObjectField(TEXT("value_type"), ValueType);
			}
			return State;
		}

		FString GetGraphKind(const UBlueprint& Blueprint, const UEdGraph* Graph)
		{
			if (Blueprint.UbergraphPages.Contains(Graph)) return TEXT("event_graph");
			if (Blueprint.FunctionGraphs.Contains(Graph)) return TEXT("function_graph");
			if (Blueprint.MacroGraphs.Contains(Graph)) return TEXT("macro_graph");
			if (Blueprint.DelegateSignatureGraphs.Contains(Graph)) return TEXT("delegate_graph");
			return TEXT("graph");
		}

		FString GetBuiltinNodeId(const UEdGraphNode* Node)
		{
			if (Node && Node->IsA<UK2Node_FunctionEntry>()) return TEXT("__entry__");
			if (Node && Node->IsA<UK2Node_FunctionResult>()) return TEXT("__result__");
			if (const UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node))
			{
				bool bHasInput = false;
				bool bHasOutput = false;
				for (const UEdGraphPin* Pin : Tunnel->Pins)
				{
					if (!Pin) continue;
					bHasInput |= Pin->Direction == EGPD_Input;
					bHasOutput |= Pin->Direction == EGPD_Output;
				}
				if (bHasOutput && !bHasInput) return TEXT("__entry__");
				if (bHasInput && !bHasOutput) return TEXT("__result__");
			}
			return FString();
		}

		FString GetNodeKind(const UEdGraphNode* Node)
		{
			if (Node && Node->IsA<UK2Node_ComponentBoundEvent>()) return TEXT("component_bound_event");
			if (Node && Node->IsA<UK2Node_EnhancedInputAction>()) return TEXT("enhanced_input_action_event");
			if (Node && Node->IsA<UK2Node_DynamicCast>()) return TEXT("dynamic_cast");
			if (Node && Node->IsA<UK2Node_CustomEvent>()) return TEXT("custom_event");
			if (Node && Node->IsA<UK2Node_Event>()) return TEXT("event");
			if (Node && Node->IsA<UK2Node_CallParentFunction>()) return TEXT("call_parent_function");
			if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
			{
				const UFunction* Function = Call->GetTargetFunction();
				if (Function
					&& Function->GetFName() == TEXT("GetLocalPlayerSubSystemFromPlayerController")
					&& Function->GetOwnerClass() == USubsystemBlueprintLibrary::StaticClass())
				{
					return TEXT("enhanced_input_get_local_player_subsystem");
				}
				if (Function
					&& Function->GetFName() == TEXT("AddMappingContext")
					&& Function->GetOwnerClass() == UEnhancedInputLocalPlayerSubsystem::StaticClass())
				{
					return TEXT("enhanced_input_add_mapping_context");
				}
				return TEXT("call_function");
			}
			if (Node && Node->IsA<UK2Node_Variable>()) return TEXT("variable_node");
			return Node ? TEXT("node_by_class") : FString();
		}

		FString GetNodeSemanticName(const UEdGraphNode& Node)
		{
			if (const UK2Node_CustomEvent* Custom = Cast<UK2Node_CustomEvent>(&Node))
			{
				return Custom->CustomFunctionName.ToString();
			}
			if (const UK2Node_Event* Event = Cast<UK2Node_Event>(&Node))
			{
				return Event->EventReference.GetMemberName().ToString();
			}
			if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(&Node))
			{
				if (const UFunction* Function = Call->GetTargetFunction()) return Function->GetName();
				return Call->FunctionReference.GetMemberName().ToString();
			}
			if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(&Node))
			{
				return Variable->VariableReference.GetMemberName().ToString();
			}
			return Node.GetClass()->GetPathName();
		}

		FString NodeReference(const UEdGraphNode* Node)
		{
			if (!Node) return FString();
			const FString Builtin = GetBuiltinNodeId(Node);
			if (!Builtin.IsEmpty()) return Builtin;
			const FString Guid = GuidText(Node->NodeGuid);
			return Guid.IsEmpty() ? Node->GetName() : Guid;
		}

		UEdGraphPin* FindPin(UEdGraphNode* Node, const TCHAR* Name)
		{
			if (!Node) return nullptr;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->PinName == Name) return Pin;
			}
			return nullptr;
		}

		TSharedRef<FJsonObject> BuildNodeState(const UEdGraphNode& Node)
		{
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			const FString Kind = GetNodeKind(&Node);
			State->SetStringField(TEXT("id"), NodeReference(&Node));
			State->SetStringField(TEXT("node_type"), Kind);
			TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
			Position->SetNumberField(TEXT("x"), Node.NodePosX);
			Position->SetNumberField(TEXT("y"), Node.NodePosY);
			State->SetObjectField(TEXT("pos"), Position);

			if (const UK2Node_ComponentBoundEvent* Bound = Cast<UK2Node_ComponentBoundEvent>(&Node))
			{
				State->SetStringField(TEXT("component_name"), Bound->ComponentPropertyName.ToString());
				State->SetStringField(TEXT("delegate_property_name"), Bound->DelegatePropertyName.ToString());
				if (const UClass* Owner = Bound->DelegateOwnerClass.Get())
				{
					State->SetStringField(TEXT("delegate_owner_class"), Owner->GetPathName());
				}
			}
			else if (const UK2Node_EnhancedInputAction* Enhanced = Cast<UK2Node_EnhancedInputAction>(&Node))
			{
				if (Enhanced->InputAction)
				{
					State->SetStringField(TEXT("input_action"), Enhanced->InputAction->GetPathName());
				}
			}
			else if (const UK2Node_DynamicCast* Dynamic = Cast<UK2Node_DynamicCast>(&Node))
			{
				if (Dynamic->TargetType)
				{
					State->SetStringField(TEXT("target_class"), Dynamic->TargetType->GetPathName());
				}
				State->SetBoolField(TEXT("pure"), Dynamic->IsNodePure());
			}
			else if (const UK2Node_CustomEvent* Custom = Cast<UK2Node_CustomEvent>(&Node))
			{
				State->SetStringField(TEXT("event_name"), Custom->CustomFunctionName.ToString());
			}
			else if (const UK2Node_Event* Event = Cast<UK2Node_Event>(&Node))
			{
				State->SetStringField(TEXT("event_name"), Event->EventReference.GetMemberName().ToString());
				if (UClass* EventClass = Event->EventReference.GetMemberParentClass(
					Event->GetBlueprintClassFromNode()))
				{
					State->SetStringField(TEXT("event_class"), EventClass->GetPathName());
				}
			}
			else if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(&Node))
			{
				const UFunction* Function = Call->GetTargetFunction();
				const FString FunctionName = Function
					? Function->GetName()
					: Call->FunctionReference.GetMemberName().ToString();
				const UClass* OwnerClass = Function
					? Function->GetOwnerClass()
					: Call->FunctionReference.GetMemberParentClass(Call->GetBlueprintClassFromNode());
				if (Kind == TEXT("enhanced_input_add_mapping_context"))
				{
					if (const UEdGraphPin* Pin = FindPin(const_cast<UEdGraphNode*>(&Node), TEXT("MappingContext")))
					{
						const FString Path = Pin->DefaultObject
							? Pin->DefaultObject->GetPathName()
							: Pin->DefaultValue;
						if (!Path.IsEmpty()) State->SetStringField(TEXT("mapping_context"), Path);
					}
					if (const UEdGraphPin* Pin = FindPin(const_cast<UEdGraphNode*>(&Node), TEXT("Priority")))
					{
						double Priority = 0.0;
						if (LexTryParseString(Priority, *Pin->DefaultValue))
						{
							State->SetNumberField(TEXT("priority"), Priority);
						}
					}
					if (const UEdGraphPin* Pin = FindPin(const_cast<UEdGraphNode*>(&Node), TEXT("Options")))
					{
						if (!Pin->DefaultValue.IsEmpty())
						{
							State->SetStringField(TEXT("options_default_value"), Pin->DefaultValue);
						}
					}
				}
				else if (Kind != TEXT("enhanced_input_get_local_player_subsystem"))
				{
					State->SetStringField(TEXT("function_name"), FunctionName);
					if (OwnerClass)
					{
						State->SetStringField(TEXT("function_owner_class"), OwnerClass->GetPathName());
					}
				}
			}
			else if (const UK2Node_Variable* Variable = Cast<UK2Node_Variable>(&Node))
			{
				State->SetStringField(
					TEXT("variable_name"), Variable->VariableReference.GetMemberName().ToString());
				State->SetStringField(
					TEXT("access"), Node.IsA<UK2Node_VariableSet>() ? TEXT("set") : TEXT("get"));
				if (const UK2Node* K2Node = Cast<UK2Node>(&Node))
				{
					State->SetBoolField(TEXT("pure"), K2Node->IsNodePure());
				}
				if (!Variable->VariableReference.IsSelfContext())
				{
					if (UClass* Parent = Variable->VariableReference.GetMemberParentClass(
						Variable->GetBlueprintClassFromNode()))
					{
						State->SetStringField(TEXT("member_parent_class"), Parent->GetPathName());
					}
				}
			}
			else
			{
				State->SetStringField(TEXT("node_class"), Node.GetClass()->GetPathName());
				if (const UK2Node_ExecutionSequence* Sequence = Cast<UK2Node_ExecutionSequence>(&Node))
				{
					int32 OutputCount = 0;
					for (const UEdGraphPin* Pin : Sequence->Pins)
					{
						if (Pin && Pin->Direction == EGPD_Output
							&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
						{
							++OutputCount;
						}
					}
					State->SetNumberField(TEXT("exec_output_pin_count"), OutputCount);
				}
			}
			return State;
		}

		void BuildSCSParentMap(
			USCS_Node* Node,
			const FString& ParentName,
			TMap<const USCS_Node*, FString>& OutParents)
		{
			if (!Node) return;
			OutParents.Add(Node, ParentName);
			for (USCS_Node* Child : Node->GetChildNodes())
			{
				BuildSCSParentMap(Child, Node->GetVariableName().ToString(), OutParents);
			}
		}

		TSharedRef<FJsonObject> BuildAssetVerificationState(
			const UBlueprint& Blueprint,
			const FString& CanonicalAssetPath)
		{
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			State->SetStringField(
				TEXT("asset_path"),
				CanonicalAssetPath.IsEmpty()
					? Blueprint.GetOutermost()->GetName()
					: CanonicalAssetPath);
			State->SetStringField(TEXT("asset_kind"), GetAssetKind(&Blueprint));
			State->SetStringField(
				TEXT("parent_class"), Blueprint.ParentClass ? Blueprint.ParentClass->GetPathName() : FString());
			TArray<FString> InterfacePaths;
			for (const FBPInterfaceDescription& Description : Blueprint.ImplementedInterfaces)
			{
				if (const UClass* InterfaceClass = Description.Interface.Get())
				{
					InterfacePaths.Add(InterfaceClass->GetPathName());
				}
			}
			InterfacePaths.Sort();
			TArray<TSharedPtr<FJsonValue>> Interfaces;
			for (const FString& InterfacePath : InterfacePaths)
			{
				Interfaces.Add(MakeShared<FJsonValueString>(InterfacePath));
			}
			State->SetArrayField(TEXT("interfaces"), MoveTemp(Interfaces));

			TArray<FString> MemberKeys;
			for (const FBPVariableDescription& Variable : Blueprint.NewVariables)
			{
				MemberKeys.Add(GuidText(Variable.VarGuid) + TEXT(":")
					+ Variable.VarName.ToString() + TEXT(":") + Variable.VarType.PinCategory.ToString());
			}
			MemberKeys.Sort();
			TArray<TSharedPtr<FJsonValue>> Members;
			for (const FString& Key : MemberKeys) Members.Add(MakeShared<FJsonValueString>(Key));
			State->SetArrayField(TEXT("members"), Members);

			TArray<FString> ComponentKeys;
			if (Blueprint.SimpleConstructionScript)
			{
				for (const USCS_Node* Node : Blueprint.SimpleConstructionScript->GetAllNodes())
				{
					if (!Node) continue;
					ComponentKeys.Add(GuidText(Node->VariableGuid) + TEXT(":")
						+ Node->GetVariableName().ToString() + TEXT(":")
						+ (Node->ComponentClass ? Node->ComponentClass->GetPathName() : FString()));
				}
			}
			ComponentKeys.Sort();
			TArray<TSharedPtr<FJsonValue>> Components;
			for (const FString& Key : ComponentKeys) Components.Add(MakeShared<FJsonValueString>(Key));
			State->SetArrayField(TEXT("components"), Components);

			TArray<FString> GraphKeys;
			TArray<UEdGraph*> Graphs;
			Blueprint.GetAllGraphs(Graphs);
			for (const UEdGraph* Graph : Graphs)
			{
				if (!Graph) continue;
				FString GraphKey = GuidText(Graph->GraphGuid) + TEXT(":") + GetGraphKind(Blueprint, Graph)
					+ TEXT(":") + Graph->GetName();
				TArray<FString> NodeKeys;
				for (const UEdGraphNode* Node : Graph->Nodes)
				{
					if (!Node || Node->IsAutomaticallyPlacedGhostNode()) continue;
					NodeKeys.Add(NodeReference(Node) + TEXT(":") + GetNodeKind(Node));
				}
				NodeKeys.Sort();
				GraphKey += TEXT("[") + FString::Join(NodeKeys, TEXT(",")) + TEXT("]");
				GraphKeys.Add(MoveTemp(GraphKey));
			}
			GraphKeys.Sort();
			TArray<TSharedPtr<FJsonValue>> GraphValues;
			for (const FString& Key : GraphKeys) GraphValues.Add(MakeShared<FJsonValueString>(Key));
			State->SetArrayField(TEXT("graphs"), GraphValues);
			return State;
		}

		FBPVariableDescription* ResolveMemberFromRequest(
			UBlueprint& Blueprint,
			const UeAgentAuthoringState::FReadRequest& Request)
		{
			const FString GuidSelector = Request.TargetIdentity.GetSelector(TEXT("member_guid"));
			FGuid Guid;
			const bool bHasGuid = FGuid::Parse(GuidSelector, Guid);
			const FString Name = Request.TargetIdentity.GetSelector(TEXT("member_name"));
			for (FBPVariableDescription& Variable : Blueprint.NewVariables)
			{
				if ((bHasGuid && Variable.VarGuid == Guid)
					|| (!Name.IsEmpty() && Variable.VarName.ToString() == Name))
				{
					return &Variable;
				}
			}
			return nullptr;
		}

		UeAgentAuthoringState::EReadOutcome ReadState(
			const UeAgentAuthoringState::FReadRequest& Request,
			UeAgentAuthoringState::FStateValue& OutValue,
			FString& OutError)
		{
			OutValue = UeAgentAuthoringState::FStateValue();
			OutValue.TargetIdentity = Request.TargetIdentity;
			OutValue.Provenance = StateAccessorId;
			OutValue.bEditable = Request.SemanticId != ExportTargetSemanticId;
			OutError.Reset();
			if (Request.Context.FindRef(TEXT("expected_absent")) == TEXT("true"))
			{
				OutValue.bAvailable = false;
				OutValue.AvailabilityReason = TEXT("target_not_found");
				return UeAgentAuthoringState::EReadOutcome::Unavailable;
			}

			if (Request.SemanticId == ParentClassSemanticId)
			{
				const UBlueprint* Blueprint = Cast<UBlueprint>(Request.RootObject);
				if (!Blueprint || !Blueprint->ParentClass)
				{
					OutError = TEXT("blueprint_parent_class_state_invalid");
					return UeAgentAuthoringState::EReadOutcome::Failed;
				}
				OutValue.Type.Kind = TEXT("string");
				OutValue.Value = MakeShared<FJsonValueString>(Blueprint->ParentClass->GetPathName());
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}

			if (Request.SemanticId == InterfaceSemanticId)
			{
				const UBlueprint* Blueprint = Cast<UBlueprint>(Request.RootObject);
				const FString InterfacePath =
					Request.TargetIdentity.GetSelector(TEXT("interface_class"));
				const FBPInterfaceDescription* Description = Blueprint
					? Blueprint->ImplementedInterfaces.FindByPredicate(
						[&InterfacePath](const FBPInterfaceDescription& Candidate)
						{
							const UClass* InterfaceClass = Candidate.Interface.Get();
							return InterfaceClass
								&& InterfaceClass->GetPathName() == InterfacePath;
						})
					: nullptr;
				if (!Description)
				{
					OutValue.bAvailable = false;
					OutValue.AvailabilityReason = TEXT("interface_not_found");
					return UeAgentAuthoringState::EReadOutcome::Unavailable;
				}
				TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
				State->SetStringField(TEXT("class_path"), InterfacePath);
				OutValue.Value = MakeShared<FJsonValueObject>(State);
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}

			OutValue.Type.Kind = TEXT("object");
			if (Request.SemanticId == AssetSemanticId || Request.SemanticId == ExportTargetSemanticId)
			{
				const UBlueprint* Blueprint = Cast<UBlueprint>(Request.RootObject);
				if (!Blueprint || GetAssetKind(Blueprint).IsEmpty())
				{
					OutValue.bAvailable = false;
					OutValue.AvailabilityReason = TEXT("target_not_found");
					return UeAgentAuthoringState::EReadOutcome::Unavailable;
				}
				TSharedRef<FJsonObject> State = Request.SemanticId == AssetSemanticId
					? BuildAssetVerificationState(
						*Blueprint,
						Request.TargetIdentity.GetSelector(TEXT("asset_path")))
					: MakeShared<FJsonObject>();
				if (Request.SemanticId == ExportTargetSemanticId)
				{
					State->SetStringField(
						TEXT("asset_path"), Request.TargetIdentity.GetSelector(TEXT("asset_path")));
					State->SetStringField(TEXT("asset_kind"), GetAssetKind(Blueprint));
				}
				OutValue.Value = MakeShared<FJsonValueObject>(State);
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}

			if (Request.SemanticId == VariableSemanticId || Request.SemanticId == DelegateSemanticId)
			{
				UBlueprint* Blueprint = const_cast<UBlueprint*>(Cast<UBlueprint>(Request.RootObject));
				FBPVariableDescription* Variable = Blueprint
					? ResolveMemberFromRequest(*Blueprint, Request)
					: nullptr;
				if (!Variable)
				{
					OutValue.bAvailable = false;
					OutValue.AvailabilityReason = TEXT("member_not_found");
					return UeAgentAuthoringState::EReadOutcome::Unavailable;
				}
				TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
				State->SetStringField(TEXT("name"), Variable->VarName.ToString());
				if (Request.SemanticId == VariableSemanticId)
				{
					State->SetObjectField(TEXT("type"), BuildPinTypeState(Variable->VarType));
					State->SetBoolField(
						TEXT("instance_editable"),
						(Variable->PropertyFlags & CPF_DisableEditOnInstance) == 0);
				}
				OutValue.Value = MakeShared<FJsonValueObject>(State);
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}

			if (Request.SemanticId == ComponentSemanticId)
			{
				const UActorComponent* Component = Cast<UActorComponent>(Request.RootObject);
				if (!Component)
				{
					OutError = TEXT("blueprint_component_state_invalid");
					return UeAgentAuthoringState::EReadOutcome::Failed;
				}
				TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
				State->SetStringField(TEXT("name"), Request.Context.FindRef(TEXT("component_name")));
				State->SetStringField(TEXT("class"), Component->GetClass()->GetPathName());
				State->SetStringField(TEXT("parent"), Request.Context.FindRef(TEXT("parent_name")));
				if (Request.Context.FindRef(TEXT("source")) == TEXT("inherited"))
				{
					State->SetStringField(TEXT("operation"), TEXT("update"));
				}
				OutValue.Value = MakeShared<FJsonValueObject>(State);
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}

			if (Request.SemanticId == GraphSemanticId)
			{
				const UEdGraph* Graph = Cast<UEdGraph>(Request.RootObject);
				if (!Graph)
				{
					OutError = TEXT("blueprint_graph_state_invalid");
					return UeAgentAuthoringState::EReadOutcome::Failed;
				}
				TSharedRef<FJsonObject> Header = MakeShared<FJsonObject>();
				Header->SetStringField(TEXT("name"), Graph->GetName());
				Header->SetStringField(TEXT("graph_kind"), Request.Context.FindRef(TEXT("graph_kind")));
				TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
				State->SetObjectField(TEXT("graph"), Header);
				OutValue.Value = MakeShared<FJsonValueObject>(State);
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}

			if (Request.SemanticId == NodeSemanticId)
			{
				const UEdGraphNode* Node = Cast<UEdGraphNode>(Request.RootObject);
				if (!Node)
				{
					OutError = TEXT("blueprint_node_state_invalid");
					return UeAgentAuthoringState::EReadOutcome::Failed;
				}
				OutValue.Value = MakeShared<FJsonValueObject>(BuildNodeState(*Node));
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}

			if (Request.SemanticId == PinDefaultSemanticId)
			{
				TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
				State->SetStringField(TEXT("pin_name"), Request.Context.FindRef(TEXT("pin_name")));
				State->SetStringField(TEXT("default_value"), Request.Context.FindRef(TEXT("default_value")));
				OutValue.Value = MakeShared<FJsonValueObject>(State);
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}

			if (Request.SemanticId == EdgeSemanticId)
			{
				TSharedRef<FJsonObject> From = MakeShared<FJsonObject>();
				From->SetStringField(TEXT("node"), Request.Context.FindRef(TEXT("from_node")));
				From->SetStringField(TEXT("pin"), Request.Context.FindRef(TEXT("from_pin")));
				TSharedRef<FJsonObject> To = MakeShared<FJsonObject>();
				To->SetStringField(TEXT("node"), Request.Context.FindRef(TEXT("to_node")));
				To->SetStringField(TEXT("pin"), Request.Context.FindRef(TEXT("to_pin")));
				TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
				State->SetObjectField(TEXT("from"), From);
				State->SetObjectField(TEXT("to"), To);
				OutValue.Value = MakeShared<FJsonValueObject>(State);
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}

			OutError = TEXT("blueprint_state_semantic_unsupported:") + Request.SemanticId;
			return UeAgentAuthoringState::EReadOutcome::Failed;
		}

		bool AppendPropertyRequests(
			const UObject* Object,
			const FString& SemanticId,
			const FString& ModelId,
			const FString& TargetKind,
			const TMap<FString, FString>& BaseSelectors,
			const TSet<FName>& Excluded,
			TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
			FString& OutError)
		{
			if (!Object) return true;
			UeAgentAuthoringState::FEditablePropertyReadOptions Options;
			Options.SemanticId = SemanticId;
			Options.ExcludedProperties = Excluded;
			if (!ModelId.IsEmpty())
			{
				Options.BuildSnapshotIdentity =
				[ModelId, TargetKind, BaseSelectors](
					const FString& PropertyPath,
					UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
					FString& IdentityError)
				{
					TMap<FString, FString> Selectors = BaseSelectors;
					Selectors.Add(TEXT("property_path"), PropertyPath);
					return BuildIdentity(ModelId, TargetKind, Selectors, OutIdentity, IdentityError);
				};
			}
			return UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(
				Object, Options, OutRequests, OutError);
		}

		UeAgentAuthoringNativeDefaults::EProbeOutcome ProbeNativeDefault(
			const UeAgentAuthoringNativeDefaults::FRequest& Request,
			UeAgentAuthoringNativeDefaults::FScratchScope& Scratch,
			UeAgentAuthoringNativeDefaults::FProbeValue& OutValue,
			FString& OutError)
		{
			OutValue = UeAgentAuthoringNativeDefaults::FProbeValue();
			OutError.Reset();
			if (Request.TargetIdentity.TargetKind != TEXT("blueprint_asset"))
			{
				return UeAgentAuthoringNativeDefaults::EProbeOutcome::Unavailable;
			}
			UPackage* Package = Scratch.CreateScratchPackage(OutError);
			if (!Package) return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;
			UObject* Seed = CreateFactorySeed(
				TEXT("actor_blueprint"), Package, NAME_None, RF_Transactional, OutError);
			if (!Seed) return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;
			OutValue.Value = MakeShared<FJsonValueObject>(
				BuildAssetVerificationState(
					*CastChecked<UBlueprint>(Seed),
					Request.TargetIdentity.GetSelector(TEXT("asset_path"))));
			OutValue.Provenance = FactoryDefaultProviderId;
			return UeAgentAuthoringNativeDefaults::EProbeOutcome::Resolved;
		}
	}

	UObject* CreateFactorySeed(
		const FString& AssetKind,
		UObject* Outer,
		const FName Name,
		const EObjectFlags Flags,
		FString& OutError)
	{
		return CreateFactorySeed(
			AssetKind, AActor::StaticClass(), Outer, Name, Flags, OutError);
	}

	UObject* CreateFactorySeed(
		const FString& AssetKind,
		UClass* ParentClass,
		UObject* Outer,
		FName Name,
		const EObjectFlags Flags,
		FString& OutError)
	{
		OutError.Reset();
		if (AssetKind != TEXT("blueprint") && AssetKind != TEXT("actor_blueprint"))
		{
			OutError = TEXT("blueprint_factory_kind_invalid:") + AssetKind;
			return nullptr;
		}
		if (!Outer || !ParentClass
			|| !FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass)
			|| (AssetKind == TEXT("actor_blueprint")
				&& !ParentClass->IsChildOf(AActor::StaticClass())))
		{
			OutError = TEXT("blueprint_factory_input_invalid");
			return nullptr;
		}
		if (Name.IsNone()) Name = MakeUniqueObjectName(Outer, UBlueprint::StaticClass(), TEXT("BlueprintSeed"));
		UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
		Factory->ParentClass = ParentClass;
		UObject* Result = Factory->FactoryCreateNew(
			UBlueprint::StaticClass(), Outer, Name, Flags, nullptr, GWarn);
		if (!Result) OutError = TEXT("blueprint_factory_create_failed");
		return Result;
	}

	bool BuildAssetSnapshotReadRequest(
		UObject* Asset,
		const FString& CanonicalAssetPath,
		const FString& AssetKind,
		const bool bExpectedAbsent,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FString& OutError)
	{
		OutError.Reset();
		if (CanonicalAssetPath.IsEmpty()
			|| (!Asset && !bExpectedAbsent)
			|| (!AssetKind.IsEmpty()
				&& AssetKind != TEXT("blueprint")
				&& AssetKind != TEXT("actor_blueprint")))
		{
			OutError = TEXT("blueprint_snapshot_identity_invalid");
			return false;
		}
		UeAgentAuthoringState::FReadRequest Request;
		Request.SemanticId = AssetSemanticId;
		if (!BuildIdentity(
			AssetIdentityModelId,
			TEXT("blueprint_asset"),
			{ { TEXT("asset_path"), CanonicalAssetPath } },
			Request.TargetIdentity,
			OutError))
		{
			return false;
		}
		Request.TargetQuery.ModelId = Request.TargetIdentity.ModelId;
		Request.TargetQuery.TargetKind = Request.TargetIdentity.TargetKind;
		Request.TargetQuery.CanonicalKey = Request.TargetIdentity.CanonicalKey;
		Request.RootObject = Asset;
		Request.RootOwnerPath = CanonicalAssetPath;
		if (bExpectedAbsent) Request.Context.Add(TEXT("expected_absent"), TEXT("true"));
		OutRequests.Add(MoveTemp(Request));
		return true;
	}

	static bool BuildBlueprintExportSnapshotReadRequests(
		UBlueprint& BlueprintObject,
		const bool bResetRequests,
		const bool bIncludeAssetEnvelope,
		const bool bIncludeActorComponents,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FString& OutError,
		const FString& CanonicalAssetPath)
	{
		if (bResetRequests) OutRequests.Reset();
		OutError.Reset();
		UBlueprint* Blueprint = &BlueprintObject;
		const FString AssetPath = CanonicalAssetPath.IsEmpty()
			? Blueprint->GetOutermost()->GetName()
			: CanonicalAssetPath;
		const TMap<FString, FString> AssetSelectors = { { TEXT("asset_path"), AssetPath } };
		if (bIncludeAssetEnvelope && (!AddReadRequest(
			ExportTargetSemanticId, AssetIdentityModelId, TEXT("blueprint_asset"),
			AssetSelectors, Blueprint, {}, OutRequests, OutError)
			|| !AddReadRequest(
				ParentClassSemanticId, AssetIdentityModelId, TEXT("blueprint_asset"),
				AssetSelectors, Blueprint, {}, OutRequests, OutError)))
		{
			return false;
		}

		for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
		{
			const UClass* InterfaceClass = Description.Interface.Get();
			if (!InterfaceClass)
			{
				continue;
			}
			const FString InterfacePath = InterfaceClass->GetPathName();
			if (!AddReadRequest(
				InterfaceSemanticId,
				InterfaceIdentityModelId,
				TEXT("blueprint_interface"),
				{ { TEXT("asset_path"), AssetPath },
					{ TEXT("interface_class"), InterfacePath } },
				Blueprint,
				{},
				OutRequests,
				OutError))
			{
				return false;
			}
		}

		if (bIncludeAssetEnvelope && !AppendPropertyRequests(
			Blueprint,
			TEXT("core.property"),
			FString(),
			FString(),
			AssetSelectors,
			{ TEXT("ParentClass"), TEXT("GeneratedClass"), TEXT("SkeletonGeneratedClass"),
				TEXT("SimpleConstructionScript"), TEXT("UbergraphPages"), TEXT("FunctionGraphs"),
				TEXT("MacroGraphs"), TEXT("DelegateSignatureGraphs"), TEXT("NewVariables") },
			OutRequests,
			OutError))
		{
			return false;
		}

		for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
		{
			const bool bDelegate = Variable.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate;
			const TMap<FString, FString> Selectors = {
				{ TEXT("asset_path"), AssetPath },
				{ TEXT("member_guid"), GuidText(Variable.VarGuid) },
				{ TEXT("member_kind"), bDelegate ? TEXT("delegate") : TEXT("variable") },
				{ TEXT("member_name"), Variable.VarName.ToString() },
			};
			if (!AddReadRequest(
				bDelegate ? DelegateSemanticId : VariableSemanticId,
				MemberIdentityModelId,
				TEXT("blueprint_member"),
				Selectors,
				Blueprint,
				{},
				OutRequests,
				OutError))
			{
				return false;
			}
		}

		if (Blueprint->GeneratedClass)
		{
			UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject(false);
			if (!AppendPropertyRequests(
				CDO,
				ClassDefaultPropertySemanticId,
				ClassDefaultPropertyIdentityModelId,
				TEXT("blueprint_class_default_property"),
				AssetSelectors,
				{},
				OutRequests,
				OutError))
			{
				return false;
			}
		}

		if (bIncludeActorComponents)
		{
		TSet<FString> SCSNames;
		if (USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
		{
			TMap<const USCS_Node*, FString> Parents;
			for (USCS_Node* Root : SCS->GetRootNodes()) BuildSCSParentMap(Root, FString(), Parents);
			for (USCS_Node* Node : SCS->GetAllNodes())
			{
				if (!Node || !Node->ComponentTemplate || !Node->ComponentClass) continue;
				const FString Name = Node->GetVariableName().ToString();
				SCSNames.Add(Name);
				const TMap<FString, FString> Selectors = {
					{ TEXT("asset_path"), AssetPath },
					{ TEXT("component_guid"), GuidText(Node->VariableGuid) },
					{ TEXT("component_kind"), Node->ComponentClass->GetName() },
					{ TEXT("component_name"), Name },
				};
				const TMap<FString, FString> Context = {
					{ TEXT("component_name"), Name },
					{ TEXT("parent_name"), Parents.FindRef(Node) },
					{ TEXT("source"), TEXT("scs") },
				};
				if (!AddReadRequest(
					ComponentSemanticId, ComponentIdentityModelId, TEXT("blueprint_component"),
					Selectors, Node->ComponentTemplate, Context, OutRequests, OutError)
					|| !AppendPropertyRequests(
						Node->ComponentTemplate,
						ComponentPropertySemanticId,
						ComponentPropertyIdentityModelId,
						TEXT("blueprint_component_property"),
						Selectors,
						{},
						OutRequests,
						OutError))
				{
					return false;
				}
			}
		}

		if (Blueprint->GeneratedClass)
		{
			UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject(false);
			TMap<UActorComponent*, FString> Inherited;
			for (TFieldIterator<FObjectPropertyBase> It(Blueprint->GeneratedClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
			{
				FObjectPropertyBase* Property = *It;
				if (!Property || !Property->PropertyClass
					|| !Property->PropertyClass->IsChildOf(UActorComponent::StaticClass()))
				{
					continue;
				}
				UActorComponent* Component = Cast<UActorComponent>(
					Property->GetObjectPropertyValue_InContainer(CDO));
				if (Component && !SCSNames.Contains(Property->GetName()))
				{
					Inherited.Add(Component, Property->GetName());
				}
			}
			for (const TPair<UActorComponent*, FString>& Pair : Inherited)
			{
				UActorComponent* Component = Pair.Key;
				FString ParentName;
				if (const USceneComponent* Scene = Cast<USceneComponent>(Component))
				{
					if (const USceneComponent* Parent = Scene->GetAttachParent())
					{
						if (const FString* Found = Inherited.Find(const_cast<USceneComponent*>(Parent)))
						{
							ParentName = *Found;
						}
					}
				}
				const TMap<FString, FString> Selectors = {
					{ TEXT("asset_path"), AssetPath },
					{ TEXT("component_kind"), Component->GetClass()->GetName() },
					{ TEXT("component_name"), Pair.Value },
				};
				const TMap<FString, FString> Context = {
					{ TEXT("component_name"), Pair.Value },
					{ TEXT("parent_name"), ParentName },
					{ TEXT("source"), TEXT("inherited") },
				};
				if (!AddReadRequest(
					ComponentSemanticId, ComponentIdentityModelId, TEXT("blueprint_component"),
					Selectors, Component, Context, OutRequests, OutError)
					|| !AppendPropertyRequests(
						Component,
						ComponentPropertySemanticId,
						ComponentPropertyIdentityModelId,
						TEXT("blueprint_component_property"),
						Selectors,
						{},
						OutRequests,
						OutError))
				{
					return false;
				}
			}
		}

		}

		auto AppendGraphs = [&](const auto& Graphs, const FString& Kind)
		{
			for (UEdGraph* Graph : Graphs)
			{
				if (!Graph) continue;
				const TMap<FString, FString> GraphSelectors = {
					{ TEXT("asset_path"), AssetPath },
					{ TEXT("graph_guid"), GuidText(Graph->GraphGuid) },
					{ TEXT("graph_kind"), Kind },
					{ TEXT("graph_name"), Graph->GetName() },
				};
				if (!AddReadRequest(
					GraphSemanticId, GraphIdentityModelId, TEXT("blueprint_graph"),
					GraphSelectors, Graph, { { TEXT("graph_kind"), Kind } },
					OutRequests, OutError))
				{
					return false;
				}

				TMap<const UEdGraphNode*, FString> NodeRefs;
				TMap<FString, int32> Occurrences;
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (!Node || Node->IsAutomaticallyPlacedGhostNode()) continue;
					NodeRefs.Add(Node, NodeReference(Node));
				}
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (!Node || Node->IsAutomaticallyPlacedGhostNode()
						|| !GetBuiltinNodeId(Node).IsEmpty())
					{
						continue;
					}
					const FString NodeKind = GetNodeKind(Node);
					const FString NodeName = GetNodeSemanticName(*Node);
					const FString OccurrenceKey = NodeKind + TEXT(":") + NodeName;
					const FString Occurrence = LexToString(Occurrences.FindOrAdd(OccurrenceKey)++);
					TMap<FString, FString> NodeSelectors = GraphSelectors;
					NodeSelectors.Add(TEXT("node_guid"), GuidText(Node->NodeGuid));
					NodeSelectors.Add(TEXT("node_kind"), NodeKind);
					NodeSelectors.Add(TEXT("node_name"), NodeName);
					NodeSelectors.Add(TEXT("node_occurrence"), Occurrence);
					if (!AddReadRequest(
						NodeSemanticId, NodeIdentityModelId, TEXT("blueprint_node"),
						NodeSelectors, Node, {}, OutRequests, OutError)
						|| !AppendPropertyRequests(
							Node,
							NodePropertySemanticId,
							NodePropertyIdentityModelId,
							TEXT("blueprint_node_property"),
							NodeSelectors,
							{ TEXT("NodeGuid"), TEXT("NodePosX"), TEXT("NodePosY"), TEXT("Pins") },
							OutRequests,
							OutError))
					{
						return false;
					}

					for (UEdGraphPin* Pin : Node->Pins)
					{
						if (!Pin || Pin->Direction != EGPD_Input || Pin->DefaultValue.IsEmpty()) continue;
						TMap<FString, FString> PinSelectors = NodeSelectors;
						const FString PinName = Pin->PinName.ToString();
						const FString PinGuid = GuidText(Pin->PinId);
						PinSelectors.Add(TEXT("pin_name"), PinName);
						if (!PinGuid.IsEmpty()) PinSelectors.Add(TEXT("pin_guid"), PinGuid);
						PinSelectors.Add(
							TEXT("property_path"),
							PinGuid.IsEmpty() ? TEXT("pin_default.") + PinName : TEXT("pin_default.") + PinGuid);
						if (!AddReadRequest(
							PinDefaultSemanticId,
							PinDefaultIdentityModelId,
							TEXT("blueprint_pin_default"),
							PinSelectors,
							Node,
							{ { TEXT("pin_name"), PinName },
								{ TEXT("default_value"), Pin->DefaultValue } },
							OutRequests,
							OutError))
						{
							return false;
						}
					}
				}

				TSet<FString> SeenEdges;
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					const FString FromNode = NodeRefs.FindRef(Node);
					if (FromNode.IsEmpty()) continue;
					for (UEdGraphPin* Pin : Node->Pins)
					{
						if (!Pin || Pin->Direction != EGPD_Output) continue;
						for (UEdGraphPin* Linked : Pin->LinkedTo)
						{
							const FString ToNode = Linked
								? NodeRefs.FindRef(Linked->GetOwningNode())
								: FString();
							if (ToNode.IsEmpty()) continue;
							const FString EdgeKey = FromNode + TEXT("|") + Pin->PinName.ToString()
								+ TEXT("|") + ToNode + TEXT("|") + Linked->PinName.ToString();
							if (SeenEdges.Contains(EdgeKey)) continue;
							SeenEdges.Add(EdgeKey);
							TMap<FString, FString> EdgeSelectors = GraphSelectors;
							EdgeSelectors.Add(TEXT("edge_key"), EdgeKey);
							if (!AddReadRequest(
								EdgeSemanticId,
								EdgeIdentityModelId,
								TEXT("blueprint_edge"),
								EdgeSelectors,
								Graph,
								{ { TEXT("from_node"), FromNode },
									{ TEXT("from_pin"), Pin->PinName.ToString() },
									{ TEXT("to_node"), ToNode },
									{ TEXT("to_pin"), Linked->PinName.ToString() } },
								OutRequests,
								OutError))
							{
								return false;
							}
						}
					}
				}
			}
			return true;
		};

		if (!AppendGraphs(Blueprint->UbergraphPages, TEXT("event_graph"))
			|| !AppendGraphs(Blueprint->FunctionGraphs, TEXT("function_graph"))
			|| !AppendGraphs(Blueprint->MacroGraphs, TEXT("macro_graph")))
		{
			return false;
		}
		return true;
	}

	bool BuildAssetExportSnapshotReadRequests(
		UObject& Asset,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FString& OutError)
	{
		UBlueprint* Blueprint = Cast<UBlueprint>(&Asset);
		if (!Blueprint || GetAssetKind(Blueprint).IsEmpty())
		{
			OutRequests.Reset();
			OutError = TEXT("blueprint_export_asset_type_invalid");
			return false;
		}
		const bool bIncludeActorComponents = Blueprint->ParentClass
			&& Blueprint->ParentClass->IsChildOf(AActor::StaticClass());
		return BuildBlueprintExportSnapshotReadRequests(
			*Blueprint, true, true, bIncludeActorComponents, OutRequests, OutError, FString());
	}

	bool AppendReusableBlueprintExportSnapshotReadRequests(
		UBlueprint& Blueprint,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FString& OutError,
		const FString& CanonicalAssetPath)
	{
		return BuildBlueprintExportSnapshotReadRequests(
			Blueprint, false, false, false, OutRequests, OutError, CanonicalAssetPath);
	}

	bool RegisterStateAccessor(FString& OutError)
	{
		UeAgentAuthoringState::FStateAccessorDescriptor Descriptor;
		Descriptor.AccessorId = StateAccessorId;
		Descriptor.TargetKinds = {
			TEXT("blueprint_asset"), TEXT("blueprint_interface"), TEXT("blueprint_member"), TEXT("blueprint_component"),
			TEXT("blueprint_graph"), TEXT("blueprint_node"), TEXT("blueprint_pin_default"),
			TEXT("blueprint_edge")
		};
		Descriptor.CapabilityTags = {
			TEXT("asset"), TEXT("interface"), TEXT("member"), TEXT("scs"), TEXT("graph"), TEXT("node")
		};
		UeAgentAuthoringState::ERegistrationResult Result;
		return UeAgentAuthoringState::FStateAccessorRegistry::Get().RegisterAccessor(
			Descriptor, ReadState, Result, OutError);
	}

	bool RegisterNativeDefaultProvider(FString& OutError)
	{
		UeAgentAuthoringNativeDefaults::FProviderDescriptor Descriptor;
		Descriptor.ProviderId = FactoryDefaultProviderId;
		Descriptor.SourceKind = UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::Factory;
		Descriptor.TargetKinds = { TEXT("blueprint_asset") };
		Descriptor.CapabilityTags = { TEXT("factory"), TEXT("asset"), TEXT("blueprint") };
		Descriptor.bRequiresScratch = true;
		UeAgentAuthoringNativeDefaults::ERegistrationResult Result;
		return UeAgentAuthoringNativeDefaults::FProviderRegistry::Get().RegisterProvider(
			Descriptor, ProbeNativeDefault, Result, OutError);
	}
}
