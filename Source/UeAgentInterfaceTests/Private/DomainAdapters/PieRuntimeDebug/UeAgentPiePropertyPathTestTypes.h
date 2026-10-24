#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UeAgentPiePropertyPathTestTypes.generated.h"

USTRUCT()
struct FUeAgentPiePathRecord
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, Category=Test)
    int32 Count = 5;
    UPROPERTY(EditAnywhere, Category=Test)
    TArray<int32> Values = {1};
    UPROPERTY(VisibleAnywhere, Category=Test)
    int32 ReadOnly = 41;
};

UCLASS()
class UUeAgentPiePathTarget : public UObject
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, Category=Test)
    TMap<FName, FUeAgentPiePathRecord> Records;
    UPROPERTY(EditAnywhere, Category=Test)
    TSet<int32> EmptySet;
    UPROPERTY(EditAnywhere, Category=Test)
    FUeAgentPiePathRecord FixedMixed[2];
};
