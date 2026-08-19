#include "QueryOwners/WorldCommands/UeAgentWorldCommandQueryOwner.h"

namespace UE::UAI::QueryOwners::WorldCommands
{
	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("world.actor.create"),
			TEXT("world.actor_hlod_layer.update"),
			TEXT("world.actor_streaming_policy.update"),
			TEXT("world.data_layer_instance.create"),
			TEXT("world.data_layer_relation.update"),
			TEXT("world.data_layer_state.update"),
			TEXT("world.level_game_mode.update"),
			TEXT("world.streaming_relation.create"),
			TEXT("world.streaming_relation.state.update"),
			TEXT("world.wall.create_with_opening"),
			TEXT("world.world_partition_streaming.update"),
		};
		return Ids;
	}

	bool IsCommand(const FString& CommandId)
	{
		return GetCommandIds().Contains(CommandId);
	}

	const TArray<FString>& GetIndependentReadbackFlows()
	{
		static const TArray<FString> Flows = {
			TEXT("world.actors"),
			TEXT("world.data_layers"),
			TEXT("world.hlod_assignments"),
			TEXT("world.partition"),
			TEXT("world.streaming_levels"),
		};
		return Flows;
	}
}
