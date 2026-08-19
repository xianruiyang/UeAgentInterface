// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FProperty;
class UEnum;
class UScriptStruct;

namespace UeAgentAuthoringTypeDescriptors
{
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

		bool operator==(const FDescriptor& Other) const;
		bool operator!=(const FDescriptor& Other) const { return !(*this == Other); }
		TSharedPtr<FJsonObject> ToJsonObject() const;
	};

	UEAGENTINTERFACE_API FString GetStructKind(const UScriptStruct* Struct);
	UEAGENTINTERFACE_API bool IsBitmaskProperty(const FProperty* Property);
	UEAGENTINTERFACE_API const UEnum* GetPropertyEnum(const FProperty* Property);
	UEAGENTINTERFACE_API FString GetValueKind(const FProperty* Property, bool bIgnoreFixedArray = false);
	UEAGENTINTERFACE_API FDescriptor Describe(const FProperty* Property, bool bIgnoreFixedArray = false);
	UEAGENTINTERFACE_API TSharedPtr<FJsonObject> BuildJson(const FProperty* Property, bool bIgnoreFixedArray = false);
	UEAGENTINTERFACE_API bool ValidateMetadata(
		const FProperty* Property,
		const TSharedPtr<FJsonObject>& Descriptor,
		FString& OutError);
}
