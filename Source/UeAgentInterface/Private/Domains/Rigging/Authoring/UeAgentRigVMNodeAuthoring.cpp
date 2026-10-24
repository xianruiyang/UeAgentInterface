#include "Domains/Rigging/Authoring/UeAgentRigVMNodeAuthoring.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"

#include "BlueprintNodeSpawner.h"
#include "ControlRigBlueprint.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringAssetIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringEncodedTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/NodeSpawners/RigVMEdGraphEnumNodeSpawner.h"
#include "EdGraph/NodeSpawners/RigVMEdGraphFunctionRefNodeSpawner.h"
#include "EdGraph/NodeSpawners/RigVMEdGraphInvokeEntryNodeSpawner.h"
#include "EdGraph/NodeSpawners/RigVMEdGraphTemplateNodeSpawner.h"
#include "EdGraph/NodeSpawners/RigVMEdGraphUnitNodeSpawner.h"
#include "EdGraph/NodeSpawners/RigVMEdGraphVariableNodeSpawner.h"
#include "RigVMModel/Nodes/RigVMEnumNode.h"
#include "RigVMModel/Nodes/RigVMFunctionReferenceNode.h"
#include "RigVMModel/Nodes/RigVMInvokeEntryNode.h"
#include "RigVMModel/Nodes/RigVMTemplateNode.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/Nodes/RigVMVariableNode.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMFunctionLibrary.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMPin.h"
#include "UObject/Class.h"

namespace UeAgentRigVMNodeAuthoring
{
	namespace
	{
		struct FNodeFamily
		{
			const TCHAR* Type;
			UClass* Spawner;
			UClass* Node;
		};

		TConstArrayView<FNodeFamily> Families()
		{
			static const FNodeFamily Items[] = {
				{TEXT("unit"), URigVMEdGraphUnitNodeSpawner::StaticClass(), URigVMUnitNode::StaticClass()},
				{TEXT("variable"), URigVMEdGraphVariableNodeSpawner::StaticClass(), URigVMVariableNode::StaticClass()},
				{TEXT("enum"), URigVMEdGraphEnumNodeSpawner::StaticClass(), URigVMEnumNode::StaticClass()},
				{TEXT("function_reference"), URigVMEdGraphFunctionRefNodeSpawner::StaticClass(), URigVMFunctionReferenceNode::StaticClass()},
				{TEXT("invoke_entry"), URigVMEdGraphInvokeEntryNodeSpawner::StaticClass(), URigVMInvokeEntryNode::StaticClass()},
				{TEXT("template"), URigVMEdGraphTemplateNodeSpawner::StaticClass(), URigVMTemplateNode::StaticClass()},
			};
			return Items;
		}

		FString FieldPointer(const FString& Field)
		{
			return TEXT("/") + Field.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		}

