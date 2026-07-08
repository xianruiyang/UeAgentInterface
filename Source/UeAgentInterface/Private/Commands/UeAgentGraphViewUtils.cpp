#include "Commands/UeAgentGraphViewUtils.h"

namespace UeAgentGraphViewUtils
{
	float SafeGraphZoomAmount(const float ZoomAmount)
	{
		return FMath::Max(FMath::Abs(ZoomAmount), UE_SMALL_NUMBER);
	}

	FVector2f SnapViewLocationToPixelGrid(const FVector2f& ViewLocation, const float ZoomAmount)
	{
		const float SafeZoom = SafeGraphZoomAmount(ZoomAmount);
		return FVector2f(
			FMath::RoundToFloat(ViewLocation.X * SafeZoom) / SafeZoom,
			FMath::RoundToFloat(ViewLocation.Y * SafeZoom) / SafeZoom);
	}

	FVector2f ViewLocationPixelResidual(const FVector2f& ViewLocation, const float ZoomAmount)
	{
		const float SafeZoom = SafeGraphZoomAmount(ZoomAmount);
		return FVector2f(
			(ViewLocation.X * SafeZoom) - FMath::RoundToFloat(ViewLocation.X * SafeZoom),
			(ViewLocation.Y * SafeZoom) - FMath::RoundToFloat(ViewLocation.Y * SafeZoom));
	}

	bool IsViewLocationPixelAligned(const FVector2f& ViewLocation, const float ZoomAmount)
	{
		const FVector2f Residual = ViewLocationPixelResidual(ViewLocation, ZoomAmount);
		return FMath::IsNearlyZero(Residual.X, 0.001f)
			&& FMath::IsNearlyZero(Residual.Y, 0.001f);
	}
}
