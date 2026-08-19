#include "Misc/AutomationTest.h"

#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectTestSupport.h"
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectTestTypes.h"

// Re-evaluate the SA-005 production seam after the header is introduced.
#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"

#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectFactoryCandidateTestSupport.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "DomainAdapters/GenericEffect/UeAgentGenericEffectAdapter.h"
#include "Curves/CurveFloat.h"
#include "Animation/Skeleton.h"
#include "Engine/DataAsset.h"
#include "Engine/DataTable.h"
#include "Engine/SkeletalMeshLODSettings.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "Engine/Texture2D.h"
#include "Engine/Texture2DArray.h"
#include "Engine/TextureCollection.h"
#include "Engine/TextureCube.h"
#include "Engine/TextureCubeArray.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/VolumeTexture.h"
#include "HAL/PlatformTime.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "Materials/Material.h"
#include "MeshDescriptionBuilder.h"
#include "MediaPlayer.h"
#include "MediaTexture.h"
#include "FileMediaSource.h"
#include "Misc/Crc.h"
#include "Misc/Paths.h"
#include "PaperFlipbook.h"
#include "PaperSprite.h"
#include "PaperTileLayer.h"
#include "PaperTileMap.h"
#include "PaperTileSet.h"
#include "PhysicsEngine/BodySetup.h"
#include "Particles/SubUVAnimation.h"
#include "Serialization/JsonWriter.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundMix.h"
#include "Sound/SoundSubmix.h"
#include "SparseVolumeTexture/SparseVolumeTexture.h"
#include "StaticMeshAttributes.h"
#include "SubmixEffects/AudioMixerSubmixEffectEQ.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "VT/RuntimeVirtualTexture.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"

namespace
{
	using namespace UE::UAI::Tests::AssetDataSimpleEffect;

	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::DomainAdapters::AssetDataSimpleEffect;
	using namespace UE::UAI::DomainAdapters::GenericEffect;
	using namespace UE::UAI::ExecutionMutation;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;

	class FOwnedAssetResolver final
		: public IUeAgentAssetDataSimpleEffectAssetResolver
	{
	public:
		bool TryResolveExisting(
			const FString& CanonicalRef,
			UObject*& OutObject,
			FExecutionMutationError& OutError) override
		{
			OutObject = Existing.FindRef(CanonicalRef);
			if (!IsValid(OutObject))
			{
				OutError = FExecutionMutationError(
					TEXT("asset_data_reference_unresolved"),
					CanonicalRef);
				return false;
			}
			return true;
		}

		bool TryResolveCloneSource(
			const FString& CanonicalRef,
			UObject*& OutObject,
			FExecutionMutationError& OutError) override
		{
			OutObject = CloneSources.FindRef(CanonicalRef);
			if (!IsValid(OutObject))
			{
				OutError = FExecutionMutationError(
					TEXT("asset_data_clone_source_unresolved"),
					CanonicalRef);
				return false;
			}
			return true;
		}

		bool TryResolveCreationTarget(
			const FString& CanonicalRef,
			UPackage*& OutPackage,
			FName& OutObjectName,
			FExecutionMutationError& OutError) override
		{
			const FCreationTarget* Target =
				CreationTargets.Find(CanonicalRef);
			if (!Target || !Target->Package
				|| Target->ObjectName.IsNone())
			{
				OutError = FExecutionMutationError(
					TEXT("asset_data_creation_target_unresolved"),
					CanonicalRef);
				return false;
			}
			OutPackage = Target->Package;
			OutObjectName = Target->ObjectName;
			return true;
		}

		void BindExisting(
			const FString& Ref,
			UObject* Object)
		{
			Existing.Add(Ref, Object);
		}

		void BindCloneSource(
			const FString& Ref,
			UObject* Object)
		{
			CloneSources.Add(Ref, Object);
		}

		void BindCreationTarget(
			const FString& Ref,
			UPackage* Package,
			const FName ObjectName)
		{
			CreationTargets.Add(
				Ref,
				FCreationTarget{Package, ObjectName});
		}

	private:
		struct FCreationTarget
		{
			UPackage* Package = nullptr;
			FName ObjectName;
		};
		TMap<FString, UObject*> Existing;
		TMap<FString, UObject*> CloneSources;
		TMap<FString, FCreationTarget> CreationTargets;
	};

	class FOwnedAssetPackage final
	{
	public:
		FOwnedAssetPackage()
		{
			const FString Suffix =
				FGuid::NewGuid().ToString(
					EGuidFormats::Digits);
			Package = CreatePackage(
				*(TEXT("/Temp/UAI/AssetData/") + Suffix));
			if (Package)
			{
				Package->SetDirtyFlag(false);
			}
		}

		~FOwnedAssetPackage()
		{
			if (Package)
			{
				Package->SetDirtyFlag(false);
				Package->MarkAsGarbage();
			}
		}

		template <typename TObject>
		TObject* New(const TCHAR* Name)
		{
			return Package
				? NewObject<TObject>(
					Package,
					Name,
					RF_Transactional)
				: nullptr;
		}

		UPackage* Package = nullptr;
	};

	TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
	MakeResources()
	{
		return MakeShared<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe>(
				FUeAgentResourceBudget({
					{EUeAgentResourceDimension::InputBytes, 1048576},
					{EUeAgentResourceDimension::OutputBytes, 1048576},
					{EUeAgentResourceDimension::StringBytes, 1048576},
					{EUeAgentResourceDimension::Nodes, 32768},
					{EUeAgentResourceDimension::Items, 32768},
					{EUeAgentResourceDimension::Files, 64},
					{EUeAgentResourceDimension::ReferenceEdges, 32768},
					{EUeAgentResourceDimension::WorkUnits, 1048576},
					{
						EUeAgentResourceDimension::
							EstimatedMemoryBytes,
						134217728,
					},
				}));
	}

