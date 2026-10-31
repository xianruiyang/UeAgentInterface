#include "DomainAdapters/ModelingGeometry/UeAgentModelingMetricMaterial.h"

#include "Components/PrimitiveComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Materials/MaterialInterface.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::ModelingMetricMaterial
{
	const TCHAR* AssetPath()
	{
		return TEXT("/UeAgentInterface/Materials/M_UAI_MetricGrid.M_UAI_MetricGrid");
	}

	UMaterialInterface* Load()
	{
		return LoadObject<UMaterialInterface>(nullptr, AssetPath());
	}

	bool IsMetric(const UMaterialInterface* Material)
	{
		return Material && Material->GetPathName() == AssetPath();
	}

	bool TryReadColor(const TSharedPtr<FJsonObject>& Input, TOptional<FLinearColor>& OutColor)
	{
		OutColor.Reset();
		if (!Input.IsValid() || !Input->HasField(TEXT("color"))) return true;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Input->TryGetArrayField(TEXT("color"), Values) || !Values || Values->Num() != 3) return false;
		double RGB[3];
		for (int32 Index = 0; Index < 3; ++Index)
		{
			if (!(*Values)[Index].IsValid() || !(*Values)[Index]->TryGetNumber(RGB[Index])
				|| !FMath::IsFinite(RGB[Index]) || RGB[Index] < 0.0 || RGB[Index] > 1.0) return false;
		}
		OutColor = FLinearColor(RGB[0], RGB[1], RGB[2], 1.0f);
		return true;
	}

	void Initialize(UPrimitiveComponent& Component, const FBox& LocalBounds, const TOptional<FLinearColor>& Color)
	{
		if (!IsMetric(Component.GetMaterial(0)) || !LocalBounds.IsValid) return;
		// RGB at 0..2, initialized at 3, fixed mesh-local origin at 4..6.
		// These are component defaults: they survive package save/load, unlike a transient MID.
		const TArray<float>& Data = Component.GetDefaultCustomPrimitiveData().Data;
		const bool bInitialized = Data.Num() >= 7 && Data[3] == 1.0f;
		Component.Modify();
		if (!bInitialized)
		{
			Component.SetDefaultCustomPrimitiveDataVector3(4, LocalBounds.Min);
		}
		if (Color.IsSet() || !bInitialized)
		{
			const FLinearColor Tint = Color.Get(FLinearColor(0.22f, 0.36f, 0.48f));
			Component.SetDefaultCustomPrimitiveDataVector4(0, FVector4(Tint.R, Tint.G, Tint.B, 1.0));
		}
		Component.MarkPackageDirty();
	}

	void RestoreDefaults(UPrimitiveComponent& Component, const FCustomPrimitiveData& Defaults)
	{
		// The public default setters only grow the array. Restore the reflected value
		// exactly so a failed modeling apply does not leave extra primitive-data slots.
		FStructProperty* Property = FindFProperty<FStructProperty>(UPrimitiveComponent::StaticClass(), TEXT("CustomPrimitiveData"));
		check(Property);
		Property->CopyCompleteValue(Property->ContainerPtrToValuePtr<void>(&Component), &Defaults);
		Component.ResetCustomPrimitiveData();
		Component.MarkRenderStateDirty();
	}
}
