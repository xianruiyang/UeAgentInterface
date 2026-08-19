// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MaterialShared.h"

class FJsonObject;
class FJsonValue;
class FProperty;
class UClass;
class UMaterial;
class UMaterialExpression;
class UMaterialFunction;
class UMaterialFunctionInterface;
class UMaterialInterface;
class UMaterialInstanceConstant;
class UObject;
class UTexture;

struct FExpressionInput;
struct FLinearColor;
struct FMaterialParameterInfo;
struct FGuid;

namespace UeAgentMaterialOps
{
	enum class ECompileMessageSeverityFilter : uint8
	{
		All,
		Error,
		Warning,
		Info,
		WarningOrError
	};

	enum class EMaterialInstanceParameterType : uint8
	{
		Scalar,
		Vector,
		Texture,
		StaticSwitch,
		StaticComponentMask
	};

	bool ParseMaterialInstanceParameterType(const FString& InText, EMaterialInstanceParameterType& OutType);
	bool ResolveStaticComponentMaskValue(
		const UMaterialInterface* MaterialInterface,
		const FMaterialParameterInfo& ParameterInfo,
		const FGuid& FallbackExpressionGuid,
		bool& OutR,
		bool& OutG,
		bool& OutB,
		bool& OutA,
		FGuid& OutExpressionGuid);
	FString NormalizeAssetPath(const FString& InPath);
	UObject* LoadAssetObject(const FString& InPath);
	bool AssetExists(const FString& InPath);
	UMaterial* LoadMaterialAsset(const FString& InPath);
	UMaterialInterface* LoadMaterialInterface(const FString& InPath);
	UMaterialFunction* LoadMaterialFunctionAsset(const FString& InPath);
	UMaterialInstanceConstant* LoadMaterialInstance(const FString& InPath);
	bool SaveAssetPackage(UObject* Asset, FString& OutError);
	bool ResolvePropertyPath(UObject* Obj, const FString& PropertyPath, FProperty*& OutProperty, void*& OutValuePtr);
	bool SetDeprecatedObjectPropertyText(UObject* Target, const FString& PropertyPath, const FString& ValueText, FString* OutAppliedValueText, FString* OutCppType, FString* OutImportError);
	bool ExportObjectPropertyText(UObject* Target, const FString& PropertyPath, FString& OutValueText, FString* OutCppType, FString* OutExportError);
	bool ParseLinearColor(const TSharedPtr<FJsonObject>& Params, const FString& Key, FLinearColor& OutColor);
	TSharedPtr<FJsonObject> ExpressionToJson(UMaterialExpression* Expression);
	bool ParseCompileSeverityFilter(const FString& InFilterText, ECompileMessageSeverityFilter& OutFilter);
	void CollectMaterialCompileMessages(
		UMaterialInterface* MaterialInterface,
		ECompileMessageSeverityFilter SeverityFilter,
		int32 MaxMessages,
		TArray<TSharedPtr<FJsonValue>>& OutMessages,
		int32& OutErrorCount);
}
