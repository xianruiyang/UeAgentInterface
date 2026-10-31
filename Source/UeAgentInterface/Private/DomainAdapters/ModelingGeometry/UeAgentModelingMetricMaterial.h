#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class UMaterialInterface;
class UPrimitiveComponent;
struct FCustomPrimitiveData;

namespace UE::UAI::ModelingMetricMaterial
{
	const TCHAR* AssetPath();
	UMaterialInterface* Load();
	bool IsMetric(const UMaterialInterface* Material);
	bool TryReadColor(const TSharedPtr<FJsonObject>& Input, TOptional<FLinearColor>& OutColor);
	void Initialize(UPrimitiveComponent& Component, const FBox& LocalBounds, const TOptional<FLinearColor>& Color = {});
	void RestoreDefaults(UPrimitiveComponent& Component, const FCustomPrimitiveData& Defaults);
}
