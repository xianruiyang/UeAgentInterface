// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"

#include "Containers/AnsiString.h"
#include "Containers/Utf8String.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/AnsiStrProperty.h"
#include "UObject/Class.h"
#include "UObject/PropertyOptional.h"
#include "UObject/UnrealType.h"
#include "UObject/Utf8StrProperty.h"

namespace UeAgentAuthoringTypeDescriptors
{
	namespace
	{
		bool DescriptorPtrEquals(const TSharedPtr<FDescriptor>& Left, const TSharedPtr<FDescriptor>& Right)
		{
			return Left.IsValid() == Right.IsValid() && (!Left.IsValid() || *Left == *Right);
		}

		bool JsonEquals(const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
		{
			if (!Left.IsValid() || !Right.IsValid() || Left->Type != Right->Type) return false;
			switch (Left->Type)
			{
			case EJson::Null: return true;
			case EJson::String: return Left->AsString() == Right->AsString();
			case EJson::Number: return Left->AsNumber() == Right->AsNumber();
			case EJson::Boolean: return Left->AsBool() == Right->AsBool();
			case EJson::Array:
			{
				const TArray<TSharedPtr<FJsonValue>>& A = Left->AsArray();
				const TArray<TSharedPtr<FJsonValue>>& B = Right->AsArray();
				if (A.Num() != B.Num()) return false;
				for (int32 Index = 0; Index < A.Num(); ++Index)
				{
					if (!JsonEquals(A[Index], B[Index])) return false;
				}
				return true;
			}
			case EJson::Object:
			{
				const TSharedPtr<FJsonObject> A = Left->AsObject();
				const TSharedPtr<FJsonObject> B = Right->AsObject();
				if (!A.IsValid() || !B.IsValid() || A->Values.Num() != B->Values.Num()) return false;
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : A->Values)
				{
					if (!JsonEquals(Pair.Value, B->TryGetField(Pair.Key))) return false;
				}
				return true;
			}
			default: return false;
			}
		}

		bool IsUnsupportedProperty(const FProperty* Property)
		{
			return Property && (CastField<FDelegateProperty>(Property)
				|| CastField<FMulticastDelegateProperty>(Property)
				|| Property->GetClass()->GetName().Contains(TEXT("FieldPath"))
				|| Property->GetClass()->GetName().Contains(TEXT("Verse")));
		}

		void SetNestedType(
			const TCHAR* FieldName,
			const TSharedPtr<FDescriptor>& Type,
			const TSharedPtr<FJsonObject>& Object)
		{
			if (Type.IsValid()) Object->SetObjectField(FieldName, Type->ToJsonObject());
		}
	}

	bool FDescriptor::operator==(const FDescriptor& Other) const
	{
		return Kind == Other.Kind
			&& StructPath == Other.StructPath
			&& EnumPath == Other.EnumPath
			&& AllowedClassPath == Other.AllowedClassPath
			&& FixedLength == Other.FixedLength
			&& DescriptorPtrEquals(ElementType, Other.ElementType)
			&& DescriptorPtrEquals(KeyType, Other.KeyType)
			&& DescriptorPtrEquals(MappedType, Other.MappedType)
			&& DescriptorPtrEquals(InnerType, Other.InnerType);
	}

