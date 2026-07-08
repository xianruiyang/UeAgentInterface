// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/UeAgentUObjectProperties.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "UObject/Class.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

namespace UeAgentUObjectProperties
{
	FExportOptions::FExportOptions()
		: Profile(EExportProfile::Authoring)
		, bIncludeTransient(false)
		, bIncludeDeprecated(false)
		, bIncludeEditorOnly(false)
		, bIncludeReadOnly(false)
		, bIncludeNonEditable(false)
		, MaxProperties(1000)
		, MaxDepth(4)
		, MaxContainerItems(64)
		, MaxStringChars(2048)
	{
	}

	FApplyOptions::FApplyOptions()
		: bAllowReadOnly(false)
	{
	}

	namespace
	{
		static FString GetObjectPathSafe(const UObject* Object)
		{
			return Object ? Object->GetPathName() : FString();
		}

		static FString GetPackagePathSafe(const UObject* Object)
		{
			return Object && Object->GetPackage() ? Object->GetPackage()->GetName() : FString();
		}

		static FString GetPropertyDisplayName(const FProperty* Property)
		{
			return Property ? Property->GetDisplayNameText().ToString() : FString();
		}

		static FString GetPropertyTooltip(const FProperty* Property)
		{
			return Property ? Property->GetToolTipText().ToString() : FString();
		}

		static TArray<TSharedPtr<FJsonValue>> BuildPropertyFlags(const FProperty* Property)
		{
			TArray<TSharedPtr<FJsonValue>> Flags;
			if (!Property)
			{
				return Flags;
			}

			auto AddFlag = [&Flags, Property](EPropertyFlags Flag, const TCHAR* Name)
			{
				if (Property->HasAnyPropertyFlags(Flag))
				{
					Flags.Add(MakeShared<FJsonValueString>(Name));
				}
			};

			AddFlag(CPF_Edit, TEXT("Edit"));
			AddFlag(CPF_EditConst, TEXT("EditConst"));
			AddFlag(CPF_BlueprintVisible, TEXT("BlueprintVisible"));
			AddFlag(CPF_BlueprintReadOnly, TEXT("BlueprintReadOnly"));
			AddFlag(CPF_Config, TEXT("Config"));
			AddFlag(CPF_GlobalConfig, TEXT("GlobalConfig"));
			AddFlag(CPF_Transient, TEXT("Transient"));
			AddFlag(CPF_Deprecated, TEXT("Deprecated"));

			AddFlag(CPF_DuplicateTransient, TEXT("DuplicateTransient"));
			AddFlag(CPF_NonPIEDuplicateTransient, TEXT("NonPIEDuplicateTransient"));
			AddFlag(CPF_TextExportTransient, TEXT("TextExportTransient"));
			AddFlag(CPF_SaveGame, TEXT("SaveGame"));
			return Flags;
		}

		static const TCHAR* GetProfileName(EExportProfile Profile)
		{
			switch (Profile)
			{
			case EExportProfile::Index:
				return TEXT("index");
			case EExportProfile::Authoring:
				return TEXT("authoring");
			case EExportProfile::Full:
				return TEXT("full");
			case EExportProfile::Debug:
				return TEXT("debug");
			default:
				return TEXT("authoring");
			}
		}

		static FExportOptions ResolveOptions(FExportOptions Options)
		{
			if (Options.Profile == EExportProfile::Full || Options.Profile == EExportProfile::Debug)
			{
				Options.bIncludeReadOnly = true;
				Options.bIncludeNonEditable = true;
				Options.bIncludeEditorOnly = true;
			}
			return Options;
		}

		static bool ShouldIncludeExtendedMetadata(const FExportOptions& Options)
		{
			return Options.Profile == EExportProfile::Full || Options.Profile == EExportProfile::Debug;
		}

		static bool ShouldIncludeAuthoringMetadata(const FExportOptions& Options)
		{
			return Options.Profile == EExportProfile::Authoring
				|| Options.Profile == EExportProfile::Full
				|| Options.Profile == EExportProfile::Debug;
		}

		static bool ShouldIncludeTooltip(const FExportOptions& Options)
		{
			return Options.Profile != EExportProfile::Index;
		}

		static bool ShouldIncludeValues(const FExportOptions& Options)
		{
			return Options.Profile != EExportProfile::Index;
		}

		static bool ShouldIncludeDefaultValues(const FExportOptions& Options)
		{
			return Options.Profile == EExportProfile::Full || Options.Profile == EExportProfile::Debug;
		}

		static bool ShouldOmitUnchangedProperties(const FExportOptions& Options)
		{
			return false;
		}

		static bool ShouldIncludeSkippedDetails(const FExportOptions& Options)
		{
			return Options.Profile == EExportProfile::Debug;
		}

		static bool IsEditableProperty(const FProperty* Property)
		{
			return Property
				&& Property->HasAnyPropertyFlags(CPF_Edit)
				&& !Property->HasAnyPropertyFlags(CPF_EditConst | CPF_DisableEditOnInstance | CPF_Deprecated | CPF_Transient);
		}

		static TArray<TSharedPtr<FJsonValue>> BuildCategoryPath(const FProperty* Property)
		{
			TArray<TSharedPtr<FJsonValue>> Path;
			if (!Property)
			{
				return Path;
			}

			TArray<FString> Parts;
			Property->GetMetaData(TEXT("Category")).ParseIntoArray(Parts, TEXT("|"), true);
			for (FString Part : Parts)
			{
				Part.TrimStartAndEndInline();
				if (!Part.IsEmpty())
				{
					Path.Add(MakeShared<FJsonValueString>(Part));
				}
			}
			return Path;
		}

