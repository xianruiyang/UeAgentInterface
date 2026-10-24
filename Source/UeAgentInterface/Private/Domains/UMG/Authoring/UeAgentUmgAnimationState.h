#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class FJsonObject;
class UWidgetAnimation;
namespace UE::UAI::Authoring::Timeline { struct FMovieSceneCreationState; }

namespace UeAgentUmgAuthoring
{
	UEAGENTINTERFACE_API bool TryQueryAnimationTrackCandidates(const FString& Target,
		const FString& AnimationName, const FString& Query, FString& OutPayload, UE::UAI::Contract::FDiagnostic& OutError,
		const FString& BindingKind = {}, const FString& ObjectRef = {}, const FString& PropertyPath = {});
	UEAGENTINTERFACE_API bool TryReadAnimationState(const UWidgetAnimation& Animation,
		const FString& CanonicalAssetPath, TSharedPtr<FJsonObject>& OutState, UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool TryUpdateAnimationState(UWidgetAnimation& Animation,
		const FString& CanonicalAssetPath, const TSharedPtr<FJsonObject>& DesiredScene, UE::UAI::Contract::FDiagnostic& OutError,
		UE::UAI::Authoring::Timeline::FMovieSceneCreationState* CreationState = nullptr);
}
