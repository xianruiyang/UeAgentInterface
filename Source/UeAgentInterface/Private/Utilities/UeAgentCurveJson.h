// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Curves/RichCurve.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

class FProperty;
class UObject;
class UClass;
struct FSimpleCurve;

namespace UeAgentCurveJson
{
	// Diagnostic Path arguments are JSON Pointers relative to the caller's
	// document/value; use an empty string for its root and escape dynamic parts.
	struct FAssetCurveContract
	{
		UClass* AssetClass;
		FString ValueType;
		FString CurveKind;
		TArray<FString> Channels;
	};
	const FAssetCurveContract* FindAssetCurveContract(const FString& ClassPath);
	const FAssetCurveContract* FindAssetCurveContract(const UObject* Asset);
	TSharedPtr<FJsonObject> BuildCurveAssetValueDefinition(const FAssetCurveContract& Contract);
	bool ValidateCurveAssetJson(const FAssetCurveContract& Contract, const TSharedPtr<FJsonObject>& Value,
		const FString& Path, TArray<UE::UAI::Contract::FDiagnostic>& Issues);
	extern const TCHAR* const SchemaName;

	void AddCurveIssue(
		TArray<UE::UAI::Contract::FDiagnostic>& Issues,
		const FString& Severity,
		const FString& Code,
		const FString& Path,
		const FString& Message,
		const FString& Expected = FString(),
		const FString& Actual = FString());

	bool HasError(const TArray<UE::UAI::Contract::FDiagnostic>& Issues);
	FString NormalizeAssetPath(const FString& InPath);

	UEAGENTINTERFACE_API TSharedPtr<FJsonObject> BuildFloatCurveJson(const FRichCurve& Curve, const FString& Storage, const FString& CarrierCppType, bool bAuthoring = false, const FString& TimeUnit = TEXT("seconds"));
	TSharedPtr<FJsonObject> BuildSimpleCurveJson(const FSimpleCurve& Curve, const FString& Storage, const FString& CarrierCppType, bool bAuthoring = false, const FString& TimeUnit = TEXT("seconds"));
	TSharedPtr<FJsonObject> BuildCurveTableRowSchema(bool bSimple);
	TSharedPtr<FJsonObject> BuildCurveValueDefinition(bool bSimple);
	TSharedPtr<FJsonObject> BuildEmptyCurveTableRowSchema();
	void WarnUnknownCurveFields(const TSharedPtr<FJsonObject>& RootObj, const FString& Path, TArray<UE::UAI::Contract::FDiagnostic>& Issues);
	bool ApplyChannelJsonToRichCurve(const TSharedPtr<FJsonObject>& RootObj, const FString& ChannelName, FRichCurve& OutCurve, const FString& Path, TArray<UE::UAI::Contract::FDiagnostic>& Issues);
	bool ApplyChannelJsonToSimpleCurve(const TSharedPtr<FJsonObject>& RootObj, const FString& ChannelName, FSimpleCurve& OutCurve, const FString& Path, TArray<UE::UAI::Contract::FDiagnostic>& Issues);

	UEAGENTINTERFACE_API TSharedPtr<FJsonObject> BuildCurveAssetJson(UObject* Asset, bool bAuthoring = false);
	bool ApplyCurveAssetJson(UObject* Asset, const TSharedPtr<FJsonObject>& RootObj, const FString& Path, TArray<UE::UAI::Contract::FDiagnostic>& Issues);
	bool TryBuildPropertyCurveJson(FProperty* Property, const void* ValuePtr, TSharedPtr<FJsonObject>& OutCurveJson, TSharedPtr<FJsonObject>* OutValueDefinition = nullptr);
	bool TryApplyPropertyCurveJson(FProperty* Property, void* ValuePtr, const TSharedPtr<FJsonObject>& RootObj, const FString& Path, TArray<UE::UAI::Contract::FDiagnostic>& Issues);
	UObject* CreateCurveAsset(const FString& AssetPathInput, const FString& CurveKind, UE::UAI::Contract::FDiagnostic& OutError);
}