		static FString GetValueKind(const FProperty* Property)
		{
			if (!Property)
			{
				return TEXT("unknown");
			}
			if (CastField<FBoolProperty>(Property))
			{
				return TEXT("bool");
			}
			if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
			{
				return ByteProperty->Enum ? TEXT("enum") : TEXT("int");
			}
			if (CastField<FEnumProperty>(Property))
			{
				return TEXT("enum");
			}
			if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
			{
				return NumericProperty->IsFloatingPoint() ? TEXT("float") : TEXT("int");
			}
			if (CastField<FStrProperty>(Property))
			{
				return TEXT("string");
			}
			if (CastField<FNameProperty>(Property))
			{
				return TEXT("name");
			}
			if (CastField<FTextProperty>(Property))
			{
				return TEXT("text");
			}
			if (CastField<FSoftObjectProperty>(Property))
			{
				return TEXT("soft_object");
			}
			if (CastField<FClassProperty>(Property) || CastField<FSoftClassProperty>(Property))
			{
				return TEXT("class");
			}
			if (CastField<FObjectPropertyBase>(Property))
			{
				return TEXT("object");
			}
			if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
			{
				if (StructProperty->Struct)
				{
					const FString StructName = StructProperty->Struct->GetName();
					if (StructName == TEXT("Vector") || StructName == TEXT("Vector2D") || StructName == TEXT("Vector3f") || StructName == TEXT("Vector2f"))
					{
						return TEXT("vector");
					}
					if (StructName == TEXT("Rotator"))
					{
						return TEXT("rotator");
					}
					if (StructName == TEXT("Transform"))
					{
						return TEXT("transform");
					}
					if (StructName == TEXT("Color") || StructName == TEXT("LinearColor"))
					{
						return TEXT("color");
					}
				}
				return TEXT("struct");
			}
			if (CastField<FArrayProperty>(Property))
			{
				return TEXT("array");
			}
			if (CastField<FSetProperty>(Property))
			{
				return TEXT("set");
			}
			if (CastField<FMapProperty>(Property))
			{
				return TEXT("map");
			}
			return TEXT("import_text");
		}

		static bool IsKnownCompactStruct(const FString& StructName)
		{
			return StructName == TEXT("Vector")
				|| StructName == TEXT("Vector2D")
				|| StructName == TEXT("Vector3f")
				|| StructName == TEXT("Vector2f")
				|| StructName == TEXT("Rotator")
				|| StructName == TEXT("Color")
				|| StructName == TEXT("LinearColor");
		}

		static FString GetControlType(const FProperty* Property)
		{
			const FString Kind = GetValueKind(Property);
			if (Kind == TEXT("bool"))
			{
				return TEXT("checkbox");
			}
			if (Kind == TEXT("enum"))
			{
				return TEXT("enum");
			}
			if (Kind == TEXT("int") || Kind == TEXT("float"))
			{
				return TEXT("number");
			}
			if (Kind == TEXT("object") || Kind == TEXT("soft_object"))
			{
				return TEXT("object_picker");
			}
			if (Kind == TEXT("class"))
			{
				return TEXT("class_picker");
			}
			if (Kind == TEXT("array") || Kind == TEXT("set") || Kind == TEXT("map"))
			{
				return Kind;
			}
			return Kind;
		}

		static TArray<TSharedPtr<FJsonValue>> BuildSupportedOperations(const FProperty* Property)
		{
			TArray<TSharedPtr<FJsonValue>> Operations;
			auto Add = [&Operations](const TCHAR* Operation)
			{
				Operations.Add(MakeShared<FJsonValueString>(Operation));
			};

			if (CastField<FArrayProperty>(Property))
			{
				Add(TEXT("set"));
				Add(TEXT("add"));
				Add(TEXT("remove"));
				Add(TEXT("clear"));
				Add(TEXT("reorder"));
			}
			else if (CastField<FSetProperty>(Property))
			{
				Add(TEXT("set"));
				Add(TEXT("add"));
				Add(TEXT("remove"));
				Add(TEXT("clear"));
			}
			else if (CastField<FMapProperty>(Property))
			{
				Add(TEXT("set"));
				Add(TEXT("add"));
				Add(TEXT("remove"));
				Add(TEXT("clear"));
			}
			else if (IsEditableProperty(Property))
			{
				Add(TEXT("set"));
			}
			return Operations;
		}

		static TSharedPtr<FJsonObject> BuildAvailability(const FProperty* Property)
		{
			const bool bEditable = IsEditableProperty(Property);
			bool bHasEditCondition = false;
			bool bHasEditConditionHides = false;
			FString EditCondition;
			if (Property)
			{
				EditCondition = Property->GetMetaData(TEXT("EditCondition"));
				bHasEditCondition = !EditCondition.IsEmpty();
				bHasEditConditionHides = Property->HasMetaData(TEXT("EditConditionHides"));
			}
			if (bEditable && !bHasEditCondition && !bHasEditConditionHides)
			{
				return nullptr;
			}
			TSharedPtr<FJsonObject> Availability = MakeShared<FJsonObject>();
			Availability->SetBoolField(TEXT("enabled"), bEditable);
			Availability->SetBoolField(TEXT("editable"), bEditable);
			if (bHasEditCondition)
			{
				Availability->SetStringField(TEXT("edit_condition"), EditCondition);
			}
			if (bHasEditConditionHides)
			{
				Availability->SetBoolField(TEXT("edit_condition_hides"), true);
			}
			if (!bEditable)
			{
				Availability->SetStringField(TEXT("disabled_reason"), TEXT("not_editable_by_property_flags"));
			}
			return Availability;
		}

