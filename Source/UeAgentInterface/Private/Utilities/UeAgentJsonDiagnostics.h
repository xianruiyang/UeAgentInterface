// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;

namespace UeAgentJsonDiagnostics
{
	FString NormalizeResolvedFilePath(FString FilePath);

	FString ResolveProjectRelativeFilePath(const FString& FilePathInput);

	FString JsonValueTypeToString(const TSharedPtr<FJsonValue>& Value);

	TSharedPtr<FJsonObject> MakeIssue(
			const FString& Severity,
			const FString& Code,
			const FString& Path,
			const FString& Message);

	void AddIssue(
			TArray<TSharedPtr<FJsonValue>>& Issues,
			const FString& Severity,
			const FString& Code,
			const FString& Path,
			const FString& Message);

	int32 CountIssuesBySeverity(
			const TArray<TSharedPtr<FJsonValue>>& Issues,
			const FString& Severity);

	bool HasIssueWithSeverity(
			const TArray<TSharedPtr<FJsonValue>>& Issues,
			const FString& Severity);

	bool HasErrorIssue(const TArray<TSharedPtr<FJsonValue>>& Issues);

	bool JsonValueHasExplicitWriteIntent(const TSharedPtr<FJsonValue>& Value);

	bool JsonObjectHasExplicitWriteIntent(const TSharedPtr<FJsonObject>& Obj);

	void AddUnsupportedApplyIssueIfWriteIntent(
			const TSharedPtr<FJsonObject>& Obj,
			const FString& Path,
			const FString& ProfileName,
			TArray<TSharedPtr<FJsonValue>>& Issues);

	bool SerializeJsonObjectCompact(const TSharedPtr<FJsonObject>& Obj, FString& OutText);

	bool SerializeJsonValueCompact(const TSharedPtr<FJsonValue>& Value, FString& OutText);

	FString SerializeJsonArrayCompact(const TArray<TSharedPtr<FJsonValue>>& Values);

	int32 EditDistance(const FString& AInput, const FString& BInput);

	FString FindClosestKnownField(const FString& FieldName, const TArray<FString>& KnownFields);

	UEAGENTINTERFACE_API void WarnUnknownFields(
			const TSharedPtr<FJsonObject>& Obj,
			const FString& Path,
			const TArray<FString>& KnownFields,
			TArray<TSharedPtr<FJsonValue>>& Warnings);

	const TSharedPtr<FJsonValue>* FindFieldValue(const TSharedPtr<FJsonObject>& Obj, const FString& FieldName);

	UEAGENTINTERFACE_API bool ReadStringField(
			const TSharedPtr<FJsonObject>& Obj,
			const FString& FieldName,
			const FString& Path,
			FString& OutValue,
			TArray<TSharedPtr<FJsonValue>>& Issues,
			const bool bRequired);

	UEAGENTINTERFACE_API bool ReadBoolField(
			const TSharedPtr<FJsonObject>& Obj,
			const FString& FieldName,
			const FString& Path,
			bool& bOutValue,
			TArray<TSharedPtr<FJsonValue>>& Issues,
			const bool bRequired);

	bool TryReadNumberValue(const TSharedPtr<FJsonValue>& Value, double& OutValue);

	bool TryReadNumberFieldByAliases(
			const TSharedPtr<FJsonObject>& Obj,
			const TArray<FString>& FieldNames,
			double& OutValue);

	bool TryReadVectorObject(const TSharedPtr<FJsonObject>& Obj, FVector& OutValue);

	bool TryReadRotatorObject(const TSharedPtr<FJsonObject>& Obj, FRotator& OutValue);

	UEAGENTINTERFACE_API bool TryReadVectorValue(const TSharedPtr<FJsonValue>& Value, FVector& OutValue);

	UEAGENTINTERFACE_API bool TryReadVector2DValue(const TSharedPtr<FJsonValue>& Value, FVector2D& OutValue);

	UEAGENTINTERFACE_API bool TryReadRotatorValue(const TSharedPtr<FJsonValue>& Value, FRotator& OutValue);

	UEAGENTINTERFACE_API bool TryReadLinearColorValue(const TSharedPtr<FJsonValue>& Value, FLinearColor& OutValue);

	UEAGENTINTERFACE_API bool ReadVectorField(
			const TSharedPtr<FJsonObject>& Obj,
			const FString& FieldName,
			const FString& Path,
			FVector& OutValue,
			TArray<TSharedPtr<FJsonValue>>& Issues,
			const bool bRequired);

	UEAGENTINTERFACE_API bool ReadRotatorField(
			const TSharedPtr<FJsonObject>& Obj,
			const FString& FieldName,
			const FString& Path,
			FRotator& OutValue,
			TArray<TSharedPtr<FJsonValue>>& Issues,
			const bool bRequired);

	bool ReadArrayField(
			const TSharedPtr<FJsonObject>& Obj,
			const FString& FieldName,
			const FString& Path,
			const TArray<TSharedPtr<FJsonValue>>*& OutArray,
			TArray<TSharedPtr<FJsonValue>>& Issues,
			const bool bRequired);

	UEAGENTINTERFACE_API bool ReadObjectFromValue(
			const TSharedPtr<FJsonValue>& Value,
			const FString& Path,
			TSharedPtr<FJsonObject>& OutObject,
			TArray<TSharedPtr<FJsonValue>>& Issues);

	bool LoadJsonObjectFile(
			const FString& FilePath,
			TSharedPtr<FJsonObject>& OutObj,
			TArray<TSharedPtr<FJsonValue>>* OutIssues,
			FString& OutError,
			const bool bOptional);
}
