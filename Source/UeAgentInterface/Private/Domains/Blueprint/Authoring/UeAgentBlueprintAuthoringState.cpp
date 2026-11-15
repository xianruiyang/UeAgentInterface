// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringState.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringIdentity.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintGraphSignature.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintNativeNormalization.h"

#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringOps.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringDescriptors.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "EnhancedInputSubsystems.h"
#include "Factories/BlueprintFactory.h"
#include "GameFramework/Actor.h"
#include "Internationalization/Text.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_EnhancedInputAction.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_MakeArray.h"
#include "K2Node_MakeContainer.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_PromotableOperator.h"
#include "K2Node_Tunnel.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Subsystems/SubsystemBlueprintLibrary.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringAssetIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringNativeDefaults.h"
#include "Utilities/UeAgentBlueprintInputBindings.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticCompare.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace UeAgentBlueprintAuthoring
{
	static bool AppendReparentHierarchyReadRequests(UBlueprint& Blueprint, const FString& AssetPath,
		TArray<UeAgentAuthoringState::FReadRequest>& Requests, FDiagnostic& Error);
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
			FDiagnostic& OutError)
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
			FDiagnostic& OutError)
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

		bool AppendMemberReadRequest(UBlueprint& Blueprint, const FString& AssetPath, const FBPVariableDescription& Member,
			TArray<UeAgentAuthoringState::FReadRequest>& Requests, FDiagnostic& Error)
		{
			const bool bDelegate = Member.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate;
			return AddReadRequest(bDelegate ? DelegateSemanticId : VariableSemanticId, MemberIdentityModelId,
				TEXT("blueprint_member"), {{TEXT("asset_path"), AssetPath}, {TEXT("member_guid"), GuidText(Member.VarGuid)},
					{TEXT("member_kind"), bDelegate ? TEXT("delegate") : TEXT("variable")}, {TEXT("member_name"), Member.VarName.ToString()}},
				&Blueprint, {}, Requests, Error);
		}

		bool AppendInterfaceReadRequest(UBlueprint& Blueprint, const FString& AssetPath, const UClass& Interface,
			TArray<UeAgentAuthoringState::FReadRequest>& Requests, FDiagnostic& Error)
		{
			return AddReadRequest(InterfaceSemanticId, InterfaceIdentityModelId, TEXT("blueprint_interface"),
				{{TEXT("asset_path"), AssetPath}, {TEXT("interface_class"), Interface.GetPathName()}}, &Blueprint, {}, Requests, Error);
		}

		bool CaptureBlueprintProjection(const FString& WorkspaceTargetKind, const TArray<UeAgentAuthoringState::FReadRequest>& Requests,
			UeAgentAuthoringState::FStateValue& OutValue, FDiagnostic& Error, int32* OutEntryCount = nullptr)
		{
			UeAgentAuthoringState::FCaptureResult Capture;
			if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture)) { Error = Capture.Diagnostic; return false; }
			if (OutEntryCount) *OutEntryCount = Capture.Snapshot.Num();
			auto Resources = UE::UAI::HjsonWorkspace::MakeSemanticWorkspaceResourceTracker();
			UE::UAI::HjsonWorkspace::FWorkspaceError ProjectionError;
			if (!UE::UAI::HjsonWorkspace::TryBuildVisibleAuthoringProjection(WorkspaceTargetKind, TEXT("llm_authoring"),
				Capture.Snapshot, Resources, OutValue.AuthoringProjection, ProjectionError)) {
				Error = ProjectionError.GetDiagnostic(); return false;
			}
			return true;
		}

		bool AppendObservedMemberReadRequests(UBlueprint& Blueprint, const UeAgentAuthoringState::FReadRequest& Request,
			const FString& AssetPath, TArray<UeAgentAuthoringState::FReadRequest>& Reads, FDiagnostic& Error);
		bool AppendLocalComponentReadRequests(const FString& AssetPath, USCS_Node& Node, const FString& Parent,
			TArray<UeAgentAuthoringState::FReadRequest>& Requests, FDiagnostic& Error);
		bool AppendObservedComponentReadRequests(UBlueprint& Blueprint, const FString& AssetPath,
			const TMap<FString, FString>& Context, TArray<UeAgentAuthoringState::FReadRequest>& Requests, FDiagnostic& Error);


		TSharedRef<FJsonObject> BuildPinTypeState(const FEdGraphPinType& PinType,
			const UBlueprint* Blueprint = nullptr, const FString& CanonicalAssetPath = FString())
		{
			return UeAgentBlueprintAuthoringOps::BuildPinTypeJsonObject(PinType, Blueprint, CanonicalAssetPath);
		}

		int32 GetPinOccurrence(const UEdGraphNode& Node, const UEdGraphPin& Target)
		{
			int32 Occurrence = 0;
			for (const UEdGraphPin* Pin : Node.Pins)
			{
				if (!Pin || Pin->bOrphanedPin)
				{
					continue;
				}
				if (Pin == &Target)
				{
					return Occurrence;
				}
				if (Pin->PinName == Target.PinName)
				{
					++Occurrence;
				}
			}
			return 0;
		}

		bool TryBuildNodePinAliases(
			const UEdGraphNode& Node,
			TMap<const UEdGraphPin*, FString>& OutAliasByPin,
			FDiagnostic& OutError)
		{
			using namespace UE::UAI::HjsonWorkspace;
			OutAliasByPin.Reset();
			TArray<FGraphAuthoringPinAliasInput> Inputs;
			TMap<FString, const UEdGraphPin*> PinBySourceId;
			for (const UEdGraphPin* Pin : Node.Pins)
			{
				if (!Pin || Pin->bOrphanedPin)
				{
					continue;
				}
				const FString SourceId = LexToString(Inputs.Num());
				Inputs.Add({
					SourceId,
					Pin->PinName.ToString(),
					GetPinOccurrence(Node, *Pin),
					Pin->Direction == EGPD_Output
						? EGraphAuthoringEndpointDirection::Output
						: EGraphAuthoringEndpointDirection::Input});
				PinBySourceId.Add(SourceId, Pin);
			}

			FGraphAuthoringPinAliasProjection Projection;
			FWorkspaceError Error;
			if (!TryBuildGraphAuthoringPinAliasProjection(Inputs, Projection, Error))
			{
				OutError = Error.GetDiagnostic();
				return false;
			}
			for (const TPair<FString, FString>& Alias : Projection.AliasBySourceId)
			{
				OutAliasByPin.Add(PinBySourceId.FindChecked(Alias.Key), Alias.Value);
			}
			return true;
		}

		TSharedRef<FJsonObject> BuildPinSelector(
			const UEdGraphNode& Node,
			const UEdGraphPin& Pin)
		{
			TSharedRef<FJsonObject> Selector = MakeShared<FJsonObject>();
			Selector->SetStringField(TEXT("name"), Pin.PinName.ToString());
			Selector->SetNumberField(TEXT("occurrence"), GetPinOccurrence(Node, Pin));
			return Selector;
		}

		bool CanAuthorPinDefault(const UEdGraphPin& Pin)
		{
			const FName Category = Pin.PinType.PinCategory;
			return Pin.Direction == EGPD_Input && !Pin.bOrphanedPin
				&& Pin.LinkedTo.IsEmpty()
				&& !Pin.bHidden && !Pin.bNotConnectable
				&& !Pin.bDefaultValueIsReadOnly && !Pin.bDefaultValueIsIgnored
				&& Pin.SubPins.IsEmpty() && Pin.PinType.ContainerType == EPinContainerType::None
				&& Category != UEdGraphSchema_K2::PC_Exec
				&& Category != UEdGraphSchema_K2::PC_Wildcard
				&& Category != UEdGraphSchema_K2::PC_Delegate
				&& Category != UEdGraphSchema_K2::PC_MCDelegate;
		}

		TSharedRef<FJsonObject> BuildPinInterfaceState(const UEdGraphNode& Node, const FString& CanonicalAssetPath)
		{
			TSharedRef<FJsonObject> Interface = MakeShared<FJsonObject>();
			const UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Node.GetGraph());
			TArray<TSharedPtr<FJsonValue>> Pins;
			for (const UEdGraphPin* Pin : Node.Pins)
			{
				if (!Pin || Pin->bOrphanedPin)
				{
					continue;
				}
				TSharedRef<FJsonObject> PinState = BuildPinSelector(Node, *Pin);
				PinState->SetStringField(
					TEXT("direction"),
					Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
				const bool bExec =
					Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
				PinState->SetStringField(TEXT("kind"), bExec ? TEXT("exec") : TEXT("data"));
				PinState->SetObjectField(TEXT("type"), BuildPinTypeState(Pin->PinType, Blueprint, CanonicalAssetPath));
				PinState->SetBoolField(
					TEXT("default_editable"),
					CanAuthorPinDefault(*Pin));
				if (Pin->bHidden)
				{
					PinState->SetBoolField(TEXT("hidden"), true);
				}
				if (Pin->bNotConnectable)
				{
					PinState->SetBoolField(TEXT("not_connectable"), true);
				}
				Pins.Add(MakeShared<FJsonValueObject>(PinState));
			}
			// Reconstruction may reorder native Pins (VariableSet moves self
			// after Output_Get). Compare the named interface, not that array order.
			Pins.Sort([](const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
			{
				const TSharedPtr<FJsonObject> L = Left->AsObject();
				const TSharedPtr<FJsonObject> R = Right->AsObject();
				const int32 NameOrder = L->GetStringField(TEXT("name")).Compare(
					R->GetStringField(TEXT("name")), ESearchCase::CaseSensitive);
				return NameOrder != 0 ? NameOrder < 0
					: L->GetNumberField(TEXT("occurrence")) < R->GetNumberField(TEXT("occurrence"));
			});
			Interface->SetArrayField(TEXT("pins"), MoveTemp(Pins));

			TArray<TSharedPtr<FJsonValue>> Groups;
			auto AddGroup = [&Node, &Groups](
				const TCHAR* Id,
				const int32 CurrentCount,
				const int32 MinCount,
				const TArray<UEdGraphPin*>& Members)
			{
				TSharedRef<FJsonObject> Group = MakeShared<FJsonObject>();
				Group->SetStringField(TEXT("id"), Id);
				Group->SetNumberField(TEXT("current_count"), CurrentCount);
				Group->SetNumberField(TEXT("min_count"), MinCount);
				Group->SetBoolField(TEXT("can_add"), true);
				Group->SetBoolField(TEXT("can_remove"), CurrentCount > MinCount);
				TArray<TSharedPtr<FJsonValue>> MemberRows;
				for (const UEdGraphPin* Member : Members)
				{
					if (Member && !Member->bOrphanedPin)
					{
						MemberRows.Add(MakeShared<FJsonValueObject>(
							BuildPinSelector(Node, *Member)));
					}
				}
				Group->SetArrayField(TEXT("members"), MoveTemp(MemberRows));
				Groups.Add(MakeShared<FJsonValueObject>(Group));
			};

			if (const UK2Node_ExecutionSequence* Sequence =
				Cast<UK2Node_ExecutionSequence>(&Node))
			{
				TArray<UEdGraphPin*> Members;
				for (UEdGraphPin* Pin : Sequence->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Output
						&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
					{
						Members.Add(Pin);
					}
				}
				AddGroup(TEXT("execution_outputs"), Members.Num(), 2, Members);
			}
			else if (const UK2Node_MakeArray* MakeArray = Cast<UK2Node_MakeArray>(&Node))
			{
				TArray<UEdGraphPin*> Members;
				for (UEdGraphPin* Pin : MakeArray->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Input && Pin->ParentPin == nullptr)
					{
						Members.Add(Pin);
					}
				}
				AddGroup(TEXT("inputs"), MakeArray->NumInputs, 1, Members);
			}
			Interface->SetArrayField(TEXT("pin_groups"), MoveTemp(Groups));
			return Interface;
		}

		FString GetGraphKind(const UBlueprint& Blueprint, const UEdGraph* Graph)
		{
			if (Blueprint.UbergraphPages.Contains(Graph)) return TEXT("event_graph");
			if (Blueprint.FunctionGraphs.Contains(Graph)) return TEXT("function_graph");
			if (Blueprint.MacroGraphs.Contains(Graph)) return TEXT("macro_graph");
			if (Blueprint.DelegateSignatureGraphs.Contains(Graph)) return TEXT("delegate_graph");
			return TEXT("graph");
		}

		FString GetNodeKind(const UEdGraphNode* Node)
		{
			if (Node && Node->IsA<UK2Node_MacroInstance>()) return TEXT("macro_instance");
			if (Node && Node->IsA<UK2Node_CallDelegate>()) return TEXT("call_delegate");
			if (Node && Node->IsA<UK2Node_ComponentBoundEvent>()) return TEXT("component_bound_event");
			if (Node && Node->IsA<UK2Node_EnhancedInputAction>()) return TEXT("enhanced_input_action_event");
			if (Node && Node->IsA<UK2Node_DynamicCast>()) return TEXT("dynamic_cast");
			if (Node && Node->IsA<UK2Node_CustomEvent>()) return TEXT("custom_event");
			if (Node && Node->IsA<UK2Node_Event>()) return TEXT("event");
			if (Node && Node->IsA<UK2Node_CallParentFunction>()) return TEXT("call_parent_function");
			if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
			{
				const UFunction* Function = Call->GetTargetFunction();
				const UBlueprint* Blueprint =
					FBlueprintEditorUtils::FindBlueprintForGraph(Node->GetGraph());
				if (Function && Blueprint
					&& (Function->GetOwnerClass() == Blueprint->GeneratedClass
						|| Function->GetOwnerClass() == Blueprint->SkeletonGeneratedClass))
				{
					TArray<UK2Node_CustomEvent*> Events;
					FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, Events);
					if (Events.ContainsByPredicate([Function](const UK2Node_CustomEvent* Event)
						{
							return Event && Event->CustomFunctionName == Function->GetFName();
						}))
					{
						return TEXT("call_custom_event");
					}
				}
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

		UEdGraphPin* FindPin(UEdGraphNode* Node, const TCHAR* Name)
		{
			if (!Node) return nullptr;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->PinName == Name) return Pin;
			}
			return nullptr;
		}

		TSharedRef<FJsonObject> BuildNodeState(const UEdGraphNode& Node, const FString& CanonicalAssetPath = FString())
		{
			TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
			const FString Kind = GetNodeKind(&Node);
			State->SetStringField(TEXT("id"), NodeReference(&Node));
			const FString NodeGuid = GuidText(Node.NodeGuid);
			if (!NodeGuid.IsEmpty())
			{
				State->SetStringField(TEXT("node_guid"), NodeGuid);
			}
			State->SetStringField(TEXT("name"), NodeSemanticName(Node));
			State->SetStringField(TEXT("node_type"), Kind);
			TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
			Position->SetNumberField(TEXT("x"), Node.NodePosX);
			Position->SetNumberField(TEXT("y"), Node.NodePosY);
			State->SetObjectField(TEXT("pos"), Position);
			State->SetObjectField(TEXT("pin_interface"), BuildPinInterfaceState(Node, CanonicalAssetPath));

			if (const UK2Node_ExecutionSequence* Sequence =
				Cast<UK2Node_ExecutionSequence>(&Node))
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
				TSharedRef<FJsonObject> PinGroups = MakeShared<FJsonObject>();
				PinGroups->SetNumberField(TEXT("execution_outputs"), OutputCount);
				State->SetObjectField(TEXT("pin_groups"), PinGroups);
			}
			else if (const UK2Node_MakeArray* MakeArray = Cast<UK2Node_MakeArray>(&Node))
			{
				TSharedRef<FJsonObject> PinGroups = MakeShared<FJsonObject>();
				PinGroups->SetNumberField(TEXT("inputs"), MakeArray->NumInputs);
				State->SetObjectField(TEXT("pin_groups"), PinGroups);
			}

			if (const UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(&Node))
			{
				if (const UEdGraph* MacroGraph = Macro->GetMacroGraph())
				{
					const UBlueprint* NodeOwner = Node.GetTypedOuter<UBlueprint>();
					const UBlueprint* MacroOwner = MacroGraph->GetTypedOuter<UBlueprint>();
					const FString TargetPackage = UeAgentAuthoringAssetIdentity::NormalizePackagePath(CanonicalAssetPath);
					const bool bLocal = MacroOwner && (MacroOwner == NodeOwner
						|| (!TargetPackage.IsEmpty() && MacroOwner->GetOutermost()->GetName() == TargetPackage));
					FString Reference = MacroGraph->GetName();
					if (!bLocal)
					{
						Reference = MacroGraph->GetPathName();
						if (MacroOwner)
							Reference = UeAgentAuthoringAssetIdentity::ToObjectPath(
								UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*MacroOwner))
								+ TEXT(":") + MacroGraph->GetPathName(MacroOwner);
					}
					State->SetStringField(TEXT("macro_graph"), Reference);
				}
			}
			else if (const UK2Node_CallDelegate* CallDelegate = Cast<UK2Node_CallDelegate>(&Node))
			{
				State->SetStringField(
					TEXT("delegate_member"), CallDelegate->GetPropertyName().ToString());
			}
			else if (const UK2Node_ComponentBoundEvent* Bound = Cast<UK2Node_ComponentBoundEvent>(&Node))
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
					State->SetStringField(TEXT("target_class"), UeAgentBlueprintAssetLifecycle::GetAuthoringTypeObjectPath(
						FBlueprintEditorUtils::FindBlueprintForGraph(Node.GetGraph()), *Dynamic->TargetType.Get(), CanonicalAssetPath));
				}
				State->SetBoolField(TEXT("pure"), Dynamic->IsNodePure());
			}
			else if (const UK2Node_CustomEvent* Custom = Cast<UK2Node_CustomEvent>(&Node))
			{
				State->SetStringField(TEXT("event_name"), Custom->CustomFunctionName.ToString());
				TSharedRef<FJsonObject> Signature = MakeShared<FJsonObject>();
				FDiagnostic SignatureError;
				if (CaptureCustomEventSignature(*Custom, Signature, SignatureError))
				{
					State->SetObjectField(TEXT("signature"), Signature);
				}
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
				if (Kind == TEXT("call_custom_event"))
				{
					State->SetStringField(TEXT("custom_event"), FunctionName);
				}
				else if (Kind == TEXT("enhanced_input_add_mapping_context"))
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
					const UBlueprint* Blueprint =
						FBlueprintEditorUtils::FindBlueprintForGraph(Node.GetGraph());
					const bool bOwnedByBlueprint = Blueprint && OwnerClass
						&& (OwnerClass == Blueprint->GeneratedClass
							|| OwnerClass == Blueprint->SkeletonGeneratedClass);
					const bool bHasLocalFunctionGraph = Blueprint
						&& Blueprint->FunctionGraphs.ContainsByPredicate(
							[&FunctionName](const UEdGraph* FunctionGraph)
							{
								return FunctionGraph
									&& FunctionGraph->GetName() == FunctionName;
							});
					if (bOwnedByBlueprint && bHasLocalFunctionGraph)
					{
						State->SetStringField(TEXT("function_graph"), FunctionName);
					}
					else
					{
						State->SetStringField(TEXT("function_name"), FunctionName);
						if (OwnerClass)
						{
							State->SetStringField(
								TEXT("function_owner_class"), OwnerClass->GetPathName());
						}
					}
				}
				if (const UK2Node_PromotableOperator* Operator =
					Cast<UK2Node_PromotableOperator>(&Node))
				{
					State->SetStringField(
						TEXT("operation_name"), Operator->GetOperationName().ToString());
					State->SetStringField(
						TEXT("node_class"), Operator->GetClass()->GetPathName());
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
			}
			return State;
		}

		void BuildSCSParentMap(
			UBlueprint& Blueprint,
			USCS_Node* Node,
			const FString& ParentName,
			const TMap<UActorComponent*, FString>& InheritedNames,
			TMap<const USCS_Node*, FString>& OutParents)
		{
			if (!Node) return;
			FString ResolvedParentName = ParentName;
			if (ResolvedParentName.IsEmpty() && Cast<USceneComponent>(Node->ComponentTemplate))
			{
				USceneComponent* ExternalParent = Node->GetParentComponentTemplate(&Blueprint);
				if (!ExternalParent && Node->ParentComponentOrVariableName.IsNone())
				{
					const AActor* CDO = Blueprint.GeneratedClass
						? Cast<AActor>(Blueprint.GeneratedClass->GetDefaultObject(false)) : nullptr;
					ExternalParent = CDO ? CDO->GetRootComponent() : nullptr;
				}
				if (ExternalParent)
				{
					const FString* ExportedName = InheritedNames.Find(ExternalParent);
					if (!ExportedName && !Node->bIsParentComponentNative
						&& !Node->ParentComponentOrVariableName.IsNone())
					{
						UActorComponent* Inherited = UeAgentBlueprintAuthoringOps::ResolveInheritedComponent(
							Blueprint, Node->ParentComponentOrVariableName.ToString());
						ExportedName = InheritedNames.Find(Inherited);
					}
					if (ExportedName) ResolvedParentName = *ExportedName;
				}
			}
			OutParents.Add(Node, ResolvedParentName);
			for (USCS_Node* Child : Node->GetChildNodes())
			{
				BuildSCSParentMap(Blueprint, Child, Node->GetVariableName().ToString(), InheritedNames, OutParents);
			}
		}

		TSharedRef<FJsonObject> BuildGeneratedGraphVerificationState(
			const UEdGraph& Graph, const FString& CanonicalAssetPath)
		{
			// Native generation order identifies children only within this snapshot.
			// Compare the generated program, not GUIDs replaced by ReconstructNode.
			TMap<const UEdGraphNode*, int32> Ordinals;
			for (const UEdGraphNode* Node : Graph.Nodes)
				if (Node && !Node->IsAutomaticallyPlacedGhostNode()) Ordinals.Add(Node, Ordinals.Num());
			TArray<TSharedPtr<FJsonValue>> Nodes;
			for (const UEdGraphNode* Node : Graph.Nodes)
			{
				if (!Node || !Ordinals.Contains(Node)) continue;
				auto Value = BuildNodeState(*Node, CanonicalAssetPath);
				for (const TCHAR* Field : {TEXT("id"), TEXT("node_guid"), TEXT("name"), TEXT("pos")})
					Value->RemoveField(Field);
				TArray<TSharedPtr<FJsonValue>> Pins;
				for (const UEdGraphPin* Pin : Node->Pins)
				{
					if (!Pin) continue;
					auto PinValue = BuildPinSelector(*Node, *Pin);
					PinValue->SetStringField(TEXT("default"), Pin->GetDefaultAsString());
					PinValue->SetBoolField(TEXT("orphaned"), Pin->bOrphanedPin);
					TArray<FString> Links;
					for (const UEdGraphPin* Other : Pin->LinkedTo) if (Other)
					{
						const UEdGraphNode* Owner = Other->GetOwningNode();
						const int32* Ordinal = Ordinals.Find(Owner);
						Links.Add((Ordinal ? LexToString(*Ordinal) : NodeReference(Owner))
							+ TEXT(":") + Other->PinName.ToString() + TEXT(":")
							+ LexToString(GetPinOccurrence(*Owner, *Other)));
					}
					Links.Sort();
					TArray<TSharedPtr<FJsonValue>> LinkValues;
					for (const auto& Link : Links) LinkValues.Add(MakeShared<FJsonValueString>(Link));
					PinValue->SetArrayField(TEXT("links"), MoveTemp(LinkValues));
					Pins.Add(MakeShared<FJsonValueObject>(PinValue));
				}
				Value->SetArrayField(TEXT("pin_values"), MoveTemp(Pins));
				Nodes.Add(MakeShared<FJsonValueObject>(Value));
			}
			auto Result = MakeShared<FJsonObject>();
			Result->SetArrayField(TEXT("nodes"), MoveTemp(Nodes));
			return Result;
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
			auto GeneratedGraphs = MakeShared<FJsonObject>();
			TArray<UEdGraph*> Graphs;
			Blueprint.GetAllGraphs(Graphs);
			for (const UEdGraph* Graph : Graphs)
			{
				if (!Graph) continue;
				FString GraphKey = GuidText(Graph->GraphGuid) + TEXT(":") + GetGraphKind(Blueprint, Graph)
					+ TEXT(":") + Graph->GetName();
				if (IsNativeGeneratedGraph(*Graph))
				{
					GeneratedGraphs->SetObjectField(GraphKey,
						BuildGeneratedGraphVerificationState(*Graph, CanonicalAssetPath));
					GraphKeys.Add(MoveTemp(GraphKey));
					continue;
				}
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
			if (!GeneratedGraphs->Values.IsEmpty()) State->SetObjectField(TEXT("generated_graphs"), GeneratedGraphs);
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

		UEdGraph* ResolveDelegateSignatureGraph(
			const UBlueprint& Blueprint,
			const FName DelegateName)
		{
			for (UEdGraph* Graph : Blueprint.DelegateSignatureGraphs)
			{
				if (Graph && Graph->GetFName() == DelegateName)
				{
					return Graph;
				}
			}
			return nullptr;
		}

		UeAgentAuthoringState::EReadOutcome ReadState(
			const UeAgentAuthoringState::FReadRequest& Request,
			UeAgentAuthoringState::FStateValue& OutValue,
			FDiagnostic& OutError)
		{
			OutValue = UeAgentAuthoringState::FStateValue();
			OutValue.TargetIdentity = Request.TargetIdentity;
			OutValue.Provenance = StateAccessorId;
			OutValue.bEditable = Request.SemanticId != ExportTargetSemanticId;
			OutError.Reset();
			const auto RootTypeValue = [&]() -> TSharedPtr<FJsonValue>
			{
				return Request.RootObject ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Request.RootObject->GetClass()->GetPathName()))
					: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
			};
			const auto FailNativeState = [&](const TCHAR* Code, const FString& Reason, const FString& Field,
				TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Name)
			{
				OutError = FDiagnostic::Failure(Code, Reason, TEXT("blueprint_state"));
				OutError.SourceKind = TEXT("native_result");
				OutError.SemanticId = Request.SemanticId;
				OutError.TargetPath = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
				OutError.CandidateName = Name;
				OutError.PropertyPath = Field;
				OutError.ActualValue = MoveTemp(Actual);
				OutError.ExpectedValue = MoveTemp(Expected);
				return UeAgentAuthoringState::EReadOutcome::Failed;
			};
			if (Request.Context.FindRef(TEXT("expected_absent")) == TEXT("true"))
			{
				// Desired absence does not imply the target is already absent during preparation.
				if (Request.Context.FindRef(TEXT("observe_absence")) == TEXT("true"))
				{
					bool bPresent = false;
					if (!UE::UAI::HjsonWorkspace::DomainAuthoring::TryInspectObjectTargetPresence(
						Request.TargetIdentity.GetSelector(TEXT("asset_path")), bPresent))
					{
						return FailNativeState(TEXT("blueprint_presence_target_invalid"),
							TEXT("The native query could not determine whether the expected-absent Blueprint target is present."),
							TEXT("presence_query_succeeded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), FString());
					}
					if (bPresent)
					{
						return FailNativeState(TEXT("blueprint_expected_absent_target_present"),
							TEXT("The Blueprint target is still present, but this snapshot requires absence."),
							TEXT("asset_present"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), FString());
					}
				}
				OutValue.bAvailable = false;
				OutValue.AvailabilityReason = TEXT("target_not_found");
				return UeAgentAuthoringState::EReadOutcome::Unavailable;
			}

			if (Request.SemanticId == ParentClassSemanticId)
			{
				const UBlueprint* Blueprint = Cast<UBlueprint>(Request.RootObject);
				if (!Blueprint || !Blueprint->ParentClass)
				{
					return FailNativeState(TEXT("blueprint_parent_class_state_invalid"),
						!Blueprint ? TEXT("Reading the Blueprint parent class requires a Blueprint root object.") : TEXT("The Blueprint has no parent class."),
						!Blueprint ? TEXT("root_object") : TEXT("parent_class"),
						!Blueprint ? RootTypeValue() : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
						MakeShared<FJsonValueString>(!Blueprint ? UBlueprint::StaticClass()->GetPathName() : UClass::StaticClass()->GetPathName()), FString());
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
				else {
					// Keep the native verification envelope for operation comparison.
					// Its GUID summaries have no published property identity.
					OutValue.bAuthoringVisible = false;
					const FString* GraphGuids = Request.Context.Find(ObservedGraphGuidsContext);
					const bool bHasMembers = Request.Context.Contains(ObservedMemberCapture);
					bool bHasComponents = false;
					if (Request.bCaptureAuthoringProjection)
						for (const auto& Field : Request.Context)
							if (Field.Key.StartsWith(ObservedComponentPrefix)) { bHasComponents = true; break; }
					if (Request.bCaptureAuthoringProjection && Request.Context.Contains(ObservedReparentHierarchy)) {
						TArray<UeAgentAuthoringState::FReadRequest> Reads;
						if (!AppendReparentHierarchyReadRequests(*const_cast<UBlueprint*>(Blueprint),
							Request.TargetIdentity.GetSelector(TEXT("asset_path")), Reads, OutError)
							|| !CaptureBlueprintProjection(TEXT("ue.blueprint.asset"), Reads, OutValue, OutError))
							return UeAgentAuthoringState::EReadOutcome::Failed;
					} else if (Request.bCaptureAuthoringProjection && (GraphGuids || bHasMembers || bHasComponents)) {
						const FString AssetPath = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
						TArray<UeAgentAuthoringState::FReadRequest> Roots;
						if (!AddReadRequest(ExportTargetSemanticId, AssetIdentityModelId, TEXT("blueprint_asset"),
							{{TEXT("asset_path"), AssetPath}}, Blueprint, {}, Roots, OutError))
							return UeAgentAuthoringState::EReadOutcome::Failed;
						if (bHasComponents && !AppendObservedComponentReadRequests(*const_cast<UBlueprint*>(Blueprint),
							AssetPath, Request.Context, Roots, OutError)) return UeAgentAuthoringState::EReadOutcome::Failed;
						if (bHasMembers && !AppendObservedMemberReadRequests(*const_cast<UBlueprint*>(Blueprint),
							Request, AssetPath, Roots, OutError)) return UeAgentAuthoringState::EReadOutcome::Failed;
						const bool bCaptured = GraphGuids
							? CaptureObservedGraphBodies(*const_cast<UBlueprint*>(Blueprint), AssetPath, *GraphGuids,
								TEXT("ue.blueprint.asset"), MoveTemp(Roots), OutValue, OutError, &Request.Context)
							: CaptureBlueprintProjection(TEXT("ue.blueprint.asset"), Roots, OutValue, OutError);
						if (!bCaptured) return UeAgentAuthoringState::EReadOutcome::Failed;
					}
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
				State->SetStringField(TEXT("guid"), GuidText(Variable->VarGuid));
				if (Request.SemanticId == VariableSemanticId)
				{
					State->SetObjectField(TEXT("type"), BuildPinTypeState(Variable->VarType, Blueprint,
						Request.TargetIdentity.GetSelector(TEXT("asset_path"))));
					State->SetBoolField(
						TEXT("instance_editable"),
						(Variable->PropertyFlags & CPF_DisableEditOnInstance) == 0);
				}
				else
				{
					UEdGraph* SignatureGraph = ResolveDelegateSignatureGraph(
						*Blueprint, Variable->VarName);
					TSharedRef<FJsonObject> Signature = MakeShared<FJsonObject>();
					if (!SignatureGraph)
					{
						return FailNativeState(TEXT("blueprint_delegate_signature_graph_missing"),
							TEXT("The Blueprint delegate has no native signature graph."),
							TEXT("signature_graph"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Variable->VarName.ToString());
					}
					if (!CaptureDelegateSignatureGraph(*SignatureGraph, Signature, OutError))
					{
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Blueprint);
						if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Variable->VarName.ToString();
						if (OutError.SourceKind.IsEmpty() && OutError.Source.SourceFile.IsEmpty()) OutError.SourceKind = TEXT("native_result");
						return UeAgentAuthoringState::EReadOutcome::Failed;
					}
					State->SetObjectField(TEXT("signature"), Signature);
				}
				OutValue.Value = MakeShared<FJsonValueObject>(State);
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}

			if (Request.SemanticId == ComponentSemanticId)
			{
				const UActorComponent* Component = Cast<UActorComponent>(Request.RootObject);
				if (!Component)
				{
					return FailNativeState(TEXT("blueprint_component_state_invalid"), TEXT("Reading component state requires an ActorComponent root object."),
						TEXT("root_object"), RootTypeValue(), MakeShared<FJsonValueString>(UActorComponent::StaticClass()->GetPathName()), Request.Context.FindRef(TEXT("component_name")));
				}
				TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
				OutValue.bEditable = Request.Context.FindRef(TEXT("source")) != TEXT("inherited")
					|| Request.Context.FindRef(TEXT("native_class_override")) == TEXT("true");
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
					return FailNativeState(TEXT("blueprint_graph_state_invalid"),
						FString::Printf(TEXT("Reading '%s' graph state requires a UEdGraph root object."), *Request.Context.FindRef(TEXT("graph_kind"))),
						TEXT("root_object"), RootTypeValue(), MakeShared<FJsonValueString>(UEdGraph::StaticClass()->GetPathName()), Request.TargetIdentity.GetSelector(TEXT("graph_name")));
				}
				TSharedRef<FJsonObject> Header = MakeShared<FJsonObject>();
				Header->SetStringField(TEXT("id"), GraphIdentity(*Graph));
				Header->SetStringField(TEXT("graph_guid"), Graph->GraphGuid.ToString(EGuidFormats::DigitsWithHyphens));
				Header->SetStringField(TEXT("name"), Graph->GetName());
				Header->SetStringField(TEXT("graph_kind"), Request.Context.FindRef(TEXT("graph_kind")));
				if (const UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph))
					AppendGraphDisplayState(*Blueprint, *Graph, *Header, Request.RootOwnerPath);
				// Generic graph declarations are asset-owned; animation schemas retain their domain owner.
				const FString GraphKind = Request.Context.FindRef(TEXT("graph_kind"));
				Header->SetBoolField(TEXT("graph_declaration_owned"), Graph->GetSchema()
					&& Graph->GetSchema()->GetClass() == UEdGraphSchema_K2::StaticClass()
					&& (GraphKind == TEXT("event_graph") || GraphKind == TEXT("function_graph")
						|| GraphKind == TEXT("macro_graph")));
				if ((GraphKind == TEXT("function_graph")
						&& CanAuthorFunctionGraphSignature(*Graph))
					|| (GraphKind == TEXT("macro_graph")
						&& CanAuthorMacroGraphSignature(*Graph)))
				{
					TSharedRef<FJsonObject> Signature = MakeShared<FJsonObject>();
					const bool bCaptured = GraphKind == TEXT("function_graph")
						? CaptureFunctionGraphSignature(*Graph, Signature, OutError)
						: CaptureMacroGraphSignature(*Graph, Signature, OutError);
					if (!bCaptured)
					{
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
						if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Graph->GetName();
						if (OutError.SourceKind.IsEmpty() && OutError.Source.SourceFile.IsEmpty()) OutError.SourceKind = TEXT("native_result");
						return UeAgentAuthoringState::EReadOutcome::Failed;
					}
					Header->SetObjectField(TEXT("signature"), Signature);
				}
				if (Request.Context.FindRef(TEXT("native_generated_verification")) == TEXT("true")
					&& IsNativeGeneratedGraph(*Graph))
				{
					Header->SetObjectField(TEXT("generated_program"), BuildGeneratedGraphVerificationState(
						*Graph, Request.TargetIdentity.GetSelector(TEXT("asset_path"))));
				}
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
					return FailNativeState(TEXT("blueprint_node_state_invalid"), TEXT("Reading node state requires a UEdGraphNode root object."),
						TEXT("root_object"), RootTypeValue(), MakeShared<FJsonValueString>(UEdGraphNode::StaticClass()->GetPathName()), FString());
				}
				OutValue.Value = MakeShared<FJsonValueObject>(BuildNodeState(*Node,
					Request.TargetIdentity.GetSelector(TEXT("asset_path"))));
				return UeAgentAuthoringState::EReadOutcome::Resolved;
			}

			if (Request.SemanticId == PinDefaultSemanticId)
			{
				const UEdGraphNode* Node = Cast<UEdGraphNode>(Request.RootObject);
				FGuid PinGuid;
				const FString PinGuidText =
					Request.TargetIdentity.GetSelector(TEXT("pin_guid"));
				if (!Node || !FGuid::Parse(PinGuidText, PinGuid))
				{
					OutError = FDiagnostic::Failure(TEXT("blueprint_pin_default_identity_invalid"),
						!Node ? TEXT("Reading a Blueprint pin default requires a graph node as the root object.")
							: TEXT("The native pin selector is not a valid GUID."), TEXT("blueprint_state"), PinGuidText);
					OutError.SourceKind = TEXT("native_result");
					OutError.TargetPath = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
					OutError.PropertyPath = !Node ? TEXT("root_object") : TEXT("pin_identity");
					OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
					OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					return UeAgentAuthoringState::EReadOutcome::Failed;
				}
				const UEdGraphPin* Pin = nullptr;
				for (const UEdGraphPin* Candidate : Node->Pins)
				{
					if (Candidate && Candidate->PinId == PinGuid)
					{
						Pin = Candidate;
						break;
					}
				}
				FString DefaultValue;
				if (Pin)
				{
					DefaultValue = Pin->GetDefaultAsString();
					if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Text)
					{
						const FString* Source = FTextInspector::GetSourceString(Pin->DefaultTextValue);
						DefaultValue = Source ? *Source : Pin->DefaultTextValue.ToString();
					}
				}
				TSharedPtr<FJsonObject> ValueDefinition;
				const UBlueprint* OwnerBlueprint = FBlueprintEditorUtils::FindBlueprintForNode(Node);
				const FString TargetPath = OwnerBlueprint ? UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*OwnerBlueprint)
					: Request.TargetIdentity.GetSelector(TEXT("asset_path"));
				if (!Pin)
				{
					OutError = FDiagnostic::Failure(TEXT("blueprint_pin_default_not_found"),
						TEXT("The selected native pin was not found on the graph node."), TEXT("blueprint_state"), PinGuidText);
					OutError.SourceKind = TEXT("native_result");
					OutError.TargetPath = TargetPath;
					OutError.CandidateName = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
					OutError.PropertyPath = TEXT("pin_present");
					OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
					OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					return UeAgentAuthoringState::EReadOutcome::Failed;
				}
				if (!UeAgentBlueprintAuthoringOps::TryDescribePinDefault(
						Pin->PinType, DefaultValue, OutValue.Type,
						OutValue.Value, OutError, &ValueDefinition))
				{
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TargetPath;
					if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = TEXT("default_value");
					if (!OutError.ActualEvidence.IsValid())
					{
						auto Facts = MakeShared<FJsonObject>();
						Facts->SetStringField(TEXT("node"), Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
						Facts->SetStringField(TEXT("pin"), Pin->PinName.ToString());
						Facts->SetObjectField(TEXT("pin_type"), UeAgentBlueprintAuthoringOps::BuildPinTypeJsonObject(Pin->PinType, OwnerBlueprint, TargetPath));
						OutError.ActualEvidence = MakeShared<FJsonValueObject>(Facts);
					}
					if (Pin)
					{
						if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Pin->PinName.ToString();
						OutError.InternalContext += FString::Printf(TEXT(":node=%s pin=%s type=%s/%s default='%s' linked=%d"),
							*Node->GetName(), *Pin->PinName.ToString(),
							*Pin->PinType.PinCategory.ToString(), *Pin->PinType.PinSubCategory.ToString(),
							*DefaultValue, Pin->LinkedTo.Num());
					}
					return UeAgentAuthoringState::EReadOutcome::Failed;
				}
				if (ValueDefinition.IsValid())
					OutValue.ValueDefinition = MakeShared<FJsonValueObject>(ValueDefinition);
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

			FailNativeState(TEXT("blueprint_state_semantic_unsupported"),
				FString::Printf(TEXT("Blueprint state accessor '%s' does not support semantic '%s'."), *FString(StateAccessorId), *Request.SemanticId),
				TEXT("semantic_id"), MakeShared<FJsonValueString>(Request.SemanticId),
				MakeShared<FJsonValueString>(TEXT("a registered Blueprint state semantic")), FString());
			OutError.Stage = TEXT("state_snapshot");
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
			FDiagnostic& OutError,
			const TSet<FName>* Included = nullptr)
		{
			if (!Object) return true;
			UeAgentAuthoringState::FEditablePropertyReadOptions Options;
			Options.SemanticId = SemanticId;
			Options.ExcludedProperties = Excluded;
			if (Included) Options.IncludeProperty = [Included](const FProperty* Property, const void*, const UObject*)
				{ return Included->Contains(Property->GetFName()); };
			if (!ModelId.IsEmpty())
			{
				Options.BuildSnapshotIdentity =
				[ModelId, TargetKind, BaseSelectors](
					const FString& PropertyPath,
					UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
					FDiagnostic& IdentityError)
				{
					TMap<FString, FString> Selectors = BaseSelectors;
					Selectors.Add(TEXT("property_path"), PropertyPath);
					return BuildIdentity(ModelId, TargetKind, Selectors, OutIdentity, IdentityError);
				};
			}
			return UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(
				Object, Options, OutRequests, OutError);
		}

		bool AppendLocalComponentReadRequests(const FString& AssetPath, USCS_Node& Node, const FString& Parent,
			TArray<UeAgentAuthoringState::FReadRequest>& Requests, FDiagnostic& Error)
		{
			const FString Name = Node.GetVariableName().ToString();
			const TMap<FString, FString> Selectors{{TEXT("asset_path"), AssetPath}, {TEXT("component_guid"), GuidText(Node.VariableGuid)},
				{TEXT("component_kind"), Node.ComponentClass->GetName()}, {TEXT("component_name"), Name}};
			return AddReadRequest(ComponentSemanticId, ComponentIdentityModelId, TEXT("blueprint_component"), Selectors, Node.ComponentTemplate,
				{{TEXT("component_name"), Name}, {TEXT("parent_name"), Parent}, {TEXT("source"), TEXT("scs")}}, Requests, Error)
				&& AppendPropertyRequests(Node.ComponentTemplate, ComponentPropertySemanticId, ComponentPropertyIdentityModelId,
					TEXT("blueprint_component_property"), Selectors, {}, Requests, Error);
		}

		bool AppendObservedComponentReadRequests(UBlueprint& Blueprint, const FString& AssetPath,
			const TMap<FString, FString>& Context, TArray<UeAgentAuthoringState::FReadRequest>& Requests, FDiagnostic& Error)
		{
			USimpleConstructionScript* SCS = Blueprint.SimpleConstructionScript;
			TArray<UeAgentBlueprintAuthoringOps::FInheritedComponent> InheritedComponents;
			UeAgentBlueprintAuthoringOps::GatherInheritedComponents(Blueprint, InheritedComponents);
			TMap<UActorComponent*, FString> Inherited;
			for (const auto& Entry : InheritedComponents) Inherited.Add(Entry.Component, Entry.Name);
			TMap<const USCS_Node*, FString> Parents;
			if (SCS)
				for (USCS_Node* Root : SCS->GetRootNodes()) BuildSCSParentMap(Blueprint, Root, FString(), Inherited, Parents);
			TSet<FName> Names;
			for (const auto& Field : Context)
			{
				if (!Field.Key.StartsWith(ObservedComponentPrefix)) continue;
				const FName Name(*Field.Key.RightChop(FCString::Strlen(ObservedComponentPrefix)));
				FGuid Guid;
				const bool bPresent = !Field.Value.IsEmpty();
				const bool bValid = !bPresent || (FGuid::ParseExact(Field.Value, EGuidFormats::DigitsWithHyphensLower, Guid) && Guid.IsValid());
				USCS_Node* Match = nullptr;
				int32 Count = 0;
				if (SCS)
					for (USCS_Node* Node : SCS->GetAllNodes())
						if (Node && (Node->GetVariableName() == Name || (bPresent && Node->VariableGuid == Guid))) { Match = Node; ++Count; }
				if (!SCS || !bValid || (bPresent ? Count != 1 || Match->VariableGuid != Guid || Match->GetVariableName() != Name
					|| !Match->ComponentClass || !Match->ComponentTemplate || !Parents.Contains(Match) : Count != 0))
				{
					Error = FDiagnostic::Failure(TEXT("blueprint_observed_component_binding_missing"),
						TEXT("The current construction script does not match the observed local component identity and presence."), TEXT("blueprint_state"));
					Error.TargetPath = AssetPath;
					Error.PropertyPath = Name.ToString();
					return false;
				}
				if (Match && !AppendLocalComponentReadRequests(AssetPath, *Match, Parents.FindChecked(Match), Requests, Error)) return false;
				Names.Add(Name);
			}
			const UObject* CDO = Blueprint.GeneratedClass ? Blueprint.GeneratedClass->GetDefaultObject(false) : nullptr;
			return AppendPropertyRequests(CDO, ClassDefaultPropertySemanticId, ClassDefaultPropertyIdentityModelId,
				TEXT("blueprint_class_default_property"), {{TEXT("asset_path"), AssetPath}}, {}, Requests, Error, &Names);
		}

		bool AppendObservedMemberReadRequests(UBlueprint& Blueprint, const UeAgentAuthoringState::FReadRequest& Request,
			const FString& AssetPath, TArray<UeAgentAuthoringState::FReadRequest>& Reads, FDiagnostic& Error)
		{
			const UObject* CDO = Blueprint.GeneratedClass ? Blueprint.GeneratedClass->GetDefaultObject(false) : nullptr;
			TSet<FName> SelectedDefaults;
			int32 RequiredDefaultCount = 0;
			for (const auto& Field : Request.Context) {
				if (Field.Key.StartsWith(ObservedInterfacePrefix)) {
					const FString Path = Field.Key.RightChop(FCString::Strlen(ObservedInterfacePrefix));
					const auto* Description = Blueprint.ImplementedInterfaces.FindByPredicate([&](const auto& Item) {
						return Item.Interface && Item.Interface->GetPathName() == Path;
					});
					if ((Description != nullptr) != (Field.Value == TEXT("present"))) {
						Error = FDiagnostic::Failure(TEXT("blueprint_observed_interface_binding_missing"),
							TEXT("The prepared interface declaration does not match the current Blueprint."), TEXT("blueprint_state"));
						Error.TargetPath = AssetPath;
						Error.PropertyPath = Path;
						return false;
					}
					if (Description && !AppendInterfaceReadRequest(Blueprint, AssetPath, *Description->Interface.Get(), Reads, Error)) return false;
					continue;
				}
				const TCHAR* Prefix = nullptr;
				for (const TCHAR* Candidate : {ObservedMemberPrefix, ObservedMissingMemberPrefix, ObservedDelegatePrefix,
					ObservedMissingDelegatePrefix, ObservedDefaultMemberPrefix})
					if (Field.Key.StartsWith(Candidate, ESearchCase::CaseSensitive)) { Prefix = Candidate; break; }
				if (!Prefix) continue;
				FGuid Guid;
				const FName Name(*Field.Value);
				const bool bGuidValid = FGuid::ParseExact(Field.Key.RightChop(FCString::Strlen(Prefix)),
					EGuidFormats::DigitsWithHyphensLower, Guid) && Guid.IsValid();
				const bool bPresent = Prefix != ObservedMissingMemberPrefix && Prefix != ObservedMissingDelegatePrefix;
				const bool bDelegate = Prefix == ObservedDelegatePrefix || Prefix == ObservedMissingDelegatePrefix;
				const bool bDefaultOnly = Prefix == ObservedDefaultMemberPrefix;
				const FBPVariableDescription* Member = Blueprint.NewVariables.FindByPredicate([&](const FBPVariableDescription& Candidate)
					{ return Candidate.VarGuid == Guid || Candidate.VarName == Name; });
				const FProperty* Property = CDO ? FindFProperty<FProperty>(CDO->GetClass(), Name) : nullptr;
				const bool bBound = bPresent
					? Member && Member->VarGuid == Guid && Member->VarName == Name && Property && Property->GetOwnerClass() == Blueprint.GeneratedClass
						&& ((Member->VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate) == bDelegate)
					: !Member && !Property;
				if (!bGuidValid || !CDO || !bBound) {
					Error = FDiagnostic::Failure(TEXT("blueprint_observed_member_binding_missing"),
						TEXT("The member and generated field must match the prepared identity, kind and capture side."), TEXT("blueprint_state"));
					Error.TargetPath = AssetPath;
					Error.PropertyPath = Field.Value;
					return false;
				}
				if (!bPresent) continue;
				if (!bDefaultOnly && !AppendMemberReadRequest(Blueprint, AssetPath, *Member, Reads, Error)) return false;
				// Delegates own their public native signature through the member accessor.
				if (!bDelegate) SelectedDefaults.Add(Name);
				if (bDefaultOnly) ++RequiredDefaultCount;
			}
			const int32 Start = Reads.Num();
			if (!SelectedDefaults.IsEmpty() && !AppendPropertyRequests(CDO, ClassDefaultPropertySemanticId, ClassDefaultPropertyIdentityModelId,
				TEXT("blueprint_class_default_property"), {{TEXT("asset_path"), AssetPath}}, {}, Reads, Error, &SelectedDefaults)) return false;
			if (RequiredDefaultCount && Reads.Num() - Start != RequiredDefaultCount) {
				Error = FDiagnostic::Failure(TEXT("blueprint_observed_member_binding_missing"),
					TEXT("A selected generated default has no editable public property binding."), TEXT("blueprint_state"));
				Error.TargetPath = AssetPath;
				return false;
			}
			return true;
		}

		UeAgentAuthoringNativeDefaults::EProbeOutcome ProbeNativeDefault(
			const UeAgentAuthoringNativeDefaults::FRequest& Request,
			UeAgentAuthoringNativeDefaults::FScratchScope& Scratch,
			UeAgentAuthoringNativeDefaults::FProbeValue& OutValue,
			FDiagnostic& OutError)
		{
			OutValue = UeAgentAuthoringNativeDefaults::FProbeValue();
			OutError.Reset();
			if (Request.TargetIdentity.TargetKind != TEXT("blueprint_asset"))
			{
				return UeAgentAuthoringNativeDefaults::EProbeOutcome::Unavailable;
			}
			UPackage* Package = Scratch.CreateScratchPackage(OutError);
			if (!Package)
			{
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
				return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;
			}
			UObject* Seed = CreateFactorySeed(
				TEXT("actor_blueprint"), Package, NAME_None, RF_Transactional | RF_Transient, OutError);
			if (!Seed)
			{
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
				return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;
			}
			OutValue.Value = MakeShared<FJsonValueObject>(
				BuildAssetVerificationState(
					*CastChecked<UBlueprint>(Seed),
					Request.TargetIdentity.GetSelector(TEXT("asset_path"))));
			OutValue.Provenance = FactoryDefaultProviderId;
			return UeAgentAuthoringNativeDefaults::EProbeOutcome::Resolved;
		}
	}

	void AppendGraphNodeSemanticState(
		const UEdGraphNode& Node,
		FJsonObject& State)
	{
		AppendGraphNodeSemanticState(Node, State, FString());
	}

	void AppendGraphNodeSemanticState(
		const UEdGraphNode& Node,
		FJsonObject& State,
		const FString& CanonicalAssetPath)
	{
		const TSharedRef<FJsonObject> SemanticState = BuildNodeState(Node, CanonicalAssetPath);
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field
			: SemanticState->Values)
		{
			if (Field.Key == TEXT("id")
				|| Field.Key == TEXT("node_type")
				|| Field.Key == TEXT("pos"))
			{
				continue;
			}
			State.SetField(Field.Key, Field.Value);
		}
		// Input identity is observable even when its native property is not
		// editable. Keep this read-only query metadata outside workspace specs;
		// creation continues to use the candidate's native action and select.
		if (const UK2Node* K2Node = Cast<UK2Node>(&Node))
		{
			TMap<FString, FString> Fields;
			UeAgentBlueprintInputBindings::Capture(*K2Node, Fields);
			if (!Fields.IsEmpty())
			{
				auto Binding = MakeShared<FJsonObject>();
				for (const auto& Field : Fields) Binding->SetStringField(Field.Key, Field.Value);
				State.SetObjectField(TEXT("input_binding"), Binding);
			}
		}
	}

	bool ProjectObservedNodePinDefaults(FJsonObject& DesiredNode, UeAgentAuthoringState::FStateEntry& NativeNode,
		const UeAgentAuthoringState::FStateSnapshot& FinalCapture, const TArray<UeAgentAuthoringState::FReadRequest>* Requests,
		FDiagnostic& Error)
	{
		const TSharedPtr<FJsonObject>* DesiredPins = nullptr;
		if (NativeNode.SemanticId != NodeSemanticId || !DesiredNode.TryGetObjectField(TEXT("pin_defaults"), DesiredPins)) return true;
		const auto ObservedPins = MakeShared<FJsonObject>();
		const auto PinTypes = MakeShared<FJsonObject>();
		const FString Asset = NativeNode.TargetIdentity.GetSelector(TEXT("asset_path"));
		const FString Graph = NativeNode.TargetIdentity.GetSelector(TEXT("graph_guid"));
		const FString Node = NativeNode.TargetIdentity.GetSelector(TEXT("node_guid"));
		for (const auto& Field : (*DesiredPins)->Values)
		{
			const FString Path = TEXT("/pin_defaults/") + UE::UAI::HjsonWorkspace::EscapeJsonPointerSegment(Field.Key);
			auto BindingError = [&](const TCHAR* Reason) {
				Error = FDiagnostic::Failure(TEXT("blueprint_pin_intent_binding_invalid"), Reason, TEXT("final_readback"));
				Error.Source.JsonPath = Path; Error.TargetPath = Asset;
				return false;
			};
			const auto Spec = Field.Value && Field.Value->Type == EJson::Object ? Field.Value->AsObject() : nullptr;
			FString Name, Authored, Guid;
			int32 Occurrence = INDEX_NONE;
			if (!Spec || !Spec->TryGetStringField(TEXT("pin_name"), Name)
				|| !Spec->TryGetNumberField(TEXT("pin_occurrence"), Occurrence)
				|| !Spec->TryGetStringField(TEXT("default_value"), Authored))
				return BindingError(TEXT("A pin default requires its resolved name, occurrence and authored text."));
			Spec->TryGetStringField(TEXT("guid"), Guid);
			const UeAgentAuthoringState::FReadRequest* Bound = nullptr;
			if (Requests && !Asset.IsEmpty() && !Graph.IsEmpty() && !Node.IsEmpty()) for (const auto& Read : *Requests)
			{
				if (Read.SemanticId != PinDefaultSemanticId) continue;
				const auto& Id = Read.TargetIdentity;
				if (Id.GetSelector(TEXT("asset_path")) != Asset || Id.GetSelector(TEXT("graph_guid")) != Graph
					|| Id.GetSelector(TEXT("node_guid")) != Node || Id.GetSelector(TEXT("pin_name")) != Name
					|| Id.GetSelector(TEXT("pin_occurrence")) != LexToString(Occurrence)
					|| (!Guid.IsEmpty() && Id.GetSelector(TEXT("pin_guid")) != Guid)) continue;
				if (Bound) return BindingError(TEXT("More than one captured pin matches this node and pin selector."));
				Bound = &Read;
			}
			FGuid PinGuid;
			const auto* Owner = Bound ? Cast<UEdGraphNode>(Bound->RootObject) : nullptr;
			const UEdGraphPin* Pin = Owner && FGuid::Parse(Bound->TargetIdentity.GetSelector(TEXT("pin_guid")), PinGuid)
				? Owner->FindPinById(PinGuid) : nullptr;
			UeAgentAuthoringState::FStateEntry Observed;
			if (!Pin || !FinalCapture.FindEntry(Bound->SemanticId, Bound->TargetIdentity.CanonicalKey, Observed)
				|| !Observed.bAvailable || !Observed.Value)
				return BindingError(TEXT("The final capture has no bound native value for this graph node pin."));
			UeAgentAuthoringTypeDescriptors::FDescriptor DesiredType;
			TSharedPtr<FJsonValue> DesiredValue;
			if (!UeAgentBlueprintAuthoringOps::TryDescribePinDefault(Pin->PinType, Authored, DesiredType, DesiredValue, Error))
			{
				Error.Source.JsonPath = Path + TEXT("/default_value"); return false;
			}
			Spec->SetField(TEXT("default_value"), DesiredValue);
			const auto Actual = MakeShared<FJsonObject>();
			Actual->SetStringField(TEXT("pin_name"), Pin->PinName.ToString());
			Actual->SetNumberField(TEXT("pin_occurrence"), GetPinOccurrence(*Owner, *Pin));
			if (!Guid.IsEmpty()) Actual->SetStringField(TEXT("guid"), GuidText(Pin->PinId));
			Actual->SetField(TEXT("default_value"), Observed.Value);
			ObservedPins->SetObjectField(Field.Key, Actual);
			const auto DefaultType = MakeShared<FJsonObject>();
			DefaultType->SetObjectField(TEXT("type"), Observed.GetType().ToJsonObject());
			if (Observed.ValueDefinition) DefaultType->SetField(TEXT("value_definition"), Observed.ValueDefinition);
			const auto Fields = MakeShared<FJsonObject>(); Fields->SetObjectField(TEXT("default_value"), DefaultType);
			const auto Definition = MakeShared<FJsonObject>(); Definition->SetObjectField(TEXT("fields"), Fields);
			const auto Type = MakeShared<FJsonObject>(); Type->SetStringField(TEXT("kind"), TEXT("object"));
			const auto Row = MakeShared<FJsonObject>();
			Row->SetObjectField(TEXT("type"), Type); Row->SetObjectField(TEXT("value_definition"), Definition);
			PinTypes->SetObjectField(Field.Key, Row);
		}
		NativeNode.Value->AsObject()->SetObjectField(TEXT("pin_defaults"), ObservedPins);
		UeAgentAuthoringSemanticCompare::BindComparisonObjectRows(NativeNode, TEXT("pin_defaults"), PinTypes);
		return true;
	}

	UObject* CreateFactorySeed(
		const FString& AssetKind,
		UObject* Outer,
		const FName Name,
		const EObjectFlags Flags,
		FDiagnostic& OutError)
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
		FDiagnostic& OutError)
	{
		OutError.Reset();
		if (AssetKind != TEXT("blueprint") && AssetKind != TEXT("actor_blueprint"))
		{
			OutError = FDiagnostic::Failure(TEXT("blueprint_factory_kind_invalid"),
				FString::Printf(TEXT("Blueprint factory kind '%s' is not supported."), *AssetKind), TEXT("asset_factory"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("asset_kind");
			OutError.ActualValue = MakeShared<FJsonValueString>(AssetKind);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("blueprint or actor_blueprint"));
			return nullptr;
		}
		const bool bCanCreate = Outer && ParentClass && FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass);
		const bool bActorParent = !bCanCreate || AssetKind != TEXT("actor_blueprint") || ParentClass->IsChildOf(AActor::StaticClass());
		if (!Outer || !ParentClass || !bCanCreate || !bActorParent)
		{
			OutError = FDiagnostic::Failure(TEXT("blueprint_factory_input_invalid"),
				!Outer ? TEXT("The Blueprint factory has no outer object.")
					: !ParentClass ? TEXT("The Blueprint factory has no parent class.")
					: !bCanCreate ? TEXT("The native editor does not permit Blueprint creation from the supplied parent class.")
					: TEXT("Actor Blueprint creation requires a parent class derived from AActor."), TEXT("blueprint_state"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = !Outer ? TEXT("outer") : TEXT("parent_class");
			OutError.ActualValue = !Outer ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false))
				: ParentClass ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(ParentClass->GetPathName())) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
			OutError.ExpectedValue = MakeShared<FJsonValueString>(!Outer ? TEXT("an outer object")
				: !bActorParent ? TEXT("a Blueprint-creatable AActor subclass") : TEXT("a parent class permitting Blueprint creation"));
			return nullptr;
		}
		if (Name.IsNone()) Name = MakeUniqueObjectName(Outer, UBlueprint::StaticClass(), TEXT("BlueprintSeed"));
		UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
		Factory->ParentClass = ParentClass;
		UObject* Result = Factory->FactoryCreateNew(
			UBlueprint::StaticClass(), Outer, Name, Flags, nullptr, GWarn);
		if (!Result)
		{
			OutError = FDiagnostic::Failure(TEXT("blueprint_factory_create_failed"),
				FString::Printf(TEXT("The native Blueprint factory returned no object for kind '%s' with parent '%s'."), *AssetKind, *ParentClass->GetPathName()),
				TEXT("blueprint_state"), FString::Printf(TEXT("outer=%s name=%s"), *Outer->GetPathName(), *Name.ToString()));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("factory_result");
			OutError.ActualValue = MakeShared<FJsonValueNull>();
			OutError.ExpectedValue = MakeShared<FJsonValueString>(UBlueprint::StaticClass()->GetPathName());
		}
		return Result;
	}

	bool BuildAssetSnapshotReadRequest(
		UObject* Asset,
		const FString& CanonicalAssetPath,
		const FString& AssetKind,
		const bool bExpectedAbsent,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FDiagnostic& OutError)
	{
		OutError.Reset();
		if (CanonicalAssetPath.IsEmpty()
			|| (!Asset && !bExpectedAbsent)
			|| (!AssetKind.IsEmpty()
				&& AssetKind != TEXT("blueprint")
				&& AssetKind != TEXT("actor_blueprint")))
		{
			OutError = FDiagnostic::Failure(TEXT("blueprint_snapshot_identity_invalid"),
				CanonicalAssetPath.IsEmpty() ? TEXT("A Blueprint snapshot requires a non-empty canonical asset path.")
					: !Asset && !bExpectedAbsent ? TEXT("The Blueprint snapshot requires a present asset unless absence is explicitly expected.")
					: TEXT("The Blueprint snapshot asset kind must be empty, blueprint, or actor_blueprint."), TEXT("blueprint_state"));
			OutError.SourceKind = TEXT("native_result");
			OutError.TargetPath = CanonicalAssetPath;
			OutError.PropertyPath = CanonicalAssetPath.IsEmpty() ? TEXT("asset_path") : !Asset && !bExpectedAbsent ? TEXT("asset_present") : TEXT("asset_kind");
			OutError.ActualValue = CanonicalAssetPath.IsEmpty() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(CanonicalAssetPath))
				: !Asset && !bExpectedAbsent ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(AssetKind));
			OutError.ExpectedValue = CanonicalAssetPath.IsEmpty() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("a non-empty canonical asset path")))
				: !Asset && !bExpectedAbsent ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true)) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("empty, blueprint, or actor_blueprint")));
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

	bool AppendBlueprintRootPropertyReadRequests(
		UBlueprint& Blueprint,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FDiagnostic& OutError)
	{
		return AppendPropertyRequests(
			&Blueprint,
			TEXT("core.property"),
			FString(),
			FString(),
			{},
			{ TEXT("ParentClass"), TEXT("GeneratedClass"), TEXT("SkeletonGeneratedClass"),
				TEXT("SimpleConstructionScript"), TEXT("UbergraphPages"), TEXT("FunctionGraphs"),
				TEXT("MacroGraphs"), TEXT("DelegateSignatureGraphs"), TEXT("NewVariables") },
			OutRequests,
			OutError);
	}

	void GatherExportGraphs(const UBlueprint& Blueprint, TArray<UEdGraph*>& OutGraphs)
	{
		OutGraphs.Reset();
		TSet<const UEdGraph*> Seen;
		auto AppendTrees = [&](const auto& Roots)
		{
			for (UEdGraph* Root : Roots)
			{
				if (!Root) continue;
				TArray<UEdGraph*> Tree;
				Tree.Add(Root);
				Root->GetAllChildrenGraphs(Tree);
				for (UEdGraph* Graph : Tree)
				{
					if (Graph && !Seen.Contains(Graph))
					{
						Seen.Add(Graph);
						OutGraphs.Add(Graph);
					}
				}
			}
		};
		// Delegate signatures retain their member owner. Child graph bodies use
		// the same capture as their existing event/function/macro roots.
		AppendTrees(Blueprint.UbergraphPages);
		AppendTrees(Blueprint.FunctionGraphs);
		AppendTrees(Blueprint.MacroGraphs);
	}

	FString ExportGraphKind(const UBlueprint& Blueprint, const UEdGraph* Graph)
	{
		return GetGraphKind(Blueprint, Graph);
	}

	void AppendGraphDisplayState(const UBlueprint& Blueprint, const UEdGraph& Graph, FJsonObject& State,
		const FString& CanonicalAssetPath)
	{
		TArray<UEdGraph*> Graphs;
		Blueprint.GetAllGraphs(Graphs);
		TSet<const UEdGraph*> NameMatches;
		for (const UEdGraph* Candidate : Graphs)
			if (Candidate && Candidate->GetName().Equals(Graph.GetName(), ESearchCase::IgnoreCase))
				NameMatches.Add(Candidate);
		if (NameMatches.Num() > 1)
		{
			const FString OwnerPath = CanonicalAssetPath.IsEmpty()
				? Blueprint.GetPathName() : UeAgentAuthoringAssetIdentity::ToObjectPath(CanonicalAssetPath);
			State.SetStringField(TEXT("query_name"), OwnerPath + TEXT(":") + Graph.GetPathName(&Blueprint));
		}
		if (const UEdGraphSchema* Schema = Graph.GetSchema())
		{
			FGraphDisplayInfo DisplayInfo;
			Schema->GetGraphDisplayInformation(Graph, DisplayInfo);
			const FString DisplayName = DisplayInfo.DisplayName.ToString();
			if (!DisplayName.IsEmpty() && DisplayName != Graph.GetName())
				State.SetStringField(TEXT("display_name"), DisplayName);
		}
		auto IsParent = [&](const UEdGraph* Parent)
		{
			return Parent && Parent->SubGraphs.ContainsByPredicate(
				[&](const UEdGraph* Child) { return Child == &Graph; });
		};
		const UEdGraphNode* OuterNode = Cast<UEdGraphNode>(Graph.GetOuter());
		const UEdGraph* Parent = OuterNode ? OuterNode->GetGraph() : nullptr;
		if (!IsParent(Parent))
		{
			UEdGraph* const* Found = Graphs.FindByPredicate(IsParent);
			Parent = Found ? *Found : nullptr;
		}
		if (Parent) State.SetStringField(TEXT("parent_graph"), Parent->GetName());
	}