	TSharedPtr<FJsonObject> FDescriptor::ToJsonObject() const
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("kind"), Kind);
		if (!StructPath.IsEmpty()) Object->SetStringField(TEXT("struct_path"), StructPath);
		if (!EnumPath.IsEmpty()) Object->SetStringField(TEXT("enum_path"), EnumPath);
		if (!AllowedClassPath.IsEmpty()) Object->SetStringField(TEXT("allowed_class"), AllowedClassPath);
		if (FixedLength != INDEX_NONE) Object->SetNumberField(TEXT("fixed_length"), FixedLength);
		SetNestedType(TEXT("element_type"), ElementType, Object);
		SetNestedType(TEXT("key_type"), KeyType, Object);
		SetNestedType(TEXT("mapped_type"), MappedType, Object);
		SetNestedType(TEXT("inner_type"), InnerType, Object);
		return Object;
	}

	FString GetStructKind(const UScriptStruct* Struct)
	{
		if (!Struct) return TEXT("struct");
		const FString Name = Struct->GetName();
		if (Name == TEXT("Vector2D") || Name == TEXT("Vector2f")) return TEXT("vector2");
		if (Name == TEXT("Vector") || Name == TEXT("Vector3f")) return TEXT("vector3");
		if (Name == TEXT("Vector4") || Name == TEXT("Vector4f")) return TEXT("vector4");
		if (Name == TEXT("Rotator") || Name == TEXT("Rotator3f")) return TEXT("rotator");
		if (Name == TEXT("Quat") || Name == TEXT("Quat4f")) return TEXT("quat");
		if (Name == TEXT("Color")) return TEXT("color8");
		if (Name == TEXT("LinearColor")) return TEXT("linear_color");
		if (Name == TEXT("Transform") || Name == TEXT("Transform3f")) return TEXT("transform");
		if (Name == TEXT("Guid")) return TEXT("guid");
		if (Name == TEXT("DateTime")) return TEXT("datetime");
		if (Name == TEXT("Timespan")) return TEXT("timespan");
		if (Name == TEXT("SoftObjectPath")) return TEXT("soft_object_ref");
		if (Name == TEXT("SoftClassPath")) return TEXT("soft_class_ref");
		if (Name == TEXT("GameplayTag")) return TEXT("gameplay_tag");
		if (Name == TEXT("GameplayTagContainer")) return TEXT("gameplay_tags");
		if (Struct == FInstancedStruct::StaticStruct()) return TEXT("instanced_struct");
		if (Struct == FInstancedPropertyBag::StaticStruct()) return TEXT("property_bag");
		if (Name == TEXT("RichCurve") || Name == TEXT("SimpleCurve") || Name == TEXT("RuntimeFloatCurve")
			|| Name == TEXT("RuntimeVectorCurve") || Name == TEXT("RuntimeLinearColorCurve")) return TEXT("curve");
		return TEXT("struct");
	}

	bool IsBitmaskProperty(const FProperty* Property)
	{
		return Property && Property->HasMetaData(TEXT("Bitmask"));
	}

	const UEnum* GetPropertyEnum(const FProperty* Property)
	{
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property)) return EnumProperty->GetEnum();
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property)) return ByteProperty->Enum;
		if (IsBitmaskProperty(Property))
		{
			const FString EnumPath = Property->GetMetaData(TEXT("BitmaskEnum"));
			return EnumPath.IsEmpty()
				? nullptr
				: UClass::TryFindTypeSlow<UEnum>(EnumPath);
		}
		return nullptr;
	}

	FString GetValueKind(const FProperty* Property, bool bIgnoreFixedArray)
	{
		if (!Property) return TEXT("unknown");
		if (!bIgnoreFixedArray && Property->ArrayDim > 1) return TEXT("fixed_array");
		if (IsUnsupportedProperty(Property)) return TEXT("unavailable");
		if (CastField<FBoolProperty>(Property)) return TEXT("bool");
		if (IsBitmaskProperty(Property) && GetPropertyEnum(Property)) return TEXT("bitmask");
		if (CastField<FEnumProperty>(Property) || (CastField<FByteProperty>(Property) && CastField<FByteProperty>(Property)->Enum)) return TEXT("enum");
		if (CastField<FInt8Property>(Property)) return TEXT("int8");
		if (CastField<FInt16Property>(Property)) return TEXT("int16");
		if (CastField<FIntProperty>(Property)) return TEXT("int32");
		if (CastField<FInt64Property>(Property)) return TEXT("int64");
		if (CastField<FByteProperty>(Property)) return TEXT("uint8");
		if (CastField<FUInt16Property>(Property)) return TEXT("uint16");
		if (CastField<FUInt32Property>(Property)) return TEXT("uint32");
		if (CastField<FUInt64Property>(Property)) return TEXT("uint64");
		if (CastField<FFloatProperty>(Property)) return TEXT("float");
		if (CastField<FDoubleProperty>(Property) || Property->GetClass()->GetName() == TEXT("LargeWorldCoordinatesRealProperty")) return TEXT("double");
		if (CastField<FNameProperty>(Property)) return TEXT("name");
		if (CastField<FStrProperty>(Property) || CastField<FAnsiStrProperty>(Property) || CastField<FUtf8StrProperty>(Property)) return TEXT("string");
		if (CastField<FTextProperty>(Property)) return TEXT("text");
		if (CastField<FSoftClassProperty>(Property)) return TEXT("soft_class_ref");
		if (CastField<FSoftObjectProperty>(Property)) return TEXT("soft_object_ref");
		if (CastField<FClassProperty>(Property)) return TEXT("class_ref");
		if (CastField<FWeakObjectProperty>(Property)) return TEXT("weak_object_ref");
		if (CastField<FLazyObjectProperty>(Property)) return TEXT("lazy_object_ref");
		if (CastField<FInterfaceProperty>(Property)) return TEXT("interface_ref");
		if (CastField<FObjectPropertyBase>(Property)) return TEXT("object_ref");
		if (const FStructProperty* Struct = CastField<FStructProperty>(Property)) return GetStructKind(Struct->Struct);
		if (CastField<FArrayProperty>(Property)) return TEXT("array");
		if (CastField<FSetProperty>(Property)) return TEXT("set");
		if (CastField<FMapProperty>(Property)) return TEXT("map");
		if (CastField<FOptionalProperty>(Property)) return TEXT("optional");
		return TEXT("unavailable");
	}

	FDescriptor Describe(const FProperty* Property, bool bIgnoreFixedArray)
	{
		FDescriptor Result;
		Result.Kind = GetValueKind(Property, bIgnoreFixedArray);
		if (!Property) return Result;
		if (!bIgnoreFixedArray && Property->ArrayDim > 1)
		{
			Result.ElementType = MakeShared<FDescriptor>(Describe(Property, true));
			Result.FixedLength = Property->ArrayDim;
			return Result;
		}
		if (const FArrayProperty* Array = CastField<FArrayProperty>(Property)) Result.ElementType = MakeShared<FDescriptor>(Describe(Array->Inner));
		else if (const FSetProperty* Set = CastField<FSetProperty>(Property)) Result.ElementType = MakeShared<FDescriptor>(Describe(Set->ElementProp));
		else if (const FMapProperty* Map = CastField<FMapProperty>(Property))
		{
			Result.KeyType = MakeShared<FDescriptor>(Describe(Map->KeyProp));
			Result.MappedType = MakeShared<FDescriptor>(Describe(Map->ValueProp));
		}
		else if (const FOptionalProperty* Optional = CastField<FOptionalProperty>(Property)) Result.InnerType = MakeShared<FDescriptor>(Describe(Optional->GetValueProperty()));
		else if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
		{
			if (GetStructKind(Struct->Struct) == TEXT("struct")) Result.StructPath = Struct->Struct ? Struct->Struct->GetPathName() : FString();
		}
		else if (const FSoftClassProperty* SoftClass = CastField<FSoftClassProperty>(Property)) Result.AllowedClassPath = SoftClass->MetaClass ? SoftClass->MetaClass->GetPathName() : FString();
		else if (const FSoftObjectProperty* SoftObject = CastField<FSoftObjectProperty>(Property)) Result.AllowedClassPath = SoftObject->PropertyClass ? SoftObject->PropertyClass->GetPathName() : FString();
		else if (const FClassProperty* Class = CastField<FClassProperty>(Property)) Result.AllowedClassPath = Class->MetaClass ? Class->MetaClass->GetPathName() : FString();
		else if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(Property)) Result.AllowedClassPath = Object->PropertyClass ? Object->PropertyClass->GetPathName() : FString();
		else if (const FInterfaceProperty* Interface = CastField<FInterfaceProperty>(Property)) Result.AllowedClassPath = Interface->InterfaceClass ? Interface->InterfaceClass->GetPathName() : FString();
		if (const UEnum* Enum = GetPropertyEnum(Property)) Result.EnumPath = Enum->GetPathName();
		return Result;
	}

	TSharedPtr<FJsonObject> BuildJson(const FProperty* Property, bool bIgnoreFixedArray)
	{
		return Describe(Property, bIgnoreFixedArray).ToJsonObject();
	}

	bool ValidateMetadata(const FProperty* Property, const TSharedPtr<FJsonObject>& Descriptor, FString& OutError)
	{
		if (!Property || !Descriptor.IsValid())
		{
			OutError = TEXT("invalid_type_descriptor");
			return false;
		}
		const FDescriptor ExpectedDescriptor = Describe(Property);
		FString DeclaredKind;
		if (!Descriptor->TryGetStringField(TEXT("value_kind"), DeclaredKind))
		{
			OutError = TEXT("missing_value_kind");
			return false;
		}
		if (DeclaredKind != ExpectedDescriptor.Kind)
		{
			OutError = FString::Printf(TEXT("value_kind_mismatch_expected_%s"), *ExpectedDescriptor.Kind);
			return false;
		}

		const TSharedPtr<FJsonObject> Expected = ExpectedDescriptor.ToJsonObject();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Expected->Values)
		{
			if (Pair.Key == TEXT("kind")) continue;
			const TSharedPtr<FJsonValue> Actual = Descriptor->TryGetField(Pair.Key);
			if (!Actual.IsValid())
			{
				OutError = FString::Printf(TEXT("missing_type_metadata_%s"), *Pair.Key);
				return false;
			}
			if (!JsonEquals(Pair.Value, Actual))
			{
				OutError = FString::Printf(TEXT("type_metadata_mismatch_%s"), *Pair.Key);
				return false;
			}
		}
		OutError.Reset();
		return true;
	}
}