	bool TryCanonicalValue(
		const TSharedPtr<FJsonValue>& JsonValue,
		FUeAgentResourceTracker& Resources,
		FUeAgentCanonicalValuePtr& OutValue,
		FUeAgentStateSemanticError& OutError)
	{
		OutValue.Reset();
		OutError = {};
		if (!JsonValue.IsValid())
		{
			return false;
		}
		switch (JsonValue->Type)
		{
		case EJson::Null:
			return FUeAgentCanonicalValue::TryNull(
				Resources,
				OutValue,
				OutError);
		case EJson::Boolean:
			return FUeAgentCanonicalValue::TryBoolean(
				JsonValue->AsBool(),
				Resources,
				OutValue,
				OutError);
		case EJson::String:
			return FUeAgentCanonicalValue::TryString(
				JsonValue->AsString(),
				Resources,
				OutValue,
				OutError);
		case EJson::Number:
			return FUeAgentCanonicalValue::TryDecimal(
				FString::SanitizeFloat(JsonValue->AsNumber()),
				Resources,
				OutValue,
				OutError);
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
				return FUeAgentCanonicalValue::TryArray(
					Items,
					Resources,
					OutValue,
					OutError);
			}
		case EJson::Object:
			{
				TArray<
					TPair<FString, FUeAgentCanonicalValueRef>>
					Items;
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
				return FUeAgentCanonicalValue::TryObject(
					Items,
					Resources,
					OutValue,
					OutError);
			}
		default:
			return false;
		}
	}

	bool ReadMediaPlayerLoopSetting(
		const UMediaPlayer& MediaPlayer)
	{
		const FBoolProperty* LoopProperty =
			FindFProperty<FBoolProperty>(
				UMediaPlayer::StaticClass(),
				TEXT("Loop"));
		return LoopProperty
			&& LoopProperty->GetPropertyValue_InContainer(
				&MediaPlayer);
	}

	bool ReadExactBoolProperty(
		const UObject& Object,
		const FName PropertyName,
		bool& bOutValue)
	{
		const FBoolProperty* Property =
			FindFProperty<FBoolProperty>(
				Object.GetClass(),
				PropertyName);
		if (!Property)
		{
			return false;
		}
		bOutValue =
			Property->GetPropertyValue_InContainer(&Object);
		return true;
	}

	bool ReadExactFloatProperty(
		const UObject& Object,
		const FName PropertyName,
		float& OutValue)
	{
		const FFloatProperty* Property =
			FindFProperty<FFloatProperty>(
				Object.GetClass(),
				PropertyName);
		if (!Property)
		{
			return false;
		}
		OutValue =
			Property->GetPropertyValue_InContainer(&Object);
		return true;
	}

	bool InitializeTexture2DSourceRect(
		UTexture2D& Texture,
		const int32 SizeX,
		const int32 SizeY,
		const uint8 Seed)
	{
		if (SizeX <= 0 || SizeY <= 0)
		{
			return false;
		}
		TArray<uint8> Data;
		Data.SetNumUninitialized(SizeX * SizeY * 4);
		for (int32 Index = 0; Index < Data.Num(); ++Index)
		{
			Data[Index] =
				static_cast<uint8>(Seed + Index * 13);
		}
		Texture.Source.Init(
			SizeX,
			SizeY,
			1,
			1,
			TSF_BGRA8,
			Data.GetData());
		Texture.SRGB = true;
		Texture.CompressionSettings = TC_Default;
		Texture.MipGenSettings = TMGS_NoMipmaps;
		return Texture.Source.IsValid();
	}

	bool InitializeTexture2DSource(
		UTexture2D& Texture,
		const int32 Size,
		const uint8 Seed)
	{
		return InitializeTexture2DSourceRect(
			Texture,
			Size,
			Size,
			Seed);
	}

	bool InitializeTextureCubeSource(
		UTextureCube& Texture,
		const int32 Size,
		const uint8 Seed)
	{
		if (Size <= 0)
		{
			return false;
		}
		TArray<uint8> Data;
		Data.SetNumUninitialized(Size * Size * 6 * 4);
		for (int32 Index = 0; Index < Data.Num(); ++Index)
		{
			Data[Index] =
				static_cast<uint8>(Seed + Index * 17);
		}
		Texture.Source.Init(
			Size,
			Size,
			6,
			1,
			TSF_BGRA8,
			Data.GetData());
		Texture.SRGB = true;
		Texture.CompressionSettings = TC_Default;
		Texture.MipGenSettings = TMGS_NoMipmaps;
		return Texture.Source.IsValid();
	}

	bool InitializeVolumeTextureSource(
		UVolumeTexture& Texture,
		UTexture2D& SourceStack,
		const int32 Size,
		const int32 Depth,
		const uint8 Seed)
	{
		if (Size <= 0
			|| Depth <= 0
			|| !InitializeTexture2DSourceRect(
				SourceStack,
				Size,
				Size * Depth,
				Seed))
		{
			return false;
		}
		Texture.Source2DTexture = &SourceStack;
		Texture.Source2DTileSizeX = Size;
		Texture.Source2DTileSizeY = Size;
		if (!Texture.UpdateSourceFromSourceTexture())
		{
			return false;
		}
		Texture.SRGB = true;
		Texture.CompressionSettings = TC_Default;
		Texture.MipGenSettings = TMGS_NoMipmaps;
		return Texture.Source.IsValid()
			&& Texture.Source.GetNumSlices() == Depth;
	}

	uint32 ReadTextureSourceTopMipCrc(UTexture& Texture)
	{
		TArray64<uint8> Data;
		if (!Texture.Source.IsValid()
			|| !Texture.Source.GetMipData(Data, 0)
			|| Data.Num() <= 0
			|| Data.Num() > MAX_int32)
		{
			return 0;
		}
		return FCrc::MemCrc32(
			Data.GetData(),
			static_cast<int32>(Data.Num()));
	}

	bool TextureSourceMatchesOrderedConcatenation(
		UTexture& Target,
		const TArray<UTexture*>& Sources)
	{
		TArray64<uint8> Actual;
		if (Sources.IsEmpty()
			|| !Target.Source.IsValid()
			|| !Target.Source.GetMipData(Actual, 0))
		{
			return false;
		}
		TArray64<uint8> Expected;
		for (UTexture* Source : Sources)
		{
			TArray64<uint8> SourceData;
			if (!Source
				|| !Source->Source.IsValid()
				|| !Source->Source.GetMipData(SourceData, 0))
			{
				return false;
			}
			Expected.Append(
				SourceData.GetData(),
				SourceData.Num());
		}
		return Actual == Expected;
	}

	FString ReadTextureFingerprint(UTexture& Texture)
	{
		return FString::Printf(
			TEXT(
				"texture:%d:%d:%d:%d:%d:%u:%s:%s:%d:%d:%d"),
			Texture.Source.IsValid() ? 1 : 0,
			Texture.Source.GetSizeX(),
			Texture.Source.GetSizeY(),
			Texture.Source.GetNumSlices(),
			Texture.Source.GetNumMips(),
			ReadTextureSourceTopMipCrc(Texture),
			*Texture.Source.GetId().ToString(
				EGuidFormats::Digits),
			*Texture.GetLightingGuid().ToString(
				EGuidFormats::Digits),
			Texture.SRGB ? 1 : 0,
			static_cast<int32>(Texture.CompressionSettings),
			static_cast<int32>(Texture.MipGenSettings));
	}

	FString ReadFingerprint(
		UObject& Object,
		const FString& ProfileId)
	{
		if (const UInputAction* InputAction =
			Cast<UInputAction>(&Object))
		{
			FString Fingerprint = FString::Printf(
				TEXT("input-action:%d:%d"),
				static_cast<int32>(InputAction->ValueType),
				InputAction->bConsumeInput ? 1 : 0);
			for (const TObjectPtr<UInputTrigger>& TriggerPtr
				: InputAction->Triggers)
			{
				const UInputTrigger* Trigger = TriggerPtr.Get();
				Fingerprint += Trigger
					? FString::Printf(
						TEXT(":trigger=%s,%s"),
						*Trigger->GetClass()->GetPathName(),
						*Trigger->GetPathName())
					: TEXT(":trigger=<null>");
			}
			for (const TObjectPtr<UInputModifier>& ModifierPtr
				: InputAction->Modifiers)
			{
				const UInputModifier* Modifier =
					ModifierPtr.Get();
				Fingerprint += Modifier
					? FString::Printf(
						TEXT(":modifier=%s,%s"),
						*Modifier->GetClass()->GetPathName(),
						*Modifier->GetPathName())
					: TEXT(":modifier=<null>");
			}
			return Fingerprint;
		}
		if (const UInputMappingContext* MappingContext =
			Cast<UInputMappingContext>(&Object))
		{
			FString Fingerprint = TEXT("input-mapping-context");
			for (const FEnhancedActionKeyMapping& Mapping
				: MappingContext->GetMappings())
			{
				Fingerprint += FString::Printf(
					TEXT(":mapping=%s,%s"),
					Mapping.Action
						? *Mapping.Action->GetPathName()
						: TEXT("<null>"),
					*Mapping.Key.GetFName().ToString());
				for (const TObjectPtr<UInputTrigger>& TriggerPtr
					: Mapping.Triggers)
				{
					const UInputTrigger* Trigger =
						TriggerPtr.Get();
					Fingerprint += Trigger
						? FString::Printf(
							TEXT(":trigger=%s,%s"),
							*Trigger->GetClass()->GetPathName(),
							*Trigger->GetPathName())
						: TEXT(":trigger=<null>");
				}
				for (const TObjectPtr<UInputModifier>& ModifierPtr
					: Mapping.Modifiers)
				{
					const UInputModifier* Modifier =
						ModifierPtr.Get();
					Fingerprint += Modifier
						? FString::Printf(
							TEXT(":modifier=%s,%s"),
							*Modifier->GetClass()->GetPathName(),
							*Modifier->GetPathName())
						: TEXT(":modifier=<null>");
				}
			}
			return Fingerprint;
		}
		if (const UPaperTileMap* TileMap =
			Cast<UPaperTileMap>(&Object))
		{
			FString Fingerprint = FString::Printf(
				TEXT("paper-tilemap:%d:%d:%d:%d:%d"),
				TileMap->MapWidth,
				TileMap->MapHeight,
				TileMap->TileWidth,
				TileMap->TileHeight,
				TileMap->TileLayers.Num());
			for (const UPaperTileLayer* Layer
				: TileMap->TileLayers)
			{
				if (!Layer)
				{
					Fingerprint += TEXT(":<null-layer>");
					continue;
				}
				bool bHiddenInGame = false;
				bool bOverrideThickness = false;
				bool bOverrideOffset = false;
				float Thickness = 0.0f;
				float Offset = 0.0f;
				const bool bPrivateStateRead =
					ReadExactBoolProperty(
						*Layer,
						TEXT("bHiddenInGame"),
						bHiddenInGame)
					&& ReadExactBoolProperty(
						*Layer,
						TEXT("bOverrideCollisionThickness"),
						bOverrideThickness)
					&& ReadExactBoolProperty(
						*Layer,
						TEXT("bOverrideCollisionOffset"),
						bOverrideOffset)
					&& ReadExactFloatProperty(
						*Layer,
						TEXT("CollisionThicknessOverride"),
						Thickness)
					&& ReadExactFloatProperty(
						*Layer,
						TEXT("CollisionOffsetOverride"),
						Offset);
				Fingerprint += FString::Printf(
					TEXT(
						":layer=%s:%s:%d:%d:%d:%d:"
						"%.9g:%d:%.9g:%s:%d:%d"),
					*Layer->GetPathName(),
					*Layer->LayerName.ToString(),
					Layer->ShouldRenderInEditor() ? 1 : 0,
					bHiddenInGame ? 0 : 1,
					Layer->GetLayerCollides() ? 1 : 0,
					bOverrideThickness ? 1 : 0,
					static_cast<double>(Thickness),
					bOverrideOffset ? 1 : 0,
					static_cast<double>(Offset),
					*Layer->GetLayerColor().ToString(),
					Layer->GetLayerWidth(),
					Layer->GetLayerHeight());
				Fingerprint += bPrivateStateRead
					? TEXT(":private-ok")
					: TEXT(":private-missing");
				for (int32 Y = 0;
					Y < Layer->GetLayerHeight();
					++Y)
				{
					for (int32 X = 0;
						X < Layer->GetLayerWidth();
						++X)
					{
						const FPaperTileInfo Cell =
							Layer->GetCell(X, Y);
						if (Cell.IsValid())
						{
							Fingerprint += FString::Printf(
								TEXT(":cell=%d,%d,%s,%d"),
								X,
								Y,
								Cell.TileSet
									? *Cell.TileSet->GetPathName()
									: TEXT(""),
								Cell.PackedTileIndex);
						}
					}
				}
			}
			return Fingerprint;
		}
		if (const UPaperTileSet* TileSet =
			Cast<UPaperTileSet>(&Object))
		{
			const UTexture2D* Texture =
				TileSet->GetTileSheetTexture();
			const FIntPoint Size = TileSet->GetTileSize();
			const FIntMargin Margin = TileSet->GetMargin();
			const FIntPoint Spacing =
				TileSet->GetPerTileSpacing();
			FString Fingerprint = FString::Printf(
				TEXT(
					"paper-tileset:%s:%d:%d:%d:%d:%d:%d:"
					"%d:%d:%d:%d:%d:%d"),
				Texture ? *Texture->GetPathName() : TEXT(""),
				Size.X,
				Size.Y,
				Margin.Left,
				Margin.Top,
				Margin.Right,
				Margin.Bottom,
				Spacing.X,
				Spacing.Y,
				TileSet->GetTileCountX(),
				TileSet->GetTileCountY(),
				TileSet->GetTileCount(),
				TileSet->GetNumTerrains());
			for (int32 Index = 0;
				Index < TileSet->GetNumTerrains();
				++Index)
			{
				const FPaperTileSetTerrain Terrain =
					TileSet->GetTerrain(Index);
				Fingerprint += FString::Printf(
					TEXT(":terrain=%s,%d"),
					*Terrain.TerrainName,
					Terrain.CenterTileIndex);
			}
			for (int32 Index = 0;
				Index < TileSet->GetTileCount();
				++Index)
			{
				if (const FPaperTileMetadata* Metadata =
					TileSet->GetTileMetadata(Index))
				{
					Fingerprint += FString::Printf(
						TEXT(
							":metadata=%d,%s,%d,%d,%d,%d,%d"),
						Index,
						*Metadata->UserDataName.ToString(),
						Metadata->CollisionData.Shapes.Num(),
						static_cast<int32>(
							Metadata->TerrainMembership[0]),
						static_cast<int32>(
							Metadata->TerrainMembership[1]),
						static_cast<int32>(
							Metadata->TerrainMembership[2]),
						static_cast<int32>(
							Metadata->TerrainMembership[3]));
				}
			}
			return Fingerprint;
		}
		if (const UPaperFlipbook* Flipbook =
			Cast<UPaperFlipbook>(&Object))
		{
			const UMaterialInterface* DefaultMaterial =
				Flipbook->GetDefaultMaterial();
			FString Fingerprint = FString::Printf(
				TEXT("paper-flipbook:%.9g:%s:%d"),
				static_cast<double>(
					Flipbook->GetFramesPerSecond()),
				DefaultMaterial
					? *DefaultMaterial->GetPathName()
					: TEXT(""),
				static_cast<int32>(
					Flipbook->GetCollisionSource()));
			for (int32 Index = 0;
				Index < Flipbook->GetNumKeyFrames();
				++Index)
			{
				const FPaperFlipbookKeyFrame& Frame =
					Flipbook->GetKeyFrameChecked(Index);
				Fingerprint += FString::Printf(
					TEXT(":%s:%d"),
					Frame.Sprite
						? *Frame.Sprite->GetPathName()
						: TEXT("<null>"),
					Frame.FrameRun);
			}
			return Fingerprint;
		}
		if (const UPaperSprite* Sprite =
			Cast<UPaperSprite>(&Object))
		{
			FVector2D CustomPivot = FVector2D::ZeroVector;
			const ESpritePivotMode::Type PivotMode =
				Sprite->GetPivotMode(CustomPivot);
			const FVector2D SourceUV = Sprite->GetSourceUV();
			const FVector2D SourceSize = Sprite->GetSourceSize();
			const UTexture2D* SourceTexture =
				Sprite->GetSourceTexture();
			FString Fingerprint = FString::Printf(
				TEXT(
					"paper-sprite:%s:%s:%s:%d:%s"),
				SourceTexture
					? *SourceTexture->GetPathName()
					: TEXT(""),
				*SourceUV.ToString(),
				*SourceSize.ToString(),
				static_cast<int32>(PivotMode),
				*CustomPivot.ToString());
			FAdditionalSpriteTextureArray AdditionalTextures;
			Sprite->GetBakedAdditionalSourceTextures(
				AdditionalTextures);
			for (const UTexture* Texture : AdditionalTextures)
			{
				Fingerprint += TEXT(":");
				Fingerprint += Texture
					? Texture->GetPathName()
					: TEXT("<null>");
			}
			return Fingerprint;
		}
		if (const UTextureCollection* Collection =
			Cast<UTextureCollection>(&Object))
		{
			FString Fingerprint = TEXT("texture-collection");
			for (const UTexture* Texture : Collection->Textures)
			{
				Fingerprint += TEXT(":");
				Fingerprint += Texture
					? Texture->GetPathName()
					: TEXT("<null>");
			}
			return Fingerprint;
		}
		if (UTexture2DArray* TextureArray =
			Cast<UTexture2DArray>(&Object))
		{
			FString Fingerprint =
				ReadTextureFingerprint(*TextureArray);
			for (const TObjectPtr<UTexture2D>& Texture
				: TextureArray->SourceTextures)
			{
				Fingerprint += TEXT(":source=");
				Fingerprint += Texture
					? Texture->GetPathName()
					: TEXT("<null>");
			}
			Fingerprint += FString::Printf(
				TEXT(":generated=%d"),
				TextureArray
						->bSourceGeneratedFromSourceTexturesArray
					? 1
					: 0);
			return Fingerprint;
		}
		if (UTextureCubeArray* TextureArray =
			Cast<UTextureCubeArray>(&Object))
		{
			FString Fingerprint =
				ReadTextureFingerprint(*TextureArray);
			for (const TObjectPtr<UTextureCube>& Texture
				: TextureArray->SourceTextures)
			{
				Fingerprint += TEXT(":source=");
				Fingerprint += Texture
					? Texture->GetPathName()
					: TEXT("<null>");
			}
			Fingerprint += FString::Printf(
				TEXT(":generated=%d"),
				TextureArray
						->bSourceGeneratedFromSourceTexturesArray
					? 1
					: 0);
			return Fingerprint;
		}
		if (UTextureCube* TextureCube =
			Cast<UTextureCube>(&Object))
		{
			return ReadTextureFingerprint(*TextureCube);
		}
		if (UVolumeTexture* VolumeTexture =
			Cast<UVolumeTexture>(&Object))
		{
			return ReadTextureFingerprint(*VolumeTexture)
				+ FString::Printf(
					TEXT(":source2d=%s:tile=%d,%d"),
					VolumeTexture->Source2DTexture
						? *VolumeTexture->Source2DTexture
							->GetPathName()
						: TEXT("<null>"),
					VolumeTexture->Source2DTileSizeX,
					VolumeTexture->Source2DTileSizeY);
		}
		if (const UTextureRenderTarget2D* RenderTarget =
			Cast<UTextureRenderTarget2D>(&Object))
		{
			return FString::Printf(
				TEXT("rt:%d:%d:%d:%s:%d"),
				RenderTarget->SizeX,
				RenderTarget->SizeY,
				static_cast<int32>(
					RenderTarget->RenderTargetFormat),
				*RenderTarget->ClearColor.ToString(),
				RenderTarget->bAutoGenerateMips ? 1 : 0);
		}
		if (const UMediaPlayer* MediaPlayer =
			Cast<UMediaPlayer>(&Object))
		{
			return FString::Printf(
				TEXT("media-player:%d:%d"),
				MediaPlayer->PlayOnOpen ? 1 : 0,
				ReadMediaPlayerLoopSetting(*MediaPlayer)
					? 1
					: 0);
		}
		if (const UMediaTexture* MediaTexture =
			Cast<UMediaTexture>(&Object))
		{
			const UMediaPlayer* MediaPlayer =
				MediaTexture->GetMediaPlayer();
			return FString::Printf(
				TEXT("media-texture:%s:%d:%s"),
				MediaPlayer
					? *MediaPlayer->GetPathName()
					: TEXT(""),
				MediaTexture->AutoClear ? 1 : 0,
				*MediaTexture->ClearColor.ToString());
		}
		if (const USubUVAnimation* SubUV =
			Cast<USubUVAnimation>(&Object))
		{
			return FString::Printf(
				TEXT("subuv:%s:%d:%d:%d"),
				SubUV->SubUVTexture
					? *SubUV->SubUVTexture->GetPathName()
					: TEXT(""),
				SubUV->SubImages_Horizontal,
				SubUV->SubImages_Vertical,
				static_cast<int32>(SubUV->BoundingMode));
		}
		if (const USubmixEffectSubmixEQPreset* Preset =
			Cast<USubmixEffectSubmixEQPreset>(&Object))
		{
			FString Fingerprint = TEXT("submix-eq");
			for (const FSubmixEffectEQBand& Band
				: Preset->Settings.EQBands)
			{
				Fingerprint += FString::Printf(
					TEXT(":%.9g,%.9g,%.9g,%d"),
					static_cast<double>(Band.Frequency),
					static_cast<double>(Band.Bandwidth),
					static_cast<double>(Band.GainDb),
					Band.bEnabled ? 1 : 0);
			}
			return Fingerprint;
		}
		if (const USoundAttenuation* Attenuation =
			Cast<USoundAttenuation>(&Object))
		{
			const FSoundAttenuationSettings& Settings =
				Attenuation->Attenuation;
			return FString::Printf(
				TEXT(
					"attenuation:%d:%d:%d:%d:%.9g:%.9g:%d:"
					"%d:%.9g:%.9g:%.9g"),
				Settings.bAttenuate ? 1 : 0,
				Settings.bSpatialize ? 1 : 0,
				static_cast<int32>(Settings.DistanceAlgorithm),
				static_cast<int32>(Settings.AttenuationShape),
				static_cast<double>(
					Settings.AttenuationShapeExtents.X),
				static_cast<double>(Settings.FalloffDistance),
				static_cast<int32>(
					Settings.SpatializationAlgorithm),
				Settings.bEnableOcclusion ? 1 : 0,
				static_cast<double>(
					Settings.OcclusionLowPassFilterFrequency),
				static_cast<double>(
					Settings.OcclusionVolumeAttenuation),
				static_cast<double>(
					Settings.OcclusionInterpolationTime));
		}
		if (const USoundClass* SoundClass =
			Cast<USoundClass>(&Object))
		{
			FString Fingerprint = FString::Printf(
				TEXT("sound-class:%.9g:%.9g:%d:%d:%s"),
				static_cast<double>(
					SoundClass->Properties.Volume),
				static_cast<double>(
					SoundClass->Properties.Pitch),
				SoundClass->Properties.bIsUISound ? 1 : 0,
				SoundClass->Properties.bIsMusic ? 1 : 0,
				SoundClass->ParentClass
					? *SoundClass->ParentClass->GetPathName()
					: TEXT(""));
			for (const USoundClass* Child
				: SoundClass->ChildClasses)
			{
				Fingerprint += TEXT(":");
				Fingerprint += Child
					? Child->GetPathName()
					: TEXT("<null>");
			}
			return Fingerprint;
		}
		if (const USoundConcurrency* Concurrency =
			Cast<USoundConcurrency>(&Object))
		{
			const FSoundConcurrencySettings& Settings =
				Concurrency->Concurrency;
			return FString::Printf(
				TEXT(
					"concurrency:%d:%d:%.9g:%d:%.9g:%d:"
					"%.9g:%.9g:%.9g"),
				Settings.MaxCount,
				Settings.bLimitToOwner ? 1 : 0,
				static_cast<double>(Settings.RetriggerTime),
				static_cast<int32>(Settings.ResolutionRule),
				static_cast<double>(Settings.GetVolumeScale()),
				static_cast<int32>(Settings.VolumeScaleMode),
				static_cast<double>(
					Settings.VolumeScaleAttackTime),
				static_cast<double>(
					Settings.VolumeScaleReleaseTime),
				static_cast<double>(
					Settings.VoiceStealReleaseTime));
		}
		if (const USoundMix* SoundMix =
			Cast<USoundMix>(&Object))
		{
			FString Fingerprint = FString::Printf(
				TEXT("sound-mix:%.9g"),
				static_cast<double>(SoundMix->InitialDelay));
			for (const FSoundClassAdjuster& Modifier
				: SoundMix->SoundClassEffects)
			{
				Fingerprint += FString::Printf(
					TEXT(":mod=%s,%.9g,%.9g,%.9g,%d,%.9g"),
					Modifier.SoundClassObject
						? *Modifier.SoundClassObject->GetPathName()
						: TEXT("<null>"),
					static_cast<double>(
						Modifier.VolumeAdjuster),
					static_cast<double>(
						Modifier.PitchAdjuster),
					static_cast<double>(
						Modifier.LowPassFilterFrequency),
					Modifier.bApplyToChildren ? 1 : 0,
					static_cast<double>(
						Modifier
							.VoiceCenterChannelVolumeAdjuster));
			}
			return Fingerprint;
		}
		if (const USoundSubmix* Submix =
			Cast<USoundSubmix>(&Object))
		{
			FString Fingerprint = TEXT("sound-submix:");
			Fingerprint += Submix->ParentSubmix
				? Submix->ParentSubmix->GetPathName()
				: TEXT("");
			for (const USoundSubmixBase* Child
				: Submix->ChildSubmixes)
			{
				Fingerprint += TEXT(":child=");
				Fingerprint += Child
					? Child->GetPathName()
					: TEXT("<null>");
			}
			for (const USoundEffectSubmixPreset* Effect
				: Submix->SubmixEffectChain)
			{
				Fingerprint += TEXT(":effect=");
				Fingerprint += Effect
					? Effect->GetPathName()
					: TEXT("<null>");
			}
			return Fingerprint;
		}
		if (const UCurveFloat* Curve = Cast<UCurveFloat>(&Object))
		{
			FString Fingerprint = TEXT("curve-float");
			for (const FRichCurveKey& Key
				: Curve->FloatCurve.GetConstRefOfKeys())
			{
				Fingerprint += FString::Printf(
					TEXT(
						":%.9g,%.9g,%d,%d,%d,%.9g,%.9g,"
						"%.9g,%.9g"),
					static_cast<double>(Key.Time),
					static_cast<double>(Key.Value),
					static_cast<int32>(Key.InterpMode),
					static_cast<int32>(Key.TangentMode),
					static_cast<int32>(Key.TangentWeightMode),
					static_cast<double>(Key.ArriveTangent),
					static_cast<double>(Key.LeaveTangent),
					static_cast<double>(Key.ArriveTangentWeight),
					static_cast<double>(Key.LeaveTangentWeight));
			}
			return Fingerprint;
		}
		if (const UUeAgentAssetDataSimpleEffectDataAsset* Data =
			Cast<UUeAgentAssetDataSimpleEffectDataAsset>(&Object))
		{
			return FString::Printf(
				TEXT("registered-data:%s:%d:%.9g:%d:%s"),
				*Data->Label,
				Data->Count,
				static_cast<double>(Data->Weight),
				Data->bEnabled ? 1 : 0,
				Data->RelatedAsset
					? *Data->RelatedAsset->GetPathName()
					: TEXT(""));
		}
		if (const UFileMediaSource* MediaSource =
			Cast<UFileMediaSource>(&Object))
		{
			return FString::Printf(
				TEXT("file-media:%s:%d:%s"),
				*MediaSource->GetFilePath(),
				MediaSource->PrecacheFile ? 1 : 0,
				*MediaSource->GetUrl());
		}
		if (const URuntimeVirtualTexture* VirtualTexture =
			Cast<URuntimeVirtualTexture>(&Object))
		{
			return FString::Printf(
				TEXT("rvt:%d:%d:%d:%d"),
				VirtualTexture->GetSize(),
				VirtualTexture->GetTileSize(),
				static_cast<int32>(
					VirtualTexture->GetMaterialType()),
				VirtualTexture->GetCompressTextures() ? 1 : 0);
		}
		if (const UStreamableSparseVolumeTexture* Sparse =
			Cast<UStreamableSparseVolumeTexture>(&Object))
		{
			return FString::Printf(
				TEXT(
					"sparse:%d:%d:%d:%d:%.9g:%d:%.9g:%.9g"),
				static_cast<int32>(Sparse->AddressX),
				static_cast<int32>(Sparse->AddressY),
				static_cast<int32>(Sparse->AddressZ),
				Sparse->bLocalDDCOnly ? 1 : 0,
				static_cast<double>(
					Sparse->StreamingPoolSizeFactor),
				Sparse->NumberOfPrefetchFrames,
				static_cast<double>(
					Sparse->PrefetchPercentageStepSize),
				static_cast<double>(
					Sparse->PrefetchPercentageBias));
		}
		if (const UStaticMesh* StaticMesh =
			Cast<UStaticMesh>(&Object))
		{
			FString Fingerprint = TEXT("static");
			for (const FStaticMaterial& Slot
				: StaticMesh->GetStaticMaterials())
			{
				Fingerprint += FString::Printf(
					TEXT(":m[%s,%s]"),
					*Slot.MaterialSlotName.ToString(),
					Slot.MaterialInterface
						? *Slot.MaterialInterface->GetPathName()
						: TEXT(""));
			}
			if (const UBodySetup* BodySetup =
				StaticMesh->GetBodySetup())
			{
				Fingerprint += FString::Printf(
					TEXT(":trace=%d"),
					static_cast<int32>(
						BodySetup->CollisionTraceFlag));
				for (const FKBoxElem& Box
					: BodySetup->AggGeom.BoxElems)
				{
					Fingerprint += FString::Printf(
						TEXT(":box[%s,%s,%s]"),
						*Box.Center.ToString(),
						*Box.Rotation.ToString(),
						*FVector(Box.X, Box.Y, Box.Z)
							.ToString());
				}
				for (const FKSphereElem& Sphere
					: BodySetup->AggGeom.SphereElems)
				{
					Fingerprint += FString::Printf(
						TEXT(":sphere[%s,%g]"),
						*Sphere.Center.ToString(),
						static_cast<double>(Sphere.Radius));
				}
				for (const FKSphylElem& Capsule
					: BodySetup->AggGeom.SphylElems)
				{
					Fingerprint += FString::Printf(
						TEXT(":capsule[%s,%s,%g,%g]"),
						*Capsule.Center.ToString(),
						*Capsule.Rotation.ToString(),
						static_cast<double>(Capsule.Radius),
						static_cast<double>(Capsule.Length));
				}
			}
			for (const UStaticMeshSocket* Socket
				: StaticMesh->Sockets)
			{
				Fingerprint += Socket
					? FString::Printf(
						TEXT(":socket[%s,%s,%s,%s]"),
						*Socket->SocketName.ToString(),
						*Socket->RelativeLocation.ToString(),
						*Socket->RelativeRotation.ToString(),
						*Socket->RelativeScale.ToString())
					: TEXT(":socket[null]");
			}
			for (int32 Index = 0;
				Index < StaticMesh->GetNumSourceModels();
				++Index)
			{
				const FMeshBuildSettings& Settings =
					StaticMesh->GetSourceModel(Index)
						.BuildSettings;
				Fingerprint += FString::Printf(
					TEXT(":build[%d,%d,%d,%d,%d,%d,%d]"),
					Index,
					Settings.bRecomputeNormals ? 1 : 0,
					Settings.bRecomputeTangents ? 1 : 0,
					Settings.bComputeWeightedNormals ? 1 : 0,
					Settings.bRemoveDegenerates ? 1 : 0,
					Settings.bUseMikkTSpace ? 1 : 0,
					Settings.bGenerateLightmapUVs ? 1 : 0);
			}
			return Fingerprint;
		}
		if (const UDataTable* DataTable =
			Cast<UDataTable>(&Object))
		{
			FString Fingerprint = FString::Printf(
				TEXT("table:%s:%d"),
				DataTable->GetRowStruct()
					? *DataTable->GetRowStruct()->GetPathName()
					: TEXT(""),
				DataTable->GetRowMap().Num());
			for (const TPair<FName, uint8*>& Row
				: DataTable->GetRowMap())
			{
				Fingerprint += TEXT(":") + Row.Key.ToString();
				if (DataTable->GetRowStruct()
					== FUeAgentAssetDataSimpleEffectTableRow::
						StaticStruct())
				{
					const auto* Typed =
						reinterpret_cast<
							const FUeAgentAssetDataSimpleEffectTableRow*>(
								Row.Value);
					Fingerprint += FString::Printf(
						TEXT("[%s,%d,%g,%d]"),
						*Typed->Label,
						Typed->Count,
						static_cast<double>(Typed->Weight),
						Typed->bEnabled ? 1 : 0);
				}
			}
			return Fingerprint;
		}
		if (const USkeletalMesh* SkeletalMesh =
			Cast<USkeletalMesh>(&Object))
		{
			FString Fingerprint = FString::Printf(
				TEXT("skeletal:%s:%s:%s"),
				*SkeletalMesh->GetImportedBounds()
					.GetBox()
					.ToString(),
				SkeletalMesh->GetSkeleton()
					? *SkeletalMesh->GetSkeleton()
						->GetPathName()
					: TEXT(""),
				SkeletalMesh->GetLODSettings()
					? *SkeletalMesh->GetLODSettings()
						->GetPathName()
					: TEXT(""));
			for (const FSkeletalMaterial& Slot
				: SkeletalMesh->GetMaterials())
			{
				Fingerprint += FString::Printf(
					TEXT(":m[%s,%s]"),
					*Slot.MaterialSlotName.ToString(),
					Slot.MaterialInterface
						? *Slot.MaterialInterface->GetPathName()
						: TEXT(""));
			}
			return Fingerprint;
		}
		return FString::Printf(
			TEXT("object:%s:%s:%s"),
			*Object.GetClass()->GetPathName(),
			*Object.GetName(),
			*ProfileId);
	}

	FUeAgentCanonicalSnapshotPtr MakeSnapshot(
		FAutomationTestBase* Test,
		const FUeAgentOperationDescriptor& Descriptor,
		const TSharedPtr<FJsonObject>& Row,
		UObject* Target,
		const FString& TargetRevision,
		UObject* Source,
		const FString& SourceRevision,
		const TArray<TPair<FString, UObject*>>& AdditionalSources,
		FUeAgentResourceTracker& Resources)
	{
		const FUeAgentStateRevision SnapshotRevision(
			TEXT("asset-data-e2"),
			TEXT("project:asset-data:r1"),
			1);
		TArray<FUeAgentStateEntry> Entries;
		auto AppendEntry =
			[&](
				UObject& Object,
				const FString& Ref,
				const FString& Revision,
				const FString& Profile)
			{
				const TSharedPtr<FJsonObject> State =
					MakeShared<FJsonObject>();
				State->SetStringField(TEXT("target_ref"), Ref);
				State->SetStringField(
					TEXT("asset_revision"),
					Revision);
				State->SetStringField(
					TEXT("profile_id"),
					Profile);
				State->SetStringField(
					TEXT("asset_class"),
					Object.GetClass()->GetPathName());
				State->SetStringField(
					TEXT("field_fingerprint"),
					ReadFingerprint(Object, Profile));
				FUeAgentCanonicalValuePtr Current;
				FUeAgentStateSemanticError Error;
				if (!TryCanonicalValue(
						MakeShared<FJsonValueObject>(State),
						Resources,
						Current,
						Error))
				{
					return false;
				}
				FUeAgentCanonicalValuePtr Default;
				if (!FUeAgentCanonicalValue::TryObject(
						{},
						Resources,
						Default,
						Error))
				{
					return false;
				}
				Entries.Emplace(
					FUeAgentCanonicalIdentity(
						TEXT("uai.asset_data:") + Ref,
						Row->GetStringField(
							TEXT("native_operation_type")),
						Object.GetOutermost()->GetName(),
						Ref,
						TEXT("asset_data"),
						TEXT("project:asset-data:r1")),
					FUeAgentCanonicalType(
						TEXT("asset_data_current_state"),
						EUeAgentValueKind::Object,
						false,
						TEXT("object")),
					Current.ToSharedRef(),
					FUeAgentDefaultValue(
						EUeAgentDefaultSourceKind::Schema,
						TEXT("asset-data:e2:empty-default"),
						TEXT("test-only independent readback"),
						Default.ToSharedRef()),
					FUeAgentStateDetails(
						Ref,
						TEXT(""),
						{TEXT("AssetDataSimpleEffect")},
						true,
						TEXT("available"),
						{}),
					SnapshotRevision);
				return true;
			};

		if (Target
			&& !AppendEntry(
				*Target,
				TEXT("uai://asset/fixture-target"),
				TargetRevision,
				Row->GetStringField(TEXT("profile_id"))))
		{
			return nullptr;
		}
		if (Source
			&& !AppendEntry(
				*Source,
				TEXT("uai://asset/fixture-source"),
				SourceRevision,
				TEXT("data_object")))
		{
			return nullptr;
		}
		for (const TPair<FString, UObject*>& Additional
			: AdditionalSources)
		{
			if (!Additional.Value
				|| !AppendEntry(
					*Additional.Value,
					Additional.Key,
					TEXT("rev:additional-source:1"),
					TEXT("data_object")))
			{
				return nullptr;
			}
		}

		FUeAgentCanonicalSnapshotPtr Snapshot;
		FUeAgentStateSemanticError Error;
		if (!Test->TestTrue(
				TEXT("create E2 independent AssetData snapshot"),
				FUeAgentCanonicalSnapshot::TryCreate(
					TEXT("snapshot:asset-data:e2:")
						+ Descriptor.GetCapabilityId(),
					EUeAgentStatePlane::AuthoringMemory,
					SnapshotRevision,
					FUeAgentTemporalPoint(
						TEXT("owned-worker-clock"),
						TEXT("1")),
					FUeAgentStateProvenance(
						TEXT("independent_readback"),
						TEXT("owned-asset-native-api"),
						TEXT("asset-data:e2:prestate")),
					Entries,
					Resources,
					Snapshot,
					Error)))
		{
			return nullptr;
		}
		return Snapshot;
	}

	FUeAgentExecutionContextRefOwner MakeExecutionContext(
		const FUeAgentDescriptorIdentity& Descriptor)
	{
		FUeAgentExecutionContextInput Input;
		Input.ProjectInstanceId = TEXT("project:asset-data");
		Input.EditorInstanceId = TEXT("editor:asset-data-worker");
		Input.StatePlane = EUeAgentStatePlane::AuthoringMemory;
		Input.WorldType = EUeAgentExecutionWorldType::Editor;
		Input.ContextRevision = TEXT("project:asset-data:r1");
		FUeAgentExecutionContextRefPtr Context;
		FUeAgentExecutionContextError Error;
		check(FUeAgentExecutionContextRef::TryCreate(
			Input,
			Descriptor,
			EUeAgentRequestKind::Operation,
			FUeAgentExecutionContextPolicy(
				{EUeAgentStatePlane::AuthoringMemory},
				{},
				EUeAgentContextResolutionPolicy::ExplicitOnly,
				EUeAgentTransientContextPolicy::
					TemporaryMutationAllowed),
			Context,
			Error));
		return Context.ToSharedRef();
	}

	void SetChangeValue(
		const TSharedPtr<FJsonObject>& Input,
		const int32 Index,
		const TSharedPtr<FJsonValue>& CanonicalValue)
	{
		Input->GetArrayField(TEXT("changes"))[Index]
			->AsObject()
			->GetObjectField(TEXT("value"))
			->SetField(
				TEXT("canonical_value"),
				CanonicalValue);
	}

	void AddChange(
		const TSharedPtr<FJsonObject>& Input,
		const FString& FieldId,
		const FString& Presence,
		const FString& TypeId,
		const TSharedPtr<FJsonValue>& CanonicalValue)
	{
		const TSharedPtr<FJsonObject> Unit =
			MakeShared<FJsonObject>();
		Unit->SetStringField(TEXT("field_id"), FieldId);
		Unit->SetStringField(TEXT("presence"), Presence);
		const TSharedPtr<FJsonObject> Value =
			MakeShared<FJsonObject>();
		Value->SetStringField(TEXT("type_id"), TypeId);
		Value->SetField(
			TEXT("canonical_value"),
			CanonicalValue);
		Unit->SetObjectField(TEXT("value"), Value);
		TArray<TSharedPtr<FJsonValue>> Changes =
			Input->GetArrayField(TEXT("changes"));
		Changes.Add(MakeShared<FJsonValueObject>(Unit));
		Input->SetArrayField(TEXT("changes"), MoveTemp(Changes));
	}

	void AddFixtureExampleChange(
		const TSharedPtr<FJsonObject>& Fixture,
		const TSharedPtr<FJsonObject>& Row,
		const TSharedPtr<FJsonObject>& Input,
		const FString& FieldId)
	{
		const TSharedPtr<FJsonObject> Field =
			FindFieldContract(Fixture, Row, FieldId);
		check(Field.IsValid());
		const TArray<FString> Presences =
			StringArray(Field, TEXT("allowed_presences"));
		check(!Presences.IsEmpty());
		AddChange(
			Input,
			FieldId,
			Presences[0],
			Field->GetStringField(TEXT("type_id")),
			CloneJsonValue(
				Field->TryGetField(TEXT("example_value"))));
	}

	void SetChangePresenceOnly(
		const TSharedPtr<FJsonObject>& Input,
		const int32 Index,
		const FString& Presence)
	{
		const TSharedPtr<FJsonObject> Unit =
			Input->GetArrayField(TEXT("changes"))[Index]
				->AsObject();
		Unit->SetStringField(TEXT("presence"), Presence);
		Unit->RemoveField(TEXT("value"));
	}

	void AddPresenceOnlyChange(
		const TSharedPtr<FJsonObject>& Input,
		const FString& FieldId,
		const FString& Presence)
	{
		const TSharedPtr<FJsonObject> Unit =
			MakeShared<FJsonObject>();
		Unit->SetStringField(TEXT("field_id"), FieldId);
		Unit->SetStringField(TEXT("presence"), Presence);
		TArray<TSharedPtr<FJsonValue>> Changes =
			Input->GetArrayField(TEXT("changes"));
		Changes.Add(MakeShared<FJsonValueObject>(Unit));
		Input->SetArrayField(TEXT("changes"), MoveTemp(Changes));
	}

	TSharedPtr<FJsonValue> MakePaperFlipbookFrame(
		const TSharedPtr<FJsonValue>& SpriteRef,
		const int32 FrameRun)
	{
		const TSharedPtr<FJsonObject> Frame =
			MakeShared<FJsonObject>();
		Frame->SetField(TEXT("sprite_ref"), SpriteRef);
		Frame->SetNumberField(TEXT("frame_run"), FrameRun);
		return MakeShared<FJsonValueObject>(Frame);
	}

	TSharedPtr<FJsonValue> MakeExtent2i(
		const int32 X,
		const int32 Y)
	{
		const TSharedPtr<FJsonObject> Extent =
			MakeShared<FJsonObject>();
		Extent->SetNumberField(TEXT("x"), X);
		Extent->SetNumberField(TEXT("y"), Y);
		return MakeShared<FJsonValueObject>(Extent);
	}

	TSharedPtr<FJsonValue> MakeLinearColor(
		const double R,
		const double G,
		const double B,
		const double A)
	{
		const TSharedPtr<FJsonObject> Color =
			MakeShared<FJsonObject>();
		Color->SetNumberField(TEXT("r"), R);
		Color->SetNumberField(TEXT("g"), G);
		Color->SetNumberField(TEXT("b"), B);
		Color->SetNumberField(TEXT("a"), A);
		return MakeShared<FJsonValueObject>(Color);
	}

	TSharedPtr<FJsonValue> MakePaperTileMapCell(
		const int32 X,
		const int32 Y,
		const FString& TileSetRef,
		const int32 TileIndex,
		const bool bFlipHorizontal,
		const bool bFlipVertical,
		const bool bFlipDiagonal)
	{
		const TSharedPtr<FJsonObject> Cell =
			MakeShared<FJsonObject>();
		Cell->SetNumberField(TEXT("x"), X);
		Cell->SetNumberField(TEXT("y"), Y);
		Cell->SetStringField(
			TEXT("tile_set_ref"),
			TileSetRef);
		Cell->SetNumberField(
			TEXT("tile_index"),
			TileIndex);
		Cell->SetBoolField(
			TEXT("flip_horizontal"),
			bFlipHorizontal);
		Cell->SetBoolField(
			TEXT("flip_vertical"),
			bFlipVertical);
		Cell->SetBoolField(
			TEXT("flip_diagonal"),
			bFlipDiagonal);
		return MakeShared<FJsonValueObject>(Cell);
	}

	TSharedPtr<FJsonValue> MakePaperTileMapLayer(
		const FString& Name,
		const bool bVisibleInEditor,
		const bool bVisibleInGame,
		const bool bCollides,
		const TSharedPtr<FJsonValue>& ThicknessOverride,
		const TSharedPtr<FJsonValue>& OffsetOverride,
		const TSharedPtr<FJsonValue>& Color,
		TArray<TSharedPtr<FJsonValue>> Cells)
	{
		const TSharedPtr<FJsonObject> Layer =
			MakeShared<FJsonObject>();
		Layer->SetStringField(TEXT("name"), Name);
		Layer->SetBoolField(
			TEXT("visible_in_editor"),
			bVisibleInEditor);
		Layer->SetBoolField(
			TEXT("visible_in_game"),
			bVisibleInGame);
		Layer->SetBoolField(TEXT("collides"), bCollides);
		Layer->SetField(
			TEXT("collision_thickness_override"),
			ThicknessOverride);
		Layer->SetField(
			TEXT("collision_offset_override"),
			OffsetOverride);
		Layer->SetField(TEXT("color"), Color);
		Layer->SetArrayField(
			TEXT("cells"),
			MoveTemp(Cells));
		return MakeShared<FJsonValueObject>(Layer);
	}

	TSharedPtr<FJsonValue> MakePaperTileSetTerrain(
		const FString& Name,
		const int32 CenterTileIndex)
	{
		const TSharedPtr<FJsonObject> Terrain =
			MakeShared<FJsonObject>();
		Terrain->SetStringField(TEXT("name"), Name);
		Terrain->SetNumberField(
			TEXT("center_tile_index"),
			CenterTileIndex);
		return MakeShared<FJsonValueObject>(Terrain);
	}

	TSharedPtr<FJsonValue> MakeClassPathArray(
		TArray<FString> ClassPaths)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(ClassPaths.Num());
		for (FString& ClassPath : ClassPaths)
		{
			Values.Add(
				MakeShared<FJsonValueString>(
					MoveTemp(ClassPath)));
		}
		return MakeShared<FJsonValueArray>(
			MoveTemp(Values));
	}

	TSharedPtr<FJsonValue> MakeInputMapping(
		const FString& ActionRef,
		const FString& Key,
		TArray<FString> TriggerClasses,
		TArray<FString> ModifierClasses)
	{
		const TSharedPtr<FJsonObject> Mapping =
			MakeShared<FJsonObject>();
		Mapping->SetStringField(
			TEXT("action_ref"),
			ActionRef);
		Mapping->SetStringField(TEXT("key"), Key);
		Mapping->SetField(
			TEXT("triggers"),
			MakeClassPathArray(MoveTemp(TriggerClasses)));
		Mapping->SetField(
			TEXT("modifiers"),
			MakeClassPathArray(MoveTemp(ModifierClasses)));
		return MakeShared<FJsonValueObject>(Mapping);
	}

	TSharedPtr<FJsonValue> MakeInputMappingIdentity(
		const FString& ActionRef,
		const FString& Key)
	{
		const TSharedPtr<FJsonObject> Mapping =
			MakeShared<FJsonObject>();
		Mapping->SetStringField(
			TEXT("action_ref"),
			ActionRef);
		Mapping->SetStringField(TEXT("key"), Key);
		return MakeShared<FJsonValueObject>(Mapping);
	}

	bool BuildPlan(
		FAutomationTestBase* Test,
		const FUeAgentOperationDescriptor& Descriptor,
		const TSharedPtr<FJsonObject>& Input,
		const FUeAgentCanonicalSnapshotRef& Snapshot,
		IUeAgentAssetDataSimpleEffectFactoryCandidateResolver&
			FactoryCandidates,
		const TSharedRef<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe>& Resources,
		FPlanningPreflightPlanPtr& OutPlan,
		FExecutionMutationError& OutError)
	{
		FString InputJson;
		const TSharedRef<TJsonWriter<>> Writer =
			TJsonWriterFactory<>::Create(&InputJson);
		if (!FJsonSerializer::Serialize(
				Input.ToSharedRef(),
				Writer))
		{
			return false;
		}
		TSharedPtr<const FUeAgentContractSchema> Compiled;
		FUeAgentContractSchemaError SchemaError;
		if (!FUeAgentContractSchema::TryCompile(
				Descriptor.GetInputSchema(),
				Compiled,
				SchemaError)
			|| !Compiled.IsValid())
		{
			return false;
		}
		TOptional<FUeAgentContractValue> CanonicalPayload;
		if (!Compiled->TryValidateAndCanonicalize(
				FUeAgentContractValue(InputJson),
				[](int64,
					const FString&,
					FUeAgentContractSchemaError&)
				{
					return true;
				},
				CanonicalPayload,
				SchemaError)
			|| !CanonicalPayload.IsSet())
		{
			return false;
		}
		const FUeAgentExecutionContextRefOwner Context =
			MakeExecutionContext(Descriptor.GetIdentity());
		TOptional<FChangeSetDraft> Draft;
		if (!TryBuildAssetDataSimpleEffectChangeSetDraft(
				FUeAgentOperationRequest(
					Descriptor.GetIdentity(),
					TEXT("request:asset-data-e2"),
					TEXT("idempotency:asset-data-e2"),
					CanonicalPayload.GetValue()),
				*Context,
				Snapshot,
				FactoryCandidates,
				*Resources,
				Draft,
				OutError)
			|| !Draft.IsSet()
			|| !Draft->IsValid())
		{
			return false;
		}
		FPlanningResult PlanningResult;
		FPlanningPreflightError PlanningError;
		if (!TryPlanChanges(
				FPlanningRequest(
					Draft.GetValue(),
					FUeAgentCallerContext(
						TEXT("agent:asset-data-e2"),
						{TEXT("uai:asset-data:write")},
						TEXT("correlation:asset-data-e2")),
					false),
				*Resources,
				PlanningResult,
				PlanningError)
			|| !PlanningResult.IsReady()
			|| !PlanningResult.GetPlan().IsValid())
		{
			return false;
		}
		OutPlan = PlanningResult.GetPlan();
		return true;
	}

	struct FNativeRequestOwner
	{
		FNativeRequestOwner(
			TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
				InResources,
			FPlanningPreflightPlanRef InPlan,
			FUeAgentExecutionContextRefOwner InContext)
			: Resources(MoveTemp(InResources))
			, Cancellation([this]() { return Now; })
			, Plan(MoveTemp(InPlan))
			, Context(MoveTemp(InContext))
		{
		}

		FExecutionMutationRequest MakeRequest()
		{
			return FExecutionMutationRequest(
				TEXT("operation:asset-data"),
				TEXT("owner:asset-data"),
				Plan,
				Context,
				Resources,
				FUeAgentOperationDeadline::FromTimeout(
					60.0,
					[this]() { return Now; }),
				Cancellation.GetToken(),
				EExecutionThreadAffinity::GameThread,
				EExecutionReentrancyPolicy::RejectSameTarget);
		}

		double Now = FPlatformTime::Seconds();
		TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>
			Resources;
		FUeAgentCancellationSource Cancellation;
		FPlanningPreflightPlanRef Plan;
		FUeAgentExecutionContextRefOwner Context;
	};

	FUeAgentGenericEffectBinding MakeBinding(
		const FUeAgentOperationDescriptor& Descriptor,
		const FUeAgentCanonicalIdentity& Identity)
	{
		return FUeAgentGenericEffectBinding(
			FUeAgentCandidateBinding(
				TEXT("binding:") + Descriptor.GetCapabilityId(),
				FUeAgentCandidateSummary(
					Identity,
					Descriptor.GetIdentity(),
					TEXT("asset_data"),
					Descriptor.GetCapabilityId(),
					TEXT("owned asset data target"),
					{TEXT("Asset"), TEXT("Data")}),
				FUeAgentCatalogRevision(
					TEXT("asset-data-e2"),
					TEXT("project:asset-data:r1"),
					1),
				TEXT("uai://asset/")
					+ Descriptor.GetCapabilityId(),
				TEXT("asset-data-read"),
				{
					EUeAgentCandidateOperation::Create,
					EUeAgentCandidateOperation::Update,
				},
				TEXT("fixture:asset-data-e2:binding")),
			TEXT("UAI.AssetData:CanonicalObject"),
			FUeAgentCanonicalType(
				TEXT("asset_data_canonical_object"),
				EUeAgentValueKind::Object,
				false,
				TEXT("object")),
			TEXT("fixture:asset-data-e2:type-binding"));
	}

	bool RunNativeTests(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonObject>& Fixture,
		const TSet<FString>* IncludedScenarioIds = nullptr)
	{
		TSharedPtr<const FUeAgentDescriptorRegistry> Registry;
		FString RegistryError;
		if (!Test->TestTrue(
				TEXT("create AssetData production registry"),
				TryCreateAssetDataSimpleEffectDescriptorRegistry(
					Registry,
					RegistryError))
			|| !Registry.IsValid())
		{
			return false;
		}
		const TSharedRef<
			FOwnedAssetResolver,
			ESPMode::ThreadSafe> Resolver =
				MakeShared<
					FOwnedAssetResolver,
					ESPMode::ThreadSafe>();
		const TSharedRef<
			FFixtureFactoryCandidateResolver,
			ESPMode::ThreadSafe> FactoryCandidates =
				MakeShared<
					FFixtureFactoryCandidateResolver,
					ESPMode::ThreadSafe>(Fixture);

		for (const TSharedPtr<FJsonValue>& ScenarioValue
			: Fixture->GetArrayField(TEXT("e2_scenarios")))
		{
			const TSharedPtr<FJsonObject> Scenario =
				ScenarioValue->AsObject();
			const FString ScenarioId =
				Scenario->GetStringField(TEXT("scenario_id"));
			if (IncludedScenarioIds
				&& !IncludedScenarioIds->Contains(ScenarioId))
			{
				continue;
			}
			const FString DescriptorId =
				Scenario->GetStringField(TEXT("descriptor_id"));
			const FUeAgentOperationDescriptor* Descriptor =
				Registry->FindByDescriptorId(DescriptorId);
			if (!Test->TestNotNull(
					*(ScenarioId + TEXT(" descriptor exists")),
					Descriptor))
			{
				continue;
			}
			TSharedPtr<FJsonObject> Row;
			for (const TSharedPtr<FJsonValue>& RowValue
				: Fixture->GetArrayField(TEXT("descriptors")))
			{
				if (RowValue->AsObject()->GetStringField(
						TEXT("descriptor_id"))
					== DescriptorId)
				{
					Row = RowValue->AsObject();
					break;
				}
			}
			if (!Row.IsValid())
			{
				continue;
			}

			FOwnedAssetPackage Owner;
			if (!Test->TestNotNull(
					*(ScenarioId + TEXT(" owns package")),
					Owner.Package))
			{
				continue;
			}
			UObject* Target = nullptr;
			UObject* Source = nullptr;
			TArray<TPair<FString, UObject*>> AdditionalSources;
			UMaterialInterface* FixtureMaterial = nullptr;
			USkeleton* DesiredSkeleton = nullptr;
			USkeletalMeshLODSettings* DesiredLodSettings = nullptr;
			UInputTrigger* InitialInputActionTrigger = nullptr;
			UInputModifier* InitialInputActionModifier = nullptr;
			UInputTrigger* InitialMappingATrigger = nullptr;
			UInputModifier* InitialMappingAModifier = nullptr;
			UInputTrigger* InitialMappingBTrigger = nullptr;
			UInputModifier* InitialMappingBModifier = nullptr;
			if (ScenarioId
				== TEXT("update_audio_effect_preset_settings"))
			{
				USubmixEffectSubmixEQPreset* Preset =
					Owner.New<USubmixEffectSubmixEQPreset>(
						TEXT("SubmixEqPreset"));
				if (Preset)
				{
					FSubmixEffectEQBand Band;
					Band.Frequency = 400.0f;
					Band.Bandwidth = 2.0f;
					Band.GainDb = -2.0f;
					Band.bEnabled = false;
					Preset->Settings.EQBands = {Band};
				}
				Target = Preset;
			}
			else if (ScenarioId
				== TEXT("update_sound_attenuation_settings"))
			{
				USoundAttenuation* Attenuation =
					Owner.New<USoundAttenuation>(
						TEXT("SoundAttenuation"));
				if (Attenuation)
				{
					Attenuation->Attenuation.FalloffDistance = 321.0f;
				}
				Target = Attenuation;
			}
			else if (ScenarioId
				== TEXT(
					"update_sound_class_properties_and_children"))
			{
				USoundClass* SoundClass =
					Owner.New<USoundClass>(TEXT("SoundClass"));
				USoundClass* Child =
					Owner.New<USoundClass>(TEXT("SoundClassChild"));
				if (SoundClass)
				{
					SoundClass->Properties.Volume = 0.25f;
					SoundClass->Properties.Pitch = 0.5f;
				}
				Target = SoundClass;
				Source = Child;
				Resolver->BindExisting(
					TEXT("uai://asset/fixture-source"),
					Child);
			}
			else if (ScenarioId
				== TEXT("update_sound_concurrency_settings"))
			{
				USoundConcurrency* Concurrency =
					Owner.New<USoundConcurrency>(
						TEXT("SoundConcurrency"));
				if (Concurrency)
				{
					Concurrency->Concurrency.MaxCount = 2;
				}
				Target = Concurrency;
			}
			else if (ScenarioId
				== TEXT("update_sound_mix_modifiers"))
			{
				USoundMix* SoundMix =
					Owner.New<USoundMix>(TEXT("SoundMix"));
				USoundClass* SoundClass =
					Owner.New<USoundClass>(
						TEXT("SoundMixClass"));
				if (SoundMix)
				{
					SoundMix->InitialDelay = 2.0f;
				}
				Target = SoundMix;
				Source = SoundClass;
				Resolver->BindExisting(
					TEXT("uai://asset/fixture-source"),
					SoundClass);
			}
			else if (ScenarioId
				== TEXT(
					"update_sound_submix_parent_and_effect_chain"))
			{
				USoundSubmix* Submix =
					Owner.New<USoundSubmix>(TEXT("SoundSubmix"));
				USoundSubmix* Parent =
					Owner.New<USoundSubmix>(
						TEXT("SoundSubmixParent"));
				USubmixEffectSubmixEQPreset* Effect =
					Owner.New<USubmixEffectSubmixEQPreset>(
						TEXT("SoundSubmixEffect"));
				Target = Submix;
				Source = Parent;
				Resolver->BindExisting(
					TEXT("uai://asset/fixture-source"),
					Parent);
				Resolver->BindExisting(
					TEXT("uai://asset/fixture-source-b"),
					Effect);
				AdditionalSources.Emplace(
					TEXT("uai://asset/fixture-source-b"),
					Effect);
			}
			else if (ScenarioId == TEXT("replace_curve_keys"))
			{
				UCurveFloat* Curve =
					Owner.New<UCurveFloat>(TEXT("CurveFloat"));
				if (Curve)
				{
					Curve->FloatCurve.AddKey(0.0f, -1.0f);
				}
				Target = Curve;
			}
			else if (ScenarioId
				== TEXT("update_data_object_registered_fields"))
			{
				UUeAgentAssetDataSimpleEffectDataAsset* Data =
					Owner.New<UUeAgentAssetDataSimpleEffectDataAsset>(
						TEXT("RegisteredData"));
				UUeAgentAssetDataSimpleEffectDataAsset* Related =
					Owner.New<UUeAgentAssetDataSimpleEffectDataAsset>(
						TEXT("RelatedData"));
				Target = Data;
				Source = Related;
				Resolver->BindExisting(
					TEXT("uai://asset/fixture-source"),
					Related);
			}
			else if (ScenarioId == TEXT("update_file_media_source"))
			{
				UFileMediaSource* MediaSource =
					Owner.New<UFileMediaSource>(
						TEXT("FileMediaSource"));
				if (MediaSource)
				{
					MediaSource->SetFilePath(TEXT("old.mov"));
					MediaSource->PrecacheFile = false;
				}
				Target = MediaSource;
			}
			else if (ScenarioId
				== TEXT("update_runtime_virtual_texture"))
			{
				Target =
					Owner.New<URuntimeVirtualTexture>(
						TEXT("RuntimeVirtualTexture"));
			}
			else if (ScenarioId
				== TEXT("update_sparse_volume_texture_settings"))
			{
				UStreamableSparseVolumeTexture* Sparse =
					Owner.New<UStreamableSparseVolumeTexture>(
						TEXT("SparseVolumeTexture"));
				if (Sparse)
				{
					Sparse->AddressX = TA_Wrap;
					Sparse->AddressY = TA_Wrap;
					Sparse->AddressZ = TA_Wrap;
					Sparse->StreamingPoolSizeFactor = 3.0f;
				}
				Target = Sparse;
			}
			else if (ScenarioId == TEXT("clone_data_object"))
			{
				UUeAgentAssetDataSimpleEffectDataAsset* CloneSource =
					Owner.New<UUeAgentAssetDataSimpleEffectDataAsset>(
						TEXT("SourceDataAsset"));
				UUeAgentAssetDataSimpleEffectDataAsset* Related =
					Owner.New<UUeAgentAssetDataSimpleEffectDataAsset>(
						TEXT("CloneRelatedDataAsset"));
				if (CloneSource && Related)
				{
					CloneSource->Label =
						TEXT("Clone Source High Signal");
					CloneSource->Count = -73;
					CloneSource->Weight = 4.125f;
					CloneSource->bEnabled = true;
					CloneSource->RelatedAsset = Related;
				}
				Source = CloneSource;
				Resolver->BindCloneSource(
					TEXT("uai://asset/fixture-source"),
					Source);
			}
			else if (ScenarioId
				== TEXT("update_texture_settings"))
			{
				UTextureRenderTarget2D* RenderTarget =
					Owner.New<UTextureRenderTarget2D>(
						TEXT("RenderTarget"));
				RenderTarget->InitAutoFormat(16, 16);
				Target = RenderTarget;
			}
			else if (ScenarioId
				== TEXT("update_static_mesh_collections"))
			{
				UStaticMesh* StaticMesh =
					Owner.New<UStaticMesh>(TEXT("StaticMesh"));
				StaticMesh->CreateBodySetup();
				StaticMesh->AddSourceModel();
				FMeshDescription MeshDescription;
				FStaticMeshAttributes MeshAttributes(MeshDescription);
				MeshAttributes.Register();
				FMeshDescriptionBuilder MeshBuilder;
				MeshBuilder.SetMeshDescription(&MeshDescription);
				const FPolygonGroupID PolygonGroup =
					MeshBuilder.AppendPolygonGroup(TEXT("Original"));
				const FVertexID VertexA =
					MeshBuilder.AppendVertex(FVector::ZeroVector);
				const FVertexID VertexB =
					MeshBuilder.AppendVertex(FVector(100.0, 0.0, 0.0));
				const FVertexID VertexC =
					MeshBuilder.AppendVertex(FVector(0.0, 100.0, 0.0));
				MeshBuilder.AppendTriangle(
					VertexA,
					VertexB,
					VertexC,
					PolygonGroup);
				StaticMesh->CreateMeshDescription(0, MeshDescription);
				StaticMesh->CommitMeshDescription(0);
				FixtureMaterial =
					LoadObject<UMaterialInterface>(
						nullptr,
						TEXT(
							"/Engine/EngineMaterials/"
							"DefaultMaterial.DefaultMaterial"));
				StaticMesh->SetStaticMaterials({
					FStaticMaterial(
						nullptr,
						TEXT("Original")),
				});
				FKBoxElem OriginalBox;
				OriginalBox.Center = FVector(-1.0, -2.0, -3.0);
				OriginalBox.X = 2.0;
				OriginalBox.Y = 4.0;
				OriginalBox.Z = 6.0;
				StaticMesh->GetBodySetup()
					->AggGeom.BoxElems.Add(OriginalBox);
				StaticMesh->GetBodySetup()->CollisionTraceFlag =
					CTF_UseDefault;
				UStaticMeshSocket* OriginalSocket =
					NewObject<UStaticMeshSocket>(
						StaticMesh,
						NAME_None,
						RF_Transactional);
				OriginalSocket->SocketName = TEXT("OriginalSocket");
				OriginalSocket->RelativeLocation =
					FVector(-1.0, -2.0, -3.0);
				StaticMesh->Sockets.Add(OriginalSocket);
				StaticMesh->GetSourceModel(0)
					.BuildSettings.bRecomputeNormals = false;
				if (FixtureMaterial)
				{
					Resolver->BindExisting(
						TEXT("uai://asset/fixture-material"),
						FixtureMaterial);
					AdditionalSources.Emplace(
						TEXT("uai://asset/fixture-material"),
						FixtureMaterial);
				}
				Target = StaticMesh;
			}
			else if (ScenarioId
				== TEXT("replace_data_table_rows"))
			{
				UDataTable* DataTable =
					Owner.New<UDataTable>(TEXT("DataTable"));
				DataTable->RowStruct =
					FUeAgentAssetDataSimpleEffectTableRow::
						StaticStruct();
				FUeAgentAssetDataSimpleEffectTableRow OriginalRow;
				OriginalRow.Label = TEXT("Original");
				OriginalRow.Count = 3;
				OriginalRow.Weight = 0.25f;
				OriginalRow.bEnabled = false;
				DataTable->AddRow(
					TEXT("OriginalRow"),
					OriginalRow);
				Target = DataTable;
			}
			else if (ScenarioId
				== TEXT("replace_skeletal_mesh_content"))
			{
				USkeletalMesh* SkeletalMesh =
					Owner.New<USkeletalMesh>(
						TEXT("SkeletalMesh"));
				FixtureMaterial =
					LoadObject<UMaterialInterface>(
						nullptr,
						TEXT(
							"/Engine/EngineMaterials/"
							"DefaultMaterial.DefaultMaterial"));
				USkeleton* OriginalSkeleton =
					Owner.New<USkeleton>(
						TEXT("OriginalSkeleton"));
				USkeletalMeshLODSettings* OriginalLodSettings =
					Owner.New<USkeletalMeshLODSettings>(
						TEXT("OriginalLodSettings"));
				DesiredSkeleton =
					Owner.New<USkeleton>(
						TEXT("DesiredSkeleton"));
				DesiredLodSettings =
					Owner.New<USkeletalMeshLODSettings>(
						TEXT("DesiredLodSettings"));
				SkeletalMesh->SetMaterials({
					FSkeletalMaterial(
						nullptr,
						FName(TEXT("Original")),
						FName(TEXT("Original"))),
				});
				SkeletalMesh->SetImportedBounds(
					FBoxSphereBounds(
						FBox(
							FVector(-1.0, -2.0, -3.0),
							FVector(1.0, 2.0, 3.0))));
				SkeletalMesh->SetSkeleton(OriginalSkeleton);
				SkeletalMesh->SetLODSettings(
					OriginalLodSettings);
				if (FixtureMaterial)
				{
					Resolver->BindExisting(
						TEXT("uai://asset/fixture-material"),
						FixtureMaterial);
					AdditionalSources.Emplace(
						TEXT("uai://asset/fixture-material"),
						FixtureMaterial);
				}
				Resolver->BindExisting(
					TEXT("uai://asset/fixture-skeleton"),
					DesiredSkeleton);
				Resolver->BindExisting(
					TEXT(
						"uai://asset/fixture-lod-settings"),
					DesiredLodSettings);
				AdditionalSources.Emplace(
					TEXT("uai://asset/fixture-skeleton"),
					DesiredSkeleton);
				AdditionalSources.Emplace(
					TEXT(
						"uai://asset/fixture-lod-settings"),
					DesiredLodSettings);
				Target = SkeletalMesh;
			}
			else if (ScenarioId
				== TEXT("update_media_player_fields"))
			{
				UMediaPlayer* MediaPlayer =
					Owner.New<UMediaPlayer>(TEXT("MediaPlayer"));
				if (MediaPlayer)
				{
					MediaPlayer->PlayOnOpen = false;
					MediaPlayer->SetLooping(false);
				}
				Target = MediaPlayer;
			}
			else if (ScenarioId
				== TEXT("update_media_texture_fields"))
			{
				UMediaTexture* MediaTexture =
					Owner.New<UMediaTexture>(TEXT("MediaTexture"));
				if (MediaTexture)
				{
					MediaTexture->SetMediaPlayer(nullptr);
					MediaTexture->AutoClear = false;
					MediaTexture->ClearColor =
						FLinearColor(0.9f, 0.8f, 0.7f, 0.6f);
				}
				Target = MediaTexture;
				Source =
					Owner.New<UMediaPlayer>(
						TEXT("ReferencedMediaPlayer"));
				Resolver->BindExisting(
					TEXT("uai://asset/fixture-source"),
					Source);
			}
			else if (ScenarioId.StartsWith(
				TEXT("update_input_action_")))
			{
				UInputAction* InputAction =
					Owner.New<UInputAction>(
						TEXT("InputAction"));
				if (InputAction)
				{
					InputAction->ValueType =
						EInputActionValueType::Boolean;
					InputAction->bConsumeInput = true;
					InitialInputActionTrigger =
						NewObject<UInputTriggerPressed>(
							InputAction,
							NAME_None,
							RF_Transactional);
					InputAction->Triggers.Add(
						InitialInputActionTrigger);
					InitialInputActionModifier =
						NewObject<UInputModifierNegate>(
							InputAction,
							NAME_None,
							RF_Transactional);
					InputAction->Modifiers.Add(
						InitialInputActionModifier);
				}
				Target = InputAction;
			}
			else if (ScenarioId.StartsWith(
				TEXT("update_input_mapping_context_")))
			{
				UInputMappingContext* MappingContext =
					Owner.New<UInputMappingContext>(
						TEXT("InputMappingContext"));
				UInputAction* ActionA =
					Owner.New<UInputAction>(
						TEXT("InputMappingActionA"));
				UInputAction* ActionB =
					Owner.New<UInputAction>(
						TEXT("InputMappingActionB"));
				UInputAction* ActionC =
					Owner.New<UInputAction>(
						TEXT("InputMappingActionC"));
				if (MappingContext
					&& ActionA
					&& ActionB
					&& ActionC)
				{
					FEnhancedActionKeyMapping& MappingA =
						MappingContext->MapKey(
							ActionA,
							EKeys::SpaceBar);
					InitialMappingATrigger =
						NewObject<UInputTriggerPressed>(
							MappingContext,
							NAME_None,
							RF_Transactional);
					MappingA.Triggers.Add(
						InitialMappingATrigger);
					InitialMappingAModifier =
						NewObject<UInputModifierNegate>(
							MappingContext,
							NAME_None,
							RF_Transactional);
					MappingA.Modifiers.Add(
						InitialMappingAModifier);
					FEnhancedActionKeyMapping& MappingB =
						MappingContext->MapKey(
							ActionB,
							EKeys::E);
					InitialMappingBTrigger =
						NewObject<UInputTriggerDown>(
							MappingContext,
							NAME_None,
							RF_Transactional);
					MappingB.Triggers.Add(
						InitialMappingBTrigger);
					InitialMappingBModifier =
						NewObject<UInputModifierScalar>(
							MappingContext,
							NAME_None,
							RF_Transactional);
					MappingB.Modifiers.Add(
						InitialMappingBModifier);
					Source = ActionA;
					Resolver->BindExisting(
						TEXT("uai://asset/fixture-source"),
						ActionA);
					Resolver->BindExisting(
						TEXT(
							"uai://asset/"
							"fixture-input-action-b"),
						ActionB);
					AdditionalSources.Emplace(
						TEXT(
							"uai://asset/"
							"fixture-input-action-b"),
						ActionB);
					Resolver->BindExisting(
						TEXT(
							"uai://asset/"
							"fixture-input-action-c"),
						ActionC);
					AdditionalSources.Emplace(
						TEXT(
							"uai://asset/"
							"fixture-input-action-c"),
						ActionC);
				}
				Target = MappingContext;
			}
			else if (ScenarioId
				== TEXT("replace_texture_array_content"))
			{
				UTexture2DArray* TextureArray =
					Owner.New<UTexture2DArray>(
						TEXT("Texture2DArray"));
				UTexture2D* Initial =
					Owner.New<UTexture2D>(
						TEXT("TextureArrayInitial"));
				UTexture2D* DesiredA =
					Owner.New<UTexture2D>(
						TEXT("TextureArrayDesiredA"));
				UTexture2D* DesiredB =
					Owner.New<UTexture2D>(
						TEXT("TextureArrayDesiredB"));
				const bool bSourcesValid =
					Initial
					&& DesiredA
					&& DesiredB
					&& InitializeTexture2DSource(
						*Initial,
						4,
						11)
					&& InitializeTexture2DSource(
						*DesiredA,
						4,
						47)
					&& InitializeTexture2DSource(
						*DesiredB,
						4,
						103);
				if (TextureArray && bSourcesValid)
				{
					TextureArray->SourceTextures = {Initial};
					Test->TestTrue(
						TEXT(
							"texture array initial source "
							"assembly"),
						TextureArray
							->UpdateSourceFromSourceTextures(
								false));
					TextureArray->SRGB = true;
					TextureArray->CompressionSettings =
						TC_Default;
					TextureArray->MipGenSettings =
						TMGS_NoMipmaps;
					Source = DesiredA;
					Resolver->BindExisting(
						TEXT("uai://asset/fixture-source"),
						DesiredA);
					Resolver->BindExisting(
						TEXT("uai://asset/fixture-source-b"),
						DesiredB);
					AdditionalSources.Emplace(
						TEXT("uai://asset/fixture-source-b"),
						DesiredB);
					Owner.Package->SetDirtyFlag(false);
				}
				Target = TextureArray;
			}
			else if (ScenarioId
				== TEXT(
					"replace_texture_cube_array_content"))
			{
				UTextureCubeArray* TextureArray =
					Owner.New<UTextureCubeArray>(
						TEXT("TextureCubeArray"));
				UTextureCube* Initial =
					Owner.New<UTextureCube>(
						TEXT("TextureCubeArrayInitial"));
				UTextureCube* DesiredA =
					Owner.New<UTextureCube>(
						TEXT("TextureCubeArrayDesiredA"));
				UTextureCube* DesiredB =
					Owner.New<UTextureCube>(
						TEXT("TextureCubeArrayDesiredB"));
				const bool bSourcesValid =
					Initial
					&& DesiredA
					&& DesiredB
					&& InitializeTextureCubeSource(
						*Initial,
						2,
						19)
					&& InitializeTextureCubeSource(
						*DesiredA,
						2,
						59)
					&& InitializeTextureCubeSource(
						*DesiredB,
						2,
						131);
				if (TextureArray && bSourcesValid)
				{
					TextureArray->SourceTextures = {Initial};
					Test->TestTrue(
						TEXT(
							"texture cube array initial "
							"source assembly"),
						TextureArray
							->UpdateSourceFromSourceTextures(
								false));
					TextureArray->SRGB = true;
					TextureArray->CompressionSettings =
						TC_Default;
					TextureArray->MipGenSettings =
						TMGS_NoMipmaps;
					Source = DesiredA;
					Resolver->BindExisting(
						TEXT("uai://asset/fixture-source"),
						DesiredA);
					Resolver->BindExisting(
						TEXT("uai://asset/fixture-source-b"),
						DesiredB);
					AdditionalSources.Emplace(
						TEXT("uai://asset/fixture-source-b"),
						DesiredB);
					Owner.Package->SetDirtyFlag(false);
				}
				Target = TextureArray;
			}
			else if (ScenarioId
				== TEXT("replace_texture_cube_content"))
			{
				UTextureCube* TextureCube =
					Owner.New<UTextureCube>(
						TEXT("TextureCube"));
				const TArray<FString> FaceNames{
					TEXT("TextureCubeFacePositiveX"),
					TEXT("TextureCubeFaceNegativeX"),
					TEXT("TextureCubeFacePositiveY"),
					TEXT("TextureCubeFaceNegativeY"),
					TEXT("TextureCubeFacePositiveZ"),
					TEXT("TextureCubeFaceNegativeZ"),
				};
				const TArray<FString> FaceRefs{
					TEXT("uai://asset/fixture-source"),
					TEXT("uai://asset/fixture-source-b"),
					TEXT("uai://asset/fixture-source-c"),
					TEXT("uai://asset/fixture-source-d"),
					TEXT("uai://asset/fixture-source-e"),
					TEXT("uai://asset/fixture-source-f"),
				};
				TArray<UTexture2D*> Faces;
				bool bSourcesValid = TextureCube
					&& InitializeTextureCubeSource(
						*TextureCube,
						2,
						23);
				for (int32 Index = 0;
					Index < FaceNames.Num();
					++Index)
				{
					UTexture2D* Face =
						Owner.New<UTexture2D>(
							*FaceNames[Index]);
					bSourcesValid = bSourcesValid
						&& Face
						&& InitializeTexture2DSource(
							*Face,
							2,
							static_cast<uint8>(
								41 + Index * 29));
					Faces.Add(Face);
				}
				if (TextureCube && bSourcesValid)
				{
					TextureCube->SRGB = true;
					TextureCube->CompressionSettings =
						TC_Default;
					TextureCube->MipGenSettings =
						TMGS_NoMipmaps;
					Source = Faces[0];
					Resolver->BindExisting(
						FaceRefs[0],
						Faces[0]);
					for (int32 Index = 1;
						Index < Faces.Num();
						++Index)
					{
						Resolver->BindExisting(
							FaceRefs[Index],
							Faces[Index]);
						AdditionalSources.Emplace(
							FaceRefs[Index],
							Faces[Index]);
					}
					Owner.Package->SetDirtyFlag(false);
				}
				Target = TextureCube;
			}
			else if (ScenarioId
				== TEXT("replace_volume_texture_content"))
			{
				UVolumeTexture* VolumeTexture =
					Owner.New<UVolumeTexture>(
						TEXT("VolumeTexture"));
				UTexture2D* InitialStack =
					Owner.New<UTexture2D>(
						TEXT("VolumeTextureInitialStack"));
				UTexture2D* DesiredA =
					Owner.New<UTexture2D>(
						TEXT("VolumeTextureDesiredA"));
				UTexture2D* DesiredB =
					Owner.New<UTexture2D>(
						TEXT("VolumeTextureDesiredB"));
				UTexture2D* DesiredC =
					Owner.New<UTexture2D>(
						TEXT("VolumeTextureDesiredC"));
				const bool bSourcesValid =
					VolumeTexture
					&& InitialStack
					&& DesiredA
					&& DesiredB
					&& DesiredC
					&& InitializeVolumeTextureSource(
						*VolumeTexture,
						*InitialStack,
						2,
						2,
						17)
					&& InitializeTexture2DSource(
						*DesiredA,
						2,
						53)
					&& InitializeTexture2DSource(
						*DesiredB,
						2,
						101)
					&& InitializeTexture2DSource(
						*DesiredC,
						2,
						149);
				if (VolumeTexture && bSourcesValid)
				{
					VolumeTexture->SRGB = true;
					VolumeTexture->CompressionSettings =
						TC_Default;
					VolumeTexture->MipGenSettings =
						TMGS_NoMipmaps;
					Source = DesiredA;
					Resolver->BindExisting(
						TEXT("uai://asset/fixture-source"),
						DesiredA);
					Resolver->BindExisting(
						TEXT("uai://asset/fixture-source-b"),
						DesiredB);
					AdditionalSources.Emplace(
						TEXT("uai://asset/fixture-source-b"),
						DesiredB);
					Resolver->BindExisting(
						TEXT("uai://asset/fixture-source-c"),
						DesiredC);
					AdditionalSources.Emplace(
						TEXT("uai://asset/fixture-source-c"),
						DesiredC);
					Owner.Package->SetDirtyFlag(false);
				}
				Target = VolumeTexture;
			}
			else if (ScenarioId.StartsWith(
				TEXT("update_subuv_")))
			{
				USubUVAnimation* SubUV =
					Owner.New<USubUVAnimation>(
						TEXT("SubUVAnimation"));
				UTexture2D* InitialTexture =
					Owner.New<UTexture2D>(
						TEXT("InitialSubUVTexture"));
				if (InitialTexture)
				{
					InitialTexture->Source.Init(
						4,
						4,
						1,
						1,
						TSF_BGRA8);
				}
				if (SubUV)
				{
					SubUV->SubUVTexture = InitialTexture;
					SubUV->SubImages_Horizontal = 3;
					SubUV->SubImages_Vertical = 5;
					SubUV->BoundingMode = BVC_FourVertices;
				}
				Target = SubUV;
				if (ScenarioId
					== TEXT("update_subuv_fields_set"))
				{
					UTexture2D* DesiredTexture =
						Owner.New<UTexture2D>(
							TEXT("DesiredSubUVTexture"));
					if (DesiredTexture)
					{
						DesiredTexture->Source.Init(
							8,
							8,
							1,
							1,
							TSF_BGRA8);
					}
					Source = DesiredTexture;
					Resolver->BindExisting(
						TEXT("uai://asset/fixture-source"),
					Source);
				}
			}
			else if (ScenarioId.StartsWith(
				TEXT("update_texture_collection_")))
			{
				UTextureCollection* Collection =
					Owner.New<UTextureCollection>(
						TEXT("TextureCollection"));
				UTexture2D* TextureA =
					Owner.New<UTexture2D>(TEXT("TextureA"));
				UTexture2D* TextureB =
					Owner.New<UTexture2D>(TEXT("TextureB"));
				UTexture2D* TextureC =
					Owner.New<UTexture2D>(TEXT("TextureC"));
				if (Collection && TextureA && TextureB && TextureC)
				{
					Collection->Textures = {TextureA, TextureB};
					Source = TextureA;
					Resolver->BindExisting(
						TEXT("uai://asset/fixture-source"),
						TextureA);
					Resolver->BindExisting(
						TEXT("uai://asset/fixture-source-c"),
						TextureC);
					AdditionalSources.Emplace(
						TEXT("uai://asset/fixture-source-c"),
						TextureC);
				}
				Target = Collection;
			}
			else if (ScenarioId.StartsWith(
				TEXT("update_paper_sprite_")))
			{
				UPaperSprite* Sprite =
					Owner.New<UPaperSprite>(TEXT("PaperSprite"));
				UTexture2D* TextureA =
					Owner.New<UTexture2D>(
						TEXT("PaperSpriteTextureA"));
				UTexture2D* TextureB =
					Owner.New<UTexture2D>(
						TEXT("PaperSpriteTextureB"));
				UTexture2D* TextureExtra =
					Owner.New<UTexture2D>(
						TEXT("PaperSpriteTextureExtra"));
				if (TextureA)
				{
					TextureA->Source.Init(
						32,
						32,
						1,
						1,
						TSF_BGRA8);
				}
				if (TextureB)
				{
					TextureB->Source.Init(
						64,
						64,
						1,
						1,
						TSF_BGRA8);
				}
				if (TextureExtra)
				{
					TextureExtra->Source.Init(
						8,
						8,
						1,
						1,
						TSF_BGRA8);
				}
				if (Sprite
					&& TextureA
					&& TextureB
					&& TextureExtra)
				{
					FSpriteAssetInitParameters Initial;
					Initial.Texture = TextureA;
					Initial.AdditionalTextures = {
						TextureExtra,
					};
					Initial.Offset = FIntPoint(1, 2);
					Initial.Dimension = FIntPoint(12, 10);
					Sprite->InitializeSprite(Initial, false);
					Sprite->SetPivotMode(
						ESpritePivotMode::Custom,
						FVector2D(3.0, 4.0),
						false);
					Source = TextureB;
					Resolver->BindExisting(
						TEXT("uai://asset/fixture-source"),
						TextureB);
					AdditionalSources.Emplace(
						TEXT(
							"uai://asset/"
							"fixture-paper-extra"),
						TextureExtra);
				}
				Target = Sprite;
			}
			else if (ScenarioId.StartsWith(
				TEXT("replace_paper_flipbook_")))
			{
				UPaperFlipbook* Flipbook =
					Owner.New<UPaperFlipbook>(
						TEXT("PaperFlipbook"));
				UPaperSprite* SpriteA =
					Owner.New<UPaperSprite>(
						TEXT("PaperFlipbookSpriteA"));
				UPaperSprite* SpriteB =
					Owner.New<UPaperSprite>(
						TEXT("PaperFlipbookSpriteB"));
				UPaperSprite* SpriteC =
					Owner.New<UPaperSprite>(
						TEXT("PaperFlipbookSpriteC"));
				if (Flipbook
					&& SpriteA
					&& SpriteB
					&& SpriteC)
				{
					FScopedFlipbookMutator Mutator(Flipbook);
					Mutator.FramesPerSecond = 12.0f;
					FPaperFlipbookKeyFrame FrameA;
					FrameA.Sprite = SpriteA;
					FrameA.FrameRun = 2;
					FPaperFlipbookKeyFrame FrameB;
					FrameB.Sprite = SpriteB;
					FrameB.FrameRun = 3;
					Mutator.KeyFrames = {
						FrameA,
						FrameB,
					};
					Source = SpriteC;
					Resolver->BindExisting(
						TEXT("uai://asset/fixture-source"),
						SpriteC);
					AdditionalSources.Emplace(
						TEXT(
							"uai://asset/"
							"fixture-paper-flipbook-a"),
						SpriteA);
					Resolver->BindExisting(
						TEXT(
							"uai://asset/"
							"fixture-paper-flipbook-a"),
						SpriteA);
				}
				Target = Flipbook;
			}
			else if (ScenarioId.StartsWith(
				TEXT("replace_paper_tilemap_")))
			{
				UPaperTileMap* TileMap =
					Owner.New<UPaperTileMap>(
						TEXT("PaperTileMap"));
				UPaperTileSet* TileSetA =
					Owner.New<UPaperTileSet>(
						TEXT("PaperTileMapSetA"));
				UPaperTileSet* TileSetB =
					Owner.New<UPaperTileSet>(
						TEXT("PaperTileMapSetB"));
				UTexture2D* TextureA =
					Owner.New<UTexture2D>(
						TEXT("PaperTileMapTextureA"));
				UTexture2D* TextureB =
					Owner.New<UTexture2D>(
						TEXT("PaperTileMapTextureB"));
				if (TextureA)
				{
					TextureA->Source.Init(
						64,
						64,
						1,
						1,
						TSF_BGRA8);
				}
				if (TextureB)
				{
					TextureB->Source.Init(
						64,
						64,
						1,
						1,
						TSF_BGRA8);
				}
				if (TileMap
					&& TileSetA
					&& TileSetB
					&& TextureA
					&& TextureB)
				{
					TileSetA->SetTileSheetTexture(TextureA);
					TileSetA->SetTileSize(FIntPoint(8, 8));
					TileSetA->PostEditChange();
					TileSetB->SetTileSheetTexture(TextureB);
					TileSetB->SetTileSize(FIntPoint(8, 8));
					TileSetB->PostEditChange();

					TileMap->MapWidth = 2;
					TileMap->MapHeight = 2;
					TileMap->TileWidth = 8;
					TileMap->TileHeight = 8;
					UPaperTileLayer* Layer =
						TileMap->AddNewLayer();
					if (Layer)
					{
						Layer->LayerName =
							FText::FromString(TEXT("Original"));
						Layer->SetShouldRenderInEditor(false);
						Layer->SetLayerCollides(true);
						Layer->SetLayerCollisionThickness(
							true,
							37.0f);
						Layer->SetLayerCollisionOffset(
							true,
							-4.0f);
						Layer->SetLayerColor(
							FLinearColor(
								0.2f,
								0.3f,
								0.4f,
								0.5f));
						FPaperTileInfo Cell;
						Cell.TileSet = TileSetA;
						Cell.PackedTileIndex = 3;
						Cell.SetFlagValue(
							EPaperTileFlags::FlipVertical,
							true);
						Layer->SetCell(1, 1, Cell);
					}
					Source = TileSetB;
					Resolver->BindExisting(
						TEXT("uai://asset/fixture-source"),
						TileSetB);
					AdditionalSources.Emplace(
						TEXT(
							"uai://asset/"
							"fixture-paper-tilemap-a"),
						TileSetA);
					Resolver->BindExisting(
						TEXT(
							"uai://asset/"
							"fixture-paper-tilemap-a"),
						TileSetA);
				}
				Target = TileMap;
			}
			else if (ScenarioId.StartsWith(
				TEXT("replace_paper_tileset_")))
			{
				UPaperTileSet* TileSet =
					Owner.New<UPaperTileSet>(
						TEXT("PaperTileSet"));
				UTexture2D* TextureA =
					Owner.New<UTexture2D>(
						TEXT("PaperTileSetTextureA"));
				UTexture2D* TextureB =
					Owner.New<UTexture2D>(
						TEXT("PaperTileSetTextureB"));
				if (TextureA)
				{
					TextureA->Source.Init(
						32,
						32,
						1,
						1,
						TSF_BGRA8);
				}
				if (TextureB)
				{
					TextureB->Source.Init(
						64,
						32,
						1,
						1,
						TSF_BGRA8);
				}
				if (TileSet && TextureA && TextureB)
				{
					TileSet->SetTileSheetTexture(TextureA);
					TileSet->SetTileSize(FIntPoint(8, 8));
					TileSet->SetMargin(FIntMargin(0));
					TileSet->SetPerTileSpacing(
						FIntPoint::ZeroValue);
					TileSet->PostEditChange();
					FPaperTileSetTerrain OriginalTerrain;
					OriginalTerrain.TerrainName =
						TEXT("OriginalTerrain");
					OriginalTerrain.CenterTileIndex = 1;
					TileSet->AddTerrainDescription(
						OriginalTerrain);
					if (FPaperTileMetadata* Metadata =
						TileSet->GetMutableTileMetadata(1))
					{
						Metadata->UserDataName =
							TEXT("OriginalMetadata");
						Metadata->TerrainMembership[0] = 0;
					}
					Source = TextureB;
					Resolver->BindExisting(
						TEXT("uai://asset/fixture-source"),
						TextureB);
				}
				Target = TileSet;
			}

			if (Target)
			{
				Resolver->BindExisting(
					TEXT("uai://asset/fixture-target"),
					Target);
			}
			Resolver->BindCreationTarget(
				TEXT("uai://asset/fixture-target"),
				Owner.Package,
				FName(TEXT("CreatedTarget")));

			const bool bDirtyBefore =
				Owner.Package->IsDirty();
			const FString TargetFingerprintBefore =
				Target
					? ReadFingerprint(
						*Target,
						Row->GetStringField(
							TEXT("profile_id")))
					: FString();
			const FString SourceFingerprintBefore =
				Source
					? ReadFingerprint(*Source, TEXT("data_object"))
					: FString();

			const TSharedRef<
				FUeAgentResourceTracker,
				ESPMode::ThreadSafe> Resources =
					MakeResources();
			const FUeAgentCanonicalSnapshotPtr Snapshot =
				MakeSnapshot(
					Test,
					*Descriptor,
					Row,
					Target,
					TEXT("rev:target:1"),
					Source,
					TEXT("rev:source:1"),
					AdditionalSources,
					*Resources);
			if (!Snapshot.IsValid())
			{
				continue;
			}
			const TSharedPtr<FJsonObject> Input =
				BuildValidInput(Fixture, Row);
			if (ScenarioId
				== TEXT(
					"update_sound_class_properties_and_children"))
			{
				AddFixtureExampleChange(
					Fixture,
					Row,
					Input,
					TEXT("child_classes"));
			}
			else if (ScenarioId
				== TEXT("update_sound_mix_modifiers"))
			{
				AddFixtureExampleChange(
					Fixture,
					Row,
					Input,
					TEXT("initial_delay"));
			}
			else if (ScenarioId
				== TEXT(
					"update_sound_submix_parent_and_effect_chain"))
			{
				AddFixtureExampleChange(
					Fixture,
					Row,
					Input,
					TEXT("effect_chain"));
			}
			else if (ScenarioId
				== TEXT("update_file_media_source"))
			{
				AddFixtureExampleChange(
					Fixture,
					Row,
					Input,
					TEXT("options"));
			}
			else if (ScenarioId
				== TEXT("update_runtime_virtual_texture"))
			{
				AddFixtureExampleChange(
					Fixture,
					Row,
					Input,
					TEXT("material_type"));
				AddFixtureExampleChange(
					Fixture,
					Row,
					Input,
					TEXT("compression"));
			}
			else if (ScenarioId
				== TEXT("update_sparse_volume_texture_settings"))
			{
				AddFixtureExampleChange(
					Fixture,
					Row,
					Input,
					TEXT("streaming"));
			}
			else if (ScenarioId
					== TEXT("replace_texture_array_content")
				|| ScenarioId
					== TEXT(
						"replace_texture_cube_array_content")
				|| ScenarioId
					== TEXT("replace_texture_cube_content")
				|| ScenarioId
					== TEXT("replace_volume_texture_content"))
			{
				const TSharedPtr<FJsonObject> FormatSettings =
					MakeShared<FJsonObject>();
				FormatSettings->SetBoolField(
					TEXT("srgb"),
					false);
				FormatSettings->SetStringField(
					TEXT("compression"),
					TEXT("masks"));
				FormatSettings->SetStringField(
					TEXT("mip_generation"),
					TEXT("simple_average"));
				AddChange(
					Input,
					TEXT("format_settings"),
					TEXT("replace"),
					TEXT(
						"uai.asset.texture."
						"format_settings.v1"),
					MakeShared<FJsonValueObject>(
						FormatSettings));
			}
			else if (ScenarioId
				== TEXT("update_texture_settings"))
			{
				const TSharedPtr<FJsonObject> Size =
					MakeShared<FJsonObject>();
				Size->SetNumberField(TEXT("x"), 64);
				Size->SetNumberField(TEXT("y"), 32);
				SetChangeValue(
					Input,
					0,
					MakeShared<FJsonValueObject>(Size));
				AddChange(
					Input,
					TEXT("format"),
					TEXT("set"),
					TEXT("uai.asset.render_target.format.v1"),
					MakeShared<FJsonValueString>(
						TEXT("rgba16f")));
				const TSharedPtr<FJsonObject> ClearColor =
					MakeShared<FJsonObject>();
				ClearColor->SetNumberField(TEXT("r"), 0.1);
				ClearColor->SetNumberField(TEXT("g"), 0.2);
				ClearColor->SetNumberField(TEXT("b"), 0.3);
				ClearColor->SetNumberField(TEXT("a"), 0.4);
				AddChange(
					Input,
					TEXT("clear_color"),
					TEXT("set"),
					TEXT("uai.value.linear_color.v1"),
					MakeShared<FJsonValueObject>(ClearColor));
				AddChange(
					Input,
					TEXT("auto_generate_mips"),
					TEXT("set"),
					TEXT("uai.value.boolean.v1"),
					MakeShared<FJsonValueBoolean>(true));
			}
			else if (ScenarioId
				== TEXT("update_static_mesh_collections"))
			{
				AddFixtureExampleChange(
					Fixture,
					Row,
					Input,
					TEXT("collision"));
				AddFixtureExampleChange(
					Fixture,
					Row,
					Input,
					TEXT("sockets"));
				AddFixtureExampleChange(
					Fixture,
					Row,
					Input,
					TEXT("build_settings"));
			}
			else if (ScenarioId
				== TEXT("replace_data_table_rows"))
			{
				AddFixtureExampleChange(
					Fixture,
					Row,
					Input,
					TEXT("rows"));
			}
			else if (ScenarioId
				== TEXT("replace_skeletal_mesh_content"))
			{
				AddFixtureExampleChange(
					Fixture,
					Row,
					Input,
					TEXT("lod_settings"));
				AddFixtureExampleChange(
					Fixture,
					Row,
					Input,
					TEXT("bounds"));
				AddFixtureExampleChange(
					Fixture,
					Row,
					Input,
					TEXT("skeleton_ref"));
			}
			else if (ScenarioId
				== TEXT("update_media_player_fields"))
			{
				AddChange(
					Input,
					TEXT("loop"),
					TEXT("set"),
					TEXT("uai.value.boolean.v1"),
					MakeShared<FJsonValueBoolean>(true));
			}
			else if (ScenarioId
				== TEXT("update_media_texture_fields"))
			{
				SetChangeValue(
					Input,
					0,
					MakeShared<FJsonValueString>(
						TEXT("uai://asset/fixture-source")));
				AddChange(
					Input,
					TEXT("auto_clear"),
					TEXT("set"),
					TEXT("uai.value.boolean.v1"),
					MakeShared<FJsonValueBoolean>(true));
				const TSharedPtr<FJsonObject> ClearColor =
					MakeShared<FJsonObject>();
				ClearColor->SetNumberField(TEXT("r"), 0.1);
				ClearColor->SetNumberField(TEXT("g"), 0.2);
				ClearColor->SetNumberField(TEXT("b"), 0.3);
				ClearColor->SetNumberField(TEXT("a"), 0.4);
				AddChange(
					Input,
					TEXT("clear_color"),
					TEXT("set"),
					TEXT("uai.value.linear_color.v1"),
					MakeShared<FJsonValueObject>(ClearColor));
			}
			else if (ScenarioId
				== TEXT("update_input_action_fields"))
			{
				SetChangeValue(
					Input,
					0,
					MakeShared<FJsonValueString>(
						TEXT("axis2d")));
				AddChange(
					Input,
					TEXT("consume_input"),
					TEXT("set"),
					TEXT("uai.value.boolean.v1"),
					MakeShared<FJsonValueBoolean>(false));
				AddChange(
					Input,
					TEXT("triggers"),
					TEXT("replace"),
					TEXT(
						"uai.asset.input_action."
						"triggers.v1"),
					MakeClassPathArray({
						TEXT(
							"/Script/EnhancedInput."
							"InputTriggerHold"),
						TEXT(
							"/Script/EnhancedInput."
							"InputTriggerReleased"),
					}));
				AddChange(
					Input,
					TEXT("modifiers"),
					TEXT("replace"),
					TEXT(
						"uai.asset.input_action."
						"modifiers.v1"),
					MakeClassPathArray({
						TEXT(
							"/Script/EnhancedInput."
							"InputModifierScalar"),
						TEXT(
							"/Script/EnhancedInput."
							"InputModifierSwizzleAxis"),
					}));
			}
			else if (ScenarioId
				== TEXT(
					"update_input_action_collections_clear"))
			{
				Input->SetArrayField(
					TEXT("changes"),
					TArray<TSharedPtr<FJsonValue>>{});
				AddPresenceOnlyChange(
					Input,
					TEXT("triggers"),
					TEXT("clear"));
				AddPresenceOnlyChange(
					Input,
					TEXT("modifiers"),
					TEXT("clear"));
			}
			else if (ScenarioId
				== TEXT(
					"update_input_action_collections_set"))
			{
				Input->SetArrayField(
					TEXT("changes"),
					TArray<TSharedPtr<FJsonValue>>{});
				AddChange(
					Input,
					TEXT("triggers"),
					TEXT("set"),
					TEXT(
						"uai.asset.input_action."
						"triggers.v1"),
					MakeClassPathArray({
						TEXT(
							"/Script/EnhancedInput."
							"InputTriggerReleased"),
					}));
				AddChange(
					Input,
					TEXT("modifiers"),
					TEXT("set"),
					TEXT(
						"uai.asset.input_action."
						"modifiers.v1"),
					MakeClassPathArray({
						TEXT(
							"/Script/EnhancedInput."
							"InputModifierScalar"),
					}));
			}
			else if (ScenarioId
				== TEXT("update_input_mapping_context_set"))
			{
				SetChangeValue(
					Input,
					0,
					MakeShared<FJsonValueArray>(
						TArray<TSharedPtr<FJsonValue>>{
							MakeInputMapping(
								TEXT(
									"uai://asset/"
									"fixture-source"),
								TEXT("SpaceBar"),
								{
									TEXT(
										"/Script/EnhancedInput."
										"InputTriggerReleased"),
								},
								{
									TEXT(
										"/Script/EnhancedInput."
										"InputModifierScalar"),
								}),
							MakeInputMapping(
								TEXT(
									"uai://asset/"
									"fixture-input-action-c"),
								TEXT("Q"),
								{
									TEXT(
										"/Script/EnhancedInput."
										"InputTriggerHold"),
								},
								{
									TEXT(
										"/Script/EnhancedInput."
										"InputModifierSwizzleAxis"),
								}),
						}));
			}
			else if (ScenarioId
				== TEXT("update_input_mapping_context_clear"))
			{
				SetChangePresenceOnly(
					Input,
					0,
					TEXT("clear"));
			}
			else if (ScenarioId
				== TEXT("update_input_mapping_context_remove"))
			{
				Input->GetArrayField(TEXT("changes"))[0]
					->AsObject()
					->SetStringField(
						TEXT("presence"),
						TEXT("remove"));
				SetChangeValue(
					Input,
					0,
					MakeShared<FJsonValueArray>(
						TArray<TSharedPtr<FJsonValue>>{
							MakeInputMappingIdentity(
								TEXT(
									"uai://asset/"
									"fixture-source"),
								TEXT("SpaceBar")),
						}));
			}
			else if (ScenarioId
				== TEXT("update_input_mapping_context_replace"))
			{
				Input->GetArrayField(TEXT("changes"))[0]
					->AsObject()
					->SetStringField(
						TEXT("presence"),
						TEXT("replace"));
				SetChangeValue(
					Input,
					0,
					MakeShared<FJsonValueArray>(
						TArray<TSharedPtr<FJsonValue>>{
							MakeInputMapping(
								TEXT(
									"uai://asset/"
									"fixture-input-action-c"),
								TEXT("Q"),
								{
									TEXT(
										"/Script/EnhancedInput."
										"InputTriggerHold"),
								},
								{
									TEXT(
										"/Script/EnhancedInput."
										"InputModifierSwizzleAxis"),
								}),
							MakeInputMapping(
								TEXT(
									"uai://asset/"
									"fixture-source"),
								TEXT("LeftShift"),
								{
									TEXT(
										"/Script/EnhancedInput."
										"InputTriggerReleased"),
								},
								{
									TEXT(
										"/Script/EnhancedInput."
										"InputModifierScalar"),
								}),
						}));
			}
			else if (ScenarioId
				== TEXT("update_subuv_fields_set"))
			{
				SetChangeValue(
					Input,
					0,
					MakeShared<FJsonValueString>(
						TEXT("uai://asset/fixture-source")));
				const TSharedPtr<FJsonObject> Grid =
					MakeShared<FJsonObject>();
				Grid->SetNumberField(TEXT("x"), 4);
				Grid->SetNumberField(TEXT("y"), 6);
				AddChange(
					Input,
					TEXT("grid"),
					TEXT("set"),
					TEXT("uai.value.extent2i.v1"),
					MakeShared<FJsonValueObject>(Grid));
				AddChange(
					Input,
					TEXT("bounding_geometry"),
					TEXT("set"),
					TEXT(
						"uai.asset.subuv_animation."
						"bounding_geometry.v1"),
					MakeShared<FJsonValueString>(
						TEXT("eight_vertices")));
			}
			else if (ScenarioId
				== TEXT("update_subuv_fields_clear"))
			{
				SetChangePresenceOnly(
					Input,
					0,
					TEXT("clear"));
				AddPresenceOnlyChange(
					Input,
					TEXT("grid"),
					TEXT("clear"));
				AddPresenceOnlyChange(
					Input,
					TEXT("bounding_geometry"),
					TEXT("clear"));
			}
			else if (ScenarioId
				== TEXT("update_subuv_source_null"))
			{
				SetChangePresenceOnly(
					Input,
					0,
					TEXT("null"));
			}
			else if (ScenarioId
				== TEXT("update_texture_collection_set"))
			{
				SetChangeValue(
					Input,
					0,
					MakeShared<FJsonValueArray>(
						TArray<TSharedPtr<FJsonValue>>{
							MakeShared<FJsonValueString>(
								TEXT(
									"uai://asset/"
									"fixture-source-c")),
							MakeShared<FJsonValueString>(
								TEXT(
									"uai://asset/"
									"fixture-source")),
						}));
			}
			else if (ScenarioId
				== TEXT("update_texture_collection_clear"))
			{
				SetChangePresenceOnly(
					Input,
					0,
					TEXT("clear"));
			}
			else if (ScenarioId
				== TEXT("update_texture_collection_remove"))
			{
				Input->GetArrayField(TEXT("changes"))[0]
					->AsObject()
					->SetStringField(
						TEXT("presence"),
						TEXT("remove"));
				SetChangeValue(
					Input,
					0,
					MakeShared<FJsonValueArray>(
						TArray<TSharedPtr<FJsonValue>>{
							MakeShared<FJsonValueString>(
								TEXT(
									"uai://asset/"
									"fixture-source")),
						}));
			}
			else if (ScenarioId
				== TEXT("update_texture_collection_replace"))
			{
				Input->GetArrayField(TEXT("changes"))[0]
					->AsObject()
					->SetStringField(
						TEXT("presence"),
						TEXT("replace"));
				SetChangeValue(
					Input,
					0,
					MakeShared<FJsonValueArray>(
						TArray<TSharedPtr<FJsonValue>>{
							MakeShared<FJsonValueString>(
								TEXT(
									"uai://asset/"
									"fixture-source-c")),
							MakeShared<FJsonValueString>(
								TEXT(
									"uai://asset/"
									"fixture-source")),
						}));
			}
			else if (ScenarioId
				== TEXT("update_paper_sprite_fields_set"))
			{
				SetChangeValue(
					Input,
					0,
					MakeShared<FJsonValueString>(
						TEXT("uai://asset/fixture-source")));
				const TSharedPtr<FJsonObject> Region =
					MakeShared<FJsonObject>();
				Region->SetNumberField(TEXT("x"), 4);
				Region->SetNumberField(TEXT("y"), 5);
				Region->SetNumberField(TEXT("width"), 20);
				Region->SetNumberField(TEXT("height"), 16);
				AddChange(
					Input,
					TEXT("source_region"),
					TEXT("set"),
					TEXT("uai.value.rect2d.v1"),
					MakeShared<FJsonValueObject>(Region));
				const TSharedPtr<FJsonObject> Pivot =
					MakeShared<FJsonObject>();
				Pivot->SetNumberField(TEXT("x"), 7);
				Pivot->SetNumberField(TEXT("y"), 8);
				AddChange(
					Input,
					TEXT("pivot"),
					TEXT("set"),
					TEXT("uai.value.vector2d.v1"),
					MakeShared<FJsonValueObject>(Pivot));
			}
			else if (ScenarioId
				== TEXT("update_paper_sprite_fields_clear"))
			{
				SetChangePresenceOnly(
					Input,
					0,
					TEXT("clear"));
				AddPresenceOnlyChange(
					Input,
					TEXT("source_region"),
					TEXT("clear"));
				AddPresenceOnlyChange(
					Input,
					TEXT("pivot"),
					TEXT("clear"));
			}
			else if (ScenarioId
				== TEXT("update_paper_sprite_source_null"))
			{
				SetChangePresenceOnly(
					Input,
					0,
					TEXT("null"));
			}
			else if (ScenarioId
				== TEXT("replace_paper_flipbook_content"))
			{
				SetChangeValue(
					Input,
					0,
					MakeShared<FJsonValueArray>(
						TArray<TSharedPtr<FJsonValue>>{
							MakePaperFlipbookFrame(
								MakeShared<FJsonValueString>(
									TEXT(
										"uai://asset/"
										"fixture-source")),
								4),
							MakePaperFlipbookFrame(
								MakeShared<FJsonValueNull>(),
								1),
							MakePaperFlipbookFrame(
								MakeShared<FJsonValueString>(
									TEXT(
										"uai://asset/"
										"fixture-paper-flipbook-a")),
								2),
						}));
				AddChange(
					Input,
					TEXT("frames_per_second"),
					TEXT("replace"),
					TEXT("uai.value.decimal.v1"),
					MakeShared<FJsonValueNumber>(24.0));
			}
			else if (ScenarioId
				== TEXT("replace_paper_flipbook_frames_empty"))
			{
				SetChangeValue(
					Input,
					0,
					MakeShared<FJsonValueArray>(
						TArray<TSharedPtr<FJsonValue>>{}));
			}
			else if (ScenarioId
				== TEXT("replace_paper_flipbook_fps_only"))
			{
				Input->SetArrayField(
					TEXT("changes"),
					TArray<TSharedPtr<FJsonValue>>{});
				AddChange(
					Input,
					TEXT("frames_per_second"),
					TEXT("replace"),
					TEXT("uai.value.decimal.v1"),
					MakeShared<FJsonValueNumber>(30.0));
			}
			else if (ScenarioId
				== TEXT("replace_paper_tilemap_content"))
			{
				SetChangeValue(
					Input,
					0,
					MakeExtent2i(3, 2));
				AddChange(
					Input,
					TEXT("tile_size"),
					TEXT("replace"),
					TEXT("uai.value.extent2i.v1"),
					MakeExtent2i(16, 24));
				TArray<TSharedPtr<FJsonValue>> GroundCells{
					MakePaperTileMapCell(
						0,
						0,
						TEXT("uai://asset/fixture-source"),
						2,
						true,
						false,
						false),
					MakePaperTileMapCell(
						2,
						1,
						TEXT(
							"uai://asset/"
							"fixture-paper-tilemap-a"),
						1,
						false,
						false,
						true),
				};
				TArray<TSharedPtr<FJsonValue>> Layers{
					MakePaperTileMapLayer(
						TEXT("Ground"),
						true,
						true,
						true,
						MakeShared<FJsonValueNull>(),
						MakeShared<FJsonValueNull>(),
						MakeLinearColor(
							1.0,
							0.5,
							0.25,
							1.0),
						MoveTemp(GroundCells)),
					MakePaperTileMapLayer(
						TEXT("Overlay"),
						false,
						false,
						false,
						MakeShared<FJsonValueNumber>(25.0),
						MakeShared<FJsonValueNumber>(-3.0),
						MakeLinearColor(
							0.1,
							0.2,
							0.3,
							0.4),
						{}),
				};
				AddChange(
					Input,
					TEXT("layers"),
					TEXT("replace"),
					TEXT(
						"uai.asset.paper_tilemap."
						"layers.v1"),
					MakeShared<FJsonValueArray>(
						MoveTemp(Layers)));
			}
			else if (ScenarioId
				== TEXT("replace_paper_tilemap_layers_empty"))
			{
				Input->SetArrayField(
					TEXT("changes"),
					TArray<TSharedPtr<FJsonValue>>{});
				AddChange(
					Input,
					TEXT("layers"),
					TEXT("replace"),
					TEXT(
						"uai.asset.paper_tilemap."
						"layers.v1"),
					MakeShared<FJsonValueArray>(
						TArray<TSharedPtr<FJsonValue>>{}));
			}
			else if (ScenarioId
				== TEXT("replace_paper_tilemap_tile_size_only"))
			{
				Input->SetArrayField(
					TEXT("changes"),
					TArray<TSharedPtr<FJsonValue>>{});
				AddChange(
					Input,
					TEXT("tile_size"),
					TEXT("replace"),
					TEXT("uai.value.extent2i.v1"),
					MakeExtent2i(12, 18));
			}
			else if (ScenarioId
				== TEXT("replace_paper_tileset_content"))
			{
				SetChangeValue(
					Input,
					0,
					MakeShared<FJsonValueString>(
						TEXT("uai://asset/fixture-source")));
				AddChange(
					Input,
					TEXT("tile_size"),
					TEXT("replace"),
					TEXT("uai.value.extent2i.v1"),
					MakeExtent2i(16, 8));
				AddChange(
					Input,
					TEXT("margin"),
					TEXT("replace"),
					TEXT("uai.value.integer.v1"),
					MakeShared<FJsonValueNumber>(1));
				AddChange(
					Input,
					TEXT("spacing"),
					TEXT("replace"),
					TEXT("uai.value.integer.v1"),
					MakeShared<FJsonValueNumber>(2));
				AddChange(
					Input,
					TEXT("terrain"),
					TEXT("replace"),
					TEXT(
						"uai.asset.paper_tileset."
						"terrain.v1"),
					MakeShared<FJsonValueArray>(
						TArray<TSharedPtr<FJsonValue>>{
							MakePaperTileSetTerrain(
								TEXT("Grass"),
								0),
							MakePaperTileSetTerrain(
								TEXT("Stone"),
								1),
						}));
			}
			else if (ScenarioId
				== TEXT("replace_paper_tileset_terrain_empty"))
			{
				Input->SetArrayField(
					TEXT("changes"),
					TArray<TSharedPtr<FJsonValue>>{});
				AddChange(
					Input,
					TEXT("terrain"),
					TEXT("replace"),
					TEXT(
						"uai.asset.paper_tileset."
						"terrain.v1"),
					MakeShared<FJsonValueArray>(
						TArray<TSharedPtr<FJsonValue>>{}));
			}
			else if (ScenarioId
				== TEXT("replace_paper_tileset_layout_only"))
			{
				Input->SetArrayField(
					TEXT("changes"),
					TArray<TSharedPtr<FJsonValue>>{});
				AddChange(
					Input,
					TEXT("tile_size"),
					TEXT("replace"),
					TEXT("uai.value.extent2i.v1"),
					MakeExtent2i(4, 4));
				AddChange(
					Input,
					TEXT("margin"),
					TEXT("replace"),
					TEXT("uai.value.integer.v1"),
					MakeShared<FJsonValueNumber>(2));
				AddChange(
					Input,
					TEXT("spacing"),
					TEXT("replace"),
					TEXT("uai.value.integer.v1"),
					MakeShared<FJsonValueNumber>(1));
			}

			FPlanningPreflightPlanPtr Plan;
			FExecutionMutationError Error;
			if (!Test->TestTrue(
					*(ScenarioId + TEXT(" builds frozen plan")),
					BuildPlan(
						Test,
						*Descriptor,
						Input,
						Snapshot.ToSharedRef(),
						*FactoryCandidates,
						Resources,
						Plan,
						Error))
				|| !Plan.IsValid())
			{
				continue;
			}

			TSharedPtr<IUeAgentAssetDataSimpleEffectNativeHook> Hook;
			if (!Test->TestTrue(
					*(ScenarioId + TEXT(" creates native hook")),
						FUeAgentAssetDataSimpleEffectNativeHookFactory::
						TryCreate(
							Resolver,
							FactoryCandidates,
							Hook,
							Error))
				|| !Hook.IsValid())
			{
				continue;
			}

			const FUeAgentExecutionContextRefOwner Context =
				MakeExecutionContext(Descriptor->GetIdentity());
			FNativeRequestOwner RequestOwner(
				Resources,
				Plan.ToSharedRef(),
				Context);
			FExecutionMutationRequest Request =
				RequestOwner.MakeRequest();
			TArray<FUeAgentGenericEffectBinding> Bindings;
			for (const FPlannedChange& Change
				: Plan->GetChangeSet().GetChanges())
			{
				Bindings.Add(
					MakeBinding(
						*Descriptor,
						Change.GetIdentity()));
			}
			FUeAgentGenericEffectApplyResult Applied;
			const bool bApplied = Hook->TryApply(
				Request,
				FLockFenceLease(
					TEXT("owner:asset-data-e2"),
					{Owner.Package->GetName()},
					1),
				Bindings,
				*Resources,
				Applied,
				Error);
			Test->TestTrue(
				*(ScenarioId + TEXT(" applies")),
				bApplied);
			Test->TestTrue(
				*(ScenarioId + TEXT(" reports mutation")),
				Applied.DidMutationStart());
			if (!bApplied)
			{
				Test->AddError(
					ScenarioId + TEXT(":")
						+ Error.GetCode());
				continue;
			}

			UObject* AppliedTarget = Target;
			if (!AppliedTarget)
			{
				AppliedTarget = FindObject<UObject>(
					Owner.Package,
					TEXT("CreatedTarget"));
			}
			Test->TestNotNull(
				*(ScenarioId
					+ TEXT(" has independent target readback")),
				AppliedTarget);
			if (Scenario->HasTypedField<EJson::String>(
					TEXT("asset_class_path")))
			{
				const FString ExpectedClassPath =
					Scenario->GetStringField(
						TEXT("asset_class_path"));
				UClass* ExpectedClass =
					FindObject<UClass>(
						nullptr,
						*ExpectedClassPath);
				if (!ExpectedClass)
				{
					ExpectedClass =
						LoadObject<UClass>(
							nullptr,
							*ExpectedClassPath);
				}
				Test->TestNotNull(
					*(ScenarioId
						+ TEXT(" expected class resolves")),
					ExpectedClass);
				Test->TestTrue(
					*(ScenarioId
						+ TEXT(" creates exact class")),
					AppliedTarget
						&& ExpectedClass
						&& AppliedTarget->IsA(ExpectedClass));
				Test->TestTrue(
					*(ScenarioId
						+ TEXT(" creates persistent object flags")),
					AppliedTarget
						&& AppliedTarget->HasAllFlags(
							RF_Public
								| RF_Standalone
								| RF_Transactional));
			}
			if (ScenarioId
				== TEXT("update_audio_effect_preset_settings"))
			{
				const USubmixEffectSubmixEQPreset* Preset =
					Cast<USubmixEffectSubmixEQPreset>(
						AppliedTarget);
				const bool bExact =
					Preset
					&& Preset->Settings.EQBands.Num() == 1
					&& FMath::IsNearlyEqual(
						Preset->Settings.EQBands[0].Frequency,
						800.0f)
					&& FMath::IsNearlyEqual(
						Preset->Settings.EQBands[0].Bandwidth,
						1.0f)
					&& FMath::IsNearlyEqual(
						Preset->Settings.EQBands[0].GainDb,
						3.0f)
					&& Preset->Settings.EQBands[0].bEnabled;
				Test->TestTrue(
					TEXT("audio preset exact EQ settings applied"),
					bExact);
			}
			else if (ScenarioId
				== TEXT("update_sound_attenuation_settings"))
			{
				const USoundAttenuation* Attenuation =
					Cast<USoundAttenuation>(AppliedTarget);
				const FSoundAttenuationSettings* Settings =
					Attenuation
						? &Attenuation->Attenuation
						: nullptr;
				const bool bExact =
					Settings
					&& Settings->bAttenuate
					&& Settings->bSpatialize
					&& Settings->DistanceAlgorithm
						== EAttenuationDistanceModel::Linear
					&& Settings->AttenuationShape
						== EAttenuationShape::Sphere
					&& FMath::IsNearlyEqual(
						Settings->AttenuationShapeExtents.X,
						200.0)
					&& FMath::IsNearlyEqual(
						Settings->FalloffDistance,
						1200.0f)
					&& Settings->SpatializationAlgorithm
						== SPATIALIZATION_Default
					&& Settings->bEnableOcclusion
					&& FMath::IsNearlyEqual(
						Settings
							->OcclusionLowPassFilterFrequency,
						1200.0f)
					&& FMath::IsNearlyEqual(
						Settings
							->OcclusionVolumeAttenuation,
						0.4f)
					&& FMath::IsNearlyEqual(
						Settings->OcclusionInterpolationTime,
						0.2f);
				Test->TestTrue(
					TEXT(
						"sound attenuation exact curated "
						"settings applied"),
					bExact);
			}
			else if (ScenarioId
				== TEXT(
					"update_sound_class_properties_and_children"))
			{
				const USoundClass* SoundClass =
					Cast<USoundClass>(AppliedTarget);
				const USoundClass* Child =
					Cast<USoundClass>(Source);
				const bool bExact =
					SoundClass
					&& Child
					&& FMath::IsNearlyEqual(
						SoundClass->Properties.Volume,
						0.75f)
					&& FMath::IsNearlyEqual(
						SoundClass->Properties.Pitch,
						1.1f)
					&& SoundClass->Properties.bIsUISound
					&& !SoundClass->Properties.bIsMusic
					&& SoundClass->ChildClasses.Num() == 1
					&& SoundClass->ChildClasses[0] == Child
					&& Child->ParentClass == SoundClass;
				Test->TestTrue(
					TEXT(
						"sound class exact properties and "
						"inverse relation applied"),
					bExact);
			}
			else if (ScenarioId
				== TEXT("update_sound_concurrency_settings"))
			{
				const USoundConcurrency* Concurrency =
					Cast<USoundConcurrency>(AppliedTarget);
				const FSoundConcurrencySettings* Settings =
					Concurrency
						? &Concurrency->Concurrency
						: nullptr;
				const bool bExact =
					Settings
					&& Settings->MaxCount == 4
					&& Settings->bLimitToOwner
					&& FMath::IsNearlyEqual(
						Settings->RetriggerTime,
						0.25f)
					&& Settings->ResolutionRule
						== EMaxConcurrentResolutionRule::
							StopOldest
					&& FMath::IsNearlyEqual(
						Settings->GetVolumeScale(),
						0.8f)
					&& Settings->VolumeScaleMode
						== EConcurrencyVolumeScaleMode::Default
					&& FMath::IsNearlyEqual(
						Settings->VolumeScaleAttackTime,
						0.02f)
					&& FMath::IsNearlyEqual(
						Settings->VolumeScaleReleaseTime,
						0.4f)
					&& FMath::IsNearlyEqual(
						Settings->VoiceStealReleaseTime,
						0.1f);
				Test->TestTrue(
					TEXT(
						"sound concurrency exact settings applied"),
					bExact);
			}
			else if (ScenarioId
				== TEXT("update_sound_mix_modifiers"))
			{
				const USoundMix* SoundMix =
					Cast<USoundMix>(AppliedTarget);
				const USoundClass* SoundClass =
					Cast<USoundClass>(Source);
				const FSoundClassAdjuster* Modifier =
					SoundMix
						&& SoundMix->SoundClassEffects.Num() == 1
							? &SoundMix->SoundClassEffects[0]
							: nullptr;
				const bool bExact =
					SoundMix
					&& SoundClass
					&& Modifier
					&& Modifier->SoundClassObject == SoundClass
					&& FMath::IsNearlyEqual(
						Modifier->VolumeAdjuster,
						0.8f)
					&& FMath::IsNearlyEqual(
						Modifier->PitchAdjuster,
						1.1f)
					&& FMath::IsNearlyEqual(
						Modifier->LowPassFilterFrequency,
						16000.0f)
					&& Modifier->bApplyToChildren
					&& FMath::IsNearlyEqual(
						Modifier
							->VoiceCenterChannelVolumeAdjuster,
						0.9f)
					&& FMath::IsNearlyZero(
						SoundMix->InitialDelay);
				Test->TestTrue(
					TEXT(
						"sound mix exact modifier and delay applied"),
					bExact);
			}
			else if (ScenarioId
				== TEXT(
					"update_sound_submix_parent_and_effect_chain"))
			{
				const USoundSubmix* Submix =
					Cast<USoundSubmix>(AppliedTarget);
				const USoundSubmix* Parent =
					Cast<USoundSubmix>(Source);
				const USoundEffectSubmixPreset* Effect =
					AdditionalSources.IsEmpty()
						? nullptr
						: Cast<USoundEffectSubmixPreset>(
							AdditionalSources[0].Value);
				const bool bExact =
					Submix
					&& Parent
					&& Effect
					&& Submix->ParentSubmix == Parent
					&& Parent->ChildSubmixes.Contains(Submix)
					&& Submix->SubmixEffectChain.Num() == 1
					&& Submix->SubmixEffectChain[0] == Effect;
				Test->TestTrue(
					TEXT(
						"sound submix exact parent inverse and "
						"effect chain applied"),
					bExact);
			}
			else if (ScenarioId == TEXT("replace_curve_keys"))
			{
				const UCurveFloat* Curve =
					Cast<UCurveFloat>(AppliedTarget);
				const TArray<FRichCurveKey>* Keys =
					Curve
						? &Curve->FloatCurve.GetConstRefOfKeys()
						: nullptr;
				const bool bExact =
					Keys
					&& Keys->Num() == 2
					&& FMath::IsNearlyZero((*Keys)[0].Time)
					&& FMath::IsNearlyEqual(
						(*Keys)[0].Value,
						0.25f)
					&& (*Keys)[0].InterpMode == RCIM_Cubic
					&& (*Keys)[0].TangentMode == RCTM_User
					&& (*Keys)[0].TangentWeightMode
						== RCTWM_WeightedNone
					&& FMath::IsNearlyEqual(
						(*Keys)[0].ArriveTangent,
						0.1f)
					&& FMath::IsNearlyEqual(
						(*Keys)[0].LeaveTangent,
						0.2f)
					&& FMath::IsNearlyEqual(
						(*Keys)[1].Time,
						1.0f)
					&& FMath::IsNearlyEqual(
						(*Keys)[1].Value,
						1.0f)
					&& (*Keys)[1].InterpMode == RCIM_Linear
					&& (*Keys)[1].TangentMode == RCTM_Auto;
				Test->TestTrue(
					TEXT("curve exact rich keys applied"),
					bExact);
			}
			else if (ScenarioId
				== TEXT("update_data_object_registered_fields"))
			{
				const UUeAgentAssetDataSimpleEffectDataAsset* Data =
					Cast<UUeAgentAssetDataSimpleEffectDataAsset>(
						AppliedTarget);
				const bool bExact =
					Data
					&& Data->Label == TEXT("Updated")
					&& Data->Count == 7
					&& FMath::IsNearlyEqual(Data->Weight, 1.5f)
					&& Data->bEnabled
					&& Data->RelatedAsset == Source;
				Test->TestTrue(
					TEXT(
						"data object exact registered fields applied"),
					bExact);
			}
			else if (ScenarioId == TEXT("update_file_media_source"))
			{
				const UFileMediaSource* MediaSource =
					Cast<UFileMediaSource>(AppliedTarget);
				const FString ExpectedPath =
					FPaths::ConvertRelativePathToFull(
						TEXT("/fixture"));
				const bool bExact =
					MediaSource
					&& MediaSource->GetFilePath()
						== ExpectedPath
					&& MediaSource->PrecacheFile;
				Test->TestTrue(
					TEXT(
						"file media exact URI and option applied"),
					bExact);
			}
			else if (ScenarioId
				== TEXT("update_runtime_virtual_texture"))
			{
				const URuntimeVirtualTexture* VirtualTexture =
					Cast<URuntimeVirtualTexture>(AppliedTarget);
				const bool bExact =
					VirtualTexture
					&& VirtualTexture->GetSize() == 256
					&& VirtualTexture->GetMaterialType()
						== ERuntimeVirtualTextureMaterialType::
							BaseColor
					&& VirtualTexture->GetCompressTextures();
				Test->TestTrue(
					TEXT(
						"runtime virtual texture exact settings applied"),
					bExact);
			}
			else if (ScenarioId
				== TEXT("update_sparse_volume_texture_settings"))
			{
				const UStreamableSparseVolumeTexture* Sparse =
					Cast<UStreamableSparseVolumeTexture>(
						AppliedTarget);
				const bool bExact =
					Sparse
					&& Sparse->AddressX == TA_Clamp
					&& Sparse->AddressY == TA_Mirror
					&& Sparse->AddressZ == TA_Wrap
					&& !Sparse->bLocalDDCOnly
					&& FMath::IsNearlyEqual(
						Sparse->StreamingPoolSizeFactor,
						4.0f)
					&& Sparse->NumberOfPrefetchFrames == 5
					&& FMath::IsNearlyEqual(
						Sparse->PrefetchPercentageStepSize,
						15.0f)
					&& FMath::IsNearlyEqual(
						Sparse->PrefetchPercentageBias,
						25.0f);
				Test->TestTrue(
					TEXT(
						"sparse volume texture exact editable "
						"settings applied"),
					bExact);
			}
			else if (ScenarioId
				== TEXT("update_texture_settings"))
			{
				const UTextureRenderTarget2D* RenderTarget =
					Cast<UTextureRenderTarget2D>(AppliedTarget);
				Test->TestTrue(
					TEXT("render target declared fields applied"),
					RenderTarget
						&& RenderTarget->SizeX == 64
						&& RenderTarget->SizeY == 32
						&& RenderTarget->RenderTargetFormat
							== RTF_RGBA16f
						&& RenderTarget->ClearColor.Equals(
							FLinearColor(
								0.1f,
								0.2f,
								0.3f,
								0.4f))
						&& RenderTarget->bAutoGenerateMips);
			}
			else if (ScenarioId
				== TEXT("update_static_mesh_collections"))
			{
				const UStaticMesh* StaticMesh =
					Cast<UStaticMesh>(AppliedTarget);
				const UBodySetup* BodySetup =
					StaticMesh
						? StaticMesh->GetBodySetup()
						: nullptr;
				const TObjectPtr<UStaticMeshSocket>* SocketSlot =
					StaticMesh
						? StaticMesh->Sockets.FindByPredicate(
							[](const UStaticMeshSocket* Candidate)
							{
								return Candidate
									&& Candidate->SocketName
										== TEXT("SocketA");
							})
						: nullptr;
				const UStaticMeshSocket* Socket =
					SocketSlot ? SocketSlot->Get() : nullptr;
				const FMeshBuildSettings* BuildSettings =
					StaticMesh
						&& StaticMesh->GetNumSourceModels() == 1
							? &StaticMesh->GetSourceModel(0)
								.BuildSettings
							: nullptr;
				Test->TestTrue(
					TEXT("static mesh material slots applied"),
					StaticMesh
						&& StaticMesh->GetStaticMaterials().Num() == 2
						&& StaticMesh->GetStaticMaterials()[1]
							.MaterialSlotName
							== TEXT("Main")
						&& StaticMesh->GetStaticMaterials()[1]
							.MaterialInterface
							== FixtureMaterial);
				Test->TestTrue(
					TEXT("static mesh collision applied"),
					BodySetup
						&& BodySetup->CollisionTraceFlag
							== CTF_UseSimpleAndComplex
						&& BodySetup->AggGeom.BoxElems.Num() == 1
						&& BodySetup->AggGeom.SphereElems.Num() == 1
						&& BodySetup->AggGeom.SphylElems.Num() == 1
						&& BodySetup->AggGeom.BoxElems[0]
							.Center.Equals(FVector(1.0, 2.0, 3.0))
						&& FMath::IsNearlyEqual(
							BodySetup->AggGeom.SphereElems[0].Radius,
							12.0f));
				Test->TestTrue(
					TEXT("static mesh socket identity applied"),
					Socket
						&& StaticMesh->Sockets.Num() == 2
						&& Socket->SocketName == TEXT("SocketA"));
				Test->TestTrue(
					TEXT("static mesh socket location applied"),
					Socket
						&& Socket->RelativeLocation.Equals(
							FVector(1.0, 2.0, 3.0)));
				Test->TestTrue(
					TEXT("static mesh socket rotation applied"),
					Socket
						&& Socket->RelativeRotation.Equals(
							FRotator(4.0, 5.0, 6.0)));
				Test->TestTrue(
					TEXT("static mesh socket scale applied"),
					Socket
						&& Socket->RelativeScale.Equals(
							FVector(1.0, 2.0, 1.0)));
				Test->TestTrue(
					TEXT("static mesh build settings applied"),
					BuildSettings
						&& BuildSettings->bRecomputeNormals
						&& BuildSettings->bRecomputeTangents
						&& BuildSettings->bComputeWeightedNormals
						&& !BuildSettings->bRemoveDegenerates
						&& BuildSettings->bUseMikkTSpace
						&& !BuildSettings->bGenerateLightmapUVs);
			}
			else if (ScenarioId
				== TEXT("replace_data_table_rows"))
			{
				const UDataTable* DataTable =
					Cast<UDataTable>(AppliedTarget);
				const uint8* const* RowPtr =
					DataTable
						? DataTable->GetRowMap().Find(
							TEXT("RowA"))
						: nullptr;
				const auto* TypedRow =
					RowPtr
						? reinterpret_cast<
							const FUeAgentAssetDataSimpleEffectTableRow*>(
								*RowPtr)
						: nullptr;
				Test->TestTrue(
					TEXT(
						"data table exact row content replaced"),
					DataTable
						&& DataTable->GetRowStruct()
							== FUeAgentAssetDataSimpleEffectTableRow::
								StaticStruct()
						&& DataTable->GetRowMap().Num() == 1
						&& TypedRow
						&& TypedRow->Label == TEXT("Updated")
						&& TypedRow->Count == 7
						&& FMath::IsNearlyEqual(
							TypedRow->Weight,
							1.5f)
						&& TypedRow->bEnabled);
			}
			else if (ScenarioId
				== TEXT("replace_skeletal_mesh_content"))
			{
				const USkeletalMesh* SkeletalMesh =
					Cast<USkeletalMesh>(AppliedTarget);
				Test->TestTrue(
					TEXT(
						"skeletal mesh exact content replaced"),
					SkeletalMesh
						&& SkeletalMesh->GetMaterials().Num()
							== 1
						&& SkeletalMesh->GetMaterials()[0]
							.MaterialSlotName
							== TEXT("Body")
						&& SkeletalMesh->GetMaterials()[0]
							.MaterialInterface
							== FixtureMaterial
						&& SkeletalMesh->GetImportedBounds()
							.GetBox()
							.Equals(
								FBox(
									FVector(-10.0, -20.0, -30.0),
									FVector(10.0, 20.0, 30.0)))
						&& SkeletalMesh->GetSkeleton()
							== DesiredSkeleton
						&& SkeletalMesh->GetLODSettings()
							== DesiredLodSettings);
			}
			else if (ScenarioId
				== TEXT("update_media_player_fields"))
			{
				const UMediaPlayer* MediaPlayer =
					Cast<UMediaPlayer>(AppliedTarget);
				Test->TestTrue(
					TEXT("media player declared fields applied"),
					MediaPlayer
						&& MediaPlayer->PlayOnOpen
						&& ReadMediaPlayerLoopSetting(
							*MediaPlayer));
			}
			else if (ScenarioId
				== TEXT("update_media_texture_fields"))
			{
				const UMediaTexture* MediaTexture =
					Cast<UMediaTexture>(AppliedTarget);
				Test->TestTrue(
					TEXT("media texture declared fields applied"),
					MediaTexture
						&& MediaTexture->GetMediaPlayer()
							== Source
						&& MediaTexture->AutoClear
						&& MediaTexture->ClearColor.Equals(
							FLinearColor(
								0.1f,
								0.2f,
								0.3f,
								0.4f)));
			}
			else if (ScenarioId.StartsWith(
				TEXT("update_input_action_")))
			{
				const UInputAction* InputAction =
					Cast<UInputAction>(AppliedTarget);
				bool bExact = InputAction != nullptr;
				if (bExact && ScenarioId
					== TEXT("update_input_action_fields"))
				{
					bExact =
						InputAction->ValueType
							== EInputActionValueType::Axis2D
						&& !InputAction->bConsumeInput
						&& InputAction->Triggers.Num() == 2
						&& InputAction->Triggers[0]
							->IsA<UInputTriggerHold>()
						&& InputAction->Triggers[1]
							->IsA<UInputTriggerReleased>()
						&& InputAction->Modifiers.Num() == 2
						&& InputAction->Modifiers[0]
							->IsA<UInputModifierScalar>()
						&& InputAction->Modifiers[1]
							->IsA<
								UInputModifierSwizzleAxis>();
				}
				else if (bExact && ScenarioId
					== TEXT(
						"update_input_action_"
						"collections_clear"))
				{
					bExact =
						InputAction->ValueType
							== EInputActionValueType::Boolean
						&& InputAction->bConsumeInput
						&& InputAction->Triggers.IsEmpty()
						&& InputAction->Modifiers.IsEmpty();
				}
				else if (bExact && ScenarioId
					== TEXT(
						"update_input_action_"
						"collections_set"))
				{
					bExact =
						InputAction->ValueType
							== EInputActionValueType::Boolean
						&& InputAction->bConsumeInput
						&& InputAction->Triggers.Num() == 2
						&& InputAction->Triggers[0]
							== InitialInputActionTrigger
						&& InputAction->Triggers[1]
							->IsA<UInputTriggerReleased>()
						&& InputAction->Modifiers.Num() == 2
						&& InputAction->Modifiers[0]
							== InitialInputActionModifier
						&& InputAction->Modifiers[1]
							->IsA<UInputModifierScalar>();
				}
				Test->TestTrue(
					*(ScenarioId
						+ TEXT(
							" exact scalar ordered class "
							"and instance semantics applied")),
					bExact);
			}
			else if (ScenarioId.StartsWith(
				TEXT("update_input_mapping_context_")))
			{
				const UInputMappingContext* MappingContext =
					Cast<UInputMappingContext>(AppliedTarget);
				const UInputAction* ActionA =
					Cast<UInputAction>(Source);
				const UInputAction* ActionB =
					FindObject<UInputAction>(
						Owner.Package,
						TEXT("InputMappingActionB"));
				const UInputAction* ActionC =
					FindObject<UInputAction>(
						Owner.Package,
						TEXT("InputMappingActionC"));
				const TArray<FEnhancedActionKeyMapping>*
					Mappings =
						MappingContext
							? &MappingContext->GetMappings()
							: nullptr;
				bool bExact = MappingContext
					&& ActionA
					&& ActionB
					&& ActionC
					&& Mappings;
				if (bExact && ScenarioId
					== TEXT(
						"update_input_mapping_context_set"))
				{
					bExact = Mappings->Num() == 3;
					if (bExact)
					{
						const FEnhancedActionKeyMapping& A =
							(*Mappings)[0];
						const FEnhancedActionKeyMapping& B =
							(*Mappings)[1];
						const FEnhancedActionKeyMapping& C =
							(*Mappings)[2];
						bExact =
							A.Action == ActionA
							&& A.Key == EKeys::SpaceBar
							&& A.Triggers.Num() == 1
							&& A.Triggers[0]
								->IsA<
									UInputTriggerReleased>()
							&& A.Modifiers.Num() == 1
							&& A.Modifiers[0]
								->IsA<
									UInputModifierScalar>()
							&& A.Triggers[0]
								!= InitialMappingATrigger
							&& A.Modifiers[0]
								!= InitialMappingAModifier
							&& B.Action == ActionB
							&& B.Key == EKeys::E
							&& B.Triggers.Num() == 1
							&& B.Triggers[0]
								== InitialMappingBTrigger
							&& B.Modifiers.Num() == 1
							&& B.Modifiers[0]
								== InitialMappingBModifier
							&& C.Action == ActionC
							&& C.Key == EKeys::Q
							&& C.Triggers.Num() == 1
							&& C.Triggers[0]
								->IsA<UInputTriggerHold>()
							&& C.Modifiers.Num() == 1
							&& C.Modifiers[0]
								->IsA<
									UInputModifierSwizzleAxis>();
					}
				}
				else if (bExact && ScenarioId
					== TEXT(
						"update_input_mapping_context_clear"))
				{
					bExact = Mappings->IsEmpty();
				}
				else if (bExact && ScenarioId
					== TEXT(
						"update_input_mapping_context_remove"))
				{
					bExact = Mappings->Num() == 1;
					if (bExact)
					{
						const FEnhancedActionKeyMapping&
							Remaining = (*Mappings)[0];
						bExact =
							Remaining.Action == ActionB
							&& Remaining.Key == EKeys::E
							&& Remaining.Triggers.Num() == 1
							&& Remaining.Triggers[0]
								== InitialMappingBTrigger
							&& Remaining.Modifiers.Num() == 1
							&& Remaining.Modifiers[0]
								== InitialMappingBModifier;
					}
				}
				else if (bExact && ScenarioId
					== TEXT(
						"update_input_mapping_context_replace"))
				{
					bExact = Mappings->Num() == 2;
					if (bExact)
					{
						const FEnhancedActionKeyMapping& C =
							(*Mappings)[0];
						const FEnhancedActionKeyMapping& A =
							(*Mappings)[1];
						bExact =
							C.Action == ActionC
							&& C.Key == EKeys::Q
							&& C.Triggers.Num() == 1
							&& C.Triggers[0]
								->IsA<UInputTriggerHold>()
							&& C.Modifiers.Num() == 1
							&& C.Modifiers[0]
								->IsA<
									UInputModifierSwizzleAxis>()
							&& A.Action == ActionA
							&& A.Key == EKeys::LeftShift
							&& A.Triggers.Num() == 1
							&& A.Triggers[0]
								->IsA<
									UInputTriggerReleased>()
							&& A.Modifiers.Num() == 1
							&& A.Modifiers[0]
								->IsA<
									UInputModifierScalar>();
					}
				}
				Test->TestTrue(
					*(ScenarioId
						+ TEXT(
							" exact ordered identity "
							"configuration and instance "
							"semantics applied")),
					bExact);
			}
			else if (ScenarioId
				== TEXT("replace_texture_array_content"))
			{
				UTexture2DArray* TextureArray =
					Cast<UTexture2DArray>(AppliedTarget);
				UTexture2D* DesiredA =
					Cast<UTexture2D>(Source);
				UTexture2D* DesiredB =
					FindObject<UTexture2D>(
						Owner.Package,
						TEXT("TextureArrayDesiredB"));
				const bool bExact =
					TextureArray
					&& DesiredA
					&& DesiredB
					&& TextureArray->SourceTextures.Num() == 2
					&& TextureArray->SourceTextures[0]
						== DesiredA
					&& TextureArray->SourceTextures[1]
						== DesiredB
					&& TextureArray
						->bSourceGeneratedFromSourceTexturesArray
					&& TextureArray->Source.GetSizeX() == 4
					&& TextureArray->Source.GetSizeY() == 4
					&& TextureArray->Source.GetNumSlices() == 2
					&& TextureArray->Source.GetNumMips() == 1
					&& TextureArray->Source.GetFormat()
						== TSF_BGRA8
					&& !TextureArray->SRGB
					&& TextureArray->CompressionSettings
						== TC_Masks
					&& TextureArray->MipGenSettings
						== TMGS_SimpleAverage
					&& TextureSourceMatchesOrderedConcatenation(
						*TextureArray,
						TArray<UTexture*>{
							DesiredA,
							DesiredB,
						});
				Test->TestTrue(
					TEXT(
						"texture array exact ordered source "
						"content and format settings applied"),
					bExact);
			}
			else if (ScenarioId
				== TEXT(
					"replace_texture_cube_array_content"))
			{
				UTextureCubeArray* TextureArray =
					Cast<UTextureCubeArray>(AppliedTarget);
				UTextureCube* DesiredA =
					Cast<UTextureCube>(Source);
				UTextureCube* DesiredB =
					FindObject<UTextureCube>(
						Owner.Package,
						TEXT("TextureCubeArrayDesiredB"));
				const bool bExact =
					TextureArray
					&& DesiredA
					&& DesiredB
					&& TextureArray->SourceTextures.Num() == 2
					&& TextureArray->SourceTextures[0]
						== DesiredA
					&& TextureArray->SourceTextures[1]
						== DesiredB
					&& TextureArray
						->bSourceGeneratedFromSourceTexturesArray
					&& TextureArray->Source.GetSizeX() == 2
					&& TextureArray->Source.GetSizeY() == 2
					&& TextureArray->Source.GetNumSlices() == 12
					&& TextureArray->Source.GetNumMips() == 1
					&& TextureArray->Source.GetFormat()
						== TSF_BGRA8
					&& !TextureArray->SRGB
					&& TextureArray->CompressionSettings
						== TC_Masks
					&& TextureArray->MipGenSettings
						== TMGS_SimpleAverage
					&& TextureSourceMatchesOrderedConcatenation(
						*TextureArray,
						TArray<UTexture*>{
							DesiredA,
							DesiredB,
						});
				Test->TestTrue(
					TEXT(
						"texture cube array exact ordered "
						"source content and format settings "
						"applied"),
					bExact);
			}
			else if (ScenarioId
				== TEXT("replace_texture_cube_content"))
			{
				UTextureCube* TextureCube =
					Cast<UTextureCube>(AppliedTarget);
				const TArray<UTexture*> Faces{
					Cast<UTexture2D>(Source),
					FindObject<UTexture2D>(
						Owner.Package,
						TEXT("TextureCubeFaceNegativeX")),
					FindObject<UTexture2D>(
						Owner.Package,
						TEXT("TextureCubeFacePositiveY")),
					FindObject<UTexture2D>(
						Owner.Package,
						TEXT("TextureCubeFaceNegativeY")),
					FindObject<UTexture2D>(
						Owner.Package,
						TEXT("TextureCubeFacePositiveZ")),
					FindObject<UTexture2D>(
						Owner.Package,
						TEXT("TextureCubeFaceNegativeZ")),
				};
				bool bFacesValid = Faces.Num() == 6;
				for (UTexture* Face : Faces)
				{
					bFacesValid = bFacesValid && Face;
				}
				const bool bExact =
					TextureCube
					&& bFacesValid
					&& TextureCube->Source.GetSizeX() == 2
					&& TextureCube->Source.GetSizeY() == 2
					&& TextureCube->Source.GetNumSlices() == 6
					&& TextureCube->Source.GetNumMips() == 1
					&& TextureCube->Source.GetFormat()
						== TSF_BGRA8
					&& !TextureCube->SRGB
					&& TextureCube->CompressionSettings
						== TC_Masks
					&& TextureCube->MipGenSettings
						== TMGS_SimpleAverage
					&& TextureSourceMatchesOrderedConcatenation(
						*TextureCube,
						Faces);
				Test->TestTrue(
					TEXT(
						"texture cube exact fixed face order "
						"source content and format settings "
						"applied"),
					bExact);
			}
			else if (ScenarioId
				== TEXT("replace_volume_texture_content"))
			{
				UVolumeTexture* VolumeTexture =
					Cast<UVolumeTexture>(AppliedTarget);
				const TArray<UTexture*> Slices{
					Cast<UTexture2D>(Source),
					FindObject<UTexture2D>(
						Owner.Package,
						TEXT("VolumeTextureDesiredB")),
					FindObject<UTexture2D>(
						Owner.Package,
						TEXT("VolumeTextureDesiredC")),
				};
				bool bSlicesValid = Slices.Num() == 3;
				for (UTexture* Slice : Slices)
				{
					bSlicesValid = bSlicesValid && Slice;
				}
				const bool bExact =
					VolumeTexture
					&& bSlicesValid
					&& VolumeTexture->Source.GetSizeX() == 2
					&& VolumeTexture->Source.GetSizeY() == 2
					&& VolumeTexture->Source.GetNumSlices() == 3
					&& VolumeTexture->Source.GetNumMips() == 1
					&& VolumeTexture->Source.GetFormat()
						== TSF_BGRA8
					&& VolumeTexture->Source2DTexture == nullptr
					&& VolumeTexture->Source2DTileSizeX == 0
					&& VolumeTexture->Source2DTileSizeY == 0
					&& !VolumeTexture->SRGB
					&& VolumeTexture->CompressionSettings
						== TC_Masks
					&& VolumeTexture->MipGenSettings
						== TMGS_SimpleAverage
					&& TextureSourceMatchesOrderedConcatenation(
						*VolumeTexture,
						Slices);
				Test->TestTrue(
					TEXT(
						"volume texture exact ordered slice "
						"source content and format settings "
						"applied"),
					bExact);
			}
			else if (ScenarioId
				== TEXT("update_subuv_fields_set"))
			{
				const USubUVAnimation* SubUV =
					Cast<USubUVAnimation>(AppliedTarget);
				Test->TestTrue(
					TEXT("SubUV declared fields applied"),
					SubUV
						&& SubUV->SubUVTexture == Source
						&& SubUV->SubImages_Horizontal == 4
						&& SubUV->SubImages_Vertical == 6
						&& SubUV->BoundingMode
							== BVC_EightVertices);
			}
			else if (ScenarioId
				== TEXT("update_subuv_fields_clear"))
			{
				const USubUVAnimation* SubUV =
					Cast<USubUVAnimation>(AppliedTarget);
				const USubUVAnimation* Default =
					GetDefault<USubUVAnimation>();
				Test->TestTrue(
					TEXT("SubUV declared fields cleared to CDO"),
					SubUV
						&& Default
						&& SubUV->SubUVTexture
							== Default->SubUVTexture
						&& SubUV->SubImages_Horizontal
							== Default->SubImages_Horizontal
						&& SubUV->SubImages_Vertical
							== Default->SubImages_Vertical
						&& SubUV->BoundingMode
							== Default->BoundingMode);
			}
			else if (ScenarioId
				== TEXT("update_subuv_source_null"))
			{
				const USubUVAnimation* SubUV =
					Cast<USubUVAnimation>(AppliedTarget);
				Test->TestTrue(
					TEXT("SubUV source explicit null applied"),
					SubUV
						&& !SubUV->SubUVTexture
						&& SubUV->SubImages_Horizontal == 3
						&& SubUV->SubImages_Vertical == 5
						&& SubUV->BoundingMode
							== BVC_FourVertices);
			}
			else if (ScenarioId.StartsWith(
				TEXT("update_texture_collection_")))
			{
				const UTextureCollection* Collection =
					Cast<UTextureCollection>(AppliedTarget);
				const UTexture* TextureA =
					Cast<UTexture>(Source);
				const UTexture* TextureB =
					FindObject<UTexture>(
						Owner.Package,
						TEXT("TextureB"));
				const UTexture* TextureC =
					FindObject<UTexture>(
						Owner.Package,
						TEXT("TextureC"));
				bool bExact = Collection
					&& TextureA
					&& TextureB
					&& TextureC;
				if (bExact && ScenarioId
					== TEXT("update_texture_collection_set"))
				{
					bExact = Collection->Textures.Num() == 3
						&& Collection->Textures[0] == TextureA
						&& Collection->Textures[1] == TextureB
						&& Collection->Textures[2] == TextureC;
				}
				else if (bExact && ScenarioId
					== TEXT("update_texture_collection_clear"))
				{
					bExact = Collection->Textures.IsEmpty();
				}
				else if (bExact && ScenarioId
					== TEXT("update_texture_collection_remove"))
				{
					bExact = Collection->Textures.Num() == 1
						&& Collection->Textures[0] == TextureB;
				}
				else if (bExact && ScenarioId
					== TEXT("update_texture_collection_replace"))
				{
					bExact = Collection->Textures.Num() == 2
						&& Collection->Textures[0] == TextureC
						&& Collection->Textures[1] == TextureA;
				}
				Test->TestTrue(
					*(ScenarioId
						+ TEXT(
							" full ordered identity applied")),
					bExact);
			}
			else if (ScenarioId.StartsWith(
				TEXT("update_paper_sprite_")))
			{
				const UPaperSprite* Sprite =
					Cast<UPaperSprite>(AppliedTarget);
				const UTexture* TextureExtra =
					FindObject<UTexture>(
						Owner.Package,
						TEXT("PaperSpriteTextureExtra"));
				FAdditionalSpriteTextureArray
					AdditionalTextures;
				if (Sprite)
				{
					Sprite->GetBakedAdditionalSourceTextures(
						AdditionalTextures);
				}
				bool bExact = Sprite
					&& TextureExtra
					&& AdditionalTextures.Num() == 1
					&& AdditionalTextures[0] == TextureExtra;
				FVector2D CustomPivot = FVector2D::ZeroVector;
				const ESpritePivotMode::Type PivotMode =
					Sprite
						? Sprite->GetPivotMode(CustomPivot)
						: ESpritePivotMode::Center_Center;
				if (bExact && ScenarioId
					== TEXT("update_paper_sprite_fields_set"))
				{
					bExact = Sprite->GetSourceTexture() == Source
						&& Sprite->GetSourceUV().Equals(
							FVector2D(4.0, 5.0))
						&& Sprite->GetSourceSize().Equals(
							FVector2D(20.0, 16.0))
						&& PivotMode == ESpritePivotMode::Custom
						&& CustomPivot.Equals(
							FVector2D(7.0, 8.0));
				}
				else if (bExact && ScenarioId
					== TEXT("update_paper_sprite_fields_clear"))
				{
					const UPaperSprite* Default =
						GetDefault<UPaperSprite>();
					FVector2D DefaultCustomPivot =
						FVector2D::ZeroVector;
					const ESpritePivotMode::Type DefaultPivotMode =
						Default
							? Default->GetPivotMode(
								DefaultCustomPivot)
							: ESpritePivotMode::Center_Center;
					bExact = Default
						&& Sprite->GetSourceTexture()
							== Default->GetSourceTexture()
						&& Sprite->GetSourceUV().Equals(
							Default->GetSourceUV())
						&& Sprite->GetSourceSize().Equals(
							Default->GetSourceSize())
						&& PivotMode == DefaultPivotMode
						&& CustomPivot.Equals(
							DefaultCustomPivot);
				}
				else if (bExact && ScenarioId
					== TEXT("update_paper_sprite_source_null"))
				{
					bExact = !Sprite->GetSourceTexture()
						&& Sprite->GetSourceUV().Equals(
							FVector2D(1.0, 2.0))
						&& Sprite->GetSourceSize().Equals(
							FVector2D(12.0, 10.0))
						&& PivotMode == ESpritePivotMode::Custom
						&& CustomPivot.Equals(
							FVector2D(3.0, 4.0));
				}
				Test->TestTrue(
					*(ScenarioId
						+ TEXT(
							" exact fields and additional "
							"textures applied")),
					bExact);
			}
			else if (ScenarioId.StartsWith(
				TEXT("replace_paper_flipbook_")))
			{
				const UPaperFlipbook* Flipbook =
					Cast<UPaperFlipbook>(AppliedTarget);
				const UPaperFlipbook* Default =
					GetDefault<UPaperFlipbook>();
				const UPaperSprite* SpriteA =
					FindObject<UPaperSprite>(
						Owner.Package,
						TEXT("PaperFlipbookSpriteA"));
				const UPaperSprite* SpriteB =
					FindObject<UPaperSprite>(
						Owner.Package,
						TEXT("PaperFlipbookSpriteB"));
				bool bExact = Flipbook
					&& Default
					&& SpriteA
					&& SpriteB
					&& Flipbook->GetDefaultMaterial()
						== Default->GetDefaultMaterial()
					&& Flipbook->GetCollisionSource()
						== Default->GetCollisionSource();
				if (bExact && ScenarioId
					== TEXT("replace_paper_flipbook_content"))
				{
					bExact =
						Flipbook->GetNumKeyFrames() == 3;
					if (bExact)
					{
						const FPaperFlipbookKeyFrame& Frame0 =
							Flipbook->GetKeyFrameChecked(0);
						const FPaperFlipbookKeyFrame& Frame1 =
							Flipbook->GetKeyFrameChecked(1);
						const FPaperFlipbookKeyFrame& Frame2 =
							Flipbook->GetKeyFrameChecked(2);
						bExact =
							Frame0.Sprite == Source
							&& Frame0.FrameRun == 4
							&& !Frame1.Sprite
							&& Frame1.FrameRun == 1
							&& Frame2.Sprite == SpriteA
							&& Frame2.FrameRun == 2
							&& Flipbook->GetNumFrames() == 7
							&& FMath::IsNearlyEqual(
								Flipbook->GetFramesPerSecond(),
								24.0f)
							&& FMath::IsNearlyEqual(
								Flipbook->GetTotalDuration(),
								7.0f / 24.0f)
							&& Flipbook->GetSpriteAtTime(
								2.0f / 24.0f,
								true) == Source
							&& !Flipbook->GetSpriteAtTime(
								4.5f / 24.0f,
								true)
							&& Flipbook->GetSpriteAtTime(
								6.0f / 24.0f,
								true) == SpriteA;
					}
				}
				else if (bExact && ScenarioId
					== TEXT(
						"replace_paper_flipbook_frames_empty"))
				{
					bExact =
						Flipbook->GetNumKeyFrames() == 0
						&& Flipbook->GetNumFrames() == 0
						&& FMath::IsNearlyEqual(
							Flipbook->GetFramesPerSecond(),
							12.0f)
						&& FMath::IsNearlyZero(
							Flipbook->GetTotalDuration());
				}
				else if (bExact && ScenarioId
					== TEXT("replace_paper_flipbook_fps_only"))
				{
					bExact =
						Flipbook->GetNumKeyFrames() == 2;
					if (bExact)
					{
						const FPaperFlipbookKeyFrame& Frame0 =
							Flipbook->GetKeyFrameChecked(0);
						const FPaperFlipbookKeyFrame& Frame1 =
							Flipbook->GetKeyFrameChecked(1);
						bExact =
							Frame0.Sprite == SpriteA
							&& Frame0.FrameRun == 2
							&& Frame1.Sprite == SpriteB
							&& Frame1.FrameRun == 3
							&& Flipbook->GetNumFrames() == 5
							&& FMath::IsNearlyEqual(
								Flipbook->GetFramesPerSecond(),
								30.0f)
							&& FMath::IsNearlyEqual(
								Flipbook->GetTotalDuration(),
								5.0f / 30.0f);
					}
				}
				Test->TestTrue(
					*(ScenarioId
						+ TEXT(
							" exact ordered content and "
							"omission semantics applied")),
					bExact);
			}
			else if (ScenarioId.StartsWith(
				TEXT("replace_paper_tilemap_")))
			{
				const UPaperTileMap* TileMap =
					Cast<UPaperTileMap>(AppliedTarget);
				const UPaperTileSet* TileSetA =
					FindObject<UPaperTileSet>(
						Owner.Package,
						TEXT("PaperTileMapSetA"));
				bool bExact =
					TileMap
					&& TileSetA;
				if (bExact
					&& ScenarioId
						== TEXT("replace_paper_tilemap_content"))
				{
					bExact =
						TileMap->MapWidth == 3
						&& TileMap->MapHeight == 2
						&& TileMap->TileWidth == 16
						&& TileMap->TileHeight == 24
						&& TileMap->TileLayers.Num() == 2;
					if (bExact)
					{
						const UPaperTileLayer* Ground =
							TileMap->TileLayers[0];
						const UPaperTileLayer* Overlay =
							TileMap->TileLayers[1];
						bool bGroundHiddenInGame = true;
						bool bGroundThicknessOverride = true;
						bool bGroundOffsetOverride = true;
						float GroundThickness = 0.0f;
						float GroundOffset = 0.0f;
						bool bOverlayHiddenInGame = false;
						bool bOverlayThicknessOverride = false;
						bool bOverlayOffsetOverride = false;
						float OverlayThickness = 0.0f;
						float OverlayOffset = 0.0f;
						const bool bPrivateRead =
							Ground
							&& Overlay
							&& ReadExactBoolProperty(
								*Ground,
								TEXT("bHiddenInGame"),
								bGroundHiddenInGame)
							&& ReadExactBoolProperty(
								*Ground,
								TEXT(
									"bOverrideCollisionThickness"),
								bGroundThicknessOverride)
							&& ReadExactBoolProperty(
								*Ground,
								TEXT(
									"bOverrideCollisionOffset"),
								bGroundOffsetOverride)
							&& ReadExactFloatProperty(
								*Ground,
								TEXT(
									"CollisionThicknessOverride"),
								GroundThickness)
							&& ReadExactFloatProperty(
								*Ground,
								TEXT(
									"CollisionOffsetOverride"),
								GroundOffset)
							&& ReadExactBoolProperty(
								*Overlay,
								TEXT("bHiddenInGame"),
								bOverlayHiddenInGame)
							&& ReadExactBoolProperty(
								*Overlay,
								TEXT(
									"bOverrideCollisionThickness"),
								bOverlayThicknessOverride)
							&& ReadExactBoolProperty(
								*Overlay,
								TEXT(
									"bOverrideCollisionOffset"),
								bOverlayOffsetOverride)
							&& ReadExactFloatProperty(
								*Overlay,
								TEXT(
									"CollisionThicknessOverride"),
								OverlayThickness)
							&& ReadExactFloatProperty(
								*Overlay,
								TEXT(
									"CollisionOffsetOverride"),
								OverlayOffset);
						const FPaperTileInfo GroundCell0 =
							Ground
								? Ground->GetCell(0, 0)
								: FPaperTileInfo();
						const FPaperTileInfo GroundCell1 =
							Ground
								? Ground->GetCell(2, 1)
								: FPaperTileInfo();
						bExact =
							bPrivateRead
							&& Ground->LayerName.ToString()
								== TEXT("Ground")
							&& Ground->ShouldRenderInEditor()
							&& !bGroundHiddenInGame
							&& Ground->GetLayerCollides()
							&& !bGroundThicknessOverride
							&& !bGroundOffsetOverride
							&& Ground->GetLayerColor().Equals(
								FLinearColor(
									1.0f,
									0.5f,
									0.25f,
									1.0f))
							&& GroundCell0.TileSet == Source
							&& GroundCell0.GetTileIndex() == 2
							&& GroundCell0.HasFlag(
								EPaperTileFlags::FlipHorizontal)
							&& !GroundCell0.HasFlag(
								EPaperTileFlags::FlipVertical)
							&& !GroundCell0.HasFlag(
								EPaperTileFlags::FlipDiagonal)
							&& GroundCell1.TileSet == TileSetA
							&& GroundCell1.GetTileIndex() == 1
							&& GroundCell1.HasFlag(
								EPaperTileFlags::FlipDiagonal)
							&& Overlay->LayerName.ToString()
								== TEXT("Overlay")
							&& !Overlay->ShouldRenderInEditor()
							&& bOverlayHiddenInGame
							&& !Overlay->GetLayerCollides()
							&& bOverlayThicknessOverride
							&& FMath::IsNearlyEqual(
								OverlayThickness,
								25.0f)
							&& bOverlayOffsetOverride
							&& FMath::IsNearlyEqual(
								OverlayOffset,
								-3.0f)
							&& Overlay->GetLayerColor().Equals(
								FLinearColor(
									0.1f,
									0.2f,
									0.3f,
									0.4f))
							&& Overlay->GetNumOccupiedCells()
								== 0;
					}
				}
				else if (bExact
					&& ScenarioId
						== TEXT(
							"replace_paper_tilemap_layers_empty"))
				{
					bExact =
						TileMap->MapWidth == 2
						&& TileMap->MapHeight == 2
						&& TileMap->TileWidth == 8
						&& TileMap->TileHeight == 8
						&& TileMap->TileLayers.IsEmpty();
				}
				else if (bExact
					&& ScenarioId
						== TEXT(
							"replace_paper_tilemap_"
							"tile_size_only"))
				{
					const UPaperTileLayer* Layer =
						TileMap->TileLayers.Num() == 1
							? TileMap->TileLayers[0]
							: nullptr;
					const FPaperTileInfo Cell =
						Layer
							? Layer->GetCell(1, 1)
							: FPaperTileInfo();
					bExact =
						TileMap->MapWidth == 2
						&& TileMap->MapHeight == 2
						&& TileMap->TileWidth == 12
						&& TileMap->TileHeight == 18
						&& Layer
						&& Layer->LayerName.ToString()
							== TEXT("Original")
						&& Cell.TileSet == TileSetA
						&& Cell.GetTileIndex() == 3
						&& Cell.HasFlag(
							EPaperTileFlags::FlipVertical);
				}
				Test->TestTrue(
					*(ScenarioId
						+ TEXT(
							" exact grid layer cell and "
							"omission semantics applied")),
					bExact);
			}
			else if (ScenarioId.StartsWith(
				TEXT("replace_paper_tileset_")))
			{
				const UPaperTileSet* TileSet =
					Cast<UPaperTileSet>(AppliedTarget);
				const UTexture2D* TextureA =
					FindObject<UTexture2D>(
						Owner.Package,
						TEXT("PaperTileSetTextureA"));
				bool bExact = TileSet && TextureA;
				if (bExact
					&& ScenarioId
						== TEXT("replace_paper_tileset_content"))
				{
					const FIntMargin Margin =
						TileSet->GetMargin();
					bExact =
						TileSet->GetTileSheetTexture() == Source
						&& TileSet->GetTileSize()
							== FIntPoint(16, 8)
						&& Margin == FIntMargin(1)
						&& TileSet->GetPerTileSpacing()
							== FIntPoint(2, 2)
						&& TileSet->GetTileCountX() == 3
						&& TileSet->GetTileCountY() == 3
						&& TileSet->GetTileCount() == 9
						&& TileSet->GetNumTerrains() == 2
						&& TileSet->GetTerrain(0).TerrainName
							== TEXT("Grass")
						&& TileSet->GetTerrain(0)
								.CenterTileIndex
							== 0
						&& TileSet->GetTerrain(1).TerrainName
							== TEXT("Stone")
						&& TileSet->GetTerrain(1)
								.CenterTileIndex
							== 1;
				}
				else if (bExact
					&& ScenarioId
						== TEXT(
							"replace_paper_tileset_"
							"terrain_empty"))
				{
					bExact =
						TileSet->GetTileSheetTexture()
							== TextureA
						&& TileSet->GetTileSize()
							== FIntPoint(8, 8)
						&& TileSet->GetMargin()
							== FIntMargin(0)
						&& TileSet->GetPerTileSpacing()
							== FIntPoint::ZeroValue
						&& TileSet->GetTileCount() == 16
						&& TileSet->GetNumTerrains() == 0;
				}
				else if (bExact
					&& ScenarioId
						== TEXT(
							"replace_paper_tileset_"
							"layout_only"))
				{
					const FPaperTileMetadata* Metadata =
						TileSet->GetTileMetadata(1);
					bExact =
						TileSet->GetTileSheetTexture()
							== TextureA
						&& TileSet->GetTileSize()
							== FIntPoint(4, 4)
						&& TileSet->GetMargin()
							== FIntMargin(2)
						&& TileSet->GetPerTileSpacing()
							== FIntPoint(1, 1)
						&& TileSet->GetTileCountX() == 5
						&& TileSet->GetTileCountY() == 5
						&& TileSet->GetTileCount() == 25
						&& TileSet->GetNumTerrains() == 1
						&& TileSet->GetTerrain(0).TerrainName
							== TEXT("OriginalTerrain")
						&& TileSet->GetTerrain(0)
								.CenterTileIndex
							== 1
						&& Metadata
						&& Metadata->UserDataName
							== TEXT("OriginalMetadata")
						&& Metadata->TerrainMembership[0] == 0;
				}
				Test->TestTrue(
					*(ScenarioId
						+ TEXT(
							" exact atlas terrain and "
							"omission semantics applied")),
					bExact);
			}
			if (Source
				&& ScenarioId == TEXT("clone_data_object"))
			{
				UUeAgentAssetDataSimpleEffectDataAsset*
						ClonedData =
							Cast<
								UUeAgentAssetDataSimpleEffectDataAsset>(
								AppliedTarget);
				Test->TestTrue(
					TEXT(
						"clone target is an independent exact-class "
						"instance"),
					ClonedData
						&& ClonedData != Source
						&& ClonedData->GetClass()
							== Source->GetClass()
						&& ClonedData->GetOutermost()
							== Owner.Package
						&& ClonedData->GetFName()
							== FName(TEXT("CreatedTarget")));
				Test->TestTrue(
					TEXT(
						"clone target has durable asset object flags"),
					ClonedData
						&& ClonedData->HasAllFlags(
							RF_Public
								| RF_Standalone
								| RF_Transactional));
				if (ClonedData)
				{
					Test->TestEqual(
						TEXT(
							"clone target preserves complete "
							"registered source state"),
						ReadFingerprint(
							*ClonedData,
							TEXT("data_object")),
						SourceFingerprintBefore);
				}
				Test->TestEqual(
					TEXT("clone source remains unchanged"),
					ReadFingerprint(*Source, TEXT("data_object")),
					SourceFingerprintBefore);
			}

			bool bRollbackConfirmed = false;
			Test->TestTrue(
				*(ScenarioId + TEXT(" rollback executes")),
				Hook->TryRollback(
					Request,
					Applied,
					*Resources,
					bRollbackConfirmed,
					Error));
			Test->TestTrue(
				*(ScenarioId + TEXT(" rollback confirmed")),
				bRollbackConfirmed);
			if (Target)
			{
				Test->TestEqual(
					*(ScenarioId
						+ TEXT(" target restored")),
					ReadFingerprint(
						*Target,
						Row->GetStringField(
							TEXT("profile_id"))),
					TargetFingerprintBefore);
			}
			else
			{
				Test->TestNull(
					*(ScenarioId
						+ TEXT(" created target removed")),
					FindObject<UObject>(
						Owner.Package,
						TEXT("CreatedTarget")));
			}
			if (ScenarioId
				== TEXT(
					"update_sound_class_properties_and_children"))
			{
				const USoundClass* SoundClass =
					Cast<USoundClass>(Target);
				const USoundClass* Child =
					Cast<USoundClass>(Source);
				Test->TestTrue(
					TEXT(
						"sound class rollback restores inverse "
						"relation"),
					SoundClass
						&& Child
						&& SoundClass->ChildClasses.IsEmpty()
						&& Child->ParentClass == nullptr);
			}
			else if (ScenarioId
				== TEXT(
					"update_sound_submix_parent_and_effect_chain"))
			{
				const USoundSubmix* Submix =
					Cast<USoundSubmix>(Target);
				const USoundSubmix* Parent =
					Cast<USoundSubmix>(Source);
				Test->TestTrue(
					TEXT(
						"sound submix rollback restores inverse "
						"relation"),
					Submix
						&& Parent
						&& Submix->ParentSubmix == nullptr
						&& Parent->ChildSubmixes.IsEmpty());
			}
			Test->TestEqual(
				*(ScenarioId
					+ TEXT(" package dirty restored")),
				Owner.Package->IsDirty(),
				bDirtyBefore);
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDataSimpleEffectNativeTest,
	"UeAgentInterface.DomainAdapters.AssetDataSimpleEffect.E2."
		"OwnedAssetReadbackRollback",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDataSimpleEffectNativeTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	return RunNativeTests(this, Fixture);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDataSimpleEffectMediaFieldsNativeTest,
	"UeAgentInterface.DomainAdapters.AssetDataSimpleEffect.E2."
		"MediaFields",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDataSimpleEffectMediaFieldsNativeTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	const TSet<FString> IncludedScenarioIds{
		TEXT("update_media_player_fields"),
		TEXT("update_media_texture_fields"),
	};
	return RunNativeTests(
		this,
		Fixture,
		&IncludedScenarioIds);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDataSimpleEffectRenderTargetFieldsNativeTest,
	"UeAgentInterface.DomainAdapters.AssetDataSimpleEffect.E2."
		"RenderTargetFields",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDataSimpleEffectRenderTargetFieldsNativeTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	const TSet<FString> IncludedScenarioIds{
		TEXT("update_texture_settings"),
	};
	return RunNativeTests(
		this,
		Fixture,
		&IncludedScenarioIds);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDataSimpleEffectSubUVFieldsNativeTest,
	"UeAgentInterface.DomainAdapters.AssetDataSimpleEffect.E2."
		"SubUVFields",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDataSimpleEffectSubUVFieldsNativeTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	const TSet<FString> IncludedScenarioIds{
		TEXT("update_subuv_fields_set"),
		TEXT("update_subuv_fields_clear"),
		TEXT("update_subuv_source_null"),
	};
	return RunNativeTests(
		this,
		Fixture,
		&IncludedScenarioIds);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDataSimpleEffectTextureCollectionFieldsNativeTest,
	"UeAgentInterface.DomainAdapters.AssetDataSimpleEffect.E2."
		"TextureCollectionFields",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDataSimpleEffectTextureCollectionFieldsNativeTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	const TSet<FString> IncludedScenarioIds{
		TEXT("update_texture_collection_set"),
		TEXT("update_texture_collection_clear"),
		TEXT("update_texture_collection_remove"),
		TEXT("update_texture_collection_replace"),
	};
	return RunNativeTests(
		this,
		Fixture,
		&IncludedScenarioIds);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDataSimpleEffectPaperSpriteFieldsNativeTest,
	"UeAgentInterface.DomainAdapters.AssetDataSimpleEffect.E2."
		"PaperSpriteFields",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDataSimpleEffectPaperSpriteFieldsNativeTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	const TSet<FString> IncludedScenarioIds{
		TEXT("update_paper_sprite_fields_set"),
		TEXT("update_paper_sprite_fields_clear"),
		TEXT("update_paper_sprite_source_null"),
	};
	return RunNativeTests(
		this,
		Fixture,
		&IncludedScenarioIds);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDataSimpleEffectPaperFlipbookContentNativeTest,
	"UeAgentInterface.DomainAdapters.AssetDataSimpleEffect.E2."
		"PaperFlipbookContent",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDataSimpleEffectPaperFlipbookContentNativeTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	const TSet<FString> IncludedScenarioIds{
		TEXT("replace_paper_flipbook_content"),
		TEXT("replace_paper_flipbook_frames_empty"),
		TEXT("replace_paper_flipbook_fps_only"),
	};
	return RunNativeTests(
		this,
		Fixture,
		&IncludedScenarioIds);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDataSimpleEffectPaperTileContentNativeTest,
	"UeAgentInterface.DomainAdapters.AssetDataSimpleEffect.E2."
		"PaperTileContent",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDataSimpleEffectPaperTileContentNativeTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	const TSet<FString> IncludedScenarioIds{
		TEXT("replace_paper_tilemap_content"),
		TEXT("replace_paper_tilemap_layers_empty"),
		TEXT("replace_paper_tilemap_tile_size_only"),
		TEXT("replace_paper_tileset_content"),
		TEXT("replace_paper_tileset_terrain_empty"),
		TEXT("replace_paper_tileset_layout_only"),
	};
	return RunNativeTests(
		this,
		Fixture,
		&IncludedScenarioIds);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDataSimpleEffectEnhancedInputNativeTest,
	"UeAgentInterface.DomainAdapters.AssetDataSimpleEffect.E2."
		"EnhancedInput",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDataSimpleEffectEnhancedInputNativeTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	const TSet<FString> IncludedScenarioIds{
		TEXT("update_input_action_fields"),
		TEXT("update_input_action_collections_clear"),
		TEXT("update_input_action_collections_set"),
		TEXT("update_input_mapping_context_set"),
		TEXT("update_input_mapping_context_clear"),
		TEXT("update_input_mapping_context_remove"),
		TEXT("update_input_mapping_context_replace"),
	};
	return RunNativeTests(
		this,
		Fixture,
		&IncludedScenarioIds);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDataSimpleEffectTextureArrayFamilyNativeTest,
	"UeAgentInterface.DomainAdapters.AssetDataSimpleEffect.E2."
		"TextureArrayFamily",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDataSimpleEffectTextureArrayFamilyNativeTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	const TSet<FString> IncludedScenarioIds{
		TEXT("replace_texture_array_content"),
		TEXT("replace_texture_cube_array_content"),
	};
	return RunNativeTests(
		this,
		Fixture,
		&IncludedScenarioIds);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDataSimpleEffectTextureSourceAssemblyNativeTest,
	"UeAgentInterface.DomainAdapters.AssetDataSimpleEffect.E2."
		"TextureSourceAssembly",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDataSimpleEffectTextureSourceAssemblyNativeTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	const TSet<FString> IncludedScenarioIds{
		TEXT("replace_texture_cube_content"),
		TEXT("replace_volume_texture_content"),
	};
	return RunNativeTests(
		this,
		Fixture,
		&IncludedScenarioIds);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDataSimpleEffectRemainingMechanicsNativeTest,
	"UeAgentInterface.DomainAdapters.AssetDataSimpleEffect.E2."
		"RemainingMechanics",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetDataSimpleEffectRemainingMechanicsNativeTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	const TSet<FString> IncludedScenarioIds{
		TEXT("update_audio_effect_preset_settings"),
		TEXT("update_sound_attenuation_settings"),
		TEXT("update_sound_class_properties_and_children"),
		TEXT("update_sound_concurrency_settings"),
		TEXT("update_sound_mix_modifiers"),
		TEXT("update_sound_submix_parent_and_effect_chain"),
		TEXT("replace_curve_keys"),
		TEXT("update_data_object_registered_fields"),
		TEXT("update_file_media_source"),
		TEXT("update_runtime_virtual_texture"),
		TEXT("update_sparse_volume_texture_settings"),
	};
	return RunNativeTests(
		this,
		Fixture,
		&IncludedScenarioIds);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAssetDataSimpleEffectStructuredAssetCompletionNativeTest,
	"UeAgentInterface.DomainAdapters.AssetDataSimpleEffect.E2."
		"StructuredAssetCompletion",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool
FUeAgentAssetDataSimpleEffectStructuredAssetCompletionNativeTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	const TSet<FString> IncludedScenarioIds{
		TEXT("update_static_mesh_collections"),
		TEXT("replace_data_table_rows"),
		TEXT("replace_skeletal_mesh_content"),
	};
	return RunNativeTests(
		this,
		Fixture,
		&IncludedScenarioIds);
}
