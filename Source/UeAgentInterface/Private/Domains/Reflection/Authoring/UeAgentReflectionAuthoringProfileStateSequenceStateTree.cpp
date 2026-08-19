// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringProfileStateSequenceStateTree.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "StateTree.h"
#include "StateTreeCompilerLog.h"
#include "StateTreeEditingSubsystem.h"
#include "StateTreeEditorData.h"
#include "StateTreeSchema.h"
#include "StateTreeState.h"
#include "UObject/SoftObjectPath.h"

namespace UeAgentReflectionAuthoring::ProfileStateSequenceStateTree
{
	namespace
	{
		enum class EProfileStateKind : uint8
		{
			Unsupported,
			Sequence,
			StateTree,
		};

		struct FSequenceState
		{
			int32 DisplayRateNumerator = 0;
			int32 DisplayRateDenominator = 0;
			int32 PlaybackStart = 0;
			int32 PlaybackEnd = 0;
		};

		struct FStateTreeState
		{
			FString SchemaClassPath;
			FString RootStateName;
			UClass* SchemaClass = nullptr;
		};

		EProfileStateKind GetKind(const FProfile& Profile)
		{
			if (Profile.TypeId.Equals(TEXT("sequence"), ESearchCase::IgnoreCase))
			{
				return EProfileStateKind::Sequence;
			}
			if (Profile.TypeId.Equals(TEXT("state_tree"), ESearchCase::IgnoreCase))
			{
				return EProfileStateKind::StateTree;
			}
			return EProfileStateKind::Unsupported;
		}

		bool FailUnsupported(const FProfile& Profile, FString& OutError)
		{
			OutError = TEXT("reflection_profile_state_surface_unsupported:") + Profile.TypeId;
			return false;
		}

		bool TryReadInt32Field(
			const TSharedPtr<FJsonObject>& Object,
			const TCHAR* FieldName,
			int32& OutValue,
			FString& OutError)
		{
			if (!Object->HasField(FieldName))
			{
				OutError = TEXT("sequence_timing_missing");
				return false;
			}
			double Number = 0.0;
			if (!Object->TryGetNumberField(FieldName, Number)
				|| !FMath::IsFinite(Number)
				|| Number < static_cast<double>(MIN_int32)
				|| Number > static_cast<double>(MAX_int32))
			{
				OutError = TEXT("sequence_timing_invalid");
				return false;
			}
			const int32 Integer = static_cast<int32>(Number);
			if (static_cast<double>(Integer) != Number)
			{
				OutError = TEXT("sequence_timing_invalid");
				return false;
			}
			OutValue = Integer;
			return true;
		}

		bool TryParseDisplayRate(
			const FString& DisplayRate,
			int32& OutNumerator,
			int32& OutDenominator)
		{
			FString NumeratorText;
			FString DenominatorText;
			if (!DisplayRate.Split(TEXT("/"), &NumeratorText, &DenominatorText)
				|| DenominatorText.Contains(TEXT("/")))
			{
				return false;
			}
			NumeratorText.TrimStartAndEndInline();
			DenominatorText.TrimStartAndEndInline();
			return LexTryParseString(OutNumerator, *NumeratorText)
				&& LexTryParseString(OutDenominator, *DenominatorText)
				&& OutNumerator > 0
				&& OutDenominator > 0;
		}

		bool ParseSequenceState(
			const TSharedPtr<FJsonValue>& Value,
			FSequenceState& OutState,
			FString& OutError)
		{
			OutState = FSequenceState();
			if (!Value.IsValid())
			{
				OutError = TEXT("sequence_timing_missing");
				return false;
			}
			if (Value->Type != EJson::Object)
			{
				OutError = TEXT("sequence_timing_invalid");
				return false;
			}
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			if (!Object.IsValid())
			{
				OutError = TEXT("sequence_timing_missing");
				return false;
			}

			if (!Object->HasField(TEXT("display_rate")))
			{
				OutError = TEXT("sequence_timing_missing");
				return false;
			}
			FString DisplayRate;
			if (!Object->TryGetStringField(TEXT("display_rate"), DisplayRate)
				|| !TryParseDisplayRate(
					DisplayRate,
					OutState.DisplayRateNumerator,
					OutState.DisplayRateDenominator))
			{
				OutError = TEXT("sequence_timing_invalid");
				return false;
			}
			if (!TryReadInt32Field(Object, TEXT("playback_start"), OutState.PlaybackStart, OutError)
				|| !TryReadInt32Field(Object, TEXT("playback_end"), OutState.PlaybackEnd, OutError))
			{
				return false;
			}
			const int64 Duration = static_cast<int64>(OutState.PlaybackEnd)
				- static_cast<int64>(OutState.PlaybackStart);
			if (Duration < 0 || Duration > MAX_int32)
			{
				OutError = TEXT("sequence_timing_invalid");
				return false;
			}
			OutError.Reset();
			return true;
		}