static bool AppendBlueprintGraphReadRequests(UBlueprint* Blueprint, const FString& AssetPath,
	const TArray<UEdGraph*>& Graphs, bool bForVerification,
	TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, FDiagnostic& OutError);

static bool BuildBlueprintContentReadRequests(
		UBlueprint& BlueprintObject,
		const bool bResetRequests,
		const bool bIncludeAssetEnvelope,
		const bool bIncludeActorComponents,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FDiagnostic& OutError,
		const FString& CanonicalAssetPath,
		const bool bForVerification, const bool bReparentHierarchy = false)
	{
		if (bResetRequests) OutRequests.Reset();
		OutError.Reset();
		UBlueprint* Blueprint = &BlueprintObject;
		const FString AssetPath = CanonicalAssetPath.IsEmpty()
			? Blueprint->GetOutermost()->GetName()
			: CanonicalAssetPath;
		const TMap<FString, FString> AssetSelectors = { { TEXT("asset_path"), AssetPath } };
		if (bIncludeAssetEnvelope || bReparentHierarchy) {
			if (!AddReadRequest(ExportTargetSemanticId, AssetIdentityModelId, TEXT("blueprint_asset"),
				AssetSelectors, Blueprint, {}, OutRequests, OutError)) return false;
			OutRequests.Last().MarkExportTarget();
			if (!AddReadRequest(ParentClassSemanticId, AssetIdentityModelId, TEXT("blueprint_asset"),
				AssetSelectors, Blueprint, {}, OutRequests, OutError)) return false;
		}

		if (!bReparentHierarchy) {
			for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces) {
				const UClass* InterfaceClass = Description.Interface.Get();
				if (InterfaceClass && !AppendInterfaceReadRequest(*Blueprint, AssetPath, *InterfaceClass, OutRequests, OutError)) return false;
			}
		}

		if (bIncludeAssetEnvelope && !AppendBlueprintRootPropertyReadRequests(
			*Blueprint,
			OutRequests,
			OutError))
		{
			return false;
		}

		if (!bReparentHierarchy) {
			for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
				if (!AppendMemberReadRequest(*Blueprint, AssetPath, Variable, OutRequests, OutError)) return false;
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
		TArray<UeAgentBlueprintAuthoringOps::FInheritedComponent> InheritedComponents;
		UeAgentBlueprintAuthoringOps::GatherInheritedComponents(*Blueprint, InheritedComponents);
		TMap<UActorComponent*, FString> Inherited;
		for (const auto& Entry : InheritedComponents) Inherited.Add(Entry.Component, Entry.Name);
		if (USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
		{
			TMap<const USCS_Node*, FString> Parents;
			for (USCS_Node* Root : SCS->GetRootNodes()) BuildSCSParentMap(*Blueprint, Root, FString(), Inherited, Parents);
			for (USCS_Node* Node : SCS->GetAllNodes())
			{
				if (!Node || !Node->ComponentTemplate || !Node->ComponentClass) continue;
				if (!AppendLocalComponentReadRequests(AssetPath, *Node, Parents.FindRef(Node), OutRequests, OutError)) return false;
			}
		}

		{
			for (const auto& Entry : InheritedComponents)
			{
				UActorComponent* Component = Entry.Component;
				TMap<FString, FString> Selectors = {
					{ TEXT("asset_path"), AssetPath },
					{ TEXT("component_kind"), Component->GetClass()->GetName() },
					{ TEXT("component_name"), Entry.Name },
				};
				if (Entry.SourceNode && Entry.SourceNode->VariableGuid.IsValid())
					Selectors.Add(TEXT("component_guid"), GuidText(Entry.SourceNode->VariableGuid));
				const TMap<FString, FString> Context = {
					{ TEXT("component_name"), Entry.Name },
					{ TEXT("parent_name"), Entry.ParentName },
					{ TEXT("source"), TEXT("inherited") },
					{ TEXT("native_class_override"), !Entry.SourceNode && Component->CreationMethod == EComponentCreationMethod::Native
						&& GetAllowNativeComponentClassOverrides() ? TEXT("true") : TEXT("false") },
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

	TArray<UEdGraph*> ExportGraphs;
	GatherExportGraphs(*Blueprint, ExportGraphs);
	return AppendBlueprintGraphReadRequests(Blueprint, AssetPath, ExportGraphs, bForVerification, OutRequests, OutError);
}

static bool AppendBlueprintGraphReadRequests(UBlueprint* Blueprint, const FString& AssetPath,
	const TArray<UEdGraph*>& Graphs, bool bForVerification,
	TArray<UeAgentAuthoringState::FReadRequest>& OutRequests, FDiagnostic& OutError)
{
			for (UEdGraph* Graph : Graphs)
			{
				if (!Graph) continue;
				const FString Kind = ExportGraphKind(*Blueprint, Graph);
				const TMap<FString, FString> GraphSelectors = {
					{ TEXT("asset_path"), AssetPath },
					{ TEXT("graph_guid"), GuidText(Graph->GraphGuid) },
					{ TEXT("graph_kind"), Kind },
					{ TEXT("graph_name"), Graph->GetName() },
				};
				const bool bGeneratedProgram = bForVerification && IsNativeGeneratedGraph(*Graph);
				TMap<FString, FString> GraphContext = { { TEXT("graph_kind"), Kind } };
				if (bGeneratedProgram) GraphContext.Add(TEXT("native_generated_verification"), TEXT("true"));
				if (!AddReadRequest(
					GraphSemanticId, GraphIdentityModelId, TEXT("blueprint_graph"),
					GraphSelectors, Graph, GraphContext,
					OutRequests, OutError))
				{
					return false;
				}

				// The graph entry captures every generated node, pin and link by program order.
				// Adding the ordinary child requests would also compare their transient GUIDs.
				if (bGeneratedProgram) continue;
				TMap<const UEdGraphNode*, FString> NodeRefs;
				TMap<FString, int32> Occurrences;
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (!Node || Node->IsAutomaticallyPlacedGhostNode()) continue;
					NodeRefs.Add(Node, NodeReference(Node));
				}
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (!Node || Node->IsAutomaticallyPlacedGhostNode())
					{
						continue;
					}
					const FString NodeKind = GetNodeKind(Node);
					const FString NodeName = NodeSemanticName(*Node);
					const FString OccurrenceKey = NodeKind + TEXT(":") + NodeName;
					const FString Occurrence = LexToString(Occurrences.FindOrAdd(OccurrenceKey)++);
					TMap<FString, FString> NodeSelectors = GraphSelectors;
					NodeSelectors.Add(TEXT("node_guid"), GuidText(Node->NodeGuid));
					NodeSelectors.Add(TEXT("node_kind"), NodeKind);
					NodeSelectors.Add(TEXT("node_name"), NodeName);
					NodeSelectors.Add(TEXT("node_occurrence"), Occurrence);
					if (!AddReadRequest(
						NodeSemanticId, NodeIdentityModelId, TEXT("blueprint_node"),
						NodeSelectors, Node, {}, OutRequests, OutError))
					{
						return false;
					}
					// Signature-owned node properties stay with the graph signature.
					// Per-node input defaults (including return values) still use the
					// same native pin editability and default-value path as other nodes.
					if (GetBuiltinNodeId(Node).IsEmpty() && !AppendPropertyRequests(
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

					TMap<const UEdGraphPin*, FString> AliasByPin;
					if (!TryBuildNodePinAliases(*Node, AliasByPin, OutError))
					{
						return false;
					}
					for (UEdGraphPin* Pin : Node->Pins)
					{
						if (!Pin || !CanAuthorPinDefault(*Pin)) continue;
						TMap<FString, FString> PinSelectors = NodeSelectors;
						const FString PinName = Pin->PinName.ToString();
						const FString PinGuid = GuidText(Pin->PinId);
						const int32 PinOccurrence = GetPinOccurrence(*Node, *Pin);
						const FString PinAlias = AliasByPin.FindRef(Pin);
						if (PinAlias.IsEmpty())
						{
							OutError = FDiagnostic::Failure(TEXT("graph_authoring_pin_alias_missing"),
								FString::Printf(TEXT("Authorable pin '%s' occurrence %d on node '%s' has no generated alias."),
									*PinName, PinOccurrence, *Node->GetNodeTitle(ENodeTitleType::ListView).ToString()),
								TEXT("blueprint_state"), FString(), PinGuid);
							OutError.SourceKind = TEXT("native_result");
							OutError.TargetPath = AssetPath;
							OutError.CandidateName = PinName;
							OutError.PropertyPath = TEXT("pin_alias");
							OutError.ActualValue = MakeShared<FJsonValueString>(PinAlias);
							OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("a non-empty generated pin alias"));
							return false;
						}
						PinSelectors.Add(TEXT("pin_name"), PinName);
						PinSelectors.Add(TEXT("pin_occurrence"), LexToString(PinOccurrence));
						if (!PinGuid.IsEmpty()) PinSelectors.Add(TEXT("pin_guid"), PinGuid);
						PinSelectors.Add(
							TEXT("property_path"),
							TEXT("pin_default.") + PinAlias);
						if (!AddReadRequest(
							PinDefaultSemanticId,
							PinDefaultIdentityModelId,
							TEXT("blueprint_pin_default"),
							PinSelectors,
							Node,
							{},
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
							const int32 FromPinOccurrence = GetPinOccurrence(*Node, *Pin);
							const UEdGraphNode* ToOwner = Linked->GetOwningNode();
							const int32 ToPinOccurrence = ToOwner
								? GetPinOccurrence(*ToOwner, *Linked)
								: 0;
							const FString EdgeKey = FromNode + TEXT("|") + Pin->PinName.ToString()
								+ TEXT("|") + LexToString(FromPinOccurrence)
								+ TEXT("|") + ToNode + TEXT("|") + Linked->PinName.ToString()
								+ TEXT("|") + LexToString(ToPinOccurrence);
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
									{ TEXT("from_pin_occurrence"), LexToString(FromPinOccurrence) },
									{ TEXT("to_node"), ToNode },
									{ TEXT("to_pin"), Linked->PinName.ToString() },
									{ TEXT("to_pin_occurrence"), LexToString(ToPinOccurrence) } },
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
	}

bool CaptureObservedGraphBodies(UBlueprint& Blueprint, const FString& AssetPath, const FString& GraphGuids,
	const FString& WorkspaceTargetKind, TArray<UeAgentAuthoringState::FReadRequest> Requests,
	UeAgentAuthoringState::FStateValue& OutValue, FDiagnostic& Error, const TMap<FString, FString>* GraphContext)
{
	TArray<FString> Guids;
	GraphGuids.ParseIntoArray(Guids, TEXT(","), false);
	TArray<UEdGraph*> ExportGraphs, Selected;
	GatherExportGraphs(Blueprint, ExportGraphs);
	TSet<FString> Matched;
	TSet<FString> Absent;
	TSet<FName> AbsentRootNames;
	if (GraphContext)
		for (const FString& Guid : Guids)
			if (const FString* Name = GraphContext->Find(ObservedAbsentGraphPrefix + Guid)) {
				Absent.Add(Guid);
				if (!Name->IsEmpty()) AbsentRootNames.Add(FName(**Name));
			}
	bool bBindingMismatch = false;
	for (UEdGraph* Graph : ExportGraphs) {
		if (!Graph || !Graph->GraphGuid.IsValid()) continue;
		const FString Id = GuidText(Graph->GraphGuid);
		const FString* ExpectedName = GraphContext ? GraphContext->Find(ObservedGraphNamePrefix + Id) : nullptr;
		if (Graph->GetOuter() == &Blueprint && AbsentRootNames.Contains(Graph->GetFName())
			&& (!ExpectedName || Graph->GetFName() != FName(**ExpectedName))) bBindingMismatch = true;
		if (!Guids.Contains(Id)) continue;
		if ((GraphContext && GraphContext->Contains(ObservedRootGraphPrefix + Id) && Graph->GetOuter() != &Blueprint)
			|| Absent.Contains(Id) || (ExpectedName && Graph->GetFName() != FName(**ExpectedName)))
			bBindingMismatch = true;
		Selected.Add(Graph);
		Matched.Add(Id);
	}
	if (Guids.IsEmpty() || bBindingMismatch || Selected.Num() + Absent.Num() != Guids.Num()
		|| Matched.Num() != Selected.Num()) {
		Error = FDiagnostic::Failure(TEXT("blueprint_observed_graph_binding_missing"),
			TEXT("The prepared graph identities do not match the current asset's public graphs."), TEXT("blueprint_state"));
		Error.TargetPath = AssetPath;
		return false;
	}
	if (!AppendBlueprintGraphReadRequests(&Blueprint, AssetPath, Selected, false, Requests, Error)) return false;
	int32 EntryCount = 0;
	if (!CaptureBlueprintProjection(WorkspaceTargetKind, Requests, OutValue, Error, &EntryCount)) return false;
	UE_LOG(LogTemp, Display, TEXT("UAI read timing phase=blueprint_observed_graphs graphs=%d requests=%d public_entries=%d"),
		Selected.Num(), Requests.Num(), EntryCount);
	return true;
}

	static bool AppendReparentHierarchyReadRequests(UBlueprint& Blueprint, const FString& AssetPath,
	TArray<UeAgentAuthoringState::FReadRequest>& Requests, FDiagnostic& Error)
{
	// ReparentBlueprint validates the SCS, refreshes all nodes and recompiles
	// the generated class and CDO. Read those surfaces on this target only.
	const bool bActor = Blueprint.ParentClass && Blueprint.ParentClass->IsChildOf(AActor::StaticClass());
	return BuildBlueprintContentReadRequests(Blueprint, false, false, bActor, Requests, Error, AssetPath, false, true);
}

bool BuildAssetExportSnapshotReadRequests(
		UObject& Asset,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FDiagnostic& OutError)
	{
		UBlueprint* Blueprint = Cast<UBlueprint>(&Asset);
		if (!Blueprint || GetAssetKind(Blueprint).IsEmpty())
		{
			OutRequests.Reset();
			OutError = FDiagnostic::Failure(TEXT("blueprint_export_asset_type_invalid"),
				!Blueprint ? TEXT("Blueprint export requires a UBlueprint asset.") : TEXT("The loaded Blueprint has no supported Blueprint export kind."),
				TEXT("blueprint_state"));
			OutError.SourceKind = TEXT("native_result");
			if (Blueprint) OutError.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Blueprint);
			OutError.PropertyPath = TEXT("asset_class");
			OutError.ActualValue = MakeShared<FJsonValueString>(Asset.GetClass()->GetPathName());
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("a Blueprint or Actor Blueprint asset"));
			return false;
		}
		const bool bIncludeActorComponents = Blueprint->ParentClass
			&& Blueprint->ParentClass->IsChildOf(AActor::StaticClass());
		return BuildBlueprintContentReadRequests(
			*Blueprint, true, true, bIncludeActorComponents, OutRequests, OutError, FString(), false);
	}

	bool AppendReusableBlueprintExportSnapshotReadRequests(
		UBlueprint& Blueprint,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FDiagnostic& OutError,
		const FString& CanonicalAssetPath)
	{
		return BuildBlueprintContentReadRequests(
			Blueprint, false, false, false, OutRequests, OutError, CanonicalAssetPath, false);
	}

	bool AppendReusableBlueprintVerificationReadRequests(
		UBlueprint& Blueprint,
		TArray<UeAgentAuthoringState::FReadRequest>& OutRequests,
		FDiagnostic& OutError,
		const FString& CanonicalAssetPath)
	{
		return BuildBlueprintContentReadRequests(
			Blueprint, false, false, false, OutRequests, OutError, CanonicalAssetPath, true);
	}

	bool RegisterStateAccessor(UE::UAI::Contract::FDiagnostic& OutError)
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

	bool RegisterNativeDefaultProvider(UE::UAI::Contract::FDiagnostic& OutError)
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
