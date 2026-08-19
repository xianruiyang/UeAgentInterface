// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonValue;
class UObject;

namespace UeAgentReflectionAuthoring
{
	struct FProfile;
}

namespace UeAgentReflectionAuthoring::ProfileStateTextureGraph
{
	UEAGENTINTERFACE_API bool Supports(const FProfile& Profile);
	UEAGENTINTERFACE_API bool Validate(
		const FProfile& Profile,
		const TSharedPtr<FJsonValue>& GraphValue,
		FString& OutError);
	UEAGENTINTERFACE_API bool Read(
		const FProfile& Profile,
		UObject& Asset,
		TSharedPtr<FJsonValue>& OutGraphValue,
		FString& OutError);
	UEAGENTINTERFACE_API bool Apply(
		const FProfile& Profile,
		UObject& Asset,
		const TSharedPtr<FJsonValue>& GraphValue,
		FString& OutError);
}
