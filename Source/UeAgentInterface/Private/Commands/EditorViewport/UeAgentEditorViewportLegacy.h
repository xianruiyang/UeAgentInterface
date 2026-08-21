// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "LegacyMechanics/UeAgentLegacyMechanics.h"

namespace UeAgentEditorViewportLegacy
{
	TSharedPtr<FJsonObject> MakeLevelTarget();
	TSharedPtr<FJsonObject> MakeAssetTarget(const FString& AssetPath, const FString& AssetType, const FString& ViewportRole, bool bOpenEditorIfNeeded);
	TSharedPtr<FJsonObject> MakeParamsWithTarget(const TSharedPtr<FJsonObject>& SourceParams, const TSharedPtr<FJsonObject>& Target);

	void AddCompatMetadata(const TSharedPtr<FJsonObject>& Data, const FString& LegacyCommand, const FString& ReplacementCommand);
	void AddLegacyCameraFields(const TSharedPtr<FJsonObject>& Data);
	void MergeFocusResultIntoLegacyFields(const TSharedPtr<FJsonObject>& Data);

	bool ExecuteCameraGet(
		const FUeAgentLegacyMechanics& Server,
		const FUeAgentRequestContext& Ctx,
		const TSharedPtr<FJsonObject>& Target,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutError);

	bool ExecuteCameraSet(
		const FUeAgentLegacyMechanics& Server,
		const FUeAgentRequestContext& Ctx,
		const TSharedPtr<FJsonObject>& Target,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutError);

	bool ExecuteFocus(
		const FUeAgentLegacyMechanics& Server,
		const FUeAgentRequestContext& Ctx,
		const TSharedPtr<FJsonObject>& Target,
		const TSharedPtr<FJsonObject>& Focus,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutError);
}
