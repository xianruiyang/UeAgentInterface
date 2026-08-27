// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Blueprint/Authoring/UeAgentBlueprintMutationModel.h"

#include "BlueprintActionDatabase.h"
#include "BlueprintEditorLibrary.h"
#include "Commands/Blueprint/UeAgentHttpServer_BlueprintOps.h"
#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputMappingContext.h"
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
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopeExit.h"
#include "Subsystems/SubsystemBlueprintLibrary.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringOperationApply/UeAgentApplyInputContract.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"

namespace UeAgentBlueprintAuthoring
{
	namespace
	{
		using EOperation = UeAgentApplyInputContract::EOperation;

		void SetError(FString& OutError, const FString& Code, const FString& Detail = FString())
		{
			OutError = Detail.IsEmpty() ? Code : Code + TEXT(":") + Detail;
		}

		bool TryGetString(
			const TSharedPtr<const FJsonObject>& Object,
			const TCHAR* Field,
			FString& OutValue)
		{
			return Object.IsValid() && Object->TryGetStringField(Field, OutValue);
		}

		bool TryGetBool(
			const TSharedPtr<const FJsonObject>& Object,
			const TCHAR* Field,
			bool& OutValue)
		{
			return Object.IsValid() && Object->TryGetBoolField(Field, OutValue);
		}

		bool TryGetNumber(
			const TSharedPtr<const FJsonObject>& Object,
			const TCHAR* Field,
			double& OutValue)
		{
			return Object.IsValid() && Object->TryGetNumberField(Field, OutValue);
		}

		bool SetPinDefaultValue(
			const UEdGraphSchema& Schema,
			UEdGraphPin& Pin,
			const FString& Value)
		{
			if (!Schema.IsPinDefaultValid(&Pin, Value, nullptr, FText::GetEmpty()).IsEmpty())
			{
				return false;
			}

			Schema.TrySetDefaultValue(Pin, Value);
			return true;
		}

		bool SetPinDefaultObject(
			const UEdGraphSchema& Schema,
			UEdGraphPin& Pin,
			UObject* Value)
		{
			if (!Schema.IsPinDefaultValid(&Pin, FString(), Value, FText::GetEmpty()).IsEmpty())
			{
				return false;
			}

			Schema.TrySetDefaultObject(Pin, Value);
			return true;
		}

		TSharedPtr<const FJsonObject> GetObject(
			const TSharedPtr<const FJsonObject>& Object,
			const TCHAR* Field)
		{
			const TSharedPtr<FJsonObject>* Value = nullptr;
			return Object.IsValid() && Object->TryGetObjectField(Field, Value) && Value
				? *Value
				: nullptr;
		}

		struct FStableMapEntry
		{
			FString Key;
			TSharedPtr<const FJsonObject> Value;
		};

		enum class EStableMapIdentity : uint8
		{
			Member,
			Component,
			Graph,
			Node,
			PinDefault,
			Edge,
		};

		bool GetObjectMap(
			const TSharedPtr<const FJsonObject>& Object,
			const TCHAR* Field,
			TArray<FStableMapEntry>& OutEntries,
			bool& bOutPresent,
			FString& OutError)
		{
			OutEntries.Reset();
			bOutPresent = Object.IsValid() && Object->HasField(Field);
			if (!bOutPresent) return true;
			const TSharedPtr<FJsonObject>* Map = nullptr;
			if (!Object->TryGetObjectField(Field, Map) || !Map || !Map->IsValid())
			{
				SetError(OutError, TEXT("blueprint_extension_object_map_invalid"), Field);
				return false;
			}
			TArray<FString> Keys;
			(*Map)->Values.GenerateKeyArray(Keys);
			Keys.Sort();
			for (const FString& Key : Keys)
			{
				const TSharedPtr<FJsonValue>* JsonValue = (*Map)->Values.Find(Key);
				const TSharedPtr<FJsonObject>* Item = nullptr;
				if (!JsonValue || !JsonValue->IsValid()
					|| !(*JsonValue)->TryGetObject(Item) || !Item || !Item->IsValid())
				{
					SetError(
						OutError,
						TEXT("blueprint_extension_map_item_invalid"),
						FString(Field) + TEXT(".") + Key);
					return false;
				}
				OutEntries.Add({ Key, *Item });
			}
			return true;
		}

		bool SetOrValidateStringIdentity(
			FJsonObject& Object,
			const TCHAR* Field,
			const FString& Expected,
			FString& OutError)
		{
			FString Existing;
			if (Object.TryGetStringField(Field, Existing))
			{
				if (!Existing.Equals(Expected, ESearchCase::IgnoreCase))
				{
					SetError(
						OutError,
						TEXT("blueprint_stable_map_identity_conflict"),
						FString(Field) + TEXT(":") + Existing + TEXT(" != ") + Expected);
					return false;
				}
				return true;
			}
			Object.SetStringField(Field, Expected);
			return true;
		}

		bool SetOrValidateGuidIdentity(
			FJsonObject& Object,
			const TArray<const TCHAR*>& Fields,
			const TCHAR* PreferredField,
			const FGuid& Expected,
			FString& OutError)
		{
			for (const TCHAR* Field : Fields)
			{
				FString Existing;
				if (!Object.TryGetStringField(Field, Existing)) continue;
				FGuid ExistingGuid;
				if (!FGuid::Parse(Existing, ExistingGuid) || ExistingGuid != Expected)
				{
					SetError(
						OutError,
						TEXT("blueprint_stable_map_identity_conflict"),
						FString(Field) + TEXT(":") + Existing);
					return false;
				}
				return true;
			}
			Object.SetStringField(PreferredField, Expected.ToString(EGuidFormats::DigitsWithHyphens));
			return true;
		}

		bool SplitStableFallbackKey(
			const FString& Key,
			FString& OutKind,
			FString& OutName,
			FString& OutOccurrence)
		{
			TArray<FString> Parts;
			Key.ParseIntoArray(Parts, TEXT(":"), false);
			if (Parts.Num() < 2 || Parts[0].IsEmpty() || Parts[1].IsEmpty()) return false;
			OutKind = Parts[0];
			OutName = Parts[1];
			OutOccurrence = Parts.Num() > 2 ? Parts[2] : FString();
			return true;
		}

		TSharedPtr<const FJsonObject> MakeStableMapSpec(
			const FStableMapEntry& Entry,
			const EStableMapIdentity IdentityKind,
			FString& OutError)
		{
			TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
			Spec->Values = Entry.Value->Values;
			if (IdentityKind == EStableMapIdentity::Edge) return Spec;

			FJsonObject* IdentityObject = Spec.Get();
			if (IdentityKind == EStableMapIdentity::Graph)
			{
				const TSharedPtr<FJsonObject>* Header = nullptr;
				if (Spec->TryGetObjectField(TEXT("graph"), Header) && Header && Header->IsValid())
				{
					TSharedPtr<FJsonObject> HeaderCopy = MakeShared<FJsonObject>();
					HeaderCopy->Values = (*Header)->Values;
					Spec->SetObjectField(TEXT("graph"), HeaderCopy);
					IdentityObject = HeaderCopy.Get();
				}
			}

			FGuid StableGuid;
			if (FGuid::Parse(Entry.Key, StableGuid) && StableGuid.IsValid())
			{
				switch (IdentityKind)
				{
				case EStableMapIdentity::Member:
					return SetOrValidateGuidIdentity(
						*IdentityObject,
						{ TEXT("guid"), TEXT("variable_guid"), TEXT("var_guid") },
						TEXT("guid"), StableGuid, OutError) ? Spec : nullptr;
				case EStableMapIdentity::Component:
					return SetOrValidateGuidIdentity(
						*IdentityObject,
						{ TEXT("guid"), TEXT("component_guid"), TEXT("variable_guid") },
						TEXT("guid"), StableGuid, OutError) ? Spec : nullptr;
				case EStableMapIdentity::Graph:
					return SetOrValidateGuidIdentity(
						*IdentityObject,
						{ TEXT("guid"), TEXT("graph_guid") },
						TEXT("guid"), StableGuid, OutError) ? Spec : nullptr;
				case EStableMapIdentity::Node:
					if (!SetOrValidateGuidIdentity(
						*IdentityObject,
						{ TEXT("guid"), TEXT("node_guid"), TEXT("ue_node_guid") },
						TEXT("guid"), StableGuid, OutError)) return nullptr;
					if (!Spec->HasField(TEXT("id"))) Spec->SetStringField(TEXT("id"), Entry.Key);
					return Spec;
				case EStableMapIdentity::PinDefault:
					return SetOrValidateGuidIdentity(
						*IdentityObject,
						{ TEXT("guid"), TEXT("pin_guid") },
						TEXT("guid"), StableGuid, OutError) ? Spec : nullptr;
				default:
					return Spec;
				}
			}

			FString Kind;
			FString Name;
			FString Occurrence;
			if (!SplitStableFallbackKey(Entry.Key, Kind, Name, Occurrence))
			{
				SetError(OutError, TEXT("blueprint_stable_map_key_invalid"), Entry.Key);
				return nullptr;
			}
			switch (IdentityKind)
			{
			case EStableMapIdentity::Member:
			case EStableMapIdentity::Component:
				return SetOrValidateStringIdentity(*IdentityObject, TEXT("name"), Name, OutError)
					? Spec : nullptr;
			case EStableMapIdentity::Graph:
				if (!SetOrValidateStringIdentity(*IdentityObject, TEXT("name"), Name, OutError)) return nullptr;
				if (!IdentityObject->HasField(TEXT("graph_kind"))
					&& !IdentityObject->HasField(TEXT("kind")))
				{
					IdentityObject->SetStringField(TEXT("graph_kind"), Kind);
				}
				return Spec;
			case EStableMapIdentity::Node:
				if (!Spec->HasField(TEXT("id"))) Spec->SetStringField(TEXT("id"), Entry.Key);
				if (!Spec->HasField(TEXT("node_type"))
					&& !Spec->HasField(TEXT("candidate")))
				{
					Spec->SetStringField(TEXT("node_type"), Kind);
				}
				return Spec;
			case EStableMapIdentity::PinDefault:
				return SetOrValidateStringIdentity(*IdentityObject, TEXT("pin_name"), Name, OutError)
					? Spec : nullptr;
			default:
				return Spec;
			}
		}

		bool ParseOperation(
			const TSharedPtr<const FJsonObject>& Object,
			const EOperation DefaultOperation,
			EOperation& OutOperation,
			FString& OutError)
		{
			OutOperation = DefaultOperation;
			FString Text;
			if (!TryGetString(Object, TEXT("operation"), Text)) return true;
			if (!UeAgentApplyInputContract::ParseOperation(Text, OutOperation)
				|| (OutOperation != EOperation::Create
					&& OutOperation != EOperation::Update
					&& OutOperation != EOperation::Replace
					&& OutOperation != EOperation::Upsert
					&& OutOperation != EOperation::Remove))
			{
				SetError(OutError, TEXT("blueprint_item_operation_invalid"), Text);
				return false;
			}
			return true;
		}

		bool ParseGuidFields(
			const TSharedPtr<const FJsonObject>& Object,
			const std::initializer_list<const TCHAR*> Fields,
			FGuid& OutGuid,
			bool& bOutHasGuid,
			FString& OutError)
		{
			OutGuid.Invalidate();
			bOutHasGuid = false;
			for (const TCHAR* Field : Fields)
			{
				if (!Object.IsValid() || !Object->HasField(Field)) continue;
				FString Text;
				if (!Object->TryGetStringField(Field, Text)
					|| !FGuid::Parse(Text.TrimStartAndEnd(), OutGuid)
					|| !OutGuid.IsValid())
				{
					SetError(OutError, TEXT("blueprint_guid_invalid"), Field);
					return false;
				}
				bOutHasGuid = true;
				return true;
			}
			return true;
		}

		bool PinTypesEqual(const FEdGraphPinType& A, const FEdGraphPinType& B)
		{
			return A.PinCategory == B.PinCategory
				&& A.PinSubCategory == B.PinSubCategory
				&& A.PinSubCategoryObject.Get() == B.PinSubCategoryObject.Get()
				&& A.ContainerType == B.ContainerType
				&& A.PinValueType.TerminalCategory == B.PinValueType.TerminalCategory
				&& A.PinValueType.TerminalSubCategory == B.PinValueType.TerminalSubCategory
				&& A.PinValueType.TerminalSubCategoryObject.Get()
					== B.PinValueType.TerminalSubCategoryObject.Get();
		}

		TSharedPtr<FJsonObject> MakePropertyPayload(
			const TSharedPtr<const FJsonObject>& Owner,
			const TCHAR* Field,
			FString& OutError)
		{
			if (!Owner.IsValid() || !Owner->HasField(Field)) return nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
			if (Owner->TryGetArrayField(Field, Properties) && Properties)
			{
				TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v2"));
				Payload->SetArrayField(TEXT("properties"), *Properties);
				return Payload;
			}
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (Owner->TryGetObjectField(Field, Object) && Object && Object->IsValid())
			{
				FString Schema;
				if ((*Object)->TryGetStringField(TEXT("schema"), Schema)
					&& Schema == TEXT("uai.uobject_properties.v2"))
				{
					return *Object;
				}

				const TArray<TSharedPtr<FJsonValue>>* NestedProperties = nullptr;
				if ((*Object)->TryGetArrayField(TEXT("properties"), NestedProperties)
					&& NestedProperties)
				{
					TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
					Payload->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v2"));
					Payload->SetArrayField(TEXT("properties"), *NestedProperties);
					return Payload;
				}
			}
			SetError(OutError, TEXT("blueprint_properties_payload_invalid"), Field);
			return nullptr;
		}