		UE::UAI::Contract::FDiagnostic InputFailure(const FJsonObject& Definition, const TCHAR* Code,
			const FString& Reason, const FString& Pointer, const TSharedPtr<FJsonValue>& Actual,
			const TSharedPtr<FJsonValue>& Expected)
		{
			auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Pointer, Expected, Actual, TEXT("rigvm_input"), Reason);
			Error.SourceKind = TEXT("authored_value");
			Error.Source.InternalJsonPath = Pointer;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValuePresent = Expected.IsValid();
			UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, Definition);
			return Error;
		}

		UE::UAI::Contract::FDiagnostic NativeFailure(const UObject& Target, const TCHAR* Code,
			const FString& Reason, const FString& Property, const TSharedPtr<FJsonValue>& Actual,
			const TSharedPtr<FJsonValue>& Expected)
		{
			auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Property, Expected, Actual, TEXT("rigvm_apply"), Reason);
			Error.SourceKind = TEXT("native_result");
			Error.Source = {};
			Error.PropertyPath = Property;
			const UControlRigBlueprint* Blueprint = Cast<UControlRigBlueprint>(&Target);
			if (!Blueprint) Blueprint = Target.GetTypedOuter<UControlRigBlueprint>();
			Error.TargetPath = Blueprint ? UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Blueprint) : Target.GetPathName();
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValuePresent = Expected.IsValid();
			return Error;
		}

		bool ReadRequired(const FJsonObject& Definition, const TCHAR* Field, FString& Value, UE::UAI::Contract::FDiagnostic& Error)
		{
			if (Definition.TryGetStringField(Field, Value) && !Value.IsEmpty()) return true;
			Error = InputFailure(Definition, TEXT("rigvm_node_field_required"),
				FString::Printf(TEXT("Node definition field '%s' must be a non-empty string."), Field), FieldPointer(Field),
				Definition.TryGetField(Field), MakeShared<FJsonValueString>(TEXT("non-empty string")));
			return false;
		}
	}

	FString GetSupportedNodeType(const UBlueprintNodeSpawner& Spawner)
	{
		for (const auto& Family : Families())
			if (Spawner.IsA(Family.Spawner)) return Family.Type;
		return FString();
	}

	bool CaptureDefinition(const UControlRigBlueprint& Blueprint, const URigVMNode& Node, FJsonObject& OutDefinition)
	{
		const FNodeFamily* Family = nullptr;
		for (const auto& Item : Families())
			if (Node.IsA(Item.Node)) { Family = &Item; break; }
		OutDefinition.SetStringField(TEXT("node_type"), Family ? Family->Type : *Node.GetClass()->GetPathName());
		if (!Family) return false;
		if (const auto* Unit = Cast<URigVMUnitNode>(&Node))
		{
			OutDefinition.SetStringField(TEXT("script_struct"), Unit->GetScriptStruct() ? Unit->GetScriptStruct()->GetPathName() : FString());
			OutDefinition.SetStringField(TEXT("method"), Unit->GetMethodName().ToString());
		}
		else if (const auto* Variable = Cast<URigVMVariableNode>(&Node))
		{
			OutDefinition.SetStringField(TEXT("variable_name"), Variable->GetVariableName().ToString());
			OutDefinition.SetStringField(TEXT("cpp_type"), Variable->GetCPPType());
			OutDefinition.SetStringField(TEXT("cpp_type_object_path"), Variable->GetCPPTypeObject() ? Variable->GetCPPTypeObject()->GetPathName() : FString());
			OutDefinition.SetBoolField(TEXT("is_getter"), Variable->IsGetter());
		}
		else if (const auto* Enum = Cast<URigVMEnumNode>(&Node))
		{
			OutDefinition.SetStringField(TEXT("enum_path"), Enum->GetEnum() ? Enum->GetEnum()->GetPathName() : FString());
		}
		else if (const auto* Function = Cast<URigVMFunctionReferenceNode>(&Node))
		{
			const auto Identifier = Function->GetFunctionIdentifier();
			const FString LibraryAsset = Identifier.GetNodeSoftPath().GetAssetPathString();
			const bool bSelf = LibraryAsset == Blueprint.GetPathName()
				|| LibraryAsset == UeAgentAuthoringAssetIdentity::ToObjectPath(
					UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint));
			OutDefinition.SetStringField(TEXT("function_host"), bSelf ? TEXT("self") : Identifier.HostObject.ToString());
			OutDefinition.SetStringField(TEXT("function_name"), Function->GetReferencedFunctionHeader().Name.ToString());
			const auto Variables = MakeShared<FJsonObject>();
			for (const auto& Mapping : Function->GetVariableMap())
				UeAgentAuthoringEncodedTypes::FScope::SetNameField(Variables, Mapping.Key.ToString(), Mapping.Value);
			UeAgentAuthoringEncodedTypes::FScope::RecordNameKeyedObject(Variables);
			OutDefinition.SetObjectField(TEXT("variable_map"), Variables);
		}
		else if (const auto* Entry = Cast<URigVMInvokeEntryNode>(&Node))
		{
			OutDefinition.SetStringField(TEXT("entry_name"), Entry->GetEntryName().ToString());
		}
		else if (const auto* Template = Cast<URigVMTemplateNode>(&Node))
		{
			OutDefinition.SetStringField(TEXT("notation"), Template->GetNotation().ToString());
			const auto Types = MakeShared<FJsonObject>();
			for (const auto* Pin : Template->GetPins())
			{
				if (!Pin || Pin->IsWildCard() || Pin->IsExecuteContext() || Pin->GetDirection() == ERigVMPinDirection::Hidden) continue;
				const auto Type = MakeShared<FJsonObject>();
				Type->SetStringField(TEXT("cpp_type"), Pin->GetCPPType());
				Type->SetStringField(TEXT("cpp_type_object_path"), Pin->GetCPPTypeObject() ? Pin->GetCPPTypeObject()->GetPathName() : FString());
				Types->SetObjectField(Pin->GetName(), Type);
			}
			OutDefinition.SetObjectField(TEXT("resolved_types"), Types);
		}
		return true;
	}

	bool IsAuthorableDefaultPin(const URigVMPin& Pin)
	{
		// A struct/array root already contains its children. Keep only one editable carrier.
		return !Pin.GetParentPin() && !Pin.IsExecuteContext() && !Pin.IsWildCard()
			&& Pin.GetDirection() != ERigVMPinDirection::Hidden
			&& (Pin.GetDirection() != ERigVMPinDirection::Output
				|| (Pin.GetNode()->IsA<URigVMVariableNode>() && Pin.GetName() == TEXT("Value")));
	}

	bool ApplyPinDefaults(URigVMController& Controller, URigVMNode& Node,
		const FJsonObject& Definition, bool bNewNode, const FString& Path, TArray<UE::UAI::Contract::FDiagnostic>& Issues)
	{
		const int32 FirstIssue = Issues.Num();
		const auto QualifyIssues = [&]() {
			for (int32 Index = FirstIssue; Index < Issues.Num(); ++Index) {
				auto& Error = Issues[Index];
				if (Error.SourceKind == TEXT("native_result")) {
					if (Error.SemanticPointer.IsEmpty()) Error.SemanticPointer = Path;
				} else if (Error.SourceKind != TEXT("command_input")) {
					UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Error, Path);
				}
			}
		};
		struct FDefaultEdit { FString PinPath; FString LocalPath; FString Value; FString Source; };
		TArray<FDefaultEdit> Edits;
		bool bValid = true;
		auto AddEdit = [&](const FString& RelativePath, const TSharedPtr<FJsonValue>& Value, const FString& Source)
		{
			FString Text;
			if (!Value || !Value->TryGetString(Text))
			{
				Issues.Add(InputFailure(Definition, TEXT("json_field_type_mismatch"), TEXT("Pin default values must be strings."),
					Source, Value, MakeShared<FJsonValueString>(TEXT("string"))));
				bValid = false;
				return;
			}
			const FString Prefix = Node.GetName() + TEXT(".");
			const FString LocalPath = RelativePath.StartsWith(Prefix) ? RelativePath.Mid(Prefix.Len()) : RelativePath;
			const URigVMPin* Pin = Node.FindPin(LocalPath);
			// Compare every legacy carrier with the same untouched baseline, not earlier writes.
			if (!bNewNode && Pin && Pin->GetDefaultValue() == Text) return;
			if (LocalPath == TEXT("WorkData")) return;
			const FString FullPath = Prefix + LocalPath;
			if (const auto* Previous = Edits.FindByPredicate([&](const auto& Edit) { return Edit.PinPath == FullPath; }))
			{
				if (Previous->Value != Text)
				{
					auto Error = InputFailure(Definition, TEXT("rigvm_pin_default_conflict"),
						TEXT("Two input fields request different defaults for the same pin."), Source,
						MakeShared<FJsonValueString>(Text), MakeShared<FJsonValueString>(Previous->Value));
					Error.Diagnostics.Add(InputFailure(Definition, TEXT("rigvm_pin_default_previous_value"),
						TEXT("The first default for this pin was supplied here."), Previous->Source,
						MakeShared<FJsonValueString>(Previous->Value), MakeShared<FJsonValueString>(Text)));
					Issues.Add(MoveTemp(Error));
					bValid = false;
				}
				return;
			}
			Edits.Add({FullPath, LocalPath, Text, Source});
		};
		if (Node.IsA<URigVMVariableNode>())
			if (const auto Value = Definition.TryGetField(TEXT("default_value")))
				AddEdit(TEXT("Value"), Value, TEXT("/default_value"));
		const TSharedPtr<FJsonObject>* Defaults = nullptr;
		if (Definition.TryGetObjectField(TEXT("pin_defaults"), Defaults))
			for (const auto& Pair : (*Defaults)->Values)
				AddEdit(Pair.Key, Pair.Value, TEXT("/pin_defaults") + FieldPointer(Pair.Key));
		else if (Definition.HasField(TEXT("pin_defaults")))
		{
			Issues.Add(InputFailure(Definition, TEXT("json_field_type_mismatch"),
				TEXT("Pin defaults must be an object keyed by pin name."), TEXT("/pin_defaults"),
				Definition.TryGetField(TEXT("pin_defaults")), MakeShared<FJsonValueString>(TEXT("object"))));
			bValid = false;
		}
		if (!bValid) { QualifyIssues(); return false; }
		// Explicit changed child values refine a changed parent; input object order is irrelevant.
		Edits.Sort([](const auto& A, const auto& B) {
			return A.PinPath.Len() != B.PinPath.Len() ? A.PinPath.Len() < B.PinPath.Len() : A.PinPath < B.PinPath;
		});
		for (const auto& Edit : Edits)
		{
			if (!Controller.SetPinDefaultValue(Edit.PinPath, Edit.Value, true, true, false, false, false))
			{
				const URigVMPin* Pin = Node.FindPin(Edit.LocalPath);
				Issues.Add(NativeFailure(Node, TEXT("rigvm_set_pin_default_failed"),
					TEXT("The native RigVM controller rejected the requested pin default."), Edit.LocalPath,
					Pin ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Pin->GetDefaultValue())) : nullptr,
					MakeShared<FJsonValueString>(Edit.Value)));
				bValid = false;
			}
		}
		QualifyIssues();
		return bValid;
	}

	URigVMNode* CreateNode(UControlRigBlueprint& Blueprint, URigVMController& Controller,
		const FJsonObject& Definition, const FString& Name, const FVector2D& Position, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		FString Type;
		if (!ReadRequired(Definition, TEXT("node_type"), Type, OutError)) return nullptr;
		URigVMNode* Node = nullptr;
		FString Identity;
		if (Type == TEXT("unit"))
		{
			if (!ReadRequired(Definition, TEXT("script_struct"), Identity, OutError)) return nullptr;
			FString Method = TEXT("Execute");
			Definition.TryGetStringField(TEXT("method"), Method);
			Node = Controller.AddUnitNodeFromStructPath(Identity, FName(*Method), Position, Name, false, false);
		}
		else if (Type == TEXT("variable"))
		{
			FString CPPType, ObjectPath, Default;
			bool bGetter = true;
			if (!ReadRequired(Definition, TEXT("variable_name"), Identity, OutError)
				|| !ReadRequired(Definition, TEXT("cpp_type"), CPPType, OutError)) return nullptr;
			Definition.TryGetStringField(TEXT("cpp_type_object_path"), ObjectPath);
			Definition.TryGetStringField(TEXT("default_value"), Default);
			Definition.TryGetBoolField(TEXT("is_getter"), bGetter);
			Node = Controller.AddVariableNodeFromObjectPath(FName(*Identity), CPPType, ObjectPath, bGetter, Default, Position, Name, false, false);
		}
		else if (Type == TEXT("enum"))
		{
			if (!ReadRequired(Definition, TEXT("enum_path"), Identity, OutError)) return nullptr;
			Node = Controller.AddEnumNode(FName(*Identity), Position, Name, false, false);
		}
		else if (Type == TEXT("function_reference"))
		{
			FString Host;
			if (!ReadRequired(Definition, TEXT("function_host"), Host, OutError)
				|| !ReadRequired(Definition, TEXT("function_name"), Identity, OutError)) return nullptr;
			if (Host == TEXT("self"))
			{
				auto* Library = Blueprint.GetLocalFunctionLibrary();
				auto* Function = Library ? Library->FindFunction(FName(*Identity)) : nullptr;
				if (Function) Node = Controller.AddFunctionReferenceNode(Function, Position, Name, false, false);
			}
			else Node = Controller.AddExternalFunctionReferenceNode(Host, FName(*Identity), Position, Name, false, false);
		}
		else if (Type == TEXT("invoke_entry"))
		{
			if (!ReadRequired(Definition, TEXT("entry_name"), Identity, OutError)) return nullptr;
			Node = Controller.AddInvokeEntryNode(FName(*Identity), Position, Name, false, false);
		}
		else if (Type == TEXT("template"))
		{
			if (!ReadRequired(Definition, TEXT("notation"), Identity, OutError)) return nullptr;
			Node = Controller.AddTemplateNode(FName(*Identity), Position, Name, false, false);
			const TSharedPtr<FJsonObject>* Types = nullptr;
			if (Node && Definition.TryGetObjectField(TEXT("resolved_types"), Types))
				for (const auto& Pair : (*Types)->Values)
				{
					const auto Value = Pair.Value && Pair.Value->Type == EJson::Object ? Pair.Value->AsObject() : nullptr;
					FString CPPType, ObjectPath;
					const FString TypePointer = TEXT("/resolved_types") + FieldPointer(Pair.Key);
					if (!Value) {
						OutError = InputFailure(Definition, TEXT("rigvm_template_type_resolution_failed"),
							TEXT("A resolved pin type must be an object."), TypePointer, Pair.Value,
							MakeShared<FJsonValueString>(TEXT("object with cpp_type and optional cpp_type_object_path")));
						return nullptr;
					}
					if (!ReadRequired(*Value, TEXT("cpp_type"), CPPType, OutError)) {
						if (OutError.SourceKind != TEXT("command_input"))
							UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError, TypePointer);
						UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, Definition);
						return nullptr;
					}
					Value->TryGetStringField(TEXT("cpp_type_object_path"), ObjectPath);
					auto* Pin = Node->FindPin(Pair.Key);
					if (!Pin || ((Pin->GetCPPType() != CPPType
						|| (Pin->GetCPPTypeObject() ? Pin->GetCPPTypeObject()->GetPathName() : FString()) != ObjectPath)
						&& !Controller.ResolveWildCardPin(Pin->GetPinPath(), CPPType, FName(*ObjectPath), false, false)))
					{
						auto Expected = MakeShared<FJsonObject>();
						Expected->SetStringField(TEXT("cpp_type"), CPPType);
						Expected->SetStringField(TEXT("cpp_type_object_path"), ObjectPath);
						TSharedPtr<FJsonValue> Actual;
						if (Pin) {
							auto Native = MakeShared<FJsonObject>();
							Native->SetStringField(TEXT("cpp_type"), Pin->GetCPPType());
							Native->SetStringField(TEXT("cpp_type_object_path"), Pin->GetCPPTypeObject() ? Pin->GetCPPTypeObject()->GetPathName() : FString());
							Actual = MakeShared<FJsonValueObject>(Native);
						}
						OutError = NativeFailure(*Node, TEXT("rigvm_template_type_resolution_failed"),
							Pin ? TEXT("The native controller could not resolve the template pin to the requested type.")
								: TEXT("The created template node has no pin with the requested name."),
							TEXT("resolved_types/") + Pair.Key, Actual, MakeShared<FJsonValueObject>(Expected));
						return nullptr;
					}
				}
		}
		else {
			TArray<TSharedPtr<FJsonValue>> Allowed;
			for (const auto& Family : Families()) Allowed.Add(MakeShared<FJsonValueString>(Family.Type));
			OutError = InputFailure(Definition, TEXT("rigvm_node_type_not_persisted"),
				TEXT("This node type has no supported persisted creation definition."), TEXT("/node_type"),
				MakeShared<FJsonValueString>(Type), MakeShared<FJsonValueArray>(MoveTemp(Allowed)));
			return nullptr;
		}
		if (!Node) {
			OutError = NativeFailure(Blueprint, TEXT("rigvm_node_create_failed"),
				FString::Printf(TEXT("The native RigVM controller could not create a '%s' node for '%s'."), *Type, *Identity),
				TEXT("node_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return nullptr;
		}
		if (!ApplyFunctionVariableMap(Controller, *Node, Definition, OutError)) return nullptr;
		return Node;
	}

	bool ValidateExistingDefinition(const UControlRigBlueprint& Blueprint, const URigVMNode& Node,
		const FJsonObject& Definition, UE::UAI::Contract::FDiagnostic& OutError)
	{
		const auto Captured = MakeShared<FJsonObject>();
		CaptureDefinition(Blueprint, Node, *Captured);
		for (const auto& Field : Captured->Values)
		{
			// Mutable defaults/remapping are applied through the native controller.
			if (Field.Key == TEXT("default_value") || Field.Key == TEXT("variable_map")) continue;
			const auto Desired = Definition.TryGetField(Field.Key);
			if (Desired && !FJsonValue::CompareEqual(*Desired, *Field.Value))
			{
				const auto Difference = UE::UAI::StateSemantic::CanonicalJson::CompareJsonValues(Field.Value, Desired, FieldPointer(Field.Key));
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("rigvm_existing_node_definition_mismatch"),
					TEXT("An existing node's creation definition cannot be changed in place."), TEXT("rigvm_input"));
				if (!Difference.bEquivalent) Difference.ApplyTo(OutError);
				else {
					OutError.ActualValue = Desired;
					OutError.ExpectedValue = Field.Value;
				}
				OutError.SourceKind = TEXT("authored_value");
				OutError.Source.InternalJsonPath = Difference.bEquivalent ? FieldPointer(Field.Key) : Difference.Path;
				OutError.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint);
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, Definition);
				return false;
			}
		}
		return true;
	}

	bool ApplyFunctionVariableMap(URigVMController& Controller, URigVMNode& Node,
		const FJsonObject& Definition, UE::UAI::Contract::FDiagnostic& OutError)
	{
		auto* Function = Cast<URigVMFunctionReferenceNode>(&Node);
		const TSharedPtr<FJsonObject>* Map = nullptr;
		if (!Function || !Definition.TryGetObjectField(TEXT("variable_map"), Map)) return true;
		TMap<FName, FName> Desired;
		for (const auto& Pair : (*Map)->Values)
		{
			FString Value;
			if (!Pair.Value || !Pair.Value->TryGetString(Value)) {
				OutError = InputFailure(Definition, TEXT("rigvm_variable_mapping_invalid"),
					TEXT("A variable mapping must name its outer variable with a string."),
					TEXT("/variable_map") + FieldPointer(Pair.Key), Pair.Value, MakeShared<FJsonValueString>(TEXT("string")));
				return false;
			}
			Desired.Add(FName(*Pair.Key), FName(*Value));
		}
		const auto Existing = Function->GetVariableMap();
		for (const auto& Pair : Existing)
			if (!Desired.Contains(Pair.Key) && !Controller.SetRemappedVariable(Function, Pair.Key, NAME_None, false))
			{
				OutError = NativeFailure(Node, TEXT("rigvm_variable_mapping_clear_failed"),
					TEXT("The native controller refused to clear the function variable mapping."),
					TEXT("variable_map/") + Pair.Key.ToString(),
					MakeShared<FJsonValueString>(Function->GetOuterVariableName(Pair.Key).ToString()), MakeShared<FJsonValueNull>());
				return false;
			}
		for (const auto& Pair : Desired)
			if (Function->GetOuterVariableName(Pair.Key) != Pair.Value && !Controller.SetRemappedVariable(Function, Pair.Key, Pair.Value, false))
			{
				OutError = NativeFailure(Node, TEXT("rigvm_variable_mapping_failed"),
					TEXT("The native controller refused to remap the function variable."),
					TEXT("variable_map/") + Pair.Key.ToString(),
					MakeShared<FJsonValueString>(Function->GetOuterVariableName(Pair.Key).ToString()),
					MakeShared<FJsonValueString>(Pair.Value.ToString()));
				return false;
			}
		return true;
	}
}
