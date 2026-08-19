#include "Composition/DestructiveImport/UeAgentDestructiveImportExternalActivation.h"

namespace UE::UAI::Composition::DestructiveImport
{
	using namespace CommandContract;

	void AppendExternalDataImportProductionCommandContracts(
		TArray<FUeAgentCommandContract>& Contracts)
	{
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
		auto Add = [&Contracts](
			const TCHAR* CommandId,
			const TCHAR* Method,
			TArray<FUeAgentCommandParameter> Parameters)
		{
			Contracts.Emplace(
				CommandId, TEXT("external"), Method, MoveTemp(Parameters),
				1, 1, EUeAgentCommandStatus::Active);
		};

		Add(TEXT("external.fbx.animation.import"),
			TEXT("fbx_animation_import"),
			{String(TEXT("source_ref")), String(TEXT("destination_ref")),
				String(TEXT("skeleton_ref"))});
		Add(TEXT("external.fbx.skeletal_mesh.import"),
			TEXT("fbx_skeletal_mesh_import"),
			{String(TEXT("source_ref")), String(TEXT("destination_ref"))});
		Add(TEXT("external.geometry_cache.import"),
			TEXT("geometry_cache_import"),
			{String(TEXT("source_ref")), String(TEXT("destination_ref"))});
		Add(TEXT("external.texture.import"), TEXT("texture_import"),
			{String(TEXT("source_ref")), String(TEXT("destination_ref"))});
		Add(TEXT("external.light_profile.import"),
			TEXT("light_profile_import"),
			{String(TEXT("source_ref")), String(TEXT("destination_ref"))});
		Add(TEXT("external.sparse_volume_texture.import"),
			TEXT("sparse_volume_texture_import"),
			{String(TEXT("source_ref")), String(TEXT("destination_ref"))});
		Add(TEXT("external.sound_wave.import"), TEXT("sound_wave_import"),
			{String(TEXT("source_ref")), String(TEXT("destination_ref"))});
		Add(TEXT("external.localization.po.import"),
			TEXT("localization_po_import"),
			{String(TEXT("source_ref")), String(TEXT("target_ref"))});
		Add(TEXT("external.skin_weight_profile.import"),
			TEXT("skin_weight_profile_import"),
			{String(TEXT("source_ref")), String(TEXT("target_ref")),
				String(TEXT("profile_name")), Integer(TEXT("lod_index"))});
	}
}
