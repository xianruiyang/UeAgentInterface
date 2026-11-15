// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringReflectionDescriptors.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"

#include "Containers/AnsiString.h"
#include "Containers/Utf8String.h"
#include "Components/SplineComponent.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/CurveVector.h"
#include "Curves/RichCurve.h"
#include "Curves/SimpleCurve.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameplayTagContainer.h"
#include "Metadata/PCGAttributePropertySelector.h"
#include "StructUtils/InstancedStruct.h"
#include "StructUtils/PropertyBag.h"
#include "UObject/AnsiStrProperty.h"
#include "UObject/Class.h"
#include "UObject/PropertyOptional.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Utf8StrProperty.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Misc/AutomationTest.h"
#endif

namespace UeAgentAuthoringTypeDescriptors
{
bool IsStructMemberAuthoringEditable(const FProperty *Property, const UScriptStruct* Scope) {
  return UeAgentAuthoringReflection::IsEditableDeclaration(Property, Scope);
}

bool RequiresHashSource(const FProperty *Property) {
  TSet<const UScriptStruct*> Visited;
  TFunction<bool(const FProperty*)> Requires = [&](const FProperty* Field) {
    if (!Field) return false;
    if (const auto* Array = CastField<FArrayProperty>(Field)) return Requires(Array->Inner);
    if (const auto* Set = CastField<FSetProperty>(Field)) return Requires(Set->ElementProp);
    if (const auto* Map = CastField<FMapProperty>(Field)) return Requires(Map->KeyProp) || Requires(Map->ValueProp);
    if (const auto* Optional = CastField<FOptionalProperty>(Field)) return Requires(Optional->GetValueProperty());
    const auto* Struct = CastField<FStructProperty>(Field);
    if (!Struct || !Struct->Struct) return false;
    const FString Kind = GetStructKind(Struct->Struct);
    if (Kind == TEXT("instanced_struct") || Kind == TEXT("property_bag")) return true;
    if (Kind != TEXT("struct") || Visited.Contains(Struct->Struct)) return false;
    Visited.Add(Struct->Struct);
    for (TFieldIterator<FProperty> It(Struct->Struct, EFieldIterationFlags::IncludeSuper); It; ++It)
      if (!IsStructMemberAuthoringEditable(*It, Struct->Struct) || Requires(*It)) return true;
    return false;
  };
  return Requires(Property);
}


	bool TryResolveEnumValueName(const UEnum* Enum, const FString& Name, int64& OutValue, bool bCheckAuthoredName)
	{
		if (!Enum) return false;
		int32 Index = bCheckAuthoredName ? Enum->GetIndexByNameString(Name, EGetByNameFlags::CheckAuthoredName) : INDEX_NONE;
		if (Index == INDEX_NONE) Index = Enum->GetIndexByNameString(Name);
		if (Index == INDEX_NONE) return false;
		OutValue = Enum->GetValueByIndex(Index);
		return true;
	}

	bool IsEnumValueEntry(const UEnum* Enum, int32 Index)
	{
		if (!Enum || Index < 0 || Index >= Enum->NumEnums()) return false;
		const FString Name = Enum->GetNameStringByIndex(Index);
		if (Name.IsEmpty()) return false;
		if (!Name.EndsWith(TEXT("MAX"), ESearchCase::CaseSensitive)) return true;
		// Match the qualified sentinel identities recognized by UEnum::ContainsExistingMax.
		const FString Qualified = Enum->GetNameByIndex(Index).ToString();
		return !Qualified.Equals(Enum->GenerateFullEnumName(TEXT("MAX")), ESearchCase::CaseSensitive)
			&& !Qualified.Equals(Enum->GenerateFullEnumName(*(Enum->GenerateEnumPrefix() + TEXT("_MAX"))), ESearchCase::CaseSensitive);
	}

	uint64 GetIntegerStorageMask(const FProperty* Property)
	{
		const auto* Numeric = CastField<FNumericProperty>(Property);
		if (!Numeric || !Numeric->IsInteger()) return 0;
		const int32 Bits = Numeric->GetElementSize() * 8;
		return Bits == 64 ? MAX_uint64 : Bits > 0 && Bits < 64 ? (uint64(1) << Bits) - 1 : 0;
	}

