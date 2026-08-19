#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectPaperTileMechanics.h"

#include "Engine/Texture2D.h"
#include "PaperTileLayer.h"
#include "PaperTileMap.h"
#include "PaperTileSet.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::DomainAdapters::AssetDataSimpleEffect
{
	using namespace ExecutionMutation;
	using namespace StateSemantic;

	namespace
	{
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

		const FUeAgentCanonicalValue* FindObjectField(
			const FUeAgentCanonicalValue& Object,
			const FString& Field)
		{
			if (Object.GetKind() != EUeAgentValueKind::Object)
			{
				return nullptr;
			}
			for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
				: Object.GetObjectItems())
			{
				if (Item.Key == Field)
				{
					return &Item.Value.Get();
				}
			}
			return nullptr;
		}

		bool TryReadNumber(
			const FUeAgentCanonicalValue& Value,
			double& OutNumber)
		{
			if (Value.GetKind() != EUeAgentValueKind::Integer
				&& Value.GetKind() != EUeAgentValueKind::Decimal)
			{
				return false;
			}
			return LexTryParseString(
				OutNumber,
				*Value.GetLexeme())
				&& FMath::IsFinite(OutNumber);
		}

		bool TryReadInt32(
			const FUeAgentCanonicalValue& Value,
			int32& OutValue)
		{
			double Number = 0.0;
			if (!TryReadNumber(Value, Number)
				|| FMath::FloorToDouble(Number) != Number
				|| Number < static_cast<double>(MIN_int32)
				|| Number > static_cast<double>(MAX_int32))
			{
				return false;
			}
			OutValue = static_cast<int32>(Number);
			return true;
		}

		bool TryReadBool(
			const FUeAgentCanonicalValue& Object,
			const FString& Field,
			bool& bOutValue)
		{
			const FUeAgentCanonicalValue* Value =
				FindObjectField(Object, Field);
			if (!Value
				|| Value->GetKind()
					!= EUeAgentValueKind::Boolean)
			{
				return false;
			}
			bOutValue = Value->GetBoolean();
			return true;
		}

		bool TryReadExtent2i(
			const FUeAgentCanonicalValue& Value,
			const int32 Minimum,
			const int32 Maximum,
			FIntPoint& OutExtent)
		{
			if (Value.GetKind() != EUeAgentValueKind::Object
				|| Value.GetObjectItems().Num() != 2)
			{
				return false;
			}
			const FUeAgentCanonicalValue* X =
				FindObjectField(Value, TEXT("x"));
			const FUeAgentCanonicalValue* Y =
				FindObjectField(Value, TEXT("y"));
			return X
				&& Y
				&& TryReadInt32(*X, OutExtent.X)
				&& TryReadInt32(*Y, OutExtent.Y)
				&& OutExtent.X >= Minimum
				&& OutExtent.Y >= Minimum
				&& OutExtent.X <= Maximum
				&& OutExtent.Y <= Maximum;
		}

		bool TryReadLinearColor(
			const FUeAgentCanonicalValue& Value,
			FLinearColor& OutColor)
		{
			if (Value.GetKind() != EUeAgentValueKind::Object
				|| Value.GetObjectItems().Num() != 4)
			{
				return false;
			}
			const FUeAgentCanonicalValue* R =
				FindObjectField(Value, TEXT("r"));
			const FUeAgentCanonicalValue* G =
				FindObjectField(Value, TEXT("g"));
			const FUeAgentCanonicalValue* B =
				FindObjectField(Value, TEXT("b"));
			const FUeAgentCanonicalValue* A =
				FindObjectField(Value, TEXT("a"));
			double Red = 0.0;
			double Green = 0.0;
			double Blue = 0.0;
			double Alpha = 0.0;
			if (!R
				|| !G
				|| !B
				|| !A
				|| !TryReadNumber(*R, Red)
				|| !TryReadNumber(*G, Green)
				|| !TryReadNumber(*B, Blue)
				|| !TryReadNumber(*A, Alpha)
				|| Red < -static_cast<double>(MAX_flt)
				|| Red > static_cast<double>(MAX_flt)
				|| Green < -static_cast<double>(MAX_flt)
				|| Green > static_cast<double>(MAX_flt)
				|| Blue < -static_cast<double>(MAX_flt)
				|| Blue > static_cast<double>(MAX_flt)
				|| Alpha < -static_cast<double>(MAX_flt)
				|| Alpha > static_cast<double>(MAX_flt))
			{
				return false;
			}
			OutColor = FLinearColor(
				static_cast<float>(Red),
				static_cast<float>(Green),
				static_cast<float>(Blue),
				static_cast<float>(Alpha));
			return true;
		}

		bool TryReadOptionalFloat(
			const FUeAgentCanonicalValue& Object,
			const FString& Field,
			const bool bRequireNonNegative,
			bool& bOutEnabled,
			float& OutValue)
		{
			const FUeAgentCanonicalValue* Value =
				FindObjectField(Object, Field);
			if (!Value)
			{
				return false;
			}
			if (Value->GetKind() == EUeAgentValueKind::Null)
			{
				bOutEnabled = false;
				OutValue = 0.0f;
				return true;
			}
			double Number = 0.0;
			if (!TryReadNumber(*Value, Number)
				|| Number < -static_cast<double>(MAX_flt)
				|| Number > static_cast<double>(MAX_flt)
				|| (bRequireNonNegative && Number < 0.0))
			{
				return false;
			}
			bOutEnabled = true;
			OutValue = static_cast<float>(Number);
			return true;
		}

		bool ValidateLayerPrivateBindings()
		{
			const UClass* Class = UPaperTileLayer::StaticClass();
			return FindFProperty<FBoolProperty>(
					Class,
					TEXT("bHiddenInGame"))
				&& FindFProperty<FBoolProperty>(
					Class,
					TEXT("bOverrideCollisionThickness"))
				&& FindFProperty<FBoolProperty>(
					Class,
					TEXT("bOverrideCollisionOffset"))
				&& FindFProperty<FFloatProperty>(
					Class,
					TEXT("CollisionThicknessOverride"))
				&& FindFProperty<FFloatProperty>(
					Class,
					TEXT("CollisionOffsetOverride"));
		}

		bool TrySetExactBoolProperty(
			UObject& Object,
			const FName PropertyName,
			const bool bValue)
		{
			FBoolProperty* Property =
				FindFProperty<FBoolProperty>(
					Object.GetClass(),
					PropertyName);
			if (!Property)
			{
				return false;
			}
			Property->SetPropertyValue_InContainer(
				&Object,
				bValue);
			return true;
		}

		struct FPaperTileMapCellState
		{
			int32 X = 0;
			int32 Y = 0;
			int32 TileIndex = 0;
			bool bFlipHorizontal = false;
			bool bFlipVertical = false;
			bool bFlipDiagonal = false;
			TWeakObjectPtr<UPaperTileSet> TileSet;
		};

		struct FPaperTileMapLayerState
		{
			FString Name;
			bool bVisibleInEditor = true;
			bool bVisibleInGame = true;
			bool bCollides = true;
			bool bOverrideThickness = false;
			float Thickness = 0.0f;
			bool bOverrideOffset = false;
			float Offset = 0.0f;
			FLinearColor Color = FLinearColor::White;
			TArray<FPaperTileMapCellState> Cells;
		};

		bool TryReadPaperTileMapLayers(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FUeAgentCanonicalValue& Value,
			const FIntPoint& MapSize,
			TArray<FPaperTileMapLayerState>& OutLayers,
			FExecutionMutationError& OutError)
		{
			OutLayers.Reset();
			if (Value.GetKind() != EUeAgentValueKind::Array)
			{
				return Fail(
					OutError,
					TEXT("asset_data_paper_tilemap_layers_invalid"));
			}
			TSet<FString> SeenNames;
			for (int32 LayerIndex = 0;
				LayerIndex < Value.GetArrayItems().Num();
				++LayerIndex)
			{
				const FUeAgentCanonicalValue& LayerValue =
					Value.GetArrayItems()[LayerIndex].Get();
				if (LayerValue.GetKind()
						!= EUeAgentValueKind::Object
					|| LayerValue.GetObjectItems().Num() != 8)
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_paper_tilemap_"
							"layer_shape_invalid"),
						FString::FromInt(LayerIndex));
				}
				const FUeAgentCanonicalValue* NameValue =
					FindObjectField(LayerValue, TEXT("name"));
				const FUeAgentCanonicalValue* ColorValue =
					FindObjectField(LayerValue, TEXT("color"));
				const FUeAgentCanonicalValue* CellsValue =
					FindObjectField(LayerValue, TEXT("cells"));
				FPaperTileMapLayerState Layer;
				if (!NameValue
					|| NameValue->GetKind()
						!= EUeAgentValueKind::String
					|| NameValue->GetText().TrimStartAndEnd().IsEmpty()
					|| !ColorValue
					|| !CellsValue
					|| CellsValue->GetKind()
						!= EUeAgentValueKind::Array
					|| !TryReadBool(
						LayerValue,
						TEXT("visible_in_editor"),
						Layer.bVisibleInEditor)
					|| !TryReadBool(
						LayerValue,
						TEXT("visible_in_game"),
						Layer.bVisibleInGame)
					|| !TryReadBool(
						LayerValue,
						TEXT("collides"),
						Layer.bCollides)
					|| !TryReadOptionalFloat(
						LayerValue,
						TEXT(
							"collision_thickness_override"),
						true,
						Layer.bOverrideThickness,
						Layer.Thickness)
					|| !TryReadOptionalFloat(
						LayerValue,
						TEXT("collision_offset_override"),
						false,
						Layer.bOverrideOffset,
						Layer.Offset)
					|| !TryReadLinearColor(
						*ColorValue,
						Layer.Color))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_paper_tilemap_"
							"layer_value_invalid"),
						FString::FromInt(LayerIndex));
				}
				Layer.Name = NameValue->GetText();
				FString CanonicalName =
					Layer.Name.TrimStartAndEnd().ToLower();
				if (SeenNames.Contains(CanonicalName))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_paper_tilemap_"
							"layer_name_duplicate"),
						Layer.Name);
				}
				SeenNames.Add(MoveTemp(CanonicalName));

				TSet<uint64> SeenCells;
				for (int32 CellIndex = 0;
					CellIndex < CellsValue->GetArrayItems().Num();
					++CellIndex)
				{
					const FUeAgentCanonicalValue& CellValue =
						CellsValue->GetArrayItems()[CellIndex].Get();
					if (CellValue.GetKind()
							!= EUeAgentValueKind::Object
						|| CellValue.GetObjectItems().Num() != 7)
					{
						return Fail(
							OutError,
							TEXT(
								"asset_data_paper_tilemap_"
								"cell_shape_invalid"),
							FString::Printf(
								TEXT("%d:%d"),
								LayerIndex,
								CellIndex));
					}
					const FUeAgentCanonicalValue* X =
						FindObjectField(CellValue, TEXT("x"));
					const FUeAgentCanonicalValue* Y =
						FindObjectField(CellValue, TEXT("y"));
					const FUeAgentCanonicalValue* TileSetRef =
						FindObjectField(
							CellValue,
							TEXT("tile_set_ref"));
					const FUeAgentCanonicalValue* TileIndexValue =
						FindObjectField(
							CellValue,
							TEXT("tile_index"));
					FPaperTileMapCellState Cell;
					if (!X
						|| !Y
						|| !TileSetRef
						|| !TileIndexValue
						|| !TryReadInt32(*X, Cell.X)
						|| !TryReadInt32(*Y, Cell.Y)
						|| Cell.X < 0
						|| Cell.Y < 0
						|| Cell.X >= MapSize.X
						|| Cell.Y >= MapSize.Y
						|| TileSetRef->GetKind()
							!= EUeAgentValueKind::String
						|| TileSetRef->GetText().IsEmpty()
						|| !TryReadInt32(
							*TileIndexValue,
							Cell.TileIndex)
						|| Cell.TileIndex < 0
						|| Cell.TileIndex
							> static_cast<int32>(
								EPaperTileFlags::TileIndexMask)
						|| !TryReadBool(
							CellValue,
							TEXT("flip_horizontal"),
							Cell.bFlipHorizontal)
						|| !TryReadBool(
							CellValue,
							TEXT("flip_vertical"),
							Cell.bFlipVertical)
						|| !TryReadBool(
							CellValue,
							TEXT("flip_diagonal"),
							Cell.bFlipDiagonal))
					{
						return Fail(
							OutError,
							TEXT(
								"asset_data_paper_tilemap_"
								"cell_value_invalid"),
							FString::Printf(
								TEXT("%d:%d"),
								LayerIndex,
								CellIndex));
					}
					const uint64 CoordinateKey =
						(static_cast<uint64>(
							static_cast<uint32>(Cell.Y))
							<< 32)
						| static_cast<uint32>(Cell.X);
					if (SeenCells.Contains(CoordinateKey))
					{
						return Fail(
							OutError,
							TEXT(
								"asset_data_paper_tilemap_"
								"cell_coordinate_duplicate"),
							FString::Printf(
								TEXT("%d:%d"),
								Cell.X,
								Cell.Y));
					}
					SeenCells.Add(CoordinateKey);
					UObject* Resolved = nullptr;
					if (!Resolver.TryResolveExisting(
							TileSetRef->GetText(),
							Resolved,
							OutError))
					{
						return false;
					}
					UPaperTileSet* TileSet =
						Cast<UPaperTileSet>(Resolved);
					if (!TileSet)
					{
						return Fail(
							OutError,
							TEXT(
								"asset_data_paper_tilemap_"
								"tile_set_class_invalid"),
							TileSetRef->GetText());
					}
					const int32 TileCount =
						TileSet->GetTileCount();
					if (TileCount <= 0
						|| Cell.TileIndex >= TileCount)
					{
						return Fail(
							OutError,
							TEXT(
								"asset_data_paper_tilemap_"
								"tile_index_out_of_range"),
							FString::FromInt(Cell.TileIndex));
					}
					Cell.TileSet = TileSet;
					Layer.Cells.Add(MoveTemp(Cell));
				}
				OutLayers.Add(MoveTemp(Layer));
			}
			return true;
		}

		bool TryWritePaperTileMapLayerState(
			UPaperTileLayer& Layer,
			const FPaperTileMapLayerState& State)
		{
			Layer.LayerName = FText::FromString(State.Name);
			Layer.SetShouldRenderInEditor(State.bVisibleInEditor);
			if (!TrySetExactBoolProperty(
					Layer,
					TEXT("bHiddenInGame"),
					!State.bVisibleInGame))
			{
				return false;
			}
			Layer.SetLayerCollides(State.bCollides);
			Layer.SetLayerCollisionThickness(
				State.bOverrideThickness,
				State.Thickness);
			Layer.SetLayerCollisionOffset(
				State.bOverrideOffset,
				State.Offset);
			Layer.SetLayerColor(State.Color);
			for (const FPaperTileMapCellState& CellState
				: State.Cells)
			{
				UPaperTileSet* TileSet =
					CellState.TileSet.Get();
				if (!TileSet)
				{
					return false;
				}
				FPaperTileInfo Cell;
				Cell.TileSet = TileSet;
				Cell.PackedTileIndex = CellState.TileIndex;
				Cell.SetFlagValue(
					EPaperTileFlags::FlipHorizontal,
					CellState.bFlipHorizontal);
				Cell.SetFlagValue(
					EPaperTileFlags::FlipVertical,
					CellState.bFlipVertical);
				Cell.SetFlagValue(
					EPaperTileFlags::FlipDiagonal,
					CellState.bFlipDiagonal);
				Layer.SetCell(
					CellState.X,
					CellState.Y,
					Cell);
			}
			return true;
		}

		bool ValidateTileSetPrivateBindings()
		{
			const UClass* Class = UPaperTileSet::StaticClass();
			const FArrayProperty* Terrains =
				FindFProperty<FArrayProperty>(
					Class,
					TEXT("Terrains"));
			const FArrayProperty* PerTileData =
				FindFProperty<FArrayProperty>(
					Class,
					TEXT("PerTileData"));
			const FStructProperty* TerrainInner =
				Terrains
					? CastField<FStructProperty>(Terrains->Inner)
					: nullptr;
			const FStructProperty* MetadataInner =
				PerTileData
					? CastField<FStructProperty>(PerTileData->Inner)
					: nullptr;
			return TerrainInner
				&& TerrainInner->Struct
					== FPaperTileSetTerrain::StaticStruct()
				&& MetadataInner
				&& MetadataInner->Struct
					== FPaperTileMetadata::StaticStruct()
				&& FindFProperty<FIntProperty>(
					Class,
					TEXT("WidthInTiles"))
				&& FindFProperty<FIntProperty>(
					Class,
					TEXT("HeightInTiles"))
				&& FindFProperty<FIntProperty>(
					Class,
					TEXT("AllocatedWidth"))
				&& FindFProperty<FIntProperty>(
					Class,
					TEXT("AllocatedHeight"));
		}

		template<typename ElementType>
		bool TryReadExactStructArray(
			const UObject& Object,
			const FName PropertyName,
			UScriptStruct* ExpectedStruct,
			TArray<ElementType>& OutValues)
		{
			const FArrayProperty* Property =
				FindFProperty<FArrayProperty>(
					Object.GetClass(),
					PropertyName);
			const FStructProperty* Inner =
				Property
					? CastField<FStructProperty>(Property->Inner)
					: nullptr;
			const TArray<ElementType>* Values =
				Property
					? Property->ContainerPtrToValuePtr<
						TArray<ElementType>>(&Object)
					: nullptr;
			if (!Inner
				|| Inner->Struct != ExpectedStruct
				|| !Values)
			{
				return false;
			}
			OutValues = *Values;
			return true;
		}

		template<typename ElementType>
		bool TryWriteExactStructArray(
			UObject& Object,
			const FName PropertyName,
			UScriptStruct* ExpectedStruct,
			const TArray<ElementType>& Values)
		{
			FArrayProperty* Property =
				FindFProperty<FArrayProperty>(
					Object.GetClass(),
					PropertyName);
			FStructProperty* Inner =
				Property
					? CastField<FStructProperty>(Property->Inner)
					: nullptr;
			TArray<ElementType>* TargetValues =
				Property
					? Property->ContainerPtrToValuePtr<
						TArray<ElementType>>(&Object)
					: nullptr;
			if (!Inner
				|| Inner->Struct != ExpectedStruct
				|| !TargetValues)
			{
				return false;
			}
			*TargetValues = Values;
			return true;
		}

		bool TryReadExactIntProperty(
			const UObject& Object,
			const FName PropertyName,
			int32& OutValue)
		{
			const FIntProperty* Property =
				FindFProperty<FIntProperty>(
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

		bool TryWriteExactIntProperty(
			UObject& Object,
			const FName PropertyName,
			const int32 Value)
		{
			FIntProperty* Property =
				FindFProperty<FIntProperty>(
					Object.GetClass(),
					PropertyName);
			if (!Property)
			{
				return false;
			}
			Property->SetPropertyValue_InContainer(
				&Object,
				Value);
			return true;
		}

		struct FPaperTileSetPrivateState
		{
			TArray<FPaperTileSetTerrain> Terrains;
			TArray<FPaperTileMetadata> PerTileData;
			int32 WidthInTiles = 0;
			int32 HeightInTiles = 0;
			int32 AllocatedWidth = 0;
			int32 AllocatedHeight = 0;
		};

		bool TryReadPaperTileSetPrivateState(
			const UPaperTileSet& TileSet,
			FPaperTileSetPrivateState& OutState)
		{
			return TryReadExactStructArray(
					TileSet,
					TEXT("Terrains"),
					FPaperTileSetTerrain::StaticStruct(),
					OutState.Terrains)
				&& TryReadExactStructArray(
					TileSet,
					TEXT("PerTileData"),
					FPaperTileMetadata::StaticStruct(),
					OutState.PerTileData)
				&& TryReadExactIntProperty(
					TileSet,
					TEXT("WidthInTiles"),
					OutState.WidthInTiles)
				&& TryReadExactIntProperty(
					TileSet,
					TEXT("HeightInTiles"),
					OutState.HeightInTiles)
				&& TryReadExactIntProperty(
					TileSet,
					TEXT("AllocatedWidth"),
					OutState.AllocatedWidth)
				&& TryReadExactIntProperty(
					TileSet,
					TEXT("AllocatedHeight"),
					OutState.AllocatedHeight);
		}

		bool TryWritePaperTileSetPrivateState(
			UPaperTileSet& TileSet,
			const FPaperTileSetPrivateState& State)
		{
			return TryWriteExactStructArray(
					TileSet,
					TEXT("Terrains"),
					FPaperTileSetTerrain::StaticStruct(),
					State.Terrains)
				&& TryWriteExactStructArray(
					TileSet,
					TEXT("PerTileData"),
					FPaperTileMetadata::StaticStruct(),
					State.PerTileData)
				&& TryWriteExactIntProperty(
					TileSet,
					TEXT("WidthInTiles"),
					State.WidthInTiles)
				&& TryWriteExactIntProperty(
					TileSet,
					TEXT("HeightInTiles"),
					State.HeightInTiles)
				&& TryWriteExactIntProperty(
					TileSet,
					TEXT("AllocatedWidth"),
					State.AllocatedWidth)
				&& TryWriteExactIntProperty(
					TileSet,
					TEXT("AllocatedHeight"),
					State.AllocatedHeight);
		}

		bool TryReadPaperTileSetTerrains(
			const FUeAgentCanonicalValue& Value,
			TArray<FPaperTileSetTerrain>& OutTerrains,
			FExecutionMutationError& OutError)
		{
			OutTerrains.Reset();
			if (Value.GetKind() != EUeAgentValueKind::Array
				|| Value.GetArrayItems().Num() > 254)
			{
				return Fail(
					OutError,
					TEXT("asset_data_paper_tileset_terrain_invalid"));
			}
			TSet<FString> SeenNames;
			for (int32 Index = 0;
				Index < Value.GetArrayItems().Num();
				++Index)
			{
				const FUeAgentCanonicalValue& TerrainValue =
					Value.GetArrayItems()[Index].Get();
				if (TerrainValue.GetKind()
						!= EUeAgentValueKind::Object
					|| TerrainValue.GetObjectItems().Num() != 2)
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_paper_tileset_"
							"terrain_shape_invalid"),
						FString::FromInt(Index));
				}
				const FUeAgentCanonicalValue* Name =
					FindObjectField(TerrainValue, TEXT("name"));
				const FUeAgentCanonicalValue* Center =
					FindObjectField(
						TerrainValue,
						TEXT("center_tile_index"));
				FPaperTileSetTerrain Terrain;
				if (!Name
					|| Name->GetKind()
						!= EUeAgentValueKind::String
					|| Name->GetText().TrimStartAndEnd().IsEmpty()
					|| !Center
					|| !TryReadInt32(
						*Center,
						Terrain.CenterTileIndex)
					|| Terrain.CenterTileIndex < 0)
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_paper_tileset_"
							"terrain_value_invalid"),
						FString::FromInt(Index));
				}
				Terrain.TerrainName = Name->GetText();
				FString CanonicalName =
					Terrain.TerrainName
						.TrimStartAndEnd()
						.ToLower();
				if (SeenNames.Contains(CanonicalName))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_paper_tileset_"
							"terrain_name_duplicate"),
						Terrain.TerrainName);
				}
				SeenNames.Add(MoveTemp(CanonicalName));
				OutTerrains.Add(MoveTemp(Terrain));
			}
			return true;
		}

		bool TryCalculateTileSetLayout(
			const UTexture2D* Texture,
			const FIntPoint& TileSize,
			const FIntMargin& Margin,
			const FIntPoint& Spacing,
			FIntPoint& OutCount,
			FExecutionMutationError& OutError)
		{
			if (!Texture)
			{
				OutCount = FIntPoint::ZeroValue;
				return true;
			}
			const FIntPoint ImportedSize =
				Texture->GetImportedSize();
			const int64 NumeratorX =
				static_cast<int64>(ImportedSize.X)
				- Margin.Left
				- Margin.Right
				+ Spacing.X;
			const int64 NumeratorY =
				static_cast<int64>(ImportedSize.Y)
				- Margin.Top
				- Margin.Bottom
				+ Spacing.Y;
			const int64 DenominatorX =
				static_cast<int64>(TileSize.X) + Spacing.X;
			const int64 DenominatorY =
				static_cast<int64>(TileSize.Y) + Spacing.Y;
			if (ImportedSize.X <= 0
				|| ImportedSize.Y <= 0
				|| TileSize.X <= 0
				|| TileSize.Y <= 0
				|| Margin.Left < 0
				|| Margin.Top < 0
				|| Margin.Right < 0
				|| Margin.Bottom < 0
				|| Spacing.X < 0
				|| Spacing.Y < 0
				|| NumeratorX <= 0
				|| NumeratorY <= 0
				|| DenominatorX <= 0
				|| DenominatorY <= 0)
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_paper_tileset_"
						"layout_invalid"));
			}
			const int64 CountX = NumeratorX / DenominatorX;
			const int64 CountY = NumeratorY / DenominatorY;
			if (CountX <= 0
				|| CountY <= 0
				|| CountX > MAX_int32
				|| CountY > MAX_int32
				|| CountX * CountY > MAX_int32)
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_paper_tileset_"
						"tile_count_invalid"));
			}
			OutCount = FIntPoint(
				static_cast<int32>(CountX),
				static_cast<int32>(CountY));
			return true;
		}
	}

	struct FPaperTileMapPreparedData
	{
		FIntPoint OriginalMapSize = FIntPoint::ZeroValue;
		FIntPoint OriginalTileSize = FIntPoint::ZeroValue;
		TArray<TStrongObjectPtr<UPaperTileLayer>> OriginalLayers;
		FIntPoint NewMapSize = FIntPoint::ZeroValue;
		FIntPoint NewTileSize = FIntPoint::ZeroValue;
		bool bChangeMapSize = false;
		bool bChangeTileSize = false;
		bool bChangeLayers = false;
		TArray<FPaperTileMapLayerState> NewLayers;
	};

	struct FPaperTileSetPreparedData
	{
		TStrongObjectPtr<UTexture2D> OriginalTexture;
		FIntPoint OriginalTileSize = FIntPoint::ZeroValue;
		FIntMargin OriginalMargin = FIntMargin(0);
		FIntPoint OriginalSpacing = FIntPoint::ZeroValue;
		FPaperTileSetPrivateState OriginalPrivateState;

		TStrongObjectPtr<UTexture2D> NewTexture;
		FIntPoint NewTileSize = FIntPoint::ZeroValue;
		FIntMargin NewMargin = FIntMargin(0);
		FIntPoint NewSpacing = FIntPoint::ZeroValue;
		TArray<FPaperTileSetTerrain> NewTerrains;
		bool bChangeTerrain = false;
	};

	bool TryPreparePaperTileMapReplacement(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		UObject* Target,
		const FUeAgentCanonicalValue* MapSize,
		const FUeAgentCanonicalValue* TileSize,
		const FUeAgentCanonicalValue* Layers,
		FPaperTileMapPreparedState& OutState,
		FExecutionMutationError& OutError)
	{
		OutState.Data.Reset();
		UPaperTileMap* TileMap = Cast<UPaperTileMap>(Target);
		if (!TileMap
			|| TileMap->MapWidth <= 0
			|| TileMap->MapHeight <= 0
			|| TileMap->TileWidth <= 0
			|| TileMap->TileHeight <= 0)
		{
			return Fail(
				OutError,
				TEXT("asset_data_paper_tilemap_invalid"));
		}
		const TSharedRef<
			FPaperTileMapPreparedData,
			ESPMode::ThreadSafe> Data =
				MakeShared<
					FPaperTileMapPreparedData,
					ESPMode::ThreadSafe>();
		Data->OriginalMapSize =
			FIntPoint(TileMap->MapWidth, TileMap->MapHeight);
		Data->OriginalTileSize =
			FIntPoint(TileMap->TileWidth, TileMap->TileHeight);
		Data->NewMapSize = Data->OriginalMapSize;
		Data->NewTileSize = Data->OriginalTileSize;
		for (UPaperTileLayer* Layer : TileMap->TileLayers)
		{
			if (!Layer)
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_paper_tilemap_"
						"existing_layer_invalid"));
			}
			Data->OriginalLayers.Emplace(Layer);
		}
		if (MapSize)
		{
			Data->bChangeMapSize = true;
			if (!TryReadExtent2i(
					*MapSize,
					1,
					1024,
					Data->NewMapSize))
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_paper_tilemap_"
						"map_size_invalid"));
			}
		}
		if (TileSize)
		{
			Data->bChangeTileSize = true;
			if (!TryReadExtent2i(
					*TileSize,
					1,
					MAX_int32,
					Data->NewTileSize))
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_paper_tilemap_"
						"tile_size_invalid"));
			}
		}
		if (Layers)
		{
			Data->bChangeLayers = true;
			if (!ValidateLayerPrivateBindings())
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_paper_tilemap_"
						"private_binding_invalid"));
			}
			if (!TryReadPaperTileMapLayers(
					Resolver,
					*Layers,
					Data->NewMapSize,
					Data->NewLayers,
					OutError))
			{
				return false;
			}
		}
		OutState.Data = Data;
		return true;
	}

	bool TryApplyPaperTileMapReplacement(
		UObject* Target,
		const FPaperTileMapPreparedState& State,
		FExecutionMutationError& OutError)
	{
		UPaperTileMap* TileMap = Cast<UPaperTileMap>(Target);
		if (!TileMap || !State.Data.IsValid())
		{
			return Fail(
				OutError,
				TEXT("asset_data_paper_tilemap_state_invalid"));
		}
		const FPaperTileMapPreparedData& Data = *State.Data;
		TArray<TObjectPtr<UPaperTileLayer>> ReplacementLayers;
		if (Data.bChangeLayers)
		{
			ReplacementLayers.Reserve(Data.NewLayers.Num());
			for (const FPaperTileMapLayerState& LayerState
				: Data.NewLayers)
			{
				UPaperTileLayer* Layer =
					NewObject<UPaperTileLayer>(
						TileMap,
						NAME_None,
						RF_Transactional);
				if (!Layer)
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_paper_tilemap_"
							"layer_create_failed"));
				}
				Layer->DestructiveAllocateMap(
					Data.NewMapSize.X,
					Data.NewMapSize.Y);
				if (!TryWritePaperTileMapLayerState(
						*Layer,
						LayerState))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_paper_tilemap_"
							"layer_write_failed"));
				}
				ReplacementLayers.Add(Layer);
			}
		}
		else if (Data.bChangeMapSize)
		{
			ReplacementLayers.Reserve(
				Data.OriginalLayers.Num());
			for (const TStrongObjectPtr<UPaperTileLayer>&
				OriginalLayer : Data.OriginalLayers)
			{
				UPaperTileLayer* Layer = DuplicateObject<
					UPaperTileLayer>(
						OriginalLayer.Get(),
						TileMap);
				if (!Layer)
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_paper_tilemap_"
							"layer_clone_failed"));
				}
				Layer->ResizeMap(
					Data.NewMapSize.X,
					Data.NewMapSize.Y);
				ReplacementLayers.Add(Layer);
			}
		}

		TileMap->Modify();
		TileMap->MapWidth = Data.NewMapSize.X;
		TileMap->MapHeight = Data.NewMapSize.Y;
		TileMap->TileWidth = Data.NewTileSize.X;
		TileMap->TileHeight = Data.NewTileSize.Y;
		if (Data.bChangeLayers || Data.bChangeMapSize)
		{
			TileMap->TileLayers = MoveTemp(ReplacementLayers);
		}
		TileMap->PostEditChange();
		return true;
	}

	bool TryRollbackPaperTileMapReplacement(
		UObject* Target,
		const FPaperTileMapPreparedState& State)
	{
		UPaperTileMap* TileMap = Cast<UPaperTileMap>(Target);
		if (!TileMap || !State.Data.IsValid())
		{
			return false;
		}
		const FPaperTileMapPreparedData& Data = *State.Data;
		TArray<TObjectPtr<UPaperTileLayer>> OriginalLayers;
		OriginalLayers.Reserve(Data.OriginalLayers.Num());
		for (const TStrongObjectPtr<UPaperTileLayer>& Layer
			: Data.OriginalLayers)
		{
			if (!Layer.Get())
			{
				return false;
			}
			OriginalLayers.Add(Layer.Get());
		}
		TileMap->Modify();
		TileMap->MapWidth = Data.OriginalMapSize.X;
		TileMap->MapHeight = Data.OriginalMapSize.Y;
		TileMap->TileWidth = Data.OriginalTileSize.X;
		TileMap->TileHeight = Data.OriginalTileSize.Y;
		TileMap->TileLayers = MoveTemp(OriginalLayers);
		TileMap->PostEditChange();
		return true;
	}

	bool TryPreparePaperTileSetReplacement(
		IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
		UObject* Target,
		const FUeAgentCanonicalValue* SourceTextureRef,
		const FUeAgentCanonicalValue* TileSize,
		const FUeAgentCanonicalValue* Margin,
		const FUeAgentCanonicalValue* Spacing,
		const FUeAgentCanonicalValue* Terrain,
		FPaperTileSetPreparedState& OutState,
		FExecutionMutationError& OutError)
	{
		OutState.Data.Reset();
		UPaperTileSet* TileSet = Cast<UPaperTileSet>(Target);
		if (!TileSet || !ValidateTileSetPrivateBindings())
		{
			return Fail(
				OutError,
				TEXT("asset_data_paper_tileset_invalid"));
		}
		const TSharedRef<
			FPaperTileSetPreparedData,
			ESPMode::ThreadSafe> Data =
				MakeShared<
					FPaperTileSetPreparedData,
					ESPMode::ThreadSafe>();
		Data->OriginalTexture =
			TStrongObjectPtr<UTexture2D>(
				TileSet->GetTileSheetTexture());
		Data->OriginalTileSize = TileSet->GetTileSize();
		Data->OriginalMargin = TileSet->GetMargin();
		Data->OriginalSpacing =
			TileSet->GetPerTileSpacing();
		if (!TryReadPaperTileSetPrivateState(
				*TileSet,
				Data->OriginalPrivateState))
		{
			return Fail(
				OutError,
				TEXT(
					"asset_data_paper_tileset_"
					"private_state_invalid"));
		}
		Data->NewTexture =
			TStrongObjectPtr<UTexture2D>(
				TileSet->GetTileSheetTexture());
		Data->NewTileSize = Data->OriginalTileSize;
		Data->NewMargin = Data->OriginalMargin;
		Data->NewSpacing = Data->OriginalSpacing;
		Data->NewTerrains =
			Data->OriginalPrivateState.Terrains;

		if (SourceTextureRef)
		{
			if (SourceTextureRef->GetKind()
					!= EUeAgentValueKind::String
				|| SourceTextureRef->GetText().IsEmpty())
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_paper_tileset_"
						"source_ref_invalid"));
			}
			UObject* Resolved = nullptr;
			if (!Resolver.TryResolveExisting(
					SourceTextureRef->GetText(),
					Resolved,
					OutError))
			{
				return false;
			}
			UTexture2D* Texture = Cast<UTexture2D>(Resolved);
			if (!Texture)
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_paper_tileset_"
						"source_class_invalid"),
					SourceTextureRef->GetText());
			}
			Data->NewTexture =
				TStrongObjectPtr<UTexture2D>(Texture);
		}
		if (TileSize
			&& !TryReadExtent2i(
				*TileSize,
				1,
				MAX_int32,
				Data->NewTileSize))
		{
			return Fail(
				OutError,
				TEXT(
					"asset_data_paper_tileset_"
					"tile_size_invalid"));
		}
		if (Margin)
		{
			int32 UniformMargin = 0;
			if (!TryReadInt32(*Margin, UniformMargin)
				|| UniformMargin < 0)
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_paper_tileset_"
						"margin_invalid"));
			}
			Data->NewMargin = FIntMargin(UniformMargin);
		}
		if (Spacing)
		{
			int32 UniformSpacing = 0;
			if (!TryReadInt32(*Spacing, UniformSpacing)
				|| UniformSpacing < 0)
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_paper_tileset_"
						"spacing_invalid"));
			}
			Data->NewSpacing =
				FIntPoint(
					UniformSpacing,
					UniformSpacing);
		}
		if (Terrain)
		{
			Data->bChangeTerrain = true;
			if (!TryReadPaperTileSetTerrains(
					*Terrain,
					Data->NewTerrains,
					OutError))
			{
				return false;
			}
		}
		FIntPoint TileCount;
		if (!TryCalculateTileSetLayout(
				Data->NewTexture.Get(),
				Data->NewTileSize,
				Data->NewMargin,
				Data->NewSpacing,
				TileCount,
				OutError))
		{
			return false;
		}
		const int64 TotalTiles =
			static_cast<int64>(TileCount.X) * TileCount.Y;
		for (const FPaperTileSetTerrain& TerrainState
			: Data->NewTerrains)
		{
			if (TerrainState.CenterTileIndex < 0
				|| TerrainState.CenterTileIndex >= TotalTiles)
			{
				return Fail(
					OutError,
					TEXT(
						"asset_data_paper_tileset_"
						"terrain_center_out_of_range"),
					TerrainState.TerrainName);
			}
		}
		OutState.Data = Data;
		return true;
	}

	bool TryApplyPaperTileSetReplacement(
		UObject* Target,
		const FPaperTileSetPreparedState& State,
		FExecutionMutationError& OutError)
	{
		UPaperTileSet* TileSet = Cast<UPaperTileSet>(Target);
		if (!TileSet || !State.Data.IsValid())
		{
			return Fail(
				OutError,
				TEXT("asset_data_paper_tileset_state_invalid"));
		}
		const FPaperTileSetPreparedData& Data = *State.Data;
		TileSet->Modify();
		TileSet->SetTileSheetTexture(Data.NewTexture.Get());
		TileSet->SetTileSize(Data.NewTileSize);
		TileSet->SetMargin(Data.NewMargin);
		TileSet->SetPerTileSpacing(Data.NewSpacing);
		if (!TryWriteExactStructArray(
				*TileSet,
				TEXT("Terrains"),
				FPaperTileSetTerrain::StaticStruct(),
				Data.NewTerrains))
		{
			return Fail(
				OutError,
				TEXT(
					"asset_data_paper_tileset_"
					"terrain_write_failed"));
		}
		TileSet->PostEditChange();
		return true;
	}

	bool TryRollbackPaperTileSetReplacement(
		UObject* Target,
		const FPaperTileSetPreparedState& State)
	{
		UPaperTileSet* TileSet = Cast<UPaperTileSet>(Target);
		if (!TileSet || !State.Data.IsValid())
		{
			return false;
		}
		const FPaperTileSetPreparedData& Data = *State.Data;
		TileSet->Modify();
		TileSet->SetTileSheetTexture(
			Data.OriginalTexture.Get());
		TileSet->SetTileSize(Data.OriginalTileSize);
		TileSet->SetMargin(Data.OriginalMargin);
		TileSet->SetPerTileSpacing(Data.OriginalSpacing);
		if (!TryWriteExactStructArray(
				*TileSet,
				TEXT("Terrains"),
				FPaperTileSetTerrain::StaticStruct(),
				Data.OriginalPrivateState.Terrains))
		{
			return false;
		}
		TileSet->PostEditChange();
		return TryWritePaperTileSetPrivateState(
			*TileSet,
			Data.OriginalPrivateState);
	}
}
