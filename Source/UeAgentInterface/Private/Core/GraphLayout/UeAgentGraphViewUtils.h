#pragma once

#include "CoreMinimal.h"

namespace UeAgentGraphViewUtils
{
	float SafeGraphZoomAmount(float ZoomAmount);
	FVector2f SnapViewLocationToPixelGrid(const FVector2f& ViewLocation, float ZoomAmount);
	FVector2f ViewLocationPixelResidual(const FVector2f& ViewLocation, float ZoomAmount);
	bool IsViewLocationPixelAligned(const FVector2f& ViewLocation, float ZoomAmount);
}
