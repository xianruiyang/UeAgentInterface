// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Curves/RichCurve.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

class FProperty;
class UObject;

namespace UeAgentCurveJson
{
	extern const TCHAR* const SchemaName;

	void AddCurveIssue(
		TArray<TSharedPtr<FJsonValue>>& Issues,
		const FString& Severity,
		const FString& Code,
		const FString& Path,
		const FString& Message,
		const FString& Expected = FString(),
		const FString& Actual = FString());

	bool HasError(const TArray<TSharedPtr<FJsonValue>>& Issues);
	FString NormalizeAssetPath(const FString& InPath);

	TSharedPtr<FJsonObject> BuildFloatCurveJson(const FRichCurve& Curve, const FString& Storage, const FString& CarrierCppType);
	void WarnUnknownCurveFields(const TSharedPtr<FJsonObject>& RootObj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues);
	bool ApplyChannelJsonToRichCurve(const TSharedPtr<FJsonObject>& RootObj, const FString& ChannelName, FRichCurve& OutCurve, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues);

	TSharedPtr<FJsonObject> BuildCurveAssetJson(UObject* Asset);
	bool ApplyCurveAssetJson(UObject* Asset, const TSharedPtr<FJsonObject>& RootObj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues);
	bool TryBuildPropertyCurveJson(FProperty* Property, const void* ValuePtr, TSharedPtr<FJsonObject>& OutCurveJson);
	bool TryApplyPropertyCurveJson(FProperty* Property, void* ValuePtr, const TSharedPtr<FJsonObject>& RootObj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues);
	UObject* CreateCurveAsset(const FString& AssetPathInput, const FString& CurveKind, FString& OutError);
}
