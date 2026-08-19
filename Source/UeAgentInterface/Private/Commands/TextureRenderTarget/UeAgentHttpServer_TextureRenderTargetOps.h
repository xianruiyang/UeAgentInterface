// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ImageCore.h"
#include "Engine/Texture.h"
#include "Engine/TextureRenderTarget.h"
#include "Engine/TextureRenderTarget2D.h"

class UTextureGraphBase;
struct FUeAgentRequestContext;

bool GetReferencesForAsset(const FString& AssetPath, TArray<TSharedPtr<FJsonValue>>& OutReferencers);

namespace UeAgentTextureOps
{
	FString NormalizeAssetPath(const FString& InPath);
	FString ToObjectPath(const FString& InPath);
	UObject* LoadAssetObject(const FString& InPath);
	bool GetRequiredString(const FUeAgentRequestContext& Ctx, const TCHAR* FieldName, FString& OutValue, FString& OutError);
	bool GetRequiredStringAny(const FUeAgentRequestContext& Ctx, const TArray<const TCHAR*>& FieldNames, FString& OutValue, FString& OutError);
	bool JsonTryGetStringAny(const TSharedPtr<FJsonObject>& Obj, const TArray<const TCHAR*>& FieldNames, FString& OutValue);
	bool GetOptionalBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, bool& InOutValue, FString& OutError);
	void EchoOptionalBool(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, const TCHAR* FieldName, bool DefaultValue, FString& OutError);
	bool GetOptionalInt(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, int32& InOutValue, FString& OutError);
	bool GetOptionalFloat(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, float& InOutValue, FString& OutError);
	bool ReadStringArray(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, TArray<FString>& OutValues);
	FString ResolveFilePath(const FString& FilePath);
	bool SaveAuthoringFile(const FString& OutputPathInput, const TSharedPtr<FJsonObject>& Root, FString& OutOutputPath, FString& OutError);
	bool SaveAuthoringFile(const FString& OutputPathInput, const TSharedPtr<FJsonObject>& Root, FString& OutOutputPath, FString& OutError, bool bStandaloneExport);
	bool LoadAuthoringFile(const FString& InputPathInput, TSharedPtr<FJsonObject>& OutRoot, FString& OutResolvedPath, FString& OutError);
	TSharedPtr<FJsonObject> MakeCoverageReport(const FString& Profile);
	TSharedPtr<FJsonObject> MakeIssue(const FString& Code, const FString& Severity, const FString& Message);
	void AddIssue(TArray<TSharedPtr<FJsonValue>>& Issues, const FString& Code, const FString& Severity, const FString& Message);
	bool SavePackagesForObjects(const TArray<UObject*>& Objects, FString& OutError);
	bool ValidateAssetPathForCreate(const FString& AssetPathInput, FString& OutAssetPath, FString& OutAssetName, FString& OutError);
	UObject* CreateAssetObject(UClass* AssetClass, const FString& AssetPathInput, bool bReplaceExisting, FString& OutError);
	void FillAssetIdentity(UObject* Asset, const TSharedPtr<FJsonObject>& OutData);
	FString PixelFormatToString(EPixelFormat Value);
	bool ParseRenderTargetFormat(const FString& FormatTextInput, ETextureRenderTargetFormat& OutFormat);
	EPixelFormat PixelFormatFromRTFormat(ETextureRenderTargetFormat Format);
	TSharedPtr<FJsonObject> MakeColorJson(const FLinearColor& Color);
	bool ParseColor(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, FLinearColor& InOutColor, FString& OutError);
	bool ParseColorAny(const TSharedPtr<FJsonObject>& Params, const TArray<const TCHAR*>& FieldNames, FLinearColor& InOutColor, FString& OutError);
	ERawImageFormat::Type TextureSourceFormatToRaw(ETextureSourceFormat Format);
	ETextureSourceFormat RawImageFormatToTextureSource(ERawImageFormat::Type Format);
	bool ReadTextureSourceImage(UTexture* Texture, int32 MipIndex, int32 SliceIndex, FImage& OutImage, FString& OutError);
	void ImageToColors(const FImage& Image, TArray<FColor>& OutColors);
	TSharedPtr<FJsonObject> BuildPixelStatsFromColors(const TArray<FColor>& Pixels, const FIntPoint& Size);
	TSharedPtr<FJsonObject> BuildPixelStatsFromImage(const FImage& Image);
	bool ReadRenderTarget2DPixels(UTextureRenderTarget2D* RenderTarget, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError);
	bool RequireRenderResourceAccess(FString& OutError);
	bool ApplyCanonicalProperty(UObject* Obj, const FString& PropertyPath, const TSharedPtr<FJsonValue>& CanonicalValue, TSharedPtr<FJsonObject>& OutSummary, FString& OutError);
	TSharedPtr<FJsonObject> BuildTextureInfo(UTexture* Texture, bool bIncludeSource, bool bIncludePixelStats);
	TSharedPtr<FJsonObject> BuildRenderTargetInfo(UTextureRenderTarget* RenderTarget);
	TSharedPtr<FJsonObject> BuildGenericAssetJson(UObject* Asset, const FString& Profile, bool bIncludeProperties);
	bool ApplyGenericProperties(UObject* Asset, const TSharedPtr<FJsonObject>& Root, TArray<TSharedPtr<FJsonValue>>& OutResults, FString& OutError);
	bool RejectAuthoringPropertiesFolderIfPresent(const FString& FolderPath, const FString& OwnerKind, const FString& AssetPath, const FString& SourceCommand, TSharedPtr<FJsonObject>& OutData, FString& OutError);
	bool ApplyAssetDetailsFromFolderIfPresent(UObject* Asset, const FString& FolderPath, const TSharedPtr<FJsonObject>& Root, const FString& OwnerKind, const FString& SourceCommand, bool bApply, TSharedPtr<FJsonObject>& DetailsApplySummary, int32& GroupedDetailsApplied, FString& OutError);
	bool RenderTextureGraphViaReflection(UObject* WorldContextObject, UTextureGraphBase* Graph, TArray<UTextureRenderTarget2D*>& OutOutputs, FString& OutError);

	template<typename TObjectType>
	TObjectType* LoadAsset(const FString& InPath)
	{
		return Cast<TObjectType>(LoadAssetObject(InPath));
	}

	template<typename EnumType>
	FString EnumToString(EnumType Value)
	{
		if (const UEnum* Enum = StaticEnum<EnumType>())
		{
			return Enum->GetNameStringByValue(static_cast<int64>(Value));
		}
		return FString::FromInt(static_cast<int32>(Value));
	}
}
