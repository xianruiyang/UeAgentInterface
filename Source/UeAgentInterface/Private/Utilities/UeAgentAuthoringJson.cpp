#include "Utilities/UeAgentAuthoringJson.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UnrealType.h"

namespace UeAgentAuthoringJson
{
	TArray<TSharedPtr<FJsonValue>> MakeStringArray(std::initializer_list<FString> Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	TSharedPtr<FJsonObject> MakeSelector(const FString& Key, const FString& Value)
	{
		TSharedPtr<FJsonObject> Selector = MakeShared<FJsonObject>();
		Selector->SetStringField(Key, Value);
		return Selector;
	}

	TSharedPtr<FJsonObject> MakeAvailability(const bool bEditable, const FString& Reason)
	{
		TSharedPtr<FJsonObject> Availability = MakeShared<FJsonObject>();
		Availability->SetBoolField(TEXT("editable"), bEditable);
		if (!Reason.IsEmpty())
		{
			Availability->SetStringField(TEXT("reason"), Reason);
		}
		return Availability;
	}

	FString GetReflectedPropertyCategoryPath(const FProperty* Property)
	{
		return Property ? Property->GetMetaData(TEXT("Category")) : FString();
	}

	FString GetReflectedPropertyControlType(const FProperty* Property)
	{
		if (!Property)
		{
			return TEXT("text");
		}
		if (CastField<FBoolProperty>(Property))
		{
			return TEXT("checkbox");
		}
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			return EnumProperty->GetEnum() ? TEXT("enum") : TEXT("text");
		}
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			return ByteProperty->Enum ? TEXT("enum") : TEXT("number");
		}
		if (CastField<FNumericProperty>(Property))
		{
			return TEXT("number");
		}
		if (CastField<FObjectPropertyBase>(Property) || CastField<FClassProperty>(Property) || CastField<FSoftObjectProperty>(Property) || CastField<FSoftClassProperty>(Property))
		{
			return TEXT("object_picker");
		}
		if (CastField<FArrayProperty>(Property) || CastField<FSetProperty>(Property) || CastField<FMapProperty>(Property))
		{
			return TEXT("collection");
		}
		if (CastField<FStructProperty>(Property))
		{
			return TEXT("struct");
		}
		return TEXT("text");
	}

	FString GetReflectedPropertyValueKind(const FProperty* Property)
	{
		if (!Property)
		{
			return TEXT("text");
		}
		if (CastField<FBoolProperty>(Property))
		{
			return TEXT("bool");
		}
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			return EnumProperty->GetEnum() ? TEXT("enum") : TEXT("text");
		}
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			return ByteProperty->Enum ? TEXT("enum") : TEXT("number");
		}
		if (CastField<FNumericProperty>(Property))
		{
			return TEXT("number");
		}
		if (CastField<FObjectPropertyBase>(Property) || CastField<FClassProperty>(Property) || CastField<FSoftObjectProperty>(Property) || CastField<FSoftClassProperty>(Property))
		{
			return TEXT("asset_path");
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
		if (CastField<FStructProperty>(Property))
		{
			return TEXT("struct");
		}
		return TEXT("text");
	}

	const UEnum* GetReflectedPropertyEnum(const FProperty* Property)
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

	TArray<TSharedPtr<FJsonValue>> MakeEnumCandidates(const UEnum* Enum)
	{
		TArray<TSharedPtr<FJsonValue>> Candidates;
		if (!Enum)
		{
			return Candidates;
		}
		for (int32 Index = 0; Index < Enum->NumEnums(); ++Index)
		{
			if (Enum->HasMetaData(TEXT("Hidden"), Index))
			{
				continue;
			}
			const FString Name = Enum->GetNameStringByIndex(Index);
			if (Name.EndsWith(TEXT("_MAX")))
			{
				continue;
			}
			TSharedPtr<FJsonObject> Candidate = MakeShared<FJsonObject>();
			Candidate->SetStringField(TEXT("name"), Name);
			Candidate->SetNumberField(TEXT("value"), static_cast<double>(Enum->GetValueByIndex(Index)));
			const FString DisplayName = Enum->GetDisplayNameTextByIndex(Index).ToString();
			if (!DisplayName.IsEmpty())
			{
				Candidate->SetStringField(TEXT("display_name"), DisplayName);
			}
			const FString Tooltip = Enum->GetMetaData(TEXT("ToolTip"), Index);
			if (!Tooltip.IsEmpty())
			{
				Candidate->SetStringField(TEXT("tooltip"), Tooltip);
			}
			Candidates.Add(MakeShared<FJsonValueObject>(Candidate));
		}
		return Candidates;
	}

	void ApplyAuthoringMetadata(
		const TSharedPtr<FJsonObject>& Obj,
		const FString& SelectorKey,
		const FString& SelectorValue,
		const FString& DisplayName,
		const FString& CategoryPath,
		const FString& ControlType,
		const FString& ValueKind,
		std::initializer_list<FString> Operations,
		const bool bEditable)
	{
		if (!Obj.IsValid())
		{
			return;
		}

		Obj->SetObjectField(TEXT("selector"), MakeSelector(SelectorKey, SelectorValue));
		if (!DisplayName.IsEmpty())
		{
			Obj->SetStringField(TEXT("display_name"), DisplayName);
		}
		if (!CategoryPath.IsEmpty())
		{
			Obj->SetStringField(TEXT("category_path"), CategoryPath);
		}
		Obj->SetStringField(TEXT("control_type"), ControlType);
		Obj->SetStringField(TEXT("value_kind"), ValueKind);
		Obj->SetObjectField(TEXT("availability"), MakeAvailability(bEditable));
		Obj->SetArrayField(TEXT("operations"), MakeStringArray(Operations));
	}

	void ApplyPropertyMetadata(
		const TSharedPtr<FJsonObject>& Obj,
		const FString& PropertyPath,
		const FString& DisplayName,
		const FString& CategoryPath,
		const FString& ValueKind)
	{
		ApplyAuthoringMetadata(
			Obj,
			TEXT("property_path"),
			PropertyPath,
			DisplayName.IsEmpty() ? PropertyPath : DisplayName,
			CategoryPath,
			TEXT("property_text"),
			ValueKind,
			{ TEXT("set") });
	}

	void ApplyReflectedPropertyMetadata(const TSharedPtr<FJsonObject>& Obj, const FProperty* Property, const FString& PropertyPath)
	{
		if (!Obj.IsValid() || !Property)
		{
			return;
		}

		const FString DisplayName = Property->GetDisplayNameText().ToString();
		Obj->SetStringField(TEXT("property_path"), PropertyPath);
		ApplyAuthoringMetadata(
			Obj,
			TEXT("property_path"),
			PropertyPath,
			DisplayName.IsEmpty() ? PropertyPath : DisplayName,
			GetReflectedPropertyCategoryPath(Property),
			GetReflectedPropertyControlType(Property),
			GetReflectedPropertyValueKind(Property),
			{ TEXT("set") },
			Property->HasAnyPropertyFlags(CPF_Edit));

		const FString Tooltip = Property->GetToolTipText().ToString();
		if (!Tooltip.IsEmpty())
		{
			Obj->SetStringField(TEXT("tooltip"), Tooltip);
		}
		if (const UEnum* Enum = GetReflectedPropertyEnum(Property))
		{
			Obj->SetArrayField(TEXT("candidates"), MakeEnumCandidates(Enum));
		}
	}
}
