#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"

#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCoreAssetMechanics.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectEnhancedInputMechanics.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectMediaMechanics.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectPaperMechanics.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectStructuredAssetMechanics.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectTextureFieldMechanics.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectTextureSourceProjection.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/DataAsset.h"
#include "FileMediaSource.h"
#include "MediaSource.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Sound/SoundEffectSource.h"
#include "Sound/SoundEffectSubmix.h"
#include "Sound/SoundSubmix.h"
#include "SubmixEffects/AudioMixerSubmixEffectEQ.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::DomainAdapters::AssetDataSimpleEffect
{
	using namespace ContextResource;
	using namespace Contract;
	using namespace Discovery;
	using namespace ExecutionMutation;
	using namespace PlanningPreflight;
	using namespace StateSemantic;

	namespace
	{
		enum class EFieldValueKind : uint8
		{
			String,
			Boolean,
			Integer,
			Number,
			Array,
			Object,
		};

		enum class EFieldReferencePolicy : uint8
		{
			None,
			AssetRef,
			RecursiveAssetRefs,
			ClassRef,
		};

		struct FFieldSpec
		{
			const TCHAR* ProfileId;
			const TCHAR* FieldId;
			const TCHAR* FieldBindingId;
			const TCHAR* TypeId;
			EFieldValueKind ValueKind;
			const TCHAR* AllowedPresences;
			EFieldReferencePolicy ReferencePolicy;
		};

#define UAI_ASSET_DATA_TYPE_AssetRef TEXT("uai.ref.asset.v1")
#define UAI_ASSET_DATA_TYPE_AudioEffectPresetSettings TEXT("uai.asset.audio_effect_preset.settings.v1")
#define UAI_ASSET_DATA_TYPE_Boolean TEXT("uai.value.boolean.v1")
#define UAI_ASSET_DATA_TYPE_Box3d TEXT("uai.value.box3d.v1")
#define UAI_ASSET_DATA_TYPE_ClassRef TEXT("uai.ref.class.v1")
#define UAI_ASSET_DATA_TYPE_CurveKeys TEXT("uai.asset.curve.keys.v1")
#define UAI_ASSET_DATA_TYPE_CurveType TEXT("uai.asset.curve.type.v1")
#define UAI_ASSET_DATA_TYPE_DataObjectRegisteredFields TEXT("uai.asset.data_object.registered_fields.v1")
#define UAI_ASSET_DATA_TYPE_DataTableRows TEXT("uai.asset.data_table.rows.v1")
#define UAI_ASSET_DATA_TYPE_Decimal TEXT("uai.value.decimal.v1")
#define UAI_ASSET_DATA_TYPE_Extent2i TEXT("uai.value.extent2i.v1")
#define UAI_ASSET_DATA_TYPE_InputActionModifiers TEXT("uai.asset.input_action.modifiers.v1")
#define UAI_ASSET_DATA_TYPE_InputActionTriggers TEXT("uai.asset.input_action.triggers.v1")
#define UAI_ASSET_DATA_TYPE_InputActionValueType TEXT("uai.asset.input_action.value_type.v1")
#define UAI_ASSET_DATA_TYPE_InputMappingContextMappings TEXT("uai.asset.input_mapping_context.mappings.v1")
#define UAI_ASSET_DATA_TYPE_Integer TEXT("uai.value.integer.v1")
#define UAI_ASSET_DATA_TYPE_LinearColor TEXT("uai.value.linear_color.v1")
#define UAI_ASSET_DATA_TYPE_MediaSourceOptions TEXT("uai.asset.media_source.options.v1")
#define UAI_ASSET_DATA_TYPE_MeshMaterialSlots TEXT("uai.asset.mesh.material_slots.v1")
#define UAI_ASSET_DATA_TYPE_OrderedAssetRefs TEXT("uai.value.ordered_asset_refs.v1")
#define UAI_ASSET_DATA_TYPE_PaperFlipbookFrames TEXT("uai.asset.paper_flipbook.frames.v1")
#define UAI_ASSET_DATA_TYPE_PaperTilemapLayers TEXT("uai.asset.paper_tilemap.layers.v1")
#define UAI_ASSET_DATA_TYPE_PaperTilesetTerrain TEXT("uai.asset.paper_tileset.terrain.v1")
#define UAI_ASSET_DATA_TYPE_Rect2d TEXT("uai.value.rect2d.v1")
#define UAI_ASSET_DATA_TYPE_RenderTargetFormat TEXT("uai.asset.render_target.format.v1")
#define UAI_ASSET_DATA_TYPE_RuntimeVirtualTextureMaterialType TEXT("uai.asset.runtime_virtual_texture.material_type.v1")
#define UAI_ASSET_DATA_TYPE_SkeletalMeshLodSettings TEXT("uai.asset.skeletal_mesh.lod_settings.v1")
#define UAI_ASSET_DATA_TYPE_SoundAttenuationSettings TEXT("uai.asset.sound_attenuation.settings.v1")
#define UAI_ASSET_DATA_TYPE_SoundClassProperties TEXT("uai.asset.sound_class.properties.v1")
#define UAI_ASSET_DATA_TYPE_SoundConcurrencySettings TEXT("uai.asset.sound_concurrency.settings.v1")
#define UAI_ASSET_DATA_TYPE_SoundMixModifiers TEXT("uai.asset.sound_mix.modifiers.v1")
#define UAI_ASSET_DATA_TYPE_SparseVolumeTextureAddressing TEXT("uai.asset.sparse_volume_texture.addressing.v1")
#define UAI_ASSET_DATA_TYPE_SparseVolumeTextureAttributes TEXT("uai.asset.sparse_volume_texture.attributes.v1")
#define UAI_ASSET_DATA_TYPE_SparseVolumeTextureStreaming TEXT("uai.asset.sparse_volume_texture.streaming.v1")
#define UAI_ASSET_DATA_TYPE_StaticMeshBuildSettings TEXT("uai.asset.static_mesh.build_settings.v1")
#define UAI_ASSET_DATA_TYPE_StaticMeshCollision TEXT("uai.asset.static_mesh.collision.v1")
#define UAI_ASSET_DATA_TYPE_StaticMeshSockets TEXT("uai.asset.static_mesh.sockets.v1")
#define UAI_ASSET_DATA_TYPE_StructRef TEXT("uai.ref.struct.v1")
#define UAI_ASSET_DATA_TYPE_SubuvAnimationBoundingGeometry TEXT("uai.asset.subuv_animation.bounding_geometry.v1")
#define UAI_ASSET_DATA_TYPE_TextureCubeFaceRefs TEXT("uai.asset.texture_cube.face_refs.v1")
#define UAI_ASSET_DATA_TYPE_TextureFormatSettings TEXT("uai.asset.texture.format_settings.v1")
#define UAI_ASSET_DATA_TYPE_Uri TEXT("uai.value.uri.v1")
#define UAI_ASSET_DATA_TYPE_Vector2d TEXT("uai.value.vector2d.v1")
#define UAI_ASSET_DATA_FIELD(Profile, Field, Type, Kind, Presences, ReferencePolicy) \
	{ \
		TEXT(#Profile), \
		TEXT(#Field), \
		TEXT("uai.asset_data.field:" #Profile ":" #Field ":v1"), \
		UAI_ASSET_DATA_TYPE_##Type, \
		EFieldValueKind::Kind, \
		TEXT(Presences), \
		EFieldReferencePolicy::ReferencePolicy, \
	},
		const FFieldSpec FieldSpecs[] = {
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectFieldCatalog.inl"
		};
#undef UAI_ASSET_DATA_FIELD
#undef UAI_ASSET_DATA_TYPE_AssetRef
#undef UAI_ASSET_DATA_TYPE_AudioEffectPresetSettings
#undef UAI_ASSET_DATA_TYPE_Boolean
#undef UAI_ASSET_DATA_TYPE_Box3d
#undef UAI_ASSET_DATA_TYPE_ClassRef
#undef UAI_ASSET_DATA_TYPE_CurveKeys
#undef UAI_ASSET_DATA_TYPE_CurveType
#undef UAI_ASSET_DATA_TYPE_DataObjectRegisteredFields
#undef UAI_ASSET_DATA_TYPE_DataTableRows
#undef UAI_ASSET_DATA_TYPE_Decimal
#undef UAI_ASSET_DATA_TYPE_Extent2i
#undef UAI_ASSET_DATA_TYPE_InputActionModifiers
#undef UAI_ASSET_DATA_TYPE_InputActionTriggers
#undef UAI_ASSET_DATA_TYPE_InputActionValueType
#undef UAI_ASSET_DATA_TYPE_InputMappingContextMappings
#undef UAI_ASSET_DATA_TYPE_Integer
#undef UAI_ASSET_DATA_TYPE_LinearColor
#undef UAI_ASSET_DATA_TYPE_MediaSourceOptions
#undef UAI_ASSET_DATA_TYPE_MeshMaterialSlots
#undef UAI_ASSET_DATA_TYPE_OrderedAssetRefs
#undef UAI_ASSET_DATA_TYPE_PaperFlipbookFrames
#undef UAI_ASSET_DATA_TYPE_PaperTilemapLayers
#undef UAI_ASSET_DATA_TYPE_PaperTilesetTerrain
#undef UAI_ASSET_DATA_TYPE_Rect2d
#undef UAI_ASSET_DATA_TYPE_RenderTargetFormat
#undef UAI_ASSET_DATA_TYPE_RuntimeVirtualTextureMaterialType
#undef UAI_ASSET_DATA_TYPE_SkeletalMeshLodSettings
#undef UAI_ASSET_DATA_TYPE_SoundAttenuationSettings
#undef UAI_ASSET_DATA_TYPE_SoundClassProperties
#undef UAI_ASSET_DATA_TYPE_SoundConcurrencySettings
#undef UAI_ASSET_DATA_TYPE_SoundMixModifiers
#undef UAI_ASSET_DATA_TYPE_SparseVolumeTextureAddressing
#undef UAI_ASSET_DATA_TYPE_SparseVolumeTextureAttributes
#undef UAI_ASSET_DATA_TYPE_SparseVolumeTextureStreaming
#undef UAI_ASSET_DATA_TYPE_StaticMeshBuildSettings
#undef UAI_ASSET_DATA_TYPE_StaticMeshCollision
#undef UAI_ASSET_DATA_TYPE_StaticMeshSockets
#undef UAI_ASSET_DATA_TYPE_StructRef
#undef UAI_ASSET_DATA_TYPE_SubuvAnimationBoundingGeometry
#undef UAI_ASSET_DATA_TYPE_TextureCubeFaceRefs
#undef UAI_ASSET_DATA_TYPE_TextureFormatSettings
#undef UAI_ASSET_DATA_TYPE_Uri
#undef UAI_ASSET_DATA_TYPE_Vector2d

		EUeAgentValueKind ToCanonicalValueKind(
			const EFieldValueKind Kind)
		{
			switch (Kind)
			{
			case EFieldValueKind::String:
				return EUeAgentValueKind::String;
			case EFieldValueKind::Boolean:
				return EUeAgentValueKind::Boolean;
			case EFieldValueKind::Integer:
				return EUeAgentValueKind::Integer;
			case EFieldValueKind::Number:
				return EUeAgentValueKind::Decimal;
			case EFieldValueKind::Array:
				return EUeAgentValueKind::Array;
			case EFieldValueKind::Object:
				return EUeAgentValueKind::Object;
			}
			return EUeAgentValueKind::Null;
		}

		FString ToReferencePolicyId(
			const EFieldReferencePolicy Policy)
		{
			switch (Policy)
			{
			case EFieldReferencePolicy::None:
				return TEXT("none");
			case EFieldReferencePolicy::AssetRef:
				return TEXT("asset_ref");
			case EFieldReferencePolicy::RecursiveAssetRefs:
				return TEXT("recursive_asset_refs");
			case EFieldReferencePolicy::ClassRef:
				return TEXT("class_ref");
			}
			return FString();
		}

		struct FDescriptorSpec
		{
			const TCHAR* DescriptorId;
			const TCHAR* CapabilityId;
			const TCHAR* ProfileId;
			const TCHAR* NativeOperationType;
			const TCHAR* FieldIds;
			const TCHAR* AllowedPresences;
		};

		const FDescriptorSpec DescriptorSpecs[] = {
			{ TEXT("uai.operation.asset.audio_effect_preset.create"), TEXT("asset.audio_effect_preset.create"), TEXT("audio_effect_preset"), TEXT("asset.object.create"), TEXT(""), TEXT("") },
			{ TEXT("uai.operation.asset.audio_effect_preset.update"), TEXT("asset.audio_effect_preset.update"), TEXT("audio_effect_preset"), TEXT("asset.object.update"), TEXT("settings"), TEXT("set|clear") },
			{ TEXT("uai.operation.asset.sound_attenuation.create"), TEXT("asset.sound_attenuation.create"), TEXT("sound_attenuation"), TEXT("asset.object.create"), TEXT(""), TEXT("") },
			{ TEXT("uai.operation.asset.sound_attenuation.update"), TEXT("asset.sound_attenuation.update"), TEXT("sound_attenuation"), TEXT("asset.object.update"), TEXT("attenuation"), TEXT("set|clear") },
			{ TEXT("uai.operation.asset.sound_class.create"), TEXT("asset.sound_class.create"), TEXT("sound_class"), TEXT("asset.object.create"), TEXT(""), TEXT("") },
			{ TEXT("uai.operation.asset.sound_class.update"), TEXT("asset.sound_class.update"), TEXT("sound_class"), TEXT("asset.collection.update"), TEXT("properties|child_classes"), TEXT("set|clear|remove|replace") },
			{ TEXT("uai.operation.asset.sound_concurrency.create"), TEXT("asset.sound_concurrency.create"), TEXT("sound_concurrency"), TEXT("asset.object.create"), TEXT(""), TEXT("") },
			{ TEXT("uai.operation.asset.sound_concurrency.update"), TEXT("asset.sound_concurrency.update"), TEXT("sound_concurrency"), TEXT("asset.object.update"), TEXT("concurrency"), TEXT("set|clear") },
			{ TEXT("uai.operation.asset.sound_mix.create"), TEXT("asset.sound_mix.create"), TEXT("sound_mix"), TEXT("asset.object.create"), TEXT(""), TEXT("") },
			{ TEXT("uai.operation.asset.sound_mix.update"), TEXT("asset.sound_mix.update"), TEXT("sound_mix"), TEXT("asset.collection.update"), TEXT("modifiers|initial_delay"), TEXT("set|clear|remove|replace") },
			{ TEXT("uai.operation.asset.sound_submix.create"), TEXT("asset.sound_submix.create"), TEXT("sound_submix"), TEXT("asset.object.create"), TEXT(""), TEXT("") },
			{ TEXT("uai.operation.asset.sound_submix.update"), TEXT("asset.sound_submix.update"), TEXT("sound_submix"), TEXT("asset.collection.update"), TEXT("parent|effect_chain"), TEXT("set|null|clear|remove|replace") },
			{ TEXT("uai.operation.asset.object.clone"), TEXT("asset.object.clone"), TEXT("clone"), TEXT("asset.object.clone"), TEXT(""), TEXT("") },
			{ TEXT("uai.operation.asset.curve.update"), TEXT("asset.curve.update"), TEXT("curve"), TEXT("asset.content.replace"), TEXT("keys"), TEXT("replace") },
			{ TEXT("uai.operation.asset.data_object.create"), TEXT("asset.data_object.create"), TEXT("data_object"), TEXT("asset.object.create"), TEXT(""), TEXT("") },
			{ TEXT("uai.operation.asset.data_object.update"), TEXT("asset.data_object.update"), TEXT("data_object"), TEXT("asset.object.update"), TEXT("registered_fields"), TEXT("set|clear") },
			{ TEXT("uai.operation.asset.data_table.replace"), TEXT("asset.data_table.replace"), TEXT("data_table"), TEXT("asset.content.replace"), TEXT("row_struct_ref|rows"), TEXT("replace") },
			{ TEXT("uai.operation.asset.data_layer.create"), TEXT("asset.data_layer.create"), TEXT("data_layer"), TEXT("asset.object.create"), TEXT(""), TEXT("") },
			{ TEXT("uai.operation.asset.skeletal_mesh.update"), TEXT("asset.skeletal_mesh.update"), TEXT("skeletal_mesh"), TEXT("asset.content.replace"), TEXT("materials|lod_settings|bounds|skeleton_ref"), TEXT("replace") },
			{ TEXT("uai.operation.asset.input_action.create"), TEXT("asset.input_action.create"), TEXT("input_action"), TEXT("asset.object.create"), TEXT(""), TEXT("") },
			{ TEXT("uai.operation.asset.input_action.update"), TEXT("asset.input_action.update"), TEXT("input_action"), TEXT("asset.object.update"), TEXT("value_type|consume_input|triggers|modifiers"), TEXT("set|clear|replace") },
			{ TEXT("uai.operation.asset.input_mapping_context.create"), TEXT("asset.input_mapping_context.create"), TEXT("input_mapping_context"), TEXT("asset.object.create"), TEXT(""), TEXT("") },
			{ TEXT("uai.operation.asset.input_mapping_context.update"), TEXT("asset.input_mapping_context.update"), TEXT("input_mapping_context"), TEXT("asset.collection.update"), TEXT("mappings"), TEXT("set|clear|remove|replace") },
			{ TEXT("uai.operation.asset.static_mesh.update"), TEXT("asset.static_mesh.update"), TEXT("static_mesh"), TEXT("asset.collection.update"), TEXT("materials|collision|sockets|build_settings"), TEXT("set|clear|remove|replace") },
			{ TEXT("uai.operation.asset.media_player.create"), TEXT("asset.media_player.create"), TEXT("media_player"), TEXT("asset.object.create"), TEXT(""), TEXT("") },
			{ TEXT("uai.operation.asset.media_player.update"), TEXT("asset.media_player.update"), TEXT("media_player"), TEXT("asset.object.update"), TEXT("play_on_open|loop"), TEXT("set") },
			{ TEXT("uai.operation.asset.media_source.create"), TEXT("asset.media_source.create"), TEXT("media_source"), TEXT("asset.object.create"), TEXT(""), TEXT("") },
			{ TEXT("uai.operation.asset.media_source.update"), TEXT("asset.media_source.update"), TEXT("media_source"), TEXT("asset.object.update"), TEXT("source_uri|options"), TEXT("set|clear|replace") },
			{ TEXT("uai.operation.asset.media_texture.create"), TEXT("asset.media_texture.create"), TEXT("media_texture"), TEXT("asset.object.create"), TEXT(""), TEXT("") },
			{ TEXT("uai.operation.asset.media_texture.update"), TEXT("asset.media_texture.update"), TEXT("media_texture"), TEXT("asset.object.update"), TEXT("media_player_ref|auto_clear|clear_color"), TEXT("set|null|clear") },
			{ TEXT("uai.operation.asset.paper_flipbook.create"), TEXT("asset.paper_flipbook.create"), TEXT("paper_flipbook"), TEXT("asset.object.create"), TEXT(""), TEXT("") },
			{ TEXT("uai.operation.asset.paper_flipbook.update"), TEXT("asset.paper_flipbook.update"), TEXT("paper_flipbook"), TEXT("asset.content.replace"), TEXT("frames|frames_per_second"), TEXT("replace") },
			{ TEXT("uai.operation.asset.paper_sprite.create"), TEXT("asset.paper_sprite.create"), TEXT("paper_sprite"), TEXT("asset.object.create"), TEXT(""), TEXT("") },
			{ TEXT("uai.operation.asset.paper_sprite.update"), TEXT("asset.paper_sprite.update"), TEXT("paper_sprite"), TEXT("asset.object.update"), TEXT("source_texture_ref|source_region|pivot"), TEXT("set|null|clear") },
			{ TEXT("uai.operation.asset.paper_tilemap.update"), TEXT("asset.paper_tilemap.update"), TEXT("paper_tilemap"), TEXT("asset.content.replace"), TEXT("map_size|tile_size|layers"), TEXT("replace") },
			{ TEXT("uai.operation.asset.paper_tileset.update"), TEXT("asset.paper_tileset.update"), TEXT("paper_tileset"), TEXT("asset.content.replace"), TEXT("source_texture_ref|tile_size|margin|spacing|terrain"), TEXT("replace") },
			{ TEXT("uai.operation.asset.render_target.create"), TEXT("asset.render_target.create"), TEXT("render_target"), TEXT("asset.object.create"), TEXT(""), TEXT("") },
			{ TEXT("uai.operation.asset.render_target.update"), TEXT("asset.render_target.update"), TEXT("render_target"), TEXT("asset.object.update"), TEXT("size|format|clear_color|auto_generate_mips"), TEXT("set|clear") },
			{ TEXT("uai.operation.asset.runtime_virtual_texture.create"), TEXT("asset.runtime_virtual_texture.create"), TEXT("runtime_virtual_texture"), TEXT("asset.object.create"), TEXT(""), TEXT("") },
			{ TEXT("uai.operation.asset.runtime_virtual_texture.update"), TEXT("asset.runtime_virtual_texture.update"), TEXT("runtime_virtual_texture"), TEXT("asset.object.update"), TEXT("size|material_type|compression"), TEXT("set") },
			{ TEXT("uai.operation.asset.sparse_volume_texture.update"), TEXT("asset.sparse_volume_texture.update"), TEXT("sparse_volume_texture"), TEXT("asset.object.update"), TEXT("addressing|streaming"), TEXT("set|clear") },
			{ TEXT("uai.operation.asset.subuv_animation.create"), TEXT("asset.subuv_animation.create"), TEXT("subuv_animation"), TEXT("asset.object.create"), TEXT(""), TEXT("") },
			{ TEXT("uai.operation.asset.subuv_animation.update"), TEXT("asset.subuv_animation.update"), TEXT("subuv_animation"), TEXT("asset.object.update"), TEXT("source_texture_ref|grid|bounding_geometry"), TEXT("set|null|clear") },
			{ TEXT("uai.operation.asset.texture_array.create"), TEXT("asset.texture_array.create"), TEXT("texture_array"), TEXT("asset.object.create"), TEXT(""), TEXT("") },
			{ TEXT("uai.operation.asset.texture_array.replace"), TEXT("asset.texture_array.replace"), TEXT("texture_array"), TEXT("asset.content.replace"), TEXT("source_texture_refs|format_settings"), TEXT("replace") },
			{ TEXT("uai.operation.asset.texture_collection.create"), TEXT("asset.texture_collection.create"), TEXT("texture_collection"), TEXT("asset.object.create"), TEXT(""), TEXT("") },
			{ TEXT("uai.operation.asset.texture_collection.update"), TEXT("asset.texture_collection.update"), TEXT("texture_collection"), TEXT("asset.collection.update"), TEXT("texture_refs"), TEXT("set|clear|remove|replace") },
			{ TEXT("uai.operation.asset.texture_cube.create"), TEXT("asset.texture_cube.create"), TEXT("texture_cube"), TEXT("asset.object.create"), TEXT(""), TEXT("") },
			{ TEXT("uai.operation.asset.texture_cube.replace"), TEXT("asset.texture_cube.replace"), TEXT("texture_cube"), TEXT("asset.content.replace"), TEXT("face_texture_refs|format_settings"), TEXT("replace") },
			{ TEXT("uai.operation.asset.texture_cube_array.create"), TEXT("asset.texture_cube_array.create"), TEXT("texture_cube_array"), TEXT("asset.object.create"), TEXT(""), TEXT("") },
			{ TEXT("uai.operation.asset.texture_cube_array.replace"), TEXT("asset.texture_cube_array.replace"), TEXT("texture_cube_array"), TEXT("asset.content.replace"), TEXT("source_cube_refs|format_settings"), TEXT("replace") },
			{ TEXT("uai.operation.asset.volume_texture.create"), TEXT("asset.volume_texture.create"), TEXT("volume_texture"), TEXT("asset.object.create"), TEXT(""), TEXT("") },
			{ TEXT("uai.operation.asset.volume_texture.replace"), TEXT("asset.volume_texture.replace"), TEXT("volume_texture"), TEXT("asset.content.replace"), TEXT("slice_texture_refs|format_settings"), TEXT("replace") },
		};

		struct FStaticCreateClassSpec
		{
			const TCHAR* CapabilityId;
			const TCHAR* ClassPath;
		};

		const FStaticCreateClassSpec StaticCreateClassSpecs[] = {
			{ TEXT("asset.sound_attenuation.create"), TEXT("/Script/Engine.SoundAttenuation") },
			{ TEXT("asset.sound_class.create"), TEXT("/Script/Engine.SoundClass") },
			{ TEXT("asset.sound_concurrency.create"), TEXT("/Script/Engine.SoundConcurrency") },
			{ TEXT("asset.sound_mix.create"), TEXT("/Script/Engine.SoundMix") },
			{ TEXT("asset.data_layer.create"), TEXT("/Script/Engine.DataLayerAsset") },
			{ TEXT("asset.input_action.create"), TEXT("/Script/EnhancedInput.InputAction") },
			{ TEXT("asset.input_mapping_context.create"), TEXT("/Script/EnhancedInput.InputMappingContext") },
			{ TEXT("asset.media_player.create"), TEXT("/Script/MediaAssets.MediaPlayer") },
			{ TEXT("asset.media_texture.create"), TEXT("/Script/MediaAssets.MediaTexture") },
			{ TEXT("asset.paper_flipbook.create"), TEXT("/Script/Paper2D.PaperFlipbook") },
			{ TEXT("asset.paper_sprite.create"), TEXT("/Script/Paper2D.PaperSprite") },
			{ TEXT("asset.render_target.create"), TEXT("/Script/Engine.TextureRenderTarget2D") },
			{ TEXT("asset.runtime_virtual_texture.create"), TEXT("/Script/Engine.RuntimeVirtualTexture") },
			{ TEXT("asset.subuv_animation.create"), TEXT("/Script/Engine.SubUVAnimation") },
			{ TEXT("asset.texture_array.create"), TEXT("/Script/Engine.Texture2DArray") },
			{ TEXT("asset.texture_collection.create"), TEXT("/Script/Engine.TextureCollection") },
			{ TEXT("asset.texture_cube.create"), TEXT("/Script/Engine.TextureCube") },
			{ TEXT("asset.texture_cube_array.create"), TEXT("/Script/Engine.TextureCubeArray") },
			{ TEXT("asset.volume_texture.create"), TEXT("/Script/Engine.VolumeTexture") },
		};

		struct FFactoryPolicySpec
		{
			const TCHAR* CapabilityId;
			const TCHAR* FactoryContractId;
			const TCHAR* ClassConstraintId;
		};

		const FFactoryPolicySpec FactoryPolicySpecs[] = {
			{
				TEXT("asset.audio_effect_preset.create"),
				TEXT(
					"uai.asset_data.factory_contract:"
					"audio_effect_preset:v1"),
				TEXT(
					"uai.asset_data.class_constraint:"
					"audio_effect_preset:v1"),
			},
			{
				TEXT("asset.sound_submix.create"),
				TEXT(
					"uai.asset_data.factory_contract:"
					"sound_submix:v1"),
				TEXT(
					"uai.asset_data.class_constraint:"
					"sound_submix:v1"),
			},
			{
				TEXT("asset.data_object.create"),
				TEXT(
					"uai.asset_data.factory_contract:"
					"data_object:v1"),
				TEXT(
					"uai.asset_data.class_constraint:"
					"data_object:v1"),
			},
			{
				TEXT("asset.media_source.create"),
				TEXT(
					"uai.asset_data.factory_contract:"
					"media_source:v1"),
				TEXT(
					"uai.asset_data.class_constraint:"
					"media_source:v1"),
			},
		};

		struct FNativeMechanicSpec
		{
			const TCHAR* CapabilityId;
			const TCHAR* MechanicId;
		};

		const FNativeMechanicSpec NativeMechanicSpecs[] = {
			{
				TEXT("asset.render_target.update"),
				TEXT(
					"uai.asset_data.mechanic:"
					"update_render_target:v1"),
			},
			{
				TEXT("asset.static_mesh.update"),
				TEXT(
					"uai.asset_data.mechanic:"
					"update_static_mesh:v1"),
			},
			{
				TEXT("asset.data_table.replace"),
				TEXT(
					"uai.asset_data.mechanic:"
					"replace_data_table:v1"),
			},
			{
				TEXT("asset.skeletal_mesh.update"),
				TEXT(
					"uai.asset_data.mechanic:"
					"update_skeletal_mesh:v1"),
			},
			{
				TEXT("asset.media_player.update"),
				TEXT(
					"uai.asset_data.mechanic:"
					"update_media_player:v1"),
			},
			{
				TEXT("asset.media_texture.update"),
				TEXT(
					"uai.asset_data.mechanic:"
					"update_media_texture:v1"),
			},
			{
				TEXT("asset.subuv_animation.update"),
				TEXT(
					"uai.asset_data.mechanic:"
					"update_subuv_animation:v1"),
			},
			{
				TEXT("asset.texture_collection.update"),
				TEXT(
					"uai.asset_data.mechanic:"
					"update_texture_collection:v1"),
			},
			{
				TEXT("asset.texture_array.replace"),
				TEXT(
					"uai.asset_data.mechanic:"
					"replace_texture_array:v1"),
			},
			{
				TEXT("asset.texture_cube_array.replace"),
				TEXT(
					"uai.asset_data.mechanic:"
					"replace_texture_cube_array:v1"),
			},
			{
				TEXT("asset.texture_cube.replace"),
				TEXT(
					"uai.asset_data.mechanic:"
					"replace_texture_cube:v1"),
			},
			{
				TEXT("asset.volume_texture.replace"),
				TEXT(
					"uai.asset_data.mechanic:"
					"replace_volume_texture:v1"),
			},
			{
				TEXT("asset.input_action.update"),
				TEXT(
					"uai.asset_data.mechanic:"
					"update_input_action:v1"),
			},
			{
				TEXT("asset.input_mapping_context.update"),
				TEXT(
					"uai.asset_data.mechanic:"
					"update_input_mapping_context:v1"),
			},
			{
				TEXT("asset.paper_sprite.update"),
				TEXT(
					"uai.asset_data.mechanic:"
					"update_paper_sprite:v1"),
			},
			{
				TEXT("asset.paper_flipbook.update"),
				TEXT(
					"uai.asset_data.mechanic:"
					"replace_paper_flipbook:v1"),
			},
			{
				TEXT("asset.paper_tilemap.update"),
				TEXT(
					"uai.asset_data.mechanic:"
					"replace_paper_tilemap:v1"),
			},
			{
				TEXT("asset.paper_tileset.update"),
				TEXT(
					"uai.asset_data.mechanic:"
					"replace_paper_tileset:v1"),
			},
			{
				TEXT("asset.audio_effect_preset.update"),
				TEXT(
					"uai.asset_data.mechanic:"
					"update_audio_effect_preset:v1"),
			},
			{
				TEXT("asset.sound_attenuation.update"),
				TEXT(
					"uai.asset_data.mechanic:"
					"update_sound_attenuation:v1"),
			},
			{
				TEXT("asset.sound_class.update"),
				TEXT(
					"uai.asset_data.mechanic:"
					"update_sound_class:v1"),
			},
			{
				TEXT("asset.sound_concurrency.update"),
				TEXT(
					"uai.asset_data.mechanic:"
					"update_sound_concurrency:v1"),
			},
			{
				TEXT("asset.sound_mix.update"),
				TEXT(
					"uai.asset_data.mechanic:"
					"update_sound_mix:v1"),
			},
			{
				TEXT("asset.sound_submix.update"),
				TEXT(
					"uai.asset_data.mechanic:"
					"update_sound_submix:v1"),
			},
			{
				TEXT("asset.curve.update"),
				TEXT(
					"uai.asset_data.mechanic:"
					"replace_curve:v1"),
			},
			{
				TEXT("asset.data_object.update"),
				TEXT(
					"uai.asset_data.mechanic:"
					"update_data_object:v1"),
			},
			{
				TEXT("asset.media_source.update"),
				TEXT(
					"uai.asset_data.mechanic:"
					"update_media_source:v1"),
			},
			{
				TEXT("asset.runtime_virtual_texture.update"),
				TEXT(
					"uai.asset_data.mechanic:"
					"update_runtime_virtual_texture:v1"),
			},
			{
				TEXT("asset.sparse_volume_texture.update"),
				TEXT(
					"uai.asset_data.mechanic:"
					"update_sparse_volume_texture:v1"),
			},
		};

		FString MakeNativeBindingId(const FDescriptorSpec& Spec)
		{
			return TEXT("uai.asset_data.binding:")
				+ FString(Spec.CapabilityId)
				+ TEXT(":v1");
		}

		const FDescriptorSpec* FindSpecByNativeBindingId(
			const FString& NativeBindingId)
		{
			for (const FDescriptorSpec& Spec : DescriptorSpecs)
			{
				if (MakeNativeBindingId(Spec) == NativeBindingId)
				{
					return &Spec;
				}
			}
			return nullptr;
		}

		FString FindStaticCreateClassPath(
			const FString& CapabilityId)
		{
			for (const FStaticCreateClassSpec& Spec
				: StaticCreateClassSpecs)
			{
				if (CapabilityId == Spec.CapabilityId)
				{
					return Spec.ClassPath;
				}
			}
			return FString();
		}

		const FFactoryPolicySpec* FindFactoryPolicy(
			const FString& CapabilityId)
		{
			for (const FFactoryPolicySpec& Policy : FactoryPolicySpecs)
			{
				if (CapabilityId == Policy.CapabilityId)
				{
					return &Policy;
				}
			}
			return nullptr;
		}

		bool IsFactoryClassAllowed(
			const FString& ProfileId,
			UClass* ConcreteClass)
		{
			if (!ConcreteClass
				|| ConcreteClass->HasAnyClassFlags(
					CLASS_Abstract
						| CLASS_Deprecated
						| CLASS_NewerVersionExists))
			{
				return false;
			}
			if (ProfileId == TEXT("audio_effect_preset"))
			{
				return ConcreteClass->IsChildOf(
					USubmixEffectSubmixEQPreset::StaticClass());
			}
			if (ProfileId == TEXT("sound_submix"))
			{
				return ConcreteClass->IsChildOf(
					USoundSubmix::StaticClass());
			}
			if (ProfileId == TEXT("data_object"))
			{
				if (!ConcreteClass->IsChildOf(
						UDataAsset::StaticClass()))
				{
					return false;
				}
				TSet<FString> PublicIds;
				for (TFieldIterator<FProperty> It(ConcreteClass);
					It;
					++It)
				{
					FProperty* Property = *It;
					if (!Property->HasMetaData(
							TEXT("UAIAssetDataField")))
					{
						continue;
					}
					const FString PublicId =
						Property->GetMetaData(
							TEXT("UAIAssetDataField"));
					const bool bSupported =
						CastField<FStrProperty>(Property)
						|| CastField<FIntProperty>(Property)
						|| CastField<FFloatProperty>(Property)
						|| CastField<FBoolProperty>(Property)
						|| (CastField<FObjectPropertyBase>(Property)
							&& Property->GetMetaData(
								TEXT("UAIAssetDataType"))
								== TEXT("asset_ref"));
					if (PublicId.IsEmpty()
						|| !bSupported
						|| PublicIds.Contains(PublicId))
					{
						return false;
					}
					PublicIds.Add(PublicId);
				}
				return !PublicIds.IsEmpty();
			}
			if (ProfileId == TEXT("media_source"))
			{
				return ConcreteClass->IsChildOf(
					UFileMediaSource::StaticClass());
			}
			return false;
		}

		FString FindNativeMechanicId(
			const FDescriptorSpec& Spec)
		{
			const FString NativeType(Spec.NativeOperationType);
			if (NativeType == TEXT("asset.object.create")
				&& !FindStaticCreateClassPath(
					Spec.CapabilityId).IsEmpty())
			{
				return TEXT("uai.asset_data.mechanic:create_object:v1");
			}
			if (NativeType == TEXT("asset.object.create")
				&& FindFactoryPolicy(Spec.CapabilityId))
			{
				return TEXT(
					"uai.asset_data.mechanic:"
					"create_candidate_object:v1");
			}
			if (NativeType == TEXT("asset.object.clone"))
			{
				return TEXT("uai.asset_data.mechanic:clone_object:v1");
			}
			for (const FNativeMechanicSpec& Mechanic
				: NativeMechanicSpecs)
			{
				if (FStringView(Spec.CapabilityId)
					== FStringView(Mechanic.CapabilityId))
				{
					return Mechanic.MechanicId;
				}
			}
			return FString();
		}

		bool Fail(
			FExecutionMutationError& OutError,
			FString Code,
			FString Detail = FString())
		{
			OutError = FExecutionMutationError(
				MoveTemp(Code),
				MoveTemp(Detail));
			return false;
		}

		TArray<FString> SplitSpecValues(const TCHAR* Source)
		{
			TArray<FString> Values;
			const FString Text(Source);
			if (!Text.IsEmpty())
			{
				Text.ParseIntoArray(Values, TEXT("|"), true);
			}
			return Values;
		}

		const FFieldSpec* FindFieldSpec(
			const FString& ProfileId,
			const FString& FieldId)
		{
			for (const FFieldSpec& Field : FieldSpecs)
			{
				if (ProfileId == Field.ProfileId
					&& FieldId == Field.FieldId)
				{
					return &Field;
				}
			}
			return nullptr;
		}

		TSharedPtr<FJsonObject> StringSchema()
		{
			const TSharedPtr<FJsonObject> Schema =
				MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("string"));
			Schema->SetNumberField(TEXT("minLength"), 1);
			return Schema;
		}

		TSharedPtr<FJsonObject> ConstStringSchema(
			const FString& Value)
		{
			const TSharedPtr<FJsonObject> Schema =
				MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("const"), Value);
			return Schema;
		}

		TSharedPtr<FJsonObject> EnumSchema(
			const TArray<FString>& Values)
		{
			const TSharedPtr<FJsonObject> Schema = StringSchema();
			TArray<TSharedPtr<FJsonValue>> EnumValues;
			for (const FString& Value : Values)
			{
				EnumValues.Add(
					MakeShared<FJsonValueString>(Value));
			}
			Schema->SetArrayField(TEXT("enum"), MoveTemp(EnumValues));
			return Schema;
		}

		TSharedPtr<FJsonObject> ObjectSchema(
			const TSharedPtr<FJsonObject>& Properties,
			const TArray<FString>& Required)
		{
			const TSharedPtr<FJsonObject> Schema =
				MakeShared<FJsonObject>();
			Schema->SetStringField(TEXT("type"), TEXT("object"));
			Schema->SetObjectField(TEXT("properties"), Properties);
			Schema->SetBoolField(TEXT("additionalProperties"), false);
			TArray<TSharedPtr<FJsonValue>> RequiredValues;
			for (const FString& Name : Required)
			{
				RequiredValues.Add(
					MakeShared<FJsonValueString>(Name));
			}
			Schema->SetArrayField(
				TEXT("required"),
				MoveTemp(RequiredValues));
			return Schema;
		}

		TSharedPtr<FJsonObject> ValueKindSchema(
			const EFieldValueKind Kind)
		{
			const TSharedPtr<FJsonObject> Schema =
				MakeShared<FJsonObject>();
			switch (Kind)
			{
			case EFieldValueKind::String:
				Schema->SetStringField(TEXT("type"), TEXT("string"));
				Schema->SetNumberField(TEXT("minLength"), 1);
				break;
			case EFieldValueKind::Boolean:
				Schema->SetStringField(TEXT("type"), TEXT("boolean"));
				break;
			case EFieldValueKind::Integer:
				Schema->SetStringField(TEXT("type"), TEXT("integer"));
				break;
			case EFieldValueKind::Number:
				Schema->SetStringField(TEXT("type"), TEXT("number"));
				break;
			case EFieldValueKind::Array:
				Schema->SetStringField(TEXT("type"), TEXT("array"));
				break;
			case EFieldValueKind::Object:
				Schema->SetStringField(TEXT("type"), TEXT("object"));
				break;
			}
			return Schema;
		}

		bool PresenceRequiresValue(const FString& Presence)
		{
			return Presence == TEXT("set")
				|| Presence == TEXT("replace")
				|| Presence == TEXT("remove");
		}

		TSharedPtr<FJsonObject> BuildChangeUnitSchema(
			const FFieldSpec& Field)
		{
			TArray<FString> ValuePresences;
			TArray<FString> NoValuePresences;
			for (const FString& Presence
				: SplitSpecValues(Field.AllowedPresences))
			{
				(PresenceRequiresValue(Presence)
						? ValuePresences
						: NoValuePresences)
					.Add(Presence);
			}
			TArray<TSharedPtr<FJsonValue>> Alternatives;
			if (!ValuePresences.IsEmpty())
			{
				const TSharedPtr<FJsonObject> ValueProperties =
					MakeShared<FJsonObject>();
				ValueProperties->SetObjectField(
					TEXT("type_id"),
					ConstStringSchema(Field.TypeId));
				ValueProperties->SetObjectField(
					TEXT("canonical_value"),
					ValueKindSchema(Field.ValueKind));

				const TSharedPtr<FJsonObject> UnitProperties =
					MakeShared<FJsonObject>();
				UnitProperties->SetObjectField(
					TEXT("field_id"),
					ConstStringSchema(Field.FieldId));
				UnitProperties->SetObjectField(
					TEXT("presence"),
					EnumSchema(ValuePresences));
				UnitProperties->SetObjectField(
					TEXT("value"),
					ObjectSchema(
						ValueProperties,
						{
							TEXT("type_id"),
							TEXT("canonical_value"),
						}));
				Alternatives.Add(
					MakeShared<FJsonValueObject>(
						ObjectSchema(
							UnitProperties,
							{
								TEXT("field_id"),
								TEXT("presence"),
								TEXT("value"),
							})));
			}
			if (!NoValuePresences.IsEmpty())
			{
				const TSharedPtr<FJsonObject> UnitProperties =
					MakeShared<FJsonObject>();
				UnitProperties->SetObjectField(
					TEXT("field_id"),
					ConstStringSchema(Field.FieldId));
				UnitProperties->SetObjectField(
					TEXT("presence"),
					EnumSchema(NoValuePresences));
				Alternatives.Add(
					MakeShared<FJsonValueObject>(
						ObjectSchema(
							UnitProperties,
							{TEXT("field_id"), TEXT("presence")})));
			}
			if (Alternatives.Num() == 1)
			{
				return Alternatives[0]->AsObject();
			}
			const TSharedPtr<FJsonObject> Schema =
				MakeShared<FJsonObject>();
			Schema->SetArrayField(
				TEXT("oneOf"),
				MoveTemp(Alternatives));
			return Schema;
		}

		TSharedPtr<FJsonObject> FactoryRefSchema()
		{
			const TSharedPtr<FJsonObject> RevisionProperties =
				MakeShared<FJsonObject>();
			RevisionProperties->SetObjectField(
				TEXT("source_id"),
				StringSchema());
			RevisionProperties->SetObjectField(
				TEXT("generation"),
				StringSchema());
			const TSharedPtr<FJsonObject> SequenceSchema =
				MakeShared<FJsonObject>();
			SequenceSchema->SetStringField(
				TEXT("type"),
				TEXT("integer"));
			SequenceSchema->SetNumberField(TEXT("minimum"), 0);
			RevisionProperties->SetObjectField(
				TEXT("sequence"),
				SequenceSchema);

			const TSharedPtr<FJsonObject> Properties =
				MakeShared<FJsonObject>();
			Properties->SetObjectField(
				TEXT("candidate_ref"),
				StringSchema());
			Properties->SetObjectField(
				TEXT("catalog_revision"),
				ObjectSchema(
					RevisionProperties,
					{
						TEXT("source_id"),
						TEXT("generation"),
						TEXT("sequence"),
					}));
			return ObjectSchema(
				Properties,
				{TEXT("candidate_ref"), TEXT("catalog_revision")});
		}

		TSharedPtr<FJsonObject> BuildInputSchema(
			const FDescriptorSpec& Spec)
		{
			const TSharedPtr<FJsonObject> Properties =
				MakeShared<FJsonObject>();
			Properties->SetObjectField(TEXT("target_ref"), StringSchema());
			TArray<FString> Required{TEXT("target_ref")};

			const FString NativeType(Spec.NativeOperationType);
			if (NativeType == TEXT("asset.object.create")
				&& FindFactoryPolicy(Spec.CapabilityId))
			{
				Properties->SetObjectField(
					TEXT("factory_ref"),
					FactoryRefSchema());
				Required.Add(TEXT("factory_ref"));
			}
			else if (NativeType == TEXT("asset.object.clone"))
			{
				Properties->SetObjectField(
					TEXT("source_ref"),
					StringSchema());
				Properties->SetObjectField(
					TEXT("expected_source_revision"),
					StringSchema());
				Properties->SetObjectField(
					TEXT("destructive_authorization_id"),
					StringSchema());
				Required.Add(TEXT("source_ref"));
				Required.Add(TEXT("expected_source_revision"));
			}
			else if (NativeType != TEXT("asset.object.create"))
			{
				Properties->SetObjectField(
					TEXT("expected_revision"),
					StringSchema());

				const TSharedPtr<FJsonObject> Changes =
					MakeShared<FJsonObject>();
				Changes->SetStringField(TEXT("type"), TEXT("array"));
				TArray<TSharedPtr<FJsonValue>> FieldAlternatives;
				for (const FString& FieldId
					: SplitSpecValues(Spec.FieldIds))
				{
					const FFieldSpec* Field =
						FindFieldSpec(Spec.ProfileId, FieldId);
					if (!Field)
					{
						continue;
					}
					FieldAlternatives.Add(
						MakeShared<FJsonValueObject>(
							BuildChangeUnitSchema(*Field)));
				}
				const TSharedPtr<FJsonObject> ItemSchema =
					MakeShared<FJsonObject>();
				ItemSchema->SetArrayField(
					TEXT("oneOf"),
					MoveTemp(FieldAlternatives));
				Changes->SetObjectField(TEXT("items"), ItemSchema);
				Changes->SetNumberField(TEXT("minItems"), 1);
				Changes->SetBoolField(TEXT("uniqueItems"), true);
				Properties->SetObjectField(TEXT("changes"), Changes);
				Required.Add(TEXT("expected_revision"));
				Required.Add(TEXT("changes"));
			}
			return ObjectSchema(Properties, Required);
		}

		TSharedPtr<FJsonObject> BuildResultSchema()
		{
			const TSharedPtr<FJsonObject> StringArraySchema =
				MakeShared<FJsonObject>();
			StringArraySchema->SetStringField(
				TEXT("type"),
				TEXT("array"));
			StringArraySchema->SetObjectField(
				TEXT("items"),
				StringSchema());
			StringArraySchema->SetBoolField(
				TEXT("uniqueItems"),
				true);

			const TSharedPtr<FJsonObject> Properties =
				MakeShared<FJsonObject>();
			Properties->SetObjectField(
				TEXT("operation_id"),
				StringSchema());
			Properties->SetObjectField(
				TEXT("outcome"),
				EnumSchema({
					TEXT("complete"),
					TEXT("partial"),
					TEXT("rolled_back"),
					TEXT("state_unknown"),
				}));
			Properties->SetObjectField(
				TEXT("target_ref"),
				StringSchema());
			Properties->SetObjectField(
				TEXT("asset_revision"),
				StringSchema());
			Properties->SetObjectField(
				TEXT("evidence_refs"),
				StringArraySchema);
			return ObjectSchema(
				Properties,
				{
					TEXT("operation_id"),
					TEXT("outcome"),
					TEXT("target_ref"),
					TEXT("asset_revision"),
				});
		}

		bool TryCanonicalize(
			const TSharedPtr<FJsonObject>& Object,
			FString& OutCanonical,
			FString& OutError)
		{
			return Object.IsValid()
				&& Json::TryCanonicalize(
					Object,
					OutCanonical,
					OutError);
		}

		bool TryBuildDescriptorHash(
			const FDescriptorSpec& Spec,
			const TSharedPtr<FJsonObject>& InputSchema,
			const TSharedPtr<FJsonObject>& ResultSchema,
			FString& OutHash,
			FString& OutError)
		{
			const TSharedPtr<FJsonObject> Policy =
				MakeShared<FJsonObject>();
			Policy->SetArrayField(
				TEXT("allowed_state_planes"),
				{
					MakeShared<FJsonValueString>(
						TEXT("authoring_memory")),
				});
			Policy->SetArrayField(TEXT("required_facets"), {});
			Policy->SetStringField(
				TEXT("resolution_policy"),
				TEXT("explicit_only"));
			Policy->SetStringField(
				TEXT("transient_access_policy"),
				TEXT("temporary_mutation_allowed"));

			const TSharedPtr<FJsonObject> Descriptor =
				MakeShared<FJsonObject>();
			Descriptor->SetStringField(
				TEXT("capability_id"),
				Spec.CapabilityId);
			Descriptor->SetStringField(
				TEXT("descriptor_id"),
				Spec.DescriptorId);
			Descriptor->SetStringField(
				TEXT("domain_id"),
				TEXT("asset_data"));
			Descriptor->SetObjectField(
				TEXT("execution_context_policy"),
				Policy);
			Descriptor->SetObjectField(
				TEXT("input_schema"),
				InputSchema);
			Descriptor->SetStringField(
				TEXT("operation_kind"),
				TEXT("operation"));
			Descriptor->SetObjectField(
				TEXT("result_schema"),
				ResultSchema);
			Descriptor->SetNumberField(TEXT("version"), 1);

			FString Canonical;
			if (!TryCanonicalize(
					Descriptor,
					Canonical,
					OutError))
			{
				return false;
			}
			OutHash = Json::Sha256Utf8(Canonical);
			return !OutHash.IsEmpty();
		}

		const FDescriptorSpec* FindSpec(
			const FUeAgentDescriptorIdentity& Identity)
		{
			for (const FDescriptorSpec& Spec : DescriptorSpecs)
			{
				if (Identity.GetDescriptorId() != Spec.DescriptorId
					|| Identity.GetVersion() != 1)
				{
					continue;
				}
				const TSharedPtr<FJsonObject> Input =
					BuildInputSchema(Spec);
				const TSharedPtr<FJsonObject> Result =
					BuildResultSchema();
				FString Hash;
				FString Error;
				if (TryBuildDescriptorHash(
						Spec,
						Input,
						Result,
						Hash,
						Error)
					&& Hash == Identity.GetDescriptorHash())
				{
					return &Spec;
				}
			}
			return nullptr;
		}

		bool TryParsePayload(
			const FUeAgentOperationRequest& Request,
			TSharedPtr<FJsonObject>& OutPayload,
			FExecutionMutationError& OutError)
		{
			OutPayload.Reset();
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(
					Request.GetPayload().GetJsonValue());
			if (!FJsonSerializer::Deserialize(Reader, OutPayload)
				|| !OutPayload.IsValid())
			{
				return Fail(
					OutError,
					TEXT("asset_data_payload_parse_failed"));
			}
			return true;
		}

		const FUeAgentStateEntry* FindEntry(
			const FUeAgentCanonicalSnapshotRef& Snapshot,
			const FString& CanonicalRef)
		{
			for (const FUeAgentStateEntry& Entry
				: Snapshot->GetEntries())
			{
				if (Entry.GetIdentity().GetSelector()
					== CanonicalRef)
				{
					return &Entry;
				}
			}
			return nullptr;
		}

		bool TryGetObjectString(
			const FUeAgentCanonicalValueRef& Value,
			const FString& Field,
			FString& OutText)
		{
			if (Value->GetKind() != EUeAgentValueKind::Object)
			{
				return false;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Value->GetObjectItems())
			{
				if (Item.Key == Field
					&& Item.Value->GetKind()
						== EUeAgentValueKind::String)
				{
					OutText = Item.Value->GetText();
					return true;
				}
			}
			return false;
		}

		bool TryCanonicalValue(
			const TSharedPtr<FJsonValue>& JsonValue,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			OutValue.Reset();
			FUeAgentStateSemanticError Error;
			if (!JsonValue.IsValid())
			{
				return Fail(
					OutError,
					TEXT("asset_data_canonical_value_missing"));
			}
			switch (JsonValue->Type)
			{
			case EJson::Null:
				if (FUeAgentCanonicalValue::TryNull(
						Resources,
						OutValue,
						Error))
				{
					return true;
				}
				break;
			case EJson::Boolean:
				if (FUeAgentCanonicalValue::TryBoolean(
						JsonValue->AsBool(),
						Resources,
						OutValue,
						Error))
				{
					return true;
				}
				break;
			case EJson::String:
				if (FUeAgentCanonicalValue::TryString(
						JsonValue->AsString(),
						Resources,
						OutValue,
						Error))
				{
					return true;
				}
				break;
			case EJson::Number:
				{
					const double Number = JsonValue->AsNumber();
					const bool bInteger =
						FMath::IsNearlyEqual(
							Number,
							FMath::FloorToDouble(Number))
						&& Number
							>= static_cast<double>(MIN_int64)
						&& Number
							<= static_cast<double>(MAX_int64);
					const bool bBuilt = bInteger
						? FUeAgentCanonicalValue::TryInteger(
							FString::Printf(
								TEXT("%lld"),
								static_cast<long long>(
									Number)),
							Resources,
							OutValue,
							Error)
						: FUeAgentCanonicalValue::TryDecimal(
							FString::SanitizeFloat(Number),
							Resources,
							OutValue,
							Error);
					if (bBuilt)
					{
						return true;
					}
					break;
				}
			case EJson::Array:
				{
					TArray<FUeAgentCanonicalValueRef> Items;
					for (const TSharedPtr<FJsonValue>& Item
						: JsonValue->AsArray())
					{
						FUeAgentCanonicalValuePtr CanonicalItem;
						if (!TryCanonicalValue(
								Item,
								Resources,
								CanonicalItem,
								OutError))
						{
							return false;
						}
						Items.Add(CanonicalItem.ToSharedRef());
					}
					if (FUeAgentCanonicalValue::TryArray(
							Items,
							Resources,
							OutValue,
							Error))
					{
						return true;
					}
					break;
				}
			case EJson::Object:
				{
					TArray<
						TPair<
							FString,
							FUeAgentCanonicalValueRef>> Items;
					for (const TPair<
						FString,
						TSharedPtr<FJsonValue>>& Pair
							: JsonValue->AsObject()->Values)
					{
						FUeAgentCanonicalValuePtr CanonicalItem;
						if (!TryCanonicalValue(
								Pair.Value,
								Resources,
								CanonicalItem,
								OutError))
						{
							return false;
						}
						Items.Emplace(
							Pair.Key,
							CanonicalItem.ToSharedRef());
					}
					if (FUeAgentCanonicalValue::TryObject(
							Items,
							Resources,
							OutValue,
							Error))
					{
						return true;
					}
					break;
				}
			default:
				break;
			}
			return Fail(
				OutError,
				Error.Code.IsEmpty()
					? TEXT("asset_data_canonical_value_failed")
					: Error.Code,
				Error.Detail);
		}

		bool JsonValueMatchesFieldKind(
			const TSharedPtr<FJsonValue>& Value,
			const EFieldValueKind Kind)
		{
			if (!Value.IsValid())
			{
				return false;
			}
			switch (Kind)
			{
			case EFieldValueKind::String:
				return Value->Type == EJson::String;
			case EFieldValueKind::Boolean:
				return Value->Type == EJson::Boolean;
			case EFieldValueKind::Integer:
				return Value->Type == EJson::Number
					&& FMath::IsNearlyEqual(
						Value->AsNumber(),
						FMath::FloorToDouble(Value->AsNumber()));
			case EFieldValueKind::Number:
				return Value->Type == EJson::Number;
			case EFieldValueKind::Array:
				return Value->Type == EJson::Array;
			case EFieldValueKind::Object:
				return Value->Type == EJson::Object;
			}
			return false;
		}

		bool CanonicalKindMatchesFieldKind(
			const EUeAgentValueKind CanonicalKind,
			const EFieldValueKind FieldKind)
		{
			switch (FieldKind)
			{
			case EFieldValueKind::String:
				return CanonicalKind == EUeAgentValueKind::String;
			case EFieldValueKind::Boolean:
				return CanonicalKind == EUeAgentValueKind::Boolean;
			case EFieldValueKind::Integer:
				return CanonicalKind == EUeAgentValueKind::Integer;
			case EFieldValueKind::Number:
				return CanonicalKind == EUeAgentValueKind::Decimal
					|| CanonicalKind == EUeAgentValueKind::Integer;
			case EFieldValueKind::Array:
				return CanonicalKind == EUeAgentValueKind::Array;
			case EFieldValueKind::Object:
				return CanonicalKind == EUeAgentValueKind::Object;
			}
			return false;
		}

		bool TryValidateSnapshotReferences(
			const FUeAgentCanonicalSnapshotRef& Snapshot,
			const TSharedPtr<FJsonValue>& Value,
			FString& OutUnresolved)
		{
			if (!Value.IsValid())
			{
				return true;
			}
			if (Value->Type == EJson::String)
			{
				const FString Text = Value->AsString();
				if (Text.StartsWith(TEXT("uai://asset/"))
					&& !FindEntry(Snapshot, Text))
				{
					OutUnresolved = Text;
					return false;
				}
				return true;
			}
			if (Value->Type == EJson::Array)
			{
				for (const TSharedPtr<FJsonValue>& Item
					: Value->AsArray())
				{
					if (!TryValidateSnapshotReferences(
							Snapshot,
							Item,
							OutUnresolved))
					{
						return false;
					}
				}
			}
			else if (Value->Type == EJson::Object)
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair
					: Value->AsObject()->Values)
				{
					if (!TryValidateSnapshotReferences(
							Snapshot,
							Pair.Value,
							OutUnresolved))
					{
						return false;
					}
				}
			}
			return true;
		}
	}

	FUeAgentAssetDataSimpleEffectFactoryBinding::
		FUeAgentAssetDataSimpleEffectFactoryBinding(
			FUeAgentCandidateBinding InCandidateBinding,
			FString InProfileId,
			UClass* InConcreteClass,
			FString InFactoryMechanicId)
		: CandidateBinding(
			MakeShared<const FUeAgentCandidateBinding>(
				MoveTemp(InCandidateBinding)))
		, ProfileId(MoveTemp(InProfileId))
		, ConcreteClass(InConcreteClass)
		, FactoryMechanicId(MoveTemp(InFactoryMechanicId))
	{
		ProfileId.TrimStartAndEndInline();
		FactoryMechanicId.TrimStartAndEndInline();
		bValid = CandidateBinding.IsValid()
			&& CandidateBinding->IsValid()
			&& CandidateBinding->Supports(
				EUeAgentCandidateOperation::Create)
			&& !ProfileId.IsEmpty()
			&& ConcreteClass.IsValid()
			&& !FactoryMechanicId.IsEmpty();
	}

	const FUeAgentCandidateBinding*
	FUeAgentAssetDataSimpleEffectFactoryBinding::
		GetCandidateBinding() const
	{
		return CandidateBinding.Get();
	}

	const FString&
	FUeAgentAssetDataSimpleEffectFactoryBinding::GetProfileId() const
	{
		return ProfileId;
	}

	UClass*
	FUeAgentAssetDataSimpleEffectFactoryBinding::GetConcreteClass() const
	{
		return ConcreteClass.Get();
	}

	const FString&
	FUeAgentAssetDataSimpleEffectFactoryBinding::
		GetFactoryMechanicId() const
	{
		return FactoryMechanicId;
	}

	bool FUeAgentAssetDataSimpleEffectFactoryBinding::IsValid() const
	{
		return bValid
			&& CandidateBinding.IsValid()
			&& CandidateBinding->IsValid()
			&& ConcreteClass.IsValid();
	}

	TArray<FString> GetAssetDataSimpleEffectAuthorityScopes()
	{
		return {TEXT("uai:asset-data:write")};
	}

	bool TryCreateAssetDataSimpleEffectDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		FString& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		const TSharedPtr<FJsonObject> ResultSchema =
			BuildResultSchema();
		FString CanonicalResult;
		if (!TryCanonicalize(
				ResultSchema,
				CanonicalResult,
				OutError))
		{
			return false;
		}

		TArray<FUeAgentOperationDescriptor> Descriptors;
		Descriptors.Reserve(UE_ARRAY_COUNT(DescriptorSpecs));
		for (const FDescriptorSpec& Spec : DescriptorSpecs)
		{
			const TSharedPtr<FJsonObject> InputSchema =
				BuildInputSchema(Spec);
			FString CanonicalInput;
			FString DescriptorHash;
			if (!TryCanonicalize(
					InputSchema,
					CanonicalInput,
					OutError)
				|| !TryBuildDescriptorHash(
					Spec,
					InputSchema,
					ResultSchema,
					DescriptorHash,
					OutError))
			{
				OutRegistry.Reset();
				return false;
			}
			Descriptors.Emplace(
				FUeAgentDescriptorIdentity(
					Spec.DescriptorId,
					1,
					MoveTemp(DescriptorHash)),
				Spec.CapabilityId,
				TEXT("asset_data"),
				EUeAgentDescriptorOperationKind::Operation,
				FUeAgentExecutionContextPolicy(
					{EUeAgentStatePlane::AuthoringMemory},
					{},
					EUeAgentContextResolutionPolicy::ExplicitOnly,
					EUeAgentTransientContextPolicy::
						TemporaryMutationAllowed),
				FUeAgentContractValue(MoveTemp(CanonicalInput)),
				FUeAgentContractValue(CanonicalResult));
		}
		if (Descriptors.Num() != 53)
		{
			OutError = TEXT("asset_data_descriptor_count_invalid");
			return false;
		}
		return FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(Descriptors),
			OutRegistry,
			OutError);
	}

	bool TryGetAssetDataSimpleEffectProductionProfiles(
		TArray<FUeAgentAssetDataSimpleEffectProductionProfile>&
			OutProfiles,
		FString& OutError)
	{
		OutProfiles.Reset();
		OutError.Reset();
		TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
		if (!TryCreateAssetDataSimpleEffectDescriptorRegistry(
				Registry,
				OutError)
			|| !Registry.IsValid())
		{
			return false;
		}

		OutProfiles.Reserve(UE_ARRAY_COUNT(DescriptorSpecs));
		for (const FDescriptorSpec& Spec : DescriptorSpecs)
		{
			const FUeAgentOperationDescriptor* Descriptor =
				Registry->GetDescriptors().FindByPredicate(
					[&Spec](
						const FUeAgentOperationDescriptor&
							Candidate)
					{
						return Candidate.GetIdentity()
							.GetDescriptorId()
							== Spec.DescriptorId;
					});
			if (!Descriptor)
			{
				OutProfiles.Reset();
				OutError =
					TEXT(
						"asset_data_production_descriptor_missing:")
					+ FString(Spec.DescriptorId);
				return false;
			}

			FUeAgentAssetDataSimpleEffectProductionProfile Profile;
			Profile.DescriptorId = Spec.DescriptorId;
			Profile.DescriptorVersion =
				Descriptor->GetIdentity().GetVersion();
			Profile.DescriptorHash =
				Descriptor->GetIdentity().GetDescriptorHash();
			Profile.CapabilityId = Spec.CapabilityId;
			Profile.ProfileId = Spec.ProfileId;
			Profile.NativeOperationType =
				Spec.NativeOperationType;
			Profile.NativeBindingId = MakeNativeBindingId(Spec);
			Profile.NativeMechanicId = FindNativeMechanicId(Spec);
			Profile.StaticCreateClassPath =
				FindStaticCreateClassPath(Spec.CapabilityId);
			Profile.FieldIds = SplitSpecValues(Spec.FieldIds);
			for (const FString& FieldId : Profile.FieldIds)
			{
				const FFieldSpec* Field =
					FindFieldSpec(Spec.ProfileId, FieldId);
				if (!Field)
				{
					OutProfiles.Reset();
					OutError =
						TEXT(
							"asset_data_production_field_missing:")
						+ FString(Spec.ProfileId)
						+ TEXT(":")
						+ FieldId;
					return false;
				}
				FUeAgentAssetDataSimpleEffectProductionField
					ProductionField;
				ProductionField.FieldId = Field->FieldId;
				ProductionField.FieldBindingId =
					Field->FieldBindingId;
				ProductionField.TypeId = Field->TypeId;
				ProductionField.ValueKind =
					ToCanonicalValueKind(Field->ValueKind);
				ProductionField.AllowedPresences =
					SplitSpecValues(Field->AllowedPresences);
				ProductionField.ReferencePolicy =
					ToReferencePolicyId(
						Field->ReferencePolicy);
				if (!ProductionField.IsValid())
				{
					OutProfiles.Reset();
					OutError =
						TEXT(
							"asset_data_production_field_invalid:")
						+ FString(Spec.ProfileId)
						+ TEXT(":")
						+ FieldId;
					return false;
				}
				Profile.Fields.Add(
					MoveTemp(ProductionField));
			}
			if (const FFactoryPolicySpec* Factory =
					FindFactoryPolicy(Spec.CapabilityId))
			{
				Profile.FactoryContractId =
					Factory->FactoryContractId;
				Profile.ClassConstraintId =
					Factory->ClassConstraintId;
			}
			if (!Profile.IsValid()
				|| (Profile.UsesStaticCreateClass()
					&& !Profile.FieldIds.IsEmpty())
				|| (Profile.UsesFactoryCandidate()
					&& !Profile.FieldIds.IsEmpty())
				|| Profile.FieldIds.Num()
					!= Profile.Fields.Num())
			{
				OutProfiles.Reset();
				OutError =
					TEXT(
						"asset_data_production_profile_invalid:")
					+ FString(Spec.DescriptorId);
				return false;
			}
			OutProfiles.Add(MoveTemp(Profile));
		}
		if (OutProfiles.Num() != 53)
		{
			OutProfiles.Reset();
			OutError =
				TEXT("asset_data_production_profile_count_mismatch");
			return false;
		}
		return true;
	}

	bool CanProjectAssetDataSimpleEffectFieldState(
		const FString& NativeMechanicId)
	{
		return CanProjectCoreAssetFieldState(
				NativeMechanicId)
			|| CanProjectEnhancedInputFieldState(
				NativeMechanicId)
			|| CanProjectMediaFieldState(
				NativeMechanicId)
			|| CanProjectPaperFieldState(
				NativeMechanicId)
			|| CanProjectTextureFieldState(
				NativeMechanicId)
			|| CanProjectTextureSourceState(
				NativeMechanicId)
			|| IsStructuredAssetMechanicId(NativeMechanicId);
	}

	bool TryProjectAssetDataSimpleEffectCurrentFieldState(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		const FString& NativeMechanicId,
		UObject* Target,
		const TArray<FString>& SelectedFieldIds,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutCurrentFields,
		FUeAgentCanonicalValuePtr& OutDefaultFields,
		FUeAgentCanonicalValuePtr& OutProjectionContext,
		FExecutionMutationError& OutError)
	{
		if (CanProjectCoreAssetFieldState(NativeMechanicId))
		{
			return TryProjectCoreAssetCurrentFieldState(
				Resolver,
				NativeMechanicId,
				Target,
				SelectedFieldIds,
				Resources,
				OutCurrentFields,
				OutDefaultFields,
				OutProjectionContext,
				OutError);
		}
		if (CanProjectEnhancedInputFieldState(NativeMechanicId))
		{
			return TryProjectEnhancedInputCurrentFieldState(
				Resolver,
				NativeMechanicId,
				Target,
				SelectedFieldIds,
				Resources,
				OutCurrentFields,
				OutDefaultFields,
				OutProjectionContext,
				OutError);
		}
		if (CanProjectMediaFieldState(NativeMechanicId))
		{
			return TryProjectMediaCurrentFieldState(
				Resolver,
				NativeMechanicId,
				Target,
				SelectedFieldIds,
				Resources,
				OutCurrentFields,
				OutDefaultFields,
				OutProjectionContext,
				OutError);
		}
		if (CanProjectPaperFieldState(NativeMechanicId))
		{
			return TryProjectPaperCurrentFieldState(
				Resolver,
				NativeMechanicId,
				Target,
				SelectedFieldIds,
				Resources,
				OutCurrentFields,
				OutDefaultFields,
				OutProjectionContext,
				OutError);
		}
		if (CanProjectTextureFieldState(NativeMechanicId))
		{
			return TryProjectTextureCurrentFieldState(
				Resolver,
				NativeMechanicId,
				Target,
				SelectedFieldIds,
				Resources,
				OutCurrentFields,
				OutDefaultFields,
				OutProjectionContext,
				OutError);
		}
		if (CanProjectTextureSourceState(NativeMechanicId))
		{
			return TryProjectTextureSourceCurrentState(
				Resolver,
				NativeMechanicId,
				Target,
				SelectedFieldIds,
				Resources,
				OutCurrentFields,
				OutDefaultFields,
				OutProjectionContext,
				OutError);
		}
		if (IsStructuredAssetMechanicId(NativeMechanicId))
		{
			return TryProjectStructuredAssetCurrentFieldState(
				Resolver,
				NativeMechanicId,
				Target,
				SelectedFieldIds,
				Resources,
				OutCurrentFields,
				OutDefaultFields,
				OutProjectionContext,
				OutError);
		}
		return Fail(
			OutError,
			TEXT("asset_data_field_projection_unsupported"),
			NativeMechanicId);
	}

	bool TryProjectAssetDataSimpleEffectExpectedFieldState(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		const FString& NativeMechanicId,
		const FUeAgentCanonicalValue& CurrentFields,
		const FUeAgentCanonicalValue& DefaultFields,
		const FUeAgentCanonicalValue& ProjectionContext,
		const FUeAgentCanonicalValue& ProposedAfter,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutExpectedFields,
		FExecutionMutationError& OutError)
	{
		if (CanProjectCoreAssetFieldState(NativeMechanicId))
		{
			return TryProjectCoreAssetExpectedFieldState(
				Resolver,
				NativeMechanicId,
				CurrentFields,
				DefaultFields,
				ProjectionContext,
				ProposedAfter,
				Resources,
				OutExpectedFields,
				OutError);
		}
		if (CanProjectEnhancedInputFieldState(NativeMechanicId))
		{
			return TryProjectEnhancedInputExpectedFieldState(
				Resolver,
				NativeMechanicId,
				CurrentFields,
				DefaultFields,
				ProjectionContext,
				ProposedAfter,
				Resources,
				OutExpectedFields,
				OutError);
		}
		if (CanProjectMediaFieldState(NativeMechanicId))
		{
			return TryProjectMediaExpectedFieldState(
				Resolver,
				NativeMechanicId,
				CurrentFields,
				DefaultFields,
				ProjectionContext,
				ProposedAfter,
				Resources,
				OutExpectedFields,
				OutError);
		}
		if (CanProjectPaperFieldState(NativeMechanicId))
		{
			return TryProjectPaperExpectedFieldState(
				Resolver,
				NativeMechanicId,
				CurrentFields,
				DefaultFields,
				ProjectionContext,
				ProposedAfter,
				Resources,
				OutExpectedFields,
				OutError);
		}
		if (CanProjectTextureFieldState(NativeMechanicId))
		{
			return TryProjectTextureExpectedFieldState(
				Resolver,
				NativeMechanicId,
				CurrentFields,
				DefaultFields,
				ProjectionContext,
				ProposedAfter,
				Resources,
				OutExpectedFields,
				OutError);
		}
		if (CanProjectTextureSourceState(NativeMechanicId))
		{
			return TryProjectTextureSourceExpectedState(
				Resolver,
				NativeMechanicId,
				CurrentFields,
				DefaultFields,
				ProjectionContext,
				ProposedAfter,
				Resources,
				OutExpectedFields,
				OutError);
		}
		if (IsStructuredAssetMechanicId(NativeMechanicId))
		{
			return TryProjectStructuredAssetExpectedFieldState(
				Resolver,
				NativeMechanicId,
				CurrentFields,
				DefaultFields,
				ProjectionContext,
				ProposedAfter,
				Resources,
				OutExpectedFields,
				OutError);
		}
		return Fail(
			OutError,
			TEXT("asset_data_field_projection_unsupported"),
			NativeMechanicId);
	}

	bool TryBuildAssetDataSimpleEffectChangeSetDraft(
		const FUeAgentOperationRequest& Request,
		const FUeAgentExecutionContextRef& Context,
		const FUeAgentCanonicalSnapshotRef& CurrentSnapshot,
		IUeAgentAssetDataSimpleEffectFactoryCandidateResolver&
			FactoryCandidates,
		FUeAgentResourceTracker& Resources,
		TOptional<FChangeSetDraft>& OutDraft,
		FExecutionMutationError& OutError)
	{
		OutDraft.Reset();
		OutError = FExecutionMutationError();
		if (!Request.IsValid()
			|| Context.GetProjectInstanceId().IsEmpty()
			|| Context.GetEditorInstanceId().IsEmpty()
			|| Context.GetContextRevision().IsEmpty()
			|| CurrentSnapshot->GetStatePlane()
				!= EUeAgentStatePlane::AuthoringMemory)
		{
			return Fail(
				OutError,
				TEXT("asset_data_draft_request_invalid"));
		}

		const FDescriptorSpec* Spec =
			FindSpec(Request.GetDescriptorIdentity());
		if (!Spec)
		{
			return Fail(
				OutError,
				TEXT("asset_data_descriptor_not_exact"));
		}

		TSharedPtr<FJsonObject> Payload;
		if (!TryParsePayload(Request, Payload, OutError))
		{
			return false;
		}

		FString TargetRef;
		if (!Payload->TryGetStringField(
				TEXT("target_ref"),
				TargetRef)
			|| TargetRef.IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("asset_data_target_ref_missing"));
		}

		const FString NativeType(Spec->NativeOperationType);
		const bool bCreate =
			NativeType == TEXT("asset.object.create");
		const bool bClone =
			NativeType == TEXT("asset.object.clone");
		const FFactoryPolicySpec* FactoryPolicy =
			bCreate
				? FindFactoryPolicy(Spec->CapabilityId)
				: nullptr;
		const FUeAgentStateEntry* TargetEntry =
			FindEntry(CurrentSnapshot, TargetRef);
		const FUeAgentStateEntry* SourceEntry = nullptr;
		FString SourceRef;
		FString SourceRevision;
		FUeAgentCanonicalValuePtr ExpectedBefore;

		if (bCreate || bClone)
		{
			if (TargetEntry)
			{
				return Fail(
					OutError,
					TEXT("asset_data_target_already_exists"),
					TargetRef);
			}
			if (bClone)
			{
				Payload->TryGetStringField(
					TEXT("source_ref"),
					SourceRef);
				SourceEntry =
					FindEntry(CurrentSnapshot, SourceRef);
				if (!SourceEntry)
				{
					return Fail(
						OutError,
						TEXT("asset_data_source_snapshot_missing"),
						SourceRef);
				}
				FString ActualRevision;
				if (!TryGetObjectString(
						SourceEntry->GetValue(),
						TEXT("asset_revision"),
						ActualRevision)
					|| !Payload->TryGetStringField(
						TEXT("expected_source_revision"),
						SourceRevision)
					|| ActualRevision != SourceRevision)
				{
					return Fail(
						OutError,
						TEXT("asset_data_source_revision_mismatch"),
						SourceRef);
				}
			}
		}
		else
		{
			if (!TargetEntry)
			{
				return Fail(
					OutError,
					TEXT("asset_data_target_snapshot_missing"),
					TargetRef);
			}
			FString ActualRevision;
			FString ExpectedRevision;
			if (!TryGetObjectString(
					TargetEntry->GetValue(),
					TEXT("asset_revision"),
					ActualRevision)
				|| !Payload->TryGetStringField(
					TEXT("expected_revision"),
					ExpectedRevision)
				|| ActualRevision != ExpectedRevision)
			{
				return Fail(
					OutError,
					TEXT("asset_data_target_revision_mismatch"),
					TargetRef);
			}
			FString ActualProfile;
			if (!TryGetObjectString(
					TargetEntry->GetValue(),
					TEXT("profile_id"),
					ActualProfile)
				|| ActualProfile != Spec->ProfileId)
			{
				return Fail(
					OutError,
					TEXT("asset_data_profile_mismatch"),
					TargetRef);
			}
			ExpectedBefore =
				FUeAgentCanonicalValuePtr(
					TargetEntry->GetValue());
		}

		FUeAgentAssetDataSimpleEffectFactoryBinding FactoryBinding;
		TOptional<FUeAgentCatalogRevision> FactoryRevision;
		if (FactoryPolicy)
		{
			const TSharedPtr<FJsonObject>* FactoryRefObject = nullptr;
			const TSharedPtr<FJsonObject>* RevisionObject = nullptr;
			FString FactoryRef;
			FString CatalogSourceId;
			FString CatalogGeneration;
			double CatalogSequenceNumber = -1.0;
			if (!Payload->TryGetObjectField(
					TEXT("factory_ref"),
					FactoryRefObject)
				|| !FactoryRefObject
				|| !FactoryRefObject->IsValid()
				|| !(*FactoryRefObject)->TryGetStringField(
					TEXT("candidate_ref"),
					FactoryRef)
				|| FactoryRef.IsEmpty()
				|| !(*FactoryRefObject)->TryGetObjectField(
					TEXT("catalog_revision"),
					RevisionObject)
				|| !RevisionObject
				|| !RevisionObject->IsValid()
				|| !(*RevisionObject)->TryGetStringField(
					TEXT("source_id"),
					CatalogSourceId)
				|| !(*RevisionObject)->TryGetStringField(
					TEXT("generation"),
					CatalogGeneration)
				|| !(*RevisionObject)->TryGetNumberField(
					TEXT("sequence"),
					CatalogSequenceNumber)
				|| CatalogSequenceNumber < 0.0
				|| !FMath::IsNearlyEqual(
					CatalogSequenceNumber,
					FMath::FloorToDouble(
						CatalogSequenceNumber)))
			{
				return Fail(
					OutError,
					TEXT("asset_data_factory_ref_invalid"),
					Spec->ProfileId);
			}
			const FUeAgentCatalogRevision ExpectedFactoryRevision(
				CatalogSourceId,
				CatalogGeneration,
				static_cast<int64>(CatalogSequenceNumber));
			if (!ExpectedFactoryRevision.IsValid())
			{
				return Fail(
					OutError,
					TEXT("asset_data_factory_ref_invalid"),
					Spec->ProfileId);
			}
			if (!FactoryCandidates.TryResolveForPlanning(
					FactoryRef,
					ExpectedFactoryRevision,
					Spec->ProfileId,
					FactoryBinding,
					OutError))
			{
				return false;
			}
			FString FactoryError;
			if (!TryValidateAssetDataSimpleEffectFactoryBinding(
					MakeNativeBindingId(*Spec),
					FactoryBinding.GetCandidateBinding()
						? FactoryBinding.GetCandidateBinding()
							->GetBindingId()
						: FString(),
					ExpectedFactoryRevision,
					FactoryBinding,
					FactoryError))
			{
				return Fail(
					OutError,
					FactoryError,
					Spec->ProfileId);
			}
			FactoryRevision.Emplace(ExpectedFactoryRevision);
		}

		TArray<TSharedPtr<FJsonValue>> TypedChanges;
		if (!bCreate && !bClone)
		{
			const TArray<TSharedPtr<FJsonValue>>* Changes = nullptr;
			if (!Payload->TryGetArrayField(
					TEXT("changes"),
					Changes)
				|| !Changes)
			{
				return Fail(
					OutError,
					TEXT("asset_data_changes_missing"));
			}
			const TSet<FString> RegisteredFields(
				SplitSpecValues(Spec->FieldIds));
			TSet<FString> SeenFields;
			for (const TSharedPtr<FJsonValue>& ChangeValue
				: *Changes)
			{
				const TSharedPtr<FJsonObject> Change =
					ChangeValue.IsValid()
						? ChangeValue->AsObject()
						: nullptr;
				FString FieldId;
				FString Presence;
				if (!Change.IsValid()
					|| !Change->TryGetStringField(
						TEXT("field_id"),
						FieldId)
					|| !RegisteredFields.Contains(FieldId))
				{
					return Fail(
						OutError,
						TEXT("asset_data_field_not_registered"),
						FieldId);
				}
				const FFieldSpec* Field =
					FindFieldSpec(Spec->ProfileId, FieldId);
				if (!Field)
				{
					return Fail(
						OutError,
						TEXT("asset_data_field_binding_missing"),
						FieldId);
				}
				if (!Change->TryGetStringField(
						TEXT("presence"),
						Presence))
				{
					return Fail(
						OutError,
						TEXT("asset_data_explicit_presence_required"),
						FieldId);
				}
				const TSet<FString> AllowedPresences(
					SplitSpecValues(Field->AllowedPresences));
				if (!AllowedPresences.Contains(Presence))
				{
					return Fail(
						OutError,
						TEXT("asset_data_presence_not_allowed"),
						FieldId + TEXT(":") + Presence);
				}
				if (SeenFields.Contains(FieldId))
				{
					return Fail(
						OutError,
						TEXT("asset_data_duplicate_field_unit"),
						FieldId);
				}
				SeenFields.Add(FieldId);

				const TSharedPtr<FJsonObject>* TypedValue = nullptr;
				const bool bRequiresValue =
					PresenceRequiresValue(Presence);
				const bool bHasTypedValue =
					Change->TryGetObjectField(
						TEXT("value"),
						TypedValue)
					&& TypedValue
					&& TypedValue->IsValid();
				if (bRequiresValue != bHasTypedValue)
				{
					return Fail(
						OutError,
						TEXT("asset_data_value_presence_mismatch"),
						FieldId + TEXT(":") + Presence);
				}

				const TSharedPtr<FJsonObject> InternalChange =
					MakeShared<FJsonObject>();
				InternalChange->SetStringField(
					TEXT("field_id"),
					FieldId);
				InternalChange->SetStringField(
					TEXT("presence"),
					Presence);
				InternalChange->SetStringField(
					TEXT("field_binding_id"),
					Field->FieldBindingId);
				if (bHasTypedValue)
				{
					FString TypeId;
					const TSharedPtr<FJsonValue> CanonicalValue =
						(*TypedValue)->TryGetField(
							TEXT("canonical_value"));
					if (!(*TypedValue)->TryGetStringField(
							TEXT("type_id"),
							TypeId)
						|| TypeId != Field->TypeId)
					{
						return Fail(
							OutError,
							TEXT("asset_data_type_id_mismatch"),
							FieldId);
					}
					if (!JsonValueMatchesFieldKind(
							CanonicalValue,
							Field->ValueKind))
					{
						return Fail(
							OutError,
							TEXT("asset_data_value_kind_mismatch"),
							FieldId);
					}
					FString Unresolved;
					if (!TryValidateSnapshotReferences(
							CurrentSnapshot,
							CanonicalValue,
							Unresolved))
					{
						return Fail(
							OutError,
							TEXT("asset_data_reference_unresolved"),
							Unresolved);
					}
					const TSharedPtr<FJsonObject> InternalValue =
						MakeShared<FJsonObject>();
					InternalValue->SetStringField(
						TEXT("type_id"),
						Field->TypeId);
					InternalValue->SetField(
						TEXT("canonical_value"),
						CanonicalValue);
					InternalChange->SetObjectField(
						TEXT("value"),
						InternalValue);
				}
				TypedChanges.Add(
					MakeShared<FJsonValueObject>(
						InternalChange));
			}
		}

		const TSharedPtr<FJsonObject> ProposedAfter =
			MakeShared<FJsonObject>();
		ProposedAfter->SetStringField(
			TEXT("native_operation_type"),
			Spec->NativeOperationType);
		ProposedAfter->SetStringField(
			TEXT("native_binding_id"),
			MakeNativeBindingId(*Spec));
		ProposedAfter->SetStringField(
			TEXT("profile_id"),
			Spec->ProfileId);
		ProposedAfter->SetStringField(
			TEXT("target_ref"),
			TargetRef);
		ProposedAfter->SetArrayField(
			TEXT("typed_changes"),
			MoveTemp(TypedChanges));
		if (FactoryPolicy
			&& FactoryRevision.IsSet()
			&& FactoryBinding.GetCandidateBinding())
		{
			ProposedAfter->SetStringField(
				TEXT("factory_binding_id"),
				FactoryBinding.GetCandidateBinding()
					->GetBindingId());
			ProposedAfter->SetStringField(
				TEXT("factory_catalog_source_id"),
				FactoryRevision->GetSourceId());
			ProposedAfter->SetStringField(
				TEXT("factory_catalog_generation"),
				FactoryRevision->GetGeneration());
			ProposedAfter->SetNumberField(
				TEXT("factory_catalog_sequence"),
				static_cast<double>(
					FactoryRevision->GetSequence()));
		}
		if (bClone)
		{
			ProposedAfter->SetStringField(
				TEXT("source_ref"),
				SourceRef);
			ProposedAfter->SetStringField(
				TEXT("source_revision"),
				SourceRevision);
		}

		FUeAgentCanonicalValuePtr CanonicalAfter;
		if (!TryCanonicalValue(
				MakeShared<FJsonValueObject>(ProposedAfter),
				Resources,
				CanonicalAfter,
				OutError))
		{
			return false;
		}

		const FUeAgentCanonicalIdentity Identity(
			TEXT("uai.asset_data:") + TargetRef,
			Spec->NativeOperationType,
			TargetEntry
				? TargetEntry->GetIdentity().GetOwnerId()
				: Context.GetProjectInstanceId(),
			TargetRef,
			TEXT("asset_data"),
			Context.GetContextRevision());
		FChangeDraft Change(
			Identity,
			bCreate || bClone
				? EChangeIntent::Create
				: EChangeIntent::Update,
			MoveTemp(ExpectedBefore),
			MoveTemp(CanonicalAfter),
			GetAssetDataSimpleEffectAuthorityScopes(),
			TEXT("asset-data:typed-change"),
			{});
		if (!Change.IsValid())
		{
			return Fail(
				OutError,
				TEXT("asset_data_change_draft_invalid"));
		}

		FChangeSetDraft Draft(
			Request.GetDescriptorIdentity(),
			Request.GetRequestId(),
			Request.GetIdempotencyKey(),
			CurrentSnapshot,
			{MoveTemp(Change)});
		if (!Draft.IsValid())
		{
			return Fail(
				OutError,
				TEXT("asset_data_change_set_draft_invalid"));
		}
		OutDraft.Emplace(MoveTemp(Draft));
		return true;
	}

	bool TryResolveAssetDataSimpleEffectNativeBinding(
		const FString& NativeBindingId,
		FString& OutNativeOperationType,
		FString& OutProfileId,
		FString& OutNativeMechanicId,
		FString& OutStaticCreateClassPath,
		FString& OutError)
	{
		OutNativeOperationType.Reset();
		OutProfileId.Reset();
		OutNativeMechanicId.Reset();
		OutStaticCreateClassPath.Reset();
		OutError.Reset();
		const FDescriptorSpec* Spec =
			FindSpecByNativeBindingId(NativeBindingId);
		if (!Spec)
		{
			OutError =
				TEXT("asset_data_native_binding_not_registered");
			return false;
		}
		OutNativeOperationType = Spec->NativeOperationType;
		OutProfileId = Spec->ProfileId;
		OutNativeMechanicId = FindNativeMechanicId(*Spec);
		OutStaticCreateClassPath =
			FindStaticCreateClassPath(Spec->CapabilityId);
		return true;
	}

	bool TryResolveAssetDataSimpleEffectFactoryContract(
		const FString& NativeBindingId,
		FString& OutFactoryContractId,
		FString& OutClassConstraintId,
		FString& OutError)
	{
		OutFactoryContractId.Reset();
		OutClassConstraintId.Reset();
		OutError.Reset();
		const FDescriptorSpec* Descriptor =
			FindSpecByNativeBindingId(NativeBindingId);
		if (!Descriptor)
		{
			OutError =
				TEXT("asset_data_native_binding_not_registered");
			return false;
		}
		if (const FFactoryPolicySpec* Policy =
			FindFactoryPolicy(Descriptor->CapabilityId))
		{
			OutFactoryContractId = Policy->FactoryContractId;
			OutClassConstraintId = Policy->ClassConstraintId;
		}
		return true;
	}

	bool TryValidateAssetDataSimpleEffectFactoryBinding(
		const FString& NativeBindingId,
		const FString& FactoryBindingId,
		const FUeAgentCatalogRevision& ExpectedRevision,
		const FUeAgentAssetDataSimpleEffectFactoryBinding& Binding,
		FString& OutError)
	{
		OutError.Reset();
		const FDescriptorSpec* Descriptor =
			FindSpecByNativeBindingId(NativeBindingId);
		const FFactoryPolicySpec* Policy = Descriptor
			? FindFactoryPolicy(Descriptor->CapabilityId)
			: nullptr;
		const FUeAgentCandidateBinding* Candidate =
			Binding.GetCandidateBinding();
		if (!Descriptor || !Policy)
		{
			OutError =
				TEXT("asset_data_factory_contract_not_registered");
			return false;
		}
		if (!Binding.IsValid()
			|| !Candidate
			|| !Candidate->IsValid()
			|| !Candidate->Supports(
				EUeAgentCandidateOperation::Create))
		{
			OutError =
				TEXT("asset_data_factory_candidate_invalid");
			return false;
		}
		if (FactoryBindingId.IsEmpty()
			|| Candidate->GetBindingId() != FactoryBindingId)
		{
			OutError =
				TEXT("asset_data_factory_binding_mismatch");
			return false;
		}
		if (!ExpectedRevision.IsValid()
			|| Candidate->GetCatalogRevision()
				!= ExpectedRevision)
		{
			OutError =
				TEXT("asset_data_factory_candidate_stale");
			return false;
		}
		if (Binding.GetProfileId() != Descriptor->ProfileId)
		{
			OutError =
				TEXT("asset_data_factory_profile_mismatch");
			return false;
		}
		if (Binding.GetFactoryMechanicId()
			!= TEXT("uai.asset_data.factory:new_object:v1"))
		{
			OutError =
				TEXT("asset_data_factory_mechanic_mismatch");
			return false;
		}
		if (!IsFactoryClassAllowed(
				Descriptor->ProfileId,
				Binding.GetConcreteClass()))
		{
			OutError =
				TEXT("asset_data_factory_class_mismatch");
			return false;
		}
		return true;
	}

	bool TryValidateAssetDataSimpleEffectFieldBinding(
		const FString& NativeBindingId,
		const FString& FieldBindingId,
		const FString& FieldId,
		const FString& Presence,
		const FString& TypeId,
		const bool bHasValue,
		const EUeAgentValueKind ValueKind,
		FString& OutError)
	{
		OutError.Reset();
		const FDescriptorSpec* Descriptor =
			FindSpecByNativeBindingId(NativeBindingId);
		const FFieldSpec* Field = Descriptor
			? FindFieldSpec(Descriptor->ProfileId, FieldId)
			: nullptr;
		if (!Descriptor || !Field)
		{
			OutError =
				TEXT("asset_data_native_field_not_registered");
			return false;
		}
		if (FieldBindingId != Field->FieldBindingId)
		{
			OutError =
				TEXT("asset_data_native_field_binding_mismatch");
			return false;
		}
		const TSet<FString> Presences(
			SplitSpecValues(Field->AllowedPresences));
		if (!Presences.Contains(Presence))
		{
			OutError =
				TEXT("asset_data_native_presence_mismatch");
			return false;
		}
		const bool bRequiresValue =
			PresenceRequiresValue(Presence);
		if (bRequiresValue != bHasValue)
		{
			OutError =
				TEXT("asset_data_native_value_presence_mismatch");
			return false;
		}
		if (bHasValue
			&& (TypeId != Field->TypeId
				|| !CanonicalKindMatchesFieldKind(
					ValueKind,
					Field->ValueKind)))
		{
			OutError =
				TEXT("asset_data_native_typed_value_mismatch");
			return false;
		}
		return true;
	}
}
