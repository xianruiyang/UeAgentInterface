#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectEnhancedInputMechanics.h"

#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
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
		bool Fail(
			FExecutionMutationError& OutError,
			FString Code,
			FString Detail = FString())
		{
			OutError = FExecutionMutationError(
				MoveTemp(Code),
				MoveTemp(Detail));
			return false;
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

		bool ValidateInputActionProperties()
		{
			const UClass* Class = UInputAction::StaticClass();
			return FindFProperty<FProperty>(
					Class,
					GET_MEMBER_NAME_CHECKED(
						UInputAction,
						ValueType))
				&& FindFProperty<FProperty>(
					Class,
					GET_MEMBER_NAME_CHECKED(
						UInputAction,
						bConsumeInput))
				&& FindFProperty<FProperty>(
					Class,
					GET_MEMBER_NAME_CHECKED(
						UInputAction,
						Triggers))
				&& FindFProperty<FProperty>(
					Class,
					GET_MEMBER_NAME_CHECKED(
						UInputAction,
						Modifiers));
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
			TArray<UClass*>& OutClasses,
			FExecutionMutationError& OutError)
		{
			if (Value.GetKind() != EUeAgentValueKind::Array)
			{
				return Fail(
					OutError,
					ErrorPrefix + TEXT("_array_invalid"));
			}
			OutClasses.Reset();
			TSet<UClass*> Seen;
			for (const FUeAgentCanonicalValueRef& Item
				: Value.GetArrayItems())
			{
				if (Item->GetKind() != EUeAgentValueKind::String
					|| Item->GetText().IsEmpty())
				{
					return Fail(
						OutError,
						ErrorPrefix + TEXT("_class_ref_invalid"));
				}
				const FString& ClassPath = Item->GetText();
				UClass* Class =
					LoadObject<UClass>(nullptr, *ClassPath);
				if (!Class
					|| Class->GetPathName() != ClassPath
					|| !Class->IsChildOf(&BaseClass)
					|| Class->HasAnyClassFlags(
						CLASS_Abstract
							| CLASS_Deprecated
							| CLASS_NewerVersionExists))
				{
					return Fail(
						OutError,
						ErrorPrefix + TEXT("_class_invalid"),
						ClassPath);
				}
				if (Seen.Contains(Class))
				{
					return Fail(
						OutError,
						ErrorPrefix + TEXT("_class_duplicate"),
						ClassPath);
				}
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
			FExecutionMutationError& OutError)
		{
			OutSpecs.Reset();
			OutSpecs.Reserve(Existing.Num());
			for (const TObjectPtr<TObject>& Item : Existing)
			{
				TObject* Object = Item.Get();
				if (!Object)
				{
					return Fail(OutError, ErrorCode);
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
						return Fail(OutError, ErrorCode);
					}
					Object = NewObject<TObject>(
						Outer,
						Spec.Class,
						NAME_None,
						RF_Transactional);
				}
				if (!Object)
				{
					return Fail(OutError, ErrorCode);
				}
				OutObjects.Add(Object);
			}
			return true;
		}

		bool TryParseValueType(
			const FUeAgentCanonicalValue& Value,
			EInputActionValueType& OutValueType)
		{
			if (Value.GetKind() != EUeAgentValueKind::String)
			{
				return false;
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
			return false;
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
			UInputAction*& OutAction,
			FKey& OutKey,
			TArray<UClass*>& OutTriggers,
			TArray<UClass*>& OutModifiers,
			FExecutionMutationError& OutError)
		{
			const int32 ExpectedFieldCount =
				bIdentityOnly ? 2 : 4;
			if (Value.GetKind() != EUeAgentValueKind::Object
				|| Value.GetObjectItems().Num()
					!= ExpectedFieldCount)
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_input_mapping_"
						"object_invalid"));
			}
			const FUeAgentCanonicalValue* ActionRef =
				FindObjectField(Value, TEXT("action_ref"));
			const FUeAgentCanonicalValue* Key =
				FindObjectField(Value, TEXT("key"));
			if (!ActionRef
				|| !Key
				|| ActionRef->GetKind()
					!= EUeAgentValueKind::String
				|| ActionRef->GetText().IsEmpty()
				|| Key->GetKind()
					!= EUeAgentValueKind::String
				|| Key->GetText().IsEmpty())
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_input_mapping_"
						"identity_invalid"));
			}
			UObject* Resolved = nullptr;
			if (!Resolver.TryResolveExisting(
					ActionRef->GetText(),
					Resolved,
					OutError))
			{
				return false;
			}
			OutAction = Cast<UInputAction>(Resolved);
			if (!OutAction)
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_input_mapping_"
						"action_class_invalid"),
					ActionRef->GetText());
			}
			OutKey = FKey(FName(*Key->GetText()));
			if (!OutKey.IsValid()
				|| OutKey.GetFName().ToString()
					!= Key->GetText())
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_input_mapping_key_invalid"),
					Key->GetText());
			}
			OutTriggers.Reset();
			OutModifiers.Reset();
			if (bIdentityOnly)
			{
				return true;
			}
			const FUeAgentCanonicalValue* Triggers =
				FindObjectField(Value, TEXT("triggers"));
			const FUeAgentCanonicalValue* Modifiers =
				FindObjectField(Value, TEXT("modifiers"));
			return Triggers
				&& Modifiers
				&& TryResolveClassArray(
					*Triggers,
					*UInputTrigger::StaticClass(),
					TEXT(
						"asset_data_input_mapping_trigger"),
					OutTriggers,
					OutError)
				&& TryResolveClassArray(
					*Modifiers,
					*UInputModifier::StaticClass(),
					TEXT(
						"asset_data_input_mapping_modifier"),
					OutModifiers,
					OutError);
		}

		bool TryBuildMappingSpecFromExisting(
			const FEnhancedActionKeyMapping& Existing,
			TArray<TStrongObjectPtr<UObject>>& Pins,
			FInputMappingSpec& OutSpec,
			FExecutionMutationError& OutError)
		{
			if (!Existing.Action || !Existing.Key.IsValid())
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_input_mapping_"
						"existing_identity_invalid"));
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
					OutError)
				&& CaptureExistingSubobjects(
					Existing.Modifiers,
					Pins,
					OutSpec.Modifiers,
					TEXT(
						"asset_data_input_mapping_"
						"existing_modifier_invalid"),
				OutError);
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
				: Fail(
					OutError,
					StateError.Code.IsEmpty()
						? TEXT(
							"asset_data_enhanced_input_"
							"projection_string_invalid")
						: StateError.Code,
					StateError.Detail);
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
				: Fail(
					OutError,
					StateError.Code.IsEmpty()
						? TEXT(
							"asset_data_enhanced_input_"
							"projection_boolean_invalid")
						: StateError.Code,
					StateError.Detail);
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
				: Fail(
					OutError,
					StateError.Code.IsEmpty()
						? TEXT(
							"asset_data_enhanced_input_"
							"projection_array_invalid")
						: StateError.Code,
					StateError.Detail);
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
				: Fail(
					OutError,
					StateError.Code.IsEmpty()
						? TEXT(
							"asset_data_enhanced_input_"
							"projection_object_invalid")
						: StateError.Code,
					StateError.Detail);
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
			FString& OutText)
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
				return false;
			}
		}

		template <typename TObject>
		bool TryBuildClassCollectionProjection(
			const TArray<TObjectPtr<TObject>>& Objects,
			const UClass& BaseClass,
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
				if (!Class
					|| !Class->IsChildOf(&BaseClass)
					|| !TryBuildProjectionString(
						Class->GetPathName(),
						Resources,
						ClassPath,
						OutError))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_enhanced_input_"
							"existing_subobject_invalid"));
				}
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
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			TArray<FUeAgentCanonicalValueRef> Items;
			Items.Reserve(Classes.Num());
			for (const UClass* Class : Classes)
			{
				FUeAgentCanonicalValuePtr ClassPath;
				if (!Class
					|| !TryBuildProjectionString(
						Class->GetPathName(),
						Resources,
						ClassPath,
						OutError))
				{
					return false;
				}
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
			OutValues.Reset();
			if (!Action || !ValidateInputActionProperties())
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_enhanced_input_"
						"action_projection_invalid"));
			}
			FString ValueType;
			FUeAgentCanonicalValuePtr ValueTypeValue;
			FUeAgentCanonicalValuePtr ConsumeValue;
			FUeAgentCanonicalValuePtr TriggersValue;
			FUeAgentCanonicalValuePtr ModifiersValue;
			if (!TryFormatValueType(Action->ValueType, ValueType)
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
					*UInputTrigger::StaticClass(),
					Resources,
					TriggersValue,
					OutError)
				|| !TryBuildClassCollectionProjection(
					Action->Modifiers,
					*UInputModifier::StaticClass(),
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
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentCanonicalValuePtr ActionValue;
			FUeAgentCanonicalValuePtr KeyValue;
			FUeAgentCanonicalValuePtr TriggersValue;
			FUeAgentCanonicalValuePtr ModifiersValue;
			if (!Action
				|| !Key.IsValid()
				|| !TryBuildProjectionString(
					Action->GetPathName(),
					Resources,
					ActionValue,
					OutError)
				|| !TryBuildProjectionString(
					Key.GetFName().ToString(),
					Resources,
					KeyValue,
					OutError)
				|| !TryBuildClassCollectionProjection(
					Triggers,
					*UInputTrigger::StaticClass(),
					Resources,
					TriggersValue,
					OutError)
				|| !TryBuildClassCollectionProjection(
					Modifiers,
					*UInputModifier::StaticClass(),
					Resources,
					ModifiersValue,
					OutError))
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_enhanced_input_"
						"mapping_projection_invalid"));
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
			OutValues.Reset();
			if (!Context)
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_enhanced_input_"
						"mapping_context_projection_invalid"));
			}
			TSet<FString> SeenIdentities;
			TArray<FUeAgentCanonicalValueRef> Mappings;
			for (const FEnhancedActionKeyMapping& Mapping
				: Context->GetMappings())
			{
				if (!Mapping.Action || !Mapping.Key.IsValid())
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_enhanced_input_"
							"mapping_identity_invalid"));
				}
				const FString Identity =
					Mapping.Action->GetPathName()
					+ TEXT("\n")
					+ Mapping.Key.GetFName().ToString();
				FUeAgentCanonicalValuePtr Value;
				if (SeenIdentities.Contains(Identity)
					|| !TryBuildMappingProjectionValue(
						Mapping.Action.Get(),
						Mapping.Key,
						Mapping.Triggers,
						Mapping.Modifiers,
						Resources,
						Value,
						OutError))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_enhanced_input_"
							"mapping_identity_duplicate"),
						Identity);
				}
				SeenIdentities.Add(Identity);
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
				return Fail(
					OutError,
					TEXT(
						"asset_data_enhanced_input_"
						"selected_fields_missing"));
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
				if (Seen.Contains(FieldId)
					|| !Current
					|| !Default
					|| !ContextTag
					|| !TryBuildProjectionString(
						ContextTag,
						Resources,
						ContextValue,
						OutError))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_enhanced_input_"
							"selected_field_invalid"),
						FieldId);
				}
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
				return Fail(
					OutError,
					TEXT(
						"asset_data_enhanced_input_"
						"expected_state_invalid"));
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
					return Fail(
						OutError,
						TEXT(
							"asset_data_enhanced_input_"
							"expected_change_invalid"));
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
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			TArray<UClass*> Classes;
			return TryResolveClassArray(
					Requested,
					BaseClass,
					ErrorPrefix,
					Classes,
					OutError)
				&& TryBuildClassPathProjection(
					Classes,
					Resources,
					OutValue,
					OutError);
		}

		bool TryApplyClassCollectionPresence(
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
			if (!Requested
				|| (Presence != TEXT("set")
					&& Presence != TEXT("replace"))
				|| !TryNormalizeClassCollection(
					*Requested,
					BaseClass,
					ErrorPrefix,
					Resources,
					Normalized,
					OutError))
			{
				return false;
			}
			if (Presence == TEXT("replace"))
			{
				OutValue = Normalized;
				return true;
			}
			if (Current.GetKind() != EUeAgentValueKind::Array)
			{
				return false;
			}
			TArray<FUeAgentCanonicalValueRef> Items =
				Current.GetArrayItems();
			TSet<FString> Existing;
			for (const FUeAgentCanonicalValueRef& Item : Items)
			{
				FString Path;
				if (!TryReadProjectionString(Item.Get(), Path))
				{
					return false;
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
			FString& OutActionPath,
			FString& OutKey)
		{
			const FUeAgentCanonicalValue* Action =
				FindObjectField(Value, TEXT("action_ref"));
			const FUeAgentCanonicalValue* Key =
				FindObjectField(Value, TEXT("key"));
			return Action
				&& Key
				&& TryReadProjectionString(
					*Action,
					OutActionPath)
				&& TryReadProjectionString(*Key, OutKey);
		}

		bool TryNormalizeRequestedMapping(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FUeAgentCanonicalValue& Requested,
			const bool bIdentityOnly,
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
					bIdentityOnly,
					Action,
					Key,
					TriggerClasses,
					ModifierClasses,
					OutError)
				|| !Action)
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
					TriggerClasses,
					Resources,
					Triggers,
					OutError)
				|| !TryBuildClassPathProjection(
					ModifierClasses,
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
				return Fail(
					OutError,
					TEXT(
						"asset_data_enhanced_input_"
						"mapping_presence_invalid"),
					Presence);
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
						bIdentityOnly,
						Resources,
						Value,
						Identity,
						OutError)
					|| SeenRequested.Contains(Identity))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_enhanced_input_"
							"mapping_identity_duplicate"),
						Identity);
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
						Item.Get(),
						ActionPath,
						Key))
				{
					return false;
				}
				const FString Identity =
					ActionPath + TEXT("\n") + Key;
				if (ExistingIndexes.Contains(Identity))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_enhanced_input_"
							"mapping_identity_duplicate"),
						Identity);
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
						FString ActionPath;
						FString Key;
						return TryReadProjectedMappingIdentity(
								Item.Get(),
								ActionPath,
								Key)
							&& SeenRequested.Contains(
								ActionPath + TEXT("\n") + Key);
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
		if (!Action || !ValidateInputActionProperties())
		{
			return Fail(
				OutError,
				TEXT("asset_data_input_action_invalid"));
		}
		if (!ValueType.bSpecified
			&& !ConsumeInput.bSpecified
			&& !Triggers.bSpecified
			&& !Modifiers.bSpecified)
		{
			return Fail(
				OutError,
				TEXT("asset_data_input_action_changes_missing"));
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
				OutError)
			|| !CaptureExistingSubobjects(
				Action->Modifiers,
				Data->Pins,
				Data->NewModifiers,
				TEXT(
					"asset_data_input_action_"
					"existing_modifier_invalid"),
				OutError))
		{
			return false;
		}

		if (ValueType.bSpecified)
		{
			Data->bChangeValueType = true;
			if (ValueType.Presence != TEXT("set")
				|| !ValueType.CanonicalValue
				|| !TryParseValueType(
					*ValueType.CanonicalValue,
					Data->NewValueType))
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_input_action_"
						"value_type_invalid"));
			}
		}
		if (ConsumeInput.bSpecified)
		{
			Data->bChangeConsumeInput = true;
			if (ConsumeInput.Presence != TEXT("set")
				|| !ConsumeInput.CanonicalValue
				|| ConsumeInput.CanonicalValue->GetKind()
					!= EUeAgentValueKind::Boolean)
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_input_action_"
						"consume_input_invalid"));
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
						Classes,
						OutError))
				{
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
					return Fail(
						OutError,
						TEXT(
							"asset_data_input_action_"
							"trigger_presence_invalid"));
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
						Classes,
						OutError))
				{
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
					return Fail(
						OutError,
						TEXT(
							"asset_data_input_action_"
							"modifier_presence_invalid"));
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
			return Fail(
				OutError,
				TEXT("asset_data_input_action_state_invalid"));
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
				NewTriggers,
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
				NewModifiers,
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
		const FEnhancedInputActionPreparedState& State)
	{
		UInputAction* Action = Cast<UInputAction>(Target);
		if (!Action || !State.Data.IsValid())
		{
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
			return Fail(
				OutError,
				TEXT(
					"asset_data_input_mapping_context_invalid"));
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
					return Fail(
						OutError,
						TEXT(
							"asset_data_input_mapping_"
							"existing_identity_duplicate"));
				}
			}
			FInputMappingSpec Spec;
			if (!TryBuildMappingSpecFromExisting(
					Existing,
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
			return Fail(
				OutError,
				TEXT(
					"asset_data_input_mapping_array_invalid"));
		}
		const bool bIdentityOnly =
			Mappings.Presence == TEXT("remove");
		if (!bIdentityOnly
			&& Mappings.Presence != TEXT("set")
			&& Mappings.Presence != TEXT("replace"))
		{
			return Fail(
				OutError,
				TEXT(
					"asset_data_input_mapping_presence_invalid"));
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
					bIdentityOnly,
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
				return Fail(
					OutError,
					TEXT(
						"asset_data_input_mapping_"
						"identity_duplicate"));
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
			return Fail(
				OutError,
				TEXT(
					"asset_data_input_mapping_context_"
					"state_invalid"));
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
					Triggers,
					OutError)
				|| !TryMaterializeSubobjects(
					Context,
					Spec.Modifiers,
					*UInputModifier::StaticClass(),
					TEXT(
						"asset_data_input_mapping_"
						"modifier_create_failed"),
					Modifiers,
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
		const FEnhancedInputMappingContextPreparedState& State)
	{
		UInputMappingContext* Context =
			Cast<UInputMappingContext>(Target);
		if (!Context || !State.Data.IsValid())
		{
			return false;
		}
		const FEnhancedInputMappingContextPreparedData& Data =
			*State.Data;
		Context->Modify();
		Context->UnmapAll();
		for (const FEnhancedActionKeyMapping& Original
			: Data.OriginalMappings)
		{
			if (!Original.Action || !Original.Key.IsValid())
			{
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
			return Fail(
				OutError,
				TEXT(
					"asset_data_enhanced_input_"
					"field_projection_unsupported"),
				NativeMechanicId);
		}
		TMap<FString, FUeAgentCanonicalValueRef> CurrentValues;
		TMap<FString, FUeAgentCanonicalValueRef> DefaultValues;
		if (NativeMechanicId
			== TEXT(
				"uai.asset_data.mechanic:"
				"update_input_action:v1"))
		{
			const UInputAction* Action = Cast<UInputAction>(Target);
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
			const UInputMappingContext* Context =
				Cast<UInputMappingContext>(Target);
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
			return Fail(
				OutError,
				TEXT(
					"asset_data_enhanced_input_"
					"field_projection_unsupported"),
				NativeMechanicId);
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
				return Fail(
					OutError,
					TEXT(
						"asset_data_enhanced_input_"
						"expected_context_invalid"),
					Current.Key);
			}
			FString Presence;
			const FUeAgentCanonicalValue* Requested = nullptr;
			if (!TryReadChangePresenceAndValue(
					**Change,
					Presence,
					Requested))
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_enhanced_input_"
						"expected_presence_invalid"),
					Current.Key);
			}

			FUeAgentCanonicalValuePtr Expected;
			if (NativeMechanicId
					== TEXT(
						"uai.asset_data.mechanic:"
						"update_input_mapping_context:v1")
				&& Current.Key == TEXT("mappings"))
			{
				if (!TryApplyMappingPresence(
						Resolver,
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
				if (Presence != TEXT("set")
					|| !Requested
					|| !TryParseValueType(
						*Requested,
						ValueType)
					|| !TryFormatValueType(
						ValueType,
						Normalized)
					|| !TryBuildProjectionString(
						Normalized,
						Resources,
						Expected,
						OutError))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_enhanced_input_"
							"value_type_expected_invalid"));
				}
			}
			else if (Current.Key == TEXT("consume_input"))
			{
				if (Presence != TEXT("set")
					|| !Requested
					|| Requested->GetKind()
						!= EUeAgentValueKind::Boolean
					|| !TryBuildProjectionBoolean(
						Requested->GetBoolean(),
						Resources,
						Expected,
						OutError))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_enhanced_input_"
							"consume_input_expected_invalid"));
				}
			}
			else if (Current.Key == TEXT("triggers"))
			{
				if (!TryApplyClassCollectionPresence(
						Presence,
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
						Presence,
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
				return Fail(
					OutError,
					TEXT(
						"asset_data_enhanced_input_"
						"expected_field_invalid"),
					Current.Key);
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
