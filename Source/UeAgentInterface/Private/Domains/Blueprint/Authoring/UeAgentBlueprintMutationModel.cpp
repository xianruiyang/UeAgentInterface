// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Blueprint/Authoring/UeAgentBlueprintMutationModel.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringIdentity.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringGraphLinks.h"
#include <algorithm>

#include "BlueprintActionDatabase.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BlueprintEditorLibrary.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringOps.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintNativeNormalization.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringState.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintGraphSignature.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphSchema_K2_Actions.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/TimelineTemplate.h"
#include "GameFramework/Actor.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "IUeAgentInterfaceMVVMModule.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallFunctionOnMember.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_EnhancedInputAction.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_MakeArray.h"
#include "K2Node_MakeContainer.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_BaseMCDelegate.h"
#include "K2Node_Tunnel.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "Subsystems/SubsystemBlueprintLibrary.h"
#include "UObject/UnrealType.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/UObjectIterator.h"
#include "WidgetBlueprint.h"
#include "Utilities/AuthoringOperationApply/UeAgentApplyInputContract.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringDirectApply.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringAssetIdentity.h"

namespace UeAgentBlueprintAuthoring
{
	namespace
	{
		using EOperation = UeAgentApplyInputContract::EOperation;

		UEdGraphNode* ResolveGraphNodeReference(UEdGraph& Graph,
			const TMap<FString, UEdGraphNode*>& NodeIds, const FString& Reference,
			const FJsonObject& Input, FDiagnostic& OutError);

		// Reuse the editor's variable-consumer discovery without its interactive prompt.
		struct FNativeBlueprintEditorUtils : FBlueprintEditorUtils
		{
			static void GetLoadedChildBlueprints(UBlueprint* Blueprint, TArray<UBlueprint*>& Out)
			{
				FBlueprintEditorUtils::GetLoadedChildBlueprints(Blueprint, Out);
				Out.RemoveAll([](const UBlueprint* Child) { return !Child || Child->HasAnyFlags(RF_Transient); });
			}
			using FBlueprintEditorUtils::GetNodesForVariable;
		};


		void LocateComponentInput(const UBlueprint& Blueprint, const FJsonObject& Spec,
			FDiagnostic& Error, const FString& Field)
		{
			if (Error.SourceKind != TEXT("native_result") || !Error.Source.SourceFile.IsEmpty())
			{
				UeAgentAuthoringDiagnostics::LocateFailure(Error, Spec, TEXT("component_apply"),
					UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), Field);
				return;
			}
			const auto NativeSource = Error.Source;
			const FString NativePointer = Error.SemanticPointer, NativePath = Error.PropertyPath;
			Error.SourceKind.Reset();
			Error.Source.InternalJsonPath = Field.IsEmpty() || !Spec.HasField(Field) ? TEXT("/")
				: TEXT("/") + Field.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
			UeAgentAuthoringDiagnostics::LocateFailure(Error, Spec, TEXT("component_apply"),
				UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), Field);
			if (Error.Source.SourceFile.IsEmpty() && Error.Source.SourceUnit.IsEmpty() && Error.SourceKind != TEXT("command_input"))
			{
				Error.Source = NativeSource;
				Error.SemanticPointer = NativePointer;
				Error.SourceKind = TEXT("native_result");
			}
			else if (Error.SourceKind != TEXT("command_input")) Error.SourceKind = TEXT("related_input");
			Error.PropertyPath = NativePath;
		}

		FDiagnostic MutationFailure(const UBlueprint& Blueprint, const TCHAR* Code,
			const FString& Reason, const FString& Stage, const FString& Candidate,
			const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			FDiagnostic Error = FDiagnostic::Failure(Code, Reason, Stage);
			Error.SourceKind = TEXT("native_result");
			Error.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint);
			Error.CandidateName = Candidate;
			Error.PropertyPath = Field;
			Error.ActualValue = MoveTemp(Actual);
			Error.ActualValuePresent = Error.ActualValue.IsValid();
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		void LocateMutationInput(FDiagnostic& Error, const FJsonObject& Input, const FString& Field,
			const FString& RelativePointer = FString())
		{
			if (!Error.Source.SourceFile.IsEmpty() || !Error.Source.SourceUnit.IsEmpty()
				|| Error.SourceKind == TEXT("command_input")) return;
			const bool bNative = Error.SourceKind == TEXT("native_result");
			const auto NativeSource = Error.Source;
			const FString NativePath = Error.PropertyPath, NativePointer = Error.SemanticPointer;
			const FString InputField = Input.HasField(Field) ? Field : FString();
			Error.SourceKind.Reset();
			Error.Source.InternalJsonPath = !RelativePointer.IsEmpty() ? RelativePointer : InputField.IsEmpty() ? TEXT("/")
				: TEXT("/") + InputField.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
			UeAgentAuthoringDiagnostics::LocateFailure(Error, Input, Error.Stage, Error.TargetPath, InputField);
			if (bNative)
			{
				if (Error.Source.SourceFile.IsEmpty() && Error.Source.SourceUnit.IsEmpty()
					&& Error.SourceKind != TEXT("command_input"))
				{
					Error.Source = NativeSource;
					Error.SemanticPointer = NativePointer;
					Error.SourceKind = TEXT("native_result");
				}
				else if (Error.SourceKind != TEXT("command_input")) Error.SourceKind = TEXT("related_input");
			}
			Error.PropertyPath = NativePath;
		}

		FDiagnostic MutationInputFailure(const UBlueprint& Blueprint, const FJsonObject& Input,
			const TCHAR* Code, const FString& Reason, const FString& Stage, const FString& Candidate,
			const FString& Field, TSharedPtr<FJsonValue> Expected)
		{
			FDiagnostic Error = MutationFailure(Blueprint, Code, Reason, Stage, Candidate, Field,
				Input.TryGetField(Field), MoveTemp(Expected));
			Error.SourceKind.Reset();
			LocateMutationInput(Error, Input, Field);
			return Error;
		}

		FDiagnostic SignatureObjectFailure(const UBlueprint& Blueprint, const FJsonObject& Owner,
			const TCHAR* Code, const FString& Reason, const FString& Candidate)
		{
			FDiagnostic Error = FDiagnostic::Failure(Code, Reason, TEXT("signature_apply"));
			Error.CandidateName = Candidate;
			Error.PropertyPath = TEXT("signature");
			Error.SemanticPointer = TEXT("/signature");
			Error.Source.InternalJsonPath = TEXT("/signature");
			Error.ActualValue = Owner.TryGetField(TEXT("signature"));
			Error.ActualValuePresent = Error.ActualValue.IsValid();
			Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("a signature object"));
			UeAgentAuthoringDiagnostics::LocateFailure(Error, Owner, TEXT("signature_apply"),
				UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), TEXT("signature"));
			return Error;
		}

		void LocateSignatureInputFailure(const UBlueprint& Blueprint, const FJsonObject& Owner, FDiagnostic& Error)
		{
			if (Error.TargetPath.IsEmpty()) Error.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint);
			if (Error.SourceKind == TEXT("native_result") || Error.SourceKind == TEXT("command_input")) return;
			if (Error.Source.SourceUnit.IsEmpty())
				UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Error, TEXT("/signature"));
			UeAgentAuthoringDiagnostics::LocateFailure(Error, Owner, TEXT("signature_apply"), Error.TargetPath, TEXT("signature"));
		}

		const FJsonObject& GraphInputHeader(const FJsonObject& Input)
		{
			const TSharedPtr<FJsonObject>* Header = nullptr;
			return Input.TryGetObjectField(TEXT("graph"), Header) && Header && Header->IsValid() ? **Header : Input;
		}

		void LocateGraphHeaderInput(const UBlueprint& Blueprint, const FJsonObject& Input,
			FDiagnostic& Error, const FString& Field)
		{
			if (Error.TargetPath.IsEmpty()) Error.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint);
			if (!Error.Source.SourceFile.IsEmpty() || !Error.Source.SourceUnit.IsEmpty()) return;
			const FJsonObject& Header = GraphInputHeader(Input);
			if (Error.SourceKind == TEXT("native_result")) LocateMutationInput(Error, Header, Field);
			else UeAgentAuthoringDiagnostics::LocateFailure(Error, Header, Error.Stage, Error.TargetPath, Field);
			if (&Header != &Input && Error.Source.SourceUnit.IsEmpty() && Error.Source.SourceFile.IsEmpty()
				&& Error.SourceKind != TEXT("native_result") && Error.SourceKind != TEXT("command_input"))
			{
				UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(Error, TEXT("/graph"));
				UeAgentAuthoringDiagnostics::LocateFailure(Error, Input, Error.Stage, Error.TargetPath, TEXT("graph"));
			}
		}

		void LocateGraphItemFailure(const UBlueprint& Blueprint, const FJsonObject& Spec, FDiagnostic& OutError,
			const TCHAR* Stage = TEXT("node_apply"))
		{
			UeAgentAuthoringDiagnostics::LocateFailure(OutError, Spec, Stage,
				UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint));
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
			const FString& Value,
			FDiagnostic& OutError)
		{
			FString ParsedValue = Value;
			TObjectPtr<UObject> ParsedObject = nullptr;
			FText ParsedText;
			if (const UEdGraphSchema_K2* K2Schema = Cast<UEdGraphSchema_K2>(&Schema))
			{
				K2Schema->GetPinDefaultValuesFromString(
					Pin.PinType,
					Pin.GetOwningNodeUnchecked(),
					Value,
					ParsedValue,
					ParsedObject,
					ParsedText,
					false);
			}
			const FString ValidationError = Schema.IsPinDefaultValid(
				&Pin, ParsedValue, ParsedObject, ParsedText);
			if (!ValidationError.IsEmpty())
			{
				OutError = FDiagnostic::Failure(TEXT("blueprint_pin_default_rejected"), ValidationError, TEXT("node_apply"));
				OutError.SourceKind = TEXT("native_result");
				OutError.CandidateName = Pin.PinName.ToString();
				OutError.PropertyPath = TEXT("default_value");
				OutError.ActualValue = MakeShared<FJsonValueString>(Value);
				OutError.ActualValuePresent = true;
				OutError.ExpectedValue = MakeShared<FJsonValueString>(FString::Printf(
					TEXT("a default value accepted by this %s pin"), *Pin.PinType.PinCategory.ToString()));
				return false;
			}

			Schema.TrySetDefaultValue(Pin, Value);
			return true;
		}

		bool SetPinDefaultObject(
			const UEdGraphSchema& Schema,
			UEdGraphPin& Pin,
			UObject* Value,
			FDiagnostic& OutError)
		{
			const FString ValidationError = Schema.IsPinDefaultValid(&Pin, FString(), Value, FText::GetEmpty());
			if (!ValidationError.IsEmpty())
			{
				OutError = FDiagnostic::Failure(TEXT("blueprint_pin_default_object_rejected"), ValidationError, TEXT("node_apply"));
				OutError.SourceKind = TEXT("native_result");
				OutError.CandidateName = Pin.PinName.ToString();
				OutError.PropertyPath = TEXT("default_object_type");
				OutError.ActualValue = MakeShared<FJsonValueString>(Value ? Value->GetClass()->GetName() : TEXT("null"));
				OutError.ActualValuePresent = true;
				OutError.ExpectedValue = MakeShared<FJsonValueString>(FString::Printf(
					TEXT("an object accepted by this %s pin"), *Pin.PinType.PinCategory.ToString()));
				OutError.InternalContext = GetPathNameSafe(Value);
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

		FDiagnostic MutationDocumentFailure(const FJsonObject& Input, const TCHAR* Code,
			const FString& Reason, const FString& Field, TSharedPtr<FJsonValue> Actual,
			TSharedPtr<FJsonValue> Expected, const FString& RelativePointer = FString())
		{
			FDiagnostic Error = FDiagnostic::Failure(Code, Reason, TEXT("mutation_input"));
			Error.PropertyPath = Field;
			Error.Source.InternalJsonPath = !RelativePointer.IsEmpty() ? RelativePointer : Field.IsEmpty() ? TEXT("/")
				: TEXT("/") + Field.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
			Error.ActualValue = MoveTemp(Actual);
			Error.ActualValuePresent = Error.ActualValue.IsValid();
			Error.ExpectedValue = MoveTemp(Expected);
			UeAgentAuthoringDiagnostics::LocateFailure(Error, Input, TEXT("mutation_input"), FString(), Field);
			return Error;
		}

		bool GetObjectMap(
			const TSharedPtr<const FJsonObject>& Object,
			const TCHAR* Field,
			TArray<FStableMapEntry>& OutEntries,
			bool& bOutPresent,
			FDiagnostic& OutError)
		{
			OutEntries.Reset();
			bOutPresent = Object.IsValid() && Object->HasField(Field);
			if (!bOutPresent) return true;
			const TSharedPtr<FJsonObject>* Map = nullptr;
			if (!Object->TryGetObjectField(Field, Map) || !Map || !Map->IsValid())
			{
				OutError = MutationDocumentFailure(*Object, TEXT("blueprint_extension_object_map_invalid"),
					TEXT("The Blueprint declaration collection must be an object map."), Field,
					Object->TryGetField(Field), MakeShared<FJsonValueString>(TEXT("an object map")));
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
					OutError = MutationDocumentFailure(**Map, TEXT("blueprint_extension_map_item_invalid"),
						TEXT("Each Blueprint declaration map entry must contain an object."), Key,
						JsonValue ? *JsonValue : TSharedPtr<FJsonValue>(),
						MakeShared<FJsonValueString>(TEXT("an object")));
					OutError.PropertyPath = Field;
					OutError.InternalContext = FString(Field) + TEXT(".") + Key;
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
			FDiagnostic& OutError)
		{
			FString Existing;
			if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(Object,
				{{Field, EJson::String}}, OutError)) return false;
			if (Object.TryGetStringField(Field, Existing))
			{
				if (!Existing.Equals(Expected, ESearchCase::IgnoreCase))
				{
					OutError = MutationDocumentFailure(Object, TEXT("blueprint_stable_map_identity_conflict"),
						TEXT("The declared name conflicts with the name encoded by its stable map key."), Field,
						MakeShared<FJsonValueString>(Existing), MakeShared<FJsonValueString>(Expected));
					return false;
				}
				return true;
			}
			Object.SetStringField(Field, Expected);
			return true;
		}

		bool ParseGuidFields(const FJsonObject& Object,
			std::initializer_list<const TCHAR*> Fields, FGuid& OutGuid,
			bool& bOutHasGuid, FDiagnostic& OutError);

		bool SetOrValidateGuidIdentity(
			FJsonObject& Object,
			const std::initializer_list<const TCHAR*> Fields,
			const TCHAR* PreferredField,
			const FGuid& Expected,
			FDiagnostic& OutError)
		{
			FGuid ExistingGuid;
			bool bPresent = false;
			if (!ParseGuidFields(Object, Fields, ExistingGuid, bPresent, OutError)) return false;
			if (bPresent)
			{
				if (ExistingGuid == Expected) return true;
				const TCHAR* InputField = PreferredField;
				for (const TCHAR* Alias : Fields)
					if (Object.HasField(Alias)) { InputField = Alias; break; }
				OutError = MutationDocumentFailure(Object, TEXT("blueprint_stable_map_identity_conflict"),
					TEXT("The declared GUID does not match the identity encoded by its stable map key."), InputField,
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.InternalContext = ExistingGuid.ToString() + TEXT(" != ") + Expected.ToString();
				return false;
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
			FDiagnostic& OutError)
		{
			bool bNestedGraph = false;
			ON_SCOPE_EXIT
			{
				if (!OutError.IsEmpty())
				{
					if (bNestedGraph && OutError.Source.SourceUnit.IsEmpty())
						UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError, TEXT("/graph"));
					UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Entry.Value,
						TEXT("mutation_input"), FString());
				}
			};
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
					bNestedGraph = true;
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
				if (IdentityKind == EStableMapIdentity::Member
					&& !Entry.Key.TrimStartAndEnd().IsEmpty())
				{
					return SetOrValidateStringIdentity(
						*IdentityObject, TEXT("name"), Entry.Key, OutError)
						? Spec : nullptr;
				}
				OutError = MutationDocumentFailure(*Entry.Value, TEXT("blueprint_stable_map_key_invalid"),
					TEXT("The stable map key must be a valid GUID or contain a non-empty kind and name separated by ':'."),
					FString(), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.PropertyPath = TEXT("stable_map_key_valid");
				OutError.InternalContext = Entry.Key;
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
				if (!Spec->HasField(TEXT("node_type")))
				{
					// All Blueprint-family document consumers share the same native
					// candidate dispatcher, even without workspace normalization.
					Spec->SetStringField(TEXT("node_type"),
						Spec->HasField(TEXT("candidate")) ? TEXT("native_action") : Kind);
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
			FDiagnostic& OutError)
		{
			OutOperation = DefaultOperation;
			FString Text;
			if (Object && !UeAgentAuthoringJsonFields::ValidateOptionalFields(*Object,
				{{TEXT("operation"), EJson::String}}, OutError)) return false;
			if (!TryGetString(Object, TEXT("operation"), Text)) return true;
			if (!UeAgentApplyInputContract::ParseOperation(Text, OutOperation)
				|| (OutOperation != EOperation::Create
					&& OutOperation != EOperation::Update
					&& OutOperation != EOperation::Replace
					&& OutOperation != EOperation::Upsert
					&& OutOperation != EOperation::Remove))
			{
				OutError = MutationDocumentFailure(*Object, TEXT("blueprint_item_operation_invalid"),
					TEXT("The Blueprint item operation is unsupported."), TEXT("operation"),
					MakeShared<FJsonValueString>(Text),
					MakeShared<FJsonValueString>(TEXT("create, update, replace, upsert or remove")));
				return false;
			}
			return true;
		}

		bool ParseGuidFields(
			const FJsonObject& Object,
			const std::initializer_list<const TCHAR*> Fields,
			FGuid& OutGuid,
			bool& bOutHasGuid,
			FDiagnostic& OutError)
		{
			OutGuid.Invalidate();
			bOutHasGuid = false;
			const TCHAR* FirstField = nullptr;
			for (const TCHAR* Field : Fields)
			{
				if (!Object.HasField(Field)) continue;
				FString Text;
				FGuid Parsed;
				const bool bString = Object.TryGetStringField(Field, Text);
				const bool bParsed = bString && FGuid::Parse(Text.TrimStartAndEnd(), Parsed);
				if (!bString || !bParsed || !Parsed.IsValid())
				{
					TSharedPtr<FJsonValue> Actual = MakeShared<FJsonValueString>(
						UeAgentJsonDiagnostics::JsonValueTypeToString(Object.TryGetField(Field)));
					TSharedPtr<FJsonValue> Expected = MakeShared<FJsonValueString>(TEXT("string"));
					if (bString)
					{
						Actual = MakeShared<FJsonValueBoolean>(false);
						Expected = MakeShared<FJsonValueBoolean>(true);
					}
					OutError = MutationDocumentFailure(Object, TEXT("blueprint_guid_invalid"),
						!bString ? TEXT("The GUID field must contain a string.")
							: !bParsed ? TEXT("The GUID field is not in a parseable GUID format.")
								: TEXT("The GUID field contains an all-zero GUID, which is not a valid identity."),
						Field, MoveTemp(Actual), MoveTemp(Expected));
					OutError.InternalContext = Text;
					return false;
				}
				if (bOutHasGuid && Parsed != OutGuid)
				{
					OutError = MutationDocumentFailure(Object, TEXT("blueprint_guid_alias_conflict"),
						FString::Printf(TEXT("The GUID fields '%s' and '%s' identify different objects."), FirstField, Field),
						Field, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					OutError.InternalContext = OutGuid.ToString() + TEXT(" != ") + Parsed.ToString();
					return false;
				}
				if (!bOutHasGuid) FirstField = Field;
				OutGuid = Parsed;
				bOutHasGuid = true;
			}
			return true;
		}

		bool ParseGuidFields(const TSharedPtr<const FJsonObject>& Object,
			const std::initializer_list<const TCHAR*> Fields, FGuid& OutGuid,
			bool& bOutHasGuid, FDiagnostic& OutError)
		{
			OutGuid.Invalidate();
			bOutHasGuid = false;
			return !Object || ParseGuidFields(*Object, Fields, OutGuid, bOutHasGuid, OutError);
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

		// Called only after the existing seven-field PinTypesEqual predicate fails.
		FDiagnostic MemberTypeDifference(const UBlueprint& Blueprint, const FString& Name,
			const FEdGraphPinType& Actual, const FEdGraphPinType& Expected)
		{
			auto Difference = [&](const FString& Field, TSharedPtr<FJsonValue> ActualValue,
				TSharedPtr<FJsonValue> ExpectedValue)
			{
				FDiagnostic Error = MutationFailure(Blueprint, TEXT("blueprint_variable_type_change_readback_failed"),
					TEXT("The Blueprint variable type differs from the requested type after the change."),
					TEXT("member_apply"), Name, TEXT("type") + Field, MoveTemp(ActualValue), MoveTemp(ExpectedValue));
				Error.SemanticPointer = TEXT("/type") + Field;
				return Error;
			};
			auto ObjectDifference = [&](const FString& Field, const UObject* ActualObject, const UObject* ExpectedObject)
			{
				const FString Target = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint);
				auto ObjectValue = [&](const UObject* Object) -> TSharedPtr<FJsonValue>
				{
					if (!Object) return MakeShared<FJsonValueNull>();
					return MakeShared<FJsonValueString>(
						UeAgentBlueprintAssetLifecycle::GetAuthoringTypeObjectPath(&Blueprint, *Object, Target));
				};
				FDiagnostic Error = Difference(Field, ObjectValue(ActualObject), ObjectValue(ExpectedObject));
				if (ActualObject && ExpectedObject && Error.ActualValue->AsString() == Error.ExpectedValue->AsString())
				{
					auto ActualFacts = MakeShared<FJsonObject>();
					ActualFacts->SetField(TEXT("authoring_type"), Error.ActualValue);
					ActualFacts->SetBoolField(TEXT("native_reference_matches_requested"), false);
					auto ExpectedFacts = MakeShared<FJsonObject>();
					ExpectedFacts->SetField(TEXT("authoring_type"), Error.ExpectedValue);
					ExpectedFacts->SetBoolField(TEXT("native_reference_matches_requested"), true);
					Error.ActualValue = MakeShared<FJsonValueObject>(ActualFacts);
					Error.ExpectedValue = MakeShared<FJsonValueObject>(ExpectedFacts);
					Error.PublicMessage = TEXT("The Blueprint variable type has a different native reference with the same authoring type path.");
				}
				Error.InternalContext = GetPathNameSafe(ActualObject) + TEXT(" -> ") + GetPathNameSafe(ExpectedObject);
				return Error;
			};
			if (Actual.PinCategory != Expected.PinCategory)
				return Difference(TEXT("/pin_category"), MakeShared<FJsonValueString>(Actual.PinCategory.ToString()),
					MakeShared<FJsonValueString>(Expected.PinCategory.ToString()));
			if (Actual.PinSubCategory != Expected.PinSubCategory)
				return Difference(TEXT("/pin_subcategory"), MakeShared<FJsonValueString>(Actual.PinSubCategory.ToString()),
					MakeShared<FJsonValueString>(Expected.PinSubCategory.ToString()));
			if (Actual.PinSubCategoryObject.Get() != Expected.PinSubCategoryObject.Get())
				return ObjectDifference(TEXT("/pin_subcategory_object"), Actual.PinSubCategoryObject.Get(), Expected.PinSubCategoryObject.Get());
			if (Actual.ContainerType != Expected.ContainerType)
				return Difference(TEXT("/container_type"), MakeShared<FJsonValueNumber>(static_cast<int32>(Actual.ContainerType)),
					MakeShared<FJsonValueNumber>(static_cast<int32>(Expected.ContainerType)));
			if (Actual.PinValueType.TerminalCategory != Expected.PinValueType.TerminalCategory)
				return Difference(TEXT("/value_type/pin_category"),
					MakeShared<FJsonValueString>(Actual.PinValueType.TerminalCategory.ToString()),
					MakeShared<FJsonValueString>(Expected.PinValueType.TerminalCategory.ToString()));
			if (Actual.PinValueType.TerminalSubCategory != Expected.PinValueType.TerminalSubCategory)
				return Difference(TEXT("/value_type/pin_subcategory"),
					MakeShared<FJsonValueString>(Actual.PinValueType.TerminalSubCategory.ToString()),
					MakeShared<FJsonValueString>(Expected.PinValueType.TerminalSubCategory.ToString()));
			return ObjectDifference(TEXT("/value_type/pin_subcategory_object"),
				Actual.PinValueType.TerminalSubCategoryObject.Get(), Expected.PinValueType.TerminalSubCategoryObject.Get());
		}

		TSharedPtr<FJsonObject> MakePropertyPayload(
			const TSharedPtr<const FJsonObject>& Owner,
			const TCHAR* Field,
			FDiagnostic& OutError)
		{
			TSharedPtr<FJsonObject> Payload;
			if (!UeAgentDetailsAuthoringService::ReadPropertyPayload(Owner, Field, Payload, OutError)) return nullptr;
			return Payload;
		}

		struct FComponentPlacement
		{
			FGuid Guid;
			FName Parent;
			FString ClassPath;
			bool operator==(const FComponentPlacement& Other) const
			{
				return Guid == Other.Guid && Parent == Other.Parent && ClassPath == Other.ClassPath;
			}
		};

		using FComponentPlacements = TMap<FName, FComponentPlacement>;

		FComponentPlacements GetLocalComponentPlacements(UBlueprint& Blueprint, USimpleConstructionScript& SCS)
		{
			FComponentPlacements Result;
			for (USCS_Node* Node : SCS.GetAllNodes())
				if (Node && Node->ComponentTemplate && Node->ComponentClass)
					Result.Add(Node->GetVariableName(), {Node->VariableGuid, Node->ParentComponentOrVariableName,
						UeAgentBlueprintAssetLifecycle::GetAuthoringTypeObjectPath(&Blueprint, *Node->ComponentClass)});
			for (USCS_Node* Parent : SCS.GetAllNodes())
				if (Parent)
					for (USCS_Node* Child : Parent->GetChildNodes())
						if (Child)
							if (auto* Placement = Result.Find(Child->GetVariableName()))
								Placement->Parent = Parent->GetVariableName();
			return Result;
		}

		struct FComponentObservation
		{
			FComponentPlacements Before, After;
			TSet<FGuid> Graphs;

			void Select(const FComponentPlacements& Initial, const FComponentPlacements& Final, const TSet<FName>& Edited)
			{
				TSet<FName> Affected = Edited;
				for (const auto& Item : Initial)
					if (const auto* Other = Final.Find(Item.Key); !Other || !(*Other == Item.Value)) Affected.Add(Item.Key);
				for (const auto& Item : Final)
					if (!Initial.Contains(Item.Key)) Affected.Add(Item.Key);
				for (FName Name : Affected)
				{
					Before.Add(Name, Initial.FindRef(Name));
					After.Add(Name, Final.FindRef(Name));
				}
			}
		};

		struct FObservedMember
		{
			FName Before, After;
			bool bDelegate = false;
		};

		struct FMemberObservation
		{
			TMap<FGuid, FObservedMember> Members;
			TSet<FGuid> ReferencingGraphs;
			bool bBoundDefaults = true;

			void IncludeReferences(UBlueprint& Blueprint, const FBPVariableDescription& Member, bool bDelegate)
			{
				TArray<UEdGraph*> Graphs;
				GatherExportGraphs(Blueprint, Graphs);
				for (const UEdGraph* Graph : Graphs)
					for (const UEdGraphNode* Node : Graph->Nodes)
						if (const auto* K2 = Cast<UK2Node>(Node)) {
							const auto* Delegate = bDelegate ? Cast<UK2Node_BaseMCDelegate>(Node) : nullptr;
							bool bOwnDelegate = false;
							if (Delegate && Delegate->GetPropertyName() == Member.VarName) {
								// The native spawner records an explicit owner class even on a local call.
								// Resolve a copy so checking membership does not fix up the node's cache.
								FMemberReference Reference = Delegate->DelegateReference;
								const auto* Property = Reference.ResolveMember<FMulticastDelegateProperty>(Blueprint.SkeletonGeneratedClass);
								const auto* Owner = Property ? UBlueprint::GetBlueprintFromClass(Property->GetOwnerClass()) : nullptr;
								bOwnDelegate = Owner && Property->GetFName() == Member.VarName
									&& UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Owner)
										== UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint);
							}
							if (K2->ReferencesVariable(Member.VarName, nullptr)
								|| (bDelegate && K2->ReferencesFunction(Member.VarName, nullptr)) || bOwnDelegate) {
								ReferencingGraphs.Add(Graph->GraphGuid);
								break;
							}
						}
			}
		};

		struct FObservedGraph {
			FName Before;
			FName After;
			bool bAssetRoot = false;
		};

		using FObservedGraphs = TMap<FGuid, FObservedGraph>;

		void AppendReferencedGraphGuids(const TSet<FGuid>& Graphs, TMap<FString, FString>& Context)
		{
			if (Graphs.IsEmpty()) return;
			TArray<FString> Guids;
			if (const FString* Existing = Context.Find(ObservedGraphGuidsContext))
				Existing->ParseIntoArray(Guids, TEXT(","), false);
			for (const FGuid& Guid : Graphs) Guids.AddUnique(Guid.ToString(EGuidFormats::DigitsWithHyphensLower));
			Guids.Sort();
			Context.Add(ObservedGraphGuidsContext, FString::Join(Guids, TEXT(",")));
		}

		void ObserveExistingGraph(FObservedGraphs& Scope, const UBlueprint& Blueprint, const UEdGraph& Graph)
		{
			if (!Scope.Contains(Graph.GraphGuid))
				Scope.Add(Graph.GraphGuid, {Graph.GetFName(), Graph.GetFName(), Graph.GetOuter() == &Blueprint});
		}

		// The domain only retains native owner identities. Decoding, recursive
		// values and diagnostics remain in the common property-intent owner.
		struct FPropertyIntents
		{
			struct FEntry
			{
				FGuid NodeGuid;
				FGuid ComponentGuid;
				FString InheritedName;
				bool bClassDefaults = false;
				TWeakObjectPtr<UEdGraphNode> PendingNode;
				UeAgentAuthoringObjectApply::FPropertyIntent Intent;
			};
			TArray<FEntry> Entries;
			// Present only during preparation of a pure CDO edit. A field with another
			// owner ends this observation scope for the entire operation.
			TOptional<TMap<FGuid, FName>> PreparingDefaultVariables;
			TOptional<FComponentObservation> Components;
			TOptional<FMemberObservation> ObservedMembers;
			TArray<UeAgentAuthoringGraphLinks::FIntent> Connections;
			TOptional<FObservedGraphs> ObservedGraphs;
			TOptional<TMap<FString, TPair<bool, bool>>> InterfacePresence;

			bool Add(UBlueprint& Blueprint, UObject& Object,
				const UeAgentAuthoringObjectApply::FPropertyIntent& Intent, FDiagnostic& Error)
			{
				FEntry Entry;
				Entry.Intent = Intent;
				if (Blueprint.GeneratedClass && Blueprint.GeneratedClass->GetDefaultObject(false) == &Object)
					Entry.bClassDefaults = true;
				else if (auto* Node = Cast<UEdGraphNode>(&Object))
				{
					Entry.NodeGuid = Node->NodeGuid;
					Entry.PendingNode = Node;
				}
				else if (auto* Component = Cast<UActorComponent>(&Object))
				{
					if (Blueprint.SimpleConstructionScript)
						for (const USCS_Node* ComponentNode : Blueprint.SimpleConstructionScript->GetAllNodes())
							if (ComponentNode && ComponentNode->ComponentTemplate == Component) Entry.ComponentGuid = ComponentNode->VariableGuid;
					if (!Entry.ComponentGuid.IsValid() && Blueprint.GeneratedClass)
					{
						TArray<UeAgentBlueprintAuthoringOps::FInheritedComponent> Inherited;
						UeAgentBlueprintAuthoringOps::GatherInheritedComponents(Blueprint, Inherited);
						for (const auto& Owner : Inherited)
							if (Owner.Component == Component) { Entry.InheritedName = Owner.Name; break; }
					}
					if (!Entry.ComponentGuid.IsValid() && Entry.InheritedName.IsEmpty())
					{
						Error = MutationFailure(Blueprint, TEXT("blueprint_property_intent_owner_missing"),
							TEXT("The component property intent could not be associated with an SCS or inherited component."),
							TEXT("property_intent_capture"), Object.GetClass()->GetName(), TEXT("component_owner_resolved"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						Error.InternalContext = Object.GetPathName();
						return false;
					}
				}
				else
				{
					Error = MutationFailure(Blueprint, TEXT("blueprint_property_intent_owner_unsupported"),
						TEXT("This object is not a supported Blueprint property-intent owner."),
						TEXT("property_intent_capture"), Object.GetClass()->GetName(), TEXT("owner_class"),
						MakeShared<FJsonValueString>(UeAgentBlueprintAssetLifecycle::GetAuthoringTypeObjectPath(&Blueprint, *Object.GetClass())),
						MakeShared<FJsonValueString>(TEXT("the Blueprint class default object, a graph node or a component")));
					Error.InternalContext = Object.GetPathName();
					return false;
				}
				Entries.Add(MoveTemp(Entry));
				return true;
			}

			bool FinalizeNodeIdentities(UBlueprint& Blueprint, FDiagnostic& Error)
			{
				for (auto& Entry : Entries)
				{
					if (Entry.PendingNode.IsValid()) Entry.NodeGuid = Entry.PendingNode->NodeGuid;
					Entry.PendingNode.Reset();
				}
				for (auto& Intent : Connections)
					if (!Intent.Freeze(Blueprint, Error)) {
						if (Error.Stage.IsEmpty()) Error.Stage = TEXT("graph_connection_capture");
						UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Error);
						Error.Reset();
					}
				return true;
			}

			bool Verify(UBlueprint& Blueprint, FDiagnostic& Error) const
			{
				TArray<UEdGraph*> Graphs;
				if (Entries.ContainsByPredicate([](const auto& Entry) { return Entry.NodeGuid.IsValid(); }))
					Blueprint.GetAllGraphs(Graphs);
				for (const auto& Entry : Entries)
				{
					UObject* Object = nullptr;
					if (Entry.bClassDefaults)
						Object = Blueprint.GeneratedClass ? Blueprint.GeneratedClass->GetDefaultObject(false) : nullptr;
					else if (Entry.NodeGuid.IsValid())
					{
						for (auto* Graph : Graphs) if (Graph)
							for (UEdGraphNode* Node : Graph->Nodes) if (Node && Node->NodeGuid == Entry.NodeGuid) Object = Node;
					}
					else if (Entry.ComponentGuid.IsValid() && Blueprint.SimpleConstructionScript)
					{
						for (auto* Node : Blueprint.SimpleConstructionScript->GetAllNodes())
							if (Node && Node->VariableGuid == Entry.ComponentGuid) Object = Node->ComponentTemplate;
					}
					else if (!Entry.InheritedName.IsEmpty())
						Object = UeAgentBlueprintAuthoringOps::ResolveInheritedComponent(Blueprint, Entry.InheritedName);
					UeAgentAuthoringObjectApply::FApplyResult Result;
					if (!Entry.Intent.VerifyObject(Object, Result))
					{ Error = Result.Diagnostic; return false; }
				}
				return true;
			}

			bool VerifyConnections(UBlueprint& Blueprint, FDiagnostic& Error) const
			{
				TArray<UEdGraph*> Graphs;
				Blueprint.GetAllGraphs(Graphs);
				for (const auto& Intent : Connections)
					if (!Intent.Verify(Blueprint, Graphs, Error)) return false;
				return true;
			}
		};

		bool ApplyProperties(
			UObject& Object,
			const TSharedPtr<FJsonObject>& Payload,
			int32& OutApplied,
			FDiagnostic& OutError,
			UBlueprint* Blueprint = nullptr, FPropertyIntents* Intents = nullptr)
		{
			if (!Payload.IsValid()) return true;
			Object.Modify();
			UeAgentAuthoringObjectApply::FOptions ApplyOptions;
			ApplyOptions.bAllowTemplateOwners = true;
			ApplyOptions.bAllowDomainAuthorizedOwners = true;
			ApplyOptions.bCompactValues = true;
			using UeAgentAuthoringDiagnostics::FScopedSourceLocations;
			const auto InputSources = FScopedSourceLocations::CaptureProjection(*Payload);
			const auto ResolveInput = [&InputSources](const FString& Pointer)
				{ return FScopedSourceLocations::ResolveProjection(InputSources, Pointer); };
			FScopedSourceLocations InputScope(Payload.Get(), ResolveInput);
			TArray<TSharedPtr<FJsonValue>> Descriptors;
			if (!UeAgentDetailsAuthoringService::DecodePropertiesDocument(Payload, Descriptors, OutError))
			{
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Payload);
				return false;
			}
			// The decoder may copy descriptors; retain their original field positions.
			auto ProjectedPayload = MakeShared<FJsonObject>();
			ProjectedPayload->SetArrayField(TEXT("properties"), Descriptors);
			FScopedSourceLocations ProjectedScope(&ProjectedPayload.Get(), ResolveInput);
			UeAgentAuthoringObjectApply::FFailure NormalizeFailure;
			if (!NormalizeNativeNodeProperties(Object, Descriptors, ApplyOptions, NormalizeFailure))
			{
				OutError = MoveTemp(NormalizeFailure.Diagnostic);
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *ProjectedPayload);
				return false;
			}
			UeAgentAuthoringDirectApply::FResult ApplyResult;
			if (!UeAgentAuthoringDirectApply::ApplyObjectPatch(
					&Object, Descriptors, ApplyOptions, ApplyResult))
			{
				OutError = MoveTemp(ApplyResult.Diagnostic);
				return false;
			}
			OutApplied += ApplyResult.PropertiesSeen;
			if (Intents && Intents->PreparingDefaultVariables.IsSet()) {
				for (const auto& Descriptor : Descriptors) {
					FString Path;
					const auto Field = Descriptor->AsObject();
					const bool bCdo = Blueprint && Blueprint->GeneratedClass && Blueprint->GeneratedClass->GetDefaultObject(false) == &Object;
					const FProperty* Property = bCdo && Field && Field->TryGetStringField(TEXT("property_path"), Path)
						? FindFProperty<FProperty>(Object.GetClass(), FName(*Path)) : nullptr;
					const int32 Index = Property && Property->GetOwnerClass() == Blueprint->GeneratedClass
						? FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, Property->GetFName()) : INDEX_NONE;
					if (Index == INDEX_NONE || !Blueprint->NewVariables[Index].VarGuid.IsValid()
						|| Blueprint->NewVariables[Index].VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate) {
						Intents->PreparingDefaultVariables.Reset();
						break;
					}
					Intents->PreparingDefaultVariables->Add(Blueprint->NewVariables[Index].VarGuid, Property->GetFName());
				}
			}
			if (Blueprint && Intents)
				for (const auto& Intent : ApplyResult.PropertyIntents)
					if (!Intents->Add(*Blueprint, Object, Intent, OutError))
					{
						LocateMutationInput(OutError, *Payload, FString());
						return false;
					}
			return true;
		}

		bool CompileBlueprint(UBlueprint& Blueprint, FDiagnostic& OutError)
		{
			UeAgentPaletteSearch::InvalidateBlueprintActionBatch();
			return UeAgentBlueprintAssetLifecycle::CompileBlueprint(&Blueprint, OutError);
		}

		FBPVariableDescription* ResolveMember(
			UBlueprint& Blueprint,
			const TSharedPtr<const FJsonObject>& Spec,
			const FJsonObject& Input,
			const bool bDelegate,
			FDiagnostic& OutError)
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
				UeAgentAuthoringDiagnostics::LocateFailure(OutError, Input, TEXT("member_resolve"),
					UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint));
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
						OutError = MutationFailure(Blueprint, TEXT("blueprint_member_guid_conflict"),
							TEXT("More than one Blueprint member matches the requested GUID."), TEXT("member_resolve"),
							Name, TEXT("observed_guid_matches"), MakeShared<FJsonValueNumber>(2), MakeShared<FJsonValueNumber>(1));
						OutError.InternalContext = Guid.ToString();
						OutError.CandidateChoices = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
							MakeShared<FJsonValueString>(GuidMatch->VarName.ToString()),
							MakeShared<FJsonValueString>(Variable.VarName.ToString())});
						LocateMutationInput(OutError, Input, Input.HasField(TEXT("guid")) ? TEXT("guid")
							: Input.HasField(TEXT("variable_guid")) ? TEXT("variable_guid") : TEXT("var_guid"));
						return nullptr;
					}
					GuidMatch = &Variable;
				}
				if (!Name.IsEmpty() && Variable.VarName.ToString().Equals(Name, ESearchCase::IgnoreCase))
				{
					NameMatches.Add(&Variable);
				}
			}
			if (!GuidMatch && NameMatches.Num() > 1)
			{
				OutError = MutationFailure(Blueprint, TEXT("blueprint_member_name_conflict"),
					TEXT("More than one Blueprint member matches the requested name."), TEXT("member_resolve"),
					Name, TEXT("name_match_count"), MakeShared<FJsonValueNumber>(NameMatches.Num()), MakeShared<FJsonValueNumber>(1));
				LocateMutationInput(OutError, Input, TEXT("name"));
				return nullptr;
			}
			if (GuidMatch && !NameMatches.IsEmpty() && !NameMatches.Contains(GuidMatch))
			{
				OutError = MutationFailure(Blueprint, TEXT("blueprint_member_identity_conflict"),
					TEXT("The requested GUID and name identify different Blueprint members."), TEXT("member_resolve"),
					Name, TEXT("name"), MakeShared<FJsonValueString>(GuidMatch->VarName.ToString()),
					MakeShared<FJsonValueString>(Name));
				OutError.InternalContext = Guid.ToString();
				LocateMutationInput(OutError, Input, TEXT("name"));
				return nullptr;
			}
			return GuidMatch ? GuidMatch : (NameMatches.Num() == 1 ? NameMatches[0] : nullptr);
		}

		bool ChangeMemberType(UBlueprint& Blueprint, FBPVariableDescription& Variable,
			const FEdGraphPinType& DesiredType, FDiagnostic& OutError)
		{
			// Bridge ChangeMemberVariableType without its interactive warning. The
			// lifecycle includes loaded children in rollback/save/readback beforehand.
			const FName Name = Variable.VarName;
			const auto* Schema = GetDefault<UEdGraphSchema_K2>();
			TArray<UBlueprint*> Children;
			FNativeBlueprintEditorUtils::GetLoadedChildBlueprints(&Blueprint, Children);
			TArray<UK2Node*> Consumers = FNativeBlueprintEditorUtils::GetNodesForVariable(Name, &Blueprint);
			for (UBlueprint* Child : Children)
				Consumers.Append(FNativeBlueprintEditorUtils::GetNodesForVariable(Name, Child));
			Blueprint.Modify();
			const UClass* Class = Cast<UClass>(DesiredType.PinSubCategoryObject.Get());
			if ((DesiredType.PinCategory == UEdGraphSchema_K2::PC_Object || DesiredType.PinCategory == UEdGraphSchema_K2::PC_Interface)
				&& Class && Class->IsChildOf(AActor::StaticClass())) Variable.PropertyFlags |= CPF_DisableEditOnTemplate;
			else Variable.PropertyFlags &= ~CPF_DisableEditOnTemplate;
			const bool bWasBoolean = Variable.VarType.PinCategory == UEdGraphSchema_K2::PC_Boolean;
			const bool bIsBoolean = DesiredType.PinCategory == UEdGraphSchema_K2::PC_Boolean;
			if (bWasBoolean != bIsBoolean) Variable.FriendlyName = FName::NameToDisplayString(Name.ToString(), bIsBoolean);
			Variable.VarType = DesiredType;
			// Match ChangeMemberVariableType: this native transition clears
			// unsupported replication settings instead of refusing the type edit.
			if (DesiredType.IsSet() || DesiredType.IsMap())
			{
				Variable.PropertyFlags &= ~(CPF_Net | CPF_RepNotify);
				Variable.RepNotifyFunc = NAME_None;
				Variable.ReplicationCondition = COND_None;
			}
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
			if (!Consumers.IsEmpty())
				for (UBlueprint* Child : Children)
					FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Child);
			for (UK2Node* Node : Consumers) Schema->ReconstructNode(*Node, true);
			// K2 owns migration, disconnection and orphan-pin semantics. Requiring
			// every pre-edit link to survive would reject the native type change.
			return true;
		}

		UEdGraph* EnsureDelegateSignatureGraph(
			UBlueprint& Blueprint,
			const FName DelegateName,
			FDiagnostic& OutError)
		{
			FBlueprintEditorUtils::ConformDelegateSignatureGraphs(&Blueprint);
			UEdGraph* Match = nullptr;
			for (UEdGraph* Graph : Blueprint.DelegateSignatureGraphs)
			{
				if (!Graph || Graph->GetFName() != DelegateName)
				{
					continue;
				}
				if (Match)
				{
					OutError = MutationFailure(Blueprint, TEXT("blueprint_delegate_signature_graph_conflict"),
						TEXT("More than one delegate signature graph has the requested name."), TEXT("delegate_signature"),
						DelegateName.ToString(), TEXT("observed_signature_graph_matches"),
						MakeShared<FJsonValueNumber>(2), MakeShared<FJsonValueNumber>(1));
					return nullptr;
				}
				Match = Graph;
			}
			if (Match)
			{
				return Match;
			}

			const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
			UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
				&Blueprint, DelegateName, UEdGraph::StaticClass(),
				UEdGraphSchema_K2::StaticClass());
			if (!Schema || !Graph)
			{
				OutError = MutationFailure(Blueprint, TEXT("blueprint_delegate_signature_graph_create_failed"),
					!Schema ? TEXT("The K2 schema is unavailable for the delegate signature graph.")
						: TEXT("Unreal did not create the requested delegate signature graph."),
					TEXT("delegate_signature"), DelegateName.ToString(),
					!Schema ? TEXT("k2_schema_available") : TEXT("signature_graph_created"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return nullptr;
			}
			Graph->bEditable = false;
			Schema->CreateDefaultNodesForGraph(*Graph);
			// A new dispatcher owns an initially empty, user-authored signature;
			// it is not cloning an existing UFunction signature.
			Schema->CreateFunctionGraphTerminators(
				*Graph, static_cast<UClass*>(nullptr));
			Schema->AddExtraFunctionFlags(
				Graph, FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public);
			Schema->MarkFunctionEntryAsEditable(Graph, true);
			Blueprint.DelegateSignatureGraphs.Add(Graph);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
			return Graph;
		}

		bool ApplyMemberMap(
			UBlueprint& Blueprint,
			const TArray<FStableMapEntry>& Entries,
			const bool bDelegate,
			FMutationCounters& OutCounters,
			bool& bOutNeedsCompile,
			TArray<TSharedPtr<FJsonValue>>& OutDefaultProperties,
			FDiagnostic& OutError, FMemberObservation* Observation)
		{
			for (const FStableMapEntry& Entry : Entries)
			{
				ON_SCOPE_EXIT
				{
					if (!OutError.IsEmpty() && OutError.Source.SourceFile.IsEmpty()
						&& OutError.Source.SourceUnit.IsEmpty() && OutError.SourceKind != TEXT("native_result")
						&& OutError.SourceKind != TEXT("command_input"))
						UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Entry.Value, TEXT("member_apply"),
							UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint));
				};
				const TSharedPtr<const FJsonObject> Spec = MakeStableMapSpec(
					Entry, EStableMapIdentity::Member, OutError);
				if (!Spec.IsValid()) return false;
				if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Spec,
					{{TEXT("name"), EJson::String}, {TEXT("type"), EJson::Object},
					 {TEXT("instance_editable"), EJson::Boolean}, {TEXT("default_property"), EJson::Object},
					 {TEXT("signature"), EJson::Object}}, OutError)) return false;
				EOperation Operation;
				if (!ParseOperation(Spec, EOperation::Upsert, Operation, OutError)) return false;
				FString Name;
				TryGetString(Spec, TEXT("name"), Name);
				auto FailMember = [&](const TCHAR* Code, const FString& Reason, const FString& Field,
					TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& InputField)
				{
					OutError = MutationFailure(Blueprint, Code, Reason, TEXT("member_apply"), Name, Field,
						MoveTemp(Actual), MoveTemp(Expected));
					LocateMutationInput(OutError, *Entry.Value, InputField);
				};
				FBPVariableDescription* Existing = ResolveMember(Blueprint, Spec, *Entry.Value, bDelegate, OutError);
				if (!OutError.IsEmpty()) return false;

				if (Operation == EOperation::Remove)
				{
					if (!Existing)
					{
						if (UeAgentBlueprintAuthoringOps::ResolveInheritedComponent(Blueprint, Name))
						{
							FailMember(TEXT("blueprint_inherited_component_structure_not_editable"),
								TEXT("The requested member is absent, and its name identifies an inherited component whose structure cannot be removed."),
								TEXT("inherited_component_exists"), MakeShared<FJsonValueBoolean>(true),
								MakeShared<FJsonValueBoolean>(false), TEXT("name"));
							return false;
						}
						continue;
					}
					const FName ExistingName = Existing->VarName;
					if (Observation) {
						Observation->IncludeReferences(Blueprint, *Existing, bDelegate);
						Observation->Members.Add(Existing->VarGuid, {ExistingName, NAME_None, bDelegate});
					}
					FBlueprintEditorUtils::RemoveMemberVariable(&Blueprint, ExistingName);
					if (bDelegate) FBlueprintEditorUtils::ConformDelegateSignatureGraphs(&Blueprint);
					bDelegate ? ++OutCounters.DelegatesRemoved : ++OutCounters.VariablesRemoved;
					bOutNeedsCompile = true;
					continue;
				}

				if (Name.TrimStartAndEnd().IsEmpty())
				{
					FailMember(TEXT("blueprint_member_name_missing"), TEXT("A Blueprint member name must be non-empty."),
						TEXT("name"), MakeShared<FJsonValueString>(Name),
						MakeShared<FJsonValueString>(TEXT("a non-empty member name")), TEXT("name"));
					OutError.InternalContext = Entry.Key;
					return false;
				}
				if (Operation == EOperation::Create && Existing)
				{
					FailMember(TEXT("blueprint_member_already_exists"), TEXT("The Blueprint member requested for creation already exists."),
						TEXT("member_exists"), MakeShared<FJsonValueBoolean>(true),
						MakeShared<FJsonValueBoolean>(false), TEXT("operation"));
					return false;
				}
				if (Operation == EOperation::Update && !Existing)
				{
					FailMember(TEXT("blueprint_member_not_found"), TEXT("The Blueprint member requested for update was not found."),
						TEXT("member_exists"), MakeShared<FJsonValueBoolean>(false),
						MakeShared<FJsonValueBoolean>(true), TEXT("name"));
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
					if (!UeAgentBlueprintAuthoringOps::BuildPinTypeFromJsonObject(
						ConstCastSharedPtr<FJsonObject>(Type), DesiredType, OutError, &Blueprint))
					{
						if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Name;
						UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError, TEXT("/type"));
						UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Entry.Value, TEXT("member_apply"),
							UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), TEXT("type"));
						return false;
					}
					bHasType = true;
				}

				const FName BeforeName = Existing ? Existing->VarName : NAME_None;
				bool bEditable = false;
				const bool bTopologyChange = Existing && (Existing->VarName.ToString() != Name
					|| (!bDelegate && bHasType && !PinTypesEqual(Existing->VarType, DesiredType))
					|| (bDelegate && Spec->HasField(TEXT("signature"))));
				const bool bObserve = Observation && (!Existing || bTopologyChange || Spec->HasField(TEXT("default_property"))
					|| (TryGetBool(Spec, TEXT("instance_editable"), bEditable)
						&& bEditable != ((Existing->PropertyFlags & CPF_DisableEditOnInstance) == 0)));
				if (Observation && bTopologyChange) Observation->IncludeReferences(Blueprint, *Existing, bDelegate);
				if (!Existing)
				{
					if (!bHasType)
					{
						OutError = MutationInputFailure(Blueprint, *Entry.Value, TEXT("blueprint_variable_type_missing"),
							TEXT("A new Blueprint variable requires a type object."), TEXT("member_apply"), Name,
							TEXT("type"), MakeShared<FJsonValueString>(TEXT("a variable type object")));
						return false;
					}
					if (!FBlueprintEditorUtils::AddMemberVariable(&Blueprint, FName(*Name), DesiredType, FString()))
					{
						FailMember(TEXT("blueprint_member_add_failed"), TEXT("Unreal rejected creation of the requested Blueprint member."),
							TEXT("member_added"), MakeShared<FJsonValueBoolean>(false),
							MakeShared<FJsonValueBoolean>(true), TEXT("type"));
						OutError.ProposedValue = MakeShared<FJsonValueObject>(UeAgentBlueprintAuthoringOps::BuildPinTypeJsonObject(DesiredType, &Blueprint,
							UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint)));
						return false;
					}
					const int32 AddedIndex = FBlueprintEditorUtils::FindNewVariableIndex(&Blueprint, FName(*Name));
					if (AddedIndex == INDEX_NONE)
					{
						FailMember(TEXT("blueprint_member_add_readback_failed"),
							TEXT("The newly added Blueprint member was not found during readback."),
							TEXT("member_exists"), MakeShared<FJsonValueBoolean>(false),
							MakeShared<FJsonValueBoolean>(true), TEXT("name"));
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
					const FString NewName = Name;
					if (!NewName.IsEmpty()
						&& ExistingName.ToString() != NewName)
					{
						FBlueprintEditorUtils::RenameMemberVariable(&Blueprint, ExistingName, FName(*NewName));
						Name = NewName;
						const int32 RenamedIndex = FBlueprintEditorUtils::FindNewVariableIndex(&Blueprint, FName(*Name));
						if (RenamedIndex == INDEX_NONE)
						{
							FailMember(TEXT("blueprint_member_rename_readback_failed"),
								TEXT("The Blueprint member was not found under the requested name after renaming."),
								TEXT("renamed_member_found"), MakeShared<FJsonValueBoolean>(false),
								MakeShared<FJsonValueBoolean>(true), TEXT("name"));
							OutError.CandidateName = ExistingName.ToString();
							OutError.ProposedValue = MakeShared<FJsonValueString>(NewName);
							return false;
						}
						Existing = &Blueprint.NewVariables[RenamedIndex];
						bOutNeedsCompile = true;
					}
					if (!bDelegate && bHasType && !PinTypesEqual(Existing->VarType, DesiredType))
					{
						const FName VariableName = Existing->VarName;
						const FGuid VariableGuid = Existing->VarGuid;
						if (!ChangeMemberType(Blueprint, *Existing, DesiredType, OutError)) return false;
						const int32 UpdatedIndex = FBlueprintEditorUtils::FindNewVariableIndex(&Blueprint, VariableName);
						if (UpdatedIndex == INDEX_NONE || Blueprint.NewVariables[UpdatedIndex].VarGuid != VariableGuid
							|| !PinTypesEqual(Blueprint.NewVariables[UpdatedIndex].VarType, DesiredType)) {
							if (UpdatedIndex == INDEX_NONE)
								OutError = MutationFailure(Blueprint, TEXT("blueprint_variable_type_change_readback_failed"),
									TEXT("The Blueprint variable was not found after its type change."), TEXT("member_apply"),
									VariableName.ToString(), TEXT("member_exists"),
									MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
							else if (Blueprint.NewVariables[UpdatedIndex].VarGuid != VariableGuid)
							{
								OutError = MutationFailure(Blueprint, TEXT("blueprint_variable_type_change_readback_failed"),
									TEXT("The Blueprint variable identity changed during its type change."), TEXT("member_apply"),
									VariableName.ToString(), TEXT("member_guid_matches"),
									MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
								OutError.InternalContext = VariableGuid.ToString() + TEXT(" -> ")
									+ Blueprint.NewVariables[UpdatedIndex].VarGuid.ToString();
							}
							else OutError = MemberTypeDifference(Blueprint, VariableName.ToString(),
								Blueprint.NewVariables[UpdatedIndex].VarType, DesiredType);
							LocateMutationInput(OutError, *Entry.Value, TEXT("type"));
							return false;
						}
						Existing = &Blueprint.NewVariables[UpdatedIndex];
						bOutNeedsCompile = true;
					}
					bDelegate ? ++OutCounters.DelegatesUpdated : ++OutCounters.VariablesUpdated;
				}

				if (bDelegate)
				{
					UEdGraph* SignatureGraph = EnsureDelegateSignatureGraph(
						Blueprint, Existing->VarName, OutError);
					if (!SignatureGraph)
					{
						LocateMutationInput(OutError, *Entry.Value, Spec->HasField(TEXT("signature")) ? TEXT("signature") : TEXT("name"));
						return false;
					}
					bOutNeedsCompile = true;
					if (Spec->HasField(TEXT("signature")))
					{
						const TSharedPtr<const FJsonObject> Signature =
							GetObject(Spec, TEXT("signature"));
						if (!Signature.IsValid())
						{
							OutError = SignatureObjectFailure(Blueprint, *Entry.Value, TEXT("blueprint_delegate_signature_invalid"),
								TEXT("A delegate signature field must contain an object."), Name);
							return false;
						}
						if (!ApplyDelegateSignatureGraph(*SignatureGraph, Signature, OutError))
						{
							LocateSignatureInputFailure(Blueprint, *Entry.Value, OutError);
							return false;
						}
					}
				}

				if (bObserve) {
					Observation->Members.Add(Existing->VarGuid, {BeforeName, Existing->VarName, bDelegate});
					if (const auto Default = GetObject(Spec, TEXT("default_property"))) {
						FString Path;
						Observation->bBoundDefaults &= Default->TryGetStringField(TEXT("property_path"), Path) && FName(*Path) == Existing->VarName;
					}
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
					if (const auto* DefaultProperty = Spec->Values.Find(TEXT("default_property")))
					{
						// Preserve explicit malformed descriptors for the shared validator.
						OutDefaultProperties.Add(*DefaultProperty);
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
			FDiagnostic& OutError, FMemberObservation* Observation)
		{
			if (!Members.IsValid()) return true;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Members->Values)
			{
				if (Field.Key != TEXT("variables") && Field.Key != TEXT("delegates"))
				{
					OutError = MutationInputFailure(Blueprint, *Members, TEXT("blueprint_members_unknown_field"),
						TEXT("The members object contains an unsupported collection."), TEXT("member_apply"),
						FString(), Field.Key, MakeShared<FJsonValueString>(TEXT("variables or delegates")));
					OutError.ActualValue = MakeShared<FJsonValueString>(Field.Key);
					OutError.ActualValuePresent = true;
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
					OutError, Observation))
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
				OutError, Observation);
		}

		bool ApplyInterfaces(
			UBlueprint& Blueprint,
			const TArray<FStableMapEntry>& Entries,
			FMutationCounters& OutCounters,
			bool& bOutNeedsCompile,
			FDiagnostic& OutError, FPropertyIntents& Intents)
		{
			for (const FStableMapEntry& Entry : Entries)
			{
				const TSharedPtr<const FJsonObject>& Spec = Entry.Value;
				ON_SCOPE_EXIT
				{
					if (!OutError.IsEmpty() && OutError.Source.SourceFile.IsEmpty()
						&& OutError.Source.SourceUnit.IsEmpty() && OutError.SourceKind != TEXT("native_result")
						&& OutError.SourceKind != TEXT("command_input"))
						UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Spec, TEXT("interface_apply"),
							UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint));
				};
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Spec->Values)
				{
					if (Field.Key != TEXT("class_path")
						&& Field.Key != TEXT("operation"))
					{
						OutError = MutationInputFailure(Blueprint, *Spec, TEXT("blueprint_interface_unknown_field"),
							TEXT("The interface declaration contains an unsupported field."), TEXT("interface_apply"),
							FString(), Field.Key, MakeShared<FJsonValueString>(TEXT("class_path or operation")));
						OutError.ActualValue = MakeShared<FJsonValueString>(Field.Key);
						OutError.ActualValuePresent = true;
						OutError.InternalContext = Entry.Key;
						return false;
					}
				}
				FString ClassPath;
				if (!TryGetString(Spec, TEXT("class_path"), ClassPath)
					|| ClassPath.TrimStartAndEnd().IsEmpty())
				{
					OutError = MutationInputFailure(Blueprint, *Spec, TEXT("blueprint_interface_class_missing"),
						TEXT("An interface declaration requires a non-empty class_path string."), TEXT("interface_apply"),
						FString(), TEXT("class_path"), MakeShared<FJsonValueString>(TEXT("a non-empty interface class path")));
					OutError.InternalContext = Entry.Key;
					return false;
				}
				UClass* InterfaceClass = UeAgentBlueprintAuthoringOps::ResolveClassByPath(ClassPath);
				if (!InterfaceClass || !InterfaceClass->HasAnyClassFlags(CLASS_Interface))
				{
					OutError = MutationInputFailure(Blueprint, *Spec, TEXT("blueprint_interface_class_invalid"),
						!InterfaceClass ? TEXT("The interface class path could not be resolved to a class.")
							: TEXT("The resolved class is not an Unreal interface."),
						TEXT("interface_apply"), ClassPath, TEXT("class_path"),
						MakeShared<FJsonValueString>(TEXT("a resolved class with CLASS_Interface")));
					if (InterfaceClass)
					{
						auto Facts = MakeShared<FJsonObject>();
						Facts->SetStringField(TEXT("resolved_class"), InterfaceClass->GetPathName());
						Facts->SetBoolField(TEXT("is_interface"), false);
						OutError.ActualEvidence = MakeShared<FJsonValueObject>(Facts);
					}
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
				if (Intents.InterfacePresence.IsSet()) {
					Intents.InterfacePresence->Add(InterfaceClass->GetPathName(),
						TPair<bool, bool>(Existing != nullptr, Operation != EOperation::Remove));
					if (Existing && Operation == EOperation::Remove) {
						TArray<UEdGraph*> Graphs;
						GatherExportGraphs(Blueprint, Graphs);
						for (const UEdGraph* Graph : Graphs) {
							if (Graph->Nodes.ContainsByPredicate([InterfaceClass](const UEdGraphNode* Node) {
								const auto* Event = Cast<UK2Node_Event>(Node);
								if (Event && Event->EventReference.GetMemberParentClass(Event->GetBlueprintClassFromNode()) == InterfaceClass)
									return true;
								const auto* Call = Cast<UK2Node_CallFunction>(Node);
								return Call && Call->FunctionReference.GetMemberParentClass(Call->GetBlueprintClassFromNode()) == InterfaceClass;
							}))
								ObserveExistingGraph(Intents.ObservedGraphs.GetValue(), Blueprint, *Graph);
						}
					}
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
					OutError = MutationFailure(Blueprint, TEXT("blueprint_interface_already_exists"),
						TEXT("The Blueprint already implements the interface requested for creation."), TEXT("interface_apply"),
						ClassPath, TEXT("interface_exists"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
					LocateMutationInput(OutError, *Spec, TEXT("operation"));
					return false;
				}
				if (Operation == EOperation::Update && !Existing)
				{
					OutError = MutationFailure(Blueprint, TEXT("blueprint_interface_not_found"),
						TEXT("The Blueprint does not implement the interface requested for update."), TEXT("interface_apply"),
						ClassPath, TEXT("interface_exists"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					LocateMutationInput(OutError, *Spec, TEXT("class_path"));
					return false;
				}
				if (!Existing)
				{
					if (!FBlueprintEditorUtils::ImplementNewInterface(&Blueprint, InterfacePath))
					{
						OutError = MutationFailure(Blueprint, TEXT("blueprint_interface_add_failed"),
							TEXT("Unreal rejected implementation of the requested interface."), TEXT("interface_apply"),
							ClassPath, TEXT("interface_implemented"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						LocateMutationInput(OutError, *Spec, TEXT("class_path"));
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
			const UBlueprint& Blueprint,
			const TArray<FStableMapEntry>& Entries,
			TArray<FStableMapEntry>& OutEntries,
			FDiagnostic& OutError)
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
				ON_SCOPE_EXIT
				{
					if (!OutError.IsEmpty())
						UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Entries[Index].Value,
							TEXT("component_preflight"), UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint));
				};
				const TSharedPtr<const FJsonObject> Spec = MakeStableMapSpec(
					Entries[Index], EStableMapIdentity::Component, OutError);
				if (!Spec.IsValid()) return false;
				FEntryDependency& Dependency = Dependencies.AddDefaulted_GetRef();
				TryGetString(Spec, TEXT("name"), Dependency.Name);
				TryGetString(Spec, TEXT("parent"), Dependency.ParentName);
				if (!ParseOperation(Spec, EOperation::Upsert, Dependency.Operation, OutError)) return false;
				if (Dependency.Name.TrimStartAndEnd().IsEmpty())
				{
					OutError = MutationInputFailure(Blueprint, *Entries[Index].Value,
						TEXT("blueprint_component_name_missing"), TEXT("A component declaration requires a non-empty name."),
						TEXT("component_preflight"), FString(), TEXT("name"),
						MakeShared<FJsonValueString>(TEXT("a non-empty component name")));
					OutError.InternalContext = Entries[Index].Key;
					return false;
				}
				const FString NormalizedName = NormalizeName(Dependency.Name);
				if (EntryIndexByName.Contains(NormalizedName))
				{
					OutError = MutationFailure(Blueprint, TEXT("blueprint_component_name_conflict"),
						TEXT("More than one component declaration has the same normalized name."),
						TEXT("component_preflight"), Dependency.Name, TEXT("observed_name_matches"),
						MakeShared<FJsonValueNumber>(2), MakeShared<FJsonValueNumber>(1));
					LocateMutationInput(OutError, *Entries[Index].Value, TEXT("name"));
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
					OutError = MutationInputFailure(Blueprint, *Entries[Index].Value,
						TEXT("blueprint_component_parent_removed"),
						TEXT("The requested parent component is scheduled for removal in this mutation."),
						TEXT("component_preflight"), Dependency.Name, TEXT("parent"),
						MakeShared<FJsonValueString>(TEXT("a parent not scheduled for removal")));
					auto Facts = MakeShared<FJsonObject>();
					Facts->SetStringField(TEXT("parent_operation"), TEXT("remove"));
					OutError.ActualEvidence = MakeShared<FJsonValueObject>(Facts);
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

				// Every blocked non-removal entry has a blocked parent here.
				// Follow those existing edges to report the cycle itself, excluding descendants.
				int32 Current = 0;
				while (bEmitted[Current] || Dependencies[Current].Operation == EOperation::Remove) ++Current;
				TArray<int32> Trace;
				TMap<int32, int32> TracePositions;
				while (!TracePositions.Contains(Current))
				{
					TracePositions.Add(Current, Trace.Num());
					Trace.Add(Current);
					Current = Dependencies[Current].ParentIndex;
				}
				TArray<TSharedPtr<FJsonValue>> CycleEdges;
				for (int32 Position = TracePositions.FindChecked(Current); Position < Trace.Num(); ++Position)
				{
					const FEntryDependency& Edge = Dependencies[Trace[Position]];
					auto Value = MakeShared<FJsonObject>();
					Value->SetStringField(TEXT("component"), Edge.Name);
					Value->SetStringField(TEXT("parent"), Edge.ParentName);
					CycleEdges.Add(MakeShared<FJsonValueObject>(Value));
				}
				OutError = MutationInputFailure(Blueprint, *Entries[Current].Value,
					TEXT("blueprint_component_parent_cycle"), TEXT("The component parent dependencies contain a cycle."),
					TEXT("component_preflight"), Dependencies[Current].Name, TEXT("parent"),
					MakeShared<FJsonValueString>(TEXT("acyclic component parent dependencies")));
				OutError.ActualValue = MakeShared<FJsonValueArray>(MoveTemp(CycleEdges));
				OutError.ActualValuePresent = true;
				if (!OutError.Source.SourceFile.IsEmpty() || !OutError.Source.SourceUnit.IsEmpty())
					if (OutError.SourceKind != TEXT("command_input")) OutError.SourceKind = TEXT("related_input");
				return false;
			}

			for (int32 Index = 0; Index < Entries.Num(); ++Index)
			{
				if (Dependencies[Index].Operation == EOperation::Remove) OutEntries.Add(Entries[Index]);
			}
			return true;
		}

		USCS_Node* ResolveComponentNode(
			const UBlueprint& Blueprint,
			USimpleConstructionScript& SCS,
			const TSharedPtr<const FJsonObject>& Spec,
			const FJsonObject& Input,
			FDiagnostic& OutError)
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
				UeAgentAuthoringDiagnostics::LocateFailure(OutError, Input, TEXT("component_resolve"),
					UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint));
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
						OutError = MutationFailure(Blueprint, TEXT("blueprint_component_guid_conflict"),
							TEXT("More than one native Blueprint component matches the requested GUID."),
							TEXT("component_resolve"), Name, TEXT("observed_guid_matches"),
							MakeShared<FJsonValueNumber>(2), MakeShared<FJsonValueNumber>(1));
						OutError.CandidateChoices = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
							MakeShared<FJsonValueString>(GuidMatch->GetVariableName().ToString()),
							MakeShared<FJsonValueString>(Node->GetVariableName().ToString())});
						OutError.InternalContext = Guid.ToString();
						LocateMutationInput(OutError, Input, Input.HasField(TEXT("guid")) ? TEXT("guid")
							: Input.HasField(TEXT("component_guid")) ? TEXT("component_guid") : TEXT("variable_guid"));
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
			if (!GuidMatch && FallbackMatches.Num() > 1)
			{
				OutError = MutationFailure(Blueprint, TEXT("blueprint_component_fallback_conflict"),
					TEXT("More than one native component matches the requested name and class selectors."),
					TEXT("component_resolve"), Name, TEXT("selector_match_count"),
					MakeShared<FJsonValueNumber>(FallbackMatches.Num()), MakeShared<FJsonValueNumber>(1));
				if (!ClassPath.IsEmpty())
				{
					OutError.ProposedValue = MakeShared<FJsonValueString>(ClassPath);
				}
				LocateMutationInput(OutError, Input, Input.HasField(TEXT("name")) ? TEXT("name") : TEXT("class"));
				return nullptr;
			}
			if (GuidMatch && !FallbackMatches.IsEmpty() && !FallbackMatches.Contains(GuidMatch))
			{
				auto Actual = MakeShared<FJsonObject>();
				Actual->SetStringField(TEXT("name"), GuidMatch->GetVariableName().ToString());
				if (GuidMatch->ComponentClass)
					Actual->SetStringField(TEXT("class"), UeAgentBlueprintAssetLifecycle::GetAuthoringTypeObjectPath(
						&Blueprint, *GuidMatch->ComponentClass, UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint)));
				else Actual->SetField(TEXT("class"), MakeShared<FJsonValueNull>());
				auto Expected = MakeShared<FJsonObject>();
				if (!Name.IsEmpty()) Expected->SetStringField(TEXT("name"), Name);
				if (!ClassPath.IsEmpty()) Expected->SetStringField(TEXT("class"), ClassPath);
				OutError = MutationFailure(Blueprint, TEXT("blueprint_component_identity_conflict"),
					TEXT("The GUID and name/class selectors identify different Blueprint components."),
					TEXT("component_resolve"), Name, TEXT("component_selectors"),
					MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected));
				OutError.InternalContext = Guid.ToString();
				LocateMutationInput(OutError, Input, Input.HasField(TEXT("name")) ? TEXT("name") : TEXT("class"));
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

		bool ApplyComponents(
			UBlueprint& Blueprint,
			const TArray<FStableMapEntry>& Entries,
			FMutationCounters& OutCounters,
			bool& bOutNeedsCompile,
			FDiagnostic& OutError, FPropertyIntents* Intents)
		{
			USimpleConstructionScript* SCS = Blueprint.SimpleConstructionScript;
			if (!SCS && !Entries.IsEmpty())
			{
				OutError = MutationFailure(Blueprint, TEXT("blueprint_has_no_scs"),
					TEXT("The Blueprint has no Simple Construction Script for component mutations."),
					TEXT("component_apply"), FString(), TEXT("simple_construction_script_available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			if (!SCS) return true;

			TArray<FStableMapEntry> OrderedEntries;
			if (!OrderComponentEntries(Blueprint, Entries, OrderedEntries, OutError)) return false;
			FComponentPlacements Initial;
			TSet<FName> Edited;
			if (Intents && Intents->Components.IsSet()) Initial = GetLocalComponentPlacements(Blueprint, *SCS);
			TMap<FName, USCS_Node*> OriginalNodesByName;
			for (USCS_Node* Node : SCS->GetAllNodes())
				if (Node) OriginalNodesByName.Add(Node->GetVariableName(), Node);

			for (const FStableMapEntry& Entry : OrderedEntries)
			{
				FString FailureField;
				ON_SCOPE_EXIT
				{
					if (!OutError.IsEmpty())
						LocateComponentInput(Blueprint, *Entry.Value, OutError, FailureField);
				};
				const TSharedPtr<const FJsonObject> Spec = MakeStableMapSpec(
					Entry, EStableMapIdentity::Component, OutError);
				if (!Spec.IsValid()) return false;
				if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Spec,
					{{TEXT("name"), EJson::String}, {TEXT("class"), EJson::String},
					 {TEXT("parent"), EJson::String}, {TEXT("candidate"), EJson::Object}}, OutError)) return false;
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
				TryGetString(CandidateSpec, TEXT("name"), CandidateName);
				TryGetString(CandidateSpec, TEXT("id"), CandidateId);
				UE::UAI::HjsonWorkspace::DomainAuthoring::FDomainHjsonCandidateSelector CandidateSelector;
				if (CandidateSpec.IsValid())
				{
					UE::UAI::HjsonWorkspace::FWorkspaceError SelectorError;
					if (!UE::UAI::HjsonWorkspace::DomainAuthoring::TryReadCandidateSelector(
						CandidateSpec, TEXT("candidate"), CandidateSelector, SelectorError))
					{
						OutError = SelectorError.GetDiagnostic();
						UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Entry.Value, TEXT("candidate"));
						return false;
					}
				}
				const bool bNativeCandidate = CandidateSpec.IsValid()
					&& (!CandidateName.IsEmpty() || !CandidateId.IsEmpty());
				auto FailComponent = [&](const TCHAR* Code, const FString& Reason, const FString& Field,
					TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& InputField)
				{
					OutError = MutationFailure(Blueprint, Code, Reason, TEXT("component_apply"), Name, Field,
						MoveTemp(Actual), MoveTemp(Expected));
					FailureField = InputField;
				};
				auto ClassValue = [&](const UClass* Class) -> TSharedPtr<FJsonValue>
				{
					if (!Class) return MakeShared<FJsonValueNull>();
					return MakeShared<FJsonValueString>(UeAgentBlueprintAssetLifecycle::GetAuthoringTypeObjectPath(
						&Blueprint, *Class, UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint)));
				};
				USCS_Node* Existing = ResolveComponentNode(Blueprint, *SCS, Spec, *Entry.Value, OutError);
				if (!OutError.IsEmpty()) return false;

				if (Intents && Intents->Components.IsSet() && Existing)
				{
					Edited.Add(Existing->GetVariableName());
					if (Operation == EOperation::Remove || Operation == EOperation::Replace
						|| (!Name.IsEmpty() && Existing->GetVariableName() != FName(*Name)))
					{
						TArray<UK2Node_Variable*> Variables;
						FBlueprintEditorUtils::GetAllNodesOfClass(&Blueprint, Variables);
						for (const UK2Node_Variable* Node : Variables)
							if (Node && Node->GetGraph() && Node->GetVarName() == Existing->GetVariableName()
								&& Node->VariableReference.GetMemberParentClass(Blueprint.GeneratedClass) == Blueprint.GeneratedClass)
								Intents->Components->Graphs.Add(Node->GetGraph()->GraphGuid);
						TArray<UK2Node_ComponentBoundEvent*> Events;
						FKismetEditorUtilities::FindAllBoundEventsForComponent(&Blueprint, Existing->GetVariableName(), Events);
						for (const auto* Node : Events)
							if (Node && Node->GetGraph()) Intents->Components->Graphs.Add(Node->GetGraph()->GraphGuid);
					}
				}
				if (Operation == EOperation::Remove)
				{
					if (!Existing)
					{
						if (UeAgentBlueprintAuthoringOps::ResolveInheritedComponent(Blueprint, Name))
						{
							FailComponent(TEXT("blueprint_inherited_component_structure_not_editable"),
								TEXT("The requested component is inherited, so its structure cannot be removed from this Blueprint."),
								TEXT("inherited_component_exists"), MakeShared<FJsonValueBoolean>(true),
								MakeShared<FJsonValueBoolean>(false), TEXT("name"));
							return false;
						}
						continue;
					}
					if (!UeAgentBlueprintAuthoringOps::TryRemoveBlueprintComponentNode(Blueprint, *Existing, OutError)) return false;
					++OutCounters.ComponentsRemoved;
					bOutNeedsCompile = true;
					continue;
				}

				if (Name.TrimStartAndEnd().IsEmpty())
				{
					OutError = MutationInputFailure(Blueprint, *Entry.Value, TEXT("blueprint_component_name_missing"),
						TEXT("A component declaration requires a non-empty name."), TEXT("component_apply"),
						FString(), TEXT("name"), MakeShared<FJsonValueString>(TEXT("a non-empty component name")));
					OutError.InternalContext = Entry.Key;
					return false;
				}
				if (Operation == EOperation::Create && Existing)
				{
					FailComponent(TEXT("blueprint_component_already_exists"),
						TEXT("The component requested for creation already exists."), TEXT("component_exists"),
						MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), TEXT("operation"));
					return false;
				}
				if (bNativeCandidate)
				{
					FailureField = TEXT("candidate");
					UeAgentPaletteSearch::FCandidate ResolvedCandidate;
					if (!UeAgentPaletteSearch::TryResolveActorComponentCandidate(CandidateName, CandidateId,
						ResolvedCandidate, OutError, CandidateSelector.Select)) return false;
					const FString ResolvedClass = ResolvedCandidate.AuthoringSelectors.FindRef(TEXT("component_class"));
					if (ResolvedClass.IsEmpty())
					{
						FailComponent(TEXT("blueprint_component_candidate_class_unresolved"),
							TEXT("The selected native candidate does not provide a component_class selector."),
							TEXT("component_class"), MakeShared<FJsonValueString>(ResolvedClass),
							MakeShared<FJsonValueString>(TEXT("a non-empty component class path")), TEXT("candidate"));
						OutError.CandidateName = CandidateName;
						OutError.InternalContext = CandidateId;
						return false;
					}
					if (!ClassPath.IsEmpty() && ClassPath != ResolvedClass)
					{
						OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("blueprint_component_candidate_class_mismatch"),
							TEXT("/class"), MakeShared<FJsonValueString>(ResolvedClass), MakeShared<FJsonValueString>(ClassPath),
							TEXT("candidate_resolve"), TEXT("The declared component class differs from the selected native candidate."));
						FailureField = TEXT("class");
						return false;
					}
					ClassPath = ResolvedClass;
					FailureField.Reset();
				}
				if (Operation != EOperation::Create && !Existing)
				{
					TArray<UeAgentBlueprintAuthoringOps::FInheritedComponent> InheritedComponents;
					UeAgentBlueprintAuthoringOps::GatherInheritedComponents(Blueprint, InheritedComponents);
					const UActorComponent* ResolvedInherited = UeAgentBlueprintAuthoringOps::ResolveInheritedComponent(Blueprint, Name);
					const auto* Inherited = InheritedComponents.FindByPredicate(
						[ResolvedInherited](const auto& Component) { return Component.Component == ResolvedInherited; });
					if (Inherited)
					{
						if (Intents) Intents->Components.Reset();
						if (Spec->HasField(TEXT("parent")) && FName(*ParentName) != FName(*Inherited->ParentName))
						{
							FailureField = TEXT("parent");
							FailComponent(TEXT("blueprint_inherited_component_structure_not_editable"),
								TEXT("The parent of an inherited component cannot be changed from this Blueprint."),
								TEXT("parent"), MakeShared<FJsonValueString>(Inherited->ParentName),
								MakeShared<FJsonValueString>(ParentName), TEXT("parent"));
							return false;
						}
						if (!ClassPath.IsEmpty() && ClassPath != Inherited->Component->GetClass()->GetPathName())
						{
							FailureField = TEXT("class");
							UClass* NewClass = UeAgentBlueprintAuthoringOps::ResolveClassByPath(ClassPath);
							if (!NewClass || !UeAgentBlueprintAuthoringOps::TryChangeInheritedComponentClass(Blueprint, Name, *NewClass, OutError))
							{
								if (!NewClass)
									FailComponent(TEXT("blueprint_component_class_invalid"),
										TEXT("The requested inherited component class path could not be resolved to a class."),
										TEXT("class"), MakeShared<FJsonValueString>(ClassPath),
										MakeShared<FJsonValueString>(TEXT("a resolved component class")),
										Spec->HasField(TEXT("class")) ? TEXT("class") : TEXT("candidate"));
								return false;
							}
							FailureField.Reset();
							bOutNeedsCompile = true;
							++OutCounters.ComponentsUpdated;
						}
						TSharedPtr<FJsonObject> Payload = MakePropertyPayload(Spec, TEXT("properties"), OutError);
						if (!OutError.IsEmpty()) return false;
						int32 Applied = 0;
						if (Payload.IsValid())
						{
							UActorComponent* Editable = UeAgentBlueprintAuthoringOps::ResolveEditableInheritedComponent(Blueprint, Name, OutError);
							if (!Editable || !ApplyProperties(*Editable, Payload, Applied, OutError, &Blueprint, Intents)) return false;
						}
						OutCounters.ComponentsUpdated += Applied > 0 ? 1 : 0;
						continue;
					}
					FailComponent(TEXT("blueprint_component_not_found"),
						TEXT("No local or inherited component matches the requested identity."), TEXT("component_exists"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("name"));
					return false;
				}

				UClass* ComponentClass = nullptr;
				if (!ClassPath.IsEmpty())
				{
					ComponentClass = UeAgentBlueprintAuthoringOps::ResolveClassByPath(ClassPath);
					if (!ComponentClass || !ComponentClass->IsChildOf(UActorComponent::StaticClass()))
					{
						FailComponent(TEXT("blueprint_component_class_invalid"),
							!ComponentClass ? TEXT("The component class path could not be resolved to a class.")
								: TEXT("The resolved class is not derived from UActorComponent."),
							TEXT("class"), MakeShared<FJsonValueString>(ClassPath),
							MakeShared<FJsonValueString>(TEXT("a resolved UActorComponent class")),
							Spec->HasField(TEXT("class")) ? TEXT("class") : TEXT("candidate"));
						return false;
					}
				}

				TMap<USCS_Node*, USCS_Node*> ParentMap;
				for (USCS_Node* Root : SCS->GetRootNodes()) BuildComponentParentMap(Root, nullptr, ParentMap);
				USCS_Node* DesiredParent = ParentName.IsEmpty()
					? nullptr
					: UeAgentBlueprintAuthoringOps::FindSCSNodeByAnyName(SCS, ParentName);
				if (!DesiredParent && !ParentName.IsEmpty())
				{
					// An unchanged parent field may still use its captured name after
					// another record renamed that same native component in this batch.
					USCS_Node* OriginalParent = OriginalNodesByName.FindRef(FName(*ParentName));
					if (OriginalParent && SCS->GetAllNodes().Contains(OriginalParent)) DesiredParent = OriginalParent;
				}
				USceneComponent* InheritedParent = !ParentName.IsEmpty() && !DesiredParent
					? Cast<USceneComponent>(UeAgentBlueprintAuthoringOps::ResolveInheritedComponent(Blueprint, ParentName)) : nullptr;
				if (!ParentName.IsEmpty() && !DesiredParent && !InheritedParent)
				{
					FailureField = TEXT("parent");
					FailComponent(TEXT("blueprint_component_parent_not_found"),
						TEXT("The requested parent is neither a local component nor an inherited scene component."),
						TEXT("parent"), MakeShared<FJsonValueString>(ParentName),
						MakeShared<FJsonValueString>(TEXT("an existing local component or inherited scene component")), TEXT("parent"));
					return false;
				}
				const bool bClassConflict = Existing && ComponentClass && Existing->ComponentClass != ComponentClass;
				TArray<USCS_Node*> ReplacedChildren;
				USCS_Node* ReplacedNode = nullptr;
				bool bRestoreReplacedRoot = false;
				const bool bParentConflict = Existing && Spec->HasField(TEXT("parent"))
					&& (InheritedParent ? Existing->GetParentComponentTemplate(&Blueprint) != InheritedParent
						: ParentMap.FindRef(Existing) != DesiredParent
							|| (!DesiredParent && !Existing->ParentComponentOrVariableName.IsNone()));
				if (bClassConflict && Operation != EOperation::Replace)
				{
					FailureField = TEXT("class");
					TSharedPtr<FJsonValue> OperationValue = Spec->TryGetField(TEXT("operation"));
					if (!OperationValue.IsValid()) OperationValue = MakeShared<FJsonValueString>(TEXT("upsert"));
					FailComponent(TEXT("blueprint_component_class_update_requires_replace"),
						TEXT("Changing a local component's class requires the replace operation."), TEXT("operation"),
						MoveTemp(OperationValue), MakeShared<FJsonValueString>(TEXT("replace")),
						Spec->HasField(TEXT("operation")) ? TEXT("operation") : TEXT("class"));
					auto Actual = MakeShared<FJsonObject>();
					Actual->SetField(TEXT("operation"), OutError.ActualValue);
					Actual->SetField(TEXT("class"), ClassValue(Existing->ComponentClass));
					auto Expected = MakeShared<FJsonObject>();
					Expected->SetField(TEXT("operation"), OutError.ExpectedValue);
					Expected->SetField(TEXT("class"), ClassValue(ComponentClass));
					OutError.PropertyPath = TEXT("component_update");
					OutError.ActualValue = MakeShared<FJsonValueObject>(Actual);
					OutError.ExpectedValue = MakeShared<FJsonValueObject>(Expected);
					return false;
				}
				if (Existing && bClassConflict)
				{
					ReplacedNode = Existing;
					if (ComponentClass->IsChildOf(USceneComponent::StaticClass()))
					{
						// Omitted relationships mean preserve, even though native removal
						// promotes children and native creation otherwise attaches to root.
						if (!Spec->HasField(TEXT("parent")))
						{
							DesiredParent = ParentMap.FindRef(Existing);
							if (!DesiredParent && !Existing->ParentComponentOrVariableName.IsNone())
								InheritedParent = Cast<USceneComponent>(Existing->GetParentComponentTemplate(&Blueprint));
						}
						bRestoreReplacedRoot = !DesiredParent && !InheritedParent;
						ReplacedChildren = Existing->GetChildNodes();
					}
					if (!UeAgentBlueprintAuthoringOps::TryRemoveBlueprintComponentNode(Blueprint, *Existing, OutError)) return false;
					Existing = nullptr;
					++OutCounters.ComponentsRemoved;
				}
				else if (bParentConflict)
				{
					FailureField = TEXT("parent");
					if (!UeAgentBlueprintAuthoringOps::TrySetBlueprintComponentParent(
						Blueprint, *Existing, DesiredParent, InheritedParent, OutError)) return false;
					bOutNeedsCompile = true;
					FailureField.Reset();
				}
				if (!Existing)
				{
					if (!ComponentClass)
					{
						OutError = MutationInputFailure(Blueprint, *Entry.Value, TEXT("blueprint_component_class_missing"),
							TEXT("Creating a component requires a class path or a candidate that resolves to a component class."),
							TEXT("component_apply"), Name, TEXT("class"),
							MakeShared<FJsonValueString>(TEXT("a component class path or resolvable native candidate")));
						return false;
					}
					if (bNativeCandidate)
					{
						FailureField = TEXT("candidate");
						UeAgentPaletteSearch::FCandidate AppliedCandidate;
						FDiagnostic CandidateError;
						if (!UeAgentPaletteSearch::TryPerformBlueprintComponentCandidate(
								Blueprint,
								DesiredParent,
								CandidateName,
								CandidateId,
								Name,
								Existing,
								AppliedCandidate,
								CandidateError,
								InheritedParent, CandidateSelector.Select))
						{
							OutError = MoveTemp(CandidateError);
							return false;
						}
						if (Existing->ComponentClass != ComponentClass)
						{
							FailComponent(TEXT("blueprint_component_native_result_mismatch"),
								TEXT("The native candidate created a component with a different class than requested."),
								TEXT("class"), ClassValue(Existing->ComponentClass), ClassValue(ComponentClass),
								Spec->HasField(TEXT("class")) ? TEXT("class") : TEXT("candidate"));
							return false;
						}
						FailureField.Reset();
					}
					else
					{
						const TFunction<void(const FSubobjectDataHandle&)> NoPostCreate;
						if (!UeAgentBlueprintAuthoringOps::TryCreateBlueprintComponentNode(
								Blueprint,
								DesiredParent,
								*ComponentClass,
								Name,
								nullptr,
								NoPostCreate,
								Existing,
								OutError,
								InheritedParent))
						{
							return false;
						}
					}
					// AddNewSubobject attaches below the current scene root. An
					// explicitly empty parent instead requests the same native root
					// promotion used when editing an existing component. That operation
					// also retires UE's DefaultSceneRoot before deferred removals run.
					if ((bRestoreReplacedRoot || (Spec->HasField(TEXT("parent")) && ParentName.IsEmpty()))
						&& Existing->ComponentTemplate && Existing->ComponentTemplate->IsA<USceneComponent>())
					{
						FailureField = TEXT("parent");
						if (!UeAgentBlueprintAuthoringOps::TrySetBlueprintComponentParent(
							Blueprint, *Existing, nullptr, nullptr, OutError)) return false;
						FailureField.Reset();
					}
					for (USCS_Node* Child : ReplacedChildren)
						if (Child && SCS->GetAllNodes().Contains(Child) && Child->ComponentTemplate
							&& Child->ComponentTemplate->IsA<USceneComponent>())
						{
							FailureField = TEXT("parent");
							if (!UeAgentBlueprintAuthoringOps::TrySetBlueprintComponentParent(
								Blueprint, *Child, Existing, nullptr, OutError)) return false;
							FailureField.Reset();
						}
					if (ReplacedNode)
						for (auto& Original : OriginalNodesByName)
							if (Original.Value == ReplacedNode) Original.Value = Existing;
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
					++OutCounters.ComponentsAdded;
					bOutNeedsCompile = true;
				}
				else
				{
					if (Existing->GetVariableName() != FName(*Name.TrimStartAndEnd()))
					{
						FailureField = TEXT("name");
						if (!UeAgentBlueprintAuthoringOps::TryRenameBlueprintComponentNode(
							Blueprint, *Existing, Name, OutError)) return false;
						bOutNeedsCompile = true;
						FailureField.Reset();
					}
					++OutCounters.ComponentsUpdated;
				}

				TSharedPtr<FJsonObject> Payload = MakePropertyPayload(Spec, TEXT("properties"), OutError);
				if (!OutError.IsEmpty()) return false;
				int32 Applied = 0;
				if (Existing->ComponentTemplate
					&& !ApplyProperties(*Existing->ComponentTemplate, Payload, Applied, OutError, &Blueprint, Intents))
				{
					return false;
				}
			}
			if (Intents && Intents->Components.IsSet())
				Intents->Components->Select(Initial, GetLocalComponentPlacements(Blueprint, *SCS), Edited);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
			return true;
		}

		bool IsBuiltinNode(const UEdGraphNode* Node)
		{
			return Node && (Node->IsA<UK2Node_FunctionEntry>()
				|| Node->IsA<UK2Node_FunctionResult>()
				|| ExactCast<UK2Node_Tunnel>(Node));
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
			const FJsonObject& Input,
			FString& OutName,
			FString& OutKind,
			FDiagnostic& OutError)
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
				LocateGraphHeaderInput(Blueprint, Input, OutError, FString());
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
						OutError = MutationFailure(Blueprint, TEXT("blueprint_graph_guid_conflict"),
							TEXT("More than one Blueprint graph matches the requested GUID."), TEXT("graph_resolve"),
							OutName, TEXT("observed_guid_matches"), MakeShared<FJsonValueNumber>(2), MakeShared<FJsonValueNumber>(1));
						OutError.CandidateChoices = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
							MakeShared<FJsonValueString>(GuidMatch->GetName()),
							MakeShared<FJsonValueString>(Graph->GetName())});
						OutError.InternalContext = Guid.ToString();
						LocateGraphHeaderInput(Blueprint, Input, OutError,
							GraphInputHeader(Input).HasField(TEXT("guid")) ? TEXT("guid") : TEXT("graph_guid"));
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
			if (!GuidMatch && FallbackMatches.Num() > 1)
			{
				OutError = MutationFailure(Blueprint, TEXT("blueprint_graph_fallback_conflict"),
					TEXT("More than one Blueprint graph matches the requested name and kind selectors."),
					TEXT("graph_resolve"), OutName, TEXT("selector_match_count"),
					MakeShared<FJsonValueNumber>(FallbackMatches.Num()), MakeShared<FJsonValueNumber>(1));
				auto Selectors = MakeShared<FJsonObject>();
				if (!OutName.IsEmpty()) Selectors->SetStringField(TEXT("name"), OutName);
				if (!OutKind.IsEmpty()) Selectors->SetStringField(TEXT("kind"), OutKind);
				OutError.ProposedValue = MakeShared<FJsonValueObject>(Selectors);
				const FJsonObject& SourceHeader = GraphInputHeader(Input);
				LocateGraphHeaderInput(Blueprint, Input, OutError, SourceHeader.HasField(TEXT("name")) ? TEXT("name")
					: SourceHeader.HasField(TEXT("graph_kind")) ? TEXT("graph_kind") : TEXT("kind"));
				return nullptr;
			}
			if (GuidMatch && !FallbackMatches.IsEmpty() && !FallbackMatches.Contains(GuidMatch))
			{
				auto Actual = MakeShared<FJsonObject>();
				Actual->SetStringField(TEXT("name"), GuidMatch->GetName());
				Actual->SetStringField(TEXT("kind"), GetGraphKind(Blueprint, GuidMatch));
				auto Expected = MakeShared<FJsonObject>();
				if (!OutName.IsEmpty()) Expected->SetStringField(TEXT("name"), OutName);
				if (!OutKind.IsEmpty()) Expected->SetStringField(TEXT("kind"), OutKind);
				OutError = MutationFailure(Blueprint, TEXT("blueprint_graph_identity_conflict"),
					TEXT("The GUID and name/kind selectors identify different Blueprint graphs."),
					TEXT("graph_resolve"), OutName, TEXT("graph_selectors"),
					MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected));
				OutError.InternalContext = Guid.ToString();
				const FJsonObject& SourceHeader = GraphInputHeader(Input);
				LocateGraphHeaderInput(Blueprint, Input, OutError, SourceHeader.HasField(TEXT("name")) ? TEXT("name")
					: SourceHeader.HasField(TEXT("graph_kind")) ? TEXT("graph_kind") : TEXT("kind"));
				return nullptr;
			}
			return GuidMatch ? GuidMatch : (FallbackMatches.Num() == 1 ? FallbackMatches[0] : nullptr);
		}

		bool MatchesBlueprintFunction(const UBlueprint& Blueprint, const FName Name, const FGuid& Guid, const UFunction* Function)
		{
			if (!Function || Function->GetFName() != Name
				|| UBlueprint::GetBlueprintFromClass(Function->GetOwnerClass()) != &Blueprint) return false;
			FGuid ActualGuid;
			UBlueprint::GetFunctionGuidFromClassByFieldName(Function->GetOwnerClass(), Name, ActualGuid);
			return Guid.IsValid() && ActualGuid == Guid;
		}

		const UFunction* ResolveDelegateFunction(const UBlueprint& Consumer, const UK2Node_CreateDelegate* Binding)
		{
			if (!Binding) return nullptr;
			// Native member resolution may repair caches; resolve only a value copy.
			UClass* SelfScope = Consumer.SkeletonGeneratedClass;
			UClass* Scope = Binding->GetScopeClass();
			const bool Self = SelfScope && Scope && (SelfScope->IsChildOf(Scope) || SelfScope->ClassGeneratedBy == Scope->ClassGeneratedBy);
			FMemberReference Reference;
			Reference.SetDirect(Binding->GetFunctionName(), Binding->SelectedFunctionGuid, Scope, Self);
			return Reference.ResolveMember<UFunction>(SelfScope);
		}

		bool IsNativeBlueprintChild(const UBlueprint& Parent, const UBlueprint& Child)
		{
			if (!Child.ParentClass || UeAgentBlueprintAssetLifecycle::IsAuthoringWorkingCopy(Child)) return false;
			TArray<UBlueprint*> Parents;
			UBlueprint::GetBlueprintHierarchyFromClass(Child.ParentClass, Parents);
			TArray<UClass*> Interfaces;
			FBlueprintEditorUtils::FindImplementedInterfaces(&Child, true, Interfaces);
			for (UClass* Interface : Interfaces)
				if (auto* Owner = UBlueprint::GetBlueprintFromClass(Interface)) Parents.Add(Owner);
			return Parents.Contains(&Parent);
		}

		enum class ENativeRenameKind { Function, Delegate, Variable };

		FDiagnostic NativeRenameFailure(const UBlueprint& Blueprint, const TCHAR* Code,
			const FString& Reason, const FString& CurrentName, const FString& RequestedName,
			const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			FDiagnostic Error = MutationFailure(Blueprint, Code, Reason, TEXT("rename_preflight"),
				CurrentName, Field, MoveTemp(Actual), MoveTemp(Expected));
			if (Error.ActualValue.IsValid() && Error.ActualValue->Type == EJson::String
				&& Error.ActualValue->AsString() == RequestedName) return Error;
			Error.ProposedValue = MakeShared<FJsonValueString>(RequestedName);
			return Error;
		}

		bool ValidateNativeRenameEffects(UBlueprint& Blueprint, const FName OldName, const FName NewName,
			const ENativeRenameKind Kind, FDiagnostic& OutError, const UK2Node_CustomEvent* CustomEvent = nullptr,
			const bool bPersistentReadback = false)
		{
			const bool bVariableReferences = Kind != ENativeRenameKind::Function;
			const bool bFunctionReferences = Kind != ENativeRenameKind::Variable;
			if (CustomEvent)
			{
				// OnRenameNode does not broadcast ReplaceFunctionReferences or validate
				// child variables. Full compilation nevertheless resolves external calls
				// by GUID, mutating serialized references even without Modify/dirty events.
				for (TObjectIterator<UBlueprint> It; It; ++It)
				{
					UBlueprint* Consumer = *It;
					if (Consumer == &Blueprint || Consumer->HasAnyFlags(RF_ClassDefaultObject)
						|| UeAgentBlueprintAssetLifecycle::IsAuthoringWorkingCopy(*Consumer)) continue;
					TArray<UEdGraph*> Graphs; Consumer->GetAllGraphs(Graphs);
					for (const UEdGraph* Graph : Graphs) for (UEdGraphNode* Node : Graph->Nodes)
					{
						const auto* Call = Cast<UK2Node_CallFunction>(Node);
						FMemberReference Reference = Call ? Call->FunctionReference : FMemberReference();
						const UFunction* Function = Call ? Reference.ResolveMember<UFunction>(Call->GetBlueprintClassFromNode()) : nullptr;
						const auto* Binding = bPersistentReadback ? Cast<UK2Node_CreateDelegate>(Node) : nullptr;
						const UFunction* BoundFunction = ResolveDelegateFunction(*Consumer, Binding);
						const bool bCallMatches = MatchesBlueprintFunction(Blueprint, OldName, CustomEvent->NodeGuid, Function);
						if (bCallMatches || MatchesBlueprintFunction(Blueprint, OldName, CustomEvent->NodeGuid, BoundFunction))
						{
							OutError = NativeRenameFailure(Blueprint, TEXT("blueprint_custom_event_rename_external_impact"),
								bCallMatches ? TEXT("Renaming the custom event would affect a function call in another loaded Blueprint.")
									: TEXT("Renaming the custom event would affect a delegate binding in another loaded Blueprint."),
								OldName.ToString(), NewName.ToString(), TEXT("external_reference_exists"),
								MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
							auto Facts = MakeShared<FJsonObject>();
							Facts->SetNumberField(TEXT("observed_affected_blueprints"), 1);
							OutError.ActualEvidence = MakeShared<FJsonValueObject>(Facts);
							OutError.InternalContext = Node->GetPathName();
							return false;
						}
					}
				}
				return true;
			}
			auto Outside = [&](const UBlueprint* Other, const FString& Reason, const FString& Field,
				TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Context = FString())
			{
				OutError = NativeRenameFailure(Blueprint,
					Kind == ENativeRenameKind::Variable ? TEXT("blueprint_variable_rename_external_impact")
						: bVariableReferences ? TEXT("blueprint_delegate_rename_external_impact") : TEXT("blueprint_graph_rename_external_impact"),
					Reason, OldName.ToString(), NewName.ToString(), Field, MoveTemp(Actual), MoveTemp(Expected));
				const TSharedPtr<FJsonObject> Facts = OutError.ActualEvidence.IsValid()
					? OutError.ActualEvidence->AsObject() : MakeShared<FJsonObject>();
				Facts->SetNumberField(TEXT("observed_affected_blueprints"), 1);
				OutError.ActualEvidence = MakeShared<FJsonValueObject>(Facts);
				OutError.InternalContext = Context.IsEmpty() ? Other->GetPathName() : Context;
				return false;
			};
			// Match RenameGraph's child validation and its ModifiedBlueprints set.
			// A dirty child also enters that set even when none of its members conflict.
			TSet<const UBlueprint*> ModifiedBlueprints{&Blueprint};
			for (TObjectIterator<UBlueprint> It; It; ++It)
			{
				UBlueprint* Child = *It;
				if (!IsNativeBlueprintChild(Blueprint, *Child)) continue;
				for (const FBPVariableDescription& Variable : Child->NewVariables)
					if (Variable.VarName == NewName) return Outside(Child, TEXT("A child Blueprint already has a variable with the requested new name."),
						TEXT("child_variable_name"), MakeShared<FJsonValueString>(Variable.VarName.ToString()),
						MakeShared<FJsonValueString>(TEXT("a name unused by child Blueprint variables")));
				if (Child->SimpleConstructionScript)
					for (const USCS_Node* Node : Child->SimpleConstructionScript->GetAllNodes())
						if (Node && Node->GetVariableName() == NewName) return Outside(Child, TEXT("A child Blueprint already has a component with the requested new name."),
							TEXT("child_component_name"), MakeShared<FJsonValueString>(Node->GetVariableName().ToString()),
							MakeShared<FJsonValueString>(TEXT("a name unused by child Blueprint components")));
				for (const UTimelineTemplate* Timeline : Child->Timelines)
					if (Timeline && Timeline->GetFName() == NewName) return Outside(Child, TEXT("A child Blueprint already has a timeline with the requested new name."),
						TEXT("child_timeline_name"), MakeShared<FJsonValueString>(Timeline->GetName()),
						MakeShared<FJsonValueString>(TEXT("a name unused by child Blueprint timelines")));
				for (const UEdGraph* Function : Child->FunctionGraphs)
					if (Function && (Function->GetFName() == NewName || (bFunctionReferences && Function->GetFName() == OldName))) return Outside(Child,
						Function->GetFName() == NewName ? TEXT("A child Blueprint already has a function with the requested new name.")
							: TEXT("A child Blueprint function with the current name would be affected by the rename."),
						TEXT("child_function_name"), MakeShared<FJsonValueString>(Function->GetName()),
						MakeShared<FJsonValueString>(TEXT("a name that does not affect a child Blueprint function")));
				if (Child->GetOutermost()->IsDirty()) ModifiedBlueprints.Add(Child);
			}
			if (bFunctionReferences) for (TObjectIterator<UK2Node_CallFunction> It(RF_ClassDefaultObject | RF_Transient); It; ++It)
			{
				const auto* Call = *It;
				if (!Cast<UEdGraph>(Call->GetOuter()) || Call->FunctionReference.GetMemberName() != OldName) continue;
				const auto* Owner = FBlueprintEditorUtils::FindBlueprintForNode(Call);
				if (!Owner || Owner == &Blueprint || UeAgentBlueprintAssetLifecycle::IsAuthoringWorkingCopy(*Owner)) continue;
				const auto* Target = UBlueprint::GetBlueprintFromClass(Call->FunctionReference.GetMemberParentClass());
				if ((Call->FunctionReference.IsSelfContext() && ModifiedBlueprints.Contains(Owner))
					|| (Target && ModifiedBlueprints.Contains(Target))) return Outside(Owner, TEXT("A function call in another Blueprint references the member affected by this rename."),
						TEXT("external_function_call_exists"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false),
						Call->GetPathName());
			}
			// ReplaceFunctionReferences traverses dependents, including native binding
			// nodes. Their ReferencesFunction contract covers the rename hook and the
			// independent same-name dirty notification; dependency alone is not a write.
			TArray<UBlueprint*> Dependents;
			FBlueprintEditorUtils::FindDependentBlueprints(&Blueprint, Dependents);
			IUeAgentInterfaceMVVMModule* MVVM = nullptr;
			if (FModuleManager::Get().IsModuleLoaded(TEXT("ModelViewViewModelEditor")))
			{
				MVVM = FModuleManager::LoadModulePtr<IUeAgentInterfaceMVVMModule>(TEXT("UeAgentInterfaceMVVM"));
				if (!MVVM)
				{
					OutError = NativeRenameFailure(Blueprint, TEXT("blueprint_graph_rename_optional_adapter_unavailable"),
						TEXT("MVVM editor support is loaded, but the UAI MVVM rename adapter could not be loaded."),
						OldName.ToString(), NewName.ToString(), TEXT("mvvm_rename_adapter_available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					OutError.InternalContext = TEXT("UeAgentInterfaceMVVM");
					return false;
				}
			}
			for (UBlueprint* Dependent : Dependents)
			{
				if (!Dependent || Dependent == &Blueprint || UeAgentBlueprintAssetLifecycle::IsAuthoringWorkingCopy(*Dependent)) continue;
				if (MVVM && MVVM->HasExternalRenameEffect(Blueprint, *Dependent, OldName, NewName)) return Outside(Dependent, TEXT("Renaming this member would affect an MVVM binding in another Blueprint."),
						TEXT("external_mvvm_rename_effect"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
				TArray<UEdGraph*> Graphs;
				Dependent->GetAllGraphs(Graphs);
				for (const UEdGraph* DependentGraph : Graphs)
					for (const UEdGraphNode* Node : DependentGraph->Nodes)
						if (const auto* K2 = Cast<UK2Node>(Node))
						{
							bool bFunctionReference = false;
							auto ReferencesFunction = [&]()
							{
								bFunctionReference = K2->ReferencesFunction(OldName, nullptr);
								return bFunctionReference;
							};
							if ((bFunctionReferences && ReferencesFunction())
								|| (bVariableReferences && K2->ReferencesVariable(OldName, nullptr)))
								return Outside(Dependent,
									bFunctionReference ? TEXT("A node in a dependent Blueprint references the function being renamed.")
										: TEXT("A node in a dependent Blueprint references the variable being renamed."),
									bFunctionReference ? TEXT("external_function_reference_exists") : TEXT("external_variable_reference_exists"),
									MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), Node->GetPathName());
						}
			}
			return true;
		}

		bool ValidateGraphNameIdentity(UBlueprint& Blueprint, const UEdGraph* ExistingGraph,
			const FString& Name, FDiagnostic& OutError)
		{
			if (Blueprint.BlueprintType != BPTYPE_MacroLibrary || Name.IsEmpty() || Name.Len() >= NAME_SIZE) return true;
			UObject* Scope = ExistingGraph ? ExistingGraph->GetOuter() : &Blueprint;
			const UObjectRedirector* Redirector = FindObjectFast<UObjectRedirector>(Scope, *Name);
			if (Redirector && (!ExistingGraph || Redirector->DestinationObject != ExistingGraph))
			{
				OutError = MutationFailure(Blueprint, TEXT("blueprint_graph_name_identity_conflict"),
					TEXT("The requested macro-library graph name is occupied by a redirector for a different graph."),
					TEXT("graph_name_validate"), Name, TEXT("conflicting_name_redirector_exists"),
					MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
				OutError.InternalContext = GetPathNameSafe(Redirector) + TEXT(" -> ")
					+ GetPathNameSafe(Redirector->DestinationObject);
				return false;
			}
			return true;
		}

		bool ValidateGraphRename(UBlueprint& Blueprint, UEdGraph& Graph,
			const FString& Name, const FString& Kind, FDiagnostic& OutError)
		{
			const auto* Schema = Cast<UEdGraphSchema_K2>(Graph.GetSchema());
			const bool IsFunction = Kind == TEXT("function_graph") && Blueprint.FunctionGraphs.Contains(&Graph);
			const bool IsMacro = Kind == TEXT("macro_graph") && Blueprint.MacroGraphs.Contains(&Graph);
			auto Forbidden = [&](const FString& Reason, const FString& Field, bool Actual, bool Expected)
			{
				OutError = NativeRenameFailure(Blueprint, TEXT("blueprint_graph_rename_forbidden"), Reason,
					Graph.GetName(), Name, Field, MakeShared<FJsonValueBoolean>(Actual), MakeShared<FJsonValueBoolean>(Expected));
				return false;
			};
			auto InvalidName = [&](const FString& Reason, const FString& Expected)
			{
				OutError = MutationFailure(Blueprint, TEXT("blueprint_graph_rename_name_invalid"), Reason,
					TEXT("rename_preflight"), Graph.GetName(), TEXT("name"),
					MakeShared<FJsonValueString>(Name), MakeShared<FJsonValueString>(Expected));
				return false;
			};
			if (!IsFunction && !IsMacro)
			{
				if (Kind != TEXT("function_graph") && Kind != TEXT("macro_graph"))
				{
					OutError = NativeRenameFailure(Blueprint, TEXT("blueprint_graph_rename_forbidden"),
						TEXT("Renaming requires a function_graph or macro_graph kind."),
						Graph.GetName(), Name, TEXT("graph_kind"), MakeShared<FJsonValueString>(Kind),
						MakeShared<FJsonValueString>(TEXT("function_graph or macro_graph")));
					return false;
				}
				return Forbidden(TEXT("The resolved graph is not in the requested function or macro collection of this Blueprint."),
					TEXT("owned_graph_matches_kind"), false, true);
			}
			if (!Schema) return Forbidden(TEXT("The graph does not have a K2 schema."), TEXT("k2_schema_available"), false, true);
			if (Schema->IsConstructionScript(&Graph))
				return Forbidden(TEXT("Construction script graphs cannot be renamed."), TEXT("is_construction_script"), true, false);
			if (!(Graph.bAllowDeletion || Graph.bAllowRenaming))
				return Forbidden(TEXT("The graph allows neither deletion nor renaming."), TEXT("rename_or_delete_allowed"), false, true);
			if (!Graph.bEditable) return Forbidden(TEXT("The graph is not editable."), TEXT("graph_editable"), false, true);
			if (Graph.HasAnyFlags(RF_Transient))
				return Forbidden(TEXT("Transient graphs cannot be renamed by this operation."), TEXT("graph_is_transient"), true, false);
			if (Graph.InterfaceGuid.IsValid())
				return Forbidden(TEXT("The graph implements an interface and cannot be renamed here."), TEXT("is_interface_graph"), true, false);
			if (IsFunction && Blueprint.ParentClass && Blueprint.ParentClass->FindFunctionByName(Graph.GetFName()))
				return Forbidden(TEXT("The function is already declared by the parent class and cannot be renamed here."),
					TEXT("parent_function_exists"), true, false);
			for (const UEdGraphNode* Node : Graph.Nodes)
				if (const auto* Entry = Cast<UK2Node_FunctionEntry>(Node); Entry && !Entry->IsEditable())
					return Forbidden(TEXT("The graph's function entry is not editable."), TEXT("function_entry_editable"), false, true);
			FKismetNameValidator Validator(&Blueprint, Graph.GetFName());
			const EValidatorResult Validity = Validator.IsValid(Name);
			if (!ValidateGraphNameIdentity(Blueprint, &Graph, Name, OutError)) return false;
			// Native MacroLibrary rename moves this exact redirector aside before its
			// REN_Test. The native name validator already permits that same collision.
			const UObjectRedirector* NameRedirector = Validity == EValidatorResult::Ok && Blueprint.BlueprintType == BPTYPE_MacroLibrary
				? FindObjectFast<UObjectRedirector>(Graph.GetOuter(), *Name) : nullptr;
			const bool ReusesRedirector = NameRedirector != nullptr;
			if (Name.TrimStartAndEnd() != Name)
				return InvalidName(TEXT("Graph names cannot begin or end with whitespace."), TEXT("a non-empty graph name without surrounding whitespace"));
			if (Name.IsEmpty()) return InvalidName(TEXT("A graph name must be non-empty."), TEXT("a non-empty graph name"));
			if (Validity != EValidatorResult::Ok && Validity != EValidatorResult::ExistingName)
				return InvalidName(FKismetNameValidator::GetErrorString(Name, Validity), TEXT("a name accepted by Unreal's graph name validator"));
			if (!ReusesRedirector && !Graph.Rename(*Name, Graph.GetOuter(), REN_Test))
			{
				OutError = NativeRenameFailure(Blueprint, TEXT("blueprint_graph_rename_name_invalid"),
					TEXT("Unreal rejected a trial rename of the graph to the requested name."), Graph.GetName(), Name,
					TEXT("native_rename_test_succeeded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			return ValidateNativeRenameEffects(Blueprint, Graph.GetFName(), FName(*Name), ENativeRenameKind::Function, OutError);
		}

		UEdGraph* CreateGraph(
			UBlueprint& Blueprint,
			const FString& Name,
			const FString& Kind,
			FDiagnostic& OutError)
		{
			if (!ValidateGraphNameIdentity(Blueprint, nullptr, Name, OutError)) return nullptr;
			if (Name.IsEmpty())
			{
				OutError = MutationFailure(Blueprint, TEXT("blueprint_graph_name_missing"),
					TEXT("Creating a Blueprint graph requires a non-empty name."), TEXT("graph_create"),
					FString(), TEXT("name"), MakeShared<FJsonValueString>(Name),
					MakeShared<FJsonValueString>(TEXT("a non-empty graph name")));
				return nullptr;
			}
			if (Kind == TEXT("function_graph"))
			{
				UEdGraph* Graph = UBlueprintEditorLibrary::AddFunctionGraph(&Blueprint, Name);
				if (!Graph)
					OutError = MutationFailure(Blueprint, TEXT("blueprint_function_graph_create_failed"),
						TEXT("Unreal did not create the requested function graph."), TEXT("graph_create"),
						Name, TEXT("function_graph_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
					OutError = MutationFailure(Blueprint, TEXT("blueprint_macro_graph_create_failed"),
						TEXT("Unreal did not create the requested macro graph."), TEXT("graph_create"),
						Name, TEXT("macro_graph_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
					OutError = MutationFailure(Blueprint, TEXT("blueprint_event_graph_create_failed"),
						TEXT("Unreal did not create the requested event graph."), TEXT("graph_create"),
						Name, TEXT("event_graph_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return nullptr;
				}
				FBlueprintEditorUtils::AddUbergraphPage(&Blueprint, Graph);
				return Graph;
			}
			OutError = MutationFailure(Blueprint, TEXT("blueprint_graph_kind_unsupported"),
				TEXT("The requested Blueprint graph kind is unsupported."), TEXT("graph_create"),
				Name, TEXT("graph_kind"), MakeShared<FJsonValueString>(Kind),
				MakeShared<FJsonValueString>(TEXT("function_graph, macro_graph or event_graph")));
			return nullptr;
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

		bool NodeMatchesFallback(
			const UEdGraphNode& Node,
			const TSharedPtr<const FJsonObject>& Spec,
			const FString& Kind)
		{
			if (!Kind.IsEmpty() && GetNodeKind(&Node) != Kind) return false;
			FString Expected;
			if (Kind == TEXT("macro_instance"))
			{
				const UK2Node_MacroInstance* Macro = CastChecked<UK2Node_MacroInstance>(&Node);
				return TryGetString(Spec, TEXT("macro_graph"), Expected)
					&& Macro->GetMacroGraph()
					&& Macro->GetMacroGraph()->GetName().Equals(Expected, ESearchCase::IgnoreCase);
			}
			if (Kind == TEXT("call_delegate"))
			{
				return TryGetString(Spec, TEXT("delegate_member"), Expected)
					&& CastChecked<UK2Node_CallDelegate>(&Node)->GetPropertyName()
						.ToString().Equals(Expected, ESearchCase::IgnoreCase);
			}
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
			if (Kind == TEXT("call_custom_event"))
			{
				return TryGetString(Spec, TEXT("custom_event"), Expected)
					&& CastChecked<UK2Node_CallFunction>(&Node)->GetTargetFunction()
					&& CastChecked<UK2Node_CallFunction>(&Node)->GetTargetFunction()
						->GetName().Equals(Expected, ESearchCase::IgnoreCase);
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
					&& UeAgentBlueprintAssetLifecycle::GetAuthoringTypeObjectPath(
						FBlueprintEditorUtils::FindBlueprintForGraph(Node.GetGraph()), *CastChecked<UK2Node_DynamicCast>(&Node)->TargetType.Get())
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

		FDiagnostic NodeMutationFailure(const UBlueprint& Blueprint, const FJsonObject& Spec,
			const FJsonObject& Input, const TCHAR* Code, const FString& Reason, const FString& Stage,
			const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& InputField = FString())
		{
			FString Kind;
			Spec.TryGetStringField(TEXT("node_type"), Kind);
			Kind.TrimStartAndEndInline();
			Kind.ToLowerInline();
			FString Candidate = Kind;
			const auto Selectors = MakeShared<FJsonObject>();
			for (const TCHAR* Selector : {TEXT("node_type"), TEXT("macro_graph"), TEXT("delegate_member"),
				TEXT("event_name"), TEXT("custom_event"), TEXT("function_name"), TEXT("function_graph"),
				TEXT("function_owner_class"), TEXT("event_class"), TEXT("delegate_owner_class"),
				TEXT("variable_name"), TEXT("member_parent_class"), TEXT("access"),
				TEXT("node_class"), TEXT("target_class"), TEXT("input_action"), TEXT("mapping_context"), TEXT("component_name"),
				TEXT("delegate_property_name")})
			{
				const TSharedPtr<FJsonValue> Value = Input.TryGetField(Selector);
				if (Value.IsValid()) Selectors->SetField(Selector, Value);
			}
			for (const TCHAR* Selector : {TEXT("event_name"), TEXT("custom_event"), TEXT("function_name"),
				TEXT("variable_name"), TEXT("function_graph"), TEXT("macro_graph"), TEXT("delegate_member"), TEXT("component_name")})
			{
				if (Input.TryGetStringField(Selector, Candidate)) break;
			}
			FDiagnostic Error = MutationFailure(Blueprint, Code, Reason, Stage, Candidate, Field,
				MoveTemp(Actual), MoveTemp(Expected));
			if (!Selectors->Values.IsEmpty())
			{
				Error.ProposedValue = MakeShared<FJsonValueObject>(Selectors);
			}
			Spec.TryGetStringField(TEXT("id"), Error.InternalContext);
			FString LocationField = InputField;
			if (LocationField.IsEmpty())
			{
				for (const TCHAR* Selector : {TEXT("guid"), TEXT("node_guid"), TEXT("ue_node_guid"), TEXT("node_type"),
					TEXT("event_name"), TEXT("custom_event"), TEXT("function_name"), TEXT("variable_name"),
					TEXT("function_graph"), TEXT("macro_graph"), TEXT("delegate_member"), TEXT("node_class"), TEXT("target_class"),
					TEXT("input_action"), TEXT("component_name")})
				{
					if (Input.HasField(Selector)) { LocationField = Selector; break; }
				}
			}
			LocateMutationInput(Error, Input, LocationField);
			return Error;
		}

		UEdGraphNode* ResolveNode(
			UBlueprint& Blueprint,
			UEdGraph& Graph,
			const TSharedPtr<const FJsonObject>& Spec,
			const FJsonObject& Input,
			FString& OutKind,
			FDiagnostic& OutError,
			const bool bAllowSemanticFallback = true)
		{
			ON_SCOPE_EXIT
			{
				if (!OutError.IsEmpty()) LocateGraphItemFailure(Blueprint, Input, OutError, TEXT("node_resolve"));
			};
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
			const bool bUniqueEvent = OutKind == TEXT("event")
				|| OutKind == TEXT("custom_event")
				|| OutKind == TEXT("component_bound_event")
				|| OutKind == TEXT("enhanced_input_action_event");
			for (UEdGraphNode* Node : Graph.Nodes)
			{
				if (!Node) continue;
				if (bHasGuid && Node->NodeGuid == Guid)
				{
					if (GuidMatch)
					{
						OutError = NodeMutationFailure(Blueprint, *Spec, Input,
							TEXT("blueprint_node_guid_conflict"),
							TEXT("More than one node in the graph has the requested stable identity."),
							TEXT("node_resolve"), TEXT("observed_identity_matches"),
							MakeShared<FJsonValueNumber>(2), MakeShared<FJsonValueNumber>(1));
						return nullptr;
					}
					GuidMatch = Node;
				}
				if ((bAllowSemanticFallback || bUniqueEvent) && !bHasGuid && !OutKind.IsEmpty()
					&& (bAllowSemanticFallback || !Node->IsAutomaticallyPlacedGhostNode())
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
					OutError = NodeMutationFailure(Blueprint, *Spec, Input,
						TEXT("blueprint_node_kind_conflict"),
						TEXT("The node selected by stable identity has a different node type."),
						TEXT("node_resolve"), TEXT("node_type"),
						MakeShared<FJsonValueString>(GetNodeKind(GuidMatch)), MakeShared<FJsonValueString>(OutKind),
						TEXT("node_type"));
					return nullptr;
				}
				return GuidMatch;
			}
			if (FallbackMatches.Num() > 1)
			{
				OutError = NodeMutationFailure(Blueprint, *Spec, Input,
					TEXT("blueprint_node_fallback_conflict"),
					TEXT("The supplied node selectors match more than one node in the graph."),
					TEXT("node_resolve"), TEXT("matching_nodes"),
					MakeShared<FJsonValueNumber>(FallbackMatches.Num()), MakeShared<FJsonValueNumber>(1));
				return nullptr;
			}
			return FallbackMatches.Num() == 1 ? FallbackMatches[0] : nullptr;
		}

		struct FRemovalPlan
		{
			TSet<const UEdGraph*> Graphs;
			TSet<const UEdGraphNode*> Nodes;
			TMap<FString, TMap<FString, UEdGraphNode*>> NodeRecords;
			TMap<FName, FGuid> Members;
			TMap<FName, FGuid> MemberTypeChanges;
		};

		bool CollectRemovalPlan(UBlueprint& Blueprint, const TSharedPtr<const FJsonObject>& Extension,
			FRemovalPlan& OutPlan, FDiagnostic& OutError)
		{
			OutPlan = FRemovalPlan();
			TArray<FStableMapEntry> GraphEntries;
			bool bCollectionPresent = false;
			if (!GetObjectMap(Extension, TEXT("graphs"), GraphEntries, bCollectionPresent, OutError)) return false;
			TArray<UEdGraph*> Roots;
			for (const bool bDelegate : {false, true})
			{
				TArray<FStableMapEntry> Members;
				if (!GetObjectMap(GetObject(Extension, TEXT("members")), bDelegate ? TEXT("delegates") : TEXT("variables"), Members, bCollectionPresent, OutError)) return false;
				for (const FStableMapEntry& Entry : Members)
				{
					ON_SCOPE_EXIT
					{
						if (!OutError.IsEmpty() && OutError.Source.SourceFile.IsEmpty()
							&& OutError.Source.SourceUnit.IsEmpty() && OutError.SourceKind != TEXT("native_result")
							&& OutError.SourceKind != TEXT("command_input"))
							UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Entry.Value, TEXT("member_preflight"),
								UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint));
					};
					const auto Spec = MakeStableMapSpec(Entry, EStableMapIdentity::Member, OutError);
					EOperation Operation;
					if (!Spec || !ParseOperation(Spec, EOperation::Upsert, Operation, OutError)) return false;
					const auto* Member = ResolveMember(Blueprint, Spec, *Entry.Value, bDelegate, OutError);
					if (!OutError.IsEmpty()) return false;
					if (Operation != EOperation::Remove)
					{
						if (bDelegate || !Member || Operation == EOperation::Create) continue;
						const auto Type = GetObject(Spec, TEXT("type")); if (!Type) continue;
						FEdGraphPinType Desired;
						if (!UeAgentBlueprintAuthoringOps::BuildPinTypeFromJsonObject(ConstCastSharedPtr<FJsonObject>(Type), Desired, OutError, &Blueprint))
						{
							if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Member->VarName.ToString();
							UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError, TEXT("/type"));
							UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Entry.Value, TEXT("member_preflight"),
								UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), TEXT("type"));
							return false;
						}
						if (PinTypesEqual(Member->VarType, Desired)) continue;
						if (!Member->VarGuid.IsValid()) {
							OutError = MutationFailure(Blueprint, TEXT("blueprint_variable_type_change_forbidden"),
							TEXT("The Blueprint variable has no valid GUID, so its identity cannot be preserved across a type change."),
							TEXT("member_preflight"), Member->VarName.ToString(), TEXT("member_guid_valid"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						OutError.InternalContext = Member->VarGuid.ToString();
						LocateMutationInput(OutError, *Entry.Value, TEXT("type"));
						return false;
						}
						if (!FBlueprintEditorUtils::IsPinTypeValid(Desired)) {
							OutError = MutationFailure(Blueprint, TEXT("blueprint_variable_type_invalid"),
							TEXT("Unreal's Blueprint variable type validator rejected the requested type."),
							TEXT("member_preflight"), Member->VarName.ToString(), TEXT("type"),
							MakeShared<FJsonValueObject>(UeAgentBlueprintAuthoringOps::BuildPinTypeJsonObject(Desired, &Blueprint,
								UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint))),
							MakeShared<FJsonValueString>(TEXT("a type accepted by FBlueprintEditorUtils::IsPinTypeValid")));
						LocateMutationInput(OutError, *Entry.Value, TEXT("type"));
						return false;
						}
						if ((Desired.PinCategory == UEdGraphSchema_K2::PC_Object || Desired.PinCategory == UEdGraphSchema_K2::PC_Interface)
							&& !Cast<UClass>(Desired.PinSubCategoryObject.Get())) {
							TSharedPtr<FJsonValue> ActualClass = MakeShared<FJsonValueNull>();
						if (const UObject* Object = Desired.PinSubCategoryObject.Get())
							ActualClass = MakeShared<FJsonValueString>(UeAgentBlueprintAssetLifecycle::GetAuthoringTypeObjectPath(
								&Blueprint, *Object, UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint)));
						OutError = MutationFailure(Blueprint, TEXT("blueprint_variable_type_invalid"),
							TEXT("An object or interface variable type must identify a UClass."),
							TEXT("member_preflight"), Member->VarName.ToString(), TEXT("type/pin_subcategory_object"),
							MoveTemp(ActualClass), MakeShared<FJsonValueString>(TEXT("a UClass")));
						OutError.SemanticPointer = TEXT("/type/pin_subcategory_object");
						LocateMutationInput(OutError, *Entry.Value, TEXT("type"));
						return false;
						}
						OutPlan.MemberTypeChanges.Add(Member->VarName, Member->VarGuid);
						continue;
					}
					const TCHAR* IdentityError = bDelegate ? TEXT("blueprint_delegate_removal_identity_unresolved") : TEXT("blueprint_variable_removal_identity_unresolved");
					if (!Member || !Member->VarGuid.IsValid())
					{
						OutError = MutationFailure(Blueprint, IdentityError,
							!Member ? TEXT("The Blueprint member requested for removal was not found.")
								: TEXT("The Blueprint member requested for removal has no valid GUID."),
							TEXT("member_preflight"), Member ? Member->VarName.ToString() : FString(),
							!Member ? TEXT("member_exists") : TEXT("member_guid_valid"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						OutError.InternalContext = Entry.Key;
						LocateMutationInput(OutError, *Entry.Value, TEXT("name"));
						return false;
					}
					if (bDelegate)
					{
						UEdGraph* Signature = FBlueprintEditorUtils::GetDelegateSignatureGraphByName(&Blueprint, Member->VarName);
						if (!Signature || Signature->GetOuter() != &Blueprint)
						{
							OutError = MutationFailure(Blueprint, IdentityError,
								!Signature ? TEXT("The delegate signature graph requested for removal was not found.")
									: TEXT("The delegate signature graph is owned by a different object."),
								TEXT("member_preflight"), Member->VarName.ToString(),
								!Signature ? TEXT("signature_graph_exists") : TEXT("signature_graph_owned_by_blueprint"),
								MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
							OutError.InternalContext = Entry.Key;
							LocateMutationInput(OutError, *Entry.Value, TEXT("name"));
							return false;
						}
						Roots.AddUnique(Signature);
					}
					OutPlan.Members.Add(Member->VarName, Member->VarGuid);
				}
			}
			for (const FStableMapEntry& Entry : GraphEntries)
			{
				const auto Spec = MakeStableMapSpec(Entry, EStableMapIdentity::Graph, OutError);
				EOperation Operation;
				if (!Spec || !ParseOperation(Spec, EOperation::Upsert, Operation, OutError))
				{
					UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Entry.Value, TEXT("graph_remove"),
						UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), TEXT("operation"));
					return false;
				}
				if (Operation != EOperation::Remove) continue;
				FString Name, Kind;
				UEdGraph* Graph = ResolveGraph(Blueprint, Spec, *Entry.Value, Name, Kind, OutError);
				if (!OutError.IsEmpty()) return false;
				if (!Graph)
				{
					OutError = MutationFailure(Blueprint, TEXT("blueprint_graph_removal_identity_unresolved"),
						TEXT("The Blueprint graph requested for removal was not found."), TEXT("graph_remove"),
						Name, TEXT("graph_exists"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					OutError.InternalContext = Entry.Key;
					LocateGraphHeaderInput(Blueprint, *Entry.Value, OutError, TEXT("name"));
					return false;
				}
				if (!Graph->bAllowDeletion || (!Blueprint.FunctionGraphs.Contains(Graph) && !Blueprint.MacroGraphs.Contains(Graph)))
				{
					OutError = MutationFailure(Blueprint, TEXT("blueprint_graph_remove_forbidden"),
						!Graph->bAllowDeletion ? TEXT("The resolved Blueprint graph does not allow deletion.")
							: TEXT("The resolved graph is not in this Blueprint's function or macro graph collections."),
						TEXT("graph_remove"), Graph->GetName(),
						!Graph->bAllowDeletion ? TEXT("graph_deletion_allowed") : TEXT("owned_function_or_macro_graph"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					LocateGraphHeaderInput(Blueprint, *Entry.Value, OutError, TEXT("name"));
					return false;
				}
				Roots.AddUnique(Graph);
			}
			TArray<UEdGraph*> AllGraphs; Blueprint.GetAllGraphs(AllGraphs);
			for (UEdGraph* Root : Roots)
			{
				OutPlan.Graphs.Add(Root);
				for (UEdGraph* Graph : AllGraphs) if (Graph && Graph->IsIn(Root)) OutPlan.Graphs.Add(Graph);
			}
			for (const FStableMapEntry& Entry : GraphEntries)
			{
				const auto Spec = MakeStableMapSpec(Entry, EStableMapIdentity::Graph, OutError);
				TArray<FStableMapEntry> Nodes; bool bPresent = false;
				if (!GetObjectMap(Spec, TEXT("nodes"), Nodes, bPresent, OutError)) return false;
				for (const FStableMapEntry& NodeEntry : Nodes)
				{
					const auto NodeSpec = MakeStableMapSpec(NodeEntry, EStableMapIdentity::Node, OutError);
					EOperation Operation;
					if (!NodeSpec || !ParseOperation(NodeSpec, EOperation::Upsert, Operation, OutError))
					{
						LocateGraphItemFailure(Blueprint, *NodeEntry.Value, OutError, TEXT("node_remove"));
						return false;
					}
					if (Operation != EOperation::Remove) continue;
					FString Name, Kind;
					UEdGraph* Graph = ResolveGraph(Blueprint, Spec, *Entry.Value, Name, Kind, OutError);
					if (!OutError.IsEmpty()) return false;
					if (!Graph)
					{
						OutError = MutationFailure(Blueprint, TEXT("blueprint_node_removal_graph_unresolved"),
							TEXT("The Blueprint graph required for node removal was not found."), TEXT("graph_remove"),
							Name, TEXT("graph_exists"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						OutError.InternalContext = Entry.Key;
						LocateGraphHeaderInput(Blueprint, *Entry.Value, OutError, TEXT("name"));
						return false;
					}
					FString NodeKind;
					UEdGraphNode* Node = ResolveNode(Blueprint, *Graph, NodeSpec, *NodeEntry.Value, NodeKind, OutError);
					if (!OutError.IsEmpty()) return false;
					if (!Node)
					{
						OutError = NodeMutationFailure(Blueprint, *NodeSpec, *NodeEntry.Value,
							TEXT("blueprint_node_removal_identity_unresolved"),
							TEXT("The node selected for removal was not found in the graph."), TEXT("node_remove"),
							TEXT("node_exists"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						return false;
					}
					if (!Node->CanUserDeleteNode())
					{
						OutError = NodeMutationFailure(Blueprint, *NodeSpec, *NodeEntry.Value,
							TEXT("blueprint_node_remove_forbidden"), TEXT("Unreal does not allow this node to be deleted."),
							TEXT("node_remove"), TEXT("can_delete_node"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("operation"));
						return false;
					}
					OutPlan.Nodes.Add(Node);
					OutPlan.NodeRecords.FindOrAdd(Entry.Key).Add(NodeEntry.Key, Node);
				}
			}
			return true;
		}

		bool SetNodePosition(UEdGraphNode& Node, const TSharedPtr<const FJsonObject>& Spec, FDiagnostic& OutError)
		{
			if (Spec && !UeAgentAuthoringJsonFields::ValidateOptionalFields(*Spec,
				{{TEXT("pos"), EJson::Object}}, OutError)) return false;
			const TSharedPtr<const FJsonObject> Position = GetObject(Spec, TEXT("pos"));
			const auto LocateAxis = [&](const TCHAR* Axis)
			{
				OutError.PropertyPath = FString(TEXT("pos.")) + Axis;
				if (Position && Position->HasField(Axis))
				{
					OutError.Source.InternalJsonPath = FString(TEXT("/")) + Axis;
					UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Position, TEXT("node_position"), FString(), Axis);
					if (OutError.Source.SourceFile.IsEmpty() && OutError.Source.SourceUnit.IsEmpty()
						&& OutError.SourceKind != TEXT("command_input"))
						UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError, TEXT("/pos"));
				}
				else OutError.SourceKind = TEXT("native_result");
			};
			double X = Node.NodePosX;
			double Y = Node.NodePosY;
			if (Position && Position->HasField(TEXT("x")) && !TryGetNumber(Position, TEXT("x"), X))
			{
				OutError = FDiagnostic::Failure(TEXT("blueprint_node_position_number_required"),
					TEXT("The node position x value must be a JSON number."), TEXT("node_position"));
				OutError.ActualValue = Position->TryGetField(TEXT("x"));
				OutError.ActualValuePresent = OutError.ActualValue.IsValid();
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("number"));
				LocateAxis(TEXT("x"));
				return false;
			}
			if (Position && Position->HasField(TEXT("y")) && !TryGetNumber(Position, TEXT("y"), Y))
			{
				OutError = FDiagnostic::Failure(TEXT("blueprint_node_position_number_required"),
					TEXT("The node position y value must be a JSON number."), TEXT("node_position"));
				OutError.ActualValue = Position->TryGetField(TEXT("y"));
				OutError.ActualValuePresent = OutError.ActualValue.IsValid();
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("number"));
				LocateAxis(TEXT("y"));
				return false;
			}
			int32 NativeX, NativeY;
			if (!UeAgentPropertyValueCodec::TryConvertInt32(X, NativeX, OutError, true))
			{
				LocateAxis(TEXT("x"));
				return false;
			}
			if (!UeAgentPropertyValueCodec::TryConvertInt32(Y, NativeY, OutError, true))
			{
				LocateAxis(TEXT("y"));
				return false;
			}
			Node.NodePosX = NativeX; Node.NodePosY = NativeY;
			return true;
		}

		bool InitializeNode(UEdGraph& Graph, UEdGraphNode& Node, const TSharedPtr<const FJsonObject>& Spec, FDiagnostic& OutError)
		{
			if (!SetNodePosition(Node, Spec, OutError)) return false;
			Graph.AddNode(&Node, true, false);
			Node.CreateNewGuid();
			Node.SetFlags(RF_Transactional);
			// Action-menu placement duplicates an already prepared node template, so
			// PostPlacedNewNode can rely on its default pins. This authoring path
			// constructs a raw node instead; prepare that same interface first.
			Node.AllocateDefaultPins();
			Node.PostPlacedNewNode();
			UEdGraphSchema_K2::SetNodeMetaData(&Node, FNodeMetadata::DefaultGraphNode);
			return true;
		}

		UK2Node_CallFunction* CreateCallFunctionNode(
			UBlueprint& Blueprint,
			UEdGraph& Graph,
			UFunction& Function,
			const TSharedPtr<const FJsonObject>& Spec,
			const FJsonObject& Input,
			FDiagnostic& OutError)
		{
			UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(&Graph);
			if (!Node)
			{
				OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_call_function_node_create_failed"),
					TEXT("Unreal did not allocate the requested function call node."), TEXT("node_create"),
					TEXT("node_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.InternalContext += TEXT(" ") + Function.GetPathName();
				return nullptr;
			}
			Node->SetFromFunction(&Function);
			if (!InitializeNode(Graph, *Node, Spec, OutError)) return nullptr;
			return Node;
		}

		UFunction* ResolveCallFunctionTarget(
			UBlueprint& Blueprint,
			const TSharedPtr<const FJsonObject>& Spec,
			const FJsonObject& Input,
			FString& OutOwnerPath,
			FString& OutFunctionName,
			FDiagnostic& OutError)
		{
			OutOwnerPath.Reset();
			OutFunctionName.Reset();
			FString FunctionGraphName;
			if (TryGetString(Spec, TEXT("function_graph"), FunctionGraphName))
			{
				FunctionGraphName.TrimStartAndEndInline();
				if (FunctionGraphName.IsEmpty()
					|| Spec->HasField(TEXT("function_name"))
					|| Spec->HasField(TEXT("function_owner_class")))
				{
					const FString Field = FunctionGraphName.IsEmpty() ? TEXT("function_graph")
						: Spec->HasField(TEXT("function_name")) ? TEXT("function_name") : TEXT("function_owner_class");
					OutError = MutationInputFailure(Blueprint, Input, TEXT("blueprint_function_graph_reference_invalid"),
						FunctionGraphName.IsEmpty() ? TEXT("A local function graph reference must have a non-empty name.")
							: TEXT("A local function_graph reference cannot be combined with function_name or function_owner_class."),
						TEXT("node_reference"), FunctionGraphName, Field,
						MakeShared<FJsonValueString>(FunctionGraphName.IsEmpty() ? TEXT("a non-empty function graph name")
							: TEXT("function_graph without function_name or function_owner_class")));
					return nullptr;
				}
				UEdGraph* FunctionGraph = nullptr;
				for (UEdGraph* Candidate : Blueprint.FunctionGraphs)
				{
					if (Candidate && Candidate->GetName() == FunctionGraphName)
					{
						FunctionGraph = Candidate;
						break;
					}
				}
				if (!FunctionGraph)
				{
					OutError = NodeMutationFailure(Blueprint, *Spec, Input,
						TEXT("blueprint_function_graph_reference_not_found"),
						TEXT("The referenced local function graph was not found in this Blueprint."),
						TEXT("node_reference"), TEXT("function_graph_exists"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("function_graph"));
					return nullptr;
				}
				UClass* OwnerClass = Blueprint.SkeletonGeneratedClass
					? Blueprint.SkeletonGeneratedClass.Get() : Blueprint.GeneratedClass.Get();
				UFunction* Function = OwnerClass
					? OwnerClass->FindFunctionByName(FName(*FunctionGraphName)) : nullptr;
				if (!Function)
				{
					OutError = NodeMutationFailure(Blueprint, *Spec, Input,
						TEXT("blueprint_function_graph_reference_unsettled"),
						!OwnerClass ? TEXT("The Blueprint has no generated class for the referenced function graph.")
							: TEXT("The referenced function graph has no function in the Blueprint generated class."),
						TEXT("node_reference"), !OwnerClass ? TEXT("generated_class_available") : TEXT("compiled_function_available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("function_graph"));
					return nullptr;
				}
				OutOwnerPath = OwnerClass->GetPathName();
				OutFunctionName = FunctionGraphName;
				return Function;
			}

			const bool bHasOwnerClass = TryGetString(Spec, TEXT("function_owner_class"), OutOwnerPath);
			if (!bHasOwnerClass || !TryGetString(Spec, TEXT("function_name"), OutFunctionName))
			{
				const FString Field = bHasOwnerClass ? TEXT("function_name") : TEXT("function_owner_class");
				OutError = MutationInputFailure(Blueprint, Input, TEXT("blueprint_function_identity_missing"),
					bHasOwnerClass ? TEXT("A function call requires a string function_name.")
						: TEXT("A function call requires a string function_owner_class."),
					TEXT("node_reference"), OutFunctionName, Field, MakeShared<FJsonValueString>(TEXT("string")));
				return nullptr;
			}
			UClass* OwnerClass = UeAgentBlueprintAuthoringOps::ResolveClassByPath(OutOwnerPath);
			UFunction* Function = OwnerClass
				? OwnerClass->FindFunctionByName(FName(*OutFunctionName)) : nullptr;
			if (!Function)
			{
				OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_function_not_found"),
					!OwnerClass ? TEXT("The owner class for the requested function could not be resolved.")
						: TEXT("The requested function was not found in its owner class."),
					TEXT("node_reference"), !OwnerClass ? TEXT("owner_class_available") : TEXT("function_exists"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
					!OwnerClass ? TEXT("function_owner_class") : TEXT("function_name"));
				OutError.InternalContext += TEXT(" ") + OutOwnerPath + TEXT(".") + OutFunctionName;
			}
			return Function;
		}

		bool ApplyCallFunctionTarget(
			UBlueprint& Blueprint,
			UK2Node_CallFunction& Node,
			const TSharedPtr<const FJsonObject>& Spec,
			const FJsonObject& Input,
			FDiagnostic& OutError)
		{
			FString OwnerPath;
			FString FunctionName;
			UFunction* Function = ResolveCallFunctionTarget(
				Blueprint, Spec, Input, OwnerPath, FunctionName, OutError);
			if (!Function)
			{
				return false;
			}
			if (Node.GetTargetFunction() != Function)
			{
				Node.SetFromFunction(Function);
				Node.ReconstructNode();
			}
			return true;
		}

		UEdGraph* ResolveMacroGraph(
			UBlueprint& Blueprint,
			const TSharedPtr<const FJsonObject>& Spec,
			const FJsonObject& Input,
			FDiagnostic& OutError,
			const UK2Node_MacroInstance* Existing = nullptr)
		{
			FString MacroGraphName;
			if (!TryGetString(Spec, TEXT("macro_graph"), MacroGraphName))
			{
				OutError = MutationInputFailure(Blueprint, Input, TEXT("blueprint_macro_graph_reference_missing"),
					TEXT("A macro instance requires a string macro_graph reference."), TEXT("node_reference"),
					FString(), TEXT("macro_graph"), MakeShared<FJsonValueString>(TEXT("string")));
				return nullptr;
			}
			MacroGraphName.TrimStartAndEndInline();
			if (MacroGraphName.Contains(TEXT(".")) || MacroGraphName.Contains(TEXT(":")))
			{
				UEdGraph* Referenced = FindObject<UEdGraph>(nullptr, *MacroGraphName);
				if (!Referenced) Referenced = LoadObject<UEdGraph>(nullptr, *MacroGraphName);
				if (!Referenced)
				{
					OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_macro_graph_reference_not_found"),
						TEXT("The referenced macro graph could not be found or loaded."), TEXT("node_reference"),
						TEXT("macro_graph_exists"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
						TEXT("macro_graph"));
					return nullptr;
				}
				const UBlueprint* Owner = Referenced->GetTypedOuter<UBlueprint>();
				const FString Target = UeAgentAuthoringAssetIdentity::NormalizePackagePath(
					UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint));
				if (Owner && Owner != &Blueprint && Owner->GetOutermost()->GetName() == Target)
				{
					for (UEdGraph* Local : Blueprint.MacroGraphs)
						if (Local && Referenced->GraphGuid.IsValid() && Local->GraphGuid == Referenced->GraphGuid)
							return Local;
					MacroGraphName = Referenced->GetName();
				}
				else
				{
					if (!Referenced->GetSchema() || Referenced->GetSchema()->GetGraphType(Referenced) != GT_Macro)
					{
						OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_macro_graph_reference_not_macro"),
							!Referenced->GetSchema() ? TEXT("The referenced graph has no schema to identify it as a macro graph.")
								: TEXT("The referenced graph is not a macro graph."),
							TEXT("node_reference"), !Referenced->GetSchema() ? TEXT("graph_schema_available") : TEXT("is_macro_graph"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("macro_graph"));
						OutError.InternalContext += TEXT(" ") + Referenced->GetPathName();
						return nullptr;
					}
					return Referenced;
				}
			}
			UEdGraph* Match = nullptr;
			for (UEdGraph* Candidate : Blueprint.MacroGraphs)
			{
				if (!Candidate || Candidate->GetName() != MacroGraphName)
				{
					continue;
				}
				if (Match)
				{
					OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_macro_graph_reference_conflict"),
						TEXT("More than one local macro graph has the requested name."), TEXT("node_reference"),
						TEXT("observed_macro_graph_matches"), MakeShared<FJsonValueNumber>(2), MakeShared<FJsonValueNumber>(1),
						TEXT("macro_graph"));
					return nullptr;
				}
				Match = Candidate;
			}
			if (!Match && Existing && Existing->GetMacroGraph()
				&& Existing->GetMacroGraph()->GetName() == MacroGraphName)
			{
				// Older exports omitted the library. Preserve the actual existing
				// binding for an unchanged unqualified reference, never guess a library.
				Match = Existing->GetMacroGraph();
			}
			if (!Match)
			{
				OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_macro_graph_reference_not_found"),
					TEXT("The referenced local macro graph was not found and no unchanged existing binding matches it."),
					TEXT("node_reference"), TEXT("macro_graph_exists"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("macro_graph"));
			}
			return Match;
		}

		FMulticastDelegateProperty* ResolveLocalDelegateProperty(
			UBlueprint& Blueprint,
			const TSharedPtr<const FJsonObject>& Spec,
			const FJsonObject& Input,
			FDiagnostic& OutError)
		{
			FString DelegateName;
			if (!TryGetString(Spec, TEXT("delegate_member"), DelegateName))
			{
				OutError = MutationInputFailure(Blueprint, Input, TEXT("blueprint_delegate_member_reference_missing"),
					TEXT("A delegate call requires a string delegate_member reference."), TEXT("node_reference"),
					FString(), TEXT("delegate_member"), MakeShared<FJsonValueString>(TEXT("string")));
				return nullptr;
			}
			DelegateName.TrimStartAndEndInline();
			const int32 VariableIndex = FBlueprintEditorUtils::FindNewVariableIndex(
				&Blueprint, FName(*DelegateName));
			if (VariableIndex == INDEX_NONE
				|| Blueprint.NewVariables[VariableIndex].VarType.PinCategory
					!= UEdGraphSchema_K2::PC_MCDelegate)
			{
				OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_delegate_member_reference_not_found"),
					VariableIndex == INDEX_NONE ? TEXT("The referenced delegate member was not found in this Blueprint.")
						: TEXT("The referenced member is not a multicast delegate."),
					TEXT("node_reference"), VariableIndex == INDEX_NONE ? TEXT("delegate_member_exists") : TEXT("pin_category"),
					VariableIndex == INDEX_NONE ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false))
						: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Blueprint.NewVariables[VariableIndex].VarType.PinCategory.ToString())),
					VariableIndex == INDEX_NONE ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true))
						: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(UEdGraphSchema_K2::PC_MCDelegate.ToString())),
					TEXT("delegate_member"));
				return nullptr;
			}
			UClass* OwnerClass = Blueprint.SkeletonGeneratedClass
				? Blueprint.SkeletonGeneratedClass.Get() : Blueprint.GeneratedClass.Get();
			FMulticastDelegateProperty* Property = OwnerClass
				? FindFProperty<FMulticastDelegateProperty>(OwnerClass, FName(*DelegateName))
				: nullptr;
			if (!Property)
			{
				OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_delegate_member_reference_unsettled"),
					!OwnerClass ? TEXT("The Blueprint has no generated class for the referenced delegate.")
						: TEXT("The referenced delegate has no compiled multicast delegate property."),
					TEXT("node_reference"), !OwnerClass ? TEXT("generated_class_available") : TEXT("compiled_delegate_available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("delegate_member"));
			}
			return Property;
		}

		UFunction* ResolveLocalCustomEventFunction(
			UBlueprint& Blueprint,
			const TSharedPtr<const FJsonObject>& Spec,
			const FJsonObject& Input,
			FDiagnostic& OutError)
		{
			FString EventName;
			if (!TryGetString(Spec, TEXT("custom_event"), EventName))
			{
				OutError = MutationInputFailure(Blueprint, Input, TEXT("blueprint_custom_event_reference_missing"),
					TEXT("A custom event call requires a string custom_event reference."), TEXT("node_reference"),
					FString(), TEXT("custom_event"), MakeShared<FJsonValueString>(TEXT("string")));
				return nullptr;
			}
			TArray<UK2Node_CustomEvent*> Events;
			FBlueprintEditorUtils::GetAllNodesOfClass(&Blueprint, Events);
			const bool bExists = Events.ContainsByPredicate([&EventName](const UK2Node_CustomEvent* Event)
			{
				return Event && Event->CustomFunctionName.ToString() == EventName;
			});
			UClass* OwnerClass = Blueprint.SkeletonGeneratedClass
				? Blueprint.SkeletonGeneratedClass.Get() : Blueprint.GeneratedClass.Get();
			UFunction* Function = bExists && OwnerClass
				? OwnerClass->FindFunctionByName(FName(*EventName)) : nullptr;
			if (!Function)
			{
				OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_custom_event_reference_unsettled"),
					!bExists ? TEXT("The referenced custom event was not found in this Blueprint.")
						: !OwnerClass ? TEXT("The Blueprint has no generated class for the referenced custom event.")
						: TEXT("The referenced custom event has no function in the Blueprint generated class."),
					TEXT("node_reference"), !bExists ? TEXT("custom_event_exists")
						: !OwnerClass ? TEXT("generated_class_available") : TEXT("compiled_function_available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("custom_event"));
			}
			return Function;
		}

		UEdGraphPin* ResolveEdgePin(
			UEdGraphNode& Node,
			const TSharedPtr<const FJsonObject>& Endpoint,
			const FString& Name,
			int32 Occurrence,
			FDiagnostic& OutError);

		UEdGraphNode* CreateNode(
			UBlueprint& Blueprint,
			UEdGraph& Graph,
			const TSharedPtr<const FJsonObject>& Spec,
			const FJsonObject& Input,
			const FString& Kind,
			const TMap<FString, UEdGraphNode*>& NodeIds,
			const bool bAllowScratchCandidateRebind,
			FDiagnostic& OutError)
		{
			if (Kind == TEXT("macro_instance"))
			{
				UEdGraph* MacroGraph = ResolveMacroGraph(Blueprint, Spec, Input, OutError);
				if (!MacroGraph)
				{
					return nullptr;
				}
				UK2Node_MacroInstance* Node = NewObject<UK2Node_MacroInstance>(&Graph);
				if (!Node)
				{
					OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_macro_instance_create_failed"),
					TEXT("Unreal did not allocate the requested macro instance node."), TEXT("node_create"), TEXT("node_created"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("macro_graph"));
					return nullptr;
				}
				Node->SetMacroGraph(MacroGraph);
				if (!InitializeNode(Graph, *Node, Spec, OutError)) return nullptr;
				return Node;
			}
			if (Kind == TEXT("call_delegate"))
			{
				FMulticastDelegateProperty* Property = ResolveLocalDelegateProperty(
					Blueprint, Spec, Input, OutError);
				if (!Property)
				{
					return nullptr;
				}
				UK2Node_CallDelegate* Node = NewObject<UK2Node_CallDelegate>(&Graph);
				if (!Node)
				{
					OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_call_delegate_create_failed"),
					TEXT("Unreal did not allocate the requested delegate call node."), TEXT("node_create"), TEXT("node_created"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("delegate_member"));
					return nullptr;
				}
				Node->SetFromProperty(Property, true, Property->GetOwnerClass());
				if (!InitializeNode(Graph, *Node, Spec, OutError)) return nullptr;
				return Node;
			}
			if (Kind == TEXT("call_custom_event"))
			{
				UFunction* Function = ResolveLocalCustomEventFunction(
					Blueprint, Spec, Input, OutError);
				return Function
					? CreateCallFunctionNode(Blueprint, Graph, *Function, Spec, Input, OutError)
					: nullptr;
			}
			if (Kind == TEXT("native_action"))
			{
				for (const TCHAR* Field : {TEXT("event_name"), TEXT("signature")}) {
					if (!Spec->HasField(Field)) continue;
					OutError = MutationDocumentFailure(Input, TEXT("blueprint_action_event_declaration_unsupported"),
						TEXT("Native menu creation uses the action's event declaration. Edit the created event to change its name or signature."),
						Field, Spec->TryGetField(Field), MakeShared<FJsonValueString>(TEXT("omit during menu creation")));
					return nullptr;
				}
				const TSharedPtr<const FJsonObject> Candidate =
					GetObject(Spec, TEXT("candidate"));
				FString Name;
				TryGetString(Candidate, TEXT("name"), Name);
				Name.TrimStartAndEndInline();
				const auto CandidateInputFailure = [&](const FString& ParentPointer,
					const TCHAR* Code, const FString& Reason, const FString& Field,
					TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
				{
					const FString SourceField = ParentPointer.StartsWith(TEXT("/candidate_context"))
						? TEXT("candidate_context") : TEXT("candidate");
					const FString Pointer = ParentPointer + TEXT("/") + Field.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
					FDiagnostic Error = MutationDocumentFailure(*Spec, Code, Reason, SourceField,
						MoveTemp(Actual), MoveTemp(Expected), Pointer);
					Error.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint);
					Error.CandidateName = Name;
					Error.PropertyPath = Field;
					return Error;
				};
				if (Name.IsEmpty())
				{
					if (Candidate.IsValid())
					{
						OutError = CandidateInputFailure(TEXT("/candidate"),
							TEXT("blueprint_action_candidate_selector_required"),
							TEXT("A native action candidate requires a non-empty string name."), TEXT("name"),
							Candidate->TryGetField(TEXT("name")), MakeShared<FJsonValueString>(TEXT("a non-empty candidate name")));
					}
					else
					{
						OutError = MutationInputFailure(Blueprint, Input, TEXT("blueprint_action_candidate_selector_required"),
							TEXT("A native action requires a candidate object containing a non-empty name."),
							TEXT("node_create"), Kind, TEXT("candidate"), MakeShared<FJsonValueString>(TEXT("an object with a non-empty name")));
					}
					return nullptr;
				}
				if (Candidate->HasField(TEXT("id")))
				{
					OutError = CandidateInputFailure(TEXT("/candidate"),
						TEXT("blueprint_action_candidate_public_id_retired"),
						TEXT("Candidate id is retired; use candidate.name and optional candidate.select."),
						TEXT("id"), MakeShared<FJsonValueString>(TEXT("id")),
						MakeShared<FJsonValueString>(TEXT("name and optional select")));
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
					OutError = MutationInputFailure(Blueprint, Input, TEXT("blueprint_surface_candidate_context_invalid"),
						TEXT("candidate_context must be an object."), TEXT("node_create"), Kind, TEXT("candidate_context"),
						MakeShared<FJsonValueString>(TEXT("object")));
					return nullptr;
				}
				if (CandidateContext.IsValid())
				{
					if (CandidateContext->HasField(TEXT("node_guid"))
						|| CandidateContext->HasField(TEXT("pin_guid"))
						|| CandidateContext->HasField(TEXT("pin_name")))
					{
						const FString Field = CandidateContext->HasField(TEXT("node_guid")) ? TEXT("node_guid")
							: CandidateContext->HasField(TEXT("pin_guid")) ? TEXT("pin_guid") : TEXT("pin_name");
						OutError = CandidateInputFailure(TEXT("/candidate_context"),
							TEXT("blueprint_surface_candidate_context_machine_identity_retired"),
							TEXT("Machine identity fields are retired in candidate_context; use node and pin aliases."),
							Field, MakeShared<FJsonValueString>(Field), MakeShared<FJsonValueString>(TEXT("node and pin aliases")));
						return nullptr;
					}
					FString ContextNodeId;
					FString ContextNodeAlias;
					FString ContextPinAlias;
					TryGetString(CandidateContext, TEXT("node"), ContextNodeId);
					TryGetString(CandidateContext, TEXT("alias"), ContextNodeAlias);
					TryGetString(CandidateContext, TEXT("pin"), ContextPinAlias);
					if (!ContextNodeId.IsEmpty())
					{
						FDiagnostic ContextResolveError;
						UEdGraphNode* ContextNode = ResolveGraphNodeReference(
							Graph, NodeIds, ContextNodeId, *CandidateContext, ContextResolveError);
						if (ContextNode == nullptr)
						{
							OutError = MoveTemp(ContextResolveError);
							if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = ContextNodeAlias;
							return nullptr;
						}
						Params.NodeGuid = ContextNode->NodeGuid.ToString(
							EGuidFormats::DigitsWithHyphens);
						if (!ContextPinAlias.IsEmpty())
						{
							UEdGraphPin* ContextPin = ResolveEdgePin(
								*ContextNode,
								CandidateContext,
								ContextPinAlias,
								0,
								OutError);
							if (!ContextPin)
							{
								if (OutError.Source.SourceFile.IsEmpty() && OutError.Source.SourceUnit.IsEmpty())
								{
									if (OutError.SourceKind == TEXT("native_result"))
									LocateMutationInput(OutError, *Spec, TEXT("candidate_context"), TEXT("/candidate_context/pin"));
									else
									{
										LocateGraphItemFailure(Blueprint, *CandidateContext, OutError);
										if (OutError.Source.SourceFile.IsEmpty() && OutError.Source.SourceUnit.IsEmpty()
											&& OutError.SourceKind != TEXT("command_input"))
											UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError, TEXT("/candidate_context"));
									}
								}
								return nullptr;
							}
							Params.PinGuid = ContextPin->PinId.ToString(
								EGuidFormats::DigitsWithHyphens);
							Params.PinName = ContextPin->PinName.ToString();
						}
					}
				}
				const TSharedPtr<const FJsonObject> Select =
					GetObject(Candidate, TEXT("select"));
				if (Candidate->HasField(TEXT("select")) && !Select.IsValid())
				{
					OutError = CandidateInputFailure(TEXT("/candidate"), TEXT("candidate_selector_invalid"),
						TEXT("candidate.select must be an object."), TEXT("select"), Candidate->TryGetField(TEXT("select")),
						MakeShared<FJsonValueString>(TEXT("object")));
					return nullptr;
				}
				if (Select.IsValid())
				{
					if (Select->Values.IsEmpty() || Select->Values.Num() > 2)
					{
						OutError = CandidateInputFailure(TEXT("/candidate"), TEXT("candidate_selector_invalid"),
							TEXT("candidate.select must contain one or two selector fields."), TEXT("select"),
							MakeShared<FJsonValueNumber>(Select->Values.Num()),
							MakeShared<FJsonValueString>(TEXT("one or two selector fields")));
						return nullptr;
					}
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Selector :
						Select->Values)
					{
						if (!Selector.Value.IsValid()
							|| Selector.Value->Type != EJson::String
							|| Selector.Value->AsString().IsEmpty())
						{
							OutError = CandidateInputFailure(TEXT("/candidate/select"), TEXT("candidate_selector_invalid"),
								TEXT("Each candidate selector value must be a non-empty string."), Selector.Key, Selector.Value,
								MakeShared<FJsonValueString>(TEXT("a non-empty string")));
							return nullptr;
						}
						Params.CandidateSelect.Add(
							Selector.Key, Selector.Value->AsString());
					}
				}
				for (const TCHAR* Field : {
					TEXT("function_name"),
					TEXT("event_name"),
					TEXT("variable_name"),
					TEXT("node_type"),
					TEXT("node_class"),
					TEXT("operation_name"), TEXT("macro_graph")})
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
				FDiagnostic PerformError;
				if (!UeAgentPaletteSearch::TryPerformBlueprintActionCandidate(
						Params,
						Name,
						FString(),
						FVector2f(
							static_cast<float>(X),
							static_cast<float>(Y)),
						Node,
						ResolvedCandidate,
						PerformError))
				{
					OutError = MoveTemp(PerformError);
					if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Name;
					if (OutError.Source.SourceFile.IsEmpty() && OutError.Source.SourceUnit.IsEmpty()
						&& OutError.SourceKind == TEXT("native_result"))
						LocateMutationInput(OutError, *Spec, TEXT("candidate"), TEXT("/candidate/name"));
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
					OutError = MutationInputFailure(Blueprint, Input, TEXT("blueprint_event_name_missing"),
					TEXT("An event node requires a non-empty string event_name."), TEXT("node_create"), Kind, TEXT("event_name"),
					MakeShared<FJsonValueString>(TEXT("a non-empty event name")));
					return nullptr;
				}
				TryGetString(Spec, TEXT("event_class"), EventClassPath);
				UClass* EventClass = UeAgentBlueprintAuthoringOps::ResolveClassByPath(EventClassPath);
				if (!EventClass)
				{
					OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_event_class_invalid"),
					TEXT("The owner class for the requested event could not be resolved."), TEXT("node_create"), TEXT("event_class_available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("event_class"));
					return nullptr;
				}
				int32 NodePosY = 0;
				const auto ExistingNodes = Graph.Nodes;
				for (UEdGraphNode* ExistingNode : ExistingNodes)
				{
					UK2Node_Event* Ghost = Cast<UK2Node_Event>(ExistingNode);
					if (Ghost && Ghost->IsAutomaticallyPlacedGhostNode()
						&& Ghost->EventReference.GetMemberName() == FName(*EventName))
						FBlueprintEditorUtils::RemoveNode(&Blueprint, Ghost, true);
				}
				UK2Node_Event* Node = FKismetEditorUtilities::AddDefaultEventNode(
					&Blueprint,
					&Graph,
					FName(*EventName),
					EventClass,
					NodePosY);
				if (!Node)
				{
					OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_event_node_create_failed"),
					TEXT("Unreal did not create the requested default event node."), TEXT("node_create"), TEXT("node_created"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("event_name"));
					return nullptr;
				}
				// Explicit authoring creates a real event, not the disabled ghost
				// placeholder produced by AddDefaultEventNode for fresh blueprints.
				Node->SetEnabledState(ENodeEnabledState::Enabled, true);
				if (!SetNodePosition(*Node, Spec, OutError)) return nullptr;
				return Node;
			}
			if (Kind == TEXT("custom_event"))
			{
				FString EventName;
				if (!TryGetString(Spec, TEXT("event_name"), EventName) || EventName.IsEmpty())
				{
					OutError = MutationInputFailure(Blueprint, Input, TEXT("blueprint_custom_event_name_missing"),
					TEXT("A custom event node requires a non-empty string event_name."), TEXT("node_create"), Kind, TEXT("event_name"),
					MakeShared<FJsonValueString>(TEXT("a non-empty custom event name")));
					return nullptr;
				}
				UK2Node_CustomEvent* Node = NewObject<UK2Node_CustomEvent>(&Graph);
				if (!Node)
				{
					OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_custom_event_create_failed"),
					TEXT("Unreal did not allocate the requested custom event node."), TEXT("node_create"), TEXT("node_created"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("event_name"));
					return nullptr;
				}
				Node->CustomFunctionName = FName(*EventName);
				if (!InitializeNode(Graph, *Node, Spec, OutError)) return nullptr;
				return Node;
			}
			if (Kind == TEXT("component_bound_event"))
			{
				FString ComponentName;
				FString DelegateName;
				FString OwnerClassPath = TEXT("/Script/Engine.PrimitiveComponent");
				const bool bHasComponentName = TryGetString(Spec, TEXT("component_name"), ComponentName);
				if (!bHasComponentName || !TryGetString(Spec, TEXT("delegate_property_name"), DelegateName))
				{
					OutError = MutationInputFailure(Blueprint, Input, TEXT("blueprint_component_event_identity_missing"),
						bHasComponentName ? TEXT("A component-bound event requires a string delegate_property_name.")
							: TEXT("A component-bound event requires a string component_name."),
						TEXT("node_create"), Kind, bHasComponentName ? TEXT("delegate_property_name") : TEXT("component_name"),
						MakeShared<FJsonValueString>(TEXT("string")));
					return nullptr;
				}
				TryGetString(Spec, TEXT("delegate_owner_class"), OwnerClassPath);
				UClass* OwnerClass = UeAgentBlueprintAuthoringOps::ResolveClassByPath(OwnerClassPath);
				if (!OwnerClass)
				{
					OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_delegate_owner_class_invalid"),
					TEXT("The owner class for the component event delegate could not be resolved."), TEXT("node_create"), TEXT("delegate_owner_class_available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("delegate_owner_class"));
					return nullptr;
				}
				if (!Blueprint.SkeletonGeneratedClass && !CompileBlueprint(Blueprint, OutError)) return nullptr;
				FObjectProperty* ComponentProperty = FindFProperty<FObjectProperty>(
					Blueprint.SkeletonGeneratedClass,
					FName(*ComponentName));
				if (!ComponentProperty)
				{
					OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_component_event_property_not_found"),
					TEXT("The referenced component has no object property in the Blueprint skeleton class."), TEXT("node_create"), TEXT("component_property_exists"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("component_name"));
					return nullptr;
				}
				UK2Node_ComponentBoundEvent* Node = const_cast<UK2Node_ComponentBoundEvent*>(
					FKismetEditorUtilities::FindBoundEventForComponent(
						&Blueprint,
						FName(*DelegateName),
						FName(*ComponentName)));
				bool bDelegatePropertyFound = true;
				if (!Node)
				{
					FMulticastDelegateProperty* DelegateProperty = FindFProperty<FMulticastDelegateProperty>(
						OwnerClass, FName(*DelegateName));
					bDelegatePropertyFound = DelegateProperty != nullptr;
					if (DelegateProperty)
					{
						// Use the native creation primitive without its interactive
						// wrapper's last-edited-graph selection and BringKismet call.
						Node = FEdGraphSchemaAction_K2NewNode::SpawnNode<UK2Node_ComponentBoundEvent>(
							&Graph, Graph.GetGoodPlaceForNewNode(), EK2NewNodeFlags::None,
							[ComponentProperty, DelegateProperty](UK2Node_ComponentBoundEvent* NewNode)
							{
								NewNode->InitializeComponentBoundEventParams(ComponentProperty, DelegateProperty);
							});
					}
				}
				if (!Node || Node->GetGraph() != &Graph)
				{
					OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_component_event_create_failed"),
						!bDelegatePropertyFound ? TEXT("The requested multicast delegate property was not found in its owner class.")
							: !Node ? TEXT("Unreal did not spawn the requested component-bound event node.")
							: TEXT("The component-bound event belongs to a different graph."),
						TEXT("node_create"), !bDelegatePropertyFound ? TEXT("delegate_property_exists")
							: !Node ? TEXT("node_created") : TEXT("node_graph_matches"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
						!bDelegatePropertyFound ? TEXT("delegate_property_name") : TEXT("component_name"));
					return nullptr;
				}
				if (!SetNodePosition(*Node, Spec, OutError)) return nullptr;
				return Node;
			}
			if (Kind == TEXT("enhanced_input_action_event"))
			{
				FString Path;
				if (!TryGetString(Spec, TEXT("input_action"), Path))
				{
					OutError = MutationInputFailure(Blueprint, Input, TEXT("blueprint_input_action_missing"),
					TEXT("An enhanced input action event requires a string input_action asset reference."), TEXT("node_create"), Kind, TEXT("input_action"),
					MakeShared<FJsonValueString>(TEXT("string")));
					return nullptr;
				}
				UObject* LoadedAction = UeAgentBlueprintAuthoringOps::LoadAssetObject(Path);
				UInputAction* Action = Cast<UInputAction>(LoadedAction);
				if (!Action)
				{
					if (!LoadedAction)
					{
						OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_input_action_not_found"),
					TEXT("The input_action asset could not be found or loaded."), TEXT("node_create"), TEXT("input_action_available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("input_action"));
					}
					else
					{
						OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_input_action_not_found"),
							TEXT("The loaded input_action asset is not an InputAction."), TEXT("node_create"), TEXT("asset_class"),
							MakeShared<FJsonValueString>(UeAgentBlueprintAssetLifecycle::GetAuthoringTypeObjectPath(&Blueprint, *LoadedAction->GetClass())),
							MakeShared<FJsonValueString>(UeAgentBlueprintAssetLifecycle::GetAuthoringTypeObjectPath(&Blueprint, *UInputAction::StaticClass())),
							TEXT("input_action"));
					}
					return nullptr;
				}
				UK2Node_EnhancedInputAction* Node = NewObject<UK2Node_EnhancedInputAction>(&Graph);
				if (!Node)
				{
					OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_input_action_node_create_failed"),
					TEXT("Unreal did not allocate the requested enhanced input action event node."), TEXT("node_create"), TEXT("node_created"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("input_action"));
					return nullptr;
				}
				Node->InputAction = Action;
				if (!InitializeNode(Graph, *Node, Spec, OutError)) return nullptr;
				Node->PostReconstructNode();
				return Node;
			}
			if (Kind == TEXT("enhanced_input_get_local_player_subsystem"))
			{
				UFunction* Function = USubsystemBlueprintLibrary::StaticClass()->FindFunctionByName(
					TEXT("GetLocalPlayerSubSystemFromPlayerController"));
				if (!Function)
				{
					OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_subsystem_function_not_found"),
					TEXT("The subsystem library does not expose GetLocalPlayerSubSystemFromPlayerController."), TEXT("node_create"), TEXT("subsystem_function_available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("node_type"));
					return nullptr;
				}
				UK2Node_CallFunction* Node = CreateCallFunctionNode(Blueprint, Graph, *Function, Spec, Input, OutError);
				if (!Node) return nullptr;
				UEdGraphPin* ClassPin = UeAgentBlueprintAuthoringOps::FindNodePinByName(Node, TEXT("Class"));
				if (!ClassPin)
				{
					OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_subsystem_class_pin_invalid"),
					TEXT("The generated subsystem function call does not expose its Class pin."), TEXT("node_create"), TEXT("pins.Class.exists"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("node_type"));
					return nullptr;
				}
				if (!SetPinDefaultObject(*Graph.GetSchema(), *ClassPin, UEnhancedInputLocalPlayerSubsystem::StaticClass(), OutError))
				{
					LocateMutationInput(OutError, Input, TEXT("node_type"));
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
					OutError = MutationInputFailure(Blueprint, Input, TEXT("blueprint_mapping_context_missing"),
					TEXT("An add-mapping-context node requires a string mapping_context asset reference."), TEXT("node_create"), Kind, TEXT("mapping_context"),
					MakeShared<FJsonValueString>(TEXT("string")));
					return nullptr;
				}
				UObject* LoadedContext = UeAgentBlueprintAuthoringOps::LoadAssetObject(Path);
				UInputMappingContext* Context = Cast<UInputMappingContext>(LoadedContext);
				UFunction* Function = UEnhancedInputLocalPlayerSubsystem::StaticClass()->FindFunctionByName(
					TEXT("AddMappingContext"));
				if (!Context || !Function)
				{
					if (!Context && LoadedContext)
					{
						OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_mapping_context_dependency_missing"),
							TEXT("The loaded mapping_context asset is not an InputMappingContext."), TEXT("node_create"), TEXT("asset_class"),
							MakeShared<FJsonValueString>(UeAgentBlueprintAssetLifecycle::GetAuthoringTypeObjectPath(&Blueprint, *LoadedContext->GetClass())),
							MakeShared<FJsonValueString>(UeAgentBlueprintAssetLifecycle::GetAuthoringTypeObjectPath(&Blueprint, *UInputMappingContext::StaticClass())),
							TEXT("mapping_context"));
					}
					else
					{
						OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_mapping_context_dependency_missing"),
							!Context ? TEXT("The mapping_context asset could not be found or loaded.")
								: TEXT("The enhanced input subsystem does not expose AddMappingContext."),
							TEXT("node_create"), !Context ? TEXT("mapping_context_available") : TEXT("add_mapping_context_function_available"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
							!Context ? TEXT("mapping_context") : TEXT("node_type"));
					}
					return nullptr;
				}
				UK2Node_CallFunction* Node = CreateCallFunctionNode(Blueprint, Graph, *Function, Spec, Input, OutError);
				if (!Node) return nullptr;
				UEdGraphPin* ContextPin = UeAgentBlueprintAuthoringOps::FindNodePinByName(Node, TEXT("MappingContext"));
				UEdGraphPin* PriorityPin = UeAgentBlueprintAuthoringOps::FindNodePinByName(Node, TEXT("Priority"));
				UEdGraphPin* OptionsPin = UeAgentBlueprintAuthoringOps::FindNodePinByName(Node, TEXT("Options"));
				int32 NativePriority = 0;
				FString Options = TEXT("(bIgnoreAllPressedKeysUntilRelease=True,bForceImmediately=False,bNotifyUserSettings=False)");
				if (!UeAgentAuthoringJsonFields::ReadInt32Field(*Spec, TEXT("priority"), NativePriority, OutError)) return nullptr;
				TryGetString(Spec, TEXT("options_default_value"), Options);
				const UEdGraphSchema* Schema = Graph.GetSchema();
				if (!ContextPin || !PriorityPin || !OptionsPin || !Schema)
				{
					const FString MissingField = !ContextPin ? TEXT("pins.MappingContext.exists")
						: !PriorityPin ? TEXT("pins.Priority.exists") : !OptionsPin ? TEXT("pins.Options.exists") : TEXT("graph_schema_available");
					OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_mapping_context_pin_invalid"),
						!ContextPin ? TEXT("The generated AddMappingContext call does not expose its MappingContext pin.")
							: !PriorityPin ? TEXT("The generated AddMappingContext call does not expose its Priority pin.")
							: !OptionsPin ? TEXT("The generated AddMappingContext call does not expose its Options pin.")
							: TEXT("The AddMappingContext graph has no schema for its pin defaults."),
						TEXT("node_create"), MissingField, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return nullptr;
				}
				if (!SetPinDefaultObject(*Schema, *ContextPin, Context, OutError))
				{
					LocateMutationInput(OutError, Input, TEXT("mapping_context"));
					return nullptr;
				}
				if (!SetPinDefaultValue(*Schema, *PriorityPin, LexToString(NativePriority), OutError))
				{
					LocateMutationInput(OutError, Input, TEXT("priority"));
					return nullptr;
				}
				if (!SetPinDefaultValue(*Schema, *OptionsPin, Options, OutError))
				{
					LocateMutationInput(OutError, Input, TEXT("options_default_value"));
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
					OutError = MutationInputFailure(Blueprint, Input, TEXT("blueprint_dynamic_cast_target_missing"),
					TEXT("A dynamic cast node requires a string target_class."), TEXT("node_create"), Kind, TEXT("target_class"),
					MakeShared<FJsonValueString>(TEXT("a string")));
					return nullptr;
				}
				UClass* TargetClass = nullptr;
				if (!UeAgentBlueprintAssetLifecycle::TryResolveAuthoringSelfClass(Blueprint, TargetClassPath, TargetClass))
					TargetClass = UeAgentBlueprintAuthoringOps::ResolveClassByPath(TargetClassPath);
				if (!TargetClass)
				{
					OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_dynamic_cast_target_invalid"),
					TEXT("The target class for the requested dynamic cast could not be resolved."), TEXT("node_create"), TEXT("target_class_available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("target_class"));
					return nullptr;
				}
				UK2Node_DynamicCast* Node = NewObject<UK2Node_DynamicCast>(&Graph);
				if (!Node)
				{
					OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_dynamic_cast_create_failed"),
					TEXT("Unreal did not allocate the requested dynamic cast node."), TEXT("node_create"), TEXT("node_created"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("target_class"));
					return nullptr;
				}
				Node->TargetType = TargetClass;
				if (!InitializeNode(Graph, *Node, Spec, OutError)) return nullptr;
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
					OutError = MutationInputFailure(Blueprint, Input, TEXT("blueprint_parent_function_name_missing"),
					TEXT("A parent function call requires a string function_name."), TEXT("node_create"), Kind, TEXT("function_name"),
					MakeShared<FJsonValueString>(TEXT("a string")));
					return nullptr;
				}
				TryGetString(Spec, TEXT("function_owner_class"), OwnerPath);
				UClass* OwnerClass = OwnerPath.IsEmpty()
					? Blueprint.ParentClass.Get()
					: UeAgentBlueprintAuthoringOps::ResolveClassByPath(OwnerPath);
				UFunction* Function = OwnerClass
					? OwnerClass->FindFunctionByName(FName(*FunctionName))
					: nullptr;
				if (!Function)
				{
					OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_parent_function_not_found"),
					!OwnerClass ? TEXT("The parent function owner class could not be resolved.")
						: TEXT("The requested function was not found in its parent owner class."),
					TEXT("node_create"), !OwnerClass ? TEXT("owner_class_available") : TEXT("parent_function_exists"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
					!OwnerClass ? TEXT("function_owner_class") : TEXT("function_name"));
					return nullptr;
				}
				FGraphNodeCreator<UK2Node_CallParentFunction> Creator(Graph);
				UK2Node_CallParentFunction* Node = Creator.CreateNode();
				Node->SetFlags(RF_Transactional);
				Node->SetFromFunction(Function);
				if (!SetNodePosition(*Node, Spec, OutError)) { Creator.Finalize(); return nullptr; }
				UEdGraphSchema_K2::SetNodeMetaData(Node, FNodeMetadata::DefaultGraphNode);
				Creator.Finalize();
				return Node;
			}
			if (Kind == TEXT("call_function"))
			{
				FString OwnerPath;
				FString FunctionName;
				UFunction* Function = ResolveCallFunctionTarget(
					Blueprint, Spec, Input, OwnerPath, FunctionName, OutError);
				if (!Function) return nullptr;
				if (UeAgentBlueprintAuthoringOps::IsUserConstructionScriptGraph(&Graph)
					&& UeAgentBlueprintAuthoringOps::IsUnsafeConstructionScriptPcgCall(OwnerPath, FunctionName))
				{
					OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("unsafe_construction_script_pcg_generation_blocked"),
					TEXT("This PCG generation function cannot be called safely from a construction script."), TEXT("node_create"), TEXT("construction_script_call_safe"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("function_name"));
					return nullptr;
				}
				if (UeAgentBlueprintAuthoringOps::RequiresExplicitParentOrEventNode(FunctionName))
				{
					OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("event_function_call_requires_explicit_parent_node"),
					TEXT("This event function requires an explicit parent-function or event node."), TEXT("node_create"), TEXT("node_type"),
					MakeShared<FJsonValueString>(Kind), MakeShared<FJsonValueString>(TEXT("call_parent_function or event")), TEXT("node_type"));
					return nullptr;
				}
				return CreateCallFunctionNode(Blueprint, Graph, *Function, Spec, Input, OutError);
			}
			if (Kind == TEXT("variable_node"))
			{
				FString VariableName;
				FString Access = TEXT("get");
				FString ParentClassPath;
				if (!TryGetString(Spec, TEXT("variable_name"), VariableName))
				{
					OutError = MutationInputFailure(Blueprint, Input, TEXT("blueprint_variable_node_name_missing"),
					TEXT("A variable node requires a string variable_name."), TEXT("node_create"), Kind, TEXT("variable_name"),
					MakeShared<FJsonValueString>(TEXT("a string")));
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
					OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_variable_node_create_failed"),
					TEXT("Unreal did not allocate the requested variable access node."), TEXT("node_create"), TEXT("node_created"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("variable_name"));
					return nullptr;
				}
				if (!ParentClassPath.IsEmpty())
				{
					UClass* ParentClass = UeAgentBlueprintAuthoringOps::ResolveClassByPath(ParentClassPath);
					FProperty* Property = ParentClass
						? FindFProperty<FProperty>(ParentClass, FName(*VariableName))
						: nullptr;
					if (!Property)
					{
						OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_external_variable_not_found"),
						!ParentClass ? TEXT("The owner class for the external variable could not be resolved.")
							: TEXT("The requested variable property was not found in its owner class."),
						TEXT("node_create"), !ParentClass ? TEXT("member_parent_class_available") : TEXT("variable_property_exists"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
						!ParentClass ? TEXT("member_parent_class") : TEXT("variable_name"));
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
						OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_variable_not_found"),
					TEXT("The requested variable was not found in the Blueprint declarations or skeleton class."), TEXT("node_create"), TEXT("variable_exists"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("variable_name"));
						return nullptr;
					}
					Node->VariableReference.SetSelfMember(FName(*VariableName));
				}
				if (!InitializeNode(Graph, *Node, Spec, OutError)) return nullptr;
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
					OutError = MutationInputFailure(Blueprint, Input, TEXT("blueprint_node_class_missing"),
					TEXT("A node_by_class request requires a string node_class."), TEXT("node_create"), Kind, TEXT("node_class"),
					MakeShared<FJsonValueString>(TEXT("a string")));
					return nullptr;
				}
				UClass* NodeClass = UeAgentBlueprintAuthoringOps::ResolveClassByPath(ClassPath);
				if (!NodeClass || !NodeClass->IsChildOf(UEdGraphNode::StaticClass())
					|| NodeClass->HasAnyClassFlags(CLASS_Abstract))
				{
					OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_node_class_invalid"),
					!NodeClass ? TEXT("The requested node class could not be resolved.")
						: !NodeClass->IsChildOf(UEdGraphNode::StaticClass()) ? TEXT("The requested class does not derive from UEdGraphNode.")
						: TEXT("The requested node class is abstract and cannot be instantiated."),
					TEXT("node_create"), !NodeClass ? TEXT("node_class_available")
						: !NodeClass->IsChildOf(UEdGraphNode::StaticClass()) ? TEXT("is_graph_node_class") : TEXT("class_is_abstract"),
					MakeShared<FJsonValueBoolean>(NodeClass && NodeClass->IsChildOf(UEdGraphNode::StaticClass())
						&& NodeClass->HasAnyClassFlags(CLASS_Abstract)),
					MakeShared<FJsonValueBoolean>(!NodeClass || !NodeClass->IsChildOf(UEdGraphNode::StaticClass())), TEXT("node_class"));
					return nullptr;
				}
				UEdGraphNode* Node = NewObject<UEdGraphNode>(&Graph, NodeClass);
				if (!Node)
				{
					OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_node_create_failed"),
					TEXT("Unreal did not allocate a node of the requested class."), TEXT("node_create"), TEXT("node_created"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("node_class"));
					return nullptr;
				}
				if (!InitializeNode(Graph, *Node, Spec, OutError)) return nullptr;
				return Node;
			}
			OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_node_type_unsupported"),
					TEXT("This Blueprint mutation node type is not supported."), TEXT("node_create"), TEXT("node_type"),
					MakeShared<FJsonValueString>(Kind), MakeShared<FJsonValueString>(TEXT("macro_instance, call_delegate, call_custom_event, native_action, event, custom_event, component_bound_event, enhanced_input_action_event, enhanced_input_get_local_player_subsystem, enhanced_input_add_mapping_context, dynamic_cast, call_parent_function, call_function, variable_node or node_by_class")), TEXT("node_type"));
			return nullptr;
		}

		UEdGraphPin* ResolvePin(
			UEdGraphNode& Node,
			const FString& Name,
			const int32 Occurrence,
			const FString& GuidText,
			const FJsonObject& Input,
			const TCHAR* NameField,
			FDiagnostic& OutError)
		{
			const auto Fail = [&](const TCHAR* Code, const FString& Reason, const FString& Field,
				TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& SourceField)
			{
				OutError = FDiagnostic::Failure(Code, Reason, TEXT("pin_resolution"));
				OutError.SourceKind = TEXT("native_result");
				OutError.CandidateName = Name;
				OutError.PropertyPath = Field;
				OutError.ActualValue = MoveTemp(Actual);
				OutError.ActualValuePresent = true;
				OutError.ExpectedValue = MoveTemp(Expected);
				if (const UBlueprint* Blueprint = Node.GetTypedOuter<UBlueprint>())
					OutError.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Blueprint);
				LocateMutationInput(OutError, Input, SourceField);
			};
			FGuid Guid;
			if (FGuid::Parse(GuidText, Guid))
			{
				for (UEdGraphPin* Pin : Node.Pins)
				{
					if (Pin && Pin->PinId == Guid) return Pin;
				}
				Fail(TEXT("blueprint_pin_guid_not_found"),
					TEXT("The node has no pin with the supplied stable identity."), TEXT("pin_exists"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
					Input.HasField(TEXT("guid")) ? TEXT("guid") : Input.HasField(TEXT("pin_guid")) ? TEXT("pin_guid") : NameField);
				OutError.InternalContext = GuidText;
				return nullptr;
			}
			TArray<UEdGraphPin*> Matches;
			for (UEdGraphPin* Pin : Node.Pins)
			{
				if (Pin && Pin->PinName.ToString().Equals(Name, ESearchCase::IgnoreCase)) Matches.Add(Pin);
			}
			if (!Matches.IsValidIndex(Occurrence))
			{
				if (Matches.IsEmpty())
					Fail(TEXT("blueprint_pin_not_found"), FString::Printf(TEXT("The %s node has no pin with the requested name."), *Node.GetClass()->GetName()),
						TEXT("matching_pins"), MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(1), NameField);
				else
					Fail(TEXT("blueprint_pin_occurrence_not_found"), FString::Printf(TEXT("The requested pin occurrence is outside the matching pin range on the %s node."), *Node.GetClass()->GetName()),
						TEXT("pin_occurrence"), MakeShared<FJsonValueNumber>(Occurrence),
						MakeShared<FJsonValueString>(FString::Printf(TEXT("0 through %d"), Matches.Num() - 1)), TEXT("pin_occurrence"));
				TArray<TSharedPtr<FJsonValue>> AvailablePins;
				for (const UEdGraphPin* Pin : Node.Pins) if (Pin)
				{
					auto Value = MakeShared<FJsonObject>();
					Value->SetStringField(TEXT("name"), Pin->PinName.ToString());
					if (Pin->bOrphanedPin) Value->SetBoolField(TEXT("orphaned"), true);
					AvailablePins.Add(MakeShared<FJsonValueObject>(Value));
				}
				OutError.CandidateChoices = MakeShared<FJsonValueArray>(MoveTemp(AvailablePins));
				if (!Node.ErrorMsg.IsEmpty())
				{
					FDiagnostic NativeMessage = FDiagnostic::Failure(TEXT("native_graph_node_message"),
						Node.ErrorMsg, TEXT("pin_resolution"));
					NativeMessage.SourceKind = TEXT("native_result");
					NativeMessage.TargetPath = OutError.TargetPath;
					NativeMessage.CandidateName = Node.GetClass()->GetName();
					OutError.PublicCause = MakeShared<FDiagnostic>(MoveTemp(NativeMessage));
				}
				return nullptr;
			}
			return Matches[Occurrence];
		}

		bool TryReadPinOccurrence(
			const TSharedPtr<const FJsonObject>& Spec,
			const FJsonObject& Input,
			int32& OutOccurrence,
			FDiagnostic& OutError)
		{
			OutOccurrence = 0;
			if (!Spec.IsValid() || !Spec->HasField(TEXT("pin_occurrence")))
			{
				return true;
			}
			if (!UeAgentAuthoringJsonFields::ReadInt32Field(*Spec, TEXT("pin_occurrence"), OutOccurrence, OutError))
			{
				LocateMutationInput(OutError, Input, TEXT("pin_occurrence"));
				return false;
			}
			if (OutOccurrence < 0)
			{
				OutError = MutationDocumentFailure(Input, TEXT("blueprint_pin_occurrence_invalid"),
					TEXT("Pin occurrence must be a non-negative integer."), TEXT("pin_occurrence"),
					MakeShared<FJsonValueNumber>(OutOccurrence), MakeShared<FJsonValueString>(TEXT("an integer greater than or equal to zero")));
				return false;
			}
			return true;
		}

		UEdGraphPin* ResolveEdgePin(
			UEdGraphNode& Node,
			const TSharedPtr<const FJsonObject>& Endpoint,
			const FString& Name,
			const int32 Occurrence,
			FDiagnostic& OutError)
		{
			if (Endpoint->HasField(TEXT("pin_occurrence")))
			{
				return ResolvePin(Node, Name, Occurrence, FString(), *Endpoint, TEXT("pin"), OutError);
			}

			using namespace UE::UAI::HjsonWorkspace;
			TArray<FGraphAuthoringPinAliasInput> Inputs;
			TMap<FString, int32> OccurrencesByName;
			TMap<FString, UEdGraphPin*> PinBySourceId;
			for (UEdGraphPin* Pin : Node.Pins)
			{
				if (!Pin || Pin->bOrphanedPin) continue;
				const FString SemanticName = Pin->PinName.ToString();
				const FString NormalizedName = SemanticName.ToLower();
				int32& PinOccurrence = OccurrencesByName.FindOrAdd(NormalizedName);
				const FString SourceId = LexToString(Inputs.Num());
				Inputs.Add({
					SourceId,
					SemanticName,
					PinOccurrence++,
					Pin->Direction == EGPD_Output
						? EGraphAuthoringEndpointDirection::Output
						: EGraphAuthoringEndpointDirection::Input});
				PinBySourceId.Add(SourceId, Pin);
			}

			FGraphAuthoringPinAliasProjection Projection;
			FWorkspaceError ProjectionError;
			if (!TryBuildGraphAuthoringPinAliasProjection(
					Inputs, Projection, ProjectionError))
			{
				OutError = ProjectionError.GetDiagnostic();
				return nullptr;
			}
			for (const TPair<FString, FString>& Alias : Projection.SourceIdByAlias)
			{
				if (Alias.Key.Equals(Name, ESearchCase::IgnoreCase))
				{
					return PinBySourceId.FindRef(Alias.Value);
				}
			}
			return ResolvePin(Node, Name, 0, FString(), *Endpoint, TEXT("pin"), OutError);
		}

		bool TryReadPinGroupCount(
			const FJsonObject& PinGroups,
			const TCHAR* Field,
			const int32 MinCount,
			int32& OutCount,
			FDiagnostic& OutError)
		{
			if (!PinGroups.HasField(Field))
			{
				OutError = MutationDocumentFailure(PinGroups, TEXT("blueprint_pin_group_count_invalid"),
					TEXT("The pin group requires an integer count."), Field, PinGroups.TryGetField(Field),
					MakeShared<FJsonValueString>(FString::Printf(TEXT("an integer greater than or equal to %d"), MinCount)));
				return false;
			}
			if (!UeAgentAuthoringJsonFields::ReadInt32Field(PinGroups, Field, OutCount, OutError)) return false;
			if (OutCount < MinCount)
			{
				OutError = MutationDocumentFailure(PinGroups, TEXT("blueprint_pin_group_count_invalid"),
					TEXT("The requested pin count is below this group's minimum."), Field, MakeShared<FJsonValueNumber>(OutCount),
					MakeShared<FJsonValueString>(FString::Printf(TEXT("an integer greater than or equal to %d"), MinCount)));
				return false;
			}
			return true;
		}

		bool ApplyPinGroups(
			UBlueprint& Blueprint,
			UEdGraphNode& Node,
			const TSharedPtr<const FJsonObject>& Spec,
			const FJsonObject& Input,
			FMutationCounters& OutCounters,
			FDiagnostic& OutError)
		{
			if (Spec->HasField(TEXT("exec_output_pin_count")))
			{
				OutError = MutationInputFailure(Blueprint, Input, TEXT("blueprint_exec_output_pin_count_retired"),
					TEXT("exec_output_pin_count is retired; use pin_groups.execution_outputs."), TEXT("pin_group_apply"),
					Node.GetClass()->GetName(), TEXT("exec_output_pin_count"), MakeShared<FJsonValueString>(TEXT("pin_groups.execution_outputs")));
				return false;
			}
			const TSharedPtr<const FJsonObject> PinGroups = GetObject(Spec, TEXT("pin_groups"));
			if (!Spec->HasField(TEXT("pin_groups")))
			{
				return true;
			}
			if (!PinGroups.IsValid())
			{
				OutError = MutationInputFailure(Blueprint, Input, TEXT("blueprint_pin_groups_invalid"),
					TEXT("pin_groups must be an object."), TEXT("pin_group_apply"), Node.GetClass()->GetName(),
					TEXT("pin_groups"), MakeShared<FJsonValueString>(TEXT("object")));
				return false;
			}

			const auto UnknownGroup = [&](const FString& ExpectedGroup)
			{
				TArray<TSharedPtr<FJsonValue>> Fields;
				for (const auto& Pair : PinGroups->Values) Fields.Add(MakeShared<FJsonValueString>(Pair.Key));
				OutError = MutationDocumentFailure(*PinGroups, TEXT("blueprint_pin_group_unknown"),
					TEXT("This node requires exactly the supported pin group field."), FString(),
					MakeShared<FJsonValueArray>(MoveTemp(Fields)), MakeShared<FJsonValueString>(ExpectedGroup));
				OutError.PropertyPath = TEXT("pin_groups");
				OutError.CandidateName = Node.GetClass()->GetName();
				OutError.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint);
			};
			const auto NativeGroupFailure = [&](const TCHAR* Code, const FString& Reason, const FString& Group,
				TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
			{
				OutError = MutationFailure(Blueprint, Code, Reason, TEXT("pin_group_apply"),
					Node.GetClass()->GetName(), Group, MoveTemp(Actual), MoveTemp(Expected));
				LocateMutationInput(OutError, *PinGroups, Group);
			};

			if (UK2Node_ExecutionSequence* Sequence = Cast<UK2Node_ExecutionSequence>(&Node))
			{
				if (PinGroups->Values.Num() != 1
					|| !PinGroups->HasField(TEXT("execution_outputs")))
				{
					UnknownGroup(TEXT("execution_outputs"));
					return false;
				}
				int32 DesiredCount = 0;
				if (!TryReadPinGroupCount(
					*PinGroups, TEXT("execution_outputs"), 2, DesiredCount, OutError))
				{
					return false;
				}
				auto CountOutputs = [Sequence]()
				{
					return Sequence->Pins.FilterByPredicate([](const UEdGraphPin* Pin)
					{
						return Pin && Pin->Direction == EGPD_Output
							&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
					}).Num();
				};
				const int32 InitialCount = CountOutputs();
				while (CountOutputs() < DesiredCount)
				{
					Sequence->AddInputPin();
				}
				while (CountOutputs() > DesiredCount && Sequence->CanRemoveExecutionPin())
				{
					UEdGraphPin* Last = Sequence->GetThenPinGivenIndex(CountOutputs() - 1);
					if (!Last)
					{
						NativeGroupFailure(TEXT("blueprint_pin_group_member_missing"),
							TEXT("The execution sequence has no pin at the last output index selected for removal."),
							TEXT("execution_outputs"), MakeShared<FJsonValueNumber>(CountOutputs() - 1),
							MakeShared<FJsonValueString>(TEXT("an existing execution output index")));
						return false;
					}
					Sequence->RemovePinFromExecutionNode(Last);
				}
				if (CountOutputs() != DesiredCount)
				{
					NativeGroupFailure(TEXT("blueprint_pin_group_resize_rejected"),
						TEXT("The execution sequence did not reach the requested output count."),
						TEXT("execution_outputs"), MakeShared<FJsonValueNumber>(CountOutputs()), MakeShared<FJsonValueNumber>(DesiredCount));
					return false;
				}
				if (InitialCount != DesiredCount)
				{
					++OutCounters.PinGroupsResized;
				}
				return true;
			}

			if (UK2Node_MakeArray* MakeArray = Cast<UK2Node_MakeArray>(&Node))
			{
				if (PinGroups->Values.Num() != 1 || !PinGroups->HasField(TEXT("inputs")))
				{
					UnknownGroup(TEXT("inputs"));
					return false;
				}
				int32 DesiredCount = 0;
				if (!TryReadPinGroupCount(*PinGroups, TEXT("inputs"), 1, DesiredCount, OutError))
				{
					return false;
				}
				const int32 InitialCount = MakeArray->NumInputs;
				while (MakeArray->NumInputs < DesiredCount)
				{
					MakeArray->AddInputPin();
				}
				while (MakeArray->NumInputs > DesiredCount)
				{
					UEdGraphPin* Last = MakeArray->FindPin(MakeArray->GetPinName(MakeArray->NumInputs - 1));
					if (!Last)
					{
						NativeGroupFailure(TEXT("blueprint_pin_group_member_missing"),
							TEXT("The array node has no pin at the last input index selected for removal."),
							TEXT("inputs"), MakeShared<FJsonValueNumber>(MakeArray->NumInputs - 1),
							MakeShared<FJsonValueString>(TEXT("an existing array input index")));
						return false;
					}
					MakeArray->RemoveInputPin(Last);
				}
				if (InitialCount != DesiredCount)
				{
					++OutCounters.PinGroupsResized;
				}
				return true;
			}

			OutError = MutationFailure(Blueprint, TEXT("blueprint_pin_group_node_unsupported"),
				TEXT("Pin group resizing is not supported for this node class."), TEXT("pin_group_apply"),
				Node.GetClass()->GetName(), TEXT("node_class"),
				MakeShared<FJsonValueString>(UeAgentBlueprintAssetLifecycle::GetAuthoringTypeObjectPath(&Blueprint, *Node.GetClass())),
				MakeShared<FJsonValueString>(TEXT("K2Node_ExecutionSequence or K2Node_MakeArray")));
			LocateMutationInput(OutError, Input, TEXT("pin_groups"));
			return false;
		}

		enum class EPinDefaultApplyResult : uint8
		{
			Applied,
			Deferred,
			Failed,
		};

		struct FPendingPinDefault
		{
			UEdGraphNode* Node = nullptr;
			TSharedPtr<const FJsonObject> Spec;
			TSharedPtr<const FJsonObject> Input;
			FString NodeId;
		};

		EPinDefaultApplyResult TryApplyPinDefault(
			UEdGraphNode& Node,
			const UEdGraphSchema& Schema,
			const TSharedPtr<const FJsonObject>& PinSpec,
			const FJsonObject& Input,
			const FString& NodeId,
			FMutationCounters& OutCounters,
			FDiagnostic& OutError)
		{
			FString PinName;
			FString DefaultValue;
			FString PinGuid;
			int32 PinOccurrence = 0;
			auto LocateFailure = [&]()
			{
				if (OutError.Stage.IsEmpty()) OutError.Stage = TEXT("node_apply");
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = PinName;
				if (OutError.TargetPath.IsEmpty())
					if (const UBlueprint* Blueprint = Node.GetTypedOuter<UBlueprint>())
						OutError.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Blueprint);
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, Input);
			};
			const bool bHasPinName = TryGetString(PinSpec, TEXT("pin_name"), PinName);
			if (!bHasPinName || !TryGetString(PinSpec, TEXT("default_value"), DefaultValue))
			{
				const FString Field = bHasPinName ? TEXT("default_value") : TEXT("pin_name");
				OutError = MutationDocumentFailure(Input, TEXT("blueprint_pin_default_invalid"),
					TEXT("A pin default requires string pin_name and default_value fields."), Field,
					Input.TryGetField(Field), MakeShared<FJsonValueString>(TEXT("string")));
				OutError.InternalContext = NodeId;
				LocateFailure();
				return EPinDefaultApplyResult::Failed;
			}
			if (!TryReadPinOccurrence(PinSpec, Input, PinOccurrence, OutError))
			{
				LocateFailure();
				return EPinDefaultApplyResult::Failed;
			}
			TryGetString(PinSpec, TEXT("guid"), PinGuid);
			FDiagnostic ResolveError;
			UEdGraphPin* Pin = ResolvePin(
				Node, PinName, PinOccurrence, PinGuid, Input, TEXT("pin_name"), ResolveError);
			if (!Pin)
			{
				if (ResolveError.Code == TEXT("blueprint_pin_not_found"))
				{
					OutError = ResolveError;
					LocateFailure();
					return EPinDefaultApplyResult::Deferred;
				}
				OutError = ResolveError;
				LocateFailure();
				return EPinDefaultApplyResult::Failed;
			}
			if (!SetPinDefaultValue(Schema, *Pin, DefaultValue, OutError))
			{
				LocateMutationInput(OutError, Input, TEXT("default_value"));
				LocateFailure();
				return Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard
					? EPinDefaultApplyResult::Deferred
					: EPinDefaultApplyResult::Failed;
			}
			++OutCounters.PinDefaultsApplied;
			OutError.Reset();
			return EPinDefaultApplyResult::Applied;
		}

		using FGraphAffectedNodes = TSet<TWeakObjectPtr<UEdGraphNode>>;

		void CollectAffectedPinNodes(const UEdGraphPin* Pin, FGraphAffectedNodes* AffectedNodes)
		{
			if (!Pin || !AffectedNodes) return;
			AffectedNodes->Add(Pin->GetOwningNode());
			for (const UEdGraphPin* Linked : Pin->LinkedTo)
				if (Linked) AffectedNodes->Add(Linked->GetOwningNode());
		}

		bool ApplyNodeRemoval(UBlueprint& Blueprint, UEdGraph& Graph, UEdGraphNode* Node,
			const FJsonObject& Spec, const FJsonObject& Input, FMutationCounters& OutCounters, FDiagnostic& OutError,
			FGraphAffectedNodes* AffectedNodes = nullptr)
		{
			if (!Node) return true;
			if (!Node->CanUserDeleteNode())
			{
				OutError = NodeMutationFailure(Blueprint, Spec, Input, TEXT("blueprint_node_remove_forbidden"),
					TEXT("Unreal does not allow this node to be deleted."), TEXT("node_remove"),
					TEXT("can_delete_node"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
					TEXT("operation"));
				return false;
			}
			for (const UEdGraphPin* Pin : Node->Pins) CollectAffectedPinNodes(Pin, AffectedNodes);
			Graph.RemoveNode(Node);
			++OutCounters.NodesRemoved;
			return true;
		}

		bool ApplyNodeSpec(
			UBlueprint& Blueprint,
			UEdGraph& Graph,
			const TSharedPtr<const FJsonObject>& Spec,
			const FJsonObject& Input,
			TMap<FString, UEdGraphNode*>& OutNodeIds,
			FMutationCounters& OutCounters,
			TArray<FPendingPinDefault>& OutPendingPinDefaults,
			const bool bAllowScratchCandidateRebind,
			FDiagnostic& OutError,
			FGraphAffectedNodes* AffectedNodes = nullptr, FPropertyIntents* Intents = nullptr)
		{
			ON_SCOPE_EXIT
			{
				if (!OutError.IsEmpty()) LocateGraphItemFailure(Blueprint, Input, OutError);
			};
			EOperation Operation;
			if (!ParseOperation(Spec, EOperation::Upsert, Operation, OutError)) return false;
			FString Kind;
			// A new graph alias denotes a new node instance, even when it calls
			// the same function or accesses the same member as another node.
			UEdGraphNode* Node = ResolveNode(
				Blueprint, Graph, Spec, Input, Kind, OutError, Operation != EOperation::Create);
			if (!OutError.IsEmpty()) return false;
			FString Id;
			TryGetString(Spec, TEXT("id"), Id);

			if (Operation == EOperation::Remove)
				return ApplyNodeRemoval(Blueprint, Graph, Node, *Spec, Input, OutCounters, OutError, AffectedNodes);
			if (Operation == EOperation::Create && Node)
			{
				OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_node_already_exists"),
					TEXT("A node matching this create request already exists in the graph."), TEXT("node_apply"),
					TEXT("node_exists"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false),
					TEXT("operation"));
				return false;
			}
			if (Operation == EOperation::Update && !Node)
			{
				OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_node_not_found"),
					TEXT("The node required by this update request was not found in the graph."), TEXT("node_apply"),
					TEXT("node_exists"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
					TEXT("operation"));
				return false;
			}
			if (!Node)
			{
				if (Kind.IsEmpty())
				{
					OutError = MutationInputFailure(Blueprint, Input, TEXT("blueprint_node_type_missing"),
						TEXT("A node type is required to create a node that was not found in the graph."),
						TEXT("node_apply"), FString(), TEXT("node_type"), MakeShared<FJsonValueString>(TEXT("a node type")));
					OutError.InternalContext = Id;
					return false;
				}
				Node = CreateNode(
					Blueprint,
					Graph,
					Spec,
					Input,
					Kind,
					OutNodeIds,
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
				if (!SetNodePosition(*Node, Spec, OutError)) return false;
				++OutCounters.NodesUpdated;
			}
			if (AffectedNodes) AffectedNodes->Add(Node);
			if (Kind == TEXT("call_function"))
			{
				UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
				if (!Call)
				{
					OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_call_function_node_invalid"),
						TEXT("The resolved node is not a function call node."), TEXT("node_apply"),
						TEXT("is_call_function_node"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				if (!ApplyCallFunctionTarget(Blueprint, *Call, Spec, Input, OutError)) return false;
			}
			else if (Kind == TEXT("macro_instance"))
			{
				UK2Node_MacroInstance* Macro = Cast<UK2Node_MacroInstance>(Node);
				UEdGraph* MacroGraph = ResolveMacroGraph(Blueprint, Spec, Input, OutError, Macro);
				if (!MacroGraph) return false;
				if (!Macro)
				{
					OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_macro_instance_invalid"),
						TEXT("The resolved node is not a macro instance node."), TEXT("node_apply"),
						TEXT("is_macro_instance_node"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				if (Macro->GetMacroGraph() != MacroGraph)
				{
					Macro->SetMacroGraph(MacroGraph);
					Macro->ReconstructNode();
				}
			}
			else if (Kind == TEXT("call_delegate"))
			{
				UK2Node_CallDelegate* Call = Cast<UK2Node_CallDelegate>(Node);
				FMulticastDelegateProperty* Property = ResolveLocalDelegateProperty(
					Blueprint, Spec, Input, OutError);
				if (!Property) return false;
				if (!Call)
				{
					OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_call_delegate_invalid"),
						TEXT("The resolved node is not a delegate call node."), TEXT("node_apply"),
						TEXT("is_call_delegate_node"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				if (Call->GetProperty() != Property)
				{
					Call->SetFromProperty(Property, true, Property->GetOwnerClass());
					Call->ReconstructNode();
				}
			}
			else if (Kind == TEXT("call_custom_event"))
			{
				UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
				UFunction* Function = ResolveLocalCustomEventFunction(
					Blueprint, Spec, Input, OutError);
				if (!Function) return false;
				if (!Call)
				{
					OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_custom_event_call_node_invalid"),
						TEXT("The resolved custom event caller is not a function call node."), TEXT("node_apply"),
						TEXT("is_call_function_node"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				if (Call->GetTargetFunction() != Function)
				{
					Call->SetFromFunction(Function);
					Call->ReconstructNode();
				}
			}

			TSharedPtr<FJsonObject> Properties = MakePropertyPayload(Spec, TEXT("properties"), OutError);
			if (!OutError.IsEmpty()) return false;
			int32 AppliedProperties = 0;
			if (!ApplyProperties(*Node, Properties, AppliedProperties, OutError, &Blueprint, Intents)) return false;
			if (AppliedProperties > 0) Node->ReconstructNode();
			if (!ApplyPinGroups(Blueprint, *Node, Spec, Input, OutCounters, OutError)) return false;

			TArray<FStableMapEntry> PinDefaults;
			bool bPinDefaultsPresent = false;
			if (!GetObjectMap(Spec, TEXT("pin_defaults"), PinDefaults, bPinDefaultsPresent, OutError)) return false;
			if (bPinDefaultsPresent)
			{
				const UEdGraphSchema* Schema = Graph.GetSchema();
				if (!Schema)
				{
					OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_graph_schema_missing"),
						TEXT("The graph has no schema to apply pin defaults."), TEXT("pin_default_apply"),
						TEXT("schema_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("pin_defaults"));
					return false;
				}
				TArray<FPendingPinDefault> PendingPinDefaults;
				PendingPinDefaults.Reserve(PinDefaults.Num());
				for (const FStableMapEntry& PinEntry : PinDefaults)
				{
					const TSharedPtr<const FJsonObject> PinSpec = MakeStableMapSpec(
						PinEntry, EStableMapIdentity::PinDefault, OutError);
					if (!PinSpec.IsValid()) return false;
					PendingPinDefaults.Add({Node, PinSpec, PinEntry.Value, Id});
				}

				while (!PendingPinDefaults.IsEmpty())
				{
					TArray<FPendingPinDefault> DeferredPinDefaults;
					bool bMadeProgress = false;
					for (const FPendingPinDefault& Pending
						: PendingPinDefaults)
					{
						FDiagnostic ApplyError;
						const EPinDefaultApplyResult Result = TryApplyPinDefault(
							*Node, *Schema, Pending.Spec, *Pending.Input, Id, OutCounters, ApplyError);
						if (Result == EPinDefaultApplyResult::Failed)
						{
							OutError = ApplyError;
							return false;
						}
						if (Result == EPinDefaultApplyResult::Deferred)
						{
							DeferredPinDefaults.Add(Pending);
							continue;
						}
						bMadeProgress = true;
					}

					if (DeferredPinDefaults.IsEmpty()) break;
					if (!bMadeProgress)
					{
						for (const FPendingPinDefault& Pending
							: DeferredPinDefaults)
						{
							OutPendingPinDefaults.Add(Pending);
						}
						break;
					}

					// A resolved default may synchronously change the node interface through
					// the schema callback. Retry only unresolved defaults against the real,
					// refreshed Pin list instead of depending on stable-map key order.
					PendingPinDefaults = MoveTemp(DeferredPinDefaults);
				}
			}

			// Both native palette actions and explicit node specs finish here.
			// A fresh object cast retains a provisional UObject subtype until its
			// native connection callback runs. Class-cast inputs are not wildcard.
			if (auto* DynamicCast = Cast<UK2Node_DynamicCast>(Node))
			{
				UEdGraphPin* SourcePin = DynamicCast->GetCastSourcePin();
				if (SourcePin && SourcePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
					DynamicCast->NotifyPinConnectionListChanged(SourcePin);
			}

			if (!Id.IsEmpty())
			{
				if (UEdGraphNode** ExistingId = OutNodeIds.Find(Id); ExistingId && *ExistingId != Node)
				{
					OutError = NodeMutationFailure(Blueprint, *Spec, Input, TEXT("blueprint_node_id_conflict"),
						TEXT("The mutation node identity is already bound to a different node."), TEXT("node_apply"),
						TEXT("identity_matches_node"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				OutNodeIds.Add(Id, Node);
			}
			return true;
		}

		UEdGraphNode* ResolveGraphNodeReference(
			UEdGraph& Graph,
			const TMap<FString, UEdGraphNode*>& NodeIds,
			const FString& Reference,
			const FJsonObject& Input,
			FDiagnostic& OutError)
		{
			if (UEdGraphNode* const* Node = NodeIds.Find(Reference);
				Node && *Node && Graph.Nodes.Contains(*Node)) return *Node;
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
			OutError = FDiagnostic::Failure(
				NameMatches.Num() > 1 ? TEXT("blueprint_edge_node_conflict") : TEXT("blueprint_edge_node_not_found"),
				NameMatches.Num() > 1 ? TEXT("The node reference matches more than one node in the graph.")
					: TEXT("The referenced node was not found in the graph."), TEXT("node_resolution"), Reference);
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("matching_nodes");
			OutError.ActualValue = MakeShared<FJsonValueNumber>(NameMatches.Num());
			OutError.ActualValuePresent = true;
			OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1);
			Input.TryGetStringField(TEXT("alias"), OutError.CandidateName);
			if (const UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(&Graph))
				OutError.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Blueprint);
			LocateMutationInput(OutError, Input, TEXT("node"));
			return nullptr;
		}

		bool ApplyEdgeSpec(
			UBlueprint& Blueprint,
			UEdGraph& Graph,
			const TSharedPtr<const FJsonObject>& Spec,
			const FJsonObject& Input,
			const TMap<FString, UEdGraphNode*>& NodeIds,
			FMutationCounters& OutCounters,
			FDiagnostic& OutError,
			UeAgentAuthoringGraphLinks::FIntent& ConnectionIntent,
			const FString& SourcePointer,
			FGraphAffectedNodes* AffectedNodes = nullptr,
			const TSet<UEdGraphNode*>* NodesBeforeMutation = nullptr)
		{
			const TSharedPtr<const FJsonObject> From = GetObject(Spec, TEXT("from"));
			const TSharedPtr<const FJsonObject> To = GetObject(Spec, TEXT("to"));
			FString FromNodeRef;
			FString FromPinName;
			FString ToNodeRef;
			FString ToPinName;
			int32 FromPinOccurrence = 0;
			int32 ToPinOccurrence = 0;
			ON_SCOPE_EXIT
			{
				if (!OutError.IsEmpty())
					UeAgentAuthoringDiagnostics::LocateFailure(OutError, Input, TEXT("edge_apply"),
						UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint));
			};
			const auto ReadEndpoint = [&](const TSharedPtr<const FJsonObject>& Endpoint, const TCHAR* EndField,
				FString& NodeRef, FString& PinName)
			{
				const bool bHasNode = TryGetString(Endpoint, TEXT("node"), NodeRef);
				if (bHasNode && TryGetString(Endpoint, TEXT("pin"), PinName)) return true;
				const FString Field = Endpoint ? (bHasNode ? TEXT("pin") : TEXT("node")) : EndField;
				OutError = MutationDocumentFailure(Endpoint ? *Endpoint : Input, TEXT("blueprint_edge_invalid"),
					Endpoint ? TEXT("An edge endpoint requires string node and pin selectors.") : TEXT("An edge endpoint must be an object."),
					Field, Endpoint && bHasNode ? Endpoint->TryGetField(Field) : nullptr,
					MakeShared<FJsonValueString>(Endpoint ? TEXT("string") : TEXT("object")));
				// A node selector can be an internal stable identity; expose its type
				// failure without echoing that identity or the complete endpoint.
				if (!Endpoint || !bHasNode)
				{
					OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
					OutError.ActualValuePresent = true;
					OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					OutError.PropertyPath = Endpoint ? TEXT("node_selector_is_string") : FString(EndField) + TEXT("_is_object");
				}
				if (Endpoint && OutError.Source.SourceFile.IsEmpty() && OutError.Source.SourceUnit.IsEmpty()
					&& OutError.SourceKind != TEXT("command_input"))
					UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError, TEXT("/") + FString(EndField));
				return false;
			};
			if (!ReadEndpoint(From, TEXT("from"), FromNodeRef, FromPinName)
				|| !ReadEndpoint(To, TEXT("to"), ToNodeRef, ToPinName)) return false;
			const auto EdgeFailure = [&](const TCHAR* Code, const FString& Reason, const FString& Field,
				TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
				const FJsonObject& RelatedInput, const FString& RelatedField)
			{
				OutError = MutationFailure(Blueprint, Code, Reason, TEXT("edge_apply"), Graph.GetName(),
					Field, MoveTemp(Actual), MoveTemp(Expected));
				auto Facts = MakeShared<FJsonObject>();
				Facts->SetStringField(TEXT("from_pin"), FromPinName);
				Facts->SetStringField(TEXT("to_pin"), ToPinName);
				FString Alias;
				if (From->TryGetStringField(TEXT("alias"), Alias)) Facts->SetStringField(TEXT("from_node"), Alias);
				if (To->TryGetStringField(TEXT("alias"), Alias)) Facts->SetStringField(TEXT("to_node"), Alias);
				OutError.ProposedValue = MakeShared<FJsonValueObject>(Facts);
				LocateMutationInput(OutError, RelatedInput, RelatedField);
			};
			if (!TryReadPinOccurrence(From, *From, FromPinOccurrence, OutError)
				|| !TryReadPinOccurrence(To, *To, ToPinOccurrence, OutError))
			{
				return false;
			}
			UEdGraphNode* FromNode = ResolveGraphNodeReference(Graph, NodeIds, FromNodeRef, *From, OutError);
			if (!FromNode) return false;
			UEdGraphNode* ToNode = ResolveGraphNodeReference(Graph, NodeIds, ToNodeRef, *To, OutError);
			if (!ToNode) return false;
			UEdGraphPin* FromPin = ResolveEdgePin(
				*FromNode, From, FromPinName, FromPinOccurrence, OutError);
			if (!FromPin) return false;
			UEdGraphPin* ToPin = ResolveEdgePin(
				*ToNode, To, ToPinName, ToPinOccurrence, OutError);
			if (!ToPin) return false;
			if (FromPin->Direction != EGPD_Output || ToPin->Direction != EGPD_Input)
			{
				const bool bInvalidFrom = FromPin->Direction != EGPD_Output;
				const EEdGraphPinDirection Direction = bInvalidFrom ? FromPin->Direction : ToPin->Direction;
				EdgeFailure(TEXT("blueprint_edge_direction_invalid"), TEXT("An edge must connect an output pin to an input pin."),
					bInvalidFrom ? TEXT("from.pin.direction") : TEXT("to.pin.direction"),
					MakeShared<FJsonValueString>(Direction == EGPD_Output ? TEXT("output")
						: Direction == EGPD_Input ? TEXT("input") : LexToString(static_cast<int32>(Direction))),
					MakeShared<FJsonValueString>(bInvalidFrom ? TEXT("output") : TEXT("input")),
					bInvalidFrom ? *From : *To, TEXT("pin"));
				return false;
			}
			EOperation Operation;
			if (!ParseOperation(Spec, EOperation::Upsert, Operation, OutError)) return false;
			const bool bConnected = FromPin->LinkedTo.Contains(ToPin);
			const UEdGraphSchema* Schema = Graph.GetSchema();
			if (!Schema)
			{
				EdgeFailure(TEXT("blueprint_graph_schema_missing"), TEXT("The graph has no schema to modify connections."),
					TEXT("schema_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Input, FString());
				return false;
			}
			if (Operation == EOperation::Remove)
			{
				ConnectionIntent.Expect(FromPin, ToPin, false, SourcePointer);
				if (bConnected)
				{
					CollectAffectedPinNodes(FromPin, AffectedNodes);
					CollectAffectedPinNodes(ToPin, AffectedNodes);
					Schema->BreakSinglePinLink(FromPin, ToPin);
					++OutCounters.EdgesRemoved;
				}
				return true;
			}
			// A native candidate may already have autowired this requested edge.
			// Only a connection between pre-existing endpoints can be a duplicate
			// of the pre-mutation graph; new endpoints belong to this same edit.
			const bool bPreexistingEndpoints = !NodesBeforeMutation
				|| (NodesBeforeMutation->Contains(FromNode) && NodesBeforeMutation->Contains(ToNode));
			if (Operation == EOperation::Create && bConnected && bPreexistingEndpoints)
			{
				EdgeFailure(TEXT("blueprint_edge_already_exists"), TEXT("Create cannot add a connection that already exists between pre-existing endpoints."),
					TEXT("edge_exists"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), Input, TEXT("operation"));
				return false;
			}
			if (Operation == EOperation::Update && !bConnected)
			{
				EdgeFailure(TEXT("blueprint_edge_not_found"), TEXT("Update requires an existing connection between these endpoints."),
					TEXT("edge_exists"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Input, TEXT("operation"));
				return false;
			}
			TSet<UEdGraphNode*> ConversionNodes;
			if (!bConnected)
			{
				CollectAffectedPinNodes(FromPin, AffectedNodes);
				CollectAffectedPinNodes(ToPin, AffectedNodes);
				const bool bConversion = Schema->CanCreateConnection(FromPin, ToPin).Response
					== CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE;
				TSet<UEdGraphNode*> PreviousNodes;
				if (bConversion) for (UEdGraphNode* Node : Graph.Nodes) PreviousNodes.Add(Node);
				if (!UeAgentBlueprintAuthoringOps::TryCreateGraphConnection(*Schema, FromPin, ToPin, OutError))
				{
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint);
					LocateMutationInput(OutError, Input, FString());
					return false;
				}
				if (bConversion) for (UEdGraphNode* Node : Graph.Nodes)
					if (Node && !PreviousNodes.Contains(Node)) ConversionNodes.Add(Node);
				// Promotion or a connection callback can reconstruct pins. Reuse the
				// original selectors, never dereference pre-callback pin pointers.
				FDiagnostic FromError, ToError;
				FromPin = ResolveEdgePin(*FromNode, From, FromPinName, FromPinOccurrence, FromError);
				ToPin = ResolveEdgePin(*ToNode, To, ToPinName, ToPinOccurrence, ToError);
				if (!FromPin || !ToPin) {
					OutError = FromPin ? MoveTemp(ToError) : MoveTemp(FromError);
					if (!FromPin && !ToPin) OutError.Diagnostics.Add(MoveTemp(ToError));
					UeAgentAuthoringDiagnostics::ReportConsistencyWarning(TEXT("graph_connection_readback_unavailable"),
						OutError, TEXT("graph_connection_observation"));
					OutError.Reset();
					++OutCounters.EdgesAdded;
					return true;
				}
				++OutCounters.EdgesAdded;
			}
			if (!ConnectionIntent.ExpectConnected(FromPin, ToPin, ConversionNodes, SourcePointer, OutError)) {
				UeAgentAuthoringDiagnostics::ReportConsistencyWarning(TEXT("graph_connection_intent_not_retained"),
					OutError, TEXT("graph_connection_observation"));
				OutError.Reset();
			}
			return true;
		}

		enum class EGraphApplyPhase : uint8
		{
			Interface,
			Body,
		};

		// Bind callers on the intact scratch graph before K2 changes the function name or reconstructs pins.
		void ObserveGraphCallers(UBlueprint& Blueprint, FName FunctionName, const FGuid& Id,
			const UEdGraph* MacroGraph, FObservedGraphs& Observed)
		{
			const FString Target = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint);
			TArray<UEdGraph*> Graphs;
			GatherExportGraphs(Blueprint, Graphs);
			for (const UEdGraph* Graph : Graphs) {
				for (const UEdGraphNode* Node : Graph->Nodes) {
					bool bReferences = false;
					if (MacroGraph) {
						const auto* Instance = Cast<UK2Node_MacroInstance>(Node);
						const UEdGraph* Macro = Instance ? Instance->GetMacroGraph() : nullptr;
						const auto* Owner = Macro ? Macro->GetTypedOuter<UBlueprint>() : nullptr;
						bReferences = Macro == MacroGraph || (Macro && Macro->GraphGuid == Id && Owner
							&& UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Owner) == Target);
					} else {
						const auto* Call = Cast<UK2Node_CallFunction>(Node);
						const UFunction* Function = nullptr;
						if (Call && Call->FunctionReference.GetMemberName() == FunctionName) {
							FMemberReference Ref = Call->FunctionReference;
							Function = Ref.ResolveMember<UFunction>(Blueprint.SkeletonGeneratedClass);
						} else if (const auto* Binding = Cast<UK2Node_CreateDelegate>(Node)) {
							Function = ResolveDelegateFunction(Blueprint, Binding);
						}
						const auto* Owner = Function ? UBlueprint::GetBlueprintFromClass(Function->GetOwnerClass()) : nullptr;
						bReferences = Owner && Function->GetFName() == FunctionName
							&& UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Owner) == Target;
					}
					if (bReferences) {
						ObserveExistingGraph(Observed, Blueprint, *Graph);
						break;
					}
				}
			}
		}

		bool ApplyGraphSpec(
			UBlueprint& Blueprint,
			const TSharedPtr<const FJsonObject>& Spec,
			const FJsonObject& Input,
			FMutationCounters& OutCounters,
			bool& bOutNeedsCompile,
			const bool bAllowScratchCandidateRebind,
			const EGraphApplyPhase Phase,
			FDiagnostic& OutError, FPropertyIntents* Intents = nullptr)
		{
			const FJsonObject& InputHeader = GraphInputHeader(Input);
			EOperation Operation;
			if (!ParseOperation(Spec, EOperation::Upsert, Operation, OutError))
			{
				UeAgentAuthoringDiagnostics::LocateFailure(OutError, Input, TEXT("graph_apply"),
					UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), TEXT("operation"));
				return false;
			}
			FString Name;
			FString Kind;
			UEdGraph* Graph = ResolveGraph(Blueprint, Spec, Input, Name, Kind, OutError);
			if (!OutError.IsEmpty()) return false;
			FObservedGraphs* Observed = Intents && Intents->ObservedGraphs.IsSet() && Phase == EGraphApplyPhase::Interface
				? &Intents->ObservedGraphs.GetValue() : nullptr;
			if (Observed && Graph) {
				ObserveExistingGraph(*Observed, Blueprint, *Graph);
				const bool bRemove = Operation == EOperation::Remove;
				const bool bRename = !Name.IsEmpty() && Name != Graph->GetName();
				if (bRemove || bRename || InputHeader.HasField(TEXT("signature"))) {
					const FString ActualKind = GetGraphKind(Blueprint, Graph);
					if (ActualKind == TEXT("function_graph") || ActualKind == TEXT("macro_graph"))
						ObserveGraphCallers(Blueprint, Graph->GetFName(), Graph->GraphGuid,
							ActualKind == TEXT("macro_graph") ? Graph : nullptr, *Observed);
				}
				if (bRemove || bRename) {
					TArray<UEdGraph*> Children;
					Graph->GetAllChildrenGraphs(Children);
					for (const UEdGraph* Child : Children) {
						ObserveExistingGraph(*Observed, Blueprint, *Child);
						if (bRemove) Observed->FindChecked(Child->GraphGuid).After = NAME_None;
					}
					if (bRemove) Observed->FindChecked(Graph->GraphGuid).After = NAME_None;
				}
			}
			auto FailGraph = [&](const TCHAR* Code, const FString& Reason, const FString& Field,
				TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& InputField)
			{
				OutError = MutationFailure(Blueprint, Code, Reason, TEXT("graph_apply"),
					Graph ? Graph->GetName() : Name, Field, MoveTemp(Actual), MoveTemp(Expected));
				if (!Graph && Name.IsEmpty() && !Kind.IsEmpty())
				{
					auto Selectors = MakeShared<FJsonObject>();
					Selectors->SetStringField(TEXT("requested_kind"), Kind);
					OutError.ProposedValue = MakeShared<FJsonValueObject>(Selectors);
				}
				LocateGraphHeaderInput(Blueprint, Input, OutError, InputField);
			};

			if (Operation == EOperation::Remove)
			{
				if (Phase == EGraphApplyPhase::Body) return true;
				if (!Graph) return true;
				const FString ExistingKind = GetGraphKind(Blueprint, Graph);
				if (ExistingKind != TEXT("function_graph") && ExistingKind != TEXT("macro_graph"))
				{
					FailGraph(TEXT("blueprint_graph_remove_forbidden"),
						TEXT("Only function graphs and macro graphs can be removed by this operation."),
						TEXT("graph_kind"), MakeShared<FJsonValueString>(ExistingKind),
						MakeShared<FJsonValueString>(TEXT("function_graph or macro_graph")),
						InputHeader.HasField(TEXT("graph_kind")) ? TEXT("graph_kind") : TEXT("kind"));
					return false;
				}
				// Match native RemoveGraphs batching: settle the complete interface
				// phase below, rather than compiling between mutually dependent removals.
				FBlueprintEditorUtils::RemoveGraph(&Blueprint, Graph, EGraphRemoveFlags::MarkTransient);
				++OutCounters.GraphsRemoved;
				bOutNeedsCompile = true;
				return true;
			}
			if (Phase == EGraphApplyPhase::Interface
				&& Operation == EOperation::Create && Graph)
			{
				FailGraph(TEXT("blueprint_graph_already_exists"), TEXT("The graph requested for creation already exists."),
					TEXT("graph_exists"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), TEXT("name"));
				return false;
			}
			if (Phase == EGraphApplyPhase::Interface
				&& Operation == EOperation::Update && !Graph)
			{
				FailGraph(TEXT("blueprint_graph_not_found"), TEXT("The graph required for this mutation phase was not found."),
					TEXT("graph_exists"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("name"));
				return false;
			}
			if (Phase == EGraphApplyPhase::Interface && !Graph)
			{
				Graph = CreateGraph(Blueprint, Name, Kind, OutError);
				if (!Graph)
				{
					LocateGraphHeaderInput(Blueprint, Input, OutError, OutError.PropertyPath == TEXT("graph_kind")
						? (InputHeader.HasField(TEXT("graph_kind")) ? TEXT("graph_kind") : TEXT("kind")) : TEXT("name"));
					return false;
				}
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
					LocateGraphHeaderInput(Blueprint, Input, OutError, FString());
					return false;
				}
				if (bHasDesiredGuid) Graph->GraphGuid = DesiredGuid;
				if (Observed)
					Observed->Add(Graph->GraphGuid, {NAME_None, Graph->GetFName(), Graph->GetOuter() == &Blueprint});
				++OutCounters.GraphsAdded;
				bOutNeedsCompile = true;
			}
			else if (Phase == EGraphApplyPhase::Interface)
			{
				if (!Name.IsEmpty() && Graph->GetName() != Name)
				{
					if (!ValidateGraphRename(Blueprint, *Graph, Name, Kind, OutError))
					{
						LocateGraphHeaderInput(Blueprint, Input, OutError, OutError.PropertyPath == TEXT("graph_kind")
							? (InputHeader.HasField(TEXT("graph_kind")) ? TEXT("graph_kind") : TEXT("kind")) : TEXT("name"));
						return false;
					}
					FBlueprintEditorUtils::RenameGraph(Graph, Name);
					if (Graph->GetName() != Name)
					{
						FailGraph(TEXT("blueprint_graph_rename_failed"),
							TEXT("The Blueprint graph name differs from the requested name after renaming."),
							TEXT("name"), MakeShared<FJsonValueString>(Graph->GetName()),
							MakeShared<FJsonValueString>(Name), TEXT("name"));
						return false;
					}
					if (Observed) Observed->FindChecked(Graph->GraphGuid).After = Graph->GetFName();
					bOutNeedsCompile = true;
				}
				++OutCounters.GraphsUpdated;
			}
			else if (!Graph)
			{
				FailGraph(TEXT("blueprint_graph_not_found"), TEXT("The graph required for this mutation phase was not found."),
					TEXT("graph_exists"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("name"));
				return false;
			}

			Graph->Modify();
			const TSharedPtr<const FJsonObject> NestedHeader = GetObject(Spec, TEXT("graph"));
			// Semantic projection emits a flat graph spec; typed callers may retain
			// the nested header. Match ResolveGraph's existing identity contract.
			const TSharedPtr<const FJsonObject> GraphHeader = NestedHeader.IsValid() ? NestedHeader : Spec;
			const TSharedPtr<const FJsonObject> Signature =
				GetObject(GraphHeader, TEXT("signature"));
			if (Phase == EGraphApplyPhase::Interface
				&& GraphHeader.IsValid() && GraphHeader->HasField(TEXT("signature")))
			{
				if (Kind != TEXT("function_graph") && Kind != TEXT("macro_graph"))
				{
					OutError = FDiagnostic::Failure(TEXT("blueprint_graph_signature_kind_unsupported"),
						TEXT("Authored graph signatures require a function_graph or macro_graph."), TEXT("signature_apply"));
					OutError.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint);
					OutError.CandidateName = Graph->GetName();
					OutError.PropertyPath = TEXT("graph_kind");
					OutError.ActualValue = MakeShared<FJsonValueString>(Kind);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("function_graph or macro_graph"));
					const FString Field = InputHeader.HasField(TEXT("graph_kind")) ? TEXT("graph_kind")
						: InputHeader.HasField(TEXT("kind")) ? TEXT("kind") : TEXT("signature");
					OutError.Source.InternalJsonPath = TEXT("/") + Field;
					UeAgentAuthoringDiagnostics::LocateFailure(OutError, InputHeader, TEXT("signature_apply"), OutError.TargetPath, Field);
					if (Field != TEXT("graph_kind") && OutError.SourceKind != TEXT("command_input")) OutError.SourceKind = TEXT("related_input");
					if (NestedHeader.IsValid() && OutError.Source.SourceUnit.IsEmpty())
					{
						UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError, TEXT("/graph"));
						UeAgentAuthoringDiagnostics::LocateFailure(OutError, Input, TEXT("signature_apply"), OutError.TargetPath, TEXT("graph"));
					}
					return false;
				}
				bool bApplied = false;
				if (!Signature.IsValid())
				{
					OutError = SignatureObjectFailure(Blueprint, InputHeader, TEXT("blueprint_graph_signature_invalid"),
						TEXT("A graph signature field must contain an object."), Graph->GetName());
				}
				else
				{
					bApplied = Kind == TEXT("function_graph")
						? ApplyFunctionGraphSignature(*Graph, Signature, OutError)
						: ApplyMacroGraphSignature(*Graph, Signature, OutError);
					if (!bApplied) LocateSignatureInputFailure(Blueprint, InputHeader, OutError);
				}
				if (!bApplied)
				{
					if (NestedHeader.IsValid() && OutError.Source.SourceUnit.IsEmpty())
					{
						UeAgentAuthoringDiagnostics::PrefixInternalDocumentFailure(OutError, TEXT("/graph"));
						UeAgentAuthoringDiagnostics::LocateFailure(OutError, Input, TEXT("signature_apply"), OutError.TargetPath, TEXT("graph"));
					}
					return false;
				}
				bOutNeedsCompile = true;
			}
			if (Phase == EGraphApplyPhase::Interface)
			{
				TArray<FStableMapEntry> InterfaceNodes;
				bool bNodesPresent = false;
				if (!GetObjectMap(
					Spec, TEXT("nodes"), InterfaceNodes, bNodesPresent, OutError))
				{
					return false;
				}
				for (const FStableMapEntry& NodeEntry : InterfaceNodes)
				{
					ON_SCOPE_EXIT
					{
						if (!OutError.IsEmpty()) LocateGraphItemFailure(Blueprint, *NodeEntry.Value, OutError);
					};
					const TSharedPtr<const FJsonObject> NodeSpec = MakeStableMapSpec(
						NodeEntry, EStableMapIdentity::Node, OutError);
					if (!NodeSpec.IsValid())
					{
						return false;
					}
					FString NodeKind;
					TryGetString(NodeSpec, TEXT("node_type"), NodeKind);
					NodeKind.TrimStartAndEndInline();
					NodeKind.ToLowerInline();
					if (NodeKind != TEXT("custom_event"))
					{
						continue;
					}
					EOperation NodeOperation;
					if (!ParseOperation(
						NodeSpec, EOperation::Upsert, NodeOperation, OutError))
					{
						return false;
					}
					UEdGraphNode* ExistingNode = ResolveNode(
						Blueprint, *Graph, NodeSpec, *NodeEntry.Value, NodeKind, OutError);
					if (!OutError.IsEmpty())
					{
						return false;
					}
					if (NodeOperation == EOperation::Remove)
					{
						continue;
					}
					if (NodeOperation == EOperation::Create && ExistingNode)
					{
						OutError = NodeMutationFailure(Blueprint, *NodeSpec, *NodeEntry.Value,
							TEXT("blueprint_node_already_exists"),
							TEXT("A custom event matching this create request already exists in the graph."),
							TEXT("node_apply"), TEXT("node_exists"),
							MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), TEXT("operation"));
						return false;
					}
					if (NodeOperation == EOperation::Update && !ExistingNode)
					{
						OutError = NodeMutationFailure(Blueprint, *NodeSpec, *NodeEntry.Value,
							TEXT("blueprint_node_not_found"),
							TEXT("The custom event required by this update request was not found in the graph."),
							TEXT("node_apply"), TEXT("node_exists"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("operation"));
						return false;
					}
					if (!ExistingNode)
					{
						const TMap<FString, UEdGraphNode*> EmptyNodeIds;
						ExistingNode = CreateNode(
						Blueprint, *Graph, NodeSpec, *NodeEntry.Value, NodeKind, EmptyNodeIds,
							bAllowScratchCandidateRebind, OutError);
						if (!ExistingNode)
						{
							return false;
						}
						++OutCounters.NodesAdded;
					}
					else
					{
						ExistingNode->Modify();
						if (!SetNodePosition(*ExistingNode, NodeSpec, OutError)) return false;
						++OutCounters.NodesUpdated;
					}
					UK2Node_CustomEvent* Event = Cast<UK2Node_CustomEvent>(ExistingNode);
					FString EventName;
					const bool bHasNewName = Event && TryGetString(NodeSpec, TEXT("event_name"), EventName)
						&& EventName != Event->CustomFunctionName.ToString();
					if (Observed && Event && NodeOperation != EOperation::Create
						&& (bHasNewName || NodeSpec->HasField(TEXT("signature"))))
						ObserveGraphCallers(Blueprint, Event->CustomFunctionName, Event->NodeGuid, nullptr, *Observed);
					if (bHasNewName)
						Event->OnRenameNode(EventName);
					const TSharedPtr<const FJsonObject> EventSignature =
						GetObject(NodeSpec, TEXT("signature"));
					if (!Event)
					{
						OutError = FDiagnostic::Failure(TEXT("blueprint_custom_event_signature_invalid"),
							TEXT("The selected or created node is not a custom event."), TEXT("signature_apply"));
						OutError.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint);
						OutError.PropertyPath = TEXT("node_class");
						OutError.ActualValue = ExistingNode ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(ExistingNode->GetClass()->GetPathName()))
							: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
						OutError.ExpectedValue = MakeShared<FJsonValueString>(UK2Node_CustomEvent::StaticClass()->GetPathName());
						OutError.Source.InternalJsonPath = TEXT("/");
						UeAgentAuthoringDiagnostics::LocateFailure(OutError, *NodeSpec, TEXT("signature_apply"), OutError.TargetPath);
						if (OutError.SourceKind != TEXT("command_input"))
							OutError.SourceKind = OutError.Source.SourceFile.IsEmpty() && OutError.Source.SourceUnit.IsEmpty() ? TEXT("native_result") : TEXT("related_input");
						return false;
					}
					if (NodeSpec->HasField(TEXT("signature")))
					{
						if (!EventSignature.IsValid())
						{
							OutError = SignatureObjectFailure(Blueprint, *NodeSpec, TEXT("blueprint_custom_event_signature_invalid"),
								TEXT("A custom event signature field must contain an object."), Event->GetNodeTitle(ENodeTitleType::ListView).ToString());
							return false;
						}
						if (!ApplyCustomEventSignature(*Event, EventSignature, OutError))
						{
							LocateSignatureInputFailure(Blueprint, *NodeSpec, OutError);
							return false;
						}
					}
					bOutNeedsCompile = true;
				}
				return true;
			}
			FGraphAffectedNodes AffectedNodes;
			if (Operation == EOperation::Replace)
			{
				TSet<FName> PreservedCustomEvents;
				TArray<FStableMapEntry> ReplacementNodes;
				bool bReplacementNodesPresent = false;
				if (!GetObjectMap(
					Spec, TEXT("nodes"), ReplacementNodes,
					bReplacementNodesPresent, OutError))
				{
					return false;
				}
				for (const FStableMapEntry& NodeEntry : ReplacementNodes)
				{
					const TSharedPtr<const FJsonObject> NodeSpec = MakeStableMapSpec(
						NodeEntry, EStableMapIdentity::Node, OutError);
					FString NodeKind;
					FString EventName;
					if (!NodeSpec.IsValid())
					{
						return false;
					}
					TryGetString(NodeSpec, TEXT("node_type"), NodeKind);
					if (NodeKind == TEXT("custom_event")
						&& TryGetString(NodeSpec, TEXT("event_name"), EventName))
					{
						PreservedCustomEvents.Add(FName(*EventName));
					}
				}
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (Node)
					{
						for (const UEdGraphPin* Pin : Node->Pins) CollectAffectedPinNodes(Pin, &AffectedNodes);
						Node->BreakAllNodeLinks();
					}
				}
				const TArray<UEdGraphNode*> ExistingNodes = Graph->Nodes;
				for (UEdGraphNode* Node : ExistingNodes)
				{
					const UK2Node_CustomEvent* Custom = Cast<UK2Node_CustomEvent>(Node);
					if (Node && !IsBuiltinNode(Node)
						&& (!Custom
							|| !PreservedCustomEvents.Contains(Custom->CustomFunctionName)))
					{
						Graph->RemoveNode(Node);
						++OutCounters.NodesRemoved;
					}
				}
			}

			UeAgentPaletteSearch::FBlueprintActionBatchScope Batch;
			TSet<UEdGraphNode*> NodesBeforeMutation;
			for (UEdGraphNode* Node : Graph->Nodes) NodesBeforeMutation.Add(Node);
			TMap<FString, UEdGraphNode*> NodeIds;
			TArray<FPendingPinDefault> DeferredPinDefaults;
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
				struct FNodeMutationSpec
				{
					TSharedPtr<const FJsonObject> Value;
					TSharedPtr<const FJsonObject> Input;
				};
				struct FPendingContextNode
				{
					TSharedPtr<const FJsonObject> Spec;
					TSharedPtr<const FJsonObject> Input;
					FString DependencyId;
					FString DependencyAlias;
					FDiagnostic ResolutionError;
				};
				TArray<FNodeMutationSpec> IndependentNodes;
				TArray<FPendingContextNode> ContextNodes;
				for (const FStableMapEntry& NodeEntry : Nodes)
				{
					ON_SCOPE_EXIT
					{
						if (!OutError.IsEmpty()) LocateGraphItemFailure(Blueprint, *NodeEntry.Value, OutError);
					};
					TSharedPtr<const FJsonObject> NodeSpec = MakeStableMapSpec(
						NodeEntry, EStableMapIdentity::Node, OutError);
					if (!NodeSpec.IsValid())
					{
						return false;
					}
					FString ProducerKind;
					TryGetString(NodeSpec, TEXT("node_type"), ProducerKind);
					ProducerKind.TrimStartAndEndInline();
					ProducerKind.ToLowerInline();
					if (ProducerKind == TEXT("custom_event"))
					{
						EOperation ProducerOperation;
						if (!ParseOperation(NodeSpec, EOperation::Upsert, ProducerOperation, OutError)) return false;
						TSharedRef<FJsonObject> BodySpec = MakeShared<FJsonObject>();
						BodySpec->Values = NodeSpec->Values;
						if (ProducerOperation != EOperation::Remove) BodySpec->SetStringField(TEXT("operation"), TEXT("update"));
						BodySpec->RemoveField(TEXT("signature"));
						NodeSpec = BodySpec;
					}
					const TSharedPtr<const FJsonObject> CandidateContext =
						GetObject(NodeSpec, TEXT("candidate_context"));
					if (NodeSpec->HasField(TEXT("candidate_context"))
						&& !CandidateContext.IsValid())
					{
						OutError = MutationInputFailure(Blueprint, *NodeEntry.Value,
							TEXT("blueprint_surface_candidate_context_invalid"),
							TEXT("candidate_context must be an object."), TEXT("candidate_context"),
							ProducerKind, TEXT("candidate_context"), MakeShared<FJsonValueString>(TEXT("object")));
						return false;
					}
					FString DependencyId;
					FString DependencyAlias;
					TryGetString(CandidateContext, TEXT("node"), DependencyId);
					TryGetString(CandidateContext, TEXT("alias"), DependencyAlias);
					DependencyId.TrimStartAndEndInline();
					if (DependencyId.IsEmpty())
					{
						IndependentNodes.Add({NodeSpec, NodeEntry.Value});
						continue;
					}
					ContextNodes.Add(
						{NodeSpec, NodeEntry.Value, DependencyId, DependencyAlias});
				}

				for (const FNodeMutationSpec& Record : IndependentNodes)
				{
					if (!ApplyNodeSpec(
							Blueprint, *Graph, Record.Value, *Record.Input, NodeIds,
							OutCounters, DeferredPinDefaults,
							bAllowScratchCandidateRebind,
							OutError, &AffectedNodes, Intents))
					{
						LocateGraphItemFailure(Blueprint, *Record.Input, OutError);
						return false;
					}
				}

				while (!ContextNodes.IsEmpty())
				{
					TSet<FString> PendingProducerIds;
					for (const FPendingContextNode& Pending : ContextNodes)
					{
						FString ProducerId;
						if (TryGetString(Pending.Spec, TEXT("id"), ProducerId) && !ProducerId.IsEmpty())
							PendingProducerIds.Add(ProducerId);
					}
					bool bProgress = false;
					for (int32 Index = ContextNodes.Num() - 1; Index >= 0; --Index)
					{
						FPendingContextNode& Pending = ContextNodes[Index];
						const FString& Dependency = Pending.DependencyId;
						// A pending producer must finish before its new interface is consumed.
						// Unchanged nodes are absent from a delta but remain valid native context.
						Pending.ResolutionError = {};
						if ((PendingProducerIds.Contains(Dependency) && !NodeIds.Contains(Dependency))
							|| !ResolveGraphNodeReference(*Graph, NodeIds, Dependency,
								*GetObject(Pending.Input, TEXT("candidate_context")), Pending.ResolutionError))
						{
							continue;
						}
						if (!ApplyNodeSpec(
							Blueprint, *Graph, ContextNodes[Index].Spec, *ContextNodes[Index].Input,
								NodeIds, OutCounters, DeferredPinDefaults,
								bAllowScratchCandidateRebind, OutError, &AffectedNodes, Intents))
						{
							LocateGraphItemFailure(Blueprint, *ContextNodes[Index].Input, OutError);
							return false;
						}
						ContextNodes.RemoveAt(Index);
						bProgress = true;
					}
					if (bProgress)
					{
						continue;
					}

					for (FPendingContextNode& Pending : ContextNodes)
					{
						if (!PendingProducerIds.Contains(Pending.DependencyId))
						{
							OutError = MoveTemp(Pending.ResolutionError);
							return false;
						}
					}
					// A registered producer can still fail native resolution. Preserve that
					// failure before diagnosing the remaining producer-only dependencies.
					for (FPendingContextNode& Pending : ContextNodes)
					{
						if (!Pending.ResolutionError.IsEmpty())
						{
							OutError = MoveTemp(Pending.ResolutionError);
							return false;
						}
					}
					TMap<FString, int32> ProducerIndices;
					for (int32 Index = 0; Index < ContextNodes.Num(); ++Index)
					{
						FString ProducerId;
						if (TryGetString(ContextNodes[Index].Spec, TEXT("id"), ProducerId) && !ProducerId.IsEmpty())
							ProducerIndices.Add(ProducerId, Index);
					}
					// All remaining dependencies point at pending producers. Follow the
					// actual edges so blocked descendants are not reported as cycle members.
					int32 Current = 0;
					TArray<int32> Trace;
					TMap<int32, int32> TracePositions;
					while (!TracePositions.Contains(Current))
					{
						TracePositions.Add(Current, Trace.Num());
						Trace.Add(Current);
						Current = ProducerIndices.FindChecked(ContextNodes[Current].DependencyId);
					}
					const int32 CycleStart = TracePositions.FindChecked(Current);
					OutError = MutationFailure(Blueprint, TEXT("blueprint_surface_candidate_context_dependency_cycle"),
						TEXT("The candidate context dependencies contain a cycle."),
						TEXT("candidate_context"), FString(), TEXT("cycle_node_count"),
						MakeShared<FJsonValueNumber>(Trace.Num() - CycleStart), MakeShared<FJsonValueNumber>(0));
					for (int32 Position = CycleStart; Position < Trace.Num(); ++Position)
					{
						const FPendingContextNode& Pending = ContextNodes[Trace[Position]];
						const TSharedPtr<const FJsonObject> Context = GetObject(Pending.Input, TEXT("candidate_context"));
						FString CandidateName;
						if (!TryGetString(GetObject(Pending.Input, TEXT("candidate")), TEXT("name"), CandidateName))
							TryGetString(Pending.Input, TEXT("node_type"), CandidateName);
						FDiagnostic Member = MutationFailure(Blueprint, TEXT("blueprint_candidate_context_cycle_member"),
							TEXT("This node depends on another pending node in the candidate context cycle."),
							TEXT("candidate_context"), CandidateName, TEXT("dependency_in_cycle"),
							MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
						Member.InternalContext = Pending.DependencyId;
						if (!Pending.DependencyAlias.IsEmpty())
						{
							auto Facts = MakeShared<FJsonObject>();
							Facts->SetStringField(TEXT("context_alias"), Pending.DependencyAlias);
							Member.ProposedValue = MakeShared<FJsonValueObject>(Facts);
						}
						LocateMutationInput(Member, *Context, TEXT("node"));
						OutError.Diagnostics.Add(MoveTemp(Member));
					}
					return false;
				}
			}

			TArray<FStableMapEntry> Edges;
			UeAgentAuthoringGraphLinks::FIntent ConnectionIntent;
			bool bEdgesPresent = false;
			if (!GetObjectMap(Spec, TEXT("edges"), Edges, bEdgesPresent, OutError)) return false;
			if (bEdgesPresent)
			{
				struct FPendingEdge
				{
					FString Key;
					TSharedPtr<const FJsonObject> Spec;
					TSharedPtr<const FJsonObject> Input;
				};
				TArray<FPendingEdge> PendingEdges;
				for (const FStableMapEntry& EdgeEntry : Edges)
				{
					const TSharedPtr<const FJsonObject> EdgeSpec = MakeStableMapSpec(
						EdgeEntry, EStableMapIdentity::Edge, OutError);
					if (!EdgeSpec.IsValid())
					{
						return false;
					}
					PendingEdges.Add({EdgeEntry.Key, EdgeSpec, EdgeEntry.Value});
				}
				const auto HasUnresolvedTypes = [&](const FPendingEdge& Edge)
				{
					EOperation Operation;
					FDiagnostic OperationError;
					if (!ParseOperation(Edge.Spec, EOperation::Upsert, Operation, OperationError)
						|| Operation == EOperation::Remove) return false;
					const auto PinFor = [&](const TCHAR* Field) -> UEdGraphPin*
					{
						const auto End = GetObject(Edge.Spec, Field);
						FString NodeRef, PinName;
						FDiagnostic Error;
						int32 Occurrence = 0;
						if (!TryGetString(End, TEXT("node"), NodeRef) || !TryGetString(End, TEXT("pin"), PinName)
							|| !TryReadPinOccurrence(End, *End, Occurrence, Error)) return nullptr;
						UEdGraphNode* Node = ResolveGraphNodeReference(*Graph, NodeIds, NodeRef, *End, Error);
						return Node ? ResolveEdgePin(*Node, End, PinName, Occurrence, Error) : nullptr;
					};
					const UEdGraphPin* From = PinFor(TEXT("from"));
					const UEdGraphPin* To = PinFor(TEXT("to"));
					// Invalid endpoints still go through ApplyEdgeSpec's full diagnostics.
					return From && To
						&& From->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard
						&& To->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard;
				};
				const auto ApplyPendingEdge = [&](const FPendingEdge& Edge)
				{
					if (ApplyEdgeSpec(Blueprint, *Graph, Edge.Spec, *Edge.Input, NodeIds, OutCounters, OutError, ConnectionIntent,
						TEXT("/edges/") + Edge.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")), &AffectedNodes, &NodesBeforeMutation)) return true;
					LocateGraphItemFailure(Blueprint, *Edge.Input, OutError, TEXT("edge_apply"));
					return false;
				};
				while (!PendingEdges.IsEmpty())
				{
					TArray<FPendingEdge> DeferredEdges;
					bool bApplied = false;
					for (const FPendingEdge& Edge : PendingEdges)
					{
						if (HasUnresolvedTypes(Edge)) { DeferredEdges.Add(Edge); continue; }
						if (!ApplyPendingEdge(Edge)) return false;
						bApplied = true;
					}
					if (!bApplied)
					{
						// No concrete anchor remains: native schemas own genuinely unresolved
						// graphs. Do not invent a type or reject otherwise legal partial edits.
						for (const FPendingEdge& Edge : DeferredEdges)
							if (!ApplyPendingEdge(Edge)) return false;
						break;
					}
					PendingEdges = MoveTemp(DeferredEdges);
				}
			}

			while (!DeferredPinDefaults.IsEmpty())
			{
				TArray<FPendingPinDefault> StillDeferred;
				FDiagnostic FirstDeferredError;
				bool bMadeProgress = false;
				const UEdGraphSchema* Schema = Graph->GetSchema();
				if (!Schema)
				{
					OutError = MutationFailure(Blueprint, TEXT("blueprint_graph_schema_missing"),
						TEXT("The graph has no schema to apply deferred pin defaults."), TEXT("pin_default_apply"),
						Graph->GetName(), TEXT("schema_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				for (const FPendingPinDefault& Pending : DeferredPinDefaults)
				{
					if (!Pending.Node)
					{
						OutError = MutationFailure(Blueprint, TEXT("blueprint_pin_default_node_missing"),
							TEXT("The node required by this deferred pin default is unavailable."), TEXT("pin_default_apply"),
							Graph->GetName(), TEXT("node_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						OutError.InternalContext = Pending.NodeId;
						LocateMutationInput(OutError, *Pending.Input, TEXT("pin_name"));
						return false;
					}
					FDiagnostic ApplyError;
					const EPinDefaultApplyResult Result = TryApplyPinDefault(
						*Pending.Node, *Schema, Pending.Spec, *Pending.Input, Pending.NodeId,
						OutCounters, ApplyError);
					if (Result == EPinDefaultApplyResult::Failed)
					{
						OutError = ApplyError;
						return false;
					}
					if (Result == EPinDefaultApplyResult::Deferred)
					{
						StillDeferred.Add(Pending);
						if (FirstDeferredError.IsEmpty()) FirstDeferredError = ApplyError;
						continue;
					}
					AffectedNodes.Add(Pending.Node);
					bMadeProgress = true;
				}
				if (StillDeferred.IsEmpty()) break;
				if (!bMadeProgress)
				{
					OutError = FirstDeferredError;
					return false;
				}
				DeferredPinDefaults = MoveTemp(StillDeferred);
			}
			// Match native compile/reload pin constraints after the complete graph edit.
			// Deferred defaults must finish before reconstruction replaces their pins.
			const UEdGraphSchema* SettlementSchema = Graph->GetSchema();
			if (SettlementSchema)
			{
				const TArray<UEdGraphNode*> SettlementOrder = Graph->Nodes;
				for (UEdGraphNode* Node : SettlementOrder)
				{
					if (Node && AffectedNodes.Contains(Node))
						SettlementSchema->ReconstructNode(*Node, true);
				}
			}
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
			bOutNeedsCompile = true;
			if (!ConnectionIntent.Verify(OutError))
			{
				LocateGraphItemFailure(Blueprint, *Spec, OutError, TEXT("edge_verify"));
				UeAgentAuthoringDiagnostics::ReportConsistencyWarning(TEXT("graph_connection_intent_not_retained"), OutError, TEXT("graph_connection_observation"));
				OutError.Reset();
			}
			if (Intents) Intents->Connections.Add(MoveTemp(ConnectionIntent));
			return true;
		}

		struct FGraphMutationSpec
		{
			TSharedPtr<const FJsonObject> Value;
			TSharedPtr<const FJsonObject> Input;
		};

		bool ApplyGraphRemovals(
			UBlueprint& Blueprint,
			const TArray<FStableMapEntry>& Entries,
			const FRemovalPlan& RemovalPlan,
			TArray<FGraphMutationSpec>& Specs,
			FMutationCounters& OutCounters,
			bool& bOutNeedsCompile,
			const bool bAllowScratchCandidateRebind,
			FDiagnostic& OutError, FPropertyIntents& Intents)
		{
			Specs.Reset();
			Specs.Reserve(Entries.Num());
			for (const FStableMapEntry& Entry : Entries)
			{
				const TSharedPtr<const FJsonObject> Spec = MakeStableMapSpec(
					Entry, EStableMapIdentity::Graph, OutError);
				if (!Spec.IsValid())
				{
					return false;
				}
				Specs.Add({Spec, Entry.Value});
			}
			// Node properties and graph signatures can also remove pins. Resolve
			// every explicit disconnection against the intact graph, before any
			// producer mutation, and retain its original absence intent below.
			{
				// The public records stay unchanged. Only this derived execution view
				// consumes explicit removals before producers and interface compilation.
				for (int32 GraphIndex = 0; GraphIndex < Specs.Num(); ++GraphIndex)
				{
					TSharedPtr<const FJsonObject>& Spec = Specs[GraphIndex].Value;
					TArray<FStableMapEntry> Nodes; bool bNodesPresent = false;
					if (!GetObjectMap(Spec, TEXT("nodes"), Nodes, bNodesPresent, OutError)) return false;
					bool bHasRemoval = false;
					TArray<FStableMapEntry> Edges; bool bEdgesPresent = false;
					if (!GetObjectMap(Spec, TEXT("edges"), Edges, bEdgesPresent, OutError)) return false;
					for (const FStableMapEntry& Entry : Edges)
					{
						const auto EdgeSpec = MakeStableMapSpec(Entry, EStableMapIdentity::Edge, OutError);
						EOperation Operation;
						if (!EdgeSpec || !ParseOperation(EdgeSpec, EOperation::Upsert, Operation, OutError)) return false;
						bHasRemoval |= Operation == EOperation::Remove;
					}
					for (const FStableMapEntry& Entry : Nodes)
					{
						const auto NodeSpec = MakeStableMapSpec(Entry, EStableMapIdentity::Node, OutError);
						EOperation Operation;
						if (!NodeSpec || !ParseOperation(NodeSpec, EOperation::Upsert, Operation, OutError)) return false;
						bHasRemoval |= Operation == EOperation::Remove;
					}
					if (!bHasRemoval) continue;
					FString Name, Kind;
					UEdGraph* Graph = ResolveGraph(Blueprint, Spec, *Specs[GraphIndex].Input, Name, Kind, OutError);
					if (!OutError.IsEmpty()) return false;
					if (!Graph)
					{
						OutError = MutationFailure(Blueprint, TEXT("blueprint_graph_removal_identity_unresolved"),
							TEXT("The graph required for explicit node or edge removals was not found."),
							TEXT("graph_remove"), Name, TEXT("graph_exists"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						LocateGraphHeaderInput(Blueprint, *Specs[GraphIndex].Input, OutError, TEXT("name"));
						return false;
					}
					auto Derived = MakeShared<FJsonObject>(); Derived->Values = Spec->Values;
					TMap<FString, UEdGraphNode*> NodeIds;
					if (bEdgesPresent)
					{
						auto RemainingEdges = MakeShared<FJsonObject>(); RemainingEdges->Values = GetObject(Spec, TEXT("edges"))->Values;
						UeAgentAuthoringGraphLinks::FIntent RemovedConnections;
						for (const FStableMapEntry& Entry : Edges)
						{
							const auto EdgeSpec = MakeStableMapSpec(Entry, EStableMapIdentity::Edge, OutError);
							EOperation Operation;
							if (!EdgeSpec || !ParseOperation(EdgeSpec, EOperation::Upsert, Operation, OutError)) return false;
							if (Operation != EOperation::Remove) continue;
							if (!ApplyEdgeSpec(Blueprint, *Graph, EdgeSpec, *Entry.Value, NodeIds, OutCounters, OutError, RemovedConnections,
								TEXT("/edges/") + Entry.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"))))
							{
								LocateGraphItemFailure(Blueprint, *Entry.Value, OutError, TEXT("edge_apply"));
								return false;
							}
							RemainingEdges->RemoveField(Entry.Key);
						}
						RemovedConnections.Observe();
						Intents.Connections.Add(MoveTemp(RemovedConnections));
						Derived->SetObjectField(TEXT("edges"), RemainingEdges);
					}
					auto RemainingNodes = MakeShared<FJsonObject>();
					if (bNodesPresent) RemainingNodes->Values = GetObject(Spec, TEXT("nodes"))->Values;
					for (const FStableMapEntry& Entry : Nodes)
					{
						const auto NodeSpec = MakeStableMapSpec(Entry, EStableMapIdentity::Node, OutError);
						EOperation Operation;
						if (!NodeSpec || !ParseOperation(NodeSpec, EOperation::Upsert, Operation, OutError))
						{
							LocateGraphItemFailure(Blueprint, *Entry.Value, OutError, TEXT("node_remove"));
							return false;
						}
						if (Operation != EOperation::Remove) continue;
						const auto* Records = RemovalPlan.NodeRecords.Find(Entries[GraphIndex].Key);
						UEdGraphNode* const* Planned = Records ? Records->Find(Entry.Key) : nullptr;
						if (!Planned || !*Planned || (*Planned)->GetGraph() != Graph)
						{
							const bool bHasPlannedNode = Planned && *Planned;
							OutError = NodeMutationFailure(Blueprint, *NodeSpec, *Entry.Value,
								TEXT("blueprint_node_removal_identity_unresolved"),
								bHasPlannedNode ? TEXT("The planned node no longer belongs to the graph being edited.")
									: TEXT("The node selected for removal is missing from the prepared removal plan."),
								TEXT("node_remove"),
								bHasPlannedNode ? TEXT("planned_node_graph_matches") : TEXT("planned_node_available"),
								MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
							LocateGraphItemFailure(Blueprint, *Entry.Value, OutError);
							return false;
						}
						// All identities and kinds were checked against the intact source.
						// Removing a producer can change a remaining caller's derived kind.
						if (!ApplyNodeRemoval(Blueprint, *Graph, *Planned, *NodeSpec, *Entry.Value, OutCounters, OutError))
						{
							LocateGraphItemFailure(Blueprint, *Entry.Value, OutError);
							return false;
						}
						RemainingNodes->RemoveField(Entry.Key);
					}
					if (bNodesPresent) Derived->SetObjectField(TEXT("nodes"), RemainingNodes);
					Spec = Derived;
					bOutNeedsCompile = true;
				}
			}

			// Complete all explicit graph removals before a member edit can refresh
			// the skeleton. Delegate signature graphs remain owned by Conform.
			for (int32 Index = Specs.Num() - 1; Index >= 0; --Index)
			{
				EOperation Operation;
				if (!ParseOperation(Specs[Index].Value, EOperation::Upsert, Operation, OutError))
				{
					UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Specs[Index].Input, TEXT("graph_remove"),
						UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), TEXT("operation"));
					return false;
				}
				if (Operation != EOperation::Remove) continue;
				if (!ApplyGraphSpec(Blueprint, Specs[Index].Value, *Specs[Index].Input, OutCounters, bOutNeedsCompile,
					bAllowScratchCandidateRebind, EGraphApplyPhase::Interface, OutError, &Intents)) return false;
				Specs.RemoveAt(Index);
			}
			return true;
		}

		bool ApplyGraphs(
			UBlueprint& Blueprint,
			TArray<FGraphMutationSpec>& Specs,
			FMutationCounters& OutCounters,
			bool& bOutNeedsCompile,
			const bool bAllowScratchCandidateRebind,
			FDiagnostic& OutError, FPropertyIntents* Intents)
		{
			struct FRetainedEventCall
			{
				int32 GraphIndex;
				FString NodeKey;
				TWeakObjectPtr<UK2Node_CallFunction> Node;
				FName OriginalName;
			};
			TArray<FRetainedEventCall> RetainedEventCalls;
			for (int32 Index = 0; Index < Specs.Num(); ++Index)
			{
				FString Name, Kind;
				UEdGraph* Graph = ResolveGraph(Blueprint, Specs[Index].Value, *Specs[Index].Input, Name, Kind, OutError);
				if (!OutError.IsEmpty()) return false;
				if (!Graph) continue;
				TArray<FStableMapEntry> Nodes; bool Present = false;
				if (!GetObjectMap(Specs[Index].Value, TEXT("nodes"), Nodes, Present, OutError)) return false;
				for (const FStableMapEntry& Entry : Nodes)
				{
					const auto NodeSpec = MakeStableMapSpec(Entry, EStableMapIdentity::Node, OutError);
					EOperation Operation;
					if (!NodeSpec || !ParseOperation(NodeSpec, EOperation::Upsert, Operation, OutError)) return false;
					if (Operation == EOperation::Create || Operation == EOperation::Remove) continue;
					FString Target;
					if (!TryGetString(NodeSpec, TEXT("custom_event"), Target)) continue;
					auto* Call = Cast<UK2Node_CallFunction>(ResolveNode(Blueprint, *Graph, NodeSpec, *Entry.Value, Kind, OutError));
					if (!OutError.IsEmpty()) return false;
					if (Call && Call->FunctionReference.GetMemberName() == FName(*Target))
						RetainedEventCalls.Add({Index, Entry.Key, Call, FName(*Target)});
				}
			}
			bool bInterfaceNeedsCompile = bOutNeedsCompile;
			for (const FGraphMutationSpec& Record : Specs)
				if (!ApplyGraphSpec(Blueprint, Record.Value, *Record.Input, OutCounters, bOutNeedsCompile,
					bAllowScratchCandidateRebind, EGraphApplyPhase::Interface, OutError, Intents)) return false;
			bInterfaceNeedsCompile |= bOutNeedsCompile;
			if (bInterfaceNeedsCompile)
			{
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
				if (!CompileBlueprint(Blueprint, OutError))
				{
					return false;
				}
				UeAgentPaletteSearch::InvalidateBlueprintActionBatch();
				FBlueprintActionDatabase::Get().RefreshAssetActions(&Blueprint);
			}
			// An unchanged reference in a whole body follows its native producer.
			// Rebase only the derived execution view after native GUID resolution;
			// explicit caller retargeting remains the requested value.
			for (const FRetainedEventCall& Retained : RetainedEventCalls)
			{
				if (!Retained.Node.IsValid() || Retained.Node->FunctionReference.GetMemberName() == Retained.OriginalName) continue;
				auto Derived = MakeShared<FJsonObject>(); Derived->Values = Specs[Retained.GraphIndex].Value->Values;
				auto Nodes = MakeShared<FJsonObject>(); Nodes->Values = GetObject(Specs[Retained.GraphIndex].Value, TEXT("nodes"))->Values;
				auto Node = MakeShared<FJsonObject>(); Node->Values = Nodes->GetObjectField(Retained.NodeKey)->Values;
				Node->SetStringField(TEXT("custom_event"), Retained.Node->FunctionReference.GetMemberName().ToString());
				Nodes->SetObjectField(Retained.NodeKey, Node); Derived->SetObjectField(TEXT("nodes"), Nodes); Specs[Retained.GraphIndex].Value = Derived;
			}

			for (const FGraphMutationSpec& Record : Specs)
			{
				if (!ApplyGraphSpec(
						Blueprint, Record.Value, *Record.Input, OutCounters, bOutNeedsCompile,
						bAllowScratchCandidateRebind, EGraphApplyPhase::Body,
						OutError, Intents))
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
			FDiagnostic& OutError)
		{
			OutVariableDefaults.Reset();
			OutClassDefaults.Reset();
			if (!Extension.IsValid()) return true;
			const TSharedPtr<const FJsonObject> Members = GetObject(Extension, TEXT("members"));
			if (Extension->HasField(TEXT("members")) && !Members.IsValid())
			{
				OutError = MutationDocumentFailure(*Extension, TEXT("blueprint_members_invalid"),
					TEXT("members must be an object."), TEXT("members"), Extension->TryGetField(TEXT("members")),
					MakeShared<FJsonValueString>(TEXT("object")));
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
					EOperation Operation;
					if (!ParseOperation(Spec, EOperation::Upsert, Operation, OutError)) return false;
					if (Operation == EOperation::Remove) continue;
					if (const auto* DefaultProperty = Spec->Values.Find(TEXT("default_property")))
					{
						OutVariableDefaults.Add(*DefaultProperty);
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
			FDiagnostic& OutError, FPropertyIntents& Intents)
		{
			if (VariableDefaults.IsEmpty() && !ClassDefaults.IsValid()) return true;
			UObject* CDO = Blueprint.GeneratedClass
				? Blueprint.GeneratedClass->GetDefaultObject(false)
				: nullptr;
			if (!CDO)
			{
				OutError = MutationFailure(Blueprint, TEXT("blueprint_class_default_object_missing"),
					Blueprint.GeneratedClass ? TEXT("The generated Blueprint class has no default object to receive the authored defaults.")
						: TEXT("The Blueprint has no generated class to receive the authored defaults."),
					TEXT("default_apply"), FString(), Blueprint.GeneratedClass ? TEXT("class_default_object_available") : TEXT("generated_class_available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				return false;
			}
			using UeAgentAuthoringDiagnostics::FScopedSourceLocations;
			const auto ClassSources = ClassDefaults ? FScopedSourceLocations::CaptureProjection(*ClassDefaults)
				: TMap<FString, UeAgentAuthoringDiagnostics::FSourceLocation>();
			const auto ResolveClassInput = [&ClassSources](const FString& Pointer)
				{ return FScopedSourceLocations::ResolveProjection(ClassSources, Pointer); };
			FScopedSourceLocations ClassInputScope(ClassDefaults.Get(), ResolveClassInput);
			TArray<TSharedPtr<FJsonValue>> Defaults = VariableDefaults;
			auto ProjectedClassDefaults = MakeShared<FJsonObject>();
			if (ClassDefaults.IsValid())
			{
				TArray<TSharedPtr<FJsonValue>> ClassDescriptors;
				if (!UeAgentDetailsAuthoringService::DecodePropertiesDocument(ClassDefaults, ClassDescriptors, OutError))
				{
					UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *ClassDefaults);
					return false;
				}
				ProjectedClassDefaults->SetArrayField(TEXT("properties"), ClassDescriptors);
				Defaults.Append(ClassDescriptors);
			}
			FScopedSourceLocations ClassProjectionScope(&ProjectedClassDefaults.Get(), ResolveClassInput);
			// Both public sources address this one native owner. Prepare them together
			// so the common duplicate/overlap rules apply before either source writes.
			auto Payload = MakeShared<FJsonObject>();
			Payload->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v2"));
			Payload->SetArrayField(TEXT("properties"), Defaults);
			return ApplyProperties(*CDO, Payload, OutCounters.ClassDefaultsApplied, OutError, &Blueprint, &Intents);
		}

		bool ApplyDeferredInheritedComponentProperties(
			UBlueprint& Blueprint,
			const TSharedPtr<const FJsonObject>& Extension,
			int32& OutApplied,
			FDiagnostic& OutError, FPropertyIntents* Intents = nullptr)
		{
			TArray<FStableMapEntry> Components;
			bool bPresent = false;
			if (!GetObjectMap(Extension, TEXT("components"), Components, bPresent, OutError)) return false;
			for (const FStableMapEntry& Entry : Components)
			{
				const auto Spec = MakeStableMapSpec(Entry, EStableMapIdentity::Component, OutError);
				if (!Spec.IsValid()) return false;
				EOperation Operation;
				if (!ParseOperation(Spec, EOperation::Upsert, Operation, OutError)) return false;
				if (Operation == EOperation::Remove) continue;
				FString Name;
				TryGetString(Spec, TEXT("name"), Name);
				if (UeAgentBlueprintAuthoringOps::FindSCSNodeByAnyName(Blueprint.SimpleConstructionScript, Name)) continue;
				const auto Properties = MakePropertyPayload(Spec, TEXT("properties"), OutError);
				if (!OutError.IsEmpty()) return false;
				if (!Properties.IsValid()) continue;
				if (!UeAgentBlueprintAuthoringOps::ResolveInheritedComponent(Blueprint, Name)) continue;
				UActorComponent* Component = UeAgentBlueprintAuthoringOps::ResolveEditableInheritedComponent(Blueprint, Name, OutError);
				if (!Component)
				{
					LocateComponentInput(Blueprint, *Spec, OutError, TEXT("name"));
					return false;
				}
				if (!ApplyProperties(*Component, Properties, OutApplied, OutError, &Blueprint, Intents)) return false;
			}
			return true;
		}

		bool ApplyDocument(
			UBlueprint& Blueprint,
			const TSharedPtr<const FJsonObject>& Extension,
			FMutationCounters& OutCounters,
			bool& bOutNeedsCompile,
			const bool bAllowScratchCandidateRebind,
			const bool bFinalizeDefaults,
			FDiagnostic& OutError, FPropertyIntents& Intents)
		{
			OutError.Reset();
			bOutNeedsCompile = false;
			if (!Extension.IsValid()) return true;
			ON_SCOPE_EXIT
			{
				if (!OutError.IsEmpty())
					UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Extension, TEXT("blueprint_apply"),
						UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint));
			};
			if (!FPreparedBlueprintMutation::ValidateGraphNames(Blueprint, Extension, OutError)
				|| !FPreparedBlueprintMutation::ValidateMemberNames(Blueprint, Extension, OutError)
				|| !FPreparedBlueprintMutation::ValidateCustomEventNames(Blueprint, Extension, OutError)
				|| !FPreparedBlueprintMutation::ValidateRemovals(Blueprint, Extension, OutError, !bAllowScratchCandidateRebind)) return false;
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
					OutError = MutationDocumentFailure(*Extension, TEXT("blueprint_provider_extension_unknown_field"),
						TEXT("This field is not supported by the Blueprint provider extension."), Field.Key,
						MakeShared<FJsonValueString>(Field.Key),
						MakeShared<FJsonValueString>(TEXT("parent_class, compile, save, interfaces, class_defaults, members, components or graphs")));
					return false;
				}
			}

			TArray<FStableMapEntry> Graphs;
			bool bGraphsPresent = false;
			FRemovalPlan RemovalPlan;
			TArray<FGraphMutationSpec> GraphSpecs;
			if (!GetObjectMap(Extension, TEXT("graphs"), Graphs, bGraphsPresent, OutError)
				|| !CollectRemovalPlan(Blueprint, Extension, RemovalPlan, OutError)
				|| !ApplyGraphRemovals(Blueprint, Graphs, RemovalPlan, GraphSpecs, OutCounters,
					bOutNeedsCompile, bAllowScratchCandidateRebind, OutError, Intents)) return false;

			FString ParentClassPath;
			if (TryGetString(Extension, TEXT("parent_class"), ParentClassPath))
			{
				UClass* ParentClass = UeAgentBlueprintAuthoringOps::ResolveClassByPath(ParentClassPath);
				if (!ParentClass || !FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
				{
					OutError = MutationFailure(Blueprint, TEXT("blueprint_parent_class_invalid"),
						ParentClass ? TEXT("The requested class cannot be used as a Blueprint parent.")
							: TEXT("The requested Blueprint parent class could not be resolved."),
						TEXT("blueprint_apply"), FString(), ParentClass ? TEXT("class_supports_blueprints") : TEXT("parent_class_available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					OutError.CandidateName = ParentClassPath;
					LocateMutationInput(OutError, *Extension, TEXT("parent_class"));
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
					Blueprint, Interfaces, OutCounters, bOutNeedsCompile, OutError, Intents)))
			{
				return false;
			}

			TArray<TSharedPtr<FJsonValue>> VariableDefaults;
			const TSharedPtr<const FJsonObject> Members = GetObject(Extension, TEXT("members"));
			if (Extension->HasField(TEXT("members")) && !Members.IsValid())
			{
				OutError = MutationDocumentFailure(*Extension, TEXT("blueprint_members_invalid"),
					TEXT("members must be an object."), TEXT("members"), Extension->TryGetField(TEXT("members")),
					MakeShared<FJsonValueString>(TEXT("object")));
				return false;
			}
			if (!ApplyMembers(
				Blueprint,
				Members,
				OutCounters,
				bOutNeedsCompile,
				VariableDefaults,
				OutError, Intents.ObservedMembers.IsSet() ? &Intents.ObservedMembers.GetValue() : nullptr))
			{
				return false;
			}

			TArray<FStableMapEntry> Components;
			bool bComponentsPresent = false;
			if (!GetObjectMap(Extension, TEXT("components"), Components, bComponentsPresent, OutError)) return false;
			if (bComponentsPresent
				&& !ApplyComponents(Blueprint, Components, OutCounters, bOutNeedsCompile, OutError, &Intents))
			{
				return false;
			}

			if (bGraphsPresent
				&& !ApplyGraphs(
					Blueprint,
					GraphSpecs,
					OutCounters,
					bOutNeedsCompile,
					bAllowScratchCandidateRebind,
					OutError, &Intents))
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
				Blueprint, VariableDefaults, ClassDefaults, OutCounters, OutError, Intents)
				|| !ApplyDeferredInheritedComponentProperties(
					Blueprint, Extension, OutCounters.ClassDefaultsApplied, OutError, &Intents))
			{
				return false;
			}
			FBlueprintEditorUtils::MarkBlueprintAsModified(&Blueprint);
			if (!Intents.Verify(Blueprint, OutError)) {
				UeAgentAuthoringDiagnostics::ReportConsistencyWarning(TEXT("blueprint_intent_not_retained"), OutError, TEXT("blueprint_intent_observation"));
				OutError.Reset();
			}
			return true;
		}
	}

	namespace
	{
		struct FGraphCreationBaseline
		{
			TMap<TWeakObjectPtr<UEdGraph>, TSet<TWeakObjectPtr<UEdGraphNode>>> GraphNodes;
			TMap<TWeakObjectPtr<UEdGraphNode>, TSet<FGuid>> PinIds;
		};
		struct FPreparedPinIdentity
		{
			FName Name;
			EEdGraphPinDirection Direction;
			FGuid Id;
			FGuid PersistentId;
		};
		struct FPreparedNodeIdentity
		{
			FString ClassPath;
			FString PublicClassPath;
			FGuid Id;
			bool bCreated = false;
			TArray<FPreparedPinIdentity> Pins;
		};
		struct FPreparedGraphCreation
		{
			FString RelativePath;
			FString Name;
			FString ClassPath;
			FString PublicClassPath;
			FGuid Id;
			bool bCreated = false;
			int32 CreatedNodeCount = 0;
			TArray<FPreparedNodeIdentity> Nodes;
		};

		FGraphCreationBaseline CaptureGraphCreationBaseline(UBlueprint& Blueprint)
		{
			FGraphCreationBaseline Result;
			TArray<UEdGraph*> Graphs;
			Blueprint.GetAllGraphs(Graphs);
			for (UEdGraph* Graph : Graphs)
			{
				if (!Graph) continue;
				auto& Nodes = Result.GraphNodes.Add(Graph);
				for (UEdGraphNode* Node : Graph->Nodes)
					if (Node)
					{
						Nodes.Add(Node);
						auto& Pins = Result.PinIds.Add(Node);
						for (const UEdGraphPin* Pin : Node->Pins) if (Pin) Pins.Add(Pin->PinId);
					}
			}
			return Result;
		}

		TArray<UEdGraphNode*> CollectCreatedNodes(UEdGraph& Graph, const FGraphCreationBaseline& Before)
		{
			TArray<UEdGraphNode*> Result;
			if (IsNativeGeneratedGraph(Graph)) return Result;
			const auto* Existing = Before.GraphNodes.Find(&Graph);
			for (UEdGraphNode* Node : Graph.Nodes)
				if (Node && (!Existing || !Existing->Contains(Node))) Result.Add(Node);
			return Result;
		}

		void CapturePreparedGraphCreations(UBlueprint& Blueprint, const FGraphCreationBaseline& Before,
			TArray<FPreparedGraphCreation>& OutCreations)
		{
			TArray<UEdGraph*> Graphs;
			Blueprint.GetAllGraphs(Graphs);
			for (UEdGraph* Graph : Graphs)
			{
				if (!Graph) continue;
				TArray<UEdGraphNode*> Nodes = CollectCreatedNodes(*Graph, Before);
				const int32 CreatedNodeCount = Nodes.Num();
				if (!IsNativeGeneratedGraph(*Graph))
					for (UEdGraphNode* Node : Graph->Nodes)
						if (Node)
							if (const auto* PreviousPins = Before.PinIds.Find(Node))
								if (Node->Pins.ContainsByPredicate([PreviousPins](const UEdGraphPin* Pin)
									{ return Pin && !PreviousPins->Contains(Pin->PinId); })) Nodes.Add(Node);
				if (Before.GraphNodes.Contains(Graph) && Nodes.IsEmpty()) continue;
				FPreparedGraphCreation& Creation = OutCreations.AddDefaulted_GetRef();
				Creation.RelativePath = Graph->GetPathName(&Blueprint);
				Creation.Name = Graph->GetName();
				Creation.ClassPath = Graph->GetClass()->GetPathName();
				Creation.PublicClassPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTypeObjectPath(&Blueprint, *Graph->GetClass());
				Creation.Id = Graph->GraphGuid;
				Creation.bCreated = !Before.GraphNodes.Contains(Graph);
				Creation.CreatedNodeCount = CreatedNodeCount;
				for (const UEdGraphNode* Node : Nodes)
				{
					FPreparedNodeIdentity& Identity = Creation.Nodes.AddDefaulted_GetRef();
					Identity.ClassPath = Node->GetClass()->GetPathName();
					Identity.PublicClassPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTypeObjectPath(&Blueprint, *Node->GetClass());
					Identity.Id = Node->NodeGuid;
					Identity.bCreated = !Before.PinIds.Contains(Node);
					for (const UEdGraphPin* Pin : Node->Pins)
					{
						if (Pin) Identity.Pins.Add({Pin->PinName, Pin->Direction, Pin->PinId, Pin->PersistentGuid});
					}
				}
			}
		}

		bool ApplyPreparedGraphCreations(UBlueprint& Blueprint, const FGraphCreationBaseline& Before,
			const TArray<FPreparedGraphCreation>& Creations, FDiagnostic& OutError)
		{
			TArray<UEdGraph*> Graphs;
			Blueprint.GetAllGraphs(Graphs);
			for (const FPreparedGraphCreation& Creation : Creations)
			{
				const auto FailReplay = [&](const TCHAR* Code, const FString& Reason, const FString& Field,
					TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
				{
					OutError = MutationFailure(Blueprint, Code, Reason, TEXT("graph_identity_replay"),
						Creation.Name, Field, MoveTemp(Actual), MoveTemp(Expected));
					OutError.InternalContext = Creation.RelativePath;
					return false;
				};
				UEdGraph* Graph = nullptr;
				for (UEdGraph* Candidate : Graphs)
				{
					if (!Candidate || Candidate->GetPathName(&Blueprint) != Creation.RelativePath) continue;
					if (Graph)
						return FailReplay(TEXT("blueprint_prepared_graph_identity_ambiguous"),
							TEXT("More than one graph matches the prepared graph's relative identity."),
							TEXT("observed_matching_graphs"), MakeShared<FJsonValueNumber>(2), MakeShared<FJsonValueNumber>(1));
					Graph = Candidate;
				}
				if (!Graph)
					return FailReplay(TEXT("blueprint_prepared_graph_identity_mismatch"),
						TEXT("The prepared graph was not found after applying the mutation."), TEXT("graph_exists"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				if (!Creation.Id.IsValid())
					return FailReplay(TEXT("blueprint_prepared_graph_identity_mismatch"),
						TEXT("The prepared graph has no valid identity to replay."), TEXT("prepared_graph_identity_valid"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				if (Graph->GetClass()->GetPathName() != Creation.ClassPath)
				{
					const FString ActualClass = UeAgentBlueprintAssetLifecycle::GetAuthoringTypeObjectPath(&Blueprint, *Graph->GetClass());
					const bool bSamePublicClass = ActualClass == Creation.PublicClassPath;
					TSharedPtr<FJsonValue> ActualValue = MakeShared<FJsonValueString>(ActualClass);
					TSharedPtr<FJsonValue> ExpectedValue = MakeShared<FJsonValueString>(Creation.PublicClassPath);
					if (bSamePublicClass)
					{
						ActualValue = MakeShared<FJsonValueBoolean>(false);
						ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					}
					FailReplay(TEXT("blueprint_prepared_graph_identity_mismatch"),
						TEXT("The applied native graph class identity differs from preparation."),
						bSamePublicClass ? TEXT("graph_class.native_identity_matches_prepared") : TEXT("graph_class"),
						MoveTemp(ActualValue), MoveTemp(ExpectedValue));
					OutError.ActualEvidence = MakeShared<FJsonValueString>(Graph->GetClass()->GetPathName());
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Creation.ClassPath);
					return false;
				}
				const bool bCreated = !Before.GraphNodes.Contains(Graph);
				if (Creation.bCreated != bCreated)
					return FailReplay(TEXT("blueprint_prepared_graph_identity_mismatch"),
						TEXT("The graph's new-versus-existing state differs from preparation."), TEXT("graph_was_created"),
						MakeShared<FJsonValueBoolean>(bCreated), MakeShared<FJsonValueBoolean>(Creation.bCreated));
				if (!Creation.bCreated && Graph->GraphGuid != Creation.Id)
					return FailReplay(TEXT("blueprint_prepared_graph_identity_mismatch"),
						TEXT("The existing graph identity differs from the prepared identity."), TEXT("graph_identity_matches_prepared"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				for (const UEdGraph* Other : Graphs)
					if (Other && Other != Graph && Other->GraphGuid == Creation.Id)
						return FailReplay(TEXT("blueprint_prepared_graph_identity_collision"),
							TEXT("The prepared graph identity is already used by another graph."), TEXT("graph_identity_is_unique"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				TArray<UEdGraphNode*> Nodes = CollectCreatedNodes(*Graph, Before);
				if (Nodes.Num() != Creation.CreatedNodeCount)
					return FailReplay(TEXT("blueprint_prepared_node_count_mismatch"),
						TEXT("The number of newly created nodes differs from preparation."), TEXT("created_node_count"),
						MakeShared<FJsonValueNumber>(Nodes.Num()), MakeShared<FJsonValueNumber>(Creation.CreatedNodeCount));
				// Existing nodes can acquire new pins through property/signature edits.
				// Match their stable node identities, then replay the same complete
				// pin interface contract used for newly created nodes below.
				for (const FPreparedNodeIdentity& Identity : Creation.Nodes)
					if (!Identity.bCreated)
					{
						TArray<UEdGraphNode*> Matches;
						for (UEdGraphNode* Candidate : Graph->Nodes)
							if (Candidate && Candidate->NodeGuid == Identity.Id) Matches.Add(Candidate);
						if (Matches.Num() != 1)
							return FailReplay(TEXT("blueprint_prepared_node_identity_mismatch"),
								TEXT("The existing node whose pins changed could not be uniquely matched to preparation."),
								TEXT("matching_existing_nodes"), MakeShared<FJsonValueNumber>(Matches.Num()), MakeShared<FJsonValueNumber>(1));
						Nodes.Add(Matches[0]);
					}
				// Preserve the complete native action result, including companion nodes.
				// Native result order and pin interface must agree before identities are reused.
				for (int32 Index = 0; Index < Nodes.Num(); ++Index)
				{
					const FPreparedNodeIdentity& Identity = Creation.Nodes[Index];
					UEdGraphNode* Node = Nodes[Index];
					const FString NodePath = FString(Identity.bCreated ? TEXT("created_nodes") : TEXT("reconstructed_nodes"))
						+ FString::Printf(TEXT("[%d]"), Index);
					if (!Identity.Id.IsValid())
						return FailReplay(TEXT("blueprint_prepared_node_interface_mismatch"),
							TEXT("The prepared node has no valid identity to replay."), NodePath + TEXT(".prepared_identity_valid"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					if (Node->GetClass()->GetPathName() != Identity.ClassPath)
					{
						const FString ActualClass = UeAgentBlueprintAssetLifecycle::GetAuthoringTypeObjectPath(&Blueprint, *Node->GetClass());
						const bool bSamePublicClass = ActualClass == Identity.PublicClassPath;
						TSharedPtr<FJsonValue> ActualValue = MakeShared<FJsonValueString>(ActualClass);
						TSharedPtr<FJsonValue> ExpectedValue = MakeShared<FJsonValueString>(Identity.PublicClassPath);
						if (bSamePublicClass)
						{
							ActualValue = MakeShared<FJsonValueBoolean>(false);
							ExpectedValue = MakeShared<FJsonValueBoolean>(true);
						}
						FailReplay(TEXT("blueprint_prepared_node_interface_mismatch"),
							TEXT("The applied native node class identity differs from preparation."),
							bSamePublicClass ? NodePath + TEXT(".class") + TEXT(".native_identity_matches_prepared") : NodePath + TEXT(".class"),
							MoveTemp(ActualValue), MoveTemp(ExpectedValue));
						OutError.ActualEvidence = MakeShared<FJsonValueString>(Node->GetClass()->GetPathName());
						OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Identity.ClassPath);
						return false;
					}
					if (Node->Pins.Num() != Identity.Pins.Num())
						return FailReplay(TEXT("blueprint_prepared_node_interface_mismatch"),
							TEXT("The node pin count differs from preparation."), NodePath + TEXT(".pin_count"),
							MakeShared<FJsonValueNumber>(Node->Pins.Num()), MakeShared<FJsonValueNumber>(Identity.Pins.Num()));
					for (const UEdGraph* Other : Graphs)
					{
						if (!Other) continue;
						for (const UEdGraphNode* Existing : Other->Nodes)
							if (Existing && Existing != Node && Existing->NodeGuid == Identity.Id)
								return FailReplay(TEXT("blueprint_prepared_node_identity_collision"),
									TEXT("The prepared node identity is already used by another node."), NodePath + TEXT(".identity_is_unique"),
									MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					}
					for (int32 PinIndex = 0; PinIndex < Node->Pins.Num(); ++PinIndex)
					{
						const UEdGraphPin* Pin = Node->Pins[PinIndex];
						const auto& Expected = Identity.Pins[PinIndex];
						const FString PinPath = NodePath + FString::Printf(TEXT(".pins[%d]"), PinIndex);
						if (!Pin)
							return FailReplay(TEXT("blueprint_prepared_pin_interface_mismatch"),
								TEXT("The node has a null pin at a prepared pin index."), PinPath + TEXT(".available"),
								MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						if (!Expected.Id.IsValid())
							return FailReplay(TEXT("blueprint_prepared_pin_interface_mismatch"),
								TEXT("The prepared pin has no valid identity to replay."), PinPath + TEXT(".prepared_identity_valid"),
								MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						if (Pin->PinName != Expected.Name)
							return FailReplay(TEXT("blueprint_prepared_pin_interface_mismatch"),
								TEXT("The pin name differs from the prepared pin name."), PinPath + TEXT(".name"),
								MakeShared<FJsonValueString>(Pin->PinName.ToString()), MakeShared<FJsonValueString>(Expected.Name.ToString()));
						if (Pin->Direction != Expected.Direction)
							return FailReplay(TEXT("blueprint_prepared_pin_interface_mismatch"),
								TEXT("The pin direction differs from the prepared pin direction."), PinPath + TEXT(".direction"),
								MakeShared<FJsonValueString>(Pin->Direction == EGPD_Input ? TEXT("input") : Pin->Direction == EGPD_Output ? TEXT("output") : TEXT("unknown")),
								MakeShared<FJsonValueString>(Expected.Direction == EGPD_Input ? TEXT("input") : Expected.Direction == EGPD_Output ? TEXT("output") : TEXT("unknown")));
						for (int32 Previous = 0; Previous < PinIndex; ++Previous)
							if (Identity.Pins[Previous].Id == Expected.Id)
								return FailReplay(TEXT("blueprint_prepared_pin_identity_collision"),
									TEXT("The prepared pin identity is duplicated at an earlier pin index."), PinPath + TEXT(".conflicting_pin_index"),
									MakeShared<FJsonValueNumber>(Previous), MakeShared<FJsonValueString>(TEXT("no duplicate prepared pin identity")));
					}
				}
				if (Creation.bCreated) Graph->GraphGuid = Creation.Id;
				for (int32 Index = 0; Index < Nodes.Num(); ++Index)
				{
					Nodes[Index]->NodeGuid = Creation.Nodes[Index].Id;
					for (int32 PinIndex = 0; PinIndex < Nodes[Index]->Pins.Num(); ++PinIndex)
					{
						Nodes[Index]->Pins[PinIndex]->PinId = Creation.Nodes[Index].Pins[PinIndex].Id;
						Nodes[Index]->Pins[PinIndex]->PersistentGuid = Creation.Nodes[Index].Pins[PinIndex].PersistentId;
					}
				}
			}
			return true;
		}
	}

	struct FPreparedBlueprintMutation::FImpl
	{
		TSharedPtr<const FJsonObject> Extension;
		TMap<FString, UE::UAI::Contract::FSourceLocation> InputSources;
		bool bRequiresCompile = false;
		bool bReparented = false;
		TArray<FPreparedGraphCreation> GraphCreations;
		TMap<FGuid, FName> ObservedVariableDefaults;
		FPropertyIntents ProposedIntents;
		FPropertyIntents ActualIntents;
		bool bAppliedToActual = false;
	};

	bool ChangeNativeMemberType(UBlueprint& Blueprint, FName VariableName,
		const FEdGraphPinType& DesiredType, FDiagnostic& OutError)
	{
		const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(&Blueprint, VariableName);
		if (Index == INDEX_NONE)
		{
			OutError = MutationFailure(Blueprint, TEXT("blueprint_member_not_found"),
				TEXT("The Blueprint member requested for a native type change was not found."),
				TEXT("member_type_change"), VariableName.ToString(), TEXT("member_exists"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		return Blueprint.NewVariables[Index].VarType == DesiredType
			|| ChangeMemberType(Blueprint, Blueprint.NewVariables[Index], DesiredType, OutError);
	}

	void CollectNativeMemberTypeDependents(UBlueprint& Blueprint, FName VariableName,
		TArray<UBlueprint*>& OutBlueprints)
	{
		TArray<UBlueprint*> Children;
		FNativeBlueprintEditorUtils::GetLoadedChildBlueprints(&Blueprint, Children);
		bool bHasConsumers = !FNativeBlueprintEditorUtils::GetNodesForVariable(VariableName, &Blueprint).IsEmpty();
		for (UBlueprint* Child : Children)
			bHasConsumers |= !FNativeBlueprintEditorUtils::GetNodesForVariable(VariableName, Child).IsEmpty();
		if (bHasConsumers) for (UBlueprint* Child : Children) OutBlueprints.AddUnique(Child);
	}

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
		PinGroupsResized += Other.PinGroupsResized;
		PinDefaultsApplied += Other.PinDefaultsApplied;
		EdgesAdded += Other.EdgesAdded;
		EdgesRemoved += Other.EdgesRemoved;
		ClassDefaultsApplied += Other.ClassDefaultsApplied;
	}

	FPreparedBlueprintMutation::FPreparedBlueprintMutation(TUniquePtr<FImpl> InImpl)
		: Impl(MoveTemp(InImpl))
	{
	}
	void FPreparedBlueprintMutation::AppendGraphObservationContext(bool bAfter, TMap<FString, FString>& Context) const
	{
		const auto& Graphs = Impl->ProposedIntents.ObservedGraphs;
		if (!Graphs.IsSet() || Graphs->IsEmpty()) return;
		TArray<FString> Guids;
		for (const auto& Pair : Graphs.GetValue()) {
			const FString Id = Pair.Key.ToString(EGuidFormats::DigitsWithHyphensLower);
			const FObservedGraph& Graph = Pair.Value;
			const FName PresentName = bAfter ? Graph.After : Graph.Before;
			const FName OtherName = bAfter ? Graph.Before : Graph.After;
			Guids.Add(Id);
			if (!PresentName.IsNone()) Context.Add(ObservedGraphNamePrefix + Id, PresentName.ToString());
			else Context.Add(ObservedAbsentGraphPrefix + Id, Graph.bAssetRoot ? OtherName.ToString() : FString());
			if (Graph.bAssetRoot) Context.Add(ObservedRootGraphPrefix + Id, TEXT("true"));
		}
		Guids.Sort();
		Context.Add(ObservedGraphGuidsContext, FString::Join(Guids, TEXT(",")));
	}
bool FPreparedBlueprintMutation::ReparentsBlueprint() const { return Impl->bReparented; }
const TMap<FGuid, FName>& FPreparedBlueprintMutation::GetObservedVariableDefaults() const { return Impl->ObservedVariableDefaults; }

	void FPreparedBlueprintMutation::AppendMemberObservationContext(bool bAfter, TMap<FString, FString>& Context) const
	{
		const auto& Prepared = Impl->ProposedIntents.ObservedMembers;
		const auto& Interfaces = Impl->ProposedIntents.InterfacePresence;
		if (Interfaces.IsSet() && !Interfaces->IsEmpty()) {
			Context.Add(ObservedMemberCapture, TEXT("interfaces"));
			for (const auto& Pair : Interfaces.GetValue())
				Context.Add(ObservedInterfacePrefix + Pair.Key, (bAfter ? Pair.Value.Value : Pair.Value.Key) ? TEXT("present") : TEXT("absent"));
		}
		if (!Prepared.IsSet() || Prepared->Members.IsEmpty()) return;
		Context.Add(ObservedMemberCapture, TEXT("declarations"));
		for (const auto& Pair : Prepared->Members) {
			const auto& Member = Pair.Value;
			const FName Name = bAfter ? Member.After : Member.Before;
			const TCHAR* Prefix = Name.IsNone()
				? (Member.bDelegate ? ObservedMissingDelegatePrefix : ObservedMissingMemberPrefix)
				: (Member.bDelegate ? ObservedDelegatePrefix : ObservedMemberPrefix);
			Context.Add(FString(Prefix) + Pair.Key.ToString(EGuidFormats::DigitsWithHyphensLower),
				(Name.IsNone() ? (bAfter ? Member.Before : Member.After) : Name).ToString());
		}
	AppendReferencedGraphGuids(Prepared->ReferencingGraphs, Context);
	}
	bool FPreparedBlueprintMutation::AppendComponentObservationContext(bool bAfter, TMap<FString, FString>& Context, FDiagnostic& Error) const
{
	const auto& Prepared = Impl->ProposedIntents.Components;
	if (!Prepared.IsSet()) return true;
	const auto& Applied = Impl->ActualIntents.Components;
	const auto& Expected = bAfter ? Prepared->After : Prepared->Before;
	if (bAfter && (!Applied.IsSet() || Applied->After.Num() != Expected.Num()
		|| Applied->Graphs.Num() != Prepared->Graphs.Num() || !Applied->Graphs.Includes(Prepared->Graphs)))
	{
		Error = FDiagnostic::Failure(TEXT("blueprint_observed_component_binding_missing"),
			TEXT("The native component and referenced graph scope does not match preparation."), TEXT("blueprint_state"));
		return false;
	}
	for (const auto& Item : Expected)
	{
		const FComponentPlacement* Bound = bAfter ? (Applied.IsSet() ? Applied->After.Find(Item.Key) : nullptr) : &Item.Value;
		const FComponentPlacement* Before = Prepared->Before.Find(Item.Key);
		if (!Bound || Bound->ClassPath != Item.Value.ClassPath || Bound->Parent != Item.Value.Parent
			|| Bound->Guid.IsValid() != Item.Value.Guid.IsValid()
			|| (Before && Before->Guid == Item.Value.Guid && Bound->Guid != Item.Value.Guid))
		{
			Error = FDiagnostic::Failure(TEXT("blueprint_observed_component_binding_missing"),
				TEXT("The native component presence, class, parent or retained identity does not match preparation."), TEXT("blueprint_state"));
			Error.PropertyPath = Item.Key.ToString();
			return false;
		}
		Context.Add(ObservedComponentPrefix + Item.Key.ToString(), Bound->Guid.IsValid()
			? Bound->Guid.ToString(EGuidFormats::DigitsWithHyphensLower) : FString());
	}
	AppendReferencedGraphGuids(Prepared->Graphs, Context);
	return true;
}

FPreparedBlueprintMutation::~FPreparedBlueprintMutation() = default;

	bool FPreparedBlueprintMutation::ValidateGraphNames(UBlueprint& Blueprint,
		const TSharedPtr<const FJsonObject>& ProviderExtension, FDiagnostic& OutError)
	{
		OutError.Reset();
		TArray<FStableMapEntry> Entries;
		bool bPresent = false;
		if (!GetObjectMap(ProviderExtension, TEXT("graphs"), Entries, bPresent, OutError)) return false;
		TSet<FName> RenamedNames;
		for (const FStableMapEntry& Entry : Entries)
		{
			const auto Spec = MakeStableMapSpec(Entry, EStableMapIdentity::Graph, OutError);
			if (!Spec.IsValid()) return false;
			EOperation Operation;
			if (!ParseOperation(Spec, EOperation::Upsert, Operation, OutError))
			{
				UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Entry.Value, TEXT("rename_preflight"),
					UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), TEXT("operation"));
				return false;
			}
			if (Operation == EOperation::Remove) continue;
			const auto Header = GetObject(Spec, TEXT("graph"));
			const auto Identity = Header.IsValid() ? Header : Spec;
			FString Name, Kind;
			if (Operation == EOperation::Create)
			{
				TryGetString(Identity, TEXT("name"), Name);
				if (!ValidateGraphNameIdentity(Blueprint, nullptr, Name, OutError))
				{
					LocateGraphHeaderInput(Blueprint, *Entry.Value, OutError, TEXT("name"));
					return false;
				}
				continue;
			}
			UEdGraph* Graph = ResolveGraph(Blueprint, Spec, *Entry.Value, Name, Kind, OutError);
			if (!OutError.IsEmpty()) return false;
			if (!Graph)
			{
				if (Operation == EOperation::Upsert && !ValidateGraphNameIdentity(Blueprint, nullptr, Name, OutError))
				{
					LocateGraphHeaderInput(Blueprint, *Entry.Value, OutError, TEXT("name"));
					return false;
				}
				continue;
			}
			if (!Identity->HasField(TEXT("name")) || Name == Graph->GetName()) continue;
			if (!ValidateGraphRename(Blueprint, *Graph, Name, Kind, OutError))
			{
				const FJsonObject& SourceHeader = GraphInputHeader(*Entry.Value);
				LocateGraphHeaderInput(Blueprint, *Entry.Value, OutError, OutError.PropertyPath == TEXT("graph_kind")
					? (SourceHeader.HasField(TEXT("graph_kind")) ? TEXT("graph_kind") : TEXT("kind")) : TEXT("name"));
				return false;
			}
			if (RenamedNames.Contains(FName(*Name)))
			{
				OutError = MutationInputFailure(Blueprint, GraphInputHeader(*Entry.Value), TEXT("blueprint_graph_rename_name_invalid"),
					TEXT("Another graph rename in this mutation already requests the same name."),
					TEXT("rename_preflight"), Graph->GetName(), TEXT("name"),
					MakeShared<FJsonValueString>(TEXT("a name unique among graph renames")));
				OutError.ActualValue = MakeShared<FJsonValueString>(Name);
				OutError.ActualValuePresent = true;
				return false;
			}
			RenamedNames.Add(FName(*Name));
		}
		return true;
	}

	bool FPreparedBlueprintMutation::ValidateCustomEventNames(UBlueprint& Blueprint,
		const TSharedPtr<const FJsonObject>& ProviderExtension, FDiagnostic& OutError)
	{
		OutError.Reset();
		bool PersistentSave = false;
		TryGetBool(ProviderExtension, TEXT("save"), PersistentSave);
		TArray<FStableMapEntry> Graphs;
		bool Present = false;
		if (!GetObjectMap(ProviderExtension, TEXT("graphs"), Graphs, Present, OutError)) return false;
		TSet<FName> Names;
		for (const FStableMapEntry& GraphEntry : Graphs)
		{
			const auto GraphSpec = MakeStableMapSpec(GraphEntry, EStableMapIdentity::Graph, OutError);
			EOperation GraphOperation;
			if (!GraphSpec || !ParseOperation(GraphSpec, EOperation::Upsert, GraphOperation, OutError))
			{
				UeAgentAuthoringDiagnostics::LocateFailure(OutError, *GraphEntry.Value, TEXT("rename_preflight"),
					UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), TEXT("operation"));
				return false;
			}
			if (GraphOperation == EOperation::Remove || GraphOperation == EOperation::Create) continue;
			FString GraphName, GraphKind;
			UEdGraph* Graph = ResolveGraph(Blueprint, GraphSpec, *GraphEntry.Value, GraphName, GraphKind, OutError);
			if (!OutError.IsEmpty()) return false;
			if (!Graph) continue;
			TArray<FStableMapEntry> Nodes;
			if (!GetObjectMap(GraphSpec, TEXT("nodes"), Nodes, Present, OutError))
			{
				UeAgentAuthoringDiagnostics::LocateFailure(OutError, *GraphEntry.Value, TEXT("rename_preflight"),
					UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), TEXT("nodes"));
				return false;
			}
			for (const FStableMapEntry& Entry : Nodes)
			{
				const auto Spec = MakeStableMapSpec(Entry, EStableMapIdentity::Node, OutError);
				EOperation Operation;
				if (!Spec || !ParseOperation(Spec, EOperation::Upsert, Operation, OutError))
				{
					UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Entry.Value, TEXT("rename_preflight"),
						UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), TEXT("operation"));
					return false;
				}
				if (Operation == EOperation::Remove || Operation == EOperation::Create) continue;
				FString Kind;
				TryGetString(Spec, TEXT("node_type"), Kind);
				if (!Kind.Equals(TEXT("custom_event"), ESearchCase::IgnoreCase)) continue;
				auto* Event = Cast<UK2Node_CustomEvent>(ResolveNode(Blueprint, *Graph, Spec, *Entry.Value, Kind, OutError));
				if (!OutError.IsEmpty()) return false;
				FString Name;
				if (!Event || !Spec->HasField(TEXT("event_name"))) continue;
				if (!TryGetString(Spec, TEXT("event_name"), Name))
				{
					OutError = MutationInputFailure(Blueprint, *Entry.Value, TEXT("blueprint_custom_event_rename_name_invalid"),
						TEXT("A custom event name must be a string."), TEXT("rename_preflight"),
						Event->CustomFunctionName.ToString(), TEXT("event_name"), MakeShared<FJsonValueString>(TEXT("a string")));
					return false;
				}
				if (Name == Event->CustomFunctionName.ToString()) continue;
				auto Forbidden = [&](const FString& Reason, const FString& Field, bool Actual, bool Expected)
				{
					OutError = NativeRenameFailure(Blueprint, TEXT("blueprint_custom_event_rename_forbidden"), Reason,
						Event->CustomFunctionName.ToString(), Name, Field,
						MakeShared<FJsonValueBoolean>(Actual), MakeShared<FJsonValueBoolean>(Expected));
					LocateMutationInput(OutError, *Entry.Value, TEXT("event_name"));
					return false;
				};
				auto InvalidName = [&](const FString& Reason, const FString& Expected)
				{
					OutError = MutationInputFailure(Blueprint, *Entry.Value, TEXT("blueprint_custom_event_rename_name_invalid"),
						Reason, TEXT("rename_preflight"), Event->CustomFunctionName.ToString(), TEXT("event_name"),
						MakeShared<FJsonValueString>(Expected));
					return false;
				};
				if (!Graph->bEditable) return Forbidden(TEXT("The custom event's graph is not editable."), TEXT("graph_editable"), false, true);
				if (!Event->bCanRenameNode) return Forbidden(TEXT("The custom event does not allow renaming."), TEXT("event_can_rename"), false, true);
				if (Event->HasAnyFlags(RF_Transient))
					return Forbidden(TEXT("A transient custom event cannot be renamed here."), TEXT("event_is_transient"), true, false);
				const auto Validator = Event->MakeNameValidator();
				const bool bHasValidator = Validator.IsValid();
				const EValidatorResult Validity = bHasValidator ? Validator->IsValid(Name) : EValidatorResult::AlreadyInUse;
				if (Name.IsEmpty()) return InvalidName(TEXT("A custom event name must be non-empty."), TEXT("a non-empty event name"));
				if (Name.TrimStartAndEnd() != Name)
					return InvalidName(TEXT("Custom event names cannot begin or end with whitespace."), TEXT("a non-empty event name without surrounding whitespace"));
				if (Names.Contains(FName(*Name)))
					return InvalidName(TEXT("Another custom event rename in this mutation already requests the same name."),
						TEXT("a name unique among custom event renames"));
				if (Validity != EValidatorResult::Ok && Validity != EValidatorResult::ExistingName)
				{
					if (!bHasValidator)
					{
						OutError = NativeRenameFailure(Blueprint, TEXT("blueprint_custom_event_rename_name_invalid"),
							TEXT("The custom event did not provide a native name validator."),
							Event->CustomFunctionName.ToString(), Name, TEXT("name_validator_available"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						LocateMutationInput(OutError, *Entry.Value, TEXT("event_name"));
						return false;
					}
					return InvalidName(FKismetNameValidator::GetErrorString(Name, Validity), TEXT("a name accepted by Unreal's event name validator"));
				}
				if (!ValidateNativeRenameEffects(Blueprint, Event->CustomFunctionName, FName(*Name),
					ENativeRenameKind::Function, OutError, Event, PersistentSave))
				{
					LocateMutationInput(OutError, *Entry.Value, TEXT("event_name"));
					return false;
				}
				Names.Add(FName(*Name));
			}
		}
		return true;
	}

	bool FPreparedBlueprintMutation::ValidateMemberNames(UBlueprint& Blueprint,
		const TSharedPtr<const FJsonObject>& ProviderExtension, FDiagnostic& OutError)
	{
		OutError.Reset();
		TSet<FName> NewNames;
		for (const bool bDelegate : {false, true})
		{
			TArray<FStableMapEntry> Entries;
			bool bPresent = false;
			if (!GetObjectMap(GetObject(ProviderExtension, TEXT("members")), bDelegate ? TEXT("delegates") : TEXT("variables"), Entries, bPresent, OutError)) return false;
			for (const FStableMapEntry& Entry : Entries)
			{
				const auto Spec = MakeStableMapSpec(Entry, EStableMapIdentity::Member, OutError);
				if (!Spec.IsValid()) return false;
				if (Spec->HasField(TEXT("new_name")))
				{
					OutError = MutationInputFailure(Blueprint, *Entry.Value,
						bDelegate ? TEXT("blueprint_delegate_rename_legacy_field_unsupported") : TEXT("blueprint_variable_rename_legacy_field_unsupported"),
						TEXT("The legacy new_name field is unsupported; use the member declaration's name field."),
						TEXT("rename_preflight"), FString(), TEXT("new_name"), MakeShared<FJsonValueString>(TEXT("name")));
					OutError.ActualValue = MakeShared<FJsonValueString>(TEXT("new_name"));
					OutError.ActualValuePresent = true;
					return false;
				}
				EOperation Operation;
				if (!ParseOperation(Spec, EOperation::Upsert, Operation, OutError))
				{
					UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Entry.Value, TEXT("rename_preflight"),
						UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint), TEXT("operation"));
					return false;
				}
				if (Operation == EOperation::Create || Operation == EOperation::Remove) continue;
				FBPVariableDescription* Member = ResolveMember(Blueprint, Spec, *Entry.Value, bDelegate, OutError);
				if (!OutError.IsEmpty()) return false;
				FString Name;
				if (!Member || !TryGetString(Spec, TEXT("name"), Name) || Name == Member->VarName.ToString()) continue;
				UEdGraph* Signature = bDelegate ? FBlueprintEditorUtils::GetDelegateSignatureGraphByName(&Blueprint, Member->VarName) : nullptr;
				auto Forbidden = [&](const FString& Reason, const FString& Field, bool Actual, bool Expected)
				{
					OutError = NativeRenameFailure(Blueprint,
						bDelegate ? TEXT("blueprint_delegate_rename_forbidden") : TEXT("blueprint_variable_rename_forbidden"),
						Reason, Member->VarName.ToString(), Name, Field,
						MakeShared<FJsonValueBoolean>(Actual), MakeShared<FJsonValueBoolean>(Expected));
					LocateMutationInput(OutError, *Entry.Value, TEXT("name"));
					return false;
				};
				auto InvalidName = [&](const FString& Reason, const FString& Expected)
				{
					OutError = MutationInputFailure(Blueprint, *Entry.Value,
						bDelegate ? TEXT("blueprint_delegate_rename_name_invalid") : TEXT("blueprint_variable_rename_name_invalid"),
						Reason, TEXT("rename_preflight"), Member->VarName.ToString(), TEXT("name"),
						MakeShared<FJsonValueString>(Expected));
					OutError.ActualValue = MakeShared<FJsonValueString>(Name);
					OutError.ActualValuePresent = true;
					return false;
				};
				// RepNotify rename invokes interactive confirmation. Reject it and
				// ineligible source members before native Modify.
				if (bDelegate && Member->VarType.PinCategory != UEdGraphSchema_K2::PC_MCDelegate)
					return Forbidden(TEXT("The resolved member is not a multicast delegate."), TEXT("is_multicast_delegate"), false, true);
				if (Member->RepNotifyFunc != NAME_None)
					return Forbidden(TEXT("Renaming a RepNotify member requires interactive native confirmation."),
						TEXT("repnotify_present"), true, false);
				if (!Member->VarGuid.IsValid())
					return Forbidden(TEXT("The member has no valid GUID to preserve its identity during rename."),
						TEXT("member_guid_valid"), false, true);
				if (bDelegate)
				{
					if (!Signature) return Forbidden(TEXT("The delegate signature graph was not found."), TEXT("signature_graph_exists"), false, true);
					if (!Blueprint.DelegateSignatureGraphs.Contains(Signature))
						return Forbidden(TEXT("The signature graph is not in this Blueprint's delegate graph collection."),
							TEXT("owned_delegate_signature_graph"), false, true);
					if (Signature->GetOuter() != &Blueprint)
						return Forbidden(TEXT("The delegate signature graph is owned by another object."),
							TEXT("signature_graph_owned_by_blueprint"), false, true);
					if (Signature->HasAnyFlags(RF_Transient))
						return Forbidden(TEXT("A transient delegate signature graph cannot be renamed here."),
							TEXT("signature_graph_is_transient"), true, false);
				}
				FKismetNameValidator Validator(&Blueprint, Member->VarName);
				const EValidatorResult Validity = Validator.IsValid(Name);
				if (Name.IsEmpty()) return InvalidName(TEXT("A member name must be non-empty."), TEXT("a non-empty member name"));
				if (Name.TrimStartAndEnd() != Name)
					return InvalidName(TEXT("Member names cannot begin or end with whitespace."), TEXT("a non-empty member name without surrounding whitespace"));
				if (NewNames.Contains(FName(*Name)))
					return InvalidName(TEXT("Another member rename in this mutation already requests the same name."),
						TEXT("a name unique among member renames"));
				if (Validity != EValidatorResult::Ok && Validity != EValidatorResult::ExistingName)
					return InvalidName(FKismetNameValidator::GetErrorString(Name, Validity), TEXT("a name accepted by Unreal's member name validator"));
				if (Signature && !Signature->Rename(*Name, Signature->GetOuter(), REN_Test))
				{
					OutError = NativeRenameFailure(Blueprint, TEXT("blueprint_delegate_rename_name_invalid"),
						TEXT("Unreal rejected a trial rename of the delegate signature graph."),
						Member->VarName.ToString(), Name, TEXT("native_rename_test_succeeded"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					LocateMutationInput(OutError, *Entry.Value, TEXT("name"));
					return false;
				}
				if (!ValidateNativeRenameEffects(Blueprint, Member->VarName, FName(*Name),
					bDelegate ? ENativeRenameKind::Delegate : ENativeRenameKind::Variable, OutError))
				{
					LocateMutationInput(OutError, *Entry.Value, TEXT("name"));
					return false;
				}
				NewNames.Add(FName(*Name));
			}
		}
		return true;
	}

	bool FPreparedBlueprintMutation::ValidateRemovals(UBlueprint& Blueprint,
		const TSharedPtr<const FJsonObject>& ProviderExtension, FDiagnostic& OutError, const bool /*bIncludeExternalConsumers*/)
	{
		OutError.Reset();
		FRemovalPlan RemovalPlan;
		// Check the requested identities and native eligibility. Existing callers,
		// including unloaded assets, are not an additional deletion veto. Native
		// removal/reconstruction owns their consequences; compilation reports actual
		// unresolved references instead of a guessed cross-asset prohibition.
		return CollectRemovalPlan(Blueprint, ProviderExtension, RemovalPlan, OutError);
	}

	bool FPreparedBlueprintMutation::CollectNativeDependentBlueprints(UBlueprint& Blueprint,
		const TSharedPtr<const FJsonObject>& ProviderExtension, TArray<UBlueprint*>& OutBlueprints,
		TArray<AActor*>& OutActors, FDiagnostic& OutError)
	{
		OutBlueprints.Reset();
		OutActors.Reset();
		FRemovalPlan Plan;
		if (!CollectRemovalPlan(Blueprint, ProviderExtension, Plan, OutError)) return false;
		for (const auto& Member : Plan.MemberTypeChanges)
			CollectNativeMemberTypeDependents(Blueprint, Member.Key, OutBlueprints);
		TArray<FStableMapEntry> Components;
		bool bPresent = false;
		if (!GetObjectMap(ProviderExtension, TEXT("components"), Components, bPresent, OutError)) return false;
		if (Blueprint.SimpleConstructionScript) for (const auto& Entry : Components)
		{
			const auto Spec = MakeStableMapSpec(Entry, EStableMapIdentity::Component, OutError);
			EOperation Operation;
			if (!Spec || !ParseOperation(Spec, EOperation::Upsert, Operation, OutError))
			{
				UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Entry.Value, TEXT("component_preflight"),
					UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(Blueprint));
				return false;
			}
			USCS_Node* Node = ResolveComponentNode(Blueprint, *Blueprint.SimpleConstructionScript, Spec, *Entry.Value, OutError);
			if (!OutError.IsEmpty()) return false;
			if (!Node) continue;
			FString ClassPath;
			TryGetString(Spec, TEXT("class"), ClassPath);
			const bool bRemovesTemplate = Operation == EOperation::Remove
				|| (Operation == EOperation::Replace && !ClassPath.IsEmpty()
					&& UeAgentBlueprintAuthoringOps::ResolveClassByPath(ClassPath) != Node->ComponentClass);
			if (bRemovesTemplate)
				UeAgentBlueprintAuthoringOps::CollectComponentRemovalDependents(Blueprint, *Node, OutBlueprints, OutActors);
		}
		return true;
	}

	bool FPreparedBlueprintMutation::Prepare(
		UBlueprint& ScratchBlueprint,
		const TSharedPtr<const FJsonObject>& ProviderExtension,
		TUniquePtr<FPreparedBlueprintMutation>& OutMutation,
		FMutationCounters& OutExpectedCounters,
		FDiagnostic& OutError)
	{
		OutMutation.Reset();
		OutExpectedCounters = FMutationCounters();
		UeAgentPaletteSearch::InvalidateBlueprintActionBatch();
		TUniquePtr<FImpl> NewImpl = MakeUnique<FImpl>();
		NewImpl->Extension = ProviderExtension;
		if (ProviderExtension)
			NewImpl->InputSources = UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(*ProviderExtension);
		if (ProviderExtension) {
		// These structural edits share one native before/final capture on this Blueprint.
			const auto OnlyFields = [&](std::initializer_list<FString> Allowed) {
				for (const auto& Field : ProviderExtension->Values)
					if (Field.Key != TEXT("compile") && Field.Key != TEXT("save")
						&& std::find(Allowed.begin(), Allowed.end(), Field.Key) == Allowed.end()) return false;
				return true;
			};
		const bool bStructuralScope = OnlyFields({TEXT("members"), TEXT("interfaces"), TEXT("graphs"), TEXT("components")});
		if (bStructuralScope && ProviderExtension->HasField(TEXT("members")))
				NewImpl->ProposedIntents.ObservedMembers.Emplace();
		if (bStructuralScope && (ProviderExtension->HasField(TEXT("graphs")) || ProviderExtension->HasField(TEXT("interfaces")))) {
				NewImpl->ProposedIntents.ObservedGraphs.Emplace();
				if (ProviderExtension->HasField(TEXT("interfaces"))) NewImpl->ProposedIntents.InterfacePresence.Emplace();
			}
			if (ProviderExtension->HasField(TEXT("class_defaults")) && OnlyFields({TEXT("class_defaults")}))
				NewImpl->ProposedIntents.PreparingDefaultVariables.Emplace();
		if (bStructuralScope && ProviderExtension->HasField(TEXT("components")))
				NewImpl->ProposedIntents.Components.Emplace();
		}
		TSet<FGuid> MembersBefore;
		for (const FBPVariableDescription& Member : ScratchBlueprint.NewVariables)
			MembersBefore.Add(Member.VarGuid);
		UClass* const ParentBefore = ScratchBlueprint.ParentClass;
		const FGraphCreationBaseline GraphsBefore = CaptureGraphCreationBaseline(ScratchBlueprint);
		if (!ApplyDocument(
			ScratchBlueprint,
			ProviderExtension,
			OutExpectedCounters,
			NewImpl->bRequiresCompile,
			true,
			true,
			OutError, NewImpl->ProposedIntents))
		{
			return false;
		}
		NewImpl->bReparented = ParentBefore != ScratchBlueprint.ParentClass;
		if (NewImpl->ProposedIntents.PreparingDefaultVariables.IsSet()) {
			NewImpl->ObservedVariableDefaults = MoveTemp(NewImpl->ProposedIntents.PreparingDefaultVariables.GetValue());
			NewImpl->ProposedIntents.PreparingDefaultVariables.Reset();
		}
		CapturePreparedGraphCreations(ScratchBlueprint, GraphsBefore, NewImpl->GraphCreations);
		if (NewImpl->ProposedIntents.InterfacePresence.IsSet() && !NewImpl->GraphCreations.IsEmpty()) {
			// InterfaceGraph implementations are private to the interface description.
			// Only regenerated graphs owned by the existing public graph exporter need separate rows.
			TArray<UEdGraph*> PublicGraphs;
			GatherExportGraphs(ScratchBlueprint, PublicGraphs);
			for (const FPreparedGraphCreation& Change : NewImpl->GraphCreations) {
				UEdGraph* const* Graph = PublicGraphs.FindByPredicate([&](const UEdGraph* Item) { return Item->GraphGuid == Change.Id; });
				if (!Graph) continue;
				auto& Scope = NewImpl->ProposedIntents.ObservedGraphs.GetValue();
				if (Change.bCreated)
					Scope.Add(Change.Id, {NAME_None, (*Graph)->GetFName(), (*Graph)->GetOuter() == &ScratchBlueprint});
				else ObserveExistingGraph(Scope, ScratchBlueprint, **Graph);
			}
		}
		if (!NewImpl->ProposedIntents.FinalizeNodeIdentities(ScratchBlueprint, OutError)) return false;
	// An inherited component override or an unbound member default has no local
	// observation for the whole edit. Keep private verification, publish no partial scope.
	auto& Prepared = NewImpl->ProposedIntents;
	if ((ProviderExtension && ProviderExtension->HasField(TEXT("components")) && !Prepared.Components.IsSet())
		|| (Prepared.ObservedMembers.IsSet() && !Prepared.ObservedMembers->bBoundDefaults))
	{
		Prepared.ObservedMembers.Reset();
		Prepared.InterfacePresence.Reset();
		Prepared.ObservedGraphs.Reset();
		Prepared.Components.Reset();
	}
		// Replay native-created member identities through the existing member parser,
		// before actual graph construction and compilation can bind to those members.
		if (const TSharedPtr<const FJsonObject> Members = GetObject(ProviderExtension, TEXT("members")))
		{

			auto PreparedExtension = MakeShared<FJsonObject>();
			PreparedExtension->Values = ProviderExtension->Values;
			auto PreparedMembers = MakeShared<FJsonObject>();
			PreparedMembers->Values = Members->Values;
			for (const TCHAR* Field : {TEXT("variables"), TEXT("delegates")})
			{
				TArray<FStableMapEntry> Entries;
				bool bPresent = false;
				if (!GetObjectMap(Members, Field, Entries, bPresent, OutError)) return false;
				if (!bPresent) continue;
				auto PreparedMap = MakeShared<FJsonObject>();
				for (const FStableMapEntry& Entry : Entries)
				{
					const auto Spec = MakeStableMapSpec(Entry, EStableMapIdentity::Member, OutError);
					if (!Spec.IsValid()) return false;
					auto PreparedSpec = MakeShared<FJsonObject>();
					PreparedSpec->Values = Entry.Value->Values;
					const FBPVariableDescription* Member = ResolveMember(
						ScratchBlueprint, Spec, *Entry.Value, FString(Field) == TEXT("delegates"), OutError);
					if (!OutError.IsEmpty()) return false;
					if (Member && !MembersBefore.Contains(Member->VarGuid))
					{
					if (!Member->VarGuid.IsValid())
					{
						OutError = MutationFailure(ScratchBlueprint, TEXT("blueprint_prepared_member_identity_invalid"),
							TEXT("The newly prepared Blueprint member has no valid GUID for replay."),
							TEXT("member_prepare"), Member->VarName.ToString(), TEXT("member_guid_valid"),
							MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						LocateMutationInput(OutError, *Entry.Value, TEXT("guid"));
						return false;
					}
					if (!SetOrValidateGuidIdentity(*PreparedSpec,
						{TEXT("guid"), TEXT("variable_guid"), TEXT("var_guid")},
						TEXT("guid"), Member->VarGuid, OutError))
					{
						UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Entry.Value, TEXT("member_prepare"),
							UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(ScratchBlueprint));
						return false;
					}
					}
					PreparedMap->SetObjectField(Entry.Key, PreparedSpec);
				}
				PreparedMembers->SetObjectField(Field, PreparedMap);
			}
			PreparedExtension->SetObjectField(TEXT("members"), PreparedMembers);
			NewImpl->Extension = PreparedExtension;
		}
		OutMutation = TUniquePtr<FPreparedBlueprintMutation>(
			new FPreparedBlueprintMutation(MoveTemp(NewImpl)));
		return true;
	}

	bool FPreparedBlueprintMutation::ApplyTo(
		UBlueprint& Blueprint,
		FMutationCounters& OutCounters,
		FDiagnostic& OutError) const
	{
		OutCounters = FMutationCounters();
		bool bNeedsCompile = false;
		const FGraphCreationBaseline GraphsBefore = CaptureGraphCreationBaseline(Blueprint);
		if (!Impl)
		{
			OutError = MutationFailure(Blueprint, TEXT("blueprint_prepared_mutation_missing"),
				TEXT("No prepared Blueprint mutation is available to apply."), TEXT("blueprint_replay"),
				FString(), TEXT("prepared_mutation_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		UeAgentAuthoringDiagnostics::FScopedSourceLocations Sources(
			Impl->InputSources.IsEmpty() ? nullptr : Impl->Extension.Get(),
			[this](const FString& Pointer)
			{
				return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(Impl->InputSources, Pointer);
			});
		Impl->ActualIntents.Entries.Reset();
		Impl->ActualIntents.Connections.Reset();
		if (Impl->ProposedIntents.Components.IsSet()) Impl->ActualIntents.Components.Emplace();
	Impl->bAppliedToActual = true;
		if (!ApplyDocument(
				Blueprint,
				Impl->Extension,
				OutCounters,
				bNeedsCompile,
				false,
				false,
				OutError, Impl->ActualIntents)
			|| !ApplyPreparedGraphCreations(Blueprint, GraphsBefore, Impl->GraphCreations, OutError)) return false;
		if (!Impl->ActualIntents.FinalizeNodeIdentities(Blueprint, OutError)) return false;
		return true;
	}

	bool FPreparedBlueprintMutation::FinalizeAfterCompile(
		UBlueprint& Blueprint,
		FMutationCounters& OutCounters,
		FDiagnostic& OutError) const
	{
		OutCounters = FMutationCounters();
		if (!Impl.IsValid())
		{
			OutError = MutationFailure(Blueprint, TEXT("blueprint_prepared_mutation_missing"),
				TEXT("No prepared Blueprint mutation is available to finalize after compilation."), TEXT("blueprint_finalize"),
				FString(), TEXT("prepared_mutation_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		UeAgentAuthoringDiagnostics::FScopedSourceLocations Sources(
			Impl->InputSources.IsEmpty() ? nullptr : Impl->Extension.Get(),
			[this](const FString& Pointer)
			{
				return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(Impl->InputSources, Pointer);
			});
		TArray<TSharedPtr<FJsonValue>> VariableDefaults;
		TSharedPtr<FJsonObject> ClassDefaults;
		if (!CollectDeferredDefaults(
			Impl->Extension, VariableDefaults, ClassDefaults, OutError))
		{
			return false;
		}
		if (!ApplyDeferredDefaults(
			Blueprint, VariableDefaults, ClassDefaults, OutCounters, OutError,
			Impl->bAppliedToActual ? Impl->ActualIntents : Impl->ProposedIntents)
			|| !ApplyDeferredInheritedComponentProperties(
				Blueprint, Impl->Extension, OutCounters.ClassDefaultsApplied, OutError,
				&(Impl->bAppliedToActual ? Impl->ActualIntents : Impl->ProposedIntents))) return false;
		if (OutCounters.ClassDefaultsApplied > 0)
		{
			// Deferred CDO edits happen after compilation built the native-property
			// initialization list. Refresh it (and derived classes) before new instances.
			FBlueprintEditorUtils::MarkBlueprintAsModified(&Blueprint);
		}
		if (!VerifyIntent(Blueprint, OutError)) {
			UeAgentAuthoringDiagnostics::ReportConsistencyWarning(TEXT("blueprint_intent_not_retained"), OutError, TEXT("blueprint_intent_observation"));
			OutError.Reset();
		}
		return true;
	}

	bool FPreparedBlueprintMutation::VerifyPropertyIntent(UBlueprint& Blueprint, FDiagnostic& OutError) const
	{
		return !Impl || (Impl->bAppliedToActual ? Impl->ActualIntents : Impl->ProposedIntents).Verify(Blueprint, OutError);
	}

	bool FPreparedBlueprintMutation::VerifyIntent(UBlueprint& Blueprint, FDiagnostic& OutError) const
	{
		if (!VerifyPropertyIntent(Blueprint, OutError)) return false;
		return !Impl || (Impl->bAppliedToActual ? Impl->ActualIntents : Impl->ProposedIntents).VerifyConnections(Blueprint, OutError);
	}

	bool FPreparedBlueprintMutation::RestoreFromBackup(
		const UBlueprint& Backup,
		UBlueprint& Target,
		FDiagnostic& OutError)
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
			TSet<UActorComponent*> CurrentTemplates;
			for (USCS_Node* Node : Target.SimpleConstructionScript->GetAllNodes())
			{
				if (Node && Node->ComponentTemplate) CurrentTemplates.Add(Node->ComponentTemplate);
			}
			if (const USCS_Node* DefaultRoot = Target.SimpleConstructionScript->GetDefaultSceneRootNode())
			{
				if (DefaultRoot->ComponentTemplate) CurrentTemplates.Add(DefaultRoot->ComponentTemplate);
			}
			for (UActorComponent* Template : CurrentTemplates)
			{
				Template->Rename(
					nullptr,
					GetTransientPackage(),
					REN_DontCreateRedirectors | REN_NonTransactional);
			}
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
		UBlueprintGeneratedClass* TargetGeneratedClass =
			Cast<UBlueprintGeneratedClass>(Target.GeneratedClass);
		Target.SimpleConstructionScript =
			Backup.SimpleConstructionScript && TargetGeneratedClass
				? DuplicateObject<USimpleConstructionScript>(
					Backup.SimpleConstructionScript,
					TargetGeneratedClass,
					Backup.SimpleConstructionScript->GetFName())
				: nullptr;
		if (Backup.SimpleConstructionScript && !Target.SimpleConstructionScript)
		{
			OutError = MutationFailure(Target, TEXT("blueprint_rollback_scs_restore_failed"),
				TargetGeneratedClass ? TEXT("Unreal failed to restore the Blueprint construction script from the backup.")
					: TEXT("The Blueprint construction script cannot be restored without a generated Blueprint class."),
				TEXT("blueprint_rollback"), FString(), TargetGeneratedClass ? TEXT("construction_script_restored") : TEXT("generated_class_available"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			UeAgentAuthoringDiagnostics::SetRollbackFacts(OutError, false);
			return false;
		}
		if (Target.SimpleConstructionScript && TargetGeneratedClass)
		{
			TMap<UObject*, UObject*> OldToNewTemplates;
			auto DuplicateTemplate = [TargetGeneratedClass, &OldToNewTemplates](USCS_Node* Node)
			{
				if (!Node || !Node->ComponentTemplate) return;
				UActorComponent* SourceTemplate = Node->ComponentTemplate;
				UActorComponent* DuplicatedTemplate = CastChecked<UActorComponent>(
					StaticDuplicateObject(
						SourceTemplate,
						TargetGeneratedClass,
						SourceTemplate->GetFName()));
				OldToNewTemplates.Add(SourceTemplate, DuplicatedTemplate);
				Node->ComponentTemplate = DuplicatedTemplate;
			};
			const TArray<USCS_Node*>& AllNodes = Target.SimpleConstructionScript->GetAllNodes();
			for (USCS_Node* Node : AllNodes)
			{
				DuplicateTemplate(Node);
			}
			if (USCS_Node* DefaultRoot = Target.SimpleConstructionScript->GetDefaultSceneRootNode())
			{
				if (!AllNodes.Contains(DefaultRoot) && DefaultRoot->ComponentTemplate)
				{
					if (UObject* const* Existing = OldToNewTemplates.Find(DefaultRoot->ComponentTemplate))
					{
						DefaultRoot->ComponentTemplate = CastChecked<UActorComponent>(*Existing);
					}
					else
					{
						DuplicateTemplate(DefaultRoot);
					}
				}
			}
			TargetGeneratedClass->SimpleConstructionScript =
				Target.SimpleConstructionScript;
			UeAgentBlueprintAssetLifecycle::RebindBlueprintSelfMemberReferences(Target);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Target);
			if (!CompileBlueprint(Target, OutError)) return false;
			if (UBlueprintGeneratedClass* RestoredGeneratedClass =
				Cast<UBlueprintGeneratedClass>(Target.GeneratedClass))
			{
				FArchiveReplaceObjectRef<UObject> ReplaceTemplateRefs(
					RestoredGeneratedClass, OldToNewTemplates);
			}
		}
		else
		{
			UeAgentBlueprintAssetLifecycle::RebindBlueprintSelfMemberReferences(Target);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Target);
			if (!CompileBlueprint(Target, OutError)) return false;
		}
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
