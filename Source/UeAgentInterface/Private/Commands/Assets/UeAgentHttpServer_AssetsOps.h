// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetIdentifier.h"
#include "AssetRegistry/IAssetRegistry.h"

class UObject;

namespace UeAgentAssetOps
{
	FString NormalizeAssetPath(const FString& InPath);
	FString ToObjectPath(const FString& InPath);
	UObject* LoadAssetObject(const FString& InPath);
	bool DetachStaleObjectAtPath(const FString& ObjectPath, bool* bOutFoundStaleObject = nullptr);
	FString NormalizeReferenceInputToPackageName(const FString& InPath);
	bool ReadStringArrayParam(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, TArray<FString>& OutValues);
	bool ShouldIncludeDependencyEdge(
		UE::AssetRegistry::EDependencyCategory Category,
		UE::AssetRegistry::EDependencyProperty Properties,
		bool bIncludeHard,
		bool bIncludeSoft,
		bool bIncludeManage,
		bool bIncludeSearchableNames);
	FString AssetDataClassPathString(const FAssetData& AssetData);
	TSharedPtr<FJsonObject> MakeAssetRegistryEdgeJson(
		const FAssetIdentifier& Source,
		const FAssetIdentifier& Target,
		const FString& Direction,
		int32 Depth,
		UE::AssetRegistry::EDependencyCategory Category,
		UE::AssetRegistry::EDependencyProperty Properties);
	TArray<TSharedPtr<FJsonValue>> CollectPackagePathStringHits(const FString& PackageName, int32 MaxHits, bool& bOutTruncated);
}
