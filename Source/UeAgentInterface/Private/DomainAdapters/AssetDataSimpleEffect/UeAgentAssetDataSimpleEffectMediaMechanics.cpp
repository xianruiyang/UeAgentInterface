#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectMediaMechanics.h"

#include "MediaPlayer.h"
#include "MediaTexture.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace UE::UAI::DomainAdapters::AssetDataSimpleEffect
{
	using namespace ContextResource;
	using namespace ExecutionMutation;
	using namespace StateSemantic;

	namespace
	{
		const TCHAR* MediaPlayerMechanicId =
			TEXT("uai.asset_data.mechanic:update_media_player:v1");
		const TCHAR* MediaTextureMechanicId =
			TEXT("uai.asset_data.mechanic:update_media_texture:v1");

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
						? TEXT("asset_data_media_projection_string_invalid")
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
						? TEXT("asset_data_media_projection_boolean_invalid")
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
					FString::SanitizeFloat(
						static_cast<double>(Value)),
					Resources,
					OutValue,
					StateError)
				? true
				: Fail(
					OutError,
					StateError.Code.IsEmpty()
						? TEXT("asset_data_media_projection_decimal_invalid")
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
						? TEXT("asset_data_media_projection_null_invalid")
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
						? TEXT("asset_data_media_projection_object_invalid")
						: StateError.Code,
					StateError.Detail);
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
			return LexTryParseString(
				OutNumber,
				*Value.GetLexeme())
				&& FMath::IsFinite(OutNumber);
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
				: TryBuildNull(
					Resources,
					OutValue,
					OutError);
		}

		bool TryReadSerializedLoop(
			const UMediaPlayer& Player,
			bool& bOutLoop,
			FExecutionMutationError& OutError)
		{
			const FBoolProperty* LoopProperty =
				FindFProperty<FBoolProperty>(
					UMediaPlayer::StaticClass(),
					TEXT("Loop"));
			if (!LoopProperty)
			{
				return Fail(
					OutError,
					TEXT("asset_data_media_player_loop_property_missing"));
			}
			bOutLoop = LoopProperty->GetPropertyValue_InContainer(
				&Player);
			return true;
		}

		bool TryBuildLinearColor(
			const FLinearColor& Color,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			FUeAgentCanonicalValuePtr Red;
			FUeAgentCanonicalValuePtr Green;
			FUeAgentCanonicalValuePtr Blue;
			FUeAgentCanonicalValuePtr Alpha;
			return TryBuildDecimal(
					Color.R,
					Resources,
					Red,
					OutError)
				&& TryBuildDecimal(
					Color.G,
					Resources,
					Green,
					OutError)
				&& TryBuildDecimal(
					Color.B,
					Resources,
					Blue,
					OutError)
				&& TryBuildDecimal(
					Color.A,
					Resources,
					Alpha,
					OutError)
				&& TryBuildObject(
					{
						{TEXT("r"), Red.ToSharedRef()},
						{TEXT("g"), Green.ToSharedRef()},
						{TEXT("b"), Blue.ToSharedRef()},
						{TEXT("a"), Alpha.ToSharedRef()},
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
			const FUeAgentCanonicalValue* Red =
				FindObjectField(Value, TEXT("r"));
			const FUeAgentCanonicalValue* Green =
				FindObjectField(Value, TEXT("g"));
			const FUeAgentCanonicalValue* Blue =
				FindObjectField(Value, TEXT("b"));
			const FUeAgentCanonicalValue* Alpha =
				FindObjectField(Value, TEXT("a"));
			double R = 0.0;
			double G = 0.0;
			double B = 0.0;
			double A = 0.0;
			if (!Red
				|| !Green
				|| !Blue
				|| !Alpha
				|| !TryReadNumber(*Red, R)
				|| !TryReadNumber(*Green, G)
				|| !TryReadNumber(*Blue, B)
				|| !TryReadNumber(*Alpha, A))
			{
				return false;
			}
			OutColor = FLinearColor(
				static_cast<float>(R),
				static_cast<float>(G),
				static_cast<float>(B),
				static_cast<float>(A));
			return FMath::IsFinite(OutColor.R)
				&& FMath::IsFinite(OutColor.G)
				&& FMath::IsFinite(OutColor.B)
				&& FMath::IsFinite(OutColor.A);
		}

		bool TryBuildMediaPlayerProjectionMap(
			const UMediaPlayer* Player,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutValues,
			FExecutionMutationError& OutError)
		{
			OutValues.Reset();
			if (!Player)
			{
				return Fail(
					OutError,
					TEXT("asset_data_media_player_projection_invalid"));
			}
			bool bLoop = false;
			FUeAgentCanonicalValuePtr PlayOnOpen;
			FUeAgentCanonicalValuePtr Loop;
			if (!TryReadSerializedLoop(*Player, bLoop, OutError)
				|| !TryBuildBoolean(
					Player->PlayOnOpen,
					Resources,
					PlayOnOpen,
					OutError)
				|| !TryBuildBoolean(
					bLoop,
					Resources,
					Loop,
					OutError))
			{
				return false;
			}
			OutValues.Add(TEXT("play_on_open"), PlayOnOpen.ToSharedRef());
			OutValues.Add(TEXT("loop"), Loop.ToSharedRef());
			return true;
		}

		bool TryBuildMediaTextureProjectionMap(
			const UMediaTexture* Texture,
			FUeAgentResourceTracker& Resources,
			TMap<FString, FUeAgentCanonicalValueRef>& OutValues,
			FExecutionMutationError& OutError)
		{
			OutValues.Reset();
			if (!Texture)
			{
				return Fail(
					OutError,
					TEXT("asset_data_media_texture_projection_invalid"));
			}
			FUeAgentCanonicalValuePtr Player;
			FUeAgentCanonicalValuePtr AutoClear;
			FUeAgentCanonicalValuePtr ClearColor;
			if (!TryBuildReference(
					Texture->GetMediaPlayer(),
					Resources,
					Player,
					OutError)
				|| !TryBuildBoolean(
					Texture->AutoClear,
					Resources,
					AutoClear,
					OutError)
				|| !TryBuildLinearColor(
					Texture->ClearColor,
					Resources,
					ClearColor,
					OutError))
			{
				return false;
			}
			OutValues.Add(TEXT("media_player_ref"), Player.ToSharedRef());
			OutValues.Add(TEXT("auto_clear"), AutoClear.ToSharedRef());
			OutValues.Add(TEXT("clear_color"), ClearColor.ToSharedRef());
			return true;
		}

		const TCHAR* FindContextTag(
			const FString& NativeMechanicId,
			const FString& FieldId)
		{
			if (NativeMechanicId == MediaPlayerMechanicId
				&& (FieldId == TEXT("play_on_open")
					|| FieldId == TEXT("loop")))
			{
				return TEXT("media_boolean");
			}
			if (NativeMechanicId == MediaTextureMechanicId)
			{
				if (FieldId == TEXT("media_player_ref"))
				{
					return TEXT("media_player_asset_ref");
				}
				if (FieldId == TEXT("auto_clear"))
				{
					return TEXT("media_boolean");
				}
				if (FieldId == TEXT("clear_color"))
				{
					return TEXT("media_linear_color");
				}
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
					TEXT("asset_data_media_selected_fields_missing"));
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
						TEXT("asset_data_media_selected_field_invalid"),
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
					TEXT("asset_data_media_expected_state_invalid"));
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
						TEXT("asset_data_media_expected_change_invalid"));
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

		bool TryBuildExpectedReference(
			IUeAgentAssetDataSimpleEffectAssetResolver& Resolver,
			const FString& Presence,
			const FUeAgentCanonicalValue& Default,
			const FUeAgentCanonicalValue* Requested,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutExpected,
			FExecutionMutationError& OutError)
		{
			if (Presence == TEXT("clear"))
			{
				if (Requested)
				{
					return Fail(
						OutError,
						TEXT("asset_data_media_reference_clear_invalid"));
				}
				if (Default.GetKind() == EUeAgentValueKind::Null)
				{
					return TryBuildNull(
						Resources,
						OutExpected,
						OutError);
				}
				FString DefaultRef;
				return TryReadString(Default, DefaultRef)
					? TryBuildString(
						DefaultRef,
						Resources,
						OutExpected,
						OutError)
					: Fail(
						OutError,
						TEXT("asset_data_media_reference_default_invalid"));
			}
			if (Presence == TEXT("null"))
			{
				return Requested
					? Fail(
						OutError,
						TEXT("asset_data_media_reference_null_invalid"))
					: TryBuildNull(Resources, OutExpected, OutError);
			}
			FString CanonicalRef;
			UObject* Resolved = nullptr;
			if (Presence != TEXT("set")
				|| !Requested
				|| !TryReadString(*Requested, CanonicalRef)
				|| !Resolver.TryResolveExisting(
					CanonicalRef,
					Resolved,
					OutError))
			{
				return Fail(
					OutError,
					TEXT("asset_data_media_texture_player_expected_invalid"),
					CanonicalRef);
			}
			UMediaPlayer* Player = Cast<UMediaPlayer>(Resolved);
			if (!Player)
			{
				return Fail(
					OutError,
					TEXT("asset_data_media_texture_player_expected_invalid"),
					CanonicalRef);
			}
			return TryBuildReference(
				Player,
				Resources,
				OutExpected,
				OutError);
		}

		bool TryBuildExpectedBoolean(
			const FString& Presence,
			const FUeAgentCanonicalValue& Default,
			const FUeAgentCanonicalValue* Requested,
			const bool bAllowClear,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutExpected,
			FExecutionMutationError& OutError)
		{
			if (bAllowClear && Presence == TEXT("clear"))
			{
				if (Requested)
				{
					return Fail(
						OutError,
						TEXT("asset_data_media_boolean_clear_invalid"));
				}
				return Default.GetKind() == EUeAgentValueKind::Boolean
					? TryBuildBoolean(
						Default.GetBoolean(),
						Resources,
						OutExpected,
						OutError)
					: Fail(
						OutError,
						TEXT("asset_data_media_boolean_default_invalid"));
			}
			if (Presence != TEXT("set")
				|| !Requested
				|| Requested->GetKind() != EUeAgentValueKind::Boolean)
			{
				return Fail(
					OutError,
					TEXT("asset_data_media_boolean_expected_invalid"));
			}
			return TryBuildBoolean(
				Requested->GetBoolean(),
				Resources,
				OutExpected,
				OutError);
		}

		bool TryBuildExpectedColor(
			const FString& Presence,
			const FUeAgentCanonicalValue& Default,
			const FUeAgentCanonicalValue* Requested,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutExpected,
			FExecutionMutationError& OutError)
		{
			if (Presence == TEXT("clear"))
			{
				if (Requested)
				{
					return Fail(
						OutError,
						TEXT("asset_data_media_color_clear_invalid"));
				}
				FLinearColor DefaultColor;
				return TryReadLinearColor(Default, DefaultColor)
					? TryBuildLinearColor(
						DefaultColor,
						Resources,
						OutExpected,
						OutError)
					: Fail(
						OutError,
						TEXT("asset_data_media_color_default_invalid"));
			}
			FLinearColor Color;
			if (Presence != TEXT("set")
				|| !Requested
				|| !TryReadLinearColor(*Requested, Color))
			{
				return Fail(
					OutError,
					TEXT("asset_data_media_color_expected_invalid"));
			}
			return TryBuildLinearColor(
				Color,
				Resources,
				OutExpected,
				OutError);
		}
	}

	bool CanProjectMediaFieldState(
		const FString& NativeMechanicId)
	{
		return NativeMechanicId == MediaPlayerMechanicId
			|| NativeMechanicId == MediaTextureMechanicId;
	}

	bool TryProjectMediaCurrentFieldState(
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
		if (!CanProjectMediaFieldState(NativeMechanicId) || !Target)
		{
			return Fail(
				OutError,
				TEXT("asset_data_media_field_projection_unsupported"),
				NativeMechanicId);
		}

		TMap<FString, FUeAgentCanonicalValueRef> CurrentValues;
		TMap<FString, FUeAgentCanonicalValueRef> DefaultValues;
		if (NativeMechanicId == MediaPlayerMechanicId)
		{
			if (!TryBuildMediaPlayerProjectionMap(
					Cast<UMediaPlayer>(Target),
					Resources,
					CurrentValues,
					OutError)
				|| !TryBuildMediaPlayerProjectionMap(
					GetDefault<UMediaPlayer>(),
					Resources,
					DefaultValues,
					OutError))
			{
				return false;
			}
		}
		else if (!TryBuildMediaTextureProjectionMap(
				Cast<UMediaTexture>(Target),
				Resources,
				CurrentValues,
				OutError)
			|| !TryBuildMediaTextureProjectionMap(
				GetDefault<UMediaTexture>(),
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

	bool TryProjectMediaExpectedFieldState(
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
		if (!CanProjectMediaFieldState(NativeMechanicId))
		{
			return Fail(
				OutError,
				TEXT("asset_data_media_field_projection_unsupported"),
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
			const FUeAgentCanonicalValue* Default =
				FindObjectField(DefaultFields, Current.Key);
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
					TEXT("asset_data_media_expected_context_invalid"),
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
					TEXT("asset_data_media_expected_presence_invalid"),
					Current.Key);
			}

			FUeAgentCanonicalValuePtr Expected;
			if (NativeMechanicId == MediaPlayerMechanicId)
			{
				if (!TryBuildExpectedBoolean(
						Presence,
						*Default,
						Requested,
						false,
						Resources,
						Expected,
						OutError))
				{
					return false;
				}
			}
			else if (Current.Key == TEXT("media_player_ref"))
			{
				if (!TryBuildExpectedReference(
						Resolver,
						Presence,
						*Default,
						Requested,
						Resources,
						Expected,
						OutError))
				{
					return false;
				}
			}
			else if (Current.Key == TEXT("auto_clear"))
			{
				if (!TryBuildExpectedBoolean(
						Presence,
						*Default,
						Requested,
						true,
						Resources,
						Expected,
						OutError))
				{
					return false;
				}
			}
			else if (Current.Key == TEXT("clear_color"))
			{
				if (!TryBuildExpectedColor(
						Presence,
						*Default,
						Requested,
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
					TEXT("asset_data_media_expected_field_invalid"),
					Current.Key);
			}
			ExpectedItems.Emplace(
				Current.Key,
				Expected.ToSharedRef());
		}
		return TryBuildObject(
			MoveTemp(ExpectedItems),
			Resources,
			OutExpectedFields,
			OutError);
	}
}
