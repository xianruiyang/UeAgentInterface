// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace UeAgentSpaceContract
{
	TSharedRef<FJsonObject> Make3D(
		const FString& Subject,
		const FString& Frame,
		const FString& SemanticFrame = TEXT("requires_command_or_asset_context"),
		const FString& ConversionChain = TEXT("semantic_context_to_ue_frame_required"));

	bool Validate3D(
		const TSharedPtr<const FJsonObject>& Contract,
		const FString& ExpectedSubject,
		const FString& ExpectedFrame,
		const FString& ErrorPrefix,
		FString& OutError);
}
