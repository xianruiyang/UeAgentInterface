#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectEnhancedInputMechanics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"

#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "Misc/ScopeExit.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::DomainAdapters::AssetDataSimpleEffect
{
	using namespace ContextResource;
	using namespace ExecutionMutation;
	using namespace StateSemantic;

	namespace
	{
        bool Fail(FExecutionMutationError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic)
        {
            OutError = MoveTemp(Diagnostic);
            return false;
        }

        FExecutionMutationError JsonFailure(const FString& Code, const FString& Reason, const FString& Field,
            const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected,
            const TCHAR* SourceKind = TEXT("authored_value"))
        {
            auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Field, Expected, Actual, TEXT("execution"), Reason);
            Error.SourceKind = SourceKind;
            Error.ActualValuePresent = Actual.IsValid();
            Error.ExpectedValuePresent = Expected.IsValid();
            if (Error.SourceKind == TEXT("native_result"))
            {
                Error.Source = {};
                Error.SemanticPointer.Reset();
            }
            return Error;
        }

        FExecutionMutationError ValueFailure(const FString& Code, const FString& Reason, const FString& Field,
            const FUeAgentCanonicalValue* Actual, const FString& Expected,
            const TCHAR* SourceKind = TEXT("authored_value"))
        {
            auto Error = JsonFailure(Code, Reason, Field, nullptr, MakeShared<FJsonValueString>(Expected), SourceKind);
            Error.ActualValuePresent = Actual != nullptr;
            Contract::FDiagnostic ProjectionError;
            if (Actual && !CanonicalJson::TryToJsonValue(*Actual, Error.ActualValue, ProjectionError))
            {
                ProjectionError.SourceKind = TEXT("native_result");
                Error.Diagnostics.Add(MoveTemp(ProjectionError));
            }
            return Error;
        }

        TSharedPtr<FJsonValue> ObjectClassValue(const UObject* Object)
        {
            return Object ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Object->GetClass()->GetPathName())) : nullptr;
        }

        bool FailDuplicateMapping(FExecutionMutationError& OutError, const FString& Action,
            const FString& Key, const int32 Index, const TCHAR* SourceKind = TEXT("authored_value"),
            const FString& Code = TEXT("asset_data_enhanced_input_mapping_identity_duplicate"))
        {
            const auto Actual = MakeShared<FJsonObject>();
            Actual->SetStringField(TEXT("action_ref"), Action);
            Actual->SetStringField(TEXT("key"), Key);
            auto Error = JsonFailure(Code, TEXT("This action and key pair occurs more than once."),
                TEXT("mappings/") + ::LexToString(Index), MakeShared<FJsonValueObject>(Actual),
                MakeShared<FJsonValueString>(TEXT("unique action_ref and key pair")), SourceKind);
            Error.OperationIndex = Index;
            return Fail(OutError, MoveTemp(Error));
        }

		const FUeAgentCanonicalValue* FindObjectField(
			const FUeAgentCanonicalValue& Object,
			const FString& Field)
		{
			if (Object.GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Object.GetObjectItems())
			{
				if (Item.Key == Field)
				{
					return &Item.Value.Get();
				}
			}
			return nullptr;
		}

		struct FInputSubobjectSpec final
		{
			UClass* Class = nullptr;
			UObject* Existing = nullptr;
		};

		struct FInputMappingSpec final
		{
			FEnhancedActionKeyMapping Base;
			TArray<FInputSubobjectSpec> Triggers;
			TArray<FInputSubobjectSpec> Modifiers;
		};

		void PinUnique(
			TArray<TStrongObjectPtr<UObject>>& Pins,
			UObject* Object)
		{
			if (!Object)
			{
				return;
			}
			for (const TStrongObjectPtr<UObject>& Pin : Pins)
			{
				if (Pin.Get() == Object)
				{
					return;
				}
			}
			Pins.Emplace(Object);
		}

        bool ValidateInputActionProperties(const FString& Code, FExecutionMutationError& OutError)
        {
            const UClass* Class = UInputAction::StaticClass();
            const FName Fields[] = {
                GET_MEMBER_NAME_CHECKED(UInputAction, ValueType),
                GET_MEMBER_NAME_CHECKED(UInputAction, bConsumeInput),
                GET_MEMBER_NAME_CHECKED(UInputAction, Triggers),
                GET_MEMBER_NAME_CHECKED(UInputAction, Modifiers)};
            for (const FName Field : Fields)
            {
                if (!FindFProperty<FProperty>(Class, Field))
                    return Fail(OutError, JsonFailure(Code,
                        TEXT("The InputAction class is missing a required reflected property."), Field.ToString(),
                        nullptr, MakeShared<FJsonValueString>(TEXT("reflected property on ") + Class->GetPathName()), TEXT("native_result")));
            }
            return true;
        }

		void NotifyInputActionProperty(
			UInputAction& Action,
			const FName PropertyName)
		{
			FProperty* Property =
				FindFProperty<FProperty>(
					UInputAction::StaticClass(),
					PropertyName);
			check(Property);
			FPropertyChangedEvent Event(Property);
			Action.PostEditChangeProperty(Event);
		}

        bool TryResolveClassArray(
            const FUeAgentCanonicalValue& Value,
            const UClass& BaseClass,
            const FString& ErrorPrefix,
            const FString& Field,
            TArray<UClass*>& OutClasses,
            FExecutionMutationError& OutError)
        {
            if (Value.GetKind() != EUeAgentValueKind::Array)
                return Fail(OutError, ValueFailure(ErrorPrefix + TEXT("_array_invalid"),
                    TEXT("The input class collection must be an array."), Field, &Value, TEXT("array of class references")));
            OutClasses.Reset();
            TSet<UClass*> Seen;
            for (const FUeAgentCanonicalValueRef& Item : Value.GetArrayItems())
            {
                const FString Path = Field + TEXT("/") + ::LexToString(OutClasses.Num());
                if (Item->GetKind() != EUeAgentValueKind::String || Item->GetText().IsEmpty())
                    return Fail(OutError, ValueFailure(ErrorPrefix + TEXT("_class_ref_invalid"),
                        TEXT("Each input class reference must be a non-empty string."), Path, &Item.Get(), TEXT("non-empty class reference")));
                const FString& ClassPath = Item->GetText();
                UClass* Class = LoadObject<UClass>(nullptr, *ClassPath);
                if (!Class || Class->GetPathName() != ClassPath || !Class->IsChildOf(&BaseClass)
                    || Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
                    return Fail(OutError, JsonFailure(ErrorPrefix + TEXT("_class_invalid"),
                        !Class ? TEXT("The class reference could not be loaded.")
                            : Class->GetPathName() != ClassPath ? TEXT("The class reference resolves to a different path.")
                            : !Class->IsChildOf(&BaseClass) ? TEXT("The class does not derive from the required base class.")
                            : Class->HasAnyClassFlags(CLASS_Abstract) ? TEXT("The input class is abstract and cannot be instantiated.")
                            : Class->HasAnyClassFlags(CLASS_Deprecated) ? TEXT("The input class is deprecated and cannot be instantiated.")
                            : TEXT("The input class is superseded and cannot be instantiated."),
                        Path, MakeShared<FJsonValueString>(Class && Class->GetPathName() != ClassPath ? Class->GetPathName() : ClassPath),
                        MakeShared<FJsonValueString>(Class && Class->GetPathName() != ClassPath ? ClassPath : BaseClass.GetPathName())));
                if (Seen.Contains(Class))
                    return Fail(OutError, JsonFailure(ErrorPrefix + TEXT("_class_duplicate"),
                        TEXT("The same class occurs more than once in the input list."), Path,
                        MakeShared<FJsonValueString>(ClassPath), MakeShared<FJsonValueString>(TEXT("a class not already present in this list"))));
                Seen.Add(Class);
                OutClasses.Add(Class);
            }
            return true;
        }

		template <typename TObject>
		bool CaptureExistingSubobjects(
			const TArray<TObjectPtr<TObject>>& Existing,
			TArray<TStrongObjectPtr<UObject>>& Pins,
			TArray<FInputSubobjectSpec>& OutSpecs,
            const FString& ErrorCode,
            const FString& Field,
            FExecutionMutationError& OutError)
		{
			OutSpecs.Reset();
			OutSpecs.Reserve(Existing.Num());
			for (const TObjectPtr<TObject>& Item : Existing)
			{
				TObject* Object = Item.Get();
				if (!Object)
				{
                    return Fail(OutError, JsonFailure(ErrorCode,
                        TEXT("The existing input subobject is unavailable."), Field + TEXT("/") + ::LexToString(OutSpecs.Num()),
                        nullptr, MakeShared<FJsonValueString>(TObject::StaticClass()->GetPathName()), TEXT("native_result")));
				}
				PinUnique(Pins, Object);
				OutSpecs.Add(
					FInputSubobjectSpec{
						Object->GetClass(),
						Object,
					});
			}
			return true;
		}

		void BuildReplacementSpecs(
			const TArray<UClass*>& Classes,
			TArray<FInputSubobjectSpec>& OutSpecs)
		{
			OutSpecs.Reset();
			OutSpecs.Reserve(Classes.Num());
			for (UClass* Class : Classes)
			{
				OutSpecs.Add(
					FInputSubobjectSpec{
						Class,
						nullptr,
					});
			}
		}

		void AppendMissingClassSpecs(
			const TArray<UClass*>& Classes,
			TArray<FInputSubobjectSpec>& InOutSpecs)
		{
			for (UClass* Class : Classes)
			{
				const bool bExists =
					InOutSpecs.ContainsByPredicate(
						[Class](
							const FInputSubobjectSpec& Spec)
						{
							return Spec.Class == Class;
						});
				if (!bExists)
				{
					InOutSpecs.Add(
						FInputSubobjectSpec{
							Class,
							nullptr,
						});
				}
			}
		}

		template <typename TObject>
		bool TryMaterializeSubobjects(
			UObject* Outer,
			const TArray<FInputSubobjectSpec>& Specs,
			const UClass& BaseClass,
            const FString& ErrorCode,
            const FString& Field,
            TArray<TObjectPtr<TObject>>& OutObjects,
            FExecutionMutationError& OutError)
		{
			OutObjects.Reset();
			OutObjects.Reserve(Specs.Num());
			for (const FInputSubobjectSpec& Spec : Specs)
			{
				TObject* Object =
					Cast<TObject>(Spec.Existing);
				if (!Object)
				{
					if (!Spec.Class
						|| !Spec.Class->IsChildOf(&BaseClass))
					{
                        return Fail(OutError, JsonFailure(ErrorCode,
                            !Spec.Class ? TEXT("The prepared input subobject has no class.")
                                : TEXT("The prepared input subobject class does not derive from the required base."),
                            Field + TEXT("/") + ::LexToString(OutObjects.Num()),
                            Spec.Class ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Spec.Class->GetPathName())) : nullptr,
                            MakeShared<FJsonValueString>(BaseClass.GetPathName()), TEXT("native_result")));
					}
					Object = NewObject<TObject>(
						Outer,
						Spec.Class,
						NAME_None,
						RF_Transactional);
				}
				if (!Object)
				{
                    auto Error = JsonFailure(ErrorCode, TEXT("Unreal failed to create the prepared input subobject."),
                        Field + TEXT("/") + ::LexToString(OutObjects.Num()), nullptr,
                        MakeShared<FJsonValueString>(Spec.Class->GetPathName()), TEXT("native_result"));
                    if (Outer) Error.TargetPath = Outer->GetPathName();
                    return Fail(OutError, MoveTemp(Error));
				}
				OutObjects.Add(Object);
			}
			return true;
		}

        bool TryParseValueType(
            const FUeAgentCanonicalValue& Value,
            EInputActionValueType& OutValueType,
            const FString& Code,
            FExecutionMutationError& OutError)
		{
			if (Value.GetKind() != EUeAgentValueKind::String)
			{
                return Fail(OutError, ValueFailure(Code,
                    TEXT("InputAction value_type must be a string."), TEXT("value_type"), &Value,
                    TEXT("boolean, axis1d, axis2d or axis3d")));
			}
			const FString& Name = Value.GetText();
			if (Name == TEXT("boolean"))
			{
				OutValueType = EInputActionValueType::Boolean;
				return true;
			}
			if (Name == TEXT("axis1d"))
			{
				OutValueType = EInputActionValueType::Axis1D;
				return true;
			}
			if (Name == TEXT("axis2d"))
			{
				OutValueType = EInputActionValueType::Axis2D;
				return true;
			}
			if (Name == TEXT("axis3d"))
			{
				OutValueType = EInputActionValueType::Axis3D;
				return true;
			}
            return Fail(OutError, ValueFailure(Code,
                TEXT("InputAction value_type is not supported."), TEXT("value_type"), &Value,
                TEXT("boolean, axis1d, axis2d or axis3d")));
		}

		bool SameMappingIdentity(
			const FInputMappingSpec& Spec,
			const UInputAction* Action,
			const FKey& Key)
		{
			return Spec.Base.Action == Action
				&& Spec.Base.Key == Key;
		}

		bool SameMappingIdentity(
			const FEnhancedActionKeyMapping& Mapping,
			const UInputAction* Action,
			const FKey& Key)
		{
			return Mapping.Action == Action
				&& Mapping.Key == Key;
		}

        bool TryReadMappingIdentity(
            IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
            const FUeAgentCanonicalValue& Value,
            const bool bIdentityOnly,
            const FString& Field,
            UInputAction*& OutAction,
            FKey& OutKey,
            TArray<UClass*>& OutTriggers,
            TArray<UClass*>& OutModifiers,
            FExecutionMutationError& OutError)
        {
            const int32 ExpectedFieldCount = bIdentityOnly ? 2 : 4;
            if (Value.GetKind() != EUeAgentValueKind::Object || Value.GetObjectItems().Num() != ExpectedFieldCount)
                return Fail(OutError, ValueFailure(TEXT("asset_data_input_mapping_object_invalid"),
                    TEXT("The mapping object has the wrong shape."), Field, &Value,
                    bIdentityOnly ? TEXT("object containing exactly action_ref and key")
                        : TEXT("object containing exactly action_ref, key, triggers and modifiers")));
            const FUeAgentCanonicalValue* ActionRef = FindObjectField(Value, TEXT("action_ref"));
            const FUeAgentCanonicalValue* Key = FindObjectField(Value, TEXT("key"));
            if (!ActionRef || !Key || ActionRef->GetKind() != EUeAgentValueKind::String
                || ActionRef->GetText().IsEmpty() || Key->GetKind() != EUeAgentValueKind::String || Key->GetText().IsEmpty())
            {
                // Missing fields precede type checks, matching the original validation order.
                const bool bAction = !ActionRef || (Key && (ActionRef->GetKind() != EUeAgentValueKind::String || ActionRef->GetText().IsEmpty()));
                return Fail(OutError, ValueFailure(TEXT("asset_data_input_mapping_identity_invalid"),
                    TEXT("The mapping identity requires non-empty action_ref and key strings."),
                    Field + (bAction ? TEXT("/action_ref") : TEXT("/key")), bAction ? ActionRef : Key, TEXT("non-empty string")));
            }
            UObject* Resolved = nullptr;
            if (!Resolver.TryResolveExisting(ActionRef->GetText(), Resolved, OutError)) return false;
            OutAction = Cast<UInputAction>(Resolved);
            if (!OutAction)
            {
                auto Actual = MakeShared<FJsonObject>();
                Actual->SetStringField(TEXT("reference"), ActionRef->GetText());
                Actual->SetField(TEXT("class"), Resolved ? ObjectClassValue(Resolved) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()));
                return Fail(OutError, JsonFailure(TEXT("asset_data_input_mapping_action_class_invalid"),
                    TEXT("The referenced asset is not an InputAction."), Field + TEXT("/action_ref"),
                    MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(UInputAction::StaticClass()->GetPathName())));
            }
            OutKey = FKey(FName(*Key->GetText()));
            if (!OutKey.IsValid() || OutKey.GetFName().ToString() != Key->GetText())
                return Fail(OutError, JsonFailure(TEXT("asset_data_input_mapping_key_invalid"),
                    TEXT("The mapping key is not a registered key name with exact spelling."), Field + TEXT("/key"),
                    MakeShared<FJsonValueString>(Key->GetText()), MakeShared<FJsonValueString>(TEXT("registered key name with exact spelling"))));
            OutTriggers.Reset();
            OutModifiers.Reset();
            if (bIdentityOnly) return true;
            const FUeAgentCanonicalValue* Triggers = FindObjectField(Value, TEXT("triggers"));
            const FUeAgentCanonicalValue* Modifiers = FindObjectField(Value, TEXT("modifiers"));
            if (!Triggers || !Modifiers)
                return Fail(OutError, ValueFailure(TEXT("asset_data_input_mapping_object_invalid"),
                    TEXT("The mapping is missing a required class collection."), Field + (!Triggers ? TEXT("/triggers") : TEXT("/modifiers")),
                    nullptr, TEXT("class reference array")));
            return TryResolveClassArray(*Triggers, *UInputTrigger::StaticClass(),
                    TEXT("asset_data_input_mapping_trigger"), Field + TEXT("/triggers"), OutTriggers, OutError)
                && TryResolveClassArray(*Modifiers, *UInputModifier::StaticClass(),
                    TEXT("asset_data_input_mapping_modifier"), Field + TEXT("/modifiers"), OutModifiers, OutError);
        }

		bool TryBuildMappingSpecFromExisting(
            const FEnhancedActionKeyMapping& Existing,
            const FString& Field,
            TArray<TStrongObjectPtr<UObject>>& Pins,
			FInputMappingSpec& OutSpec,
			FExecutionMutationError& OutError)
		{
			if (!Existing.Action || !Existing.Key.IsValid())
			{
                return Fail(OutError, JsonFailure(TEXT("asset_data_input_mapping_existing_identity_invalid"),
                    !Existing.Action ? TEXT("The existing mapping has no input action.") : TEXT("The existing mapping key is invalid."),
                    Field + (!Existing.Action ? TEXT("/action_ref") : TEXT("/key")),
                    !Existing.Action ? nullptr : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Existing.Key.GetFName().ToString())),
                    MakeShared<FJsonValueString>(!Existing.Action ? TEXT("InputAction asset") : TEXT("registered key")), TEXT("native_result")));
			}
			OutSpec.Base = Existing;
			PinUnique(
				Pins,
				const_cast<UInputAction*>(
					Existing.Action.Get()));
			return CaptureExistingSubobjects(
					Existing.Triggers,
					Pins,
					OutSpec.Triggers,
					TEXT(
						"asset_data_input_mapping_"
						"existing_trigger_invalid"),
                    Field + TEXT("/triggers"), OutError)
				&& CaptureExistingSubobjects(
					Existing.Modifiers,
					Pins,
					OutSpec.Modifiers,
					TEXT(
						"asset_data_input_mapping_"
						"existing_modifier_invalid"),
                    Field + TEXT("/modifiers"), OutError);
		}

		bool TryBuildProjectionString(
			const FString& Text,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError StateError;
			return FUeAgentCanonicalValue::TryString(
					Text,
					Resources,
					OutValue,
					StateError)
				? true
				: Fail(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(StateError.Code.IsEmpty()
						? TEXT(
							"asset_data_enhanced_input_"
							"projection_string_invalid")
						: StateError.Code, StateError, TEXT("execution")));
		}

		bool TryBuildProjectionBoolean(
			const bool bValue,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError StateError;
			return FUeAgentCanonicalValue::TryBoolean(
					bValue,
					Resources,
					OutValue,
					StateError)
				? true
				: Fail(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(StateError.Code.IsEmpty()
						? TEXT(
							"asset_data_enhanced_input_"
							"projection_boolean_invalid")
						: StateError.Code, StateError, TEXT("execution")));
		}

		bool TryBuildProjectionArray(
			const TArray<FUeAgentCanonicalValueRef>& Items,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError StateError;
			return FUeAgentCanonicalValue::TryArray(
					Items,
					Resources,
					OutValue,
					StateError)
				? true
				: Fail(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(StateError.Code.IsEmpty()
						? TEXT(
							"asset_data_enhanced_input_"
							"projection_array_invalid")
						: StateError.Code, StateError, TEXT("execution")));
		}

		bool TryBuildProjectionObject(
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			Items.Sort(
				[](
					const TPair<
						FString,
						FUeAgentCanonicalValueRef>& Left,
					const TPair<
						FString,
						FUeAgentCanonicalValueRef>& Right)
				{
					return Left.Key < Right.Key;
				});
			FUeAgentStateSemanticError StateError;
			return FUeAgentCanonicalValue::TryObject(
					Items,
					Resources,
					OutValue,
					StateError)
				? true
				: Fail(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(StateError.Code.IsEmpty()
						? TEXT(
							"asset_data_enhanced_input_"
							"projection_object_invalid")
						: StateError.Code, StateError, TEXT("execution")));
		}

		bool TryReadProjectionString(
			const FUeAgentCanonicalValue& Value,
			FString& OutText)
		{
			if (Value.GetKind() != EUeAgentValueKind::String
				|| Value.GetText().IsEmpty())
			{
				OutText.Reset();
				return false;
			}
			OutText = Value.GetText();
			return true;
		}

        bool TryFormatValueType(
            const EInputActionValueType ValueType,
            FString& OutText,
            FExecutionMutationError& OutError)
		{
			switch (ValueType)
			{
			case EInputActionValueType::Boolean:
				OutText = TEXT("boolean");
				return true;
			case EInputActionValueType::Axis1D:
				OutText = TEXT("axis1d");
				return true;
			case EInputActionValueType::Axis2D:
				OutText = TEXT("axis2d");
				return true;
			case EInputActionValueType::Axis3D:
				OutText = TEXT("axis3d");
				return true;
			default:
				OutText.Reset();
                return Fail(OutError, JsonFailure(TEXT("asset_data_enhanced_input_native_value_type_invalid"),
                    TEXT("The native InputAction value type is not supported."), TEXT("value_type"),
                    MakeShared<FJsonValueNumber>(static_cast<int32>(ValueType)),
                    MakeShared<FJsonValueString>(TEXT("Boolean, Axis1D, Axis2D or Axis3D")), TEXT("native_result")));
			}
		}

		template <typename TObject>
		bool TryBuildClassCollectionProjection(
			const TArray<TObjectPtr<TObject>>& Objects,
            const UClass& BaseClass,
            const FString& Field,
            FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			TArray<FUeAgentCanonicalValueRef> Items;
			Items.Reserve(Objects.Num());
			for (const TObjectPtr<TObject>& Item : Objects)
			{
				const UObject* Object = Item.Get();
				const UClass* Class =
					Object ? Object->GetClass() : nullptr;
				FUeAgentCanonicalValuePtr ClassPath;
                if (!Class || !Class->IsChildOf(&BaseClass))
                    return Fail(OutError, JsonFailure(TEXT("asset_data_enhanced_input_existing_subobject_invalid"),
                        !Class ? TEXT("The existing input subobject is unavailable.")
                            : TEXT("The existing input subobject has the wrong class."),
                        Field + TEXT("/") + ::LexToString(Items.Num()),
                        Class ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Class->GetPathName())) : nullptr,
                        MakeShared<FJsonValueString>(BaseClass.GetPathName()), TEXT("native_result")));
                if (!TryBuildProjectionString(Class->GetPathName(), Resources, ClassPath, OutError)) return false;
				Items.Add(ClassPath.ToSharedRef());
			}
			return TryBuildProjectionArray(
				Items,
				Resources,
				OutValue,
				OutError);
		}

		bool TryBuildClassPathProjection(
            const TArray<UClass*>& Classes,
            const FString& Field,
            FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			TArray<FUeAgentCanonicalValueRef> Items;
			Items.Reserve(Classes.Num());
			for (const UClass* Class : Classes)
			{
				FUeAgentCanonicalValuePtr ClassPath;
                if (!Class)
                    return Fail(OutError, JsonFailure(TEXT("asset_data_enhanced_input_class_projection_invalid"),
                        TEXT("A resolved input class is unavailable during projection."), Field + TEXT("/") + ::LexToString(Items.Num()),
                        nullptr, MakeShared<FJsonValueString>(TEXT("resolved class")), TEXT("native_result")));
                if (!TryBuildProjectionString(Class->GetPathName(), Resources, ClassPath, OutError)) return false;
				Items.Add(ClassPath.ToSharedRef());
			}
			return TryBuildProjectionArray(
				Items,
				Resources,
				OutValue,
				OutError);
		}

		bool TryBuildInputActionProjectionMap(
			const UInputAction* Action,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutValues,
			FExecutionMutationError& OutError)
		{
            ON_SCOPE_EXIT
            {
                if (Action && OutError.IsValid() && OutError.TargetPath.IsEmpty())
                    OutError.TargetPath = Action->GetPathName();
            };
			OutValues.Reset();
            if (!Action)
                return Fail(OutError, JsonFailure(TEXT("asset_data_enhanced_input_action_projection_invalid"),
                    TEXT("InputAction field projection requires an InputAction object."), TEXT("target"),
                    nullptr, MakeShared<FJsonValueString>(UInputAction::StaticClass()->GetPathName()), TEXT("native_result")));
            if (!ValidateInputActionProperties(TEXT("asset_data_enhanced_input_action_projection_invalid"), OutError))
            {
                OutError.TargetPath = Action->GetPathName();
                return false;
            }
			FString ValueType;
			FUeAgentCanonicalValuePtr ValueTypeValue;
			FUeAgentCanonicalValuePtr ConsumeValue;
			FUeAgentCanonicalValuePtr TriggersValue;
			FUeAgentCanonicalValuePtr ModifiersValue;
            if (!TryFormatValueType(Action->ValueType, ValueType, OutError)
				|| !TryBuildProjectionString(
					ValueType,
					Resources,
					ValueTypeValue,
					OutError)
				|| !TryBuildProjectionBoolean(
					Action->bConsumeInput,
					Resources,
					ConsumeValue,
					OutError)
				|| !TryBuildClassCollectionProjection(
					Action->Triggers,
                    *UInputTrigger::StaticClass(), TEXT("triggers"),
                    Resources,
					TriggersValue,
					OutError)
				|| !TryBuildClassCollectionProjection(
					Action->Modifiers,
                    *UInputModifier::StaticClass(), TEXT("modifiers"),
                    Resources,
					ModifiersValue,
					OutError))
			{
				return false;
			}
			OutValues.Add(
				TEXT("value_type"),
				ValueTypeValue.ToSharedRef());
			OutValues.Add(
				TEXT("consume_input"),
				ConsumeValue.ToSharedRef());
			OutValues.Add(
				TEXT("triggers"),
				TriggersValue.ToSharedRef());
			OutValues.Add(
				TEXT("modifiers"),
				ModifiersValue.ToSharedRef());
			return true;
		}

		bool TryBuildMappingProjectionValue(
			const UInputAction* Action,
			const FKey& Key,
			const TArray<TObjectPtr<UInputTrigger>>& Triggers,
            const TArray<TObjectPtr<UInputModifier>>& Modifiers,
            const FString& Field,
            FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentCanonicalValuePtr ActionValue;
			FUeAgentCanonicalValuePtr KeyValue;
			FUeAgentCanonicalValuePtr TriggersValue;
			FUeAgentCanonicalValuePtr ModifiersValue;
            if (!Action || !Key.IsValid())
                return Fail(OutError, JsonFailure(TEXT("asset_data_enhanced_input_mapping_projection_invalid"),
                    !Action ? TEXT("The captured mapping has no InputAction.") : TEXT("The captured mapping has an invalid key."),
                    Field + (!Action ? TEXT("/action_ref") : TEXT("/key")),
                    !Action ? nullptr : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Key.GetFName().ToString())),
                    MakeShared<FJsonValueString>(!Action ? TEXT("InputAction asset") : TEXT("registered key")), TEXT("native_result")));
            if (!TryBuildProjectionString(Action->GetPathName(), Resources, ActionValue, OutError)
				|| !TryBuildProjectionString(
					Key.GetFName().ToString(),
					Resources,
					KeyValue,
					OutError)
				|| !TryBuildClassCollectionProjection(
					Triggers,
                    *UInputTrigger::StaticClass(), Field + TEXT("/triggers"),
                    Resources,
					TriggersValue,
					OutError)
				|| !TryBuildClassCollectionProjection(
					Modifiers,
                    *UInputModifier::StaticClass(), Field + TEXT("/modifiers"),
                    Resources,
					ModifiersValue,
					OutError))
			{
                return false;
			}
			return TryBuildProjectionObject(
				{
					{
						TEXT("action_ref"),
						ActionValue.ToSharedRef(),
					},
					{
						TEXT("key"),
						KeyValue.ToSharedRef(),
					},
					{
						TEXT("triggers"),
						TriggersValue.ToSharedRef(),
					},
					{
						TEXT("modifiers"),
						ModifiersValue.ToSharedRef(),
					},
				},
				Resources,
				OutValue,
				OutError);
		}

		bool TryBuildMappingContextProjectionMap(
			const UInputMappingContext* Context,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutValues,
			FExecutionMutationError& OutError)
		{
            ON_SCOPE_EXIT
            {
                if (Context && OutError.IsValid() && OutError.TargetPath.IsEmpty())
                    OutError.TargetPath = Context->GetPathName();
            };
			OutValues.Reset();
			if (!Context)
			{
                return Fail(OutError, JsonFailure(TEXT("asset_data_enhanced_input_mapping_context_projection_invalid"),
                    TEXT("Mapping field projection requires an InputMappingContext object."), TEXT("target"),
                    nullptr, MakeShared<FJsonValueString>(UInputMappingContext::StaticClass()->GetPathName()), TEXT("native_result")));
			}
			TSet<FString> SeenIdentities;
			TArray<FUeAgentCanonicalValueRef> Mappings;
			for (const FEnhancedActionKeyMapping& Mapping
				: Context->GetMappings())
			{
				if (!Mapping.Action || !Mapping.Key.IsValid())
				{
                    auto Error = JsonFailure(TEXT("asset_data_enhanced_input_mapping_identity_invalid"),
                        !Mapping.Action ? TEXT("The existing mapping has no input action.") : TEXT("The existing mapping key is invalid."),
                        TEXT("mappings/") + ::LexToString(Mappings.Num()) + (!Mapping.Action ? TEXT("/action_ref") : TEXT("/key")),
                        !Mapping.Action ? nullptr : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Mapping.Key.GetFName().ToString())),
                        MakeShared<FJsonValueString>(!Mapping.Action ? TEXT("InputAction asset") : TEXT("registered key")), TEXT("native_result"));
                    Error.TargetPath = Context->GetPathName();
                    return Fail(OutError, MoveTemp(Error));
				}
				const FString Identity =
					Mapping.Action->GetPathName()
					+ TEXT("\n")
					+ Mapping.Key.GetFName().ToString();
				FUeAgentCanonicalValuePtr Value;
				if (SeenIdentities.Contains(Identity))
				{
                    return FailDuplicateMapping(OutError, Mapping.Action->GetPathName(), Mapping.Key.GetFName().ToString(), Mappings.Num(), TEXT("native_result"));
				}
				SeenIdentities.Add(Identity);
                if (!TryBuildMappingProjectionValue(Mapping.Action.Get(), Mapping.Key, Mapping.Triggers,
                    Mapping.Modifiers, TEXT("mappings/") + ::LexToString(Mappings.Num()), Resources, Value, OutError)) return false;
				Mappings.Add(Value.ToSharedRef());
			}
			FUeAgentCanonicalValuePtr MappingsValue;
			if (!TryBuildProjectionArray(
					Mappings,
					Resources,
					MappingsValue,
					OutError))
			{
				return false;
			}
			OutValues.Add(
				TEXT("mappings"),
				MappingsValue.ToSharedRef());
			return true;
		}

		const TCHAR* FindEnhancedInputContextTag(
			const FString& NativeMechanicId,
			const FString& FieldId)
		{
			if (NativeMechanicId
				== TEXT(
					"uai.asset_data.mechanic:"
					"update_input_action:v1"))
			{
				if (FieldId == TEXT("value_type"))
				{
					return TEXT("enhanced_input_value_type");
				}
				if (FieldId == TEXT("consume_input"))
				{
					return TEXT("enhanced_input_boolean");
				}
				if (FieldId == TEXT("triggers"))
				{
					return TEXT(
						"enhanced_input_trigger_class_collection");
				}
				if (FieldId == TEXT("modifiers"))
				{
					return TEXT(
						"enhanced_input_modifier_class_collection");
				}
			}
			if (NativeMechanicId
					== TEXT(
						"uai.asset_data.mechanic:"
						"update_input_mapping_context:v1")
				&& FieldId == TEXT("mappings"))
			{
				return TEXT("enhanced_input_mapping_collection");
			}
			return nullptr;
		}

		bool TrySelectProjectionFields(
			const FString& NativeMechanicId,
			const TArray<FString>& SelectedFieldIds,
			const TMap<FString, FUeAgentCanonicalValueRef>& CurrentValues,
			const TMap<FString, FUeAgentCanonicalValueRef>& DefaultValues,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutCurrentFields,
			FUeAgentCanonicalValuePtr& OutDefaultFields,
			FUeAgentCanonicalValuePtr& OutProjectionContext,
			FExecutionMutationError& OutError)
		{
			if (SelectedFieldIds.IsEmpty())
			{
                return Fail(OutError, JsonFailure(TEXT("asset_data_enhanced_input_selected_fields_missing"),
                    TEXT("Enhanced Input projection requires at least one selected field."), TEXT("selected_fields"),
                    MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("one or more fields"))));
			}
			TSet<FString> Seen;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>>
				CurrentItems;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>>
				DefaultItems;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>>
				ContextItems;
			for (const FString& FieldId : SelectedFieldIds)
			{
				const FUeAgentCanonicalValueRef* Current =
					CurrentValues.Find(FieldId);
				const FUeAgentCanonicalValueRef* Default =
					DefaultValues.Find(FieldId);
				const TCHAR* ContextTag =
					FindEnhancedInputContextTag(
						NativeMechanicId,
						FieldId);
				FUeAgentCanonicalValuePtr ContextValue;
                if (Seen.Contains(FieldId) || !Current || !Default || !ContextTag)
                    return Fail(OutError, JsonFailure(TEXT("asset_data_enhanced_input_selected_field_invalid"),
                        Seen.Contains(FieldId) ? TEXT("The field is selected more than once.")
                            : !Current ? TEXT("The selected field has no captured current value.")
                            : !Default ? TEXT("The selected field has no captured default value.")
                            : TEXT("The field has no projection context for this asset type."),
                        FieldId, MakeShared<FJsonValueString>(FieldId),
                        MakeShared<FJsonValueString>(TEXT("unique supported field with current, default and context values"))));
                if (!TryBuildProjectionString(ContextTag, Resources, ContextValue, OutError)) return false;
				Seen.Add(FieldId);
				CurrentItems.Emplace(FieldId, *Current);
				DefaultItems.Emplace(FieldId, *Default);
				ContextItems.Emplace(
					FieldId,
					ContextValue.ToSharedRef());
			}
			return TryBuildProjectionObject(
					MoveTemp(CurrentItems),
					Resources,
					OutCurrentFields,
					OutError)
				&& TryBuildProjectionObject(
					MoveTemp(DefaultItems),
					Resources,
					OutDefaultFields,
					OutError)
				&& TryBuildProjectionObject(
					MoveTemp(ContextItems),
					Resources,
					OutProjectionContext,
					OutError);
		}

		bool TryReadTypedChangeMap(
			const FUeAgentCanonicalValue& CurrentFields,
			const FUeAgentCanonicalValue& DefaultFields,
			const FUeAgentCanonicalValue& ProjectionContext,
			const FUeAgentCanonicalValue& ProposedAfter,
			TMap<FString, const FUeAgentCanonicalValue*>& OutChanges,
			FExecutionMutationError& OutError)
		{
			OutChanges.Reset();
			const FUeAgentCanonicalValue* TypedChanges =
				FindObjectField(
					ProposedAfter,
					TEXT("typed_changes"));
			if (CurrentFields.GetKind()
					!= EUeAgentValueKind::Object
				|| DefaultFields.GetKind()
					!= EUeAgentValueKind::Object
				|| ProjectionContext.GetKind()
					!= EUeAgentValueKind::Object
				|| !TypedChanges
				|| TypedChanges->GetKind()
					!= EUeAgentValueKind::Array
				|| TypedChanges->GetArrayItems().Num()
					!= CurrentFields.GetObjectItems().Num())
			{
                if (CurrentFields.GetKind() != EUeAgentValueKind::Object)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_enhanced_input_expected_state_invalid"),
                        TEXT("Captured current input fields must be an object."), TEXT("field_state"), &CurrentFields, TEXT("object"), TEXT("native_result")));
                if (DefaultFields.GetKind() != EUeAgentValueKind::Object)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_enhanced_input_expected_state_invalid"),
                        TEXT("Captured default input fields must be an object."), TEXT("field_defaults"), &DefaultFields, TEXT("object"), TEXT("native_result")));
                if (ProjectionContext.GetKind() != EUeAgentValueKind::Object)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_enhanced_input_expected_state_invalid"),
                        TEXT("Input field projection context must be an object."), TEXT("field_projection_context"), &ProjectionContext, TEXT("object"), TEXT("native_result")));
                if (!TypedChanges || TypedChanges->GetKind() != EUeAgentValueKind::Array)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_enhanced_input_expected_state_invalid"),
                        TEXT("The proposed input state requires a typed_changes array."), TEXT("typed_changes"), TypedChanges, TEXT("array")));
                return Fail(OutError, JsonFailure(TEXT("asset_data_enhanced_input_expected_state_invalid"),
                    TEXT("The number of typed changes must match the number of selected current fields."), TEXT("typed_changes"),
                    MakeShared<FJsonValueNumber>(TypedChanges->GetArrayItems().Num()),
                    MakeShared<FJsonValueNumber>(CurrentFields.GetObjectItems().Num())));
			}
			for (const FUeAgentCanonicalValueRef& Change
				: TypedChanges->GetArrayItems())
			{
				const FUeAgentCanonicalValue* Field =
					FindObjectField(Change.Get(), TEXT("field_id"));
				FString FieldId;
				if (!Field
					|| !TryReadProjectionString(
						*Field,
						FieldId)
					|| OutChanges.Contains(FieldId))
				{
                    return Fail(OutError, ValueFailure(TEXT("asset_data_enhanced_input_expected_change_invalid"),
                        OutChanges.Contains(FieldId) ? TEXT("The same field_id occurs more than once in typed_changes.")
                            : TEXT("Each typed change requires a non-empty string field_id."),
                        TEXT("typed_changes/") + ::LexToString(OutChanges.Num()) + TEXT("/field_id"),
                        Field, TEXT("unique non-empty field_id")));
				}
				OutChanges.Add(FieldId, &Change.Get());
			}
			return true;
		}

		bool TryReadChangePresenceAndValue(
			const FUeAgentCanonicalValue& Change,
			FString& OutPresence,
			const FUeAgentCanonicalValue*& OutRequested)
		{
			OutPresence.Reset();
			OutRequested = nullptr;
			const FUeAgentCanonicalValue* Presence =
				FindObjectField(Change, TEXT("presence"));
			if (!Presence
				|| !TryReadProjectionString(
					*Presence,
					OutPresence))
			{
				return false;
			}
			const FUeAgentCanonicalValue* TypedValue =
				FindObjectField(Change, TEXT("value"));
			OutRequested = TypedValue
				? FindObjectField(
					*TypedValue,
					TEXT("canonical_value"))
				: nullptr;
			return true;
		}

		bool TryNormalizeClassCollection(
			const FUeAgentCanonicalValue& Requested,
			const UClass& BaseClass,
            const FString& ErrorPrefix,
            const FString& Field,
            FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			TArray<UClass*> Classes;
			return TryResolveClassArray(
					Requested,
					BaseClass,
                    ErrorPrefix, Field,
                    Classes,
					OutError)
				&& TryBuildClassPathProjection(
                    Classes, Field,
                    Resources,
					OutValue,
					OutError);
		}

		bool TryApplyClassCollectionPresence(
            const FString& Field,
            const FString& Presence,
			const FUeAgentCanonicalValue& Current,
			const FUeAgentCanonicalValue* Requested,
			const UClass& BaseClass,
			const FString& ErrorPrefix,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			if (Presence == TEXT("clear"))
			{
				return TryBuildProjectionArray(
					{},
					Resources,
					OutValue,
					OutError);
			}
            FUeAgentCanonicalValuePtr Normalized;
            if (!Requested)
                return Fail(OutError, ValueFailure(ErrorPrefix + TEXT("_value_missing"),
                    TEXT("Updating an input class collection requires a value."), Field, nullptr, TEXT("class reference array")));
            if (Presence != TEXT("set") && Presence != TEXT("replace"))
                return Fail(OutError, JsonFailure(ErrorPrefix + TEXT("_presence_invalid"),
                    TEXT("The input class collection uses an unsupported operation."), Field + TEXT("/presence"),
                    MakeShared<FJsonValueString>(Presence), MakeShared<FJsonValueString>(TEXT("set, replace or clear"))));
            if (!TryNormalizeClassCollection(*Requested, BaseClass, ErrorPrefix, Field, Resources, Normalized, OutError)) return false;
			if (Presence == TEXT("replace"))
			{
				OutValue = Normalized;
				return true;
			}
			if (Current.GetKind() != EUeAgentValueKind::Array)
			{
                return Fail(OutError, ValueFailure(ErrorPrefix + TEXT("_current_invalid"),
                    TEXT("The captured input class collection must be an array."), Field, &Current,
                    TEXT("class reference array"), TEXT("native_result")));
			}
			TArray<FUeAgentCanonicalValueRef> Items =
				Current.GetArrayItems();
			TSet<FString> Existing;
            for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
                const FUeAgentCanonicalValueRef& Item = Items[Index];
                FString Path;
				if (!TryReadProjectionString(Item.Get(), Path))
				{
                    return Fail(OutError, ValueFailure(ErrorPrefix + TEXT("_current_class_ref_invalid"),
                        TEXT("The captured input class reference must be a non-empty string."),
                        Field + TEXT("/") + ::LexToString(Index), &Item.Get(), TEXT("non-empty class reference"), TEXT("native_result")));
				}
				Existing.Add(Path);
			}
			for (const FUeAgentCanonicalValueRef& Item
				: Normalized->GetArrayItems())
			{
				if (!Existing.Contains(Item->GetText()))
				{
					Existing.Add(Item->GetText());
					Items.Add(Item);
				}
			}
			return TryBuildProjectionArray(
				Items,
				Resources,
				OutValue,
				OutError);
		}

        bool TryReadProjectedMappingIdentity(
            const FUeAgentCanonicalValue& Value,
            const FString& Field,
            FString& OutActionPath,
            FString& OutKey,
            FExecutionMutationError& OutError)
        {
            const FUeAgentCanonicalValue* Action = FindObjectField(Value, TEXT("action_ref"));
            const FUeAgentCanonicalValue* Key = FindObjectField(Value, TEXT("key"));
            if (!Action || !Key)
                return Fail(OutError, ValueFailure(TEXT("asset_data_enhanced_input_captured_mapping_identity_invalid"),
                    TEXT("The captured mapping is missing an identity field."),
                    Field + (!Action ? TEXT("/action_ref") : TEXT("/key")), nullptr, TEXT("non-empty string"), TEXT("native_result")));
            if (!TryReadProjectionString(*Action, OutActionPath))
                return Fail(OutError, ValueFailure(TEXT("asset_data_enhanced_input_captured_mapping_identity_invalid"),
                    TEXT("The captured mapping action_ref must be a non-empty string."), Field + TEXT("/action_ref"),
                    Action, TEXT("non-empty asset reference"), TEXT("native_result")));
            if (!TryReadProjectionString(*Key, OutKey))
                return Fail(OutError, ValueFailure(TEXT("asset_data_enhanced_input_captured_mapping_identity_invalid"),
                    TEXT("The captured mapping key must be a non-empty string."), Field + TEXT("/key"),
                    Key, TEXT("non-empty key name"), TEXT("native_result")));
            return true;
        }

		bool TryNormalizeRequestedMapping(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FUeAgentCanonicalValue& Requested,
            const bool bIdentityOnly,
            const FString& Field,
            FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FString& OutIdentity,
			FExecutionMutationError& OutError)
		{
			UInputAction* Action = nullptr;
			FKey Key;
			TArray<UClass*> TriggerClasses;
			TArray<UClass*> ModifierClasses;
			if (!TryReadMappingIdentity(
					Resolver,
					Requested,
                    bIdentityOnly, Field,
					Action,
					Key,
					TriggerClasses,
					ModifierClasses,
                    OutError))
			{
				return false;
			}
			const FString ActionPath = Action->GetPathName();
			const FString KeyName = Key.GetFName().ToString();
			OutIdentity = ActionPath + TEXT("\n") + KeyName;
			if (bIdentityOnly)
			{
				FUeAgentCanonicalValuePtr ActionValue;
				FUeAgentCanonicalValuePtr KeyValue;
				return TryBuildProjectionString(
						ActionPath,
						Resources,
						ActionValue,
						OutError)
					&& TryBuildProjectionString(
						KeyName,
						Resources,
						KeyValue,
						OutError)
					&& TryBuildProjectionObject(
						{
							{
								TEXT("action_ref"),
								ActionValue.ToSharedRef(),
							},
							{
								TEXT("key"),
								KeyValue.ToSharedRef(),
							},
						},
						Resources,
						OutValue,
						OutError);
			}
			FUeAgentCanonicalValuePtr Triggers;
			FUeAgentCanonicalValuePtr Modifiers;
			FUeAgentCanonicalValuePtr ActionValue;
			FUeAgentCanonicalValuePtr KeyValue;
			if (!TryBuildClassPathProjection(
                    TriggerClasses, Field + TEXT("/triggers"),
                    Resources,
					Triggers,
					OutError)
				|| !TryBuildClassPathProjection(
                    ModifierClasses, Field + TEXT("/modifiers"),
                    Resources,
					Modifiers,
					OutError)
				|| !TryBuildProjectionString(
					ActionPath,
					Resources,
					ActionValue,
					OutError)
				|| !TryBuildProjectionString(
					KeyName,
					Resources,
					KeyValue,
					OutError))
			{
				return false;
			}
			return TryBuildProjectionObject(
				{
					{
						TEXT("action_ref"),
						ActionValue.ToSharedRef(),
					},
					{
						TEXT("key"),
						KeyValue.ToSharedRef(),
					},
					{
						TEXT("triggers"),
						Triggers.ToSharedRef(),
					},
					{
						TEXT("modifiers"),
						Modifiers.ToSharedRef(),
					},
				},
				Resources,
				OutValue,
				OutError);
		}

		bool TryApplyMappingPresence(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
            const FString& Field,
            const FString& Presence,
			const FUeAgentCanonicalValue& Current,
			const FUeAgentCanonicalValue* Requested,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			if (Presence == TEXT("clear"))
			{
				return TryBuildProjectionArray(
					{},
					Resources,
					OutValue,
					OutError);
			}
			const bool bIdentityOnly = Presence == TEXT("remove");
			if (!Requested
				|| Requested->GetKind() != EUeAgentValueKind::Array
				|| Current.GetKind() != EUeAgentValueKind::Array
				|| (!bIdentityOnly
					&& Presence != TEXT("set")
					&& Presence != TEXT("replace")))
			{
                if (!Requested || Requested->GetKind() != EUeAgentValueKind::Array)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_enhanced_input_mapping_presence_invalid"),
                        TEXT("The mapping operation requires an array value."), Field, Requested, TEXT("mapping array")));
                if (Current.GetKind() != EUeAgentValueKind::Array)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_enhanced_input_mapping_presence_invalid"),
                        TEXT("The captured mappings must be an array."), Field, &Current, TEXT("mapping array"), TEXT("native_result")));
                return Fail(OutError, JsonFailure(TEXT("asset_data_enhanced_input_mapping_presence_invalid"),
                    TEXT("The mapping change uses an unsupported operation."), Field + TEXT("/presence"),
                    MakeShared<FJsonValueString>(Presence), MakeShared<FJsonValueString>(TEXT("set, replace, remove or clear"))));
			}
			TArray<FUeAgentCanonicalValueRef> Normalized;
			TArray<FString> RequestedIdentities;
			TSet<FString> SeenRequested;
			for (const FUeAgentCanonicalValueRef& Item
				: Requested->GetArrayItems())
			{
				FUeAgentCanonicalValuePtr Value;
				FString Identity;
				if (!TryNormalizeRequestedMapping(
						Resolver,
						Item.Get(),
                        bIdentityOnly, Field + TEXT("/") + ::LexToString(Normalized.Num()),
						Resources,
						Value,
						Identity,
						OutError))
				{
                    // The reader already received the complete relative mapping field path.
					return false;
				}
				if (SeenRequested.Contains(Identity))
				{
					return FailDuplicateMapping(OutError, FindObjectField(Item.Get(), TEXT("action_ref"))->GetText(),
						FindObjectField(Item.Get(), TEXT("key"))->GetText(), Normalized.Num());
				}
				SeenRequested.Add(Identity);
				RequestedIdentities.Add(Identity);
				Normalized.Add(Value.ToSharedRef());
			}
			if (Presence == TEXT("replace"))
			{
				return TryBuildProjectionArray(
					Normalized,
					Resources,
					OutValue,
					OutError);
			}

			TArray<FUeAgentCanonicalValueRef> Result;
			TMap<FString, int32> ExistingIndexes;
			for (const FUeAgentCanonicalValueRef& Item
				: Current.GetArrayItems())
			{
				FString ActionPath;
				FString Key;
				if (!TryReadProjectedMappingIdentity(
                        Item.Get(), Field + TEXT("/") + ::LexToString(Result.Num()),
                        ActionPath,
                        Key, OutError))
				{
					return false;
				}
				const FString Identity =
					ActionPath + TEXT("\n") + Key;
				if (ExistingIndexes.Contains(Identity))
				{
                    return FailDuplicateMapping(OutError, ActionPath, Key, Result.Num(), TEXT("native_result"));
				}
				ExistingIndexes.Add(Identity, Result.Num());
				Result.Add(Item);
			}
			if (Presence == TEXT("remove"))
			{
				Result.RemoveAll(
					[&SeenRequested](
						const FUeAgentCanonicalValueRef& Item)
					{
                            // Every current mapping identity was validated before this removal predicate.
                            return SeenRequested.Contains(FindObjectField(Item.Get(), TEXT("action_ref"))->GetText()
                                + TEXT("\n") + FindObjectField(Item.Get(), TEXT("key"))->GetText());
					});
			}
			else
			{
				for (int32 Index = 0;
					Index < Normalized.Num();
					++Index)
				{
					if (const int32* ExistingIndex =
							ExistingIndexes.Find(
								RequestedIdentities[Index]))
					{
						Result[*ExistingIndex] = Normalized[Index];
					}
					else
					{
						ExistingIndexes.Add(
							RequestedIdentities[Index],
							Result.Num());
						Result.Add(Normalized[Index]);
					}
				}
			}
			return TryBuildProjectionArray(
				Result,
				Resources,
				OutValue,
				OutError);
		}
	}

	struct FEnhancedInputActionPreparedData
	{
		EInputActionValueType OriginalValueType =
			EInputActionValueType::Boolean;
		bool bOriginalConsumeInput = true;
		TArray<TObjectPtr<UInputTrigger>> OriginalTriggers;
		TArray<TObjectPtr<UInputModifier>> OriginalModifiers;
		TArray<TStrongObjectPtr<UObject>> Pins;

		bool bChangeValueType = false;
		EInputActionValueType NewValueType =
			EInputActionValueType::Boolean;
		bool bChangeConsumeInput = false;
		bool bNewConsumeInput = true;
		bool bChangeTriggers = false;
		TArray<FInputSubobjectSpec> NewTriggers;
		bool bChangeModifiers = false;
		TArray<FInputSubobjectSpec> NewModifiers;
	};

	struct FEnhancedInputMappingContextPreparedData
	{
		TArray<FEnhancedActionKeyMapping> OriginalMappings;
		TArray<TStrongObjectPtr<UObject>> Pins;
		TArray<FInputMappingSpec> NewMappings;
	};

	bool TryPrepareEnhancedInputActionUpdate(
		UObject* Target,
		const FEnhancedInputChangeView& ValueType,
		const FEnhancedInputChangeView& ConsumeInput,
		const FEnhancedInputChangeView& Triggers,
		const FEnhancedInputChangeView& Modifiers,
		FEnhancedInputActionPreparedState& OutState,
		FExecutionMutationError& OutError)
	{
		OutState.Data.Reset();
		UInputAction* Action = Cast<UInputAction>(Target);
        if (!Action)
        {
            auto Error = JsonFailure(TEXT("asset_data_input_action_invalid"),
                TEXT("InputAction update requires an InputAction target."), TEXT("target"), ObjectClassValue(Target),
                MakeShared<FJsonValueString>(UInputAction::StaticClass()->GetPathName()), TEXT("native_result"));
            if (Target) Error.TargetPath = Target->GetPathName();
            return Fail(OutError, MoveTemp(Error));
        }
        if (!ValidateInputActionProperties(TEXT("asset_data_input_action_invalid"), OutError))
        {
            OutError.TargetPath = Action->GetPathName();
            return false;
        }
		if (!ValueType.bSpecified
			&& !ConsumeInput.bSpecified
			&& !Triggers.bSpecified
			&& !Modifiers.bSpecified)
		{
            return Fail(OutError, JsonFailure(TEXT("asset_data_input_action_changes_missing"),
                TEXT("InputAction update requires at least one specified field change."), TEXT("changes"),
                MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("value_type, consume_input, triggers or modifiers"))));
		}

		const TSharedRef<
			FEnhancedInputActionPreparedData,
			ESPMode::ThreadSafe> Data =
				MakeShared<
					FEnhancedInputActionPreparedData,
					ESPMode::ThreadSafe>();
		Data->OriginalValueType = Action->ValueType;
		Data->bOriginalConsumeInput = Action->bConsumeInput;
		Data->OriginalTriggers = Action->Triggers;
		Data->OriginalModifiers = Action->Modifiers;
		if (!CaptureExistingSubobjects(
				Action->Triggers,
				Data->Pins,
				Data->NewTriggers,
				TEXT(
					"asset_data_input_action_"
					"existing_trigger_invalid"),
                TEXT("triggers"), OutError)
			|| !CaptureExistingSubobjects(
				Action->Modifiers,
				Data->Pins,
				Data->NewModifiers,
				TEXT(
					"asset_data_input_action_"
					"existing_modifier_invalid"),
                TEXT("modifiers"), OutError))
		{
			return false;
		}

		if (ValueType.bSpecified)
		{
			Data->bChangeValueType = true;
            if (ValueType.Presence != TEXT("set"))
                return Fail(OutError, JsonFailure(TEXT("asset_data_input_action_value_type_invalid"),
                    TEXT("InputAction value_type only supports set."), TEXT("value_type/presence"),
                    MakeShared<FJsonValueString>(ValueType.Presence), MakeShared<FJsonValueString>(TEXT("set"))));
            if (!ValueType.CanonicalValue)
                return Fail(OutError, ValueFailure(TEXT("asset_data_input_action_value_type_invalid"),
                    TEXT("Setting InputAction value_type requires a value."), TEXT("value_type"), nullptr,
                    TEXT("boolean, axis1d, axis2d or axis3d")));
            if (!TryParseValueType(*ValueType.CanonicalValue, Data->NewValueType,
                TEXT("asset_data_input_action_value_type_invalid"), OutError)) return false;
		}
		if (ConsumeInput.bSpecified)
		{
			Data->bChangeConsumeInput = true;
			if (ConsumeInput.Presence != TEXT("set")
				|| !ConsumeInput.CanonicalValue
				|| ConsumeInput.CanonicalValue->GetKind()
					!= EUeAgentValueKind::Boolean)
			{
                if (ConsumeInput.Presence != TEXT("set"))
                    return Fail(OutError, JsonFailure(TEXT("asset_data_input_action_consume_input_invalid"),
                        TEXT("InputAction consume_input only supports set."), TEXT("consume_input/presence"),
                        MakeShared<FJsonValueString>(ConsumeInput.Presence), MakeShared<FJsonValueString>(TEXT("set"))));
                return Fail(OutError, ValueFailure(TEXT("asset_data_input_action_consume_input_invalid"),
                    TEXT("InputAction consume_input requires a boolean value."), TEXT("consume_input"),
                    ConsumeInput.CanonicalValue, TEXT("boolean")));
			}
			Data->bNewConsumeInput =
				ConsumeInput.CanonicalValue->GetBoolean();
		}
		if (Triggers.bSpecified)
		{
			Data->bChangeTriggers = true;
			if (Triggers.Presence == TEXT("clear"))
			{
				Data->NewTriggers.Reset();
			}
			else
			{
				TArray<UClass*> Classes;
				if (!Triggers.CanonicalValue
					|| !TryResolveClassArray(
						*Triggers.CanonicalValue,
						*UInputTrigger::StaticClass(),
						TEXT(
							"asset_data_input_action_trigger"),
                        TEXT("triggers"), Classes,
						OutError))
				{
                    if (!Triggers.CanonicalValue)
                        return Fail(OutError, ValueFailure(TEXT("asset_data_input_action_trigger_value_missing"),
                            TEXT("Updating triggers requires a class reference array."), TEXT("triggers"), nullptr, TEXT("array")));
                    return false;
				}
				if (Triggers.Presence == TEXT("replace"))
				{
					BuildReplacementSpecs(
						Classes,
						Data->NewTriggers);
				}
				else if (Triggers.Presence == TEXT("set"))
				{
					AppendMissingClassSpecs(
						Classes,
						Data->NewTriggers);
				}
				else
				{
                    return Fail(OutError, JsonFailure(TEXT("asset_data_input_action_trigger_presence_invalid"),
                        TEXT("The triggers change uses an unsupported operation."), TEXT("triggers/presence"),
                        MakeShared<FJsonValueString>(Triggers.Presence), MakeShared<FJsonValueString>(TEXT("set, replace or clear"))));
				}
			}
		}
		if (Modifiers.bSpecified)
		{
			Data->bChangeModifiers = true;
			if (Modifiers.Presence == TEXT("clear"))
			{
				Data->NewModifiers.Reset();
			}
			else
			{
				TArray<UClass*> Classes;
				if (!Modifiers.CanonicalValue
					|| !TryResolveClassArray(
						*Modifiers.CanonicalValue,
						*UInputModifier::StaticClass(),
						TEXT(
							"asset_data_input_action_modifier"),
                        TEXT("modifiers"), Classes,
						OutError))
				{
                    if (!Modifiers.CanonicalValue)
                        return Fail(OutError, ValueFailure(TEXT("asset_data_input_action_modifier_value_missing"),
                            TEXT("Updating modifiers requires a class reference array."), TEXT("modifiers"), nullptr, TEXT("array")));
                    return false;
				}
				if (Modifiers.Presence == TEXT("replace"))
				{
					BuildReplacementSpecs(
						Classes,
						Data->NewModifiers);
				}
				else if (Modifiers.Presence == TEXT("set"))
				{
					AppendMissingClassSpecs(
						Classes,
						Data->NewModifiers);
				}
				else
				{
                    return Fail(OutError, JsonFailure(TEXT("asset_data_input_action_modifier_presence_invalid"),
                        TEXT("The modifiers change uses an unsupported operation."), TEXT("modifiers/presence"),
                        MakeShared<FJsonValueString>(Modifiers.Presence), MakeShared<FJsonValueString>(TEXT("set, replace or clear"))));
				}
			}
		}
		OutState.Data = Data;
		return true;
	}

	bool TryApplyEnhancedInputActionUpdate(
		UObject* Target,
		const FEnhancedInputActionPreparedState& State,
		FExecutionMutationError& OutError)
	{
		UInputAction* Action = Cast<UInputAction>(Target);
		if (!Action || !State.Data.IsValid())
		{
            auto Error = !Action
                ? JsonFailure(TEXT("asset_data_input_action_state_invalid"), TEXT("The InputAction target is unavailable or has the wrong class."),
                    TEXT("target"), ObjectClassValue(Target), MakeShared<FJsonValueString>(UInputAction::StaticClass()->GetPathName()), TEXT("native_result"))
                : JsonFailure(TEXT("asset_data_input_action_state_invalid"), TEXT("InputAction update has no prepared state."),
                    TEXT("prepared_state_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("native_result"));
            if (Target) Error.TargetPath = Target->GetPathName();
            return Fail(OutError, MoveTemp(Error));
		}
		const FEnhancedInputActionPreparedData& Data = *State.Data;
		TArray<TObjectPtr<UInputTrigger>> NewTriggers;
		TArray<TObjectPtr<UInputModifier>> NewModifiers;
		if (Data.bChangeTriggers
			&& !TryMaterializeSubobjects(
				Action,
				Data.NewTriggers,
				*UInputTrigger::StaticClass(),
				TEXT(
					"asset_data_input_action_"
					"trigger_create_failed"),
                TEXT("triggers"), NewTriggers,
				OutError))
		{
			return false;
		}
		if (Data.bChangeModifiers
			&& !TryMaterializeSubobjects(
				Action,
				Data.NewModifiers,
				*UInputModifier::StaticClass(),
				TEXT(
					"asset_data_input_action_"
					"modifier_create_failed"),
                TEXT("modifiers"), NewModifiers,
				OutError))
		{
			return false;
		}

		Action->Modify();
		if (Data.bChangeValueType)
		{
			Action->ValueType = Data.NewValueType;
		}
		if (Data.bChangeConsumeInput)
		{
			Action->bConsumeInput = Data.bNewConsumeInput;
		}
		if (Data.bChangeTriggers)
		{
			Action->Triggers = MoveTemp(NewTriggers);
		}
		if (Data.bChangeModifiers)
		{
			Action->Modifiers = MoveTemp(NewModifiers);
		}
		if (Data.bChangeValueType)
		{
			NotifyInputActionProperty(
				*Action,
				GET_MEMBER_NAME_CHECKED(
					UInputAction,
					ValueType));
		}
		if (Data.bChangeConsumeInput)
		{
			NotifyInputActionProperty(
				*Action,
				GET_MEMBER_NAME_CHECKED(
					UInputAction,
					bConsumeInput));
		}
		if (Data.bChangeTriggers)
		{
			NotifyInputActionProperty(
				*Action,
				GET_MEMBER_NAME_CHECKED(
					UInputAction,
					Triggers));
		}
		if (Data.bChangeModifiers)
		{
			NotifyInputActionProperty(
				*Action,
				GET_MEMBER_NAME_CHECKED(
					UInputAction,
					Modifiers));
		}
		return true;
	}

	bool TryRollbackEnhancedInputActionUpdate(
		UObject* Target,
		const FEnhancedInputActionPreparedState& State,
		FExecutionMutationError& OutError)
	{
		UInputAction* Action = Cast<UInputAction>(Target);
		if (!Action || !State.Data.IsValid())
		{
			OutError = !Action
                ? JsonFailure(TEXT("asset_data_input_action_rollback_invalid"),
                    TEXT("Input action rollback requires a live InputAction target."), TEXT("target"), ObjectClassValue(Target),
                    MakeShared<FJsonValueString>(UInputAction::StaticClass()->GetPathName()), TEXT("native_result"))
                : JsonFailure(TEXT("asset_data_input_action_rollback_invalid"),
                    TEXT("Input action rollback has no captured state."), TEXT("prepared_state"), MakeShared<FJsonValueBoolean>(false),
                    MakeShared<FJsonValueBoolean>(true), TEXT("native_result"));
            if (Target) OutError.TargetPath = Target->GetPathName();
            return false;
		}
		const FEnhancedInputActionPreparedData& Data = *State.Data;
		Action->Modify();
		Action->ValueType = Data.OriginalValueType;
		Action->bConsumeInput = Data.bOriginalConsumeInput;
		Action->Triggers = Data.OriginalTriggers;
		Action->Modifiers = Data.OriginalModifiers;
		if (Data.bChangeValueType)
		{
			NotifyInputActionProperty(
				*Action,
				GET_MEMBER_NAME_CHECKED(
					UInputAction,
					ValueType));
		}
		if (Data.bChangeConsumeInput)
		{
			NotifyInputActionProperty(
				*Action,
				GET_MEMBER_NAME_CHECKED(
					UInputAction,
					bConsumeInput));
		}
		if (Data.bChangeTriggers)
		{
			NotifyInputActionProperty(
				*Action,
				GET_MEMBER_NAME_CHECKED(
					UInputAction,
					Triggers));
		}
		if (Data.bChangeModifiers)
		{
			NotifyInputActionProperty(
				*Action,
				GET_MEMBER_NAME_CHECKED(
					UInputAction,
					Modifiers));
		}
		return true;
	}

	bool TryPrepareEnhancedInputMappingContextUpdate(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		UObject* Target,
		const FEnhancedInputChangeView& Mappings,
		FEnhancedInputMappingContextPreparedState& OutState,
		FExecutionMutationError& OutError)
	{
		OutState.Data.Reset();
		UInputMappingContext* Context =
			Cast<UInputMappingContext>(Target);
		if (!Context || !Mappings.bSpecified)
		{
            auto Error = !Context
                ? JsonFailure(TEXT("asset_data_input_mapping_context_invalid"),
                    TEXT("Mapping update requires an InputMappingContext target."), TEXT("target"), ObjectClassValue(Target),
                    MakeShared<FJsonValueString>(UInputMappingContext::StaticClass()->GetPathName()), TEXT("native_result"))
                : JsonFailure(TEXT("asset_data_input_mapping_context_invalid"),
                    TEXT("Mapping update requires a specified mappings change."), TEXT("mappings_specified"),
                    MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
            if (Target) Error.TargetPath = Target->GetPathName();
            return Fail(OutError, MoveTemp(Error));
		}
		const TSharedRef<
			FEnhancedInputMappingContextPreparedData,
			ESPMode::ThreadSafe> Data =
				MakeShared<
					FEnhancedInputMappingContextPreparedData,
					ESPMode::ThreadSafe>();
		Data->OriginalMappings = Context->GetMappings();
		for (int32 Index = 0;
			Index < Data->OriginalMappings.Num();
			++Index)
		{
			const FEnhancedActionKeyMapping& Existing =
				Data->OriginalMappings[Index];
			for (int32 Prior = 0; Prior < Index; ++Prior)
			{
				if (SameMappingIdentity(
					Data->OriginalMappings[Prior],
					Existing.Action.Get(),
					Existing.Key))
				{
                    auto Actual = MakeShared<FJsonObject>();
                    Actual->SetField(TEXT("action_ref"), Existing.Action
                        ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Existing.Action->GetPathName()))
                        : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()));
                    Actual->SetStringField(TEXT("key"), Existing.Key.GetFName().ToString());
                    auto Error = JsonFailure(TEXT("asset_data_input_mapping_existing_identity_duplicate"),
                        FString::Printf(TEXT("The existing mapping repeats the action/key pair from index %d."), Prior),
                        TEXT("mappings/") + ::LexToString(Index), MakeShared<FJsonValueObject>(Actual),
                        MakeShared<FJsonValueString>(TEXT("unique action_ref and key pair")), TEXT("native_result"));
                    Error.TargetPath = Context->GetPathName();
                    Error.OperationIndex = Index;
                    return Fail(OutError, MoveTemp(Error));
				}
			}
			FInputMappingSpec Spec;
			if (!TryBuildMappingSpecFromExisting(
                    Existing, TEXT("mappings/") + ::LexToString(Index),
					Data->Pins,
					Spec,
					OutError))
			{
				return false;
			}
			Data->NewMappings.Add(MoveTemp(Spec));
		}

		if (Mappings.Presence == TEXT("clear"))
		{
			Data->NewMappings.Reset();
			OutState.Data = Data;
			return true;
		}
		if (!Mappings.CanonicalValue
			|| Mappings.CanonicalValue->GetKind()
				!= EUeAgentValueKind::Array)
		{
            return Fail(OutError, ValueFailure(TEXT("asset_data_input_mapping_array_invalid"),
                TEXT("The requested mappings value must be an array."), TEXT("mappings"),
                Mappings.CanonicalValue, TEXT("mapping array")));
		}
		const bool bIdentityOnly =
			Mappings.Presence == TEXT("remove");
		if (!bIdentityOnly
			&& Mappings.Presence != TEXT("set")
			&& Mappings.Presence != TEXT("replace"))
		{
            return Fail(OutError, JsonFailure(TEXT("asset_data_input_mapping_presence_invalid"),
                TEXT("The mappings change uses an unsupported operation."), TEXT("mappings/presence"),
                MakeShared<FJsonValueString>(Mappings.Presence), MakeShared<FJsonValueString>(TEXT("set, replace, remove or clear"))));
		}

		TArray<FInputMappingSpec> Requested;
		for (const FUeAgentCanonicalValueRef& Value
			: Mappings.CanonicalValue->GetArrayItems())
		{
			UInputAction* Action = nullptr;
			FKey Key;
			TArray<UClass*> TriggerClasses;
			TArray<UClass*> ModifierClasses;
			if (!TryReadMappingIdentity(
					Resolver,
					Value.Get(),
                    bIdentityOnly, TEXT("mappings/") + ::LexToString(Requested.Num()),
					Action,
					Key,
					TriggerClasses,
					ModifierClasses,
					OutError))
			{
				return false;
			}
			if (Requested.ContainsByPredicate(
				[Action, &Key](
					const FInputMappingSpec& Spec)
				{
					return SameMappingIdentity(
						Spec,
						Action,
						Key);
				}))
			{
                return FailDuplicateMapping(OutError, Action->GetPathName(), Key.GetFName().ToString(),
                    Requested.Num(), TEXT("authored_value"), TEXT("asset_data_input_mapping_identity_duplicate"));
			}
			PinUnique(Data->Pins, Action);
			FInputMappingSpec Spec;
			Spec.Base =
				FEnhancedActionKeyMapping(Action, Key);
			BuildReplacementSpecs(
				TriggerClasses,
				Spec.Triggers);
			BuildReplacementSpecs(
				ModifierClasses,
				Spec.Modifiers);
			Requested.Add(MoveTemp(Spec));
		}

		if (Mappings.Presence == TEXT("replace"))
		{
			Data->NewMappings = MoveTemp(Requested);
		}
		else if (Mappings.Presence == TEXT("remove"))
		{
			Data->NewMappings.RemoveAll(
				[&Requested](
					const FInputMappingSpec& Existing)
				{
					return Requested.ContainsByPredicate(
						[&Existing](
							const FInputMappingSpec& Removed)
						{
							return SameMappingIdentity(
								Existing,
								Removed.Base.Action.Get(),
								Removed.Base.Key);
						});
				});
		}
		else
		{
			for (FInputMappingSpec& RequestedSpec : Requested)
			{
				const int32 ExistingIndex =
					Data->NewMappings.IndexOfByPredicate(
						[&RequestedSpec](
							const FInputMappingSpec& Existing)
						{
							return SameMappingIdentity(
								Existing,
								RequestedSpec.Base.Action.Get(),
								RequestedSpec.Base.Key);
						});
				if (ExistingIndex == INDEX_NONE)
				{
					Data->NewMappings.Add(
						MoveTemp(RequestedSpec));
				}
				else
				{
					Data->NewMappings[ExistingIndex].Triggers =
						MoveTemp(RequestedSpec.Triggers);
					Data->NewMappings[ExistingIndex].Modifiers =
						MoveTemp(RequestedSpec.Modifiers);
				}
			}
		}
		OutState.Data = Data;
		return true;
	}

	bool TryApplyEnhancedInputMappingContextUpdate(
		UObject* Target,
		const FEnhancedInputMappingContextPreparedState& State,
		FExecutionMutationError& OutError)
	{
		UInputMappingContext* Context =
			Cast<UInputMappingContext>(Target);
		if (!Context || !State.Data.IsValid())
		{
            auto Error = !Context
                ? JsonFailure(TEXT("asset_data_input_mapping_context_state_invalid"),
                    TEXT("The InputMappingContext target is unavailable or has the wrong class."), TEXT("target"), ObjectClassValue(Target),
                    MakeShared<FJsonValueString>(UInputMappingContext::StaticClass()->GetPathName()), TEXT("native_result"))
                : JsonFailure(TEXT("asset_data_input_mapping_context_state_invalid"),
                    TEXT("Mapping context update has no prepared state."), TEXT("prepared_state_available"),
                    MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("native_result"));
            if (Target) Error.TargetPath = Target->GetPathName();
            return Fail(OutError, MoveTemp(Error));
		}
		const FEnhancedInputMappingContextPreparedData& Data =
			*State.Data;
		TArray<FEnhancedActionKeyMapping> Materialized;
		Materialized.Reserve(Data.NewMappings.Num());
		for (const FInputMappingSpec& Spec : Data.NewMappings)
		{
			TArray<TObjectPtr<UInputTrigger>> Triggers;
			TArray<TObjectPtr<UInputModifier>> Modifiers;
			if (!TryMaterializeSubobjects(
					Context,
					Spec.Triggers,
					*UInputTrigger::StaticClass(),
					TEXT(
						"asset_data_input_mapping_"
						"trigger_create_failed"),
                    TEXT("mappings/") + ::LexToString(Materialized.Num()) + TEXT("/triggers"), Triggers,
					OutError)
				|| !TryMaterializeSubobjects(
					Context,
					Spec.Modifiers,
					*UInputModifier::StaticClass(),
					TEXT(
						"asset_data_input_mapping_"
						"modifier_create_failed"),
                    TEXT("mappings/") + ::LexToString(Materialized.Num()) + TEXT("/modifiers"), Modifiers,
					OutError))
			{
				return false;
			}
			FEnhancedActionKeyMapping Mapping = Spec.Base;
			Mapping.Triggers = MoveTemp(Triggers);
			Mapping.Modifiers = MoveTemp(Modifiers);
			Materialized.Add(MoveTemp(Mapping));
		}

		Context->Modify();
		Context->UnmapAll();
		for (const FEnhancedActionKeyMapping& Desired
			: Materialized)
		{
			FEnhancedActionKeyMapping& Mapping =
				Context->MapKey(
					Desired.Action.Get(),
					Desired.Key);
			Mapping = Desired;
		}
		Context->PostEditChange();
		return true;
	}

	bool TryRollbackEnhancedInputMappingContextUpdate(
		UObject* Target,
		const FEnhancedInputMappingContextPreparedState& State,
		FExecutionMutationError& OutError)
	{
		UInputMappingContext* Context =
			Cast<UInputMappingContext>(Target);
		if (!Context || !State.Data.IsValid())
		{
			OutError = !Context
                ? JsonFailure(TEXT("asset_data_input_mapping_context_rollback_invalid"),
                    TEXT("Mapping rollback requires a live InputMappingContext target."), TEXT("target"), ObjectClassValue(Target),
                    MakeShared<FJsonValueString>(UInputMappingContext::StaticClass()->GetPathName()), TEXT("native_result"))
                : JsonFailure(TEXT("asset_data_input_mapping_context_rollback_invalid"),
                    TEXT("Mapping rollback has no captured state."), TEXT("prepared_state"), MakeShared<FJsonValueBoolean>(false),
                    MakeShared<FJsonValueBoolean>(true), TEXT("native_result"));
            if (Target) OutError.TargetPath = Target->GetPathName();
            return false;
		}
		const FEnhancedInputMappingContextPreparedData& Data =
			*State.Data;
		Context->Modify();
		Context->UnmapAll();
		for (int32 Index = 0; Index < Data.OriginalMappings.Num(); ++Index)
		{
            const FEnhancedActionKeyMapping& Original = Data.OriginalMappings[Index];
			if (!Original.Action || !Original.Key.IsValid())
			{
                const FString Field = TEXT("mappings/") + FString::FromInt(Index)
                    + (!Original.Action ? TEXT("/action_ref") : TEXT("/key"));
                OutError = JsonFailure(TEXT("asset_data_input_mapping_context_rollback_mapping_invalid"),
                    !Original.Action ? TEXT("The captured mapping action is no longer available.")
                        : TEXT("The captured mapping key is invalid."),
                    Field, !Original.Action ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>())
                        : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Original.Key.GetFName().ToString())),
                    MakeShared<FJsonValueString>(!Original.Action ? TEXT("live InputAction") : TEXT("valid input key")),
                    TEXT("native_result"));
                OutError.OperationIndex = Index;
                OutError.TargetPath = Context->GetPathName();
                OutError.MutationMayHaveStarted = true;
                return false;
			}
			FEnhancedActionKeyMapping& Mapping =
				Context->MapKey(
					Original.Action.Get(),
					Original.Key);
			Mapping = Original;
		}
		Context->PostEditChange();
		return true;
	}

	bool CanProjectEnhancedInputFieldState(
		const FString& NativeMechanicId)
	{
		return NativeMechanicId
				== TEXT(
					"uai.asset_data.mechanic:"
					"update_input_action:v1")
			|| NativeMechanicId
				== TEXT(
					"uai.asset_data.mechanic:"
					"update_input_mapping_context:v1");
	}

	bool TryProjectEnhancedInputCurrentFieldState(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		const FString& NativeMechanicId,
		UObject* Target,
		const TArray<FString>& SelectedFieldIds,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutCurrentFields,
		FUeAgentCanonicalValuePtr& OutDefaultFields,
		FUeAgentCanonicalValuePtr& OutProjectionContext,
		FExecutionMutationError& OutError)
	{
		(void)Resolver;
		OutCurrentFields.Reset();
		OutDefaultFields.Reset();
		OutProjectionContext.Reset();
		if (!CanProjectEnhancedInputFieldState(NativeMechanicId)
			|| !Target)
		{
            auto Error = JsonFailure(TEXT("asset_data_enhanced_input_field_projection_unsupported"),
                !CanProjectEnhancedInputFieldState(NativeMechanicId)
                    ? TEXT("No Enhanced Input field projector is registered for this requested mechanic.")
                    : TEXT("Enhanced Input field projection requires an available target object."),
                !CanProjectEnhancedInputFieldState(NativeMechanicId) ? TEXT("input_projector_available") : TEXT("target_available"),
                MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("native_result"));
            Error.InternalContext = NativeMechanicId;
            if (Target) Error.TargetPath = Target->GetPathName();
            return Fail(OutError, MoveTemp(Error));
		}
		TMap<FString, FUeAgentCanonicalValueRef> CurrentValues;
		TMap<FString, FUeAgentCanonicalValueRef> DefaultValues;
		if (NativeMechanicId
			== TEXT(
				"uai.asset_data.mechanic:"
				"update_input_action:v1"))
		{
            const UInputAction* Action = Cast<UInputAction>(Target);
            if (!Action)
            {
                auto Error = JsonFailure(TEXT("asset_data_enhanced_input_action_projection_invalid"),
                    TEXT("The InputAction projector received an object of another class."), TEXT("target"),
                    ObjectClassValue(Target), MakeShared<FJsonValueString>(UInputAction::StaticClass()->GetPathName()), TEXT("native_result"));
                Error.TargetPath = Target->GetPathName();
                return Fail(OutError, MoveTemp(Error));
            }
			const UInputAction* Default =
				GetDefault<UInputAction>();
			if (!TryBuildInputActionProjectionMap(
					Action,
					Resources,
					CurrentValues,
					OutError)
				|| !TryBuildInputActionProjectionMap(
					Default,
					Resources,
					DefaultValues,
					OutError))
			{
				return false;
			}
		}
		else
		{
            const UInputMappingContext* Context = Cast<UInputMappingContext>(Target);
            if (!Context)
            {
                auto Error = JsonFailure(TEXT("asset_data_enhanced_input_mapping_context_projection_invalid"),
                    TEXT("The InputMappingContext projector received an object of another class."), TEXT("target"),
                    ObjectClassValue(Target), MakeShared<FJsonValueString>(UInputMappingContext::StaticClass()->GetPathName()), TEXT("native_result"));
                Error.TargetPath = Target->GetPathName();
                return Fail(OutError, MoveTemp(Error));
            }
			const UInputMappingContext* Default =
				GetDefault<UInputMappingContext>();
			if (!TryBuildMappingContextProjectionMap(
					Context,
					Resources,
					CurrentValues,
					OutError)
				|| !TryBuildMappingContextProjectionMap(
					Default,
					Resources,
					DefaultValues,
					OutError))
			{
				return false;
			}
		}
		return TrySelectProjectionFields(
			NativeMechanicId,
			SelectedFieldIds,
			CurrentValues,
			DefaultValues,
			Resources,
			OutCurrentFields,
			OutDefaultFields,
			OutProjectionContext,
			OutError);
	}

	bool TryProjectEnhancedInputExpectedFieldState(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		const FString& NativeMechanicId,
		const FUeAgentCanonicalValue& CurrentFields,
		const FUeAgentCanonicalValue& DefaultFields,
		const FUeAgentCanonicalValue& ProjectionContext,
		const FUeAgentCanonicalValue& ProposedAfter,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutExpectedFields,
		FExecutionMutationError& OutError)
	{
		OutExpectedFields.Reset();
		if (!CanProjectEnhancedInputFieldState(NativeMechanicId))
		{
            auto Error = JsonFailure(TEXT("asset_data_enhanced_input_field_projection_unsupported"),
                TEXT("No Enhanced Input expected-state projector is registered for this requested mechanic."),
                TEXT("input_projector_available"), MakeShared<FJsonValueBoolean>(false),
                MakeShared<FJsonValueBoolean>(true), TEXT("native_result"));
            Error.InternalContext = NativeMechanicId;
            return Fail(OutError, MoveTemp(Error));
		}
		TMap<FString, const FUeAgentCanonicalValue*> Changes;
		if (!TryReadTypedChangeMap(
				CurrentFields,
				DefaultFields,
				ProjectionContext,
				ProposedAfter,
				Changes,
				OutError))
		{
			return false;
		}
		TArray<TPair<FString, FUeAgentCanonicalValueRef>>
			ExpectedItems;
		for (const TPair<FString, FUeAgentCanonicalValueRef>& Current
			: CurrentFields.GetObjectItems())
		{
			const FUeAgentCanonicalValue* const* Change =
				Changes.Find(Current.Key);
			const FUeAgentCanonicalValue* Default =
				FindObjectField(
					DefaultFields,
					Current.Key);
			const FUeAgentCanonicalValue* Context =
				FindObjectField(
					ProjectionContext,
					Current.Key);
			const TCHAR* ExpectedContext =
				FindEnhancedInputContextTag(
					NativeMechanicId,
					Current.Key);
			FString ContextText;
			if (!Change
				|| !Default
				|| !Context
				|| !ExpectedContext
				|| !TryReadProjectionString(
					*Context,
					ContextText)
				|| ContextText != ExpectedContext)
			{
                if (!Change)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_enhanced_input_expected_context_invalid"),
                        TEXT("The selected input field has no matching typed change."), Current.Key, nullptr, TEXT("typed change for this field")));
                if (!Default)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_enhanced_input_expected_context_invalid"),
                        TEXT("The selected input field has no captured default value."), Current.Key, nullptr, TEXT("captured default value"), TEXT("native_result")));
                if (!Context)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_enhanced_input_expected_context_invalid"),
                        TEXT("The selected input field has no projection context."), Current.Key, nullptr, TEXT("field projection context"), TEXT("native_result")));
                if (!ExpectedContext)
                    return Fail(OutError, JsonFailure(TEXT("asset_data_enhanced_input_expected_context_invalid"),
                        TEXT("This input field is not supported by the selected projector."), Current.Key,
                        MakeShared<FJsonValueString>(Current.Key), MakeShared<FJsonValueString>(TEXT("supported Enhanced Input field"))));
                return Fail(OutError, ValueFailure(TEXT("asset_data_enhanced_input_expected_context_invalid"),
                    TEXT("The captured input field projection context has the wrong type or tag."),
                    Current.Key, Context, ExpectedContext, TEXT("native_result")));
			}
			FString Presence;
			const FUeAgentCanonicalValue* Requested = nullptr;
			if (!TryReadChangePresenceAndValue(
					**Change,
					Presence,
					Requested))
			{
                return Fail(OutError, ValueFailure(TEXT("asset_data_enhanced_input_expected_presence_invalid"),
                    TEXT("The input field change must declare a non-empty string presence operation."),
                    Current.Key + TEXT("/presence"), FindObjectField(**Change, TEXT("presence")), TEXT("non-empty operation string")));
			}

			FUeAgentCanonicalValuePtr Expected;
			if (NativeMechanicId
					== TEXT(
						"uai.asset_data.mechanic:"
						"update_input_mapping_context:v1")
				&& Current.Key == TEXT("mappings"))
			{
				if (!TryApplyMappingPresence(
                        Resolver, Current.Key,
                        Presence,
						Current.Value.Get(),
						Requested,
						Resources,
						Expected,
						OutError))
				{
					return false;
				}
			}
			else if (Current.Key == TEXT("value_type"))
			{
				EInputActionValueType ValueType;
				FString Normalized;
                if (Presence != TEXT("set"))
                    return Fail(OutError, JsonFailure(TEXT("asset_data_enhanced_input_value_type_expected_invalid"),
                        TEXT("InputAction value_type only supports set."), Current.Key + TEXT("/presence"),
                        MakeShared<FJsonValueString>(Presence), MakeShared<FJsonValueString>(TEXT("set"))));
                if (!Requested)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_enhanced_input_value_type_expected_invalid"),
                        TEXT("Setting InputAction value_type requires a value."), Current.Key, nullptr,
                        TEXT("boolean, axis1d, axis2d or axis3d")));
                if (!TryParseValueType(*Requested, ValueType, TEXT("asset_data_enhanced_input_value_type_expected_invalid"), OutError)
                    || !TryFormatValueType(ValueType, Normalized, OutError)
                    || !TryBuildProjectionString(Normalized, Resources, Expected, OutError)) return false;
			}
			else if (Current.Key == TEXT("consume_input"))
			{
                if (Presence != TEXT("set"))
                    return Fail(OutError, JsonFailure(TEXT("asset_data_enhanced_input_consume_input_expected_invalid"),
                        TEXT("InputAction consume_input only supports set."), Current.Key + TEXT("/presence"),
                        MakeShared<FJsonValueString>(Presence), MakeShared<FJsonValueString>(TEXT("set"))));
                if (!Requested || Requested->GetKind() != EUeAgentValueKind::Boolean)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_enhanced_input_consume_input_expected_invalid"),
                        TEXT("InputAction consume_input requires a boolean value."), Current.Key, Requested, TEXT("boolean")));
                if (!TryBuildProjectionBoolean(Requested->GetBoolean(), Resources, Expected, OutError)) return false;
			}
			else if (Current.Key == TEXT("triggers"))
			{
				if (!TryApplyClassCollectionPresence(
                        Current.Key, Presence,
						Current.Value.Get(),
						Requested,
						*UInputTrigger::StaticClass(),
						TEXT(
							"asset_data_input_action_trigger"),
						Resources,
						Expected,
						OutError))
				{
					return false;
				}
			}
			else if (Current.Key == TEXT("modifiers"))
			{
				if (!TryApplyClassCollectionPresence(
                        Current.Key, Presence,
						Current.Value.Get(),
						Requested,
						*UInputModifier::StaticClass(),
						TEXT(
							"asset_data_input_action_modifier"),
						Resources,
						Expected,
						OutError))
				{
					return false;
				}
			}
			else
			{
                return Fail(OutError, JsonFailure(TEXT("asset_data_enhanced_input_expected_field_invalid"),
                    TEXT("This field has no Enhanced Input expected-state builder for the selected asset type."), Current.Key,
                    MakeShared<FJsonValueString>(Current.Key), MakeShared<FJsonValueString>(TEXT("supported Enhanced Input field"))));
			}
			ExpectedItems.Emplace(
				Current.Key,
				Expected.ToSharedRef());
		}
		return TryBuildProjectionObject(
			MoveTemp(ExpectedItems),
			Resources,
			OutExpectedFields,
			OutError);
	}
}
