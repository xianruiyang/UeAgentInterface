// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace UeAgentReflectionAuthoringCommandBridge
{
	UEAGENTINTERFACE_API bool Export(
		const FString& TypeId,
		const TSharedPtr<FJsonObject>& Params,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutError);

	UEAGENTINTERFACE_API bool Apply(
		const FString& TypeId,
		const TSharedPtr<FJsonObject>& Params,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutError,
		const FString& RequestId = FString());

	UEAGENTINTERFACE_API bool Validate(
		const FString& TypeId,
		const TSharedPtr<FJsonObject>& Params,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutError);
}
