#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"

#include "UeAgentT167AssetReferenceTestTypes.generated.h"

UCLASS()
class UUeAgentT167AssetReferenceData final : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere)
	TObjectPtr<UObject> HardReference = nullptr;

	UPROPERTY(EditAnywhere)
	TSoftObjectPtr<UObject> SoftReference;
};
