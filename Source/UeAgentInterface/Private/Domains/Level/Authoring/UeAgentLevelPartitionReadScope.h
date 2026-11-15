#pragma once

#include "CoreMinimal.h"
#include "WorldPartition/WorldPartitionHelpers.h"

class UWorld;

namespace UeAgentLevelAuthoring
{
	/** Existing Level loading owner. Loading references live through the last
	 * native reader; pure captured values never keep them across publication. */
	void RetainWorldPartitionActorsForAuthoring(UWorld& World,
		FWorldPartitionHelpers::FForEachActorWithLoadingResult& OutLoadedActors);

	struct FLevelPartitionReadScope
	{
		explicit FLevelPartitionReadScope(UWorld& World);
		FWorldPartitionHelpers::FForEachActorWithLoadingResult LoadedActors;
	};
}