		bool ParseStateTreeState(
			const TSharedPtr<FJsonValue>& Value,
			FStateTreeState& OutState,
			FString& OutError)
		{
			OutState = FStateTreeState();
			if (!Value.IsValid() || Value->Type != EJson::Object)
			{
				OutError = TEXT("state_tree_root_missing");
				return false;
			}
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			if (!Object.IsValid())
			{
				OutError = TEXT("state_tree_root_missing");
				return false;
			}

			if (!Object->HasField(TEXT("schema_class")))
			{
				OutError = TEXT("state_tree_schema_missing");
				return false;
			}
			if (!Object->TryGetStringField(TEXT("schema_class"), OutState.SchemaClassPath))
			{
				OutError = TEXT("state_tree_schema_class_invalid");
				return false;
			}
			OutState.SchemaClassPath.TrimStartAndEndInline();
			if (OutState.SchemaClassPath.IsEmpty())
			{
				OutError = TEXT("state_tree_schema_missing");
				return false;
			}

			const TSharedPtr<FJsonObject>* RootState = nullptr;
			if (!Object->TryGetObjectField(TEXT("root_state"), RootState)
				|| !RootState
				|| !RootState->IsValid()
				|| !(*RootState)->TryGetStringField(TEXT("name"), OutState.RootStateName))
			{
				OutError = TEXT("state_tree_root_missing");
				return false;
			}
			OutState.RootStateName.TrimStartAndEndInline();
			if (OutState.RootStateName.IsEmpty() || FName(*OutState.RootStateName).IsNone())
			{
				OutError = TEXT("state_tree_root_missing");
				return false;
			}

			const FSoftClassPath SchemaClassPath(OutState.SchemaClassPath);
			OutState.SchemaClass = SchemaClassPath.TryLoadClass<UStateTreeSchema>();
			if (!OutState.SchemaClass
				|| !OutState.SchemaClass->IsChildOf(UStateTreeSchema::StaticClass()))
			{
				OutError = TEXT("state_tree_schema_class_invalid");
				return false;
			}
			OutState.SchemaClassPath = OutState.SchemaClass->GetPathName();
			OutError.Reset();
			return true;
		}

		bool ReadSequence(
			UObject& Asset,
			TSharedPtr<FJsonValue>& OutValue,
			FString& OutError)
		{
			const ULevelSequence* Sequence = Cast<ULevelSequence>(&Asset);
			if (!Sequence)
			{
				OutError = TEXT("sequence_asset_type_invalid");
				return false;
			}
			const UMovieScene* MovieScene = Sequence->GetMovieScene();
			if (!MovieScene)
			{
				OutError = TEXT("sequence_movie_scene_missing");
				return false;
			}
			const TRange<FFrameNumber> PlaybackRange = MovieScene->GetPlaybackRange();
			if (!PlaybackRange.HasLowerBound() || !PlaybackRange.HasUpperBound())
			{
				OutError = TEXT("sequence_playback_range_unbounded");
				return false;
			}
			const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
			if (DisplayRate.Numerator <= 0 || DisplayRate.Denominator <= 0)
			{
				OutError = TEXT("sequence_timing_invalid");
				return false;
			}

			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(
				TEXT("display_rate"),
				FString::Printf(TEXT("%d/%d"), DisplayRate.Numerator, DisplayRate.Denominator));
			Object->SetNumberField(
				TEXT("playback_start"),
				PlaybackRange.GetLowerBoundValue().Value);
			Object->SetNumberField(
				TEXT("playback_end"),
				PlaybackRange.GetUpperBoundValue().Value);
			OutValue = MakeShared<FJsonValueObject>(Object);
			OutError.Reset();
			return true;
		}

