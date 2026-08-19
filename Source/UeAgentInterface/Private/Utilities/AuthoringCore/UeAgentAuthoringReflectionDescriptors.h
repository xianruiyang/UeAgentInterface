// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"

class FJsonObject;
class FJsonValue;
class FProperty;
class UObject;

namespace UeAgentAuthoringReflection
{
	struct UEAGENTINTERFACE_API FDescriptor
	{
		FString PropertyName;
		FString OwnerStructPath;
		FString DisplayName;
		FString Tooltip;
		TArray<FString> CategoryPath;
		TArray<FString> PropertyFlags;
		UeAgentAuthoringTypeDescriptors::FDescriptor Type;
		TMap<FString, FString> Constraints;
		FString EditCondition;
		FString AvailabilityReason;
		bool bEditConditionHides = false;
		bool bEditableByProperty = false;
		bool bCodecSupported = false;
		bool bEditable = false;
		bool bEditorOnly = false;
		bool bTransient = false;
		bool bDeprecated = false;

		TSharedPtr<FJsonObject> MakeAvailabilityJson() const;
		TSharedPtr<FJsonObject> MakeConstraintsJson() const;
		TArray<TSharedPtr<FJsonValue>> MakeCategoryPathJson() const;
		TArray<TSharedPtr<FJsonValue>> MakePropertyFlagsJson() const;
	};

	UEAGENTINTERFACE_API bool IsEditableByFlags(const FProperty* Property, const UObject* Owner = nullptr);
	UEAGENTINTERFACE_API bool IsOwnedObjectProperty(const FProperty* Property);
	UEAGENTINTERFACE_API bool EvaluateEditCondition(
		UObject* Owner,
		const FProperty* Property,
		bool& bOutSatisfied,
		FString& OutReason);
	UEAGENTINTERFACE_API bool IsEditableProperty(
		const FProperty* Property,
		UObject* Owner = nullptr,
		FString* OutReason = nullptr);
	UEAGENTINTERFACE_API bool Build(
		const FProperty* Property,
		UObject* Owner,
		const void* ValuePtr,
		FDescriptor& OutDescriptor,
		FString& OutError);
}