		bool ApplyProperties(
			UObject& Object,
			const TSharedPtr<FJsonObject>& Payload,
			int32& OutApplied,
			FString& OutError)
		{
			if (!Payload.IsValid()) return true;
			Object.Modify();
			UeAgentAuthoringObjectApply::FOptions ApplyOptions;
			ApplyOptions.bAllowTemplateOwners = true;
			ApplyOptions.bAllowDomainAuthorizedOwners = true;
			UeAgentAuthoringDirectApply::FResult ApplyResult;
			if (!UeAgentDetailsAuthoringService::ApplyObjectProperties(
					&Object, Payload, ApplyOptions, ApplyResult, OutError))
			{
				SetError(
					OutError,
					ApplyResult.ErrorCode.IsEmpty() ? TEXT("blueprint_properties_apply_failed") : ApplyResult.ErrorCode,
					ApplyResult.ErrorDetail);
				return false;
			}
			OutApplied += ApplyResult.PropertiesSeen;
			return true;
		}

		bool CompileBlueprint(UBlueprint& Blueprint, FString& OutError)
		{
			FCompilerResultsLog CompilerLog;
			FKismetEditorUtilities::CompileBlueprint(
				&Blueprint,
				EBlueprintCompileOptions::None,
				&CompilerLog);
			if (CompilerLog.NumErrors > 0 || Blueprint.Status == BS_Error)
			{
				SetError(
					OutError,
					TEXT("blueprint_compile_failed"),
					LexToString(CompilerLog.NumErrors));
				return false;
			}
			return true;
		}

		FBPVariableDescription* ResolveMember(
			UBlueprint& Blueprint,
			const TSharedPtr<const FJsonObject>& Spec,
			const bool bDelegate,
			FString& OutError)
		{
			FGuid Guid;
			bool bHasGuid = false;
			if (!ParseGuidFields(
				Spec,
				{ TEXT("guid"), TEXT("variable_guid"), TEXT("var_guid") },
				Guid,
				bHasGuid,
				OutError))
			{
				return nullptr;
			}

			FString Name;
			TryGetString(Spec, TEXT("name"), Name);
			FBPVariableDescription* GuidMatch = nullptr;
			TArray<FBPVariableDescription*> NameMatches;
			for (FBPVariableDescription& Variable : Blueprint.NewVariables)
			{
				const bool bCurrentDelegate = Variable.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate;
				if (bCurrentDelegate != bDelegate) continue;
				if (bHasGuid && Variable.VarGuid == Guid)
				{
					if (GuidMatch)
					{
						SetError(OutError, TEXT("blueprint_member_guid_conflict"), Guid.ToString());
						return nullptr;
					}
					GuidMatch = &Variable;
				}
				if (!Name.IsEmpty() && Variable.VarName.ToString().Equals(Name, ESearchCase::IgnoreCase))
				{
					NameMatches.Add(&Variable);
				}
			}
			if (NameMatches.Num() > 1)
			{
				SetError(OutError, TEXT("blueprint_member_name_conflict"), Name);
				return nullptr;
			}
			if (GuidMatch && NameMatches.Num() == 1 && GuidMatch != NameMatches[0])
			{
				SetError(OutError, TEXT("blueprint_member_identity_conflict"), Name);
				return nullptr;
			}
			return GuidMatch ? GuidMatch : (NameMatches.Num() == 1 ? NameMatches[0] : nullptr);
		}

		bool ApplyMemberMap(
			UBlueprint& Blueprint,
			const TArray<FStableMapEntry>& Entries,
			const bool bDelegate,
			FMutationCounters& OutCounters,
			bool& bOutNeedsCompile,
			TArray<TSharedPtr<FJsonValue>>& OutDefaultProperties,
			FString& OutError)
		{
			for (const FStableMapEntry& Entry : Entries)
			{
				const TSharedPtr<const FJsonObject> Spec = MakeStableMapSpec(
					Entry, EStableMapIdentity::Member, OutError);
				if (!Spec.IsValid()) return false;
				EOperation Operation;
				if (!ParseOperation(Spec, EOperation::Upsert, Operation, OutError)) return false;
				FString Name;
				TryGetString(Spec, TEXT("name"), Name);
				FBPVariableDescription* Existing = ResolveMember(Blueprint, Spec, bDelegate, OutError);
				if (!OutError.IsEmpty()) return false;

				if (Operation == EOperation::Remove)
				{
					if (!Existing) continue;
					const FName ExistingName = Existing->VarName;
					FBlueprintEditorUtils::RemoveMemberVariable(&Blueprint, ExistingName);
					if (bDelegate) FBlueprintEditorUtils::ConformDelegateSignatureGraphs(&Blueprint);
					bDelegate ? ++OutCounters.DelegatesRemoved : ++OutCounters.VariablesRemoved;
					bOutNeedsCompile = true;
					continue;
				}

				if (Name.TrimStartAndEnd().IsEmpty())
				{
					SetError(OutError, TEXT("blueprint_member_name_missing"), Entry.Key);
					return false;
				}
				if (Operation == EOperation::Create && Existing)
				{
					SetError(OutError, TEXT("blueprint_member_already_exists"), Name);
					return false;
				}
				if (Operation == EOperation::Update && !Existing)
				{
					SetError(OutError, TEXT("blueprint_member_not_found"), Name);
					return false;
				}

				FEdGraphPinType DesiredType;
				bool bHasType = false;
				if (bDelegate)
				{
					DesiredType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
					bHasType = true;
				}
				else if (const TSharedPtr<const FJsonObject> Type = GetObject(Spec, TEXT("type")))
				{
					if (!UeAgentBlueprintOps::BuildPinTypeFromJsonObject(
						ConstCastSharedPtr<FJsonObject>(Type), DesiredType, OutError))
					{
						return false;
					}
					bHasType = true;
				}

				if (!Existing)
				{
					if (!bHasType)
					{
						SetError(OutError, TEXT("blueprint_variable_type_missing"), Name);
						return false;
					}
					if (!FBlueprintEditorUtils::AddMemberVariable(&Blueprint, FName(*Name), DesiredType, FString()))
					{
						SetError(OutError, TEXT("blueprint_member_add_failed"), Name);
						return false;
					}
					const int32 AddedIndex = FBlueprintEditorUtils::FindNewVariableIndex(&Blueprint, FName(*Name));
					if (AddedIndex == INDEX_NONE)
					{
						SetError(OutError, TEXT("blueprint_member_add_readback_failed"), Name);
						return false;
					}
					FGuid DesiredGuid;
					bool bHasDesiredGuid = false;
					if (!ParseGuidFields(
						Spec,
						{ TEXT("guid"), TEXT("variable_guid"), TEXT("var_guid") },
						DesiredGuid,
						bHasDesiredGuid,
						OutError))
					{
						return false;
					}
					if (bHasDesiredGuid) Blueprint.NewVariables[AddedIndex].VarGuid = DesiredGuid;
					Existing = &Blueprint.NewVariables[AddedIndex];
					if (bDelegate)
					{
						FBlueprintEditorUtils::ConformDelegateSignatureGraphs(&Blueprint);
						++OutCounters.DelegatesAdded;
					}
					else
					{
						++OutCounters.VariablesAdded;
					}
					bOutNeedsCompile = true;
				}
				else
				{
					const FName ExistingName = Existing->VarName;
					FString NewName;
					if (TryGetString(Spec, TEXT("new_name"), NewName)
						&& !NewName.IsEmpty()
						&& ExistingName.ToString() != NewName)
					{
						FBlueprintEditorUtils::RenameMemberVariable(&Blueprint, ExistingName, FName(*NewName));
						Name = NewName;
						Existing = &Blueprint.NewVariables[FBlueprintEditorUtils::FindNewVariableIndex(
							&Blueprint, FName(*Name))];
						bOutNeedsCompile = true;
					}
					if (!bDelegate && bHasType && !PinTypesEqual(Existing->VarType, DesiredType))
					{
						FBlueprintEditorUtils::ChangeMemberVariableType(&Blueprint, Existing->VarName, DesiredType);
						bOutNeedsCompile = true;
					}
					bDelegate ? ++OutCounters.DelegatesUpdated : ++OutCounters.VariablesUpdated;
				}

				if (!bDelegate)
				{
					bool bInstanceEditable = false;
					if (TryGetBool(Spec, TEXT("instance_editable"), bInstanceEditable))
					{
						FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(
							&Blueprint,
							Existing->VarName,
							!bInstanceEditable);
					}
					const TSharedPtr<FJsonObject>* DefaultProperty = nullptr;
					if (Spec->TryGetObjectField(TEXT("default_property"), DefaultProperty)
						&& DefaultProperty && DefaultProperty->IsValid())
					{
						OutDefaultProperties.Add(MakeShared<FJsonValueObject>(*DefaultProperty));
						bOutNeedsCompile = true;
					}
				}
			}
			return true;
		}

		bool ApplyMembers(
			UBlueprint& Blueprint,
			const TSharedPtr<const FJsonObject>& Members,
			FMutationCounters& OutCounters,
			bool& bOutNeedsCompile,
			TArray<TSharedPtr<FJsonValue>>& OutDefaultProperties,
			FString& OutError)
		{
			if (!Members.IsValid()) return true;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Members->Values)
			{
				if (Field.Key != TEXT("variables") && Field.Key != TEXT("delegates"))
				{
					SetError(OutError, TEXT("blueprint_members_unknown_field"), Field.Key);
					return false;
				}
			}
			TArray<FStableMapEntry> Variables;
			bool bVariablesPresent = false;
			if (!GetObjectMap(Members, TEXT("variables"), Variables, bVariablesPresent, OutError)) return false;
			if (bVariablesPresent
				&& !ApplyMemberMap(
					Blueprint,
					Variables,
					false,
					OutCounters,
					bOutNeedsCompile,
					OutDefaultProperties,
					OutError))
			{
				return false;
			}
			TArray<FStableMapEntry> Delegates;
			bool bDelegatesPresent = false;
			if (!GetObjectMap(Members, TEXT("delegates"), Delegates, bDelegatesPresent, OutError)) return false;
			return !bDelegatesPresent || ApplyMemberMap(
				Blueprint,
				Delegates,
				true,
				OutCounters,
				bOutNeedsCompile,
				OutDefaultProperties,
				OutError);
		}

