#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectMediaMechanics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"

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

        bool Fail(FExecutionMutationError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic)
        {
            OutError = MoveTemp(Diagnostic);
            return false;
        }

        FExecutionMutationError JsonFailure(const TCHAR* Code, const FString& Reason, const FString& Field,
            const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected,
            const TCHAR* SourceKind = TEXT("authored_value"))
        {
            auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, Field, Expected, Actual, TEXT("execution"), Reason);
            Error.SourceKind = SourceKind;
            Error.ActualValuePresent = Actual.IsValid();
            Error.ExpectedValuePresent = Expected.IsValid();
            return Error;
        }

        FExecutionMutationError ValueFailure(const TCHAR* Code, const FString& Reason, const FString& Field,
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
				: Fail(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(StateError.Code.IsEmpty()
						? TEXT("asset_data_media_projection_string_invalid")
						: StateError.Code, StateError, TEXT("execution")));
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
				: Fail(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(StateError.Code.IsEmpty()
						? TEXT("asset_data_media_projection_boolean_invalid")
						: StateError.Code, StateError, TEXT("execution")));
		}

		bool TryBuildDecimal(
			const float Value,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
            if (!FMath::IsFinite(Value))
                return Fail(OutError, JsonFailure(TEXT("asset_data_media_projection_decimal_invalid"),
                    TEXT("The native media value is not finite and cannot be represented as a canonical decimal."), FString(),
                    MakeShared<FJsonValueString>(FString::SanitizeFloat(static_cast<double>(Value))),
                    MakeShared<FJsonValueString>(TEXT("finite decimal")), TEXT("native_result")));
            FUeAgentStateSemanticError StateError;
            return FUeAgentCanonicalValue::TryDecimal(
                FString::SanitizeFloat(static_cast<double>(Value)), Resources, OutValue, StateError)
                ? true
                : Fail(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(StateError.Code.IsEmpty()
                    ? TEXT("asset_data_media_projection_decimal_invalid") : StateError.Code, StateError, TEXT("execution")));
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
				: Fail(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(StateError.Code.IsEmpty()
						? TEXT("asset_data_media_projection_null_invalid")
						: StateError.Code, StateError, TEXT("execution")));
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
				: Fail(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(StateError.Code.IsEmpty()
						? TEXT("asset_data_media_projection_object_invalid")
						: StateError.Code, StateError, TEXT("execution")));
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
                auto Error = JsonFailure(TEXT("asset_data_media_player_loop_property_missing"),
                    TEXT("The MediaPlayer class does not expose the required serialized Loop boolean property."),
                    TEXT("Loop"), nullptr, MakeShared<FJsonValueString>(TEXT("FBoolProperty")), TEXT("native_result"));
                Error.TargetPath = Player.GetPathName();
                return Fail(OutError, MoveTemp(Error));
			}
			bOutLoop = LoopProperty->GetPropertyValue_InContainer(
				&Player);
			return true;
		}

		bool TryBuildLinearColor(
			const FLinearColor& Color,
			const FString& Field,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
            TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
            const TCHAR* Names[] = {TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")};
            const float Components[] = {Color.R, Color.G, Color.B, Color.A};
            for (int32 Index = 0; Index < 4; ++Index)
            {
                FUeAgentCanonicalValuePtr Component;
                if (!TryBuildDecimal(Components[Index], Resources, Component, OutError))
                {
                    if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = Field + TEXT("/") + Names[Index];
                    return false;
                }
                Items.Emplace(Names[Index], Component.ToSharedRef());
            }
            return TryBuildObject(MoveTemp(Items), Resources, OutValue, OutError);
		}

        bool TryReadLinearColor(
            const FUeAgentCanonicalValue& Value,
            const FString& Field,
            const TCHAR* Code,
            const TCHAR* SourceKind,
            FLinearColor& OutColor,
            FExecutionMutationError& OutError)
        {
            if (Value.GetKind() != EUeAgentValueKind::Object || Value.GetObjectItems().Num() != 4)
                return Fail(OutError, ValueFailure(Code,
                    TEXT("A linear color must be an object containing exactly r, g, b and a."), Field,
                    &Value, TEXT("object with four numeric components: r, g, b, a"), SourceKind));
            const TCHAR* Names[] = {TEXT("r"), TEXT("g"), TEXT("b"), TEXT("a")};
            const FUeAgentCanonicalValue* Components[4] = {};
            double Numbers[4] = {};
            for (int32 Index = 0; Index < 4; ++Index)
            {
                Components[Index] = FindObjectField(Value, Names[Index]);
                if (!Components[Index])
                    return Fail(OutError, ValueFailure(Code,
                        TEXT("The linear color is missing a required component."), Field + TEXT("/") + Names[Index],
                        nullptr, TEXT("finite number"), SourceKind));
            }
            for (int32 Index = 0; Index < 4; ++Index)
            {
                if (!TryReadNumber(*Components[Index], Numbers[Index]))
                    return Fail(OutError, ValueFailure(Code,
                        TEXT("The linear color component must be a finite number."), Field + TEXT("/") + Names[Index],
                        Components[Index], TEXT("finite number"), SourceKind));
            }
            OutColor = FLinearColor(static_cast<float>(Numbers[0]), static_cast<float>(Numbers[1]),
                static_cast<float>(Numbers[2]), static_cast<float>(Numbers[3]));
            const float Floats[] = {OutColor.R, OutColor.G, OutColor.B, OutColor.A};
            for (int32 Index = 0; Index < 4; ++Index)
            {
                if (!FMath::IsFinite(Floats[Index]))
                    return Fail(OutError, ValueFailure(Code,
                        TEXT("The linear color component is outside the finite native float range."), Field + TEXT("/") + Names[Index],
                        Components[Index], TEXT("finite float"), SourceKind));
            }
            return true;
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
                return Fail(OutError, JsonFailure(TEXT("asset_data_media_player_projection_invalid"),
                    TEXT("A MediaPlayer object is required to capture media fields."), TEXT("target"),
                    nullptr, MakeShared<FJsonValueString>(UMediaPlayer::StaticClass()->GetPathName()), TEXT("native_result")));
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
                return Fail(OutError, JsonFailure(TEXT("asset_data_media_texture_projection_invalid"),
                    TEXT("A MediaTexture object is required to capture media fields."), TEXT("target"),
                    nullptr, MakeShared<FJsonValueString>(UMediaTexture::StaticClass()->GetPathName()), TEXT("native_result")));
			}
			FUeAgentCanonicalValuePtr Player;
			FUeAgentCanonicalValuePtr AutoClear;
			FUeAgentCanonicalValuePtr ClearColor;
            if (!TryBuildReference(Texture->GetMediaPlayer(), Resources, Player, OutError))
            {
                if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = TEXT("media_player_ref");
                return false;
            }
            if (!TryBuildBoolean(Texture->AutoClear, Resources, AutoClear, OutError))
            {
                if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = TEXT("auto_clear");
                return false;
            }
            if (!TryBuildLinearColor(Texture->ClearColor, TEXT("clear_color"), Resources, ClearColor, OutError))
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
                return Fail(OutError, JsonFailure(TEXT("asset_data_media_selected_fields_missing"),
                    TEXT("Media field projection requires at least one selected field."), TEXT("selected_fields"),
                    MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("one or more fields"))));
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
					|| !ContextTag)
				{
                    return Fail(OutError, JsonFailure(TEXT("asset_data_media_selected_field_invalid"),
                        Seen.Contains(FieldId) ? TEXT("The field is selected more than once.")
                            : !Current ? TEXT("The selected field has no captured current value.")
                            : !Default ? TEXT("The selected field has no captured default value.")
                            : TEXT("The selected field has no projection context for this asset type."),
                        FieldId, MakeShared<FJsonValueString>(FieldId),
                        MakeShared<FJsonValueString>(TEXT("unique supported field with current, default and context values"))));
				}
				if (!TryBuildString(ContextTag, Resources, Context, OutError)) return false;
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
                if (CurrentFields.GetKind() != EUeAgentValueKind::Object)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_media_expected_state_invalid"),
                        TEXT("Captured current media fields must be an object."), TEXT("field_state"), &CurrentFields, TEXT("object"), TEXT("native_result")));
                if (DefaultFields.GetKind() != EUeAgentValueKind::Object)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_media_expected_state_invalid"),
                        TEXT("Captured default media fields must be an object."), TEXT("field_defaults"), &DefaultFields, TEXT("object"), TEXT("native_result")));
                if (ProjectionContext.GetKind() != EUeAgentValueKind::Object)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_media_expected_state_invalid"),
                        TEXT("Media field projection context must be an object."), TEXT("field_projection_context"), &ProjectionContext, TEXT("object"), TEXT("native_result")));
                if (!TypedChanges || TypedChanges->GetKind() != EUeAgentValueKind::Array)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_media_expected_state_invalid"),
                        TEXT("The proposed media state requires a typed_changes array."), TEXT("typed_changes"), TypedChanges, TEXT("array")));
                return Fail(OutError, JsonFailure(TEXT("asset_data_media_expected_state_invalid"),
                    TEXT("The number of typed changes must match the number of selected current fields."), TEXT("typed_changes"),
                    MakeShared<FJsonValueNumber>(TypedChanges->GetArrayItems().Num()),
                    MakeShared<FJsonValueNumber>(CurrentFields.GetObjectItems().Num())));
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
                    return Fail(OutError, ValueFailure(TEXT("asset_data_media_expected_change_invalid"),
                        OutChanges.Contains(FieldId) ? TEXT("The same field_id occurs more than once in typed_changes.")
                            : TEXT("Each typed change requires a non-empty string field_id."),
                        TEXT("typed_changes/") + ::LexToString(OutChanges.Num()) + TEXT("/field_id"),
                        Field, TEXT("unique non-empty field_id")));
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
            const FString& Field,
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
                    return Fail(OutError, ValueFailure(TEXT("asset_data_media_reference_clear_invalid"),
                        TEXT("Clearing a media reference must omit the requested value."), Field,
                        Requested, TEXT("value absent")));
                if (Default.GetKind() == EUeAgentValueKind::Null)
                    return TryBuildNull(Resources, OutExpected, OutError);
                FString DefaultRef;
                return TryReadString(Default, DefaultRef)
                    ? TryBuildString(DefaultRef, Resources, OutExpected, OutError)
                    : Fail(OutError, ValueFailure(TEXT("asset_data_media_reference_default_invalid"),
                        TEXT("The native default media reference must be null or a non-empty string."),
                        Field, &Default, TEXT("null or non-empty asset reference"), TEXT("native_result")));
            }
            if (Presence == TEXT("null"))
                return Requested
                    ? Fail(OutError, ValueFailure(TEXT("asset_data_media_reference_null_invalid"),
                        TEXT("A null media reference operation must omit the requested value."), Field,
                        Requested, TEXT("value absent")))
                    : TryBuildNull(Resources, OutExpected, OutError);
            if (Presence != TEXT("set"))
                return Fail(OutError, JsonFailure(TEXT("asset_data_media_texture_player_expected_invalid"),
                    TEXT("The media reference change uses an unsupported operation."), Field + TEXT("/presence"),
                    MakeShared<FJsonValueString>(Presence), MakeShared<FJsonValueString>(TEXT("set, clear or null"))));
            FString CanonicalRef;
            if (!Requested || !TryReadString(*Requested, CanonicalRef))
                return Fail(OutError, ValueFailure(TEXT("asset_data_media_texture_player_expected_invalid"),
                    TEXT("Setting a media reference requires a non-empty asset reference string."),
                    Field, Requested, TEXT("non-empty MediaPlayer asset reference")));
            UObject* Resolved = nullptr;
            if (!Resolver.TryResolveExisting(CanonicalRef, Resolved, OutError))
                return Fail(OutError, OutError.IsValid()
                    ? UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("asset_data_media_texture_player_expected_invalid"), OutError)
                    : ValueFailure(TEXT("asset_data_media_texture_player_expected_invalid"),
                        TEXT("The asset resolver could not resolve the requested MediaPlayer reference."),
                        Field, Requested, TEXT("resolvable MediaPlayer asset reference")));
            UMediaPlayer* Player = Cast<UMediaPlayer>(Resolved);
            if (!Player)
            {
                auto Actual = MakeShared<FJsonObject>();
                Actual->SetStringField(TEXT("reference"), CanonicalRef);
                if (Resolved) Actual->SetStringField(TEXT("class"), Resolved->GetClass()->GetPathName());
                else Actual->SetField(TEXT("class"), MakeShared<FJsonValueNull>());
                return Fail(OutError, JsonFailure(TEXT("asset_data_media_texture_player_expected_invalid"),
                    TEXT("The resolved reference is not a MediaPlayer object."), Field,
                    MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(UMediaPlayer::StaticClass()->GetPathName())));
            }
            return TryBuildReference(Player, Resources, OutExpected, OutError);
        }

        bool TryBuildExpectedBoolean(
            const FString& Field,
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
                    return Fail(OutError, ValueFailure(TEXT("asset_data_media_boolean_clear_invalid"),
                        TEXT("Clearing a media boolean must omit the requested value."),
                        Field, Requested, TEXT("value absent")));
                return Default.GetKind() == EUeAgentValueKind::Boolean
                    ? TryBuildBoolean(Default.GetBoolean(), Resources, OutExpected, OutError)
                    : Fail(OutError, ValueFailure(TEXT("asset_data_media_boolean_default_invalid"),
                        TEXT("The native default media value must be a boolean."),
                        Field, &Default, TEXT("boolean"), TEXT("native_result")));
            }
            if (Presence != TEXT("set"))
                return Fail(OutError, JsonFailure(TEXT("asset_data_media_boolean_expected_invalid"),
                    TEXT("The media boolean change uses an unsupported operation."), Field + TEXT("/presence"),
                    MakeShared<FJsonValueString>(Presence), MakeShared<FJsonValueString>(bAllowClear ? TEXT("set or clear") : TEXT("set"))));
            if (!Requested || Requested->GetKind() != EUeAgentValueKind::Boolean)
                return Fail(OutError, ValueFailure(TEXT("asset_data_media_boolean_expected_invalid"),
                    TEXT("Setting a media boolean requires a boolean value."), Field, Requested, TEXT("boolean")));
            return TryBuildBoolean(Requested->GetBoolean(), Resources, OutExpected, OutError);
        }

        bool TryBuildExpectedColor(
            const FString& Field,
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
                    return Fail(OutError, ValueFailure(TEXT("asset_data_media_color_clear_invalid"),
                        TEXT("Clearing a media color must omit the requested value."), Field, Requested, TEXT("value absent")));
                FLinearColor DefaultColor;
                if (!TryReadLinearColor(Default, Field, TEXT("asset_data_media_color_default_invalid"),
                    TEXT("native_result"), DefaultColor, OutError)) return false;
                return TryBuildLinearColor(DefaultColor, Field, Resources, OutExpected, OutError);
            }
            if (Presence != TEXT("set"))
                return Fail(OutError, JsonFailure(TEXT("asset_data_media_color_expected_invalid"),
                    TEXT("The media color change uses an unsupported operation."), Field + TEXT("/presence"),
                    MakeShared<FJsonValueString>(Presence), MakeShared<FJsonValueString>(TEXT("set or clear"))));
            if (!Requested)
                return Fail(OutError, ValueFailure(TEXT("asset_data_media_color_expected_invalid"),
                    TEXT("Setting a media color requires a value."), Field, nullptr, TEXT("linear color object")));
            FLinearColor Color;
            if (!TryReadLinearColor(*Requested, Field, TEXT("asset_data_media_color_expected_invalid"),
                TEXT("authored_value"), Color, OutError)) return false;
            return TryBuildLinearColor(Color, Field, Resources, OutExpected, OutError);
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
            auto Error = JsonFailure(TEXT("asset_data_media_field_projection_unsupported"),
                !CanProjectMediaFieldState(NativeMechanicId)
                    ? TEXT("No media field projector is registered for this requested mechanic.")
                    : TEXT("Media field projection requires an available target object."),
                !CanProjectMediaFieldState(NativeMechanicId) ? TEXT("media_projector_available") : TEXT("target_available"),
                MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("native_result"));
            Error.InternalContext = NativeMechanicId;
            if (Target) Error.TargetPath = Target->GetPathName();
            return Fail(OutError, MoveTemp(Error));
		}

		TMap<FString, FUeAgentCanonicalValueRef> CurrentValues;
		TMap<FString, FUeAgentCanonicalValueRef> DefaultValues;
		if (NativeMechanicId == MediaPlayerMechanicId)
		{
            UMediaPlayer* PlayerTarget = Cast<UMediaPlayer>(Target);
            if (!PlayerTarget)
            {
                auto Error = JsonFailure(TEXT("asset_data_media_player_projection_invalid"),
                    TEXT("The media player projector received an object of another class."), TEXT("target"),
                    MakeShared<FJsonValueString>(Target->GetClass()->GetPathName()),
                    MakeShared<FJsonValueString>(UMediaPlayer::StaticClass()->GetPathName()), TEXT("native_result"));
                Error.TargetPath = Target->GetPathName();
                return Fail(OutError, MoveTemp(Error));
            }
            if (!TryBuildMediaPlayerProjectionMap(
                    PlayerTarget,
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
        else
        {
            UMediaTexture* TextureTarget = Cast<UMediaTexture>(Target);
            if (!TextureTarget)
            {
                auto Error = JsonFailure(TEXT("asset_data_media_texture_projection_invalid"),
                    TEXT("The media texture projector received an object of another class."), TEXT("target"),
                    MakeShared<FJsonValueString>(Target->GetClass()->GetPathName()),
                    MakeShared<FJsonValueString>(UMediaTexture::StaticClass()->GetPathName()), TEXT("native_result"));
                Error.TargetPath = Target->GetPathName();
                return Fail(OutError, MoveTemp(Error));
            }
            if (!TryBuildMediaTextureProjectionMap(TextureTarget, Resources, CurrentValues, OutError)
                || !TryBuildMediaTextureProjectionMap(GetDefault<UMediaTexture>(), Resources, DefaultValues, OutError))
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
            auto Error = JsonFailure(TEXT("asset_data_media_field_projection_unsupported"),
                TEXT("No media expected-state projector is registered for this requested mechanic."),
                TEXT("media_projector_available"), MakeShared<FJsonValueBoolean>(false),
                MakeShared<FJsonValueBoolean>(true), TEXT("native_result"));
            Error.InternalContext = NativeMechanicId;
            return Fail(OutError, MoveTemp(Error));
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
                if (!Change)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_media_expected_context_invalid"),
                        TEXT("The selected media field has no matching typed change."), Current.Key,
                        nullptr, TEXT("typed change for this field")));
                if (!Default)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_media_expected_context_invalid"),
                        TEXT("The selected media field has no captured default value."), Current.Key,
                        nullptr, TEXT("captured default value"), TEXT("native_result")));
                if (!Context)
                    return Fail(OutError, ValueFailure(TEXT("asset_data_media_expected_context_invalid"),
                        TEXT("The selected media field has no projection context."), Current.Key,
                        nullptr, TEXT("field projection context"), TEXT("native_result")));
                if (!ExpectedContext)
                    return Fail(OutError, JsonFailure(TEXT("asset_data_media_expected_context_invalid"),
                        TEXT("This media field is not supported by the selected projector."), Current.Key,
                        MakeShared<FJsonValueString>(Current.Key), MakeShared<FJsonValueString>(TEXT("supported media field"))));
                return Fail(OutError, ValueFailure(TEXT("asset_data_media_expected_context_invalid"),
                    TEXT("The captured media field projection context has the wrong type or tag."), Current.Key,
                    Context, ExpectedContext, TEXT("native_result")));
			}

			FString Presence;
			const FUeAgentCanonicalValue* Requested = nullptr;
			if (!TryReadPresenceAndValue(
					**Change,
					Presence,
					Requested))
			{
                return Fail(OutError, ValueFailure(TEXT("asset_data_media_expected_presence_invalid"),
                    TEXT("The media field change must declare a non-empty string presence operation."),
                    Current.Key + TEXT("/presence"), FindObjectField(**Change, TEXT("presence")), TEXT("non-empty operation string")));
			}

			FUeAgentCanonicalValuePtr Expected;
			if (NativeMechanicId == MediaPlayerMechanicId)
			{
                if (!TryBuildExpectedBoolean(
                        Current.Key,
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
                        Current.Key,
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
                        Current.Key,
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
                        Current.Key,
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
                return Fail(OutError, JsonFailure(TEXT("asset_data_media_expected_field_invalid"),
                    TEXT("This field has no media expected-state builder for the selected asset type."), Current.Key,
                    MakeShared<FJsonValueString>(Current.Key), MakeShared<FJsonValueString>(TEXT("supported media field"))));
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
