#include "DomainAdapters/AssetSimple/UeAgentAssetSimpleMechanicsBoundary.h"

namespace UE::UAI::DomainAdapters::AssetSimple
{
	const TArray<FString>& GetAssetSimpleMechanicsCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("asset.audio_effect_preset.create"),
			TEXT("asset.audio_effect_preset.update"),
			TEXT("asset.curve.update"),
			TEXT("asset.data_layer.create"),
			TEXT("asset.data_object.create"),
			TEXT("asset.data_object.update"),
			TEXT("asset.data_table.replace"),
			TEXT("asset.input_action.create"),
			TEXT("asset.input_action.update"),
			TEXT("asset.input_mapping_context.create"),
			TEXT("asset.input_mapping_context.update"),
			TEXT("asset.media_player.create"),
			TEXT("asset.media_player.update"),
			TEXT("asset.media_source.create"),
			TEXT("asset.media_source.update"),
			TEXT("asset.media_texture.create"),
			TEXT("asset.media_texture.update"),
			TEXT("asset.paper_flipbook.create"),
			TEXT("asset.paper_flipbook.update"),
			TEXT("asset.paper_sprite.create"),
			TEXT("asset.paper_sprite.update"),
			TEXT("asset.paper_tilemap.update"),
			TEXT("asset.paper_tileset.update"),
			TEXT("asset.render_target.create"),
			TEXT("asset.render_target.update"),
			TEXT("asset.runtime_virtual_texture.create"),
			TEXT("asset.runtime_virtual_texture.update"),
			TEXT("asset.skeletal_mesh.update"),
			TEXT("asset.sound_attenuation.create"),
			TEXT("asset.sound_attenuation.update"),
			TEXT("asset.sound_class.create"),
			TEXT("asset.sound_class.update"),
			TEXT("asset.sound_concurrency.create"),
			TEXT("asset.sound_concurrency.update"),
			TEXT("asset.sound_mix.create"),
			TEXT("asset.sound_mix.update"),
			TEXT("asset.sound_submix.create"),
			TEXT("asset.sound_submix.update"),
			TEXT("asset.sparse_volume_texture.update"),
			TEXT("asset.static_mesh.update"),
			TEXT("asset.subuv_animation.create"),
			TEXT("asset.subuv_animation.update"),
			TEXT("asset.texture_array.create"),
			TEXT("asset.texture_array.replace"),
			TEXT("asset.texture_collection.create"),
			TEXT("asset.texture_collection.update"),
			TEXT("asset.texture_cube.create"),
			TEXT("asset.texture_cube.replace"),
			TEXT("asset.texture_cube_array.create"),
			TEXT("asset.texture_cube_array.replace"),
			TEXT("asset.volume_texture.create"),
			TEXT("asset.volume_texture.replace"),
		};
		return Ids;
	}

	bool IsAssetSimpleMechanicsCommand(const FString& CommandId)
	{
		return GetAssetSimpleMechanicsCommandIds().Contains(CommandId);
	}

	const TArray<FString>& GetAssetSimpleRequiredAuthorityScopes()
	{
		static const TArray<FString> Scopes = {
			TEXT("uai:asset-data:write"),
		};
		return Scopes;
	}
}
