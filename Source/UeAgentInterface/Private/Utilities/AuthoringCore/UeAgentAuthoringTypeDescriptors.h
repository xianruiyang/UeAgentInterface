// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include <type_traits>

class FJsonObject;
class FProperty;
class UEnum;
class UScriptStruct;

namespace UeAgentAuthoringTypeDescriptors
{
	/** Field names allowed only at a proven semantic-unit or native-codec envelope boundary. */
	UEAGENTINTERFACE_API bool IsDirectPropertyValueWrapper(const FString& FieldName);
	/** Native codec envelope declared by a formal type kind; empty means no compact envelope. */
	UEAGENTINTERFACE_API FString NativeCodecValueWrapper(const FString& Kind);
	/** Explanation produced with a type, never an assertion or decoding input. */
	UEAGENTINTERFACE_API bool IsReadOnlyTypeMetadataField(const FString& FieldName);
	UEAGENTINTERFACE_API bool IsStructMemberAuthoringEditable(const FProperty* Property, const UScriptStruct* Scope = nullptr);
	/** Type-level source requirement, including empty/open dynamic values. */
	UEAGENTINTERFACE_API bool RequiresHashSource(const FProperty* Property);
	struct UEAGENTINTERFACE_API FDescriptor
	{
		FString Kind;
		FString StructPath;
		FString EnumPath;
		FString AllowedClassPath;
		int32 FixedLength = INDEX_NONE;
		TSharedPtr<FDescriptor> ElementType;
		TSharedPtr<FDescriptor> KeyType;
		TSharedPtr<FDescriptor> MappedType;
		TSharedPtr<FDescriptor> InnerType;
		bool bRequiresHashSource = false;
		// Read-only meaning of the codec's public value, not native storage assertions.
		TSharedPtr<FJsonObject> ValueSemantics;

		bool operator==(const FDescriptor& Other) const;
		bool operator!=(const FDescriptor& Other) const { return !(*this == Other); }
		TSharedPtr<FJsonObject> ToJsonObject() const;
	};

	/** Native scalar storage outside FProperty uses the same formal type names. */
	template <typename T> FDescriptor DescribeScalar()
	{
		static_assert(std::is_integral_v<T> || std::is_same_v<T, float> || std::is_same_v<T, double>, "Scalar storage must have a formal HJSON type");
		FDescriptor Type;
		if constexpr (std::is_same_v<T, bool>) Type.Kind = TEXT("bool");
		else if constexpr (std::is_same_v<T, float>) Type.Kind = TEXT("float");
		else if constexpr (std::is_same_v<T, double>) Type.Kind = TEXT("double");
		else Type.Kind = FString::Printf(TEXT("%s%d"), std::is_signed_v<T> ? TEXT("int") : TEXT("uint"), static_cast<int32>(sizeof(T) * 8));
		return Type;
	}

	UEAGENTINTERFACE_API FString GetStructKind(const UScriptStruct* Struct);
	UEAGENTINTERFACE_API FDescriptor DescribeStruct(const UScriptStruct* Struct);
	UEAGENTINTERFACE_API bool IsBitmaskProperty(const FProperty* Property);
	UEAGENTINTERFACE_API const UEnum* GetPropertyEnum(const FProperty* Property);
	/** Lookup status is separate from the value: a declared enumerator may be -1. */
	UEAGENTINTERFACE_API bool TryResolveEnumValueName(const UEnum* Enum, const FString& Name, int64& OutValue, bool bCheckAuthoredName = false);
	/** Declared value identity, independent of position and editor Hidden metadata. */
	UEAGENTINTERFACE_API bool IsEnumValueEntry(const UEnum* Enum, int32 Index);
	UEAGENTINTERFACE_API uint64 GetIntegerStorageMask(const FProperty* Property);
	UEAGENTINTERFACE_API bool TryGetBitmaskMask(const FProperty* Property, const UEnum* Enum, int64 EnumValue, uint64& OutMask);
	/** Shared enumeration/filtering; ordinary authoring keeps names-only rows. */
	UEAGENTINTERFACE_API TSharedPtr<FJsonObject> BuildEnumCandidates(const UEnum* Enum, bool bIncludeDisplayMetadata = false, const FProperty* Property = nullptr);
	UEAGENTINTERFACE_API FString GetValueKind(const FProperty* Property, bool bIgnoreFixedArray = false);
	UEAGENTINTERFACE_API FDescriptor Describe(const FProperty* Property, bool bIgnoreFixedArray = false);
	UEAGENTINTERFACE_API TSharedPtr<FJsonObject> BuildJson(const FProperty* Property, bool bIgnoreFixedArray = false);
	UEAGENTINTERFACE_API bool ValidateMetadata(
		const FProperty* Property,
		const TSharedPtr<FJsonObject>& Descriptor,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool ValidateMetadata(
		const FProperty* Property,
		const TSharedPtr<FJsonObject>& Descriptor,
		UE::UAI::Contract::FDiagnostic& OutError, bool bSingleFixedElement);
}