		bool ReadStateTree(
			UObject& Asset,
			TSharedPtr<FJsonValue>& OutValue,
			FString& OutError)
		{
			const UStateTree* StateTree = Cast<UStateTree>(&Asset);
			if (!StateTree)
			{
				OutError = TEXT("state_tree_asset_type_invalid");
				return false;
			}
#if WITH_EDITORONLY_DATA
			const UStateTreeEditorData* EditorData = Cast<UStateTreeEditorData>(StateTree->EditorData);
			if (!EditorData)
			{
				OutValue.Reset();
				OutError.Reset();
				return true;
			}
			if (!EditorData->Schema)
			{
				OutValue.Reset();
				OutError.Reset();
				return true;
			}
			if (EditorData->SubTrees.Num() == 0 || !EditorData->SubTrees[0])
			{
				OutValue.Reset();
				OutError.Reset();
				return true;
			}
			if (EditorData->SubTrees.Num() > 1)
			{
				OutError = TEXT("state_tree_root_ambiguous");
				return false;
			}
			const UStateTreeState& RootState = *EditorData->SubTrees[0];
			if (RootState.Name.IsNone())
			{
				OutValue.Reset();
				OutError.Reset();
				return true;
			}

			TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
			RootObject->SetStringField(TEXT("name"), RootState.Name.ToString());
			TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("schema_class"), EditorData->Schema->GetClass()->GetPathName());
			Object->SetObjectField(TEXT("root_state"), RootObject);
			OutValue = MakeShared<FJsonValueObject>(Object);
			OutError.Reset();
			return true;
#else
			OutError = TEXT("state_tree_editor_data_unavailable");
			return false;
#endif
		}

		bool ApplySequence(
			UObject& Asset,
			const TSharedPtr<FJsonValue>& Value,
			FString& OutError)
		{
			FSequenceState State;
			if (!ParseSequenceState(Value, State, OutError))
			{
				return false;
			}
			ULevelSequence* Sequence = Cast<ULevelSequence>(&Asset);
			if (!Sequence)
			{
				OutError = TEXT("sequence_asset_type_invalid");
				return false;
			}
			UMovieScene* MovieScene = Sequence->GetMovieScene();
			if (!MovieScene)
			{
				OutError = TEXT("sequence_movie_scene_missing");
				return false;
			}

			Sequence->Modify();
			MovieScene->Modify();
			MovieScene->SetDisplayRate(FFrameRate(
				State.DisplayRateNumerator,
				State.DisplayRateDenominator));
			MovieScene->SetPlaybackRange(
				FFrameNumber(State.PlaybackStart),
				State.PlaybackEnd - State.PlaybackStart);
			Sequence->MarkPackageDirty();
			OutError.Reset();
			return true;
		}

