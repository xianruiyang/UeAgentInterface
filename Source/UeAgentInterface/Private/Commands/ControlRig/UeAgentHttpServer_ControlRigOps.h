// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "ControlRigGizmoLibrary.h"

class UObject;
class UControlRigShapeLibrary;

namespace UeAgentControlRigOps
{
	FString NormalizeAssetPath(const FString& InPath);
	UObject* LoadAssetObject(const FString& InPath);
	UControlRigShapeLibrary* LoadShapeLibrary(const FString& InPath);
	bool SaveAssetPackage(UObject* Asset, FString& OutError);
	bool HasBlockingIssue(const TArray<TSharedPtr<FJsonValue>>& Issues);
	bool SaveJsonFile(const FString& FilePath, const TSharedPtr<FJsonObject>& Obj, FString& OutError);
	bool LoadJsonFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObj, TArray<TSharedPtr<FJsonValue>>& Issues, FString& OutError, const bool bOptional);
	FString DefaultFolderForAsset(const FString& AssetPath);
	bool ApplyShapeDefinitionJson(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FControlRigShapeDefinition& OutShape, TArray<TSharedPtr<FJsonValue>>& Issues);
	TSharedPtr<FJsonObject> BuildShapeLibraryInfo(UControlRigShapeLibrary* Library);
	UControlRigShapeLibrary* CreateShapeLibraryAsset(const FString& AssetPath, FString& OutError);
}
