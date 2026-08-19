#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"
#include "Templates/SharedPointer.h"

class UObject;

namespace UE::UAI::DomainAdapters::AssetDataSimpleEffect
{
	struct FTextureSourcePreparedData;

	struct FTextureSourceChangeView final
	{
		bool bSpecified = false;
		FString Presence;
		const StateSemantic::FUeAgentCanonicalValue*
			CanonicalValue = nullptr;
	};

	struct FTextureSourcePreparedState final
	{
		TSharedPtr<
			FTextureSourcePreparedData,
			ESPMode::ThreadSafe> Data;
	};

	bool TryPrepareTexture2DArrayReplacement(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		UObject* Target,
		const FTextureSourceChangeView& SourceTextureRefs,
		const FTextureSourceChangeView& FormatSettings,
		FTextureSourcePreparedState& OutState,
		ExecutionMutation::FExecutionMutationError& OutError);

	bool TryPrepareTextureCubeArrayReplacement(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		UObject* Target,
		const FTextureSourceChangeView& SourceCubeRefs,
		const FTextureSourceChangeView& FormatSettings,
		FTextureSourcePreparedState& OutState,
		ExecutionMutation::FExecutionMutationError& OutError);

	bool TryPrepareTextureCubeReplacement(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		UObject* Target,
		const FTextureSourceChangeView& FaceTextureRefs,
		const FTextureSourceChangeView& FormatSettings,
		FTextureSourcePreparedState& OutState,
		ExecutionMutation::FExecutionMutationError& OutError);

	bool TryPrepareVolumeTextureReplacement(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		UObject* Target,
		const FTextureSourceChangeView& SliceTextureRefs,
		const FTextureSourceChangeView& FormatSettings,
		FTextureSourcePreparedState& OutState,
		ExecutionMutation::FExecutionMutationError& OutError);

	bool TryApplyTextureSourceReplacement(
		UObject* Target,
		const FTextureSourcePreparedState& State,
		ExecutionMutation::FExecutionMutationError& OutError);

	bool TryRollbackTextureSourceReplacement(
		UObject* Target,
		const FTextureSourcePreparedState& State);
}
