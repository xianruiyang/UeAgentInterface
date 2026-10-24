#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentDataObjectEligibility.h"

#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
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
#include "Factories/Factory.h"
#include "FileMediaSource.h"
#include "MediaSource.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include <initializer_list>
#include "Sound/SoundEffectSource.h"
#include "Sound/SoundEffectSubmix.h"
#include "Sound/SoundSubmix.h"
#include "SubmixEffects/AudioMixerSubmixEffectEQ.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringAssetIdentity.h"

namespace UE::UAI::DomainAdapters::AssetDataSimpleEffect
{
    namespace
    {
        Contract::FDiagnostic CapabilityFailure(const FString& Code, const FString& Reason, const FString& Field,
            const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected,
            const TCHAR* SourceKind = TEXT("command_input"), const TCHAR* Stage = TEXT("execution"))
        {
            auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Field, Expected, Actual, Stage, Reason);
            Error.SourceKind = SourceKind;
            Error.ActualValuePresent = Actual.IsValid();
            Error.ExpectedValuePresent = Expected.IsValid();
            if (Error.SourceKind == TEXT("native_result"))
            {
                Error.Source = {};
                Error.SemanticPointer.Reset();
            }
            return Error;
        }

        Contract::FDiagnostic RegistryFailure(const FString& Code, const FString& Reason, const FString& Field,
            const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
        {
            return CapabilityFailure(Code, Reason, Field, Actual, Expected, TEXT("native_result"), TEXT("registry"));
        }

        Contract::FDiagnostic InternalRegistryFailure(const FString& Code, const FString& Reason,
            const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
        {
            auto Error = RegistryFailure(Code, Reason, FString(), nullptr, nullptr);
            Error.ActualEvidence = Actual;
            Error.ExpectedEvidence = Expected;
            return Error;
        }

        TSharedPtr<FJsonValue> Facts(std::initializer_list<TPair<FString, TSharedPtr<FJsonValue>>> Fields)
        {
            const auto Object = MakeShared<FJsonObject>();
            for (const auto& Field : Fields) Object->SetField(Field.Key, Field.Value);
            return MakeShared<FJsonValueObject>(Object);
        }

        void MarkNativeFailure(Contract::FDiagnostic& Error)
        {
            Error.SourceKind = TEXT("native_result");
            if (Error.Source.InternalJsonPath.IsEmpty())
                Error.Source.InternalJsonPath = Error.SemanticPointer.IsEmpty() ? Error.Source.JsonPath : Error.SemanticPointer;
            static_cast<Contract::FPublicSourceLocation&>(Error.Source) = {};
            Error.SemanticPointer.Reset();
            for (auto& Child : Error.Diagnostics) MarkNativeFailure(Child);
            if (Error.PublicCause)
            {
                auto Cause = MakeShared<Contract::FDiagnostic>(*Error.PublicCause);
                MarkNativeFailure(*Cause);
                Error.PublicCause = MoveTemp(Cause);
            }
        }

        TSharedPtr<FJsonValue> CatalogRevisionFact(const Discovery::FUeAgentCatalogRevision& Revision)
        {
            return Facts({{TEXT("source_id"), MakeShared<FJsonValueString>(Revision.GetSourceId())},
                {TEXT("generation"), MakeShared<FJsonValueString>(Revision.GetGeneration())},
                {TEXT("sequence"), MakeShared<FJsonValueNumberString>(::LexToString(Revision.GetSequence()))}});
        }

        const TCHAR* CanonicalKindName(const StateSemantic::EUeAgentValueKind Kind)
        {
            switch (Kind)
            {
            case StateSemantic::EUeAgentValueKind::Null: return TEXT("null");
            case StateSemantic::EUeAgentValueKind::Boolean: return TEXT("boolean");
            case StateSemantic::EUeAgentValueKind::String: return TEXT("string");
            case StateSemantic::EUeAgentValueKind::Integer: return TEXT("integer");
            case StateSemantic::EUeAgentValueKind::Decimal: return TEXT("decimal");
            case StateSemantic::EUeAgentValueKind::Array: return TEXT("array");
            case StateSemantic::EUeAgentValueKind::Object: return TEXT("object");
            case StateSemantic::EUeAgentValueKind::Temporal: return TEXT("temporal");
            case StateSemantic::EUeAgentValueKind::Spatial: return TEXT("spatial");
            }
            return TEXT("unknown");
        }

        TSharedPtr<FJsonValue> ClassFact(const UClass* Class)
        {
            return Class ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Class->GetPathName())) : nullptr;
        }
    }

	bool CollectRegisteredDataObjectFields(
		const UClass* ConcreteClass, TMap<FString, FProperty*>& OutFields, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutFields.Reset();
		OutError.Reset();
		if (!ConcreteClass || !ConcreteClass->IsChildOf(UDataAsset::StaticClass())
			|| ConcreteClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			OutError = RegistryFailure(TEXT("asset_data_registered_class_invalid"),
                !ConcreteClass ? TEXT("The registered data object class is missing.")
                    : !ConcreteClass->IsChildOf(UDataAsset::StaticClass()) ? TEXT("The registered class is not a DataAsset.")
                    : ConcreteClass->HasAnyClassFlags(CLASS_Abstract) ? TEXT("The registered data object class is abstract.")
                    : ConcreteClass->HasAnyClassFlags(CLASS_Deprecated) ? TEXT("The registered data object class is deprecated.")
                    : TEXT("The registered data object class has been superseded."),
                TEXT("class"), ClassFact(ConcreteClass), MakeShared<FJsonValueString>(TEXT("concrete current DataAsset class")));
			return false;
		}
		for (TFieldIterator<FProperty> It(ConcreteClass); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property->HasMetaData(TEXT("UAIAssetDataField"))) continue;
			const FString PublicId = Property->GetMetaData(TEXT("UAIAssetDataField"));
			if (PublicId.IsEmpty() || OutFields.Contains(PublicId))
			{
                const FProperty* ExistingProperty = OutFields.FindRef(PublicId);
                const FString ExistingName = ExistingProperty ? ExistingProperty->GetName() : FString();
                OutFields.Reset();
                OutError = RegistryFailure(TEXT("asset_data_registered_field_metadata_invalid"),
                    PublicId.IsEmpty() ? TEXT("The registered field metadata has an empty public id.") : TEXT("Two registered properties use the same public field id."),
                    Property->GetName() + TEXT("/UAIAssetDataField"), Facts({{TEXT("public_id"), MakeShared<FJsonValueString>(PublicId)},
                        {TEXT("existing_property"), MakeShared<FJsonValueString>(ExistingName)}}),
                    MakeShared<FJsonValueString>(TEXT("nonempty unique public field id")));
                OutError.TargetPath = ConcreteClass->GetPathName();
				return false;
			}
			OutFields.Add(PublicId, Property);
		}
		if (OutFields.IsEmpty())
		{
			OutError = RegistryFailure(TEXT("asset_data_registered_fields_missing"),
                TEXT("The data object class has no registered authoring fields."), TEXT("UAIAssetDataField"),
                MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one registered property")));
            OutError.TargetPath = ConcreteClass->GetPathName();
			return false;
		}
		return true;
	}

	bool IsEligibleDataObjectClass(const UClass* ConcreteClass)
	{
		TMap<FString, FProperty*> Fields;
		UE::UAI::Contract::FDiagnostic Error;
		return CollectRegisteredDataObjectFields(ConcreteClass, Fields, Error);
	}

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
			{ TEXT("uai.operation.asset.object.create"), TEXT("asset.object.create"), TEXT("generic_default_create"), TEXT("asset.object.create"), TEXT(""), TEXT("") },
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
				TEXT("asset.object.create"),
				TEXT(
					"uai.asset_data.factory_contract:"
					"generic_default_create:v1"),
				TEXT(
					"uai.asset_data.class_constraint:"
					"registered_asset_constructor:v1"),
			},
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
			if (ProfileId == TEXT("generic_default_create"))
			{
				return ConcreteClass->IsChildOf(UObject::StaticClass());
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
				return IsEligibleDataObjectClass(ConcreteClass);
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
			if (FString(Spec.CapabilityId)
				== TEXT("asset.object.create"))
			{
				return TEXT(
					"uai.asset_data.mechanic:"
					"create_native_factory_asset:v1");
			}
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

		bool Fail(FExecutionMutationError& OutError, FExecutionMutationError Diagnostic,
            const FString& PropertyPath = FString())
        {
            OutError = MoveTemp(Diagnostic);
            if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = PropertyPath;
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

        TSharedPtr<FJsonValue> SpecValuesFact(const TCHAR* Source)
        {
            TArray<TSharedPtr<FJsonValue>> Values;
            for (const auto& Value : SplitSpecValues(Source)) Values.Add(MakeShared<FJsonValueString>(Value));
            return MakeShared<FJsonValueArray>(Values);
        }

        const TCHAR* FieldKindName(const EFieldValueKind Kind)
        {
            switch (Kind)
            {
            case EFieldValueKind::String: return TEXT("string");
            case EFieldValueKind::Boolean: return TEXT("boolean");
            case EFieldValueKind::Integer: return TEXT("integer");
            case EFieldValueKind::Number: return TEXT("number");
            case EFieldValueKind::Array: return TEXT("array");
            case EFieldValueKind::Object: return TEXT("object");
            }
            return TEXT("registered value kind");
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

		TSharedPtr<FJsonObject> GenericFactorySelectorSchema()
		{
			const TSharedPtr<FJsonObject> SelectProperties = MakeShared<FJsonObject>();
			SelectProperties->SetStringField(TEXT("type"), TEXT("object"));
			SelectProperties->SetBoolField(TEXT("additionalProperties"), true);
			const TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
			Properties->SetObjectField(TEXT("name"), StringSchema());
			Properties->SetObjectField(TEXT("select"), SelectProperties);
			return ObjectSchema(Properties, {TEXT("name")});
		}

		TSharedPtr<FJsonObject> BuildInputSchema(
			const FDescriptorSpec& Spec)
		{
			const TSharedPtr<FJsonObject> Properties =
				MakeShared<FJsonObject>();
			Properties->SetObjectField(TEXT("target_ref"), StringSchema());
			TArray<FString> Required{TEXT("target_ref")};

			const FString NativeType(Spec.NativeOperationType);
			if (FString(Spec.CapabilityId)
				== TEXT("asset.object.create"))
			{
				const TSharedPtr<FJsonObject> FactorySchema = MakeShared<FJsonObject>();
				FactorySchema->SetArrayField(TEXT("oneOf"), {
					MakeShared<FJsonValueObject>(StringSchema()),
					MakeShared<FJsonValueObject>(GenericFactorySelectorSchema())});
				Properties->SetObjectField(
					TEXT("factory"),
					FactorySchema);
				Required.Add(TEXT("factory"));
			}
			else if (NativeType == TEXT("asset.object.create")
				&& FindFactoryPolicy(Spec.CapabilityId))
			{
				Properties->SetObjectField(
					TEXT("factory"),
					GenericFactorySelectorSchema());
				Required.Add(TEXT("factory"));
			}
			else if (NativeType == TEXT("asset.object.clone"))
			{
				Properties->SetObjectField(
					TEXT("source_ref"),
					StringSchema());
				Properties->SetObjectField(
					TEXT("destructive_authorization_id"),
					StringSchema());
				Required.Add(TEXT("source_ref"));
			}
			else if (NativeType != TEXT("asset.object.create"))
			{
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
			UE::UAI::Contract::FDiagnostic& OutError)
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
			UE::UAI::Contract::FDiagnostic& OutError)
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
			if (!OutHash.IsEmpty()) return true;
            OutError = RegistryFailure(TEXT("asset_data_descriptor_hash_missing"),
                TEXT("The canonical descriptor did not produce a hash."), TEXT("descriptor"),
                MakeShared<FJsonValueString>(Spec.DescriptorId), MakeShared<FJsonValueString>(TEXT("nonempty descriptor hash")));
            return false;
		}

		const FDescriptorSpec* FindSpec(
			const FUeAgentDescriptorIdentity& Identity, FExecutionMutationError& OutError)
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
                Contract::FDiagnostic Error;
                if (!TryBuildDescriptorHash(Spec, Input, Result, Hash, Error))
                {
                    if (OutError.IsEmpty()) OutError = MoveTemp(Error);
                    continue;
                }
                if (Hash == Identity.GetDescriptorHash())
                {
                    OutError.Reset();
                    return &Spec;
                }
            }
            if (OutError.IsEmpty())
                OutError = InternalRegistryFailure(TEXT("asset_data_descriptor_not_exact"),
                    TEXT("The requested descriptor identity does not exactly match a registered asset operation."),
                    Facts({{TEXT("descriptor_id"), MakeShared<FJsonValueString>(Identity.GetDescriptorId())},
                        {TEXT("version"), MakeShared<FJsonValueNumber>(Identity.GetVersion())},
                        {TEXT("hash"), MakeShared<FJsonValueString>(Identity.GetDescriptorHash())}}),
                    MakeShared<FJsonValueString>(TEXT("registered descriptor id, version, and hash")));
			return nullptr;
		}

		bool TryParsePayload(
			const FUeAgentOperationRequest& Request,
			TSharedPtr<FJsonObject>& OutPayload,
			FExecutionMutationError& OutError)
		{
			UE::UAI::Contract::FDiagnostic ParseError;
			if (!Contract::Json::TryParseObject(
				Request.GetPayload().GetJsonValue(), OutPayload, ParseError))
			{
				return Fail(OutError, ParseError);
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

        bool TryReadSnapshotString(const FUeAgentCanonicalValueRef& Value, const FString& Field,
            FString& OutText, const FString& TargetRef, const FString& Code,
            FExecutionMutationError& OutError)
        {
            const bool bObject = Value->GetKind() == EUeAgentValueKind::Object;
            const FUeAgentCanonicalValue* Actual = bObject ? nullptr : &Value.Get();
            if (bObject)
                for (const auto& Item : Value->GetObjectItems())
                    if (Item.Key == Field) { Actual = &Item.Value.Get(); break; }
            if (bObject && Actual && Actual->GetKind() == EUeAgentValueKind::String)
            {
                OutText = Actual->GetText();
                return true;
            }
            OutError = CapabilityFailure(Code,
                !bObject ? TEXT("The captured asset snapshot is not an object.")
                    : !Actual ? TEXT("The captured asset snapshot is missing a required string field.")
                    : TEXT("The captured asset snapshot field is not a string."),
                bObject ? Field : FString(), nullptr, MakeShared<FJsonValueString>(bObject ? TEXT("string") : TEXT("object")), TEXT("native_result"));
            OutError.TargetPath = TargetRef;
            OutError.ActualValuePresent = Actual != nullptr;
            Contract::FDiagnostic ProjectionError;
            if (Actual && !CanonicalJson::TryToJsonValue(*Actual, OutError.ActualValue, ProjectionError))
            {
                MarkNativeFailure(ProjectionError);
                OutError.Diagnostics.Add(MoveTemp(ProjectionError));
            }
            return false;
        }

		bool TryCanonicalValue(
			const TSharedPtr<FJsonValue>& JsonValue,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			if (!JsonValue.IsValid())
				return Fail(OutError, CapabilityFailure(TEXT("asset_data_canonical_value_missing"), TEXT("The generated change has no canonical value."),
                    FString(), nullptr, MakeShared<FJsonValueString>(TEXT("canonical value")), TEXT("native_result")));
            if (CanonicalJson::TryFromJsonValue(JsonValue, Resources, OutValue, OutError)) return true;
            MarkNativeFailure(OutError);
            return false;
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
				{
					FString Lexeme;
					return CanonicalJson::TryGetNumberLexeme(*Value, Lexeme)
						&& IsIntegralNumberLexeme(Lexeme);
				}
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
				// Integrality is checked from the value lexeme in planning/native reads.
				return CanonicalKind == EUeAgentValueKind::Integer
					|| CanonicalKind == EUeAgentValueKind::Decimal;
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

        bool TryValidateSnapshotReferences(const FUeAgentCanonicalSnapshotRef& Snapshot,
            const TSharedPtr<FJsonValue>& Value, const FString& Pointer, FExecutionMutationError& OutError)
        {
            if (!Value.IsValid()) return true;
            if (Value->Type == EJson::String)
            {
                const FString Text = Value->AsString();
                if (Text.StartsWith(TEXT("uai://asset/")) && !FindEntry(Snapshot, Text))
                    return Fail(OutError, CapabilityFailure(TEXT("asset_data_reference_unresolved"),
                        TEXT("The referenced asset is absent from the captured snapshot."), Pointer,
                        MakeShared<FJsonValueString>(Text), MakeShared<FJsonValueString>(TEXT("asset reference present in the snapshot"))));
                return true;
            }
            if (Value->Type == EJson::Array)
            {
                for (int32 Index = 0; Index < Value->AsArray().Num(); ++Index)
                    if (!TryValidateSnapshotReferences(Snapshot, Value->AsArray()[Index],
                        Pointer + TEXT("/") + ::LexToString(Index), OutError)) return false;
            }
            else if (Value->Type == EJson::Object)
            {
                for (const auto& Pair : Value->AsObject()->Values)
                    if (!TryValidateSnapshotReferences(Snapshot, Pair.Value,
                        Pointer + TEXT("/") + Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")), OutError)) return false;
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
		: FUeAgentAssetDataSimpleEffectFactoryBinding(
			MoveTemp(InCandidateBinding),
			MoveTemp(InProfileId),
			InConcreteClass,
			InFactoryMechanicId,
			nullptr,
			InFactoryMechanicId
				== TEXT(
					"uai.asset_data.factory:asset_tools_create:v1"))
	{
	}

	FUeAgentAssetDataSimpleEffectFactoryBinding::
		FUeAgentAssetDataSimpleEffectFactoryBinding(
			FUeAgentCandidateBinding InCandidateBinding,
			FString InProfileId,
			UClass* InConcreteClass,
			FString InFactoryMechanicId,
			UFactory* InFactory,
			const bool bInGenericReady)
		: CandidateBinding(
			MakeShared<const FUeAgentCandidateBinding>(
				MoveTemp(InCandidateBinding)))
		, ProfileId(MoveTemp(InProfileId))
		, ConcreteClass(InConcreteClass)
		, FactoryMechanicId(MoveTemp(InFactoryMechanicId))
		, Factory(InFactory)
		, bGenericReady(bInGenericReady)
	{
		ProfileId.TrimStartAndEndInline();
		FactoryMechanicId.TrimStartAndEndInline();
		bValid = CandidateBinding.IsValid()
			&& CandidateBinding->IsValid()
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

	UFactory*
	FUeAgentAssetDataSimpleEffectFactoryBinding::GetFactory() const
	{
		return Factory.Get();
	}

	bool FUeAgentAssetDataSimpleEffectFactoryBinding::
		IsGenericReady() const
	{
		return bGenericReady
			&& CandidateBinding.IsValid()
			&& CandidateBinding->Supports(
				EUeAgentCandidateOperation::Create);
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
		UE::UAI::Contract::FDiagnostic& OutError)
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
		return FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(Descriptors),
			OutRegistry,
			OutError);
	}

	bool TryGetAssetDataSimpleEffectProductionProfiles(
		TArray<FUeAgentAssetDataSimpleEffectProductionProfile>&
			OutProfiles,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutProfiles.Reset();
		OutError.Reset();
		TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
        if (!TryCreateAssetDataSimpleEffectDescriptorRegistry(Registry, OutError)) return false;
        if (!Registry.IsValid())
        {
            OutError = RegistryFailure(TEXT("asset_data_production_registry_missing"),
                TEXT("Descriptor registration returned no registry."), TEXT("descriptor_registry"), nullptr,
                MakeShared<FJsonValueString>(TEXT("registered asset operations")));
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
				OutError = InternalRegistryFailure(TEXT("asset_data_production_descriptor_missing"),
                    TEXT("A declared asset operation has no registered descriptor."), MakeShared<FJsonValueString>(Spec.DescriptorId),
                    MakeShared<FJsonValueString>(TEXT("descriptor present in registry")));
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
					OutError = RegistryFailure(TEXT("asset_data_production_field_missing"),
                        TEXT("The operation profile names a field without a registered field specification."), FieldId,
                        MakeShared<FJsonValueString>(FieldId), MakeShared<FJsonValueString>(TEXT("registered field specification")));
                    OutError.InternalContext = Spec.ProfileId;
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
					const FString InvalidField = ProductionField.FieldId.IsEmpty() ? TEXT("field_id")
                        : ProductionField.FieldBindingId.IsEmpty() ? TEXT("field_binding_id")
                        : ProductionField.TypeId.IsEmpty() ? TEXT("type_id")
                        : ProductionField.ValueKind == EUeAgentValueKind::Null ? TEXT("value_kind")
                        : ProductionField.AllowedPresences.IsEmpty() ? TEXT("allowed_presences") : TEXT("reference_policy");
                    OutError = RegistryFailure(TEXT("asset_data_production_field_invalid"),
                        TEXT("The registered field specification is missing required metadata."), FieldId + TEXT("/") + InvalidField,
                        InvalidField == TEXT("value_kind") ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("null")))
                            : InvalidField == TEXT("allowed_presences") ? SpecValuesFact(Field->AllowedPresences)
                            : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT(""))),
                        MakeShared<FJsonValueString>(InvalidField == TEXT("value_kind") ? TEXT("non-null value kind")
                            : InvalidField == TEXT("allowed_presences") ? TEXT("nonempty allowed presence list") : TEXT("nonempty metadata value")));
                    OutError.InternalContext = Spec.ProfileId;
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
					&& !Profile.FieldIds.IsEmpty()))
			{
				OutProfiles.Reset();
                const FString InvalidField = Profile.DescriptorId.IsEmpty() ? TEXT("descriptor_id")
                    : Profile.DescriptorVersion <= 0 ? TEXT("descriptor_version")
                    : Profile.DescriptorHash.IsEmpty() ? TEXT("descriptor_hash")
                    : Profile.CapabilityId.IsEmpty() ? TEXT("capability_id")
                    : Profile.ProfileId.IsEmpty() ? TEXT("profile_id")
                    : Profile.NativeOperationType.IsEmpty() ? TEXT("native_operation_type")
                    : Profile.NativeBindingId.IsEmpty() ? TEXT("native_binding_id")
                    : Profile.NativeMechanicId.IsEmpty() ? TEXT("native_mechanic_id") : TEXT("field_ids");
                OutError = RegistryFailure(TEXT("asset_data_production_profile_invalid"),
                    !Profile.IsValid() ? TEXT("The registered operation profile is missing required metadata.")
                        : TEXT("A creation profile cannot also declare field update units."),
                    InvalidField,
                    InvalidField == TEXT("descriptor_version") ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Profile.DescriptorVersion))
                        : InvalidField == TEXT("field_ids") ? SpecValuesFact(Spec.FieldIds) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT(""))),
                    MakeShared<FJsonValueString>(InvalidField == TEXT("descriptor_version") ? TEXT("positive version")
                        : InvalidField == TEXT("field_ids") ? TEXT("no field update units") : TEXT("nonempty metadata")));
                OutError.InternalContext = Spec.DescriptorId;
				return false;
			}
			OutProfiles.Add(MoveTemp(Profile));
		}
		return true;
	}

	bool IsGenericAssetObjectCreateSuccessor(
		const FUeAgentAssetDataSimpleEffectProductionProfile& Profile)
	{
		if (!Profile.IsCreate()
			|| Profile.CapabilityId == TEXT("asset.object.create"))
		{
			return false;
		}
		if (Profile.UsesStaticCreateClass())
		{
			return true;
		}
		return Profile.ProfileId == TEXT("audio_effect_preset")
			|| Profile.ProfileId == TEXT("data_object")
			|| Profile.ProfileId == TEXT("sound_submix")
			|| Profile.ProfileId == TEXT("media_source");
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
        OutError = InternalRegistryFailure(TEXT("asset_data_field_projection_unsupported"),
            TEXT("The asset operation does not support field projection."),
            MakeShared<FJsonValueString>(NativeMechanicId), MakeShared<FJsonValueString>(TEXT("registered field projection mechanic")));
        OutError.Stage = TEXT("execution");
        return false;
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
        OutError = InternalRegistryFailure(TEXT("asset_data_field_projection_unsupported"),
            TEXT("The asset operation does not support field projection."),
            MakeShared<FJsonValueString>(NativeMechanicId), MakeShared<FJsonValueString>(TEXT("registered field projection mechanic")));
        OutError.Stage = TEXT("execution");
        return false;
	}

	void LocateAssetDataSimpleEffectInputFailure(Contract::FDiagnostic& Error,
		const StateSemantic::FUeAgentCanonicalValue& ProposedAfter)
	{
		const auto FindField = [](const StateSemantic::FUeAgentCanonicalValue& Object, const FString& Key)
			-> const StateSemantic::FUeAgentCanonicalValue*
		{
			if (Object.GetKind() != StateSemantic::EUeAgentValueKind::Object) return nullptr;
			for (const auto& Item : Object.GetObjectItems())
				if (Item.Key == Key) return &Item.Value.Get();
			return nullptr;
		};
		const auto* Changes = FindField(ProposedAfter, TEXT("typed_changes"));
		if (!Changes || Changes->GetKind() != StateSemantic::EUeAgentValueKind::Array) return;
		if (Error.SourceKind == TEXT("authored_value") && Error.Source.SourceFile.IsEmpty()
			&& Error.Source.JsonPath.IsEmpty() && Error.SemanticPointer.IsEmpty())
		{
			int32 MatchedIndex = INDEX_NONE;
			FString Suffix;
			bool bHasCanonicalValue = false;
			for (int32 Index = 0; Index < Changes->GetArrayItems().Num(); ++Index)
			{
				const auto& Unit = Changes->GetArrayItems()[Index].Get();
				const auto* FieldId = FindField(Unit, TEXT("field_id"));
				if (!FieldId || FieldId->GetKind() != StateSemantic::EUeAgentValueKind::String) continue;
				const FString& Field = FieldId->GetText();
				if (Error.PropertyPath != Field && !Error.PropertyPath.StartsWith(Field + TEXT("/"))) continue;
				if (MatchedIndex != INDEX_NONE) { MatchedIndex = INDEX_NONE; break; }
				MatchedIndex = Index;
				Suffix = Error.PropertyPath.Mid(Field.Len());
				const auto* Value = FindField(Unit, TEXT("value"));
				bHasCanonicalValue = Value && FindField(*Value, TEXT("canonical_value"));
			}
			if (MatchedIndex != INDEX_NONE)
			{
				// Draft construction copies exactly one typed change per input change, in order.
				const FString Pointer = TEXT("/changes/") + ::LexToString(MatchedIndex)
					+ (bHasCanonicalValue ? TEXT("/value/canonical_value") + Suffix : FString(TEXT("/presence")));
				Error.SourceKind = TEXT("command_input");
				Error.SemanticPointer = Pointer;
				Error.Source.JsonPath = Pointer;
				Error.Source.bCommandInput = true;
			}
		}
		for (Contract::FDiagnostic& Child : Error.Diagnostics)
			LocateAssetDataSimpleEffectInputFailure(Child, ProposedAfter);
		if (Error.PublicCause.IsValid())
		{
			auto Cause = MakeShared<Contract::FDiagnostic>(*Error.PublicCause);
			LocateAssetDataSimpleEffectInputFailure(*Cause, ProposedAfter);
			Error.PublicCause = Cause;
		}
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
        if (!Request.IsValid()) return Fail(OutError, Request.GetValidationDiagnostic());
        const FString MissingContextField = Context.GetProjectInstanceId().IsEmpty() ? TEXT("project_instance_id")
            : Context.GetEditorInstanceId().IsEmpty() ? TEXT("editor_instance_id")
            : Context.GetContextRevision().IsEmpty() ? TEXT("context_revision") : FString();
        if (!MissingContextField.IsEmpty())
            return Fail(OutError, CapabilityFailure(TEXT("asset_data_draft_request_invalid"), TEXT("The execution context is missing a required identity or revision."), MissingContextField, MakeShared<FJsonValueString>(TEXT("")), MakeShared<FJsonValueString>(TEXT("nonempty context value")), TEXT("native_result")));
        if (CurrentSnapshot->GetStatePlane() != EUeAgentStatePlane::AuthoringMemory)
            return Fail(OutError, CapabilityFailure(TEXT("asset_data_draft_request_invalid"), TEXT("Asset changes require an authoring-memory snapshot."), TEXT("state_plane"), MakeShared<FJsonValueNumber>(static_cast<int32>(CurrentSnapshot->GetStatePlane())), MakeShared<FJsonValueNumber>(static_cast<int32>(EUeAgentStatePlane::AuthoringMemory)), TEXT("native_result")));

        const FDescriptorSpec* Spec = FindSpec(Request.GetDescriptorIdentity(), OutError);
        if (!Spec) return false;

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
			return Fail(OutError, CapabilityFailure(TEXT("asset_data_target_ref_missing"), TEXT("The asset target reference must be a nonempty string."), TEXT("/target_ref"), Payload->TryGetField(TEXT("target_ref")), MakeShared<FJsonValueString>(TEXT("nonempty asset path"))));
		}
		// Match the resolved snapshot identity for either public path spelling.
		TargetRef = UeAgentAuthoringAssetIdentity::ToObjectPath(TargetRef);

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
				return Fail(OutError, CapabilityFailure(TEXT("asset_data_target_already_exists"), TEXT("The requested creation target already exists in the captured snapshot."), TEXT("/target_ref"), MakeShared<FJsonValueString>(TargetRef), MakeShared<FJsonValueString>(TEXT("unused asset path"))));
			}
			if (bClone)
			{
				Payload->TryGetStringField(
					TEXT("source_ref"),
					SourceRef);
				SourceRef = UeAgentAuthoringAssetIdentity::ToObjectPath(SourceRef);
				SourceEntry =
					FindEntry(CurrentSnapshot, SourceRef);
				if (!SourceEntry)
				{
					return Fail(OutError, CapabilityFailure(TEXT("asset_data_source_snapshot_missing"), TEXT("The clone source is absent from the captured snapshot."), TEXT("/source_ref"), Payload->TryGetField(TEXT("source_ref")), MakeShared<FJsonValueString>(TEXT("asset present in the snapshot"))));
				}
                FString ActualRevision;
                if (!TryReadSnapshotString(SourceEntry->GetValue(), TEXT("asset_revision"), ActualRevision, SourceRef,
                    TEXT("asset_data_source_revision_mismatch"), OutError)) return false;
                if (ActualRevision.IsEmpty())
                {
                    OutError = CapabilityFailure(TEXT("asset_data_source_revision_mismatch"), TEXT("The captured clone source has an empty asset revision."), TEXT("asset_revision"), MakeShared<FJsonValueString>(ActualRevision), MakeShared<FJsonValueString>(TEXT("nonempty captured revision")), TEXT("native_result"));
                    OutError.TargetPath = SourceRef;
                    return false;
                }
				SourceRevision = ActualRevision;
			}
		}
		else
		{
			if (!TargetEntry)
			{
				return Fail(OutError, CapabilityFailure(TEXT("asset_data_target_snapshot_missing"), TEXT("The requested target is absent from the captured snapshot."), TEXT("/target_ref"), MakeShared<FJsonValueString>(TargetRef), MakeShared<FJsonValueString>(TEXT("asset present in the snapshot"))));
			}
            FString ActualRevision;
            if (!TryReadSnapshotString(TargetEntry->GetValue(), TEXT("asset_revision"), ActualRevision, TargetRef,
                TEXT("asset_data_target_revision_mismatch"), OutError)) return false;
            if (ActualRevision.IsEmpty())
            {
                OutError = CapabilityFailure(TEXT("asset_data_target_revision_mismatch"), TEXT("The captured target has an empty asset revision."), TEXT("asset_revision"), MakeShared<FJsonValueString>(ActualRevision), MakeShared<FJsonValueString>(TEXT("nonempty captured revision")), TEXT("native_result"));
                OutError.TargetPath = TargetRef;
                return false;
            }
            FString ActualProfile;
            if (!TryReadSnapshotString(TargetEntry->GetValue(), TEXT("profile_id"), ActualProfile, TargetRef,
                TEXT("asset_data_profile_mismatch"), OutError)) return false;
            if (ActualProfile != Spec->ProfileId)
            {
                OutError = InternalRegistryFailure(TEXT("asset_data_profile_mismatch"),
                    TEXT("The captured target profile does not match the requested operation."), MakeShared<FJsonValueString>(ActualProfile), MakeShared<FJsonValueString>(Spec->ProfileId));
                OutError.TargetPath = TargetRef;
                return false;
            }
			ExpectedBefore =
				FUeAgentCanonicalValuePtr(
					TargetEntry->GetValue());
		}

		FUeAgentAssetDataSimpleEffectFactoryBinding FactoryBinding;
		TOptional<FUeAgentCatalogRevision> FactoryRevision;
		if (FactoryPolicy)
		{
			FString FactoryName;
			TMap<FString, FString> FactorySelect;
			UE::UAI::Contract::FDiagnostic SelectorError;
			const bool bGenericSelector =
				FString(Spec->CapabilityId)
					== TEXT("asset.object.create");
			if (!TryParseAssetDataSimpleEffectFactorySelector(
					*Payload,
					bGenericSelector,
					FactoryName,
					FactorySelect,
					SelectorError))
			{
				return Fail(OutError, SelectorError, Spec->ProfileId);
			}
			if (!FactoryCandidates.TryResolveForPlanning(
					FactoryName,
					FactorySelect,
					Spec->ProfileId,
					FactoryBinding,
					OutError))
			{
				return false;
			}
			UE::UAI::Contract::FDiagnostic FactoryError;
			if (!TryValidateAssetDataSimpleEffectFactoryBinding(
					MakeNativeBindingId(*Spec),
					FactoryBinding.GetCandidateBinding()
						? FactoryBinding.GetCandidateBinding()
							->GetBindingId()
						: FString(),
					FactoryBinding.GetCandidateBinding()
						? FactoryBinding.GetCandidateBinding()->GetCatalogRevision()
						: FUeAgentCatalogRevision(TEXT("invalid"), TEXT("invalid"), -1),
					FactoryBinding,
					FactoryError))
			{
				return Fail(OutError, FactoryError, Spec->ProfileId);
			}
			FactoryRevision.Emplace(FactoryBinding.GetCandidateBinding()
				? FactoryBinding.GetCandidateBinding()->GetCatalogRevision()
				: FUeAgentCatalogRevision(TEXT("invalid"), TEXT("invalid"), -1));
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
				return Fail(OutError, CapabilityFailure(TEXT("asset_data_changes_missing"), TEXT("The operation requires a changes array."), TEXT("/changes"), Payload->TryGetField(TEXT("changes")), MakeShared<FJsonValueString>(TEXT("array"))));
			}
			const TSet<FString> RegisteredFields(
				SplitSpecValues(Spec->FieldIds));
			TSet<FString> SeenFields;
            for (int32 ChangeIndex = 0; ChangeIndex < Changes->Num(); ++ChangeIndex)
            {
                const auto& ChangeValue = (*Changes)[ChangeIndex];
                const FString ChangePointer = TEXT("/changes/") + ::LexToString(ChangeIndex);
                const TSharedPtr<FJsonObject> Change = ChangeValue.IsValid() && ChangeValue->Type == EJson::Object
                    ? ChangeValue->AsObject() : nullptr;
				FString FieldId;
				FString Presence;
				if (!Change.IsValid()
					|| !Change->TryGetStringField(
						TEXT("field_id"),
						FieldId)
					|| !RegisteredFields.Contains(FieldId))
				{
					return Fail(OutError, CapabilityFailure(TEXT("asset_data_field_not_registered"), TEXT("The change must name a field registered for this operation."), Change.IsValid() ? ChangePointer + TEXT("/field_id") : ChangePointer, Change.IsValid() ? Change->TryGetField(TEXT("field_id")) : ChangeValue, SpecValuesFact(Spec->FieldIds)));
				}
				const FFieldSpec* Field =
					FindFieldSpec(Spec->ProfileId, FieldId);
				if (!Field)
				{
					return Fail(OutError, RegistryFailure(TEXT("asset_data_field_binding_missing"), TEXT("The registered operation field has no field binding."), FieldId, MakeShared<FJsonValueString>(FieldId), MakeShared<FJsonValueString>(TEXT("registered field binding"))));
				}
				if (!Change->TryGetStringField(
						TEXT("presence"),
						Presence))
				{
					return Fail(OutError, CapabilityFailure(TEXT("asset_data_explicit_presence_required"), TEXT("Each field change requires an explicit presence string."), ChangePointer + TEXT("/presence"), Change->TryGetField(TEXT("presence")), SpecValuesFact(Field->AllowedPresences)));
				}
				const TSet<FString> AllowedPresences(
					SplitSpecValues(Field->AllowedPresences));
				if (!AllowedPresences.Contains(Presence))
				{
					return Fail(OutError, CapabilityFailure(TEXT("asset_data_presence_not_allowed"), TEXT("The requested presence is not supported for this field."), ChangePointer + TEXT("/presence"), MakeShared<FJsonValueString>(Presence), SpecValuesFact(Field->AllowedPresences)));
				}
				if (SeenFields.Contains(FieldId))
				{
					return Fail(OutError, CapabilityFailure(TEXT("asset_data_duplicate_field_unit"), TEXT("A field may occur only once in the changes array."), ChangePointer + TEXT("/field_id"), MakeShared<FJsonValueString>(FieldId), MakeShared<FJsonValueString>(TEXT("unique field id"))));
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
					return Fail(OutError, CapabilityFailure(TEXT("asset_data_value_presence_mismatch"), TEXT("The typed value presence does not match the requested field presence."), ChangePointer + TEXT("/value"), Change->TryGetField(TEXT("value")), MakeShared<FJsonValueString>(bRequiresValue ? TEXT("typed value object") : TEXT("no typed value object"))));
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
						return Fail(OutError, CapabilityFailure(TEXT("asset_data_type_id_mismatch"), TEXT("The typed value uses a different type from the registered field."), ChangePointer + TEXT("/value/type_id"), (*TypedValue)->TryGetField(TEXT("type_id")), MakeShared<FJsonValueString>(Field->TypeId)));
					}
					if (!JsonValueMatchesFieldKind(
							CanonicalValue,
							Field->ValueKind))
					{
						return Fail(OutError, CapabilityFailure(TEXT("asset_data_value_kind_mismatch"), TEXT("The canonical value does not have the registered field kind."), ChangePointer + TEXT("/value/canonical_value"), CanonicalValue, MakeShared<FJsonValueString>(FieldKindName(Field->ValueKind))));
					}
                    if (!TryValidateSnapshotReferences(CurrentSnapshot, CanonicalValue,
                        ChangePointer + TEXT("/value/canonical_value"), OutError)) return false;
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
			ProposedAfter->SetField(
				TEXT("factory_catalog_sequence"),
				MakeShared<FJsonValueNumberString>(
					::LexToString(FactoryRevision->GetSequence())));
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
return Fail(OutError, Change.GetValidationDiagnostic());
		}

		FChangeSetDraft Draft(
			Request.GetDescriptorIdentity(),
			Request.GetRequestId(),
			Request.GetIdempotencyKey(),
			CurrentSnapshot,
			{MoveTemp(Change)});
		if (!Draft.IsValid())
		{
return Fail(OutError, Draft.GetValidationDiagnostic());
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
		UE::UAI::Contract::FDiagnostic& OutError)
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
			OutError = InternalRegistryFailure(TEXT("asset_data_native_binding_not_registered"), TEXT("The native binding does not identify a registered asset operation."), MakeShared<FJsonValueString>(NativeBindingId), MakeShared<FJsonValueString>(TEXT("registered native binding")));
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutFactoryContractId.Reset();
		OutClassConstraintId.Reset();
		OutError.Reset();
		const FDescriptorSpec* Descriptor =
			FindSpecByNativeBindingId(NativeBindingId);
		if (!Descriptor)
		{
			OutError = InternalRegistryFailure(TEXT("asset_data_native_binding_not_registered"), TEXT("The native binding does not identify a registered asset operation."), MakeShared<FJsonValueString>(NativeBindingId), MakeShared<FJsonValueString>(TEXT("registered native binding")));
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

	bool TryParseAssetDataSimpleEffectFactorySelector(
		const FJsonObject& Payload,
		const bool bGenericSelector,
		FString& OutName,
		TMap<FString, FString>& OutSelect,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutName.Reset();
		OutSelect.Reset();
		OutError.Reset();
		TSharedPtr<FJsonObject> Candidate;
		FString ShorthandName;
		if (bGenericSelector && Payload.TryGetStringField(TEXT("factory"), ShorthandName))
		{
			Candidate = MakeShared<FJsonObject>();
			Candidate->SetStringField(TEXT("name"), ShorthandName);
		}
		const TSharedPtr<FJsonObject>* Selector = nullptr;
		if (!Candidate.IsValid() && Payload.TryGetObjectField(TEXT("factory"), Selector)
			&& Selector && Selector->IsValid())
		{
			Candidate = *Selector;
		}
		HjsonWorkspace::DomainAuthoring::FDomainHjsonCandidateSelector Parsed;
		HjsonWorkspace::FWorkspaceError ParseError;
		if (!HjsonWorkspace::DomainAuthoring::TryReadCandidateSelector(
			Candidate, TEXT("/factory"), Parsed, ParseError))
		{
			OutError = ParseError.GetDiagnostic();
			return false;
		}
		OutName = MoveTemp(Parsed.Name);
		OutSelect = MoveTemp(Parsed.Select);
		return true;
	}

	bool TryValidateAssetDataSimpleEffectFactoryBinding(
		const FString& NativeBindingId,
		const FString& FactoryBindingId,
		const FUeAgentCatalogRevision& ExpectedRevision,
		const FUeAgentAssetDataSimpleEffectFactoryBinding& Binding,
		UE::UAI::Contract::FDiagnostic& OutError)
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
			OutError = InternalRegistryFailure(TEXT("asset_data_factory_contract_not_registered"), TEXT("The asset operation has no registered factory contract."), MakeShared<FJsonValueString>(NativeBindingId), MakeShared<FJsonValueString>(TEXT("operation with a factory contract")));
			return false;
		}
        if (Candidate && !Candidate->IsValid())
        {
            if (const auto* Error = Candidate->GetValidationDiagnostic())
            {
                OutError = *Error;
                MarkNativeFailure(OutError);
                return false;
            }
        }
		if (!Binding.IsValid()
			|| !Candidate
			|| !Candidate->IsValid())
		{
			OutError = InternalRegistryFailure(TEXT("asset_data_factory_candidate_invalid"), TEXT("The captured factory binding or its candidate is invalid."), Facts({{TEXT("binding_valid"), MakeShared<FJsonValueBoolean>(Binding.IsValid())}, {TEXT("candidate_present"), MakeShared<FJsonValueBoolean>(Candidate != nullptr)}, {TEXT("candidate_valid"), MakeShared<FJsonValueBoolean>(Candidate && Candidate->IsValid())}, {TEXT("class"), Binding.GetConcreteClass() ? ClassFact(Binding.GetConcreteClass()) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>())}, {TEXT("profile"), MakeShared<FJsonValueString>(Binding.GetProfileId())}, {TEXT("mechanic"), MakeShared<FJsonValueString>(Binding.GetFactoryMechanicId())}}), MakeShared<FJsonValueString>(TEXT("valid candidate binding with a concrete class, profile, and factory mechanic")));
			return false;
		}
		if (FactoryBindingId.IsEmpty()
			|| Candidate->GetBindingId() != FactoryBindingId)
		{
			OutError = InternalRegistryFailure(TEXT("asset_data_factory_binding_mismatch"), TEXT("The resolved factory candidate differs from the requested frozen binding."), MakeShared<FJsonValueString>(Candidate->GetBindingId()), MakeShared<FJsonValueString>(FactoryBindingId));
			return false;
		}
        if (!ExpectedRevision.IsValid())
        {
            if (const auto* Error = ExpectedRevision.GetValidationDiagnostic())
            {
                OutError = *Error;
                MarkNativeFailure(OutError);
                return false;
            }
        }
        if (!ExpectedRevision.IsValid() || Candidate->GetCatalogRevision() != ExpectedRevision)
		{
			OutError = InternalRegistryFailure(TEXT("asset_data_factory_candidate_stale"), TEXT("The factory candidate catalog revision no longer matches the captured revision."), CatalogRevisionFact(Candidate->GetCatalogRevision()), CatalogRevisionFact(ExpectedRevision));
			return false;
		}
		if (Binding.GetProfileId() != Descriptor->ProfileId)
		{
			OutError = InternalRegistryFailure(TEXT("asset_data_factory_profile_mismatch"), TEXT("The factory candidate belongs to a different asset operation profile."), MakeShared<FJsonValueString>(Binding.GetProfileId()), MakeShared<FJsonValueString>(Descriptor->ProfileId));
			return false;
		}
		const bool bGeneric = FString(Descriptor->CapabilityId)
			== TEXT("asset.object.create");
		const bool bExpectedMechanic = bGeneric
			? Binding.GetFactoryMechanicId()
					== TEXT("uai.asset_data.factory:asset_tools_create:v1")
				|| Binding.GetFactoryMechanicId()
					== TEXT("uai.asset_data.factory:new_object:v1")
			: Binding.GetFactoryMechanicId()
				== TEXT("uai.asset_data.factory:new_object:v1");
		if (!bExpectedMechanic)
		{
			OutError = InternalRegistryFailure(TEXT("asset_data_factory_mechanic_mismatch"), TEXT("The factory candidate uses a mechanic unsupported by this creation operation."), MakeShared<FJsonValueString>(Binding.GetFactoryMechanicId()), SpecValuesFact(bGeneric ? TEXT("uai.asset_data.factory:asset_tools_create:v1|uai.asset_data.factory:new_object:v1") : TEXT("uai.asset_data.factory:new_object:v1")));
			return false;
		}
		if (bGeneric
			&& (!Binding.IsGenericReady()
				|| !Candidate->Supports(
					EUeAgentCandidateOperation::Create)))
		{
			OutError = RegistryFailure(TEXT("asset_data_factory_interactive_required"),
                TEXT("The selected factory cannot create this asset without interactive setup."), TEXT("factory"),
                Facts({{TEXT("ready"), MakeShared<FJsonValueBoolean>(Binding.IsGenericReady())},
                    {TEXT("supports_create"), MakeShared<FJsonValueBoolean>(Candidate->Supports(EUeAgentCandidateOperation::Create))}}),
                MakeShared<FJsonValueString>(TEXT("factory ready for noninteractive creation")));
			return false;
		}
		if (!bGeneric
			&& !Candidate->Supports(
				EUeAgentCandidateOperation::Create))
		{
			OutError = RegistryFailure(TEXT("asset_data_factory_candidate_invalid"),
                TEXT("The selected factory candidate does not support asset creation."), TEXT("factory"),
                MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		if (!IsFactoryClassAllowed(
				Descriptor->ProfileId,
				Binding.GetConcreteClass()))
		{
			OutError = RegistryFailure(TEXT("asset_data_factory_class_mismatch"),
                TEXT("The selected factory class is not allowed by the requested asset creation profile."), TEXT("class"),
                ClassFact(Binding.GetConcreteClass()), MakeShared<FJsonValueString>(Policy->ClassConstraintId));
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		const FDescriptorSpec* Descriptor =
			FindSpecByNativeBindingId(NativeBindingId);
		const FFieldSpec* Field = Descriptor
			? FindFieldSpec(Descriptor->ProfileId, FieldId)
			: nullptr;
		if (!Descriptor || !Field)
		{
			OutError = RegistryFailure(TEXT("asset_data_native_field_not_registered"),
                TEXT("The asset operation has no registered binding for this field."), FieldId,
                MakeShared<FJsonValueString>(FieldId), Descriptor ? SpecValuesFact(Descriptor->FieldIds) : nullptr);
            OutError.InternalContext = NativeBindingId;
			return false;
		}
		if (FieldBindingId != Field->FieldBindingId)
		{
			OutError = InternalRegistryFailure(TEXT("asset_data_native_field_binding_mismatch"), TEXT("The planned field binding does not match the registered field."), MakeShared<FJsonValueString>(FieldBindingId), MakeShared<FJsonValueString>(Field->FieldBindingId));
            OutError.PropertyPath = FieldId;
			return false;
		}
		const TSet<FString> Presences(
			SplitSpecValues(Field->AllowedPresences));
		if (!Presences.Contains(Presence))
		{
			OutError = RegistryFailure(TEXT("asset_data_native_presence_mismatch"), TEXT("The planned presence is unsupported by the field."),
                FieldId + TEXT("/presence"), MakeShared<FJsonValueString>(Presence), SpecValuesFact(Field->AllowedPresences));
			return false;
		}
		const bool bRequiresValue =
			PresenceRequiresValue(Presence);
		if (bRequiresValue != bHasValue)
		{
			OutError = RegistryFailure(TEXT("asset_data_native_value_presence_mismatch"), TEXT("The planned value presence differs from the field presence requirement."),
                FieldId + TEXT("/value"), MakeShared<FJsonValueBoolean>(bHasValue), MakeShared<FJsonValueBoolean>(bRequiresValue));
			return false;
		}
        if (bHasValue && TypeId != Field->TypeId)
        {
            OutError = RegistryFailure(TEXT("asset_data_native_typed_value_mismatch"),
                TEXT("The planned value type differs from the registered field type."), FieldId + TEXT("/value/type_id"),
                MakeShared<FJsonValueString>(TypeId), MakeShared<FJsonValueString>(Field->TypeId));
            return false;
        }
        if (bHasValue && !CanonicalKindMatchesFieldKind(ValueKind, Field->ValueKind))
        {
            OutError = RegistryFailure(TEXT("asset_data_native_typed_value_mismatch"),
                TEXT("The planned canonical value kind differs from the registered field kind."), FieldId + TEXT("/value/canonical_value"),
                MakeShared<FJsonValueString>(CanonicalKindName(ValueKind)), MakeShared<FJsonValueString>(FieldKindName(Field->ValueKind)));
            return false;
        }
		return true;
	}
}
