// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class UObject;
class UWorld;

namespace UeAgentLevelContentJson
{
	bool ExportPropertyText(UObject* Obj, const FString& PropertyPath, FString& OutText, FString& OutClass);
	bool SetObjectPropertyFromJson(UObject* Obj, const FString& PropertyPath, const TSharedPtr<FJsonValue>& Value, UWorld* World, TSharedPtr<FJsonObject>& OutResult, FString& OutError);
	bool ValidatePropertyExists(UObject* Obj, const FString& PropertyPath);
}
