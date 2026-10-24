// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class UObject;
class UClass;
class UPackage;

namespace UeAgentReflectionAuthoring
{
	struct FProfile;

	UEAGENTINTERFACE_API FString NormalizePackagePath(const FString& AssetPath);
	UEAGENTINTERFACE_API UClass* ResolveAssetClass(const FProfile& Profile, bool bForCreate, UE::UAI::Contract::FDiagnostic& OutError);
	// Null without a diagnostic means no asset is registered at the requested path.
	UEAGENTINTERFACE_API UObject* LoadAsset(const FProfile& Profile, const FString& AssetPath, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API UObject* CreateFactorySeed(
		const FProfile& Profile,
		UPackage* Outer,
		FName Name,
		EObjectFlags Flags,
		UE::UAI::Contract::FDiagnostic& OutError,
		UClass* RequestedClass = nullptr);
	UEAGENTINTERFACE_API UObject* DuplicateAsset(UObject* Source, UObject* Outer, FName Name, EObjectFlags Flags, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API UObject* PublishAsset(UObject* Source, const FString& PackagePath, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool SaveAsset(UObject& Asset, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool DeleteAsset(UObject* Asset, const FString& Target, UE::UAI::Contract::FDiagnostic& OutError);
}
