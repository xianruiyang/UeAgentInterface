#pragma once

#include "WorldPartition/WorldPartitionBuilder.h"

#include "UeAgentWorldPartitionActorGuidResaveBuilder.generated.h"

UCLASS()
class UUeAgentWorldPartitionActorGuidResaveBuilder final
	: public UWorldPartitionBuilder
{
	GENERATED_BODY()

public:
	virtual bool RequiresCommandletRendering() const override { return false; }
	virtual ELoadingMode GetLoadingMode() const override { return ELoadingMode::Custom; }

protected:
	virtual bool PreRun(
		UWorld* World,
		FPackageSourceControlHelper& PackageHelper) override;
	virtual bool RunInternal(
		UWorld* World,
		const FCellInfo& CellInfo,
		FPackageSourceControlHelper& PackageHelper) override;

private:
	TArray<FGuid> ActorGuids;
};
