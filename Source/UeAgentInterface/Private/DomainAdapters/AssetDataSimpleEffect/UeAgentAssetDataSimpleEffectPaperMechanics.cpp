#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectPaperMechanics.h"

#include "Engine/Texture2D.h"
#include "PaperFlipbook.h"
#include "PaperSprite.h"
#include "PaperTileLayer.h"
#include "PaperTileMap.h"
#include "PaperTileSet.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::DomainAdapters::AssetDataSimpleEffect
{
	using namespace ContextResource;
	using namespace ExecutionMutation;
	using namespace StateSemantic;

	namespace
	{
		const TCHAR* PaperFlipbookMechanicId =
			TEXT("uai.asset_data.mechanic:replace_paper_flipbook:v1");
		const TCHAR* PaperSpriteMechanicId =
			TEXT("uai.asset_data.mechanic:update_paper_sprite:v1");
		const TCHAR* PaperTileMapMechanicId =
			TEXT("uai.asset_data.mechanic:replace_paper_tilemap:v1");
		const TCHAR* PaperTileSetMechanicId =
			TEXT("uai.asset_data.mechanic:replace_paper_tileset:v1");

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

		bool TryBuildString(
			const FString& Text,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError StateError;
			return FUeAgentCanonicalValue::TryString(
					Text,
					Resources,
					OutValue,
					StateError)
				? true
				: Fail(
					OutError,
					StateError.Code.IsEmpty()
						? TEXT("asset_data_paper_projection_string_invalid")
						: StateError.Code,
					StateError.Detail);
		}

		bool TryBuildBoolean(
			const bool bValue,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError StateError;
			return FUeAgentCanonicalValue::TryBoolean(
					bValue,
					Resources,
					OutValue,
					StateError)
				? true
				: Fail(
					OutError,
					StateError.Code.IsEmpty()
						? TEXT("asset_data_paper_projection_boolean_invalid")
						: StateError.Code,
					StateError.Detail);
		}

		bool TryBuildInteger(
			const int32 Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError StateError;
			return FUeAgentCanonicalValue::TryInteger(
					FString::FromInt(Value),
					Resources,
					OutValue,
					StateError)
				? true
				: Fail(
					OutError,
					StateError.Code.IsEmpty()
						? TEXT("asset_data_paper_projection_integer_invalid")
						: StateError.Code,
					StateError.Detail);
		}

		bool TryBuildDecimal(
			const double Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError StateError;
			return FMath::IsFinite(Value)
				&& FUeAgentCanonicalValue::TryDecimal(
					FString::SanitizeFloat(Value),
					Resources,
					OutValue,
					StateError)
				? true
				: Fail(
					OutError,
					StateError.Code.IsEmpty()
						? TEXT("asset_data_paper_projection_decimal_invalid")
						: StateError.Code,
					StateError.Detail);
		}

		bool TryBuildNull(
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError StateError;
			return FUeAgentCanonicalValue::TryNull(
					Resources,
					OutValue,
					StateError)
				? true
				: Fail(
					OutError,
					StateError.Code.IsEmpty()
						? TEXT("asset_data_paper_projection_null_invalid")
						: StateError.Code,
					StateError.Detail);
		}

		bool TryBuildArray(
			TArray<FUeAgentCanonicalValueRef> Items,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError StateError;
			return FUeAgentCanonicalValue::TryArray(
					Items,
					Resources,
					OutValue,
					StateError)
				? true
				: Fail(
					OutError,
					StateError.Code.IsEmpty()
						? TEXT("asset_data_paper_projection_array_invalid")
						: StateError.Code,
					StateError.Detail);
		}

		bool TryBuildObject(
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			Items.Sort(
				[](const TPair<FString, FUeAgentCanonicalValueRef>& Left,
					const TPair<FString, FUeAgentCanonicalValueRef>& Right)
				{
					return Left.Key < Right.Key;
				});
			FUeAgentStateSemanticError StateError;
			return FUeAgentCanonicalValue::TryObject(
					Items,
					Resources,
					OutValue,
					StateError)
				? true
				: Fail(
					OutError,
					StateError.Code.IsEmpty()
						? TEXT("asset_data_paper_projection_object_invalid")
						: StateError.Code,
					StateError.Detail);
		}

		bool TryBuildReference(
			const UObject* Object,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			return Object
				? TryBuildString(
					Object->GetPathName(),
					Resources,
					OutValue,
					OutError)
				: TryBuildNull(Resources, OutValue, OutError);
		}

		bool TryReadString(
			const FUeAgentCanonicalValue& Value,
			FString& OutText)
		{
			if (Value.GetKind() != EUeAgentValueKind::String
				|| Value.GetText().IsEmpty())
			{
				OutText.Reset();
				return false;
			}
			OutText = Value.GetText();
			return true;
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
			return LexTryParseString(OutNumber, *Value.GetLexeme())
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

		bool TryReadBooleanField(
			const FUeAgentCanonicalValue& Object,
			const FString& Field,
			bool& bOutValue)
		{
			const FUeAgentCanonicalValue* Value =
				FindObjectField(Object, Field);
			if (!Value
				|| Value->GetKind() != EUeAgentValueKind::Boolean)
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

		bool TryBuildExtent2i(
			const FIntPoint& Extent,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentCanonicalValuePtr X;
			FUeAgentCanonicalValuePtr Y;
			return TryBuildInteger(Extent.X, Resources, X, OutError)
				&& TryBuildInteger(Extent.Y, Resources, Y, OutError)
				&& TryBuildObject(
					{
						{TEXT("x"), X.ToSharedRef()},
						{TEXT("y"), Y.ToSharedRef()},
					},
					Resources,
					OutValue,
					OutError);
		}

		bool TryBuildRect2d(
			const FIntPoint& Offset,
			const FIntPoint& Size,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentCanonicalValuePtr X;
			FUeAgentCanonicalValuePtr Y;
			FUeAgentCanonicalValuePtr Width;
			FUeAgentCanonicalValuePtr Height;
			return TryBuildInteger(Offset.X, Resources, X, OutError)
				&& TryBuildInteger(Offset.Y, Resources, Y, OutError)
				&& TryBuildInteger(Size.X, Resources, Width, OutError)
				&& TryBuildInteger(Size.Y, Resources, Height, OutError)
				&& TryBuildObject(
					{
						{TEXT("height"), Height.ToSharedRef()},
						{TEXT("width"), Width.ToSharedRef()},
						{TEXT("x"), X.ToSharedRef()},
						{TEXT("y"), Y.ToSharedRef()},
					},
					Resources,
					OutValue,
					OutError);
		}

		bool TryReadRect2d(
			const FUeAgentCanonicalValue& Value,
			FIntPoint& OutOffset,
			FIntPoint& OutSize)
		{
			if (Value.GetKind() != EUeAgentValueKind::Object
				|| Value.GetObjectItems().Num() != 4)
			{
				return false;
			}
			const FUeAgentCanonicalValue* X =
				FindObjectField(Value, TEXT("x"));
			const FUeAgentCanonicalValue* Y =
				FindObjectField(Value, TEXT("y"));
			const FUeAgentCanonicalValue* Width =
				FindObjectField(Value, TEXT("width"));
			const FUeAgentCanonicalValue* Height =
				FindObjectField(Value, TEXT("height"));
			return X
				&& Y
				&& Width
				&& Height
				&& TryReadInt32(*X, OutOffset.X)
				&& TryReadInt32(*Y, OutOffset.Y)
				&& TryReadInt32(*Width, OutSize.X)
				&& TryReadInt32(*Height, OutSize.Y)
				&& OutOffset.X >= 0
				&& OutOffset.Y >= 0
				&& OutSize.X > 0
				&& OutSize.Y > 0;
		}

		bool TryBuildLinearColor(
			const FLinearColor& Color,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentCanonicalValuePtr R;
			FUeAgentCanonicalValuePtr G;
			FUeAgentCanonicalValuePtr B;
			FUeAgentCanonicalValuePtr A;
			return TryBuildDecimal(Color.R, Resources, R, OutError)
				&& TryBuildDecimal(Color.G, Resources, G, OutError)
				&& TryBuildDecimal(Color.B, Resources, B, OutError)
				&& TryBuildDecimal(Color.A, Resources, A, OutError)
				&& TryBuildObject(
					{
						{TEXT("a"), A.ToSharedRef()},
						{TEXT("b"), B.ToSharedRef()},
						{TEXT("g"), G.ToSharedRef()},
						{TEXT("r"), R.ToSharedRef()},
					},
					Resources,
					OutValue,
					OutError);
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
			double R = 0.0;
			double G = 0.0;
			double B = 0.0;
			double A = 0.0;
			const FUeAgentCanonicalValue* RV = FindObjectField(Value, TEXT("r"));
			const FUeAgentCanonicalValue* GV = FindObjectField(Value, TEXT("g"));
			const FUeAgentCanonicalValue* BV = FindObjectField(Value, TEXT("b"));
			const FUeAgentCanonicalValue* AV = FindObjectField(Value, TEXT("a"));
			if (!RV || !GV || !BV || !AV
				|| !TryReadNumber(*RV, R)
				|| !TryReadNumber(*GV, G)
				|| !TryReadNumber(*BV, B)
				|| !TryReadNumber(*AV, A)
				|| R < -MAX_flt || R > MAX_flt
				|| G < -MAX_flt || G > MAX_flt
				|| B < -MAX_flt || B > MAX_flt
				|| A < -MAX_flt || A > MAX_flt)
			{
				return false;
			}
			OutColor = FLinearColor(
				static_cast<float>(R),
				static_cast<float>(G),
				static_cast<float>(B),
				static_cast<float>(A));
			return true;
		}

		bool TryReadExactBoolProperty(
			const UObject& Object,
			const FName PropertyName,
			bool& bOutValue)
		{
			const FBoolProperty* Property = FindFProperty<FBoolProperty>(
				Object.GetClass(),
				PropertyName);
			if (!Property)
			{
				return false;
			}
			bOutValue = Property->GetPropertyValue_InContainer(&Object);
			return true;
		}

		bool TryReadExactFloatProperty(
			const UObject& Object,
			const FName PropertyName,
			float& OutValue)
		{
			const FFloatProperty* Property = FindFProperty<FFloatProperty>(
				Object.GetClass(),
				PropertyName);
			if (!Property)
			{
				return false;
			}
			OutValue = Property->GetPropertyValue_InContainer(&Object);
			return FMath::IsFinite(OutValue);
		}

		bool TryBuildPaperFlipbookFrames(
			const UPaperFlipbook& Flipbook,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			TArray<FUeAgentCanonicalValueRef> Frames;
			Frames.Reserve(Flipbook.GetNumKeyFrames());
			int64 TotalFrames = 0;
			for (int32 Index = 0; Index < Flipbook.GetNumKeyFrames(); ++Index)
			{
				const FPaperFlipbookKeyFrame& Frame =
					Flipbook.GetKeyFrameChecked(Index);
				TotalFrames += Frame.FrameRun;
				if (Frame.FrameRun < 1 || TotalFrames > MAX_int32)
				{
					return Fail(
						OutError,
						TEXT("asset_data_paper_flipbook_existing_frame_invalid"),
						FString::FromInt(Index));
				}
				FUeAgentCanonicalValuePtr Sprite;
				FUeAgentCanonicalValuePtr FrameRun;
				FUeAgentCanonicalValuePtr FrameValue;
				if (!TryBuildReference(
						Frame.Sprite,
						Resources,
						Sprite,
						OutError)
					|| !TryBuildInteger(
						Frame.FrameRun,
						Resources,
						FrameRun,
						OutError)
					|| !TryBuildObject(
						{
							{TEXT("frame_run"), FrameRun.ToSharedRef()},
							{TEXT("sprite_ref"), Sprite.ToSharedRef()},
						},
						Resources,
						FrameValue,
						OutError))
				{
					return false;
				}
				Frames.Add(FrameValue.ToSharedRef());
			}
			return TryBuildArray(
				MoveTemp(Frames),
				Resources,
				OutValue,
				OutError);
		}

		bool TryBuildPaperFlipbookProjectionMap(
			const UPaperFlipbook* Flipbook,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutValues,
			FExecutionMutationError& OutError)
		{
			OutValues.Reset();
			if (!Flipbook
				|| !FMath::IsFinite(Flipbook->GetFramesPerSecond())
				|| Flipbook->GetFramesPerSecond() < 0.0f)
			{
				return Fail(
					OutError,
					TEXT("asset_data_paper_flipbook_projection_invalid"));
			}
			FUeAgentCanonicalValuePtr Frames;
			FUeAgentCanonicalValuePtr FramesPerSecond;
			if (!TryBuildPaperFlipbookFrames(
					*Flipbook,
					Resources,
					Frames,
					OutError)
				|| !TryBuildDecimal(
					Flipbook->GetFramesPerSecond(),
					Resources,
					FramesPerSecond,
					OutError))
			{
				return false;
			}
			OutValues.Add(TEXT("frames"), Frames.ToSharedRef());
			OutValues.Add(
				TEXT("frames_per_second"),
				FramesPerSecond.ToSharedRef());
			return true;
		}

		bool TryReadExistingSpriteRegion(
			const UPaperSprite& Sprite,
			FIntPoint& OutOffset,
			FIntPoint& OutSize)
		{
			const FVector2D UV = Sprite.GetSourceUV();
			const FVector2D Size = Sprite.GetSourceSize();
			if (!FMath::IsFinite(UV.X)
				|| !FMath::IsFinite(UV.Y)
				|| !FMath::IsFinite(Size.X)
				|| !FMath::IsFinite(Size.Y)
				|| FMath::FloorToDouble(UV.X) != UV.X
				|| FMath::FloorToDouble(UV.Y) != UV.Y
				|| FMath::FloorToDouble(Size.X) != Size.X
				|| FMath::FloorToDouble(Size.Y) != Size.Y
				|| UV.X < MIN_int32 || UV.X > MAX_int32
				|| UV.Y < MIN_int32 || UV.Y > MAX_int32
				|| Size.X < MIN_int32 || Size.X > MAX_int32
				|| Size.Y < MIN_int32 || Size.Y > MAX_int32)
			{
				return false;
			}
			OutOffset = FIntPoint(
				static_cast<int32>(UV.X),
				static_cast<int32>(UV.Y));
			OutSize = FIntPoint(
				static_cast<int32>(Size.X),
				static_cast<int32>(Size.Y));
			return true;
		}

		bool TryBuildPaperSpritePivotState(
			const ESpritePivotMode::Type Mode,
			const FVector2D& Pivot,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentCanonicalValuePtr ModeValue;
			FUeAgentCanonicalValuePtr X;
			FUeAgentCanonicalValuePtr Y;
			return TryBuildInteger(
					static_cast<int32>(Mode),
					Resources,
					ModeValue,
					OutError)
				&& TryBuildDecimal(Pivot.X, Resources, X, OutError)
				&& TryBuildDecimal(Pivot.Y, Resources, Y, OutError)
				&& TryBuildObject(
					{
						{TEXT("mode"), ModeValue.ToSharedRef()},
						{TEXT("x"), X.ToSharedRef()},
						{TEXT("y"), Y.ToSharedRef()},
					},
					Resources,
					OutValue,
					OutError);
		}

		bool TryBuildPaperSpriteProjectionMap(
			const UPaperSprite* Sprite,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutValues,
			FExecutionMutationError& OutError)
		{
			OutValues.Reset();
			if (!Sprite)
			{
				return Fail(
					OutError,
					TEXT("asset_data_paper_sprite_projection_invalid"));
			}
			FIntPoint Offset;
			FIntPoint Size;
			FVector2D CustomPivot;
			const ESpritePivotMode::Type PivotMode =
				Sprite->GetPivotMode(CustomPivot);
			FUeAgentCanonicalValuePtr Source;
			FUeAgentCanonicalValuePtr Region;
			FUeAgentCanonicalValuePtr Pivot;
			if (!TryReadExistingSpriteRegion(*Sprite, Offset, Size)
				|| !TryBuildReference(
					Sprite->GetSourceTexture(),
					Resources,
					Source,
					OutError)
				|| !TryBuildRect2d(
					Offset,
					Size,
					Resources,
					Region,
					OutError)
				|| !TryBuildPaperSpritePivotState(
					PivotMode,
					CustomPivot,
					Resources,
					Pivot,
					OutError))
			{
				return false;
			}
			OutValues.Add(TEXT("source_texture_ref"), Source.ToSharedRef());
			OutValues.Add(TEXT("source_region"), Region.ToSharedRef());
			OutValues.Add(TEXT("pivot"), Pivot.ToSharedRef());
			return true;
		}

		bool TryBuildPaperTileMapCell(
			const int32 X,
			const int32 Y,
			const FPaperTileInfo& Cell,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			if (!Cell.IsValid() || !Cell.TileSet)
			{
				return Fail(
					OutError,
					TEXT("asset_data_paper_tilemap_existing_cell_invalid"));
			}
			FUeAgentCanonicalValuePtr XValue;
			FUeAgentCanonicalValuePtr YValue;
			FUeAgentCanonicalValuePtr TileSet;
			FUeAgentCanonicalValuePtr TileIndex;
			FUeAgentCanonicalValuePtr FlipHorizontal;
			FUeAgentCanonicalValuePtr FlipVertical;
			FUeAgentCanonicalValuePtr FlipDiagonal;
			return TryBuildInteger(X, Resources, XValue, OutError)
				&& TryBuildInteger(Y, Resources, YValue, OutError)
				&& TryBuildString(
					Cell.TileSet->GetPathName(),
					Resources,
					TileSet,
					OutError)
				&& TryBuildInteger(
					Cell.GetTileIndex(),
					Resources,
					TileIndex,
					OutError)
				&& TryBuildBoolean(
					Cell.HasFlag(EPaperTileFlags::FlipHorizontal),
					Resources,
					FlipHorizontal,
					OutError)
				&& TryBuildBoolean(
					Cell.HasFlag(EPaperTileFlags::FlipVertical),
					Resources,
					FlipVertical,
					OutError)
				&& TryBuildBoolean(
					Cell.HasFlag(EPaperTileFlags::FlipDiagonal),
					Resources,
					FlipDiagonal,
					OutError)
				&& TryBuildObject(
					{
						{TEXT("flip_diagonal"), FlipDiagonal.ToSharedRef()},
						{TEXT("flip_horizontal"), FlipHorizontal.ToSharedRef()},
						{TEXT("flip_vertical"), FlipVertical.ToSharedRef()},
						{TEXT("tile_index"), TileIndex.ToSharedRef()},
						{TEXT("tile_set_ref"), TileSet.ToSharedRef()},
						{TEXT("x"), XValue.ToSharedRef()},
						{TEXT("y"), YValue.ToSharedRef()},
					},
					Resources,
					OutValue,
					OutError);
		}

		bool TryBuildPaperTileMapLayer(
			const UPaperTileLayer& Layer,
			const FIntPoint& MapSize,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			bool bHiddenInGame = false;
			bool bOverrideThickness = false;
			bool bOverrideOffset = false;
			float Thickness = 0.0f;
			float Offset = 0.0f;
			if (!TryReadExactBoolProperty(
					Layer,
					TEXT("bHiddenInGame"),
					bHiddenInGame)
				|| !TryReadExactBoolProperty(
					Layer,
					TEXT("bOverrideCollisionThickness"),
					bOverrideThickness)
				|| !TryReadExactBoolProperty(
					Layer,
					TEXT("bOverrideCollisionOffset"),
					bOverrideOffset)
				|| !TryReadExactFloatProperty(
					Layer,
					TEXT("CollisionThicknessOverride"),
					Thickness)
				|| !TryReadExactFloatProperty(
					Layer,
					TEXT("CollisionOffsetOverride"),
					Offset))
			{
				return Fail(
					OutError,
					TEXT("asset_data_paper_tilemap_private_binding_invalid"));
			}

			TArray<FUeAgentCanonicalValueRef> Cells;
			for (int32 Y = 0; Y < MapSize.Y; ++Y)
			{
				for (int32 X = 0; X < MapSize.X; ++X)
				{
					const FPaperTileInfo Cell = Layer.GetCell(X, Y);
					if (!Cell.IsValid())
					{
						continue;
					}
					FUeAgentCanonicalValuePtr CellValue;
					if (!TryBuildPaperTileMapCell(
							X,
							Y,
							Cell,
							Resources,
							CellValue,
							OutError))
					{
						return false;
					}
					Cells.Add(CellValue.ToSharedRef());
				}
			}
			if (Cells.Num() != Layer.GetNumOccupiedCells())
			{
				return Fail(
					OutError,
					TEXT("asset_data_paper_tilemap_existing_cell_out_of_bounds"),
					Layer.LayerName.ToString());
			}

			FUeAgentCanonicalValuePtr Name;
			FUeAgentCanonicalValuePtr VisibleInEditor;
			FUeAgentCanonicalValuePtr VisibleInGame;
			FUeAgentCanonicalValuePtr Collides;
			FUeAgentCanonicalValuePtr ThicknessValue;
			FUeAgentCanonicalValuePtr OffsetValue;
			FUeAgentCanonicalValuePtr Color;
			FUeAgentCanonicalValuePtr CellArray;
			if (!TryBuildString(
					Layer.LayerName.ToString(),
					Resources,
					Name,
					OutError)
				|| !TryBuildBoolean(
					Layer.ShouldRenderInEditor(),
					Resources,
					VisibleInEditor,
					OutError)
				|| !TryBuildBoolean(
					!bHiddenInGame,
					Resources,
					VisibleInGame,
					OutError)
				|| !TryBuildBoolean(
					Layer.GetLayerCollides(),
					Resources,
					Collides,
					OutError)
				|| !(bOverrideThickness
					? TryBuildDecimal(
						Thickness,
						Resources,
						ThicknessValue,
						OutError)
					: TryBuildNull(
						Resources,
						ThicknessValue,
						OutError))
				|| !(bOverrideOffset
					? TryBuildDecimal(
						Offset,
						Resources,
						OffsetValue,
						OutError)
					: TryBuildNull(
						Resources,
						OffsetValue,
						OutError))
				|| !TryBuildLinearColor(
					Layer.GetLayerColor(),
					Resources,
					Color,
					OutError)
				|| !TryBuildArray(
					MoveTemp(Cells),
					Resources,
					CellArray,
					OutError))
			{
				return false;
			}
			return TryBuildObject(
				{
					{TEXT("cells"), CellArray.ToSharedRef()},
					{TEXT("collides"), Collides.ToSharedRef()},
					{TEXT("collision_offset_override"), OffsetValue.ToSharedRef()},
					{TEXT("collision_thickness_override"), ThicknessValue.ToSharedRef()},
					{TEXT("color"), Color.ToSharedRef()},
					{TEXT("name"), Name.ToSharedRef()},
					{TEXT("visible_in_editor"), VisibleInEditor.ToSharedRef()},
					{TEXT("visible_in_game"), VisibleInGame.ToSharedRef()},
				},
				Resources,
				OutValue,
				OutError);
		}

		bool TryBuildPaperTileMapLayers(
			const UPaperTileMap& TileMap,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			const FIntPoint MapSize(TileMap.MapWidth, TileMap.MapHeight);
			TArray<FUeAgentCanonicalValueRef> Layers;
			Layers.Reserve(TileMap.TileLayers.Num());
			for (int32 Index = 0; Index < TileMap.TileLayers.Num(); ++Index)
			{
				const UPaperTileLayer* Layer = TileMap.TileLayers[Index];
				FUeAgentCanonicalValuePtr LayerValue;
				if (!Layer
					|| !TryBuildPaperTileMapLayer(
						*Layer,
						MapSize,
						Resources,
						LayerValue,
						OutError))
				{
					return Fail(
						OutError,
						TEXT("asset_data_paper_tilemap_existing_layer_invalid"),
						FString::FromInt(Index));
				}
				Layers.Add(LayerValue.ToSharedRef());
			}
			return TryBuildArray(
				MoveTemp(Layers),
				Resources,
				OutValue,
				OutError);
		}

		bool TryBuildPaperTileMapProjectionMap(
			const UPaperTileMap* TileMap,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutValues,
			FExecutionMutationError& OutError)
		{
			OutValues.Reset();
			if (!TileMap
				|| TileMap->MapWidth <= 0
				|| TileMap->MapHeight <= 0
				|| TileMap->TileWidth <= 0
				|| TileMap->TileHeight <= 0)
			{
				return Fail(
					OutError,
					TEXT("asset_data_paper_tilemap_projection_invalid"));
			}
			FUeAgentCanonicalValuePtr MapSize;
			FUeAgentCanonicalValuePtr TileSize;
			FUeAgentCanonicalValuePtr Layers;
			if (!TryBuildExtent2i(
					FIntPoint(TileMap->MapWidth, TileMap->MapHeight),
					Resources,
					MapSize,
					OutError)
				|| !TryBuildExtent2i(
					FIntPoint(TileMap->TileWidth, TileMap->TileHeight),
					Resources,
					TileSize,
					OutError)
				|| !TryBuildPaperTileMapLayers(
					*TileMap,
					Resources,
					Layers,
					OutError))
			{
				return false;
			}
			OutValues.Add(TEXT("map_size"), MapSize.ToSharedRef());
			OutValues.Add(TEXT("tile_size"), TileSize.ToSharedRef());
			OutValues.Add(TEXT("layers"), Layers.ToSharedRef());
			return true;
		}

		bool TryBuildPaperTileSetMarginState(
			const FIntMargin& Margin,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentCanonicalValuePtr Left;
			FUeAgentCanonicalValuePtr Top;
			FUeAgentCanonicalValuePtr Right;
			FUeAgentCanonicalValuePtr Bottom;
			return TryBuildInteger(Margin.Left, Resources, Left, OutError)
				&& TryBuildInteger(Margin.Top, Resources, Top, OutError)
				&& TryBuildInteger(Margin.Right, Resources, Right, OutError)
				&& TryBuildInteger(Margin.Bottom, Resources, Bottom, OutError)
				&& TryBuildObject(
					{
						{TEXT("bottom"), Bottom.ToSharedRef()},
						{TEXT("left"), Left.ToSharedRef()},
						{TEXT("right"), Right.ToSharedRef()},
						{TEXT("top"), Top.ToSharedRef()},
					},
					Resources,
					OutValue,
					OutError);
		}

		bool TryBuildPaperTileSetTerrain(
			const UPaperTileSet& TileSet,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			if (TileSet.GetNumTerrains() > 254)
			{
				return Fail(
					OutError,
					TEXT("asset_data_paper_tileset_existing_terrain_invalid"));
			}
			TArray<FUeAgentCanonicalValueRef> Terrains;
			Terrains.Reserve(TileSet.GetNumTerrains());
			for (int32 Index = 0; Index < TileSet.GetNumTerrains(); ++Index)
			{
				const FPaperTileSetTerrain& Terrain = TileSet.GetTerrain(Index);
				FUeAgentCanonicalValuePtr Name;
				FUeAgentCanonicalValuePtr Center;
				FUeAgentCanonicalValuePtr TerrainValue;
				if (Terrain.TerrainName.IsEmpty()
					|| Terrain.CenterTileIndex < 0
					|| !TryBuildString(
						Terrain.TerrainName,
						Resources,
						Name,
						OutError)
					|| !TryBuildInteger(
						Terrain.CenterTileIndex,
						Resources,
						Center,
						OutError)
					|| !TryBuildObject(
						{
							{TEXT("center_tile_index"), Center.ToSharedRef()},
							{TEXT("name"), Name.ToSharedRef()},
						},
						Resources,
						TerrainValue,
						OutError))
				{
					return false;
				}
				Terrains.Add(TerrainValue.ToSharedRef());
			}
			return TryBuildArray(
				MoveTemp(Terrains),
				Resources,
				OutValue,
				OutError);
		}

		bool TryBuildPaperTileSetProjectionMap(
			const UPaperTileSet* TileSet,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutValues,
			FExecutionMutationError& OutError)
		{
			OutValues.Reset();
			if (!TileSet)
			{
				return Fail(
					OutError,
					TEXT("asset_data_paper_tileset_projection_invalid"));
			}
			FUeAgentCanonicalValuePtr Source;
			FUeAgentCanonicalValuePtr TileSize;
			FUeAgentCanonicalValuePtr Margin;
			FUeAgentCanonicalValuePtr Spacing;
			FUeAgentCanonicalValuePtr Terrain;
			if (!TryBuildReference(
					TileSet->GetTileSheetTexture(),
					Resources,
					Source,
					OutError)
				|| !TryBuildExtent2i(
					TileSet->GetTileSize(),
					Resources,
					TileSize,
					OutError)
				|| !TryBuildPaperTileSetMarginState(
					TileSet->GetMargin(),
					Resources,
					Margin,
					OutError)
				|| !TryBuildExtent2i(
					TileSet->GetPerTileSpacing(),
					Resources,
					Spacing,
					OutError)
				|| !TryBuildPaperTileSetTerrain(
					*TileSet,
					Resources,
					Terrain,
					OutError))
			{
				return false;
			}
			OutValues.Add(TEXT("source_texture_ref"), Source.ToSharedRef());
			OutValues.Add(TEXT("tile_size"), TileSize.ToSharedRef());
			OutValues.Add(TEXT("margin"), Margin.ToSharedRef());
			OutValues.Add(TEXT("spacing"), Spacing.ToSharedRef());
			OutValues.Add(TEXT("terrain"), Terrain.ToSharedRef());
			return true;
		}

		const TCHAR* FindContextTag(
			const FString& NativeMechanicId,
			const FString& FieldId)
		{
			if (NativeMechanicId == PaperFlipbookMechanicId)
			{
				if (FieldId == TEXT("frames"))
				{
					return TEXT("paper_flipbook_frames");
				}
				if (FieldId == TEXT("frames_per_second"))
				{
					return TEXT("paper_flipbook_fps");
				}
			}
			else if (NativeMechanicId == PaperSpriteMechanicId)
			{
				if (FieldId == TEXT("source_texture_ref"))
				{
					return TEXT("paper_sprite_texture_ref");
				}
				if (FieldId == TEXT("source_region"))
				{
					return TEXT("paper_sprite_region");
				}
				if (FieldId == TEXT("pivot"))
				{
					return TEXT("paper_sprite_pivot_state");
				}
			}
			else if (NativeMechanicId == PaperTileMapMechanicId)
			{
				if (FieldId == TEXT("map_size"))
				{
					return TEXT("paper_tilemap_extent");
				}
				if (FieldId == TEXT("tile_size"))
				{
					return TEXT("paper_tilemap_tile_extent");
				}
				if (FieldId == TEXT("layers"))
				{
					return TEXT("paper_tilemap_layers");
				}
			}
			else if (NativeMechanicId == PaperTileSetMechanicId)
			{
				if (FieldId == TEXT("source_texture_ref"))
				{
					return TEXT("paper_tileset_texture_ref");
				}
				if (FieldId == TEXT("tile_size"))
				{
					return TEXT("paper_tileset_extent");
				}
				if (FieldId == TEXT("margin"))
				{
					return TEXT("paper_tileset_margin_state");
				}
				if (FieldId == TEXT("spacing"))
				{
					return TEXT("paper_tileset_spacing_state");
				}
				if (FieldId == TEXT("terrain"))
				{
					return TEXT("paper_tileset_terrain");
				}
			}
			return nullptr;
		}

		bool TryBuildContextValue(
			const FString& NativeMechanicId,
			const FString& FieldId,
			UObject* Target,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			const TCHAR* Tag = FindContextTag(NativeMechanicId, FieldId);
			if (!Tag)
			{
				return Fail(
					OutError,
					TEXT("asset_data_paper_projection_context_invalid"),
					FieldId);
			}
			if (NativeMechanicId != PaperTileMapMechanicId
				|| FieldId != TEXT("layers"))
			{
				return TryBuildString(
					Tag,
					Resources,
					OutValue,
					OutError);
			}
			const UPaperTileMap* TileMap = Cast<UPaperTileMap>(Target);
			if (!TileMap || TileMap->MapWidth <= 0 || TileMap->MapHeight <= 0)
			{
				return Fail(
					OutError,
					TEXT("asset_data_paper_tilemap_projection_context_invalid"));
			}
			FUeAgentCanonicalValuePtr TagValue;
			FUeAgentCanonicalValuePtr MapSize;
			return TryBuildString(Tag, Resources, TagValue, OutError)
				&& TryBuildExtent2i(
					FIntPoint(TileMap->MapWidth, TileMap->MapHeight),
					Resources,
					MapSize,
					OutError)
				&& TryBuildObject(
					{
						{TEXT("map_size"), MapSize.ToSharedRef()},
						{TEXT("tag"), TagValue.ToSharedRef()},
					},
					Resources,
					OutValue,
					OutError);
		}

		bool TryReadContextTag(
			const FUeAgentCanonicalValue& Context,
			FString& OutTag)
		{
			if (Context.GetKind() == EUeAgentValueKind::String)
			{
				return TryReadString(Context, OutTag);
			}
			const FUeAgentCanonicalValue* Tag =
				FindObjectField(Context, TEXT("tag"));
			return Tag && TryReadString(*Tag, OutTag);
		}

		bool TrySelectFields(
			const FString& NativeMechanicId,
			UObject* Target,
			const TArray<FString>& SelectedFieldIds,
			const TMap<FString, FUeAgentCanonicalValueRef>& CurrentValues,
			const TMap<FString, FUeAgentCanonicalValueRef>& DefaultValues,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutCurrentFields,
			FUeAgentCanonicalValuePtr& OutDefaultFields,
			FUeAgentCanonicalValuePtr& OutProjectionContext,
			FExecutionMutationError& OutError)
		{
			if (SelectedFieldIds.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("asset_data_paper_selected_fields_missing"));
			}
			TSet<FString> Seen;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> CurrentItems;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> DefaultItems;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> ContextItems;
			for (const FString& FieldId : SelectedFieldIds)
			{
				const FUeAgentCanonicalValueRef* Current =
					CurrentValues.Find(FieldId);
				const FUeAgentCanonicalValueRef* Default =
					DefaultValues.Find(FieldId);
				FUeAgentCanonicalValuePtr Context;
				if (Seen.Contains(FieldId)
					|| !Current
					|| !Default
					|| !TryBuildContextValue(
						NativeMechanicId,
						FieldId,
						Target,
						Resources,
						Context,
						OutError))
				{
					return Fail(
						OutError,
						TEXT("asset_data_paper_selected_field_invalid"),
						FieldId);
				}
				Seen.Add(FieldId);
				CurrentItems.Emplace(FieldId, *Current);
				DefaultItems.Emplace(FieldId, *Default);
				ContextItems.Emplace(FieldId, Context.ToSharedRef());
			}
			return TryBuildObject(
					MoveTemp(CurrentItems),
					Resources,
					OutCurrentFields,
					OutError)
				&& TryBuildObject(
					MoveTemp(DefaultItems),
					Resources,
					OutDefaultFields,
					OutError)
				&& TryBuildObject(
					MoveTemp(ContextItems),
					Resources,
					OutProjectionContext,
					OutError);
		}

		bool TryReadTypedChangeMap(
			const FUeAgentCanonicalValue& CurrentFields,
			const FUeAgentCanonicalValue& DefaultFields,
			const FUeAgentCanonicalValue& ProjectionContext,
			const FUeAgentCanonicalValue& ProposedAfter,
			TMap<FString, const FUeAgentCanonicalValue*>& OutChanges,
			FExecutionMutationError& OutError)
		{
			OutChanges.Reset();
			const FUeAgentCanonicalValue* TypedChanges =
				FindObjectField(ProposedAfter, TEXT("typed_changes"));
			if (CurrentFields.GetKind() != EUeAgentValueKind::Object
				|| DefaultFields.GetKind() != EUeAgentValueKind::Object
				|| ProjectionContext.GetKind() != EUeAgentValueKind::Object
				|| !TypedChanges
				|| TypedChanges->GetKind() != EUeAgentValueKind::Array
				|| TypedChanges->GetArrayItems().Num()
					!= CurrentFields.GetObjectItems().Num())
			{
				return Fail(
					OutError,
					TEXT("asset_data_paper_expected_state_invalid"));
			}
			for (const FUeAgentCanonicalValueRef& Change
				: TypedChanges->GetArrayItems())
			{
				const FUeAgentCanonicalValue* Field =
					FindObjectField(Change.Get(), TEXT("field_id"));
				FString FieldId;
				if (!Field
					|| !TryReadString(*Field, FieldId)
					|| OutChanges.Contains(FieldId))
				{
					return Fail(
						OutError,
						TEXT("asset_data_paper_expected_change_invalid"));
				}
				OutChanges.Add(FieldId, &Change.Get());
			}
			return true;
		}

		bool TryReadPresenceAndValue(
			const FUeAgentCanonicalValue& Change,
			FString& OutPresence,
			const FUeAgentCanonicalValue*& OutRequested)
		{
			OutPresence.Reset();
			OutRequested = nullptr;
			const FUeAgentCanonicalValue* Presence =
				FindObjectField(Change, TEXT("presence"));
			if (!Presence || !TryReadString(*Presence, OutPresence))
			{
				return false;
			}
			const FUeAgentCanonicalValue* TypedValue =
				FindObjectField(Change, TEXT("value"));
			OutRequested = TypedValue
				? FindObjectField(*TypedValue, TEXT("canonical_value"))
				: nullptr;
			return true;
		}

		bool TryBuildExpectedTextureReference(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FUeAgentCanonicalValue& Requested,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutExpected,
			FExecutionMutationError& OutError)
		{
			FString Reference;
			UObject* Resolved = nullptr;
			if (!TryReadString(Requested, Reference)
				|| !Resolver.TryResolveExisting(
					Reference,
					Resolved,
					OutError))
			{
				return Fail(
					OutError,
					TEXT("asset_data_paper_texture_reference_invalid"),
					Reference);
			}
			UTexture2D* Texture = Cast<UTexture2D>(Resolved);
			return Texture
				? TryBuildString(
					Texture->GetPathName(),
					Resources,
					OutExpected,
					OutError)
				: Fail(
					OutError,
					TEXT("asset_data_paper_texture_reference_class_invalid"),
					Reference);
		}

		bool TryBuildExpectedFlipbookFrames(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FUeAgentCanonicalValue& Requested,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutExpected,
			FExecutionMutationError& OutError)
		{
			if (Requested.GetKind() != EUeAgentValueKind::Array)
			{
				return Fail(
					OutError,
					TEXT("asset_data_paper_flipbook_frames_invalid"));
			}
			TArray<FUeAgentCanonicalValueRef> Frames;
			Frames.Reserve(Requested.GetArrayItems().Num());
			int64 TotalFrames = 0;
			for (int32 Index = 0;
				Index < Requested.GetArrayItems().Num();
				++Index)
			{
				const FUeAgentCanonicalValue& Frame =
					Requested.GetArrayItems()[Index].Get();
				const FUeAgentCanonicalValue* SpriteRef =
					FindObjectField(Frame, TEXT("sprite_ref"));
				const FUeAgentCanonicalValue* FrameRunValue =
					FindObjectField(Frame, TEXT("frame_run"));
				int32 FrameRun = 0;
				if (Frame.GetKind() != EUeAgentValueKind::Object
					|| Frame.GetObjectItems().Num() != 2
					|| !SpriteRef
					|| !FrameRunValue
					|| !TryReadInt32(*FrameRunValue, FrameRun)
					|| FrameRun < 1)
				{
					return Fail(
						OutError,
						TEXT("asset_data_paper_flipbook_frame_invalid"),
						FString::FromInt(Index));
				}
				TotalFrames += FrameRun;
				if (TotalFrames > MAX_int32)
				{
					return Fail(
						OutError,
						TEXT("asset_data_paper_flipbook_total_frames_overflow"));
				}

				FUeAgentCanonicalValuePtr CanonicalSprite;
				if (SpriteRef->GetKind() == EUeAgentValueKind::Null)
				{
					if (!TryBuildNull(
							Resources,
							CanonicalSprite,
							OutError))
					{
						return false;
					}
				}
				else
				{
					FString Reference;
					UObject* Resolved = nullptr;
					if (!TryReadString(*SpriteRef, Reference)
						|| !Resolver.TryResolveExisting(
							Reference,
							Resolved,
							OutError)
						|| !Cast<UPaperSprite>(Resolved)
						|| !TryBuildString(
							Resolved->GetPathName(),
							Resources,
							CanonicalSprite,
							OutError))
					{
						return Fail(
							OutError,
							TEXT("asset_data_paper_flipbook_sprite_ref_invalid"),
							Reference);
					}
				}
				FUeAgentCanonicalValuePtr CanonicalFrameRun;
				FUeAgentCanonicalValuePtr CanonicalFrame;
				if (!TryBuildInteger(
						FrameRun,
						Resources,
						CanonicalFrameRun,
						OutError)
					|| !TryBuildObject(
						{
							{TEXT("frame_run"), CanonicalFrameRun.ToSharedRef()},
							{TEXT("sprite_ref"), CanonicalSprite.ToSharedRef()},
						},
						Resources,
						CanonicalFrame,
						OutError))
				{
					return false;
				}
				Frames.Add(CanonicalFrame.ToSharedRef());
			}
			return TryBuildArray(
				MoveTemp(Frames),
				Resources,
				OutExpected,
				OutError);
		}

		bool TryBuildExpectedSpritePivot(
			const FUeAgentCanonicalValue& Requested,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutExpected,
			FExecutionMutationError& OutError)
		{
			if (Requested.GetKind() != EUeAgentValueKind::Object
				|| Requested.GetObjectItems().Num() != 2)
			{
				return Fail(
					OutError,
					TEXT("asset_data_paper_sprite_pivot_invalid"));
			}
			const FUeAgentCanonicalValue* X =
				FindObjectField(Requested, TEXT("x"));
			const FUeAgentCanonicalValue* Y =
				FindObjectField(Requested, TEXT("y"));
			int32 PixelX = 0;
			int32 PixelY = 0;
			if (!X
				|| !Y
				|| !TryReadInt32(*X, PixelX)
				|| !TryReadInt32(*Y, PixelY))
			{
				return Fail(
					OutError,
					TEXT("asset_data_paper_sprite_pivot_invalid"));
			}
			return TryBuildPaperSpritePivotState(
				ESpritePivotMode::Custom,
				FVector2D(PixelX, PixelY),
				Resources,
				OutExpected,
				OutError);
		}

		bool TryBuildUniformMarginState(
			const FUeAgentCanonicalValue& Requested,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutExpected,
			FExecutionMutationError& OutError)
		{
			int32 Value = 0;
			return TryReadInt32(Requested, Value) && Value >= 0
				? TryBuildPaperTileSetMarginState(
					FIntMargin(Value),
					Resources,
					OutExpected,
					OutError)
				: Fail(
					OutError,
					TEXT("asset_data_paper_tileset_margin_invalid"));
		}

		bool TryBuildUniformSpacingState(
			const FUeAgentCanonicalValue& Requested,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutExpected,
			FExecutionMutationError& OutError)
		{
			int32 Value = 0;
			return TryReadInt32(Requested, Value) && Value >= 0
				? TryBuildExtent2i(
					FIntPoint(Value, Value),
					Resources,
					OutExpected,
					OutError)
				: Fail(
					OutError,
					TEXT("asset_data_paper_tileset_spacing_invalid"));
		}

		bool TryBuildRequestedOptionalFloat(
			const FUeAgentCanonicalValue& Value,
			const bool bRequireNonNegative,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutExpected,
			FExecutionMutationError& OutError)
		{
			if (Value.GetKind() == EUeAgentValueKind::Null)
			{
				return TryBuildNull(Resources, OutExpected, OutError);
			}
			double Number = 0.0;
			return TryReadNumber(Value, Number)
					&& Number >= -static_cast<double>(MAX_flt)
					&& Number <= static_cast<double>(MAX_flt)
					&& (!bRequireNonNegative || Number >= 0.0)
				? TryBuildDecimal(
					Number,
					Resources,
					OutExpected,
					OutError)
				: Fail(
					OutError,
					TEXT("asset_data_paper_tilemap_optional_float_invalid"));
		}

		bool TryBuildExpectedPaperTileMapCell(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FUeAgentCanonicalValue& Requested,
			const FIntPoint& MapSize,
			TSet<uint64>& SeenCells,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutExpected,
			FExecutionMutationError& OutError)
		{
			if (Requested.GetKind() != EUeAgentValueKind::Object
				|| Requested.GetObjectItems().Num() != 7)
			{
				return Fail(
					OutError,
					TEXT("asset_data_paper_tilemap_cell_shape_invalid"));
			}
			const FUeAgentCanonicalValue* XValue =
				FindObjectField(Requested, TEXT("x"));
			const FUeAgentCanonicalValue* YValue =
				FindObjectField(Requested, TEXT("y"));
			const FUeAgentCanonicalValue* TileSetRef =
				FindObjectField(Requested, TEXT("tile_set_ref"));
			const FUeAgentCanonicalValue* TileIndexValue =
				FindObjectField(Requested, TEXT("tile_index"));
			int32 X = 0;
			int32 Y = 0;
			int32 TileIndex = 0;
			bool bFlipHorizontal = false;
			bool bFlipVertical = false;
			bool bFlipDiagonal = false;
			FString Reference;
			if (!XValue
				|| !YValue
				|| !TileSetRef
				|| !TileIndexValue
				|| !TryReadInt32(*XValue, X)
				|| !TryReadInt32(*YValue, Y)
				|| X < 0 || Y < 0 || X >= MapSize.X || Y >= MapSize.Y
				|| !TryReadString(*TileSetRef, Reference)
				|| !TryReadInt32(*TileIndexValue, TileIndex)
				|| TileIndex < 0
				|| TileIndex > static_cast<int32>(EPaperTileFlags::TileIndexMask)
				|| !TryReadBooleanField(
					Requested,
					TEXT("flip_horizontal"),
					bFlipHorizontal)
				|| !TryReadBooleanField(
					Requested,
					TEXT("flip_vertical"),
					bFlipVertical)
				|| !TryReadBooleanField(
					Requested,
					TEXT("flip_diagonal"),
					bFlipDiagonal))
			{
				return Fail(
					OutError,
					TEXT("asset_data_paper_tilemap_cell_value_invalid"));
			}
			const uint64 CellKey =
				(static_cast<uint64>(static_cast<uint32>(Y)) << 32)
				| static_cast<uint32>(X);
			if (SeenCells.Contains(CellKey))
			{
				return Fail(
					OutError,
					TEXT("asset_data_paper_tilemap_cell_duplicate"));
			}
			SeenCells.Add(CellKey);
			UObject* Resolved = nullptr;
			if (!Resolver.TryResolveExisting(Reference, Resolved, OutError)
				|| !Cast<UPaperTileSet>(Resolved))
			{
				return Fail(
					OutError,
					TEXT("asset_data_paper_tilemap_tileset_invalid"),
					Reference);
			}

			FUeAgentCanonicalValuePtr XCanonical;
			FUeAgentCanonicalValuePtr YCanonical;
			FUeAgentCanonicalValuePtr TileSetCanonical;
			FUeAgentCanonicalValuePtr TileIndexCanonical;
			FUeAgentCanonicalValuePtr FlipHorizontal;
			FUeAgentCanonicalValuePtr FlipVertical;
			FUeAgentCanonicalValuePtr FlipDiagonal;
			return TryBuildInteger(X, Resources, XCanonical, OutError)
				&& TryBuildInteger(Y, Resources, YCanonical, OutError)
				&& TryBuildString(
					Resolved->GetPathName(),
					Resources,
					TileSetCanonical,
					OutError)
				&& TryBuildInteger(
					TileIndex,
					Resources,
					TileIndexCanonical,
					OutError)
				&& TryBuildBoolean(
					bFlipHorizontal,
					Resources,
					FlipHorizontal,
					OutError)
				&& TryBuildBoolean(
					bFlipVertical,
					Resources,
					FlipVertical,
					OutError)
				&& TryBuildBoolean(
					bFlipDiagonal,
					Resources,
					FlipDiagonal,
					OutError)
				&& TryBuildObject(
					{
						{TEXT("flip_diagonal"), FlipDiagonal.ToSharedRef()},
						{TEXT("flip_horizontal"), FlipHorizontal.ToSharedRef()},
						{TEXT("flip_vertical"), FlipVertical.ToSharedRef()},
						{TEXT("tile_index"), TileIndexCanonical.ToSharedRef()},
						{TEXT("tile_set_ref"), TileSetCanonical.ToSharedRef()},
						{TEXT("x"), XCanonical.ToSharedRef()},
						{TEXT("y"), YCanonical.ToSharedRef()},
					},
					Resources,
					OutExpected,
					OutError);
		}

		bool TryBuildExpectedPaperTileMapLayers(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FUeAgentCanonicalValue& Requested,
			const FIntPoint& MapSize,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutExpected,
			FExecutionMutationError& OutError)
		{
			if (Requested.GetKind() != EUeAgentValueKind::Array)
			{
				return Fail(
					OutError,
					TEXT("asset_data_paper_tilemap_layers_invalid"));
			}
			TSet<FString> SeenNames;
			TArray<FUeAgentCanonicalValueRef> Layers;
			Layers.Reserve(Requested.GetArrayItems().Num());
			for (int32 LayerIndex = 0;
				LayerIndex < Requested.GetArrayItems().Num();
				++LayerIndex)
			{
				const FUeAgentCanonicalValue& Layer =
					Requested.GetArrayItems()[LayerIndex].Get();
				const FUeAgentCanonicalValue* NameValue =
					FindObjectField(Layer, TEXT("name"));
				const FUeAgentCanonicalValue* ThicknessValue =
					FindObjectField(
						Layer,
						TEXT("collision_thickness_override"));
				const FUeAgentCanonicalValue* OffsetValue =
					FindObjectField(
						Layer,
						TEXT("collision_offset_override"));
				const FUeAgentCanonicalValue* ColorValue =
					FindObjectField(Layer, TEXT("color"));
				const FUeAgentCanonicalValue* CellsValue =
					FindObjectField(Layer, TEXT("cells"));
				FString Name;
				bool bVisibleInEditor = false;
				bool bVisibleInGame = false;
				bool bCollides = false;
				FLinearColor Color;
				if (Layer.GetKind() != EUeAgentValueKind::Object
					|| Layer.GetObjectItems().Num() != 8
					|| !NameValue
					|| !TryReadString(*NameValue, Name)
					|| Name.TrimStartAndEnd().IsEmpty()
					|| !ThicknessValue
					|| !OffsetValue
					|| !ColorValue
					|| !TryReadLinearColor(*ColorValue, Color)
					|| !CellsValue
					|| CellsValue->GetKind() != EUeAgentValueKind::Array
					|| !TryReadBooleanField(
						Layer,
						TEXT("visible_in_editor"),
						bVisibleInEditor)
					|| !TryReadBooleanField(
						Layer,
						TEXT("visible_in_game"),
						bVisibleInGame)
					|| !TryReadBooleanField(
						Layer,
						TEXT("collides"),
						bCollides))
				{
					return Fail(
						OutError,
						TEXT("asset_data_paper_tilemap_layer_value_invalid"),
						FString::FromInt(LayerIndex));
				}
				const FString CanonicalName =
					Name.TrimStartAndEnd().ToLower();
				if (SeenNames.Contains(CanonicalName))
				{
					return Fail(
						OutError,
						TEXT("asset_data_paper_tilemap_layer_name_duplicate"),
						Name);
				}
				SeenNames.Add(CanonicalName);

				TSet<uint64> SeenCells;
				TArray<FUeAgentCanonicalValueRef> Cells;
				Cells.Reserve(CellsValue->GetArrayItems().Num());
				for (const FUeAgentCanonicalValueRef& Cell
					: CellsValue->GetArrayItems())
				{
					FUeAgentCanonicalValuePtr CanonicalCell;
					if (!TryBuildExpectedPaperTileMapCell(
							Resolver,
							Cell.Get(),
							MapSize,
							SeenCells,
							Resources,
							CanonicalCell,
							OutError))
					{
						return false;
					}
					Cells.Add(CanonicalCell.ToSharedRef());
				}

				FUeAgentCanonicalValuePtr CanonicalNameValue;
				FUeAgentCanonicalValuePtr VisibleInEditor;
				FUeAgentCanonicalValuePtr VisibleInGame;
				FUeAgentCanonicalValuePtr Collides;
				FUeAgentCanonicalValuePtr Thickness;
				FUeAgentCanonicalValuePtr Offset;
				FUeAgentCanonicalValuePtr CanonicalColor;
				FUeAgentCanonicalValuePtr CanonicalCells;
				FUeAgentCanonicalValuePtr CanonicalLayer;
				if (!TryBuildString(
						Name,
						Resources,
						CanonicalNameValue,
						OutError)
					|| !TryBuildBoolean(
						bVisibleInEditor,
						Resources,
						VisibleInEditor,
						OutError)
					|| !TryBuildBoolean(
						bVisibleInGame,
						Resources,
						VisibleInGame,
						OutError)
					|| !TryBuildBoolean(
						bCollides,
						Resources,
						Collides,
						OutError)
					|| !TryBuildRequestedOptionalFloat(
						*ThicknessValue,
						true,
						Resources,
						Thickness,
						OutError)
					|| !TryBuildRequestedOptionalFloat(
						*OffsetValue,
						false,
						Resources,
						Offset,
						OutError)
					|| !TryBuildLinearColor(
						Color,
						Resources,
						CanonicalColor,
						OutError)
					|| !TryBuildArray(
						MoveTemp(Cells),
						Resources,
						CanonicalCells,
						OutError)
					|| !TryBuildObject(
						{
							{TEXT("cells"), CanonicalCells.ToSharedRef()},
							{TEXT("collides"), Collides.ToSharedRef()},
							{TEXT("collision_offset_override"), Offset.ToSharedRef()},
							{TEXT("collision_thickness_override"), Thickness.ToSharedRef()},
							{TEXT("color"), CanonicalColor.ToSharedRef()},
							{TEXT("name"), CanonicalNameValue.ToSharedRef()},
							{TEXT("visible_in_editor"), VisibleInEditor.ToSharedRef()},
							{TEXT("visible_in_game"), VisibleInGame.ToSharedRef()},
						},
						Resources,
						CanonicalLayer,
						OutError))
				{
					return false;
				}
				Layers.Add(CanonicalLayer.ToSharedRef());
			}
			return TryBuildArray(
				MoveTemp(Layers),
				Resources,
				OutExpected,
				OutError);
		}

		bool TryBuildExpectedPaperTileSetTerrain(
			const FUeAgentCanonicalValue& Requested,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutExpected,
			FExecutionMutationError& OutError)
		{
			if (Requested.GetKind() != EUeAgentValueKind::Array
				|| Requested.GetArrayItems().Num() > 254)
			{
				return Fail(
					OutError,
					TEXT("asset_data_paper_tileset_terrain_invalid"));
			}
			TSet<FString> SeenNames;
			TArray<FUeAgentCanonicalValueRef> Terrains;
			Terrains.Reserve(Requested.GetArrayItems().Num());
			for (int32 Index = 0;
				Index < Requested.GetArrayItems().Num();
				++Index)
			{
				const FUeAgentCanonicalValue& Terrain =
					Requested.GetArrayItems()[Index].Get();
				const FUeAgentCanonicalValue* NameValue =
					FindObjectField(Terrain, TEXT("name"));
				const FUeAgentCanonicalValue* CenterValue =
					FindObjectField(Terrain, TEXT("center_tile_index"));
				FString Name;
				int32 Center = 0;
				if (Terrain.GetKind() != EUeAgentValueKind::Object
					|| Terrain.GetObjectItems().Num() != 2
					|| !NameValue
					|| !TryReadString(*NameValue, Name)
					|| Name.TrimStartAndEnd().IsEmpty()
					|| !CenterValue
					|| !TryReadInt32(*CenterValue, Center)
					|| Center < 0)
				{
					return Fail(
						OutError,
						TEXT("asset_data_paper_tileset_terrain_value_invalid"),
						FString::FromInt(Index));
				}
				const FString CanonicalName =
					Name.TrimStartAndEnd().ToLower();
				if (SeenNames.Contains(CanonicalName))
				{
					return Fail(
						OutError,
						TEXT("asset_data_paper_tileset_terrain_name_duplicate"),
						Name);
				}
				SeenNames.Add(CanonicalName);
				FUeAgentCanonicalValuePtr CanonicalNameValue;
				FUeAgentCanonicalValuePtr CanonicalCenter;
				FUeAgentCanonicalValuePtr CanonicalTerrain;
				if (!TryBuildString(
						Name,
						Resources,
						CanonicalNameValue,
						OutError)
					|| !TryBuildInteger(
						Center,
						Resources,
						CanonicalCenter,
						OutError)
					|| !TryBuildObject(
						{
							{TEXT("center_tile_index"), CanonicalCenter.ToSharedRef()},
							{TEXT("name"), CanonicalNameValue.ToSharedRef()},
						},
						Resources,
						CanonicalTerrain,
						OutError))
				{
					return false;
				}
				Terrains.Add(CanonicalTerrain.ToSharedRef());
			}
			return TryBuildArray(
				MoveTemp(Terrains),
				Resources,
				OutExpected,
				OutError);
		}

		const FUeAgentCanonicalValueRef* FindObjectFieldRef(
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
					return &Item.Value;
				}
			}
			return nullptr;
		}

		bool TryResolveExpectedTileMapSize(
			const TMap<FString, const FUeAgentCanonicalValue*>& Changes,
			const FUeAgentCanonicalValue& ProjectionContext,
			FIntPoint& OutMapSize,
			FExecutionMutationError& OutError)
		{
			if (const FUeAgentCanonicalValue* const* MapSizeChange =
				Changes.Find(TEXT("map_size")))
			{
				FString Presence;
				const FUeAgentCanonicalValue* Requested = nullptr;
				if (!TryReadPresenceAndValue(
						**MapSizeChange,
						Presence,
						Requested)
					|| Presence != TEXT("replace")
					|| !Requested
					|| !TryReadExtent2i(
						*Requested,
						1,
						1024,
						OutMapSize))
				{
					return Fail(
						OutError,
						TEXT("asset_data_paper_tilemap_map_size_invalid"));
				}
				return true;
			}
			const FUeAgentCanonicalValue* LayerContext =
				FindObjectField(ProjectionContext, TEXT("layers"));
			const FUeAgentCanonicalValue* MapSize = LayerContext
				? FindObjectField(*LayerContext, TEXT("map_size"))
				: nullptr;
			return MapSize
					&& TryReadExtent2i(*MapSize, 1, 1024, OutMapSize)
				? true
				: Fail(
					OutError,
					TEXT("asset_data_paper_tilemap_projection_context_invalid"));
		}
	}

	bool CanProjectPaperFieldState(
		const FString& NativeMechanicId)
	{
		return NativeMechanicId == PaperFlipbookMechanicId
			|| NativeMechanicId == PaperSpriteMechanicId
			|| NativeMechanicId == PaperTileMapMechanicId
			|| NativeMechanicId == PaperTileSetMechanicId;
	}

	bool TryProjectPaperCurrentFieldState(
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
		(void)Resolver;
		OutCurrentFields.Reset();
		OutDefaultFields.Reset();
		OutProjectionContext.Reset();
		if (!CanProjectPaperFieldState(NativeMechanicId) || !Target)
		{
			return Fail(
				OutError,
				TEXT("asset_data_paper_field_projection_unsupported"),
				NativeMechanicId);
		}

		TMap<FString, FUeAgentCanonicalValueRef> CurrentValues;
		TMap<FString, FUeAgentCanonicalValueRef> DefaultValues;
		if (NativeMechanicId == PaperFlipbookMechanicId)
		{
			if (!TryBuildPaperFlipbookProjectionMap(
					Cast<UPaperFlipbook>(Target),
					Resources,
					CurrentValues,
					OutError)
				|| !TryBuildPaperFlipbookProjectionMap(
					GetDefault<UPaperFlipbook>(),
					Resources,
					DefaultValues,
					OutError))
			{
				return false;
			}
		}
		else if (NativeMechanicId == PaperSpriteMechanicId)
		{
			if (!TryBuildPaperSpriteProjectionMap(
					Cast<UPaperSprite>(Target),
					Resources,
					CurrentValues,
					OutError)
				|| !TryBuildPaperSpriteProjectionMap(
					GetDefault<UPaperSprite>(),
					Resources,
					DefaultValues,
					OutError))
			{
				return false;
			}
		}
		else if (NativeMechanicId == PaperTileMapMechanicId)
		{
			if (!TryBuildPaperTileMapProjectionMap(
					Cast<UPaperTileMap>(Target),
					Resources,
					CurrentValues,
					OutError)
				|| !TryBuildPaperTileMapProjectionMap(
					GetDefault<UPaperTileMap>(),
					Resources,
					DefaultValues,
					OutError))
			{
				return false;
			}
		}
		else if (!TryBuildPaperTileSetProjectionMap(
				Cast<UPaperTileSet>(Target),
				Resources,
				CurrentValues,
				OutError)
			|| !TryBuildPaperTileSetProjectionMap(
				GetDefault<UPaperTileSet>(),
				Resources,
				DefaultValues,
				OutError))
		{
			return false;
		}

		return TrySelectFields(
			NativeMechanicId,
			Target,
			SelectedFieldIds,
			CurrentValues,
			DefaultValues,
			Resources,
			OutCurrentFields,
			OutDefaultFields,
			OutProjectionContext,
			OutError);
	}

	bool TryProjectPaperExpectedFieldState(
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
		OutExpectedFields.Reset();
		if (!CanProjectPaperFieldState(NativeMechanicId))
		{
			return Fail(
				OutError,
				TEXT("asset_data_paper_field_projection_unsupported"),
				NativeMechanicId);
		}
		TMap<FString, const FUeAgentCanonicalValue*> Changes;
		if (!TryReadTypedChangeMap(
				CurrentFields,
				DefaultFields,
				ProjectionContext,
				ProposedAfter,
				Changes,
				OutError))
		{
			return false;
		}

		FIntPoint EffectiveTileMapSize = FIntPoint::ZeroValue;
		if (NativeMechanicId == PaperTileMapMechanicId
			&& Changes.Contains(TEXT("layers"))
			&& !TryResolveExpectedTileMapSize(
				Changes,
				ProjectionContext,
				EffectiveTileMapSize,
				OutError))
		{
			return false;
		}

		TArray<TPair<FString, FUeAgentCanonicalValueRef>> ExpectedItems;
		for (const TPair<FString, FUeAgentCanonicalValueRef>& Current
			: CurrentFields.GetObjectItems())
		{
			const FUeAgentCanonicalValue* const* Change =
				Changes.Find(Current.Key);
			const FUeAgentCanonicalValueRef* Default =
				FindObjectFieldRef(DefaultFields, Current.Key);
			const FUeAgentCanonicalValue* Context =
				FindObjectField(ProjectionContext, Current.Key);
			const TCHAR* ExpectedContext =
				FindContextTag(NativeMechanicId, Current.Key);
			FString ContextTag;
			if (!Change
				|| !Default
				|| !Context
				|| !ExpectedContext
				|| !TryReadContextTag(*Context, ContextTag)
				|| ContextTag != ExpectedContext)
			{
				return Fail(
					OutError,
					TEXT("asset_data_paper_expected_context_invalid"),
					Current.Key);
			}

			FString Presence;
			const FUeAgentCanonicalValue* Requested = nullptr;
			if (!TryReadPresenceAndValue(
					**Change,
					Presence,
					Requested))
			{
				return Fail(
					OutError,
					TEXT("asset_data_paper_expected_presence_invalid"),
					Current.Key);
			}

			if (NativeMechanicId == PaperSpriteMechanicId
				&& Presence == TEXT("clear"))
			{
				if (Requested
					|| (Current.Key != TEXT("source_texture_ref")
						&& Current.Key != TEXT("source_region")
						&& Current.Key != TEXT("pivot")))
				{
					return Fail(
						OutError,
						TEXT("asset_data_paper_sprite_clear_invalid"),
						Current.Key);
				}
				ExpectedItems.Emplace(Current.Key, *Default);
				continue;
			}

			FUeAgentCanonicalValuePtr Expected;
			if (NativeMechanicId == PaperFlipbookMechanicId)
			{
				if (Presence != TEXT("replace") || !Requested)
				{
					return Fail(
						OutError,
						TEXT("asset_data_paper_flipbook_expected_invalid"),
						Current.Key);
				}
				if (Current.Key == TEXT("frames"))
				{
					if (!TryBuildExpectedFlipbookFrames(
							Resolver,
							*Requested,
							Resources,
							Expected,
							OutError))
					{
						return false;
					}
				}
				else if (Current.Key == TEXT("frames_per_second"))
				{
					double FramesPerSecond = 0.0;
					if (!TryReadNumber(*Requested, FramesPerSecond)
						|| FramesPerSecond < 0.0
						|| FramesPerSecond > 1000.0)
					{
						return Fail(
							OutError,
							TEXT("asset_data_paper_flipbook_fps_invalid"));
					}
					const float NativeFramesPerSecond =
						static_cast<float>(FramesPerSecond);
					if (!TryBuildDecimal(
							NativeFramesPerSecond,
							Resources,
							Expected,
							OutError))
					{
						return false;
					}
				}
				else
				{
					return Fail(
						OutError,
						TEXT("asset_data_paper_flipbook_expected_field_invalid"),
						Current.Key);
				}
			}
			else if (NativeMechanicId == PaperSpriteMechanicId)
			{
				if (Current.Key == TEXT("source_texture_ref"))
				{
					if (Presence == TEXT("null") && !Requested)
					{
						if (!TryBuildNull(Resources, Expected, OutError))
						{
							return false;
						}
					}
					else if (Presence == TEXT("set") && Requested)
					{
						if (!TryBuildExpectedTextureReference(
								Resolver,
								*Requested,
								Resources,
								Expected,
								OutError))
						{
							return false;
						}
					}
					else
					{
						return Fail(
							OutError,
							TEXT("asset_data_paper_sprite_source_expected_invalid"));
					}
				}
				else if (Current.Key == TEXT("source_region"))
				{
					FIntPoint Offset;
					FIntPoint Size;
					if (Presence != TEXT("set")
						|| !Requested
						|| !TryReadRect2d(*Requested, Offset, Size)
						|| !TryBuildRect2d(
							Offset,
							Size,
							Resources,
							Expected,
							OutError))
					{
						return Fail(
							OutError,
							TEXT("asset_data_paper_sprite_region_expected_invalid"));
					}
				}
				else if (Current.Key == TEXT("pivot"))
				{
					if (Presence != TEXT("set")
						|| !Requested
						|| !TryBuildExpectedSpritePivot(
							*Requested,
							Resources,
							Expected,
							OutError))
					{
						return Fail(
							OutError,
							TEXT("asset_data_paper_sprite_pivot_expected_invalid"));
					}
				}
				else
				{
					return Fail(
						OutError,
						TEXT("asset_data_paper_sprite_expected_field_invalid"),
						Current.Key);
				}
			}
			else if (NativeMechanicId == PaperTileMapMechanicId)
			{
				if (Presence != TEXT("replace") || !Requested)
				{
					return Fail(
						OutError,
						TEXT("asset_data_paper_tilemap_expected_invalid"),
						Current.Key);
				}
				if (Current.Key == TEXT("map_size")
					|| Current.Key == TEXT("tile_size"))
				{
					FIntPoint Extent;
					const int32 Maximum = Current.Key == TEXT("map_size")
						? 1024
						: MAX_int32;
					if (!TryReadExtent2i(*Requested, 1, Maximum, Extent)
						|| !TryBuildExtent2i(
							Extent,
							Resources,
							Expected,
							OutError))
					{
						return Fail(
							OutError,
							TEXT("asset_data_paper_tilemap_extent_expected_invalid"),
							Current.Key);
					}
				}
				else if (Current.Key == TEXT("layers"))
				{
					if (!TryBuildExpectedPaperTileMapLayers(
							Resolver,
							*Requested,
							EffectiveTileMapSize,
							Resources,
							Expected,
							OutError))
					{
						return false;
					}
				}
				else
				{
					return Fail(
						OutError,
						TEXT("asset_data_paper_tilemap_expected_field_invalid"),
						Current.Key);
				}
			}
			else
			{
				if (Presence != TEXT("replace") || !Requested)
				{
					return Fail(
						OutError,
						TEXT("asset_data_paper_tileset_expected_invalid"),
						Current.Key);
				}
				if (Current.Key == TEXT("source_texture_ref"))
				{
					if (!TryBuildExpectedTextureReference(
							Resolver,
							*Requested,
							Resources,
							Expected,
							OutError))
					{
						return false;
					}
				}
				else if (Current.Key == TEXT("tile_size"))
				{
					FIntPoint Extent;
					if (!TryReadExtent2i(
							*Requested,
							1,
							MAX_int32,
							Extent)
						|| !TryBuildExtent2i(
							Extent,
							Resources,
							Expected,
							OutError))
					{
						return Fail(
							OutError,
							TEXT("asset_data_paper_tileset_tile_size_invalid"));
					}
				}
				else if (Current.Key == TEXT("margin"))
				{
					if (!TryBuildUniformMarginState(
							*Requested,
							Resources,
							Expected,
							OutError))
					{
						return false;
					}
				}
				else if (Current.Key == TEXT("spacing"))
				{
					if (!TryBuildUniformSpacingState(
							*Requested,
							Resources,
							Expected,
							OutError))
					{
						return false;
					}
				}
				else if (Current.Key == TEXT("terrain"))
				{
					if (!TryBuildExpectedPaperTileSetTerrain(
							*Requested,
							Resources,
							Expected,
							OutError))
					{
						return false;
					}
				}
				else
				{
					return Fail(
						OutError,
						TEXT("asset_data_paper_tileset_expected_field_invalid"),
						Current.Key);
				}
			}
			ExpectedItems.Emplace(Current.Key, Expected.ToSharedRef());
		}
		return TryBuildObject(
			MoveTemp(ExpectedItems),
			Resources,
			OutExpectedFields,
			OutError);
	}
}
