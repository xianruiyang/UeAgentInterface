#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectPaperTileMechanics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Misc/ScopeExit.h"

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

        FExecutionMutationError NativeFailure(const FString& Code, const FString& Reason, const FString& Field,
            const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
        {
            auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Field, Expected, Actual, TEXT("execution"), Reason);
            Error.SourceKind = TEXT("native_result");
            Error.Source = {};
            Error.SemanticPointer.Reset();
            Error.ActualValuePresent = Actual.IsValid();
            Error.ExpectedValuePresent = Expected.IsValid();
            return Error;
        }

        TSharedPtr<FJsonValue> ObjectClassValue(const UObject* Object)
        {
            return Object ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Object->GetClass()->GetPathName()))
                : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
        }

        bool Fail(FExecutionMutationError& OutError, FExecutionMutationError Error)
        {
            OutError = MoveTemp(Error);
            return false;
        }

        FExecutionMutationError JsonFailure(const FString& Code, const FString& Reason, const FString& Field,
            const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected,
            const TCHAR* SourceKind = TEXT("authored_value"))
        {
            auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Field, Expected, Actual, TEXT("execution"), Reason);
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

        FExecutionMutationError ValueFailure(const FString& Code, const FString& Reason, const FString& Field,
            const FUeAgentCanonicalValue* Actual, const FString& Expected,
            const TCHAR* SourceKind = TEXT("authored_value"))
        {
            auto Error = JsonFailure(Code, Reason, Field, nullptr, MakeShared<FJsonValueString>(Expected), SourceKind);
            Error.ActualValuePresent = Actual != nullptr;
            Contract::FDiagnostic ProjectionError;
            if (Actual && !CanonicalJson::TryToJsonValue(*Actual, Error.ActualValue, ProjectionError))
            {
                ProjectionError.SourceKind = TEXT("native_result");
                Error.Diagnostics.Add(MoveTemp(ProjectionError));
            }
            return Error;
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

        bool TryReadNumber(const FUeAgentCanonicalValue& Value, double& OutNumber,
            const FString& Field, FExecutionMutationError& OutError,
            const TCHAR* SourceKind = TEXT("authored_value"))
        {
            if (Value.GetKind() != EUeAgentValueKind::Integer && Value.GetKind() != EUeAgentValueKind::Decimal)
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_number_invalid"),
                    TEXT("A numeric value is required."), Field, &Value, TEXT("finite number"), SourceKind));
            if (!LexTryParseString(OutNumber, *Value.GetLexeme()) || !FMath::IsFinite(OutNumber))
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_number_invalid"),
                    TEXT("The number cannot be represented as a finite native value."), Field, &Value, TEXT("finite number"), SourceKind));
            return true;
        }

        bool TryReadInt32(const FUeAgentCanonicalValue& Value, int32& OutValue,
            const FString& Field, FExecutionMutationError& OutError,
            const TCHAR* SourceKind = TEXT("authored_value"))
        {
            int64 Number = 0;
            if ((Value.GetKind() != EUeAgentValueKind::Integer && Value.GetKind() != EUeAgentValueKind::Decimal)
                || !TryParseInt64Number(Value.GetLexeme(), Number) || Number < MIN_int32 || Number > MAX_int32)
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_int32_invalid"),
                    TEXT("The value must be an integer representable as int32."), Field, &Value,
                    TEXT("integer in [-2147483648, 2147483647]"), SourceKind));
            OutValue = static_cast<int32>(Number);
            return true;
        }

        bool TryReadBool(const FUeAgentCanonicalValue& Object, const FString& Field, bool& bOutValue,
            const FString& Parent, FExecutionMutationError& OutError)
        {
            const FUeAgentCanonicalValue* Value = FindObjectField(Object, Field);
            if (!Value || Value->GetKind() != EUeAgentValueKind::Boolean)
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_boolean_invalid"),
                    Value ? TEXT("The field must be a boolean.") : TEXT("The required boolean field is missing."),
                    Parent + TEXT("/") + Field, Value, TEXT("boolean")));
            bOutValue = Value->GetBoolean();
            return true;
        }

        bool TryReadExtent2i(const FUeAgentCanonicalValue& Value, const int32 Minimum, const int32 Maximum,
            FIntPoint& OutExtent, const FString& Field, FExecutionMutationError& OutError,
            const TCHAR* SourceKind = TEXT("authored_value"))
        {
            if (Value.GetKind() != EUeAgentValueKind::Object || Value.GetObjectItems().Num() != 2)
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_extent_invalid"),
                    TEXT("The extent must contain exactly x and y."), Field, &Value, TEXT("object with x and y"), SourceKind));
            const FUeAgentCanonicalValue* X = FindObjectField(Value, TEXT("x"));
            const FUeAgentCanonicalValue* Y = FindObjectField(Value, TEXT("y"));
            if (!X || !Y)
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_extent_invalid"),
                    TEXT("A required extent component is missing."), Field + (!X ? TEXT("/x") : TEXT("/y")),
                    nullptr, TEXT("int32 component"), SourceKind));
            if (!TryReadInt32(*X, OutExtent.X, Field + TEXT("/x"), OutError, SourceKind)
                || !TryReadInt32(*Y, OutExtent.Y, Field + TEXT("/y"), OutError, SourceKind)) return false;
            if (OutExtent.X < Minimum || OutExtent.Y < Minimum || OutExtent.X > Maximum || OutExtent.Y > Maximum)
            {
                const bool bX = OutExtent.X < Minimum || (OutExtent.Y >= Minimum && OutExtent.X > Maximum);
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_extent_invalid"),
                    TEXT("The extent component is outside the supported range."), Field + (bX ? TEXT("/x") : TEXT("/y")),
                    bX ? X : Y, FString::Printf(TEXT("integer in [%d, %d]"), Minimum, Maximum), SourceKind));
            }
            return true;
        }

        bool TryReadLinearColor(const FUeAgentCanonicalValue& Value, FLinearColor& OutColor,
            const FString& Field, FExecutionMutationError& OutError)
        {
            if (Value.GetKind() != EUeAgentValueKind::Object || Value.GetObjectItems().Num() != 4)
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_color_invalid"),
                    TEXT("A color must contain exactly r, g, b and a."), Field, &Value, TEXT("object with r, g, b and a")));
            const TCHAR* Names[] = {TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")};
            const FUeAgentCanonicalValue* Values[] = {FindObjectField(Value, Names[0]), FindObjectField(Value, Names[1]),
                FindObjectField(Value, Names[2]), FindObjectField(Value, Names[3])};
            double Components[4] = {};
            for (int32 Index = 0; Index < 4; ++Index)
                if (!Values[Index])
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_color_invalid"),
                        TEXT("A required color component is missing."), Field + TEXT("/") + Names[Index], nullptr, TEXT("finite float32 number")));
            for (int32 Index = 0; Index < 4; ++Index)
                if (!TryReadNumber(*Values[Index], Components[Index], Field + TEXT("/") + Names[Index], OutError)) return false;
            for (int32 Index = 0; Index < 4; ++Index)
                if (Components[Index] < -MAX_flt || Components[Index] > MAX_flt)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_color_invalid"),
                        TEXT("The color component is outside the finite float32 range."), Field + TEXT("/") + Names[Index],
                        Values[Index], TEXT("finite float32 number")));
            OutColor = FLinearColor(static_cast<float>(Components[0]), static_cast<float>(Components[1]),
                static_cast<float>(Components[2]), static_cast<float>(Components[3]));
            return true;
        }

		bool TryReadOptionalFloat(
			const FUeAgentCanonicalValue& Object,
			const FString& Field,
			const bool bRequireNonNegative,
			bool& bOutEnabled,
            float& OutValue, const FString& Parent, FExecutionMutationError& OutError)
        {
            const FUeAgentCanonicalValue* Value = FindObjectField(Object, Field);
            const FString Path = Parent + TEXT("/") + Field;
            if (!Value)
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tilemap_optional_float_invalid"),
                    TEXT("The required override field is missing."), Path, nullptr, TEXT("finite float32 number or null")));
            if (Value->GetKind() == EUeAgentValueKind::Null)
            {
                bOutEnabled = false;
                OutValue = 0.0f;
                return true;
            }
            double Number = 0.0;
            if (!TryReadNumber(*Value, Number, Path, OutError)) return false;
            if (Number < -static_cast<double>(MAX_flt) || Number > static_cast<double>(MAX_flt)
                || (bRequireNonNegative && Number < 0.0))
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tilemap_optional_float_invalid"),
                    Number < -static_cast<double>(MAX_flt) || Number > static_cast<double>(MAX_flt)
                        ? TEXT("The override is outside the finite float32 range.") : TEXT("The collision thickness override must be nonnegative."),
                    Path, Value, bRequireNonNegative ? TEXT("finite float32 number >= 0, or null") : TEXT("finite float32 number, or null")));
			bOutEnabled = true;
			OutValue = static_cast<float>(Number);
			return true;
		}

        bool ValidateLayerPrivateBindings(FExecutionMutationError& OutError)
        {
            const UClass* Class = UPaperTileLayer::StaticClass();
            const TCHAR* Names[] = {TEXT("bHiddenInGame"), TEXT("bOverrideCollisionThickness"),
                TEXT("bOverrideCollisionOffset"), TEXT("CollisionThicknessOverride"), TEXT("CollisionOffsetOverride")};
            for (int32 Index = 0; Index < 5; ++Index)
            {
                const FProperty* Property = FindFProperty<FProperty>(Class, Names[Index]);
                if ((Index < 3 && !CastField<FBoolProperty>(Property))
                    || (Index >= 3 && !CastField<FFloatProperty>(Property)))
                    return Fail(OutError, NativeFailure(TEXT("asset_data_paper_tilemap_private_binding_invalid"),
                        TEXT("The native tile layer property is missing or has an incompatible type."), Names[Index],
                        MakeShared<FJsonValueString>(Property ? Property->GetCPPType(nullptr, 0) : TEXT("missing")),
                        MakeShared<FJsonValueString>(Index < 3 ? TEXT("boolean property") : TEXT("float property"))));
            }
            return true;
        }

		bool TrySetExactBoolProperty(
			UObject& Object,
			const FName PropertyName,
            const bool bValue, const FString& Field, FExecutionMutationError& OutError)
        {
            FProperty* Raw = FindFProperty<FProperty>(Object.GetClass(), PropertyName);
            FBoolProperty* Property = CastField<FBoolProperty>(Raw);
            if (!Property)
            {
                OutError = NativeFailure(TEXT("asset_data_paper_tilemap_private_binding_invalid"),
                    TEXT("The native tile layer boolean property is missing or has an incompatible type."), Field,
                    MakeShared<FJsonValueString>(Raw ? Raw->GetCPPType(nullptr, 0) : TEXT("missing")),
                    MakeShared<FJsonValueString>(TEXT("boolean property")));
                OutError.TargetPath = Object.GetPathName();
                OutError.InternalContext = PropertyName.ToString();
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
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tilemap_layers_invalid"),
                    TEXT("Tile map layers must be an array."), TEXT("layers"), &Value, TEXT("array")));
			}
			TSet<FString> SeenNames;
			for (int32 LayerIndex = 0;
				LayerIndex < Value.GetArrayItems().Num();
				++LayerIndex)
			{
				const FUeAgentCanonicalValue& LayerValue =
					Value.GetArrayItems()[LayerIndex].Get();
                const FString Field = TEXT("layers/") + FString::FromInt(LayerIndex);
                if (LayerValue.GetKind() != EUeAgentValueKind::Object || LayerValue.GetObjectItems().Num() != 8)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tilemap_layer_shape_invalid"),
                        TEXT("A layer must contain exactly its eight supported fields."), Field, &LayerValue,
                        TEXT("object with name, color, cells, visible_in_editor, visible_in_game, collides, collision_thickness_override and collision_offset_override")));
				const FUeAgentCanonicalValue* NameValue =
					FindObjectField(LayerValue, TEXT("name"));
				const FUeAgentCanonicalValue* ColorValue =
					FindObjectField(LayerValue, TEXT("color"));
				const FUeAgentCanonicalValue* CellsValue =
					FindObjectField(LayerValue, TEXT("cells"));
				FPaperTileMapLayerState Layer;
                if (!NameValue || NameValue->GetKind() != EUeAgentValueKind::String
                    || NameValue->GetText().TrimStartAndEnd().IsEmpty())
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tilemap_layer_value_invalid"),
                        TEXT("The layer name must be a nonblank string."), Field + TEXT("/name"), NameValue, TEXT("nonblank string")));
                if (!ColorValue)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tilemap_layer_value_invalid"),
                        TEXT("The layer color is missing."), Field + TEXT("/color"), nullptr, TEXT("RGBA color object")));
                if (!CellsValue || CellsValue->GetKind() != EUeAgentValueKind::Array)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tilemap_layer_value_invalid"),
                        TEXT("The layer requires a cells array."), Field + TEXT("/cells"), CellsValue, TEXT("array")));
                if (!TryReadBool(LayerValue, TEXT("visible_in_editor"), Layer.bVisibleInEditor, Field, OutError)
                    || !TryReadBool(LayerValue, TEXT("visible_in_game"), Layer.bVisibleInGame, Field, OutError)
                    || !TryReadBool(LayerValue, TEXT("collides"), Layer.bCollides, Field, OutError)
                    || !TryReadOptionalFloat(LayerValue, TEXT("collision_thickness_override"), true,
                        Layer.bOverrideThickness, Layer.Thickness, Field, OutError)
                    || !TryReadOptionalFloat(LayerValue, TEXT("collision_offset_override"), false,
                        Layer.bOverrideOffset, Layer.Offset, Field, OutError)
                    || !TryReadLinearColor(*ColorValue, Layer.Color, Field + TEXT("/color"), OutError)) return false;
				Layer.Name = NameValue->GetText();
				FString CanonicalName =
					Layer.Name.TrimStartAndEnd().ToLower();
				if (SeenNames.Contains(CanonicalName))
				{
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tilemap_layer_name_duplicate"),
                        TEXT("Another layer has the same name after trimming whitespace and ignoring case."),
                        Field + TEXT("/name"), NameValue, TEXT("unique layer name")));
				}
				SeenNames.Add(MoveTemp(CanonicalName));

				TSet<uint64> SeenCells;
				for (int32 CellIndex = 0;
					CellIndex < CellsValue->GetArrayItems().Num();
					++CellIndex)
				{
					const FUeAgentCanonicalValue& CellValue =
						CellsValue->GetArrayItems()[CellIndex].Get();
                    const FString CellField = Field + TEXT("/cells/") + FString::FromInt(CellIndex);
                    if (CellValue.GetKind() != EUeAgentValueKind::Object || CellValue.GetObjectItems().Num() != 7)
                        return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tilemap_cell_shape_invalid"),
                            TEXT("A tile map cell must contain exactly its seven supported fields."), CellField, &CellValue,
                            TEXT("object with x, y, tile_set_ref, tile_index, flip_horizontal, flip_vertical and flip_diagonal")));
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
                    const TCHAR* Names[] = {TEXT("x"), TEXT("y"), TEXT("tile_set_ref"), TEXT("tile_index")};
                    const FUeAgentCanonicalValue* Values[] = {X, Y, TileSetRef, TileIndexValue};
                    for (int32 Index = 0; Index < 4; ++Index)
                        if (!Values[Index])
                            return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tilemap_cell_value_invalid"),
                                TEXT("A required cell field is missing."), CellField + TEXT("/") + Names[Index], nullptr,
                                Index == 2 ? TEXT("PaperTileSet reference") : TEXT("int32 value")));
                    if (!TryReadInt32(*X, Cell.X, CellField + TEXT("/x"), OutError)
                        || !TryReadInt32(*Y, Cell.Y, CellField + TEXT("/y"), OutError)) return false;
                    if (Cell.X < 0 || Cell.Y < 0 || Cell.X >= MapSize.X || Cell.Y >= MapSize.Y)
                    {
                        const bool bX = Cell.X < 0 || (Cell.Y >= 0 && Cell.X >= MapSize.X);
                        return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tilemap_cell_value_invalid"),
                            TEXT("The cell coordinate is outside the map dimensions."), CellField + (bX ? TEXT("/x") : TEXT("/y")),
                            bX ? X : Y, FString::Printf(TEXT("integer in [0, %d)"), bX ? MapSize.X : MapSize.Y)));
                    }
                    if (TileSetRef->GetKind() != EUeAgentValueKind::String || TileSetRef->GetText().IsEmpty())
                        return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tilemap_cell_value_invalid"),
                            TEXT("The cell requires a nonempty tile set reference."), CellField + TEXT("/tile_set_ref"), TileSetRef, TEXT("PaperTileSet reference")));
                    if (!TryReadInt32(*TileIndexValue, Cell.TileIndex, CellField + TEXT("/tile_index"), OutError)) return false;
                    if (Cell.TileIndex < 0 || Cell.TileIndex > static_cast<int32>(EPaperTileFlags::TileIndexMask))
                        return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tilemap_cell_value_invalid"),
                            TEXT("The tile index is outside the range supported by Paper tile flags."), CellField + TEXT("/tile_index"),
                            TileIndexValue, FString::Printf(TEXT("integer in [0, %d]"), static_cast<int32>(EPaperTileFlags::TileIndexMask))));
                    if (!TryReadBool(CellValue, TEXT("flip_horizontal"), Cell.bFlipHorizontal, CellField, OutError)
                        || !TryReadBool(CellValue, TEXT("flip_vertical"), Cell.bFlipVertical, CellField, OutError)
                        || !TryReadBool(CellValue, TEXT("flip_diagonal"), Cell.bFlipDiagonal, CellField, OutError)) return false;
					const uint64 CoordinateKey =
						(static_cast<uint64>(
							static_cast<uint32>(Cell.Y))
							<< 32)
						| static_cast<uint32>(Cell.X);
					if (SeenCells.Contains(CoordinateKey))
					{
                        auto Actual = MakeShared<FJsonObject>();
                        Actual->SetNumberField(TEXT("x"), Cell.X);
                        Actual->SetNumberField(TEXT("y"), Cell.Y);
                        return Fail(OutError, JsonFailure(TEXT("asset_data_paper_tilemap_cell_coordinate_duplicate"),
                            TEXT("Another cell in this layer uses the same coordinates."), CellField, MakeShared<FJsonValueObject>(Actual),
                            MakeShared<FJsonValueString>(TEXT("unique cell coordinates within the layer"))));
					}
					SeenCells.Add(CoordinateKey);
					UObject* Resolved = nullptr;
                    if (!Resolver.TryResolveExisting(TileSetRef->GetText(), Resolved, OutError))
                    {
                        if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = CellField + TEXT("/tile_set_ref");
                        return false;
                    }
					UPaperTileSet* TileSet =
						Cast<UPaperTileSet>(Resolved);
					if (!TileSet)
					{
                        auto Actual = MakeShared<FJsonObject>();
                        Actual->SetStringField(TEXT("reference"), TileSetRef->GetText());
                        Actual->SetField(TEXT("resolved_class"), ObjectClassValue(Resolved));
                        return Fail(OutError, JsonFailure(TEXT("asset_data_paper_tilemap_tile_set_class_invalid"),
                            TEXT("The cell reference does not resolve to a Paper Tile Set."), CellField + TEXT("/tile_set_ref"),
                            MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("PaperTileSet"))));
					}
					const int32 TileCount =
						TileSet->GetTileCount();
					if (TileCount <= 0
						|| Cell.TileIndex >= TileCount)
					{
                        auto Actual = MakeShared<FJsonObject>();
                        Actual->SetStringField(TEXT("tile_set"), TileSet->GetPathName());
                        Actual->SetNumberField(TEXT("tile_index"), Cell.TileIndex);
                        Actual->SetNumberField(TEXT("tile_count"), TileCount);
                        return Fail(OutError, JsonFailure(TEXT("asset_data_paper_tilemap_tile_index_out_of_range"),
                            TileCount <= 0 ? TEXT("The referenced tile set has no available tiles.")
                                : TEXT("The cell tile index is outside the referenced tile set."),
                            CellField + TEXT("/tile_index"), MakeShared<FJsonValueObject>(Actual),
                            MakeShared<FJsonValueString>(FString::Printf(TEXT("integer in [0, %d)"), TileCount))));
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
            const FPaperTileMapLayerState& State, const FString& Field, FExecutionMutationError& OutError)
        {
            Layer.LayerName = FText::FromString(State.Name);
            Layer.SetShouldRenderInEditor(State.bVisibleInEditor);
            ON_SCOPE_EXIT { if (!OutError.IsEmpty()) { OutError.MutationMayHaveStarted = true;
                if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Layer.GetPathName(); } };
            if (!TrySetExactBoolProperty(Layer, TEXT("bHiddenInGame"), !State.bVisibleInGame,
                Field + TEXT("/visible_in_game"), OutError)) return false;
			Layer.SetLayerCollides(State.bCollides);
			Layer.SetLayerCollisionThickness(
				State.bOverrideThickness,
				State.Thickness);
			Layer.SetLayerCollisionOffset(
				State.bOverrideOffset,
				State.Offset);
			Layer.SetLayerColor(State.Color);
            for (int32 Index = 0; Index < State.Cells.Num(); ++Index)
            {
                const FPaperTileMapCellState& CellState = State.Cells[Index];
                UPaperTileSet* TileSet = CellState.TileSet.Get();
                if (!TileSet)
                {
                    auto Actual = MakeShared<FJsonObject>();
                    Actual->SetNumberField(TEXT("x"), CellState.X);
                    Actual->SetNumberField(TEXT("y"), CellState.Y);
                    Actual->SetField(TEXT("tile_set"), MakeShared<FJsonValueNull>());
                    return Fail(OutError, NativeFailure(TEXT("asset_data_paper_tilemap_layer_write_failed"),
                        TEXT("The prepared cell tile set is no longer available."),
                        Field + TEXT("/cells/") + FString::FromInt(Index) + TEXT("/tile_set_ref"),
                        MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("live PaperTileSet"))));
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

        bool ValidateTileSetPrivateBindings(FExecutionMutationError& OutError)
        {
            const UClass* Class = UPaperTileSet::StaticClass();
            const TCHAR* ArrayNames[] = {TEXT("Terrains"), TEXT("PerTileData")};
            const UScriptStruct* Structs[] = {FPaperTileSetTerrain::StaticStruct(), FPaperTileMetadata::StaticStruct()};
            for (int32 Index = 0; Index < 2; ++Index)
            {
                const FProperty* Raw = FindFProperty<FProperty>(Class, ArrayNames[Index]);
                const FArrayProperty* Array = CastField<FArrayProperty>(Raw);
                const FStructProperty* Inner = Array ? CastField<FStructProperty>(Array->Inner) : nullptr;
                if (!Inner || Inner->Struct != Structs[Index])
                    return Fail(OutError, NativeFailure(TEXT("asset_data_paper_tileset_private_binding_invalid"),
                        TEXT("The native tile set array is missing or has an incompatible element type."), ArrayNames[Index],
                        MakeShared<FJsonValueString>(Raw ? Raw->GetCPPType(nullptr, 0) : TEXT("missing")),
                        MakeShared<FJsonValueString>(TEXT("array of ") + Structs[Index]->GetPathName())));
            }
            const TCHAR* IntNames[] = {TEXT("WidthInTiles"), TEXT("HeightInTiles"), TEXT("AllocatedWidth"), TEXT("AllocatedHeight")};
            for (const TCHAR* Name : IntNames)
            {
                const FProperty* Raw = FindFProperty<FProperty>(Class, Name);
                if (!CastField<FIntProperty>(Raw))
                    return Fail(OutError, NativeFailure(TEXT("asset_data_paper_tileset_private_binding_invalid"),
                        TEXT("The native tile set integer property is missing or has an incompatible type."), Name,
                        MakeShared<FJsonValueString>(Raw ? Raw->GetCPPType(nullptr, 0) : TEXT("missing")),
                        MakeShared<FJsonValueString>(TEXT("int32 property"))));
            }
            return true;
        }

		template<typename ElementType>
		bool TryReadExactStructArray(
			const UObject& Object,
			const FName PropertyName,
			UScriptStruct* ExpectedStruct,
            TArray<ElementType>& OutValues, FExecutionMutationError& OutError)
        {
            const FProperty* Raw = FindFProperty<FProperty>(Object.GetClass(), PropertyName);
            const FArrayProperty* Property = CastField<FArrayProperty>(Raw);
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
                OutError = NativeFailure(TEXT("asset_data_paper_tileset_struct_array_read_invalid"),
                    !Inner || Inner->Struct != ExpectedStruct
                        ? TEXT("The native tile set array is missing or has an incompatible element type.") : TEXT("The native tile set array storage is unavailable."),
                    PropertyName.ToString(), MakeShared<FJsonValueString>(Raw ? Raw->GetCPPType(nullptr, 0) : TEXT("missing")),
                    MakeShared<FJsonValueString>(TEXT("array of ") + ExpectedStruct->GetPathName()));
                OutError.TargetPath = Object.GetPathName();
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
			const TArray<ElementType>& Values, FExecutionMutationError& OutError)
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
                OutError = NativeFailure(TEXT("asset_data_paper_tileset_struct_array_write_invalid"),
                    !Property ? TEXT("The native tile set array property is missing.")
                        : !Inner ? TEXT("The native tile set array does not contain structs.")
                            : Inner->Struct != ExpectedStruct ? TEXT("The native tile set array has an incompatible element structure.")
                                : TEXT("The native tile set array has no writable storage."),
                    PropertyName.ToString(),
                    MakeShared<FJsonValueString>(Inner && Inner->Struct ? Inner->Struct->GetPathName()
                        : Property ? Property->GetCPPType(nullptr, 0) : TEXT("missing")),
                    MakeShared<FJsonValueString>(ExpectedStruct ? ExpectedStruct->GetPathName() : TEXT("struct array")));
                OutError.TargetPath = Object.GetPathName();
                return false;
			}
			*TargetValues = Values;
			return true;
		}

		bool TryReadExactIntProperty(
			const UObject& Object,
			const FName PropertyName,
            int32& OutValue, FExecutionMutationError& OutError)
        {
            const FProperty* Raw = FindFProperty<FProperty>(Object.GetClass(), PropertyName);
            const FIntProperty* Property = CastField<FIntProperty>(Raw);
            if (!Property)
            {
                OutError = NativeFailure(TEXT("asset_data_paper_tileset_int_property_read_invalid"),
                    TEXT("The native tile set integer property is missing or has an incompatible type."), PropertyName.ToString(),
                    MakeShared<FJsonValueString>(Raw ? Raw->GetCPPType(nullptr, 0) : TEXT("missing")),
                    MakeShared<FJsonValueString>(TEXT("int32 property")));
                OutError.TargetPath = Object.GetPathName();
                return false;
            }
			OutValue =
				Property->GetPropertyValue_InContainer(&Object);
			return true;
		}

		bool TryWriteExactIntProperty(
			UObject& Object,
			const FName PropertyName,
			const int32 Value, FExecutionMutationError& OutError)
		{
			FIntProperty* Property =
				FindFProperty<FIntProperty>(
					Object.GetClass(),
					PropertyName);
			if (!Property)
			{
                OutError = NativeFailure(TEXT("asset_data_paper_tileset_int_property_write_invalid"),
                    TEXT("The native tile set integer property is missing or has an incompatible type."), PropertyName.ToString(),
                    MakeShared<FJsonValueString>(TEXT("missing or incompatible property")),
                    MakeShared<FJsonValueString>(TEXT("int32 property")));
                OutError.TargetPath = Object.GetPathName();
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
			FPaperTileSetPrivateState& OutState, FExecutionMutationError& OutError)
		{
			return TryReadExactStructArray(
					TileSet,
					TEXT("Terrains"),
					FPaperTileSetTerrain::StaticStruct(),
					OutState.Terrains, OutError)
				&& TryReadExactStructArray(
					TileSet,
					TEXT("PerTileData"),
					FPaperTileMetadata::StaticStruct(),
					OutState.PerTileData, OutError)
				&& TryReadExactIntProperty(
					TileSet,
					TEXT("WidthInTiles"),
					OutState.WidthInTiles, OutError)
				&& TryReadExactIntProperty(
					TileSet,
					TEXT("HeightInTiles"),
					OutState.HeightInTiles, OutError)
				&& TryReadExactIntProperty(
					TileSet,
					TEXT("AllocatedWidth"),
					OutState.AllocatedWidth, OutError)
				&& TryReadExactIntProperty(
					TileSet,
					TEXT("AllocatedHeight"),
					OutState.AllocatedHeight, OutError);
		}

		bool TryWritePaperTileSetPrivateState(
			UPaperTileSet& TileSet,
			const FPaperTileSetPrivateState& State, FExecutionMutationError& OutError)
		{
			return TryWriteExactStructArray(
					TileSet,
					TEXT("Terrains"),
					FPaperTileSetTerrain::StaticStruct(),
					State.Terrains, OutError)
				&& TryWriteExactStructArray(
					TileSet,
					TEXT("PerTileData"),
					FPaperTileMetadata::StaticStruct(),
					State.PerTileData, OutError)
				&& TryWriteExactIntProperty(
					TileSet,
					TEXT("WidthInTiles"),
					State.WidthInTiles, OutError)
				&& TryWriteExactIntProperty(
					TileSet,
					TEXT("HeightInTiles"),
					State.HeightInTiles, OutError)
				&& TryWriteExactIntProperty(
					TileSet,
					TEXT("AllocatedWidth"),
					State.AllocatedWidth, OutError)
				&& TryWriteExactIntProperty(
					TileSet,
					TEXT("AllocatedHeight"),
					State.AllocatedHeight, OutError);
		}

		bool TryReadPaperTileSetTerrains(
			const FUeAgentCanonicalValue& Value,
			TArray<FPaperTileSetTerrain>& OutTerrains,
			FExecutionMutationError& OutError)
		{
			OutTerrains.Reset();
            if (Value.GetKind() != EUeAgentValueKind::Array)
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tileset_terrain_invalid"),
                    TEXT("Tile set terrain must be an array."), TEXT("terrain"), &Value, TEXT("array")));
            if (Value.GetArrayItems().Num() > 254)
                return Fail(OutError, JsonFailure(TEXT("asset_data_paper_tileset_terrain_invalid"),
                    TEXT("A tile set supports at most 254 terrains."), TEXT("terrain"),
                    MakeShared<FJsonValueNumber>(Value.GetArrayItems().Num()), MakeShared<FJsonValueString>(TEXT("at most 254 entries"))));
			TSet<FString> SeenNames;
			for (int32 Index = 0;
				Index < Value.GetArrayItems().Num();
				++Index)
			{
				const FUeAgentCanonicalValue& TerrainValue =
					Value.GetArrayItems()[Index].Get();
                const FString Field = TEXT("terrain/") + FString::FromInt(Index);
                if (TerrainValue.GetKind() != EUeAgentValueKind::Object || TerrainValue.GetObjectItems().Num() != 2)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tileset_terrain_shape_invalid"),
                        TEXT("A terrain must contain exactly name and center_tile_index."), Field, &TerrainValue,
                        TEXT("object with name and center_tile_index")));
				const FUeAgentCanonicalValue* Name =
					FindObjectField(TerrainValue, TEXT("name"));
				const FUeAgentCanonicalValue* Center =
					FindObjectField(
						TerrainValue,
						TEXT("center_tile_index"));
				FPaperTileSetTerrain Terrain;
                if (!Name || Name->GetKind() != EUeAgentValueKind::String || Name->GetText().TrimStartAndEnd().IsEmpty())
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tileset_terrain_value_invalid"),
                        TEXT("The terrain name must be a nonblank string."), Field + TEXT("/name"), Name, TEXT("nonblank string")));
                if (!Center)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tileset_terrain_value_invalid"),
                        TEXT("The terrain center tile index is missing."), Field + TEXT("/center_tile_index"), nullptr, TEXT("integer >= 0")));
                if (!TryReadInt32(*Center, Terrain.CenterTileIndex, Field + TEXT("/center_tile_index"), OutError)) return false;
                if (Terrain.CenterTileIndex < 0)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tileset_terrain_value_invalid"),
                        TEXT("The terrain center tile index must be nonnegative."), Field + TEXT("/center_tile_index"), Center, TEXT("integer >= 0")));
				Terrain.TerrainName = Name->GetText();
				FString CanonicalName =
					Terrain.TerrainName
						.TrimStartAndEnd()
						.ToLower();
				if (SeenNames.Contains(CanonicalName))
				{
                    return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tileset_terrain_name_duplicate"),
                        TEXT("Another terrain has the same name after trimming whitespace and ignoring case."),
                        Field + TEXT("/name"), Name, TEXT("unique terrain name")));
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
            FExecutionMutationError& OutError,
            const bool bAuthoredTileSize, const bool bAuthoredMargin, const bool bAuthoredSpacing)
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
            const TCHAR* Fields[] = {TEXT("imported_size/x"), TEXT("imported_size/y"), TEXT("tile_size/x"), TEXT("tile_size/y"),
                TEXT("margin/left"), TEXT("margin/top"), TEXT("margin/right"), TEXT("margin/bottom"), TEXT("spacing/x"), TEXT("spacing/y")};
            const int32 Values[] = {ImportedSize.X, ImportedSize.Y, TileSize.X, TileSize.Y,
                Margin.Left, Margin.Top, Margin.Right, Margin.Bottom, Spacing.X, Spacing.Y};
            for (int32 Index = 0; Index < 10; ++Index)
            {
                if (Values[Index] < (Index < 4 ? 1 : 0))
                {
                    const bool bAuthored = Index >= 2 && (Index < 4 ? bAuthoredTileSize : Index < 8 ? bAuthoredMargin : bAuthoredSpacing);
                    // Authored margin and spacing are scalar values; native captures retain their components.
                    const FString Field = bAuthored && Index >= 4 ? (Index < 8 ? TEXT("margin") : TEXT("spacing")) : Fields[Index];
                    OutError = JsonFailure(TEXT("asset_data_paper_tileset_layout_invalid"),
                        Index < 4 ? TEXT("The tile layout dimension must be positive.") : TEXT("The tile layout margin or spacing must be nonnegative."),
                        Field, MakeShared<FJsonValueNumber>(Values[Index]),
                        MakeShared<FJsonValueString>(Index < 4 ? TEXT("integer >= 1") : TEXT("integer >= 0")),
                        bAuthored ? TEXT("authored_value") : TEXT("native_result"));
                    if (Index < 2) OutError.TargetPath = Texture->GetPathName();
                    return false;
                }
            }
            auto LayoutValues = [&]()
            {
                auto Actual = MakeShared<FJsonObject>();
                Actual->SetStringField(TEXT("texture"), Texture->GetPathName());
                Actual->SetNumberField(TEXT("texture_width"), ImportedSize.X);
                Actual->SetNumberField(TEXT("texture_height"), ImportedSize.Y);
                Actual->SetNumberField(TEXT("tile_width"), TileSize.X);
                Actual->SetNumberField(TEXT("tile_height"), TileSize.Y);
                Actual->SetNumberField(TEXT("margin_left"), Margin.Left);
                Actual->SetNumberField(TEXT("margin_top"), Margin.Top);
                Actual->SetNumberField(TEXT("margin_right"), Margin.Right);
                Actual->SetNumberField(TEXT("margin_bottom"), Margin.Bottom);
                Actual->SetNumberField(TEXT("spacing_x"), Spacing.X);
                Actual->SetNumberField(TEXT("spacing_y"), Spacing.Y);
                return Actual;
            };
            if (NumeratorX <= 0 || NumeratorY <= 0 || DenominatorX <= 0 || DenominatorY <= 0)
            {
                const bool bNumerator = NumeratorX <= 0 || NumeratorY <= 0;
                const bool bX = bNumerator ? NumeratorX <= 0 : DenominatorX <= 0;
                auto Actual = LayoutValues();
                Actual->SetStringField(TEXT("axis"), bX ? TEXT("x") : TEXT("y"));
                return Fail(OutError, NativeFailure(TEXT("asset_data_paper_tileset_layout_invalid"),
                    bNumerator ? TEXT("The texture dimensions and margins leave no positive space for the tile layout.")
                        : TEXT("The combined tile size and spacing must be positive."),
                    TEXT("layout"), MakeShared<FJsonValueObject>(Actual),
                    MakeShared<FJsonValueString>(bNumerator ? TEXT("texture size - both margins + spacing > 0") : TEXT("tile size + spacing > 0"))));
            }
			const int64 CountX = NumeratorX / DenominatorX;
			const int64 CountY = NumeratorY / DenominatorY;
			if (CountX <= 0
				|| CountY <= 0
				|| CountX > MAX_int32
				|| CountY > MAX_int32
				|| CountX * CountY > MAX_int32)
			{
                auto Actual = LayoutValues();
                Actual->SetField(TEXT("tile_count_x"), MakeShared<FJsonValueNumberString>(FString::Printf(TEXT("%lld"), CountX)));
                Actual->SetField(TEXT("tile_count_y"), MakeShared<FJsonValueNumberString>(FString::Printf(TEXT("%lld"), CountY)));
                return Fail(OutError, NativeFailure(TEXT("asset_data_paper_tileset_tile_count_invalid"),
                    CountX <= 0 || CountY <= 0 ? TEXT("The texture layout does not contain at least one complete tile on each axis.")
                        : TEXT("The computed tile count exceeds int32 range."),
                    TEXT("layout"), MakeShared<FJsonValueObject>(Actual),
                    MakeShared<FJsonValueString>(TEXT("positive tile counts and total tiles <= 2147483647"))));
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
        ON_SCOPE_EXIT { if (!OutError.IsEmpty() && OutError.TargetPath.IsEmpty() && Target)
            OutError.TargetPath = Target->GetPathName(); };
        if (!TileMap)
            return Fail(OutError, NativeFailure(TEXT("asset_data_paper_tilemap_invalid"),
                TEXT("The replacement target is unavailable or is not a Paper Tile Map."), FString(), ObjectClassValue(Target),
                MakeShared<FJsonValueString>(TEXT("PaperTileMap"))));
        const TCHAR* Fields[] = {TEXT("map_size/x"), TEXT("map_size/y"), TEXT("tile_size/x"), TEXT("tile_size/y")};
        const int32 Dimensions[] = {TileMap->MapWidth, TileMap->MapHeight, TileMap->TileWidth, TileMap->TileHeight};
        for (int32 Index = 0; Index < 4; ++Index)
            if (Dimensions[Index] <= 0)
                return Fail(OutError, NativeFailure(TEXT("asset_data_paper_tilemap_invalid"),
                    TEXT("The native tile map dimension must be positive."), Fields[Index], MakeShared<FJsonValueNumber>(Dimensions[Index]),
                    MakeShared<FJsonValueString>(TEXT("integer >= 1"))));
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
        for (int32 Index = 0; Index < TileMap->TileLayers.Num(); ++Index)
        {
            UPaperTileLayer* Layer = TileMap->TileLayers[Index];
            if (!Layer)
                return Fail(OutError, NativeFailure(TEXT("asset_data_paper_tilemap_existing_layer_invalid"),
                    TEXT("The existing tile map layer is unavailable."), TEXT("layers/") + FString::FromInt(Index),
                    ObjectClassValue(Layer), MakeShared<FJsonValueString>(TEXT("live PaperTileLayer"))));
			Data->OriginalLayers.Emplace(Layer);
		}
		if (MapSize)
		{
			Data->bChangeMapSize = true;
            if (!TryReadExtent2i(*MapSize, 1, 1024, Data->NewMapSize, TEXT("map_size"), OutError)) return false;
		}
		if (TileSize)
		{
			Data->bChangeTileSize = true;
            if (!TryReadExtent2i(*TileSize, 1, MAX_int32, Data->NewTileSize, TEXT("tile_size"), OutError)) return false;
		}
		if (Layers)
		{
			Data->bChangeLayers = true;
            if (!ValidateLayerPrivateBindings(OutError)) return false;
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
        ON_SCOPE_EXIT { if (!OutError.IsEmpty() && OutError.TargetPath.IsEmpty() && Target)
            OutError.TargetPath = Target->GetPathName(); };
        if (!TileMap)
            return Fail(OutError, NativeFailure(TEXT("asset_data_paper_tilemap_state_invalid"),
                TEXT("The replacement target is unavailable or is not a Paper Tile Map."), FString(), ObjectClassValue(Target),
                MakeShared<FJsonValueString>(TEXT("PaperTileMap"))));
        if (!State.Data.IsValid())
            return Fail(OutError, NativeFailure(TEXT("asset_data_paper_tilemap_state_invalid"),
                TEXT("The prepared tile map replacement state is unavailable."), FString(), MakeShared<FJsonValueBoolean>(false),
                MakeShared<FJsonValueString>(TEXT("available prepared replacement state"))));
		const FPaperTileMapPreparedData& Data = *State.Data;
		TArray<TObjectPtr<UPaperTileLayer>> ReplacementLayers;
		if (Data.bChangeLayers)
		{
			ReplacementLayers.Reserve(Data.NewLayers.Num());
            for (int32 Index = 0; Index < Data.NewLayers.Num(); ++Index)
            {
                const FPaperTileMapLayerState& LayerState = Data.NewLayers[Index];
                const FString Field = TEXT("layers/") + FString::FromInt(Index);
				UPaperTileLayer* Layer =
					NewObject<UPaperTileLayer>(
						TileMap,
						NAME_None,
						RF_Transactional);
				if (!Layer)
				{
                    return Fail(OutError, NativeFailure(TEXT("asset_data_paper_tilemap_layer_create_failed"),
                        TEXT("A replacement tile layer could not be created."), Field, ObjectClassValue(Layer),
                        MakeShared<FJsonValueString>(TEXT("live PaperTileLayer"))));
				}
				Layer->DestructiveAllocateMap(
					Data.NewMapSize.X,
					Data.NewMapSize.Y);
                if (!TryWritePaperTileMapLayerState(*Layer, LayerState, Field, OutError)) return false;
				ReplacementLayers.Add(Layer);
			}
		}
		else if (Data.bChangeMapSize)
		{
			ReplacementLayers.Reserve(
				Data.OriginalLayers.Num());
            for (int32 Index = 0; Index < Data.OriginalLayers.Num(); ++Index)
            {
                const TStrongObjectPtr<UPaperTileLayer>& OriginalLayer = Data.OriginalLayers[Index];
				UPaperTileLayer* Layer = DuplicateObject<
					UPaperTileLayer>(
						OriginalLayer.Get(),
						TileMap);
				if (!Layer)
				{
                    return Fail(OutError, NativeFailure(TEXT("asset_data_paper_tilemap_layer_clone_failed"),
                        TEXT("The existing tile layer could not be duplicated for resizing."),
                        TEXT("layers/") + FString::FromInt(Index),
                        OriginalLayer.IsValid() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(OriginalLayer->GetPathName()))
                            : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
                        MakeShared<FJsonValueString>(TEXT("duplicated PaperTileLayer"))));
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
		const FPaperTileMapPreparedState& State,
		FExecutionMutationError& OutError)
	{
        ON_SCOPE_EXIT { if (!OutError.IsEmpty() && Target && OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target->GetPathName(); };
		UPaperTileMap* TileMap = Cast<UPaperTileMap>(Target);
		if (!TileMap || !State.Data.IsValid())
		{
            return Fail(OutError, NativeFailure(TEXT("asset_data_paper_tilemap_rollback_state_invalid"),
                !TileMap ? TEXT("Tile map rollback requires a PaperTileMap target.") : TEXT("Tile map rollback has no captured state."),
                !TileMap ? TEXT("target") : TEXT("prepared_state"),
                !TileMap ? ObjectClassValue(Target) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)),
                MakeShared<FJsonValueString>(!TileMap ? UPaperTileMap::StaticClass()->GetPathName() : TEXT("captured rollback state"))));
		}
		const FPaperTileMapPreparedData& Data = *State.Data;
		TArray<TObjectPtr<UPaperTileLayer>> OriginalLayers;
		OriginalLayers.Reserve(Data.OriginalLayers.Num());
		for (int32 Index = 0; Index < Data.OriginalLayers.Num(); ++Index)
		{
            const TStrongObjectPtr<UPaperTileLayer>& Layer = Data.OriginalLayers[Index];
			if (!Layer.Get())
			{
                OutError = NativeFailure(TEXT("asset_data_paper_tilemap_rollback_layer_unavailable"),
                    TEXT("A captured tile map layer is no longer available."), TEXT("layers/") + FString::FromInt(Index),
                    MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(UPaperTileLayer::StaticClass()->GetPathName()));
                OutError.OperationIndex = Index;
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
        ON_SCOPE_EXIT { if (!OutError.IsEmpty() && OutError.TargetPath.IsEmpty() && Target)
            OutError.TargetPath = Target->GetPathName(); };
        if (!TileSet)
            return Fail(OutError, NativeFailure(TEXT("asset_data_paper_tileset_invalid"),
                TEXT("The replacement target is unavailable or is not a Paper Tile Set."), FString(), ObjectClassValue(Target),
                MakeShared<FJsonValueString>(TEXT("PaperTileSet"))));
        if (!ValidateTileSetPrivateBindings(OutError)) return false;
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
        if (!TryReadPaperTileSetPrivateState(*TileSet, Data->OriginalPrivateState, OutError)) return false;
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
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tileset_source_ref_invalid"),
                    TEXT("The source texture reference must be a nonempty string."), TEXT("source_texture_ref"),
                    SourceTextureRef, TEXT("Texture2D reference")));
			}
			UObject* Resolved = nullptr;
            if (!Resolver.TryResolveExisting(SourceTextureRef->GetText(), Resolved, OutError))
            {
                if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = TEXT("source_texture_ref");
                return false;
            }
			UTexture2D* Texture = Cast<UTexture2D>(Resolved);
			if (!Texture)
			{
                auto Actual = MakeShared<FJsonObject>();
                Actual->SetStringField(TEXT("reference"), SourceTextureRef->GetText());
                Actual->SetField(TEXT("resolved_class"), ObjectClassValue(Resolved));
                return Fail(OutError, JsonFailure(TEXT("asset_data_paper_tileset_source_class_invalid"),
                    TEXT("The source reference does not resolve to a 2D texture."), TEXT("source_texture_ref"),
                    MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("Texture2D"))));
			}
			Data->NewTexture =
				TStrongObjectPtr<UTexture2D>(Texture);
		}
        if (TileSize && !TryReadExtent2i(*TileSize, 1, MAX_int32, Data->NewTileSize, TEXT("tile_size"), OutError)) return false;
		if (Margin)
		{
			int32 UniformMargin = 0;
            if (!TryReadInt32(*Margin, UniformMargin, TEXT("margin"), OutError)) return false;
            if (UniformMargin < 0)
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tileset_margin_invalid"),
                    TEXT("The tile set margin must be nonnegative."), TEXT("margin"), Margin, TEXT("integer >= 0")));
			Data->NewMargin = FIntMargin(UniformMargin);
		}
		if (Spacing)
		{
			int32 UniformSpacing = 0;
            if (!TryReadInt32(*Spacing, UniformSpacing, TEXT("spacing"), OutError)) return false;
            if (UniformSpacing < 0)
                return Fail(OutError, ValueFailure(TEXT("asset_data_paper_tileset_spacing_invalid"),
                    TEXT("The tile set spacing must be nonnegative."), TEXT("spacing"), Spacing, TEXT("integer >= 0")));
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
                OutError, TileSize != nullptr, Margin != nullptr, Spacing != nullptr))
		{
			return false;
		}
		const int64 TotalTiles =
			static_cast<int64>(TileCount.X) * TileCount.Y;
        for (int32 Index = 0; Index < Data->NewTerrains.Num(); ++Index)
        {
            const FPaperTileSetTerrain& TerrainState = Data->NewTerrains[Index];
			if (TerrainState.CenterTileIndex < 0
				|| TerrainState.CenterTileIndex >= TotalTiles)
			{
                auto Actual = MakeShared<FJsonObject>();
                Actual->SetStringField(TEXT("terrain"), TerrainState.TerrainName);
                Actual->SetNumberField(TEXT("center_tile_index"), TerrainState.CenterTileIndex);
                Actual->SetField(TEXT("tile_count"), MakeShared<FJsonValueNumberString>(FString::Printf(TEXT("%lld"), TotalTiles)));
                return Fail(OutError, JsonFailure(TEXT("asset_data_paper_tileset_terrain_center_out_of_range"),
                    TEXT("The terrain center index is outside the resulting tile set."),
                    TEXT("terrain/") + FString::FromInt(Index) + TEXT("/center_tile_index"),
                    MakeShared<FJsonValueObject>(Actual),
                    MakeShared<FJsonValueString>(FString::Printf(TEXT("integer in [0, %lld)"), TotalTiles)),
                    Terrain ? TEXT("authored_value") : TEXT("native_result")));
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
        ON_SCOPE_EXIT { if (!OutError.IsEmpty() && OutError.TargetPath.IsEmpty() && Target)
            OutError.TargetPath = Target->GetPathName(); };
        if (!TileSet || !State.Data.IsValid())
            return Fail(OutError, NativeFailure(TEXT("asset_data_paper_tileset_state_invalid"),
                !TileSet ? TEXT("The replacement target is unavailable or is not a Paper Tile Set.")
                    : TEXT("The prepared tile set replacement state is unavailable."),
                !TileSet ? TEXT("target") : TEXT("prepared_state"),
                !TileSet ? ObjectClassValue(Target) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)),
                MakeShared<FJsonValueString>(!TileSet ? TEXT("PaperTileSet") : TEXT("available prepared replacement state"))));
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
				Data.NewTerrains, OutError))
		{
            OutError.MutationMayHaveStarted = true;
            return false;
		}
		TileSet->PostEditChange();
		return true;
	}

	bool TryRollbackPaperTileSetReplacement(
		UObject* Target,
		const FPaperTileSetPreparedState& State,
		FExecutionMutationError& OutError)
	{
        ON_SCOPE_EXIT { if (!OutError.IsEmpty() && Target && OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target->GetPathName(); };
		UPaperTileSet* TileSet = Cast<UPaperTileSet>(Target);
		if (!TileSet || !State.Data.IsValid())
		{
            return Fail(OutError, NativeFailure(TEXT("asset_data_paper_tileset_rollback_state_invalid"),
                !TileSet ? TEXT("Tile set rollback requires a PaperTileSet target.") : TEXT("Tile set rollback has no captured state."),
                !TileSet ? TEXT("target") : TEXT("prepared_state"),
                !TileSet ? ObjectClassValue(Target) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)),
                MakeShared<FJsonValueString>(!TileSet ? UPaperTileSet::StaticClass()->GetPathName() : TEXT("captured rollback state"))));
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
				Data.OriginalPrivateState.Terrains, OutError))
		{
			OutError.MutationMayHaveStarted = true;
            return false;
		}
		TileSet->PostEditChange();
        if (!TryWritePaperTileSetPrivateState(*TileSet, Data.OriginalPrivateState, OutError))
        {
            OutError.MutationMayHaveStarted = true;
            return false;
        }
        return true;
	}
}
