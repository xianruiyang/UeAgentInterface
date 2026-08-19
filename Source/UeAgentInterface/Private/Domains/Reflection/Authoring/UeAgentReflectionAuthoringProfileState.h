// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class UObject;

namespace UeAgentReflectionAuthoring
{
	struct FProfile;

	namespace ProfileState
	{
		UEAGENTINTERFACE_API bool HasSurface(const FProfile& Profile);

		UEAGENTINTERFACE_API bool PreparePatch(
			const FProfile& Profile,
			const TSharedPtr<const FJsonObject>& ProviderExtension,
			TSharedPtr<FJsonValue>& OutPatch,
			bool& bOutHasPatch,
			FString& OutError);

		UEAGENTINTERFACE_API bool Read(
			const FProfile& Profile,
			UObject& Asset,
			TSharedPtr<FJsonValue>& OutValue,
			FString& OutError);

		UEAGENTINTERFACE_API bool Apply(
			const FProfile& Profile,
			UObject& Asset,
			const TSharedPtr<FJsonValue>& Patch,
			FString& OutError);

		UEAGENTINTERFACE_API bool CompileIfNeeded(
			const FProfile& Profile,
			UObject& Asset,
			FString& OutError);
	}
}