		static TSharedPtr<FJsonObject> BuildConstraints(const FProperty* Property)
		{
			TSharedPtr<FJsonObject> Constraints = MakeShared<FJsonObject>();
			if (!Property)
			{
				return Constraints;
			}

			auto AddMeta = [Property, Constraints](const TCHAR* SourceKey, const TCHAR* OutputKey)
			{
				const FString Value = Property->GetMetaData(SourceKey);
				if (!Value.IsEmpty())
				{
					Constraints->SetStringField(OutputKey, Value);
				}
			};

			AddMeta(TEXT("ClampMin"), TEXT("clamp_min"));
			AddMeta(TEXT("ClampMax"), TEXT("clamp_max"));
			AddMeta(TEXT("UIMin"), TEXT("ui_min"));
			AddMeta(TEXT("UIMax"), TEXT("ui_max"));
			AddMeta(TEXT("Units"), TEXT("units"));

			if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
			{
				if (ObjectProperty->PropertyClass)
				{
					Constraints->SetStringField(TEXT("allowed_class"), GetClassPath(ObjectProperty->PropertyClass));
				}
			}
			if (const FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
			{
				if (ClassProperty->MetaClass)
				{
					Constraints->SetStringField(TEXT("allowed_class"), GetClassPath(ClassProperty->MetaClass));
				}
			}
			if (const FSoftClassProperty* SoftClassProperty = CastField<FSoftClassProperty>(Property))
			{
				if (SoftClassProperty->MetaClass)
				{
					Constraints->SetStringField(TEXT("allowed_class"), GetClassPath(SoftClassProperty->MetaClass));
				}
			}
			if (Constraints->Values.IsEmpty())
			{
				return nullptr;
			}
			return Constraints;
		}


		static bool TryCompareWithDefault(UObject* Object, FProperty* Property, bool& bOutIdentical)
		{
			bOutIdentical = true;
			if (!Object || !Property || !Object->GetClass() || Object->HasAnyFlags(RF_ClassDefaultObject))
			{
				return false;
			}

			UObject* DefaultObject = Object->GetClass()->GetDefaultObject(false);
			if (!DefaultObject || DefaultObject == Object)
			{
				return false;
			}

			const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
			const void* DefaultValuePtr = Property->ContainerPtrToValuePtr<void>(DefaultObject);
			if (!ValuePtr || !DefaultValuePtr)
			{
				return false;
			}

			bOutIdentical = Property->Identical(ValuePtr, DefaultValuePtr, PPF_None);
			return true;
		}

		static TSharedPtr<FJsonValue> MakeBoundedStringValue(const FString& Value, const FExportOptions& Options)
		{
			if (Options.MaxStringChars <= 0 || Value.Len() <= Options.MaxStringChars)
			{
				return MakeShared<FJsonValueString>(Value);
			}

			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("kind"), TEXT("string"));
			Obj->SetStringField(TEXT("preview"), Value.Left(Options.MaxStringChars));
			Obj->SetNumberField(TEXT("full_length"), Value.Len());
			Obj->SetBoolField(TEXT("truncated"), true);
			return MakeShared<FJsonValueObject>(Obj);
		}

