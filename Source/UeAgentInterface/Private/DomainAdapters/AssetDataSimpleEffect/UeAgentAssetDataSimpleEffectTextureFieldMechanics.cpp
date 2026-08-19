#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectTextureFieldMechanics.h"

#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureCollection.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Particles/SubUVAnimation.h"

namespace UE::UAI::DomainAdapters::AssetDataSimpleEffect
{
	using namespace ContextResource;
	using namespace ExecutionMutation;
	using namespace StateSemantic;

	namespace
	{
		const TCHAR* RenderTargetMechanicId =
			TEXT("uai.asset_data.mechanic:update_render_target:v1");
		const TCHAR* SubUVMechanicId =
			TEXT("uai.asset_data.mechanic:update_subuv_animation:v1");
		const TCHAR* TextureCollectionMechanicId =
			TEXT("uai.asset_data.mechanic:update_texture_collection:v1");

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
						? TEXT("asset_data_texture_projection_string_invalid")
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
						? TEXT("asset_data_texture_projection_boolean_invalid")
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
						? TEXT("asset_data_texture_projection_integer_invalid")
						: StateError.Code,
					StateError.Detail);
		}

		bool TryBuildDecimal(
			const float Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentStateSemanticError StateError;
			return FMath::IsFinite(Value)
				&& FUeAgentCanonicalValue::TryDecimal(
					FString::SanitizeFloat(static_cast<double>(Value)),
					Resources,
					OutValue,
					StateError)
				? true
				: Fail(
					OutError,
					StateError.Code.IsEmpty()
						? TEXT("asset_data_texture_projection_decimal_invalid")
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
						? TEXT("asset_data_texture_projection_null_invalid")
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
						? TEXT("asset_data_texture_projection_array_invalid")
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
						? TEXT("asset_data_texture_projection_object_invalid")
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

		bool TryReadExtent2i(
			const FUeAgentCanonicalValue& Value,
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
				&& OutExtent.X > 0
				&& OutExtent.Y > 0;
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
				|| !TryReadNumber(*A, Alpha))
			{
				return false;
			}
			OutColor = FLinearColor(
				static_cast<float>(Red),
				static_cast<float>(Green),
				static_cast<float>(Blue),
				static_cast<float>(Alpha));
			return FMath::IsFinite(OutColor.R)
				&& FMath::IsFinite(OutColor.G)
				&& FMath::IsFinite(OutColor.B)
				&& FMath::IsFinite(OutColor.A);
		}

		bool TryGetRenderTargetFormatName(
			const ETextureRenderTargetFormat Format,
			FString& OutName)
		{
			switch (Format)
			{
			case RTF_R8: OutName = TEXT("r8"); return true;
			case RTF_RG8: OutName = TEXT("rg8"); return true;
			case RTF_RGBA8: OutName = TEXT("rgba8"); return true;
			case RTF_RGBA8_SRGB: OutName = TEXT("rgba8_srgb"); return true;
			case RTF_R16f: OutName = TEXT("r16f"); return true;
			case RTF_RG16f: OutName = TEXT("rg16f"); return true;
			case RTF_RGBA16f: OutName = TEXT("rgba16f"); return true;
			case RTF_R32f: OutName = TEXT("r32f"); return true;
			case RTF_RG32f: OutName = TEXT("rg32f"); return true;
			case RTF_RGBA32f: OutName = TEXT("rgba32f"); return true;
			case RTF_RGB10A2: OutName = TEXT("rgb10a2"); return true;
			default:
				OutName.Reset();
				return false;
			}
		}

		bool TryNormalizeRenderTargetFormat(
			const FUeAgentCanonicalValue& Value,
			FString& OutName)
		{
			if (!TryReadString(Value, OutName))
			{
				return false;
			}
			OutName.TrimStartAndEndInline();
			OutName.ToLowerInline();
			return OutName == TEXT("r8")
				|| OutName == TEXT("rg8")
				|| OutName == TEXT("rgba8")
				|| OutName == TEXT("rgba8_srgb")
				|| OutName == TEXT("r16f")
				|| OutName == TEXT("rg16f")
				|| OutName == TEXT("rgba16f")
				|| OutName == TEXT("r32f")
				|| OutName == TEXT("rg32f")
				|| OutName == TEXT("rgba32f")
				|| OutName == TEXT("rgb10a2");
		}

		bool TryGetSubUVBoundingName(
			const ESubUVBoundingVertexCount Mode,
			FString& OutName)
		{
			if (Mode == BVC_FourVertices)
			{
				OutName = TEXT("four_vertices");
				return true;
			}
			if (Mode == BVC_EightVertices)
			{
				OutName = TEXT("eight_vertices");
				return true;
			}
			OutName.Reset();
			return false;
		}

		bool TryNormalizeSubUVBounding(
			const FUeAgentCanonicalValue& Value,
			FString& OutName)
		{
			if (!TryReadString(Value, OutName))
			{
				return false;
			}
			OutName.TrimStartAndEndInline();
			OutName.ToLowerInline();
			return OutName == TEXT("four_vertices")
				|| OutName == TEXT("eight_vertices");
		}

		bool TryBuildStringArray(
			const TArray<FString>& Strings,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			TArray<FUeAgentCanonicalValueRef> Items;
			Items.Reserve(Strings.Num());
			for (const FString& String : Strings)
			{
				FUeAgentCanonicalValuePtr Item;
				if (!TryBuildString(String, Resources, Item, OutError))
				{
					return false;
				}
				Items.Add(Item.ToSharedRef());
			}
			return TryBuildArray(
				MoveTemp(Items),
				Resources,
				OutValue,
				OutError);
		}

		bool TryReadStringArray(
			const FUeAgentCanonicalValue& Value,
			TArray<FString>& OutStrings)
		{
			OutStrings.Reset();
			if (Value.GetKind() != EUeAgentValueKind::Array)
			{
				return false;
			}
			TSet<FString> Seen;
			for (const FUeAgentCanonicalValueRef& Item
				: Value.GetArrayItems())
			{
				FString Text;
				if (!TryReadString(Item.Get(), Text)
					|| Seen.Contains(Text))
				{
					OutStrings.Reset();
					return false;
				}
				Seen.Add(Text);
				OutStrings.Add(MoveTemp(Text));
			}
			return true;
		}

		bool TryBuildRenderTargetProjectionMap(
			const UTextureRenderTarget2D* Target,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutValues,
			FExecutionMutationError& OutError)
		{
			OutValues.Reset();
			FString FormatName;
			FUeAgentCanonicalValuePtr Size;
			FUeAgentCanonicalValuePtr Format;
			FUeAgentCanonicalValuePtr ClearColor;
			FUeAgentCanonicalValuePtr AutoGenerateMips;
			if (!Target
				|| Target->SizeX < 1
				|| Target->SizeY < 1
				|| !TryGetRenderTargetFormatName(
					Target->RenderTargetFormat,
					FormatName)
				|| !TryBuildExtent2i(
					FIntPoint(Target->SizeX, Target->SizeY),
					Resources,
					Size,
					OutError)
				|| !TryBuildString(
					FormatName,
					Resources,
					Format,
					OutError)
				|| !TryBuildLinearColor(
					Target->ClearColor,
					Resources,
					ClearColor,
					OutError)
				|| !TryBuildBoolean(
					Target->bAutoGenerateMips,
					Resources,
					AutoGenerateMips,
					OutError))
			{
				return Fail(
					OutError,
					TEXT("asset_data_render_target_projection_invalid"));
			}
			OutValues.Add(TEXT("size"), Size.ToSharedRef());
			OutValues.Add(TEXT("format"), Format.ToSharedRef());
			OutValues.Add(TEXT("clear_color"), ClearColor.ToSharedRef());
			OutValues.Add(
				TEXT("auto_generate_mips"),
				AutoGenerateMips.ToSharedRef());
			return true;
		}

		bool TryBuildSubUVProjectionMap(
			const USubUVAnimation* SubUV,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutValues,
			FExecutionMutationError& OutError)
		{
			OutValues.Reset();
			FString BoundingName;
			FUeAgentCanonicalValuePtr Source;
			FUeAgentCanonicalValuePtr Grid;
			FUeAgentCanonicalValuePtr Bounding;
			if (!SubUV
				|| SubUV->SubImages_Horizontal < 1
				|| SubUV->SubImages_Vertical < 1
				|| !TryGetSubUVBoundingName(
					SubUV->BoundingMode,
					BoundingName)
				|| !TryBuildReference(
					SubUV->SubUVTexture.Get(),
					Resources,
					Source,
					OutError)
				|| !TryBuildExtent2i(
					FIntPoint(
						SubUV->SubImages_Horizontal,
						SubUV->SubImages_Vertical),
					Resources,
					Grid,
					OutError)
				|| !TryBuildString(
					BoundingName,
					Resources,
					Bounding,
					OutError))
			{
				return Fail(
					OutError,
					TEXT("asset_data_subuv_projection_invalid"));
			}
			OutValues.Add(TEXT("source_texture_ref"), Source.ToSharedRef());
			OutValues.Add(TEXT("grid"), Grid.ToSharedRef());
			OutValues.Add(TEXT("bounding_geometry"), Bounding.ToSharedRef());
			return true;
		}

		bool TryBuildTextureCollectionProjectionMap(
			const UTextureCollection* Collection,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutValues,
			FExecutionMutationError& OutError)
		{
			OutValues.Reset();
			if (!Collection)
			{
				return Fail(
					OutError,
					TEXT("asset_data_texture_collection_projection_invalid"));
			}
			TArray<FString> Paths;
			TSet<const UTexture*> Seen;
			Paths.Reserve(Collection->Textures.Num());
			for (const UTexture* Texture : Collection->Textures)
			{
				if (!Texture || Seen.Contains(Texture))
				{
					return Fail(
						OutError,
						TEXT(
							"asset_data_texture_collection_"
							"projection_member_invalid"));
				}
				Seen.Add(Texture);
				Paths.Add(Texture->GetPathName());
			}
			FUeAgentCanonicalValuePtr Refs;
			if (!TryBuildStringArray(Paths, Resources, Refs, OutError))
			{
				return false;
			}
			OutValues.Add(TEXT("texture_refs"), Refs.ToSharedRef());
			return true;
		}

		const TCHAR* FindContextTag(
			const FString& NativeMechanicId,
			const FString& FieldId)
		{
			if (NativeMechanicId == RenderTargetMechanicId)
			{
				if (FieldId == TEXT("size"))
				{
					return TEXT("render_target_extent");
				}
				if (FieldId == TEXT("format"))
				{
					return TEXT("render_target_format");
				}
				if (FieldId == TEXT("clear_color"))
				{
					return TEXT("texture_linear_color");
				}
				if (FieldId == TEXT("auto_generate_mips"))
				{
					return TEXT("texture_boolean");
				}
			}
			if (NativeMechanicId == SubUVMechanicId)
			{
				if (FieldId == TEXT("source_texture_ref"))
				{
					return TEXT("texture_2d_asset_ref");
				}
				if (FieldId == TEXT("grid"))
				{
					return TEXT("subuv_grid_extent");
				}
				if (FieldId == TEXT("bounding_geometry"))
				{
					return TEXT("subuv_bounding_geometry");
				}
			}
			if (NativeMechanicId == TextureCollectionMechanicId
				&& FieldId == TEXT("texture_refs"))
			{
				return TEXT("ordered_texture_asset_refs");
			}
			return nullptr;
		}

		bool TrySelectFields(
			const FString& NativeMechanicId,
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
					TEXT("asset_data_texture_selected_fields_missing"));
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
				const TCHAR* ContextTag =
					FindContextTag(NativeMechanicId, FieldId);
				FUeAgentCanonicalValuePtr Context;
				if (Seen.Contains(FieldId)
					|| !Current
					|| !Default
					|| !ContextTag
					|| !TryBuildString(
						ContextTag,
						Resources,
						Context,
						OutError))
				{
					return Fail(
						OutError,
						TEXT("asset_data_texture_selected_field_invalid"),
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
				|| CurrentFields.GetObjectItems().Num()
					!= DefaultFields.GetObjectItems().Num()
				|| CurrentFields.GetObjectItems().Num()
					!= ProjectionContext.GetObjectItems().Num()
				|| !TypedChanges
				|| TypedChanges->GetKind() != EUeAgentValueKind::Array
				|| TypedChanges->GetArrayItems().Num()
					!= CurrentFields.GetObjectItems().Num())
			{
				return Fail(
					OutError,
					TEXT("asset_data_texture_expected_state_invalid"));
			}
			for (const FUeAgentCanonicalValueRef& Change
				: TypedChanges->GetArrayItems())
			{
				const FUeAgentCanonicalValue* Field =
					FindObjectField(Change.Get(), TEXT("field_id"));
				FString FieldId;
				if (!Field
					|| !TryReadString(*Field, FieldId)
					|| OutChanges.Contains(FieldId)
					|| !FindObjectField(CurrentFields, FieldId))
				{
					return Fail(
						OutError,
						TEXT("asset_data_texture_expected_change_invalid"));
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

		bool TryResolveTexturePaths(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FUeAgentCanonicalValue& Requested,
			TArray<FString>& OutPaths,
			FExecutionMutationError& OutError)
		{
			OutPaths.Reset();
			if (Requested.GetKind() != EUeAgentValueKind::Array)
			{
				return Fail(
					OutError,
					TEXT("asset_data_texture_refs_expected_invalid"));
			}
			TSet<const UTexture*> Seen;
			for (const FUeAgentCanonicalValueRef& Item
				: Requested.GetArrayItems())
			{
				FString CanonicalRef;
				UObject* Resolved = nullptr;
				if (!TryReadString(Item.Get(), CanonicalRef)
					|| !Resolver.TryResolveExisting(
						CanonicalRef,
						Resolved,
						OutError))
				{
					return Fail(
						OutError,
						TEXT("asset_data_texture_ref_expected_invalid"),
						CanonicalRef);
				}
				const UTexture* Texture = Cast<UTexture>(Resolved);
				if (!Texture || Seen.Contains(Texture))
				{
					return Fail(
						OutError,
						TEXT("asset_data_texture_ref_expected_invalid"),
						CanonicalRef);
				}
				Seen.Add(Texture);
				OutPaths.Add(Texture->GetPathName());
			}
			return true;
		}
	}

	bool CanProjectTextureFieldState(
		const FString& NativeMechanicId)
	{
		return NativeMechanicId == RenderTargetMechanicId
			|| NativeMechanicId == SubUVMechanicId
			|| NativeMechanicId == TextureCollectionMechanicId;
	}

	bool TryProjectTextureCurrentFieldState(
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
		if (!CanProjectTextureFieldState(NativeMechanicId) || !Target)
		{
			return Fail(
				OutError,
				TEXT("asset_data_texture_field_projection_unsupported"),
				NativeMechanicId);
		}

		TMap<FString, FUeAgentCanonicalValueRef> CurrentValues;
		TMap<FString, FUeAgentCanonicalValueRef> DefaultValues;
		if (NativeMechanicId == RenderTargetMechanicId)
		{
			if (!TryBuildRenderTargetProjectionMap(
					Cast<UTextureRenderTarget2D>(Target),
					Resources,
					CurrentValues,
					OutError)
				|| !TryBuildRenderTargetProjectionMap(
					GetDefault<UTextureRenderTarget2D>(),
					Resources,
					DefaultValues,
					OutError))
			{
				return false;
			}
		}
		else if (NativeMechanicId == SubUVMechanicId)
		{
			if (!TryBuildSubUVProjectionMap(
					Cast<USubUVAnimation>(Target),
					Resources,
					CurrentValues,
					OutError)
				|| !TryBuildSubUVProjectionMap(
					GetDefault<USubUVAnimation>(),
					Resources,
					DefaultValues,
					OutError))
			{
				return false;
			}
		}
		else if (!TryBuildTextureCollectionProjectionMap(
				Cast<UTextureCollection>(Target),
				Resources,
				CurrentValues,
				OutError)
			|| !TryBuildTextureCollectionProjectionMap(
				GetDefault<UTextureCollection>(),
				Resources,
				DefaultValues,
				OutError))
		{
			return false;
		}

		return TrySelectFields(
			NativeMechanicId,
			SelectedFieldIds,
			CurrentValues,
			DefaultValues,
			Resources,
			OutCurrentFields,
			OutDefaultFields,
			OutProjectionContext,
			OutError);
	}

	bool TryProjectTextureExpectedFieldState(
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
		if (!CanProjectTextureFieldState(NativeMechanicId))
		{
			return Fail(
				OutError,
				TEXT("asset_data_texture_field_projection_unsupported"),
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
			FString ContextText;
			if (!Change
				|| !Default
				|| !Context
				|| !ExpectedContext
				|| !TryReadString(*Context, ContextText)
				|| ContextText != ExpectedContext)
			{
				return Fail(
					OutError,
					TEXT("asset_data_texture_expected_context_invalid"),
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
					TEXT("asset_data_texture_expected_presence_invalid"),
					Current.Key);
			}
			if (Presence == TEXT("clear"))
			{
				if (Requested)
				{
					return Fail(
						OutError,
						TEXT("asset_data_texture_clear_value_invalid"),
						Current.Key);
				}
				if (NativeMechanicId == TextureCollectionMechanicId)
				{
					FUeAgentCanonicalValuePtr Empty;
					if (!TryBuildStringArray({}, Resources, Empty, OutError))
					{
						return false;
					}
					ExpectedItems.Emplace(
						Current.Key,
						Empty.ToSharedRef());
				}
				else
				{
					ExpectedItems.Emplace(Current.Key, *Default);
				}
				continue;
			}

			FUeAgentCanonicalValuePtr Expected;
			if (NativeMechanicId == RenderTargetMechanicId)
			{
				if (Presence != TEXT("set") || !Requested)
				{
					return Fail(
						OutError,
						TEXT("asset_data_render_target_expected_invalid"),
						Current.Key);
				}
				if (Current.Key == TEXT("size"))
				{
					FIntPoint Extent;
					if (!TryReadExtent2i(*Requested, Extent)
						|| Extent.X > GTextureRenderTarget2DMaxSizeX
						|| Extent.Y > GTextureRenderTarget2DMaxSizeY
						|| !TryBuildExtent2i(
							Extent,
							Resources,
							Expected,
							OutError))
					{
						return Fail(
							OutError,
							TEXT("asset_data_render_target_size_expected_invalid"));
					}
				}
				else if (Current.Key == TEXT("format"))
				{
					FString Name;
					if (!TryNormalizeRenderTargetFormat(*Requested, Name)
						|| !TryBuildString(
							Name,
							Resources,
							Expected,
							OutError))
					{
						return Fail(
							OutError,
							TEXT("asset_data_render_target_format_expected_invalid"));
					}
				}
				else if (Current.Key == TEXT("clear_color"))
				{
					FLinearColor Color;
					if (!TryReadLinearColor(*Requested, Color)
						|| !TryBuildLinearColor(
							Color,
							Resources,
							Expected,
							OutError))
					{
						return Fail(
							OutError,
							TEXT("asset_data_render_target_color_expected_invalid"));
					}
				}
				else if (Current.Key == TEXT("auto_generate_mips")
					&& Requested->GetKind() == EUeAgentValueKind::Boolean)
				{
					if (!TryBuildBoolean(
							Requested->GetBoolean(),
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
						TEXT("asset_data_render_target_expected_field_invalid"),
						Current.Key);
				}
			}
			else if (NativeMechanicId == SubUVMechanicId)
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
					else
					{
						FString CanonicalRef;
						UObject* Resolved = nullptr;
						if (Presence != TEXT("set")
							|| !Requested
							|| !TryReadString(*Requested, CanonicalRef)
							|| !Resolver.TryResolveExisting(
								CanonicalRef,
								Resolved,
								OutError)
							|| !Cast<UTexture2D>(Resolved)
							|| !TryBuildReference(
								Resolved,
								Resources,
								Expected,
								OutError))
						{
							return Fail(
								OutError,
								TEXT("asset_data_subuv_texture_expected_invalid"),
								CanonicalRef);
						}
					}
				}
				else if (Current.Key == TEXT("grid"))
				{
					FIntPoint Grid;
					if (Presence != TEXT("set")
						|| !Requested
						|| !TryReadExtent2i(*Requested, Grid)
						|| static_cast<int64>(Grid.X) * Grid.Y
							> MAX_int32 / 8
						|| !TryBuildExtent2i(
							Grid,
							Resources,
							Expected,
							OutError))
					{
						return Fail(
							OutError,
							TEXT("asset_data_subuv_grid_expected_invalid"));
					}
				}
				else if (Current.Key == TEXT("bounding_geometry"))
				{
					FString Name;
					if (Presence != TEXT("set")
						|| !Requested
						|| !TryNormalizeSubUVBounding(*Requested, Name)
						|| !TryBuildString(
							Name,
							Resources,
							Expected,
							OutError))
					{
						return Fail(
							OutError,
							TEXT("asset_data_subuv_bounding_expected_invalid"));
					}
				}
				else
				{
					return Fail(
						OutError,
						TEXT("asset_data_subuv_expected_field_invalid"),
						Current.Key);
				}
			}
			else if (Current.Key == TEXT("texture_refs"))
			{
				TArray<FString> CurrentPaths;
				TArray<FString> RequestedPaths;
				if (!Requested
					|| !TryReadStringArray(Current.Value.Get(), CurrentPaths)
					|| !TryResolveTexturePaths(
						Resolver,
						*Requested,
						RequestedPaths,
						OutError))
				{
					return Fail(
						OutError,
						TEXT("asset_data_texture_collection_expected_invalid"));
				}
				if (Presence == TEXT("set"))
				{
					for (const FString& Path : RequestedPaths)
					{
						CurrentPaths.AddUnique(Path);
					}
				}
				else if (Presence == TEXT("remove"))
				{
					const TSet<FString> Removed(RequestedPaths);
					CurrentPaths.RemoveAll(
						[&Removed](const FString& Path)
						{
							return Removed.Contains(Path);
						});
				}
				else if (Presence == TEXT("replace"))
				{
					CurrentPaths = MoveTemp(RequestedPaths);
				}
				else
				{
					return Fail(
						OutError,
						TEXT("asset_data_texture_collection_presence_invalid"),
						Presence);
				}
				if (!TryBuildStringArray(
						CurrentPaths,
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
					TEXT("asset_data_texture_expected_field_invalid"),
					Current.Key);
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
