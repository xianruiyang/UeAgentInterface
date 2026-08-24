#include "DomainAdapters/Sequence/UeAgentSequenceHjsonContract.h"

#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "LevelSequence.h"
#include "MovieScene.h"

namespace UE::UAI::DomainAdapters::Sequence
{
	using namespace HjsonWorkspace;

	namespace
	{
		const FString SchemaId = TEXT("uai.sequence.edit/1");
		const FString RootType = TEXT("/Script/LevelSequence.LevelSequence");
		const FString FieldOwner = TEXT("sequence-domain-provider");
		const FString FieldSetId = TEXT("sequence.timeline.settings/1");

		const TArray<FString>& FieldIds()
		{
			static const TArray<FString> Values{
				TEXT("display_rate"),
				TEXT("tick_resolution"),
				TEXT("playback_range"),
			};
			return Values;
		}

		FString PointerFor(const FString& FieldId)
		{
			return TEXT("/root/fields/") + FieldId;
		}

		bool Fail(
			FWorkspaceError& OutError,
			FString Code,
			FString Pointer)
		{
			OutError.Set(MoveTemp(Code), FieldOwner, MoveTemp(Pointer));
			return false;
		}

		TSharedRef<FJsonObject> Rate(const FFrameRate& Value)
		{
			const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(
				TEXT("numerator"), FString::FromInt(Value.Numerator));
			Result->SetStringField(
				TEXT("denominator"), FString::FromInt(Value.Denominator));
			return Result;
		}

		TSharedRef<FJsonObject> Point(
			const FString& ClockId,
			const FFrameRate& TickResolution,
			const int32 Tick)
		{
			const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("clock_id"), ClockId);
			Result->SetStringField(TEXT("tick"), FString::FromInt(Tick));
			Result->SetObjectField(TEXT("rate"), Rate(TickResolution));
			Result->SetStringField(TEXT("state"), TEXT("authored"));
			return Result;
		}

