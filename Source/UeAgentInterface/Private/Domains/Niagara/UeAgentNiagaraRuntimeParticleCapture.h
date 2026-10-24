// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FNiagaraEmitterInstance;

namespace UeAgentNiagaraRuntimeParticleCapture
{
	struct FOptions
	{
		TArray<FString> AttributeNames;

		bool IsEnabled() const { return !AttributeNames.IsEmpty(); }
	};

	UEAGENTINTERFACE_API int32 ResolveAttributeNameIndex(
		const TArray<FString>& AvailableNames,
		const FString& RequestedName);

	UEAGENTINTERFACE_API TSharedRef<FJsonObject> Capture(
		FNiagaraEmitterInstance& EmitterInstance,
		const FOptions& Options);
}
