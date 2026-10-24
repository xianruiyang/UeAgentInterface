#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "UeAgentT208DataObjectTestTypes.generated.h"

class UPhysicalMaterial;

UCLASS()
class UUeAgentT208RegisteredDataBase : public UDataAsset
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, meta=(UAIAssetDataField="title"))
	FString Title = TEXT("original");
	// Metadata can authorize a field that is not exposed by native edit flags.
	UPROPERTY(meta=(UAIAssetDataField="counter"))
	int32 Counter = 2;
	UPROPERTY(EditAnywhere)
	int32 Unregistered = 31;
};

UCLASS()
class UUeAgentT208RegisteredData final : public UUeAgentT208RegisteredDataBase
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, meta=(UAIAssetDataField="material", UAIAssetDataType="asset_ref"))
	TObjectPtr<UPhysicalMaterial> Material = nullptr;
};

UCLASS()
class UUeAgentT208InvalidRegisteredData final : public UUeAgentT208RegisteredDataBase
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, meta=(UAIAssetDataField="title"))
	FString DuplicateId;
};