	bool TryGetBitmaskMask(const FProperty* Property, const UEnum* Enum, int64 EnumValue, uint64& OutMask)
	{
		const uint64 StorageMask = GetIntegerStorageMask(Property);
		if (!StorageMask || !Enum) return false;
		const FProperty* Declaration = Property->GetOwnerProperty();
		const bool bRawMask = Declaration->GetBoolMetaData(TEXT("UseEnumValuesAsMaskValuesInEditor"))
			|| Enum->GetBoolMetaData(TEXT("UseEnumValuesAsMaskValuesInEditor"));
		const int32 Bits = Property->GetElementSize() * 8;
		if (!bRawMask)
		{
			if (EnumValue < 0 || EnumValue >= Bits) return false;
			OutMask = uint64(1) << EnumValue;
			return true;
		}
		// HJSON preserves declared composite masks as well as individual flags.
		// Negative raw masks use the target width's two's-complement bits.
		if ((EnumValue >= 0 && static_cast<uint64>(EnumValue) > StorageMask)
			|| (EnumValue < 0 && Bits < 64 && EnumValue < -(int64(1) << (Bits - 1)))) return false;
		OutMask = static_cast<uint64>(EnumValue) & StorageMask;
		return true;
	}

	static void VisitEnumCandidates(const UEnum* Enum, const FProperty* Property,
		TFunctionRef<void(int32, const FString&)> Visitor)
	{
		if (!Enum) return;
		for (int32 Index = 0; Index < Enum->NumEnums(); ++Index) {
			if (!IsEnumValueEntry(Enum, Index) || Enum->HasMetaData(TEXT("Hidden"), Index)) continue;
			uint64 Mask = 0;
			if (IsBitmaskProperty(Property) && !TryGetBitmaskMask(Property, Enum, Enum->GetValueByIndex(Index), Mask)) continue;
			Visitor(Index, Enum->GetNameStringByIndex(Index));
		}
	}

	TArray<FString> ReadEnumCandidateNames(const UEnum* Enum, const FProperty* Property)
	{
		TArray<FString> Names;
		VisitEnumCandidates(Enum, Property, [&](int32, const FString& Name) { Names.Add(Name); });
		return Names;
	}

