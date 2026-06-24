// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include <initializer_list>

class UWorld;

namespace UeAgentLevelContentJson
{
	bool TryGetStringAny(const TSharedPtr<FJsonObject>& Obj, std::initializer_list<const TCHAR*> Keys, FString& OutValue);
	bool TryGetBoolAny(const TSharedPtr<FJsonObject>& Obj, std::initializer_list<const TCHAR*> Keys, bool& OutValue);
	bool TryGetObject(const TSharedPtr<FJsonObject>& Obj, const FString& Key, TSharedPtr<FJsonObject>& OutObj);
	TSharedPtr<FJsonObject> BuildRepairDiagnosis(UWorld* World, const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> MergeThreeWay(const TSharedPtr<FJsonObject>& Base, const TSharedPtr<FJsonObject>& Current, const TSharedPtr<FJsonObject>& Target);
}
