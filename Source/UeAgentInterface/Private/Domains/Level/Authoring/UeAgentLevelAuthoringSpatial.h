// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonObject;

namespace UeAgentLevelAuthoring
{
	enum class ETransformFrame : uint8
	{
		ActorWorld,
		ComponentParentRelative,
	};

	struct FTransformPatch
	{
		TOptional<FVector> Translation;
		TOptional<FRotator> Rotation;
		TOptional<FVector> Scale;

		bool IsEmpty() const
		{
			return !Translation.IsSet() && !Rotation.IsSet() && !Scale.IsSet();
		}
	};

	// The owning Actor/Component operation fixes the frame; input metadata does not convert values.
	UEAGENTINTERFACE_API bool ParseTransformPatch(
		const TSharedPtr<const FJsonObject>& Object,
		ETransformFrame Frame,
		FTransformPatch& OutPatch,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API void ApplyTransformPatch(
		const FTransformPatch& Patch,
		FTransform& InOutTransform);
	UEAGENTINTERFACE_API TSharedRef<FJsonObject> BuildTransformState(
		const FTransform& Transform,
		ETransformFrame Frame,
		bool bIncludeFrameMetadata = false);
}
