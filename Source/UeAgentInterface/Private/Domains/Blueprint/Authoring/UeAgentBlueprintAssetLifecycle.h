// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class UPackage;

namespace UeAgentBlueprintAssetLifecycle
{
	UEAGENTINTERFACE_API FString NormalizePackagePath(const FString& InPath);
	UEAGENTINTERFACE_API FString ToObjectPath(const FString& PackagePath);
	UEAGENTINTERFACE_API UBlueprint* LoadBlueprint(const FString& InPath);
	UEAGENTINTERFACE_API UBlueprint* DuplicateBlueprint(
		UBlueprint* Source,
		UObject* Outer,
		FName Name = NAME_None);
	UEAGENTINTERFACE_API UBlueprint* PublishBlueprint(
		UBlueprint* Source,
		UPackage* Package,
		const FString& Name);
	UEAGENTINTERFACE_API bool DeleteBlueprint(UBlueprint* Blueprint, FString& OutError);
	UEAGENTINTERFACE_API bool CompileBlueprint(UBlueprint* Blueprint, FString& OutError);
}
