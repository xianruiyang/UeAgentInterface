#pragma once

#include "CoreMinimal.h"

namespace UeAgentGraphViewUtils
{
	static FORCEINLINE FVector2f SnapViewLocationToPixelGrid(const FVector2f& ViewLocation, const float ZoomAmount)
	{
		const float SafeZoom = FMath::Max(FMath::Abs(ZoomAmount), UE_SMALL_NUMBER);
		return FVector2f(
			FMath::RoundToFloat(ViewLocation.X * SafeZoom) / SafeZoom,
			FMath::RoundToFloat(ViewLocation.Y * SafeZoom) / SafeZoom);
	}

	static FORCEINLINE bool IsViewLocationPixelAligned(const FVector2f& ViewLocation, const float ZoomAmount)
	{
		const FVector2f Snapped = SnapViewLocationToPixelGrid(ViewLocation, ZoomAmount);
		return FMath::IsNearlyEqual(ViewLocation.X, Snapped.X, 0.001f)
			&& FMath::IsNearlyEqual(ViewLocation.Y, Snapped.Y, 0.001f);
	}
}
