// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;

struct UEAGENTINTERFACE_API FUeAgentResponseOptions
{
	FUeAgentResponseOptions();

	FString View;
	int32 MaxItems;
};

struct FUeAgentRequestContext
{
	FString RequestId;
	FString Command;
	TSharedPtr<FJsonObject> Params;
	FUeAgentResponseOptions ResponseOptions;
};

struct UEAGENTINTERFACE_API FUeAgentCommandOutput
{
	TSharedPtr<FJsonObject> Data;
	TSharedPtr<FJsonObject> Debug;
	TArray<TSharedPtr<FJsonValue>> Artifacts;
	TArray<TSharedPtr<FJsonValue>> Warnings;

	void Reset();
	TSharedPtr<FJsonObject> GetDataOrEmpty() const;
	static FUeAgentCommandOutput FromLegacy(const TSharedPtr<FJsonObject>& InData, const TArray<TSharedPtr<FJsonValue>>& InArtifacts);
};

struct UEAGENTINTERFACE_API FUeAgentResponseBuilder
{
	static TSharedPtr<FJsonObject> BuildResponseEnvelope(
		const FUeAgentRequestContext& Ctx,
		bool bOk,
		const FString& Error,
		const FUeAgentCommandOutput& Output);
};
