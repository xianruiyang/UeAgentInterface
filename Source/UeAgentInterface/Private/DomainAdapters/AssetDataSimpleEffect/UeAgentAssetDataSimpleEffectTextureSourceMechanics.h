#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"
#include "Templates/SharedPointer.h"

class UObject;
class UTexture;
class UTexture2D;

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

	/** Native assembly after the public reference decoder has resolved ordered inputs.
	 * Preserves current format settings; ordinary reflected edits are applied separately.
	 * Shares payload preparation/application with the retained typed contract.
	 * The caller owns atomic restoration; this path does not capture a second before-state.
	 */
	UEAGENTINTERFACE_API bool TryPrepareResolvedTextureSourceAssembly(
		UTexture& Target,
		const TArray<UTexture2D*>& OrderedSources,
		FTextureSourcePreparedState& OutState,
		ExecutionMutation::FExecutionMutationError& OutError);

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

	UEAGENTINTERFACE_API bool TryApplyTextureSourceReplacement(
		UObject* Target,
		const FTextureSourcePreparedState& State,
		ExecutionMutation::FExecutionMutationError& OutError);

	/** Compare native pixels with ordered source bytes frozen at prepare, not
	 * with another generated texture or the current contents of input assets. */
	UEAGENTINTERFACE_API bool TryVerifyTextureSourceAssembly(
		UObject* Target,
		const FTextureSourcePreparedState& State,
		ExecutionMutation::FExecutionMutationError& OutError);

	bool TryRollbackTextureSourceReplacement(
		UObject* Target,
		const FTextureSourcePreparedState& State,
		ExecutionMutation::FExecutionMutationError& OutError);
}