	TSharedPtr<FJsonObject> BuildEnumCandidates(const UEnum* Enum, bool bIncludeDisplayMetadata, const FProperty* Property)
	{
		if (!Enum) return nullptr;
		struct FCandidate { FString Name, DisplayName, Tooltip; };
		TArray<FCandidate> Candidates;
		bool bDisplay = false, bTooltip = false;
		VisitEnumCandidates(Enum, Property, [&](int32 Index, const FString& Name) {
			const FString Display = bIncludeDisplayMetadata ? Enum->GetDisplayNameTextByIndex(Index).ToString() : FString();
			const FString Tooltip = bIncludeDisplayMetadata ? Enum->GetMetaData(TEXT("ToolTip"), Index) : FString();
			bDisplay |= !Display.IsEmpty() && Display != Name; bTooltip |= !Tooltip.IsEmpty();
			Candidates.Add({Name, Display, Tooltip});
		});
		if (Candidates.IsEmpty()) return nullptr;
		TArray<TSharedPtr<FJsonValue>> Header{MakeShared<FJsonValueString>(TEXT("name"))}, Rows;
		if (bDisplay) Header.Add(MakeShared<FJsonValueString>(TEXT("display_name")));
		if (bTooltip) Header.Add(MakeShared<FJsonValueString>(TEXT("tooltip")));
		for (const auto& Candidate : Candidates)
		{
			TArray<TSharedPtr<FJsonValue>> Row{MakeShared<FJsonValueString>(Candidate.Name)};
			if (bDisplay) Row.Add(MakeShared<FJsonValueString>(Candidate.DisplayName.IsEmpty() ? Candidate.Name : Candidate.DisplayName));
			if (bTooltip) Row.Add(MakeShared<FJsonValueString>(Candidate.Tooltip));
			Rows.Add(MakeShared<FJsonValueArray>(MoveTemp(Row)));
		}
		const auto Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("candidates_header"), Header); Result->SetArrayField(TEXT("candidates"), Rows);
		return Result;
	}

	bool IsDirectPropertyValueWrapper(const FString& FieldName)
	{
		static const TSet<FString> DirectWrappers{
			TEXT("literal"), TEXT("selected"), TEXT("items"),
			TEXT("components"), TEXT("path"), TEXT("value")};
		return DirectWrappers.Contains(FieldName);
	}

	bool IsReadOnlyTypeMetadataField(const FString& FieldName)
	{
		return FieldName == TEXT("value_semantics");
	}

	FString NativeCodecValueWrapper(const FString& Kind)
	{
		static const TSet<FString> Literals{
			TEXT("bool"), TEXT("int8"), TEXT("int16"), TEXT("int32"), TEXT("int64"),
			TEXT("uint8"), TEXT("uint16"), TEXT("uint32"), TEXT("uint64"), TEXT("float"), TEXT("double"),
			TEXT("string"), TEXT("name"), TEXT("text"), TEXT("gameplay_tag"), TEXT("pcg_selector")};
		static const TSet<FString> References{
			TEXT("object_ref"), TEXT("class_ref"), TEXT("soft_object_ref"), TEXT("soft_class_ref"),
			TEXT("weak_object_ref"), TEXT("lazy_object_ref"), TEXT("interface_ref")};
		static const TSet<FString> Components{
			TEXT("vector2"), TEXT("vector3"), TEXT("vector4"), TEXT("quat"), TEXT("rotator"),
			TEXT("color8"), TEXT("linear_color")};
		if (Literals.Contains(Kind)) return TEXT("literal");
		if (Kind == TEXT("enum") || Kind == TEXT("bitmask")) return TEXT("selected");
		if (Kind == TEXT("array") || Kind == TEXT("fixed_array") || Kind == TEXT("set") || Kind == TEXT("gameplay_tags")) return TEXT("items");
		if (References.Contains(Kind)) return TEXT("path");
		if (Components.Contains(Kind)) return TEXT("components");
		return FString();
	}

	static bool MayContainNativeObjectReferenceImpl(const UE::UAI::StateSemantic::FUeAgentCanonicalValue& Descriptor,
		TFunctionRef<bool(const UE::UAI::StateSemantic::FUeAgentCanonicalValue&)> ReferenceLeaf)
	{
		using namespace UE::UAI::StateSemantic;
		if (Descriptor.GetKind() != EUeAgentValueKind::Object) return true;
		const auto Field = [&](const TCHAR* Name) -> const FUeAgentCanonicalValue* {
			const auto* Entry = Descriptor.GetObjectItems().FindByPredicate([&](const auto& Item) { return Item.Key == Name; });
			return Entry ? &Entry->Value.Get() : nullptr;
		};
		const auto* KindValue = Field(TEXT("kind"));
		if (!KindValue || KindValue->GetKind() != EUeAgentValueKind::String) return true;
		const FString& Kind = KindValue->GetText();
		const FString Wrapper = NativeCodecValueWrapper(Kind);
		if (Wrapper == TEXT("literal") || Wrapper == TEXT("selected") || Wrapper == TEXT("components")
			|| Kind == TEXT("gameplay_tags")) return false;
		if (Wrapper == TEXT("path")) return ReferenceLeaf(Descriptor);
		const auto NestedMayReference = [&](const TCHAR* Name) {
			const auto* Nested = Field(Name);
			return !Nested || MayContainNativeObjectReferenceImpl(*Nested, ReferenceLeaf);
		};
		if (Kind == TEXT("array") || Kind == TEXT("fixed_array") || Kind == TEXT("set"))
			return NestedMayReference(TEXT("element_type"));
		if (Kind == TEXT("map")) return NestedMayReference(TEXT("key_type")) || NestedMayReference(TEXT("mapped_type"));
		if (Kind == TEXT("optional")) return NestedMayReference(TEXT("inner_type"));
		return true;
	}
	bool MayContainNativeObjectReference(const UE::UAI::StateSemantic::FUeAgentCanonicalValue& Descriptor)
	{
		return MayContainNativeObjectReferenceImpl(Descriptor, [](const auto&) { return true; });
	}
	bool MayContainNativeObjectReferenceTo(const UE::UAI::StateSemantic::FUeAgentCanonicalValue& Descriptor,
		TConstArrayView<const UObject*> Objects)
	{
		using namespace UE::UAI::StateSemantic;
		if (Objects.IsEmpty()) return MayContainNativeObjectReference(Descriptor);
		return MayContainNativeObjectReferenceImpl(Descriptor, [&](const FUeAgentCanonicalValue& Leaf) {
			const auto Read = [&](const TCHAR* Name) {
				const auto* Field = Leaf.GetObjectItems().FindByPredicate([&](const auto& Item) { return Item.Key == Name; });
				return Field && Field->Value->GetKind() == EUeAgentValueKind::String ? Field->Value->GetText() : FString();
			};
			const FString Kind = Read(TEXT("kind"));
			const bool bClassReference = Kind == TEXT("class_ref") || Kind == TEXT("soft_class_ref");
			const FString AllowedPath = Read(TEXT("allowed_class"));
			const UClass* Allowed = AllowedPath.IsEmpty() ? nullptr : FindObject<UClass>(nullptr, *AllowedPath);
			for (const UObject* Object : Objects) {
				if (!Object) return true; // A missing actual target supplies no exclusion proof.
				if (bClassReference) {
					const UClass* Class = Cast<UClass>(Object);
					if (Class && (!Allowed || Class->IsChildOf(Allowed))) return true;
				} else if (!Allowed || (Kind == TEXT("interface_ref")
					? Object->GetClass()->ImplementsInterface(Allowed) : Object->IsA(Allowed))) return true;
			}
			return false;
		});
	}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentNativeReferenceTargetTypeTest,
	"UeAgentInterface.Core.AuthoringTypes.NativeReferenceTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentNativeReferenceTargetTypeTest::RunTest(const FString&)
{
	using namespace UE::UAI::StateSemantic;
	UE::UAI::ContextResource::FUeAgentResourceTracker Resources;
	const UObject* Removed = USplineComponent::StaticClass()->GetDefaultObject();
	const TArray<const UObject*> Instances{Removed};
	const auto Check = [&](const TCHAR* Label, const FDescriptor& Type, bool Expected,
		TConstArrayView<const UObject*> Targets) {
		FUeAgentCanonicalValuePtr Canonical; UE::UAI::Contract::FDiagnostic Error;
		if (!CanonicalJson::TryFromJsonValue(MakeShared<FJsonValueObject>(Type.ToJsonObject()), Resources, Canonical, Error)) {
			AddError(Error.PublicMessage); return;
		}
		TestEqual(Label, MayContainNativeObjectReferenceTo(*Canonical, Targets), Expected);
	};
	FDescriptor Scene; Scene.Kind = TEXT("object_ref"); Scene.AllowedClassPath = USceneComponent::StaticClass()->GetPathName();
	Check(TEXT("compatible component references remain candidates"), Scene, true, Instances);
	FDescriptor Curve; Curve.Kind = TEXT("object_ref"); Curve.AllowedClassPath = UCurveFloat::StaticClass()->GetPathName();
	Check(TEXT("curve asset fields cannot reference a removed component"), Curve, false, Instances);
	FDescriptor Array; Array.Kind = TEXT("array"); Array.ElementType = MakeShared<FDescriptor>(Curve);
	Check(TEXT("same original container traversal excludes incompatible element classes"), Array, false, Instances);
	FDescriptor Class = Scene; Class.Kind = TEXT("class_ref");
	Check(TEXT("class values do not reference component instances"), Class, false, Instances);
	const TArray<const UObject*> Classes{USplineComponent::StaticClass()};
	Check(TEXT("real class targets retain the original metaclass semantics"), Class, true, Classes);
	FDescriptor Unknown = Scene; Unknown.AllowedClassPath = TEXT("/Script/Unknown.NotLoaded");
	Check(TEXT("unknown allowed class is not an exclusion proof"), Unknown, true, Instances);
	FDescriptor Struct; Struct.Kind = TEXT("struct"); Struct.StructPath = TEXT("/Script/Unknown.Struct");
	Check(TEXT("unknown struct keeps its actual value consumer"), Struct, true, Instances);
	return !HasAnyErrors();
}
#endif

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
			case EJson::Number: return UE::UAI::StateSemantic::CanonicalJson::AreNumbersEquivalent(*Left, *Right);
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
				if (!A.IsValid() || !B.IsValid() || B->Values.Num() > A->Values.Num()) return false;
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : A->Values)
				{
					if (IsReadOnlyTypeMetadataField(Pair.Key) && A->HasField(TEXT("kind"))) continue;
					FString Kind;
					if (Pair.Key == TEXT("struct_path") && !B->HasField(Pair.Key)
						&& A->TryGetStringField(TEXT("kind"), Kind) && Kind != TEXT("struct") && Kind != TEXT("pcg_selector")) continue;
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

		bool IsNativeRotator(const UStruct* Struct)
		{
			return Struct && (Struct == TBaseStructure<FRotator>::Get()
				|| Struct == TVariantStructure<FRotator3f>::Get() || Struct == TVariantStructure<FRotator3d>::Get());
		}

		TSharedPtr<FJsonObject> DescribeStructValueSemantics(const UScriptStruct* Struct, const FString& Kind)
		{
			const bool bReflectedRotatorFields = Kind == TEXT("struct") && Struct
				&& (Struct->IsChildOf(TBaseStructure<FRotator>::Get())
					|| Struct->IsChildOf(TVariantStructure<FRotator3f>::Get()) || Struct->IsChildOf(TVariantStructure<FRotator3d>::Get()));
			if (!bReflectedRotatorFields && !IsNativeRotator(Struct) && Kind != TEXT("transform")) return nullptr;
			const auto Rotation = MakeShared<FJsonObject>();
			if (bReflectedRotatorFields)
			{
				// Explicit Rotator3d has historically used reflected named fields.
				// In derived structs only fields declared by the native Rotator inherit
				// its unit; newly declared or shadowing fields retain their own contract.
				const auto Fields = MakeShared<FJsonObject>();
				for (const TCHAR* Name : {TEXT("Pitch"), TEXT("Yaw"), TEXT("Roll")})
					if (const FProperty* Field = FindFProperty<FProperty>(Struct, Name);
						Field && IsNativeRotator(Field->GetOwnerStruct()))
						Fields->SetStringField(Name, TEXT("degrees"));
				if (Fields->Values.IsEmpty()) return nullptr;
				Rotation->SetObjectField(TEXT("field_units"), Fields);
				return Rotation;
			}
			Rotation->SetStringField(TEXT("unit"), TEXT("degrees"));
			TArray<TSharedPtr<FJsonValue>> Components;
			for (const TCHAR* Name : {TEXT("pitch"), TEXT("yaw"), TEXT("roll")})
				Components.Add(MakeShared<FJsonValueString>(Name));
			Rotation->SetArrayField(TEXT("component_order"), Components);
			if (Kind != TEXT("transform")) return Rotation;
			// The codec exposes Transform::Rotator(), not its stored quaternion.
			const auto Transform = MakeShared<FJsonObject>();
			Transform->SetObjectField(TEXT("rotation"), Rotation);
			return Transform;
		}
	}

	bool FDescriptor::operator==(const FDescriptor& Other) const
	{
		return Kind == Other.Kind
			&& StructPath == Other.StructPath
			&& EnumPath == Other.EnumPath
			&& AllowedClassPath == Other.AllowedClassPath
			&& FixedLength == Other.FixedLength
			&& bRequiresHashSource == Other.bRequiresHashSource
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
		if (bRequiresHashSource) Object->SetStringField(TEXT("hash_source"), TEXT("from_required_null_creates"));
		if (ValueSemantics) Object->SetObjectField(TEXT("value_semantics"), ValueSemantics);
		SetNestedType(TEXT("element_type"), ElementType, Object);
		SetNestedType(TEXT("key_type"), KeyType, Object);
		SetNestedType(TEXT("mapped_type"), MappedType, Object);
		SetNestedType(TEXT("inner_type"), InnerType, Object);
		return Object;
	}

	FString GetStructKind(const UScriptStruct* Struct)
	{
		if (!Struct) return TEXT("struct");
		if (Struct->IsChildOf(FPCGAttributePropertySelector::StaticStruct())) return TEXT("pcg_selector");
		// Native layouts are identified by their reflected type, never a short name.
		// An unrelated struct with the same name must retain recursive field encoding.
		if (Struct == FSplineCurves::StaticStruct()) return TEXT("spline");
		if (Struct == TBaseStructure<FVector2D>::Get() || Struct == TVariantStructure<FVector2f>::Get()) return TEXT("vector2");
		if (Struct == TBaseStructure<FVector>::Get() || Struct == TVariantStructure<FVector3f>::Get()) return TEXT("vector3");
		if (Struct == TBaseStructure<FVector4>::Get() || Struct == TVariantStructure<FVector4f>::Get()) return TEXT("vector4");
		if (Struct == TBaseStructure<FRotator>::Get() || Struct == TVariantStructure<FRotator3f>::Get()) return TEXT("rotator");
		if (Struct == TBaseStructure<FQuat>::Get() || Struct == TVariantStructure<FQuat4f>::Get()) return TEXT("quat");
		if (Struct == TBaseStructure<FColor>::Get()) return TEXT("color8");
		if (Struct == TBaseStructure<FLinearColor>::Get()) return TEXT("linear_color");
		if (Struct == TBaseStructure<FTransform>::Get() || Struct == TVariantStructure<FTransform3f>::Get()) return TEXT("transform");
		if (Struct == TBaseStructure<FGuid>::Get()) return TEXT("guid");
		if (Struct == TBaseStructure<FDateTime>::Get()) return TEXT("datetime");
		// Timespan has no TBaseStructure specialization in UE 5.6.
		if (Struct->GetPathName().Equals(TEXT("/Script/CoreUObject.Timespan"), ESearchCase::CaseSensitive)) return TEXT("timespan");
		if (Struct == TBaseStructure<FSoftObjectPath>::Get()) return TEXT("soft_object_ref");
		if (Struct == TBaseStructure<FSoftClassPath>::Get()) return TEXT("soft_class_ref");
		if (Struct == FGameplayTag::StaticStruct()) return TEXT("gameplay_tag");
		if (Struct == FGameplayTagContainer::StaticStruct()) return TEXT("gameplay_tags");
		if (Struct == FInstancedStruct::StaticStruct()) return TEXT("instanced_struct");
		if (Struct == FInstancedPropertyBag::StaticStruct()) return TEXT("property_bag");
		if (Struct == FRichCurve::StaticStruct() || Struct == FSimpleCurve::StaticStruct() || Struct == FRuntimeFloatCurve::StaticStruct()
			|| Struct == FRuntimeVectorCurve::StaticStruct() || Struct == FRuntimeCurveLinearColor::StaticStruct()) return TEXT("curve");
		return TEXT("struct");
	}

	bool IsBitmaskProperty(const FProperty* Property)
	{
		return GetIntegerStorageMask(Property) && Property->GetOwnerProperty()->HasMetaData(TEXT("Bitmask"));
	}

	const UEnum* GetPropertyEnum(const FProperty* Property)
	{
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property)) return EnumProperty->GetEnum();
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property); ByteProperty && ByteProperty->Enum) return ByteProperty->Enum;
		if (IsBitmaskProperty(Property))
		{
			const FString EnumPath = Property->GetOwnerProperty()->GetMetaData(TEXT("BitmaskEnum"));
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
		else if (const FSetProperty* Set = CastField<FSetProperty>(Property))
		{
			Result.ElementType = MakeShared<FDescriptor>(Describe(Set->ElementProp));
			Result.bRequiresHashSource = RequiresHashSource(Set->ElementProp);
		}
		else if (const FMapProperty* Map = CastField<FMapProperty>(Property))
		{
			Result.KeyType = MakeShared<FDescriptor>(Describe(Map->KeyProp));
			Result.MappedType = MakeShared<FDescriptor>(Describe(Map->ValueProp));
			Result.bRequiresHashSource = RequiresHashSource(Map->KeyProp);
		}
		else if (const FOptionalProperty* Optional = CastField<FOptionalProperty>(Property)) Result.InnerType = MakeShared<FDescriptor>(Describe(Optional->GetValueProperty()));
		else if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
		{
			// Specialized shapes still have a native storage type (for example
			// Vector and Vector3f). Keep it in the same descriptor as ordinary structs.
			Result = DescribeStruct(Struct->Struct);
		}
		else if (const FSoftClassProperty* SoftClass = CastField<FSoftClassProperty>(Property)) Result.AllowedClassPath = SoftClass->MetaClass ? SoftClass->MetaClass->GetPathName() : FString();
		else if (const FSoftObjectProperty* SoftObject = CastField<FSoftObjectProperty>(Property)) Result.AllowedClassPath = SoftObject->PropertyClass ? SoftObject->PropertyClass->GetPathName() : FString();
		else if (const FClassProperty* Class = CastField<FClassProperty>(Property)) Result.AllowedClassPath = Class->MetaClass ? Class->MetaClass->GetPathName() : FString();
		else if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(Property)) Result.AllowedClassPath = Object->PropertyClass ? Object->PropertyClass->GetPathName() : FString();
		else if (const FInterfaceProperty* Interface = CastField<FInterfaceProperty>(Property)) Result.AllowedClassPath = Interface->InterfaceClass ? Interface->InterfaceClass->GetPathName() : FString();
		if (const UEnum* Enum = GetPropertyEnum(Property)) Result.EnumPath = Enum->GetPathName();
		return Result;
	}

	FDescriptor DescribeStruct(const UScriptStruct* Struct)
	{
		FDescriptor Result;
		Result.Kind = GetStructKind(Struct);
		Result.StructPath = Struct ? Struct->GetPathName() : FString();
		Result.ValueSemantics = DescribeStructValueSemantics(Struct, Result.Kind);
		return Result;
	}

	TSharedPtr<FJsonObject> BuildJson(const FProperty* Property, bool bIgnoreFixedArray)
	{
		return Describe(Property, bIgnoreFixedArray).ToJsonObject();
	}

	bool ValidateMetadata(const FProperty* Property, const TSharedPtr<FJsonObject>& Descriptor, UE::UAI::Contract::FDiagnostic& OutError)
	{
		return ValidateMetadata(Property, Descriptor, OutError, false);
	}

	bool ValidateMetadata(const FProperty* Property, const TSharedPtr<FJsonObject>& Descriptor,
		UE::UAI::Contract::FDiagnostic& OutError, bool bSingleFixedElement)
	{
		if (!Property || !Descriptor.IsValid())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("invalid_type_descriptor"),
				!Property ? TEXT("The native property needed to validate the type descriptor is unavailable.")
					: TEXT("The declared property type must be an object."), TEXT("type_descriptor"));
			if (!Property) OutError.SourceKind = TEXT("native_result");
			else { OutError.ActualValuePresent = false; OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object")); }
			return false;
		}
		const FDescriptor ExpectedDescriptor = Describe(Property, bSingleFixedElement);
		FString DeclaredKind;
		if (!Descriptor->TryGetStringField(TEXT("kind"), DeclaredKind))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("missing_kind"),
				TEXT("The type descriptor requires a string kind field."), TEXT("type_descriptor"));
			OutError.PropertyPath = TEXT("kind");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(ExpectedDescriptor.Kind);
			OutError.ActualValue = Descriptor->TryGetField(TEXT("kind"));
			OutError.ActualValuePresent = OutError.ActualValue.IsValid();
			return false;
		}
		if (DeclaredKind != ExpectedDescriptor.Kind)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(FString::Printf(TEXT("kind_mismatch_expected_%s"), *ExpectedDescriptor.Kind), TEXT("The declared kind does not match the native property kind."), TEXT("type_descriptor"));
			OutError.ExpectedValue = MakeShared<FJsonValueString>(ExpectedDescriptor.Kind);
			OutError.ActualValue = MakeShared<FJsonValueString>(DeclaredKind);
			OutError.PropertyPath = TEXT("kind");
			return false;
		}

		const TSharedPtr<FJsonObject> Expected = ExpectedDescriptor.ToJsonObject();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Expected->Values)
		{
			// Explanatory output is optional on old inputs and never controls decoding.
			if (IsReadOnlyTypeMetadataField(Pair.Key)) continue;
			const TSharedPtr<FJsonValue> Actual = Descriptor->TryGetField(Pair.Key);
			if (!Actual.IsValid())
			{
				// Existing workspaces omitted native identity for specialized shapes.
				// Their values are decoded against the current reflected property;
				// an explicitly supplied identity must still match below.
				if (Pair.Key == TEXT("struct_path") && ExpectedDescriptor.Kind != TEXT("struct")
					&& ExpectedDescriptor.Kind != TEXT("pcg_selector")) continue;
				OutError = UE::UAI::Contract::FDiagnostic::Failure(FString::Printf(TEXT("missing_type_metadata_%s"), *Pair.Key), TEXT("Required type metadata is missing."), TEXT("type_descriptor"));
				OutError.PropertyPath = Pair.Key;
				OutError.ExpectedValue = Pair.Value;
				OutError.ActualValue = Actual;
				OutError.ActualValuePresent = false;
				return false;
			}
			if (!JsonEquals(Pair.Value, Actual))
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(FString::Printf(TEXT("type_metadata_mismatch_%s"), *Pair.Key), TEXT("The declared type metadata does not match the native property."), TEXT("type_descriptor"));
				OutError.PropertyPath = Pair.Key;
				OutError.ExpectedValue = Pair.Value;
				OutError.ActualValue = Actual;
				return false;
			}
		}
		OutError.Reset();
		return true;
	}
}
