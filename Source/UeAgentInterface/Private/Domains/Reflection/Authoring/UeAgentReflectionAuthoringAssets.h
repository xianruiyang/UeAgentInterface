// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UObject;
class UClass;
class UPackage;

namespace UeAgentReflectionAuthoring
{
	struct FProfile;

	UEAGENTINTERFACE_API FString NormalizePackagePath(const FString& AssetPath);
	UEAGENTINTERFACE_API UClass* ResolveAssetClass(const FProfile& Profile, bool bForCreate, FString& OutError);
	UEAGENTINTERFACE_API UObject* LoadAsset(const FProfile& Profile, const FString& AssetPath);
	UEAGENTINTERFACE_API UObject* CreateFactorySeed(
		const FProfile& Profile,
		UPackage* Outer,
		FName Name,
		EObjectFlags Flags,
		FString& OutError,
		UClass* RequestedClass = nullptr);
	UEAGENTINTERFACE_API UObject* DuplicateAsset(UObject* Source, UObject* Outer, FName Name, EObjectFlags Flags);
	UEAGENTINTERFACE_API UObject* PublishAsset(UObject* Source, const FString& PackagePath, FString& OutError);
	UEAGENTINTERFACE_API bool SaveAsset(UObject& Asset, FString& OutError);
	UEAGENTINTERFACE_API bool DeleteAsset(UObject* Asset, FString& OutError);
}
