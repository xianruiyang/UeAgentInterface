// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Blueprint/Authoring/UeAgentBlueprintGraphSignature.h"

#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringOps.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Tunnel.h"
#include "Kismet2/BlueprintEditorUtils.h"

namespace UeAgentBlueprintAuthoring
{
	namespace
	{
		struct FPlannedPin
		{
			FName Name;
			FEdGraphPinType Type;
			FString DefaultValue;
			bool bHasDefaultValue = false;
			TSharedPtr<const FJsonObject> Source;
			FString SourcePointer;
		};

		struct FFunctionTerminators
		{
			UK2Node_FunctionEntry* Entry = nullptr;
			TArray<UK2Node_FunctionResult*> Results;
		};

		struct FMacroTerminators
		{
			UK2Node_Tunnel* Entry = nullptr;
			UK2Node_Tunnel* Result = nullptr;
		};

		enum class EFunctionSignatureOwner : uint8
		{
			FunctionGraph,
			DelegateMember,
		};


		FDiagnostic NativeSignatureFailure(const UEdGraph* Graph, const FString& Code, const FString& Reason,
			const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Candidate)
		{
			FDiagnostic Error = FDiagnostic::Failure(Code, Reason, TEXT("blueprint_signature"));
			Error.SourceKind = TEXT("native_result");
			if (Graph)
				if (const UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Graph))
					Error.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Blueprint);
			Error.CandidateName = !Candidate.IsEmpty() ? Candidate : Graph ? Graph->GetName() : FString();
			Error.PropertyPath = Field;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		bool ResolveTerminators(
			const UEdGraph& Graph,
			const EFunctionSignatureOwner Owner,
			FFunctionTerminators& OutTerminators,
			FDiagnostic& OutError)
		{
			OutTerminators = {};
			const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph.GetSchema());
			const UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(&Graph);
			const bool bOwnedFunction = Blueprint
				&& Blueprint->FunctionGraphs.ContainsByPredicate(
					[&Graph](const UEdGraph* Candidate) { return Candidate == &Graph; });
			const bool bOwnedDelegate = Blueprint
				&& Blueprint->DelegateSignatureGraphs.ContainsByPredicate(
					[&Graph](const UEdGraph* Candidate) { return Candidate == &Graph; });
			int32 ObservedGraphType = INDEX_NONE;
			bool bConstructionScript = false;
			const auto IsConstructionScript = [&]()
			{
				bConstructionScript = Schema->IsConstructionScript(&Graph);
				return bConstructionScript;
			};
			if (!Schema
				|| (Owner == EFunctionSignatureOwner::FunctionGraph
					&& (ObservedGraphType = static_cast<int32>(Schema->GetGraphType(&Graph))) != static_cast<int32>(EGraphType::GT_Function))
				|| !Blueprint
				|| (Owner == EFunctionSignatureOwner::FunctionGraph
					? !bOwnedFunction : !bOwnedDelegate)
				|| IsConstructionScript()
				|| Graph.GetFName() == UEdGraphSchema_K2::FN_UserConstructionScript)
			{
				const bool bOwned = Owner == EFunctionSignatureOwner::FunctionGraph ? bOwnedFunction : bOwnedDelegate;
				OutError = NativeSignatureFailure(&Graph, TEXT("blueprint_function_signature_graph_unsupported"),
					!Schema ? TEXT("Function or delegate signatures require the K2 graph schema.")
						: Owner == EFunctionSignatureOwner::FunctionGraph && ObservedGraphType != static_cast<int32>(EGraphType::GT_Function)
							? FString::Printf(TEXT("Function signatures require a function graph; the native graph type is %d."), ObservedGraphType)
						: !Blueprint ? TEXT("The signature graph has no owning Blueprint.")
						: !bOwned ? TEXT("The Blueprint does not own the graph in the requested function/delegate collection.")
						: bConstructionScript ? TEXT("Construction-script graphs do not support this signature operation.")
						: TEXT("The user construction script cannot be edited as a function signature."),
					TEXT("signature_graph_supported"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Graph.GetName());
				auto Facts = MakeShared<FJsonObject>();
				Facts->SetStringField(TEXT("schema"), Graph.GetSchema() ? Graph.GetSchema()->GetClass()->GetPathName() : FString());
				Facts->SetBoolField(TEXT("blueprint_owner_present"), Blueprint != nullptr);
				Facts->SetBoolField(TEXT("owned_in_requested_collection"), bOwned);
				if (ObservedGraphType != INDEX_NONE) Facts->SetNumberField(TEXT("graph_type"), ObservedGraphType);
				OutError.ActualEvidence = MakeShared<FJsonValueObject>(Facts);
				return false;
			}

			for (UEdGraphNode* Node : Graph.Nodes)
			{
				if (UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
				{
					if (OutTerminators.Entry)
					{
						OutError = NativeSignatureFailure(&Graph, TEXT("blueprint_function_signature_entry_conflict"),
							TEXT("A second function entry node was found; the signature requires exactly one."),
							TEXT("observed_entry_count"), MakeShared<FJsonValueNumber>(2), MakeShared<FJsonValueNumber>(1), Graph.GetName());
						return false;
					}
					OutTerminators.Entry = Entry;
				}
				if (UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Node))
				{
					OutTerminators.Results.Add(Result);
				}
			}
			if (!OutTerminators.Entry)
			{
				OutError = NativeSignatureFailure(&Graph, TEXT("blueprint_function_signature_entry_missing"),
					TEXT("The function/delegate signature graph has no function entry node."),
					TEXT("entry_count"), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(1), Graph.GetName());
				return false;
			}
			if (!OutTerminators.Entry->IsEditable())
			{
				OutError = NativeSignatureFailure(&Graph, TEXT("blueprint_function_signature_read_only"),
					TEXT("The function entry node is not editable."),
					TEXT("entry_editable"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Graph.GetName());
				return false;
			}
			return true;
		}

		FDiagnostic DescribePinTypeDifference(const UK2Node_EditablePinBase& ReferenceNode,
			const UK2Node_EditablePinBase& ActualNode, const FUserPinInfo& ReferencePin, const FUserPinInfo& ActualPin,
			const int32 PinIndex, const int32 ResultIndex)
		{
			const FEdGraphPinType& Left = ReferencePin.PinType;
			const FEdGraphPinType& Right = ActualPin.PinType;
			const UEdGraph* Graph = ActualNode.GetGraph();
			const UBlueprint* Blueprint = Graph ? FBlueprintEditorUtils::FindBlueprintForGraph(Graph) : nullptr;
			const FString AssetPath = Blueprint ? UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Blueprint) : FString();
			const FString Base = FString::Printf(TEXT("/outputs/%d/type/"), PinIndex);
			const auto Fail = [&](const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
			{
				FDiagnostic Error = NativeSignatureFailure(Graph, TEXT("blueprint_function_signature_result_conflict"),
					FString::Printf(TEXT("Native result node %d differs from the first result at signature pin %d, type field '%s'."), ResultIndex, PinIndex, *Field),
					Base + Field, MoveTemp(Actual), MoveTemp(Expected), ActualPin.PinName.ToString());
				Error.InternalContext = FString::Printf(TEXT("expected_node=%s actual_node=%s"), *ReferenceNode.GetName(), *ActualNode.GetName());
				return Error;
			};
			const auto ObjectValue = [&](UObject* Object) -> TSharedPtr<FJsonValue>
			{
				FEdGraphPinType ObjectType;
				ObjectType.PinCategory = UEdGraphSchema_K2::PC_Object;
				ObjectType.PinSubCategoryObject = Object;
				const auto Json = UeAgentBlueprintAuthoringOps::BuildPinTypeJsonObject(ObjectType, Blueprint, AssetPath);
				const auto Value = Json->TryGetField(TEXT("pin_subcategory_object"));
				return Value.IsValid() ? Value : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
			};
			const auto ObjectDifference = [&](const FString& Field, UObject* Actual, UObject* Expected)
			{
				FDiagnostic Error = Fail(Field, ObjectValue(Actual), ObjectValue(Expected));
				auto Facts = MakeShared<FJsonObject>();
				Facts->SetBoolField(TEXT("native_object_reference_equal"), false);
				Error.ActualEvidence = MakeShared<FJsonValueObject>(Facts);
				return Error;
			};
			if (Left.PinCategory != Right.PinCategory) return Fail(TEXT("pin_category"), MakeShared<FJsonValueString>(Right.PinCategory.ToString()), MakeShared<FJsonValueString>(Left.PinCategory.ToString()));
			if (Left.PinSubCategory != Right.PinSubCategory) return Fail(TEXT("pin_subcategory"), MakeShared<FJsonValueString>(Right.PinSubCategory.ToString()), MakeShared<FJsonValueString>(Left.PinSubCategory.ToString()));
			if (Left.PinSubCategoryObject != Right.PinSubCategoryObject)
				return ObjectDifference(TEXT("pin_subcategory_object"), Right.PinSubCategoryObject.Get(), Left.PinSubCategoryObject.Get());
			if (Left.PinValueType != Right.PinValueType)
			{
				const auto& L = Left.PinValueType;
				const auto& R = Right.PinValueType;
				if (L.TerminalCategory != R.TerminalCategory)
					return Fail(TEXT("value_type/pin_category"), MakeShared<FJsonValueString>(R.TerminalCategory.ToString()), MakeShared<FJsonValueString>(L.TerminalCategory.ToString()));
				if (L.TerminalSubCategory != R.TerminalSubCategory)
					return Fail(TEXT("value_type/pin_subcategory"), MakeShared<FJsonValueString>(R.TerminalSubCategory.ToString()), MakeShared<FJsonValueString>(L.TerminalSubCategory.ToString()));
				if (L.TerminalSubCategoryObject != R.TerminalSubCategoryObject)
					return ObjectDifference(TEXT("value_type/pin_subcategory_object"), R.TerminalSubCategoryObject.Get(), L.TerminalSubCategoryObject.Get());
				if (L.bTerminalIsConst != R.bTerminalIsConst)
					return Fail(TEXT("value_type/native_is_const"), MakeShared<FJsonValueBoolean>(static_cast<bool>(R.bTerminalIsConst)), MakeShared<FJsonValueBoolean>(static_cast<bool>(L.bTerminalIsConst)));
				return Fail(TEXT("value_type/native_is_weak_pointer"), MakeShared<FJsonValueBoolean>(static_cast<bool>(R.bTerminalIsWeakPointer)), MakeShared<FJsonValueBoolean>(static_cast<bool>(L.bTerminalIsWeakPointer)));
			}
			if (Left.ContainerType != Right.ContainerType)
				return Fail(TEXT("container_type"), MakeShared<FJsonValueNumber>(static_cast<int32>(Right.ContainerType)), MakeShared<FJsonValueNumber>(static_cast<int32>(Left.ContainerType)));
			if (Left.bIsReference != Right.bIsReference) return Fail(TEXT("native_is_reference"), MakeShared<FJsonValueBoolean>(static_cast<bool>(Right.bIsReference)), MakeShared<FJsonValueBoolean>(static_cast<bool>(Left.bIsReference)));
			if (Left.bIsWeakPointer != Right.bIsWeakPointer) return Fail(TEXT("native_is_weak_pointer"), MakeShared<FJsonValueBoolean>(static_cast<bool>(Right.bIsWeakPointer)), MakeShared<FJsonValueBoolean>(static_cast<bool>(Left.bIsWeakPointer)));
			const auto& LMember = Left.PinSubCategoryMemberReference;
			const auto& RMember = Right.PinSubCategoryMemberReference;
			if (!(LMember == RMember))
			{
				if (LMember.MemberParent != RMember.MemberParent)
				{
					FDiagnostic Error = Fail(TEXT("native_member_reference/parent_identity_equal"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					Error.InternalContext += FString::Printf(TEXT(" expected_parent=%s actual_parent=%s"),
						LMember.MemberParent ? *LMember.MemberParent->GetPathName() : TEXT("<missing>"),
						RMember.MemberParent ? *RMember.MemberParent->GetPathName() : TEXT("<missing>"));
					return Error;
				}
				if (LMember.MemberName != RMember.MemberName)
					return Fail(TEXT("native_member_reference/member_name"), MakeShared<FJsonValueString>(RMember.MemberName.ToString()), MakeShared<FJsonValueString>(LMember.MemberName.ToString()));
				FDiagnostic Error = Fail(TEXT("native_member_reference/member_identity_equal"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				Error.InternalContext += FString::Printf(TEXT(" expected_member_guid=%s actual_member_guid=%s"),
					*LMember.MemberGuid.ToString(), *RMember.MemberGuid.ToString());
				return Error;
			}
			// With UE 5.6's preceding equality fields equal, the native rejection is constness.
			return Fail(TEXT("native_is_const"), MakeShared<FJsonValueBoolean>(static_cast<bool>(Right.bIsConst)), MakeShared<FJsonValueBoolean>(static_cast<bool>(Left.bIsConst)));
		}

		bool AreUserPinsEquivalent(
			const UK2Node_EditablePinBase& Left,
			const UK2Node_EditablePinBase& Right, const int32 ResultIndex, FDiagnostic& OutError)
		{
			const auto Fail = [&](const FString& Field, const FString& Reason, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
			{
				OutError = NativeSignatureFailure(Right.GetGraph(), TEXT("blueprint_function_signature_result_conflict"),
					Reason, Field, MoveTemp(Actual), MoveTemp(Expected), FString());
				OutError.InternalContext = FString::Printf(TEXT("expected_node=%s actual_node=%s"), *Left.GetName(), *Right.GetName());
				return false;
			};
			if (Left.UserDefinedPins.Num() != Right.UserDefinedPins.Num())
			{
				return Fail(TEXT("/outputs"),
					FString::Printf(TEXT("Native result node %d has a different signature pin count from the first result."), ResultIndex),
					MakeShared<FJsonValueNumber>(Right.UserDefinedPins.Num()), MakeShared<FJsonValueNumber>(Left.UserDefinedPins.Num()));
			}
			for (int32 Index = 0; Index < Left.UserDefinedPins.Num(); ++Index)
			{
				const TSharedPtr<FUserPinInfo>& LeftPin = Left.UserDefinedPins[Index];
				const TSharedPtr<FUserPinInfo>& RightPin = Right.UserDefinedPins[Index];
				const FString Base = FString::Printf(TEXT("/outputs/%d/"), Index);
				if (!LeftPin.IsValid() || !RightPin.IsValid())
				{
					return Fail(Base + (!LeftPin.IsValid() ? TEXT("reference_pin_present") : TEXT("pin_present")),
						FString::Printf(TEXT("Signature pin %d is missing its native record on %s."), Index,
							!LeftPin.IsValid() ? TEXT("the reference result node") : *FString::Printf(TEXT("result node %d"), ResultIndex)),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				}
				if (LeftPin->PinName != RightPin->PinName)
				{
					return Fail(Base + TEXT("name"),
						FString::Printf(TEXT("Native result node %d has a different name for signature pin %d."), ResultIndex, Index),
						MakeShared<FJsonValueString>(RightPin->PinName.ToString()), MakeShared<FJsonValueString>(LeftPin->PinName.ToString()));
				}
				if (LeftPin->PinType != RightPin->PinType)
				{
					OutError = DescribePinTypeDifference(Left, Right, *LeftPin, *RightPin, Index, ResultIndex);
					return false;
				}
			}
			return true;
		}

		bool ResolveMacroTerminators(
			const UEdGraph& Graph,
			FMacroTerminators& OutTerminators,
			FDiagnostic& OutError)
		{
			OutTerminators = {};
			const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph.GetSchema());
			const UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(&Graph);
			int32 ObservedGraphType = INDEX_NONE;
			bool bOwnedMacro = false;
			const auto IsOwnedMacro = [&]()
			{
				bOwnedMacro = Blueprint->MacroGraphs.ContainsByPredicate(
					[&Graph](const UEdGraph* Candidate) { return Candidate == &Graph; });
				return bOwnedMacro;
			};
			if (!Schema
				|| (ObservedGraphType = static_cast<int32>(Schema->GetGraphType(&Graph))) != static_cast<int32>(EGraphType::GT_Macro)
				|| !Blueprint
				|| !IsOwnedMacro())
			{
				OutError = NativeSignatureFailure(&Graph, TEXT("blueprint_macro_signature_graph_unsupported"),
					!Schema ? TEXT("Macro signatures require the K2 graph schema.")
						: ObservedGraphType != static_cast<int32>(EGraphType::GT_Macro) ? FString::Printf(TEXT("Macro signatures require a macro graph; the native graph type is %d."), ObservedGraphType)
						: !Blueprint ? TEXT("The macro graph has no owning Blueprint.") : TEXT("The Blueprint does not own this graph in its macro collection."),
					TEXT("signature_graph_supported"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Graph.GetName());
				auto Facts = MakeShared<FJsonObject>();
				Facts->SetStringField(TEXT("schema"), Graph.GetSchema() ? Graph.GetSchema()->GetClass()->GetPathName() : FString());
				Facts->SetBoolField(TEXT("blueprint_owner_present"), Blueprint != nullptr);
				if (Blueprint && ObservedGraphType == static_cast<int32>(EGraphType::GT_Macro))
					Facts->SetBoolField(TEXT("owned_macro_graph"), bOwnedMacro);
				if (ObservedGraphType != INDEX_NONE) Facts->SetNumberField(TEXT("graph_type"), ObservedGraphType);
				OutError.ActualEvidence = MakeShared<FJsonValueObject>(Facts);
				return false;
			}

			for (UEdGraphNode* Node : Graph.Nodes)
			{
				UK2Node_Tunnel* Tunnel = ExactCast<UK2Node_Tunnel>(Node);
				if (!Tunnel || !Tunnel->IsEditable())
				{
					continue;
				}
				if (Tunnel->DrawNodeAsEntry())
				{
					if (OutTerminators.Entry)
					{
						OutError = NativeSignatureFailure(&Graph, TEXT("blueprint_macro_signature_entry_conflict"),
							TEXT("A second editable macro entry tunnel was found; exactly one is required."),
							TEXT("observed_entry_count"), MakeShared<FJsonValueNumber>(2), MakeShared<FJsonValueNumber>(1), Graph.GetName());
						return false;
					}
					OutTerminators.Entry = Tunnel;
				}
				else if (Tunnel->DrawNodeAsExit())
				{
					if (OutTerminators.Result)
					{
						OutError = NativeSignatureFailure(&Graph, TEXT("blueprint_macro_signature_result_conflict"),
							TEXT("A second editable macro result tunnel was found; exactly one is required."),
							TEXT("observed_result_count"), MakeShared<FJsonValueNumber>(2), MakeShared<FJsonValueNumber>(1), Graph.GetName());
						return false;
					}
					OutTerminators.Result = Tunnel;
				}
			}
			if (!OutTerminators.Entry || !OutTerminators.Result)
			{
				OutError = NativeSignatureFailure(&Graph, TEXT("blueprint_macro_signature_terminator_missing"),
					!OutTerminators.Entry ? TEXT("The macro graph has no editable entry tunnel.") : TEXT("The macro graph has no editable result tunnel."),
					!OutTerminators.Entry ? TEXT("entry_present") : TEXT("result_present"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Graph.GetName());
				return false;
			}
			return true;
		}

		bool CapturePins(
			const UK2Node_EditablePinBase& Node,
			TArray<TSharedPtr<FJsonValue>>& OutPins,
			FDiagnostic& OutError)
		{
			OutPins.Reset();
			TSet<FName> Names;
			for (int32 PinIndex = 0; PinIndex < Node.UserDefinedPins.Num(); ++PinIndex)
			{
				const TSharedPtr<FUserPinInfo>& Pin = Node.UserDefinedPins[PinIndex];
				if (!Pin.IsValid() || Pin->PinName.IsNone())
				{
					OutError = NativeSignatureFailure(Node.GetGraph(), TEXT("blueprint_function_signature_pin_invalid"),
						FString::Printf(TEXT("Native signature pin %d on '%s' %s."), PinIndex, *Node.GetNodeTitle(ENodeTitleType::ListView).ToString(),
							!Pin.IsValid() ? TEXT("has no pin record") : TEXT("has an empty name")),
						FString::Printf(TEXT("/pins/%d/name"), PinIndex),
						Pin.IsValid() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Pin->PinName.ToString())) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
						MakeShared<FJsonValueString>(TEXT("a non-empty signature pin name")), FString());
					return false;
				}
				if (Names.Contains(Pin->PinName))
				{
					OutError = NativeSignatureFailure(Node.GetGraph(), TEXT("blueprint_function_signature_pin_name_conflict"),
						FString::Printf(TEXT("Native signature pin %d on '%s' repeats pin name '%s'."), PinIndex, *Node.GetNodeTitle(ENodeTitleType::ListView).ToString(), *Pin->PinName.ToString()),
						FString::Printf(TEXT("/pins/%d/name"), PinIndex), MakeShared<FJsonValueString>(Pin->PinName.ToString()),
						MakeShared<FJsonValueString>(TEXT("a unique name within this node")), Pin->PinName.ToString());
					return false;
				}
				Names.Add(Pin->PinName);
				TSharedRef<FJsonObject> Spec = MakeShared<FJsonObject>();
				Spec->SetStringField(TEXT("name"), Pin->PinName.ToString());
				Spec->SetObjectField(
					TEXT("type"),
					UeAgentBlueprintAuthoringOps::BuildPinTypeJsonObject(Pin->PinType,
						FBlueprintEditorUtils::FindBlueprintForGraph(Node.GetGraph())));
				if (!Pin->PinDefaultValue.IsEmpty())
				{
					Spec->SetStringField(TEXT("default_value"), Pin->PinDefaultValue);
				}
				OutPins.Add(MakeShared<FJsonValueObject>(Spec));
			}
			return true;
		}

		FDiagnostic SignatureInputFailure(const TSharedPtr<const FJsonObject>& Signature, const UBlueprint* Blueprint,
			const FString& Code, const FString& Reason, const FString& Pointer,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Candidate)
		{
			FDiagnostic Error = FDiagnostic::Failure(Code, Reason, TEXT("blueprint_signature"));
			Error.PropertyPath = Pointer;
			Error.SemanticPointer = Pointer;
			Error.Source.InternalJsonPath = Pointer;
			Error.ActualValue = MoveTemp(Actual);
			Error.ActualValuePresent = Error.ActualValue.IsValid();
			Error.ExpectedValue = MoveTemp(Expected);
			Error.CandidateName = Candidate;
			if (Blueprint) Error.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Blueprint);
			if (Signature.IsValid()) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Signature);
			return Error;
		}

		void RelateSignatureInput(FDiagnostic& Error, const TSharedPtr<const FJsonObject>& Source, const FString& Field, const FString& Pointer)
		{
			if (!Source.IsValid()) return;
			const FString NativePath = Error.PropertyPath;
			Error.SourceKind.Reset();
			Error.SemanticPointer = TEXT("/") + Field;
			Error.Source.InternalJsonPath = Error.SemanticPointer;
			UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Source, Field);
			if (Error.Source.SourceFile.IsEmpty() && Error.Source.SourceUnit.IsEmpty() && Error.SourceKind != TEXT("command_input"))
			{
				Error.SemanticPointer = Pointer;
				Error.Source.InternalJsonPath = Error.SemanticPointer;
			}
			if (Error.SourceKind != TEXT("command_input")) Error.SourceKind = TEXT("related_input");
			Error.PropertyPath = NativePath;
		}

		bool PlanPins(
			const TSharedPtr<const FJsonObject>& Signature,
			const TCHAR* Field,
			TArray<FPlannedPin>& OutPins,
			FDiagnostic& OutError, UBlueprint* Blueprint)
		{
			OutPins.Reset();
			const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
			if (!Signature.IsValid() || !Signature->TryGetArrayField(Field, Pins) || !Pins)
			{
				OutError = SignatureInputFailure(Signature, Blueprint, TEXT("blueprint_function_signature_group_invalid"),
					Signature.IsValid() ? FString::Printf(TEXT("Signature group '%s' must be an array."), Field) : FString(TEXT("A signature object is required.")),
					Signature.IsValid() ? TEXT("/") + FString(Field) : TEXT("/"),
					Signature.IsValid() ? Signature->TryGetField(Field) : TSharedPtr<FJsonValue>(),
					MakeShared<FJsonValueString>(Signature.IsValid() ? TEXT("an array of signature pin objects") : TEXT("a signature object")), FString());
				return false;
			}
			TSet<FName> Names;
			for (int32 PinIndex = 0; PinIndex < Pins->Num(); ++PinIndex)
			{
				const TSharedPtr<FJsonValue>& Value = (*Pins)[PinIndex];
				const FString PinPointer = FString::Printf(TEXT("/%s/%d"), Field, PinIndex);
				const TSharedPtr<FJsonObject>* Spec = nullptr;
				FString Name;
				if (!Value.IsValid()
					|| !Value->TryGetObject(Spec)
					|| !Spec || !Spec->IsValid()
					|| !(*Spec)->TryGetStringField(TEXT("name"), Name))
				{
					const bool bPinObject = Spec && Spec->IsValid();
					OutError = SignatureInputFailure(Signature, Blueprint, TEXT("blueprint_function_signature_pin_invalid"),
						bPinObject ? TEXT("A signature pin requires a string name.") : TEXT("Each signature pin must be an object."),
						PinPointer + (bPinObject ? TEXT("/name") : TEXT("")),
						bPinObject ? (*Spec)->TryGetField(TEXT("name")) : Value,
						MakeShared<FJsonValueString>(bPinObject ? TEXT("a string pin name") : TEXT("a pin object with name and type")), FString());
					return false;
				}
				Name.TrimStartAndEndInline();
				const FName PinName(*Name);
				if (PinName.IsNone() || Names.Contains(PinName))
				{
					OutError = SignatureInputFailure(Signature, Blueprint, TEXT("blueprint_function_signature_pin_name_conflict"),
						PinName.IsNone() ? TEXT("The signature pin name is empty or resolves to None.") : TEXT("The signature pin name duplicates another name in this group."),
						PinPointer + TEXT("/name"), MakeShared<FJsonValueString>(Name),
						MakeShared<FJsonValueString>(TEXT("a non-None, unique pin name")), Name);
					return false;
				}
				Names.Add(PinName);
				const TSharedPtr<FJsonObject>* Type = nullptr;
				if (!(*Spec)->TryGetObjectField(TEXT("type"), Type) || !Type)
				{
					OutError = FDiagnostic::Failure(TEXT("blueprint_function_signature_pin_type_missing"),
						FString::Printf(TEXT("Signature pin '%s' requires a type object."), *Name), TEXT("blueprint_signature"));
					OutError.CandidateName = Name;
					OutError.PropertyPath = TEXT("type");
					OutError.SemanticPointer = TEXT("/type");
					OutError.ActualValue = (*Spec)->TryGetField(TEXT("type"));
					OutError.ActualValuePresent = OutError.ActualValue.IsValid();
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("a pin type object"));
					UeAgentAuthoringDiagnostics::LocateFailure(OutError, **Spec, TEXT("blueprint_signature"),
						Blueprint ? UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Blueprint) : FString(), TEXT("type"));
					if (OutError.Source.SourceUnit.IsEmpty())
						UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError, PinPointer);
					if (Signature.IsValid()) UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Signature);
					return false;
				}
				FPlannedPin Planned;
				Planned.Name = PinName;
				Planned.Source = *Spec;
				Planned.SourcePointer = PinPointer;
				if (!UeAgentBlueprintAuthoringOps::BuildPinTypeFromJsonObject(
						*Type, Planned.Type, OutError, Blueprint))
				{
					if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Name;
					UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError, TEXT("/type"));
					UeAgentAuthoringDiagnostics::LocateFailure(OutError, **Spec, TEXT("blueprint_signature"), OutError.TargetPath, TEXT("type"));
					if (OutError.Source.SourceUnit.IsEmpty())
						UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError, PinPointer);
					if (Signature.IsValid()) UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Signature);
					return false;
				}
				Planned.bHasDefaultValue = (*Spec)->TryGetStringField(TEXT("default_value"), Planned.DefaultValue);
				OutPins.Add(MoveTemp(Planned));
			}
			return true;
		}

		bool ReplacePins(
			UK2Node_EditablePinBase& Node,
			const TArray<FPlannedPin>& Pins,
			const EEdGraphPinDirection Direction,
			FDiagnostic& OutError)
		{
			TMap<FName, const FPlannedPin*> DesiredByName;
			for (const FPlannedPin& Pin : Pins)
			{
				FText Reason;
				if (!Node.CanCreateUserDefinedPin(Pin.Type, Direction, Reason))
				{
					OutError = NativeSignatureFailure(Node.GetGraph(), TEXT("blueprint_function_signature_pin_rejected"),
						FString::Printf(TEXT("The native node '%s' rejected signature pin '%s' in the %s direction."),
							*Node.GetNodeTitle(ENodeTitleType::ListView).ToString(), *Pin.Name.ToString(), Direction == EGPD_Input ? TEXT("input") : TEXT("output")),
						TEXT("type"), MakeShared<FJsonValueObject>(UeAgentBlueprintAuthoringOps::BuildPinTypeJsonObject(Pin.Type,
							FBlueprintEditorUtils::FindBlueprintForGraph(Node.GetGraph()))), MakeShared<FJsonValueString>(TEXT("a type accepted by this node in the requested direction")), Pin.Name.ToString());
					if (!Reason.IsEmpty())
					{
						auto Cause = MakeShared<FDiagnostic>(FDiagnostic::Failure(TEXT("blueprint_signature_native_pin_validation_failed"), Reason.ToString(), TEXT("native_pin_validation")));
						Cause->SourceKind = TEXT("native_result");
						OutError.PublicCause = MoveTemp(Cause);
					}
					RelateSignatureInput(OutError, Pin.Source, TEXT("type"), Pin.SourcePointer + TEXT("/type"));
					return false;
				}
				DesiredByName.Add(Pin.Name, &Pin);
			}

			const TArray<TSharedPtr<FUserPinInfo>> Existing = Node.UserDefinedPins;
			for (const TSharedPtr<FUserPinInfo>& Pin : Existing)
			{
				const FPlannedPin* const* Desired = Pin.IsValid()
					? DesiredByName.Find(Pin->PinName) : nullptr;
				if (Desired == nullptr || *Desired == nullptr
					|| Pin->PinType != (*Desired)->Type)
				{
					Node.RemoveUserDefinedPin(Pin);
					continue;
				}
				if ((*Desired)->bHasDefaultValue && Pin->PinDefaultValue != (*Desired)->DefaultValue
					&& !Node.ModifyUserDefinedPinDefaultValue(
						Pin, (*Desired)->DefaultValue))
				{
					OutError = NativeSignatureFailure(Node.GetGraph(), TEXT("blueprint_function_signature_pin_default_rejected"),
						FString::Printf(TEXT("The native node could not set the requested default for existing signature pin '%s'."), *Pin->PinName.ToString()),
						TEXT("default_value"), MakeShared<FJsonValueString>(Pin->PinDefaultValue), MakeShared<FJsonValueString>((*Desired)->DefaultValue), Pin->PinName.ToString());
					RelateSignatureInput(OutError, (*Desired)->Source, TEXT("default_value"), (*Desired)->SourcePointer + TEXT("/default_value"));
					return false;
				}
				DesiredByName.Remove(Pin->PinName);
			}

			for (const FPlannedPin& Pin : Pins)
			{
				if (!DesiredByName.Contains(Pin.Name))
				{
					continue;
				}
				if (!Node.CreateUserDefinedPin(Pin.Name, Pin.Type, Direction, false))
				{
					OutError = NativeSignatureFailure(Node.GetGraph(), TEXT("blueprint_function_signature_pin_create_failed"),
						FString::Printf(TEXT("The native node did not create signature pin '%s'."), *Pin.Name.ToString()),
						TEXT("pin_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Pin.Name.ToString());
					auto Facts = MakeShared<FJsonObject>();
					Facts->SetObjectField(TEXT("requested_type"), UeAgentBlueprintAuthoringOps::BuildPinTypeJsonObject(Pin.Type,
						FBlueprintEditorUtils::FindBlueprintForGraph(Node.GetGraph())));
					Facts->SetStringField(TEXT("direction"), Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
					OutError.ActualEvidence = MakeShared<FJsonValueObject>(Facts);
					RelateSignatureInput(OutError, Pin.Source, TEXT("name"), Pin.SourcePointer + TEXT("/name"));
					return false;
				}
				const TSharedPtr<FUserPinInfo>* CreatedInfo = Node.UserDefinedPins.FindByPredicate(
					[&Pin](const TSharedPtr<FUserPinInfo>& Item)
					{
						return Item.IsValid() && Item->PinName == Pin.Name;
					});
				if (Pin.bHasDefaultValue
					&& (!CreatedInfo || !CreatedInfo->IsValid()
						|| !Node.ModifyUserDefinedPinDefaultValue(
							*CreatedInfo, Pin.DefaultValue)))
				{
					const bool bPinRecordPresent = CreatedInfo && CreatedInfo->IsValid();
					OutError = NativeSignatureFailure(Node.GetGraph(), TEXT("blueprint_function_signature_pin_default_rejected"),
						bPinRecordPresent ? FString::Printf(TEXT("The native node could not set the requested default for new signature pin '%s'."), *Pin.Name.ToString())
							: FString::Printf(TEXT("New signature pin '%s' has no native record for assigning requested default '%s'."), *Pin.Name.ToString(), *Pin.DefaultValue),
						bPinRecordPresent ? TEXT("default_value") : TEXT("pin_record_present"),
						bPinRecordPresent ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>((*CreatedInfo)->PinDefaultValue)) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)),
						bPinRecordPresent ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Pin.DefaultValue)) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true)), Pin.Name.ToString());
					RelateSignatureInput(OutError, Pin.Source, TEXT("default_value"), Pin.SourcePointer + TEXT("/default_value"));
					return false;
				}
			}

			TMap<FName, int32> DesiredOrder;
			for (int32 Index = 0; Index < Pins.Num(); ++Index)
			{
				DesiredOrder.Add(Pins[Index].Name, Index);
			}
			Node.UserDefinedPins.StableSort(
				[&DesiredOrder](const TSharedPtr<FUserPinInfo>& Left,
					const TSharedPtr<FUserPinInfo>& Right)
				{
					const int32* LeftIndex = Left.IsValid()
						? DesiredOrder.Find(Left->PinName) : nullptr;
					const int32* RightIndex = Right.IsValid()
						? DesiredOrder.Find(Right->PinName) : nullptr;
					return (LeftIndex ? *LeftIndex : MAX_int32)
						< (RightIndex ? *RightIndex : MAX_int32);
				});
			Node.ReconstructNode();
			return true;
		}

		UK2Node_FunctionResult* CreateFunctionResult(
			UEdGraph& Graph,
			UK2Node_FunctionEntry& Entry)
		{
			FGraphNodeCreator<UK2Node_FunctionResult> Creator(Graph);
			UK2Node_FunctionResult* Result = Creator.CreateNode();
			Result->SetFlags(RF_Transactional);
			Result->NodePosX = Entry.NodePosX + Entry.NodeWidth + 256;
			Result->NodePosY = Entry.NodePosY;
			Result->bIsEditable = true;
			UEdGraphSchema_K2::SetNodeMetaData(Result, FNodeMetadata::DefaultGraphNode);
			Creator.Finalize();

			const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Graph.GetSchema());
			UEdGraphPin* ThenPin = Schema
				? Schema->FindExecutionPin(Entry, EGPD_Output) : nullptr;
			UEdGraphPin* ReturnPin = Schema
				? Schema->FindExecutionPin(*Result, EGPD_Input) : nullptr;
			if (ThenPin && ReturnPin && ThenPin->LinkedTo.IsEmpty())
			{
				ThenPin->MakeLinkTo(ReturnPin);
			}
			return Result;
		}
	}

	bool CanAuthorFunctionGraphSignature(const UEdGraph& Graph)
	{
		FFunctionTerminators Terminators;
		FDiagnostic Error;
		return ResolveTerminators(
			Graph, EFunctionSignatureOwner::FunctionGraph, Terminators, Error);
	}

	bool CaptureFunctionGraphSignature(
		const UEdGraph& Graph,
		TSharedRef<FJsonObject>& OutSignature,
		FDiagnostic& OutError)
	{
		OutError.Reset();
		FFunctionTerminators Terminators;
		if (!ResolveTerminators(
			Graph, EFunctionSignatureOwner::FunctionGraph, Terminators, OutError))
		{
			return false;
		}
		for (int32 Index = 1; Index < Terminators.Results.Num(); ++Index)
		{
			if (!AreUserPinsEquivalent(
					*Terminators.Results[0], *Terminators.Results[Index], Index, OutError))
			{
				return false;
			}
		}

		TArray<TSharedPtr<FJsonValue>> Inputs;
		TArray<TSharedPtr<FJsonValue>> Outputs;
		if (!CapturePins(*Terminators.Entry, Inputs, OutError)
			|| (!Terminators.Results.IsEmpty()
				&& !CapturePins(*Terminators.Results[0], Outputs, OutError)))
		{
			return false;
		}
		TSharedRef<FJsonObject> Signature = MakeShared<FJsonObject>();
		Signature->SetArrayField(TEXT("inputs"), MoveTemp(Inputs));
		Signature->SetArrayField(TEXT("outputs"), MoveTemp(Outputs));
		OutSignature = Signature;
		return true;
	}

	bool ApplyFunctionGraphSignature(
		UEdGraph& Graph,
		const TSharedPtr<const FJsonObject>& Signature,
		FDiagnostic& OutError)
	{
		OutError.Reset();
		TArray<FPlannedPin> Inputs;
		TArray<FPlannedPin> Outputs;
		UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(&Graph);
		if (!PlanPins(Signature, TEXT("inputs"), Inputs, OutError, Blueprint)
			|| !PlanPins(Signature, TEXT("outputs"), Outputs, OutError, Blueprint))
		{
			return false;
		}

		FFunctionTerminators Terminators;
		if (!ResolveTerminators(
			Graph, EFunctionSignatureOwner::FunctionGraph, Terminators, OutError))
		{
			return false;
		}
		if (Terminators.Results.IsEmpty() && !Outputs.IsEmpty())
		{
			UK2Node_FunctionResult* Result = CreateFunctionResult(
				Graph, *Terminators.Entry);
			if (!Result)
			{
				OutError = NativeSignatureFailure(&Graph, TEXT("blueprint_function_signature_result_create_failed"),
					FString::Printf(TEXT("The native editor could not create a function result node for %d requested output pins."), Outputs.Num()),
					TEXT("result_node_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Graph.GetName());
				RelateSignatureInput(OutError, Signature, TEXT("outputs"), TEXT("/outputs"));
				return false;
			}
			Terminators.Results.Add(Result);
		}

		Terminators.Entry->Modify();
		if (!ReplacePins(*Terminators.Entry, Inputs, EGPD_Output, OutError))
		{
			return false;
		}
		for (UK2Node_FunctionResult* Result : Terminators.Results)
		{
			Result->Modify();
			if (!ReplacePins(*Result, Outputs, EGPD_Input, OutError))
			{
				return false;
			}
		}
		return true;
	}

	bool CanAuthorMacroGraphSignature(const UEdGraph& Graph)
	{
		FMacroTerminators Terminators;
		FDiagnostic Error;
		return ResolveMacroTerminators(Graph, Terminators, Error);
	}

	bool CaptureMacroGraphSignature(
		const UEdGraph& Graph,
		TSharedRef<FJsonObject>& OutSignature,
		FDiagnostic& OutError)
	{
		OutError.Reset();
		FMacroTerminators Terminators;
		if (!ResolveMacroTerminators(Graph, Terminators, OutError))
		{
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Inputs;
		TArray<TSharedPtr<FJsonValue>> Outputs;
		if (!CapturePins(*Terminators.Entry, Inputs, OutError)
			|| !CapturePins(*Terminators.Result, Outputs, OutError))
		{
			return false;
		}
		TSharedRef<FJsonObject> Signature = MakeShared<FJsonObject>();
		Signature->SetArrayField(TEXT("inputs"), MoveTemp(Inputs));
		Signature->SetArrayField(TEXT("outputs"), MoveTemp(Outputs));
		OutSignature = Signature;
		return true;
	}

	bool ApplyMacroGraphSignature(
		UEdGraph& Graph,
		const TSharedPtr<const FJsonObject>& Signature,
		FDiagnostic& OutError)
	{
		OutError.Reset();
		TArray<FPlannedPin> Inputs;
		TArray<FPlannedPin> Outputs;
		UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(&Graph);
		if (!PlanPins(Signature, TEXT("inputs"), Inputs, OutError, Blueprint)
			|| !PlanPins(Signature, TEXT("outputs"), Outputs, OutError, Blueprint))
		{
			return false;
		}
		FMacroTerminators Terminators;
		if (!ResolveMacroTerminators(Graph, Terminators, OutError))
		{
			return false;
		}
		Terminators.Entry->Modify();
		Terminators.Result->Modify();
		return ReplacePins(*Terminators.Entry, Inputs, EGPD_Output, OutError)
			&& ReplacePins(*Terminators.Result, Outputs, EGPD_Input, OutError);
	}

	bool CaptureDelegateSignatureGraph(
		const UEdGraph& Graph,
		TSharedRef<FJsonObject>& OutSignature,
		FDiagnostic& OutError)
	{
		OutError.Reset();
		FFunctionTerminators Terminators;
		if (!ResolveTerminators(
			Graph, EFunctionSignatureOwner::DelegateMember, Terminators, OutError))
		{
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Inputs;
		TArray<TSharedPtr<FJsonValue>> Outputs;
		if (!CapturePins(*Terminators.Entry, Inputs, OutError)
			|| (!Terminators.Results.IsEmpty()
				&& !CapturePins(*Terminators.Results[0], Outputs, OutError)))
		{
			return false;
		}
		TSharedRef<FJsonObject> Signature = MakeShared<FJsonObject>();
		Signature->SetArrayField(TEXT("inputs"), MoveTemp(Inputs));
		Signature->SetArrayField(TEXT("outputs"), MoveTemp(Outputs));
		OutSignature = Signature;
		return true;
	}

	bool ApplyDelegateSignatureGraph(
		UEdGraph& Graph,
		const TSharedPtr<const FJsonObject>& Signature,
		FDiagnostic& OutError)
	{
		OutError.Reset();
		TArray<FPlannedPin> Inputs;
		TArray<FPlannedPin> Outputs;
		UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(&Graph);
		if (!PlanPins(Signature, TEXT("inputs"), Inputs, OutError, Blueprint)
			|| !PlanPins(Signature, TEXT("outputs"), Outputs, OutError, Blueprint))
		{
			return false;
		}
		if (!Outputs.IsEmpty())
		{
			OutError = SignatureInputFailure(Signature, Blueprint, TEXT("blueprint_delegate_signature_outputs_unsupported"),
				TEXT("Delegate signatures cannot declare output pins."), TEXT("/outputs"),
				MakeShared<FJsonValueNumber>(Outputs.Num()), MakeShared<FJsonValueNumber>(0), Graph.GetName());
			return false;
		}
		FFunctionTerminators Terminators;
		if (!ResolveTerminators(
			Graph, EFunctionSignatureOwner::DelegateMember, Terminators, OutError))
		{
			return false;
		}
		Terminators.Entry->Modify();
		return ReplacePins(*Terminators.Entry, Inputs, EGPD_Output, OutError);
	}

	bool CaptureCustomEventSignature(
		const UK2Node_CustomEvent& Event,
		TSharedRef<FJsonObject>& OutSignature,
		FDiagnostic& OutError)
	{
		OutError.Reset();
		TArray<TSharedPtr<FJsonValue>> Inputs;
		if (!Event.IsEditable())
		{
			OutError = NativeSignatureFailure(Event.GetGraph(), TEXT("blueprint_custom_event_signature_read_only"),
				TEXT("The native custom event is not editable."), TEXT("event_editable"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Event.GetNodeTitle(ENodeTitleType::ListView).ToString());
			return false;
		}
		if (!CapturePins(Event, Inputs, OutError)) return false;
		TSharedRef<FJsonObject> Signature = MakeShared<FJsonObject>();
		Signature->SetArrayField(TEXT("inputs"), MoveTemp(Inputs));
		Signature->SetArrayField(
			TEXT("outputs"), TArray<TSharedPtr<FJsonValue>>());
		OutSignature = Signature;
		return true;
	}

	bool ApplyCustomEventSignature(
		UK2Node_CustomEvent& Event,
		const TSharedPtr<const FJsonObject>& Signature,
		FDiagnostic& OutError)
	{
		OutError.Reset();
		TArray<FPlannedPin> Inputs;
		TArray<FPlannedPin> Outputs;
		UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(Event.GetGraph());
		if (!PlanPins(Signature, TEXT("inputs"), Inputs, OutError, Blueprint)
			|| !PlanPins(Signature, TEXT("outputs"), Outputs, OutError, Blueprint))
		{
			return false;
		}
		if (!Outputs.IsEmpty())
		{
			OutError = SignatureInputFailure(Signature, Blueprint, TEXT("blueprint_custom_event_signature_outputs_unsupported"),
				TEXT("Custom event signatures cannot declare output pins."), TEXT("/outputs"),
				MakeShared<FJsonValueNumber>(Outputs.Num()), MakeShared<FJsonValueNumber>(0), Event.GetNodeTitle(ENodeTitleType::ListView).ToString());
			return false;
		}
		if (!Event.IsEditable())
		{
			OutError = NativeSignatureFailure(Event.GetGraph(), TEXT("blueprint_custom_event_signature_read_only"),
				TEXT("The native custom event is not editable."), TEXT("event_editable"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Event.GetNodeTitle(ENodeTitleType::ListView).ToString());
			RelateSignatureInput(OutError, Signature, FString(), TEXT("/"));
			return false;
		}
		Event.Modify();
		return ReplacePins(Event, Inputs, EGPD_Output, OutError);
	}
}
