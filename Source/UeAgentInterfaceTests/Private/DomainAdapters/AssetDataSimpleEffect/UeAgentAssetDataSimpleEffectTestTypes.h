#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Engine/DataTable.h"

#include "UeAgentAssetDataSimpleEffectTestTypes.generated.h"

USTRUCT()
struct FUeAgentAssetDataSimpleEffectTableRow
	: public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere)
	FString Label = TEXT("Default");

	UPROPERTY(EditAnywhere)
	int32 Count = 1;

	UPROPERTY(EditAnywhere)
	float Weight = 0.5f;

	UPROPERTY(EditAnywhere)
	bool bEnabled = false;
};

UCLASS()
class UUeAgentAssetDataSimpleEffectDataAsset final
	: public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(
		EditAnywhere,
		meta = (UAIAssetDataField = "label"))
	FString Label = TEXT("Default");

	UPROPERTY(
		EditAnywhere,
		meta = (UAIAssetDataField = "count"))
	int32 Count = 1;

	UPROPERTY(
		EditAnywhere,
		meta = (UAIAssetDataField = "weight"))
	float Weight = 0.5f;

	UPROPERTY(
		EditAnywhere,
		meta = (UAIAssetDataField = "enabled"))
	bool bEnabled = false;

	UPROPERTY(
		EditAnywhere,
		meta = (
			UAIAssetDataField = "related_asset",
			UAIAssetDataType = "asset_ref"))
	TObjectPtr<UObject> RelatedAsset = nullptr;
};
