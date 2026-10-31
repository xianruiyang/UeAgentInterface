#pragma once

#include "Commandlets/WorldPartitionConvertCommandlet.h"

#include "UeAgentWorldPartitionConvertCommandlet.generated.h"

// Native conversion with source-preserving output checks. The transform and
// package publication remain owned by UWorldPartitionConvertCommandlet.
UCLASS()
class UUeAgentWorldPartitionConvertCommandlet final : public UWorldPartitionConvertCommandlet
{
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