		static TSharedPtr<FJsonValue> MakeDepthExceededValue(const FProperty* Property, int32 Depth)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("kind"), TEXT("depth_exceeded"));
			Obj->SetStringField(TEXT("cpp_type"), Property ? Property->GetCPPType() : FString());
			Obj->SetNumberField(TEXT("depth"), Depth);
			return MakeShared<FJsonValueObject>(Obj);
		}

		static TSharedPtr<FJsonObject> BuildObjectReferenceValue(const UObject* ObjectValue)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("kind"), TEXT("object_reference"));
			Obj->SetBoolField(TEXT("is_null"), ObjectValue == nullptr);
			if (ObjectValue)
			{
				Obj->SetStringField(TEXT("object_path"), ObjectValue->GetPathName());
				Obj->SetStringField(TEXT("name"), ObjectValue->GetName());
				Obj->SetStringField(TEXT("class_path"), GetClassPath(ObjectValue->GetClass()));
				Obj->SetStringField(TEXT("package_path"), ObjectValue->GetPackage() ? ObjectValue->GetPackage()->GetName() : FString());
			}
			return Obj;
		}

		static void WriteEnumCandidatesToDescriptor(TSharedPtr<FJsonObject>& Descriptor, const UEnum* Enum)
		{
			if (!Enum || !Descriptor.IsValid())
			{
				return;
			}

			// First pass: detect which optional columns exist
			bool bHasDisplayName = false;
			bool bHasTooltip = false;
			for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
			{
				if (Enum->HasMetaData(TEXT("Hidden"), Index))
				{
					continue;
				}
				const FString Name = Enum->GetNameStringByIndex(Index);
				if (Name.IsEmpty() || Name.Equals(TEXT("MAX"), ESearchCase::IgnoreCase) || Name.EndsWith(TEXT("_MAX"), ESearchCase::IgnoreCase))
				{
					continue;
				}
				const FString DisplayName = Enum->GetDisplayNameTextByIndex(Index).ToString();
				if (!DisplayName.IsEmpty() && DisplayName != Name)
				{
					bHasDisplayName = true;
				}
				if (!Enum->GetMetaData(TEXT("ToolTip"), Index).IsEmpty())
				{
					bHasTooltip = true;
				}
				if (bHasDisplayName && bHasTooltip)
				{
					break;
				}
			}

			// Second pass: build rows
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
			{
				if (Enum->HasMetaData(TEXT("Hidden"), Index))
				{
					continue;
				}
				const FString Name = Enum->GetNameStringByIndex(Index);
				if (Name.IsEmpty() || Name.Equals(TEXT("MAX"), ESearchCase::IgnoreCase) || Name.EndsWith(TEXT("_MAX"), ESearchCase::IgnoreCase))
				{
					continue;
				}

				TArray<TSharedPtr<FJsonValue>> Row;
				Row.Add(MakeShared<FJsonValueString>(Name));
				if (bHasDisplayName)
				{
					const FString DisplayName = Enum->GetDisplayNameTextByIndex(Index).ToString();
					if (!DisplayName.IsEmpty() && DisplayName != Name)
					{
						Row.Add(MakeShared<FJsonValueString>(DisplayName));
					}
					else
					{
						Row.Add(MakeShared<FJsonValueString>(Name));
					}
				}
				if (bHasTooltip)
				{
					const FString Tooltip = Enum->GetMetaData(TEXT("ToolTip"), Index);
					Row.Add(MakeShared<FJsonValueString>(Tooltip.IsEmpty() ? FString() : Tooltip));
				}
				Rows.Add(MakeShared<FJsonValueArray>(Row));
			}

			if (Rows.Num() == 0)
			{
				return;
			}

			TArray<TSharedPtr<FJsonValue>> Header;
			Header.Add(MakeShared<FJsonValueString>(TEXT("name")));
			if (bHasDisplayName)
			{
				Header.Add(MakeShared<FJsonValueString>(TEXT("display_name")));
			}
			if (bHasTooltip)
			{
				Header.Add(MakeShared<FJsonValueString>(TEXT("tooltip")));
			}
			Descriptor->SetArrayField(TEXT("candidates_header"), Header);
			Descriptor->SetArrayField(TEXT("candidates"), Rows);
		}

		static void ExportTextForProperty(FProperty* Property, const void* ValuePtr, UObject* Owner, FString& OutText)
		{
			OutText.Reset();
			if (Property && ValuePtr)
			{
				Property->ExportTextItem_Direct(OutText, ValuePtr, nullptr, Owner, PPF_None);
			}
		}

		static TSharedPtr<FJsonObject> MakeApplyResult(const FString& PropertyPath, const FString& Status, const FString& Reason = FString())
		{
			TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("property_path"), PropertyPath);
			Result->SetStringField(TEXT("status"), Status);
			if (!Reason.IsEmpty())
			{
				Result->SetStringField(TEXT("reason"), Reason);
			}
			return Result;
		}

		static bool TryGetStringFromJsonValue(const TSharedPtr<FJsonValue>& Value, FString& OutString)
		{
			if (!Value.IsValid() || Value->IsNull())
			{
				return false;
			}
			if (Value->Type == EJson::String)
			{
				OutString = Value->AsString();
				return true;
			}
			if (Value->Type == EJson::Number)
			{
				OutString = FString::SanitizeFloat(Value->AsNumber());
				return true;
			}
			if (Value->Type == EJson::Boolean)
			{
				OutString = Value->AsBool() ? TEXT("True") : TEXT("False");
				return true;
			}
			return false;
		}

		static bool TryGetPropertyPathFromDescriptor(const TSharedPtr<FJsonObject>& Descriptor, FString& OutPropertyPath)
		{
			OutPropertyPath.Reset();
			if (!Descriptor.IsValid())
			{
				return false;
			}

			auto TryField = [&OutPropertyPath](const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
			{
				return Object.IsValid()
					&& Object->TryGetStringField(FieldName, OutPropertyPath)
					&& !OutPropertyPath.IsEmpty();
			};

			if (TryField(Descriptor, TEXT("property_path"))
				|| TryField(Descriptor, TEXT("property_name"))
				|| TryField(Descriptor, TEXT("name")))
			{
				return true;
			}

			const TSharedPtr<FJsonObject>* Selector = nullptr;
			if (Descriptor->TryGetObjectField(TEXT("selector"), Selector) && Selector && Selector->IsValid())
			{
				return TryField(*Selector, TEXT("property_path"))
					|| TryField(*Selector, TEXT("property_name"))
					|| TryField(*Selector, TEXT("name"));
			}
			return false;
		}

		static bool TryGetImportTextFromDescriptor(const TSharedPtr<FJsonObject>& Descriptor, const FProperty* Property, FString& OutImportText, FString& OutReason)
		{
			OutImportText.Reset();
			OutReason.Reset();
			if (!Descriptor.IsValid())
			{
				OutReason = TEXT("invalid_descriptor");
				return false;
			}

			const TSharedPtr<FJsonValue> ValueTextField = Descriptor->TryGetField(TEXT("value_text"));
			if (TryGetStringFromJsonValue(ValueTextField, OutImportText))
			{
				return true;
			}
			if (ValueTextField.IsValid() && ValueTextField->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> ValueTextObject = ValueTextField->AsObject();
				if (ValueTextObject.IsValid() && !ValueTextObject->GetBoolField(TEXT("truncated")))
				{
					if (ValueTextObject->TryGetStringField(TEXT("preview"), OutImportText))
					{
						return true;
					}
				}
			}

			const TSharedPtr<FJsonValue> ValueField = Descriptor->TryGetField(TEXT("value"));
			if (TryGetStringFromJsonValue(ValueField, OutImportText))
			{
				return true;
			}
			if (ValueField.IsValid() && ValueField->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> ValueObject = ValueField->AsObject();
				if (ValueObject.IsValid())
				{
					FString Kind;
					ValueObject->TryGetStringField(TEXT("kind"), Kind);
					if (Kind == TEXT("enum"))
					{
						if (ValueObject->TryGetStringField(TEXT("name"), OutImportText))
						{
							return true;
						}
						double RawValue = 0.0;
						if (ValueObject->TryGetNumberField(TEXT("value"), RawValue))
						{
							OutImportText = FString::FromInt(static_cast<int32>(RawValue));
							return true;
						}
					}
					if (Kind == TEXT("object_reference"))
					{
						if (ValueObject->GetBoolField(TEXT("is_null")))
						{
							OutImportText = TEXT("None");
							return true;
						}
						if (ValueObject->TryGetStringField(TEXT("object_path"), OutImportText))
						{
							return true;
						}
					}
					if (Kind == TEXT("import_text"))
					{
						const TSharedPtr<FJsonValue> TextField = ValueObject->TryGetField(TEXT("text"));
						if (TryGetStringFromJsonValue(TextField, OutImportText))
						{
							return true;
						}
					}
				}
			}

			if (ValueField.IsValid() && ValueField->Type == EJson::Array)
			{
				const FStructProperty* StructProp = CastField<FStructProperty>(Property);
				if (StructProp && StructProp->Struct)
				{
					const TArray<TSharedPtr<FJsonValue>>& Arr = ValueField->AsArray();
					TArray<FString> Parts;
					int32 Idx = 0;
					for (TFieldIterator<FProperty> It(StructProp->Struct, EFieldIterationFlags::IncludeSuper); It; ++It)
					{
						FProperty* Inner = *It;
						FString Skip;
						if (ShouldSkipProperty(Inner, FExportOptions(), Skip))
						{
							continue;
						}
						if (Idx >= Arr.Num())
						{
							break;
						}
						const TSharedPtr<FJsonValue>& Elem = Arr[Idx];
						FString Text;
						if (TryGetStringFromJsonValue(Elem, Text))
						{
							Parts.Add(FString::Printf(TEXT("%s=%s"), *Inner->GetName(), *Text));
						}
						else if (Elem.IsValid() && Elem->Type == EJson::Number)
						{
							Parts.Add(FString::Printf(TEXT("%s=%s"), *Inner->GetName(), *FString::SanitizeFloat(Elem->AsNumber())));
						}
						else if (Elem.IsValid() && Elem->Type == EJson::Boolean)
						{
							Parts.Add(FString::Printf(TEXT("%s=%s"), *Inner->GetName(), Elem->AsBool() ? TEXT("true") : TEXT("false")));
						}
						else
						{
							Parts.Add(FString::Printf(TEXT("%s=0"), *Inner->GetName()));
						}
						++Idx;
					}
					if (Parts.Num() > 0)
					{
						OutImportText = FString::Printf(TEXT("(%s)"), *FString::Join(Parts, TEXT(",")));
						return true;
					}
				}
			}

			OutReason = TEXT("missing_import_text");
			return false;
		}
	}

	FString GetClassPath(const UClass* Class)
	{
		if (!Class)
		{
			return FString();
		}
		return Class->GetPathName();
	}

	TSharedPtr<FJsonObject> BuildObjectIdentity(const UObject* Object)
	{
		TSharedPtr<FJsonObject> Identity = MakeShared<FJsonObject>();
		Identity->SetStringField(TEXT("name"), Object ? Object->GetName() : FString());
		Identity->SetStringField(TEXT("object_path"), GetObjectPathSafe(Object));
		Identity->SetStringField(TEXT("package_path"), GetPackagePathSafe(Object));
		Identity->SetStringField(TEXT("class_path"), Object ? GetClassPath(Object->GetClass()) : FString());
		Identity->SetStringField(TEXT("outer_path"), Object && Object->GetOuter() ? Object->GetOuter()->GetPathName() : FString());
		Identity->SetBoolField(TEXT("is_cdo"), Object ? Object->HasAnyFlags(RF_ClassDefaultObject) : false);
		return Identity;
	}

	bool ShouldSkipProperty(const FProperty* Property, const FExportOptions& Options, FString& OutReason)
	{
		OutReason.Reset();
		if (!Property)
		{
			OutReason = TEXT("invalid_property");
			return true;
		}
		if (!Options.bIncludeTransient && Property->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient | CPF_TextExportTransient))
		{
			OutReason = TEXT("transient");
			return true;
		}
		if (!Options.bIncludeDeprecated && (Property->HasAnyPropertyFlags(CPF_Deprecated) || Property->HasMetaData(TEXT("DeprecatedProperty"))))
		{
			OutReason = TEXT("deprecated");
			return true;
		}
		if (!Options.bIncludeEditorOnly && Property->IsEditorOnlyProperty())
		{
			OutReason = TEXT("editor_only");
			return true;
		}
		const bool bEditable = IsEditableProperty(Property);
		if (!Options.bIncludeNonEditable && !bEditable)
		{
			OutReason = TEXT("non_editable");
			return true;
		}
		if (!Options.bIncludeReadOnly && !bEditable)
		{
			OutReason = TEXT("read_only");
			return true;
		}
		return false;
	}

	bool IsUnsignedNumericProperty(const FNumericProperty* Property)
	{
		return CastField<FByteProperty>(Property)
			|| CastField<FUInt16Property>(Property)
			|| CastField<FUInt32Property>(Property)
			|| CastField<FUInt64Property>(Property);
	}

	static const UEnum* GetEnumFromProperty(FProperty* Property)
	{
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			return EnumProperty->GetEnum();
		}
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			return ByteProperty->Enum;
		}
		return nullptr;
	}

	TSharedPtr<FJsonValue> ExportPropertyValue(FProperty* Property, const void* ValuePtr, UObject* Owner, const FExportOptions& Options, int32 Depth)
	{
		if (!Property || !ValuePtr)
		{
			return MakeShared<FJsonValueNull>();
		}
		if (Options.MaxDepth >= 0 && Depth > Options.MaxDepth)
		{
			return MakeDepthExceededValue(Property, Depth);
		}

		if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			return MakeShared<FJsonValueBoolean>(BoolProperty->GetPropertyValue(ValuePtr));
		}

		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			const int64 RawValue = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
			const UEnum* Enum = EnumProperty->GetEnum();
			return MakeShared<FJsonValueString>(Enum ? Enum->GetNameStringByValue(RawValue) : FString());
		}

		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			if (ByteProperty->Enum)
			{
				const int64 RawValue = ByteProperty->GetPropertyValue(ValuePtr);
				return MakeShared<FJsonValueString>(ByteProperty->Enum->GetNameStringByValue(RawValue));
			}
			return MakeShared<FJsonValueNumber>(ByteProperty->GetPropertyValue(ValuePtr));
		}

		if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			if (NumericProperty->IsFloatingPoint())
			{
				return MakeShared<FJsonValueNumber>(NumericProperty->GetFloatingPointPropertyValue(ValuePtr));
			}
			if (NumericProperty->IsInteger())
			{
				if (IsUnsignedNumericProperty(NumericProperty))
				{
					return MakeShared<FJsonValueNumber>(static_cast<double>(NumericProperty->GetUnsignedIntPropertyValue(ValuePtr)));
				}
				return MakeShared<FJsonValueNumber>(static_cast<double>(NumericProperty->GetSignedIntPropertyValue(ValuePtr)));
			}
		}

		if (const FStrProperty* StrProperty = CastField<FStrProperty>(Property))
		{
			return MakeBoundedStringValue(StrProperty->GetPropertyValue(ValuePtr), Options);
		}
		if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			return MakeBoundedStringValue(NameProperty->GetPropertyValue(ValuePtr).ToString(), Options);
		}
		if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			return MakeBoundedStringValue(TextProperty->GetPropertyValue(ValuePtr).ToString(), Options);
		}

		if (const FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
		{
			const FSoftObjectPtr SoftValue = SoftObjectProperty->GetPropertyValue(ValuePtr);
			const FSoftObjectPath Path = SoftValue.ToSoftObjectPath();
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("kind"), TEXT("soft_object_reference"));
			Obj->SetBoolField(TEXT("is_null"), Path.IsNull());
			Obj->SetStringField(TEXT("path"), Path.ToString());
			Obj->SetStringField(TEXT("asset_path"), Path.GetAssetPathString());
			Obj->SetStringField(TEXT("sub_path"), Path.GetSubPathString());
			return MakeShared<FJsonValueObject>(Obj);
		}

		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			return MakeShared<FJsonValueObject>(BuildObjectReferenceValue(ObjectProperty->GetObjectPropertyValue(ValuePtr)));
		}

		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			const bool bCompact = StructProperty->Struct && IsKnownCompactStruct(StructProperty->Struct->GetName());
			if (bCompact)
			{
				TArray<TSharedPtr<FJsonValue>> Items;
				int32 FieldCount = 0;
				if (StructProperty->Struct)
				{
					for (TFieldIterator<FProperty> It(StructProperty->Struct, EFieldIterationFlags::IncludeSuper); It; ++It)
					{
						FProperty* InnerProperty = *It;
						FString SkipReason;
						if (ShouldSkipProperty(InnerProperty, Options, SkipReason))
						{
							continue;
						}
						if (Options.MaxContainerItems > 0 && FieldCount >= Options.MaxContainerItems)
						{
							break;
						}
						const void* InnerPtr = InnerProperty->ContainerPtrToValuePtr<void>(ValuePtr);
						Items.Add(ExportPropertyValue(InnerProperty, InnerPtr, Owner, Options, Depth + 1));
						++FieldCount;
					}
				}
				return MakeShared<FJsonValueArray>(Items);
			}

			TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
			int32 FieldCount = 0;
			if (StructProperty->Struct)
			{
				for (TFieldIterator<FProperty> It(StructProperty->Struct, EFieldIterationFlags::IncludeSuper); It; ++It)
				{
					FProperty* InnerProperty = *It;
					FString SkipReason;
					if (ShouldSkipProperty(InnerProperty, Options, SkipReason))
					{
						continue;
					}
					if (Options.MaxContainerItems > 0 && FieldCount >= Options.MaxContainerItems)
					{
						break;
					}
					const void* InnerPtr = InnerProperty->ContainerPtrToValuePtr<void>(ValuePtr);
					Fields->SetField(InnerProperty->GetName(), ExportPropertyValue(InnerProperty, InnerPtr, Owner, Options, Depth + 1));
					++FieldCount;
				}
			}
			return MakeShared<FJsonValueObject>(Fields);
		}

		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
			TArray<TSharedPtr<FJsonValue>> Items;
			const int32 Count = Helper.Num();
			const int32 Limit = Options.MaxContainerItems > 0 ? FMath::Min(Count, Options.MaxContainerItems) : Count;
			for (int32 Index = 0; Index < Limit; ++Index)
			{
				Items.Add(ExportPropertyValue(ArrayProperty->Inner, Helper.GetRawPtr(Index), Owner, Options, Depth + 1));
			}
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("kind"), TEXT("array"));
			Obj->SetNumberField(TEXT("count"), Count);
			Obj->SetBoolField(TEXT("truncated"), Limit < Count);
			Obj->SetArrayField(TEXT("items"), Items);
			return MakeShared<FJsonValueObject>(Obj);
		}

		if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
		{
			FScriptSetHelper Helper(SetProperty, ValuePtr);
			TArray<TSharedPtr<FJsonValue>> Items;
			int32 Added = 0;
			for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
			{
				if (!Helper.IsValidIndex(Index))
				{
					continue;
				}
				if (Options.MaxContainerItems > 0 && Added >= Options.MaxContainerItems)
				{
					break;
				}
				Items.Add(ExportPropertyValue(SetProperty->ElementProp, Helper.GetElementPtr(Index), Owner, Options, Depth + 1));
				++Added;
			}
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("kind"), TEXT("set"));
			Obj->SetNumberField(TEXT("count"), Helper.Num());
			Obj->SetBoolField(TEXT("truncated"), Options.MaxContainerItems > 0 && Added < Helper.Num());
			Obj->SetArrayField(TEXT("items"), Items);
			return MakeShared<FJsonValueObject>(Obj);
		}

		if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			FScriptMapHelper Helper(MapProperty, ValuePtr);
			TArray<TSharedPtr<FJsonValue>> Entries;
			int32 Added = 0;
			for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
			{
				if (!Helper.IsValidIndex(Index))
				{
					continue;
				}
				if (Options.MaxContainerItems > 0 && Added >= Options.MaxContainerItems)
				{
					break;
				}
				TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetField(TEXT("key"), ExportPropertyValue(MapProperty->KeyProp, Helper.GetKeyPtr(Index), Owner, Options, Depth + 1));
				Entry->SetField(TEXT("value"), ExportPropertyValue(MapProperty->ValueProp, Helper.GetValuePtr(Index), Owner, Options, Depth + 1));
				Entries.Add(MakeShared<FJsonValueObject>(Entry));
				++Added;
			}
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("kind"), TEXT("map"));
			Obj->SetNumberField(TEXT("count"), Helper.Num());
			Obj->SetBoolField(TEXT("truncated"), Options.MaxContainerItems > 0 && Added < Helper.Num());
			Obj->SetArrayField(TEXT("entries"), Entries);
			return MakeShared<FJsonValueObject>(Obj);
		}

		FString TextValue;
		ExportTextForProperty(Property, ValuePtr, Owner, TextValue);
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("kind"), TEXT("import_text"));
		Obj->SetField(TEXT("text"), MakeBoundedStringValue(TextValue, Options));
		return MakeShared<FJsonValueObject>(Obj);
	}

	TSharedPtr<FJsonObject> BuildPropertyDescriptor(UObject* Object, FProperty* Property, const FExportOptions& Options)
	{
		if (!Object || !Property)
		{
			return nullptr;
		}

		const FExportOptions ResolvedOptions = ResolveOptions(Options);
		FString SkipReason;
		if (ShouldSkipProperty(Property, ResolvedOptions, SkipReason))
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Descriptor = MakeShared<FJsonObject>();
		const UClass* ObjectClass = Object->GetClass();
		const UStruct* OwnerStruct = Property->GetOwnerStruct();
		const UClass* OwnerClass = Cast<UClass>(OwnerStruct);
		bool bIdenticalToDefault = true;
		const bool bHasDefaultComparison = TryCompareWithDefault(Object, Property, bIdenticalToDefault);
		if (ShouldOmitUnchangedProperties(ResolvedOptions) && bHasDefaultComparison && bIdenticalToDefault)
		{
			return nullptr;
		}

		Descriptor->SetStringField(TEXT("property_path"), Property->GetName());
		if (ShouldIncludeAuthoringMetadata(ResolvedOptions))
		{
			Descriptor->SetStringField(TEXT("display_name"), GetPropertyDisplayName(Property));
			Descriptor->SetArrayField(TEXT("category_path"), BuildCategoryPath(Property));
			Descriptor->SetStringField(TEXT("control_type"), GetControlType(Property));
			Descriptor->SetStringField(TEXT("value_kind"), GetValueKind(Property));
			if (TSharedPtr<FJsonObject> Availability = BuildAvailability(Property); Availability.IsValid())
			{
				Descriptor->SetObjectField(TEXT("availability"), Availability);
			}
			Descriptor->SetArrayField(TEXT("operations"), BuildSupportedOperations(Property));
			if (TSharedPtr<FJsonObject> ConstraintsRef = BuildConstraints(Property); ConstraintsRef.IsValid())
			{
				Descriptor->SetObjectField(TEXT("constraints"), ConstraintsRef);
			}
			if (const UEnum* PropEnum = GetEnumFromProperty(Property))
			{
				WriteEnumCandidatesToDescriptor(Descriptor, PropEnum);
			}
		}
		if (ShouldIncludeExtendedMetadata(ResolvedOptions))
		{
			Descriptor->SetStringField(TEXT("property_name"), Property->GetName());
			Descriptor->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
			Descriptor->SetStringField(TEXT("property_class"), Property->GetClass() ? Property->GetClass()->GetName() : FString());
			Descriptor->SetStringField(TEXT("owner_struct"), OwnerStruct ? OwnerStruct->GetName() : FString());
			Descriptor->SetStringField(TEXT("owner_class_path"), OwnerClass ? GetClassPath(OwnerClass) : FString());
			Descriptor->SetStringField(TEXT("category"), Property->GetMetaData(TEXT("Category")));
			Descriptor->SetNumberField(TEXT("array_dim"), Property->ArrayDim);
			Descriptor->SetBoolField(TEXT("inherited"), ObjectClass && OwnerClass && OwnerClass != ObjectClass);
			Descriptor->SetBoolField(TEXT("is_transient"), Property->HasAnyPropertyFlags(CPF_Transient));
			Descriptor->SetBoolField(TEXT("is_deprecated"), Property->HasAnyPropertyFlags(CPF_Deprecated) || Property->HasMetaData(TEXT("DeprecatedProperty")));
		}

		if (ShouldIncludeTooltip(ResolvedOptions))
		{
			const FString Tooltip = GetPropertyTooltip(Property);
			if (!Tooltip.IsEmpty())
			{
				Descriptor->SetStringField(TEXT("tooltip"), Tooltip);
			}
		}

		if (ShouldIncludeExtendedMetadata(ResolvedOptions))
		{
			Descriptor->SetBoolField(TEXT("editable"), IsEditableProperty(Property));
			Descriptor->SetBoolField(TEXT("read_only"), !IsEditableProperty(Property));
			Descriptor->SetBoolField(TEXT("is_editor_only"), Property->IsEditorOnlyProperty());
			Descriptor->SetArrayField(TEXT("property_flags"), BuildPropertyFlags(Property));
			Descriptor->SetBoolField(TEXT("has_cdo_default"), bHasDefaultComparison);
			Descriptor->SetBoolField(TEXT("differs_from_cdo"), bHasDefaultComparison ? !bIdenticalToDefault : false);
		}

		if (ShouldIncludeValues(ResolvedOptions))
		{
			const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
			FString ValueText;
			ExportTextForProperty(Property, ValuePtr, Object, ValueText);
			Descriptor->SetField(TEXT("value"), ExportPropertyValue(Property, ValuePtr, Object, ResolvedOptions));
			Descriptor->SetField(TEXT("value_text"), MakeBoundedStringValue(ValueText, ResolvedOptions));

			if (ShouldIncludeDefaultValues(ResolvedOptions) && bHasDefaultComparison)
			{
				UObject* DefaultObject = Object->GetClass() ? Object->GetClass()->GetDefaultObject(false) : nullptr;
				const void* DefaultValuePtr = DefaultObject ? Property->ContainerPtrToValuePtr<void>(DefaultObject) : nullptr;
				FString DefaultValueText;
				ExportTextForProperty(Property, DefaultValuePtr, DefaultObject, DefaultValueText);
				Descriptor->SetField(TEXT("default_value"), ExportPropertyValue(Property, DefaultValuePtr, DefaultObject, ResolvedOptions));
				Descriptor->SetField(TEXT("default_value_text"), MakeBoundedStringValue(DefaultValueText, ResolvedOptions));
			}
			else if (ShouldIncludeDefaultValues(ResolvedOptions))
			{
				Descriptor->SetField(TEXT("default_value"), MakeShared<FJsonValueNull>());
				Descriptor->SetField(TEXT("default_value_text"), MakeShared<FJsonValueNull>());
			}
		}

		return Descriptor;
	}

	TSharedPtr<FJsonObject> ExportObjectProperties(UObject* Object, const FExportOptions& Options)
	{
		const FExportOptions ResolvedOptions = ResolveOptions(Options);
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v1"));
		Root->SetStringField(TEXT("profile"), GetProfileName(ResolvedOptions.Profile));
		Root->SetObjectField(TEXT("object"), BuildObjectIdentity(Object));

		TArray<TSharedPtr<FJsonValue>> Properties;
		TArray<TSharedPtr<FJsonValue>> Skipped;
		bool bTruncated = false;

		if (Object && Object->GetClass())
		{
			for (TFieldIterator<FProperty> It(Object->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
			{
				FProperty* Property = *It;
				FString SkipReason;
				if (ShouldSkipProperty(Property, ResolvedOptions, SkipReason))
				{
					if (ShouldIncludeSkippedDetails(ResolvedOptions))
					{
						TSharedPtr<FJsonObject> SkipObj = MakeShared<FJsonObject>();
						SkipObj->SetStringField(TEXT("property_path"), Property ? Property->GetName() : FString());
						SkipObj->SetStringField(TEXT("reason"), SkipReason);
						Skipped.Add(MakeShared<FJsonValueObject>(SkipObj));
					}
					else
					{
						Skipped.Add(MakeShared<FJsonValueNull>());
					}
					continue;
				}

				if (ResolvedOptions.MaxProperties > 0 && Properties.Num() >= ResolvedOptions.MaxProperties)
				{
					bTruncated = true;
					break;
				}

				if (TSharedPtr<FJsonObject> Descriptor = BuildPropertyDescriptor(Object, Property, ResolvedOptions))
				{
					Properties.Add(MakeShared<FJsonValueObject>(Descriptor));
				}
			}
		}

		Root->SetNumberField(TEXT("property_count"), Properties.Num());
		Root->SetBoolField(TEXT("truncated"), bTruncated);
		Root->SetArrayField(TEXT("properties"), Properties);
		Root->SetNumberField(TEXT("skipped_count"), Skipped.Num());
		if (ShouldIncludeSkippedDetails(ResolvedOptions))
		{
			Root->SetArrayField(TEXT("skipped"), Skipped);
		}
		return Root;
	}

	TSharedPtr<FJsonObject> ExportActorProperties(AActor* Actor, const FExportOptions& Options)
	{
		return ExportObjectProperties(Actor, Options);
	}

	TSharedPtr<FJsonObject> ExportComponentProperties(UActorComponent* Component, const FExportOptions& Options)
	{
		return ExportObjectProperties(Component, Options);
	}

	TArray<TSharedPtr<FJsonValue>> ExportActorComponentProperties(AActor* Actor, const FExportOptions& Options)
	{
		TArray<TSharedPtr<FJsonValue>> OutComponents;
		if (!Actor)
		{
			return OutComponents;
		}

		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (UActorComponent* Component : Components)
		{
			if (!Component)
			{
				continue;
			}

			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("component_name"), Component->GetName());
			Entry->SetStringField(TEXT("component_class_path"), GetClassPath(Component->GetClass()));
			Entry->SetStringField(TEXT("component_schema"), TEXT("uai.uobject_properties.v1"));
			Entry->SetObjectField(TEXT("properties"), ExportComponentProperties(Component, Options));
			OutComponents.Add(MakeShared<FJsonValueObject>(Entry));
		}
		return OutComponents;
	}

	TSharedPtr<FJsonObject> ApplyObjectProperties(UObject* Object, const TSharedPtr<FJsonObject>& PropertiesJson, const FApplyOptions& Options)
	{
		TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties_apply.v1"));
		Summary->SetObjectField(TEXT("object"), BuildObjectIdentity(Object));

		TArray<TSharedPtr<FJsonValue>> Results;
		int32 Seen = 0;
		int32 Applied = 0;
		int32 Skipped = 0;
		int32 Failed = 0;

		if (!Object || !Object->GetClass())
		{
			Summary->SetStringField(TEXT("status"), TEXT("failed"));
			Summary->SetStringField(TEXT("reason"), TEXT("invalid_object"));
			Summary->SetNumberField(TEXT("properties_seen"), 0);
			Summary->SetNumberField(TEXT("properties_applied"), 0);
			Summary->SetNumberField(TEXT("properties_skipped"), 0);
			Summary->SetNumberField(TEXT("properties_failed"), 1);
			Summary->SetArrayField(TEXT("properties"), Results);
			return Summary;
		}

		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (!PropertiesJson.IsValid() || !PropertiesJson->TryGetArrayField(TEXT("properties"), Properties) || !Properties)
		{
			Summary->SetStringField(TEXT("status"), TEXT("skipped"));
			Summary->SetStringField(TEXT("reason"), TEXT("missing_properties_array"));
			Summary->SetNumberField(TEXT("properties_seen"), 0);
			Summary->SetNumberField(TEXT("properties_applied"), 0);
			Summary->SetNumberField(TEXT("properties_skipped"), 0);
			Summary->SetNumberField(TEXT("properties_failed"), 0);
			Summary->SetArrayField(TEXT("properties"), Results);
			return Summary;
		}

		for (const TSharedPtr<FJsonValue>& PropertyValue : *Properties)
		{
			Seen++;
			const TSharedPtr<FJsonObject> Descriptor = PropertyValue.IsValid() ? PropertyValue->AsObject() : nullptr;
			FString PropertyPath;
			if (!TryGetPropertyPathFromDescriptor(Descriptor, PropertyPath))
			{
				Skipped++;
				Results.Add(MakeShared<FJsonValueObject>(MakeApplyResult(PropertyPath, TEXT("skipped"), TEXT("missing_property_path"))));
				continue;
			}

			FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), *PropertyPath);
			if (!Property)
			{
				Skipped++;
				Results.Add(MakeShared<FJsonValueObject>(MakeApplyResult(PropertyPath, TEXT("skipped"), TEXT("property_not_found"))));
				continue;
			}
			if (!Options.bAllowReadOnly && !IsEditableProperty(Property))
			{
				Skipped++;
				Results.Add(MakeShared<FJsonValueObject>(MakeApplyResult(PropertyPath, TEXT("skipped"), TEXT("read_only_or_non_editable"))));
				continue;
			}

			FString ImportText;
			FString ImportReason;
			if (!TryGetImportTextFromDescriptor(Descriptor, Property, ImportText, ImportReason))
			{
				Skipped++;
				Results.Add(MakeShared<FJsonValueObject>(MakeApplyResult(PropertyPath, TEXT("skipped"), ImportReason)));
				continue;
			}

			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
			if (!ValuePtr)
			{
				Failed++;
				Results.Add(MakeShared<FJsonValueObject>(MakeApplyResult(PropertyPath, TEXT("failed"), TEXT("missing_value_ptr"))));
				continue;
			}

			Object->PreEditChange(Property);
			const TCHAR* ImportEnd = Property->ImportText_Direct(*ImportText, ValuePtr, Object, PPF_None);
			if (!ImportEnd)
			{
				Failed++;
				Results.Add(MakeShared<FJsonValueObject>(MakeApplyResult(PropertyPath, TEXT("failed"), TEXT("import_text_failed"))));
				continue;
			}
			FPropertyChangedEvent ChangeEvent(Property, EPropertyChangeType::ValueSet);
			Object->PostEditChangeProperty(ChangeEvent);
			Object->MarkPackageDirty();

			Applied++;
			Results.Add(MakeShared<FJsonValueObject>(MakeApplyResult(PropertyPath, TEXT("applied"))));
		}

		Summary->SetStringField(TEXT("status"), Failed > 0 ? TEXT("partial") : TEXT("applied"));
		Summary->SetNumberField(TEXT("properties_seen"), Seen);
		Summary->SetNumberField(TEXT("properties_applied"), Applied);
		Summary->SetNumberField(TEXT("properties_skipped"), Skipped);
		Summary->SetNumberField(TEXT("properties_failed"), Failed);
		Summary->SetArrayField(TEXT("properties"), Results);
		return Summary;
	}
}
