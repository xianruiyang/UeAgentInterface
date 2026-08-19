#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"
#include "Templates/SharedPointer.h"

class UObject;

namespace UE::UAI::DomainAdapters::AssetDataSimpleEffect
{
	struct FPaperTileMapPreparedData;
	struct FPaperTileSetPreparedData;

	struct FPaperTileMapPreparedState final
	{
		TSharedPtr<
			FPaperTileMapPreparedData,
			ESPMode::ThreadSafe> Data;
	};

	struct FPaperTileSetPreparedState final
	{
		TSharedPtr<
			FPaperTileSetPreparedData,
			ESPMode::ThreadSafe> Data;
	};

	bool TryPreparePaperTileMapReplacement(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		UObject* Target,
		const StateSemantic::FUeAgentCanonicalValue* MapSize,
		const StateSemantic::FUeAgentCanonicalValue* TileSize,
		const StateSemantic::FUeAgentCanonicalValue* Layers,
		FPaperTileMapPreparedState& OutState,
		ExecutionMutation::FExecutionMutationError& OutError);

	bool TryApplyPaperTileMapReplacement(
		UObject* Target,
		const FPaperTileMapPreparedState& State,
		ExecutionMutation::FExecutionMutationError& OutError);

	bool TryRollbackPaperTileMapReplacement(
		UObject* Target,
		const FPaperTileMapPreparedState& State);

	bool TryPreparePaperTileSetReplacement(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		UObject* Target,
		const StateSemantic::FUeAgentCanonicalValue* SourceTextureRef,
		const StateSemantic::FUeAgentCanonicalValue* TileSize,
		const StateSemantic::FUeAgentCanonicalValue* Margin,
		const StateSemantic::FUeAgentCanonicalValue* Spacing,
		const StateSemantic::FUeAgentCanonicalValue* Terrain,
		FPaperTileSetPreparedState& OutState,
		ExecutionMutation::FExecutionMutationError& OutError);

	bool TryApplyPaperTileSetReplacement(
		UObject* Target,
		const FPaperTileSetPreparedState& State,
		ExecutionMutation::FExecutionMutationError& OutError);

	bool TryRollbackPaperTileSetReplacement(
		UObject* Target,
		const FPaperTileSetPreparedState& State);
}