		TSharedRef<FJsonObject> Bound(
			const TCHAR* Kind,
			const TSharedRef<FJsonObject>& PointValue)
		{
			const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("kind"), Kind);
			Result->SetObjectField(TEXT("point"), PointValue);
			return Result;
		}

		TSharedRef<FJsonObject> PlaybackRange(
			const ULevelSequence& Sequence,
			const UMovieScene& Scene)
		{
			const FString ClockId = TEXT("level_sequence:")
				+ Sequence.GetPathName();
			const FFrameRate TickResolution = Scene.GetTickResolution();
			const TRange<FFrameNumber> Native = Scene.GetPlaybackRange();
			const int32 Lower = Native.HasLowerBound()
				? Native.GetLowerBoundValue().Value
				: 0;
			const int32 Upper = Native.HasUpperBound()
				? Native.GetUpperBoundValue().Value
				: Lower;
			const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("clock_id"), ClockId);
			Result->SetStringField(TEXT("state"), TEXT("authored"));
			Result->SetObjectField(
				TEXT("lower"), Bound(TEXT("inclusive"),
					Point(ClockId, TickResolution, Lower)));
			Result->SetObjectField(
				TEXT("upper"), Bound(TEXT("exclusive"),
					Point(ClockId, TickResolution, Upper)));
			return Result;
		}

		bool TryCaptureTimelineSettings(
			const FString& TargetIdentity,
			TSharedPtr<FJsonObject>& OutStructure,
			FWorkspaceError& OutError)
		{
			OutStructure.Reset();
			ULevelSequence* Sequence = LoadObject<ULevelSequence>(
				nullptr,
				*DomainAuthoring::ObjectPathFromPackageIdentity(TargetIdentity));
			const UMovieScene* Scene = Sequence ? Sequence->GetMovieScene() : nullptr;
			if (!Sequence || !Scene)
			{
				return Fail(
					OutError,
					TEXT("sequence_hjson_target_unresolved"),
					TEXT("/root"));
			}
			OutStructure = MakeShared<FJsonObject>();
			OutStructure->SetObjectField(
				TEXT("display_rate"), Rate(Scene->GetDisplayRate()));
			OutStructure->SetObjectField(
				TEXT("tick_resolution"), Rate(Scene->GetTickResolution()));
			OutStructure->SetObjectField(
				TEXT("playback_range"), PlaybackRange(*Sequence, *Scene));
			return true;
		}

		TSharedRef<FJsonObject> FieldSchema(
			const FString& FieldId,
			const FString& ValueContract,
			const bool bSetSupported,
			const FString& SetSemantics = FString())
		{
			const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("field_id"), FieldId);
			Result->SetStringField(TEXT("path"), PointerFor(FieldId));
			Result->SetStringField(TEXT("owner"), FieldOwner);
			Result->SetStringField(TEXT("field_set"), FieldSetId);
			Result->SetStringField(TEXT("value_contract"), ValueContract);
			Result->SetStringField(TEXT("missing"), TEXT("preserve"));
			Result->SetBoolField(TEXT("nullable"), false);
			Result->SetBoolField(TEXT("default_supported"), false);
			TArray<TSharedPtr<FJsonValue>> AllowedIntents{
				MakeShared<FJsonValueString>(TEXT("preserve")),
			};
			if (bSetSupported)
			{
				AllowedIntents.Add(MakeShared<FJsonValueString>(TEXT("set")));
			}
			Result->SetArrayField(
				TEXT("allowed_intents"), MoveTemp(AllowedIntents));
			Result->SetStringField(
				TEXT("write_status"),
				bSetSupported ? TEXT("supported") : TEXT("read_only"));
			if (!SetSemantics.IsEmpty())
			{
				Result->SetStringField(
					bSetSupported
						? TEXT("set_semantics")
						: TEXT("write_limitation"),
					SetSemantics);
			}
			return Result;
		}
	}

	bool FSequenceHjsonPatch::IsValid() const
	{
		return Structure.IsValid() && !ChangedFields.IsEmpty();
	}

	const FString& GetSequenceHjsonSchemaId()
	{
		return SchemaId;
	}

	const FString& GetSequenceHjsonRootType()
	{
		return RootType;
	}

	const FString& GetSequenceHjsonFieldOwner()
	{
		return FieldOwner;
	}

	const FString& GetSequenceHjsonFieldSetId()
	{
		return FieldSetId;
	}

	bool TryBuildSequenceHjsonAuthoringSchema(
		const FString& TargetIdentity,
		TSharedPtr<FJsonObject>& OutSchema,
		FWorkspaceError& OutError)
	{
		OutSchema.Reset();
		OutError.Reset();
		TSharedPtr<FJsonObject> Current;
		if (!TryCaptureTimelineSettings(TargetIdentity, Current, OutError))
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* CurrentPlayback = nullptr;
		if (!Current->TryGetObjectField(TEXT("playback_range"), CurrentPlayback)
			|| !CurrentPlayback || !CurrentPlayback->IsValid())
		{
			return Fail(
				OutError,
				TEXT("sequence_hjson_current_values_invalid"),
				TEXT("/root/fields/playback_range"));
		}

		TArray<TSharedPtr<FJsonValue>> Fields;
		Fields.Add(MakeShared<FJsonValueObject>(
			FieldSchema(
				TEXT("display_rate"),
				TEXT("timeline.rate"),
				true,
				TEXT("replace_display_rate_preserve_sequence_structure"))));
		Fields.Add(MakeShared<FJsonValueObject>(
			FieldSchema(
				TEXT("tick_resolution"),
				TEXT("timeline.rate"),
				false,
				TEXT("read_only_until_non_interactive_retime_contract"))));
		Fields.Add(MakeShared<FJsonValueObject>(
			FieldSchema(
				TEXT("playback_range"),
				TEXT("timeline.temporal_range"),
				true,
				TEXT("replace_playback_range_preserve_sequence_structure"))));

		const TSharedRef<FJsonObject> Patch = MakeShared<FJsonObject>();
		Patch->SetStringField(TEXT("intent"), TEXT("set"));
		Patch->SetObjectField(TEXT("value"), *CurrentPlayback);
		const TSharedRef<FJsonObject> PatchFields = MakeShared<FJsonObject>();
		PatchFields->SetObjectField(TEXT("playback_range"), Patch);
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("type"), RootType);
		Root->SetObjectField(TEXT("fields"), PatchFields);
		const TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetStringField(TEXT("schema"), SchemaId);
		Document->SetObjectField(TEXT("root"), Root);

		OutSchema = MakeShared<FJsonObject>();
		OutSchema->SetStringField(TEXT("schema_id"), SchemaId);
		OutSchema->SetStringField(TEXT("root_type"), RootType);
		OutSchema->SetStringField(TEXT("provider"), FieldOwner);
		OutSchema->SetStringField(TEXT("field_set"), FieldSetId);
		OutSchema->SetArrayField(TEXT("fields"), MoveTemp(Fields));
		OutSchema->SetObjectField(TEXT("current_values"), Current);
		OutSchema->SetObjectField(TEXT("template"), Document);
		OutSchema->SetStringField(
			TEXT("template_guidance"),
			TEXT("Copy the template, edit only a supported field value, omit unrelated fields, and use the exact returned revision."));
		return true;
	}

	bool TryBuildSequenceHjsonExportDocument(
		const FString& TargetIdentity,
		TSharedPtr<FJsonObject>& OutDocument,
		FWorkspaceError& OutError)
	{
		OutDocument.Reset();
		OutError.Reset();
		TSharedPtr<FJsonObject> Current;
		if (!TryCaptureTimelineSettings(TargetIdentity, Current, OutError)
			|| !Current.IsValid())
		{
			return false;
		}

		const TSharedRef<FJsonObject> Fields = MakeShared<FJsonObject>();
		for (const FString& FieldId : FieldIds())
		{
			const TSharedRef<FJsonObject> Preserve = MakeShared<FJsonObject>();
			Preserve->SetStringField(TEXT("intent"), TEXT("preserve"));
			Fields->SetObjectField(FieldId, Preserve);
		}

		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("type"), RootType);
		Root->SetStringField(TEXT("field_set"), FieldSetId);
		Root->SetObjectField(TEXT("current_values"), Current.ToSharedRef());
		Root->SetObjectField(TEXT("fields"), Fields);

		OutDocument = MakeShared<FJsonObject>();
		OutDocument->SetStringField(TEXT("schema"), SchemaId);
		OutDocument->SetObjectField(TEXT("root"), Root);
		return true;
	}

	bool TryResolveSequenceHjsonPatch(
		const FString& TargetIdentity,
		const bool bTargetPresent,
		const TSharedPtr<FJsonObject>& Root,
		FSequenceHjsonPatch& OutPatch,
		FWorkspaceError& OutError)
	{
		OutPatch = {};
		OutError.Reset();
		if (!Root.IsValid())
		{
			return Fail(
				OutError, TEXT("sequence_hjson_root_invalid"), TEXT("/root"));
		}
		const TSharedPtr<FJsonObject>* Structure = nullptr;
		const TSharedPtr<FJsonObject>* Fields = nullptr;
		Root->TryGetObjectField(TEXT("structure"), Structure);
		Root->TryGetObjectField(TEXT("fields"), Fields);
		if (bTargetPresent
			&& Structure && Structure->IsValid()
			&& Fields && Fields->IsValid())
		{
			return Fail(
				OutError,
				TEXT("sequence_hjson_edit_modes_conflict"),
				TEXT("/root"));
		}
		if ((!Fields || !Fields->IsValid())
			&& Structure && Structure->IsValid())
		{
			OutPatch.Structure = MakeShared<FJsonObject>(**Structure);
			OutPatch.ChangedFields = {TEXT("structure")};
			return true;
		}
		if (!Fields || !Fields->IsValid())
		{
			return Fail(
				OutError,
				TEXT("sequence_hjson_edit_required"),
				TEXT("/root"));
		}
		if (!bTargetPresent && (!Structure || !Structure->IsValid()))
		{
			return Fail(
				OutError,
				TEXT("sequence_hjson_create_structure_required"),
				TEXT("/root/structure"));
		}
		if (Structure && Structure->IsValid())
		{
			OutPatch.Structure = MakeShared<FJsonObject>(**Structure);
		}
		else if (!TryCaptureTimelineSettings(
				TargetIdentity, OutPatch.Structure, OutError))
		{
			return false;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair
			: (*Fields)->Values)
		{
			OutPatch.bSparseSettings = true;
			if (!FieldIds().Contains(Pair.Key))
			{
				return Fail(
					OutError,
					TEXT("sequence_hjson_field_unknown"),
					PointerFor(Pair.Key));
			}
			const TSharedPtr<FJsonObject> Field = Pair.Value.IsValid()
				? Pair.Value->AsObject()
				: nullptr;
			FString Intent;
			if (!Field.IsValid()
				|| !Field->TryGetStringField(TEXT("intent"), Intent))
			{
				return Fail(
					OutError,
					TEXT("sequence_hjson_field_contract_invalid"),
					PointerFor(Pair.Key));
			}
			Intent.TrimStartAndEndInline();
			if (Intent == TEXT("preserve"))
			{
				if (Field->Values.Num() != 1)
				{
					return Fail(
						OutError,
						TEXT("sequence_hjson_preserve_value_forbidden"),
						PointerFor(Pair.Key));
				}
				continue;
			}
			if (Intent != TEXT("set"))
			{
				return Fail(
					OutError,
					TEXT("sequence_hjson_field_intent_unsupported"),
					PointerFor(Pair.Key));
			}
			if (Pair.Key == TEXT("tick_resolution"))
			{
				return Fail(
					OutError,
					TEXT("sequence_hjson_field_intent_unsupported"),
					PointerFor(Pair.Key));
			}
			const TSharedPtr<FJsonObject>* Value = nullptr;
			if (Field->Values.Num() != 2
				|| !Field->TryGetObjectField(TEXT("value"), Value)
				|| !Value || !Value->IsValid())
			{
				return Fail(
					OutError,
					TEXT("sequence_hjson_field_value_invalid"),
					PointerFor(Pair.Key));
			}
			OutPatch.Structure->SetObjectField(Pair.Key, *Value);
			OutPatch.ChangedFields.Add(Pair.Key);
		}
		if (OutPatch.ChangedFields.IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("sequence_hjson_no_change"),
				TEXT("/root/fields"));
		}
		OutPatch.ChangedFields.Sort();
		return OutPatch.IsValid();
	}
}