		bool ApplyStateTree(
			UObject& Asset,
			const TSharedPtr<FJsonValue>& Value,
			FString& OutError)
		{
			FStateTreeState State;
			if (!ParseStateTreeState(Value, State, OutError))
			{
				return false;
			}
			UStateTree* StateTree = Cast<UStateTree>(&Asset);
			if (!StateTree)
			{
				OutError = TEXT("state_tree_asset_type_invalid");
				return false;
			}
#if WITH_EDITORONLY_DATA
			UStateTreeEditorData* EditorData = Cast<UStateTreeEditorData>(StateTree->EditorData);
			if (StateTree->EditorData && !EditorData)
			{
				OutError = TEXT("state_tree_editor_data_invalid");
				return false;
			}
			if (EditorData && EditorData->SubTrees.Num() > 1)
			{
				OutError = TEXT("state_tree_root_ambiguous");
				return false;
			}

			StateTree->Modify();
			if (!EditorData)
			{
				EditorData = NewObject<UStateTreeEditorData>(
					StateTree,
					UStateTreeEditorData::StaticClass(),
					NAME_None,
					RF_Transactional);
				if (!EditorData)
				{
					OutError = TEXT("state_tree_editor_data_create_failed");
					return false;
				}
				StateTree->EditorData = EditorData;
			}
			EditorData->Modify();

			if (!EditorData->Schema || EditorData->Schema->GetClass() != State.SchemaClass)
			{
				UStateTreeSchema* Schema = NewObject<UStateTreeSchema>(
					EditorData,
					State.SchemaClass,
					NAME_None,
					RF_Transactional);
				if (!Schema)
				{
					OutError = TEXT("state_tree_schema_create_failed");
					return false;
				}
				EditorData->Schema = Schema;
			}

			UStateTreeState* RootState = nullptr;
			if (EditorData->SubTrees.Num() == 1 && EditorData->SubTrees[0])
			{
				RootState = EditorData->SubTrees[0];
			}
			else
			{
				EditorData->SubTrees.Reset();
				RootState = &EditorData->AddRootState();
			}
			RootState->Modify();
			if (!RootState->ID.IsValid())
			{
				RootState->ID = FGuid::NewGuid();
			}
			RootState->Name = FName(*State.RootStateName);
			StateTree->MarkPackageDirty();
			OutError.Reset();
			return true;
#else
			OutError = TEXT("state_tree_editor_data_unavailable");
			return false;
#endif
		}
	}

	bool Supports(const FProfile& Profile)
	{
		return GetKind(Profile) != EProfileStateKind::Unsupported;
	}

	bool Validate(
		const FProfile& Profile,
		const TSharedPtr<FJsonValue>& Value,
		FString& OutError)
	{
		switch (GetKind(Profile))
		{
		case EProfileStateKind::Sequence:
		{
			FSequenceState State;
			return ParseSequenceState(Value, State, OutError);
		}
		case EProfileStateKind::StateTree:
		{
			FStateTreeState State;
			return ParseStateTreeState(Value, State, OutError);
		}
		default:
			return FailUnsupported(Profile, OutError);
		}
	}

	bool Read(
		const FProfile& Profile,
		UObject& Asset,
		TSharedPtr<FJsonValue>& OutValue,
		FString& OutError)
	{
		OutValue.Reset();
		OutError.Reset();
		switch (GetKind(Profile))
		{
		case EProfileStateKind::Sequence:
			return ReadSequence(Asset, OutValue, OutError);
		case EProfileStateKind::StateTree:
			return ReadStateTree(Asset, OutValue, OutError);
		default:
			return FailUnsupported(Profile, OutError);
		}
	}

	bool Apply(
		const FProfile& Profile,
		UObject& Asset,
		const TSharedPtr<FJsonValue>& Value,
		FString& OutError)
	{
		OutError.Reset();
		switch (GetKind(Profile))
		{
		case EProfileStateKind::Sequence:
			return ApplySequence(Asset, Value, OutError);
		case EProfileStateKind::StateTree:
			return ApplyStateTree(Asset, Value, OutError);
		default:
			return FailUnsupported(Profile, OutError);
		}
	}

	bool CompileIfNeeded(
		const FProfile& Profile,
		UObject& Asset,
		FString& OutError)
	{
		OutError.Reset();
		switch (GetKind(Profile))
		{
		case EProfileStateKind::Sequence:
			return true;
		case EProfileStateKind::StateTree:
		{
			UStateTree* StateTree = Cast<UStateTree>(&Asset);
			if (!StateTree)
			{
				OutError = TEXT("state_tree_asset_type_invalid");
				return false;
			}
#if WITH_EDITOR
			FStateTreeCompilerLog CompileLog;
			if (!UStateTreeEditingSubsystem::CompileStateTree(StateTree, CompileLog))
			{
				OutError = TEXT("state_tree_compile_failed");
				return false;
			}
			StateTree->MarkPackageDirty();
			return true;
#else
			OutError = TEXT("state_tree_compile_unavailable");
			return false;
#endif
		}
		default:
			return FailUnsupported(Profile, OutError);
		}
	}
}