		bool ApplyInterfaces(
			UBlueprint& Blueprint,
			const TArray<FStableMapEntry>& Entries,
			FMutationCounters& OutCounters,
			bool& bOutNeedsCompile,
			FString& OutError)
		{
			for (const FStableMapEntry& Entry : Entries)
			{
				const TSharedPtr<const FJsonObject>& Spec = Entry.Value;
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Spec->Values)
				{
					if (Field.Key != TEXT("class_path")
						&& Field.Key != TEXT("operation"))
					{
						SetError(OutError, TEXT("blueprint_interface_unknown_field"),
							Entry.Key + TEXT(".") + Field.Key);
						return false;
					}
				}
				FString ClassPath;
				if (!TryGetString(Spec, TEXT("class_path"), ClassPath)
					|| ClassPath.TrimStartAndEnd().IsEmpty())
				{
					SetError(OutError, TEXT("blueprint_interface_class_missing"), Entry.Key);
					return false;
				}
				UClass* InterfaceClass = UeAgentBlueprintOps::ResolveClassByPath(ClassPath);
				if (!InterfaceClass || !InterfaceClass->HasAnyClassFlags(CLASS_Interface))
				{
					SetError(OutError, TEXT("blueprint_interface_class_invalid"), ClassPath);
					return false;
				}
				const FTopLevelAssetPath InterfacePath = InterfaceClass->GetClassPathName();
				const FBPInterfaceDescription* Existing = Blueprint.ImplementedInterfaces.FindByPredicate(
					[&InterfacePath](const FBPInterfaceDescription& Description)
					{
						const UClass* ExistingClass = Description.Interface.Get();
						return ExistingClass
							&& ExistingClass->GetClassPathName() == InterfacePath;
					});
				EOperation Operation;
				if (!ParseOperation(Spec, EOperation::Upsert, Operation, OutError))
				{
					return false;
				}
				if (Operation == EOperation::Remove)
				{
					if (Existing)
					{
						FBlueprintEditorUtils::RemoveInterface(&Blueprint, InterfacePath, false);
						++OutCounters.InterfacesRemoved;
						bOutNeedsCompile = true;
					}
					continue;
				}
				if (Operation == EOperation::Create && Existing)
				{
					SetError(OutError, TEXT("blueprint_interface_already_exists"), ClassPath);
					return false;
				}
				if (Operation == EOperation::Update && !Existing)
				{
					SetError(OutError, TEXT("blueprint_interface_not_found"), ClassPath);
					return false;
				}
				if (!Existing)
				{
					if (!FBlueprintEditorUtils::ImplementNewInterface(&Blueprint, InterfacePath))
					{
						SetError(OutError, TEXT("blueprint_interface_add_failed"), ClassPath);
						return false;
					}
					++OutCounters.InterfacesAdded;
					bOutNeedsCompile = true;
				}
				else
				{
					++OutCounters.InterfacesUpdated;
				}
			}
			return true;
		}

		bool OrderComponentEntries(
			const TArray<FStableMapEntry>& Entries,
			TArray<FStableMapEntry>& OutEntries,
			FString& OutError)
		{
			struct FEntryDependency
			{
				FString Name;
				FString ParentName;
				EOperation Operation = EOperation::Upsert;
				int32 ParentIndex = INDEX_NONE;
			};

			OutEntries.Reset();
			TArray<FEntryDependency> Dependencies;
			Dependencies.Reserve(Entries.Num());
			TMap<FString, int32> EntryIndexByName;
			auto NormalizeName = [](FString Name)
			{
				Name.TrimStartAndEndInline();
				Name.ToLowerInline();
				return Name;
			};

			for (int32 Index = 0; Index < Entries.Num(); ++Index)
			{
				const TSharedPtr<const FJsonObject> Spec = MakeStableMapSpec(
					Entries[Index], EStableMapIdentity::Component, OutError);
				if (!Spec.IsValid()) return false;
				FEntryDependency& Dependency = Dependencies.AddDefaulted_GetRef();
				TryGetString(Spec, TEXT("name"), Dependency.Name);
				TryGetString(Spec, TEXT("parent"), Dependency.ParentName);
				if (!ParseOperation(Spec, EOperation::Upsert, Dependency.Operation, OutError)) return false;
				if (Dependency.Name.TrimStartAndEnd().IsEmpty())
				{
					SetError(OutError, TEXT("blueprint_component_name_missing"), Entries[Index].Key);
					return false;
				}
				const FString NormalizedName = NormalizeName(Dependency.Name);
				if (EntryIndexByName.Contains(NormalizedName))
				{
					SetError(OutError, TEXT("blueprint_component_name_conflict"), Dependency.Name);
					return false;
				}
				EntryIndexByName.Add(NormalizedName, Index);
			}

			int32 NonRemovalCount = 0;
			for (int32 Index = 0; Index < Dependencies.Num(); ++Index)
			{
				FEntryDependency& Dependency = Dependencies[Index];
				if (Dependency.Operation == EOperation::Remove) continue;
				++NonRemovalCount;
				if (Dependency.ParentName.TrimStartAndEnd().IsEmpty()) continue;
				const int32* ParentIndex = EntryIndexByName.Find(NormalizeName(Dependency.ParentName));
				if (!ParentIndex) continue;
				if (Dependencies[*ParentIndex].Operation == EOperation::Remove)
				{
					SetError(
						OutError,
						TEXT("blueprint_component_parent_removed"),
						Dependency.Name + TEXT(":") + Dependency.ParentName);
					return false;
				}
				Dependency.ParentIndex = *ParentIndex;
			}

			TArray<bool> bEmitted;
			bEmitted.Init(false, Entries.Num());
			while (OutEntries.Num() < NonRemovalCount)
			{
				bool bProgress = false;
				for (int32 Index = 0; Index < Entries.Num(); ++Index)
				{
					const FEntryDependency& Dependency = Dependencies[Index];
					if (bEmitted[Index] || Dependency.Operation == EOperation::Remove) continue;
					if (Dependency.ParentIndex != INDEX_NONE && !bEmitted[Dependency.ParentIndex]) continue;
					OutEntries.Add(Entries[Index]);
					bEmitted[Index] = true;
					bProgress = true;
				}
				if (bProgress) continue;

				TArray<FString> CycleNames;
				for (int32 Index = 0; Index < Entries.Num(); ++Index)
				{
					if (!bEmitted[Index] && Dependencies[Index].Operation != EOperation::Remove)
					{
						CycleNames.Add(Dependencies[Index].Name);
					}
				}
				SetError(
					OutError,
					TEXT("blueprint_component_parent_cycle"),
					FString::Join(CycleNames, TEXT(",")));
				return false;
			}

			for (int32 Index = 0; Index < Entries.Num(); ++Index)
			{
				if (Dependencies[Index].Operation == EOperation::Remove) OutEntries.Add(Entries[Index]);
			}
			return true;
		}

		USCS_Node* ResolveComponentNode(
			USimpleConstructionScript& SCS,
			const TSharedPtr<const FJsonObject>& Spec,
			FString& OutError)
		{
			FGuid Guid;
			bool bHasGuid = false;
			if (!ParseGuidFields(
				Spec,
				{ TEXT("guid"), TEXT("component_guid"), TEXT("variable_guid") },
				Guid,
				bHasGuid,
				OutError))
			{
				return nullptr;
			}
			FString Name;
			TryGetString(Spec, TEXT("name"), Name);
			FString ClassPath;
			TryGetString(Spec, TEXT("class"), ClassPath);
			USCS_Node* GuidMatch = nullptr;
			TArray<USCS_Node*> FallbackMatches;
			for (USCS_Node* Node : SCS.GetAllNodes())
			{
				if (!Node) continue;
				if (bHasGuid && Node->VariableGuid == Guid)
				{
					if (GuidMatch)
					{
						SetError(OutError, TEXT("blueprint_component_guid_conflict"), Guid.ToString());
						return nullptr;
					}
					GuidMatch = Node;
				}
				const bool bNameMatches = Name.IsEmpty()
					|| Node->GetVariableName().ToString().Equals(Name, ESearchCase::IgnoreCase);
				const bool bClassMatches = ClassPath.IsEmpty()
					|| (Node->ComponentClass
						&& Node->ComponentClass->GetPathName().Equals(ClassPath, ESearchCase::IgnoreCase));
				if ((!Name.IsEmpty() || !ClassPath.IsEmpty()) && bNameMatches && bClassMatches)
				{
					FallbackMatches.Add(Node);
				}
			}
			if (FallbackMatches.Num() > 1)
			{
				SetError(OutError, TEXT("blueprint_component_fallback_conflict"), Name);
				return nullptr;
			}
			if (GuidMatch && FallbackMatches.Num() == 1 && GuidMatch != FallbackMatches[0])
			{
				SetError(OutError, TEXT("blueprint_component_identity_conflict"), Name);
				return nullptr;
			}
			return GuidMatch ? GuidMatch : (FallbackMatches.Num() == 1 ? FallbackMatches[0] : nullptr);
		}

		void BuildComponentParentMap(
			USCS_Node* Node,
			USCS_Node* Parent,
			TMap<USCS_Node*, USCS_Node*>& OutParents)
		{
			if (!Node) return;
			OutParents.Add(Node, Parent);
			for (USCS_Node* Child : Node->GetChildNodes()) BuildComponentParentMap(Child, Node, OutParents);
		}

		UActorComponent* ResolveInheritedComponent(UBlueprint& Blueprint, const FString& Name)
		{
			if (Name.IsEmpty() || !Blueprint.GeneratedClass) return nullptr;
			UObject* CDO = Blueprint.GeneratedClass->GetDefaultObject(false);
			if (!CDO) return nullptr;
			if (FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(Blueprint.GeneratedClass, FName(*Name)))
			{
				return Cast<UActorComponent>(Property->GetObjectPropertyValue_InContainer(CDO));
			}
			return nullptr;
		}

		bool ApplyComponents(
			UBlueprint& Blueprint,
			const TArray<FStableMapEntry>& Entries,
			FMutationCounters& OutCounters,
			bool& bOutNeedsCompile,
			FString& OutError)
		{
			USimpleConstructionScript* SCS = Blueprint.SimpleConstructionScript;
			if (!SCS && !Entries.IsEmpty())
			{
				SetError(OutError, TEXT("blueprint_has_no_scs"));
				return false;
			}
			if (!SCS) return true;

			TArray<FStableMapEntry> OrderedEntries;
			if (!OrderComponentEntries(Entries, OrderedEntries, OutError)) return false;

			for (const FStableMapEntry& Entry : OrderedEntries)
			{
				const TSharedPtr<const FJsonObject> Spec = MakeStableMapSpec(
					Entry, EStableMapIdentity::Component, OutError);
				if (!Spec.IsValid()) return false;
				EOperation Operation;
				if (!ParseOperation(Spec, EOperation::Upsert, Operation, OutError)) return false;
				FString Name;
				FString ClassPath;
				FString ParentName;
				TryGetString(Spec, TEXT("name"), Name);
				TryGetString(Spec, TEXT("class"), ClassPath);
				TryGetString(Spec, TEXT("parent"), ParentName);
				const TSharedPtr<const FJsonObject> CandidateSpec =
					GetObject(Spec, TEXT("candidate"));
				FString CandidateName;
				FString CandidateId;
				FString LegacyBinding;
				TryGetString(CandidateSpec, TEXT("name"), CandidateName);
				TryGetString(CandidateSpec, TEXT("id"), CandidateId);
				if (CandidateId.IsEmpty())
				{
					TryGetString(CandidateSpec, TEXT("candidate_id"), CandidateId);
				}
				TryGetString(
					CandidateSpec, TEXT("binding_revision"), LegacyBinding);
				const bool bNativeCandidate = CandidateSpec.IsValid()
					&& LegacyBinding.IsEmpty()
					&& (!CandidateName.IsEmpty() || !CandidateId.IsEmpty());
				USCS_Node* Existing = ResolveComponentNode(*SCS, Spec, OutError);
				if (!OutError.IsEmpty()) return false;

				if (Operation == EOperation::Remove)
				{
					if (!Existing) continue;
					if (Existing->GetVariableName() == TEXT("DefaultSceneRoot"))
					{
						SetError(OutError, TEXT("blueprint_default_scene_root_remove_forbidden"));
						return false;
					}
					SCS->RemoveNodeAndPromoteChildren(Existing);
					++OutCounters.ComponentsRemoved;
					bOutNeedsCompile = true;
					continue;
				}

				if (Name.TrimStartAndEnd().IsEmpty())
				{
					SetError(OutError, TEXT("blueprint_component_name_missing"), Entry.Key);
					return false;
				}
				if (Operation == EOperation::Create && Existing)
				{
					SetError(OutError, TEXT("blueprint_component_already_exists"), Name);
					return false;
				}
				if (Operation == EOperation::Update && !Existing)
				{
					if (UActorComponent* Inherited = ResolveInheritedComponent(Blueprint, Name))
					{
						TSharedPtr<FJsonObject> Payload = MakePropertyPayload(Spec, TEXT("properties"), OutError);
						if (!OutError.IsEmpty()) return false;
						int32 Applied = 0;
						if (!ApplyProperties(*Inherited, Payload, Applied, OutError)) return false;
						OutCounters.ComponentsUpdated += Applied > 0 ? 1 : 0;
						continue;
					}
					SetError(OutError, TEXT("blueprint_component_not_found"), Name);
					return false;
				}

				UClass* ComponentClass = nullptr;
				if (!ClassPath.IsEmpty())
				{
					ComponentClass = UeAgentBlueprintOps::ResolveClassByPath(ClassPath);
					if (!ComponentClass || !ComponentClass->IsChildOf(UActorComponent::StaticClass()))
					{
						SetError(OutError, TEXT("blueprint_component_class_invalid"), ClassPath);
						return false;
					}
				}

				TMap<USCS_Node*, USCS_Node*> ParentMap;
				for (USCS_Node* Root : SCS->GetRootNodes()) BuildComponentParentMap(Root, nullptr, ParentMap);
				USCS_Node* DesiredParent = ParentName.IsEmpty()
					? nullptr
					: UeAgentBlueprintOps::FindSCSNodeByAnyName(SCS, ParentName);
				if (!ParentName.IsEmpty() && !DesiredParent)
				{
					SetError(OutError, TEXT("blueprint_component_parent_not_found"), ParentName);
					return false;
				}
				const bool bClassConflict = Existing && ComponentClass && Existing->ComponentClass != ComponentClass;
				const bool bParentConflict = Existing && Spec->HasField(TEXT("parent"))
					&& ParentMap.FindRef(Existing) != DesiredParent;
				if ((bClassConflict || bParentConflict) && Operation != EOperation::Replace)
				{
					SetError(
						OutError,
						bClassConflict
							? TEXT("blueprint_component_class_update_requires_replace")
							: TEXT("blueprint_component_parent_update_requires_replace"),
						Name);
					return false;
				}
				if (Existing && (bClassConflict || bParentConflict))
				{
					SCS->RemoveNodeAndPromoteChildren(Existing);
					Existing = nullptr;
					++OutCounters.ComponentsRemoved;
				}
				if (!Existing)
				{
					if (!ComponentClass)
					{
						SetError(OutError, TEXT("blueprint_component_class_missing"), Name);
						return false;
					}
					if (bNativeCandidate)
					{
						UeAgentPaletteSearch::FCandidate AppliedCandidate;
						FString CandidateError;
						if (!UeAgentPaletteSearch::TryPerformBlueprintComponentCandidate(
								Blueprint,
								DesiredParent,
								CandidateName,
								CandidateId,
								Name,
								Existing,
								AppliedCandidate,
								CandidateError))
						{
							SetError(
								OutError,
								CandidateError.IsEmpty()
									? FString(TEXT("blueprint_component_native_create_failed"))
									: CandidateError,
								Name);
							return false;
						}
						if (!Existing || Existing->ComponentClass != ComponentClass)
						{
							SetError(
								OutError,
								TEXT("blueprint_component_native_result_mismatch"),
								Name);
							return false;
						}
					}
					else
					{
						Existing = SCS->CreateNode(ComponentClass, FName(*Name));
					}
					if (!Existing)
					{
						SetError(OutError, TEXT("blueprint_component_create_failed"), Name);
						return false;
					}
					FGuid DesiredGuid;
					bool bHasDesiredGuid = false;
					if (!ParseGuidFields(
						Spec,
						{ TEXT("guid"), TEXT("component_guid"), TEXT("variable_guid") },
						DesiredGuid,
						bHasDesiredGuid,
						OutError))
					{
						return false;
					}
					if (bHasDesiredGuid) Existing->VariableGuid = DesiredGuid;
					if (!bNativeCandidate)
					{
						if (DesiredParent) DesiredParent->AddChildNode(Existing);
						else SCS->AddNode(Existing);
					}
					++OutCounters.ComponentsAdded;
					bOutNeedsCompile = true;
				}
				else
				{
					++OutCounters.ComponentsUpdated;
				}

				TSharedPtr<FJsonObject> Payload = MakePropertyPayload(Spec, TEXT("properties"), OutError);
				if (!OutError.IsEmpty()) return false;
				int32 Applied = 0;
				if (Existing->ComponentTemplate
					&& !ApplyProperties(*Existing->ComponentTemplate, Payload, Applied, OutError))
				{
					return false;
				}
			}
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
			return true;
		}

		bool IsBuiltinNode(const UEdGraphNode* Node)
		{
			return Node && (Node->IsA<UK2Node_FunctionEntry>()
				|| Node->IsA<UK2Node_FunctionResult>()
				|| Node->IsA<UK2Node_Tunnel>());
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

		FString NormalizeGraphKind(FString Kind)
		{
			Kind.TrimStartAndEndInline();
			Kind.ToLowerInline();
			if (Kind == TEXT("event") || Kind == TEXT("ubergraph")) return TEXT("event_graph");
			if (Kind == TEXT("function")) return TEXT("function_graph");
			if (Kind == TEXT("macro")) return TEXT("macro_graph");
			return Kind;
		}

		FString GetGraphKind(const UBlueprint& Blueprint, const UEdGraph* Graph)
		{
			if (Blueprint.UbergraphPages.Contains(Graph)) return TEXT("event_graph");
			if (Blueprint.FunctionGraphs.Contains(Graph)) return TEXT("function_graph");
			if (Blueprint.MacroGraphs.Contains(Graph)) return TEXT("macro_graph");
			if (Blueprint.DelegateSignatureGraphs.Contains(Graph)) return TEXT("delegate_graph");
			return TEXT("graph");
		}

		UEdGraph* ResolveGraph(
			UBlueprint& Blueprint,
			const TSharedPtr<const FJsonObject>& Spec,
			FString& OutName,
			FString& OutKind,
			FString& OutError)
		{
			const TSharedPtr<const FJsonObject> Header = GetObject(Spec, TEXT("graph"));
			const TSharedPtr<const FJsonObject> Identity = Header.IsValid() ? Header : Spec;
			TryGetString(Identity, TEXT("name"), OutName);
			if (!TryGetString(Identity, TEXT("graph_kind"), OutKind))
			{
				TryGetString(Identity, TEXT("kind"), OutKind);
			}
			OutKind = NormalizeGraphKind(OutKind);

			FGuid Guid;
			bool bHasGuid = false;
			if (!ParseGuidFields(
				Identity,
				{ TEXT("guid"), TEXT("graph_guid") },
				Guid,
				bHasGuid,
				OutError))
			{
				return nullptr;
			}
			TArray<UEdGraph*> AllGraphs;
			Blueprint.GetAllGraphs(AllGraphs);
			UEdGraph* GuidMatch = nullptr;
			TArray<UEdGraph*> FallbackMatches;
			for (UEdGraph* Graph : AllGraphs)
			{
				if (!Graph) continue;
				if (bHasGuid && Graph->GraphGuid == Guid)
				{
					if (GuidMatch)
					{
						SetError(OutError, TEXT("blueprint_graph_guid_conflict"), Guid.ToString());
						return nullptr;
					}
					GuidMatch = Graph;
				}
				const bool bNameMatches = OutName.IsEmpty()
					|| Graph->GetName().Equals(OutName, ESearchCase::IgnoreCase);
				const bool bKindMatches = OutKind.IsEmpty()
					|| GetGraphKind(Blueprint, Graph) == OutKind;
				if ((!OutName.IsEmpty() || !OutKind.IsEmpty()) && bNameMatches && bKindMatches)
				{
					FallbackMatches.Add(Graph);
				}
			}
			if (FallbackMatches.Num() > 1)
			{
				SetError(OutError, TEXT("blueprint_graph_fallback_conflict"), OutName + TEXT("/") + OutKind);
				return nullptr;
			}
			if (GuidMatch && FallbackMatches.Num() == 1 && GuidMatch != FallbackMatches[0])
			{
				SetError(OutError, TEXT("blueprint_graph_identity_conflict"), OutName);
				return nullptr;
			}
			return GuidMatch ? GuidMatch : (FallbackMatches.Num() == 1 ? FallbackMatches[0] : nullptr);
		}

		UEdGraph* CreateGraph(
			UBlueprint& Blueprint,
			const FString& Name,
			const FString& Kind,
			FString& OutError)
		{
			if (Name.IsEmpty())
			{
				SetError(OutError, TEXT("blueprint_graph_name_missing"));
				return nullptr;
			}
			if (Kind == TEXT("function_graph"))
			{
				UEdGraph* Graph = UBlueprintEditorLibrary::AddFunctionGraph(&Blueprint, Name);
				if (!Graph) SetError(OutError, TEXT("blueprint_function_graph_create_failed"), Name);
				return Graph;
			}
			if (Kind == TEXT("macro_graph"))
			{
				UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
					&Blueprint,
					FName(*Name),
					UEdGraph::StaticClass(),
					UEdGraphSchema_K2::StaticClass());
				if (!Graph)
				{
					SetError(OutError, TEXT("blueprint_macro_graph_create_failed"), Name);
					return nullptr;
				}
				FBlueprintEditorUtils::AddMacroGraph(&Blueprint, Graph, true, nullptr);
				return Graph;
			}
			if (Kind == TEXT("event_graph"))
			{
				UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
					&Blueprint,
					FName(*Name),
					UEdGraph::StaticClass(),
					UEdGraphSchema_K2::StaticClass());
				if (!Graph)
				{
					SetError(OutError, TEXT("blueprint_event_graph_create_failed"), Name);
					return nullptr;
				}
				FBlueprintEditorUtils::AddUbergraphPage(&Blueprint, Graph);
				return Graph;
			}
			SetError(OutError, TEXT("blueprint_graph_kind_unsupported"), Kind);
			return nullptr;
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

		bool NodeMatchesFallback(
			const UEdGraphNode& Node,
			const TSharedPtr<const FJsonObject>& Spec,
			const FString& Kind)
		{
			if (!Kind.IsEmpty() && GetNodeKind(&Node) != Kind) return false;
			FString Expected;
			if (Kind == TEXT("event"))
			{
				return TryGetString(Spec, TEXT("event_name"), Expected)
					&& CastChecked<UK2Node_Event>(&Node)->EventReference.GetMemberName().ToString()
						.Equals(Expected, ESearchCase::IgnoreCase);
			}
			if (Kind == TEXT("custom_event"))
			{
				return TryGetString(Spec, TEXT("event_name"), Expected)
					&& CastChecked<UK2Node_CustomEvent>(&Node)->CustomFunctionName.ToString()
						.Equals(Expected, ESearchCase::IgnoreCase);
			}
			if (Kind == TEXT("call_function") || Kind == TEXT("call_parent_function"))
			{
				if (!TryGetString(Spec, TEXT("function_name"), Expected)) return false;
				const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(&Node);
				const UFunction* Function = Call ? Call->GetTargetFunction() : nullptr;
				return Function && Function->GetName().Equals(Expected, ESearchCase::IgnoreCase);
			}
			if (Kind == TEXT("variable_node"))
			{
				if (!TryGetString(Spec, TEXT("variable_name"), Expected)) return false;
				const UK2Node_Variable* Variable = CastChecked<UK2Node_Variable>(&Node);
				FString Access = TEXT("get");
				TryGetString(Spec, TEXT("access"), Access);
				return Variable->VariableReference.GetMemberName().ToString().Equals(Expected, ESearchCase::IgnoreCase)
					&& (Access.Equals(TEXT("set"), ESearchCase::IgnoreCase) == Node.IsA<UK2Node_VariableSet>());
			}
			if (Kind == TEXT("node_by_class"))
			{
				return TryGetString(Spec, TEXT("node_class"), Expected)
					&& Node.GetClass()->GetPathName().Equals(Expected, ESearchCase::IgnoreCase);
			}
			if (Kind == TEXT("dynamic_cast"))
			{
				return TryGetString(Spec, TEXT("target_class"), Expected)
					&& CastChecked<UK2Node_DynamicCast>(&Node)->TargetType
					&& CastChecked<UK2Node_DynamicCast>(&Node)->TargetType->GetPathName()
						.Equals(Expected, ESearchCase::IgnoreCase);
			}
			if (Kind == TEXT("enhanced_input_action_event"))
			{
				return TryGetString(Spec, TEXT("input_action"), Expected)
					&& CastChecked<UK2Node_EnhancedInputAction>(&Node)->InputAction
					&& CastChecked<UK2Node_EnhancedInputAction>(&Node)->InputAction->GetPathName()
						.Equals(Expected, ESearchCase::IgnoreCase);
			}
			if (Kind == TEXT("component_bound_event"))
			{
				FString ComponentName;
				FString DelegateName;
				const UK2Node_ComponentBoundEvent* Bound = CastChecked<UK2Node_ComponentBoundEvent>(&Node);
				return TryGetString(Spec, TEXT("component_name"), ComponentName)
					&& TryGetString(Spec, TEXT("delegate_property_name"), DelegateName)
					&& Bound->ComponentPropertyName.ToString().Equals(ComponentName, ESearchCase::IgnoreCase)
					&& Bound->DelegatePropertyName.ToString().Equals(DelegateName, ESearchCase::IgnoreCase);
			}
			return !Kind.IsEmpty();
		}

		UEdGraphNode* ResolveNode(
			UEdGraph& Graph,
			const TSharedPtr<const FJsonObject>& Spec,
			FString& OutKind,
			FString& OutError)
		{
			TryGetString(Spec, TEXT("node_type"), OutKind);
			OutKind.TrimStartAndEndInline();
			OutKind.ToLowerInline();
			FGuid Guid;
			bool bHasGuid = false;
			if (!ParseGuidFields(
				Spec,
				{ TEXT("guid"), TEXT("node_guid"), TEXT("ue_node_guid") },
				Guid,
				bHasGuid,
				OutError))
			{
				return nullptr;
			}
			UEdGraphNode* GuidMatch = nullptr;
			TArray<UEdGraphNode*> FallbackMatches;
			for (UEdGraphNode* Node : Graph.Nodes)
			{
				if (!Node) continue;
				if (bHasGuid && Node->NodeGuid == Guid)
				{
					if (GuidMatch)
					{
						SetError(OutError, TEXT("blueprint_node_guid_conflict"), Guid.ToString());
						return nullptr;
					}
					GuidMatch = Node;
				}
				if (!bHasGuid && !OutKind.IsEmpty()
					&& NodeMatchesFallback(*Node, Spec, OutKind))
				{
					FallbackMatches.Add(Node);
				}
			}
			if (bHasGuid)
			{
				if (GuidMatch && !OutKind.IsEmpty()
					&& OutKind != TEXT("native_action")
					&& GetNodeKind(GuidMatch) != OutKind)
				{
					SetError(OutError, TEXT("blueprint_node_kind_conflict"), OutKind);
					return nullptr;
				}
				return GuidMatch;
			}
			if (FallbackMatches.Num() > 1)
			{
				SetError(OutError, TEXT("blueprint_node_fallback_conflict"), OutKind);
				return nullptr;
			}
			return FallbackMatches.Num() == 1 ? FallbackMatches[0] : nullptr;
		}

		void SetNodePosition(UEdGraphNode& Node, const TSharedPtr<const FJsonObject>& Spec)
		{
			const TSharedPtr<const FJsonObject> Position = GetObject(Spec, TEXT("pos"));
			double X = Node.NodePosX;
			double Y = Node.NodePosY;
			TryGetNumber(Position, TEXT("x"), X);
			TryGetNumber(Position, TEXT("y"), Y);
			Node.NodePosX = FMath::RoundToInt(X);
			Node.NodePosY = FMath::RoundToInt(Y);
		}

		void InitializeNode(UEdGraph& Graph, UEdGraphNode& Node, const TSharedPtr<const FJsonObject>& Spec)
		{
			Graph.AddNode(&Node, true, false);
			Node.CreateNewGuid();
			Node.PostPlacedNewNode();
			Node.SetFlags(RF_Transactional);
			SetNodePosition(Node, Spec);
			Node.AllocateDefaultPins();
			UEdGraphSchema_K2::SetNodeMetaData(&Node, FNodeMetadata::DefaultGraphNode);
		}

		UK2Node_CallFunction* CreateCallFunctionNode(
			UEdGraph& Graph,
			UFunction& Function,
			const TSharedPtr<const FJsonObject>& Spec,
			FString& OutError)
		{
			UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(&Graph);
			if (!Node)
			{
				SetError(OutError, TEXT("blueprint_call_function_node_create_failed"), Function.GetName());
				return nullptr;
			}
			Node->SetFromFunction(&Function);
			InitializeNode(Graph, *Node, Spec);
			return Node;
		}

		UEdGraphNode* CreateNode(
			UBlueprint& Blueprint,
			UEdGraph& Graph,
			const TSharedPtr<const FJsonObject>& Spec,
			const FString& Kind,
			const bool bAllowScratchCandidateRebind,
			FString& OutError)
		{
			if (Kind == TEXT("native_action"))
			{
				FString StableNodeId;
				TryGetString(Spec, TEXT("id"), StableNodeId);
				const TSharedPtr<const FJsonObject> Candidate =
					GetObject(Spec, TEXT("candidate"));
				FString Name;
				FString Id;
				TryGetString(Candidate, TEXT("name"), Name);
				TryGetString(Candidate, TEXT("id"), Id);
				if (Id.IsEmpty())
				{
					TryGetString(Candidate, TEXT("candidate_id"), Id);
				}
				Name.TrimStartAndEndInline();
				Id.TrimStartAndEndInline();
				if (Name.IsEmpty() && Id.IsEmpty())
				{
					SetError(
						OutError,
						TEXT("blueprint_action_candidate_selector_required"));
					return nullptr;
				}

				UeAgentPaletteSearch::FSearchParams Params;
				Params.CatalogType = TEXT("blueprint_node");
				Params.AssetPath = Blueprint.GetPathName();
				Params.GraphName = Graph.GetName();
				Params.bContextSensitive = true;
				const TSharedPtr<const FJsonObject> CandidateContext =
					GetObject(Spec, TEXT("candidate_context"));
				if (Spec->HasField(TEXT("candidate_context"))
					&& !CandidateContext.IsValid())
				{
					SetError(
						OutError,
						TEXT("blueprint_surface_candidate_context_invalid"));
					return nullptr;
				}
				TryGetString(
					CandidateContext, TEXT("node_guid"), Params.NodeGuid);
				TryGetString(
					CandidateContext, TEXT("pin_guid"), Params.PinGuid);
				TryGetString(
					CandidateContext, TEXT("pin_name"), Params.PinName);
				for (const TCHAR* Field : {
					TEXT("function_name"),
					TEXT("event_name"),
					TEXT("variable_name"),
					TEXT("node_type"),
					TEXT("node_class"),
					TEXT("operation_name")})
				{
					FString Value;
					if (bAllowScratchCandidateRebind
						&& TryGetString(Candidate, Field, Value)
						&& !Value.IsEmpty())
					{
						Params.RequiredAuthoringSelectors.Add(Field, Value);
					}
				}

				double X = 0.0;
				double Y = 0.0;
				const TSharedPtr<const FJsonObject> Position =
					GetObject(Spec, TEXT("pos"));
				TryGetNumber(Position, TEXT("x"), X);
				TryGetNumber(Position, TEXT("y"), Y);
				UEdGraphNode* Node = nullptr;
				UeAgentPaletteSearch::FCandidate ResolvedCandidate;
				FString PerformError;
				if (!UeAgentPaletteSearch::TryPerformBlueprintActionCandidate(
						Params,
						Name,
						Id,
						FVector2f(
							static_cast<float>(X),
							static_cast<float>(Y)),
						Node,
						ResolvedCandidate,
						PerformError))
				{
					FString PerformCode = PerformError;
					FString PerformDetail;
					if (PerformCode.Split(
							TEXT(":"), &PerformCode, &PerformDetail))
					{
						PerformCode.TrimStartAndEndInline();
					}
					const FString PublicSelector = !Id.IsEmpty()
						? TEXT("id(") + Id + TEXT(")")
						: TEXT("name(") + Name + TEXT(")");
					const FString LocationDetail = FString::Printf(
						TEXT("node=%s,selector=%s"),
						*StableNodeId,
						*PublicSelector);
					SetError(
						OutError,
						PerformCode.IsEmpty()
							? FString(TEXT(
								"blueprint_action_candidate_create_failed"))
							: PerformCode,
						PerformDetail.IsEmpty()
							? LocationDetail
							: LocationDetail + TEXT(",cause=") + PerformDetail);
					return nullptr;
				}
				if (!Node || Node->GetGraph() != &Graph)
				{
					SetError(
						OutError,
						TEXT("blueprint_action_candidate_graph_mismatch"));
					return nullptr;
				}
				return Node;
			}
			if (Kind == TEXT("event"))
			{
				FString EventName;
				FString EventClassPath = TEXT("/Script/Engine.Actor");
				if (!TryGetString(Spec, TEXT("event_name"), EventName) || EventName.IsEmpty())
				{
					SetError(OutError, TEXT("blueprint_event_name_missing"));
					return nullptr;
				}
				TryGetString(Spec, TEXT("event_class"), EventClassPath);
				UClass* EventClass = UeAgentBlueprintOps::ResolveClassByPath(EventClassPath);
				if (!EventClass)
				{
					SetError(OutError, TEXT("blueprint_event_class_invalid"), EventClassPath);
					return nullptr;
				}
				int32 NodePosY = 0;
				UK2Node_Event* Node = FKismetEditorUtilities::AddDefaultEventNode(
					&Blueprint,
					&Graph,
					FName(*EventName),
					EventClass,
					NodePosY);
				if (!Node)
				{
					SetError(OutError, TEXT("blueprint_event_node_create_failed"), EventName);
					return nullptr;
				}
				SetNodePosition(*Node, Spec);
				return Node;
			}
			if (Kind == TEXT("custom_event"))
			{
				FString EventName;
				if (!TryGetString(Spec, TEXT("event_name"), EventName) || EventName.IsEmpty())
				{
					SetError(OutError, TEXT("blueprint_custom_event_name_missing"));
					return nullptr;
				}
				UK2Node_CustomEvent* Node = NewObject<UK2Node_CustomEvent>(&Graph);
				if (!Node)
				{
					SetError(OutError, TEXT("blueprint_custom_event_create_failed"), EventName);
					return nullptr;
				}
				Node->CustomFunctionName = FName(*EventName);
				InitializeNode(Graph, *Node, Spec);
				return Node;
			}
			if (Kind == TEXT("component_bound_event"))
			{
				FString ComponentName;
				FString DelegateName;
				FString OwnerClassPath = TEXT("/Script/Engine.PrimitiveComponent");
				if (!TryGetString(Spec, TEXT("component_name"), ComponentName)
					|| !TryGetString(Spec, TEXT("delegate_property_name"), DelegateName))
				{
					SetError(OutError, TEXT("blueprint_component_event_identity_missing"));
					return nullptr;
				}
				TryGetString(Spec, TEXT("delegate_owner_class"), OwnerClassPath);
				UClass* OwnerClass = UeAgentBlueprintOps::ResolveClassByPath(OwnerClassPath);
				if (!OwnerClass)
				{
					SetError(OutError, TEXT("blueprint_delegate_owner_class_invalid"), OwnerClassPath);
					return nullptr;
				}
				if (!Blueprint.SkeletonGeneratedClass && !CompileBlueprint(Blueprint, OutError)) return nullptr;
				FObjectProperty* ComponentProperty = FindFProperty<FObjectProperty>(
					Blueprint.SkeletonGeneratedClass,
					FName(*ComponentName));
				if (!ComponentProperty)
				{
					SetError(OutError, TEXT("blueprint_component_event_property_not_found"), ComponentName);
					return nullptr;
				}
				FKismetEditorUtilities::CreateNewBoundEventForClass(
					OwnerClass,
					FName(*DelegateName),
					&Blueprint,
					ComponentProperty);
				UK2Node_ComponentBoundEvent* Node = const_cast<UK2Node_ComponentBoundEvent*>(
					FKismetEditorUtilities::FindBoundEventForComponent(
						&Blueprint,
						FName(*DelegateName),
						FName(*ComponentName)));
				if (!Node || Node->GetGraph() != &Graph)
				{
					SetError(OutError, TEXT("blueprint_component_event_create_failed"), ComponentName);
					return nullptr;
				}
				SetNodePosition(*Node, Spec);
				return Node;
			}
			if (Kind == TEXT("enhanced_input_action_event"))
			{
				FString Path;
				if (!TryGetString(Spec, TEXT("input_action"), Path))
				{
					SetError(OutError, TEXT("blueprint_input_action_missing"));
					return nullptr;
				}
				UInputAction* Action = Cast<UInputAction>(UeAgentBlueprintOps::LoadAssetObject(Path));
				if (!Action)
				{
					SetError(OutError, TEXT("blueprint_input_action_not_found"), Path);
					return nullptr;
				}
				UK2Node_EnhancedInputAction* Node = NewObject<UK2Node_EnhancedInputAction>(&Graph);
				if (!Node)
				{
					SetError(OutError, TEXT("blueprint_input_action_node_create_failed"), Path);
					return nullptr;
				}
				Node->InputAction = Action;
				InitializeNode(Graph, *Node, Spec);
				Node->PostReconstructNode();
				return Node;
			}
			if (Kind == TEXT("enhanced_input_get_local_player_subsystem"))
			{
				UFunction* Function = USubsystemBlueprintLibrary::StaticClass()->FindFunctionByName(
					TEXT("GetLocalPlayerSubSystemFromPlayerController"));
				UK2Node_CallFunction* Node = Function
					? CreateCallFunctionNode(Graph, *Function, Spec, OutError)
					: nullptr;
				if (!Node)
				{
					if (OutError.IsEmpty()) SetError(OutError, TEXT("blueprint_subsystem_function_not_found"));
					return nullptr;
				}
				UEdGraphPin* ClassPin = UeAgentBlueprintOps::FindNodePinByName(Node, TEXT("Class"));
				if (!ClassPin || !SetPinDefaultObject(
					*Graph.GetSchema(), *ClassPin, UEnhancedInputLocalPlayerSubsystem::StaticClass()))
				{
					SetError(OutError, TEXT("blueprint_subsystem_class_pin_invalid"));
					return nullptr;
				}
				Node->ReconstructNode();
				return Node;
			}
			if (Kind == TEXT("enhanced_input_add_mapping_context"))
			{
				FString Path;
				if (!TryGetString(Spec, TEXT("mapping_context"), Path))
				{
					SetError(OutError, TEXT("blueprint_mapping_context_missing"));
					return nullptr;
				}
				UInputMappingContext* Context = Cast<UInputMappingContext>(
					UeAgentBlueprintOps::LoadAssetObject(Path));
				UFunction* Function = UEnhancedInputLocalPlayerSubsystem::StaticClass()->FindFunctionByName(
					TEXT("AddMappingContext"));
				if (!Context || !Function)
				{
					SetError(OutError, TEXT("blueprint_mapping_context_dependency_missing"), Path);
					return nullptr;
				}
				UK2Node_CallFunction* Node = CreateCallFunctionNode(Graph, *Function, Spec, OutError);
				if (!Node) return nullptr;
				UEdGraphPin* ContextPin = UeAgentBlueprintOps::FindNodePinByName(Node, TEXT("MappingContext"));
				UEdGraphPin* PriorityPin = UeAgentBlueprintOps::FindNodePinByName(Node, TEXT("Priority"));
				UEdGraphPin* OptionsPin = UeAgentBlueprintOps::FindNodePinByName(Node, TEXT("Options"));
				double Priority = 0.0;
				FString Options = TEXT("(bIgnoreAllPressedKeysUntilRelease=True,bForceImmediately=False,bNotifyUserSettings=False)");
				TryGetNumber(Spec, TEXT("priority"), Priority);
				TryGetString(Spec, TEXT("options_default_value"), Options);
				const UEdGraphSchema* Schema = Graph.GetSchema();
				if (!ContextPin || !PriorityPin || !OptionsPin || !Schema
					|| !SetPinDefaultObject(*Schema, *ContextPin, Context)
					|| !SetPinDefaultValue(*Schema, *PriorityPin, LexToString(FMath::RoundToInt(Priority)))
					|| !SetPinDefaultValue(*Schema, *OptionsPin, Options))
				{
					SetError(OutError, TEXT("blueprint_mapping_context_pin_invalid"));
					return nullptr;
				}
				Node->ReconstructNode();
				return Node;
			}
			if (Kind == TEXT("dynamic_cast"))
			{
				FString TargetClassPath;
				if (!TryGetString(Spec, TEXT("target_class"), TargetClassPath))
				{
					SetError(OutError, TEXT("blueprint_dynamic_cast_target_missing"));
					return nullptr;
				}
				UClass* TargetClass = UeAgentBlueprintOps::ResolveClassByPath(TargetClassPath);
				if (!TargetClass)
				{
					SetError(OutError, TEXT("blueprint_dynamic_cast_target_invalid"), TargetClassPath);
					return nullptr;
				}
				UK2Node_DynamicCast* Node = NewObject<UK2Node_DynamicCast>(&Graph);
				if (!Node)
				{
					SetError(OutError, TEXT("blueprint_dynamic_cast_create_failed"));
					return nullptr;
				}
				Node->TargetType = TargetClass;
				InitializeNode(Graph, *Node, Spec);
				bool bPure = true;
				TryGetBool(Spec, TEXT("pure"), bPure);
				Node->SetPurity(bPure);
				Node->PostReconstructNode();
				return Node;
			}
			if (Kind == TEXT("call_parent_function"))
			{
				FString FunctionName;
				FString OwnerPath;
				if (!TryGetString(Spec, TEXT("function_name"), FunctionName))
				{
					SetError(OutError, TEXT("blueprint_parent_function_name_missing"));
					return nullptr;
				}
				TryGetString(Spec, TEXT("function_owner_class"), OwnerPath);
				UClass* OwnerClass = OwnerPath.IsEmpty()
					? Blueprint.ParentClass.Get()
					: UeAgentBlueprintOps::ResolveClassByPath(OwnerPath);
				UFunction* Function = OwnerClass
					? OwnerClass->FindFunctionByName(FName(*FunctionName))
					: nullptr;
				if (!Function)
				{
					SetError(OutError, TEXT("blueprint_parent_function_not_found"), FunctionName);
					return nullptr;
				}
				FGraphNodeCreator<UK2Node_CallParentFunction> Creator(Graph);
				UK2Node_CallParentFunction* Node = Creator.CreateNode();
				Node->SetFlags(RF_Transactional);
				Node->SetFromFunction(Function);
				SetNodePosition(*Node, Spec);
				UEdGraphSchema_K2::SetNodeMetaData(Node, FNodeMetadata::DefaultGraphNode);
				Creator.Finalize();
				return Node;
			}
			if (Kind == TEXT("call_function"))
			{
				FString OwnerPath;
				FString FunctionName;
				if (!TryGetString(Spec, TEXT("function_owner_class"), OwnerPath)
					|| !TryGetString(Spec, TEXT("function_name"), FunctionName))
				{
					SetError(OutError, TEXT("blueprint_function_identity_missing"));
					return nullptr;
				}
				UClass* OwnerClass = UeAgentBlueprintOps::ResolveClassByPath(OwnerPath);
				UFunction* Function = OwnerClass
					? OwnerClass->FindFunctionByName(FName(*FunctionName))
					: nullptr;
				if (!Function)
				{
					SetError(OutError, TEXT("blueprint_function_not_found"), OwnerPath + TEXT(".") + FunctionName);
					return nullptr;
				}
				if (UeAgentBlueprintOps::IsUserConstructionScriptGraph(&Graph)
					&& UeAgentBlueprintOps::IsUnsafeConstructionScriptPcgCall(OwnerPath, FunctionName))
				{
					SetError(OutError, TEXT("unsafe_construction_script_pcg_generation_blocked"), FunctionName);
					return nullptr;
				}
				if (UeAgentBlueprintOps::RequiresExplicitParentOrEventNode(FunctionName))
				{
					SetError(OutError, TEXT("event_function_call_requires_explicit_parent_node"), FunctionName);
					return nullptr;
				}
				return CreateCallFunctionNode(Graph, *Function, Spec, OutError);
			}
			if (Kind == TEXT("variable_node"))
			{
				FString VariableName;
				FString Access = TEXT("get");
				FString ParentClassPath;
				if (!TryGetString(Spec, TEXT("variable_name"), VariableName))
				{
					SetError(OutError, TEXT("blueprint_variable_node_name_missing"));
					return nullptr;
				}
				TryGetString(Spec, TEXT("access"), Access);
				TryGetString(Spec, TEXT("member_parent_class"), ParentClassPath);
				const bool bSet = Access.Equals(TEXT("set"), ESearchCase::IgnoreCase);
				UK2Node_Variable* Node = bSet
					? static_cast<UK2Node_Variable*>(NewObject<UK2Node_VariableSet>(&Graph))
					: static_cast<UK2Node_Variable*>(NewObject<UK2Node_VariableGet>(&Graph));
				if (!Node)
				{
					SetError(OutError, TEXT("blueprint_variable_node_create_failed"));
					return nullptr;
				}
				if (!ParentClassPath.IsEmpty())
				{
					UClass* ParentClass = UeAgentBlueprintOps::ResolveClassByPath(ParentClassPath);
					FProperty* Property = ParentClass
						? FindFProperty<FProperty>(ParentClass, FName(*VariableName))
						: nullptr;
					if (!Property)
					{
						SetError(OutError, TEXT("blueprint_external_variable_not_found"), VariableName);
						return nullptr;
					}
					Node->SetFromProperty(Property, false, ParentClass);
				}
				else
				{
					if (FBlueprintEditorUtils::FindNewVariableIndex(&Blueprint, FName(*VariableName)) == INDEX_NONE
						&& (!Blueprint.SkeletonGeneratedClass
							|| !FindFProperty<FProperty>(Blueprint.SkeletonGeneratedClass, FName(*VariableName))))
					{
						SetError(OutError, TEXT("blueprint_variable_not_found"), VariableName);
						return nullptr;
					}
					Node->VariableReference.SetSelfMember(FName(*VariableName));
				}
				InitializeNode(Graph, *Node, Spec);
				if (UK2Node_VariableGet* GetNode = Cast<UK2Node_VariableGet>(Node))
				{
					bool bPure = true;
					TryGetBool(Spec, TEXT("pure"), bPure);
					GetNode->SetPurity(bPure);
				}
				return Node;
			}
			if (Kind == TEXT("node_by_class"))
			{
				FString ClassPath;
				if (!TryGetString(Spec, TEXT("node_class"), ClassPath))
				{
					SetError(OutError, TEXT("blueprint_node_class_missing"));
					return nullptr;
				}
				UClass* NodeClass = UeAgentBlueprintOps::ResolveClassByPath(ClassPath);
				if (!NodeClass || !NodeClass->IsChildOf(UEdGraphNode::StaticClass())
					|| NodeClass->HasAnyClassFlags(CLASS_Abstract))
				{
					SetError(OutError, TEXT("blueprint_node_class_invalid"), ClassPath);
					return nullptr;
				}
				UEdGraphNode* Node = NewObject<UEdGraphNode>(&Graph, NodeClass);
				if (!Node)
				{
					SetError(OutError, TEXT("blueprint_node_create_failed"), ClassPath);
					return nullptr;
				}
				InitializeNode(Graph, *Node, Spec);
				double DesiredOutputs = 0.0;
				if (TryGetNumber(Spec, TEXT("exec_output_pin_count"), DesiredOutputs))
				{
					if (UK2Node_ExecutionSequence* Sequence = Cast<UK2Node_ExecutionSequence>(Node))
					{
						auto CountOutputs = [Sequence]()
						{
							return Sequence->Pins.FilterByPredicate([](const UEdGraphPin* Pin)
							{
								return Pin && Pin->Direction == EGPD_Output
									&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
							}).Num();
						};
						while (CountOutputs() < FMath::RoundToInt(DesiredOutputs)) Sequence->AddInputPin();
					}
				}
				return Node;
			}
			SetError(OutError, TEXT("blueprint_node_type_unsupported"), Kind);
			return nullptr;
		}

		UEdGraphPin* ResolvePin(UEdGraphNode& Node, const FString& Name, FString& OutError)
		{
			TArray<UEdGraphPin*> Matches;
			for (UEdGraphPin* Pin : Node.Pins)
			{
				if (Pin && Pin->PinName.ToString().Equals(Name, ESearchCase::IgnoreCase)) Matches.Add(Pin);
			}
			if (Matches.Num() > 1)
			{
				SetError(OutError, TEXT("blueprint_pin_name_conflict"), Node.GetName() + TEXT(".") + Name);
				return nullptr;
			}
			if (Matches.IsEmpty())
			{
				SetError(OutError, TEXT("blueprint_pin_not_found"), Node.GetName() + TEXT(".") + Name);
				return nullptr;
			}
			return Matches[0];
		}

		bool ApplyNodeSpec(
			UBlueprint& Blueprint,
			UEdGraph& Graph,
			const TSharedPtr<const FJsonObject>& Spec,
			TMap<FString, UEdGraphNode*>& OutNodeIds,
			FMutationCounters& OutCounters,
			const bool bAllowScratchCandidateRebind,
			FString& OutError)
		{
			EOperation Operation;
			if (!ParseOperation(Spec, EOperation::Upsert, Operation, OutError)) return false;
			FString Kind;
			UEdGraphNode* Node = ResolveNode(Graph, Spec, Kind, OutError);
			if (!OutError.IsEmpty()) return false;
			FString Id;
			TryGetString(Spec, TEXT("id"), Id);

			if (Operation == EOperation::Remove)
			{
				if (Node)
				{
					if (IsBuiltinNode(Node))
					{
						SetError(OutError, TEXT("blueprint_builtin_node_remove_forbidden"), Id);
						return false;
					}
					Graph.RemoveNode(Node);
					++OutCounters.NodesRemoved;
				}
				return true;
			}
			if (Operation == EOperation::Create && Node)
			{
				SetError(OutError, TEXT("blueprint_node_already_exists"), Id);
				return false;
			}
			if (Operation == EOperation::Update && !Node)
			{
				SetError(OutError, TEXT("blueprint_node_not_found"), Id);
				return false;
			}
			if (!Node)
			{
				if (Kind.IsEmpty())
				{
					SetError(OutError, TEXT("blueprint_node_type_missing"), Id);
					return false;
				}
				Node = CreateNode(
					Blueprint,
					Graph,
					Spec,
					Kind,
					bAllowScratchCandidateRebind,
					OutError);
				if (!Node) return false;
				FGuid DesiredGuid;
				bool bHasDesiredGuid = false;
				if (!ParseGuidFields(
					Spec,
					{ TEXT("guid"), TEXT("node_guid"), TEXT("ue_node_guid") },
					DesiredGuid,
					bHasDesiredGuid,
					OutError))
				{
					return false;
				}
				if (bHasDesiredGuid) Node->NodeGuid = DesiredGuid;
				++OutCounters.NodesAdded;
			}
			else
			{
				Node->Modify();
				SetNodePosition(*Node, Spec);
				++OutCounters.NodesUpdated;
			}

			TSharedPtr<FJsonObject> Properties = MakePropertyPayload(Spec, TEXT("properties"), OutError);
			if (!OutError.IsEmpty()) return false;
			int32 AppliedProperties = 0;
			if (!ApplyProperties(*Node, Properties, AppliedProperties, OutError)) return false;
			if (AppliedProperties > 0) Node->ReconstructNode();

			TArray<FStableMapEntry> PinDefaults;
			bool bPinDefaultsPresent = false;
			if (!GetObjectMap(Spec, TEXT("pin_defaults"), PinDefaults, bPinDefaultsPresent, OutError)) return false;
			if (bPinDefaultsPresent)
			{
				const UEdGraphSchema* Schema = Graph.GetSchema();
				if (!Schema)
				{
					SetError(OutError, TEXT("blueprint_graph_schema_missing"), Graph.GetName());
					return false;
				}
				for (const FStableMapEntry& PinEntry : PinDefaults)
				{
					const TSharedPtr<const FJsonObject> PinSpec = MakeStableMapSpec(
						PinEntry, EStableMapIdentity::PinDefault, OutError);
					if (!PinSpec.IsValid()) return false;
					FString PinName;
					FString DefaultValue;
					if (!TryGetString(PinSpec, TEXT("pin_name"), PinName)
						|| !TryGetString(PinSpec, TEXT("default_value"), DefaultValue))
					{
						SetError(OutError, TEXT("blueprint_pin_default_invalid"), Id);
						return false;
					}
					UEdGraphPin* Pin = ResolvePin(*Node, PinName, OutError);
					if (!Pin || !SetPinDefaultValue(*Schema, *Pin, DefaultValue))
					{
						if (OutError.IsEmpty()) SetError(OutError, TEXT("blueprint_pin_default_rejected"), PinName);
						return false;
					}
					++OutCounters.PinDefaultsApplied;
				}
			}

			if (!Id.IsEmpty())
			{
				if (UEdGraphNode** ExistingId = OutNodeIds.Find(Id); ExistingId && *ExistingId != Node)
				{
					SetError(OutError, TEXT("blueprint_node_id_conflict"), Id);
					return false;
				}
				OutNodeIds.Add(Id, Node);
			}
			return true;
		}

		UEdGraphNode* ResolveEdgeNode(
			UEdGraph& Graph,
			const TMap<FString, UEdGraphNode*>& NodeIds,
			const FString& Reference,
			FString& OutError)
		{
			if (UEdGraphNode* const* Node = NodeIds.Find(Reference)) return *Node;
			FGuid Guid;
			if (FGuid::Parse(Reference, Guid))
			{
				for (UEdGraphNode* Node : Graph.Nodes) if (Node && Node->NodeGuid == Guid) return Node;
			}
			TArray<UEdGraphNode*> NameMatches;
			for (UEdGraphNode* Node : Graph.Nodes)
			{
				if (Node && Node->GetName().Equals(Reference, ESearchCase::IgnoreCase)) NameMatches.Add(Node);
			}
			if (NameMatches.Num() == 1) return NameMatches[0];
			SetError(
				OutError,
				NameMatches.Num() > 1 ? TEXT("blueprint_edge_node_conflict") : TEXT("blueprint_edge_node_not_found"),
				Reference);
			return nullptr;
		}

		bool ApplyEdgeSpec(
			UEdGraph& Graph,
			const TSharedPtr<const FJsonObject>& Spec,
			const TMap<FString, UEdGraphNode*>& NodeIds,
			FMutationCounters& OutCounters,
			FString& OutError)
		{
			const TSharedPtr<const FJsonObject> From = GetObject(Spec, TEXT("from"));
			const TSharedPtr<const FJsonObject> To = GetObject(Spec, TEXT("to"));
			FString FromNodeRef;
			FString FromPinName;
			FString ToNodeRef;
			FString ToPinName;
			if (!TryGetString(From, TEXT("node"), FromNodeRef)
				|| !TryGetString(From, TEXT("pin"), FromPinName)
				|| !TryGetString(To, TEXT("node"), ToNodeRef)
				|| !TryGetString(To, TEXT("pin"), ToPinName))
			{
				SetError(OutError, TEXT("blueprint_edge_invalid"), Graph.GetName());
				return false;
			}
			UEdGraphNode* FromNode = ResolveEdgeNode(Graph, NodeIds, FromNodeRef, OutError);
			if (!FromNode) return false;
			UEdGraphNode* ToNode = ResolveEdgeNode(Graph, NodeIds, ToNodeRef, OutError);
			if (!ToNode) return false;
			UEdGraphPin* FromPin = ResolvePin(*FromNode, FromPinName, OutError);
			if (!FromPin) return false;
			UEdGraphPin* ToPin = ResolvePin(*ToNode, ToPinName, OutError);
			if (!ToPin) return false;
			if (FromPin->Direction != EGPD_Output || ToPin->Direction != EGPD_Input)
			{
				SetError(OutError, TEXT("blueprint_edge_direction_invalid"), FromNodeRef + TEXT("->") + ToNodeRef);
				return false;
			}
			EOperation Operation;
			if (!ParseOperation(Spec, EOperation::Upsert, Operation, OutError)) return false;
			const bool bConnected = FromPin->LinkedTo.Contains(ToPin);
			const UEdGraphSchema* Schema = Graph.GetSchema();
			if (!Schema)
			{
				SetError(OutError, TEXT("blueprint_graph_schema_missing"), Graph.GetName());
				return false;
			}
			if (Operation == EOperation::Remove)
			{
				if (bConnected)
				{
					Schema->BreakSinglePinLink(FromPin, ToPin);
					++OutCounters.EdgesRemoved;
				}
				return true;
			}
			if (Operation == EOperation::Create && bConnected)
			{
				SetError(OutError, TEXT("blueprint_edge_already_exists"), FromNodeRef + TEXT("->") + ToNodeRef);
				return false;
			}
			if (Operation == EOperation::Update && !bConnected)
			{
				SetError(OutError, TEXT("blueprint_edge_not_found"), FromNodeRef + TEXT("->") + ToNodeRef);
				return false;
			}
			if (!bConnected)
			{
				if (!Schema->TryCreateConnection(FromPin, ToPin))
				{
					SetError(OutError, TEXT("blueprint_edge_connection_rejected"), FromNodeRef + TEXT("->") + ToNodeRef);
					return false;
				}
				++OutCounters.EdgesAdded;
			}
			return true;
		}

		bool ApplyGraphSpec(
			UBlueprint& Blueprint,
			const TSharedPtr<const FJsonObject>& Spec,
			FMutationCounters& OutCounters,
			bool& bOutNeedsCompile,
			const bool bAllowScratchCandidateRebind,
			FString& OutError)
		{
			EOperation Operation;
			if (!ParseOperation(Spec, EOperation::Upsert, Operation, OutError)) return false;
			FString Name;
			FString Kind;
			UEdGraph* Graph = ResolveGraph(Blueprint, Spec, Name, Kind, OutError);
			if (!OutError.IsEmpty()) return false;

			if (Operation == EOperation::Remove)
			{
				if (!Graph) return true;
				const FString ExistingKind = GetGraphKind(Blueprint, Graph);
				if (ExistingKind != TEXT("function_graph") && ExistingKind != TEXT("macro_graph"))
				{
					SetError(OutError, TEXT("blueprint_graph_remove_forbidden"), Graph->GetName());
					return false;
				}
				FBlueprintEditorUtils::RemoveGraph(&Blueprint, Graph);
				++OutCounters.GraphsRemoved;
				bOutNeedsCompile = true;
				return true;
			}
			if (Operation == EOperation::Create && Graph)
			{
				SetError(OutError, TEXT("blueprint_graph_already_exists"), Name);
				return false;
			}
			if (Operation == EOperation::Update && !Graph)
			{
				SetError(OutError, TEXT("blueprint_graph_not_found"), Name);
				return false;
			}
			if (!Graph)
			{
				Graph = CreateGraph(Blueprint, Name, Kind, OutError);
				if (!Graph) return false;
				const TSharedPtr<const FJsonObject> Header = GetObject(Spec, TEXT("graph"));
				FGuid DesiredGuid;
				bool bHasDesiredGuid = false;
				if (!ParseGuidFields(
					Header.IsValid() ? Header : Spec,
					{ TEXT("guid"), TEXT("graph_guid") },
					DesiredGuid,
					bHasDesiredGuid,
					OutError))
				{
					return false;
				}
				if (bHasDesiredGuid) Graph->GraphGuid = DesiredGuid;
				++OutCounters.GraphsAdded;
				bOutNeedsCompile = true;
			}
			else
			{
				++OutCounters.GraphsUpdated;
			}

			Graph->Modify();
			if (Operation == EOperation::Replace)
			{
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (Node) Node->BreakAllNodeLinks();
				}
				const TArray<UEdGraphNode*> ExistingNodes = Graph->Nodes;
				for (UEdGraphNode* Node : ExistingNodes)
				{
					if (Node && !IsBuiltinNode(Node))
					{
						Graph->RemoveNode(Node);
						++OutCounters.NodesRemoved;
					}
				}
			}

			TMap<FString, UEdGraphNode*> NodeIds;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				const FString BuiltinId = GetBuiltinNodeId(Node);
				if (!BuiltinId.IsEmpty()) NodeIds.Add(BuiltinId, Node);
			}
			TArray<FStableMapEntry> Nodes;
			bool bNodesPresent = false;
			if (!GetObjectMap(Spec, TEXT("nodes"), Nodes, bNodesPresent, OutError)) return false;
			if (bNodesPresent)
			{
				struct FPendingContextNode
				{
					TSharedPtr<const FJsonObject> Spec;
					FGuid DependencyGuid;
					FString DependencyText;
				};
				TArray<TSharedPtr<const FJsonObject>> IndependentNodes;
				TArray<FPendingContextNode> ContextNodes;
				for (const FStableMapEntry& NodeEntry : Nodes)
				{
					const TSharedPtr<const FJsonObject> NodeSpec = MakeStableMapSpec(
						NodeEntry, EStableMapIdentity::Node, OutError);
					if (!NodeSpec.IsValid())
					{
						return false;
					}
					const TSharedPtr<const FJsonObject> CandidateContext =
						GetObject(NodeSpec, TEXT("candidate_context"));
					if (NodeSpec->HasField(TEXT("candidate_context"))
						&& !CandidateContext.IsValid())
					{
						SetError(
							OutError,
							TEXT("blueprint_surface_candidate_context_invalid"),
							NodeEntry.Key);
						return false;
					}
					FString DependencyText;
					TryGetString(
						CandidateContext, TEXT("node_guid"), DependencyText);
					DependencyText.TrimStartAndEndInline();
					if (DependencyText.IsEmpty())
					{
						IndependentNodes.Add(NodeSpec);
						continue;
					}
					FGuid DependencyGuid;
					if (!FGuid::Parse(DependencyText, DependencyGuid)
						|| !DependencyGuid.IsValid())
					{
						SetError(
							OutError,
							TEXT("blueprint_surface_candidate_context_node_guid_invalid"),
							DependencyText);
						return false;
					}
					ContextNodes.Add(
						{NodeSpec, DependencyGuid, DependencyText});
				}

				for (const TSharedPtr<const FJsonObject>& NodeSpec : IndependentNodes)
				{
					if (!ApplyNodeSpec(
							Blueprint, *Graph, NodeSpec, NodeIds,
							OutCounters, bAllowScratchCandidateRebind,
							OutError))
					{
						return false;
					}
				}

				auto GraphContainsGuid = [Graph](const FGuid& Guid)
				{
					return Graph->Nodes.ContainsByPredicate(
						[&Guid](const UEdGraphNode* Node)
						{
							return Node && Node->NodeGuid == Guid;
						});
				};
				while (!ContextNodes.IsEmpty())
				{
					bool bProgress = false;
					for (int32 Index = ContextNodes.Num() - 1; Index >= 0; --Index)
					{
						if (!GraphContainsGuid(ContextNodes[Index].DependencyGuid))
						{
							continue;
						}
						if (!ApplyNodeSpec(
								Blueprint, *Graph, ContextNodes[Index].Spec,
								NodeIds, OutCounters,
								bAllowScratchCandidateRebind, OutError))
						{
							return false;
						}
						ContextNodes.RemoveAt(Index);
						bProgress = true;
					}
					if (bProgress)
					{
						continue;
					}

					TSet<FGuid> PendingProducerGuids;
					for (const FPendingContextNode& Pending : ContextNodes)
					{
						FGuid ProducerGuid;
						bool bHasProducerGuid = false;
						if (!ParseGuidFields(
								Pending.Spec,
								{TEXT("guid"), TEXT("node_guid"), TEXT("ue_node_guid")},
								ProducerGuid,
								bHasProducerGuid,
								OutError))
						{
							return false;
						}
						if (bHasProducerGuid)
						{
							PendingProducerGuids.Add(ProducerGuid);
						}
					}
					for (const FPendingContextNode& Pending : ContextNodes)
					{
						if (!PendingProducerGuids.Contains(Pending.DependencyGuid))
						{
							SetError(
								OutError,
								TEXT("blueprint_surface_candidate_context_node_missing"),
								Pending.DependencyText);
							return false;
						}
					}
					SetError(
						OutError,
						TEXT("blueprint_surface_candidate_context_dependency_cycle"),
						ContextNodes[0].DependencyText);
					return false;
				}
			}

			TArray<FStableMapEntry> Edges;
			bool bEdgesPresent = false;
			if (!GetObjectMap(Spec, TEXT("edges"), Edges, bEdgesPresent, OutError)) return false;
			if (bEdgesPresent)
			{
				for (const FStableMapEntry& EdgeEntry : Edges)
				{
					const TSharedPtr<const FJsonObject> EdgeSpec = MakeStableMapSpec(
						EdgeEntry, EStableMapIdentity::Edge, OutError);
					if (!EdgeSpec.IsValid()
						|| !ApplyEdgeSpec(*Graph, EdgeSpec, NodeIds, OutCounters, OutError))
					{
						return false;
					}
				}
			}
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
			bOutNeedsCompile = true;
			return true;
		}

		bool ApplyGraphs(
			UBlueprint& Blueprint,
			const TArray<FStableMapEntry>& Entries,
			FMutationCounters& OutCounters,
			bool& bOutNeedsCompile,
			const bool bAllowScratchCandidateRebind,
			FString& OutError)
		{
			for (const FStableMapEntry& Entry : Entries)
			{
				const TSharedPtr<const FJsonObject> Spec = MakeStableMapSpec(
					Entry, EStableMapIdentity::Graph, OutError);
				if (!Spec.IsValid()
					|| !ApplyGraphSpec(
						Blueprint, Spec, OutCounters, bOutNeedsCompile,
						bAllowScratchCandidateRebind, OutError))
				{
					return false;
				}
			}
			return true;
		}

		bool CollectDeferredDefaults(
			const TSharedPtr<const FJsonObject>& Extension,
			TArray<TSharedPtr<FJsonValue>>& OutVariableDefaults,
			TSharedPtr<FJsonObject>& OutClassDefaults,
			FString& OutError)
		{
			OutVariableDefaults.Reset();
			OutClassDefaults.Reset();
			if (!Extension.IsValid()) return true;
			const TSharedPtr<const FJsonObject> Members = GetObject(Extension, TEXT("members"));
			if (Extension->HasField(TEXT("members")) && !Members.IsValid())
			{
				SetError(OutError, TEXT("blueprint_members_invalid"));
				return false;
			}
			TArray<FStableMapEntry> Variables;
			bool bVariablesPresent = false;
			if (!GetObjectMap(Members, TEXT("variables"), Variables, bVariablesPresent, OutError)) return false;
			if (bVariablesPresent)
			{
				for (const FStableMapEntry& Entry : Variables)
				{
					const TSharedPtr<const FJsonObject> Spec = MakeStableMapSpec(
						Entry, EStableMapIdentity::Member, OutError);
					if (!Spec.IsValid()) return false;
					const TSharedPtr<FJsonObject>* DefaultProperty = nullptr;
					if (Spec->TryGetObjectField(TEXT("default_property"), DefaultProperty)
						&& DefaultProperty && DefaultProperty->IsValid())
					{
						OutVariableDefaults.Add(MakeShared<FJsonValueObject>(*DefaultProperty));
					}
				}
			}
			OutClassDefaults = MakePropertyPayload(Extension, TEXT("class_defaults"), OutError);
			return OutError.IsEmpty();
		}

		bool ApplyDeferredDefaults(
			UBlueprint& Blueprint,
			const TArray<TSharedPtr<FJsonValue>>& VariableDefaults,
			const TSharedPtr<FJsonObject>& ClassDefaults,
			FMutationCounters& OutCounters,
			FString& OutError)
		{
			if (VariableDefaults.IsEmpty() && !ClassDefaults.IsValid()) return true;
			UObject* CDO = Blueprint.GeneratedClass
				? Blueprint.GeneratedClass->GetDefaultObject(false)
				: nullptr;
			if (!CDO)
			{
				SetError(OutError, TEXT("blueprint_class_default_object_missing"));
				return false;
			}
			if (!VariableDefaults.IsEmpty())
			{
				TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v2"));
				Payload->SetArrayField(TEXT("properties"), VariableDefaults);
				if (!ApplyProperties(*CDO, Payload, OutCounters.ClassDefaultsApplied, OutError)) return false;
			}
			return !ClassDefaults.IsValid()
				|| ApplyProperties(*CDO, ClassDefaults, OutCounters.ClassDefaultsApplied, OutError);
		}

		bool ApplyDocument(
			UBlueprint& Blueprint,
			const TSharedPtr<const FJsonObject>& Extension,
			FMutationCounters& OutCounters,
			bool& bOutNeedsCompile,
			const bool bAllowScratchCandidateRebind,
			const bool bFinalizeDefaults,
			FString& OutError)
		{
			OutError.Reset();
			bOutNeedsCompile = false;
			if (!Extension.IsValid()) return true;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Extension->Values)
			{
				if (Field.Key != TEXT("parent_class")
					&& Field.Key != TEXT("compile")
					&& Field.Key != TEXT("save")
					&& Field.Key != TEXT("interfaces")
					&& Field.Key != TEXT("class_defaults")
					&& Field.Key != TEXT("members")
					&& Field.Key != TEXT("components")
					&& Field.Key != TEXT("graphs"))
				{
					SetError(OutError, TEXT("blueprint_provider_extension_unknown_field"), Field.Key);
					return false;
				}
			}

			FString ParentClassPath;
			if (TryGetString(Extension, TEXT("parent_class"), ParentClassPath))
			{
				UClass* ParentClass = UeAgentBlueprintOps::ResolveClassByPath(ParentClassPath);
				if (!ParentClass || !FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
				{
					SetError(OutError, TEXT("blueprint_parent_class_invalid"), ParentClassPath);
					return false;
				}
				if (Blueprint.ParentClass != ParentClass)
				{
					UBlueprintEditorLibrary::ReparentBlueprint(&Blueprint, ParentClass);
					bOutNeedsCompile = true;
				}
			}

			TArray<FStableMapEntry> Interfaces;
			bool bInterfacesPresent = false;
			if (!GetObjectMap(Extension, TEXT("interfaces"), Interfaces,
				bInterfacesPresent, OutError)
				|| (bInterfacesPresent && !ApplyInterfaces(
					Blueprint, Interfaces, OutCounters, bOutNeedsCompile, OutError)))
			{
				return false;
			}

			TArray<TSharedPtr<FJsonValue>> VariableDefaults;
			const TSharedPtr<const FJsonObject> Members = GetObject(Extension, TEXT("members"));
			if (Extension->HasField(TEXT("members")) && !Members.IsValid())
			{
				SetError(OutError, TEXT("blueprint_members_invalid"));
				return false;
			}
			if (!ApplyMembers(
				Blueprint,
				Members,
				OutCounters,
				bOutNeedsCompile,
				VariableDefaults,
				OutError))
			{
				return false;
			}

			TArray<FStableMapEntry> Components;
			bool bComponentsPresent = false;
			if (!GetObjectMap(Extension, TEXT("components"), Components, bComponentsPresent, OutError)) return false;
			if (bComponentsPresent
				&& !ApplyComponents(Blueprint, Components, OutCounters, bOutNeedsCompile, OutError))
			{
				return false;
			}

			TArray<FStableMapEntry> Graphs;
			bool bGraphsPresent = false;
			if (!GetObjectMap(Extension, TEXT("graphs"), Graphs, bGraphsPresent, OutError)) return false;
			if (bGraphsPresent
				&& !ApplyGraphs(
					Blueprint,
					Graphs,
					OutCounters,
					bOutNeedsCompile,
					bAllowScratchCandidateRebind,
					OutError))
			{
				return false;
			}

			TSharedPtr<FJsonObject> ClassDefaults = MakePropertyPayload(
				Extension,
				TEXT("class_defaults"),
				OutError);
			if (!OutError.IsEmpty()) return false;
			bOutNeedsCompile |= !VariableDefaults.IsEmpty() || ClassDefaults.IsValid();
			if (!bFinalizeDefaults)
			{
				FBlueprintEditorUtils::MarkBlueprintAsModified(&Blueprint);
				return true;
			}
			if (bOutNeedsCompile || !VariableDefaults.IsEmpty() || ClassDefaults.IsValid())
			{
				if (!CompileBlueprint(Blueprint, OutError)) return false;
			}
			if (!ApplyDeferredDefaults(
				Blueprint, VariableDefaults, ClassDefaults, OutCounters, OutError))
			{
				return false;
			}
			FBlueprintEditorUtils::MarkBlueprintAsModified(&Blueprint);
			return true;
		}
	}

	struct FPreparedBlueprintMutation::FImpl
	{
		TSharedPtr<const FJsonObject> Extension;
		bool bRequiresCompile = false;
	};

	void FMutationCounters::Add(const FMutationCounters& Other)
	{
		VariablesAdded += Other.VariablesAdded;
		VariablesUpdated += Other.VariablesUpdated;
		VariablesRemoved += Other.VariablesRemoved;
		DelegatesAdded += Other.DelegatesAdded;
		DelegatesUpdated += Other.DelegatesUpdated;
		DelegatesRemoved += Other.DelegatesRemoved;
		InterfacesAdded += Other.InterfacesAdded;
		InterfacesUpdated += Other.InterfacesUpdated;
		InterfacesRemoved += Other.InterfacesRemoved;
		ComponentsAdded += Other.ComponentsAdded;
		ComponentsUpdated += Other.ComponentsUpdated;
		ComponentsRemoved += Other.ComponentsRemoved;
		GraphsAdded += Other.GraphsAdded;
		GraphsUpdated += Other.GraphsUpdated;
		GraphsRemoved += Other.GraphsRemoved;
		NodesAdded += Other.NodesAdded;
		NodesUpdated += Other.NodesUpdated;
		NodesRemoved += Other.NodesRemoved;
		PinDefaultsApplied += Other.PinDefaultsApplied;
		EdgesAdded += Other.EdgesAdded;
		EdgesRemoved += Other.EdgesRemoved;
		ClassDefaultsApplied += Other.ClassDefaultsApplied;
	}

	FPreparedBlueprintMutation::FPreparedBlueprintMutation(TUniquePtr<FImpl> InImpl)
		: Impl(MoveTemp(InImpl))
	{
	}

	FPreparedBlueprintMutation::~FPreparedBlueprintMutation() = default;

	bool FPreparedBlueprintMutation::Prepare(
		UBlueprint& ScratchBlueprint,
		const TSharedPtr<const FJsonObject>& ProviderExtension,
		TUniquePtr<FPreparedBlueprintMutation>& OutMutation,
		FMutationCounters& OutExpectedCounters,
		FString& OutError)
	{
		OutMutation.Reset();
		OutExpectedCounters = FMutationCounters();
		FBlueprintActionDatabase::Get().RefreshAssetActions(&ScratchBlueprint);
		ON_SCOPE_EXIT
		{
			FBlueprintActionDatabase::Get().ClearAssetActions(&ScratchBlueprint);
		};
		TUniquePtr<FImpl> NewImpl = MakeUnique<FImpl>();
		NewImpl->Extension = ProviderExtension;
		if (!ApplyDocument(
			ScratchBlueprint,
			ProviderExtension,
			OutExpectedCounters,
			NewImpl->bRequiresCompile,
			true,
			true,
			OutError))
		{
			return false;
		}
		OutMutation = TUniquePtr<FPreparedBlueprintMutation>(
			new FPreparedBlueprintMutation(MoveTemp(NewImpl)));
		return true;
	}

	bool FPreparedBlueprintMutation::ApplyTo(
		UBlueprint& Blueprint,
		FMutationCounters& OutCounters,
		FString& OutError) const
	{
		OutCounters = FMutationCounters();
		bool bNeedsCompile = false;
		return Impl.IsValid()
			&& ApplyDocument(
				Blueprint,
				Impl->Extension,
				OutCounters,
				bNeedsCompile,
				false,
				false,
				OutError);
	}

	bool FPreparedBlueprintMutation::FinalizeAfterCompile(
		UBlueprint& Blueprint,
		FMutationCounters& OutCounters,
		FString& OutError) const
	{
		OutCounters = FMutationCounters();
		if (!Impl.IsValid())
		{
			SetError(OutError, TEXT("blueprint_prepared_mutation_missing"));
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> VariableDefaults;
		TSharedPtr<FJsonObject> ClassDefaults;
		if (!CollectDeferredDefaults(
			Impl->Extension, VariableDefaults, ClassDefaults, OutError))
		{
			return false;
		}
		return ApplyDeferredDefaults(
			Blueprint, VariableDefaults, ClassDefaults, OutCounters, OutError);
	}

	bool FPreparedBlueprintMutation::RestoreFromBackup(
		const UBlueprint& Backup,
		UBlueprint& Target,
		FString& OutError)
	{
		OutError.Reset();
		Target.Modify();
		TArray<UEdGraph*> CurrentGraphs;
		Target.GetAllGraphs(CurrentGraphs);
		for (UEdGraph* Graph : CurrentGraphs)
		{
			if (Graph)
			{
				Graph->Rename(
					nullptr,
					GetTransientPackage(),
					REN_DontCreateRedirectors | REN_NonTransactional);
			}
		}
		if (Target.SimpleConstructionScript)
		{
			Target.SimpleConstructionScript->Rename(
				nullptr,
				GetTransientPackage(),
				REN_DontCreateRedirectors | REN_NonTransactional);
		}
		Target.ParentClass = Backup.ParentClass;
		Target.ImplementedInterfaces = Backup.ImplementedInterfaces;
		Target.NewVariables = Backup.NewVariables;
		Target.UbergraphPages.Reset();
		Target.FunctionGraphs.Reset();
		Target.MacroGraphs.Reset();
		Target.DelegateSignatureGraphs.Reset();
		auto DuplicateGraphs = [&Target](const auto& Source, auto& Destination)
		{
			for (UEdGraph* Graph : Source)
			{
				if (Graph)
				{
					Destination.Add(DuplicateObject<UEdGraph>(Graph, &Target, Graph->GetFName()));
				}
			}
		};
		DuplicateGraphs(Backup.UbergraphPages, Target.UbergraphPages);
		DuplicateGraphs(Backup.FunctionGraphs, Target.FunctionGraphs);
		DuplicateGraphs(Backup.MacroGraphs, Target.MacroGraphs);
		DuplicateGraphs(Backup.DelegateSignatureGraphs, Target.DelegateSignatureGraphs);
		Target.SimpleConstructionScript = Backup.SimpleConstructionScript
			? DuplicateObject<USimpleConstructionScript>(
				Backup.SimpleConstructionScript,
				&Target,
				Backup.SimpleConstructionScript->GetFName())
			: nullptr;
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Target);
		if (!CompileBlueprint(Target, OutError)) return false;
		UObject* SourceCDO = Backup.GeneratedClass
			? Backup.GeneratedClass->GetDefaultObject(false)
			: nullptr;
		UObject* TargetCDO = Target.GeneratedClass
			? Target.GeneratedClass->GetDefaultObject(false)
			: nullptr;
		if (SourceCDO && TargetCDO)
		{
			UEngine::CopyPropertiesForUnrelatedObjects(SourceCDO, TargetCDO);
		}
		return true;
	}

	bool FPreparedBlueprintMutation::RequiresCompile() const
	{
		return Impl.IsValid() && Impl->bRequiresCompile;
	}
}
