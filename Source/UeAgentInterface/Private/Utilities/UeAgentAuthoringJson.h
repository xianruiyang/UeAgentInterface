#pragma once

#include "CoreMinimal.h"

#include <initializer_list>

class FJsonObject;
class FJsonValue;
class FProperty;
class UEnum;

namespace UeAgentAuthoringJson
{
	TArray<TSharedPtr<FJsonValue>> MakeStringArray(std::initializer_list<FString> Values);

	TSharedPtr<FJsonObject> MakeSelector(const FString& Key, const FString& Value);

	TSharedPtr<FJsonObject> MakeAvailability(const bool bEditable, const FString& Reason = FString());

	FString GetReflectedPropertyCategoryPath(const FProperty* Property);

	FString GetReflectedPropertyControlType(const FProperty* Property);

	FString GetReflectedPropertyValueKind(const FProperty* Property);

	const UEnum* GetReflectedPropertyEnum(const FProperty* Property);

	TArray<TSharedPtr<FJsonValue>> MakeEnumCandidates(const UEnum* Enum);

	void ApplyAuthoringMetadata(
			const TSharedPtr<FJsonObject>& Obj,
			const FString& SelectorKey,
			const FString& SelectorValue,
			const FString& DisplayName,
			const FString& CategoryPath,
			const FString& ControlType,
			const FString& ValueKind,
			std::initializer_list<FString> Operations,
			const bool bEditable = true);

	void ApplyPropertyMetadata(
			const TSharedPtr<FJsonObject>& Obj,
			const FString& PropertyPath,
			const FString& DisplayName,
			const FString& CategoryPath,
			const FString& ValueKind);

	void ApplyReflectedPropertyMetadata(const TSharedPtr<FJsonObject>& Obj, const FProperty* Property, const FString& PropertyPath);
}
