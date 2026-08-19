#include "Composition/DestructiveImport/UeAgentDestructiveImportWorldActivation.h"

namespace UE::UAI::Composition::DestructiveImport
{
	using namespace CommandContract;

	void AppendWorldDestructiveProductionCommandContracts(
		TArray<FUeAgentCommandContract>& Contracts)
	{
		auto Add = [&Contracts](
			const TCHAR* CommandId,
			const TCHAR* Namespace,
			const TCHAR* Method,
			TArray<FUeAgentCommandParameter> Parameters)
		{
			Contracts.Emplace(
				CommandId, Namespace, Method, MoveTemp(Parameters),
				1, 1, EUeAgentCommandStatus::Active);
		};
		auto String = [](const TCHAR* Name)
		{
			return FUeAgentCommandParameter(
				Name, EUeAgentCommandWireType::String, true, false);
		};
		auto Integer = [](const TCHAR* Name)
		{
			return FUeAgentCommandParameter(
				Name, EUeAgentCommandWireType::Integer, true, false);
		};
		Add(TEXT("world.data_layer.delete"), TEXT("world"),
			TEXT("data_layer_delete"), {String(TEXT("layer"))});
		Add(TEXT("world.level_content.delete_scope"), TEXT("world"),
			TEXT("level_content_delete_scope"),
			{String(TEXT("folder_path"))});
		Add(TEXT("world.pcg.cleanup"), TEXT("world"), TEXT("pcg_cleanup"),
			{String(TEXT("actor_ref")), String(TEXT("component_name"))});
		Add(TEXT("world.level_streaming.move_actors"), TEXT("world"),
			TEXT("level_streaming_move_actors"),
			{String(TEXT("actor_ref")), String(TEXT("level_package"))});
		Add(TEXT("world.level_streaming.remove"), TEXT("world"),
			TEXT("level_streaming_remove"),
			{String(TEXT("level_package"))});
		Add(TEXT("modeling.actor.mesh.replace"), TEXT("modeling"),
			TEXT("actor_mesh_replace"),
			{String(TEXT("actor_ref")), String(TEXT("mesh_ref"))});
		Add(TEXT("modeling.asset.material_slot.remove"), TEXT("modeling"),
			TEXT("asset_material_slot_remove"),
			{String(TEXT("target_ref")), Integer(TEXT("index"))});
		Add(TEXT("modeling.voxel.bounds.remove"), TEXT("modeling"),
			TEXT("voxel_bounds_remove"),
			{
				String(TEXT("actor_ref")), String(TEXT("component_name")),
				String(TEXT("space")), Integer(TEXT("min_x")),
				Integer(TEXT("min_y")), Integer(TEXT("min_z")),
				Integer(TEXT("max_x")), Integer(TEXT("max_y")),
				Integer(TEXT("max_z")),
			});
	}
}
